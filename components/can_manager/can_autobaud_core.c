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
 * @file can_autobaud_core.c
 * @brief Listen before talk: the bitrate and mode policy of the native CAN
 *        node (pure logic, host-testable). See can_autobaud_core.h.
 */
#include <string.h>

#include "can_autobaud_core.h"

static can_ab_action_t no_action(void)
{
    can_ab_action_t a = { 0 };

    return a;
}

static can_ab_action_t apply_action(const can_ab_t *ab, bool listen_only)
{
    can_ab_action_t a =
    {
        .apply       = true,
        .baud_kbps   = can_ab_baud(ab),
        .listen_only = listen_only,
    };

    return a;
}

static void restart_span(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                         uint32_t bad)
{
    ab->span_ms = now_ms;
    ab->span_frames0 = frames;
    ab->span_bad0 = bad;
}

/** Errors and no frame in this span, from a node that only listens: is it
 *  the second helping? The first one is noted and the count starts again
 *  (false). A frame in between cleared the note (see can_ab_step). */
static bool confirmed(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                      uint32_t bad)
{
    if (ab->suspect && now_ms - ab->suspect_ms >= CAN_AB_CONFIRM_MS)
    {
        ab->suspect = false;
        return true;
    }

    if (!ab->suspect)
    {
        ab->suspect = true;
        ab->suspect_ms = now_ms;
    }

    restart_span(ab, now_ms, frames, bad);
    return false;
}

static void next_candidate(can_ab_t *ab)
{
    ab->idx = (uint8_t)((ab->idx + 1u) % ab->cfg.n_candidates);
    ab->switches++;
    ab->verified = false;
    ab->adopted = false;
}

void can_ab_mark(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                 uint32_t bad)
{
    if (ab == NULL)
    {
        return;
    }

    ab->enter_ms = now_ms;
    ab->frames0 = frames;
    ab->suspect = false;
    restart_span(ab, now_ms, frames, bad);
}

uint16_t can_ab_baud(const can_ab_t *ab)
{
    if (ab == NULL || ab->cfg.n_candidates == 0)
    {
        return 500;
    }

    return ab->cfg.candidates[ab->idx];
}

const char *can_ab_state_name(const can_ab_t *ab)
{
    if (ab == NULL)
    {
        return "listening";
    }

    switch (ab->state)
    {
    case CAN_AB_RUNNING:
        return "running";

    case CAN_AB_MISMATCH:
        return "mismatch";

    default:
        return (ab->cfg.n_candidates > 1) ? "detecting" : "listening";
    }
}

can_ab_action_t can_ab_init(can_ab_t *ab, const can_ab_cfg_t *cfg,
                            uint32_t now_ms, uint32_t frames, uint32_t bad)
{
    if (ab == NULL)
    {
        return no_action();
    }

    memset(ab, 0, sizeof(*ab));

    if (cfg != NULL && cfg->n_candidates > 0)
    {
        ab->cfg = *cfg;

        if (ab->cfg.n_candidates > CAN_AB_MAX_CANDIDATES)
        {
            ab->cfg.n_candidates = CAN_AB_MAX_CANDIDATES;
        }
    }
    else
    {
        /* nothing to go by: listen at 500k and never talk */
        ab->cfg.candidates[0] = 500;
        ab->cfg.n_candidates = 1;
        ab->cfg.want_normal = false;
    }

    ab->state = CAN_AB_LISTENING;
    can_ab_mark(ab, now_ms, frames, bad);
    return apply_action(ab, true);
}

can_ab_action_t can_ab_want_normal(can_ab_t *ab, bool want_normal,
                                   uint32_t now_ms, uint32_t frames,
                                   uint32_t bad)
{
    if (ab == NULL || ab->cfg.n_candidates == 0)
    {
        return no_action();
    }

    ab->cfg.want_normal = want_normal;

    if (!want_normal)
    {
        if (!ab->normal)
        {
            return no_action();
        }

        ab->normal = false;
        can_ab_mark(ab, now_ms, frames, bad);
        return apply_action(ab, true);
    }

    if (ab->state == CAN_AB_RUNNING && !ab->normal)
    {
        ab->normal = true;
        can_ab_mark(ab, now_ms, frames, bad);
        return apply_action(ab, false);
    }

    return no_action(); /* no verdict yet: the policy promotes later */
}

can_ab_action_t can_ab_adopt(can_ab_t *ab, uint16_t kbps, uint32_t now_ms,
                             uint32_t frames, uint32_t bad, bool *taken)
{
    if (taken != NULL)
    {
        *taken = false;
    }

    if (ab == NULL || ab->cfg.n_candidates < 2 ||
        ab->state == CAN_AB_RUNNING || ab->bitten)
    {
        /* no policy, a fixed bitrate (the settings' word), a verdict of
           its own, or a bus that once carried unreadable traffic */
        return no_action();
    }

    /* the bus has to be silent at the candidate in use: the proof a fixed
       bitrate needs before it talks (a gatewayed OBD port speaks only
       when asked, and a tester is about to ask) */
    if (frames - ab->frames0 != 0 || bad - ab->span_bad0 != 0 ||
        now_ms - ab->enter_ms < CAN_AB_SILENT_MS)
    {
        return no_action();
    }

    uint8_t idx = ab->cfg.n_candidates;

    for (uint8_t i = 0; i < ab->cfg.n_candidates; i++)
    {
        if (ab->cfg.candidates[i] == kbps)
        {
            idx = i;
        }
    }

    if (idx == ab->cfg.n_candidates)
    {
        return no_action(); /* not a bitrate this node knows */
    }

    if (idx != ab->idx)
    {
        ab->idx = idx;
        ab->switches++;
    }

    ab->state = CAN_AB_RUNNING;
    ab->verified = false;
    ab->adopted = true;
    ab->misses = 0;
    ab->normal = ab->cfg.want_normal;
    can_ab_mark(ab, now_ms, frames, bad);

    if (taken != NULL)
    {
        *taken = true;
    }

    /* the configured mode at the stated bitrate (a bounce even when the
       candidate stays: the mode changes) */
    return apply_action(ab, !ab->cfg.want_normal);
}

/** Frames were read at the bitrate in use: the configured mode applies. */
static can_ab_action_t proven(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                              uint32_t bad)
{
    ab->verified = true;
    ab->misses = 0;
    ab->detected_kbps = can_ab_baud(ab);
    ab->state = CAN_AB_RUNNING;
    can_ab_mark(ab, now_ms, frames, bad);

    if (ab->cfg.want_normal && !ab->normal)
    {
        ab->normal = true;
        return apply_action(ab, false);
    }

    return no_action();
}

/** Errors and no frame: the bus carries traffic this bitrate cannot read. */
static can_ab_action_t mismatch(can_ab_t *ab, uint32_t now_ms,
                                uint32_t frames, uint32_t bad)
{
    bool was_normal = ab->normal;

    ab->bitten = true;
    ab->verified = false;
    ab->adopted = false;
    ab->normal = false;

    if (was_normal)
    {
        ab->demotions++;
    }

    if (ab->cfg.n_candidates > 1)
    {
        ab->misses++;

        if (ab->misses < ab->cfg.n_candidates)
        {
            next_candidate(ab);
            ab->state = CAN_AB_LISTENING;
            can_ab_mark(ab, now_ms, frames, bad);
            return apply_action(ab, true);
        }

        /* every candidate tried, none reads this bus: rest here instead
           of bouncing the node for ever (CAN_AB_RETRY_MS starts a new
           round) */
    }

    ab->state = CAN_AB_MISMATCH;
    can_ab_mark(ab, now_ms, frames, bad);

    /* the bitrate stays where it is; only a talking node changes mode */
    return was_normal ? apply_action(ab, true) : no_action();
}

can_ab_action_t can_ab_step(can_ab_t *ab, uint32_t now_ms, uint32_t frames,
                            uint32_t bad)
{
    if (ab == NULL || ab->cfg.n_candidates == 0)
    {
        return no_action();
    }

    uint32_t df_enter = frames - ab->frames0;
    uint32_t df_span = frames - ab->span_frames0;
    uint32_t db_span = bad - ab->span_bad0;
    uint32_t age = now_ms - ab->enter_ms;       /* wrap-safe (unsigned)   */
    uint32_t span_age = now_ms - ab->span_ms;
    bool garbage = (db_span >= CAN_AB_BAD_MIN) && (df_span == 0);

    if (df_span != 0)
    {
        ab->suspect = false; /* a frame was read: the errors were its own */
    }

    if (garbage && !ab->normal)
    {
        /* listening: the evidence has to come twice */
        garbage = confirmed(ab, now_ms, frames, bad);
    }

    if (ab->state == CAN_AB_RUNNING)
    {
        /* the watchdog: on the evidence, not at the end of the window (a
           talking node at the wrong bitrate destroys traffic meanwhile) */
        if (garbage)
        {
            return mismatch(ab, now_ms, frames, bad);
        }

        if (!ab->verified && df_enter >= CAN_AB_OK_FRAMES)
        {
            /* running on the word of the settings (a silent bus): frames
               at this bitrate are the proof that was missing */
            ab->verified = true;
            ab->detected_kbps = can_ab_baud(ab);
        }

        if (span_age >= CAN_AB_WATCH_MS)
        {
            restart_span(ab, now_ms, frames, bad);
        }

        return no_action();
    }

    /* LISTENING or MISMATCH: listen-only, waiting for evidence */
    if (df_enter >= CAN_AB_OK_FRAMES)
    {
        return proven(ab, now_ms, frames, bad);
    }

    uint32_t retry_ms = ab->cfg.retry_ms ? ab->cfg.retry_ms
                                         : CAN_AB_RETRY_MS;

    if (ab->state == CAN_AB_MISMATCH && ab->cfg.n_candidates > 1 &&
        age >= retry_ms)
    {
        /* auto, after a full round without a readable bitrate: again */
        ab->misses = 0;
        next_candidate(ab);
        ab->state = CAN_AB_LISTENING;
        can_ab_mark(ab, now_ms, frames, bad);
        return apply_action(ab, true);
    }

    if (garbage)
    {
        if (ab->state == CAN_AB_MISMATCH)
        {
            /* still unreadable: stay, count from here */
            restart_span(ab, now_ms, frames, bad);
            return no_action();
        }

        return mismatch(ab, now_ms, frames, bad);
    }

    if (ab->state == CAN_AB_LISTENING && df_enter == 0 && db_span == 0 &&
        age >= CAN_AB_SILENT_MS && ab->cfg.n_candidates == 1 && !ab->bitten)
    {
        /* a silent bus and a fixed bitrate: the configured mode, as the
           node always did (a gatewayed OBD port speaks only when asked) */
        ab->state = CAN_AB_RUNNING;
        can_ab_mark(ab, now_ms, frames, bad);

        if (ab->cfg.want_normal)
        {
            ab->normal = true;
            return apply_action(ab, false);
        }

        return no_action();
    }

    if (span_age >= CAN_AB_SPAN_MS)
    {
        if (ab->state == CAN_AB_MISMATCH && db_span == 0 && df_span == 0)
        {
            /* the unreadable traffic stopped: back to plain listening
               (bitten stays: silence will not promote this node) */
            ab->state = CAN_AB_LISTENING;
            can_ab_mark(ab, now_ms, frames, bad);
            return no_action();
        }

        restart_span(ab, now_ms, frames, bad);
    }

    return no_action();
}
