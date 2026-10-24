// Wire format decoder. Deliberately free of libnds so it can be compiled and
// tested on a host machine -- the DS has no MMU, so a length field taken on
// trust is a memory corruption bug with no backstop.
//
//   +--------+---------+------+--------+---------+
//   | magic  | version | type | length | payload |
//   | 2 (DS) |    1    |  1   |   2    |  0..N   |
//   +--------+---------+------+--------+---------+
//
// Big-endian. Must stay byte-identical to dsterm/protocol.py.

#ifndef DSTERM_PROTO_H
#define DSTERM_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "camera.h"     // the preview's size is the camera's, not the wire's
#include "clip.h"       // the clipboard's caps and its one allowed control byte
#include "sessions.h"   // bounded, formatting-safe MSG_SESSIONS state
#include "remote.h"     // the same, for MSG_REMOTE
#include "snippets.h"   // the pushed-snippet limits are snippets.c's rules
#include "voice.h"      // the microphone's formats, chunk size and limits

// The grid geometry is a build-time decision that must match the server's
// DSTERM_COLS/DSTERM_ROWS EXACTLY: a mismatch is an out-of-bounds write on a
// device with no MMU.  The Makefile overrides these (-DPROTO_COLS=64
// -DPROTO_ROWS=32, or 51x27) and the font/renderer follow from them.
#ifndef PROTO_COLS
#define PROTO_COLS        42
#endif
#ifndef PROTO_ROWS
#define PROTO_ROWS        24
#endif
#define PROTO_CELLS       (PROTO_COLS * PROTO_ROWS)

#define PROTO_MAGIC0      'D'
#define PROTO_MAGIC1      'S'
#define PROTO_VERSION     1
#define PROTO_HEADER_SIZE 6
#define PROTO_MAX_PAYLOAD 8192
#define PROTO_CELL_SIZE   3
#define PROTO_DIFF_ENTRY  5
#define PROTO_NONCE_SIZE  32
#define PROTO_MAC_SIZE    32
#define PROTO_MAX_KEY_PAYLOAD 64
// The HELLO capability list lives here, not in main.c, because handshake()
// encodes it into a stack buffer and the host suite cannot see main.c: when
// a fourteen-byte token grew this payload from 23 to 38 bytes, the MAC-sized
// handshake buffer overflowed by six bytes and the token's tail was restored
// into a callee-saved register and dereferenced: a hardware data abort at
// 0x726F7463 that no per-module test could reproduce.
//
// These are the terminal's own tokens.  An extension may append its own
// (ext_hello_tokens(), each with a leading space) up to PROTO_HELLO_EXT_MAX
// bytes; the buffer is sized for that bound here, where the suite can see
// it, rather than for whatever a particular extension happens to send.
// `grid=COLSxROWS` is this build's geometry, so a server set up for the other
// size can refuse the connection by name instead of streaming cells into a
// grid of the wrong shape.
#define PROTO_STR_(x) #x
#define PROTO_STR(x) PROTO_STR_(x)
//
// `project64` says this card takes up to PROTO_PROJECT_ROWS_MAX (64) PROJECT
// rows and draws the 0x80..0xBF glyphs (blocks, sparklines) in them.  A
// server sends more than 48 rows, or any of those bytes, only to a card that
// said it; an older ROM gets the old page.  The glyph bytes are display only
// on this card: project_action_keyable() keeps them off the wire.
#define PROTO_HELLO_CAPS "ds-term/1 mode snippets project clip remote state seal seal3 commands mirror ask mic project64 " \
                         "grid=" PROTO_STR(PROTO_COLS) "x" PROTO_STR(PROTO_ROWS)
#define PROTO_HELLO_CAPS_LEN (sizeof(PROTO_HELLO_CAPS) - 1)
// A DSi-mode card adds " camera" to the core tokens: it is the gate for every
// camera type, and a card without the hardware never announces it.
#define PROTO_HELLO_CAMERA_LEN (sizeof(" camera") - 1)
#define PROTO_HELLO_EXT_MAX 32
// A paired card adds " id=" and its eight-hex device id, which selects the
// key the server checks its AUTH against.  Twelve bytes, counted here.
#define PROTO_DEVICE_ID_BYTES 4
#define PROTO_DEVICE_ID_HEX (PROTO_DEVICE_ID_BYTES * 2)
#define PROTO_HELLO_ID_LEN (4 + PROTO_DEVICE_ID_HEX)
// Every connection adds " cn=" and the card's own fresh nonce in hex
// (docs/SEAL.md): it goes into the seal's keys, so a server replayed at the
// card can never make it reuse an old connection's keystream.
#define PROTO_CARD_NONCE_BYTES 16
#define PROTO_HELLO_CN_LEN (4 + 2 * PROTO_CARD_NONCE_BYTES)
#define PROTO_HELLO_MAX (PROTO_HELLO_CAPS_LEN + PROTO_HELLO_CAMERA_LEN + \
                         PROTO_HELLO_ID_LEN + PROTO_HELLO_CN_LEN +       \
                         PROTO_HELLO_EXT_MAX)

// One stack buffer serves both handshake sends; it must hold the larger of
// the HELLO frame and the AUTH frame, decided here where tests can see it.
#define PROTO_HANDSHAKE_BUF_SIZE                                          \
    (PROTO_HEADER_SIZE + (PROTO_HELLO_MAX > (size_t)PROTO_MAC_SIZE       \
                              ? PROTO_HELLO_MAX : (size_t)PROTO_MAC_SIZE))

// MSG_SESSIONS is deliberately one entry per frame. The fixed part is count,
// index, flags, big-endian idle seconds, and name length; the empty-list reply
// is the one-byte count alone. The name is clamped and sanitized by
// proto_apply_sessions() before anything reaches a display buffer.
#define PROTO_SESSIONS_EMPTY_SIZE 1
#define PROTO_SESSIONS_CHUNK_SIZE 6
#define PROTO_SESSION_PICK_SIZE   1

// MSG_PROJECT carries the PROJECT page's rows, one per frame, on exactly the
// same discipline MSG_SESSIONS uses and for the same reason: every server to
// client message has to fit inside the DS's IPv4 minimum MSS, so a whole page
// cannot travel at once.  The fixed part is count, index, tab, colour, and the
// two lengths; the empty-snapshot reply is the one-byte count alone, distinct
// from silence because "the server has nothing to report" and "the server has
// not told me" have to look different on the handheld.
//
// A card advertises support with the inert HELLO token "project" before a
// server may send this type, so an older ROM never sees a type it cannot
// frame.  Nothing travels the other way: an action on the page is typed into
// the PTY as ordinary MSG_KEY, because there is deliberately no message in
// this protocol that names a command.
#define PROTO_PROJECT_EMPTY_SIZE 1
#define PROTO_PROJECT_CHUNK_SIZE 6
#define PROTO_PROJECT_ROWS_MAX 64

// MSG_CLIP carries the desktop's clipboard down, chunked on the same
// discipline for the same MSS reason.  The fixed part is count, index, seq,
// flags, total (u32 big-endian) and one length byte; the empty frame is two
// bytes, count zero and a flag byte, because "the clipboard is empty" and "the
// copy was concealed and withheld" must not look alike -- a blank pane after
// copying a password invites copying it again.
//
// `total` is the length of the CONVERTED text, folded to printable ASCII and
// newline-normalised by the server, before truncation.  Measuring after the
// fold is what makes the bytes received exactly min(total, CLIP_TEXT_MAX), an
// invariant clip.c asserts on the completing frame; left ambiguous it is a
// classic shape of bug -- two encoders agreeing in testing and disagreeing on
// the wire.  u32 and not u16: a saturating total is a wrong number rather than
// a coarse one, and a 200 KB clipboard reported as 65.5K would be drawn as
// fact.
//
// Gated on the inert HELLO token "clip", and nothing travels the other way.
#define PROTO_CLIP_EMPTY_SIZE 2
#define PROTO_CLIP_CHUNK_SIZE 9

// Spelled in proto.h as well as sessions.h so wire-facing callers do not need
// to infer which flags are safe to render. Reserved bits are ignored.
#define PROTO_SESSION_RUNNING    SESSION_FLAG_RUNNING
#define PROTO_SESSION_ACTIVE     SESSION_FLAG_ACTIVE
#define PROTO_SESSION_NEW_OUTPUT SESSION_FLAG_NEW_OUTPUT

// Largest view delta the server will act on; it clamps anything beyond this,
// so a bigger request is not refused, only truncated. Mirrors VIEW_MAX_DELTA in
// dsterm/protocol.py and matches the session's history, so "return to live"
// reaches live from the deepest point the history can hold. Worth clamping on
// this side too: a value above 32767 would encode as a negative int16 and
// scroll further back rather than forward.
#define PROTO_VIEW_MAX_DELTA 5000

// MODE flags byte. Bit 0: the application on screen owns its scrollback, so
// scroll gestures must be PageUp/PageDown (ESC[5~ / ESC[6~) rather than VIEW
// requests. Mirrors MODE_APP_SCROLLS in dsterm/protocol.py.
#define PROTO_MODE_APP_SCROLLS 0x01

typedef enum {
    MSG_HELLO      = 0x01,
    MSG_AUTH       = 0x02,
    MSG_KEY        = 0x10,
    MSG_RESYNC     = 0x11,
    MSG_PING       = 0x12,
    // "Move my view N lines" -- signed, big-endian, negative is back. An
    // intent, not content: the server owns the history and decides what is
    // representable, so this cannot name a file, a path, or a command.
    MSG_VIEW       = 0x13,
    // Pick a server-advertised position in the most recently received session
    // list. It must not be sent before MSG_SESSIONS proves support.
    MSG_SESSION_PICK = 0x15,
    // Pairing, before the card holds a key: see pair.h.  Allowed before
    // authentication on the server, and unable to reach the PTY there.
    MSG_PAIR_HELLO   = 0x16,
    MSG_PAIR_CONFIRM = 0x17,    // the card's nonce
    MSG_PAIR_ACCEPT  = 0x1A,    // the codes matched here (pair.h)
    // REMOTE (docs/REMOTE.md): the picker opened (1) or closed (0), and a
    // pick by row index and action.  Like SESSION_PICK, never sent before a
    // MSG_REMOTE has arrived on this connection to prove the server speaks
    // them -- an unknown client type is a dropped connection on an old one.
    MSG_REMOTE_OPEN  = 0x18,
    MSG_REMOTE_PICK  = 0x19,
    // The DSi camera (camera.h): pixels and intents, all bounded, and none
    // names a file, a path or a command.  Sent only after MSG_CAMERA_READY
    // has arrived on this connection -- an old server raises on an unknown
    // client type and drops the link, and a server started with --no-camera
    // never sends one, so it is never sent these.
    MSG_CAMERA        = 0x1B,   // u8: 0 off, 1 inner, 2 outer
    MSG_CAMERA_FRAME  = 0x1C,   // format, flags, seq, 3072 pixels
    MSG_PHOTO_CHUNK   = 0x1D,   // u32be offset, 1..1024 bytes
    MSG_PHOTO_END     = 0x1E,   // format, camera, u32be total, u32be CRC-32
    MSG_PHOTO_DISCARD = 0x1F,   // u16be id the server issued
    // The bottom screen as text, for `dsterm view` (mirror.h).  Sent only
    // while the server's MSG_MIRROR says a view is watching -- which is also
    // the proof that it knows this type.
    MSG_SUBSCREEN     = 0x20,   // cols, rows, flags, then 6 bytes a cell
    // Enter, tied to the permission prompt it answers: u32be the ask id from
    // the last MSG_STATE, then the key bytes.  The server writes them only if
    // that prompt is still the one open, so a press meant for one prompt can
    // never answer the next.  Sent only after a MSG_STATE carrying an ask id
    // has arrived on this connection, which is the proof the server knows it.
    MSG_ANSWER        = 0x21,
    // Push-to-Talk microphone (voice.h): audio samples and a language hint.
    // None names a file, a path or a command; the server transcribes the
    // utterance and returns MSG_VOICE_TEXT, which lands only in the local
    // composer for a second deliberate press of A.  Sent only after
    // MSG_MIC_READY has arrived on this connection.
    MSG_VOICE_START   = 0x22,   // u8 format (1=IMA ADPCM 8kHz), u8 lang_len, lang
    MSG_VOICE_CHUNK   = 0x23,   // u32be offset, 1..480 ADPCM bytes
    MSG_VOICE_END     = 0x24,   // u32be total_bytes, u32be crc32

    MSG_NONCE      = 0x81,
    MSG_AUTH_OK    = 0x82,
    MSG_AUTH_FAIL  = 0x83,
    MSG_FRAME_FULL = 0x90,
    MSG_FRAME_DIFF = 0x91,
    MSG_SCROLL     = 0x92,
    MSG_PONG       = 0x93,
    MSG_CURSOR     = 0x94,
    // Where this client's view sits: uint16 lines back, uint16 lines available.
    // Display state, like MSG_CURSOR. Scrolling without it is scrolling blind --
    // no way to tell a press that did nothing from one that reached the top.
    MSG_VIEW_POS   = 0x95,
    // Which scrolling regime the child is in, one byte of flags. Bit 0 set
    // means the application owns its scrollback, so the handheld's scroll
    // gestures become PageUp/PageDown instead of VIEW. Display state, like
    // CURSOR -- it names nothing and grants nothing. Reserved bits are masked
    // off, never refused, so a future flag is not a flag day.
    MSG_MODE       = 0x96,
    // One entry of the server's current session list: u8 count, u8 index, u8
    // flags (running, active, new output), u16 idle seconds, u8 name length,
    // name. Chunks atomically replace the UI list in sessions.c.
    MSG_SESSIONS   = 0x97,
    // One chunk of a pushed snippet set: u8 index, u8 total, u8 label_len,
    // label, u16 body_len, body (docs/design/SNIPPETS_PUSH.md). Content state like
    // MODE -- it names nothing, and picked snippets still pass through the
    // composer before any byte reaches the PTY, which is the entire safety
    // case on a link with no per-message integrity. Gated by the HELLO
    // token so an old card never sees it.
    MSG_SNIPPETS   = 0x98,
    // Reserved for an extension: server-to-client only, and sent only to a
    // card whose HELLO carried the extension's own token.  The terminal
    // never handles it; ext_message() gets the first look, and a body it
    // does not want is dropped, never a connection-level error.
    MSG_EXTENSION  = 0x99,
    // The PROJECT page's rows. Capability-gated on the "project" HELLO token.
    MSG_PROJECT    = 0x9A,
    MSG_CLIP       = 0x9B,
    // Sent only in answer to this card's own PAIR_HELLO / PAIR_CONFIRM.
    MSG_PAIR_REPLY = 0x9C,
    MSG_PAIR_OK    = 0x9D,
    MSG_PAIR_FAIL  = 0x9E,
    MSG_PAIR_DONE  = 0xA2,      // the computer agreed too; the key is saved there
    // One row of the REMOTE list (Claude sessions on the server's machine),
    // and the one-line outcome of a pick.  Gated by the "remote" token.
    MSG_REMOTE     = 0x9F,
    MSG_REMOTE_NOTE = 0xA0,
    // What the Claude session on screen is doing: idle, working, waiting for
    // an approval, waiting for an answer.  Gated by "state".  Display state
    // like MODE: it names nothing and grants nothing, and the worst a forged
    // one can do is play the wrong chime.
    MSG_STATE      = 0xA1,
    // The computer's own slash commands, for QUICK (quick.h): names only,
    // newline-separated, into the composer.  Gated by "commands".
    MSG_COMMANDS   = 0xA3,
    // The camera's answers: the capability proof right after AUTH_OK, the
    // flow-control credits, and where a picture was saved.  Only ever sent
    // to a card that announced "camera".
    MSG_CAMERA_READY = 0xA4,    // u8 flags: bit0 pictures can be saved
    MSG_CAMERA_ACK   = 0xA5,    // u8 kind, u32be value
    MSG_PHOTO_SAVED  = 0xA6,    // status, u16be id, u8 len, path
    // `dsterm view`: whether one is watching the bottom screen (gated by the
    // "mirror" token), and a click on it, as a tap at a pixel -- which the
    // card acts on only with SETTINGS -> VIEW TAPS on.
    MSG_MIRROR       = 0xA7,    // u8 flags: bit0 a view is watching
    MSG_TOUCH        = 0xA8,    // u8 x (0..255), u8 y (0..191)
    // The microphone's answers, sent only to a card that announced "mic".
    MSG_MIC_READY    = 0xA9,    // u8 flags: bit0 speech-to-text is available
    MSG_VOICE_ACK    = 0xAA,    // u32be received ADPCM bytes (credit window)
    MSG_VOICE_TEXT   = 0xAB,    // u8 status, u16be len, printable ASCII + LF for composer
    MSG_ERROR      = 0xFF
} msg_type_t;

typedef enum {
    PROTO_OK = 0,
    PROTO_NEED_MORE,      // not an error: wait for more bytes
    PROTO_ERR_MAGIC,
    PROTO_ERR_VERSION,
    PROTO_ERR_TYPE,
    PROTO_ERR_LENGTH,
    PROTO_ERR_PAYLOAD,
    PROTO_ERR_BOUNDS,
    PROTO_ERR_OVERFLOW
} proto_result_t;

const char *proto_strerror(proto_result_t r);

// One screen cell. Matches pack_cell() in dsterm/protocol.py.
typedef struct {
    uint8_t glyph;   // ASCII 0x20..0x7E
    uint8_t attr;    // fg index in high nibble, bg index in low nibble
    uint8_t flags;   // bit0 bold, bit1 reverse
} cell_t;

#define CELL_FLAG_BOLD    0x01
#define CELL_FLAG_REVERSE 0x02

#define CELL_FG(c) ((uint8_t)((c).attr >> 4))
#define CELL_BG(c) ((uint8_t)((c).attr & 0x0F))

// Where the terminal caret is, from MSG_CURSOR (row, col, visible).
typedef struct {
    uint8_t row;
    uint8_t col;
    uint8_t visible;
} cursor_t;

// Scrollback position from MSG_VIEW_POS: both fields are unsigned, big-endian
// uint16 values on the wire.
typedef struct {
    uint16_t lines_back;
    uint16_t available;
} view_pos_t;

typedef struct {
    cell_t cells[PROTO_CELLS];
} grid_t;

void grid_clear(grid_t *g);

// Rows [top..PROTO_ROWS) move up by `lines`; vacated rows are blanked.
// `lines` must be 1..PROTO_ROWS-1.
proto_result_t grid_scroll(grid_t *g, int lines);

// Accumulating reader. One recv() is not one message: it may hold half a
// header, three messages, or a single byte.
// `pos` is a read cursor rather than compacting on every pop: the payload
// pointer returned by framereader_next() points into `buf`, so shifting the
// buffer inside that call would overwrite the payload before the caller ever
// read it. Compaction happens on feed, once the caller is done.
typedef struct {
    uint8_t buf[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD];
    size_t  len;
    size_t  pos;
} framereader_t;

void framereader_init(framereader_t *fr);

// Append raw bytes. Returns PROTO_ERR_OVERFLOW if they cannot fit, which can
// only happen if the caller ignores framereader_space().
proto_result_t framereader_feed(framereader_t *fr, const uint8_t *data, size_t len);

size_t framereader_space(const framereader_t *fr);

// Pop one complete message. PROTO_NEED_MORE means the buffer holds only a
// partial message; any other non-OK result is fatal for the connection, because
// a desynchronised length prefix makes every later offset meaningless.
proto_result_t framereader_next(framereader_t *fr, msg_type_t *type,
                                const uint8_t **payload, uint16_t *payload_len);

// Payload decoders. Each validates length and bounds before touching `g`.
proto_result_t proto_apply_full(grid_t *g, const uint8_t *payload, uint16_t len);
proto_result_t proto_apply_diff(grid_t *g, const uint8_t *payload, uint16_t len);
proto_result_t proto_apply_scroll(grid_t *g, const uint8_t *payload, uint16_t len,
                                  int *lines_out);
proto_result_t proto_apply_cursor(cursor_t *cur, const uint8_t *payload, uint16_t len);
proto_result_t proto_apply_view_pos(view_pos_t *pos, const uint8_t *payload,
                                    uint16_t len);
proto_result_t proto_apply_mode(uint8_t *flags_out, const uint8_t *payload,
                                uint16_t len);

// Decode one MSG_SESSIONS chunk into a safe, bounded record for sessions.c.
// Layout for a non-empty list is: count, index, flags, idle_s (big-endian),
// name_len, name. Count=0 is valid only as the one-byte empty-list payload.
// The wire name may be longer than SESSION_NAME_MAX or contain unsafe bytes;
// it is clamped and printable-ASCII-sanitized rather than copied unchecked.
// Decode one MSG_REMOTE frame (docs/REMOTE.md) into a bounded, printable
// record for remote.c.  Structure is checked strictly; text is clamped and
// unprintable bytes become '?', as for session names.
proto_result_t proto_apply_remote(remote_chunk_t *chunk,
                                  const uint8_t *payload, uint16_t len);
// MSG_REMOTE_NOTE: one printable line, at most REMOTE_NOTE_MAX bytes.
proto_result_t proto_apply_remote_note(char *out, size_t cap,
                                       const uint8_t *payload, uint16_t len);
// MSG_STATE: the state byte and its detail (waitingFor), clamped.
#define PROTO_STATE_UNKNOWN 0
#define PROTO_STATE_IDLE 1
#define PROTO_STATE_WORKING 2
#define PROTO_STATE_APPROVAL 3
#define PROTO_STATE_INPUT 4
#define PROTO_STATE_DETAIL_MAX 24
proto_result_t proto_apply_state(uint8_t *state, char *detail, size_t cap,
                                 const uint8_t *payload, uint16_t len);
// The same, and STATE's tail for a card that announced "ask": the open
// question's id and a flags byte.  *has_ask is 0 when the tail is absent.
#define PROTO_ASK_TAIL 5
#define PROTO_ASK_FLAG_DROPPED 0x01   // the last Enter was not delivered
proto_result_t proto_apply_state_ask(uint8_t *state, char *detail, size_t cap,
                                     int *has_ask, uint32_t *ask_id,
                                     uint8_t *flags,
                                     const uint8_t *payload, uint16_t len);
// MSG_ANSWER: the ask id, then 1..PROTO_MAX_KEY_PAYLOAD key bytes.  `out`
// holds PROTO_HEADER_SIZE + 4 + PROTO_MAX_KEY_PAYLOAD.  0 if it cannot.
#define PROTO_ANSWER_MAX (4 + PROTO_MAX_KEY_PAYLOAD)
size_t proto_encode_answer(uint8_t *out, uint32_t ask_id,
                           const uint8_t *keys, size_t len);
// The client's two REMOTE messages.  Return the framed length.
size_t proto_encode_remote_open(uint8_t *out, int open);
size_t proto_encode_remote_pick(uint8_t *out, uint8_t index, uint8_t action,
                                uint16_t check);

// --------------------------------------------------------------- camera ---
//
// Mirrors the camera section of dsterm/protocol.py byte for byte.  The card
// sends pixels and intents; the server alone chooses where a picture goes,
// and PHOTO_SAVED's path is COMMANDS' argument -- server text that lands
// only in the composer, checked printable again on the way in.

#define PROTO_CAMERA_OFF   0
#define PROTO_CAMERA_INNER 1
#define PROTO_CAMERA_OUTER 2

// The preview's sizes are the camera's (camera.h): the frame carries a
// format byte, a flags byte and a seq byte, then the view in that format --
// 2, 128x96 greys two pixels a byte (6,144 bytes), or 3, 96x72 RGB332 (6,912).
// Format 1 was the first version's 64x48 dithered RGB332; it is not sent.
#define PROTO_PREVIEW_W     CAMERA_PREVIEW_W
#define PROTO_PREVIEW_H     CAMERA_PREVIEW_H
#define PROTO_PREVIEW_PIXELS CAMERA_PREVIEW_PIXELS                  // 12288
#define PROTO_PREVIEW_BYTES  CAMERA_PREVIEW_BYTES                   // 6144
#define PROTO_PREVIEW_FORMAT_GRAY4  CAMERA_VIEW_GREY                // 2
#define PROTO_PREVIEW_FORMAT_RGB332 CAMERA_VIEW_COLOUR              // 3
#define PROTO_PREVIEW_FLAG_STILL    0x01
#define PROTO_PREVIEW_FLAGS         PROTO_PREVIEW_FLAG_STILL
#define PROTO_CAMERA_FRAME_SIZE (3 + CAMERA_VIEW_BYTES_MAX)        // 6915, the larger

// The largest payload this card ever emits: one CAMERA_FRAME.  send_all()
// sizes its static seal buffer from it, and a frame that does not fit is
// refused rather than sent unsealed.
#define PROTO_MAX_CARD_PAYLOAD PROTO_CAMERA_FRAME_SIZE

// Header (6) and slack (2) around the payload: a preview that does not fit
// a frame is a build error, not a runtime surprise.
_Static_assert(PROTO_CAMERA_FRAME_SIZE + 8 <= PROTO_MAX_PAYLOAD,
               "a camera preview frame must fit inside a payload");

#define PROTO_PHOTO_W      640
#define PROTO_PHOTO_H      480
// The picture crosses as 4:2:0 planar (camera.h packs it): format 1 is the
// full sensor, format 2 a quarter of it.  Chunk offsets are bounded by the
// larger.
#define PROTO_PHOTO_FORMAT_FULL  CAMERA_PHOTO_FORMAT_FULL
#define PROTO_PHOTO_FORMAT_SMALL CAMERA_PHOTO_FORMAT_SMALL
#define PROTO_PHOTO_BYTES_MAX    CAMERA_PHOTO_BYTES_MAX            // 460800
#define PROTO_PHOTO_CHUNK_MAX 1024
#define PROTO_PHOTO_PATH_MAX  200

#define PROTO_ACK_PREVIEW 0
#define PROTO_ACK_PHOTO   1

#define PROTO_READY_CAN_SAVE 0x01

#define PROTO_SAVED_OK        0
#define PROTO_SAVED_BUSY      1
#define PROTO_SAVED_INTEGRITY 2
#define PROTO_SAVED_DISK      3
#define PROTO_SAVED_TOO_SOON  4
#define PROTO_SAVED_BAD_NAME  5
#define PROTO_SAVED_MAX       PROTO_SAVED_BAD_NAME

// The camera's encoders: one fixed buffer each, the framed length returned,
// or 0 for a parameter no honest caller produces.
size_t proto_encode_camera(uint8_t *out, int state);
// `px` holds camera_view_bytes(format) bytes.
size_t proto_encode_camera_frame(uint8_t *out, int format, uint8_t flags,
                                 uint8_t seq, const uint8_t *px);
size_t proto_encode_photo_chunk(uint8_t *out, uint32_t offset,
                                const uint8_t *data, uint16_t len);
size_t proto_encode_photo_end(uint8_t *out, uint8_t format, uint8_t camera,
                              uint32_t crc);
size_t proto_encode_photo_discard(uint8_t *out, uint16_t id);

// MSG_MIRROR: bit0, a view is watching.  Reserved bits are refused.
proto_result_t proto_decode_mirror(int *watching_out, const uint8_t *payload,
                                   uint16_t len);
// MSG_TOUCH: a tap at (x, y) on the bottom screen.
proto_result_t proto_decode_touch(uint8_t *x_out, uint8_t *y_out,
                                  const uint8_t *payload, uint16_t len);

// MSG_CAMERA_READY: bit0, pictures can be saved.  Reserved bits are refused.
proto_result_t proto_decode_camera_ready(int *can_save_out,
                                         const uint8_t *payload, uint16_t len);

// MSG_CAMERA_ACK: kind (0 preview, 1 photo) and its u32be value.  A preview
// ack's value is the seq byte it answers, so it must fit one.
proto_result_t proto_decode_camera_ack(uint8_t *kind_out, uint32_t *value_out,
                                       const uint8_t *payload, uint16_t len);

// MSG_PHOTO_SAVED, decoded.  Status 0 carries the id and the path (printable
// ASCII, at most 200 bytes, copied and re-checked); any other carries
// neither.
typedef struct {
    uint8_t status;
    uint16_t id;
    uint8_t path_len;
    char path[PROTO_PHOTO_PATH_MAX + 1];
} photo_saved_t;

proto_result_t proto_decode_photo_saved(photo_saved_t *out,
                                        const uint8_t *payload, uint16_t len);

proto_result_t proto_apply_sessions(session_chunk_t *chunk,
                                    const uint8_t *payload, uint16_t len);

// Decode one MSG_PROJECT chunk. Layout for a non-empty snapshot is: count,
// index, tab, fg, label_len, value_len, label, value. Count=0 is valid only as
// the one-byte empty-snapshot payload. Both strings are attacker-influenced,
// so both are length-checked against the caller's capacity and sanitized to
// printable ASCII rather than copied unchecked -- the same rule the session
// name gets, because both end up in a display buffer.
proto_result_t proto_apply_project(unsigned *count_out, unsigned *index_out,
                                   unsigned *tab_out, unsigned *fg_out,
                                   char *label_out, size_t label_cap,
                                   char *value_out, size_t value_cap,
                                   const uint8_t *payload, uint16_t len);

// Decode one MSG_CLIP frame. Layout for a non-empty one is: count, index, seq,
// flags, total:u32be, len, bytes. Count=0 is valid only as the two-byte empty
// frame, whose second byte is the flags -- withheld has to be distinguishable
// from empty. The body is attacker-influenced, so it is length-checked against
// the caller's capacity and refused outright if it holds anything but
// printable ASCII and newline; unlike the session name it is NOT sanitized in
// place, because a clipboard silently altered on the way in is a paste that
// does something other than what was copied.
proto_result_t proto_apply_clip(unsigned *count_out, unsigned *index_out,
                                unsigned *seq_out, unsigned *flags_out,
                                uint32_t *total_out,
                                char *body_out, size_t body_cap,
                                unsigned *body_len_out,
                                const uint8_t *payload, uint16_t len);

// Decode one pushed-snippet chunk. Every field is attacker-influenced off
// the wire, so every limit the file parser enforces is enforced here before
// anything is copied: total 1..SNIPPETS_MAX, index < total, label_len
// 1..SNIPPET_LABEL_MAX, body_len <= SNIPPET_BODY_MAX, and the declared lengths
// must equal the payload length exactly. The caller's buffers are respected
// as an additional cap. Body *content* is validated by snippets_apply_pushed,
// which mirrors the parser's character rule.
proto_result_t proto_apply_snippet(unsigned *index_out, unsigned *total_out,
                                   char *label_out, size_t label_cap,
                                   size_t *label_len_out,
                                   char *body_out, size_t body_cap,
                                   size_t *body_len_out,
                                   const uint8_t *payload, uint16_t len);

// Encoders. `out` must have room for PROTO_HEADER_SIZE + payload.
size_t proto_encode(uint8_t *out, msg_type_t type, const uint8_t *payload, uint16_t len);

// The HELLO payload: the terminal's tokens, " camera" when the card has one
// (a DSi does, and isDSiMode() is what main.c passes), then " id=<hex>" when
// the card has a device id (NULL or "" for none), then " cn=<hex>", this
// connection's card nonce (PROTO_CARD_NONCE_BYTES; NULL leaves it out, which
// only a test does -- a server refuses a HELLO without one), then the
// extension's tokens.  Writes at most `cap` bytes including the NUL and returns the
// payload length.  An extension string that would not fit is left out
// entirely -- the core tokens and the id are what the link needs, and a
// truncated token is a lie -- and an id that is not exactly eight lowercase
// hex digits is never sent.  Returns -1 only if `cap` cannot hold the core
// tokens.
int proto_hello_payload(char *out, size_t cap, const char *device_id_hex,
                        const uint8_t *card_nonce,
                        const char *ext_tokens, int camera);
size_t proto_encode_empty(uint8_t *out, msg_type_t type);

// The camera's own message types: CAMERA, CAMERA_FRAME, PHOTO_CHUNK,
// PHOTO_END and PHOTO_DISCARD.  The PHOTO screen sends these and nothing
// else (host_send_camera in main.c refuses the rest), so it cannot type.
int proto_camera_type(uint8_t type);

// ----------------------------------------------------------------- voice ---
//
// Mirrors the voice section of dsterm/protocol.py byte for byte.  Gated by
// the "mic" HELLO token and MSG_MIC_READY.

#define PROTO_MIC_CAN_TRANSCRIBE 0x01

typedef struct {
    uint8_t status;
    uint16_t text_len;
    char text[VOICE_TEXT_MAX + 1];
} voice_text_t;

size_t proto_encode_voice_start(uint8_t *out, uint8_t format, const char *lang);
size_t proto_encode_voice_chunk(uint8_t *out, uint32_t offset,
                                const uint8_t *data, uint16_t len);
size_t proto_encode_voice_end(uint8_t *out, uint32_t total, uint32_t crc);

proto_result_t proto_decode_mic_ready(int *can_transcribe_out,
                                      const uint8_t *payload, uint16_t len);
proto_result_t proto_decode_voice_ack(uint32_t *acked_out,
                                      const uint8_t *payload, uint16_t len);
proto_result_t proto_decode_voice_text(voice_text_t *out,
                                       const uint8_t *payload, uint16_t len);

int proto_voice_type(uint8_t type);

#endif  // DSTERM_PROTO_H
