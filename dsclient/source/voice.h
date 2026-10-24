// Push-to-Talk DS microphone codec and credit-windowed upload state.
//
// Pure C with no libnds dependency: the 4-bit IMA ADPCM encoder/decoder,
// peak level meter, and 8 KB sliding credit window are host-tested and
// cross-checked byte for byte against dsterm/voice.py.
//
// At 8,000 Hz mono, 4-bit IMA ADPCM produces 4,000 bytes/sec (less than a
// quarter of the camera viewfinder's 18 KB/s).  Every VOICE_CHUNK is at most
// 480 bytes of ADPCM so the sealed frame (6 header + 4 offset + 480 data +
// 8 tag = 498 bytes) fits inside a single 536-byte DS TCP segment.

#ifndef DSTERM_VOICE_H
#define DSTERM_VOICE_H

#include <stddef.h>
#include <stdint.h>

#define VOICE_FORMAT_IMA_ADPCM_8K 1

#define VOICE_SAMPLE_RATE 8000u
#define VOICE_BYTES_PER_SEC (VOICE_SAMPLE_RATE / 2u)   // 4000 B/s
#define VOICE_MAX_SECONDS 15u
#define VOICE_BYTES_MAX (VOICE_BYTES_PER_SEC * VOICE_MAX_SECONDS)  // 60000
#define VOICE_CHUNK_MAX 480u
#define VOICE_WINDOW_BYTES 8192u
#define VOICE_LANG_MAX 7u
#define VOICE_TEXT_MAX 480u

// Touch target on the Composer subscreen (rows 22..23) for holding MIC with
// a stylus or thumb when not using physical R.
#define VOICE_BAR_X 4
#define VOICE_BAR_Y (22 * 8)
#define VOICE_BAR_W 248
#define VOICE_BAR_H 16

typedef enum {
    VOICE_STATUS_OK = 0,
    VOICE_STATUS_BUSY = 1,
    VOICE_STATUS_INTEGRITY = 2,
    VOICE_STATUS_NO_STT = 3,
    VOICE_STATUS_EMPTY = 4,
    VOICE_STATUS_MAX = VOICE_STATUS_EMPTY,
} voice_status_t;

typedef struct {
    int32_t predictor;
    int32_t index;
} voice_adpcm_state_t;

void voice_adpcm_init(voice_adpcm_state_t *st);

// Encode `samples` (must be even) of 16-bit signed PCM into `samples / 2`
// bytes of 4-bit IMA ADPCM (low nibble first, high nibble second).
size_t voice_adpcm_encode(voice_adpcm_state_t *st, const int16_t *pcm,
                          size_t samples, uint8_t *out);

// Decode `bytes` of 4-bit IMA ADPCM into `2 * bytes` 16-bit signed PCM
// samples.
size_t voice_adpcm_decode(voice_adpcm_state_t *st, const uint8_t *adpcm,
                          size_t bytes, int16_t *pcm_out);

// Peak amplitude of `samples` PCM frames, mapped to 0..8 bars for the UI.
unsigned voice_peak_bars(const int16_t *pcm, size_t samples);

typedef struct {
    voice_adpcm_state_t adpcm;
    uint32_t total;     // ADPCM bytes encoded so far
    uint32_t sent;      // ADPCM bytes sent in VOICE_CHUNK frames
    uint32_t acked;     // ADPCM bytes acknowledged by VOICE_ACK
    uint32_t crc;       // running CRC-32 over encoded ADPCM bytes
    unsigned peak_bars; // latest 0..8 peak meter reading
    int full;           // 1 once VOICE_BYTES_MAX is reached
} voice_upload_t;

void voice_upload_init(voice_upload_t *u);

// Encode and append `samples` (even) of PCM into `buf` (capacity `cap`, at
// most VOICE_BYTES_MAX).  Updates `u->total`, `u->crc` and `u->peak_bars`.
// Returns how many ADPCM bytes were appended.
size_t voice_upload_append(voice_upload_t *u, uint8_t *buf, size_t cap,
                           const int16_t *pcm, size_t samples);

// Next chunk to send within the 8 KB credit window, or 0 if stalled on ACKs
// or nothing unsent remains.  While `recording` is 1, waits until at least
// VOICE_CHUNK_MAX bytes are ready so the link carries full chunks; once
// `recording` is 0, drains the final partial tail.
int voice_upload_next_chunk(voice_upload_t *u, const uint8_t *buf,
                            int recording, uint32_t *offset_out,
                            const uint8_t **data_out, uint16_t *len_out);

// Record a server VOICE_ACK credit.  Returns 0 if `acked` is outside
// [u->acked, u->sent].
int voice_upload_ack(voice_upload_t *u, uint32_t acked);

// True when a touch at (px, py) lands inside the Composer's MIC bar.
int voice_bar_hit(int px, int py);

// ------------------------------------------------- what the server owes ---
//
// VOICE_ACK and VOICE_TEXT carry no utterance id, so on their own a late
// reply to an utterance the person cancelled reads exactly like a reply to
// the one they started next.  Read as the new one's credit, a stale ack
// either trips the window check (and used to drop the whole connection) or,
// worse, credits bytes the server never saw; a stale transcription would put
// words the person took back into the composer.
//
// The tag is the order of the stream instead of a field.  The server answers
// every VOICE_CHUNK with exactly one VOICE_ACK and every VOICE_END with
// exactly one VOICE_TEXT (dsterm/server_voice.py), on one ordered TCP stream,
// and a malformed frame drops the link rather than going unanswered.  So the
// card counts what it is owed: on abandoning an utterance, whatever was still
// owed moves to a "stale" tally, and the next replies of that kind pay it off
// before anything is credited to the live utterance.
//
// Acks are exact: every ack for an old chunk was queued before the server
// even read the new VOICE_START.  Texts are not -- a new utterance's BUSY can
// overtake an old transcription still running on the computer -- so the card
// refuses to start a new utterance while a stale text is owed
// (voice_owed_can_start); the reply that pays it off always comes, BUSY or
// otherwise, and a reconnect clears the whole ledger.
typedef struct {
    uint32_t acks_live;    // VOICE_CHUNKs of the live utterance not yet acked
    uint32_t acks_stale;   // acks still owed for abandoned utterances' chunks
    uint32_t texts_stale;  // VOICE_TEXTs still owed for abandoned utterances
    uint8_t  text_live;    // the live utterance's VOICE_END has been sent
} voice_owed_t;

typedef enum {
    VOICE_REPLY_LIVE = 0,      // answers the utterance in progress
    VOICE_REPLY_STALE,         // answers an abandoned one: drop it, quietly
    VOICE_REPLY_UNEXPECTED,    // nothing was owed: drop it, quietly
} voice_reply_t;

void voice_owed_init(voice_owed_t *o);
// The live utterance sent one VOICE_CHUNK / its VOICE_END.
void voice_owed_chunk(voice_owed_t *o);
void voice_owed_end(voice_owed_t *o);
// The live utterance is over without its text (cancel, X, timeout, failed
// send): everything still owed for it becomes stale.
void voice_owed_abandon(voice_owed_t *o);
// Classify (and pay off) one arriving VOICE_ACK / VOICE_TEXT.
voice_reply_t voice_owed_ack(voice_owed_t *o);
voice_reply_t voice_owed_text(voice_owed_t *o);
// 0 while an abandoned utterance's text is still owed (see above).
int voice_owed_can_start(const voice_owed_t *o);

// ------------------------------------------- push-to-talk and latch ---
//
// Holding R for VOICE_HOLD_FRAMES (6 frames, ~100 ms) starts a recording so
// an L+R focus chord never opens the microphone by accident.  Which shoulder
// came first decides what L+R means, so the two never collide: L first, then
// R, is focus (the microphone needs R held *without* L, so it cannot start);
// R first, then L, is the microphone's -- latch in LATCH mode, cancel in
// HOLD.  In HOLD mode (the default), letting go of R stops and transcribes,
// and L+R cancels and shows the focus notice.  In LATCH mode, pressing L
// while R is held and recording latches the microphone on so both shoulders
// can be released; a subsequent tap (press edge) of R or L stops and
// transcribes, while the release that follows that tap is ignored until both
// shoulders are up.
#define VOICE_HOLD_FRAMES 6u

typedef enum {
    VOICE_PTT_NONE = 0,
    VOICE_PTT_START,       // R held for VOICE_HOLD_FRAMES: start recording
    VOICE_PTT_LATCH,       // L pressed while holding R in LATCH mode
    VOICE_PTT_STOP,        // R released (unlatched) or R/L tapped (latched)
    VOICE_PTT_AUTO_STOP,   // 60 KB cap (VOICE_BYTES_MAX) reached: stop + status
    VOICE_PTT_CANCEL,      // X pressed, or L+R chord when not latching
    VOICE_PTT_FOCUS,       // L held first, then R pressed: the focus chord
} voice_ptt_action_t;

typedef struct {
    unsigned r_frames;     // consecutive frames R held without L
    uint8_t  active;       // 1 while a shoulder-started recording is running
    uint8_t  latched;      // 1 once L has latched the active recording
    uint8_t  wait_release; // 1 after stop/cancel/full until both R and L are up
    uint8_t  chord_notice; // 1 when L+R acted as the composer focus-notice chord
} voice_ptt_t;

void voice_ptt_init(voice_ptt_t *p);
voice_ptt_action_t voice_ptt_step(voice_ptt_t *p, int latch_enabled,
                                  int r_held, int l_held,
                                  int r_edge, int l_edge, int x_edge,
                                  int recording, int waiting, int full);

#endif  // DSTERM_VOICE_H
