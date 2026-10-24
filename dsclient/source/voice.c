#include "voice.h"

#include <string.h>

#include "camera.h"

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t INDEX_TABLE[8] = {
    -1, -1, -1, -1, 2, 4, 6, 8
};

static int32_t clamp_i16(int32_t v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static int32_t clamp_index(int32_t idx)
{
    return idx < 0 ? 0 : (idx > 88 ? 88 : idx);
}

void voice_adpcm_init(voice_adpcm_state_t *st)
{
    if (!st) return;
    st->predictor = 0;
    st->index = 0;
}

static uint8_t encode_nibble(voice_adpcm_state_t *st, int16_t sample)
{
    const int32_t step = STEP_TABLE[st->index];
    int32_t diff = (int32_t)sample - st->predictor;
    uint8_t nibble = 0;
    if (diff < 0) {
        nibble = 8;
        diff = -diff;
    }
    int32_t vpdiff = step >> 3;
    if (diff >= step) {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
    }
    if (diff >= (step >> 1)) {
        nibble |= 2;
        diff -= (step >> 1);
        vpdiff += (step >> 1);
    }
    if (diff >= (step >> 2)) {
        nibble |= 1;
        vpdiff += (step >> 2);
    }
    if (nibble & 8) st->predictor -= vpdiff;
    else            st->predictor += vpdiff;
    st->predictor = clamp_i16(st->predictor);
    st->index = clamp_index(st->index + INDEX_TABLE[nibble & 7]);
    return nibble & 0x0F;
}

static int16_t decode_nibble(voice_adpcm_state_t *st, uint8_t nibble)
{
    const int32_t step = STEP_TABLE[st->index];
    int32_t vpdiff = step >> 3;
    if (nibble & 4) vpdiff += step;
    if (nibble & 2) vpdiff += (step >> 1);
    if (nibble & 1) vpdiff += (step >> 2);
    if (nibble & 8) st->predictor -= vpdiff;
    else            st->predictor += vpdiff;
    st->predictor = clamp_i16(st->predictor);
    st->index = clamp_index(st->index + INDEX_TABLE[nibble & 7]);
    return (int16_t)st->predictor;
}

size_t voice_adpcm_encode(voice_adpcm_state_t *st, const int16_t *pcm,
                          size_t samples, uint8_t *out)
{
    if (!st || !pcm || !out || samples < 2) return 0;
    const size_t pairs = samples / 2;
    for (size_t i = 0; i < pairs; i++) {
        const uint8_t lo = encode_nibble(st, pcm[2 * i]);
        const uint8_t hi = encode_nibble(st, pcm[2 * i + 1]);
        out[i] = (uint8_t)(lo | (hi << 4));
    }
    return pairs;
}

size_t voice_adpcm_decode(voice_adpcm_state_t *st, const uint8_t *adpcm,
                          size_t bytes, int16_t *pcm_out)
{
    if (!st || !adpcm || !pcm_out || bytes == 0) return 0;
    for (size_t i = 0; i < bytes; i++) {
        pcm_out[2 * i]     = decode_nibble(st, (uint8_t)(adpcm[i] & 0x0F));
        pcm_out[2 * i + 1] = decode_nibble(st, (uint8_t)((adpcm[i] >> 4) & 0x0F));
    }
    return bytes * 2;
}

unsigned voice_peak_bars(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return 0;
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (peak < 256) return 0;
    unsigned bars = (unsigned)((peak * 8) / 16384);
    if (bars < 1u) bars = 1u;
    if (bars > 8u) bars = 8u;
    return bars;
}

void voice_upload_init(voice_upload_t *u)
{
    if (!u) return;
    memset(u, 0, sizeof(*u));
    voice_adpcm_init(&u->adpcm);
}

size_t voice_upload_append(voice_upload_t *u, uint8_t *buf, size_t cap,
                           const int16_t *pcm, size_t samples)
{
    if (!u || !buf || !pcm || samples < 2) return 0;
    if (cap > VOICE_BYTES_MAX) cap = VOICE_BYTES_MAX;
    if (u->total >= cap) {
        u->full = 1;
        return 0;
    }
    u->peak_bars = voice_peak_bars(pcm, samples);
    const size_t space = cap - u->total;
    size_t pairs = samples / 2;
    if (pairs > space) {
        pairs = space;
        u->full = 1;
    }
    const size_t written = voice_adpcm_encode(&u->adpcm, pcm, pairs * 2,
                                              buf + u->total);
    u->crc = camera_crc32_update(u->crc, buf + u->total, written);
    u->total += (uint32_t)written;
    if (u->total >= cap) u->full = 1;
    return written;
}

int voice_upload_next_chunk(voice_upload_t *u, const uint8_t *buf,
                            int recording, uint32_t *offset_out,
                            const uint8_t **data_out, uint16_t *len_out)
{
    if (!u || !buf || !offset_out || !data_out || !len_out) return 0;
    if (u->sent >= u->total) return 0;
    if (u->sent - u->acked >= VOICE_WINDOW_BYTES) return 0;

    const uint32_t avail = u->total - u->sent;
    const uint32_t window = VOICE_WINDOW_BYTES - (u->sent - u->acked);
    uint32_t take = avail < VOICE_CHUNK_MAX ? avail : VOICE_CHUNK_MAX;
    if (take > window) take = window;
    if (recording && take < VOICE_CHUNK_MAX) return 0;
    if (take == 0) return 0;

    *offset_out = u->sent;
    *data_out = buf + u->sent;
    *len_out = (uint16_t)take;
    u->sent += take;
    return 1;
}

int voice_upload_ack(voice_upload_t *u, uint32_t acked)
{
    if (!u) return 0;
    if (acked < u->acked || acked > u->sent) return 0;
    u->acked = acked;
    return 1;
}

int voice_bar_hit(int px, int py)
{
    return px >= VOICE_BAR_X && px < VOICE_BAR_X + VOICE_BAR_W &&
           py >= VOICE_BAR_Y && py < VOICE_BAR_Y + VOICE_BAR_H;
}

// ------------------------------------------------- what the server owes ---

void voice_owed_init(voice_owed_t *o)
{
    if (o) memset(o, 0, sizeof(*o));
}

void voice_owed_chunk(voice_owed_t *o)
{
    if (o && o->acks_live < UINT32_MAX) o->acks_live++;
}

void voice_owed_end(voice_owed_t *o)
{
    if (o) o->text_live = 1;
}

void voice_owed_abandon(voice_owed_t *o)
{
    if (!o) return;
    // Saturating: a tally can only grow by one chunk per sent frame, so
    // this never binds in practice, but a wrap would forgive a debt.
    o->acks_stale = o->acks_live > UINT32_MAX - o->acks_stale
                        ? UINT32_MAX : o->acks_stale + o->acks_live;
    o->acks_live = 0;
    if (o->text_live && o->texts_stale < UINT32_MAX) o->texts_stale++;
    o->text_live = 0;
}

voice_reply_t voice_owed_ack(voice_owed_t *o)
{
    if (!o) return VOICE_REPLY_UNEXPECTED;
    // Old chunks' acks come first on the stream, so they are paid first.
    if (o->acks_stale > 0) {
        o->acks_stale--;
        return VOICE_REPLY_STALE;
    }
    if (o->acks_live > 0) {
        o->acks_live--;
        return VOICE_REPLY_LIVE;
    }
    return VOICE_REPLY_UNEXPECTED;
}

voice_reply_t voice_owed_text(voice_owed_t *o)
{
    if (!o) return VOICE_REPLY_UNEXPECTED;
    if (o->texts_stale > 0) {
        o->texts_stale--;
        return VOICE_REPLY_STALE;
    }
    if (o->text_live) {
        o->text_live = 0;
        // Every chunk is acked before its END is even read, so nothing of
        // this utterance can still be owed once its text is here.
        o->acks_live = 0;
        return VOICE_REPLY_LIVE;
    }
    return VOICE_REPLY_UNEXPECTED;
}

int voice_owed_can_start(const voice_owed_t *o)
{
    return o && o->texts_stale == 0;
}

// ------------------------------------------- push-to-talk and latch ---

void voice_ptt_init(voice_ptt_t *p)
{
    if (p) memset(p, 0, sizeof(*p));
}

voice_ptt_action_t voice_ptt_step(voice_ptt_t *p, int latch_enabled,
                                  int r_held, int l_held,
                                  int r_edge, int l_edge, int x_edge,
                                  int recording, int waiting, int full)
{
    if (!p) return VOICE_PTT_NONE;
    p->chord_notice = 0;

    if (!recording) {
        if (p->active && (r_held || l_held)) p->wait_release = 1u;
        p->active = 0;
        p->latched = 0;
    }

    if (x_edge && (recording || waiting)) {
        p->r_frames = 0;
        p->active = 0;
        p->latched = 0;
        p->wait_release = (r_held || l_held) ? 1u : 0u;
        return VOICE_PTT_CANCEL;
    }

    if (recording && full) {
        p->r_frames = 0;
        p->active = 0;
        p->latched = 0;
        p->wait_release = (r_held || l_held) ? 1u : 0u;
        return VOICE_PTT_AUTO_STOP;
    }

    // While latched, releasing R and L keeps recording; only a fresh press
    // edge of R or L stops and transcribes, and wait_release swallows the
    // hold and release of that stop tap so it cannot start a new utterance.
    if (p->latched) {
        if (r_edge || l_edge) {
            p->r_frames = 0;
            p->active = 0;
            p->latched = 0;
            p->wait_release = 1u;
            return VOICE_PTT_STOP;
        }
        return VOICE_PTT_NONE;
    }

    // L+R shoulder chord: latches if R was already holding a recording in
    // LATCH mode; otherwise keeps its focus-notice / cancel meaning.
    if (r_held && l_held && (r_edge || l_edge)) {
        // L was down first (or both landed together): focus, never the mic.
        // R is the second press, so no R-hold was counting toward a start.
        if (r_edge && !recording) {
            p->r_frames = 0;
            p->active = 0;
            p->latched = 0;
            p->wait_release = 1u;
            return VOICE_PTT_FOCUS;
        }
        if (recording && p->active && latch_enabled && l_edge) {
            p->latched = 1u;
            return VOICE_PTT_LATCH;
        }
        p->r_frames = 0;
        p->active = 0;
        p->latched = 0;
        p->wait_release = 1u;
        p->chord_notice = 1u;
        return recording ? VOICE_PTT_CANCEL : VOICE_PTT_NONE;
    }

    if (p->wait_release) {
        if (!r_held && !l_held) p->wait_release = 0;
        p->r_frames = 0;
        return VOICE_PTT_NONE;
    }

    if (r_held && !l_held) {
        if (++p->r_frames == VOICE_HOLD_FRAMES && !recording && !waiting) {
            p->active = 1u;
            return VOICE_PTT_START;
        }
    } else {
        const int was_active = p->active != 0;
        p->r_frames = 0;
        p->active = 0;
        if (was_active && recording) {
            p->wait_release = (r_held || l_held) ? 1u : 0u;
            return VOICE_PTT_STOP;
        }
    }
    return VOICE_PTT_NONE;
}

