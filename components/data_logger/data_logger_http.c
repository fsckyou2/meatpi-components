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
 * @file data_logger_http.c
 * @brief The optional /api/logger status route (§9.1). File
 *        browse/download/delete deliberately NOT duplicated here: the
 *        files live under /sd/logs and the existing /api/fs file
 *        manager routes cover them.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include "http_server_manager.h"

#include "data_logger.h"
#include "data_logger_private.h"

static const char *TAG = "data_logger";

static esp_err_t logger_handler(httpd_req_t *req)
{
    data_logger_stats_t st;
    char body[896];

    (void)data_logger_stats(&st);
    snprintf(body, sizeof(body),
             "{\"enabled\":%s,\"running\":%s,\"paused\":%s,"
             "\"storage_ok\":%s,"
             "\"file\":\"%s\",\"file_rows\":%lu,\"files\":%lu,"
             "\"queued\":%lu,"
             "\"written\":%lu,\"dropped\":%lu,\"errors\":%lu,"
             "\"rotations\":%lu,"
             "\"can\":{\"enabled\":%s,\"file\":\"%s\","
             "\"file_rows\":%lu,\"files\":%lu,\"queued\":%lu,"
             "\"frames_written\":%lu,\"frames_dropped\":%lu,"
             "\"rotations\":%lu},"
             "\"salvaged\":%lu,\"corrupt\":%lu,"
             "\"clock\":{\"valid\":%s,\"source\":\"%s\","
             "\"steps_this_boot\":%lu,\"rotations_by_clock\":%lu,"
             "\"journal_bytes\":%lu,\"journal\":\"/sd/logs/"
             DL_JOURNAL_NAME "\"},"
             "\"dir\":\"/sd/logs\"}",
             st.enabled ? "true" : "false",
             st.running ? "true" : "false",
             st.paused ? "true" : "false",
             st.storage_ok ? "true" : "false",
             st.file,
             (unsigned long)st.file_rows, (unsigned long)st.files,
             (unsigned long)st.queued,
             (unsigned long)st.written, (unsigned long)st.dropped,
             (unsigned long)st.errors, (unsigned long)st.rotations,
             st.can_enabled ? "true" : "false",
             st.can_file,
             (unsigned long)st.can_file_rows,
             (unsigned long)st.can_files,
             (unsigned long)st.can_queued,
             (unsigned long)st.frames_written,
             (unsigned long)st.frames_dropped,
             (unsigned long)st.can_rotations,
             (unsigned long)st.salvaged, (unsigned long)st.corrupt,
             st.clock_valid ? "true" : "false", st.clock_source,
             (unsigned long)st.clock_steps,
             (unsigned long)st.clock_rotations,
             (unsigned long)st.journal_bytes);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* POST /api/logger/gate {"enabled":bool}: runtime (non-persisted)
 * logging gate, the HTTP twin of the logger.enable/logger.disable event
 * actions (API-first §1b: anything a rule can do, a client can do). The
 * observable state is GET /api/logger's "paused" field. */
static esp_err_t gate_post_handler(httpd_req_t *req)
{
    char body[128];
    size_t len = req->content_len;

    if (len == 0 || len >= sizeof(body))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "missing/oversized body");
        return ESP_FAIL;
    }

    size_t got = 0;

    while (got < len)
    {
        int r = httpd_req_recv(req, body + got, len - got);

        if (r <= 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "body read failed");
            return ESP_FAIL;
        }

        got += (size_t)r;
    }

    body[len] = '\0';

    /* one bool field: a full cJSON parse is overkill */
    bool on;

    if (strstr(body, "\"enabled\"") == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }
    else if (strstr(body, "true") != NULL)
    {
        on = true;
    }
    else if (strstr(body, "false") != NULL)
    {
        on = false;
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "need bool 'enabled'");
        return ESP_FAIL;
    }

    dl_runtime_gate(on);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* ---- GET /api/logger/export?stream=params&since=<cursor>&limit=<n> --------
 * Device-contract ask #8: incremental export of the params stream (the
 * jsonl engine; the csv engine too since 2026-09-06, same cursor, same
 * budgets, the trailing meta line stays JSON). `name=<param>` (2026-09-06, the dashboard's
 * history) returns only that parameter's records: the cursor still
 * advances over every line read, bounded per request by
 * DL_EXPORT_SCAN_CAP. Cursor = "<file-epoch>:<byte-offset>", opaque to
 * the client; the FINAL line of every response is a meta record
 * {"_cursor":"<next>","more":true|false}: resend it as `since` to
 * continue. Files are epoch-named (dl_<epoch>.jsonl) so the cursor
 * survives rotation: a retired epoch resumes at the next newer file.
 * Reads are budgeted (lines + bytes) and cut on record boundaries; the
 * ACTIVE file's trailing partial line is never emitted. /sd reads are
 * httpd-task-safe (only internal flash has the cache corollary). */
#define DL_EXPORT_MAX_FILES  64
#define DL_EXPORT_BYTE_CAP   65536          /* emitted bytes per request  */
#define DL_EXPORT_SCAN_CAP   (512 * 1024)   /* bytes read per request     */
#define DL_EXPORT_LINE_CAP   2000
#define DL_EXPORT_LINE_DEF   500

/* ?name=<param> filter (dashboard history, 2026-09-06): a record's
 * "param":"<source>.<name>" matches on the full value or on the part
 * after its last '.', so the UI can ask by parameter name without knowing
 * the sink's source prefix. Plain substring scan: no JSON parse. */
static bool export_line_matches(const char *line, size_t len,
                                const char *name, bool csv)
{
    static const char KEY[] = "\"param\":\"";
    const size_t klen = sizeof(KEY) - 1;
    const char *p = NULL;
    const char *q = NULL;

    if (csv)
    {
        /* "ts_ms,source.name,value": the param is the second field (the
         * header row "ts_ms,param,value" never matches a real name) */
        p = memchr(line, ',', len);

        if (p == NULL)
        {
            return false;
        }

        p++;
        q = memchr(p, ',', len - (size_t)(p - line));
    }
    else
    {
        for (size_t i = 0; i + klen <= len; i++)
        {
            if (line[i] == '"' && memcmp(line + i, KEY, klen) == 0)
            {
                p = line + i + klen;
                break;
            }
        }

        if (p == NULL)
        {
            return false;
        }

        q = memchr(p, '"', len - (size_t)(p - line));
    }

    if (q == NULL)
    {
        return false;
    }

    size_t vlen = (size_t)(q - p);
    size_t nlen = strlen(name);

    if (vlen == nlen)
    {
        return memcmp(p, name, nlen) == 0;
    }

    return vlen > nlen && p[vlen - nlen - 1] == '.' &&
           memcmp(p + vlen - nlen, name, nlen) == 0;
}

/* the in-band clock-step marker (2026-10-13): not a record, never a point
 * for a consumer of this route (the journal and the file itself keep it) */
static bool export_line_is_marker(const char *line, size_t len, bool csv)
{
    static const char JSONL[] = "\"event\":\"clock_step\"";
    static const char CSV[] = ",logger.clock_step,";
    const char *key = csv ? CSV : JSONL;
    size_t klen = (csv ? sizeof(CSV) : sizeof(JSONL)) - 1;

    for (size_t i = 0; i + klen <= len; i++)
    {
        if (line[i] == key[0] && memcmp(line + i, key, klen) == 0)
        {
            return true;
        }
    }

    return false;
}

static int export_list_epochs(int64_t *out, int cap, const char *ext)
{
    DIR *d = opendir(DL_DIR);

    if (d == NULL)
    {
        return 0;
    }

    int n = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL && n < cap)
    {
        int64_t epoch;
        const char *dot = strrchr(e->d_name, '.');

        if (dot != NULL && strcmp(dot, ext) == 0 &&
            strncmp(e->d_name, DL_PREFIX_PARAM,
                    strlen(DL_PREFIX_PARAM)) == 0 &&
            dl_files_parse(e->d_name, &epoch))
        {
            out[n++] = epoch;
        }
    }

    closedir(d);

    /* ascending (oldest first): insertion sort, n is small */
    for (int i = 1; i < n; i++)
    {
        int64_t v = out[i];
        int j = i - 1;

        while (j >= 0 && out[j] > v)
        {
            out[j + 1] = out[j];
            j--;
        }

        out[j + 1] = v;
    }

    return n;
}

static esp_err_t export_get_handler(httpd_req_t *req)
{
    const dl_cfg_t *cfg = dl_settings_config();

    /* the two line-oriented engines stream through here (jsonl since the
     * device contract, csv since 2026-09-06 for the dashboard history);
     * binary and sqlite files are fetched whole via /api/fs/download */
    const char *ext = NULL;
    bool csv = false;

    if (cfg != NULL && strcmp(cfg->format, "jsonl") == 0)
    {
        ext = ".jsonl";
    }
    else if (cfg != NULL && strcmp(cfg->format, "csv") == 0)
    {
        ext = ".csv";
        csv = true;
    }

    if (ext == NULL)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "params stream is not jsonl or csv");
        return ESP_FAIL;
    }

    char query[160] = "";
    char val[48];
    char name[48] = "";
    int64_t since_epoch = 0;
    long since_off = 0;
    int limit = DL_EXPORT_LINE_DEF;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "stream", val, sizeof(val)) ==
                ESP_OK && strcmp(val, "params") != 0)
        {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "only stream=params");
            return ESP_FAIL;
        }

        if (httpd_query_key_value(query, "since", val, sizeof(val)) ==
                ESP_OK)
        {
            sscanf(val, "%lld:%ld", (long long *)&since_epoch, &since_off);
        }

        if (httpd_query_key_value(query, "limit", val, sizeof(val)) ==
                ESP_OK)
        {
            limit = atoi(val);

            if (limit < 1 || limit > DL_EXPORT_LINE_CAP)
            {
                limit = DL_EXPORT_LINE_DEF;
            }
        }

        if (httpd_query_key_value(query, "name", name, sizeof(name)) !=
                ESP_OK)
        {
            name[0] = '\0';
        }
    }

    int64_t epochs[DL_EXPORT_MAX_FILES];
    int nfiles = export_list_epochs(epochs, DL_EXPORT_MAX_FILES, ext);

    /* first file at/after the cursor (a retired epoch resumes at the
       next newer file, offset 0) */
    int idx = 0;

    while (idx < nfiles && epochs[idx] < since_epoch)
    {
        idx++;
    }

    if (idx < nfiles && epochs[idx] != since_epoch)
    {
        since_off = 0;
    }

    httpd_resp_set_type(req, csv ? "text/plain" : "application/x-ndjson");

    long off = since_off;
    int lines_left = limit;
    int bytes_left = DL_EXPORT_BYTE_CAP;
    int scan_left = DL_EXPORT_SCAN_CAP;
    bool more = false;
    bool gated_here = false;
    int64_t cur_epoch = (idx < nfiles) ? epochs[idx] : since_epoch;

    while (idx < nfiles && lines_left > 0 && bytes_left > 0 && scan_left > 0)
    {
        char fname[40];
        char path[64];

        cur_epoch = epochs[idx];
        dl_files_make(fname, sizeof(fname), DL_PREFIX_PARAM, cur_epoch,
                      ext);
        snprintf(path, sizeof(path), DL_DIR "/%s", fname);

        FILE *f = fopen(path, "rb");

        if (f == NULL && !gated_here)
        {
            /* the ACTIVE file is exclusively held by the writer (FATFS
               FS_LOCK): pause the gate; the writer closes its files
               within one ~200 ms loop. Restored after the read unless a
               logger.disable rule had already paused it. */
            data_logger_stats_t st;

            if (data_logger_stats(&st) == ESP_OK && !st.paused)
            {
                dl_runtime_gate(false);
                gated_here = true;

                /* the writer is notified and closes within its loop; the
                   retry window still covers a writer busy in a batch */
                for (int t = 0; t < 25 && f == NULL; t++)
                {
                    vTaskDelay(pdMS_TO_TICKS(60));
                    f = fopen(path, "rb");
                }
            }
        }

        if (f == NULL)
        {
            idx++;
            off = 0;
            continue;
        }

        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);

        /* which file the rows that follow come from (2026-10-13): a
           consumer with the clock journal re-bases them per file */
        char flin[64];
        int fn = snprintf(flin, sizeof(flin), "{\"_file\":\"%s\"}\n", fname);

        httpd_resp_send_chunk(req, flin, fn);
        bytes_left -= fn;

        while (lines_left > 0 && bytes_left > 0 && scan_left > 0 &&
               off < fsize)
        {
            char buf[1024];
            size_t want = sizeof(buf);

            if ((long)want > fsize - off)
            {
                want = (size_t)(fsize - off);
            }

            if ((int)want > scan_left)
            {
                want = (size_t)scan_left;
            }

            /* every window starts exactly at the cursor: the partial tail
               of the previous window is re-read, never skipped */
            fseek(f, off, SEEK_SET);

            size_t n = fread(buf, 1, want, f);

            if (n == 0)
            {
                break;
            }

            /* walk the complete lines: forward the matching ones, keep the
               offsets exact, stop once a budget is spent */
            size_t consumed = 0;
            bool stop = false;

            for (size_t i = 0; i < n && !stop; i++)
            {
                if (buf[i] != '\n')
                {
                    continue;
                }

                const char *line = buf + consumed;
                size_t len = i + 1 - consumed;

                if (!export_line_is_marker(line, len, csv) &&
                    (name[0] == '\0' ||
                     export_line_matches(line, len, name, csv)))
                {
                    if ((int)len > bytes_left && lines_left < limit)
                    {
                        stop = true;   /* does not fit: next request */
                        break;
                    }

                    httpd_resp_send_chunk(req, line, len);
                    bytes_left -= (int)len;
                    lines_left--;
                }

                consumed = i + 1;

                if (lines_left == 0 || bytes_left <= 0)
                {
                    stop = true;
                }
            }

            if (consumed == 0)
            {
                /* no boundary in this window: a >1 KB record mid-file is
                   forwarded raw when unfiltered (offsets stay exact) or
                   skipped when filtered; a trailing partial line (active
                   file mid-append) is left for next time */
                if (off + (long)n < fsize && n == sizeof(buf))
                {
                    if (name[0] == '\0')
                    {
                        httpd_resp_send_chunk(req, buf, n);
                        bytes_left -= (int)n;
                    }

                    consumed = n;
                }
                else
                {
                    break;
                }
            }

            off += (long)consumed;
            scan_left -= (int)consumed;

            if (stop)
            {
                break;
            }
        }

        bool file_done = (off >= fsize);

        fclose(f);

        if (!file_done)
        {
            more = true; /* budget hit or partial tail pending */
            break;
        }

        if (idx + 1 < nfiles)
        {
            idx++;
            off = 0;
        }
        else
        {
            break; /* fully caught up on the newest file */
        }
    }

    if (gated_here)
    {
        dl_runtime_gate(true);
    }

    char meta[96];

    snprintf(meta, sizeof(meta),
             "{\"_cursor\":\"%lld:%ld\",\"more\":%s}\n",
             (long long)cur_epoch, off, more ? "true" : "false");
    httpd_resp_send_chunk(req, meta, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t data_logger_register_http(void)
{
    static const httpd_uri_t URIS[] =
    {
        { .uri = "/api/logger", .method = HTTP_GET,
          .handler = logger_handler },
        { .uri = "/api/logger/gate", .method = HTTP_POST,
          .handler = gate_post_handler },
        { .uri = "/api/logger/export", .method = HTTP_GET,
          .handler = export_get_handler },
    };

    esp_err_t err = http_server_manager_register_handlers(
        URIS, sizeof(URIS) / sizeof(URIS[0]));

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "/api/logger registered");
    }

    return err;
}
