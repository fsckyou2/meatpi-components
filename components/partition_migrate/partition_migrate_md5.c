/**
 * @file partition_migrate_md5.c
 * @brief The partition layout's id: the MD5 digest IDF appends to every
 *        table (CONFIG_PARTITION_TABLE_MD5, the 0xEBEB entry) is the
 *        digest of the 32-byte entries in flash order. Computed here from
 *        the entries themselves, so a table without the MD5 entry still
 *        gets an id, and a build's own table the same id as the publisher's
 *        (meatpi-devices manifests, 2026-10-18). RFC 1321, pure.
 */
#include "partition_migrate_core.h"

#include <stdio.h>
#include <string.h>

static const uint32_t K[64] =
{
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU,
    0xa8304613U, 0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U,
    0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U,
    0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
    0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U,
    0xffeff47dU, 0x85845dd1U, 0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U,
};

static const uint8_t S[64] =
{
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t rotl(uint32_t x, uint8_t c)
{
    return (x << c) | (x >> (32U - c));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void block(uint32_t st[4], const uint8_t *p)
{
    uint32_t m[16];
    uint32_t a = st[0];
    uint32_t b = st[1];
    uint32_t c = st[2];
    uint32_t d = st[3];

    for (int i = 0; i < 16; i++)
    {
        m[i] = le32(p + 4 * i);
    }

    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;

        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }

        f = f + a + K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + rotl(f, S[i]);
    }

    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

void pm_md5(const uint8_t *data, size_t len, uint8_t digest[16])
{
    uint32_t st[4] = { 0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U };
    uint8_t tail[128];
    size_t done = 0;
    size_t rest;
    uint64_t bits = (uint64_t)len * 8U;

    while (len - done >= 64U)
    {
        block(st, data + done);
        done += 64U;
    }

    rest = len - done;
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + done, rest);
    tail[rest] = 0x80U;

    size_t pad_len = (rest < 56U) ? 64U : 128U;

    for (int i = 0; i < 8; i++)
    {
        tail[pad_len - 8U + (size_t)i] = (uint8_t)(bits >> (8 * i));
    }

    block(st, tail);

    if (pad_len == 128U)
    {
        block(st, tail + 64);
    }

    for (int i = 0; i < 4; i++)
    {
        digest[4 * i + 0] = (uint8_t)st[i];
        digest[4 * i + 1] = (uint8_t)(st[i] >> 8);
        digest[4 * i + 2] = (uint8_t)(st[i] >> 16);
        digest[4 * i + 3] = (uint8_t)(st[i] >> 24);
    }
}

bool pm_table_md5(const uint8_t *buf, size_t len, char out[33])
{
    pm_entry_t e[PM_MAX_ENTRIES];
    uint8_t digest[16];
    size_t n;

    if (out == NULL)
    {
        return false;
    }

    out[0] = '\0';
    n = pm_table_parse(buf, len, e, PM_MAX_ENTRIES);

    if (n == 0)
    {
        return false;
    }

    pm_md5(buf, n * PM_ENTRY_LEN, digest);

    for (int i = 0; i < 16; i++)
    {
        snprintf(out + 2 * i, 3, "%02x", digest[i]);
    }

    return true;
}
