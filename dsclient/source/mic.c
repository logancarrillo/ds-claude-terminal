#include "mic.h"

#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "host.h"
#include "lang.h"
#include "sound.h"
#include "subscreen.h"

typedef enum {
    MIC_OFF = 0,
    MIC_RECORDING,
    MIC_FLUSHING,
    MIC_WAITING,
} mic_state_t;

// 256 samples per half-buffer (32 ms at 8,000 Hz): 1,024 bytes total DMA ring.
#define MIC_HALF_SAMPLES 256u
#define MIC_DMA_SAMPLES (MIC_HALF_SAMPLES * 2u)
#define MIC_WAIT_TIMEOUT_FRAMES (20u * 60u)
// FLUSHING waits on the server's credit.  A whole 15-second utterance is
// 60 KB; ten seconds without one byte of progress is a stall, not a slow link.
#define MIC_FLUSH_TIMEOUT_FRAMES (10u * 60u)

static int16_t mic_dma_buf[MIC_DMA_SAMPLES] __attribute__((aligned(32)));
static int16_t mic_stage_buf[MIC_HALF_SAMPLES * 4u];
static volatile unsigned mic_stage_len = 0;

// 60,000 bytes (15 seconds of 4-bit IMA ADPCM at 8,000 Hz).
static uint8_t mic_adpcm_buf[VOICE_BYTES_MAX];
static voice_upload_t mic_upload;

static mic_state_t mic_state = MIC_OFF;
static int proved = 0;
static int can_transcribe = 0;
static int unavailable_said = 0;
static int mic_is_latched = 0;
static unsigned long waiting_since = 0;
static unsigned long flush_progress_at = 0;

// Whether the ARM7 is recording.  Every way out of RECORDING -- a release, a
// cancel, a full buffer, a failed send, a reconnect underneath a send -- goes
// through mic_hw_off(), and this flag makes it turn the microphone off exactly
// once however many of those paths a single exit happens to cross (a failed
// send reconnects, the reconnect forgets the mic, and the sender then cancels
// as well).
static int mic_hw_on = 0;

// What the server still owes for utterances on this connection (voice.h).
static voice_owed_t owed;

static void mic_hw_off(void)
{
    if (!mic_hw_on) return;
    mic_hw_on = 0;
    soundMicOff();
}

static void mic_sync_ui(void)
{
    subscreen_set_voice(proved && can_transcribe,
                        mic_state == MIC_RECORDING,
                        mic_state == MIC_FLUSHING || mic_state == MIC_WAITING,
                        mic_upload.peak_bars);
    subscreen_set_voice_latched(mic_is_latched && mic_state == MIC_RECORDING);
    host_refresh_sub();
}

static void mic_dma_cb(void *data, int byte_len)
{
    if (!data || byte_len <= 0 || mic_state != MIC_RECORDING) return;
    DC_InvalidateRange(data, (u32)byte_len);
    const unsigned samples = (unsigned)byte_len / sizeof(int16_t);
    const unsigned cur = mic_stage_len;
    const unsigned cap = (unsigned)(sizeof(mic_stage_buf) / sizeof(mic_stage_buf[0]));
    if (cur + samples <= cap) {
        memcpy(mic_stage_buf + cur, data, samples * sizeof(int16_t));
        mic_stage_len = cur + samples;
    }
}

static void mic_drain_stage(void)
{
    const int ime = enterCriticalSection();
    const unsigned n = mic_stage_len & ~1u;
    static int16_t local[MIC_HALF_SAMPLES * 4u];
    if (n > 0) {
        memcpy(local, mic_stage_buf, n * sizeof(int16_t));
        mic_stage_len = 0;
    }
    leaveCriticalSection(ime);
    if (n > 0) {
        (void)voice_upload_append(&mic_upload, mic_adpcm_buf,
                                  sizeof(mic_adpcm_buf), local, n);
    }
}

static int mic_send_chunks(int recording, unsigned long *tx)
{
    uint32_t offset = 0;
    const uint8_t *data = NULL;
    uint16_t len = 0;
    while (voice_upload_next_chunk(&mic_upload, mic_adpcm_buf, recording,
                                   &offset, &data, &len)) {
        static uint8_t out[PROTO_HEADER_SIZE + 4 + VOICE_CHUNK_MAX];
        const size_t n = proto_encode_voice_chunk(out, offset, data, len);
        if (!n || !host_send_voice(out, n, tx)) {
            // A failed send has already reconnected (and so forgotten the
            // ledger); an encoder refusal has not, and its chunk never left.
            mic_cancel();
            return 0;
        }
        // Counted only once it has left: the server acks what it received.
        voice_owed_chunk(&owed);
    }
    return 1;
}

void mic_ready(int transcribes)
{
    proved = 1;
    can_transcribe = transcribes ? 1 : 0;
    unavailable_said = 0;
    mic_sync_ui();
}

void mic_forget(void)
{
    mic_cancel();
    // A new connection owes nothing: the old one's replies died with it.
    voice_owed_init(&owed);
    proved = 0;
    can_transcribe = 0;
    unavailable_said = 0;
    mic_sync_ui();
}

int mic_proved(void) { return proved; }
int mic_can_transcribe(void) { return proved && can_transcribe; }
int mic_recording(void) { return mic_state == MIC_RECORDING; }
int mic_latched(void) { return mic_is_latched && mic_state == MIC_RECORDING; }
void mic_set_latched(int latched)
{
    mic_is_latched = (latched && mic_state == MIC_RECORDING) ? 1 : 0;
    mic_sync_ui();
}
int mic_waiting(void) { return mic_state == MIC_FLUSHING || mic_state == MIC_WAITING; }
int mic_full(void) { return mic_state == MIC_RECORDING && mic_upload.full; }
unsigned mic_peak_bars(void) { return mic_upload.peak_bars; }

void mic_explain_unavailable(void)
{
    // A server that never announced a microphone is an older one, and R in
    // the composer did nothing at all before the microphone existed: it still
    // does nothing.  One that did announce it without speech-to-text gets a
    // single line per connection, not one per press.
    if (!proved || can_transcribe || unavailable_said) return;
    unavailable_said = 1;
    host_status("voice: this computer has no speech-to-text");
}

int mic_start(const char *lang, unsigned long *tx)
{
    // The proof rule: no VOICE_START until this connection's MIC_READY said
    // it can transcribe.  Callers check first (mic_can_transcribe), so this
    // is the backstop and stays silent.
    if (!proved || !can_transcribe) return 0;
    if (mic_state != MIC_OFF) return 0;
    if (!voice_owed_can_start(&owed)) {
        host_status("voice: still finishing the last recording");
        return 0;
    }

    uint8_t out[PROTO_HEADER_SIZE + 2 + VOICE_LANG_MAX];
    const size_t n = proto_encode_voice_start(out, VOICE_FORMAT_IMA_ADPCM_8K,
                                              lang && lang[0] ? lang : "en");
    if (!n || !host_send_voice(out, n, tx)) return 0;

    voice_upload_init(&mic_upload);
    mic_stage_len = 0;
    mic_is_latched = 0;
    mic_state = MIC_RECORDING;
    // Marked on before the call: a refusal still gets its one soundMicOff().
    mic_hw_on = 1;
    if (!soundMicRecord(mic_dma_buf, sizeof(mic_dma_buf), MicFormat_12Bit,
                        VOICE_SAMPLE_RATE, mic_dma_cb)) {
        host_status("voice: the microphone did not start");
        mic_cancel();
        return 0;
    }
    mic_sync_ui();
    return 1;
}

void mic_stop(unsigned long *tx)
{
    if (mic_state != MIC_RECORDING) return;
    mic_hw_off();
    mic_is_latched = 0;
    mic_drain_stage();
    if (mic_upload.total == 0) {
        // Nothing was heard, so no chunk and no END left: nothing is owed.
        mic_state = MIC_OFF;
        mic_sync_ui();
        return;
    }
    mic_state = MIC_FLUSHING;
    flush_progress_at = host_frame_count();
    mic_tick(tx);
    mic_sync_ui();
}

void mic_cancel(void)
{
    mic_hw_off();
    // Whatever the server still owes this utterance now answers nothing:
    // book it as stale so it is recognised and dropped when it arrives.
    if (mic_state != MIC_OFF) voice_owed_abandon(&owed);
    mic_stage_len = 0;
    mic_is_latched = 0;
    voice_upload_init(&mic_upload);
    mic_state = MIC_OFF;
    mic_sync_ui();
}

void mic_tick(unsigned long *tx)
{
    if (mic_state == MIC_OFF) return;

    if (mic_state == MIC_RECORDING) {
        mic_drain_stage();
        if (!mic_send_chunks(1, tx)) return;
        if (mic_upload.full) {
            host_status("voice: 15s limit reached -- transcribing");
            mic_stop(tx);
            return;
        }
        mic_sync_ui();
        return;
    }

    if (mic_state == MIC_FLUSHING) {
        const uint32_t sent_before = mic_upload.sent;
        if (!mic_send_chunks(0, tx)) return;
        if (mic_upload.sent != sent_before) flush_progress_at = host_frame_count();
        if (mic_upload.sent >= mic_upload.total) {
            uint8_t out[PROTO_HEADER_SIZE + 8];
            const size_t n = proto_encode_voice_end(out, mic_upload.total,
                                                    mic_upload.crc);
            if (!n || !host_send_voice(out, n, tx)) {
                mic_cancel();
                return;
            }
            voice_owed_end(&owed);
            mic_state = MIC_WAITING;
            waiting_since = host_frame_count();
            mic_sync_ui();
            return;
        }
        // Stalled on credit the server never sends: the window is full and
        // nothing has moved.  Same bound and same outcome as WAITING's.
        if (host_frame_count() - flush_progress_at >= MIC_FLUSH_TIMEOUT_FRAMES) {
            host_status("voice: timed out sending the recording");
            mic_cancel();
        }
        return;
    }

    if (mic_state == MIC_WAITING) {
        if (host_frame_count() - waiting_since >= MIC_WAIT_TIMEOUT_FRAMES) {
            host_status("voice: timed out waiting for transcription");
            mic_cancel();
        }
    }
}

void mic_ack(uint32_t acked)
{
    // Every ack pays off one chunk in the ledger, whatever state the card is
    // in, so the count stays exact; only a live one in a sending state moves
    // the credit window.  A stale or unexpected ack is dropped and never
    // drops the connection: the frame was well formed and it was late.
    const voice_reply_t kind = voice_owed_ack(&owed);
    if (kind != VOICE_REPLY_LIVE) return;
    if (mic_state != MIC_RECORDING && mic_state != MIC_FLUSHING) return;
    if (!voice_upload_ack(&mic_upload, acked)) {
        // The live utterance's own ack, out of its window: the computer has
        // lost track of this recording.  Give up on it, not on the link.
        host_status("voice: the computer lost track of the recording");
        mic_cancel();
        return;
    }
    if (mic_state == MIC_FLUSHING) flush_progress_at = host_frame_count();
}

void mic_ack_malformed(void)
{
    // A malformed VOICE_ACK is still the one reply the server owed for a
    // chunk (the stream is ordered, one ack per chunk): pay the ledger, or
    // every later ack is booked one chunk off.  Its count is unreadable, so
    // a live upload cannot trust its window any more -- give that utterance
    // up the way an out-of-window ack does, never the link.
    const voice_reply_t kind = voice_owed_ack(&owed);
    if (kind != VOICE_REPLY_LIVE) return;
    if (mic_state != MIC_RECORDING && mic_state != MIC_FLUSHING) return;
    host_status("voice: a damaged reply from the computer -- try again");
    mic_cancel();
}

void mic_text_malformed(void)
{
    // A malformed VOICE_TEXT still answers one VOICE_END.  Ignored, it left
    // the ledger owing that text forever: voice_owed_can_start() said "still
    // finishing the last recording" on every press until a reconnect.  Book
    // it as the failed reply it is, so the next recording starts.
    const voice_reply_t kind = voice_owed_text(&owed);
    if (kind != VOICE_REPLY_LIVE || mic_state != MIC_WAITING) {
        host_status("ignored a malformed voice transcription");
        return;
    }
    mic_state = MIC_OFF;
    mic_upload.peak_bars = 0;
    mic_sync_ui();
    host_status("voice: the transcription arrived damaged -- try again");
}

int mic_text_arrived(const voice_text_t *vt)
{
    if (!vt) return 0;
    const voice_reply_t kind = voice_owed_text(&owed);
    if (kind != VOICE_REPLY_LIVE || mic_state != MIC_WAITING) {
        // Late for a recording that was cancelled or timed out, or answering
        // nothing this card asked: it must not reach the composer.
        host_status("voice: dropped a transcription nobody is waiting for");
        return 0;
    }

    mic_state = MIC_OFF;
    mic_upload.peak_bars = 0;
    mic_sync_ui();

    if (vt->status != VOICE_STATUS_OK) {
        const char *why = vt->status == VOICE_STATUS_BUSY ? "server busy" :
                          vt->status == VOICE_STATUS_INTEGRITY ? "audio damaged" :
                          vt->status == VOICE_STATUS_NO_STT ? "no speech-to-text on server" :
                          "no speech heard";
        host_status("voice: %s", why);
        return 0;
    }
    return 1;
}
