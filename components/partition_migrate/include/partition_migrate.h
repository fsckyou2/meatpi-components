/**
 * @file partition_migrate.h
 * @brief The partition table in flash becomes this build's when a safe
 *        migration exists (2026-10-10).
 *
 * An OTA writes an app slot only. A unit updated from a firmware with
 * another partition layout keeps that layout: the WiCAN Pro factory
 * firmware (v4.5x) has ONE 6 MB `storage` where v6 keeps `settings` +
 * `storage`, so after the update nothing could persist (41 x `persist
 * failed`, the boot_errors fault, a factory reset that fails). This
 * component compares the table in flash with the one this build was made
 * with (embedded from the build's partition-table.bin) and rewrites it
 * when only data partitions differ: the app slots, `otadata`, `phy_init`
 * and `nvs` must be identical, so the bootloader finds the same image and
 * the calibration and keys stay. The data partitions' owners format what
 * they then find (settings_manager and filesystem probe their superblocks).
 *
 * Cost: a 3 KB read at every boot; once in a unit's life one 4 KB sector
 * erase and one page write at the table's offset, measured on the WiCAN
 * Pro (see README.md), then a restart by the caller.
 *
 * Needs `CONFIG_SPI_FLASH_DANGEROUS_WRITE_ALLOWED`: without it the IDF
 * flash driver refuses the write (the result is then
 * PARTITION_MIGRATE_NEEDED with an error line, never an abort).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    PARTITION_MIGRATE_OURS = 0,  /**< the table in flash is this build's   */
    PARTITION_MIGRATE_REWRITTEN, /**< rewritten and verified: restart now  */
    PARTITION_MIGRATE_NEEDED,    /**< a rewrite is due but was not allowed
                                      (or not built in)                   */
    PARTITION_MIGRATE_FOREIGN,   /**< another layout this build must not
                                      touch (an app slot differs, or no
                                      table parses)                       */
    PARTITION_MIGRATE_FAILED,    /**< read, erase, write or verify failed  */
} partition_migrate_result_t;

/** Registers the log descriptor. No flash access. */
esp_err_t partition_migrate_init(void);

/**
 * Compare the table in flash with this build's and, when allowed and
 * safe, rewrite it. Call BEFORE anything mounts a data partition, from a
 * task with an internal-RAM stack (the write runs with the cache off).
 *
 * @param allow_rewrite  false = report only (the caller's loop guard: a
 *                       boot that follows a migration restart must not
 *                       try again)
 * @param out_result     what happened (never NULL)
 * @param out_ms         erase + write time in ms when rewritten, else 0
 *                       (may be NULL)
 * @return ESP_OK for every verdict; an esp_err_t only when the flash
 *         read, erase, write or read-back failed (result FAILED).
 */
esp_err_t partition_migrate_run(bool allow_rewrite,
                                partition_migrate_result_t *out_result,
                                uint32_t *out_ms);

/** The result as text: ours / rewritten / needed / foreign / failed. */
const char *partition_migrate_result_to_str(partition_migrate_result_t r);

/**
 * The layout ids (2026-10-18): the MD5 of the table's entries, as the
 * meatpi-devices manifests key their verdicts (`layout.md5`,
 * `layout_from`). The flashed one is cached from the boot read in
 * partition_migrate_run() (false and "" before it, or when no table
 * parses); the build's own comes from the embedded table. 32 lowercase
 * hex characters into @p out[33].
 */
bool partition_migrate_flashed_md5(char out[33]);
bool partition_migrate_build_md5(char out[33]);
