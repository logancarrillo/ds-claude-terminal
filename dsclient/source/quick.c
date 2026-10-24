#include "quick.h"

#include <stdio.h>
#include <string.h>

#include "lang.h"

static const struct {
    tr_id_t label_id;
    tr_id_t note_id;
} ROWS[QUICK_ROW_COUNT] = {
    [QUICK_ROW_MODE]     = { TR_QUICK_ROW_MODE,     TR_QUICK_ROW_MODE_NOTE     },
    [QUICK_ROW_UNDO]     = { TR_QUICK_ROW_UNDO,     TR_QUICK_ROW_UNDO_NOTE     },
    [QUICK_ROW_CHANGES]  = { TR_QUICK_ROW_CHANGES,  TR_QUICK_ROW_CHANGES_NOTE  },
    [QUICK_ROW_COMMANDS] = { TR_QUICK_ROW_COMMANDS, TR_QUICK_ROW_COMMANDS_NOTE },
    [QUICK_ROW_AGAIN]    = { TR_QUICK_ROW_AGAIN,    TR_QUICK_ROW_AGAIN_NOTE    },
    [QUICK_ROW_VOICE]    = { TR_QUICK_ROW_VOICE,    TR_QUICK_ROW_VOICE_NOTE    },
    [QUICK_ROW_CONTROLS] = { TR_QUICK_ROW_CONTROLS, TR_QUICK_ROW_CONTROLS_NOTE },
    [QUICK_ROW_PHOTO]    = { TR_QUICK_ROW_PHOTO,    TR_QUICK_ROW_PHOTO_NOTE    },
};

unsigned quick_row_count(int camera)
{
    return (unsigned)QUICK_ROW_COUNT - (camera ? 0u : 1u);
}

const char *quick_row_label(quick_row_t row)
{
    return (unsigned)row < QUICK_ROW_COUNT ? tr(ROWS[row].label_id) : "";
}

// Whether this computer can hold a picture is only known when CAMERA_READY
// lands, so PHOTO's note can be replaced by the connection's truth.  Kept as
// a string id, not a pointer, so a language picked afterwards still applies.
static tr_id_t photo_note = TR_COUNT;
static tr_id_t voice_note = TR_COUNT;

void quick_set_photo_note(tr_id_t note)
{
    photo_note = note;
}

void quick_set_voice_note(tr_id_t note)
{
    voice_note = note;
}

const char *quick_row_note(quick_row_t row)
{
    if (row == QUICK_ROW_PHOTO && photo_note < TR_COUNT) return tr(photo_note);
    if (row == QUICK_ROW_VOICE && voice_note < TR_COUNT) return tr(voice_note);
    return (unsigned)row < QUICK_ROW_COUNT ? tr(ROWS[row].note_id) : "";
}

// ------------------------------------------------------------------ MODE ---

static int is_lower(char c) { return c >= 'a' && c <= 'z'; }

// The word before " mode on", or "edits" / "bypass" for the two modes Claude
// Code names another way.
int quick_mode_from_line(const char *line, char out[QUICK_MODE_MAX + 1])
{
    if (!line || !out) return 0;
    if (strstr(line, "accept edits on")) {
        snprintf(out, QUICK_MODE_MAX + 1, "edits");
        return 1;
    }
    if (strstr(line, "bypass permissions on")) {
        snprintf(out, QUICK_MODE_MAX + 1, "bypass");
        return 1;
    }
    const char *at = strstr(line, " mode on");
    if (!at || at == line) {
        // Claude Code versions that name no mode for the ordinary one show
        // only this hint where the mode would be.
        if (strstr(line, "? for shortcuts")) {
            snprintf(out, QUICK_MODE_MAX + 1, "normal");
            return 1;
        }
        return 0;
    }
    const char *start = at;
    while (start > line && is_lower(start[-1])) start--;
    const size_t n = (size_t)(at - start);
    if (n == 0 || n > QUICK_MODE_MAX) return 0;
    memcpy(out, start, n);
    out[n] = '\0';
    return 1;
}

// ------------------------------------------------------------- COMMANDS ---

// Always offered: the ones worth a button and not worth typing.  /clear and
// /compact reach the composer like the rest, so even those take an A.
static const char *const BUILTIN[] = {
    "/compact", "/context", "/clear", "/resume", "/review", "/init",
};
#define BUILTIN_COUNT (sizeof(BUILTIN) / sizeof(BUILTIN[0]))

unsigned quick_commands_builtin(void) { return (unsigned)BUILTIN_COUNT; }

void quick_commands_init(quick_commands_t *c)
{
    if (!c) return;
    memset(c, 0, sizeof(*c));
    for (unsigned i = 0; i < BUILTIN_COUNT; i++) {
        snprintf(c->names[i], sizeof(c->names[i]), "%s", BUILTIN[i]);
    }
    c->count = (unsigned)BUILTIN_COUNT;
}

static int name_char(char ch)
{
    return is_lower(ch) || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-' || ch == ':' || ch == '.';
}

static int valid_name(const char *s, size_t n)
{
    if (n < 2 || n > QUICK_COMMAND_MAX || s[0] != '/') return 0;
    if (!name_char(s[1]) || s[1] == '-' || s[1] == ':' || s[1] == '.') return 0;
    for (size_t i = 1; i < n; i++) {
        if (!name_char(s[i])) return 0;
    }
    return 1;
}

unsigned quick_commands_apply(quick_commands_t *c, const uint8_t *payload,
                              size_t len)
{
    if (!c) return 0;
    quick_commands_init(c);
    if (!payload) return 0;
    unsigned taken = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len && c->count < QUICK_COMMANDS_MAX; i++) {
        if (i < len && payload[i] != '\n') continue;
        const char *name = (const char *)payload + start;
        const size_t n = i - start;
        start = i + 1;
        if (!valid_name(name, n)) continue;
        int duplicate = 0;
        for (unsigned k = 0; k < c->count; k++) {
            if (strlen(c->names[k]) == n && memcmp(c->names[k], name, n) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;
        memcpy(c->names[c->count], name, n);
        c->names[c->count][n] = '\0';
        c->count++;
        taken++;
    }
    return taken;
}

// ------------------------------------------- the files' one-line format ---
//
// SAVED and AGAIN are stored the same way: one entry a line, with a newline
// inside an entry written as \n and a backslash as \\.  One writer and one
// reader for both, so the two files cannot drift apart.

// Append `item`, escaped, and its line's LF at `*at`.  Returns 0 -- leaving
// the output unusable, as the format functions report it -- when it would
// not fit with the terminator.
static int escape_line(const char *item, char *out, size_t *at, size_t cap)
{
    for (const char *p = item; *p; p++) {
        const char *esc = *p == '\n' ? "\\n" : *p == '\\' ? "\\\\" : NULL;
        const size_t need = esc ? 2 : 1;
        if (*at + need + 2 > cap) return 0;
        if (esc) {
            out[(*at)++] = esc[0];
            out[(*at)++] = esc[1];
        } else {
            out[(*at)++] = *p;
        }
    }
    if (*at + 2 > cap) return 0;
    out[(*at)++] = '\n';
    return 1;
}

// Decode the `n` bytes at `line` into `out` (room for `max` bytes and a NUL).
// Returns the decoded length, or 0 for a line to skip: an unknown or
// dangling escape, a control or non-ASCII byte, more than `max` bytes, or
// nothing at all.  A CR is dropped, so a file edited on Windows still reads.
static size_t unescape_line(const char *line, size_t n, char *out, size_t max)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = line[i];
        if (ch == '\\') {
            if (i + 1 >= n) return 0;
            const char next = line[++i];
            ch = next == 'n' ? '\n' : next == '\\' ? '\\' : 0;
            if (!ch) return 0;
        } else if (ch == '\r') {
            continue;
        } else if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) {
            return 0;
        }
        if (k >= max) return 0;
        out[k++] = ch;
    }
    out[k] = '\0';
    return k;
}

// ------------------------------------------------------- SAVED COMMANDS ---

void quick_saved_init(quick_saved_t *s)
{
    if (s) memset(s, 0, sizeof(*s));
}

quick_add_result_t quick_saved_add_at(quick_saved_t *s, const char *text,
                                      unsigned *index)
{
    if (!s || !text) return QUICK_ADD_EMPTY;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\r' ||
                 text[n - 1] == '\n' || text[n - 1] == '\t')) {
        n--;
    }
    size_t lead = 0;
    while (lead < n && (text[lead] == ' ' || text[lead] == '\t')) lead++;
    if (lead == n) return QUICK_ADD_EMPTY;
    if (n > QUICK_SAVED_TEXT_MAX) return QUICK_ADD_TOO_LONG;

    // Static: a kilobyte is too much for the ARM9 stack (DTCM, ~16 KB).
    static char clean[QUICK_SAVED_TEXT_MAX + 1];
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)text[i];
        if (ch == '\t') clean[i] = ' ';
        else if ((ch >= 0x20 && ch <= 0x7e) || ch == '\n') clean[i] = (char)ch;
        else return QUICK_ADD_INVALID;
    }
    clean[n] = '\0';

    for (unsigned i = 0; i < s->count; i++) {
        if (strcmp(s->items[i], clean) == 0) {
            if (index) *index = i;
            return QUICK_ADD_OK;
        }
    }
    if (s->count >= QUICK_SAVED_MAX) return QUICK_ADD_FULL;
    memcpy(s->items[s->count], clean, n + 1);
    if (index) *index = s->count;
    s->count++;
    return QUICK_ADD_OK;
}

int quick_saved_add(quick_saved_t *s, const char *text)
{
    return quick_saved_add_at(s, text, NULL) == QUICK_ADD_OK;
}

int quick_saved_remove(quick_saved_t *s, unsigned index)
{
    if (!s || index >= s->count) return 0;
    memmove(s->items[index], s->items[index + 1],
            (size_t)(s->count - index - 1) * sizeof(s->items[0]));
    s->count--;
    memset(s->items[s->count], 0, sizeof(s->items[0]));
    return 1;
}

size_t quick_saved_format(const quick_saved_t *s, char *out, size_t cap)
{
    if (!s || !out || cap == 0) return 0;
    size_t at = 0;
    for (unsigned i = 0; i < s->count; i++) {
        if (!escape_line(s->items[i], out, &at, cap)) return 0;
    }
    out[at] = '\0';
    return at;
}

void quick_saved_parse(quick_saved_t *s, const char *text)
{
    if (!s) return;
    quick_saved_init(s);
    if (!text) return;
    const char *line = text;
    while (*line && s->count < QUICK_SAVED_MAX) {
        const char *end = strchr(line, '\n');
        const size_t n = end ? (size_t)(end - line) : strlen(line);
        // Static: a kilobyte is too much for the ARM9 stack.
        static char decoded[QUICK_SAVED_TEXT_MAX + 1];
        if (unescape_line(line, n, decoded, QUICK_SAVED_TEXT_MAX)) {
            (void)quick_saved_add(s, decoded);
        }
        if (!end) break;
        line = end + 1;
    }
}

unsigned quick_commands_total(const quick_commands_t *c,
                              const quick_saved_t *s)
{
    const unsigned base = c ? c->count : (unsigned)BUILTIN_COUNT;
    const unsigned saved = s ? s->count : 0u;
    return base + saved + 1u;
}

quick_cmd_kind_t quick_commands_entry(const quick_commands_t *c,
                                      const quick_saved_t *s,
                                      unsigned index,
                                      const char **text_out,
                                      unsigned *saved_idx)
{
    if (text_out) *text_out = "";
    if (saved_idx) *saved_idx = 0;
    const unsigned builtin = (unsigned)BUILTIN_COUNT;
    const unsigned saved = s ? s->count : 0u;
    const unsigned total_c = c ? c->count : builtin;
    const unsigned server = total_c > builtin ? total_c - builtin : 0u;

    if (index < builtin) {
        if (text_out) *text_out = c ? c->names[index] : BUILTIN[index];
        return QUICK_CMD_BUILTIN;
    }
    if (index < builtin + saved) {
        const unsigned si = index - builtin;
        if (text_out) *text_out = s->items[si];
        if (saved_idx) *saved_idx = si;
        return QUICK_CMD_SAVED;
    }
    if (index < builtin + saved + server) {
        const unsigned ci = builtin + (index - builtin - saved);
        if (text_out) *text_out = c->names[ci];
        return QUICK_CMD_SERVER;
    }
    if (index == builtin + saved + server) {
        if (text_out) *text_out = tr(TR_QUICK_ADD);
        return QUICK_CMD_ADD;
    }
    return QUICK_CMD_NONE;
}

// ---------------------------------------------------------------- AGAIN ---

void quick_recent_init(quick_recent_t *r)
{
    if (r) memset(r, 0, sizeof(*r));
}

int quick_recent_add(quick_recent_t *r, const char *text)
{
    if (!r || !text) return 0;
    size_t n = strlen(text);
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\r' ||
                 text[n - 1] == '\n' || text[n - 1] == '\t')) {
        n--;
    }
    if (n == 0 || n > QUICK_RECENT_TEXT_MAX) return 0;

    // Already there: move it to the front.  Not there: everything moves down
    // one, and the oldest falls off a full list.
    int found = -1;
    for (unsigned i = 0; i < r->count; i++) {
        if (strlen(r->items[i]) == n && memcmp(r->items[i], text, n) == 0) {
            found = (int)i;
            break;
        }
    }
    if (found == 0) return 0;
    unsigned shift;
    if (found > 0) {
        shift = (unsigned)found;
    } else {
        shift = r->count < QUICK_RECENT_MAX ? r->count : QUICK_RECENT_MAX - 1;
        if (r->count < QUICK_RECENT_MAX) r->count++;
    }
    memmove(r->items[1], r->items[0], (size_t)shift * sizeof(r->items[0]));
    memcpy(r->items[0], text, n);
    r->items[0][n] = '\0';
    return 1;
}

int quick_recent_remove(quick_recent_t *r, unsigned index)
{
    if (!r || index >= r->count) return 0;
    memmove(r->items[index], r->items[index + 1],
            (size_t)(r->count - index - 1) * sizeof(r->items[0]));
    r->count--;
    memset(r->items[r->count], 0, sizeof(r->items[0]));
    return 1;
}

size_t quick_recent_format(const quick_recent_t *r, char *out, size_t cap)
{
    if (!r || !out || cap == 0) return 0;
    size_t at = 0;
    for (unsigned i = 0; i < r->count; i++) {
        if (!escape_line(r->items[i], out, &at, cap)) return 0;
    }
    out[at] = '\0';
    return at;
}

void quick_recent_parse(quick_recent_t *r, const char *text)
{
    if (!r) return;
    quick_recent_init(r);
    if (!text) return;
    const char *line = text;
    while (*line && r->count < QUICK_RECENT_MAX) {
        const char *end = strchr(line, '\n');
        const size_t n = end ? (size_t)(end - line) : strlen(line);
        static char decoded[QUICK_RECENT_TEXT_MAX + 1];
        const size_t k = unescape_line(line, n, decoded, QUICK_RECENT_TEXT_MAX);
        if (k) {
            memcpy(r->items[r->count], decoded, k + 1);
            r->count++;
        }
        if (!end) break;
        line = end + 1;
    }
}

void quick_recent_row(const char *text, char *out, size_t cols)
{
    if (!out || cols == 0) return;
    size_t at = 0;
    for (const char *p = text ? text : ""; *p && at < cols; p++) {
        if (*p == '\n') {
            for (const char *sep = " / "; *sep && at < cols; sep++) out[at++] = *sep;
        } else {
            out[at++] = *p;
        }
    }
    out[at] = '\0';
}

void quick_line_reset(quick_line_t *l)
{
    if (l) memset(l, 0, sizeof(*l));
}

int quick_line_feed(quick_line_t *l, const char *bytes, size_t n,
                    char *out, size_t cap)
{
    if (!l || !bytes) return 0;
    int done = 0;
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)bytes[i];
        if (ch == '\r' || ch == '\n') {
            if (!l->lost && l->len && out && cap > l->len) {
                memcpy(out, l->buf, l->len);
                out[l->len] = '\0';
                done = 1;
            }
            quick_line_reset(l);
        } else if (ch == 0x7f || ch == 0x08) {
            if (l->len) l->len--;
        } else if (ch == 0x15 || ch == 0x03) {
            // Ctrl-U clears the line; Ctrl-C abandons it.
            quick_line_reset(l);
        } else if (ch >= 0x20 && ch <= 0x7e) {
            if (l->len < QUICK_RECENT_TEXT_MAX) {
                l->buf[l->len++] = (char)ch;
            } else {
                l->lost = 1;
            }
        } else {
            // Esc (and every arrow, which starts with it), Tab, other control
            // keys: the line on screen is no longer the one typed here.
            l->lost = 1;
        }
    }
    return done;
}
