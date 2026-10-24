// REMOTE on the card: the decoder, the staged list, the rows, what A does,
// and that every line it draws fits the geometry it is built for.
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "support.h"

#include "../source/proto.h"
#include "../source/remote.h"

static size_t row_frame(uint8_t *out, unsigned count, unsigned index,
                        unsigned kind, unsigned state, unsigned flags,
                        unsigned age, const char *name, const char *detail)
{
    size_t n = 0;
    out[n++] = (uint8_t)count;
    out[n++] = (uint8_t)index;
    out[n++] = (uint8_t)kind;
    out[n++] = (uint8_t)state;
    out[n++] = (uint8_t)flags;
    out[n++] = (uint8_t)(age >> 8);
    out[n++] = (uint8_t)age;
    out[n++] = (uint8_t)strlen(name);
    memcpy(out + n, name, strlen(name));
    n += strlen(name);
    out[n++] = (uint8_t)strlen(detail);
    memcpy(out + n, detail, strlen(detail));
    n += strlen(detail);
    return n;
}

static void test_remote_decoder_is_strict(void)
{
    remote_chunk_t c;
    uint8_t p[96];
    size_t n = row_frame(p, 2, 1, 0, 2, 0x02, 75, "fix-tests",
                         "app - permission prompt");
    CHECK(proto_apply_remote(&c, p, (uint16_t)n) == PROTO_OK, "a good row failed");
    CHECK(c.count == 2 && c.entry.index == 1 && c.entry.state == 2 &&
              c.entry.flags == 0x02 && c.entry.age_min == 75 &&
              strcmp(c.entry.name, "fix-tests") == 0 &&
              strcmp(c.entry.detail, "app - permission prompt") == 0,
          "the row decoded wrong");
    CHECK(proto_apply_remote(&c, p, (uint16_t)(n - 1)) != PROTO_OK,
          "a short row was accepted");
    CHECK(proto_apply_remote(&c, p, (uint16_t)(n + 1)) != PROTO_OK,
          "a row with a trailing byte was accepted");
    p[1] = 2;                       // index == count
    CHECK(proto_apply_remote(&c, p, (uint16_t)n) != PROTO_OK,
          "an index past the count was accepted");
    const uint8_t empty[2] = { 0, REMOTE_STATUS_NO_CLAUDE };
    CHECK(proto_apply_remote(&c, empty, 2) == PROTO_OK && c.count == 0 &&
              c.status == REMOTE_STATUS_NO_CLAUDE,
          "the empty set with a status did not decode");
    CHECK(proto_apply_remote(&c, empty, 1) != PROTO_OK,
          "an empty set without its status byte was accepted");
    // Unprintable bytes become '?', and unknown flags and states are masked
    // rather than refused.
    n = row_frame(p, 1, 0, 1, 99, 0xF8 | 0x04, 1, "a\x01" "b", "\x7f");
    CHECK(proto_apply_remote(&c, p, (uint16_t)n) == PROTO_OK &&
              strcmp(c.entry.name, "a?b") == 0 &&
              strcmp(c.entry.detail, "?") == 0 &&
              c.entry.state == REMOTE_STATE_UNKNOWN &&
              c.entry.flags == REMOTE_FLAG_COPY,
          "an odd row was not cleaned up");

    char note[REMOTE_NOTE_MAX + 1];
    CHECK(proto_apply_remote_note(note, sizeof(note),
                                  (const uint8_t *)"opening fix-tests", 17) ==
              PROTO_OK && strcmp(note, "opening fix-tests") == 0,
          "a note did not decode");
    uint8_t st;
    char detail[32];
    const uint8_t sp[] = { 3, 17, 'p','e','r','m','i','s','s','i','o','n',' ',
                           'p','r','o','m','p','t' };
    CHECK(proto_apply_state(&st, detail, sizeof(detail), sp, sizeof(sp)) ==
              PROTO_OK && st == PROTO_STATE_APPROVAL &&
              strcmp(detail, "permission prompt") == 0,
          "STATE did not decode");
    const uint8_t future[] = { 9, 0 };
    CHECK(proto_apply_state(&st, detail, sizeof(detail), future, 2) == PROTO_OK &&
              st == PROTO_STATE_UNKNOWN,
          "a state from the future was not read as unknown");
    CHECK(proto_apply_state(&st, detail, sizeof(detail), sp, 5) != PROTO_OK,
          "a truncated STATE was accepted");

    uint8_t out[16];
    const size_t pn = proto_encode_remote_pick(out, 1, REMOTE_PICK_OPEN, 0xBEEF);
    CHECK(pn == PROTO_HEADER_SIZE + 4 && out[3] == MSG_REMOTE_PICK &&
              out[6] == 1 && out[7] == 0 && out[8] == 0xBE && out[9] == 0xEF,
          "REMOTE_PICK encoded wrong");
}

static void apply_row(remote_list_t *r, unsigned count, unsigned index,
                      unsigned state, unsigned flags, const char *name)
{
    uint8_t p[96];
    remote_chunk_t c;
    const size_t n = row_frame(p, count, index, 0, state, flags, 3, name, "app");
    (void)proto_apply_remote(&c, p, (uint16_t)n);
    (void)remote_apply_chunk(r, &c);
}

static void test_remote_list_commits_whole_sets(void)
{
    remote_list_t r;
    remote_init(&r);
    CHECK(!r.ready, "a fresh list claimed the server speaks REMOTE");
    apply_row(&r, 2, 0, REMOTE_STATE_WORKING, REMOTE_FLAG_LIVE, "one");
    CHECK(r.count == 0 && !r.ready, "half a set was committed");
    apply_row(&r, 2, 1, REMOTE_STATE_NEEDS_YOU, REMOTE_FLAG_COPY, "two");
    CHECK(r.count == 2 && r.ready && remote_needs_you(&r) == 1,
          "a whole set was not committed");
    // A new, smaller set replaces it only when it is complete.
    apply_row(&r, 1, 0, REMOTE_STATE_IDLE, 0, "solo");
    CHECK(r.count == 1 && strcmp(r.items[0].name, "solo") == 0,
          "a complete one-row set did not replace the old one");
}

static void make_world(remote_list_t *r, remote_computers_t *c)
{
    remote_init(r);
    apply_row(r, 3, 0, REMOTE_STATE_NEEDS_YOU, REMOTE_FLAG_LIVE, "fix-tests");
    apply_row(r, 3, 1, REMOTE_STATE_WORKING, REMOTE_FLAG_COPY,
              "a-name-of-24-characters!");
    apply_row(r, 3, 2, REMOTE_STATE_FAILED, 0, "broken");
    memset(c, 0, sizeof(*c));
    c->count = 2;
    snprintf(c->items[0].name, sizeof(c->items[0].name), "MacBook");
    snprintf(c->items[0].host, sizeof(c->items[0].host), "192.168.0.10");
    c->items[0].current = 1;
    c->items[0].saved = 1;
    snprintf(c->items[1].name, sizeof(c->items[1].name), "DESKTOP-WITH-20CHARS");
    snprintf(c->items[1].host, sizeof(c->items[1].host), "192.168.100.200");
    c->items[1].saved = 1;
    c->items[1].online = 1;
}

static void test_remote_rows_and_actions(void)
{
    remote_list_t r;
    remote_computers_t c;
    make_world(&r, &c);
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&r, &c, rows, REMOTE_ROWS_MAX);
    // header, 3 sessions, new, header, 2 computers
    CHECK(n == 8, "expected 8 rows, got %u", n);
    CHECK(rows[0].kind == REMOTE_ROW_HEADER && rows[4].kind == REMOTE_ROW_NEW &&
              rows[5].kind == REMOTE_ROW_HEADER,
          "the rows are not in their sections");

    remote_ui_t ui;
    remote_ui_init(&ui);
    remote_ui_settle(&ui, rows, n);
    CHECK(ui.selection == 1, "the first selectable row is not selected");
    uint8_t index = 0;
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_OPEN &&
              index == 0,
          "A on a live session does not open it");
    remote_ui_move(&ui, rows, n, 1);
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_ARM_COPY,
          "the first A on a copy row does not ask first");
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_COPY &&
              index == 1,
          "the second A on a copy row does not open the copy");
    remote_ui_move(&ui, rows, n, 1);
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_REFUSE,
          "a failed session can be opened");
    remote_ui_move(&ui, rows, n, 1);
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_NEW &&
              index == REMOTE_PICK_NEW_INDEX,
          "the new-session row does not start one");
    remote_ui_move(&ui, rows, n, 1);   // skips the header
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_ALREADY,
          "the current computer is offered as a switch");
    remote_ui_move(&ui, rows, n, 1);
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_SWITCH &&
              index == 1,
          "another computer is not a switch");
    remote_ui_move(&ui, rows, n, 1);   // wraps past the header
    CHECK(ui.selection == 1, "the selection did not wrap to the top");

    // Every line fits the row, and every explanation fits its buffers.
    const size_t widths[] = { 42, 64 };
    for (unsigned w = 0; w < 2; w++) {
        for (unsigned i = 0; i < n; i++) {
            char text[80];
            memset(text, 'X', sizeof(text));
            remote_row_text(&r, &c, &rows[i], "MacBook", text, widths[w]);
            CHECK(strlen(text) == widths[w], "row %u is %zu wide at %zu",
                  i, strlen(text), widths[w]);
        }
    }
    remote_ui_init(&ui);
    remote_ui_settle(&ui, rows, n);
    for (unsigned i = 0; i < n; i++) {
        if (!remote_row_selectable(&rows[i])) continue;
        ui.selection = (int)i;
        char title[64], body[640], footer[64];
        remote_explain(&ui, &r, &c, rows, n, "MacBook", title, sizeof(title),
                       body, sizeof(body), footer, sizeof(footer));
        CHECK(title[0] && body[0] && footer[0], "row %u explains nothing", i);
        // The top screen wraps at the grid's width; no body line is longer.
        const char *line = body;
        while (*line) {
            const char *end = strchr(line, '\n');
            const size_t len = end ? (size_t)(end - line) : strlen(line);
            CHECK(len <= 42, "row %u's explanation has a %zu-column line", i, len);
            if (!end) break;
            line = end + 1;
        }
    }
}

// The list re-sorts under the person's thumb: an armed copy is forgotten,
// and the selection stays on the same session, not the same row.
static void test_remote_selection_follows_the_session(void)
{
    remote_list_t r;
    remote_computers_t c;
    make_world(&r, &c);
    remote_ui_t ui;
    remote_ui_init(&ui);
    remote_row_t rows[REMOTE_ROWS_MAX];
    unsigned n = remote_rows(&r, &c, rows, REMOTE_ROWS_MAX);
    remote_ui_settle(&ui, rows, n);
    remote_ui_move(&ui, rows, n, 1);          // the copy row
    uint8_t index;
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_ARM_COPY,
          "setup: the copy row did not arm");
    char name[REMOTE_NAME_MAX + 1], detail[REMOTE_DETAIL_MAX + 1];
    snprintf(name, sizeof(name), "%s", r.items[1].name);
    snprintf(detail, sizeof(detail), "%s", r.items[1].detail);
    // A new set with that session moved to the top.
    remote_init(&r);
    apply_row(&r, 3, 0, REMOTE_STATE_WORKING, REMOTE_FLAG_COPY,
              "a-name-of-24-characters!");
    apply_row(&r, 3, 1, REMOTE_STATE_NEEDS_YOU, REMOTE_FLAG_LIVE, "fix-tests");
    apply_row(&r, 3, 2, REMOTE_STATE_FAILED, 0, "broken");
    r.ready = 1;
    remote_ui_follow(&ui, &r, &c, name, detail);
    n = remote_rows(&r, &c, rows, REMOTE_ROWS_MAX);
    CHECK(!ui.copy_armed, "an armed copy survived a new list");
    CHECK(ui.selection >= 0 && rows[ui.selection].kind == REMOTE_ROW_SESSION &&
              strcmp(r.items[rows[ui.selection].index].name, name) == 0,
          "the selection did not follow its session");
    // The check differs between two sessions and is stable for one.
    CHECK(remote_row_check(&r.items[0]) != remote_row_check(&r.items[1]),
          "two sessions share a check");
    CHECK(remote_row_check(&r.items[1]) == remote_row_check(&r.items[1]),
          "a check is not stable");
}

static void test_remote_before_the_server_speaks_it(void)
{
    // An old server never sends MSG_REMOTE: no new-session row (a pick
    // would be a message that server drops the link over), and the note
    // says why the half is empty.
    remote_list_t r;
    remote_computers_t c;
    remote_init(&r);
    memset(&c, 0, sizeof(c));
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&r, &c, rows, REMOTE_ROWS_MAX);
    for (unsigned i = 0; i < n; i++) {
        CHECK(rows[i].kind != REMOTE_ROW_NEW,
              "a new-session row was offered before the server proved REMOTE");
    }
    char text[43];
    remote_row_text(&r, &c, &rows[1], "x", text, 42);
    CHECK(strstr(text, "Update dsterm") != NULL, "the empty half does not say the fix: %s",
          text);
}

// The Python server's own encodings of the same messages (dsterm/protocol.py,
// printed by its tests), decoded here byte for byte: the two halves cannot
// drift without one of these failing.
static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    for (; hex[0] && hex[1] && n < cap; hex += 2) {
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void test_remote_matches_the_server(void)
{
    uint8_t f[128];
    remote_chunk_t c;
    size_t n = unhex("4453019f00020002", f, sizeof(f));
    CHECK(f[3] == MSG_REMOTE &&
              proto_apply_remote(&c, f + 6, (uint16_t)(n - 6)) == PROTO_OK &&
              c.count == 0 && c.status == REMOTE_STATUS_NO_CLAUDE,
          "the server's empty set (status 2) did not decode");
    n = unhex("4453019f00290201000202004b096669782d746573747317617070202d207065"
              "726d697373696f6e2070726f6d7074", f, sizeof(f));
    CHECK(proto_apply_remote(&c, f + 6, (uint16_t)(n - 6)) == PROTO_OK &&
              c.count == 2 && c.entry.index == 1 && c.entry.kind == 0 &&
              c.entry.state == 2 && c.entry.flags == REMOTE_FLAG_LIVE &&
              c.entry.age_min == 75 && strcmp(c.entry.name, "fix-tests") == 0 &&
              strcmp(c.entry.detail, "app - permission prompt") == 0,
          "the server's row did not decode");
    char note[REMOTE_NOTE_MAX + 1];
    n = unhex("445301a000116f70656e696e67206669782d7465737473", f, sizeof(f));
    CHECK(f[3] == MSG_REMOTE_NOTE &&
              proto_apply_remote_note(note, sizeof(note), f + 6,
                                      (uint16_t)(n - 6)) == PROTO_OK &&
              strcmp(note, "opening fix-tests") == 0,
          "the server's note did not decode");
    uint8_t st;
    char detail[32];
    n = unhex("445301a1001303117065726d697373696f6e2070726f6d7074", f, sizeof(f));
    CHECK(f[3] == MSG_STATE &&
              proto_apply_state(&st, detail, sizeof(detail), f + 6,
                                (uint16_t)(n - 6)) == PROTO_OK &&
              st == PROTO_STATE_APPROVAL && strcmp(detail, "permission prompt") == 0,
          "the server's needs-approval STATE did not decode");
    // The same STATE as the server sends it to an "ask" card: the open
    // question's id and the dropped flag ride behind the detail.
    {
        int has_ask = 0;
        uint32_t ask_id = 0;
        uint8_t flags = 0;
        n = unhex("445301a1001803117065726d697373696f6e2070726f6d7074c0ffee0101",
                  f, sizeof(f));
        CHECK(proto_apply_state_ask(&st, detail, sizeof(detail), &has_ask,
                                    &ask_id, &flags, f + 6,
                                    (uint16_t)(n - 6)) == PROTO_OK &&
                  st == PROTO_STATE_APPROVAL &&
                  strcmp(detail, "permission prompt") == 0 && has_ask &&
                  ask_id == 0xC0FFEE01u && flags == PROTO_ASK_FLAG_DROPPED,
              "the server's STATE with an ask id did not decode");
        CHECK(proto_apply_state(&st, detail, sizeof(detail), f + 6,
                                (uint16_t)(n - 6)) == PROTO_OK,
              "a STATE with an ask id broke the plain decoder");
        CHECK(proto_apply_state_ask(&st, detail, sizeof(detail), &has_ask,
                                    &ask_id, &flags, f + 6,
                                    (uint16_t)(n - 7)) != PROTO_OK,
              "a STATE with a short ask tail was accepted");
        n = unhex("445301a100020100", f, sizeof(f));
        CHECK(proto_apply_state_ask(&st, detail, sizeof(detail), &has_ask,
                                    &ask_id, &flags, f + 6,
                                    (uint16_t)(n - 6)) == PROTO_OK && !has_ask,
              "a STATE without the tail claimed an ask id");
    }
    n = unhex("445301a100020100", f, sizeof(f));
    CHECK(proto_apply_state(&st, detail, sizeof(detail), f + 6,
                            (uint16_t)(n - 6)) == PROTO_OK &&
              st == PROTO_STATE_IDLE && detail[0] == '\0',
          "the server's idle STATE did not decode");
    // The pick, check and all, exactly as the server decodes it: the row
    // "fix-tests" / "app - permission prompt" checks to 0x10BB.
    remote_session_t row;
    memset(&row, 0, sizeof(row));
    snprintf(row.name, sizeof(row.name), "fix-tests");
    snprintf(row.detail, sizeof(row.detail), "app - permission prompt");
    CHECK(remote_row_check(&row) == 0x10BB, "the row check is 0x%04X, not 0x10BB",
          remote_row_check(&row));
    uint8_t out[16], want[16];
    size_t on = proto_encode_remote_pick(out, 1, REMOTE_PICK_OPEN,
                                         remote_row_check(&row));
    n = unhex("4453011900040100" "10bb", want, sizeof(want));
    CHECK(on == n && memcmp(out, want, n) == 0,
          "REMOTE_PICK does not match what the server decodes");
    on = proto_encode_remote_pick(out, REMOTE_PICK_NEW_INDEX, REMOTE_PICK_NEW, 0);
    n = unhex("445301190004ff020000", want, sizeof(want));
    CHECK(on == n && memcmp(out, want, n) == 0,
          "a NEW pick does not match what the server decodes");
}

// Every server-to-client type the card handles must also get past the frame
// reader.  Decoding payloads directly (as the tests above do) cannot see a
// type the reader refuses -- which is how REMOTE, REMOTE_NOTE and STATE once
// shipped as types the card would have dropped the link over, every time.
static void test_every_handled_type_passes_the_reader(void)
{
    static const msg_type_t TYPES[] = {
        MSG_NONCE, MSG_AUTH_OK, MSG_AUTH_FAIL, MSG_FRAME_FULL, MSG_FRAME_DIFF,
        MSG_SCROLL, MSG_PONG, MSG_CURSOR, MSG_VIEW_POS, MSG_MODE, MSG_SESSIONS,
        MSG_SNIPPETS, MSG_EXTENSION, MSG_PROJECT, MSG_CLIP, MSG_PAIR_REPLY,
        MSG_PAIR_OK, MSG_PAIR_FAIL, MSG_REMOTE, MSG_REMOTE_NOTE, MSG_STATE,
        MSG_CAMERA_READY, MSG_CAMERA_ACK, MSG_PHOTO_SAVED,
        MSG_MIC_READY, MSG_VOICE_ACK, MSG_VOICE_TEXT,
        MSG_ERROR,
    };
    for (unsigned i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        static framereader_t fr;
        framereader_init(&fr);
        uint8_t frame[16];
        const uint8_t body[2] = { 0, 1 };
        const size_t n = proto_encode(frame, TYPES[i], body, sizeof(body));
        CHECK(framereader_feed(&fr, frame, n) == PROTO_OK, "feed failed");
        msg_type_t type;
        const uint8_t *payload;
        uint16_t len;
        CHECK(framereader_next(&fr, &type, &payload, &len) == PROTO_OK &&
                  type == TYPES[i],
              "the frame reader refuses type 0x%02X", TYPES[i]);
    }
}

static void test_remote_computer_rows_fit_names_and_say_moved(void)
{
    remote_list_t r;
    remote_init(&r);
    remote_computers_t c;
    memset(&c, 0, sizeof(c));
    c.count = 2;
    snprintf(c.items[0].name, sizeof(c.items[0].name), "DESKTOP-ABC1234");
    snprintf(c.items[0].host, sizeof(c.items[0].host), "192.168.0.20");
    c.items[0].current = 1;
    c.items[0].saved = 1;
    snprintf(c.items[1].name, sizeof(c.items[1].name), "StudyPC");
    snprintf(c.items[1].host, sizeof(c.items[1].host), "192.168.0.31");
    snprintf(c.items[1].old_host, sizeof(c.items[1].old_host), "192.168.0.30");
    c.items[1].saved = 1;
    c.items[1].moved = 1;
    c.items[1].online = 1;

    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&r, &c, rows, REMOTE_ROWS_MAX);
    char text[43];
    int saw_here = 0, saw_moved = 0;
    for (unsigned i = 0; i < n; i++) {
        if (rows[i].kind != REMOTE_ROW_COMPUTER) continue;
        remote_row_text(&r, &c, &rows[i], "x", text, 42);
        CHECK(strlen(text) == 42, "a computer row is %zu wide", strlen(text));
        if (rows[i].index == 0) {
            // A 15-character hostname fits whole, beside its address and tag.
            saw_here = strstr(text, "DESKTOP-ABC1234") && strstr(text, "HERE");
        } else {
            saw_moved = strstr(text, "192.168.0.31") && strstr(text, "MOVED");
        }
    }
    CHECK(saw_here, "the PC's row lost its name or HERE");
    CHECK(saw_moved, "a moved computer is not shown at its new address as MOVED");

    // The explanation names both addresses and what A does.
    remote_ui_t ui;
    remote_ui_init(&ui);
    for (unsigned i = 0; i < n; i++) {
        if (rows[i].kind == REMOTE_ROW_COMPUTER && rows[i].index == 1) ui.selection = (int)i;
    }
    char title[64], body[640], footer[64];
    remote_explain(&ui, &r, &c, rows, n, "DESKTOP-ABC1234", title, sizeof(title),
                   body, sizeof(body), footer, sizeof(footer));
    CHECK(strstr(body, "192.168.0.31") && strstr(body, "192.168.0.30") &&
              strstr(footer, "A SWITCH"),
          "the moved row's explanation: %s / %s", body, footer);
    for (const char *line = body; *line; ) {
        const char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        CHECK(len <= 42, "an explanation line is %zu wide", len);
        line = end ? end + 1 : line + len;
    }
    uint8_t index = 0;
    CHECK(remote_ui_activate(&ui, &r, &c, rows, n, &index) == REMOTE_DO_SWITCH &&
              index == 1,
          "A on a moved computer does not switch to it");
}

void test_remote_all(void)
{
    test_every_handled_type_passes_the_reader();
    test_remote_matches_the_server();
    printf("== remote ==\n");
    test_remote_decoder_is_strict();
    test_remote_list_commits_whole_sets();
    test_remote_rows_and_actions();
    test_remote_selection_follows_the_session();
    test_remote_before_the_server_speaks_it();
    test_remote_computer_rows_fit_names_and_say_moved();
}
