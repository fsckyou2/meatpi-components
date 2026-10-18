/**
 * @file test_main.c
 * @brief Host tests for fw_update's pure core: the version grammar and
 *        order, channel visibility, the manifest parse with its bounds,
 *        the selection rule (latest, stepping, yanked, dev builds, an
 *        unknown installed string), the layout verdict, the URL rules, the
 *        job record's codec and the boot decision table.
 *        Expected: 17 Tests 0 Failures 0 Ignored.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fw_update_core.h"
#include "fw_update_record.h"

void test_image_head_parse(void); /* test_image.c */

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- fixtures ------------------------------------------------------------------- */

#define SHA "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
#define MD5_V6 "35c16cb5677477a27c662bd912582329"
#define MD5_V4 "225242e6822a60aa93a9a67abd9442ed"

#define HEAD(dev) "{\"schema\":1,\"device_type\":\"" dev "\",\"model\":\"WiCAN PRO\"," \
    "\"project_name\":\"wican-fw_obd_pro\",\"source_repo\":\"meatpiHQ/wican-fw\"," \
    "\"updated\":\"2026-10-24T09:12:40Z\",\"channels\":{\"stable\":{\"latest\":null}," \
    "\"beta\":{\"latest\":null},\"alfa\":{\"latest\":null}},\"releases\":["
#define TAIL "]}"

#define IMG(ver, url, size) "\"image\":{\"url\":\"" url "\",\"size\":" size \
    ",\"sha256\":\"" SHA "\",\"project_name\":\"wican-fw_obd_pro\"},"
#define LAYOUT(md5) "\"layout\":{\"md5\":\"" md5 "\",\"table\":\"x.csv\"},"
#define FROM_V4 "\"layout_from\":{\"" MD5_V4 "\":{\"migration\":\"wipe\",\"wipes\":[\"storage\"]}},"
#define FROM_SERIAL "\"layout_from\":{\"" MD5_V4 "\":{\"migration\":\"serial\"}},"
#define FROM_LOSSLESS "\"layout_from\":{\"" MD5_V4 "\":{\"migration\":\"lossless\"}},"

/* one release with the required fields; `extra` carries optional members */
#define REL(ver, fw, ch, yanked, extra) "{\"version\":\"" ver "\",\"fw_version\":\"" fw \
    "\",\"tag\":\"v" ver "p\",\"channel\":\"" ch "\",\"date\":\"2026-10-20\"," \
    "\"summary\":\"Release " ver ".\",\"notes\":\"wican_pro/firmware/notes/" ver ".md\"," \
    IMG(ver, "https://github.com/x/releases/download/" ver "/f.bin", "3000000") \
    LAYOUT(MD5_V6) extra "\"yanked\":" yanked "}"

static const char M1[] = HEAD("wican_pro")
    REL("6.1.0", "6.1.0", "stable", "false", FROM_V4) ","
    REL("6.1.0-beta.2", "6.1.0-beta.2", "beta", "false", "") ","
    REL("6.2.0-alfa.1", "6.2.0-alfa.1", "alfa", "false", "") ","
    REL("6.0.0-alfa.6", "v6.00p_alfa-06", "alfa", "false", FROM_V4) ","
    "{\"version\":\"4.51.0\",\"fw_version\":\"v4.51p\",\"channel\":\"stable\","
    "\"date\":\"2026-09-04\",\"summary\":\"IDF 6.0.2.\",\"notes\":\"n/4.51.0.md\","
    IMG("4.51.0", "https://github.com/meatpiHQ/wican-fw/releases/download/v4.51p/w.bin", "3570592")
    LAYOUT(MD5_V4) "\"yanked\":false}"
    TAIL;

/* stepping: 6.3.0 needs 6.2.0 first */
static const char M2[] = HEAD("wican_pro")
    REL("6.3.0", "6.3.0", "stable", "false", "\"requires\":{\"min_version\":\"6.2.0\"},") ","
    REL("6.2.0", "6.2.0", "stable", "false", "") ","
    REL("6.1.0", "6.1.0", "stable", "false", "")
    TAIL;

/* the stepping stone yanked: no path */
static const char M3[] = HEAD("wican_pro")
    REL("6.3.0", "6.3.0", "stable", "false", "\"requires\":{\"min_version\":\"6.2.0\"},") ","
    REL("6.2.0", "6.2.0", "stable", "true", "\"yank_reason\":\"boot loop with a card\",") ","
    REL("6.1.0", "6.1.0", "stable", "false", "")
    TAIL;

/* the newest release yanked */
static const char M4[] = HEAD("wican_pro")
    REL("6.3.0", "6.3.0", "stable", "true", "\"yank_reason\":\"bad\",") ","
    REL("6.2.0", "6.2.0", "stable", "false", "")
    TAIL;

static fwu_manifest_t s_m; /* 70 KB: never a stack frame */

static void parse_ok(const char *doc)
{
    char err[64];

    TEST_ASSERT_EQUAL_MESSAGE(FWU_PARSE_OK,
                              fwu_manifest_parse(doc, strlen(doc), "wican_pro",
                                                 false, &s_m, err, sizeof(err)),
                              err);
}

static fwu_version_t V(const char *s)
{
    fwu_version_t v;

    TEST_ASSERT_TRUE_MESSAGE(fwu_version_parse_tolerant(s, &v), s);
    return v;
}

/* ---- versions ------------------------------------------------------------------- */

static void test_strict_grammar(void)
{
    fwu_version_t v;

    TEST_ASSERT_TRUE(fwu_version_parse_strict("6.1.0", &v));
    TEST_ASSERT_EQUAL(6, v.major);
    TEST_ASSERT_EQUAL(1, v.minor);
    TEST_ASSERT_EQUAL(0, v.patch);
    TEST_ASSERT_EQUAL(FWU_PRE_NONE, v.pre);
    TEST_ASSERT_FALSE(v.dev);
    TEST_ASSERT_TRUE(fwu_version_parse_strict("6.1.0-alfa.12", &v));
    TEST_ASSERT_EQUAL(FWU_PRE_ALFA, v.pre);
    TEST_ASSERT_EQUAL(12, v.pre_n);
    TEST_ASSERT_TRUE(fwu_version_parse_strict("1.23.0-beta.1", &v));
    TEST_ASSERT_EQUAL(FWU_PRE_BETA, v.pre);
    TEST_ASSERT_TRUE(fwu_version_parse_strict("0.0.0", &v));
    TEST_ASSERT_TRUE(fwu_version_parse_strict("65535.0.1", &v));

    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.00.1", &v));   /* leading zero */
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1", &v));      /* no patch */
    TEST_ASSERT_FALSE(fwu_version_parse_strict("v6.1.0", &v));   /* the v */
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1.0p", &v));   /* the letter */
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1.0-rc.1", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1.0-alfa", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1.0-alfa.01", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict("65536.0.0", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict("6.1.0 ", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict("", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_strict(NULL, &v));
}

static void test_tolerant_grammar(void)
{
    fwu_version_t v;
    char s[32];

    v = V("v4.51p");
    TEST_ASSERT_EQUAL(4, v.major);
    TEST_ASSERT_EQUAL(51, v.minor);
    TEST_ASSERT_EQUAL(0, v.patch);
    TEST_ASSERT_EQUAL(FWU_PRE_NONE, v.pre);
    TEST_ASSERT_FALSE(v.dev);

    v = V("v6.00p_alfa-06");
    fwu_version_str(&v, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("6.0.0-alfa.6", s);
    TEST_ASSERT_FALSE(v.dev);

    v = V("v6.00p_alfa-04-2-g2a40245-dirty");
    fwu_version_str(&v, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("6.0.0-alfa.4", s);
    TEST_ASSERT_TRUE(v.dev);

    v = V("v1.22-61-gcc81591");
    fwu_version_str(&v, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("1.22.0", s);
    TEST_ASSERT_TRUE(v.dev);

    v = V("v6.1.0p-alfa.1");
    fwu_version_str(&v, s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING("6.1.0-alfa.1", s);

    v = V("6.1.0-alfa.1");            /* a strict string parses too */
    TEST_ASSERT_EQUAL(FWU_PRE_ALFA, v.pre);
    v = V("6.0");                     /* 6.0 == 6.0.0 */
    TEST_ASSERT_EQUAL(0, v.patch);
    v = V("v6.1.0p-alpha-3");         /* alpha is alfa */
    TEST_ASSERT_EQUAL(FWU_PRE_ALFA, v.pre);
    TEST_ASSERT_EQUAL(3, v.pre_n);
    v = V("v6.1.0p-dirty");
    TEST_ASSERT_TRUE(v.dev);
    v = V("v6.00p_alfa-04-2-g2a40245-dirt"); /* cut at 31 characters */
    TEST_ASSERT_TRUE(v.dev);
    TEST_ASSERT_EQUAL(4, v.pre_n);

    TEST_ASSERT_FALSE(fwu_version_parse_tolerant("garbage", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_tolerant("v6", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_tolerant("6.1.0 stable", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_tolerant("", &v));
    TEST_ASSERT_FALSE(fwu_version_parse_tolerant(NULL, &v));
}

static void test_version_order(void)
{
    const char *asc[] =
    {
        "6.0.0-alfa.6", "6.0.0-beta.1", "6.0.0", "6.0.1", "6.1.0-alfa.1",
        "6.1.0-alfa.3", "6.1.0-beta.1", "6.1.0-beta.9", "6.1.0-beta.10",
        "6.1.0", "6.10.0",
    };

    for (size_t i = 1; i < sizeof(asc) / sizeof(asc[0]); i++)
    {
        fwu_version_t a = V(asc[i - 1]);
        fwu_version_t b = V(asc[i]);

        TEST_ASSERT_TRUE_MESSAGE(fwu_version_cmp(&a, &b) < 0, asc[i]);
        TEST_ASSERT_TRUE_MESSAGE(fwu_version_cmp(&b, &a) > 0, asc[i]);
    }

    fwu_version_t x = V("6.1.0");
    fwu_version_t y = V("6.1.0-5-gabcdef0");   /* a dev build of the same */
    fwu_version_t z = V("6.00.1");

    TEST_ASSERT_EQUAL(0, fwu_version_cmp(&x, &y));
    x = V("6.0.1");
    TEST_ASSERT_EQUAL(0, fwu_version_cmp(&x, &z));
    x = V("4.52.0");
    y = V("6.1.0-alfa.1");
    TEST_ASSERT_TRUE(fwu_version_cmp(&x, &y) < 0);
}

static void test_channels(void)
{
    TEST_ASSERT_EQUAL(FWU_CH_STABLE, fwu_channel_from_str("stable"));
    TEST_ASSERT_EQUAL(FWU_CH_BETA, fwu_channel_from_str("beta"));
    TEST_ASSERT_EQUAL(FWU_CH_ALFA, fwu_channel_from_str("alfa"));
    TEST_ASSERT_EQUAL(-1, fwu_channel_from_str("alpha"));
    TEST_ASSERT_EQUAL(-1, fwu_channel_from_str(NULL));
    TEST_ASSERT_TRUE(fwu_channel_visible(FWU_CH_STABLE, FWU_CH_STABLE));
    TEST_ASSERT_FALSE(fwu_channel_visible(FWU_CH_STABLE, FWU_CH_BETA));
    TEST_ASSERT_FALSE(fwu_channel_visible(FWU_CH_STABLE, FWU_CH_ALFA));
    TEST_ASSERT_TRUE(fwu_channel_visible(FWU_CH_BETA, FWU_CH_STABLE));
    TEST_ASSERT_TRUE(fwu_channel_visible(FWU_CH_BETA, FWU_CH_BETA));
    TEST_ASSERT_FALSE(fwu_channel_visible(FWU_CH_BETA, FWU_CH_ALFA));
    TEST_ASSERT_TRUE(fwu_channel_visible(FWU_CH_ALFA, FWU_CH_ALFA));
    TEST_ASSERT_EQUAL_STRING("alfa", fwu_channel_str(FWU_CH_ALFA));
    TEST_ASSERT_EQUAL_STRING("wipe", fwu_layout_str(FWU_LAYOUT_WIPE));
    TEST_ASSERT_EQUAL_STRING("unknown", fwu_layout_str(FWU_LAYOUT_UNKNOWN));
}

/* ---- the manifest ---------------------------------------------------------------- */

static void test_manifest_parse_sorted(void)
{
    parse_ok(M1);
    TEST_ASSERT_EQUAL(5, s_m.n);
    TEST_ASSERT_EQUAL(0, s_m.dropped);
    TEST_ASSERT_EQUAL_STRING("wican_pro", s_m.device_type);
    TEST_ASSERT_EQUAL_STRING("WiCAN PRO", s_m.model);
    TEST_ASSERT_EQUAL_STRING("wican-fw_obd_pro", s_m.project_name);
    /* newest first whatever the document's order */
    TEST_ASSERT_EQUAL_STRING("6.2.0-alfa.1", s_m.rel[0].version_str);
    TEST_ASSERT_EQUAL_STRING("6.1.0", s_m.rel[1].version_str);
    TEST_ASSERT_EQUAL_STRING("6.1.0-beta.2", s_m.rel[2].version_str);
    TEST_ASSERT_EQUAL_STRING("6.0.0-alfa.6", s_m.rel[3].version_str);
    TEST_ASSERT_EQUAL_STRING("4.51.0", s_m.rel[4].version_str);
    TEST_ASSERT_EQUAL_STRING("v4.51p", s_m.rel[4].fw_version);
    TEST_ASSERT_EQUAL(FWU_CH_STABLE, s_m.rel[4].channel);
    TEST_ASSERT_EQUAL(3570592, s_m.rel[4].size);
    TEST_ASSERT_EQUAL_STRING(SHA, s_m.rel[4].sha256);
    TEST_ASSERT_EQUAL_STRING(MD5_V4, s_m.rel[4].layout_md5);
    TEST_ASSERT_EQUAL(0, s_m.rel[4].n_from);
    TEST_ASSERT_EQUAL(1, s_m.rel[1].n_from);
    TEST_ASSERT_EQUAL_STRING(MD5_V4, s_m.rel[1].from[0].md5);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_WIPE, s_m.rel[1].from[0].migration);
    TEST_ASSERT_EQUAL_STRING("storage", s_m.rel[1].from[0].wipes);
    TEST_ASSERT_EQUAL_STRING("Release 6.1.0.", s_m.rel[1].summary);
    TEST_ASSERT_EQUAL_STRING("wican_pro/firmware/notes/6.1.0.md", s_m.rel[1].notes);
    TEST_ASSERT_FALSE(s_m.rel[1].has_min);
}

static void test_manifest_refusals(void)
{
    char err[64];
    static const char no_schema[] = "{\"device_type\":\"wican_pro\",\"releases\":[]}";
    static const char schema2[] = "{\"schema\":2,\"device_type\":\"wican_pro\",\"releases\":[]}";
    static const char other[] = HEAD("espnetlink") REL("1.23.0", "1.23.0", "alfa", "false", "") TAIL;
    static const char no_rel[] = "{\"schema\":1,\"device_type\":\"wican_pro\"}";
    static const char empty[] = HEAD("wican_pro") TAIL;

    TEST_ASSERT_EQUAL(FWU_PARSE_INVALID,
                      fwu_manifest_parse("not json", 8, "wican_pro", false,
                                         &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(FWU_PARSE_INVALID,
                      fwu_manifest_parse(no_schema, strlen(no_schema), "wican_pro",
                                         false, &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(FWU_PARSE_INVALID,
                      fwu_manifest_parse(schema2, strlen(schema2), "wican_pro",
                                         false, &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("schema is not 1", err);
    TEST_ASSERT_EQUAL(FWU_PARSE_WRONG_DEVICE,
                      fwu_manifest_parse(other, strlen(other), "wican_pro",
                                         false, &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(FWU_PARSE_INVALID,
                      fwu_manifest_parse(no_rel, strlen(no_rel), "wican_pro",
                                         false, &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(FWU_PARSE_EMPTY,
                      fwu_manifest_parse(empty, strlen(empty), "wican_pro",
                                         false, &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(FWU_PARSE_INVALID,
                      fwu_manifest_parse(NULL, 0, "wican_pro", false, &s_m,
                                         err, sizeof(err)));
}

static void test_manifest_drops_a_malformed_release(void)
{
    /* http refused unless allowed, size 0, a short sha256, a bad version, a
       requires that is not below the version, a wipe without wipes: each
       drops its release and nothing else */
    static const char doc[] = HEAD("wican_pro")
        "{\"version\":\"6.1.0\",\"fw_version\":\"6.1.0\",\"channel\":\"stable\","
        "\"date\":\"2026-10-20\",\"summary\":\"s\",\"notes\":\"n.md\","
        IMG("6.1.0", "http://10.42.1.1:8446/f.bin", "3000000") LAYOUT(MD5_V6) "\"yanked\":false},"
        "{\"version\":\"6.0.9\",\"fw_version\":\"6.0.9\",\"channel\":\"stable\","
        "\"date\":\"2026-10-20\",\"summary\":\"s\",\"notes\":\"n.md\","
        IMG("6.0.9", "https://h/f.bin", "0") LAYOUT(MD5_V6) "\"yanked\":false},"
        "{\"version\":\"6.0.8\",\"fw_version\":\"6.0.8\",\"channel\":\"stable\","
        "\"date\":\"2026-10-20\",\"summary\":\"s\",\"notes\":\"n.md\","
        "\"image\":{\"url\":\"https://h/f.bin\",\"size\":3000000,\"sha256\":\"abcd\"},"
        LAYOUT(MD5_V6) "\"yanked\":false},"
        REL("6.00.7", "6.0.7", "stable", "false", "") ","
        REL("6.0.6", "6.0.6", "stable", "false", "\"requires\":{\"min_version\":\"6.0.6\"},") ","
        REL("6.0.5", "6.0.5", "stable", "false",
            "\"layout_from\":{\"" MD5_V4 "\":{\"migration\":\"wipe\"}},") ","
        REL("6.0.4", "6.0.4", "stable", "false", "")
        TAIL;
    char err[64];

    TEST_ASSERT_EQUAL(FWU_PARSE_OK,
                      fwu_manifest_parse(doc, strlen(doc), "wican_pro", false,
                                         &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(2, s_m.n);         /* 6.0.5 and 6.0.4 */
    TEST_ASSERT_EQUAL(5, s_m.dropped);   /* http, size 0, short sha, 6.00.7,
                                            the requires not below 6.0.6 */
    TEST_ASSERT_EQUAL_STRING("6.0.5", s_m.rel[0].version_str);
    TEST_ASSERT_EQUAL(0, s_m.rel[0].n_from); /* the wipe without wipes */
    TEST_ASSERT_EQUAL_STRING("6.0.4", s_m.rel[1].version_str);

    /* the same document with http allowed keeps the first release */
    TEST_ASSERT_EQUAL(FWU_PARSE_OK,
                      fwu_manifest_parse(doc, strlen(doc), "wican_pro", true,
                                         &s_m, err, sizeof(err)));
    TEST_ASSERT_EQUAL(3, s_m.n);
    TEST_ASSERT_EQUAL_STRING("6.1.0", s_m.rel[0].version_str);
    TEST_ASSERT_EQUAL_STRING("http://10.42.1.1:8446/f.bin", s_m.rel[0].url);
}

static void test_manifest_bounds(void)
{
    /* 70 releases: the newest 64 are kept, the summary is cut at 140 */
    static char doc[70 * 520 + 600];
    char long_summary[200];
    size_t used;

    memset(long_summary, 'a', sizeof(long_summary) - 1);
    long_summary[sizeof(long_summary) - 1] = '\0';
    used = (size_t)snprintf(doc, sizeof(doc), "%s", HEAD("wican_pro"));

    for (int i = 0; i < 70; i++)
    {
        used += (size_t)snprintf(doc + used, sizeof(doc) - used,
            "%s{\"version\":\"6.%d.0\",\"fw_version\":\"6.%d.0\",\"channel\":\"stable\","
            "\"date\":\"2026-10-20\",\"summary\":\"%s\",\"notes\":\"n.md\","
            "\"image\":{\"url\":\"https://h/%d.bin\",\"size\":3000000,\"sha256\":\"" SHA "\"},"
            LAYOUT(MD5_V6) "\"yanked\":false}",
            i ? "," : "", i, i, long_summary, i);
    }

    snprintf(doc + used, sizeof(doc) - used, "%s", TAIL);
    parse_ok(doc);
    TEST_ASSERT_EQUAL(FWU_MAX_RELEASES, s_m.n);
    TEST_ASSERT_EQUAL(6, s_m.dropped);
    TEST_ASSERT_EQUAL(FWU_SUMMARY_LEN - 1, strlen(s_m.rel[0].summary));
    TEST_ASSERT_EQUAL_STRING("6.63.0", s_m.rel[0].version_str);
    TEST_ASSERT_EQUAL_STRING("6.0.0", s_m.rel[63].version_str);
}

/* ---- the selection -------------------------------------------------------------- */

static void test_select_up_to_date_and_latest(void)
{
    fwu_selection_t sel;

    parse_ok(M1);

    fwu_select(&s_m, FWU_CH_STABLE, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available);
    TEST_ASSERT_EQUAL_STRING("up_to_date", sel.reason);
    TEST_ASSERT_TRUE(sel.installed_known);
    TEST_ASSERT_EQUAL(1, sel.me);
    TEST_ASSERT_EQUAL(1, sel.latest);  /* 6.1.0 is the stable latest */

    /* the alfa unit on the alfa channel: the newest alfa */
    fwu_select(&s_m, FWU_CH_ALFA, "v6.00p_alfa-06", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_TRUE(sel.installable);
    TEST_ASSERT_TRUE(sel.installed_known);
    TEST_ASSERT_FALSE(sel.dev_build);
    TEST_ASSERT_EQUAL_STRING("6.2.0-alfa.1", s_m.rel[sel.target].version_str);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_NONE, sel.verdict);
    TEST_ASSERT_FALSE(sel.step);

    /* the same unit on the stable channel: 6.1.0 */
    fwu_select(&s_m, FWU_CH_STABLE, "v6.00p_alfa-06", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_EQUAL_STRING("6.1.0", s_m.rel[sel.target].version_str);

    /* beta sees the beta, not the alfa */
    fwu_select(&s_m, FWU_CH_BETA, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available); /* 6.1.0-beta.2 < 6.1.0 */
    fwu_select(&s_m, FWU_CH_BETA, "6.0.0", MD5_V6, &sel);
    TEST_ASSERT_EQUAL_STRING("6.1.0", s_m.rel[sel.target].version_str);
}

static void test_select_dev_build_and_unknown(void)
{
    fwu_selection_t sel;

    parse_ok(M1);

    /* a dev build of the latest: equal, not an update */
    fwu_select(&s_m, FWU_CH_ALFA, "v6.2.0p-alfa.1-3-gabcdef0-dirty", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available);
    TEST_ASSERT_TRUE(sel.dev_build);
    TEST_ASSERT_FALSE(sel.installed_known);
    TEST_ASSERT_EQUAL_STRING("up_to_date", sel.reason);

    /* a dev build of an older base sees the latest */
    fwu_select(&s_m, FWU_CH_ALFA, "v6.1.0p-2-g1234567", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_TRUE(sel.dev_build);
    TEST_ASSERT_EQUAL_STRING("6.2.0-alfa.1", s_m.rel[sel.target].version_str);

    /* a string nobody can place: nothing is offered, the latest is shown */
    fwu_select(&s_m, FWU_CH_ALFA, "nogit", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available);
    TEST_ASSERT_FALSE(sel.installed_parsed);
    TEST_ASSERT_EQUAL_STRING("unknown_version", sel.reason);
    TEST_ASSERT_EQUAL(0, sel.latest);

    /* an empty manifest view */
    fwu_select(NULL, FWU_CH_ALFA, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_EQUAL_STRING("no_release", sel.reason);
    TEST_ASSERT_EQUAL(-1, sel.latest);
}

static void test_select_layout_verdicts(void)
{
    fwu_selection_t sel;

    parse_ok(M1);

    /* a factory v4 unit: 6.1.0 wipes storage on the way */
    fwu_select(&s_m, FWU_CH_STABLE, "v4.51p", MD5_V4, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_TRUE(sel.installable);
    TEST_ASSERT_TRUE(sel.installed_known);
    TEST_ASSERT_EQUAL_STRING("6.1.0", s_m.rel[sel.target].version_str);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_WIPE, sel.verdict);
    TEST_ASSERT_EQUAL_STRING("storage", sel.wipes);

    /* a layout the manifest does not know */
    fwu_select(&s_m, FWU_CH_STABLE, "v4.51p",
               "deadbeefdeadbeefdeadbeefdeadbeef", &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_FALSE(sel.installable);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_UNKNOWN, sel.verdict);
    TEST_ASSERT_EQUAL_STRING("layout_unknown", sel.reason);

    /* no MD5 entry in flash at all */
    fwu_select(&s_m, FWU_CH_STABLE, "v4.51p", "", &sel);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_UNKNOWN, sel.verdict);

    /* serial and lossless, per release */
    static const char doc[] = HEAD("wican_pro")
        REL("6.2.0", "6.2.0", "stable", "false", FROM_SERIAL) ","
        REL("6.1.0", "6.1.0", "stable", "false", FROM_LOSSLESS)
        TAIL;
    const char *wipes = NULL;

    parse_ok(doc);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_SERIAL, fwu_layout_verdict(&s_m.rel[0], MD5_V4, &wipes));
    TEST_ASSERT_EQUAL_STRING("", wipes);
    TEST_ASSERT_EQUAL(FWU_LAYOUT_LOSSLESS, fwu_layout_verdict(&s_m.rel[1], MD5_V4, &wipes));
    TEST_ASSERT_EQUAL(FWU_LAYOUT_NONE, fwu_layout_verdict(&s_m.rel[1], MD5_V6, NULL));
    fwu_select(&s_m, FWU_CH_STABLE, "6.0.0", MD5_V4, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_FALSE(sel.installable);
    TEST_ASSERT_EQUAL_STRING("layout_serial", sel.reason);
}

static void test_select_stepping(void)
{
    fwu_selection_t sel;

    parse_ok(M2);
    fwu_select(&s_m, FWU_CH_STABLE, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_TRUE(sel.step);
    TEST_ASSERT_EQUAL_STRING("6.2.0", s_m.rel[sel.target].version_str);
    TEST_ASSERT_EQUAL_STRING("6.3.0", s_m.rel[sel.latest].version_str);

    fwu_select(&s_m, FWU_CH_STABLE, "6.2.0", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.step);
    TEST_ASSERT_EQUAL_STRING("6.3.0", s_m.rel[sel.target].version_str);

    /* the stone yanked: no path, the latest still named */
    parse_ok(M3);
    fwu_select(&s_m, FWU_CH_STABLE, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available);
    TEST_ASSERT_EQUAL(-1, sel.target);
    TEST_ASSERT_EQUAL_STRING("no_path", sel.reason);
    TEST_ASSERT_EQUAL_STRING("6.3.0", s_m.rel[sel.latest].version_str);
}

static void test_select_yanked(void)
{
    fwu_selection_t sel;

    parse_ok(M4);

    /* the newest is yanked: 6.2.0 is the latest */
    fwu_select(&s_m, FWU_CH_STABLE, "6.1.0", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_EQUAL_STRING("6.2.0", s_m.rel[sel.target].version_str);

    /* a unit on the yanked release is told to move, downwards */
    fwu_select(&s_m, FWU_CH_STABLE, "6.3.0", MD5_V6, &sel);
    TEST_ASSERT_TRUE(sel.available);
    TEST_ASSERT_TRUE(sel.installed_yanked);
    TEST_ASSERT_TRUE(sel.installed_known);
    TEST_ASSERT_EQUAL_STRING("6.2.0", s_m.rel[sel.target].version_str);
    TEST_ASSERT_EQUAL_STRING("bad", s_m.rel[sel.me].yank_reason);

    /* 6.2.0 itself: up to date */
    fwu_select(&s_m, FWU_CH_STABLE, "6.2.0", MD5_V6, &sel);
    TEST_ASSERT_FALSE(sel.available);
    TEST_ASSERT_EQUAL_STRING("up_to_date", sel.reason);

    TEST_ASSERT_EQUAL(0, fwu_find_version(&s_m, "6.3.0"));
    TEST_ASSERT_EQUAL(1, fwu_find_version(&s_m, "6.2.0"));
    TEST_ASSERT_EQUAL(-1, fwu_find_version(&s_m, "6.9.0"));
    TEST_ASSERT_EQUAL(-1, fwu_find_version(&s_m, NULL));
}

/* ---- URLs ----------------------------------------------------------------------- */

static void test_urls(void)
{
    bool http = true;
    char out[FWU_URL_LEN];

    TEST_ASSERT_TRUE(fwu_base_url_valid(
        "https://raw.githubusercontent.com/meatpiHQ/meatpi-devices/main/", &http));
    TEST_ASSERT_FALSE(http);
    TEST_ASSERT_TRUE(fwu_base_url_valid("http://10.42.1.1:8446/", &http));
    TEST_ASSERT_TRUE(http);
    TEST_ASSERT_FALSE(fwu_base_url_valid("https://x.y", NULL));      /* no slash */
    TEST_ASSERT_FALSE(fwu_base_url_valid("ftp://x.y/", NULL));
    TEST_ASSERT_FALSE(fwu_base_url_valid("https:///", NULL));        /* no host */
    TEST_ASSERT_FALSE(fwu_base_url_valid("https://x y/", NULL));     /* a space */
    TEST_ASSERT_FALSE(fwu_base_url_valid(NULL, NULL));

    TEST_ASSERT_TRUE(fwu_compose_url("https://h/m/", "wican_pro/firmware/manifest.json",
                                     out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/m/wican_pro/firmware/manifest.json", out);
    TEST_ASSERT_TRUE(fwu_compose_url("https://h/m/", "/notes/6.1.0.md", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/m/notes/6.1.0.md", out);
    TEST_ASSERT_FALSE(fwu_compose_url("https://h/m/", "x", out, 12));
}

/* ---- the record ----------------------------------------------------------------- */

static void test_record_codec(void)
{
    fwu_record_t r;

    TEST_ASSERT_EQUAL(FWU_RECORD_LEN, sizeof(r));
    memset(&r, 0, sizeof(r));
    r.job_id = 7;
    r.state = FW_JOB_REBOOTING;
    r.slot_subtype = 0x11;
    snprintf(r.from_version, sizeof(r.from_version), "6.0.3");
    snprintf(r.to_version, sizeof(r.to_version), "6.1.0");
    TEST_ASSERT_FALSE(fwu_record_valid(&r));
    fwu_record_seal(&r);
    TEST_ASSERT_TRUE(fwu_record_valid(&r));
    TEST_ASSERT_EQUAL_HEX32(FWU_RECORD_MAGIC, r.magic);
    TEST_ASSERT_EQUAL(1, r.version);

    r.to_version[0] = '7'; /* a flipped byte */
    TEST_ASSERT_FALSE(fwu_record_valid(&r));
    r.to_version[0] = '6';
    TEST_ASSERT_TRUE(fwu_record_valid(&r));
    r.version = 2;
    TEST_ASSERT_FALSE(fwu_record_valid(&r));
    fwu_record_seal(&r); /* seal pins the version */
    TEST_ASSERT_TRUE(fwu_record_valid(&r));
    r.len = 100;
    TEST_ASSERT_FALSE(fwu_record_valid(&r));
    TEST_ASSERT_FALSE(fwu_record_valid(NULL));

    /* a known CRC-32 vector */
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, fwu_crc32((const uint8_t *)"123456789", 9));
}

static void test_boot_decision_table(void)
{
    fwu_boot_in_t in;

    memset(&in, 0, sizeof(in));
    TEST_ASSERT_EQUAL(FWU_BOOT_NOTHING, fwu_boot_decide(&in));
    TEST_ASSERT_EQUAL(FWU_BOOT_NOTHING, fwu_boot_decide(NULL));

    in.have_record = true;
    in.rec_state = FW_JOB_VERIFIED;
    TEST_ASSERT_EQUAL(FWU_BOOT_NOTHING, fwu_boot_decide(&in));

    /* the new image is not running: the bootloader fell back */
    in.rec_state = FW_JOB_REBOOTING;
    in.rec_slot = 0x11;
    in.running_slot = 0x10;
    in.last_invalid_known = true;
    in.last_invalid_slot = 0x11;
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_ROLLED_BACK_BL, fwu_boot_decide(&in));

    /* or it never ran at all */
    in.last_invalid_known = false;
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_BOOT_FAILED, fwu_boot_decide(&in));
    in.last_invalid_known = true;
    in.last_invalid_slot = 0x10;
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_BOOT_FAILED, fwu_boot_decide(&in));

    /* the first boot of the new image: pending */
    in.running_slot = 0x11;
    in.latest_planned = true;
    in.latest_reason = FWU_REASON_OTA_APPLY;
    TEST_ASSERT_EQUAL(FWU_BOOT_KEEP_PENDING, fwu_boot_decide(&in));

    /* the partition rewrite's restart inside the first second: pending */
    in.latest_reason = FWU_REASON_PARTITION_MIGRATE;
    TEST_ASSERT_EQUAL(FWU_BOOT_KEEP_PENDING, fwu_boot_decide(&in));

    /* a layout-changing job is confirmed at its first boot */
    in.rec_flags = FWU_FLAG_LAYOUT_CHANGED;
    in.latest_reason = FWU_REASON_OTA_APPLY;
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_VERIFIED, fwu_boot_decide(&in));
    in.rec_flags = 0;

    /* a settings submit two minutes in: the run was operated */
    in.latest_reason = 2; /* CONFIG_APPLY */
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_VERIFIED, fwu_boot_decide(&in));

    /* a rollback or a park retry says nothing */
    in.latest_reason = FWU_REASON_OTA_ROLLBACK;
    TEST_ASSERT_EQUAL(FWU_BOOT_KEEP_PENDING, fwu_boot_decide(&in));
    in.latest_reason = FWU_REASON_PARK_RETRY;
    in.brake_parks = true;
    TEST_ASSERT_EQUAL(FWU_BOOT_ROLLBACK_APP, fwu_boot_decide(&in));

    /* three quick crashes: roll back; one crash: pending */
    in.latest_planned = false;
    in.latest_reason = 0;
    TEST_ASSERT_EQUAL(FWU_BOOT_ROLLBACK_APP, fwu_boot_decide(&in));
    in.brake_parks = false;
    TEST_ASSERT_EQUAL(FWU_BOOT_KEEP_PENDING, fwu_boot_decide(&in));

    /* a layout-changing job is never rolled back */
    in.brake_parks = true;
    in.rec_flags = FWU_FLAG_LAYOUT_CHANGED;
    TEST_ASSERT_EQUAL(FWU_BOOT_WRITE_VERIFIED, fwu_boot_decide(&in));
}

static void test_state_words(void)
{
    TEST_ASSERT_EQUAL_STRING("pending_verify", fwu_job_state_str(FW_JOB_PENDING_VERIFY));
    TEST_ASSERT_EQUAL_STRING("rolled_back", fwu_job_state_str(FW_JOB_ROLLED_BACK));
    TEST_ASSERT_EQUAL_STRING("idle", fwu_job_state_str(FW_JOB_IDLE));
    TEST_ASSERT_EQUAL_STRING("bootloader", fwu_by_str(FWU_BY_BOOTLOADER));
    TEST_ASSERT_EQUAL_STRING("app", fwu_by_str(FWU_BY_APP));
    TEST_ASSERT_EQUAL_STRING("", fwu_by_str(FWU_BY_NONE));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_strict_grammar);
    RUN_TEST(test_tolerant_grammar);
    RUN_TEST(test_version_order);
    RUN_TEST(test_channels);
    RUN_TEST(test_manifest_parse_sorted);
    RUN_TEST(test_manifest_refusals);
    RUN_TEST(test_manifest_drops_a_malformed_release);
    RUN_TEST(test_manifest_bounds);
    RUN_TEST(test_select_up_to_date_and_latest);
    RUN_TEST(test_select_dev_build_and_unknown);
    RUN_TEST(test_select_layout_verdicts);
    RUN_TEST(test_select_stepping);
    RUN_TEST(test_select_yanked);
    RUN_TEST(test_urls);
    RUN_TEST(test_image_head_parse);
    RUN_TEST(test_record_codec);
    RUN_TEST(test_boot_decision_table);
    RUN_TEST(test_state_words);
    UNITY_END();
}
