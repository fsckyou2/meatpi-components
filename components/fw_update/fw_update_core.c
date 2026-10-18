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
 * @file fw_update_core.c
 * @brief The manifest parse (bounded, tolerant of a malformed release,
 *        strict about the document) and the release selection: the one
 *        rule the device and the Home Assistant integration share. Pure.
 */
#include "fw_update_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ---- field helpers -------------------------------------------------------------- */

static void say(char *err, size_t err_len, const char *text)
{
    if (err != NULL && err_len > 0)
    {
        snprintf(err, err_len, "%s", text);
    }
}

/** A string field into @p out. @p allow_cut keeps the first cap-1 chars of
 *  a longer value (the summary); otherwise too long = false. */
static bool get_str(const cJSON *obj, const char *key, char *out, size_t cap,
                    bool allow_cut)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(v) || v->valuestring == NULL)
    {
        return false;
    }

    size_t n = strlen(v->valuestring);

    if (n >= cap)
    {
        if (!allow_cut)
        {
            return false;
        }

        n = cap - 1;
    }

    memcpy(out, v->valuestring, n);
    out[n] = '\0';
    return true;
}

/** Exactly @p n hex digits, stored lowercase. */
static bool get_hex(const cJSON *obj, const char *key, size_t n, char *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(v) || v->valuestring == NULL ||
        strlen(v->valuestring) != n)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        unsigned char c = (unsigned char)v->valuestring[i];

        if (!isxdigit(c))
        {
            return false;
        }

        out[i] = (char)tolower(c);
    }

    out[n] = '\0';
    return true;
}

static bool hex_ok(const char *s, size_t n)
{
    if (strlen(s) != n)
    {
        return false;
    }

    for (size_t i = 0; i < n; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return false;
        }
    }

    return true;
}

static bool url_ok(const char *url, bool allow_http)
{
    bool http;

    /* the same character rules as a base_url, the trailing slash aside */
    if (!fwu_base_url_valid(url, &http))
    {
        char tmp[FWU_URL_LEN + 1];
        size_t n = strlen(url);

        if (n + 1 >= sizeof(tmp))
        {
            return false;
        }

        memcpy(tmp, url, n);
        tmp[n] = '/';
        tmp[n + 1] = '\0';

        if (!fwu_base_url_valid(tmp, &http))
        {
            return false;
        }
    }

    return !http || allow_http;
}

/* ---- one release ---------------------------------------------------------------- */

static int migration_from_str(const char *s)
{
    if (strcmp(s, "lossless") == 0)
    {
        return FWU_LAYOUT_LOSSLESS;
    }

    if (strcmp(s, "wipe") == 0)
    {
        return FWU_LAYOUT_WIPE;
    }

    if (strcmp(s, "serial") == 0)
    {
        return FWU_LAYOUT_SERIAL;
    }

    return -1;
}

/** `layout_from`: up to FWU_MAX_FROM entries; a malformed entry is skipped
 *  (a device on that layout then reads "unknown", the safe side). */
static void parse_layout_from(const cJSON *from, fwu_release_t *r)
{
    const cJSON *e;

    r->n_from = 0;

    if (!cJSON_IsObject(from))
    {
        return;
    }

    cJSON_ArrayForEach(e, from)
    {
        fwu_layout_from_t *f;
        char mig[12];
        const cJSON *wipes;

        if (r->n_from >= FWU_MAX_FROM || e->string == NULL ||
            !hex_ok(e->string, FWU_MD5_LEN - 1) || !cJSON_IsObject(e))
        {
            continue;
        }

        if (!get_str(e, "migration", mig, sizeof(mig), false) ||
            migration_from_str(mig) < 0)
        {
            continue;
        }

        f = &r->from[r->n_from];
        memset(f, 0, sizeof(*f));

        for (size_t i = 0; i < FWU_MD5_LEN - 1; i++)
        {
            f->md5[i] = (char)tolower((unsigned char)e->string[i]);
        }

        f->migration = (uint8_t)migration_from_str(mig);
        wipes = cJSON_GetObjectItemCaseSensitive(e, "wipes");

        if (cJSON_IsArray(wipes))
        {
            const cJSON *w;
            size_t used = 0;

            cJSON_ArrayForEach(w, wipes)
            {
                if (!cJSON_IsString(w) || w->valuestring == NULL)
                {
                    continue;
                }

                int n = snprintf(f->wipes + used, sizeof(f->wipes) - used,
                                 "%s%s", used ? "," : "", w->valuestring);

                if (n < 0 || (size_t)n >= sizeof(f->wipes) - used)
                {
                    f->wipes[used] = '\0';
                    break;
                }

                used += (size_t)n;
            }
        }

        if (f->migration == FWU_LAYOUT_WIPE && f->wipes[0] == '\0')
        {
            continue; /* a wipe must say what it wipes */
        }

        r->n_from++;
    }
}

static bool parse_release(const cJSON *o, bool allow_http, fwu_release_t *r)
{
    const cJSON *image;
    const cJSON *layout;
    const cJSON *req;
    const cJSON *yanked;
    const cJSON *size;
    char channel[8];

    memset(r, 0, sizeof(*r));

    if (!cJSON_IsObject(o) ||
        !get_str(o, "version", r->version_str, sizeof(r->version_str), false) ||
        !fwu_version_parse_strict(r->version_str, &r->version) ||
        !get_str(o, "fw_version", r->fw_version, sizeof(r->fw_version), false) ||
        r->fw_version[0] == '\0' ||
        !get_str(o, "channel", channel, sizeof(channel), false) ||
        fwu_channel_from_str(channel) < 0 ||
        !get_str(o, "date", r->date, sizeof(r->date), false) ||
        !get_str(o, "summary", r->summary, sizeof(r->summary), true) ||
        !get_str(o, "notes", r->notes, sizeof(r->notes), false))
    {
        return false;
    }

    r->channel = (uint8_t)fwu_channel_from_str(channel);
    yanked = cJSON_GetObjectItemCaseSensitive(o, "yanked");

    if (!cJSON_IsBool(yanked))
    {
        return false;
    }

    r->yanked = cJSON_IsTrue(yanked);

    if (r->yanked)
    {
        (void)get_str(o, "yank_reason", r->yank_reason, sizeof(r->yank_reason),
                      true);
    }

    image = cJSON_GetObjectItemCaseSensitive(o, "image");
    size = cJSON_IsObject(image)
               ? cJSON_GetObjectItemCaseSensitive(image, "size") : NULL;

    if (!cJSON_IsObject(image) ||
        !get_str(image, "url", r->url, sizeof(r->url), false) ||
        !url_ok(r->url, allow_http) ||
        !cJSON_IsNumber(size) || size->valuedouble < 65536.0 ||
        size->valuedouble > 4294967295.0 ||
        !get_hex(image, "sha256", FWU_SHA_LEN - 1, r->sha256))
    {
        return false;
    }

    r->size = (uint32_t)size->valuedouble;
    (void)get_str(image, "project_name", r->image_project,
                  sizeof(r->image_project), true);

    layout = cJSON_GetObjectItemCaseSensitive(o, "layout");

    if (!cJSON_IsObject(layout) ||
        !get_hex(layout, "md5", FWU_MD5_LEN - 1, r->layout_md5))
    {
        return false;
    }

    parse_layout_from(cJSON_GetObjectItemCaseSensitive(o, "layout_from"), r);

    req = cJSON_GetObjectItemCaseSensitive(o, "requires");

    if (cJSON_IsObject(req))
    {
        if (!get_str(req, "min_version", r->min_version_str,
                     sizeof(r->min_version_str), false) ||
            !fwu_version_parse_strict(r->min_version_str, &r->min_version) ||
            fwu_version_cmp(&r->min_version, &r->version) >= 0)
        {
            return false;
        }

        r->has_min = true;
    }

    return true;
}

/* ---- the document ---------------------------------------------------------------- */

/** Newest first. One caller at a time (the job lock): the swap buffer is
 *  static because a release is a kilobyte, too big for a task stack. */
static void sort_releases(fwu_manifest_t *m)
{
    static fwu_release_t tmp;

    for (size_t i = 1; i < m->n; i++)
    {
        size_t j = i;

        while (j > 0 &&
               fwu_version_cmp(&m->rel[j - 1].version, &m->rel[j].version) < 0)
        {
            memcpy(&tmp, &m->rel[j - 1], sizeof(tmp));
            memcpy(&m->rel[j - 1], &m->rel[j], sizeof(tmp));
            memcpy(&m->rel[j], &tmp, sizeof(tmp));
            j--;
        }
    }
}

fwu_parse_rc_t fwu_manifest_parse(const char *json, size_t len,
                                  const char *device_type, bool allow_http,
                                  fwu_manifest_t *out, char *err,
                                  size_t err_len)
{
    cJSON *root;
    const cJSON *schema;
    const cJSON *releases;
    const cJSON *r;
    fwu_parse_rc_t rc = FWU_PARSE_OK;

    say(err, err_len, "");

    if (json == NULL || out == NULL || device_type == NULL)
    {
        say(err, err_len, "no document");
        return FWU_PARSE_INVALID;
    }

    memset(out, 0, sizeof(*out));
    root = cJSON_ParseWithLength(json, len);

    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        say(err, err_len, "not a JSON object");
        return FWU_PARSE_INVALID;
    }

    schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    releases = cJSON_GetObjectItemCaseSensitive(root, "releases");

    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0)
    {
        say(err, err_len, "schema is not 1");
        rc = FWU_PARSE_INVALID;
    }
    else if (!get_str(root, "device_type", out->device_type,
                      sizeof(out->device_type), false))
    {
        say(err, err_len, "no device_type");
        rc = FWU_PARSE_INVALID;
    }
    else if (strcmp(out->device_type, device_type) != 0)
    {
        say(err, err_len, "another device's manifest");
        rc = FWU_PARSE_WRONG_DEVICE;
    }
    else if (!cJSON_IsArray(releases))
    {
        say(err, err_len, "no releases array");
        rc = FWU_PARSE_INVALID;
    }

    if (rc != FWU_PARSE_OK)
    {
        cJSON_Delete(root);
        return rc;
    }

    (void)get_str(root, "model", out->model, sizeof(out->model), true);
    (void)get_str(root, "project_name", out->project_name,
                  sizeof(out->project_name), true);
    (void)get_str(root, "updated", out->updated, sizeof(out->updated), true);

    cJSON_ArrayForEach(r, releases)
    {
        if (out->n >= FWU_MAX_RELEASES)
        {
            out->dropped++;
            continue;
        }

        if (parse_release(r, allow_http, &out->rel[out->n]))
        {
            out->n++;
        }
        else
        {
            out->dropped++;
        }
    }

    cJSON_Delete(root);

    if (out->n == 0)
    {
        say(err, err_len, "no valid release");
        return FWU_PARSE_EMPTY;
    }

    sort_releases(out);
    return FWU_PARSE_OK;
}

/* ---- the selection -------------------------------------------------------------- */

int fwu_find_version(const fwu_manifest_t *m, const char *version)
{
    if (m == NULL || version == NULL)
    {
        return -1;
    }

    for (size_t i = 0; i < m->n; i++)
    {
        if (strcmp(m->rel[i].version_str, version) == 0)
        {
            return (int)i;
        }
    }

    return -1;
}

fwu_layout_t fwu_layout_verdict(const fwu_release_t *r,
                                const char *flashed_md5, const char **wipes)
{
    if (wipes != NULL)
    {
        *wipes = "";
    }

    if (flashed_md5 == NULL || flashed_md5[0] == '\0')
    {
        return FWU_LAYOUT_UNKNOWN;
    }

    if (strcmp(r->layout_md5, flashed_md5) == 0)
    {
        return FWU_LAYOUT_NONE;
    }

    for (size_t i = 0; i < r->n_from; i++)
    {
        if (strcmp(r->from[i].md5, flashed_md5) == 0)
        {
            if (wipes != NULL && r->from[i].migration == FWU_LAYOUT_WIPE)
            {
                *wipes = r->from[i].wipes;
            }

            return (fwu_layout_t)r->from[i].migration;
        }
    }

    return FWU_LAYOUT_UNKNOWN;
}

static bool candidate(const fwu_manifest_t *m, size_t i, uint8_t channel)
{
    return !m->rel[i].yanked && fwu_channel_visible(channel, m->rel[i].channel);
}

/** The newest candidate strictly between @p low and @p high, -1 none. */
static int newest_between(const fwu_manifest_t *m, uint8_t channel,
                          const fwu_version_t *low, const fwu_version_t *high)
{
    for (size_t i = 0; i < m->n; i++)
    {
        if (candidate(m, i, channel) &&
            fwu_version_cmp(&m->rel[i].version, low) > 0 &&
            fwu_version_cmp(&m->rel[i].version, high) < 0)
        {
            return (int)i; /* newest first */
        }
    }

    return -1;
}

void fwu_select(const fwu_manifest_t *m, uint8_t channel,
                const char *installed_fw_version, const char *flashed_md5,
                fwu_selection_t *out)
{
    fwu_version_t installed = { 0 };

    memset(out, 0, sizeof(*out));
    out->target = -1;
    out->latest = -1;
    out->me = -1;
    out->wipes = "";

    if (m == NULL)
    {
        snprintf(out->reason, sizeof(out->reason), "no_release");
        return;
    }

    /* the installed release: by its exact fw_version, else by the words */
    if (installed_fw_version != NULL)
    {
        for (size_t i = 0; i < m->n; i++)
        {
            if (strcmp(m->rel[i].fw_version, installed_fw_version) == 0)
            {
                out->me = (int)i;
                break;
            }
        }
    }

    if (out->me >= 0)
    {
        installed = m->rel[out->me].version;
        out->installed_known = true;
        out->installed_parsed = true;
        out->installed_yanked = m->rel[out->me].yanked;
    }
    else if (fwu_version_parse_tolerant(installed_fw_version, &installed))
    {
        out->installed_parsed = true;
        out->dev_build = installed.dev;
    }

    for (size_t i = 0; i < m->n; i++)
    {
        if (candidate(m, i, channel))
        {
            out->latest = (int)i;
            break;
        }
    }

    if (out->latest < 0)
    {
        snprintf(out->reason, sizeof(out->reason), "no_release");
        return;
    }

    if (!out->installed_parsed)
    {
        snprintf(out->reason, sizeof(out->reason), "unknown_version");
        return;
    }

    /* a yanked installed release is replaced even by a lower target */
    if (fwu_version_cmp(&m->rel[out->latest].version, &installed) <= 0 &&
        !out->installed_yanked)
    {
        snprintf(out->reason, sizeof(out->reason), "up_to_date");
        return;
    }

    out->target = out->latest;

    while (m->rel[out->target].has_min &&
           fwu_version_cmp(&m->rel[out->target].min_version, &installed) > 0)
    {
        int between = newest_between(m, channel, &installed,
                                     &m->rel[out->target].version);

        if (between < 0)
        {
            out->target = -1;
            snprintf(out->reason, sizeof(out->reason), "no_path");
            return;
        }

        out->target = between;
        out->step = true;
    }

    out->available = true;
    out->verdict = fwu_layout_verdict(&m->rel[out->target], flashed_md5,
                                      &out->wipes);

    switch (out->verdict)
    {
        case FWU_LAYOUT_NONE:
        case FWU_LAYOUT_LOSSLESS:
        case FWU_LAYOUT_WIPE:
            out->installable = true;
            break;
        case FWU_LAYOUT_SERIAL:
            snprintf(out->reason, sizeof(out->reason), "layout_serial");
            break;
        default:
            snprintf(out->reason, sizeof(out->reason), "layout_unknown");
            break;
    }
}

/* ---- the image head -------------------------------------------------------------- */

static void copy_field(const uint8_t *src, size_t field_len, char *out, size_t cap)
{
    size_t n = 0;

    while (n < field_len && src[n] != '\0')
    {
        n++;
    }

    if (n >= cap)
    {
        n = cap - 1;
    }

    memcpy(out, src, n);
    out[n] = '\0';
}

bool fwu_image_head_parse(const uint8_t *head, size_t len, char *project,
                          size_t project_cap, char *version,
                          size_t version_cap)
{
    if (head == NULL || len < FWU_IMAGE_HEAD_LEN || project == NULL ||
        version == NULL || project_cap == 0 || version_cap == 0)
    {
        return false;
    }

    /* esp_image_header_t: the magic, then the chip id at offset 12 (LE) */
    if (head[0] != 0xE9 || (head[12] | (head[13] << 8)) != 0x0009)
    {
        return false;
    }

    /* esp_app_desc_t at 32: magic 0xABCD5432 (LE), version at +16,
       project_name at +48, 32 bytes each */
    if (head[32] != 0x32 || head[33] != 0x54 || head[34] != 0xCD ||
        head[35] != 0xAB)
    {
        return false;
    }

    copy_field(head + 32 + 16, 32, version, version_cap);
    copy_field(head + 32 + 48, 32, project, project_cap);
    return true;
}
