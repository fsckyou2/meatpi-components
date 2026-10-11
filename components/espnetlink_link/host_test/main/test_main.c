/**
 * @file test_main.c
 * @brief Host suite for espnetlink_link's pure core: SSID match, the
 *        dongle `/api/wifi_modem` health / `/api/info` / credentials
 *        parsers, the "changed?" compare, URL build, the settings
 *        migration, the shared GPS parser against the dongle's HTTP
 *        `/api/gps` document (latitude/longitude, unlike the console's
 *        lat/lon), plus the pairing state machine (test_sm.c).
 */
#include <string.h>

#include "unity.h"

#include "cJSON.h"

#include "espnetlink_link_core.h"
#include "espnetlink_link_migrate.h"
#include "usb_acm_gps.h"

/* GET /api/wifi_modem as the dongle serves it */
static const char *HEALTH =
    "{\"usb_mode\":\"auto\",\"usb_data\":false,\"ap_share\":true,"
    "\"napt\":true,"
    "\"ap\":{\"ssid\":\"ESPNetLink_A1B2C3\",\"ip\":\"192.168.80.1\","
    "\"started\":true,\"clients\":1},"
    "\"lte\":{\"valid\":true,\"sim\":\"ready\",\"stage\":\"connected\","
    "\"attached\":true,\"connected\":true,"
    "\"rssi_dbm\":-51,\"operator\":\"ALDI Mobile\",\"network_type\":\"eMTC\","
    "\"ip\":\"10.86.12.44\"},"
    "\"gps\":{\"valid\":true,\"fix\":false,\"satellites\":0,\"age_ms\":120},"
    "\"uptime_s\":42,\"device_id\":\"abcdef\"}";

/* LTE not yet up: valid:false and nothing else inside */
static const char *HEALTH_LTE_DOWN =
    "{\"usb_mode\":\"power_only\",\"usb_data\":true,\"ap_share\":true,"
    "\"napt\":false,"
    "\"ap\":{\"ip\":\"192.168.80.1\",\"started\":true,\"clients\":0},"
    "\"lte\":{\"valid\":false},\"gps\":{\"valid\":false,\"fix\":false},"
    "\"uptime_s\":3,\"device_id\":\"abcdef\"}";

/* GET /api/info */
static const char *INFO =
    "{\"device_type\":\"espnetlink\",\"model\":\"ESPNetLink\","
    "\"fw_version\":\"v1.22-40-g83df4ae\",\"device_id\":\"206ef1894a5d\","
    "\"mac\":\"20:6E:F1:89:4A:5C\",\"api_level\":6}";

static const char *INFO_WICAN =
    "{\"device_type\":\"wican\",\"fw_version\":\"v4.51\","
    "\"device_id\":\"68ee8f5a653d\",\"api_level\":6}";

/* GET /api/wifi_modem/credentials (USB only) */
static const char *CREDS =
    "{\"ssid\":\"ESPNetLink_894A5D\",\"password\":\"Kx7mQp2vRt9w\","
    "\"device_id\":\"206ef1894a5d\",\"ap_started\":true}";

/* GET /api/gps with a live fix (gps_http.c shape) */
static const char *GPS_HTTP_FIX =
    "{\"valid\":true,\"fix\":true,\"satellites\":9,\"sats_in_view\":14,"
    "\"fix_quality\":1,\"fix_type\":3,\"age_ms\":350,"
    "\"latitude\":-25.3444000,\"longitude\":131.0369000,"
    "\"altitude_m\":88.8,\"hdop\":0.9,\"speed_kmph\":12.5,"
    "\"course_deg\":181.0}";

/* GET /api/gps: valid NMEA stream but no fix yet */
static const char *GPS_HTTP_NOFIX =
    "{\"valid\":true,\"fix\":false,\"satellites\":0,\"sats_in_view\":6,"
    "\"fix_quality\":0,\"fix_type\":1,\"age_ms\":200}";

void test_identity(void)
{
    TEST_ASSERT_TRUE(espnl_core_is_espnetlink(0x303A, 0x4007));
    /* the S3's boot-window serial-JTAG is NOT the dongle */
    TEST_ASSERT_FALSE(espnl_core_is_espnetlink(0x303A, 0x1001));
    TEST_ASSERT_FALSE(espnl_core_is_espnetlink(0x0BDA, 0x8152));
    TEST_ASSERT_FALSE(espnl_core_is_espnetlink(0, 0));
}

void test_ssid_match(void)
{
    TEST_ASSERT_TRUE(espnl_core_ssid_match("ESPNetLink_A1B2C3",
                                           "ESPNetLink_A1B2C3"));
    TEST_ASSERT_FALSE(espnl_core_ssid_match("ESPNetLink_A1B2C3",
                                            "espnetlink_a1b2c3"));
    TEST_ASSERT_FALSE(espnl_core_ssid_match("", ""));
    TEST_ASSERT_FALSE(espnl_core_ssid_match("", "Home"));
    TEST_ASSERT_FALSE(espnl_core_ssid_match(NULL, "Home"));
    TEST_ASSERT_FALSE(espnl_core_ssid_match("Home", NULL));
}

void test_health_parse(void)
{
    espnl_health_t h;

    TEST_ASSERT_TRUE(espnl_core_parse_health(HEALTH, &h));
    TEST_ASSERT_TRUE(h.valid);
    TEST_ASSERT_TRUE(h.napt);
    TEST_ASSERT_FALSE(h.usb_data);
    TEST_ASSERT_TRUE(h.lte_valid);
    TEST_ASSERT_TRUE(h.lte_attached);
    TEST_ASSERT_TRUE(h.lte_connected);
    TEST_ASSERT_EQUAL_INT(-51, h.rssi_dbm);
    TEST_ASSERT_EQUAL_STRING("ALDI Mobile", h.operator_name);
    TEST_ASSERT_EQUAL_STRING("eMTC", h.network_type);
    /* api 8 (2026-10-07): the SIM state, the stage and the carrier address */
    TEST_ASSERT_EQUAL_STRING("ready", h.sim);
    TEST_ASSERT_EQUAL_STRING("connected", h.stage);
    TEST_ASSERT_EQUAL_STRING("10.86.12.44", h.ip);
    TEST_ASSERT_TRUE(h.gps_valid);
    TEST_ASSERT_FALSE(h.gps_fix);
    TEST_ASSERT_EQUAL_INT(1, h.ap_clients);
}

/* a dongle with no SIM says so before any attach (api 8), and an older
 * dongle (no sim/stage keys) leaves the strings empty */
void test_health_parse_sim_states(void)
{
    static const char *NO_SIM =
        "{\"usb_mode\":\"auto\",\"usb_data\":true,\"napt\":false,"
        "\"lte\":{\"valid\":false,\"sim\":\"missing\",\"stage\":\"connecting\"},"
        "\"gps\":{\"valid\":false,\"fix\":false}}";
    espnl_health_t h;

    TEST_ASSERT_TRUE(espnl_core_parse_health(NO_SIM, &h));
    TEST_ASSERT_FALSE(h.lte_valid);
    TEST_ASSERT_EQUAL_STRING("missing", h.sim);
    TEST_ASSERT_EQUAL_STRING("connecting", h.stage);
    TEST_ASSERT_EQUAL_STRING("", h.ip);

    TEST_ASSERT_TRUE(espnl_core_parse_health(HEALTH_LTE_DOWN, &h));
    TEST_ASSERT_EQUAL_STRING("", h.sim);
    TEST_ASSERT_EQUAL_STRING("", h.stage);
}

void test_health_parse_lte_down(void)
{
    espnl_health_t h;

    TEST_ASSERT_TRUE(espnl_core_parse_health(HEALTH_LTE_DOWN, &h));
    TEST_ASSERT_FALSE(h.napt);
    TEST_ASSERT_TRUE(h.usb_data);
    TEST_ASSERT_FALSE(h.lte_valid);
    /* the section's own "valid" must not bleed into gps/lte flags */
    TEST_ASSERT_FALSE(h.lte_connected);
    TEST_ASSERT_FALSE(h.gps_valid);
    TEST_ASSERT_EQUAL_INT(0, h.rssi_dbm);
    TEST_ASSERT_EQUAL_STRING("", h.operator_name);
    TEST_ASSERT_EQUAL_INT(0, h.ap_clients);
}

void test_health_parse_garbage(void)
{
    espnl_health_t h;

    TEST_ASSERT_FALSE(espnl_core_parse_health("<html>busy</html>", &h));
    TEST_ASSERT_FALSE(h.valid);
    TEST_ASSERT_FALSE(espnl_core_parse_health(NULL, &h));
    TEST_ASSERT_FALSE(espnl_core_parse_health(HEALTH, NULL));
}

void test_info_parse(void)
{
    espnl_info_t i;

    TEST_ASSERT_TRUE(espnl_core_parse_info(INFO, &i));
    TEST_ASSERT_TRUE(i.valid);
    TEST_ASSERT_TRUE(i.is_espnetlink);
    TEST_ASSERT_EQUAL_STRING("206ef1894a5d", i.device_id);
    TEST_ASSERT_EQUAL_STRING("v1.22-40-g83df4ae", i.fw_version);
    TEST_ASSERT_EQUAL_INT(6, i.api_level);

    TEST_ASSERT_TRUE(espnl_core_parse_info(INFO_WICAN, &i));
    TEST_ASSERT_FALSE(i.is_espnetlink);
    TEST_ASSERT_EQUAL_STRING("68ee8f5a653d", i.device_id);

    TEST_ASSERT_FALSE(espnl_core_parse_info("", &i));
    TEST_ASSERT_FALSE(i.valid);
    TEST_ASSERT_FALSE(espnl_core_parse_info(NULL, &i));
}

void test_credentials_parse(void)
{
    espnl_creds_t c;

    TEST_ASSERT_TRUE(espnl_core_parse_credentials(CREDS, &c));
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_EQUAL_STRING("ESPNetLink_894A5D", c.ssid);
    TEST_ASSERT_EQUAL_STRING("Kx7mQp2vRt9w", c.password);
    TEST_ASSERT_EQUAL_STRING("206ef1894a5d", c.device_id);
    TEST_ASSERT_TRUE(c.ap_started);

    /* 403 over WiFi: an error document, no credentials */
    TEST_ASSERT_FALSE(espnl_core_parse_credentials(
        "{\"error\":\"credentials are served over USB only\"}", &c));
    TEST_ASSERT_FALSE(c.valid);
    TEST_ASSERT_EQUAL_STRING("", c.password);

    /* escaped quote inside the password survives; oversize is refused */
    TEST_ASSERT_TRUE(espnl_core_parse_credentials(
        "{\"ssid\":\"A\",\"password\":\"p\\\"q\",\"device_id\":\"0\"}", &c));
    TEST_ASSERT_EQUAL_STRING("p\"q", c.password);
    TEST_ASSERT_FALSE(espnl_core_parse_credentials(
        "{\"ssid\":\"ThisSsidIsWayTooLongForAnyWifiNetworkOut\","
        "\"password\":\"x\",\"device_id\":\"0\"}", &c));
}

void test_credentials_changed(void)
{
    espnl_creds_t c;

    TEST_ASSERT_TRUE(espnl_core_parse_credentials(CREDS, &c));
    TEST_ASSERT_FALSE(espnl_core_creds_changed(&c, "ESPNetLink_894A5D",
                                               "Kx7mQp2vRt9w",
                                               "206ef1894a5d"));
    TEST_ASSERT_TRUE(espnl_core_creds_changed(&c, "ESPNetLink_894A5D",
                                              "rotated", "206ef1894a5d"));
    TEST_ASSERT_TRUE(espnl_core_creds_changed(&c, "", "", ""));
    TEST_ASSERT_TRUE(espnl_core_creds_changed(&c, NULL, NULL, NULL));
    /* a swapped dongle: same key shape, different id */
    TEST_ASSERT_TRUE(espnl_core_creds_changed(&c, "ESPNetLink_894A5D",
                                              "Kx7mQp2vRt9w",
                                              "ffffffffffff"));
    c.valid = false;
    TEST_ASSERT_FALSE(espnl_core_creds_changed(&c, "", "", ""));
}

void test_url(void)
{
    char url[64];

    TEST_ASSERT_TRUE(espnl_core_url(url, sizeof(url), "192.168.80.1",
                                    "/api/gps"));
    TEST_ASSERT_EQUAL_STRING("http://192.168.80.1/api/gps", url);
    TEST_ASSERT_TRUE(espnl_core_url(url, sizeof(url), ESPNL_USB_HOST_ADDR,
                                    "api/gps"));
    TEST_ASSERT_EQUAL_STRING("http://192.168.7.1/api/gps", url);
    TEST_ASSERT_FALSE(espnl_core_url(url, sizeof(url), "", "/api/gps"));
    TEST_ASSERT_FALSE(espnl_core_url(url, 10, "192.168.80.1", "/api/gps"));
    TEST_ASSERT_EQUAL_STRING("", url);
}

void test_mode_tokens(void)
{
    /* decode (the settings on_apply path) */
    TEST_ASSERT_EQUAL(ESPNL_CORE_MODE_WIFI_MODEM,
                      espnl_core_mode_from_str("wifi_modem"));
    TEST_ASSERT_EQUAL(ESPNL_CORE_MODE_USB_NCM,
                      espnl_core_mode_from_str("usb_ncm"));
    TEST_ASSERT_EQUAL(ESPNL_CORE_MODE_USB_RNDIS,
                      espnl_core_mode_from_str("usb_rndis"));
    TEST_ASSERT_EQUAL(ESPNL_CORE_MODE_WIFI_MODEM,
                      espnl_core_mode_from_str("bogus"));
    TEST_ASSERT_EQUAL(ESPNL_CORE_MODE_WIFI_MODEM,
                      espnl_core_mode_from_str(NULL));

    /* render round-trips (log/HTTP/CLI share these) */
    TEST_ASSERT_EQUAL_STRING("wifi_modem",
                             espnl_core_mode_str(ESPNL_CORE_MODE_WIFI_MODEM));
    TEST_ASSERT_EQUAL_STRING("usb_ncm",
                             espnl_core_mode_str(ESPNL_CORE_MODE_USB_NCM));
    TEST_ASSERT_EQUAL_STRING("usb_rndis",
                             espnl_core_mode_str(ESPNL_CORE_MODE_USB_RNDIS));

    /* the dongle class each mode asks for */
    TEST_ASSERT_NULL(espnl_core_mode_usb_class(ESPNL_CORE_MODE_WIFI_MODEM));
    TEST_ASSERT_EQUAL_STRING("ncm",
                             espnl_core_mode_usb_class(ESPNL_CORE_MODE_USB_NCM));
    TEST_ASSERT_EQUAL_STRING("rndis",
                             espnl_core_mode_usb_class(ESPNL_CORE_MODE_USB_RNDIS));
}

void test_gps_http_fix(void)
{
    usb_acm_gps_t g;

    TEST_ASSERT_TRUE(usb_acm_gps_parse(GPS_HTTP_FIX, &g));
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -25.3444, g.latitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 131.0369, g.longitude);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 88.8, g.altitude_m);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 12.5, g.speed_kmph);
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 181.0, g.heading_deg);
    TEST_ASSERT_EQUAL_INT(9, g.satellites);
    TEST_ASSERT_EQUAL_INT(5, g.accuracy_m); /* round(0.9*5) */
}

void test_gps_http_nofix(void)
{
    usb_acm_gps_t g;

    /* "valid":true only says the NMEA stream is alive; no coordinates
     * means no fix to publish */
    TEST_ASSERT_FALSE(usb_acm_gps_parse(GPS_HTTP_NOFIX, &g));
    TEST_ASSERT_FALSE(g.valid);
}

void test_migrate_v1_unpaired_enables(void)
{
    /* v1 default object: enabled=false because that was v1's default */
    cJSON *s = cJSON_Parse("{\"enabled\":false,\"ssid\":\"\",\"host\":\"\","
                           "\"gps_poll_s\":2,\"health_poll_s\":10,"
                           "\"cli\":true}");

    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(ESP_OK, espnl_settings_migrate(1, s));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(s, "enabled")));
    /* untouched keys survive; new keys are the schema's job */
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(s, "gps_poll_s")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(s, "mode"));
    cJSON_Delete(s);
}

void test_migrate_v1_paired_keeps_choice(void)
{
    cJSON *s = cJSON_Parse("{\"enabled\":false,\"ssid\":\"ESPNetLink_1\","
                           "\"host\":\"\",\"gps_poll_s\":2,"
                           "\"health_poll_s\":10,\"cli\":true}");

    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL(ESP_OK, espnl_settings_migrate(1, s));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(s, "enabled")));
    cJSON_Delete(s);

    /* current version: a no-op */
    s = cJSON_Parse("{\"enabled\":false,\"ssid\":\"\"}");
    TEST_ASSERT_EQUAL(ESP_OK, espnl_settings_migrate(2, s));
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(s, "enabled")));
    cJSON_Delete(s);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espnl_settings_migrate(1, NULL));
}

void run_sm_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_ssid_match);
    RUN_TEST(test_health_parse);
    RUN_TEST(test_health_parse_lte_down);
    RUN_TEST(test_health_parse_sim_states);
    RUN_TEST(test_health_parse_garbage);
    RUN_TEST(test_info_parse);
    RUN_TEST(test_credentials_parse);
    RUN_TEST(test_credentials_changed);
    RUN_TEST(test_url);
    RUN_TEST(test_mode_tokens);
    RUN_TEST(test_gps_http_fix);
    RUN_TEST(test_gps_http_nofix);
    RUN_TEST(test_migrate_v1_unpaired_enables);
    RUN_TEST(test_migrate_v1_paired_keeps_choice);
    run_sm_tests();
    UNITY_END();
}
