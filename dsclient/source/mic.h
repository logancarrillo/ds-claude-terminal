// Push-to-Talk microphone capture and upload choreography (DS / DSi / 3DS).
//
// Like photo.h, this file owns the hardware state (soundMicRecord on the
// ARM7, cache invalidation, and feeding voice.c's ADPCM encoder and 8 KB
// sliding credit window).  It reaches main.c only through host.h and sends
// only through host_send_voice(), which accepts MSG_VOICE_* and nothing else.
// Transcribed text returned in MSG_VOICE_TEXT lands in the local composer,
// never on the PTY directly -- and only text answering the utterance this
// card is waiting on; main.c, not this file, opens the composer for it.

#ifndef DSTERM_MIC_H
#define DSTERM_MIC_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"
#include "voice.h"

// MSG_MIC_READY arrived: this connection proved its microphone, and
// `transcribes` is its speech-to-text bit (PROTO_MIC_CAN_TRANSCRIBE).
void mic_ready(int transcribes);
void mic_forget(void);
int  mic_proved(void);
int  mic_can_transcribe(void);

int      mic_recording(void);
int      mic_latched(void);
void     mic_set_latched(int latched);
int      mic_waiting(void);
// The recording has reached VOICE_BYTES_MAX (about 15 s).  mic_tick() stops
// it itself, the same frame, with a status line -- so by the time the
// push-to-talk helper (voice_ptt_step) could see `full`, the recording is
// usually over.  The helper's VOICE_PTT_AUTO_STOP is the backstop for a
// frame where the two disagree, and it only ever stops.
int      mic_full(void);
unsigned mic_peak_bars(void);

// R was held in the composer and mic_can_transcribe() is 0: say why once per
// connection if this server announced a microphone without speech-to-text,
// and nothing at all for a server that never announced one.
void mic_explain_unavailable(void);

// Start recording an utterance in `lang` ("en", "es", ...).  Returns 1 if
// recording started.  Silent no-op unless this connection's MIC_READY proved
// speech-to-text (callers check mic_can_transcribe() first).
int  mic_start(const char *lang, unsigned long *tx);

// Stop recording and finish uploading the utterance (sends VOICE_END once the
// credit window drains).
void mic_stop(unsigned long *tx);

// Abort any active recording or pending transcription immediately.  The
// ARM7 microphone is off afterwards, and anything the server still owes the
// abandoned utterance is dropped when it arrives (voice.h, voice_owed_t).
void mic_cancel(void);

// Called once per frame from the main loop: encodes newly captured PCM, feeds
// VOICE_CHUNK frames inside the 8 KB credit window, and sends VOICE_END when
// the user releases R / the MIC bar.
void mic_tick(unsigned long *tx);

// Handle MSG_VOICE_ACK credits.  Late acks (for a cancelled utterance, or
// after the upload finished) are dropped; none of them ever reconnects.
void mic_ack(uint32_t acked);

// Handle MSG_VOICE_TEXT.  Returns 1 only when `vt` is successful text for the
// utterance this card is WAITING on: the caller then puts vt->text in the
// composer, opening it the way its other openers do.  Anything else -- a
// failure status, or a late reply to a cancelled or timed-out utterance --
// returns 0 with at most a status line.
int  mic_text_arrived(const voice_text_t *vt);

// A VOICE_ACK / VOICE_TEXT arrived that would not decode.  It is still the
// reply the server owed (one per chunk / per VOICE_END, in order), so each
// pays the voice_owed_t ledger like a good one; ignoring it wedged the
// microphone on "still finishing the last recording" until a reconnect.  A
// live utterance it answered is given up with a status line, never the link.
void mic_ack_malformed(void);
void mic_text_malformed(void);

#endif  // DSTERM_MIC_H
