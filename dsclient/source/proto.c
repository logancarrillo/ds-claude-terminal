#include "proto.h"

#include <stdio.h>
#include <string.h>

const char *proto_strerror(proto_result_t r)
{
    switch (r) {
    case PROTO_OK:          return "ok";
    case PROTO_NEED_MORE:   return "need more data";
    case PROTO_ERR_MAGIC:   return "bad magic";
    case PROTO_ERR_VERSION: return "unsupported version";
    case PROTO_ERR_TYPE:    return "unknown message type";
    case PROTO_ERR_LENGTH:  return "declared length too large";
    case PROTO_ERR_PAYLOAD: return "malformed payload";
    case PROTO_ERR_BOUNDS:  return "cell position outside grid";
    case PROTO_ERR_OVERFLOW:return "buffer overflow";
    }
    return "unknown";
}

static const cell_t BLANK_CELL = { .glyph = ' ', .attr = 0x00, .flags = 0 };

void grid_clear(grid_t *g)
{
    for (int i = 0; i < PROTO_CELLS; i++) g->cells[i] = BLANK_CELL;
}

proto_result_t grid_scroll(grid_t *g, int lines)
{
    if (lines < 1 || lines >= PROTO_ROWS) return PROTO_ERR_BOUNDS;

    const size_t moved = (size_t)(PROTO_ROWS - lines) * PROTO_COLS;
    memmove(g->cells, g->cells + (size_t)lines * PROTO_COLS, moved * sizeof(cell_t));
    for (size_t i = moved; i < PROTO_CELLS; i++) g->cells[i] = BLANK_CELL;
    return PROTO_OK;
}

static int type_is_known(uint8_t t)
{
    switch (t) {
    case MSG_HELLO: case MSG_AUTH: case MSG_KEY: case MSG_RESYNC: case MSG_PING:
    case MSG_VIEW: case MSG_SESSION_PICK:
    case MSG_NONCE: case MSG_AUTH_OK: case MSG_AUTH_FAIL:
    case MSG_FRAME_FULL: case MSG_FRAME_DIFF: case MSG_SCROLL:
    case MSG_PONG: case MSG_CURSOR: case MSG_VIEW_POS: case MSG_MODE:
    case MSG_SESSIONS: case MSG_SNIPPETS: case MSG_EXTENSION: case MSG_PROJECT:
    case MSG_CLIP: case MSG_ERROR:
    case MSG_PAIR_HELLO: case MSG_PAIR_CONFIRM: case MSG_PAIR_ACCEPT:
    case MSG_PAIR_REPLY: case MSG_PAIR_OK: case MSG_PAIR_FAIL: case MSG_PAIR_DONE:
    case MSG_REMOTE_OPEN: case MSG_REMOTE_PICK:
    case MSG_REMOTE: case MSG_REMOTE_NOTE: case MSG_STATE: case MSG_COMMANDS:
    case MSG_CAMERA: case MSG_CAMERA_FRAME: case MSG_PHOTO_CHUNK:
    case MSG_PHOTO_END: case MSG_PHOTO_DISCARD:
    case MSG_CAMERA_READY: case MSG_CAMERA_ACK: case MSG_PHOTO_SAVED:
    case MSG_SUBSCREEN: case MSG_MIRROR: case MSG_TOUCH: case MSG_ANSWER:
    case MSG_VOICE_START: case MSG_VOICE_CHUNK: case MSG_VOICE_END:
    case MSG_MIC_READY: case MSG_VOICE_ACK: case MSG_VOICE_TEXT:
        return 1;
    default:
        return 0;
    }
}

void framereader_init(framereader_t *fr)
{
    fr->len = 0;
    fr->pos = 0;
}

// Drop already-consumed bytes. Called on feed rather than on pop, so a payload
// pointer stays valid until the caller asks for more data.
static void framereader_compact(framereader_t *fr)
{
    if (fr->pos == 0) return;
    const size_t remaining = fr->len - fr->pos;
    if (remaining) memmove(fr->buf, fr->buf + fr->pos, remaining);
    fr->len = remaining;
    fr->pos = 0;
}

size_t framereader_space(const framereader_t *fr)
{
    return sizeof(fr->buf) - (fr->len - fr->pos);
}

proto_result_t framereader_feed(framereader_t *fr, const uint8_t *data, size_t len)
{
    if (len > framereader_space(fr)) return PROTO_ERR_OVERFLOW;
    framereader_compact(fr);
    memcpy(fr->buf + fr->len, data, len);
    fr->len += len;
    return PROTO_OK;
}

proto_result_t framereader_next(framereader_t *fr, msg_type_t *type,
                                const uint8_t **payload, uint16_t *payload_len)
{
    const uint8_t *p = fr->buf + fr->pos;
    const size_t avail = fr->len - fr->pos;

    if (avail < PROTO_HEADER_SIZE) return PROTO_NEED_MORE;

    if (p[0] != PROTO_MAGIC0 || p[1] != PROTO_MAGIC1) return PROTO_ERR_MAGIC;
    if (p[2] != PROTO_VERSION) return PROTO_ERR_VERSION;
    if (!type_is_known(p[3])) return PROTO_ERR_TYPE;

    const uint16_t len = (uint16_t)((p[4] << 8) | p[5]);
    // Rejected from the header alone: waiting for the body of a hostile length
    // would let a peer pin the whole receive buffer with six bytes.
    if (len > PROTO_MAX_PAYLOAD) return PROTO_ERR_LENGTH;

    const size_t total = (size_t)PROTO_HEADER_SIZE + len;
    if (avail < total) return PROTO_NEED_MORE;

    *type = (msg_type_t)p[3];
    *payload = p + PROTO_HEADER_SIZE;
    *payload_len = len;

    // Advance the cursor only. The returned payload still points into buf and
    // stays valid until the next feed().
    fr->pos += total;
    return PROTO_OK;
}

proto_result_t proto_apply_full(grid_t *g, const uint8_t *payload, uint16_t len)
{
    if (len != PROTO_CELLS * PROTO_CELL_SIZE) return PROTO_ERR_PAYLOAD;
    for (int i = 0; i < PROTO_CELLS; i++) {
        const uint8_t *p = payload + (size_t)i * PROTO_CELL_SIZE;
        g->cells[i].glyph = p[0];
        g->cells[i].attr  = p[1];
        g->cells[i].flags = p[2];
    }
    return PROTO_OK;
}

proto_result_t proto_apply_diff(grid_t *g, const uint8_t *payload, uint16_t len)
{
    if (len % PROTO_DIFF_ENTRY != 0) return PROTO_ERR_PAYLOAD;

    const uint16_t count = (uint16_t)(len / PROTO_DIFF_ENTRY);
    // Validate every position BEFORE writing any of them, so a diff that is
    // malformed halfway through cannot leave the screen half-updated.
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = payload + (size_t)i * PROTO_DIFF_ENTRY;
        if (p[0] >= PROTO_ROWS || p[1] >= PROTO_COLS) return PROTO_ERR_BOUNDS;
    }
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = payload + (size_t)i * PROTO_DIFF_ENTRY;
        cell_t *c = &g->cells[(size_t)p[0] * PROTO_COLS + p[1]];
        c->glyph = p[2];
        c->attr  = p[3];
        c->flags = p[4];
    }
    return PROTO_OK;
}

proto_result_t proto_apply_scroll(grid_t *g, const uint8_t *payload, uint16_t len,
                                  int *lines_out)
{
    if (len != 3) return PROTO_ERR_PAYLOAD;

    const uint8_t top = payload[0];
    const uint8_t bottom = payload[1];
    const int8_t lines = (int8_t)payload[2];

    if (top >= PROTO_ROWS || bottom >= PROTO_ROWS || top > bottom) return PROTO_ERR_BOUNDS;
    if (lines == 0) return PROTO_ERR_PAYLOAD;
    // Only whole-screen upward scrolls are produced by the server today.
    // Anything else is rejected rather than half-implemented, so a future
    // server change fails loudly instead of rendering wrong.
    if (top != 0 || bottom != PROTO_ROWS - 1 || lines < 0) return PROTO_ERR_BOUNDS;

    if (lines_out) *lines_out = lines;
    return grid_scroll(g, lines);
}

proto_result_t proto_apply_cursor(cursor_t *cur, const uint8_t *payload, uint16_t len)
{
    if (len != 3) return PROTO_ERR_PAYLOAD;
    // A position past the grid would be rendered outside the framebuffer; the
    // DS has no MMU to stop that, so it is refused on the decoder's side.
    if (payload[0] >= PROTO_ROWS || payload[1] >= PROTO_COLS) return PROTO_ERR_BOUNDS;

    cur->row     = payload[0];
    cur->col     = payload[1];
    cur->visible = payload[2] ? 1 : 0;
    return PROTO_OK;
}

proto_result_t proto_apply_view_pos(view_pos_t *pos, const uint8_t *payload,
                                    uint16_t len)
{
    if (len != 4) return PROTO_ERR_PAYLOAD;

    pos->lines_back = (uint16_t)((payload[0] << 8) | payload[1]);
    pos->available  = (uint16_t)((payload[2] << 8) | payload[3]);
    return PROTO_OK;
}

proto_result_t proto_apply_mode(uint8_t *flags_out, const uint8_t *payload,
                                uint16_t len)
{
    if (len != 1) return PROTO_ERR_PAYLOAD;
    // Mask, not validate: reserved bits mean a future flag, and a future flag
    // on the wire must not halt today's card any more than an unknown HELLO
    // token must. Only the bits this build knows are ever acted on.
    *flags_out = (uint8_t)(payload[0] & PROTO_MODE_APP_SCROLLS);
    return PROTO_OK;
}

proto_result_t proto_apply_sessions(session_chunk_t *chunk,
                                    const uint8_t *payload, uint16_t len)
{
    // Layout: count, index, flags, idle_s_hi, idle_s_lo, name_len, name.
    // Unlike snippets, entries are emitted one-per-frame so the whole list
    // fits safely below the small-link ceiling even at eight 24-byte names.
    if (!chunk || len < PROTO_SESSIONS_EMPTY_SIZE) return PROTO_ERR_PAYLOAD;

    const unsigned count = payload[0];
    if (count > SESSIONS_MAX) return PROTO_ERR_PAYLOAD;

    // The empty list is a complete answer, not a zero-length malformed chunk.
    // Insisting on exactly one byte prevents trailing junk from silently being
    // interpreted as a subsequent format revision.
    if (count == 0) {
        if (len != PROTO_SESSIONS_EMPTY_SIZE) return PROTO_ERR_PAYLOAD;
        memset(chunk, 0, sizeof(*chunk));
        return PROTO_OK;
    }

    if (len < PROTO_SESSIONS_CHUNK_SIZE) return PROTO_ERR_PAYLOAD;
    const unsigned index = payload[1];
    const size_t raw_name_len = payload[5];
    if (index >= count) return PROTO_ERR_PAYLOAD;
    if ((size_t)PROTO_SESSIONS_CHUNK_SIZE + raw_name_len != (size_t)len)
        return PROTO_ERR_PAYLOAD;

    // Every structural check happened before this copy.  Long and non-ASCII
    // names are presentation problems rather than reasons to tear down a live
    // terminal: they become a bounded printable name on the card.
    memset(chunk, 0, sizeof(*chunk));
    chunk->count = (uint8_t)count;
    chunk->entry.index = (uint8_t)index;
    chunk->entry.flags = (uint8_t)(payload[2] & SESSION_FLAG_KNOWN);
    chunk->entry.idle_s = (uint16_t)((payload[3] << 8) | payload[4]);
    chunk->entry.name_len = (uint8_t)session_name_sanitize(
        chunk->entry.name, sizeof(chunk->entry.name),
        payload + PROTO_SESSIONS_CHUNK_SIZE, raw_name_len);
    return PROTO_OK;
}

// One displayable field, clamped to the destination.  Both of the page's
// strings come off the wire and both end up on a screen, so both get the same
// treatment the session name gets: a byte that cannot be drawn becomes '?'
// rather than a reason to tear down a live terminal, and a field longer than
// the destination is truncated rather than trusted.  Printable ASCII and the
// 0x80..0xBF glyph block (font6x8_ext: blocks, sparklines) pass, because the
// card announced `project64`.  The glyph bytes are for the screen only -- on
// a PTY they are C1 controls -- so a row is display text, and anything that
// would type one checks project_action_keyable() first.
static void project_text_copy(char *out, size_t cap, const uint8_t *src,
                              size_t len)
{
    if (!out || cap == 0) return;
    size_t n = len;
    if (n > cap - 1) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = src[i];
        out[i] = ((b >= 0x20 && b <= 0x7E) || (b >= 0x80 && b < 0xC0)) ? (char)b : '?';
    }
    out[n] = '\0';
}

proto_result_t proto_apply_project(unsigned *count_out, unsigned *index_out,
                                   unsigned *tab_out, unsigned *fg_out,
                                   char *label_out, size_t label_cap,
                                   char *value_out, size_t value_cap,
                                   const uint8_t *payload, uint16_t len)
{
    // Layout: count, index, tab, fg, label_len, value_len, label, value.
    if (!count_out || !index_out || !tab_out || !fg_out ||
        !label_out || !value_out || label_cap == 0 || value_cap == 0 ||
        len < PROTO_PROJECT_EMPTY_SIZE) {
        return PROTO_ERR_PAYLOAD;
    }

    const unsigned count = payload[0];
    if (count > PROTO_PROJECT_ROWS_MAX) return PROTO_ERR_PAYLOAD;

    // The empty snapshot is a complete answer, not a malformed chunk.  Exactly
    // one byte, so trailing junk cannot be read as a later format revision.
    if (count == 0) {
        if (len != PROTO_PROJECT_EMPTY_SIZE) return PROTO_ERR_PAYLOAD;
        *count_out = 0;
        *index_out = 0;
        *tab_out = 0;
        *fg_out = 0;
        label_out[0] = '\0';
        value_out[0] = '\0';
        return PROTO_OK;
    }

    if (len < PROTO_PROJECT_CHUNK_SIZE) return PROTO_ERR_PAYLOAD;
    const unsigned index = payload[1];
    const unsigned tab = payload[2];
    const unsigned fg = payload[3];
    const size_t label_len = payload[4];
    const size_t value_len = payload[5];
    if (index >= count) return PROTO_ERR_PAYLOAD;
    // The colour is an index into a sixteen-entry palette.  Bounding it here
    // rather than at the drawing end means a hostile value cannot reach a
    // lookup at all.
    if (fg > 15u) return PROTO_ERR_PAYLOAD;
    if ((size_t)PROTO_PROJECT_CHUNK_SIZE + label_len + value_len != (size_t)len)
        return PROTO_ERR_PAYLOAD;

    // Every structural check has happened; only presentation is left.
    *count_out = count;
    *index_out = index;
    *tab_out = tab;
    *fg_out = fg;
    project_text_copy(label_out, label_cap,
                      payload + PROTO_PROJECT_CHUNK_SIZE, label_len);
    project_text_copy(value_out, value_cap,
                      payload + PROTO_PROJECT_CHUNK_SIZE + label_len,
                      value_len);
    return PROTO_OK;
}

proto_result_t proto_apply_clip(unsigned *count_out, unsigned *index_out,
                                unsigned *seq_out, unsigned *flags_out,
                                uint32_t *total_out,
                                char *body_out, size_t body_cap,
                                unsigned *body_len_out,
                                const uint8_t *payload, uint16_t len)
{
    // Layout: count, index, seq, flags, total:u32be, body_len, body.
    if (!count_out || !index_out || !seq_out || !flags_out || !total_out ||
        !body_out || !body_len_out || body_cap == 0 ||
        len < PROTO_CLIP_EMPTY_SIZE) {
        return PROTO_ERR_PAYLOAD;
    }

    const unsigned count = payload[0];
    if (count > CLIP_CHUNKS_MAX) return PROTO_ERR_PAYLOAD;

    // The empty frame is a complete answer, not a malformed chunk. Exactly two
    // bytes so trailing junk cannot be read as a later format revision -- and
    // the second byte is load-bearing: WITHHELD must not decode as EMPTY.
    if (count == 0) {
        if (len != PROTO_CLIP_EMPTY_SIZE) return PROTO_ERR_PAYLOAD;
        *count_out = 0;
        *index_out = 0;
        *seq_out = 0;
        *flags_out = payload[1];
        *total_out = 0;
        *body_len_out = 0;
        body_out[0] = '\0';
        return PROTO_OK;
    }

    if (len < PROTO_CLIP_CHUNK_SIZE) return PROTO_ERR_PAYLOAD;
    const unsigned index = payload[1];
    const unsigned seq = payload[2];
    const unsigned flags = payload[3];
    const uint32_t total = ((uint32_t)payload[4] << 24) |
                           ((uint32_t)payload[5] << 16) |
                           ((uint32_t)payload[6] << 8) |
                           (uint32_t)payload[7];
    const size_t body_len = payload[8];

    if (index >= count) return PROTO_ERR_PAYLOAD;
    if (body_len < 1 || body_len > CLIP_CHUNK_MAX) return PROTO_ERR_PAYLOAD;
    if (body_len > body_cap) return PROTO_ERR_PAYLOAD;
    if ((size_t)PROTO_CLIP_CHUNK_SIZE + body_len != (size_t)len) {
        return PROTO_ERR_PAYLOAD;
    }

    // Refused, not sanitized. Everywhere else in this file a hostile byte is
    // replaced so a display buffer stays printable; here the bytes are what a
    // person is about to paste into a prompt, and quietly swapping one for
    // another produces text that does something other than what was copied.
    // The server will not encode anything but printable ASCII and newline, so
    // a frame that holds more is a frame from something that is not the server
    // this card thinks it is talking to.
    for (size_t i = 0; i < body_len; i++) {
        const uint8_t byte = payload[PROTO_CLIP_CHUNK_SIZE + i];
        if (byte != CLIP_LF && (byte < 0x20 || byte > 0x7E)) {
            return PROTO_ERR_PAYLOAD;
        }
    }

    memcpy(body_out, payload + PROTO_CLIP_CHUNK_SIZE, body_len);
    if (body_len < body_cap) body_out[body_len] = '\0';
    *count_out = count;
    *index_out = index;
    *seq_out = seq;
    *flags_out = flags;
    *total_out = total;
    *body_len_out = (unsigned)body_len;
    return PROTO_OK;
}

proto_result_t proto_apply_snippet(unsigned *index_out, unsigned *total_out,
                                   char *label_out, size_t label_cap,
                                   size_t *label_len_out,
                                   char *body_out, size_t body_cap,
                                   size_t *body_len_out,
                                   const uint8_t *payload, uint16_t len)
{
    // Layout: u8 index, u8 total, u8 label_len, label[label_len], u16
    // body_len, body[body_len] -- the body length sits AFTER the label, so
    // its offset depends on label_len (docs/design/SNIPPETS_PUSH.md).
    if (len < 5) return PROTO_ERR_PAYLOAD;
    const unsigned index = payload[0];
    const unsigned total = payload[1];
    const size_t label_len = payload[2];
    const size_t body_len_off = (size_t)3 + label_len;
    if (len < body_len_off + 2) return PROTO_ERR_PAYLOAD;
    const size_t body_len = ((size_t)payload[body_len_off] << 8) | payload[body_len_off + 1];

    // Bounds before copies: the DS has no MMU, and this is the first message
    // that carries attacker-influenced variable-length text toward a buffer.
    if (total < 1 || total > SNIPPETS_MAX) return PROTO_ERR_PAYLOAD;
    if (index >= total) return PROTO_ERR_PAYLOAD;
    if (label_len < 1 || label_len > SNIPPET_LABEL_MAX) return PROTO_ERR_PAYLOAD;
    if (body_len > SNIPPET_BODY_MAX) return PROTO_ERR_PAYLOAD;
    if (label_len > label_cap || body_len > body_cap) return PROTO_ERR_PAYLOAD;
    if (body_len_off + 2 + body_len != (size_t)len) return PROTO_ERR_PAYLOAD;

    memcpy(label_out, payload + 3, label_len);
    memcpy(body_out, payload + body_len_off + 2, body_len);
    *index_out = index;
    *total_out = total;
    *label_len_out = label_len;
    *body_len_out = body_len;
    return PROTO_OK;
}

size_t proto_encode(uint8_t *out, msg_type_t type, const uint8_t *payload, uint16_t len)
{
    out[0] = PROTO_MAGIC0;
    out[1] = PROTO_MAGIC1;
    out[2] = PROTO_VERSION;
    out[3] = (uint8_t)type;
    out[4] = (uint8_t)(len >> 8);
    out[5] = (uint8_t)(len & 0xFF);
    if (len && payload) memcpy(out + PROTO_HEADER_SIZE, payload, len);
    return (size_t)PROTO_HEADER_SIZE + len;
}

int proto_camera_type(uint8_t type)
{
    switch (type) {
    case MSG_CAMERA:
    case MSG_CAMERA_FRAME:
    case MSG_PHOTO_CHUNK:
    case MSG_PHOTO_END:
    case MSG_PHOTO_DISCARD:
        return 1;
    default:
        return 0;
    }
}

size_t proto_encode_empty(uint8_t *out, msg_type_t type)
{
    return proto_encode(out, type, NULL, 0);
}

static int is_device_id_hex(const char *s)
{
    if (!s) return 0;
    for (int i = 0; i < PROTO_DEVICE_ID_HEX; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return s[PROTO_DEVICE_ID_HEX] == '\0';
}

int proto_hello_payload(char *out, size_t cap, const char *device_id_hex,
                        const uint8_t *card_nonce,
                        const char *ext_tokens, int camera)
{
    if (!out || cap < PROTO_HELLO_CAPS_LEN + 1) return -1;
    memcpy(out, PROTO_HELLO_CAPS, PROTO_HELLO_CAPS_LEN + 1);
    size_t len = PROTO_HELLO_CAPS_LEN;

    if (camera && len + PROTO_HELLO_CAMERA_LEN + 1 <= cap) {
        memcpy(out + len, " camera", PROTO_HELLO_CAMERA_LEN + 1);
        len += PROTO_HELLO_CAMERA_LEN;
    }

    if (device_id_hex && device_id_hex[0] && is_device_id_hex(device_id_hex) &&
        len + PROTO_HELLO_ID_LEN + 1 <= cap) {
        memcpy(out + len, " id=", 4);
        memcpy(out + len + 4, device_id_hex, PROTO_DEVICE_ID_HEX);
        len += PROTO_HELLO_ID_LEN;
        out[len] = '\0';
    }

    if (card_nonce && len + PROTO_HELLO_CN_LEN + 1 <= cap) {
        static const char HEX[] = "0123456789abcdef";
        memcpy(out + len, " cn=", 4);
        for (int i = 0; i < PROTO_CARD_NONCE_BYTES; i++) {
            out[len + 4 + 2 * i] = HEX[card_nonce[i] >> 4];
            out[len + 5 + 2 * i] = HEX[card_nonce[i] & 15];
        }
        len += PROTO_HELLO_CN_LEN;
        out[len] = '\0';
    }

    if (ext_tokens && ext_tokens[0]) {
        const size_t n = strlen(ext_tokens);
        if (n <= PROTO_HELLO_EXT_MAX && len + n + 1 <= cap) {
            memcpy(out + len, ext_tokens, n + 1);
            len += n;
        }
    }
    return (int)len;
}

// --------------------------------------------------------------- camera ---

size_t proto_encode_camera(uint8_t *out, int state)
{
    if (state < PROTO_CAMERA_OFF || state > PROTO_CAMERA_OUTER) return 0;
    const uint8_t body[1] = { (uint8_t)state };
    return proto_encode(out, MSG_CAMERA, body, sizeof(body));
}

size_t proto_encode_camera_frame(uint8_t *out, int format, uint8_t flags,
                                 uint8_t seq, const uint8_t *px)
{
    const size_t len = camera_view_bytes(format);
    if (!px || !len || (flags & ~PROTO_PREVIEW_FLAGS)) return 0;
    uint8_t body[PROTO_CAMERA_FRAME_SIZE];
    body[0] = (uint8_t)format;
    body[1] = flags;
    body[2] = seq;
    memcpy(body + 3, px, len);
    return proto_encode(out, MSG_CAMERA_FRAME, body, (uint16_t)(3 + len));
}

proto_result_t proto_decode_mirror(int *watching_out, const uint8_t *payload,
                                   uint16_t len)
{
    if (!payload || len != 1 || (payload[0] & ~1u)) return PROTO_ERR_PAYLOAD;
    if (watching_out) *watching_out = payload[0] & 1;
    return PROTO_OK;
}

proto_result_t proto_decode_touch(uint8_t *x_out, uint8_t *y_out,
                                  const uint8_t *payload, uint16_t len)
{
    if (!payload || len != 2 || payload[1] >= 192) return PROTO_ERR_PAYLOAD;
    if (x_out) *x_out = payload[0];
    if (y_out) *y_out = payload[1];
    return PROTO_OK;
}

size_t proto_encode_photo_chunk(uint8_t *out, uint32_t offset,
                                const uint8_t *data, uint16_t len)
{
    // No empty chunk, none bigger than a KB, and none that runs past the
    // picture: the server counts bytes, and a chunk its count cannot place
    // is a dropped connection.
    if (!data || len == 0 || len > PROTO_PHOTO_CHUNK_MAX) return 0;
    if (offset >= PROTO_PHOTO_BYTES_MAX ||
        offset + len > PROTO_PHOTO_BYTES_MAX) return 0;
    uint8_t body[4 + PROTO_PHOTO_CHUNK_MAX];
    body[0] = (uint8_t)(offset >> 24);
    body[1] = (uint8_t)(offset >> 16);
    body[2] = (uint8_t)(offset >> 8);
    body[3] = (uint8_t)offset;
    memcpy(body + 4, data, len);
    return proto_encode(out, MSG_PHOTO_CHUNK, body, (uint16_t)(4 + len));
}

size_t proto_encode_photo_end(uint8_t *out, uint8_t format, uint8_t camera,
                              uint32_t crc)
{
    const size_t total = camera_photo_bytes(format);
    if (!total) return 0;
    if (camera != PROTO_CAMERA_INNER && camera != PROTO_CAMERA_OUTER) return 0;
    uint8_t body[10];
    body[0] = format;
    body[1] = camera;
    body[2] = (uint8_t)(total >> 24);
    body[3] = (uint8_t)(total >> 16);
    body[4] = (uint8_t)(total >> 8);
    body[5] = (uint8_t)total;
    body[6] = (uint8_t)(crc >> 24);
    body[7] = (uint8_t)(crc >> 16);
    body[8] = (uint8_t)(crc >> 8);
    body[9] = (uint8_t)crc;
    return proto_encode(out, MSG_PHOTO_END, body, sizeof(body));
}

size_t proto_encode_photo_discard(uint8_t *out, uint16_t id)
{
    if (id == 0) return 0;
    const uint8_t body[2] = { (uint8_t)(id >> 8), (uint8_t)id };
    return proto_encode(out, MSG_PHOTO_DISCARD, body, sizeof(body));
}

proto_result_t proto_decode_camera_ready(int *can_save_out,
                                         const uint8_t *payload, uint16_t len)
{
    if (!can_save_out || !payload || len != 1) return PROTO_ERR_PAYLOAD;
    // Reserved bits are refused, not masked: a server that claims a flag this
    // build does not know is not the server it thinks it is.
    if (payload[0] & ~PROTO_READY_CAN_SAVE) return PROTO_ERR_PAYLOAD;
    *can_save_out = (payload[0] & PROTO_READY_CAN_SAVE) ? 1 : 0;
    return PROTO_OK;
}

proto_result_t proto_decode_camera_ack(uint8_t *kind_out, uint32_t *value_out,
                                       const uint8_t *payload, uint16_t len)
{
    if (!kind_out || !value_out || !payload || len != 5) {
        return PROTO_ERR_PAYLOAD;
    }
    const uint8_t kind = payload[0];
    const uint32_t value = ((uint32_t)payload[1] << 24) |
                           ((uint32_t)payload[2] << 16) |
                           ((uint32_t)payload[3] << 8) |
                           (uint32_t)payload[4];
    if (kind != PROTO_ACK_PREVIEW && kind != PROTO_ACK_PHOTO) {
        return PROTO_ERR_PAYLOAD;
    }
    // A preview ack answers a seq, which went out as one byte.
    if (kind == PROTO_ACK_PREVIEW && value > 0xFF) return PROTO_ERR_PAYLOAD;
    *kind_out = kind;
    *value_out = value;
    return PROTO_OK;
}

proto_result_t proto_decode_photo_saved(photo_saved_t *out,
                                        const uint8_t *payload, uint16_t len)
{
    if (!out || !payload || len < 4) return PROTO_ERR_PAYLOAD;
    memset(out, 0, sizeof(*out));
    const uint8_t status = payload[0];
    const uint16_t id = (uint16_t)((payload[1] << 8) | payload[2]);
    const size_t path_len = payload[3];
    if (status > PROTO_SAVED_MAX) return PROTO_ERR_PAYLOAD;
    if ((size_t)4 + path_len != (size_t)len) return PROTO_ERR_PAYLOAD;
    if (status != PROTO_SAVED_OK) {
        // No id, no path: an unsaved photo claims nothing.
        if (id != 0 || path_len != 0) return PROTO_ERR_PAYLOAD;
        out->status = status;
        return PROTO_OK;
    }
    if (id == 0 || path_len < 1 || path_len > PROTO_PHOTO_PATH_MAX) {
        return PROTO_ERR_PAYLOAD;
    }
    // The path is this file's COMMANDS argument: server text that lands only
    // in the composer.  Refused, not sanitized, for clip.c's reason -- a path
    // quietly altered on the way in names something other than what was
    // written.
    for (size_t i = 0; i < path_len; i++) {
        const uint8_t byte = payload[4 + i];
        if (byte < 0x20 || byte > 0x7E) return PROTO_ERR_PAYLOAD;
    }
    memcpy(out->path, payload + 4, path_len);
    out->path[path_len] = '\0';
    out->status = status;
    out->id = id;
    out->path_len = (uint8_t)path_len;
    return PROTO_OK;
}

// --------------------------------------------------------------- REMOTE ---

#define REMOTE_FIXED 8      // count index kind state flags age(2) name_len

proto_result_t proto_apply_remote(remote_chunk_t *chunk,
                                  const uint8_t *payload, uint16_t len)
{
    if (!chunk || !payload || len < 2) return PROTO_ERR_PAYLOAD;
    memset(chunk, 0, sizeof(*chunk));
    const unsigned count = payload[0];
    if (count > REMOTE_MAX) return PROTO_ERR_PAYLOAD;
    if (count == 0) {
        // Exactly two bytes: count and status.  Anything else is a later
        // format this card does not know, not something to guess at.
        if (len != 2) return PROTO_ERR_PAYLOAD;
        chunk->status = payload[1];
        return PROTO_OK;
    }
    if (len < REMOTE_FIXED + 1) return PROTO_ERR_PAYLOAD;
    const unsigned index = payload[1];
    if (index >= count) return PROTO_ERR_PAYLOAD;
    const size_t name_len = payload[7];
    if (name_len > REMOTE_NAME_MAX) return PROTO_ERR_PAYLOAD;
    if ((size_t)REMOTE_FIXED + name_len + 1 > len) return PROTO_ERR_PAYLOAD;
    const size_t detail_len = payload[REMOTE_FIXED + name_len];
    if (detail_len > REMOTE_DETAIL_MAX) return PROTO_ERR_PAYLOAD;
    if ((size_t)REMOTE_FIXED + name_len + 1 + detail_len != len) {
        return PROTO_ERR_PAYLOAD;
    }
    chunk->count = (uint8_t)count;
    remote_session_t *e = &chunk->entry;
    e->index = (uint8_t)index;
    e->kind = payload[2];
    e->state = payload[3] < REMOTE_STATE_COUNT ? payload[3] : REMOTE_STATE_UNKNOWN;
    e->flags = (uint8_t)(payload[4] & REMOTE_FLAG_KNOWN);
    e->age_min = (uint16_t)((payload[5] << 8) | payload[6]);
    const uint8_t *name = payload + REMOTE_FIXED;
    const uint8_t *detail = name + name_len + 1;
    for (size_t i = 0; i < name_len; i++) {
        e->name[i] = (name[i] >= 0x20 && name[i] <= 0x7e) ? (char)name[i] : '?';
    }
    e->name[name_len] = '\0';
    if (!name_len) snprintf(e->name, sizeof(e->name), "untitled");
    for (size_t i = 0; i < detail_len; i++) {
        e->detail[i] = (detail[i] >= 0x20 && detail[i] <= 0x7e)
                           ? (char)detail[i] : '?';
    }
    e->detail[detail_len] = '\0';
    return PROTO_OK;
}

proto_result_t proto_apply_remote_note(char *out, size_t cap,
                                       const uint8_t *payload, uint16_t len)
{
    if (!out || !cap || (!payload && len)) return PROTO_ERR_PAYLOAD;
    if (len > REMOTE_NOTE_MAX) return PROTO_ERR_PAYLOAD;
    size_t n = len < cap - 1 ? len : cap - 1;
    for (size_t i = 0; i < n; i++) {
        out[i] = (payload[i] >= 0x20 && payload[i] <= 0x7e) ? (char)payload[i]
                                                             : '?';
    }
    out[n] = '\0';
    return PROTO_OK;
}

proto_result_t proto_apply_state(uint8_t *state, char *detail, size_t cap,
                                 const uint8_t *payload, uint16_t len)
{
    int has_ask;
    uint32_t ask_id;
    uint8_t flags;
    return proto_apply_state_ask(state, detail, cap, &has_ask, &ask_id, &flags,
                                 payload, len);
}

proto_result_t proto_apply_state_ask(uint8_t *state, char *detail, size_t cap,
                                     int *has_ask, uint32_t *ask_id,
                                     uint8_t *flags,
                                     const uint8_t *payload, uint16_t len)
{
    if (!state || !has_ask || !ask_id || !flags || !payload || len < 2) {
        return PROTO_ERR_PAYLOAD;
    }
    const size_t n = payload[1];
    if (n > PROTO_STATE_DETAIL_MAX ||
        ((size_t)len != 2 + n && (size_t)len != 2 + n + PROTO_ASK_TAIL)) {
        return PROTO_ERR_PAYLOAD;
    }
    // A state this card has no word for reads as unknown, never as an error:
    // a newer server is allowed to know more than this card does.
    *state = payload[0] <= PROTO_STATE_INPUT ? payload[0] : PROTO_STATE_UNKNOWN;
    if (detail && cap) {
        const size_t take = n < cap - 1 ? n : cap - 1;
        for (size_t i = 0; i < take; i++) {
            const uint8_t ch = payload[2 + i];
            detail[i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?';
        }
        detail[take] = '\0';
    }
    *has_ask = (size_t)len == 2 + n + PROTO_ASK_TAIL;
    if (*has_ask) {
        const uint8_t *t = payload + 2 + n;
        *ask_id = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 |
                  (uint32_t)t[2] << 8 | t[3];
        *flags = t[4];
    }
    return PROTO_OK;
}

size_t proto_encode_answer(uint8_t *out, uint32_t ask_id,
                           const uint8_t *keys, size_t len)
{
    if (!out || !keys || len == 0 || len > PROTO_MAX_KEY_PAYLOAD) return 0;
    uint8_t body[PROTO_ANSWER_MAX];
    body[0] = (uint8_t)(ask_id >> 24);
    body[1] = (uint8_t)(ask_id >> 16);
    body[2] = (uint8_t)(ask_id >> 8);
    body[3] = (uint8_t)ask_id;
    memcpy(body + 4, keys, len);
    return proto_encode(out, MSG_ANSWER, body, (uint16_t)(4 + len));
}

size_t proto_encode_remote_open(uint8_t *out, int open)
{
    const uint8_t body[1] = { (uint8_t)(open ? 1 : 0) };
    return proto_encode(out, MSG_REMOTE_OPEN, body, sizeof(body));
}

size_t proto_encode_remote_pick(uint8_t *out, uint8_t index, uint8_t action,
                                uint16_t check)
{
    const uint8_t body[4] = { index, action, (uint8_t)(check >> 8),
                              (uint8_t)check };
    return proto_encode(out, MSG_REMOTE_PICK, body, sizeof(body));
}

// ----------------------------------------------------------------- voice ---

int proto_voice_type(uint8_t type)
{
    return type == MSG_VOICE_START || type == MSG_VOICE_CHUNK ||
           type == MSG_VOICE_END;
}

static int valid_voice_lang_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

size_t proto_encode_voice_start(uint8_t *out, uint8_t format, const char *lang)
{
    if (!out || format != VOICE_FORMAT_IMA_ADPCM_8K) return 0;
    const size_t n = lang ? strlen(lang) : 0;
    if (n > VOICE_LANG_MAX) return 0;
    uint8_t body[2 + VOICE_LANG_MAX];
    body[0] = format;
    body[1] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) {
        if (!valid_voice_lang_char(lang[i])) return 0;
        body[2 + i] = (uint8_t)lang[i];
    }
    return proto_encode(out, MSG_VOICE_START, body, (uint16_t)(2 + n));
}

size_t proto_encode_voice_chunk(uint8_t *out, uint32_t offset,
                                const uint8_t *data, uint16_t len)
{
    if (!out || !data || len == 0 || len > VOICE_CHUNK_MAX) return 0;
    if (offset >= VOICE_BYTES_MAX || offset + len > VOICE_BYTES_MAX) return 0;
    // Static: 484 bytes is past what the ARM9's ~16 KB DTCM stack should
    // carry in one frame, with no MMU to catch an overflow.  Only the main
    // loop encodes chunks (mic.c, never an interrupt), so nothing re-enters.
    static uint8_t body[4 + VOICE_CHUNK_MAX];
    body[0] = (uint8_t)(offset >> 24);
    body[1] = (uint8_t)(offset >> 16);
    body[2] = (uint8_t)(offset >> 8);
    body[3] = (uint8_t)offset;
    memcpy(body + 4, data, len);
    return proto_encode(out, MSG_VOICE_CHUNK, body, (uint16_t)(4 + len));
}

size_t proto_encode_voice_end(uint8_t *out, uint32_t total, uint32_t crc)
{
    if (!out || total == 0 || total > VOICE_BYTES_MAX) return 0;
    const uint8_t body[8] = {
        (uint8_t)(total >> 24), (uint8_t)(total >> 16),
        (uint8_t)(total >> 8),  (uint8_t)total,
        (uint8_t)(crc >> 24),   (uint8_t)(crc >> 16),
        (uint8_t)(crc >> 8),    (uint8_t)crc,
    };
    return proto_encode(out, MSG_VOICE_END, body, sizeof(body));
}

proto_result_t proto_decode_mic_ready(int *can_transcribe_out,
                                      const uint8_t *payload, uint16_t len)
{
    if (!can_transcribe_out || !payload || len != 1) return PROTO_ERR_PAYLOAD;
    if (payload[0] & ~PROTO_MIC_CAN_TRANSCRIBE) return PROTO_ERR_PAYLOAD;
    *can_transcribe_out = (payload[0] & PROTO_MIC_CAN_TRANSCRIBE) ? 1 : 0;
    return PROTO_OK;
}

proto_result_t proto_decode_voice_ack(uint32_t *acked_out,
                                      const uint8_t *payload, uint16_t len)
{
    if (!acked_out || !payload || len != 4) return PROTO_ERR_PAYLOAD;
    const uint32_t v = ((uint32_t)payload[0] << 24) |
                       ((uint32_t)payload[1] << 16) |
                       ((uint32_t)payload[2] << 8) |
                       (uint32_t)payload[3];
    if (v > VOICE_BYTES_MAX) return PROTO_ERR_PAYLOAD;
    *acked_out = v;
    return PROTO_OK;
}

proto_result_t proto_decode_voice_text(voice_text_t *out,
                                       const uint8_t *payload, uint16_t len)
{
    if (!out || !payload || len < 3) return PROTO_ERR_PAYLOAD;
    memset(out, 0, sizeof(*out));
    const uint8_t status = payload[0];
    const uint16_t text_len = (uint16_t)((payload[1] << 8) | payload[2]);
    if (status > VOICE_STATUS_MAX) return PROTO_ERR_PAYLOAD;
    if ((size_t)3 + text_len != (size_t)len) return PROTO_ERR_PAYLOAD;
    if (status != VOICE_STATUS_OK) {
        if (text_len != 0) return PROTO_ERR_PAYLOAD;
        out->status = status;
        return PROTO_OK;
    }
    if (text_len < 1 || text_len > VOICE_TEXT_MAX) return PROTO_ERR_PAYLOAD;
    for (size_t i = 0; i < text_len; i++) {
        const uint8_t b = payload[3 + i];
        if (!((b >= 0x20 && b <= 0x7E) || b == '\n')) return PROTO_ERR_PAYLOAD;
    }
    memcpy(out->text, payload + 3, text_len);
    out->text[text_len] = '\0';
    out->status = status;
    out->text_len = text_len;
    return PROTO_OK;
}
