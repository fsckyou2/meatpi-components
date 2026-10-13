/**
 * @file test_main.c
 * @brief Host tests for the pure core of config_backup: the member
 *        scanner, the redaction rule and its inverse, the name checks, the
 *        key-file codec, the manifest codec, the ISO time.
 *        Expected: "25 Tests 0 Failures 0 Ignored".
 */
#include <string.h>

#include "unity.h"
#include "cJSON.h"

#include "config_backup_private.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- the scanner ---------------------------------------------------------------------- */

typedef struct
{
    int    n;
    char   keys[8][16];
    size_t lens[8];
} walk_t;

static esp_err_t walk_cb(const cb_member_t *m, void *ctx)
{
    walk_t *w = ctx;

    if (w->n < 8)
    {
        snprintf(w->keys[w->n], 16, "%.*s", (int)m->key_len, m->key);
        w->lens[w->n] = m->val_len;
    }

    w->n++;
    return ESP_OK;
}

static void test_members_walks_every_type(void)
{
    const char *doc = " { \"a\" : 1 , \"b\":\"x\\\"y\", \"c\": {\"d\":[1,2,{\"e\":\"}\"}]},"
                      " \"f\" : [ ], \"g\":true,\"h\":null }";
    walk_t w = { 0 };

    TEST_ASSERT_EQUAL(ESP_OK, cb_json_members(doc, strlen(doc), walk_cb, &w));
    TEST_ASSERT_EQUAL_INT(6, w.n);
    TEST_ASSERT_EQUAL_STRING("a", w.keys[0]);
    TEST_ASSERT_EQUAL_UINT(1, w.lens[0]);
    TEST_ASSERT_EQUAL_STRING("b", w.keys[1]);
    TEST_ASSERT_EQUAL_UINT(6, w.lens[1]); /* "x\"y" with the quotes */
    TEST_ASSERT_EQUAL_STRING("c", w.keys[2]);
    TEST_ASSERT_EQUAL_UINT(strlen("{\"d\":[1,2,{\"e\":\"}\"}]}"), w.lens[2]);
    TEST_ASSERT_EQUAL_STRING("f", w.keys[3]);
    TEST_ASSERT_EQUAL_STRING("g", w.keys[4]);
    TEST_ASSERT_EQUAL_STRING("h", w.keys[5]);
}

static void test_members_rejects_malformed(void)
{
    walk_t w = { 0 };

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_json_members("[1]", 3, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_json_members("{\"a\":1", 6, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_json_members("{\"a\" 1}", 7, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_json_members("{\"a\":\"x}", 8, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_json_members("{\"a\":{}", 7, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_OK, cb_json_members("{}", 2, walk_cb, &w));
    TEST_ASSERT_EQUAL(ESP_OK, cb_json_members(" {  } ", 6, walk_cb, &w));
}

static void test_find_and_scalars(void)
{
    const char *doc = "{\"wican_backup\":2,\"encrypted\":true,\"device_id\":\"68ee8f5a653d\","
                      "\"created\":\"\",\"parts\":{\"settings\":{}},\"n\":null}";
    cb_member_t m;
    char        s[32];

    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "wican_backup", &m));
    TEST_ASSERT_EQUAL_INT(2, cb_json_long(&m));
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "encrypted", &m));
    TEST_ASSERT_TRUE(cb_json_bool(&m));
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "device_id", &m));
    TEST_ASSERT_TRUE(cb_json_string(&m, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("68ee8f5a653d", s);
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "created", &m));
    TEST_ASSERT_TRUE(cb_json_string(&m, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "n", &m));
    TEST_ASSERT_TRUE(cb_json_is_null(&m));
    TEST_ASSERT_FALSE(cb_json_find(doc, strlen(doc), "nope", &m));
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "parts", &m));
    TEST_ASSERT_EQUAL_UINT(strlen("{\"settings\":{}}"), m.val_len);
}

static void test_string_unescapes(void)
{
    const char *doc = "{\"s\":\"a\\nb\\\"c\\\\d\\u00e9e\"}";
    cb_member_t m;
    char        s[32];

    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "s", &m));
    TEST_ASSERT_TRUE(cb_json_string(&m, s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("a\nb\"c\\d?e", s);
    TEST_ASSERT_FALSE(cb_json_string(&m, s, 4)); /* does not fit */
}

static void test_valid_whole_buffer(void)
{
    TEST_ASSERT_TRUE(cb_json_valid(" {\"a\":[1,2]} \n", 14));
    TEST_ASSERT_TRUE(cb_json_valid("[]", 2));
    TEST_ASSERT_TRUE(cb_json_valid("\"x\"", 3));
    TEST_ASSERT_FALSE(cb_json_valid("{\"a\":1} x", 9));
    TEST_ASSERT_FALSE(cb_json_valid("{\"a\":1", 6));
    TEST_ASSERT_FALSE(cb_json_valid("", 0));
}

static void test_part_names(void)
{
    TEST_ASSERT_EQUAL_INT(CB_PART_SETTINGS, cb_part_from_name("settings", 8));
    TEST_ASSERT_EQUAL_INT(CB_PART_CERTS, cb_part_from_name("certs", 5));
    TEST_ASSERT_EQUAL_INT(-1, cb_part_from_name("setting", 7));
    TEST_ASSERT_EQUAL_INT(-1, cb_part_from_name("settingsx", 9));
    TEST_ASSERT_EQUAL_INT(-1, cb_part_from_name(NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(0x1fu, CB_MASK_FILE);
    TEST_ASSERT_EQUAL_UINT32(0x3fu, CB_MASK_ALL);
}

/* ---- the redaction rule ----------------------------------------------------------------- */

static void test_secret_rule(void)
{
    TEST_ASSERT_TRUE(cb_key_is_secret("wifi_manager", "sta_password"));
    TEST_ASSERT_TRUE(cb_key_is_secret("wifi_manager", "fallback3_password"));
    TEST_ASSERT_TRUE(cb_key_is_secret("vpn_manager", "private_key"));
    TEST_ASSERT_TRUE(cb_key_is_secret("vpn_manager", "preshared_key"));
    TEST_ASSERT_TRUE(cb_key_is_secret("vpn_manager", "ts_auth_key"));
    TEST_ASSERT_TRUE(cb_key_is_secret("data_destinations", "auth_token"));
    TEST_ASSERT_TRUE(cb_key_is_secret("data_destinations", "api_key"));
    TEST_ASSERT_TRUE(cb_key_is_secret("data_destinations", "url"));
    TEST_ASSERT_TRUE(cb_key_is_secret("mqtt_manager", "username"));
    TEST_ASSERT_TRUE(cb_key_is_secret("espnetlink", "apn_user"));
    TEST_ASSERT_TRUE(cb_key_is_secret("ha_webhooks", "url2"));
    TEST_ASSERT_FALSE(cb_key_is_secret("vpn_manager", "peer_public_key"));
    TEST_ASSERT_FALSE(cb_key_is_secret("mqtt_manager", "url"));
    TEST_ASSERT_FALSE(cb_key_is_secret("wifi_manager", "sta_ssid"));
    TEST_ASSERT_FALSE(cb_key_is_secret(NULL, "username"));
    TEST_ASSERT_FALSE(cb_key_is_secret("x", NULL));
}

static void test_redact_walks_nested_and_arrays(void)
{
    const char *doc = "{\"wifi_manager\":{\"version\":1,\"data\":{\"sta_ssid\":\"Home\","
                      "\"sta_password\":\"pw\",\"nested\":{\"x_password\":\"q\"}}},"
                      "\"data_destinations\":{\"version\":1,\"data\":{\"destinations\":"
                      "[{\"name\":\"a\",\"url\":\"http://x\",\"api_key\":\"k\",\"period_s\":5}]}}}";
    cJSON *root = cJSON_Parse(doc);

    TEST_ASSERT_NOT_NULL(root);
    cb_redact_settings(root);

    cJSON *wifi = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "wifi_manager"), "data");

    TEST_ASSERT_EQUAL_STRING("Home", cJSON_GetObjectItem(wifi, "sta_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(wifi, "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(cJSON_GetObjectItem(wifi, "nested"),
                                                    "x_password")->valuestring);

    cJSON *dd = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data_destinations"), "data");
    cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItem(dd, "destinations"), 0);

    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(row, "url")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(row, "api_key")->valuestring);
    TEST_ASSERT_EQUAL_STRING("a", cJSON_GetObjectItem(row, "name")->valuestring);
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetObjectItem(row, "period_s")->valueint);
    cJSON_Delete(root);
}

static void test_unredact_takes_stored_by_name_and_index(void)
{
    cJSON *in = cJSON_Parse("{\"sta_password\":\"\",\"sta_ssid\":\"New\",\"ap_password\":\"set\","
                            "\"list\":[{\"name\":\"b\",\"x_password\":\"\"},{\"x_password\":\"\"}]}");
    cJSON *stored = cJSON_Parse("{\"sta_password\":\"old\",\"sta_ssid\":\"Old\",\"ap_password\":\"ap\","
                                "\"list\":[{\"name\":\"a\",\"x_password\":\"pa\"},"
                                "{\"name\":\"b\",\"x_password\":\"pb\"}]}");

    cb_unredact(in, stored, "wifi_manager");
    TEST_ASSERT_EQUAL_STRING("old", cJSON_GetObjectItem(in, "sta_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("New", cJSON_GetObjectItem(in, "sta_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("set", cJSON_GetObjectItem(in, "ap_password")->valuestring);

    cJSON *list = cJSON_GetObjectItem(in, "list");

    /* the first row matches stored "b" by name, the second falls back to index 1 */
    TEST_ASSERT_EQUAL_STRING("pb", cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0),
                                                       "x_password")->valuestring);
    TEST_ASSERT_EQUAL_STRING("pb", cJSON_GetObjectItem(cJSON_GetArrayItem(list, 1),
                                                       "x_password")->valuestring);
    cJSON_Delete(in);
    cJSON_Delete(stored);
}

static void test_unredact_tolerates_missing_stored(void)
{
    cJSON *in = cJSON_Parse("{\"sta_password\":\"\"}");

    cb_unredact(in, NULL, "wifi_manager");
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetObjectItem(in, "sta_password")->valuestring);
    cJSON_Delete(in);
}

/* ---- names ----------------------------------------------------------------------------------- */

static void test_name_rules(void)
{
    TEST_ASSERT_TRUE(cb_name_ok("hello.be", 8));
    TEST_ASSERT_TRUE(cb_name_ok("generic-codes_v2.csv", 20));
    TEST_ASSERT_FALSE(cb_name_ok(".hidden", 7));
    TEST_ASSERT_FALSE(cb_name_ok("a/b", 3));
    TEST_ASSERT_FALSE(cb_name_ok("a b", 3));
    TEST_ASSERT_FALSE(cb_name_ok("", 0));
    TEST_ASSERT_FALSE(cb_name_ok(NULL, 1));

    char longname[CB_NAME_MAX + 2];

    memset(longname, 'a', sizeof(longname));
    TEST_ASSERT_FALSE(cb_name_ok(longname, CB_NAME_MAX));
    TEST_ASSERT_TRUE(cb_name_ok(longname, CB_NAME_MAX - 1));
}

static void test_set_and_vehicle_key_rules(void)
{
    TEST_ASSERT_TRUE(cb_set_name_ok("homeca", 6));
    TEST_ASSERT_TRUE(cb_set_name_ok("bench-1_x", 9));
    TEST_ASSERT_FALSE(cb_set_name_ok("HomeCA", 6));
    TEST_ASSERT_FALSE(cb_set_name_ok("a.b", 3));
    TEST_ASSERT_FALSE(cb_set_name_ok("abcdefghijklmnopqrstuvwxy", 25));
    TEST_ASSERT_TRUE(cb_vehicle_key_ok("1WCAN0FW0P0000001", 17));
    TEST_ASSERT_TRUE(cb_vehicle_key_ok("fp:1a2b3c4d", 11));
    TEST_ASSERT_FALSE(cb_vehicle_key_ok("a/b", 3));
    TEST_ASSERT_FALSE(cb_vehicle_key_ok("", 0));
}

/* ---- the key file --------------------------------------------------------------------------- */

static void test_key_text_decodes_file_and_bare_hex(void)
{
    const char *file = "# WiCAN backup key, device 68ee8f5a653d, made 2026-10-11.\n"
                       "# Keep this file apart from the backups.\n"
                       "9f3c6b1e0a7d44c2b8e15f9a2d6c0e71b3a4f8d2c6e9017b5a3d8f1c4e7b2a60\n";
    uint8_t key[CB_KEY_LEN];

    TEST_ASSERT_TRUE(cb_key_text_decode(file, strlen(file), key));
    TEST_ASSERT_EQUAL_HEX8(0x9f, key[0]);
    TEST_ASSERT_EQUAL_HEX8(0x60, key[31]);

    const char *bare = "  9F3C6B1E0A7D44C2B8E15F9A2D6C0E71B3A4F8D2C6E9017B5A3D8F1C4E7B2A60";

    TEST_ASSERT_TRUE(cb_key_text_decode(bare, strlen(bare), key));
    TEST_ASSERT_EQUAL_HEX8(0x3c, key[1]);
}

static void test_key_text_rejects_short_and_junk(void)
{
    uint8_t key[CB_KEY_LEN];

    TEST_ASSERT_FALSE(cb_key_text_decode("9f3c", 4, key));
    TEST_ASSERT_FALSE(cb_key_text_decode("zz3c6b1e0a7d44c2b8e15f9a2d6c0e71b3a4f8d2c6e9017b5a3d8f1c4e7b2a60", 64, key));
    TEST_ASSERT_FALSE(cb_key_text_decode("9f3c6b1e0a7d44c2b8e15f9a2d6c0e71b3a4f8d2c6e9017b5a3d8f1c4e7b2a60 x", 66, key));
    TEST_ASSERT_TRUE(cb_key_text_decode("9f3c6b1e0a7d44c2b8e15f9a2d6c0e71b3a4f8d2c6e9017b5a3d8f1c4e7b2a60 # ok", 69, key));
    TEST_ASSERT_FALSE(cb_key_text_decode(NULL, 0, key));
}

static void test_hex_round_trip(void)
{
    uint8_t in[4] = { 0x00, 0xab, 0xff, 0x10 };
    char    hex[9];
    uint8_t out[4];

    cb_hex_encode(in, 4, hex);
    TEST_ASSERT_EQUAL_STRING("00abff10", hex);
    TEST_ASSERT_TRUE(cb_hex_decode(hex, 8, out));
    TEST_ASSERT_EQUAL_MEMORY(in, out, 4);
    TEST_ASSERT_FALSE(cb_hex_decode("abc", 3, out));
    TEST_ASSERT_FALSE(cb_hex_decode("zz", 2, out));
}

static void test_crc32_vectors(void)
{
    /* the IEEE check value, the empty input, one byte at a time = one call */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, cb_crc32((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0x00000000u, cb_crc32((const uint8_t *)"", 0));
    TEST_ASSERT_EQUAL_HEX32(0x356FF657u, cb_crc32((const uint8_t *)"WCBS", 4));
}

/* ---- the manifest --------------------------------------------------------------------------- */

static void test_manifest_round_trip(void)
{
    cb_manifest_t m;

    memset(&m, 0, sizeof(m));
    snprintf(m.device_id, sizeof(m.device_id), "68ee8f5a653d");
    snprintf(m.fw, sizeof(m.fw), "v6.00p_alfa-03");
    m.refreshed = 1791693028;
    m.parts[CB_PART_SETTINGS].present = true;
    m.parts[CB_PART_SETTINGS].bytes = 7100;
    m.parts[CB_PART_SETTINGS].crc = 0x9ABCDEF0u;
    memset(m.parts[CB_PART_SETTINGS].hmac, 0xab, CB_HMAC_LEN);
    m.parts[CB_PART_CERTS].present = true;
    m.parts[CB_PART_CERTS].bytes = 2100;
    memset(m.parts[CB_PART_CERTS].hmac, 0x01, CB_HMAC_LEN);

    cJSON *j = cb_manifest_to_json(&m);

    TEST_ASSERT_NOT_NULL(j);

    cb_manifest_t back;

    TEST_ASSERT_EQUAL(ESP_OK, cb_manifest_from_json(j, &back));
    TEST_ASSERT_TRUE(back.loaded);
    TEST_ASSERT_EQUAL_STRING("68ee8f5a653d", back.device_id);
    TEST_ASSERT_EQUAL_STRING("v6.00p_alfa-03", back.fw);
    TEST_ASSERT_EQUAL_INT64(1791693028, back.refreshed);
    TEST_ASSERT_TRUE(back.parts[CB_PART_SETTINGS].present);
    TEST_ASSERT_EQUAL_UINT32(7100, back.parts[CB_PART_SETTINGS].bytes);
    TEST_ASSERT_EQUAL_MEMORY(m.parts[CB_PART_SETTINGS].hmac,
                             back.parts[CB_PART_SETTINGS].hmac, CB_HMAC_LEN);
    TEST_ASSERT_EQUAL_HEX32(0x9ABCDEF0u, back.parts[CB_PART_SETTINGS].crc);
    TEST_ASSERT_EQUAL_HEX32(0, back.parts[CB_PART_CERTS].crc); /* unknown */
    TEST_ASSERT_FALSE(back.parts[CB_PART_VEHICLES].present);
    TEST_ASSERT_TRUE(back.parts[CB_PART_CERTS].present);
    TEST_ASSERT_EQUAL_UINT32(9200, cb_manifest_bytes(&back));
    TEST_ASSERT_EQUAL_UINT(2, cb_manifest_count(&back));
    cJSON_Delete(j);
}

static void test_manifest_rejects_other_versions_and_bad_entries(void)
{
    cb_manifest_t m;
    cJSON *bad = cJSON_Parse("{\"wican_copy\":2,\"device_id\":\"x\",\"parts\":{}}");

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_manifest_from_json(bad, &m));
    cJSON_Delete(bad);

    cJSON *damaged = cJSON_Parse("{\"wican_copy\":1,\"device_id\":\"x\",\"parts\":"
                                 "{\"settings\":{\"bytes\":1,\"hmac\":\"zz\"},"
                                 "\"nope\":{\"bytes\":1,\"hmac\":\"00\"},"
                                 "\"scripts\":{\"bytes\":3,\"hmac\":\""
                                 "0000000000000000000000000000000000000000000000000000000000000000\"}}}");

    TEST_ASSERT_EQUAL(ESP_OK, cb_manifest_from_json(damaged, &m));
    TEST_ASSERT_FALSE(m.parts[CB_PART_SETTINGS].present); /* damaged = rewritten */
    TEST_ASSERT_TRUE(m.parts[CB_PART_SCRIPTS].present);
    TEST_ASSERT_EQUAL_HEX32(0, m.parts[CB_PART_SCRIPTS].crc); /* no crc: a manifest
                                                                 from before the check */
    TEST_ASSERT_EQUAL_INT64(0, m.refreshed);
    cJSON_Delete(damaged);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cb_manifest_from_json(NULL, &m));
}

/* ---- time ------------------------------------------------------------------------------------- */

static void test_iso_time(void)
{
    char iso[CB_ISO_LEN];

    cb_iso_from_epoch(0, iso);
    TEST_ASSERT_EQUAL_STRING("", iso);
    cb_iso_from_epoch(1791693028, iso);
    TEST_ASSERT_EQUAL_STRING("2026-10-11T04:30:28Z", iso);
}

/* ---- a whole document through the scanner ------------------------------------------------- */

typedef struct
{
    uint32_t mask;
} parts_t;

static esp_err_t parts_cb(const cb_member_t *m, void *ctx)
{
    parts_t *p = ctx;
    int      part = cb_part_from_name(m->key, m->key_len);

    if (part >= 0)
    {
        p->mask |= CB_MASK(part);
    }

    return ESP_OK;
}

static void test_document_parts_index(void)
{
    const char *doc = "{\"wican_backup\":2,\"device_id\":\"68ee8f5a653d\",\"fw_version\":\"v6\","
                      "\"created\":\"2026-10-11T09:14:05Z\",\"redacted\":false,\"parts\":{"
                      "\"settings\":{\"wifi_manager\":{\"version\":1,\"data\":{}}},"
                      "\"vehicles\":{\"index\":{\"vehicles\":[]},\"tables\":{}},"
                      "\"databases\":{\"dtc_db\":[],\"dbc\":[{\"name\":\"a.dbc\",\"size\":0,\"b64\":\"\"}]},"
                      "\"scripts\":[],\"dashboard\":null,\"extra\":1}}";
    cb_member_t parts;
    parts_t     p = { 0 };

    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "parts", &parts));
    TEST_ASSERT_EQUAL(ESP_OK, cb_json_members(parts.val, parts.val_len, parts_cb, &p));
    TEST_ASSERT_EQUAL_UINT32(CB_MASK_FILE, p.mask);
}

static void test_version_one_document(void)
{
    const char *doc = "{\"wican_backup\":1,\"device_id\":\"14c19f44e349\",\"components\":"
                      "{\"wifi_manager\":{\"version\":1,\"data\":{\"mode\":\"apsta\"}}}}";
    cb_member_t m;

    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "wican_backup", &m));
    TEST_ASSERT_EQUAL_INT(1, cb_json_long(&m));
    TEST_ASSERT_FALSE(cb_json_find(doc, strlen(doc), "parts", &m));
    TEST_ASSERT_TRUE(cb_json_find(doc, strlen(doc), "components", &m));
    TEST_ASSERT_TRUE(cb_json_valid(m.val, m.val_len));
}

static void test_big_string_is_not_walked(void)
{
    /* a 64 KB base64-like string inside the document: the scanner only skips it */
    static char doc[70000];
    size_t      n = 0;

    n += (size_t)snprintf(doc, sizeof(doc), "{\"data\":\"");
    memset(doc + n, 'A', 65536);
    n += 65536;
    n += (size_t)snprintf(doc + n, sizeof(doc) - n, "\",\"tail\":7}");

    cb_member_t m;

    TEST_ASSERT_TRUE(cb_json_find(doc, n, "tail", &m));
    TEST_ASSERT_EQUAL_INT(7, cb_json_long(&m));
    TEST_ASSERT_TRUE(cb_json_find(doc, n, "data", &m));
    TEST_ASSERT_EQUAL_UINT(65538, m.val_len);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_members_walks_every_type);
    RUN_TEST(test_members_rejects_malformed);
    RUN_TEST(test_find_and_scalars);
    RUN_TEST(test_string_unescapes);
    RUN_TEST(test_valid_whole_buffer);
    RUN_TEST(test_part_names);
    RUN_TEST(test_secret_rule);
    RUN_TEST(test_redact_walks_nested_and_arrays);
    RUN_TEST(test_unredact_takes_stored_by_name_and_index);
    RUN_TEST(test_unredact_tolerates_missing_stored);
    RUN_TEST(test_name_rules);
    RUN_TEST(test_set_and_vehicle_key_rules);
    RUN_TEST(test_key_text_decodes_file_and_bare_hex);
    RUN_TEST(test_key_text_rejects_short_and_junk);
    RUN_TEST(test_hex_round_trip);
    RUN_TEST(test_crc32_vectors);
    RUN_TEST(test_manifest_round_trip);
    RUN_TEST(test_manifest_rejects_other_versions_and_bad_entries);
    RUN_TEST(test_iso_time);
    RUN_TEST(test_document_parts_index);
    RUN_TEST(test_version_one_document);
    RUN_TEST(test_big_string_is_not_walked);
    UNITY_END();
}
