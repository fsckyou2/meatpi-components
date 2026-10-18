/**
 * @file partition_migrate.c
 * @brief The flash half of partition_migrate: read the table at
 *        CONFIG_PARTITION_TABLE_OFFSET, compare it with the embedded one,
 *        rewrite and verify.
 */
#include "partition_migrate.h"

#include <string.h>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "log_manager.h"

#include "partition_migrate_core.h"

static const char *TAG = "partition_migrate";

#define PM_SECTOR_LEN 0x1000U     /* the table's sector: one erase unit */

/* this build's partition table, embedded by CMakeLists.txt from the
   build's partition_table/partition-table.bin */
extern const uint8_t pm_table_start[] asm("_binary_partition_table_bin_start");
extern const uint8_t pm_table_end[]   asm("_binary_partition_table_bin_end");

/* the layout id of the table in flash, from the boot read ("" before) */
static char s_flashed_md5[33];

bool partition_migrate_flashed_md5(char out[33])
{
    if (out == NULL)
    {
        return false;
    }

    memcpy(out, s_flashed_md5, sizeof(s_flashed_md5));
    return s_flashed_md5[0] != '\0';
}

bool partition_migrate_build_md5(char out[33])
{
    const size_t ours_len = (size_t)(pm_table_end - pm_table_start);

    return pm_table_md5(pm_table_start, ours_len, out);
}

esp_err_t partition_migrate_init(void)
{
    static const log_descriptor_t LOG =
    {
        .name          = "partition_migrate",
        .default_level = ESP_LOG_INFO,
    };

    if (log_manager_register(&LOG) != ESP_OK)
    {
        ESP_LOGW(TAG, "log descriptor not registered");
    }

    return ESP_OK;
}

const char *partition_migrate_result_to_str(partition_migrate_result_t r)
{
    switch (r)
    {
        case PARTITION_MIGRATE_OURS:      return "ours";
        case PARTITION_MIGRATE_REWRITTEN: return "rewritten";
        case PARTITION_MIGRATE_NEEDED:    return "needed";
        case PARTITION_MIGRATE_FOREIGN:   return "foreign";
        case PARTITION_MIGRATE_FAILED:    return "failed";
        default:                          return "?";
    }
}

/* the bytes in flash after the rewrite must be ours + 0xFF to the end */
static bool verify(const uint8_t *flash, size_t flash_len,
                   const uint8_t *ours, size_t ours_len)
{
    if (ours_len > flash_len || memcmp(flash, ours, ours_len) != 0)
    {
        return false;
    }

    for (size_t i = ours_len; i < flash_len; i++)
    {
        if (flash[i] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

static esp_err_t rewrite(uint8_t *buf, const uint8_t *ours, size_t ours_len,
                         uint32_t *out_ms)
{
    int64_t t0;
    int64_t t1;
    int64_t t2;
    esp_err_t err;

    memset(buf, 0xFF, PM_TABLE_MAX_LEN);
    memcpy(buf, ours, ours_len);

    t0  = esp_timer_get_time();
    err = esp_flash_erase_region(NULL, CONFIG_PARTITION_TABLE_OFFSET,
                                 PM_SECTOR_LEN);
    t1  = esp_timer_get_time();

    if (err == ESP_OK)
    {
        err = esp_flash_write(NULL, buf, CONFIG_PARTITION_TABLE_OFFSET,
                              PM_TABLE_MAX_LEN);
    }

    t2 = esp_timer_get_time();

    if (out_ms != NULL)
    {
        *out_ms = (uint32_t)((t2 - t0) / 1000);
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "partition table rewrite failed: %s (erase %lld us, "
                 "write %lld us)", esp_err_to_name(err),
                 (long long)(t1 - t0), (long long)(t2 - t1));
        return err;
    }

    err = esp_flash_read(NULL, buf, CONFIG_PARTITION_TABLE_OFFSET,
                         PM_TABLE_MAX_LEN);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "partition table read-back failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    if (!verify(buf, PM_TABLE_MAX_LEN, ours, ours_len))
    {
        ESP_LOGE(TAG, "partition table rewrite did not verify");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "partition table rewritten in %lu ms (erase %lu ms, "
             "write %lu ms): restarting into the new layout",
             (unsigned long)((t2 - t0) / 1000),
             (unsigned long)((t1 - t0) / 1000),
             (unsigned long)((t2 - t1) / 1000));
    return ESP_OK;
}

esp_err_t partition_migrate_run(bool allow_rewrite,
                                partition_migrate_result_t *out_result,
                                uint32_t *out_ms)
{
    const size_t ours_len = (size_t)(pm_table_end - pm_table_start);
    char why[160];
    uint8_t *buf;
    esp_err_t err;
    pm_verdict_t verdict;

    if (out_ms != NULL)
    {
        *out_ms = 0;
    }

    if (out_result == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_result = PARTITION_MIGRATE_FAILED;

    if (ours_len == 0 || ours_len > PM_TABLE_MAX_LEN)
    {
        ESP_LOGE(TAG, "embedded partition table has %u bytes",
                 (unsigned)ours_len);
        return ESP_ERR_INVALID_SIZE;
    }

    /* internal: read with the cache on, then the write source with it off */
    buf = heap_caps_malloc(PM_TABLE_MAX_LEN,
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        ESP_LOGE(TAG, "no internal RAM for the table buffer");
        return ESP_ERR_NO_MEM;
    }

    err = esp_flash_read(NULL, buf, CONFIG_PARTITION_TABLE_OFFSET,
                         PM_TABLE_MAX_LEN);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "partition table read failed: %s",
                 esp_err_to_name(err));
        heap_caps_free(buf);
        return err;
    }

    (void)pm_table_md5(buf, PM_TABLE_MAX_LEN, s_flashed_md5);
    verdict = pm_table_compare(buf, PM_TABLE_MAX_LEN, pm_table_start,
                               ours_len, why, sizeof(why));

    switch (verdict)
    {
        case PM_TABLE_SAME:
            *out_result = PARTITION_MIGRATE_OURS;
            ESP_LOGD(TAG, "the partition table in flash is this build's");
            break;

        case PM_TABLE_FOREIGN:
            *out_result = PARTITION_MIGRATE_FOREIGN;
            ESP_LOGE(TAG, "the partition table in flash is another layout "
                     "this firmware cannot migrate (%s): data partitions "
                     "may be missing", why);
            break;

        case PM_TABLE_MIGRATE:
        default:
            if (!allow_rewrite)
            {
                *out_result = PARTITION_MIGRATE_NEEDED;
                ESP_LOGW(TAG, "the partition table in flash is another "
                         "firmware's layout (%s) and this boot may not "
                         "rewrite it", why);
                break;
            }

#if !CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED
            *out_result = PARTITION_MIGRATE_NEEDED;
            ESP_LOGE(TAG, "the partition table in flash is another "
                     "firmware's layout (%s) and this build cannot rewrite "
                     "it: CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED is off",
                     why);
#else
            ESP_LOGW(TAG, "the partition table in flash is another "
                     "firmware's layout (%s): rewriting it with this "
                     "build's (one 4 KB erase, one write, then a restart)",
                     why);
            err = rewrite(buf, pm_table_start, ours_len, out_ms);
            *out_result = (err == ESP_OK) ? PARTITION_MIGRATE_REWRITTEN
                                          : PARTITION_MIGRATE_FAILED;
#endif
            break;
    }

    heap_caps_free(buf);
    return err;
}
