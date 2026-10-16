/**
 * @file test_main.c
 * @brief Host suite for the pure slcan codec (translator_slcan_codec.c):
 *        encode (ASCII→CAN-wire) and decode (CAN-wire→ASCII) round-trips for
 *        11/29-bit, RTR, dlc 0..8; input fragmented at every byte boundary;
 *        one chunk carrying several frames; malformed input rejected with a
 *        clean ctx; control commands absorbed.
 */
#include <string.h>

#include "unity.h"

#include "bridge_manager.h"
#include "can_frame_wire.h"

#include "translator_slcan_private.h"

extern const bridge_translator_t translator_slcan_desc;

/* ---- sink collectors --------------------------------------------------- */

/* encode emits CAN-wire chunks; collect them decoded back into frames */
#define MAXF 16
static can_core_frame_t g_frames[MAXF];
static int g_nframes;

static esp_err_t frame_sink(void *arg, const uint8_t *out, size_t len)
{
    (void)arg;
    if (g_nframes < MAXF && can_wire_decode(out, len, &g_frames[g_nframes]))
    {
        g_nframes++;
    }
    return ESP_OK;
}

/* decode emits ASCII lines; collect the concatenation */
static char g_ascii[256];
static size_t g_alen;

static esp_err_t ascii_sink(void *arg, const uint8_t *out, size_t len)
{
    (void)arg;
    if (g_alen + len < sizeof(g_ascii))
    {
        memcpy(g_ascii + g_alen, out, len);
        g_alen += len;
        g_ascii[g_alen] = 0;
    }
    return ESP_OK;
}

static void reset(void)
{
    g_nframes = 0;
    g_alen = 0;
    g_ascii[0] = 0;
}

/* feed a whole string to encode through one fresh ctx */
static void enc(const char *s)
{
    uint8_t ctx[64];
    translator_slcan_desc.ctx_init(ctx);
    translator_slcan_desc.encode(ctx, (const uint8_t *)s, strlen(s),
                                 frame_sink, NULL);
}

/* feed a string byte-by-byte (worst-case fragmentation) through one ctx */
static void enc_fragmented(const char *s)
{
    uint8_t ctx[64];
    translator_slcan_desc.ctx_init(ctx);
    for (size_t i = 0; s[i]; i++)
    {
        uint8_t b = (uint8_t)s[i];
        translator_slcan_desc.encode(ctx, &b, 1, frame_sink, NULL);
    }
}

static void dec(const can_core_frame_t *f)
{
    uint8_t wire[CAN_WIRE_MAX];
    uint8_t ctx[64];
    size_t n = can_wire_encode(f, wire);
    translator_slcan_desc.ctx_init(ctx);
    translator_slcan_desc.decode(ctx, wire, n, ascii_sink, NULL);
}

/* ---- encode tests ------------------------------------------------------ */

void test_encode_std_data(void)
{
    reset();
    enc("t1002AABB\r"); /* id 0x100, dlc 2, AA BB */
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x100, g_frames[0].id);
    TEST_ASSERT_FALSE(g_frames[0].ext);
    TEST_ASSERT_FALSE(g_frames[0].rtr);
    TEST_ASSERT_EQUAL(2, g_frames[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(0xAA, g_frames[0].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, g_frames[0].data[1]);
}

void test_encode_ext_data(void)
{
    reset();
    enc("T18DAF11081122334455667788\r"); /* 29-bit, dlc 8 */
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, g_frames[0].id);
    TEST_ASSERT_TRUE(g_frames[0].ext);
    TEST_ASSERT_EQUAL(8, g_frames[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(0x88, g_frames[0].data[7]);
}

void test_encode_rtr_and_dlc0(void)
{
    reset();
    enc("r7000\r");  /* std RTR, dlc 0 */
    enc("t0000\r");  /* std data, dlc 0, no data bytes */
    TEST_ASSERT_EQUAL(2, g_nframes);
    TEST_ASSERT_TRUE(g_frames[0].rtr);
    TEST_ASSERT_EQUAL(0, g_frames[0].dlc);
    TEST_ASSERT_FALSE(g_frames[1].rtr);
    TEST_ASSERT_EQUAL(0, g_frames[1].dlc);
}

void test_encode_multiframe_one_chunk(void)
{
    reset();
    enc("t1001AA\rt2001BB\rt3001CC\r");
    TEST_ASSERT_EQUAL(3, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x100, g_frames[0].id);
    TEST_ASSERT_EQUAL_HEX32(0x300, g_frames[2].id);
    TEST_ASSERT_EQUAL_HEX8(0xCC, g_frames[2].data[0]);
}

void test_encode_fragmented_every_byte(void)
{
    reset();
    enc_fragmented("T18DAF11081122334455667788\r");
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x18DAF110, g_frames[0].id);
    TEST_ASSERT_EQUAL(8, g_frames[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(0x11, g_frames[0].data[0]);
}

void test_encode_absorbs_control(void)
{
    reset();
    enc("S6\rO\rt1001AA\rC\r"); /* bitrate, open, one frame, close */
    TEST_ASSERT_EQUAL(1, g_nframes); /* only the frame produces output */
    TEST_ASSERT_EQUAL_HEX32(0x100, g_frames[0].id);
}

static uint16_t g_stated;

static void stated_hook(uint16_t kbps)
{
    g_stated = kbps;
}

void test_encode_s_command_states_the_bitrate(void)
{
    /* the one impure edge (2026-10-17): S<n> reaches the hook as kbit/s,
       still produces no frame, and nothing else does */
    reset();
    g_stated = 0;
    slcan_bitrate_hook = stated_hook;
    enc("S6\rO\r");
    TEST_ASSERT_EQUAL_UINT16(500, g_stated);
    enc("S5\r");
    TEST_ASSERT_EQUAL_UINT16(250, g_stated);
    g_stated = 0;
    enc("Sx\r");                 /* a code the codec does not know */
    enc("S61\r");                /* not an S line                  */
    enc("t1001AA\r");
    TEST_ASSERT_EQUAL_UINT16(0, g_stated);
    TEST_ASSERT_EQUAL(1, g_nframes);
    slcan_bitrate_hook = NULL;
    enc("S6\r");                 /* no hook: absorbed as before    */
    TEST_ASSERT_EQUAL_UINT16(0, g_stated);
    TEST_ASSERT_EQUAL_UINT16(1000, slcan_bitrate_kbps('8'));
    TEST_ASSERT_EQUAL_UINT16(83, slcan_bitrate_kbps('9'));
    TEST_ASSERT_EQUAL_UINT16(10, slcan_bitrate_kbps('0'));
    TEST_ASSERT_EQUAL_UINT16(0, slcan_bitrate_kbps('A'));
}

void test_encode_rejects_malformed(void)
{
    reset();
    enc("t10\r");        /* too short (no dlc) */
    enc("tZZ01AA\r");    /* bad hex id */
    enc("t1009AA\r");    /* dlc 9 invalid */
    enc("t1002AA\r");    /* dlc 2 but only 1 data byte */
    TEST_ASSERT_EQUAL(0, g_nframes);
    /* ctx not corrupted: a good frame right after still works */
    enc("t1001AA\r");
    TEST_ASSERT_EQUAL(1, g_nframes);
}

void test_encode_overlong_line_no_corruption(void)
{
    reset();
    char big[80];
    memset(big, 'A', sizeof(big));
    big[sizeof(big) - 1] = 0; /* 79 'A', no '\r' → overflow, dropped */
    enc(big);
    TEST_ASSERT_EQUAL(0, g_nframes);
    enc("t1001AA\r");
    TEST_ASSERT_EQUAL(1, g_nframes);
}

/* ---- decode tests ------------------------------------------------------ */

void test_decode_std_data(void)
{
    reset();
    can_core_frame_t f = { .id = 0x100, .ext = false, .rtr = false, .dlc = 2,
                             .data = { 0xAA, 0xBB } };
    dec(&f);
    TEST_ASSERT_EQUAL_STRING("t1002AABB\r", g_ascii);
}

void test_decode_coalesced_chunk(void)
{
    /* the can endpoint packs several wire frames per chunk (2026-07-18);
       decode must emit one line per frame */
    reset();

    can_core_frame_t a = { .id = 0x100, .dlc = 1, .data = { 0x11 } };
    can_core_frame_t b = { .id = 0x700, .rtr = true, .dlc = 0 };
    can_core_frame_t c = { .id = 0x2A, .dlc = 2, .data = { 0xDE, 0xAD } };

    uint8_t wire[3 * CAN_WIRE_MAX];
    size_t n = can_wire_encode(&a, wire);

    n += can_wire_encode(&b, wire + n);
    n += can_wire_encode(&c, wire + n);

    uint8_t ctx[64];

    translator_slcan_desc.ctx_init(ctx);
    translator_slcan_desc.decode(ctx, wire, n, ascii_sink, NULL);
    TEST_ASSERT_EQUAL_STRING("t100111\rr7000\rt02A2DEAD\r", g_ascii);
}

void test_decode_ext_and_rtr(void)
{
    reset();
    can_core_frame_t e = { .id = 0x18DAF110, .ext = true, .dlc = 1,
                             .data = { 0x42 } };
    dec(&e);
    TEST_ASSERT_EQUAL_STRING("T18DAF110142\r", g_ascii);

    reset();
    can_core_frame_t r = { .id = 0x700, .ext = false, .rtr = true, .dlc = 0 };
    dec(&r);
    TEST_ASSERT_EQUAL_STRING("r7000\r", g_ascii);
}

void test_roundtrip_encode_decode(void)
{
    /* decode a frame to ASCII, feed that ASCII back to encode → same frame */
    reset();
    can_core_frame_t f = { .id = 0x123, .ext = false, .dlc = 4,
                             .data = { 0xDE, 0xAD, 0xBE, 0xEF } };
    dec(&f);
    char line[64];
    strcpy(line, g_ascii);
    reset();
    enc(line);
    TEST_ASSERT_EQUAL(1, g_nframes);
    TEST_ASSERT_EQUAL_HEX32(0x123, g_frames[0].id);
    TEST_ASSERT_EQUAL(4, g_frames[0].dlc);
    TEST_ASSERT_EQUAL_HEX8(0xEF, g_frames[0].data[3]);
}

void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_std_data);
    RUN_TEST(test_encode_ext_data);
    RUN_TEST(test_encode_rtr_and_dlc0);
    RUN_TEST(test_encode_multiframe_one_chunk);
    RUN_TEST(test_encode_fragmented_every_byte);
    RUN_TEST(test_encode_absorbs_control);
    RUN_TEST(test_encode_s_command_states_the_bitrate);
    RUN_TEST(test_encode_rejects_malformed);
    RUN_TEST(test_encode_overlong_line_no_corruption);
    RUN_TEST(test_decode_std_data);
    RUN_TEST(test_decode_coalesced_chunk);
    RUN_TEST(test_decode_ext_and_rtr);
    RUN_TEST(test_roundtrip_encode_decode);
    UNITY_END();
}
