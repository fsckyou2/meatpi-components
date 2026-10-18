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
 * @file fw_update_dongle_job.c
 * @brief The relay job: the dongle manifest again, the image into a PSRAM
 *        staging buffer, the digest, the image head, the product and the
 *        version checked, the push into the dongle's own upload route over
 *        the USB link or its WiFi, the dongle's restart and its new version
 *        waited for (2026-10-19, internal/TASK_fw_update.md section 6.2).
 */
#include "fw_update_dongle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "http_client_manager.h"

static const char *TAG = FWU_TAG;

static bool sha256_hex_of(const uint8_t *buf, size_t len, char hex[65])
{
    static const char DIGITS[] = "0123456789abcdef";
    uint8_t out[32];
    size_t olen = 0;

    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, buf, len, out, sizeof(out), &olen)
            != PSA_SUCCESS || olen != 32)
    {
        return false;
    }

    for (size_t i = 0; i < 32; i++)
    {
        hex[2 * i] = DIGITS[out[i] >> 4];
        hex[2 * i + 1] = DIGITS[out[i] & 0x0F];
    }

    hex[64] = '\0';
    return true;
}

/* ---- the relay job ----------------------------------------------------------------------- */

static void dl_progress(size_t received, size_t total)
{
    fwu_engine_lock();
    FWU->job.received = (uint32_t)received;
    FWU->job.total = (uint32_t)total;
    fwu_engine_unlock();
}

static bool push_progress(void *ctx, size_t sent, size_t total)
{
    (void)ctx;
    fwu_engine_lock();
    FWU->job.received = (uint32_t)sent;
    FWU->job.total = (uint32_t)total;
    fwu_engine_unlock();
    return true; /* an abort during the push is too late (HTTP_API.md) */
}

static void set_state(fw_update_job_state_t st)
{
    fwu_engine_lock();
    FWU->job.state = st;
    fwu_engine_unlock();
    fwu_notify();
}

/** Wait until @p pred holds for the endpoint, polling once a second. */
static bool wait_endpoint(bool (*pred)(const fw_update_dongle_endpoint_t *ep),
                          uint32_t limit_s, fw_update_dongle_endpoint_t *ep)
{
    for (uint32_t s = 0; s <= limit_s; s++)
    {
        if (fwd_endpoint_now(ep) && pred(ep))
        {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return false;
}

static bool pred_steady(const fw_update_dongle_endpoint_t *ep)
{
    return ep->reachable && ep->steady;
}

static bool pred_dropped(const fw_update_dongle_endpoint_t *ep)
{
    return !ep->reachable || (strcmp(ep->via, "usb") == 0 && !ep->usb_attached);
}

/** The job ends: the row's last result, the job's final state. */
static void finish(uint32_t id, fw_update_job_state_t state, const char *code,
                   const char *detail, const char *from, const char *to,
                   uint32_t push_s)
{
    fwu_engine_lock();

    if (state == FW_JOB_VERIFIED)
    {
        FWU->job.state = FW_JOB_VERIFIED;
        FWU->job.active = false;
        FWU->job.received = FWU->job.total;
        ESP_LOGI(TAG, "dongle job %lu: %s installed and verified (push %lu s)",
                 (unsigned long)id, to, (unsigned long)push_s);
    }
    else
    {
        fwu_job_fail(state, code, detail);
    }

    memset(&FWD->last, 0, sizeof(FWD->last));
    FWD->last.valid = true;
    FWD->last.id = id;
    FWD->last.state = state;
    FWD->last.time = (int64_t)time(NULL);
    FWD->last.push_s = push_s;
    strlcpy(FWD->last.from, from, sizeof(FWD->last.from));
    strlcpy(FWD->last.to, to, sizeof(FWD->last.to));
    strlcpy(FWD->last.error, (code != NULL) ? code : "", sizeof(FWD->last.error));
    strlcpy(FWD->last.detail, (detail != NULL) ? detail : "", sizeof(FWD->last.detail));

    if (FWD->last.time < 1600000000LL)
    {
        FWD->last.time = 0;
    }

    fwu_engine_unlock();
    fwu_notify();
}

void fwu_dongle_install(const fwu_cmd_t *cmd)
{
    const fwu_config_t *cfg = fwu_settings_config();
    const fw_update_dongle_ops_t *ops = FWU->dongle;
    fw_update_dongle_endpoint_t ep;
    fwu_fetch_result_t r;
    char code[FW_UPDATE_CODE_LEN] = "";
    char detail[FW_UPDATE_DETAIL_LEN] = "";
    char from[24] = "";
    uint8_t *buf = NULL;
    bool held = false;
    uint32_t push_s = 0;

    fwu_engine_lock();
    fwu_job_begin(cmd->job_id, FW_UPDATE_TARGET_DONGLE, cmd->version, cmd->flags,
                  FW_JOB_CHECKING);
    strlcpy(FWU->job.transport, FWD->ep.via, sizeof(FWU->job.transport));
    strlcpy(from, FWD->ep.fw_version, sizeof(from));
    FWU->abort = false;
    fwu_engine_unlock();
    FWU->hold_us = 0;
    fwu_renew_hold();
    fwu_notify();

    if (ops == NULL)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "dongle_absent", "", from, cmd->version, 0);
        return;
    }

    /* 1. the manifest again: a yank must beat a cached view */
    esp_err_t err = fwu_fetch_manifest(cfg, FWU_DONGLE_DEVICE_TYPE, NULL, &r);

    fwu_engine_lock();

    if (err != ESP_OK)
    {
        fwu_engine_unlock();
        free(r.body);
        finish(cmd->job_id, FW_JOB_FAILED, "manifest_fetch_failed", r.code, from,
               cmd->version, 0);
        return;
    }

    char why[64];
    fwu_parse_rc_t prc = fwu_manifest_parse(r.body, r.len, FWU_DONGLE_DEVICE_TYPE,
                                            cfg->base_is_http, &FWD->view, why,
                                            sizeof(why));

    free(r.body);
    FWD->view_valid = prc == FWU_PARSE_OK;
    strlcpy(FWD->etag, FWD->view_valid ? r.etag : "", sizeof(FWD->etag));
    fwd_select();

    int idx = FWD->view_valid ? fwu_find_version(&FWD->view, cmd->version) : -1;

    if (idx < 0)
    {
        fwu_engine_unlock();
        finish(cmd->job_id, FW_JOB_FAILED, "unknown_version",
               FWD->view_valid ? "the release left the manifest" : why, from,
               cmd->version, 0);
        return;
    }

    memcpy(&FWD->rel, &FWD->view.rel[idx], sizeof(FWD->rel));

    if (FWD->rel.yanked)
    {
        fwu_engine_unlock();
        finish(cmd->job_id, FW_JOB_FAILED, "yanked", FWD->rel.yank_reason, from,
               cmd->version, 0);
        return;
    }

    FWU->job.total = FWD->rel.size;
    fwu_engine_unlock();

    /* 2. the dongle must be there before a byte is downloaded */
    if (!fwd_endpoint_now(&ep) || !ep.present || !ep.reachable)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "no_dongle",
               "the dongle is not reachable", from, cmd->version, 0);
        return;
    }

    fwu_engine_lock();
    FWD->ep = ep;
    FWD->ep_valid = true;
    strlcpy(FWU->job.transport, ep.via, sizeof(FWU->job.transport));
    fwu_engine_unlock();

    /* 3. the staging buffer */
    if (!fwd_stage_fits(FWD->rel.size, detail))
    {
        finish(cmd->job_id, FW_JOB_FAILED, "no_memory", detail, from, cmd->version, 0);
        return;
    }

    buf = heap_caps_malloc(FWD->rel.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "no_memory", "the staging buffer", from,
               cmd->version, 0);
        return;
    }

    /* 4. the download, into RAM */
    set_state(FW_JOB_DOWNLOADING);

    uint8_t attempt = 0;

    err = fwu_fetch_image_to_ram(&FWD->rel, cfg, buf, dl_progress, &FWU->abort,
                                 &attempt, code, detail);
    fwu_engine_lock();
    FWU->job.attempt = attempt;
    fwu_engine_unlock();

    if (err != ESP_OK)
    {
        finish(cmd->job_id, strcmp(code, "aborted") == 0 ? FW_JOB_ABORTED : FW_JOB_FAILED,
               code, detail, from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    /* 5. the checks: the digest, the image head, the product, the version */
    set_state(FW_JOB_VERIFYING);

    char hex[65];
    char project[33];
    char version[33];

    if (!sha256_hex_of(buf, FWD->rel.size, hex) || strcmp(hex, FWD->rel.sha256) != 0)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "sha256_mismatch",
               "the download does not match the manifest digest", from,
               cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (!fwu_image_head_parse(buf, FWD->rel.size, project, sizeof(project), version,
                              sizeof(version)))
    {
        finish(cmd->job_id, FW_JOB_FAILED, "wrong_image",
               "not an ESP32-S3 application image", from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    const char *want_project = FWD->view.project_name[0] ? FWD->view.project_name
                                                       : "espnetlink-fw";

    if (strncmp(project, want_project, strlen(want_project)) != 0)
    {
        snprintf(detail, sizeof(detail), "the image is %s, not %s", project, want_project);
        finish(cmd->job_id, FW_JOB_FAILED, "wrong_product", detail, from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (strcmp(version, FWD->rel.fw_version) != 0)
    {
        snprintf(detail, sizeof(detail), "the image says %s, the manifest %s", version,
                 FWD->rel.fw_version);
        finish(cmd->job_id, FW_JOB_FAILED, "wrong_image", detail, from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (FWU->abort)
    {
        finish(cmd->job_id, FW_JOB_ABORTED, "aborted", "", from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    /* 6. before the push: the dongle idle, its upload route there */
    if (!wait_endpoint(pred_steady, FWD_STEADY_WAIT_S, &ep))
    {
        finish(cmd->job_id, FW_JOB_FAILED, "no_dongle",
               ep.reachable ? "the dongle link is busy pairing" : "the dongle is not reachable",
               from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    int probe = fwd_probe_ota(ep.host);

    fwu_engine_lock();
    fwd_note_probe(&ep, probe);
    fwu_engine_unlock();

    if (probe == 0)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "needs_usb_flash",
               "the dongle firmware has no update route: flash it once over USB from a computer",
               from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (probe == 2)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "dongle_busy",
               "the dongle is receiving another image", from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (probe < 0)
    {
        finish(cmd->job_id, FW_JOB_FAILED, "no_dongle",
               "the dongle web server does not answer", from, cmd->version, 0);
        heap_caps_free(buf);
        return;
    }

    if (ops->hold != NULL)
    {
        held = ops->hold(true) == ESP_OK;
    }

    /* 7. the push */
    fwu_engine_lock();
    FWU->job.state = FW_JOB_PUSHING;
    FWU->job.received = 0;
    FWU->job.total = FWD->rel.size;
    fwu_engine_unlock();
    fwu_notify();

    char url[128];
    http_client_upload_info_t info;

    snprintf(url, sizeof(url), "http://%s/api/ota/upload?sha256=%s&source=wican", ep.host,
             FWD->rel.sha256);

    http_client_request_t req =
    {
        .url          = url,
        .method       = HTTP_CLIENT_POST,
        .body         = buf,
        .body_len     = FWD->rel.size,
        .content_type = "application/octet-stream",
        .timeout_ms   = FWD_PUSH_TIMEOUT_MS,
    };
    int64_t t0 = esp_timer_get_time();

    ESP_LOGI(TAG, "dongle job %lu: pushing %s (%lu bytes) over %s", (unsigned long)cmd->job_id,
             FWD->rel.version_str, (unsigned long)FWD->rel.size, ep.via);
    err = http_client_manager_upload(&req, FWD_PUSH_CHUNK, push_progress, NULL, &info);
    push_s = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
    heap_caps_free(buf);
    buf = NULL;

    bool ok = false;

    if (err == ESP_OK && info.status_code == 200)
    {
        cJSON *ans = cJSON_Parse(info.body);

        ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ans, "ok"));
        cJSON_Delete(ans);

        if (!ok)
        {
            snprintf(detail, sizeof(detail), "%.80s", info.body);
            strlcpy(code, "dongle_rejected", sizeof(code));
        }
    }
    else if (err != ESP_OK)
    {
        snprintf(detail, sizeof(detail), "%lu of %lu bytes sent (%s)",
                 (unsigned long)info.sent, (unsigned long)FWD->rel.size, esp_err_to_name(err));
        strlcpy(code, "push_failed", sizeof(code));
    }
    else if (info.status_code == 409)
    {
        strlcpy(code, "dongle_busy", sizeof(code));
        snprintf(detail, sizeof(detail), "%.80s", info.body);
    }
    else
    {
        cJSON *ans = cJSON_Parse(info.body);
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(ans, "error");

        snprintf(detail, sizeof(detail), "HTTP %d: %.70s", info.status_code,
                 cJSON_IsString(e) ? e->valuestring : info.body);
        cJSON_Delete(ans);
        strlcpy(code, "dongle_rejected", sizeof(code));
    }

    if (!ok)
    {
        if (held && ops->hold != NULL)
        {
            (void)ops->hold(false);
        }

        finish(cmd->job_id, FW_JOB_FAILED, code, detail, from, cmd->version, push_s);
        return;
    }

    ESP_LOGI(TAG, "dongle job %lu: image accepted in %lu s, the dongle restarts",
             (unsigned long)cmd->job_id, (unsigned long)push_s);

    /* 8. the restart: the drop, then the return */
    set_state(FW_JOB_REBOOTING);

    if (ops->expect_reboot != NULL)
    {
        ops->expect_reboot();
    }

    bool was_usb = strcmp(ep.via, "usb") == 0;

    if (!wait_endpoint(pred_dropped, FWD_DROP_WAIT_S, &ep))
    {
        ESP_LOGW(TAG, "dongle job %lu: the link never dropped; waiting for the new "
                      "firmware anyway", (unsigned long)cmd->job_id);
    }

    if (!wait_endpoint(pred_steady, was_usb ? FWD_RETURN_WAIT_USB_S : FWD_RETURN_WAIT_AP_S,
                       &ep))
    {
        if (held && ops->hold != NULL)
        {
            (void)ops->hold(false);
        }

        finish(cmd->job_id, FW_JOB_FAILED, "timeout_reboot",
               was_usb ? "the dongle did not come back on the USB link"
                       : "the dongle did not come back on its WiFi", from, cmd->version,
               push_s);
        return;
    }

    /* 9. the new version */
    set_state(FW_JOB_PENDING_VERIFY);

    bool verified = false;

    for (uint32_t s = 0; s < FWD_VERIFY_WAIT_S && !verified; s++)
    {
        if (fwd_endpoint_now(&ep) && fwd_version_equal(ep.fw_version, FWD->rel.fw_version))
        {
            verified = true;
            break;
        }

        if (s % 5 == 4 && ops->identify != NULL)
        {
            (void)ops->identify(5000, &ep);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (held && ops->hold != NULL)
    {
        (void)ops->hold(false);
    }

    fwu_engine_lock();
    FWD->ep = ep;
    FWD->ep_valid = true;
    FWD->ota_probed = false; /* the new firmware is probed afresh */
    fwd_select();
    fwu_engine_unlock();

    if (!verified)
    {
        snprintf(detail, sizeof(detail), "the dongle reports %s",
                 ep.fw_version[0] ? ep.fw_version : "nothing yet");
        finish(cmd->job_id, FW_JOB_FAILED, "version_mismatch_after", detail, from,
               cmd->version, push_s);
        return;
    }

    finish(cmd->job_id, FW_JOB_VERIFIED, "", "", from, FWD->rel.fw_version, push_s);
}
