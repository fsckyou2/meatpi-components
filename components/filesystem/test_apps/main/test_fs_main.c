/**
 * @file test_fs_main.c
 * @brief On-target test app for the filesystem component. Self-contained:
 *        exercises the real `storage` LittleFS partition (from the MAIN
 *        firmware's partition table, Coding Standard rev 2.1 §7) and prints
 *        markers that pytest_filesystem.py asserts in order.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "filesystem.h"

static const char *TAG = "fs_test";

/* ---- the 32 GB card of issue #982 -------------------------------------------
 * A FAT32 volume of 1,897,503 clusters of 16 KiB (31,088,689,152 bytes) with
 * one cluster in use: the geometry behind the File Manager's "976.48 MB" for
 * a 32 GB card (31,088,689,152 mod 2^32 = 1,023,918,080 bytes = 976.48 MiB,
 * and the one cluster is its "16 KB used"). This app runs without a card, so
 * the FATFS capacity query is wrapped at link time (-Wl,--wrap, the main
 * CMakeLists) and answers for /sd with that volume while s_big_card is set;
 * every other call goes to the real function. */
#define BIG_CARD_CLUSTER_BYTES  16384ULL
#define BIG_CARD_CLUSTERS       1897503ULL
#define BIG_CARD_TOTAL          (BIG_CARD_CLUSTERS * BIG_CARD_CLUSTER_BYTES)
#define BIG_CARD_USED           BIG_CARD_CLUSTER_BYTES

static bool s_big_card;
static int  s_failures;

esp_err_t __real_esp_vfs_fat_info(const char *base_path,
                                  uint64_t *out_total_bytes,
                                  uint64_t *out_free_bytes);

esp_err_t __wrap_esp_vfs_fat_info(const char *base_path,
                                  uint64_t *out_total_bytes,
                                  uint64_t *out_free_bytes)
{
    if (!s_big_card || base_path == NULL || strcmp(base_path, "/sd") != 0)
    {
        return __real_esp_vfs_fat_info(base_path, out_total_bytes,
                                       out_free_bytes);
    }

    if (out_total_bytes != NULL)
    {
        *out_total_bytes = BIG_CARD_TOTAL;
    }

    if (out_free_bytes != NULL)
    {
        *out_free_bytes = BIG_CARD_TOTAL - BIG_CARD_USED;
    }

    return ESP_OK;
}

typedef struct
{
    int  files;
    int  dirs;
    bool saw_file;
    bool saw_dir;
} list_ctx_t;

static esp_err_t list_cb(const char *name, bool is_dir, size_t size,
                         void *ctx)
{
    list_ctx_t *c = ctx;

    (void)size;

    if (is_dir)
    {
        c->dirs++;

        if (strcmp(name, "b") == 0)
        {
            c->saw_dir = true;
        }
    }
    else
    {
        c->files++;

        if (strcmp(name, "hello.txt") == 0)
        {
            c->saw_file = true;
        }
    }

    return ESP_OK;
}

void app_main(void)
{
    char   buf[64];
    size_t n = 0;

    /* ---- mount ---------------------------------------------------------- */
    esp_err_t err = filesystem_init();

    printf("MOUNT ok=%d\n", err == ESP_OK);
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(filesystem_start());

    /* clean slate so re-runs behave like first runs */
    filesystem_delete("/data/t/hello.txt");
    filesystem_delete("/data/t/a/b/deep.txt");

    /* ---- write / read roundtrip ----------------------------------------- */
    ESP_ERROR_CHECK(filesystem_write("/data/t/hello.txt", "hello-fs", 8));
    memset(buf, 0, sizeof(buf));
    ESP_ERROR_CHECK(filesystem_read("/data/t/hello.txt", buf, sizeof(buf), &n));
    printf("RW body=%.*s len=%u\n", (int)n, buf, (unsigned)n);

    /* ---- exists ---------------------------------------------------------- */
    printf("EXISTS file=%d missing=%d\n",
           filesystem_exists("/data/t/hello.txt"),
           filesystem_exists("/data/t/nope.txt"));

    /* ---- atomic overwrite: new content, no temp leftover ----------------- */
    ESP_ERROR_CHECK(filesystem_write("/data/t/hello.txt", "v2-content", 10));
    memset(buf, 0, sizeof(buf));
    ESP_ERROR_CHECK(filesystem_read("/data/t/hello.txt", buf, sizeof(buf), &n));
    printf("ATOMIC body=%.*s leftover=%d\n", (int)n, buf,
           filesystem_exists("/data/t/hello.txt.tmp"));

    /* ---- nested parent auto-create --------------------------------------- */
    ESP_ERROR_CHECK(filesystem_write("/data/t/a/b/deep.txt", "deep", 4));
    memset(buf, 0, sizeof(buf));
    ESP_ERROR_CHECK(filesystem_read("/data/t/a/b/deep.txt", buf, sizeof(buf), &n));
    printf("NESTED body=%.*s\n", (int)n, buf);

    /* ---- size + too-small buffer ----------------------------------------- */
    size_t sz = 0;

    ESP_ERROR_CHECK(filesystem_size("/data/t/hello.txt", &sz));

    char tiny[4];
    esp_err_t small = filesystem_read("/data/t/hello.txt", tiny, sizeof(tiny), &n);

    printf("SIZE n=%u small=%s need=%u\n", (unsigned)sz,
           esp_err_to_name(small), (unsigned)n);

    /* ---- list ------------------------------------------------------------- */
    list_ctx_t lc = { 0 };

    ESP_ERROR_CHECK(filesystem_list("/data/t", list_cb, &lc));
    /* /data/t contains hello.txt + dir a; /data/t/a contains dir b */
    list_ctx_t lc2 = { 0 };

    ESP_ERROR_CHECK(filesystem_list("/data/t/a", list_cb, &lc2));
    printf("LIST saw_file=%d saw_dir=%d\n", lc.saw_file, lc2.saw_dir);

    /* ---- delete ------------------------------------------------------------ */
    ESP_ERROR_CHECK(filesystem_delete("/data/t/hello.txt"));
    printf("DELETE ok=1 exists=%d\n", filesystem_exists("/data/t/hello.txt"));

    /* ---- invalid paths ------------------------------------------------------ */
    const char *bad[] =
    {
        "../x", "/data/../etc", "/nope/x", "/data//x", "/data/a/",
    };
    int rejected = 0;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        if (filesystem_write(bad[i], "x", 1) == ESP_ERR_INVALID_ARG)
        {
            rejected++;
        }
    }

    printf("BADPATH rejected=%d\n", rejected);

    /* ---- /sd reserved but unavailable in v1 --------------------------------- */
    printf("SDPATH state=%d\n",
           filesystem_write("/sd/x", "x", 1) == ESP_ERR_INVALID_STATE);

    /* ---- streaming open ------------------------------------------------------ */
    FILE *f = filesystem_open("/data/t/a/b/deep.txt", "rb");

    if (f != NULL)
    {
        memset(buf, 0, sizeof(buf));
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        printf("OPEN body=%.*s\n", (int)n, buf);
    }

    /* ---- capacity ------------------------------------------------------------- */
    uint64_t total = 0;
    uint64_t used = 0;

    ESP_ERROR_CHECK(filesystem_info("/data", &total, &used));
    printf("INFO total_kib=%u used_ok=%d\n", (unsigned)(total / 1024),
           used > 0);

    /* ---- capacity of a 32 GB card (issue #982) --------------------------------- */
    s_big_card = true;
    ESP_ERROR_CHECK(filesystem_sd_set_mounted(true));

    uint64_t sd_total = 0;
    uint64_t sd_used = 0;

    err = filesystem_info("/sd", &sd_total, &sd_used);

    bool sd_ok = (err == ESP_OK) && (sd_total == BIG_CARD_TOTAL) &&
                 (sd_used == BIG_CARD_USED);

    printf("INFO_SD err=%s total=%" PRIu64 " used=%" PRIu64
           " expect_total=%" PRIu64 " expect_used=%" PRIu64 " ok=%d\n",
           esp_err_to_name(err), sd_total, sd_used,
           (uint64_t)BIG_CARD_TOTAL, (uint64_t)BIG_CARD_USED, sd_ok);
    s_failures += sd_ok ? 0 : 1;
    ESP_ERROR_CHECK(filesystem_sd_set_mounted(false));
    s_big_card = false;

    /* ---- persistence across remount (stop -> init) ----------------------------- */
    ESP_ERROR_CHECK(filesystem_stop());
    ESP_ERROR_CHECK(filesystem_init());
    ESP_ERROR_CHECK(filesystem_start());
    memset(buf, 0, sizeof(buf));
    ESP_ERROR_CHECK(filesystem_read("/data/t/a/b/deep.txt", buf, sizeof(buf), &n));
    printf("REMOUNT body=%.*s\n", (int)n, buf);

    if (s_failures > 0)
    {
        /* the capture harness waits for TEST DONE: a failed check keeps it
           away, so the stage goes red instead of passing on "no crash" */
        ESP_LOGE(TAG, "%d check(s) failed", s_failures);
        printf("TEST FAILED checks=%d\n", s_failures);
        return;
    }

    ESP_LOGI(TAG, "suite finished");
    printf("TEST DONE\n");
}
