/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file data_logger_db.c
 * @brief The sqlite storage engine. Called from the writer task ONLY:
 *        the vendored port is compiled SQLITE_THREADSAFE=0
 *        (sqlite3/PROVENANCE.md), so the single-toucher rule is a hard
 *        requirement, not style. Takes a dl_sq_ctx_t since 2026-07-09
 *        (two streams can both pick sqlite = two open .db files).
 *
 * Pragma hygiene per BENCHMARKS.md (the 5.6× fix): everything set ONCE
 * at open, plus forever-prepared INSERTs. Since 2026-09-07 (ROBUSTNESS.md
 * case 1) the commits are ATOMIC: the port syncs again (SQLITE_NO_SYNC
 * off), journal_mode=PERSIST (rollback journal on the card, header zeroed
 * per commit, no create/delete churn) and synchronous=FULL. A resumed
 * file gets PRAGMA quick_check first; SQLITE_CORRUPT/NOTADB anywhere sets
 * the ctx's `corrupt` flag so the writer sets the file aside instead of
 * retrying it (that loop dropped every record on the bench).
 *
 * Schema: params(id, source, name UNIQUE) + records(ts, param_id,
 * value REAL) + frames(ts, id, ext, rtr, dlc, data BLOB), ts-indexed,
 * + clock_steps(ts, delta_ms, source) (2026-10-13: the in-band marker).
 * NOTE the measured ~700 rows/s ceiling: sqlite for the CAN stream is
 * a filtered/slow-bus choice, not a busy-bus one (README).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "sqlite3.h"

#include "data_logger_private.h"

static const char *TAG = "data_logger";

/* ---- PSRAM allocator for sqlite -------------------------------------------
   Internal RAM is the scarce resource on this firmware (~25 KB free at
   steady state); sqlite's page cache and parser must not compete with
   the WiFi/DMA pools, so all its heap goes to PSRAM. */

static void *sq_mem_malloc(int n)
{
    return heap_caps_malloc((size_t)n,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void sq_mem_free(void *p)
{
    heap_caps_free(p);
}

static void *sq_mem_realloc(void *p, int n)
{
    return heap_caps_realloc(p, (size_t)n,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static int sq_mem_size(void *p)
{
    return (p != NULL) ? (int)heap_caps_get_allocated_size(p) : 0;
}

static int sq_mem_roundup(int n)
{
    return (n + 7) & ~7;
}

static int sq_mem_init(void *arg)
{
    (void)arg;
    return SQLITE_OK;
}

static void sq_mem_shutdown(void *arg)
{
    (void)arg;
}

static bool rc_is_corrupt(int rc)
{
    rc &= 0xFF;
    return rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB;
}

static esp_err_t dl_exec(sqlite3 *db, const char *sql)
{
    char *errmsg = NULL;

    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "sql '%s': %s", sql,
                 (errmsg != NULL) ? errmsg : "?");
        sqlite3_free(errmsg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* dl_exec on the ctx's connection, noting a corrupt file */
static esp_err_t sq_exec(dl_sq_ctx_t *c, const char *sql)
{
    esp_err_t err = dl_exec((sqlite3 *)c->db, sql);

    if (err != ESP_OK && rc_is_corrupt(sqlite3_errcode((sqlite3 *)c->db)))
    {
        c->corrupt = true;
    }

    return err;
}

static esp_err_t sq_tables(sqlite3 *db)
{
    if (dl_exec(db, "CREATE TABLE IF NOT EXISTS params ("
                "id INTEGER PRIMARY KEY, source TEXT, name TEXT, "
                "UNIQUE(source, name));") != ESP_OK ||
        dl_exec(db, "CREATE TABLE IF NOT EXISTS records ("
                "ts INTEGER, param_id INTEGER, value REAL);") != ESP_OK ||
        dl_exec(db, "CREATE TABLE IF NOT EXISTS frames ("
                "ts INTEGER, id INTEGER, ext INTEGER, rtr INTEGER, "
                "dlc INTEGER, data BLOB);") != ESP_OK ||
        /* the clock stepped while the file was open (2026-10-13): rows
           with ts <= the step's ts - delta_ms were stamped on the old clock */
        dl_exec(db, "CREATE TABLE IF NOT EXISTS clock_steps ("
                "ts INTEGER, delta_ms INTEGER, source TEXT);") != ESP_OK)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t sq_indexes(sqlite3 *db)
{
    if (dl_exec(db, "CREATE INDEX IF NOT EXISTS records_ts "
                "ON records(ts);") != ESP_OK ||
        dl_exec(db, "CREATE INDEX IF NOT EXISTS frames_ts "
                "ON frames(ts);") != ESP_OK)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t sq_schema(sqlite3 *db)
{
    return (sq_tables(db) == ESP_OK && sq_indexes(db) == ESP_OK) ? ESP_OK
                                                                  : ESP_FAIL;
}

/* one-row count; false = the statement failed (corrupt flagged when so) */
static bool sq_count(dl_sq_ctx_t *c, const char *sql, uint64_t *out)
{
    sqlite3_stmt *st = NULL;
    bool ok = false;

    if (sqlite3_prepare_v2((sqlite3 *)c->db, sql, -1, &st, NULL) == SQLITE_OK)
    {
        int rc = sqlite3_step(st);

        if (rc == SQLITE_ROW)
        {
            *out = (uint64_t)sqlite3_column_int64(st, 0);
            ok = true;
        }
        else
        {
            ESP_LOGE(TAG, "%s: %s", sql, sqlite3_errmsg((sqlite3 *)c->db));

            if (rc_is_corrupt(rc))
            {
                c->corrupt = true;
            }
        }
    }
    else if (rc_is_corrupt(sqlite3_errcode((sqlite3 *)c->db)))
    {
        c->corrupt = true;
    }

    sqlite3_finalize(st);
    return ok;
}

/* PRAGMA quick_check(1): "ok", or a damage report. No answer at all (a
 * build with the check omitted: that was the case on 2026-09-07 and it
 * set every good file aside) counts as UNKNOWN, never as corrupt. */
static bool sq_quick_check(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    bool ok = true;

    if (sqlite3_prepare_v2(db, "PRAGMA quick_check(1);", -1, &st, NULL) ==
        SQLITE_OK)
    {
        int rc = sqlite3_step(st);

        if (rc == SQLITE_ROW)
        {
            const unsigned char *txt = sqlite3_column_text(st, 0);

            ok = txt != NULL && strcmp((const char *)txt, "ok") == 0;

            if (!ok)
            {
                ESP_LOGE(TAG, "quick_check: %s", txt ? (const char *)txt : "?");
            }
        }
        else if (rc_is_corrupt(rc))
        {
            ESP_LOGE(TAG, "quick_check: %s", sqlite3_errmsg(db));
            ok = false;
        }
        else
        {
            ESP_LOGW(TAG, "quick_check gave no answer (rc %d); trusting the file", rc);
        }
    }
    else
    {
        ESP_LOGW(TAG, "quick_check unavailable; trusting the file");
    }

    sqlite3_finalize(st);
    return ok;
}

static void sq_close(void *ctx)
{
    dl_sq_ctx_t *c = ctx;

    if (c->ins_rec != NULL)
    {
        sqlite3_finalize((sqlite3_stmt *)c->ins_rec);
        c->ins_rec = NULL;
    }

    if (c->ins_frame != NULL)
    {
        sqlite3_finalize((sqlite3_stmt *)c->ins_frame);
        c->ins_frame = NULL;
    }

    if (c->db != NULL)
    {
        sqlite3_close((sqlite3 *)c->db);
        c->db = NULL;
    }
}

static esp_err_t sq_open(void *ctx, const char *path, bool frames)
{
    dl_sq_ctx_t *c = ctx;

    (void)frames; /* both tables are always created (cheap) */

    /* the port defines SQLITE_OMIT_AUTOINIT: without this, the first
     * malloc jumps through an uninitialized xMalloc pointer */
    static bool s_lib_ready;

    if (c->db != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_lib_ready)
    {
        static const sqlite3_mem_methods PSRAM_MEM =
        {
            .xMalloc = sq_mem_malloc,
            .xFree = sq_mem_free,
            .xRealloc = sq_mem_realloc,
            .xSize = sq_mem_size,
            .xRoundup = sq_mem_roundup,
            .xInit = sq_mem_init,
            .xShutdown = sq_mem_shutdown,
        };

        (void)sqlite3_config(SQLITE_CONFIG_MALLOC, &PSRAM_MEM);

        if (sqlite3_initialize() != SQLITE_OK)
        {
            ESP_LOGE(TAG, "sqlite3_initialize failed");
            return ESP_FAIL;
        }

        s_lib_ready = true;
    }

    sqlite3 *db = NULL;
    struct stat sb;
    bool existing = stat(path, &sb) == 0 && sb.st_size > 0;

    c->corrupt = false;

    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "open %s: %s", path,
                 (db != NULL) ? sqlite3_errmsg(db) : "?");

        if (db != NULL && rc_is_corrupt(sqlite3_errcode(db)))
        {
            c->corrupt = true;
        }

        c->db = db;
        sq_close(c);
        return ESP_FAIL;
    }

    c->db = db;

    /* a resumed file is checked before it is trusted (bounded: a few
       seconds of card reads); a hot journal from a cut commit is rolled
       back by this first access */
    if (existing && (uint64_t)sb.st_size <= DL_QUICKCHECK_MAX)
    {
        int64_t t0 = esp_timer_get_time();

        if (!sq_quick_check(db))
        {
            c->corrupt = true;
            sq_close(c);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "%s: quick_check ok (%lld ms)", path,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }

    if (sq_exec(c, "PRAGMA journal_mode=PERSIST;") != ESP_OK ||
        sq_exec(c, "PRAGMA synchronous=FULL;") != ESP_OK ||
        sq_exec(c, "PRAGMA temp_store=MEMORY;") != ESP_OK ||
        sq_schema(db) != ESP_OK)
    {
        if (rc_is_corrupt(sqlite3_errcode(db)))
        {
            c->corrupt = true;
        }

        sq_close(c);
        return ESP_FAIL;
    }

    sqlite3_stmt *st = NULL;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO records (ts, param_id, value) "
                           "VALUES (?, ?, ?);", -1, &st, NULL) !=
        SQLITE_OK)
    {
        ESP_LOGE(TAG, "prepare records: %s", sqlite3_errmsg(db));
        sq_close(c);
        return ESP_FAIL;
    }

    c->ins_rec = st;
    st = NULL;

    if (sqlite3_prepare_v2(db,
                           "INSERT INTO frames "
                           "(ts, id, ext, rtr, dlc, data) "
                           "VALUES (?, ?, ?, ?, ?, ?);", -1, &st,
                           NULL) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "prepare frames: %s", sqlite3_errmsg(db));
        sq_close(c);
        return ESP_FAIL;
    }

    c->ins_frame = st;

    /* resuming an existing file: seed the size estimates. These full
       scans double as THE resume check: the port answers nothing to
       PRAGMA quick_check (bench 2026-09-07), but walking each table's
       btree surfaces SQLITE_CORRUPT on a torn file, which sets the
       ctx's corrupt flag and fails the open (the writer sets the file
       aside). */
    c->rows = 0;
    c->frame_rows = 0;

    if (!sq_count(c, "SELECT COUNT(*) FROM records;", &c->rows) ||
        !sq_count(c, "SELECT COUNT(*) FROM frames;", &c->frame_rows))
    {
        ESP_LOGE(TAG, "%s: table scan failed (%s)", path,
                 c->corrupt ? "corrupt" : "error");
        sq_close(c);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool sq_is_open(void *ctx)
{
    return ((dl_sq_ctx_t *)ctx)->db != NULL;
}

/* intern source.name into params, returns db id (>0) or -1 */
static int sq_intern(sqlite3 *db, const char *source, const char *name)
{
    sqlite3_stmt *st = NULL;
    int id = -1;

    if (sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO params (source, name) "
                           "VALUES (?, ?);", -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }

    sqlite3_finalize(st);
    st = NULL;

    if (sqlite3_prepare_v2(db,
                           "SELECT id FROM params WHERE source = ? "
                           "AND name = ?;", -1, &st, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, source, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);

        if (sqlite3_step(st) == SQLITE_ROW)
        {
            id = sqlite3_column_int(st, 0);
        }
    }

    sqlite3_finalize(st);

    if (id < 0)
    {
        ESP_LOGE(TAG, "intern %s.%s failed: %s", source, name,
                 sqlite3_errmsg(db));
    }

    return id;
}

static esp_err_t sq_begin(void *ctx)
{
    return sq_exec((dl_sq_ctx_t *)ctx, "BEGIN;");
}

static bool sq_corrupt(void *ctx)
{
    return ((dl_sq_ctx_t *)ctx)->corrupt;
}

static esp_err_t sq_write(void *ctx, const dl_record_t *rec,
                          dl_param_entry_t *p)
{
    dl_sq_ctx_t *c = ctx;

    if (rec->kind == DL_REC_FRAME)
    {
        sqlite3_stmt *st = c->ins_frame;

        if (st == NULL)
        {
            return ESP_ERR_INVALID_STATE;
        }

        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, rec->ts_ms);
        sqlite3_bind_int64(st, 2, (int64_t)rec->u.f.id);
        sqlite3_bind_int(st, 3, (rec->u.f.flags & DL_FRAME_EXT) ? 1 : 0);
        sqlite3_bind_int(st, 4, (rec->u.f.flags & DL_FRAME_RTR) ? 1 : 0);
        sqlite3_bind_int(st, 5, rec->u.f.dlc);
        sqlite3_bind_blob(st, 6, rec->u.f.data, rec->u.f.dlc,
                          SQLITE_STATIC);

        if (sqlite3_step(st) != SQLITE_DONE)
        {
            ESP_LOGE(TAG, "frame insert: %s",
                     sqlite3_errmsg((sqlite3 *)c->db));

            if (rc_is_corrupt(sqlite3_errcode((sqlite3 *)c->db)))
            {
                c->corrupt = true;
            }

            return ESP_FAIL;
        }

        c->frame_rows++;
        return ESP_OK;
    }

    sqlite3_stmt *st = c->ins_rec;

    if (st == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (p->db_id <= 0)
    {
        p->db_id = sq_intern((sqlite3 *)c->db, p->source, p->name);

        if (p->db_id <= 0)
        {
            if (rc_is_corrupt(sqlite3_errcode((sqlite3 *)c->db)))
            {
                c->corrupt = true;
            }

            return ESP_FAIL;
        }
    }

    sqlite3_reset(st);
    sqlite3_bind_int64(st, 1, rec->ts_ms);
    sqlite3_bind_int(st, 2, p->db_id);
    sqlite3_bind_double(st, 3, rec->u.p.value);

    if (sqlite3_step(st) != SQLITE_DONE)
    {
        ESP_LOGE(TAG, "insert: %s", sqlite3_errmsg((sqlite3 *)c->db));

        if (rc_is_corrupt(sqlite3_errcode((sqlite3 *)c->db)))
        {
            c->corrupt = true;
        }

        return ESP_FAIL;
    }

    c->rows++;
    return ESP_OK;
}

static esp_err_t sq_commit(void *ctx)
{
    return sq_exec((dl_sq_ctx_t *)ctx, "COMMIT;");
}

static uint64_t sq_bytes(void *ctx)
{
    dl_sq_ctx_t *c = ctx;

    return c->rows * DL_BYTES_PER_ROW +
           c->frame_rows * DL_BYTES_PER_FRAME;
}

/* a row in clock_steps, its own transaction (called between batches) */
static esp_err_t sq_mark(void *ctx, const dl_clock_step_t *step)
{
    dl_sq_ctx_t *c = ctx;
    char sql[128];

    if (c->db == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(sql, sizeof(sql),
             "INSERT INTO clock_steps (ts, delta_ms, source) "
             "VALUES (%lld, %lld, '%s');",
             (long long)step->wall_ms, (long long)step->delta_ms,
             dl_clock_source_name(step->source));

    if (sq_exec(c, "BEGIN;") != ESP_OK)
    {
        return ESP_FAIL;
    }

    if (sq_exec(c, sql) != ESP_OK)
    {
        (void)sq_exec(c, "ROLLBACK;");
        return ESP_FAIL;
    }

    return sq_exec(c, "COMMIT;");
}

const dl_engine_t dl_engine_sqlite =
{
    .ext = ".db",
    .open = sq_open,
    .close = sq_close,
    .is_open = sq_is_open,
    .corrupt = sq_corrupt,
    .begin = sq_begin,
    .write = sq_write,
    .commit = sq_commit,
    .bytes = sq_bytes,
    .mark = sq_mark,
};

/* ---- salvage (ROBUSTNESS.md case 5) -------------------------------------------
   Copy what sqlite can still read out of a set-aside file into a fresh one:
   the params dictionary first (ids keep their meaning), then the rows in
   rowid order, in 1000-row transactions, stopping at the first read error
   or when the time budget is spent. Writer task only (single toucher);
   the destination uses the fast pragmas: a crash mid-salvage leaves a
   valid partial file that the next boot resumes, and the source is never
   retried. */
esp_err_t dl_sq_salvage(const char *src, const char *dst, bool frames,
                        uint32_t budget_ms, uint32_t *rows_out)
{
    sqlite3 *in = NULL;
    sqlite3 *out = NULL;
    sqlite3_stmt *sel = NULL;
    sqlite3_stmt *ins = NULL;
    uint32_t rows = 0;
    int64_t t0 = esp_timer_get_time();

    *rows_out = 0;

    if (sqlite3_open_v2(src, &in, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
    {
        ESP_LOGW(TAG, "salvage: cannot open %s: %s", src,
                 (in != NULL) ? sqlite3_errmsg(in) : "?");
        sqlite3_close(in);
        return ESP_FAIL;
    }

    unlink(dst);

    /* tables only: the indexes are built once, after the copy (an index
       maintained per insert halved the salvage rate on the bench) */
    if (sqlite3_open(dst, &out) != SQLITE_OK ||
        dl_exec(out, "PRAGMA journal_mode=MEMORY;") != ESP_OK ||
        dl_exec(out, "PRAGMA synchronous=OFF;") != ESP_OK ||
        dl_exec(out, "PRAGMA temp_store=MEMORY;") != ESP_OK || /* the index sort: no temp file on the card (bench: "disk I/O error") */
        sq_tables(out) != ESP_OK)
    {
        ESP_LOGW(TAG, "salvage: cannot create %s", dst);
        sqlite3_close(in);
        sqlite3_close(out);
        unlink(dst);
        return ESP_FAIL;
    }

    if (sqlite3_prepare_v2(in, "SELECT id, source, name FROM params "
                           "ORDER BY id;", -1, &sel, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(out, "INSERT OR IGNORE INTO params "
                           "(id, source, name) VALUES (?, ?, ?);", -1,
                           &ins, NULL) == SQLITE_OK &&
        dl_exec(out, "BEGIN;") == ESP_OK)
    {
        while (sqlite3_step(sel) == SQLITE_ROW)
        {
            sqlite3_reset(ins);
            sqlite3_bind_int(ins, 1, sqlite3_column_int(sel, 0));
            sqlite3_bind_text(ins, 2,
                              (const char *)sqlite3_column_text(sel, 1), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 3,
                              (const char *)sqlite3_column_text(sel, 2), -1,
                              SQLITE_TRANSIENT);

            if (sqlite3_step(ins) != SQLITE_DONE)
            {
                break;
            }
        }

        dl_exec(out, "COMMIT;");
    }

    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sel = NULL;
    ins = NULL;

    const char *q = frames
        ? "SELECT ts, id, ext, rtr, dlc, data FROM frames ORDER BY rowid;"
        : "SELECT ts, param_id, value FROM records ORDER BY rowid;";
    const char *iq = frames
        ? "INSERT INTO frames (ts, id, ext, rtr, dlc, data) "
          "VALUES (?, ?, ?, ?, ?, ?);"
        : "INSERT INTO records (ts, param_id, value) VALUES (?, ?, ?);";

    if (sqlite3_prepare_v2(in, q, -1, &sel, NULL) == SQLITE_OK &&
        sqlite3_prepare_v2(out, iq, -1, &ins, NULL) == SQLITE_OK)
    {
        bool open_tx = dl_exec(out, "BEGIN;") == ESP_OK;

        while (open_tx && sqlite3_step(sel) == SQLITE_ROW)
        {
            sqlite3_reset(ins);

            if (frames)
            {
                sqlite3_bind_int64(ins, 1, sqlite3_column_int64(sel, 0));
                sqlite3_bind_int64(ins, 2, sqlite3_column_int64(sel, 1));
                sqlite3_bind_int(ins, 3, sqlite3_column_int(sel, 2));
                sqlite3_bind_int(ins, 4, sqlite3_column_int(sel, 3));
                sqlite3_bind_int(ins, 5, sqlite3_column_int(sel, 4));
                sqlite3_bind_blob(ins, 6, sqlite3_column_blob(sel, 5),
                                  sqlite3_column_bytes(sel, 5),
                                  SQLITE_TRANSIENT);
            }
            else
            {
                sqlite3_bind_int64(ins, 1, sqlite3_column_int64(sel, 0));
                sqlite3_bind_int(ins, 2, sqlite3_column_int(sel, 1));
                sqlite3_bind_double(ins, 3, sqlite3_column_double(sel, 2));
            }

            if (sqlite3_step(ins) != SQLITE_DONE)
            {
                break;
            }

            rows++;

            if ((rows % 5000) == 0)
            {
                dl_exec(out, "COMMIT;");

                if ((esp_timer_get_time() - t0) / 1000 > (int64_t)budget_ms)
                {
                    ESP_LOGW(TAG, "salvage: time budget spent after %lu rows",
                             (unsigned long)rows);
                    open_tx = false;
                    break;
                }

                open_tx = dl_exec(out, "BEGIN;") == ESP_OK;
            }
        }

        if (open_tx)
        {
            dl_exec(out, "COMMIT;");
        }
    }

    sqlite3_finalize(sel);
    sqlite3_finalize(ins);
    sqlite3_close(in);

    if (rows > 0)
    {
        /* the ts index needs a sort; the port has no temp files (its VFS
           answered "disk I/O error" on the bench), so give the sorter room
           in PSRAM and take a miss as a warning: readers stay correct */
        char *errmsg = NULL;

        (void)sqlite3_exec(out, "PRAGMA cache_size=-4096;", NULL, NULL, NULL);

        if (sqlite3_exec(out, "CREATE INDEX IF NOT EXISTS records_ts "
                         "ON records(ts); CREATE INDEX IF NOT EXISTS frames_ts "
                         "ON frames(ts);", NULL, NULL, &errmsg) != SQLITE_OK)
        {
            ESP_LOGW(TAG, "salvage: ts index not built (%s); the file is "
                     "complete, queries just walk it", errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
        }
    }

    sqlite3_close(out);

    if (rows == 0)
    {
        unlink(dst);
    }

    *rows_out = rows;
    return ESP_OK;
}
