/**
 * @file partition_migrate_core.h
 * @brief The pure half of partition_migrate: parse an ESP-IDF partition
 *        table and decide whether this build may replace the table in
 *        flash with its own. No IDF dependencies: host-tested.
 *
 * The rule: the bootloader picks the image from `otadata` and the app
 * entries, and `phy_init` / `nvs` hold calibration and keys, so those are
 * ANCHORED: the table in flash is migrated only when every anchored entry
 * is identical in both tables (both ways) and only the other data
 * partitions differ. Anything else (a moved app slot, a table that does
 * not parse) is FOREIGN and is left alone.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PM_ENTRY_LEN      32U
#define PM_TABLE_MAX_LEN  0xC00U         /* ESP_PARTITION_TABLE_MAX_LEN   */
#define PM_MAX_ENTRIES    16U            /* a product table; more = foreign */
#define PM_MAGIC          0x50AAU
#define PM_MAGIC_MD5      0xEBEBU
#define PM_TYPE_APP       0x00U
#define PM_TYPE_DATA      0x01U
#define PM_DATA_OTA       0x00U
#define PM_DATA_PHY       0x01U
#define PM_DATA_NVS       0x02U

typedef struct
{
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    uint32_t flags;
    char     label[17];
} pm_entry_t;

typedef enum
{
    PM_TABLE_SAME = 0,   /**< entry for entry this build's table          */
    PM_TABLE_MIGRATE,    /**< anchors identical, other data partitions
                              differ: safe to rewrite                     */
    PM_TABLE_FOREIGN,    /**< unreadable, or an anchored entry differs:
                              leave it                                    */
} pm_verdict_t;

/**
 * Parse entries up to the terminator (a blank entry) or the MD5 entry.
 * @return the entry count; 0 when the bytes are not a partition table
 *         (bad magic, no entry, more than PM_MAX_ENTRIES, a zero size).
 */
size_t pm_table_parse(const uint8_t *buf, size_t len, pm_entry_t *out,
                      size_t max_entries);

/** App slots, otadata, phy_init and nvs: the bootloader's and the radio's. */
bool pm_entry_is_anchored(const pm_entry_t *e);

/**
 * The verdict for the table read from flash against this build's.
 * @param why  optional: a short text for the log (what differs, or why
 *             the table is foreign)
 */
pm_verdict_t pm_table_compare(const uint8_t *flash, size_t flash_len,
                              const uint8_t *ours, size_t ours_len,
                              char *why, size_t why_len);

/**
 * The layout id (2026-10-18): the MD5 of the 32-byte entries in flash order,
 * which is the digest IDF appends to the table (CONFIG_PARTITION_TABLE_MD5)
 * and what the meatpi-devices manifests key their layout verdicts on. As 32
 * lowercase hex characters. False (and "") when the bytes are not a table.
 */
bool pm_table_md5(const uint8_t *buf, size_t len, char out[33]);

/** RFC 1321 over @p len bytes (partition_migrate_md5.c). */
void pm_md5(const uint8_t *data, size_t len, uint8_t digest[16]);
