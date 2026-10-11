/**
 * @file test_main.c
 * @brief Host tests for ip4_routes' pure core: the CIDR parser and
 *        formatter, the list parser, the table's set / clear / match.
 *        Expected output: 16 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "ip4_routes_private.h"

#define IP(a, b, c, d) ((uint32_t)(((a) << 24) | ((b) << 16) | ((c) << 8) | (d)))

static ip4_routes_slot_t s_table[IP4_ROUTES_MAX];
static int s_owner_a, s_owner_b; /* any distinct addresses do */

void setUp(void)
{
    memset(s_table, 0, sizeof(s_table));
}

void tearDown(void)
{
}

static void test_parse_cidr_plain(void)
{
    ip4_routes_entry_t e;

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.66.0.0/24", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(10, 66, 0, 0), e.network);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFF00u, e.mask);

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("192.168.0.0/16", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 0, 0), e.network);
    TEST_ASSERT_EQUAL_HEX32(0xFFFF0000u, e.mask);

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("0.0.0.0/0", &e));
    TEST_ASSERT_EQUAL_HEX32(0u, e.network);
    TEST_ASSERT_EQUAL_HEX32(0u, e.mask);
}

static void test_parse_cidr_clears_host_bits(void)
{
    ip4_routes_entry_t e;

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("192.168.1.77/24", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 1, 0), e.network);
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.255.255.255/8", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(10, 0, 0, 0), e.network);
}

static void test_parse_cidr_bare_address_and_spaces(void)
{
    ip4_routes_entry_t e;

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.66.0.1", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(10, 66, 0, 1), e.network);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, e.mask);
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("  10.66.0.0/24  ", &e));
    TEST_ASSERT_EQUAL_HEX32(IP(10, 66, 0, 0), e.network);
}

static void test_parse_cidr_rejects(void)
{
    ip4_routes_entry_t e;

    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr(NULL, &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0/24", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0.1/24", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0/33", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0/", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0/-1", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0/024", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("256.1.1.1/8", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("a.b.c.d/8", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("10.66.0.0 /24", &e));
    TEST_ASSERT_FALSE(ip4_routes_parse_cidr("fe80::1/64", &e));
}

static void test_parse_list_counts_and_separators(void)
{
    ip4_routes_entry_t out[8];
    char err[64];

    int n = ip4_routes_parse_list("10.66.0.0/24, 192.168.1.0/24 10.0.0.0/8",
                                  out, 8, err, sizeof(err));

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_HEX32(IP(10, 66, 0, 0), out[0].network);
    TEST_ASSERT_EQUAL_HEX32(IP(192, 168, 1, 0), out[1].network);
    TEST_ASSERT_EQUAL_HEX32(0xFF000000u, out[2].mask);
    TEST_ASSERT_EQUAL_STRING("", err);

    TEST_ASSERT_EQUAL_INT(2, ip4_routes_parse_list(" 10.66.0.0/24 ;\n 10.77.0.0/24 ,",
                                                   out, 8, err, sizeof(err)));
}

static void test_parse_list_empty(void)
{
    ip4_routes_entry_t out[2];
    char err[64];

    TEST_ASSERT_EQUAL_INT(0, ip4_routes_parse_list("", out, 2, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(0, ip4_routes_parse_list("  , ", out, 2, err, sizeof(err)));
    TEST_ASSERT_EQUAL_INT(0, ip4_routes_parse_list(NULL, out, 2, err, sizeof(err)));
}

static void test_parse_list_names_the_bad_token(void)
{
    ip4_routes_entry_t out[8];
    char err[64];

    TEST_ASSERT_EQUAL_INT(-1, ip4_routes_parse_list("10.66.0.0/24, nonsense",
                                                    out, 8, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "'nonsense'"));
    TEST_ASSERT_NOT_NULL(strstr(err, "CIDR"));
}

static void test_parse_list_too_many(void)
{
    ip4_routes_entry_t out[2];
    char err[64];

    TEST_ASSERT_EQUAL_INT(-1, ip4_routes_parse_list(
        "10.0.0.0/8, 10.1.0.0/16, 10.2.0.0/16", out, 2, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "more than 2"));
}

static void test_parse_list_count_only(void)
{
    /* out == NULL only counts (a validator that wants the number) */
    char err[64];

    TEST_ASSERT_EQUAL_INT(2, ip4_routes_parse_list("10.0.0.0/8 10.1.0.0/16",
                                                   NULL, 8, err, sizeof(err)));
}

static void test_format_round_trip(void)
{
    ip4_routes_entry_t e;
    char out[24];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("192.168.1.77/24", &e));
    ip4_routes_format(&e, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("192.168.1.0/24", out);

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.66.0.1", &e));
    ip4_routes_format(&e, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("10.66.0.1/32", out);

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("0.0.0.0/0", &e));
    ip4_routes_format(&e, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("0.0.0.0/0", out);
}

static void test_match_longest_prefix(void)
{
    ip4_routes_entry_t a[1], b[1];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.0.0.0/8", &a[0]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.66.0.0/24", &b[0]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_a, a, 1);

    TEST_ASSERT_EQUAL_INT(1, n);
    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_b, b, 1);
    TEST_ASSERT_EQUAL_INT(2, n);

    TEST_ASSERT_EQUAL_PTR(&s_owner_b, ip4_routes_core_match(s_table, 2, IP(10, 66, 0, 5)));
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, 2, IP(10, 1, 2, 3)));
    TEST_ASSERT_NULL(ip4_routes_core_match(s_table, 2, IP(192, 168, 1, 1)));
    TEST_ASSERT_NULL(ip4_routes_core_match(s_table, 0, IP(10, 66, 0, 5)));
}

static void test_match_default_route_entry(void)
{
    ip4_routes_entry_t e[2];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("0.0.0.0/0", &e[0]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.66.0.0/24", &e[1]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_a, e, 2);

    TEST_ASSERT_EQUAL_INT(2, n);
    /* everything matches the /0; the /24 still wins where it applies */
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, 2, IP(8, 8, 8, 8)));
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, 2, IP(10, 66, 0, 9)));
}

static void test_set_replaces_an_owners_routes(void)
{
    ip4_routes_entry_t one[1], two[2], other[1];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.1.0.0/16", &one[0]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.2.0.0/16", &two[0]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.3.0.0/16", &two[1]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("172.16.0.0/12", &other[0]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_b, other, 1);

    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_a, one, 1);
    TEST_ASSERT_EQUAL_INT(2, n);
    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_a, two, 2);
    TEST_ASSERT_EQUAL_INT(3, n);
    /* the old route of that owner is gone, the other owner's stays */
    TEST_ASSERT_NULL(ip4_routes_core_match(s_table, 3, IP(10, 1, 5, 5)));
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, 3, IP(10, 3, 5, 5)));
    TEST_ASSERT_EQUAL_PTR(&s_owner_b, ip4_routes_core_match(s_table, 3, IP(172, 16, 5, 5)));

    /* n == 0 clears that owner only */
    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_a, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_PTR(&s_owner_b, ip4_routes_core_match(s_table, 1, IP(172, 16, 5, 5)));
}

static void test_clear_drops_only_the_owner(void)
{
    ip4_routes_entry_t a[2], b[1];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.1.0.0/16", &a[0]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.2.0.0/16", &a[1]));
    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.3.0.0/16", &b[0]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_a, a, 2);

    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_b, b, 1);
    TEST_ASSERT_EQUAL_INT(3, n);

    size_t left = ip4_routes_core_clear(s_table, (size_t)n, &s_owner_a);

    TEST_ASSERT_EQUAL_UINT(1, left);
    TEST_ASSERT_EQUAL_PTR(&s_owner_b, ip4_routes_core_match(s_table, left, IP(10, 3, 1, 1)));
    TEST_ASSERT_NULL(ip4_routes_core_match(s_table, left, IP(10, 1, 1, 1)));
    /* clearing an owner that has nothing changes nothing */
    TEST_ASSERT_EQUAL_UINT(1, ip4_routes_core_clear(s_table, left, &s_owner_a));
}

static void test_full_table_refused_untouched(void)
{
    ip4_routes_entry_t many[IP4_ROUTES_MAX];
    ip4_routes_entry_t one[1];

    for (int i = 0; i < IP4_ROUTES_MAX; i++)
    {
        many[i].network = IP(10, (uint32_t)i, 0, 0);
        many[i].mask = 0xFFFF0000u;
    }

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("172.16.0.0/12", &one[0]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_a, many,
                                IP4_ROUTES_MAX);

    TEST_ASSERT_EQUAL_INT(IP4_ROUTES_MAX, n);
    /* no room for another owner: refused, the table as it was */
    TEST_ASSERT_EQUAL_INT(-1, ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX,
                                                  &s_owner_b, one, 1));
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, (size_t)n, IP(10, 7, 1, 1)));
    /* the same owner may replace its own full set */
    TEST_ASSERT_EQUAL_INT(1, ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX,
                                                 &s_owner_a, one, 1));
}

static void test_match_ties_take_the_first(void)
{
    ip4_routes_entry_t same[1];

    TEST_ASSERT_TRUE(ip4_routes_parse_cidr("10.5.0.0/16", &same[0]));

    int n = ip4_routes_core_set(s_table, 0, IP4_ROUTES_MAX, &s_owner_a, same, 1);

    n = ip4_routes_core_set(s_table, (size_t)n, IP4_ROUTES_MAX, &s_owner_b, same, 1);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_PTR(&s_owner_a, ip4_routes_core_match(s_table, 2, IP(10, 5, 9, 9)));
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_cidr_plain);
    RUN_TEST(test_parse_cidr_clears_host_bits);
    RUN_TEST(test_parse_cidr_bare_address_and_spaces);
    RUN_TEST(test_parse_cidr_rejects);
    RUN_TEST(test_parse_list_counts_and_separators);
    RUN_TEST(test_parse_list_empty);
    RUN_TEST(test_parse_list_names_the_bad_token);
    RUN_TEST(test_parse_list_too_many);
    RUN_TEST(test_parse_list_count_only);
    RUN_TEST(test_format_round_trip);
    RUN_TEST(test_match_longest_prefix);
    RUN_TEST(test_match_default_route_entry);
    RUN_TEST(test_set_replaces_an_owners_routes);
    RUN_TEST(test_clear_drops_only_the_owner);
    RUN_TEST(test_full_table_refused_untouched);
    RUN_TEST(test_match_ties_take_the_first);
    UNITY_END();
}
