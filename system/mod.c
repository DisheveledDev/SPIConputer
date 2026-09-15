/* mod.c — ProTracker MOD player, streamed from the card; see mod.h */
#include "mod.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "ff.h"
#include "fs_lua.h"

/* ------------------------------------------------------------------ */
/* Tables                                                              */
/* ------------------------------------------------------------------ */

/* Amiga periods, finetune 0, C-1 .. B-3 (three octaves). */
static const uint16_t s_periods[36] = {
    856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
    428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
    214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
};

/* Finetune multiplier 2^(-ft/96) in Q16, ft = -8..7 (index ft + 8). */
static const uint32_t s_finetune_q16[16] = {
    69433, 68933, 68438, 67945, 67456, 66971, 66489, 66011,
    65536, 65065, 64597, 64132, 63670, 63212, 62757, 62306,
};

/* ProTracker vibrato/tremolo sine, 32 steps of half a cycle. */
static const uint8_t s_vib_table[32] = {
    0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24,
};

/* Q16 output-frame step for an Amiga period at the mixer's rate:
 * frequency = 7093789.2 / (2 * period) Hz (PAL). */
static uint32_t step_for_period(uint16_t period) {
    if (period < 28) period = 28;
    /* 7093789.2 / 2 / 44100 * 65536 = 5,270,852 */
    return 5270852u / period;
}

static uint16_t finetuned(uint16_t period, int8_t finetune) {
    return (uint16_t)(((uint32_t)period * s_finetune_q16[finetune + 8]) >> 16);
}

/* Nearest note index (0..35) for a period, for arpeggio. */
static int note_index(uint16_t period) {
    int best = 0, best_d = 100000;
    for (int i = 0; i < 36; i++) {
        int d = abs((int)s_periods[i] - (int)period);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static uint16_t clamp_period(int p) {
    if (p < 113) return 113;
    if (p > 856) return 856;
    return (uint16_t)p;
}

/* ------------------------------------------------------------------ */
/* File access (core 1)                                                */
/* ------------------------------------------------------------------ */

static bool read_at(int32_t handle, uint32_t offset, void *dst, size_t len) {
    if (fs_lua_seek_handle(handle, (int32_t)offset) != FR_OK) {
        return false;
    }
    uint8_t *p = (uint8_t *)dst;
    while (len > 0) {
        size_t got = 0;
        if (fs_lua_read_chunk(handle, p, len, &got) != FR_OK || got == 0) {
            return false;
        }
        p += got;
        len -= got;
    }
    return true;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

void mod_free(mod_t *m) {
    if (!m) return;
    if (m->handle) {
        fs_lua_close_handle(m->handle);
    }
    for (int c = 0; c < MOD_CHANNELS; c++) free(m->ch[c].ring);
    free(m->pat[0]);
    free(m->pat[1]);
    free(m->pool);
    free(m);
}

/* Load pattern `num` into buffer `slot`. */
static bool load_pattern(mod_t *m, int num, int slot) {
    if (num >= m->pattern_count) {
        return false;
    }
    if (!read_at(m->handle, m->pattern_base + (uint32_t)num * MOD_PATTERN_BYTES,
                 m->pat[slot], MOD_PATTERN_BYTES)) {
        return false;
    }
    atomic_signal_fence(memory_order_seq_cst);
    m->pat_num[slot] = (uint8_t)num;
    return true;
}

mod_t *mod_load(const char *path, const char **err) {
    *err = NULL;
    int32_t handle = 0;
    if (fs_lua_open_read(path, &handle) != FR_OK) {
        *err = "cannot open module";
        return NULL;
    }
    uint8_t *hdr = (uint8_t *)malloc(MOD_HEADER_BYTES);
    mod_t *m = (mod_t *)calloc(1, sizeof(mod_t));
    if (!hdr || !m) {
        free(hdr);
        free(m);
        fs_lua_close_handle(handle);
        *err = "out of memory";
        return NULL;
    }
    m->handle = handle;
    if (!read_at(handle, 0, hdr, MOD_HEADER_BYTES)) {
        *err = "module too short";
        goto fail;
    }
    const uint8_t *sig = hdr + 1080;
    if (!(memcmp(sig, "M.K.", 4) == 0 || memcmp(sig, "M!K!", 4) == 0 ||
          memcmp(sig, "4CHN", 4) == 0 || memcmp(sig, "FLT4", 4) == 0)) {
        *err = (memcmp(sig, "6CHN", 4) == 0 || memcmp(sig, "8CHN", 4) == 0)
                   ? "only 4-channel modules are supported"
                   : "not a ProTracker module";
        goto fail;
    }
    memcpy(m->name, hdr, 20);
    m->name[20] = 0;

    uint32_t sample_bytes = 0;
    for (int i = 1; i <= MOD_SAMPLES; i++) {
        const uint8_t *s = hdr + 20 + (i - 1) * 30;
        mod_sample_t *smp = &m->samples[i];
        smp->length = (uint32_t)be16(s + 22) * 2;
        int ft = s[24] & 0x0f;
        smp->finetune = (int8_t)(ft >= 8 ? ft - 16 : ft);
        smp->volume = s[25] > 64 ? 64 : s[25];
        smp->loop_start = (uint32_t)be16(s + 26) * 2;
        smp->loop_len = (uint32_t)be16(s + 28) * 2;
        /* ProTracker's "no loop" is a loop of one word at 0 (0/2): the
         * sample plays through once, then silence. */
        if (smp->loop_len <= 2) smp->loop_len = 0;
        if (smp->loop_start + smp->loop_len > smp->length) {
            smp->loop_len = smp->length > smp->loop_start ? smp->length - smp->loop_start : 0;
        }
        sample_bytes += smp->length;
    }
    m->order_count = hdr[950];
    m->restart_pos = hdr[951] < MOD_ORDERS ? hdr[951] : 0;
    memcpy(m->order, hdr + 952, MOD_ORDERS);
    if (m->order_count == 0 || m->order_count > MOD_ORDERS) {
        *err = "bad order list";
        goto fail;
    }
    int npat = 0;
    for (int i = 0; i < MOD_ORDERS; i++) {
        if (m->order[i] + 1 > npat) npat = m->order[i] + 1;
    }
    m->pattern_count = (uint8_t)npat;
    m->pattern_base = MOD_HEADER_BYTES;
    uint32_t offset = m->pattern_base + (uint32_t)npat * MOD_PATTERN_BYTES;
    for (int i = 1; i <= MOD_SAMPLES; i++) {
        m->samples[i].file_offset = offset;
        offset += m->samples[i].length;
    }
    (void)sample_bytes;
    free(hdr);
    hdr = NULL;

    /* Buffers: two patterns, four rings, the resident pool. */
    m->pat[0] = (uint8_t *)malloc(MOD_PATTERN_BYTES);
    m->pat[1] = (uint8_t *)malloc(MOD_PATTERN_BYTES);
    m->pool = (int8_t *)malloc(MOD_POOL_BYTES);
    for (int c = 0; c < MOD_CHANNELS; c++) {
        m->ch[c].ring = (int8_t *)malloc(MOD_RING_BYTES);
        if (!m->ch[c].ring) {
            *err = "out of memory";
            goto fail;
        }
        m->ch[c].ring_sample = 0;
        m->ch[c].arp_note = 0xff;
    }
    if (!m->pat[0] || !m->pat[1] || !m->pool) {
        *err = "out of memory";
        goto fail;
    }
    m->pat_num[0] = m->pat_num[1] = 0xff;

    /* Resident samples: whole ones shortest first while the pool lasts,
     * then the head of every remaining one (a note must start at once;
     * the ring catches up behind the head). */
    int order[MOD_SAMPLES];
    int n = 0;
    for (int i = 1; i <= MOD_SAMPLES; i++) {
        if (m->samples[i].length > 0) order[n++] = i;
    }
    for (int a = 1; a < n; a++) { /* insertion sort by length */
        int v = order[a], b = a - 1;
        while (b >= 0 && m->samples[order[b]].length > m->samples[v].length) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = v;
    }
    /* Every sample gets a head even in a 31-sample module: shrink the
     * head until they all fit the pool (a note must never start late). */
    uint32_t head = MOD_HEAD_BYTES;
    while (head > 256 && (uint32_t)n * head > MOD_POOL_BYTES) head /= 2;
    for (int k = 0; k < n; k++) {
        mod_sample_t *smp = &m->samples[order[k]];
        uint32_t reserve = (uint32_t)(n - k - 1) * head; /* heads still to place */
        if (m->pool_used + smp->length + reserve <= MOD_POOL_BYTES) {
            smp->resident_len = smp->length;
        } else {
            smp->resident_len = smp->length < head ? smp->length : head;
            if (m->pool_used + smp->resident_len > MOD_POOL_BYTES) {
                smp->resident_len = 0;
            }
        }
        if (smp->resident_len) {
            smp->resident = m->pool + m->pool_used;
            if (!read_at(handle, smp->file_offset, m->pool + m->pool_used, smp->resident_len)) {
                *err = "module truncated (samples)";
                goto fail;
            }
            m->pool_used += smp->resident_len;
        }
    }

    /* The first two patterns of the order list. */
    if (!load_pattern(m, m->order[0], 0)) {
        *err = "module truncated (patterns)";
        goto fail;
    }
    if (m->order_count > 1 && m->order[1] != m->order[0]) {
        load_pattern(m, m->order[1], 1);
    }
    m->speed = 6;
    m->bpm = 125;
    m->frames_per_tick = AUDIO_SAMPLE_RATE * 5 / (2 * 125);
    m->jump_order = -1;
    m->jump_row = -1;
    m->pat_want = 0xff;
    return m;

fail:
    free(hdr);
    mod_free(m);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Streaming service (core 1)                                          */
/* ------------------------------------------------------------------ */

/* The sample frame at stream position `p`: the frame itself on the
 * first pass, then round the loop. */
static inline uint32_t stream_frame(const mod_sample_t *smp, uint32_t p) {
    uint32_t end = smp->loop_start + smp->loop_len;
    if (!smp->loop_len || p < end) return p;
    return smp->loop_start + (p - end) % smp->loop_len;
}

/* True when the ring holds the loop's whole streamed stretch: the fill
 * stopped at the loop end and every frame from the loop start (or the
 * head's end) is still in the window. Both cores use it: the filler to
 * stop, the reader to index the ring by frame after the wrap. */
static inline bool loop_held(const mod_channel_t *ch, const mod_sample_t *smp) {
    uint32_t end = smp->loop_start + smp->loop_len;
    uint32_t lo = smp->loop_start > smp->resident_len ? smp->loop_start : smp->resident_len;
    return smp->loop_len && ch->fill_end == end && lo >= ch->fill_start &&
           end - lo <= MOD_RING_BYTES;
}

void mod_service(mod_t *m) {
    if (!m) return;
    /* A pattern the player asked for. */
    uint32_t seq = m->pat_seq;
    if (seq != m->pat_done) {
        uint8_t want = m->pat_want, slot = m->pat_want_slot;
        if (want != 0xff && slot < 2) {
            load_pattern(m, want, slot);
        }
        m->pat_done = seq;
    }
    /* The rings: refill after a restart, then keep ahead of the reader. */
    for (int c = 0; c < MOD_CHANNELS; c++) {
        mod_channel_t *ch = &m->ch[c];
        uint32_t rs = ch->restart_seq;
        uint8_t sample = ch->ring_sample;
        if (sample == 0 || sample > MOD_SAMPLES) continue;
        const mod_sample_t *smp = &m->samples[sample];
        if (smp->resident_len >= smp->length) continue; /* fully resident */
        uint32_t stop = smp->loop_len ? smp->loop_start + smp->loop_len : smp->length;
        if (rs != ch->restart_done) {
            /* Start the window where the head ends, or where the note
             * will be when it leaves the resident part (a restart is
             * always a fresh pass, so positions are frames here). */
            uint32_t from = ch->read_pos;
            if (from < smp->resident_len) from = smp->resident_len;
            ch->fill_start = from;
            ch->fill_end = from;
            atomic_signal_fence(memory_order_seq_cst);
            ch->restart_done = rs;
        }
        uint32_t end = ch->fill_end;
        uint32_t reader = ch->read_pos;
        if (end >= stop) {
            /* The first pass is complete. A one-shot is done; a loop
             * whose streamed stretch the ring holds whole stays put (the
             * reader indexes it by frame); otherwise stream on round it. */
            if (!smp->loop_len || loop_held(ch, smp)) continue;
        }
        if (reader > end) {
            /* The reader outran the fill (an underrun): skip to it. */
            ch->fill_start = reader;
            ch->fill_end = reader;
            end = reader;
        }
        uint32_t ahead = end - reader;
        if (ahead + MOD_RING_CHUNK > MOD_RING_BYTES) continue; /* enough ahead */
        uint32_t frame = stream_frame(smp, end);
        uint32_t len = stop - frame; /* to the loop end / sample end */
        if (len > MOD_RING_CHUNK) len = MOD_RING_CHUNK;
        uint32_t idx = end % MOD_RING_BYTES;
        uint32_t first = MOD_RING_BYTES - idx;
        if (first > len) first = len;
        bool ok = read_at(m->handle, smp->file_offset + frame, ch->ring + idx, first);
        if (ok && len > first) {
            ok = read_at(m->handle, smp->file_offset + frame + first, ch->ring, len - first);
        }
        if (!ok) continue;
        atomic_signal_fence(memory_order_seq_cst);
        ch->fill_end = end + len;
    }
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

void mod_request_play(mod_t *m, int loop) {
    if (!m) return;
    m->req_loop = (uint8_t)(loop != 0);
    m->req_play = 1;
}

void mod_request_stop(mod_t *m) {
    if (!m) return;
    m->req_stop = 1;
}

bool mod_is_playing(const mod_t *m) {
    return m && !m->req_stop && (m->playing || m->req_play);
}

void mod_pause(mod_t *m) {
    if (!m) return;
    for (int c = 0; c < MOD_CHANNELS; c++) {
        m->ch[c].active = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Player (core 0)                                                     */
/* ------------------------------------------------------------------ */

/* The sample byte at frame `frame` / stream position `p` of the
 * channel's sample: from the resident data, the ring, or 0 when it has
 * not arrived. */
static inline int8_t fetch(mod_t *m, mod_channel_t *ch, const mod_sample_t *smp,
                           uint32_t frame, uint32_t p) {
    if (frame < smp->resident_len) {
        return smp->resident[frame];
    }
    uint32_t end = ch->fill_end, start = ch->fill_start;
    if (p >= start && p < end && end - p <= MOD_RING_BYTES) {
        return ch->ring[p % MOD_RING_BYTES];
    }
    if (loop_held(ch, smp) && frame >= start) {
        return ch->ring[frame % MOD_RING_BYTES];
    }
    m->underruns++;
    return 0;
}

/* Point the ring at a new position: a note-on, a sample offset or a
 * retrigger, each a fresh pass through the sample. */
static void ring_restart(mod_channel_t *ch, uint8_t sample, uint32_t pos) {
    ch->spos = pos;
    ch->read_pos = pos;
    ch->ring_sample = sample;
    atomic_signal_fence(memory_order_seq_cst);
    ch->restart_seq++;
}

static void note_on(mod_t *m, mod_channel_t *ch, uint16_t period, uint32_t offset) {
    if (ch->sample == 0 || ch->sample > MOD_SAMPLES) return;
    const mod_sample_t *smp = &m->samples[ch->sample];
    if (smp->length == 0) {
        ch->active = 0;
        return;
    }
    ch->period = period;
    ch->pos = offset < smp->length ? offset : smp->length;
    ch->frac = 0;
    ch->step = step_for_period(period);
    ch->active = 1;
    ch->vib_pos = 0;
    ch->trem_pos = 0;
    ch->vib_delta = 0;
    ch->trem_delta = 0;
    ring_restart(ch, ch->sample, ch->pos);
}

static void set_tempo(mod_t *m, int bpm) {
    if (bpm < 32) bpm = 32;
    if (bpm > 255) bpm = 255;
    m->bpm = (uint8_t)bpm;
    m->frames_per_tick = (uint32_t)AUDIO_SAMPLE_RATE * 5u / (2u * (uint32_t)bpm);
}

/* Request pattern `num` into the slot not playing (a prefetch or a jump). */
static int want_pattern(mod_t *m, int num) {
    for (int s = 0; s < 2; s++) {
        if (m->pat_num[s] == num) return s;
    }
    int slot = m->cur_slot ^ 1;
    if (m->pat_want == num && m->pat_want_slot == slot && m->pat_seq != m->pat_done) {
        return -1; /* already asked */
    }
    m->pat_num[slot] = 0xff;
    m->pat_want = (uint8_t)num;
    m->pat_want_slot = (uint8_t)slot;
    atomic_signal_fence(memory_order_seq_cst);
    m->pat_seq++;
    return -1;
}

/* Move to order `pos`, row `row`; the pattern may still be loading. */
static void goto_order(mod_t *m, int pos, int row) {
    if (pos >= m->order_count) {
        if (!m->loop) {
            m->playing = 0;
            for (int c = 0; c < MOD_CHANNELS; c++) m->ch[c].active = 0;
            return;
        }
        pos = m->restart_pos < m->order_count ? m->restart_pos : 0;
    }
    m->order_pos = (uint8_t)pos;
    m->row = (uint8_t)row;
    int slot = want_pattern(m, m->order[pos]);
    if (slot < 0) {
        m->waiting = 1;
    } else {
        m->cur_slot = (uint8_t)slot;
        m->waiting = 0;
        /* Prefetch the next order's pattern into the other buffer. */
        int next = pos + 1 < m->order_count ? m->order[pos + 1]
                                            : m->order[m->restart_pos < m->order_count ? m->restart_pos : 0];
        if (next != m->order[pos]) want_pattern(m, next);
    }
}

static void vol_slide(mod_channel_t *ch, uint8_t param) {
    int v = ch->volume;
    if (param >> 4) v += param >> 4; else v -= param & 0x0f;
    ch->volume = (uint8_t)(v < 0 ? 0 : (v > 64 ? 64 : v));
}

static void tone_porta(mod_channel_t *ch) {
    if (!ch->target || !ch->porta_speed) return;
    int p = ch->period;
    if (p < ch->target) {
        p += ch->porta_speed;
        if (p > ch->target) p = ch->target;
    } else if (p > ch->target) {
        p -= ch->porta_speed;
        if (p < ch->target) p = ch->target;
    }
    ch->period = (uint16_t)p;
    ch->step = step_for_period(ch->period);
}

static void vibrato(mod_channel_t *ch) {
    int depth = ch->vib_param & 0x0f, speed = ch->vib_param >> 4;
    int delta = (s_vib_table[ch->vib_pos & 31] * depth) / 128;
    if (ch->vib_pos & 32) delta = -delta;
    ch->vib_delta = (int16_t)delta;
    ch->vib_pos = (uint8_t)((ch->vib_pos + speed) & 63);
    ch->step = step_for_period(clamp_period(ch->period + delta));
}

static void tremolo(mod_channel_t *ch) {
    int depth = ch->trem_param & 0x0f, speed = ch->trem_param >> 4;
    int delta = (s_vib_table[ch->trem_pos & 31] * depth) / 64;
    if (ch->trem_pos & 32) delta = -delta;
    ch->trem_delta = (int16_t)delta;
    ch->trem_pos = (uint8_t)((ch->trem_pos + speed) & 63);
}

/* Tick 0: read the row's cells. */
static void play_row(mod_t *m) {
    const uint8_t *row = m->pat[m->cur_slot] + (uint32_t)m->row * MOD_CHANNELS * 4;
    for (int c = 0; c < MOD_CHANNELS; c++) {
        mod_channel_t *ch = &m->ch[c];
        const uint8_t *cell = row + c * 4;
        int sample = (cell[0] & 0xf0) | (cell[2] >> 4);
        int period = ((cell[0] & 0x0f) << 8) | cell[1];
        int effect = cell[2] & 0x0f;
        int param = cell[3];
        ch->effect = (uint8_t)effect;
        ch->param = (uint8_t)param;
        ch->vib_delta = 0;
        ch->trem_delta = 0;

        if (sample > 0 && sample <= MOD_SAMPLES) {
            ch->note_sample = (uint8_t)sample;
            ch->sample = (uint8_t)sample;
            ch->volume = m->samples[sample].volume;
        }
        uint32_t offset = 0;
        if (effect == 0x9) {
            if (param) ch->offset_param = (uint8_t)param;
            offset = (uint32_t)ch->offset_param << 8;
        }
        if (period > 0) {
            int ft = ch->sample ? m->samples[ch->sample].finetune : 0;
            if (effect == 0xE && (param >> 4) == 0x5) ft = (param & 0x0f) >= 8 ? (param & 0x0f) - 16 : (param & 0x0f);
            uint16_t fp = finetuned((uint16_t)period, (int8_t)ft);
            if (effect == 0x3 || effect == 0x5) {
                ch->target = fp;
            } else if (effect == 0xE && (param >> 4) == 0xD && (param & 0x0f) > 0) {
                ch->delayed_period = fp; /* note delay */
                ch->delay_tick = (uint8_t)(param & 0x0f);
            } else {
                note_on(m, ch, fp, offset);
                ch->arp_note = (uint8_t)note_index((uint16_t)period);
            }
        } else if (effect == 0x9 && ch->active) {
            ch->pos = offset < m->samples[ch->sample].length ? offset : ch->pos;
            ring_restart(ch, ch->sample, ch->pos);
        }

        switch (effect) {
        case 0x3: if (param) ch->porta_speed = (uint8_t)param; break;
        case 0x4: if (param & 0x0f) ch->vib_param = (uint8_t)((ch->vib_param & 0xf0) | (param & 0x0f));
                  if (param & 0xf0) ch->vib_param = (uint8_t)((ch->vib_param & 0x0f) | (param & 0xf0));
                  break;
        case 0x7: if (param & 0x0f) ch->trem_param = (uint8_t)((ch->trem_param & 0xf0) | (param & 0x0f));
                  if (param & 0xf0) ch->trem_param = (uint8_t)((ch->trem_param & 0x0f) | (param & 0xf0));
                  break;
        case 0x8: ch->pan = (int8_t)((param >> 1) - 64); break;
        case 0xB: m->jump_order = (int16_t)param; m->jump_row = 0; break;
        case 0xC: ch->volume = (uint8_t)(param > 64 ? 64 : param); break;
        case 0xD: {
            int r = (param >> 4) * 10 + (param & 0x0f);
            if (m->jump_order < 0) m->jump_order = (int16_t)(m->order_pos + 1);
            m->jump_row = (int16_t)(r < MOD_ROWS ? r : 0);
            break;
        }
        case 0xE:
            switch (param >> 4) {
            case 0x1: ch->period = clamp_period(ch->period - (param & 0x0f)); ch->step = step_for_period(ch->period); break;
            case 0x2: ch->period = clamp_period(ch->period + (param & 0x0f)); ch->step = step_for_period(ch->period); break;
            case 0x6:
                if ((param & 0x0f) == 0) {
                    ch->loop_row = m->row;
                } else if (ch->loop_count < (param & 0x0f)) {
                    ch->loop_count++;
                    m->jump_order = (int16_t)m->order_pos;
                    m->jump_row = ch->loop_row;
                } else {
                    ch->loop_count = 0;
                }
                break;
            case 0x9: ch->retrig = (uint8_t)(param & 0x0f); break;
            case 0xA: ch->volume = (uint8_t)(ch->volume + (param & 0x0f) > 64 ? 64 : ch->volume + (param & 0x0f)); break;
            case 0xB: ch->volume = (uint8_t)(ch->volume < (param & 0x0f) ? 0 : ch->volume - (param & 0x0f)); break;
            case 0xC: ch->cut_tick = (uint8_t)(param & 0x0f); if (ch->cut_tick == 0) ch->volume = 0; break;
            case 0xE: m->pattern_delay = (uint8_t)(param & 0x0f); break;
            default: break;
            }
            break;
        case 0xF:
            if (param == 0) break;
            if (param <= 32) m->speed = (uint8_t)param; else set_tempo(m, param);
            break;
        default: break;
        }
    }
    m->rows_played++;
}

/* Ticks 1..speed-1: the per-tick effects. */
static void play_tick(mod_t *m) {
    for (int c = 0; c < MOD_CHANNELS; c++) {
        mod_channel_t *ch = &m->ch[c];
        uint8_t param = ch->param;
        switch (ch->effect) {
        case 0x0:
            if (param && ch->arp_note != 0xff) {
                int n = ch->arp_note;
                int off = m->tick % 3 == 1 ? param >> 4 : (m->tick % 3 == 2 ? param & 0x0f : 0);
                n += off;
                if (n > 35) n = 35;
                int ft = ch->sample ? m->samples[ch->sample].finetune : 0;
                ch->step = step_for_period(finetuned(s_periods[n], (int8_t)ft));
            }
            break;
        case 0x1: ch->period = clamp_period(ch->period - param); ch->step = step_for_period(ch->period); break;
        case 0x2: ch->period = clamp_period(ch->period + param); ch->step = step_for_period(ch->period); break;
        case 0x3: tone_porta(ch); break;
        case 0x4: vibrato(ch); break;
        case 0x5: tone_porta(ch); vol_slide(ch, param); break;
        case 0x6: vibrato(ch); vol_slide(ch, param); break;
        case 0x7: tremolo(ch); break;
        case 0xA: vol_slide(ch, param); break;
        case 0xE:
            switch (param >> 4) {
            case 0x9:
                if (ch->retrig && m->tick % ch->retrig == 0) {
                    ch->pos = 0; ch->frac = 0; ch->active = 1;
                    ring_restart(ch, ch->sample, 0);
                }
                break;
            case 0xC: if (m->tick == ch->cut_tick) ch->volume = 0; break;
            case 0xD:
                if (m->tick == ch->delay_tick && ch->delayed_period) {
                    note_on(m, ch, ch->delayed_period, 0);
                    ch->delayed_period = 0;
                }
                break;
            default: break;
            }
            break;
        default: break;
        }
    }
}

static void advance_row(mod_t *m) {
    if (m->jump_order >= 0) {
        int pos = m->jump_order, row = m->jump_row < 0 ? 0 : m->jump_row;
        m->jump_order = -1;
        m->jump_row = -1;
        goto_order(m, pos, row);
        return;
    }
    if (m->row + 1 < MOD_ROWS) {
        m->row++;
        return;
    }
    goto_order(m, m->order_pos + 1, 0);
}

static void tick(mod_t *m) {
    if (m->waiting) {
        /* The pattern is still loading: try again, keep the clock. */
        int slot = -1;
        for (int s = 0; s < 2; s++) {
            if (m->pat_num[s] == m->order[m->order_pos]) slot = s;
        }
        if (slot < 0) return;
        m->cur_slot = (uint8_t)slot;
        m->waiting = 0;
        int next_pos = m->order_pos + 1 < m->order_count ? m->order_pos + 1 : m->restart_pos;
        if (next_pos < m->order_count && m->order[next_pos] != m->order[m->order_pos]) {
            want_pattern(m, m->order[next_pos]);
        }
    }
    if (m->tick == 0) {
        play_row(m);
    } else {
        play_tick(m);
    }
    m->tick++;
    if (m->tick >= m->speed) {
        m->tick = 0;
        if (m->pattern_delay) {
            m->pattern_delay--; /* EE: repeat the row */
        } else {
            advance_row(m);
        }
    }
}

void mod_advance(mod_t *m, int frames) {
    if (!m) return;
    if (m->req_stop) {
        m->req_stop = 0;
        m->playing = 0;
        for (int c = 0; c < MOD_CHANNELS; c++) m->ch[c].active = 0;
    }
    if (m->req_play) {
        m->req_play = 0;
        m->loop = m->req_loop;
        m->speed = 6;
        set_tempo(m, 125);
        m->tick = 0;
        m->frame_acc = 0;
        m->pattern_delay = 0;
        m->jump_order = -1;
        m->jump_row = -1;
        for (int c = 0; c < MOD_CHANNELS; c++) {
            mod_channel_t *ch = &m->ch[c];
            ch->active = 0;
            ch->sample = 0;
            ch->volume = 0;
            ch->pan = (c == 1 || c == 2) ? 32 : -32; /* Amiga L R R L, softened */
            ch->loop_row = 0;
            ch->loop_count = 0;
            ch->arp_note = 0xff;
            ch->peak = 0;
            ch->level = 0;
        }
        goto_order(m, 0, 0);
        m->playing = 1;
        m->frame_acc = m->frames_per_tick; /* the first tick is now */
    }
    if (!m->playing) return;
    /* Channel levels for visuals: the block's peak, decaying 1/32 a block. */
    for (int c = 0; c < MOD_CHANNELS; c++) {
        mod_channel_t *ch = &m->ch[c];
        int32_t lvl = ch->level;
        lvl -= lvl >> 5;
        int32_t peak = ch->peak > 32767 ? 32767 : ch->peak;
        if (peak > lvl) lvl = peak;
        ch->level = (uint16_t)lvl;
        ch->peak = 0;
    }
    m->frame_acc += (uint32_t)frames;
    while (m->frame_acc >= m->frames_per_tick && m->playing) {
        m->frame_acc -= m->frames_per_tick;
        tick(m);
    }
}

void mod_mix_frame(mod_t *m, int32_t *l, int32_t *r) {
    if (!m || !m->playing) return;
    for (int c = 0; c < MOD_CHANNELS; c++) {
        mod_channel_t *ch = &m->ch[c];
        if (!ch->active || ch->sample == 0) continue;
        const mod_sample_t *smp = &m->samples[ch->sample];
        uint32_t end = smp->loop_len ? smp->loop_start + smp->loop_len : smp->length;
        if (ch->pos >= end) {
            if (smp->loop_len) {
                /* The stream position runs on; the ring either holds the
                 * loop or is being filled round it (mod_service). */
                ch->pos = smp->loop_start + (ch->pos - end) % smp->loop_len;
            } else {
                ch->active = 0;
                continue;
            }
        }
        int32_t s0 = fetch(m, ch, smp, ch->pos, ch->spos);
        uint32_t next = ch->pos + 1, next_p = ch->spos + 1;
        if (next >= end) {
            if (smp->loop_len) next = smp->loop_start; else { next = ch->pos; next_p = ch->spos; }
        }
        int32_t s1 = fetch(m, ch, smp, next, next_p);
        int32_t s = (s0 * (int32_t)(65536 - ch->frac) + s1 * (int32_t)ch->frac) >> 8; /* 8-bit -> 16-bit */
        int vol = ch->volume + ch->trem_delta;
        if (vol < 0) vol = 0;
        if (vol > 64) vol = 64;
        /* Four channels share the range: 3/8 each keeps a loud module
         * clear of the clamp (the Amiga summed four 8-bit channels into
         * 14 bits the same way). */
        s = (s * vol * 3) >> 9;
        {
            int32_t as = s < 0 ? -s : s;
            if (as > ch->peak) ch->peak = as;
        }
        int pan = ch->pan;
        int lg = 255 - (pan > 0 ? pan * 4 : 0);
        int rg = 255 + (pan < 0 ? pan * 4 : 0);
        *l += (s * lg) >> 8;
        *r += (s * rg) >> 8;
        /* Advance. */
        ch->frac += ch->step;
        ch->pos += ch->frac >> 16;
        ch->spos += ch->frac >> 16;
        ch->frac &= 0xffff;
        ch->read_pos = ch->spos;
    }
}

/* ------------------------------------------------------------------ */
/* Tracker view helpers (core 1)                                       */
/* ------------------------------------------------------------------ */

void mod_note_name(uint16_t period, char out[4]) {
    static const char names[12][2] = {
        {'C', '-'}, {'C', '#'}, {'D', '-'}, {'D', '#'}, {'E', '-'}, {'F', '-'},
        {'F', '#'}, {'G', '-'}, {'G', '#'}, {'A', '-'}, {'A', '#'}, {'B', '-'},
    };
    if (period == 0) {
        out[0] = out[1] = out[2] = '.';
        out[3] = 0;
        return;
    }
    int n = note_index(period);
    out[0] = names[n % 12][0];
    out[1] = names[n % 12][1];
    out[2] = (char)('1' + n / 12);
    out[3] = 0;
}

bool mod_format_row(const mod_t *m, int pattern, int row, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    if (!m || row < 0 || row >= MOD_ROWS || cap < MOD_CHANNELS * 11) return false;
    const uint8_t *pat = NULL;
    for (int s = 0; s < 2; s++) {
        if (m->pat_num[s] == pattern) pat = m->pat[s];
    }
    if (!pat) return false;
    const uint8_t *cells = pat + (uint32_t)row * MOD_CHANNELS * 4;
    char *p = out;
    for (int c = 0; c < MOD_CHANNELS; c++) {
        const uint8_t *cell = cells + c * 4;
        int sample = (cell[0] & 0xf0) | (cell[2] >> 4);
        int period = ((cell[0] & 0x0f) << 8) | cell[1];
        int effect = cell[2] & 0x0f, param = cell[3];
        char note[4];
        mod_note_name((uint16_t)period, note);
        if (c) *p++ = ' ';
        memcpy(p, note, 3);
        p += 3;
        *p++ = ' ';
        if (sample) {
            *p++ = hex[sample >> 4];
            *p++ = hex[sample & 15];
        } else {
            *p++ = '.';
            *p++ = '.';
        }
        *p++ = ' ';
        if (effect || param) {
            *p++ = hex[effect];
            *p++ = hex[param >> 4];
            *p++ = hex[param & 15];
        } else {
            *p++ = '.';
            *p++ = '.';
            *p++ = '.';
        }
    }
    *p = 0;
    return true;
}
