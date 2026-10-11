/**
 * @file test_main.c
 * @brief Host tests for cmdline_manager's pure pieces: the line
 *        assembler (every transport chunking pattern, whole lines, split
 *        lines, CRLF, batches, oversize floods, must come out as clean
 *        lines) and the dispatcher's verdict line (what a client is owed
 *        after a command; issue #980: a console failure must not be
 *        silent). Expected: 13 Tests 0 Failures 0 Ignored.
 */
#include <string.h>

#include "unity.h"

#include "cmdline_manager_private.h"

static cm_line_asm_t s_asm;
static char s_verdict[CM_VERDICT_BUF];

void setUp(void)
{
    cm_line_reset(&s_asm);
    memset(s_verdict, 'x', sizeof(s_verdict)); /* silence must write "" */
}

void tearDown(void)
{
}

/** Feed everything, collecting completed lines into @p out[]. */
static int feed_all(const char *bytes, const char *out[], int max)
{
    size_t len = strlen(bytes);
    size_t off = 0;
    int found = 0;
    static char store[8][256];

    while (off < len)
    {
        const char *line = NULL;

        off += cm_line_feed(&s_asm, (const uint8_t *)bytes + off,
                            len - off, &line);

        if (line != NULL && found < max)
        {
            snprintf(store[found], sizeof(store[found]), "%s", line);
            out[found] = store[found];
            found++;
        }
    }

    return found;
}

static void test_simple_line(void)
{
    const char *lines[4];

    TEST_ASSERT_EQUAL_INT(1, feed_all("version\n", lines, 4));
    TEST_ASSERT_EQUAL_STRING("version", lines[0]);
}

static void test_crlf_yields_one_line(void)
{
    const char *lines[4];

    /* the \r terminates; the \n tail must NOT become an empty line */
    TEST_ASSERT_EQUAL_INT(1, feed_all("status\r\n", lines, 4));
    TEST_ASSERT_EQUAL_STRING("status", lines[0]);
}

static void test_split_across_chunks(void)
{
    const char *line = NULL;

    /* transports chunk arbitrarily: "ver" now, "sion\n" later */
    cm_line_feed(&s_asm, (const uint8_t *)"ver", 3, &line);
    TEST_ASSERT_NULL(line);
    cm_line_feed(&s_asm, (const uint8_t *)"sion\n", 5, &line);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING("version", line);
}

static void test_batch_of_lines_one_chunk(void)
{
    const char *lines[4];

    TEST_ASSERT_EQUAL_INT(3, feed_all("help\nrtc sync\r\nimu\n",
                                      lines, 4));
    TEST_ASSERT_EQUAL_STRING("help", lines[0]);
    TEST_ASSERT_EQUAL_STRING("rtc sync", lines[1]);
    TEST_ASSERT_EQUAL_STRING("imu", lines[2]);
}

static void test_empty_lines_skipped(void)
{
    const char *lines[4];

    /* a terminal sending bare newlines must not run empty commands */
    TEST_ASSERT_EQUAL_INT(1, feed_all("\n\r\n\nled\n", lines, 4));
    TEST_ASSERT_EQUAL_STRING("led", lines[0]);
}

static void test_overflow_discarded_then_recovers(void)
{
    static char flood[400];
    const char *lines[4];

    memset(flood, 'x', sizeof(flood) - 2);
    flood[sizeof(flood) - 2] = '\n';
    flood[sizeof(flood) - 1] = '\0';

    /* an oversize line (buf is 256) is dropped whole... */
    TEST_ASSERT_EQUAL_INT(0, feed_all(flood, lines, 4));
    /* ...and the assembler is clean for the next command */
    TEST_ASSERT_EQUAL_INT(1, feed_all("battery\n", lines, 4));
    TEST_ASSERT_EQUAL_STRING("battery", lines[0]);
}

/* ---- the dispatcher's verdict line --------------------------------------------- */

static void test_verdict_success_is_silent(void)
{
    /* the command ran and returned 0: its own output is the answer */
    TEST_ASSERT_EQUAL_INT(0, (int)cm_exec_verdict(ESP_OK, 0, "version",
                                                  s_verdict,
                                                  sizeof(s_verdict)));
    TEST_ASSERT_EQUAL_STRING("", s_verdict);
}

static void test_verdict_empty_line_is_silent(void)
{
    /* esp_console_run: ESP_ERR_INVALID_ARG = whitespace only, nothing ran */
    TEST_ASSERT_EQUAL_INT(0, (int)cm_exec_verdict(ESP_ERR_INVALID_ARG, 0,
                                                  "  ", s_verdict,
                                                  sizeof(s_verdict)));
    TEST_ASSERT_EQUAL_STRING("", s_verdict);
}

static void test_verdict_unknown_command(void)
{
    size_t n = cm_exec_verdict(ESP_ERR_NOT_FOUND, 0, "bogus 1 2",
                               s_verdict, sizeof(s_verdict));

    TEST_ASSERT_EQUAL_STRING("Unknown command: bogus 1 2\nTry: help\n",
                             s_verdict);
    TEST_ASSERT_EQUAL_INT((int)strlen(s_verdict), (int)n);
}

static void test_verdict_longest_unknown_line_fits(void)
{
    /* the assembler's longest line (255 chars) keeps both its lines:
     * cmdline_printf's 256-byte buffer used to cut the newline off */
    static char line[256];
    memset(line, 'q', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    size_t n = cm_exec_verdict(ESP_ERR_NOT_FOUND, 0, line, s_verdict,
                               sizeof(s_verdict));

    TEST_ASSERT_EQUAL_INT(17 + 255 + 11, (int)n);
    TEST_ASSERT_EQUAL_STRING("\nTry: help\n", s_verdict + 17 + 255);
}

static void test_verdict_handler_failure_names_the_code(void)
{
    cm_exec_verdict(ESP_OK, 3, "rtc -x", s_verdict, sizeof(s_verdict));
    TEST_ASSERT_EQUAL_STRING("command failed (3)\n", s_verdict);
}

static void test_verdict_console_failure_is_printed(void)
{
    /* issue #980: esp_console_run failing on its own (no memory for the
     * argument vector, console not initialised) printed NOTHING, so the
     * command looked as if it had worked. The verdict names the error
     * and says the command did not run. */
    size_t n = cm_exec_verdict(ESP_ERR_NO_MEM, 0, "version", s_verdict,
                               sizeof(s_verdict));

    TEST_ASSERT_TRUE_MESSAGE(n > 0,
                             "a console failure printed nothing (issue #980)");
    TEST_ASSERT_NOT_NULL(strstr(s_verdict, "Internal error"));
    TEST_ASSERT_NOT_NULL(strstr(s_verdict, "ESP_ERR_NO_MEM"));
    TEST_ASSERT_NOT_NULL(strstr(s_verdict, "0x101"));
    TEST_ASSERT_NOT_NULL(strstr(s_verdict, "did not run"));
    TEST_ASSERT_EQUAL_CHAR('\n', s_verdict[n - 1]);

    cm_exec_verdict(ESP_ERR_INVALID_STATE, 0, "version", s_verdict,
                    sizeof(s_verdict));
    TEST_ASSERT_NOT_NULL(strstr(s_verdict, "ESP_ERR_INVALID_STATE"));
}

static void test_verdict_truncates_to_the_buffer(void)
{
    char small[8];
    size_t n = cm_exec_verdict(ESP_ERR_NOT_FOUND, 0, "bogus", small,
                               sizeof(small));

    TEST_ASSERT_EQUAL_INT(7, (int)n);
    TEST_ASSERT_EQUAL_STRING("Unknown", small);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simple_line);
    RUN_TEST(test_crlf_yields_one_line);
    RUN_TEST(test_split_across_chunks);
    RUN_TEST(test_batch_of_lines_one_chunk);
    RUN_TEST(test_empty_lines_skipped);
    RUN_TEST(test_overflow_discarded_then_recovers);
    RUN_TEST(test_verdict_success_is_silent);
    RUN_TEST(test_verdict_empty_line_is_silent);
    RUN_TEST(test_verdict_unknown_command);
    RUN_TEST(test_verdict_longest_unknown_line_fits);
    RUN_TEST(test_verdict_handler_failure_names_the_code);
    RUN_TEST(test_verdict_console_failure_is_printed);
    RUN_TEST(test_verdict_truncates_to_the_buffer);
    UNITY_END();
}
