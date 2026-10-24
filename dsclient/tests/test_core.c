// Host-compiled tests for the platform-independent half of the DS client.
//
// The DS toolchain is not available here, but framing, bounds checking, and
// HMAC are plain C with no libnds dependency -- and they are the parts where a
// bug is either a security hole or an unreproducible glitch on a handheld. So
// they get tested on the host rather than debugged over a photograph.
//
// This is the terminal's suite: everything that ships when nothing is attached,
// linked against ext_none.c so the tests see exactly that build.  An
// extension's suite (tests/sources.ext.txt) links the same sources plus its
// own.

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../source/backoff.h"
#include "../source/buttons.h"
#include "../source/ext.h"
#include "../source/config.h"
#include "../source/chime.h"
#include "../source/clip.h"
#include "../source/composer.h"
#include "../source/focus.h"
#include "../source/font.h"
#include "../source/kbd.h"
#include "../source/keys.h"
#include "../source/lang.h"
#include "../source/log.h"
#include "../source/project.h"
#include "../source/proto.h"
#include "../source/sessions.h"
#include "../source/settings.h"
#include "../source/sha256.h"
#include "../source/mirror.h"
#include "../source/privacy.h"
#include "../source/quick.h"
#include "../source/snippets.h"
#include "../source/sound.h"
#include "../source/status.h"
#include "../source/ui_font.h"
#include "../source/voice.h"
#include "../source/guide.h"
#include "check.h"
#include "support.h"

// The conductor, as the terminal alone ships it.
void reset_sound(void)
{
    sound_reset();
}

// The key tick as the device does it (main.c's key_tick): texture stops, the
// extension may voice the press, and the conductor's PSG tick answers
// otherwise.  With nothing attached the PSG always answers.
void key_typed(int backspace)
{
    sound_phrase_stop();
    if (!ext_key_tick(backspace)) sound_key_typed(backspace);
}

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char *d = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[b[i] >> 4];
        out[i * 2 + 1] = d[b[i] & 0xF];
    }
    out[n * 2] = 0;
}

// ---------------------------------------------------------------- sha256 ---

static void test_sha256_vectors(void)
{
    char got[65];
    uint8_t d[32];

    // NIST FIPS 180-2 examples.
    sha256((const uint8_t *)"abc", 3, d);
    hex(d, 32, got);
    CHECK(strcmp(got, "ba7816bf8f01cfea414140de5dae2223"
                      "b00361a396177a9cb410ff61f20015ad") == 0, "abc -> %s", got);

    sha256((const uint8_t *)"", 0, d);
    hex(d, 32, got);
    CHECK(strcmp(got, "e3b0c44298fc1c149afbf4c8996fb924"
                      "27ae41e4649b934ca495991b7852b855") == 0, "empty -> %s", got);

    // Multi-block, exercises the length padding across a block boundary.
    sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
    hex(d, 32, got);
    CHECK(strcmp(got, "248d6a61d20638b8e5c026930c3e6039"
                      "a33ce45964ff2167f6ecedd419db06c1") == 0, "448-bit -> %s", got);
}

static void test_sha256_streaming_matches_oneshot(void)
{
    // The DS feeds bytes as they arrive; chunking must not change the digest.
    uint8_t a[32], b[32];
    const char *msg = "the quick brown fox jumps over the lazy dog, twice over";
    const size_t n = strlen(msg);

    sha256((const uint8_t *)msg, n, a);

    sha256_ctx ctx;
    sha256_init(&ctx);
    for (size_t i = 0; i < n; i++) sha256_update(&ctx, (const uint8_t *)msg + i, 1);
    sha256_final(&ctx, b);

    CHECK(memcmp(a, b, 32) == 0, "byte-at-a-time digest differs from one-shot");
}

static void test_hmac_rfc4231(void)
{
    char got[65];
    uint8_t mac[32];

    // RFC 4231 test case 1.
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    hmac_sha256(key, sizeof(key), (const uint8_t *)"Hi There", 8, mac);
    hex(mac, 32, got);
    CHECK(strcmp(got, "b0344c61d8db38535ca8afceaf0bf12b"
                      "881dc200c9833da726e9376c2e32cff7") == 0, "case1 -> %s", got);

    // Case 2: key shorter than the block.
    hmac_sha256((const uint8_t *)"Jefe", 4,
                (const uint8_t *)"what do ya want for nothing?", 28, mac);
    hex(mac, 32, got);
    CHECK(strcmp(got, "5bdcc146bf60754e6a042426089575c7"
                      "5a003f089d2739839dec58b964ec3843") == 0, "case2 -> %s", got);

    // Case 3 uses a 131-byte key, which must be hashed down first (RFC 2104).
    uint8_t longkey[131];
    memset(longkey, 0xaa, sizeof(longkey));
    hmac_sha256(longkey, sizeof(longkey),
                (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, mac);
    hex(mac, 32, got);
    CHECK(strcmp(got, "60e431591ee0b67f0d8a26aacbf5b77f"
                      "8e0bc6213728c5140546040f0ee37f54") == 0, "long key -> %s", got);
}

static void test_ct_equal(void)
{
    uint8_t a[4] = { 1, 2, 3, 4 };
    uint8_t b[4] = { 1, 2, 3, 4 };
    uint8_t c[4] = { 1, 2, 3, 5 };
    CHECK(ct_equal(a, b, 4) == 1, "equal buffers should match");
    CHECK(ct_equal(a, c, 4) == 0, "differing last byte should not match");
}

// --------------------------------------------------------------- framing ---

static size_t make_msg(uint8_t *out, msg_type_t t, const char *body)
{
    const uint16_t n = (uint16_t)(body ? strlen(body) : 0);
    return proto_encode(out, t, (const uint8_t *)body, n);
}

static void test_frame_roundtrip(void)
{
    uint8_t raw[64];
    const size_t n = make_msg(raw, MSG_KEY, "hello");

    framereader_t fr;
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, n) == PROTO_OK, "feed failed");

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_OK, "next failed");
    CHECK(t == MSG_KEY, "wrong type");
    CHECK(len == 5, "wrong length %u", len);
    CHECK(memcmp(p, "hello", 5) == 0, "wrong payload");
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_NEED_MORE, "expected NEED_MORE");
}

static void test_payload_survives_until_next_feed(void)
{
    // Regression: an earlier version compacted the buffer inside
    // framereader_next(), overwriting the payload it had just returned.
    uint8_t raw[128];
    size_t n = make_msg(raw, MSG_KEY, "FIRSTPAYLOAD");
    n += make_msg(raw + n, MSG_KEY, "SECONDPAYLOAD");

    framereader_t fr;
    framereader_init(&fr);
    framereader_feed(&fr, raw, n);

    msg_type_t t;
    const uint8_t *p1;
    uint16_t l1;
    CHECK(framereader_next(&fr, &t, &p1, &l1) == PROTO_OK, "first pop failed");
    CHECK(memcmp(p1, "FIRSTPAYLOAD", 12) == 0, "first payload wrong at pop");

    const uint8_t *p2;
    uint16_t l2;
    CHECK(framereader_next(&fr, &t, &p2, &l2) == PROTO_OK, "second pop failed");
    // The first payload must still be intact after popping the second.
    CHECK(memcmp(p1, "FIRSTPAYLOAD", 12) == 0, "first payload clobbered by second pop");
    CHECK(memcmp(p2, "SECONDPAYLOAD", 13) == 0, "second payload wrong");
}

static void test_byte_at_a_time(void)
{
    // One recv() is not one message. This is the failure the whole design warns
    // about, so it is tested at every possible split.
    uint8_t raw[128];
    const size_t n = make_msg(raw, MSG_FRAME_DIFF, "fragmented across boundaries");

    framereader_t fr;
    framereader_init(&fr);

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    int got = 0;
    for (size_t i = 0; i < n; i++) {
        CHECK(framereader_feed(&fr, raw + i, 1) == PROTO_OK, "feed byte %zu", i);
        proto_result_t r = framereader_next(&fr, &t, &p, &len);
        if (r == PROTO_OK) got++;
        else CHECK(r == PROTO_NEED_MORE, "unexpected %s at byte %zu", proto_strerror(r), i);
    }
    CHECK(got == 1, "expected exactly one message, got %d", got);
}

static void test_many_messages_one_feed(void)
{
    uint8_t raw[256];
    size_t n = 0;
    n += make_msg(raw + n, MSG_PING, NULL);
    n += make_msg(raw + n, MSG_KEY, "ab");
    n += make_msg(raw + n, MSG_PONG, NULL);

    framereader_t fr;
    framereader_init(&fr);
    framereader_feed(&fr, raw, n);

    msg_type_t types[3];
    const uint8_t *p;
    uint16_t len;
    for (int i = 0; i < 3; i++) {
        CHECK(framereader_next(&fr, &types[i], &p, &len) == PROTO_OK, "pop %d", i);
    }
    CHECK(types[0] == MSG_PING && types[1] == MSG_KEY && types[2] == MSG_PONG,
          "wrong message order");
}

static void test_hostile_headers(void)
{
    framereader_t fr;
    msg_type_t t;
    const uint8_t *p;
    uint16_t len;

    uint8_t bad_magic[] = { 'X', 'X', 1, MSG_PING, 0, 0 };
    framereader_init(&fr);
    framereader_feed(&fr, bad_magic, sizeof(bad_magic));
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_ERR_MAGIC, "bad magic accepted");

    uint8_t bad_ver[] = { 'D', 'S', 99, MSG_PING, 0, 0 };
    framereader_init(&fr);
    framereader_feed(&fr, bad_ver, sizeof(bad_ver));
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_ERR_VERSION, "bad version accepted");

    uint8_t bad_type[] = { 'D', 'S', 1, 0x7A, 0, 0 };
    framereader_init(&fr);
    framereader_feed(&fr, bad_type, sizeof(bad_type));
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_ERR_TYPE, "unknown type accepted");

    // 0xFFFF declared. Must be refused from the header alone, before the body
    // is waited for -- otherwise six bytes pin the entire receive buffer.
    uint8_t huge[] = { 'D', 'S', 1, MSG_FRAME_DIFF, 0xFF, 0xFF };
    framereader_init(&fr);
    framereader_feed(&fr, huge, sizeof(huge));
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_ERR_LENGTH, "oversize length accepted");
}

// ------------------------------------------------------------------ grid ---

static void test_full_frame(void)
{
    grid_t g;
    grid_clear(&g);

    uint8_t payload[PROTO_CELLS * PROTO_CELL_SIZE];
    for (int i = 0; i < PROTO_CELLS; i++) {
        payload[i * 3] = (uint8_t)('A' + (i % 26));
        payload[i * 3 + 1] = 0x21;
        payload[i * 3 + 2] = 0;
    }
    CHECK(proto_apply_full(&g, payload, sizeof(payload)) == PROTO_OK, "full frame rejected");
    CHECK(g.cells[0].glyph == 'A', "first cell wrong");
    CHECK(CELL_FG(g.cells[0]) == 2 && CELL_BG(g.cells[0]) == 1, "attr unpack wrong");

    CHECK(proto_apply_full(&g, payload, sizeof(payload) - 1) == PROTO_ERR_PAYLOAD,
          "truncated full frame accepted");
}

static void test_diff_bounds(void)
{
    grid_t g;
    grid_clear(&g);

    uint8_t ok_entry[] = { PROTO_ROWS - 1, PROTO_COLS - 1, 'Z', 0x30, 0 };
    CHECK(proto_apply_diff(&g, ok_entry, sizeof(ok_entry)) == PROTO_OK, "boundary cell rejected");
    CHECK(g.cells[PROTO_CELLS - 1].glyph == 'Z', "boundary cell not written");

    // Writing row 200 into a 24-row array is the exact bug this guards.
    uint8_t bad_row[] = { 200, 0, 'x', 0, 0 };
    CHECK(proto_apply_diff(&g, bad_row, sizeof(bad_row)) == PROTO_ERR_BOUNDS, "row 200 accepted");

    uint8_t bad_col[] = { 0, 200, 'x', 0, 0 };
    CHECK(proto_apply_diff(&g, bad_col, sizeof(bad_col)) == PROTO_ERR_BOUNDS, "col 200 accepted");

    uint8_t ragged[] = { 0, 1, 2 };
    CHECK(proto_apply_diff(&g, ragged, sizeof(ragged)) == PROTO_ERR_PAYLOAD, "ragged diff accepted");

    CHECK(proto_apply_diff(&g, NULL, 0) == PROTO_OK, "empty diff should be a no-op");
}

static void test_diff_validates_before_writing(void)
{
    // A diff whose last entry is out of bounds must leave the screen untouched,
    // not half-applied -- a partially updated grid never resynchronises.
    grid_t g;
    grid_clear(&g);

    uint8_t entries[] = {
        0, 0, 'G', 0, 0,
        200, 0, 'B', 0, 0,
    };
    CHECK(proto_apply_diff(&g, entries, sizeof(entries)) == PROTO_ERR_BOUNDS, "expected bounds error");
    CHECK(g.cells[0].glyph == ' ', "first entry was applied despite a later bad entry");
}

static void test_scroll(void)
{
    grid_t g;
    grid_clear(&g);
    for (int r = 0; r < PROTO_ROWS; r++) g.cells[r * PROTO_COLS].glyph = (uint8_t)('a' + r);

    uint8_t payload[] = { 0, PROTO_ROWS - 1, 3 };
    int lines = 0;
    CHECK(proto_apply_scroll(&g, payload, sizeof(payload), &lines) == PROTO_OK, "scroll rejected");
    CHECK(lines == 3, "wrong line count %d", lines);
    CHECK(g.cells[0].glyph == 'd', "row 0 should hold old row 3, got %c", g.cells[0].glyph);
    CHECK(g.cells[(PROTO_ROWS - 1) * PROTO_COLS].glyph == ' ', "vacated row not blanked");

    uint8_t zero[] = { 0, PROTO_ROWS - 1, 0 };
    CHECK(proto_apply_scroll(&g, zero, sizeof(zero), NULL) == PROTO_ERR_PAYLOAD, "zero scroll accepted");

    uint8_t too_far[] = { 0, PROTO_ROWS - 1, PROTO_ROWS };
    CHECK(proto_apply_scroll(&g, too_far, sizeof(too_far), NULL) == PROTO_ERR_BOUNDS,
          "scroll past screen accepted");

    uint8_t short_payload[] = { 0, 1 };
    CHECK(proto_apply_scroll(&g, short_payload, sizeof(short_payload), NULL) == PROTO_ERR_PAYLOAD,
          "short scroll payload accepted");
}

// ----------------------------------------------------------------- cursor ---

static void test_cursor_apply(void)
{
    cursor_t cur;
    uint8_t ok[] = { 5, 10, 1 };
    CHECK(proto_apply_cursor(&cur, ok, sizeof(ok)) == PROTO_OK, "valid cursor rejected");
    CHECK(cur.row == 5 && cur.col == 10 && cur.visible == 1, "cursor decoded wrong");

    uint8_t hidden[] = { PROTO_ROWS - 1, PROTO_COLS - 1, 0 };
    CHECK(proto_apply_cursor(&cur, hidden, sizeof(hidden)) == PROTO_OK, "hidden cursor rejected");
    CHECK(cur.visible == 0, "hidden cursor reported visible");

    uint8_t bad_row[] = { PROTO_ROWS, 0, 1 };
    CHECK(proto_apply_cursor(&cur, bad_row, sizeof(bad_row)) == PROTO_ERR_BOUNDS, "row out of range accepted");

    uint8_t bad_col[] = { 0, PROTO_COLS, 1 };
    CHECK(proto_apply_cursor(&cur, bad_col, sizeof(bad_col)) == PROTO_ERR_BOUNDS, "col out of range accepted");

    uint8_t short_payload[] = { 1, 2 };
    CHECK(proto_apply_cursor(&cur, short_payload, sizeof(short_payload)) == PROTO_ERR_PAYLOAD,
          "short cursor payload accepted");
}

static void test_cursor_frame_accepted_by_reader(void)
{
    // The client halts the connection on an unknown type, so a CURSOR the
    // decoder does not recognise would hang the console on the first caret
    // update. The reader must accept the frame and hand it through.
    uint8_t raw[16];
    uint8_t payload[] = { 3, 7, 1 };
    const size_t n = proto_encode(raw, MSG_CURSOR, payload, sizeof(payload));

    framereader_t fr;
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, n) == PROTO_OK, "feed failed");

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    CHECK(framereader_next(&fr, &t, &p, &len) == PROTO_OK, "CURSOR frame rejected");
    CHECK(t == MSG_CURSOR, "wrong type %d", t);
    CHECK(len == 3 && memcmp(p, payload, 3) == 0, "CURSOR payload mangled");
}

static void test_view_pos_apply(void)
{
    view_pos_t pos = { 0, 0 };
    const uint8_t ok[] = { 0x00, 0x0c, 0x00, 0x28 };
    CHECK(proto_apply_view_pos(&pos, ok, sizeof(ok)) == PROTO_OK,
          "valid VIEW_POS rejected");
    CHECK(pos.lines_back == 12 && pos.available == 40,
          "VIEW_POS decoded as %u of %u",
          (unsigned)pos.lines_back, (unsigned)pos.available);

    const uint8_t max[] = { 0xff, 0xff, 0xff, 0xff };
    CHECK(proto_apply_view_pos(&pos, max, sizeof(max)) == PROTO_OK,
          "maximum VIEW_POS rejected");
    CHECK(pos.lines_back == 0xffff && pos.available == 0xffff,
          "maximum VIEW_POS decoded wrong");

    const uint8_t short_payload[] = { 0, 12, 0 };
    CHECK(proto_apply_view_pos(&pos, short_payload, sizeof(short_payload)) == PROTO_ERR_PAYLOAD,
          "short VIEW_POS accepted");
    const uint8_t long_payload[] = { 0, 12, 0, 40, 0 };
    CHECK(proto_apply_view_pos(&pos, long_payload, sizeof(long_payload)) == PROTO_ERR_PAYLOAD,
          "long VIEW_POS accepted");
}

static void test_view_pos_frame_accepted_by_reader(void)
{
    // Regression: sending VIEW opts the client into VIEW_POS. If the reader
    // does not recognise the response, main.c halts forever on the first scroll.
    uint8_t raw[16];
    const uint8_t payload[] = { 0, 12, 0, 40 };
    const size_t n = proto_encode(raw, MSG_VIEW_POS, payload, sizeof(payload));

    framereader_t fr;
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, n) == PROTO_OK, "feed failed");

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    const proto_result_t r = framereader_next(&fr, &t, &p, &len);
    CHECK(r == PROTO_OK, "VIEW_POS frame rejected: %s", proto_strerror(r));
    if (r != PROTO_OK) return;  // avoid checks against uninitialised outputs
    CHECK(t == MSG_VIEW_POS, "wrong type %d", t);
    CHECK(len == sizeof(payload) && memcmp(p, payload, sizeof(payload)) == 0,
          "VIEW_POS payload mangled");

    // Keep the C reader's known-type table in parity with msg_type_t. VIEW is
    // outbound on the DS, but it is still a defined protocol frame.
    const uint8_t view_payload[] = { 0xff, 0xf4 };  // -12
    const size_t view_n = proto_encode(raw, MSG_VIEW, view_payload, sizeof(view_payload));
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, view_n) == PROTO_OK, "VIEW feed failed");
    const proto_result_t view_r = framereader_next(&fr, &t, &p, &len);
    CHECK(view_r == PROTO_OK, "VIEW frame rejected: %s", proto_strerror(view_r));
    if (view_r != PROTO_OK) return;
    CHECK(t == MSG_VIEW, "wrong VIEW type %d", t);
    CHECK(len == sizeof(view_payload) && memcmp(p, view_payload, sizeof(view_payload)) == 0,
          "VIEW payload mangled");
}

// ---------------------------------------------------------------- config ---

static const char *GOOD_CFG =
    "# dsterm config\n"
    "ssid = Example Network\n"
    "wifi_key = example-passphrase\n"
    "ip = 192.0.2.50\n"
    "gateway = 192.0.2.1\n"
    "server = 192.0.2.10\n"
    "port = 8347\n"
    "psk = 000102030405060708090a0b0c0d0e0f\n";

static void test_config_valid(void)
{
    dsterm_config_t c;
    char err[128] = "";
    CHECK(config_parse(GOOD_CFG, &c, err, sizeof(err)) == 1, "rejected: %s", err);
    // SSIDs with spaces are common; trimming one would break association in a
    // way that looks like a wrong password.
    CHECK(strcmp(c.ssid, "Example Network") == 0, "ssid mangled: '%s'", c.ssid);
    CHECK(strcmp(c.wifi_key, "example-passphrase") == 0, "wifi_key wrong: '%s'", c.wifi_key);
    CHECK(strcmp(c.server, "192.0.2.10") == 0, "server wrong");
    CHECK(c.port == 8347, "port wrong: %u", c.port);
    CHECK(c.psk[0] == 0x00 && c.psk[15] == 0x0f, "psk bytes wrong");
    CHECK(strcmp(c.netmask, "255.255.255.0") == 0, "netmask default not applied");
}

static void test_config_psk_grouped(void)
{
    // The server prints the key in groups of four so it can be read off a
    // screen; the config must accept it in that form.
    char text[512];
    snprintf(text, sizeof(text),
             "ssid=n\nwifi_key=k\nip=1.2.3.4\ngateway=1.2.3.1\nserver=1.2.3.9\n"
             "psk=0001 0203 0405 0607 0809 0a0b 0c0d 0e0f\n");
    dsterm_config_t c;
    char err[128] = "";
    CHECK(config_parse(text, &c, err, sizeof(err)) == 1, "grouped psk rejected: %s", err);
    CHECK(c.psk[1] == 0x01 && c.psk[15] == 0x0f, "grouped psk decoded wrong");
}

static void test_config_missing_fields(void)
{
    struct { const char *key; const char *want; } cases[] = {
        { "ssid",     "missing ssid" },
        { "wifi_key", "missing wifi_key" },
        { "server",   "missing server" },
        { "psk",      "missing psk" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        // Rebuild the good config without one line.
        char text[512] = "";
        const char *p = GOOD_CFG;
        while (*p) {
            const char *nl = strchr(p, '\n');
            const size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
            if (strncmp(p, cases[i].key, strlen(cases[i].key)) != 0) {
                strncat(text, p, len);
            }
            p = nl ? nl + 1 : p + strlen(p);
        }
        dsterm_config_t c;
        char err[128] = "";
        CHECK(config_parse(text, &c, err, sizeof(err)) == 0,
              "accepted config missing %s", cases[i].key);
        // The message lands on a 42-column screen, so it must name the field.
        CHECK(strstr(err, cases[i].want) != NULL,
              "unhelpful error for missing %s: '%s'", cases[i].key, err);
    }
}

static void test_config_bad_psk(void)
{
    dsterm_config_t c;
    char err[128] = "";
    const char *base = "ssid=n\nwifi_key=k\nip=1.2.3.4\ngateway=1.2.3.1\nserver=1.2.3.9\n";

    char text[512];
    snprintf(text, sizeof(text), "%spsk=00010203\n", base);
    CHECK(config_parse(text, &c, err, sizeof(err)) == 0, "short psk accepted");
    CHECK(strstr(err, "32 hex") != NULL, "unhelpful short-psk error: '%s'", err);

    snprintf(text, sizeof(text), "%spsk=zz0102030405060708090a0b0c0d0e0f\n", base);
    CHECK(config_parse(text, &c, err, sizeof(err)) == 0, "non-hex psk accepted");
}

static void test_config_robustness(void)
{
    dsterm_config_t c;
    char err[128] = "";

    // Unknown keys must not break an older build reading a newer config.
    char text[640];
    snprintf(text, sizeof(text), "%sfuture_option = 42\n", GOOD_CFG);
    CHECK(config_parse(text, &c, err, sizeof(err)) == 1, "unknown key rejected: %s", err);

    // A line without '=' is a typo worth reporting, with its line number.
    snprintf(text, sizeof(text), "%sgarbage line\n", GOOD_CFG);
    CHECK(config_parse(text, &c, err, sizeof(err)) == 0, "malformed line accepted");
    CHECK(strstr(err, "line") != NULL, "error should cite the line: '%s'", err);

    // Over-long SSID must be refused rather than truncated into a wrong network.
    char longssid[600];
    snprintf(longssid, sizeof(longssid), "ssid=");
    for (int i = 0; i < 40; i++) strcat(longssid, "abcdefghij");
    strcat(longssid, "\n");
    char text2[1400];
    snprintf(text2, sizeof(text2), "%s%s", longssid, GOOD_CFG);
    CHECK(config_parse(text2, &c, err, sizeof(err)) == 0, "over-long ssid accepted");

    CHECK(config_parse("", &c, err, sizeof(err)) == 0, "empty config accepted");
}

// ------------------------------------------------------------------ log ----

static void test_log_ring(void)
{
    log_init();
    CHECK(log_count() == 0, "log not empty after init");
    CHECK(strcmp(log_get(0), "") == 0, "empty-log get should be ''");

    log_printf("hello");
    CHECK(log_count() == 1, "count wrong after one line");
    CHECK(strcmp(log_get(0), "hello") == 0, "line wrong: '%s'", log_get(0));

    // Fill past capacity: the 24 most recent lines survive, "hello" drops.
    for (int i = 0; i < LOG_ROWS; i++) log_printf("line %d", i);
    CHECK(log_count() == LOG_ROWS, "count should cap at LOG_ROWS");
    CHECK(strcmp(log_get(0), "line 0") == 0, "oldest line wrong: '%s'", log_get(0));
    CHECK(strcmp(log_get(LOG_ROWS - 1), "line 23") == 0, "newest line wrong: '%s'",
          log_get(LOG_ROWS - 1));
    CHECK(strcmp(log_get(LOG_ROWS), "") == 0, "out-of-range get should be ''");
}

static void test_log_truncation(void)
{
    log_init();
    char long_line[LOG_COLS + 32];
    memset(long_line, 'x', sizeof(long_line) - 1);
    long_line[sizeof(long_line) - 1] = 0;
    log_printf("%s", long_line);

    CHECK(log_count() == 1, "truncated line should still count");
    CHECK(strlen(log_get(0)) == LOG_COLS, "line not truncated to LOG_COLS, len=%zu",
          strlen(log_get(0)));
    CHECK(log_get(0)[LOG_COLS] == '\0', "line not NUL-terminated");
}

static void test_log_vprintf_forwards(void)
{
    log_init();
    log_printf("a %s %d", "x", 7);
    CHECK(strcmp(log_get(0), "a x 7") == 0, "format mangled: '%s'", log_get(0));
}

static void test_log_blank(void)
{
    log_init();
    log_blank();
    CHECK(log_count() == 1, "blank line should count");
    CHECK(strlen(log_get(0)) == 0, "blank line should be empty");

    log_printf("after");
    CHECK(log_count() == 2, "count wrong after blank+line");
    CHECK(strcmp(log_get(0), "") == 0, "blank line not at 0");
    CHECK(strcmp(log_get(1), "after") == 0, "text line not at 1");
}

// --------------------------------------------------------------- status ----

static void test_mode_apply(void)
{
    uint8_t flags = 0xFF;
    CHECK(proto_apply_mode(&flags, (const uint8_t *)"\x01", 1) == PROTO_OK,
          "valid MODE rejected");
    CHECK(flags == PROTO_MODE_APP_SCROLLS, "APP_SCROLLS bit not decoded: %u", flags);

    CHECK(proto_apply_mode(&flags, (const uint8_t *)"\x00", 1) == PROTO_OK,
          "clear MODE rejected");
    CHECK(flags == 0, "clear MODE decoded with a bit set: %u", flags);

    // Reserved bits must be masked off, never carried into the caller and
    // never rejected -- a future flag on the wire must not halt today's card.
    CHECK(proto_apply_mode(&flags, (const uint8_t *)"\xFE", 1) == PROTO_OK,
          "reserved bits rejected");
    CHECK(flags == 0, "reserved bits leaked: %u", flags);
    CHECK(proto_apply_mode(&flags, (const uint8_t *)"\xFF", 1) == PROTO_OK,
          "all-bits MODE rejected");
    CHECK(flags == PROTO_MODE_APP_SCROLLS, "all-bits MODE masked wrong: %u", flags);

    CHECK(proto_apply_mode(&flags, (const uint8_t *)"\x01\x00", 2) == PROTO_ERR_PAYLOAD,
          "two-byte MODE accepted");
    CHECK(proto_apply_mode(&flags, (const uint8_t *)"", 0) == PROTO_ERR_PAYLOAD,
          "empty MODE accepted");
}

static void test_mode_frame_accepted_by_reader(void)
{
    // Regression in the same family as VIEW_POS: the server sends MODE to any
    // card that announced "mode" in its HELLO, so an unknown type here halts
    // the card on the first regime announcement. It must reach the application.
    const uint8_t raw[] = { 'D', 'S', 1, MSG_MODE, 0x00, 0x01, 0x01 };
    framereader_t fr;
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, sizeof(raw)) == PROTO_OK, "MODE feed failed");

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    const proto_result_t r = framereader_next(&fr, &t, &p, &len);
    CHECK(r == PROTO_OK, "MODE frame rejected: %s", proto_strerror(r));
    if (r != PROTO_OK) return;
    CHECK(t == MSG_MODE, "wrong type %d", t);
    CHECK(len == 1 && p[0] == 0x01, "MODE payload mangled");
}

// The scroll at which a settings row is on screen: its tab scrolled just far
// enough to show it at the bottom, or not at all if it is among the first.
static unsigned settings_test_scroll(settings_row_t row)
{
    unsigned index = 0;
    for (int i = 0; i < (int)row; i++) {
        if (((settings_row_t)i == SETTINGS_ROW_SHUTTER ||
             (settings_row_t)i == SETTINGS_ROW_PICTURE ||
             (settings_row_t)i == SETTINGS_ROW_LIVEVIEW) &&
            !settings_camera_available()) continue;   // an absent row takes no space
        if (settings_row_tab((settings_row_t)i) == settings_row_tab(row)) index++;
    }
    return index >= SETTINGS_VISIBLE_ROWS ? index - SETTINGS_VISIBLE_ROWS + 1u : 0u;
}

static void test_buttons_and_their_tab(void)
{
    // The map: defaults, the bytes each action sends, and Enter can never be
    // mapped away.
    button_map_t m;
    button_map_defaults(&m);
    size_t len = 0;
    CHECK(m.action[BUTTON_A] == BUTTON_ENTER && m.action[BUTTON_B] == BUTTON_DELETE &&
              m.action[BUTTON_X] == BUTTON_ESC && m.action[BUTTON_Y] == BUTTON_TAB &&
              m.action[BUTTON_START] == BUTTON_STOP,
          "the default buttons are not A Enter, B Delete, X Esc, Y Tab, START Stop");
    CHECK(memcmp(button_action_bytes(BUTTON_ENTER, &len), "\r", 1) == 0 && len == 1 &&
              memcmp(button_action_bytes(BUTTON_DELETE, &len), "\x7f", 1) == 0 &&
              memcmp(button_action_bytes(BUTTON_STOP, &len), "\x03", 1) == 0 &&
              memcmp(button_action_bytes(BUTTON_SPACE, &len), " ", 1) == 0,
          "an action sends the wrong bytes");
    CHECK(!button_map_set(&m, BUTTON_A, BUTTON_SPACE) && m.action[BUTTON_A] == BUTTON_ENTER,
          "Enter was mapped off its only button");
    CHECK(button_map_set(&m, BUTTON_B, BUTTON_ENTER) &&
              button_map_set(&m, BUTTON_A, BUTTON_SPACE) &&
              button_for(&m, BUTTON_ENTER) == BUTTON_B,
          "Enter could not move once another button had it");
    char legend[48];
    button_map_legend(&m, legend, sizeof(legend));
    CHECK(strcmp(legend, "A space  B enter  X esc  Y tab") == 0, "legend: '%s'", legend);
    CHECK(strlen(legend) <= 42, "the legend does not fit a line");

    // The tab: button rows set the map, refuse to drop Enter, and DEFAULTS
    // puts the buttons (and every other tab) back behind a confirm.
    settings_t st;
    settings_ui_t ui;
    settings_defaults(&st);
    settings_ui_init(&ui);
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_BUTTON_Y, BUTTON_SPACE, NULL) ==
              SETTINGS_PRESS_CHANGED && st.buttons.action[BUTTON_Y] == BUTTON_SPACE,
          "Y could not be set to Space");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_BUTTON_A, BUTTON_TAB, NULL) ==
              SETTINGS_PRESS_NOTHING && ui.refused &&
              st.buttons.action[BUTTON_A] == BUTTON_ENTER,
          "taking Enter off A was not refused and said");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_NOTHING && ui.reset_armed &&
              st.buttons.action[BUTTON_Y] == BUTTON_SPACE,
          "DEFAULTS fired on the first press");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_RESET && st.buttons.action[BUTTON_Y] == BUTTON_TAB,
          "DEFAULTS did not put the buttons back");

    // The file: the map round-trips, and a map with no Enter is refused.
    char err[64];
    settings_t back;
    st.buttons.action[BUTTON_START] = BUTTON_ESC;
    st.buttons.action[BUTTON_X] = BUTTON_STOP;
    static const char path[] = "/tmp/dsterm-buttons-settings.cfg";
    CHECK(settings_save(path, &st, err, sizeof(err)) &&
              settings_load(path, &back, err, sizeof(err)) &&
              back.buttons.action[BUTTON_START] == BUTTON_ESC &&
              back.buttons.action[BUTTON_X] == BUTTON_STOP,
          "the button map did not survive a save: %s", err);
    remove(path);
    // No button left on Enter: the buttons go back to their defaults, and
    // only the buttons.
    CHECK(settings_parse("version=4\nbutton_a=space\ntheme=sea\n", &back, err,
                         sizeof(err)) &&
              back.buttons.action[BUTTON_A] == BUTTON_ENTER &&
              back.theme == SETTINGS_THEME_SEA,
          "a file with no button on Enter cost more than the buttons: %s", err);
    CHECK(settings_parse("version=4\nBUTTON_A=enter\nbutton_b=space\n", &back, err,
                         sizeof(err)) &&
              back.buttons.action[BUTTON_B] == BUTTON_SPACE,
          "a button key in capitals was not read: %s", err);

    // Every tab fits on one screen without scrolling: walking down BUTTONS
    // keeps every row in view with scroll == 0 and wraps back to the top.
    settings_ui_init(&ui);
    settings_ui_tab(&st, &ui, SETTINGS_TAB_BUTTONS);
    for (unsigned t = 0; t < SETTINGS_TAB_COUNT; t++) {
        CHECK(settings_tab_rows(t) <= SETTINGS_VISIBLE_ROWS,
              "tab %u has %u rows and needs scrolling", t, settings_tab_rows(t));
    }
    CHECK(ui.scroll == 0 && !settings_more_above(0) &&
              !settings_more_below(SETTINGS_TAB_BUTTONS, 0),
          "the arrows are wrong at the top of BUTTONS");
    for (unsigned i = 0; i + 1 < settings_tab_rows(SETTINGS_TAB_BUTTONS); i++) {
        settings_ui_move(&st, &ui, 0, 1);
        int y = 0;
        CHECK(settings_row_rect((settings_row_t)ui.row, ui.scroll, NULL, &y, NULL, NULL),
              "the highlight (row %u) scrolled out of view at scroll %u", ui.row, ui.scroll);
    }
    CHECK(ui.row == SETTINGS_ROW_DEFAULTS && ui.scroll == 0 &&
              !settings_more_below(SETTINGS_TAB_BUTTONS, ui.scroll),
          "the bottom of BUTTONS is not DEFAULTS");
    CHECK(settings_scroll_hit(SETTINGS_TAB_BUTTONS, 1, 250, SETTINGS_ROW_Y + 4) == -1 &&
              settings_scroll_hit(SETTINGS_TAB_BUTTONS, 1, 100, SETTINGS_ROW_Y + 4) == 0,
          "the up arrow is not where it is drawn");
    settings_ui_move(&st, &ui, 0, 1);
    CHECK(ui.row == SETTINGS_ROW_BUTTON_A && ui.scroll == 0,
          "wrapping past the bottom did not go back to the top");
}

static void test_shutter_setting(void)
{
    settings_t st;
    settings_ui_t ui;
    char err[80];

    // The shutter itself: names, words, and the key-word masks.
    CHECK(!strcmp(camera_shutter_name(SHUTTER_R), "R") &&
              !strcmp(camera_shutter_name(SHUTTER_L), "L") &&
              !strcmp(camera_shutter_name(SHUTTER_A), "A"),
          "shutter names are wrong");
    CHECK(!strcmp(camera_shutter_word(SHUTTER_R), "r") &&
              !strcmp(camera_shutter_word(SHUTTER_L), "l") &&
              !strcmp(camera_shutter_word(SHUTTER_A), "a"),
          "shutter file words are wrong");
    CHECK(camera_shutter_key_mask(SHUTTER_R) == (1u << 8) &&
              camera_shutter_key_mask(SHUTTER_L) == (1u << 9) &&
              camera_shutter_key_mask(SHUTTER_A) == 1u,
          "shutter masks are not the key word's R/L/A bits");
    CHECK(camera_shutter_other_mask(SHUTTER_R) == (1u << 9) &&
              camera_shutter_other_mask(SHUTTER_L) == (1u << 8) &&
              camera_shutter_other_mask(SHUTTER_A) == 0,
          "the spoiling shoulders are wrong");
    CHECK(camera_shutter_name(SHUTTER_COUNT)[0] == 'R',
          "an out-of-range shutter did not answer like R");

    // A card with no camera: the row does not exist.  The flag defaults off.
    settings_set_camera_available(0);
    settings_defaults(&st);
    settings_ui_init(&ui);
    // A, B, X, Y, START, CONTROLS, DEFAULTS.
    CHECK(settings_tab_rows(SETTINGS_TAB_BUTTONS) == 7,
          "a card with no camera grew a shutter row");
    int x = 0, y = 0, w = 0, h = 0;
    CHECK(!settings_row_rect(SETTINGS_ROW_SHUTTER, 0, &x, &y, &w, &h),
          "an absent row still had a rect");
    CHECK(settings_row_hit(SETTINGS_TAB_BUTTONS, 0, 100,
                           SETTINGS_ROW_Y + 5 * SETTINGS_ROW_PITCH) ==
              SETTINGS_ROW_CONTROLS,
          "a touch where SHUTTER would be hit it anyway");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_SHUTTER, 1, NULL) ==
              SETTINGS_PRESS_NOTHING,
          "an absent row still took a press");
    // Navigation skips it: walking down off START lands on CONTROLS.
    settings_ui_tab(&st, &ui, SETTINGS_TAB_BUTTONS);
    ui.row = SETTINGS_ROW_BUTTON_START;
    settings_ui_move(&st, &ui, 0, 1);
    CHECK(ui.row == SETTINGS_ROW_CONTROLS,
          "the absent shutter row was still in the walk");

    // A card with one (a DSi): the row exists, with three chips on screen.
    settings_set_camera_available(1);
    CHECK(settings_camera_available(), "the flag did not set");
    CHECK(settings_tab_rows(SETTINGS_TAB_BUTTONS) == 8,
          "BUTTONS did not grow the shutter row");
    CHECK(settings_row_rect(SETTINGS_ROW_SHUTTER, 0, &x, &y, &w, &h) &&
              x >= 0 && x + w <= 256 && y + h <= 160,
          "the shutter row is off the screen");
    int right = 0;
    for (unsigned o = 0; o < 3; o++) {
        int cx, cy, cw, ch;
        CHECK(settings_option_rect(SETTINGS_ROW_SHUTTER, o, 0,
                                   &cx, &cy, &cw, &ch) && cx + cw <= x + w,
              "shutter chip %u runs off its row", o);
        if (o == 2) right = cx + cw;
    }
    CHECK(right <= 256, "the shutter chips run off the screen at %d", right);
    CHECK(settings_row_hit(SETTINGS_TAB_BUTTONS, 0, 100,
                           y + h / 2) == SETTINGS_ROW_SHUTTER,
          "a touch on the shutter row hit something else");

    // The value, its press, and DEFAULTS that must restore R.
    CHECK(settings_option_current(&st, SETTINGS_ROW_SHUTTER) == 0,
          "the default shutter is not R");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_SHUTTER, 1, NULL) ==
              SETTINGS_PRESS_CHANGED && st.camera_shutter == SHUTTER_L,
          "the shutter could not be set to L");
    settings_press(&st, &ui, SETTINGS_ROW_SHUTTER, 2, NULL);
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_NOTHING &&
              settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_RESET && st.camera_shutter == SHUTTER_R,
          "DEFAULTS did not put the shutter back to R");

    // The file: always written, and a bad value refuses like the rest.
    settings_t back;
    CHECK(settings_parse("version=4\nbutton_b=space\n", &back, err, sizeof(err)) &&
              back.camera_shutter == SHUTTER_R,
          "a version 4 file did not load shutter's default: %s", err);
    CHECK(settings_parse("version=4\nshutter=l\n", &back, err, sizeof(err)) &&
              back.camera_shutter == SHUTTER_L,
          "shutter=l was not read: %s", err);
    CHECK(settings_parse("version=4\nshutter=x\n", &back, err, sizeof(err)) &&
              back.camera_shutter == SHUTTER_R,
          "shutter=x was not its default: %s", err);
    CHECK(!settings_parse("version=5\nshutter=l\n", &back, err, sizeof(err)),
          "a version 5 file was accepted");
    st.camera_shutter = SHUTTER_A;
    static const char path[] = "/tmp/dsterm-shutter-settings.cfg";
    CHECK(settings_save(path, &st, err, sizeof(err)) &&
              settings_load(path, &back, err, sizeof(err)) &&
              back.camera_shutter == SHUTTER_A,
          "the shutter did not survive a save: %s", err);
    // The save writes version 4: a new key joins it rather than bumping the
    // version, which an older card would take as a reason to reset all of it.
    {
        char text[SETTINGS_FILE_MAX + 1];
        FILE *fh = fopen(path, "rb");
        size_t got = fh ? fread(text, 1, sizeof(text) - 1, fh) : 0;
        if (fh) fclose(fh);
        text[got] = '\0';
        CHECK(fh && strstr(text, "version=4\n") && !strstr(text, "version=5"),
              "the file did not say version=4");
    }
    remove(path);

    // With the row present BUTTONS has eight rows and still fits without
    // scrolling.
    settings_ui_init(&ui);
    settings_ui_tab(&st, &ui, SETTINGS_TAB_BUTTONS);
    for (unsigned i = 0; i + 1 < settings_tab_rows(SETTINGS_TAB_BUTTONS); i++) {
        settings_ui_move(&st, &ui, 0, 1);
        int ry = 0;
        CHECK(settings_row_rect((settings_row_t)ui.row, ui.scroll,
                                NULL, &ry, NULL, NULL),
              "row %u scrolled out of view at scroll %u", ui.row, ui.scroll);
    }
    CHECK(ui.row == SETTINGS_ROW_DEFAULTS && ui.scroll == 0,
          "the bottom of BUTTONS is not DEFAULTS");
    settings_ui_move(&st, &ui, 0, 1);
    CHECK(ui.row == SETTINGS_ROW_BUTTON_A && ui.scroll == 0,
          "wrapping BUTTONS did not go back to the top");

    // Put the flag back: the rest of the suite runs on a card with no camera.
    settings_set_camera_available(0);
}

static void test_picture_setting(void)
{
    settings_t st;
    settings_ui_t ui;
    char err[80];
    int x = 0, y = 0, w = 0, h = 0;

    // A card with no camera: the row does not exist.
    settings_set_camera_available(0);
    settings_defaults(&st);
    settings_ui_init(&ui);
    CHECK(settings_tab_rows(SETTINGS_TAB_LINK) == 3,
          "a card with no camera grew a picture row");
    CHECK(!settings_row_rect(SETTINGS_ROW_PICTURE, 0, &x, &y, &w, &h),
          "an absent row still had a rect");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_PICTURE, 1, NULL) ==
              SETTINGS_PRESS_NOTHING,
          "an absent row still took a press");

    // A card with one: the row exists, FULL is the default, SMALL sticks.
    settings_set_camera_available(1);
    CHECK(settings_tab_rows(SETTINGS_TAB_LINK) == 5,
          "LINK did not grow the picture and live-view rows");
    CHECK(settings_row_tab(SETTINGS_ROW_PICTURE) == SETTINGS_TAB_LINK,
          "the picture row is not on LINK");
    const unsigned pscroll = settings_test_scroll(SETTINGS_ROW_PICTURE);
    CHECK(settings_row_rect(SETTINGS_ROW_PICTURE, pscroll, &x, &y, &w, &h) &&
              x >= 0 && x + w <= 256 && y + h <= 160,
          "the picture row is off the screen");
    CHECK(!strcmp(settings_option_label(SETTINGS_ROW_PICTURE, 0), "FULL") &&
              !strcmp(settings_option_label(SETTINGS_ROW_PICTURE, 1), "SMALL"),
          "the picture chips are wrong");
    CHECK(settings_option_current(&st, SETTINGS_ROW_PICTURE) ==
              SETTINGS_PICTURE_FULL,
          "the default picture is not FULL");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_PICTURE, 1, NULL) ==
              SETTINGS_PRESS_CHANGED && st.picture == SETTINGS_PICTURE_SMALL,
          "the picture could not be set to SMALL");
    // The same value again is a no-op, and DEFAULTS restores FULL.
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_PICTURE, 1, NULL) ==
              SETTINGS_PRESS_NOTHING,
          "pressing the picture already set changed something");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_NOTHING &&
              settings_press(&st, &ui, SETTINGS_ROW_DEFAULTS, 0, NULL) ==
              SETTINGS_PRESS_RESET && st.picture == SETTINGS_PICTURE_FULL,
          "DEFAULTS did not put the picture back to FULL");

    // LIVE VIEW: camera cards only, GREY by default, COLOUR sticks, the file
    // carries it, and DEFAULTS puts GREY back.
    CHECK(settings_option_current(&st, SETTINGS_ROW_LIVEVIEW) ==
              SETTINGS_LIVEVIEW_GREY &&
              !strcmp(settings_option_label(SETTINGS_ROW_LIVEVIEW, 1), "COLOUR"),
          "LIVE VIEW does not start on GREY");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_LIVEVIEW, 1, NULL) ==
              SETTINGS_PRESS_CHANGED && st.liveview == SETTINGS_LIVEVIEW_COLOUR,
          "LIVE VIEW could not be set to COLOUR");
    {
        settings_t lv;
        CHECK(settings_parse("version=4\nliveview=colour\n", &lv, err, sizeof(err)) &&
                  lv.liveview == SETTINGS_LIVEVIEW_COLOUR,
              "liveview=colour was not read: %s", err);
        CHECK(settings_parse("version=4\nsound=chip\n", &lv, err, sizeof(err)) &&
                  lv.liveview == SETTINGS_LIVEVIEW_GREY,
              "a file without liveview= did not get GREY: %s", err);
        CHECK(settings_parse("version=4\nliveview=sepia\n", &lv, err, sizeof(err)) &&
                  lv.liveview == SETTINGS_LIVEVIEW_GREY,
              "liveview=sepia was not its default: %s", err);
    }
    settings_set_camera_available(0);
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_LIVEVIEW, 0, NULL) ==
              SETTINGS_PRESS_NOTHING,
          "a card with no camera took a LIVE VIEW press");
    settings_set_camera_available(1);
    st.liveview = SETTINGS_LIVEVIEW_GREY;

    // The file: always written, refused on a bad value, and a version 4 file
    // without it gets FULL.
    settings_t back;
    CHECK(settings_parse("version=4\nbutton_b=space\n", &back, err, sizeof(err)) &&
              back.picture == SETTINGS_PICTURE_FULL,
          "a version 4 file did not load picture's default: %s", err);
    CHECK(settings_parse("version=4\npicture=small\n", &back, err, sizeof(err)) &&
              back.picture == SETTINGS_PICTURE_SMALL,
          "picture=small was not read: %s", err);
    CHECK(settings_parse("version=4\npicture=x\n", &back, err, sizeof(err)) &&
              back.picture == SETTINGS_PICTURE_FULL,
          "picture=x was not its default: %s", err);
    st.picture = SETTINGS_PICTURE_SMALL;
    static const char path[] = "/tmp/dsterm-picture-settings.cfg";
    CHECK(settings_save(path, &st, err, sizeof(err)) &&
              settings_load(path, &back, err, sizeof(err)) &&
              back.picture == SETTINGS_PICTURE_SMALL,
          "the picture size did not survive a save: %s", err);
    remove(path);

    settings_set_camera_available(0);
}

static void test_settings_page(void)
{
    // Five rows of pickable chips and a file.  What is worth asserting here is
    // what the last version of this page got wrong: every chip must be
    // reachable and pressable at the place it is drawn, a chip that types must
    // say what it types, the destructive one must not fire on one press, and
    // the file has to survive a round trip and refuse a lie.
    settings_t settings;
    settings_ui_t ui;
    settings_defaults(&settings);
    settings_ui_init(&ui);
    CHECK(settings_valid(&settings), "the defaults are not valid settings");
    CHECK(!settings.god_mode,
          "god mode shipped armed -- a safety rule that defaults to off is "
          "the only kind that is one");
    CHECK(settings.model == SETTINGS_MODEL_UNSET,
          "the default settings claimed a model had been asked for");

    // Every row has a label, a note, at least one chip, and no chip is wider
    // than the strip it has to live in.
    for (int r = 0; r < SETTINGS_ROW_COUNT; r++) {
        const settings_row_t row = (settings_row_t)r;
        const char *label = settings_row_label(row);
        CHECK(label && label[0], "row %d has no label", r);
        CHECK((int)lang_display_cols(label) * 6 <= settings_row_option_x(row) - SETTINGS_LABEL_X,
              "row %d's label runs into its chips", r);
        CHECK(settings_row_note(row)[0],
              "row %d has no note saying what it does", r);
        const unsigned count = settings_option_count(row);
        CHECK(count >= 1 && count <= SETTINGS_OPTIONS_MAX,
              "row %d offers %u options", r, count);
        for (unsigned o = 0; o < count; o++) {
            CHECK(settings_option_label(row, o)[0],
                  "row %d option %u has no label", r, o);
        }
        CHECK(!settings_option_label(row, count)[0],
              "row %d had a label past its last option", r);
    }

    // The rows tile the screen, do not overlap, and end above the extension's
    // floor -- rows 20 to 23 are left for an extension to draw on, and a
    // switch there would be one drawn over.  Every chip is inside its row, does not overlap its neighbour, and a
    // touch in the middle of it finds that chip and no other.
    for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
        const settings_row_t row = (settings_row_t)i;
        if ((row == SETTINGS_ROW_SHUTTER || row == SETTINGS_ROW_PICTURE ||
             row == SETTINGS_ROW_LIVEVIEW) &&
            !settings_camera_available()) continue;
        int x, y, w, h;
        CHECK(settings_row_rect(row, settings_test_scroll(row), &x, &y, &w, &h), "row %d has no rect", i);
        CHECK(x >= 0 && x + w <= 256 && y >= 0 && y + h <= 160,
              "row %d's rect runs into the extension's floor or off the screen", i);
        CHECK(settings_row_hit(settings_row_tab(row), settings_test_scroll(row), x + w / 2, y + h / 2) == row,
              "a touch in the middle of row %d hit something else", i);
        // Rows only share the screen with their own tab.  A row that is not
        // present (the camera's, on a card without one) has no rect to
        // compare against.
        const int prev_present = i == 0 ||
            settings_camera_available() ||
            ((settings_row_t)(i - 1) != SETTINGS_ROW_SHUTTER &&
             (settings_row_t)(i - 1) != SETTINGS_ROW_PICTURE &&
             (settings_row_t)(i - 1) != SETTINGS_ROW_LIVEVIEW);
        if (i > 0 && prev_present &&
            settings_row_tab((settings_row_t)(i - 1)) == settings_row_tab(row)) {
            int px, py, pw, ph;
            (void)settings_row_rect((settings_row_t)(i - 1), settings_test_scroll(row),
                                    &px, &py, &pw, &ph);
            CHECK(y >= py + ph, "rows %d and %d overlap", i - 1, i);
        }

        const unsigned count = settings_option_count(row);
        // Chips start clear of the row's own label.
        int prev_right = SETTINGS_LABEL_X + (int)strlen(settings_row_label(row)) * 6;
        for (unsigned o = 0; o < count; o++) {
            int cx, cy, cw, ch;
            CHECK(settings_option_rect(row, o, settings_test_scroll(row), &cx, &cy, &cw, &ch),
                  "row %d chip %u has no rect", i, o);
            CHECK(cx > prev_right, "row %d chips %u and %u overlap", i, o - 1, o);
            CHECK(cx + cw <= x + w,
                  "row %d chip %u runs off the end of the strip (%d > %d)",
                  i, o, cx + cw, x + w);
            CHECK(cy >= y && cy + ch <= y + h,
                  "row %d chip %u is not inside its row", i, o);
            CHECK(settings_option_hit(row, settings_test_scroll(row), cx + cw / 2,
                                      cy + ch / 2) == o,
                  "a touch on row %d chip %u found something else", i, o);
            // Wide enough for the text that goes in it, with a margin. The
            // drawing code centres the label in this rect and does not clip,
            // so a chip narrower than its own label spills into its
            // neighbour -- a defect you can otherwise only see on a screen.
            const char *text = settings_option_label(row, o);
            CHECK(cw >= (int)strlen(text) * 6 + 6,
                  "row %d chip %u is %dpx for the %d-glyph label '%s'",
                  i, o, cw, (int)strlen(text), text);
            prev_right = cx + cw;
        }
        // The gutter the label lives in is not a chip: resting a thumb on the
        // word MODEL must not change the model.
        CHECK(settings_option_hit(row, settings_test_scroll(row), SETTINGS_LABEL_X, y + h / 2) ==
                  SETTINGS_OPTIONS_MAX,
              "a touch on row %d's label pressed a chip", i);
    }
    CHECK(settings_row_hit(SETTINGS_TAB_SOUND, 0, 128, 180) == SETTINGS_ROW_COUNT,
          "a touch on the extension's floor hit a settings row");

    // And sized to their own labels rather than to a constant: SONNET is two
    // glyphs longer than OPUS and its chip has to be wider by exactly that.
    // A uniform width passes every check above -- it fits, it does not
    // overlap, the hit test agrees -- and still clips the longest label.
    {
        int ow, sw;
        (void)settings_option_rect(SETTINGS_ROW_MODEL, 0, 0, NULL, NULL, &ow, NULL);
        (void)settings_option_rect(SETTINGS_ROW_MODEL, 1, 0, NULL, NULL, &sw, NULL);
        CHECK(sw - ow == 2 * 6,
              "SONNET's chip is %dpx and OPUS's is %dpx; chips are not sized "
              "to their labels", sw, ow);
    }

    // A chip that types has to say exactly what it types, and a chip that only
    // changes something on the card must not claim to type anything -- that
    // pairing is what puts the string on the top screen next to the button.
    for (int r = 0; r < SETTINGS_ROW_COUNT; r++) {
        const settings_row_t row = (settings_row_t)r;
        const int types = row == SETTINGS_ROW_MODEL ||
                          row == SETTINGS_ROW_SESSION;
        for (unsigned o = 0; o < settings_option_count(row); o++) {
            const char *sends = settings_option_sends(row, o);
            if (types) {
                CHECK(sends && sends[0] == '/',
                      "row %d chip %u types %s, which is not a slash command",
                      r, o, sends ? sends : "(nothing)");
            } else {
                CHECK(sends == NULL,
                      "row %d chip %u claims to type %s", r, o, sends);
            }
        }
    }

    // Pressing a value chip stores it, and the page then shows it as current.
    settings_press_t got;
    const char *send = (const char *)1;   // must be cleared even on a no-op
    got = settings_press(&settings, &ui, SETTINGS_ROW_BOOT,
                         SETTINGS_BOOT_PROJECT, &send);
    CHECK(got == SETTINGS_PRESS_CHANGED, "picking a boot screen changed nothing");
    CHECK(send == NULL, "a value chip left a string to type");
    CHECK(settings.boot == SETTINGS_BOOT_PROJECT, "the boot screen did not stick");
    CHECK(settings_option_current(&settings, SETTINGS_ROW_BOOT) ==
              SETTINGS_BOOT_PROJECT,
          "the page does not show the boot screen that is set");
    // Pressing the one already set is not a change, so it cannot spend a card
    // write every time a thumb lands on it.
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_BOOT,
                         SETTINGS_BOOT_PROJECT, &send) ==
              SETTINGS_PRESS_NOTHING,
          "re-picking the current value reported a change");

    // Model: nothing is current until something has been asked for, and
    // picking one both records it and types it.
    settings_defaults(&settings);
    CHECK(settings_option_current(&settings, SETTINGS_ROW_MODEL) ==
              SETTINGS_OPTIONS_MAX,
          "the page claimed a model was current before one was asked for");
    got = settings_press(&settings, &ui, SETTINGS_ROW_MODEL, 1, &send);
    CHECK(got == SETTINGS_PRESS_SENDS, "picking a model did not type anything");
    CHECK(send && strcmp(send, "/model sonnet") == 0,
          "picking the second model typed %s", send ? send : "(nothing)");
    CHECK(settings.model == SETTINGS_MODEL_SONNET,
          "the model pick was not recorded");
    CHECK(settings_option_current(&settings, SETTINGS_ROW_MODEL) == 1,
          "the page does not show the model that was asked for");

    // The session commands type and store nothing: they are things you do.
    got = settings_press(&settings, &ui, SETTINGS_ROW_SESSION, 0, &send);
    CHECK(got == SETTINGS_PRESS_SENDS && send && strcmp(send, "/compact") == 0,
          "the first session chip did not type /compact");
    CHECK(settings_option_current(&settings, SETTINGS_ROW_SESSION) ==
              SETTINGS_OPTIONS_MAX,
          "a session command became a stored value");

    // DEFAULTS takes two presses, and anything in between abandons the first.
    settings_defaults(&settings);
    settings_ui_init(&ui);
    settings.god_mode = 1;
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_DEFAULTS, 0, &send) ==
              SETTINGS_PRESS_NOTHING,
          "one press of RESET restored the defaults");
    CHECK(ui.reset_armed, "the first press of RESET did not arm the confirm");
    CHECK(settings.god_mode == 1, "one press of RESET already changed something");
    (void)settings_press(&settings, &ui, SETTINGS_ROW_GOD_MODE, 0, &send);
    CHECK(!ui.reset_armed, "a press on another row left RESET armed");
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_DEFAULTS, 0, &send) ==
              SETTINGS_PRESS_NOTHING,
          "RESET fired on the press that should have re-armed it");
    settings.god_mode = 1;
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_DEFAULTS, 0, &send) ==
              SETTINGS_PRESS_RESET,
          "the second press of RESET did nothing");
    CHECK(!settings.god_mode && settings.model == SETTINGS_MODEL_UNSET,
          "RESET did not restore the defaults");
    CHECK(!ui.reset_armed, "RESET stayed armed after firing");

    // Out of range presses do nothing at all, and still clear the send.
    send = (const char *)1;
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_COUNT, 0, &send) ==
              SETTINGS_PRESS_NOTHING,
          "a row that does not exist was pressed");
    CHECK(send == NULL, "a refused press left a string to type");
    CHECK(settings_press(&settings, &ui, SETTINGS_ROW_MODEL,
                         SETTINGS_OPTIONS_MAX, &send) ==
              SETTINGS_PRESS_NOTHING,
          "a chip that does not exist was pressed");

    // The d-pad never leaves the highlight on a chip that is not there: rows
    // do not all have the same number, so walking them has to clamp.
    settings_ui_init(&ui);
    for (int step = 0; step < SETTINGS_ROW_COUNT * 3; step++) {
        settings_ui_move(&settings, &ui, 1, 0);   // hard right first
        settings_ui_move(&settings, &ui, 1, 0);
        settings_ui_move(&settings, &ui, 1, 0);
        settings_ui_move(&settings, &ui, 0, 1);   // then down a row
        CHECK(ui.row < SETTINGS_ROW_COUNT, "the highlight left the page");
        CHECK(ui.option < settings_option_count((settings_row_t)ui.row),
              "the highlight sat on chip %u of row %u, which has %u",
              ui.option, ui.row,
              settings_option_count((settings_row_t)ui.row));
    }
    // Rows wrap within their tab, options clamp.
    settings_ui_init(&ui);
    unsigned session_rows = 0;
    for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
        if (settings_row_tab((settings_row_t)i) == SETTINGS_TAB_SESSION) session_rows++;
    }
    for (unsigned i = 0; i < session_rows; i++) {
        settings_ui_move(&settings, &ui, 0, 1);
        CHECK(settings_row_tab((settings_row_t)ui.row) == SETTINGS_TAB_SESSION,
              "moving down left the SESSION tab");
    }
    CHECK(ui.row == 0, "a full lap of the rows did not come back to the first");
    settings_ui_move(&settings, &ui, -1, 0);
    CHECK(ui.option == 0, "left off the first chip did not stop");

    // The tabs: CUSTOMIZE opens on its first row, on that row's value, and
    // its own lap stays inside it.  Every tab fits above the extension strip.
    settings_ui_tab(&settings, &ui, SETTINGS_TAB_SOUND);
    CHECK(ui.row == SETTINGS_ROW_SOUND &&
              ui.option == settings_option_current(&settings, SETTINGS_ROW_SOUND),
          "CUSTOMIZE did not open on SOUND, on its value");
    for (int i = 0; i < 20; i++) {
        settings_ui_move(&settings, &ui, 0, -1);
        CHECK(settings_row_tab((settings_row_t)ui.row) == SETTINGS_TAB_SOUND,
              "moving up left the CUSTOMIZE tab");
    }
    for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
        int y = 0, h = 0;
        (void)settings_row_rect((settings_row_t)i, settings_test_scroll((settings_row_t)i),
                                NULL, &y, NULL, &h);
        CHECK(y + h <= 160, "row %d runs into the extension strip (ends %d)", i, y + h);
    }
    for (unsigned t = 0; t < SETTINGS_TAB_COUNT; t++) {
        int x, y, w, h;
        CHECK(settings_tab_rect(t, &x, &y, &w, &h) &&
                  settings_tab_hit(x + w / 2, y + h / 2) == t,
              "tab %u is not where it is drawn", t);
        CHECK(y + h <= SETTINGS_ROW_Y, "tab %u overlaps the rows", t);
    }
    CHECK(settings_tab_hit(250, 8) == SETTINGS_TAB_COUNT, "the hint is a tab");
    // THEME and TYPING are values, saved, and the theme is a real colour.
    {
        settings_t t;
        settings_defaults(&t);
        CHECK(t.typing == 1 && t.theme == SETTINGS_THEME_CLAY,
              "the defaults are not typing on, clay");
        settings_ui_t tu;
        settings_ui_init(&tu);
        CHECK(settings_press(&t, &tu, SETTINGS_ROW_THEME, 1, NULL) ==
                  SETTINGS_PRESS_CHANGED && t.theme == SETTINGS_THEME_SEA,
              "THEME did not switch");
        CHECK(settings_press(&t, &tu, SETTINGS_ROW_TYPING, 1, NULL) ==
                  SETTINGS_PRESS_CHANGED && t.typing == 0,
              "TYPING did not switch off");
        CHECK(settings_theme_rgb(SETTINGS_THEME_CLAY) != settings_theme_rgb(SETTINGS_THEME_SEA),
              "two themes are the same colour");
        char terr[64];
        settings_t back;
        CHECK(settings_parse("version=4\ntheme=moss\ntyping=off\n", &back, terr,
                             sizeof(terr)) &&
                  back.theme == SETTINGS_THEME_MOSS && back.typing == 0,
              "theme or typing not read: %s", terr);
        CHECK(settings_parse("version=4\ntheme=purple\ntyping=off\n", &back, terr,
                             sizeof(terr)) &&
                  back.theme == SETTINGS_THEME_CLAY && back.typing == 0,
              "an unknown theme was not its default alone: %s", terr);
        CHECK(settings_parse("version=4\nsound=soft\n", &back, terr, sizeof(terr)) &&
                  back.sound == SETTINGS_SOUND_SNES,
              "sound=soft (the chip's own word) was refused: %s", terr);
    }

    // The page paints every cell it is given, including the blank ones, and
    // never writes outside the grid.  Same two things the terminal has to get
    // right, checked the same way project.c's page is.
    {
        static grid_t g;
        settings_about_t about;
        settings_about_init(&about);
        memset(&g, 0xAB, sizeof(g));
        settings_ui_init(&ui);
        settings_render(&settings, &ui, &about, &g);
        int untouched = 0, out_of_range = 0;
        for (int i = 0; i < PROTO_CELLS; i++) {
            if (g.cells[i].glyph == 0xAB) untouched++;
            if (g.cells[i].glyph < 0x20 || g.cells[i].glyph > 0x7E) {
                out_of_range++;
            }
        }
        // Aggregated rather than one CHECK per cell: 1008 passing checks tell
        // nobody anything, and inflate the count this suite is judged by.
        CHECK(untouched == 0, "%d cells were left showing the last page",
              untouched);
        CHECK(out_of_range == 0, "%d cells hold something unprintable",
              out_of_range);
        // The one row nobody should be able to miss.
        settings.god_mode = 1;
        settings_render(&settings, &ui, &about, &g);
        int warned = 0;
        for (int r = 0; r < PROTO_ROWS; r++) {
            if (g.cells[r * PROTO_COLS].glyph == 'G' &&
                g.cells[r * PROTO_COLS + 1].glyph == 'O') warned = 1;
        }
        CHECK(warned, "the page did not say god mode was armed");
        settings.god_mode = 0;
    }

    // The file: a round trip, and a refusal.
    const char *path = "build/test_settings.cfg";
    remove(path);
    settings_defaults(&settings);
    settings.model = SETTINGS_MODEL_SONNET;
    settings.boot = SETTINGS_BOOT_KEYS;
    settings.god_mode = 1;
    char err[96] = "";
    CHECK(settings_save(path, &settings, err, sizeof(err)),
          "could not save settings: %s", err);
    settings_t loaded;
    CHECK(settings_load(path, &loaded, err, sizeof(err)),
          "could not load settings: %s", err);
    CHECK(loaded.model == SETTINGS_MODEL_SONNET &&
          loaded.boot == SETTINGS_BOOT_KEYS && loaded.god_mode == 1,
          "settings changed on the way through the card");

    // Saving over an existing file, which is every save after the first, and
    // which is exactly the case that once silently failed on libfat for four
    // builds in another preferences file.
    settings.god_mode = 0;
    CHECK(settings_save(path, &settings, err, sizeof(err)),
          "could not save over an existing settings file: %s", err);
    CHECK(settings_load(path, &loaded, err, sizeof(err)) &&
          loaded.god_mode == 0,
          "the second save did not replace the first");
    char temporary[320];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *leftover = fopen(temporary, "rb");
    if (leftover) fclose(leftover);
    CHECK(leftover == NULL, "a settings save left its temporary behind");

    // A file with no version is refused to defaults rather than half-applied;
    // a value this build does not know costs only its own key.
    CHECK(!settings_parse("model=opus\n", &loaded, err, sizeof(err)),
          "a settings file with no version was accepted");
    CHECK(loaded.god_mode == 0 && loaded.model == SETTINGS_MODEL_UNSET,
          "a refused settings file left values behind");
    CHECK(settings_parse("version=2\nmodel=nonesuch\nboot=keys\n", &loaded,
                         err, sizeof(err)) &&
              loaded.model == SETTINGS_MODEL_UNSET && loaded.boot == SETTINGS_BOOT_KEYS,
          "an unknown model was not its default alone: %s", err);
    // But an unknown *key* is forward compatibility, not corruption.
    CHECK(settings_parse("version=2\nmodel=opus\nfuture=42\n", &loaded,
                         err, sizeof(err)) &&
          loaded.model == SETTINGS_MODEL_OPUS,
          "a key from a later build threw the whole file away");
    // And god mode is never something a file can turn on by being malformed.
    CHECK(settings_parse("version=2\ngod_mode=on\ngod_mode=maybe\n", &loaded,
                         err, sizeof(err)) && loaded.god_mode == 0,
          "a malformed god_mode left it armed");

    // A version 1 file still loads.  It carried two keys this build does not
    // have and spelled the unset model "default"; the point of the version is
    // that upgrading the card keeps the one setting that survived rather than
    // throwing the file away and re-arming nothing.
    CHECK(settings_parse("version=1\nmodel=default\ndim=medium\n"
                         "repeat=fast\ngod_mode=on\n",
                         &loaded, err, sizeof(err)),
          "a version 1 settings file was rejected: %s", err);
    CHECK(loaded.model == SETTINGS_MODEL_UNSET && loaded.god_mode == 1,
          "a version 1 file did not carry its god_mode across");
    CHECK(!settings_parse("version=6\n", &loaded, err, sizeof(err)),
          "a version from the future was accepted");
    // A version 3 file (no autoconnect key) keeps connecting by itself; a
    // version 4 file can turn it off, and says so when saved.
    CHECK(settings_parse("version=3\nsound=chip\n", &loaded, err, sizeof(err)) &&
              loaded.autoconnect == 1,
          "a version 3 file lost autoconnect's default: %s", err);
    CHECK(settings_parse("version=4\nautoconnect=off\n", &loaded, err, sizeof(err)) &&
              loaded.autoconnect == 0,
          "autoconnect=off was not read: %s", err);
    CHECK(settings_parse("version=4\nautoconnect=maybe\n", &loaded, err, sizeof(err)) &&
              loaded.autoconnect == 1,
          "a bad autoconnect value was not its default: %s", err);
    {
        settings_t off;
        settings_defaults(&off);
        off.autoconnect = 0;
        CHECK(settings_save(path, &off, err, sizeof(err)) &&
                  settings_load(path, &loaded, err, sizeof(err)) &&
                  loaded.autoconnect == 0,
              "autoconnect=off did not survive a save: %s", err);
        settings_ui_t ui;
        settings_ui_init(&ui);
        CHECK(settings_option_current(&off, SETTINGS_ROW_AUTOCONNECT) == 1 &&
                  settings_press(&off, &ui, SETTINGS_ROW_AUTOCONNECT, 0, NULL) ==
                      SETTINGS_PRESS_CHANGED &&
                  off.autoconnect == 1,
              "the AUTOCONNECT chips do not switch it");
    }
    // ADDRESSES: shown unless asked, read and written as show/hide, and a
    // file from before it existed keeps showing them.
    CHECK(settings_parse("version=4\nsound=chip\n", &loaded, err, sizeof(err)) &&
              loaded.hide_addresses == 0,
          "a file without addresses= hid them: %s", err);
    CHECK(settings_parse("version=4\naddresses=hide\n", &loaded, err, sizeof(err)) &&
              loaded.hide_addresses == 1,
          "addresses=hide was not read: %s", err);
    CHECK(settings_parse("version=4\naddresses=blur\n", &loaded, err, sizeof(err)) &&
              loaded.hide_addresses == 0,
          "a bad addresses value was not its default: %s", err);
    {
        settings_t hide;
        settings_defaults(&hide);
        settings_ui_t ui;
        settings_ui_init(&ui);
        CHECK(settings_row_tab(SETTINGS_ROW_ADDRESSES) == SETTINGS_TAB_SCREEN &&
                  settings_option_current(&hide, SETTINGS_ROW_ADDRESSES) == 0 &&
                  settings_press(&hide, &ui, SETTINGS_ROW_ADDRESSES, 1, NULL) ==
                      SETTINGS_PRESS_CHANGED &&
                  hide.hide_addresses == 1,
              "the ADDRESSES chips do not switch it");
        CHECK(settings_save(path, &hide, err, sizeof(err)) &&
                  settings_load(path, &loaded, err, sizeof(err)) &&
                  loaded.hide_addresses == 1,
              "addresses=hide did not survive a save: %s", err);
    }
    remove(path);
}

// QUICK: the mode read off Claude's footer, the command list, recent prompts.
static void test_quick(void)
{
    char mode[QUICK_MODE_MAX + 1];
    struct { const char *line; const char *want; } modes[] = {
        { "  >> auto mode on (shift+tab to cycle) .", "auto" },
        { "  || manual mode on . ? for shortcuts", "manual" },
        { "  >> accept edits on (shift+tab to  . <", "edits" },
        { "  || plan mode on (shift+tab to cycle) .", "plan" },
        { "  >> bypass permissions on", "bypass" },
        { "  ? for shortcuts", "normal" },
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        CHECK(quick_mode_from_line(modes[i].line, mode) && strcmp(mode, modes[i].want) == 0,
              "'%s' read as '%s', wanted '%s'", modes[i].line, mode, modes[i].want);
    }
    CHECK(!quick_mode_from_line("Try \"how does main.c work?\"", mode),
          "a line with no mode read as one");
    CHECK(!quick_mode_from_line(" mode on", mode), "a mode with no name read as one");
    CHECK(!quick_mode_from_line("averyveryverylongword mode on", mode),
          "a mode word longer than the field was read");

    quick_commands_t cmds;
    quick_commands_init(&cmds);
    const unsigned builtin = quick_commands_builtin();
    CHECK(cmds.count == builtin && strcmp(cmds.names[0], "/compact") == 0,
          "the built-in commands are not first");
    const char wire[] = "/deploy\n/review\nbad name\n/frontend:test\n;rm\n/x.y_z-1\n"
                        "/-no\n/this-name-is-far-too-long-for-a-row";
    CHECK(quick_commands_apply(&cmds, (const uint8_t *)wire, sizeof(wire) - 1) == 3 &&
              cmds.count == builtin + 3 &&
              strcmp(cmds.names[builtin], "/deploy") == 0 &&
              strcmp(cmds.names[builtin + 1], "/frontend:test") == 0 &&
              strcmp(cmds.names[builtin + 2], "/x.y_z-1") == 0,
          "the computer's commands were not taken as names (%u)", cmds.count);
    {
        // The interop vector: the server's payload, as this card keeps it.
        const char vec[] = "/deploy\n/frontend:test\n/x.y_z-1";
        (void)quick_commands_apply(&cmds, (const uint8_t *)vec, sizeof(vec) - 1);
        char joined[128] = "", hex[257];
        for (unsigned i = builtin; i < cmds.count; i++) {
            if (i > builtin) strcat(joined, "|");
            strcat(joined, cmds.names[i]);
        }
        for (size_t i = 0; joined[i]; i++) {
            snprintf(hex + 2 * i, 3, "%02x", (unsigned char)joined[i]);
        }
        hex[2 * strlen(joined)] = '\0';
        printf("INTEROP commands %s\n", hex);
    }
    CHECK(quick_commands_apply(&cmds, NULL, 0) == 0 && cmds.count == builtin,
          "an empty list did not leave the built-ins alone");

    quick_recent_t r;
    quick_recent_init(&r);
    CHECK(quick_recent_add(&r, "run the tests\r") && r.count == 1 &&
              strcmp(r.items[0], "run the tests") == 0,
          "a prompt was not kept, trimmed");
    CHECK(!quick_recent_add(&r, "   "), "an empty prompt was kept");
    CHECK(quick_recent_add(&r, "commit this") && quick_recent_add(&r, "run the tests") &&
              r.count == 2 && strcmp(r.items[0], "run the tests") == 0 &&
              strcmp(r.items[1], "commit this") == 0,
          "a repeat did not move to the front");
    CHECK(!quick_recent_add(&r, "run the tests"), "the newest re-added changed the list");
    for (int i = 0; i < 15; i++) {
        // Sized past gcc's worst-case %d bound, which it cannot narrow to
        // this loop's range.
        char text[24];
        snprintf(text, sizeof(text), "prompt %d", i);
        (void)quick_recent_add(&r, text);
    }
    CHECK(r.count == QUICK_RECENT_MAX && strcmp(r.items[0], "prompt 14") == 0 &&
              strcmp(r.items[QUICK_RECENT_MAX - 1], "prompt 5") == 0,
          "the list did not keep the newest ten");
    char big[QUICK_RECENT_TEXT_MAX + 2];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(!quick_recent_add(&r, big), "an overlong prompt was kept");
    CHECK(quick_recent_remove(&r, 0) && r.count == QUICK_RECENT_MAX - 1 &&
              strcmp(r.items[0], "prompt 13") == 0 && !quick_recent_remove(&r, 99),
          "forgetting one did not close the gap");

    quick_recent_init(&r);
    (void)quick_recent_add(&r, "plain");
    (void)quick_recent_add(&r, "two\nlines and a \\ slash");
    char file[1024];
    const size_t flen = quick_recent_format(&r, file, sizeof(file));
    CHECK(flen > 0 && strcmp(file, "two\\nlines and a \\\\ slash\nplain\n") == 0,
          "the file is not one escaped prompt per line: '%s'", file);
    quick_recent_t back;
    quick_recent_parse(&back, file);
    CHECK(back.count == 2 && strcmp(back.items[0], r.items[0]) == 0 &&
              strcmp(back.items[1], "plain") == 0,
          "the file did not read back");
    quick_recent_parse(&back, "ok\nbad \\q escape\n\x01 control\ntrailing\\");
    CHECK(back.count == 1 && strcmp(back.items[0], "ok") == 0,
          "lines that do not decode were kept (%u)", back.count);
    CHECK(quick_recent_format(&r, file, 8) == 0, "a list that does not fit was cut");
    char row[16];
    quick_recent_row("two\nlines", row, 12);
    CHECK(strcmp(row, "two / lines") == 0, "a row shows '%s'", row);

    quick_line_t line;
    quick_line_reset(&line);
    char got[QUICK_RECENT_TEXT_MAX + 1];
    CHECK(!quick_line_feed(&line, "fix the bugz", 12, got, sizeof(got)) &&
              !quick_line_feed(&line, "\x7f", 1, got, sizeof(got)) &&
              quick_line_feed(&line, "\r", 1, got, sizeof(got)) &&
              strcmp(got, "fix the bug") == 0,
          "a typed line with a backspace was not kept as typed");
    CHECK(!quick_line_feed(&line, "abc\x1b[Dx\r", 8, got, sizeof(got)),
          "a line edited with an arrow was kept");
    CHECK(quick_line_feed(&line, "next\r", 5, got, sizeof(got)) && strcmp(got, "next") == 0,
          "the line after a lost one was not kept");
    CHECK(!quick_line_feed(&line, "\r", 1, got, sizeof(got)), "an empty line was kept");
    CHECK(!quick_line_feed(&line, "gone\x15", 5, got, sizeof(got)) &&
              quick_line_feed(&line, "kept\r", 5, got, sizeof(got)) && strcmp(got, "kept") == 0,
          "Ctrl-U did not clear the line");

    // PHOTO is the last row, and a card with no camera has a five-row menu.
    CHECK(QUICK_ROW_PHOTO == QUICK_ROW_COUNT - 1,
          "PHOTO is not the last row");
    CHECK(!strcmp(quick_row_label(QUICK_ROW_PHOTO), "PHOTO") &&
              !strcmp(quick_row_note(QUICK_ROW_PHOTO), "see it, send it"),
          "the PHOTO row's words are wrong");
    CHECK(quick_row_count(1) == QUICK_ROW_COUNT && quick_row_count(0) == QUICK_ROW_COUNT - 1,
          "the camera-only row did not drop off");

    // Saved quick commands (/dsterm/quick.txt): any printable prompt text (not
    // only slash commands), ordered built-ins -> saved -> server's -> + ADD,
    // bounded at QUICK_SAVED_MAX (16) and QUICK_SAVED_TEXT_MAX (1024), corrupt
    // lines skipped, empty file yields 0, round-trip preserved.
    static quick_saved_t qs, qs_back;
    quick_saved_init(&qs);
    CHECK(qs.count == 0, "quick_saved_init did not zero count");
    CHECK(quick_saved_add(&qs, "run pytest and fix any failure   \n") &&
              qs.count == 1 &&
              strcmp(qs.items[0], "run pytest and fix any failure") == 0,
          "plain prompt text was not saved and trimmed");
    CHECK(quick_saved_add(&qs, "line one\nline two with \\ backslash") &&
              qs.count == 2,
          "multi-line prompt was not saved");
    // Adding an existing command is idempotent.
    CHECK(quick_saved_add(&qs, "run pytest and fix any failure") && qs.count == 2,
          "duplicate saved command grew the list");
    // Empty, control bytes, and > 1024 bytes are refused.
    CHECK(!quick_saved_add(&qs, "   \t\n") &&
              !quick_saved_add(&qs, "bad\x01char"),
          "empty or control-byte command was accepted");
    static char overlong[QUICK_SAVED_TEXT_MAX + 3];
    memset(overlong, 'x', QUICK_SAVED_TEXT_MAX + 1);
    overlong[QUICK_SAVED_TEXT_MAX + 1] = '\0';
    CHECK(!quick_saved_add(&qs, overlong), "overlong command (> 1024) was accepted");
    overlong[QUICK_SAVED_TEXT_MAX] = '\0';
    CHECK(quick_saved_add(&qs, overlong) && qs.count == 3,
          "exact composer-limit command (1024 bytes) was refused");
    CHECK(quick_saved_remove(&qs, 2) && qs.count == 2,
          "removing the 1024-byte command failed");

    // Why a save fails, so the composer can keep the text and say so; a tab
    // (from a snippet or the clipboard) is a space, not a refusal; and a
    // command already in the list reports where it is.
    {
        static quick_saved_t qa;
        unsigned at = 99;
        quick_saved_init(&qa);
        CHECK(quick_saved_add_at(&qa, " \t \n", &at) == QUICK_ADD_EMPTY,
              "spaces and a tab were not reported empty");
        CHECK(quick_saved_add_at(&qa, "run\tthe tests", &at) == QUICK_ADD_OK &&
                  at == 0 && strcmp(qa.items[0], "run the tests") == 0,
              "a tab was refused or kept instead of becoming a space");
        CHECK(quick_saved_add_at(&qa, "bad\x01", &at) == QUICK_ADD_INVALID,
              "a control byte was not reported invalid");
        CHECK(quick_saved_add_at(&qa, overlong, &at) == QUICK_ADD_OK && at == 1,
              "the composer-limit command was refused");
        overlong[QUICK_SAVED_TEXT_MAX] = 'x';
        CHECK(quick_saved_add_at(&qa, overlong, &at) == QUICK_ADD_TOO_LONG,
              "an overlong command was not reported too long");
        overlong[QUICK_SAVED_TEXT_MAX] = '\0';
        for (unsigned i = qa.count; i < QUICK_SAVED_MAX; i++) {
            char item[16];
            snprintf(item, sizeof(item), "cmd %u", i);
            (void)quick_saved_add(&qa, item);
        }
        CHECK(quick_saved_add_at(&qa, "one too many", &at) == QUICK_ADD_FULL,
              "a full list was not reported full");
        at = 99;
        CHECK(quick_saved_add_at(&qa, "run the tests", &at) == QUICK_ADD_OK && at == 0,
              "an existing command did not report its own place");
    }

    // COMMANDS menu ordering: built-ins, then saved, then server's, then + ADD.
    (void)quick_commands_apply(&cmds, (const uint8_t *)"/deploy\n", 8);
    CHECK(quick_commands_total(&cmds, &qs) == builtin + 2u + 1u + 1u,
          "quick_commands_total wrong (%u)", quick_commands_total(&cmds, &qs));
    const char *etext = NULL;
    unsigned sidx = 99;
    CHECK(quick_commands_entry(&cmds, &qs, 0, &etext, &sidx) == QUICK_CMD_BUILTIN &&
              strcmp(etext, "/compact") == 0,
          "entry 0 is not /compact");
    CHECK(quick_commands_entry(&cmds, &qs, builtin, &etext, &sidx) == QUICK_CMD_SAVED &&
              sidx == 0 && strcmp(etext, "run pytest and fix any failure") == 0,
          "saved command 0 is not right after built-ins");
    CHECK(quick_commands_entry(&cmds, &qs, builtin + 1u, &etext, &sidx) == QUICK_CMD_SAVED &&
              sidx == 1,
          "saved command 1 is not second");
    CHECK(quick_commands_entry(&cmds, &qs, builtin + 2u, &etext, &sidx) == QUICK_CMD_SERVER &&
              strcmp(etext, "/deploy") == 0,
          "server command is not after saved commands");
    CHECK(quick_commands_entry(&cmds, &qs, builtin + 3u, &etext, &sidx) == QUICK_CMD_ADD &&
              strcmp(etext, "+ ADD") == 0,
          "last row of COMMANDS is not + ADD");
    CHECK(quick_commands_entry(&cmds, &qs, builtin + 4u, &etext, &sidx) == QUICK_CMD_NONE,
          "out-of-range row was not QUICK_CMD_NONE");

    // Round-trip format & parse, empty file, corrupt file, and count cap (16).
    static char qbuf[4096];
    const size_t qlen = quick_saved_format(&qs, qbuf, sizeof(qbuf));
    CHECK(qlen > 0, "quick_saved_format failed");
    quick_saved_parse(&qs_back, qbuf);
    CHECK(qs_back.count == 2 &&
              strcmp(qs_back.items[0], qs.items[0]) == 0 &&
              strcmp(qs_back.items[1], qs.items[1]) == 0,
          "quick_saved round-trip failed");
    quick_saved_parse(&qs_back, "");
    CHECK(qs_back.count == 0, "empty quick.txt did not parse to 0 items");
    quick_saved_parse(&qs_back, "good prompt\nbad \\z escape\n\x02\nsecond good\ntrailing\\");
    CHECK(qs_back.count == 2 &&
              strcmp(qs_back.items[0], "good prompt") == 0 &&
              strcmp(qs_back.items[1], "second good") == 0,
          "corrupt lines in quick.txt were not skipped cleanly (%u)", qs_back.count);

    quick_saved_init(&qs);
    for (unsigned i = 0; i < QUICK_SAVED_MAX + 4u; i++) {
        char item[32];
        snprintf(item, sizeof(item), "custom command %u", i);
        const int ok = quick_saved_add(&qs, item);
        CHECK(ok == (i < QUICK_SAVED_MAX),
              "quick_saved_add at %u returned %d", i, ok);
    }
    CHECK(qs.count == QUICK_SAVED_MAX, "saved command count did not cap at %u",
          QUICK_SAVED_MAX);
    CHECK(quick_saved_remove(&qs, 0) && qs.count == QUICK_SAVED_MAX - 1u &&
              strcmp(qs.items[0], "custom command 1") == 0 &&
              !quick_saved_remove(&qs, 99),
          "quick_saved_remove did not shift items");
}

// The bottom screen as text (mirror.h): glyphs land in the cell under their
// centre, fills blank the cells they cover, and the wire form is exact.
static void test_mirror(void)
{
    static mirror_t m;
    mirror_clear(&m, 0x1234);
    CHECK(m.cells[0][0].glyph == ' ' && m.cells[23][41].bg == 0x1234,
          "a cleared mirror is not blank");
    mirror_glyph_px(&m, 6 * 5, 8 * 3, 'Q', 0x7FFF, 0x0001);
    CHECK(m.cells[3][5].glyph == 'Q' && m.cells[3][5].fg == 0x7FFF &&
              m.cells[3][5].bg == 0x0001,
          "a cell-aligned glyph missed its cell");
    // A tile label drawn off the grid: the cell under its centre.
    mirror_glyph_px(&m, 6 * 10 + 4, 8 * 7 + 5, 'K', 1, 2);
    CHECK(m.cells[8][11].glyph == 'K', "an off-grid glyph landed elsewhere");
    mirror_glyph_px(&m, -6, 0, 'X', 1, 2);             // off the screen: nothing
    mirror_glyph_px(&m, 256, 0, 'X', 1, 2);
    mirror_fill_px(&m, 6 * 5, 8 * 3, 12, 8, 0x0042);    // over the Q and one more
    CHECK(m.cells[3][5].glyph == ' ' && m.cells[3][5].bg == 0x0042 &&
              m.cells[3][6].bg == 0x0042 && m.cells[3][7].bg == 0x1234,
          "a fill did not blank exactly the cells it covers");
    mirror_flag_rows(&m, 8 * 2, 8 * 4 - 1, MIRROR_FLAG_CAMERA);
    CHECK((m.cells[2][0].flags & MIRROR_FLAG_CAMERA) &&
              (m.cells[3][41].flags & MIRROR_FLAG_CAMERA) &&
              !(m.cells[4][0].flags & MIRROR_FLAG_CAMERA),
          "the camera's rows were not flagged exactly");

    static uint8_t wire[MIRROR_PAYLOAD_SIZE];
    CHECK(mirror_encode(&m, MIRROR_FRAME_TAPS, wire, sizeof(wire) - 1) == 0,
          "a mirror was encoded into too small a buffer");
    CHECK(mirror_encode(&m, MIRROR_FRAME_TAPS, wire, sizeof(wire)) == MIRROR_PAYLOAD_SIZE &&
              wire[0] == 42 && wire[1] == 24 && wire[2] == MIRROR_FRAME_TAPS,
          "the mirror's header is wrong");
    const size_t k = 3 + (8 * 42 + 11) * MIRROR_CELL_BYTES;
    CHECK(wire[k] == 'K' && wire[k + 2] == 0 && wire[k + 3] == 1 &&
              wire[k + 4] == 0 && wire[k + 5] == 2,
          "a cell's bytes are wrong");
    const uint32_t before = mirror_hash(&m);
    mirror_glyph_px(&m, 0, 0, 'Z', 1, 2);
    CHECK(mirror_hash(&m) != before, "a changed screen hashed the same");

    // INTEROP: the server must read this exactly.
    static mirror_t v;
    mirror_clear(&v, 0x0421);
    mirror_glyph_px(&v, 0, 0, 'D', 0x7FFF, 0x0421);
    mirror_glyph_px(&v, 6 * 41, 8 * 23, 'S', 0x001F, 0x7C00);
    mirror_flag_rows(&v, 8, 15, MIRROR_FLAG_CAMERA);
    (void)mirror_encode(&v, 0, wire, sizeof(wire));
    static char hex[2 * 64 + 1];
    for (int i = 0; i < 64; i++) snprintf(hex + 2 * i, 3, "%02x", wire[i]);
    printf("INTEROP subscreen-head %s\n", hex);
    for (int i = 0; i < 64; i++) snprintf(hex + 2 * i, 3, "%02x", wire[MIRROR_PAYLOAD_SIZE - 64 + i]);
    printf("INTEROP subscreen-tail %s\n", hex);

    // The two server messages.
    int watching = -1;
    uint8_t x = 0, y = 0;
    CHECK(proto_decode_mirror(&watching, (const uint8_t *)"\x01", 1) == PROTO_OK &&
              watching == 1 &&
              proto_decode_mirror(&watching, (const uint8_t *)"\x00", 1) == PROTO_OK &&
              watching == 0,
          "MIRROR did not decode");
    CHECK(proto_decode_mirror(&watching, (const uint8_t *)"\x02", 1) != PROTO_OK &&
              proto_decode_mirror(&watching, (const uint8_t *)"\x01\x00", 2) != PROTO_OK,
          "a MIRROR with reserved bits or the wrong size was taken");
    CHECK(proto_decode_touch(&x, &y, (const uint8_t *)"\xff\xbf", 2) == PROTO_OK &&
              x == 255 && y == 191,
          "TOUCH did not decode");
    CHECK(proto_decode_touch(&x, &y, (const uint8_t *)"\x10\xc0", 2) != PROTO_OK &&
              proto_decode_touch(&x, &y, (const uint8_t *)"\x10", 1) != PROTO_OK,
          "a TOUCH below the screen or short was taken");

    // VIEW TAPS: off by default, read and written as on/off.
    settings_t st;
    settings_defaults(&st);
    char err[64];
    CHECK(st.view_taps == 0, "VIEW TAPS is on by default");
    CHECK(settings_parse("version=4\nviewtaps=on\n", &st, err, sizeof(err)) &&
              st.view_taps == 1,
          "viewtaps=on was not read: %s", err);
    settings_ui_t ui;
    settings_ui_init(&ui);
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_VIEW_TAPS, 0, NULL) ==
              SETTINGS_PRESS_CHANGED && st.view_taps == 0,
          "the VIEW TAPS chips do not switch it");
}

// HIDE ADDRESSES: addresses become stars of the same shape; nothing else moves.
static void test_privacy_mask(void)
{
    struct { const char *in, *out; } cases[] = {
        { "tcp: 192.0.2.10:8347", "tcp: ***.*.*.**:8347" },
        { "wifi: ok, ip 10.0.0.255", "wifi: ok, ip **.*.*.***" },
        { "MacBook (198.51.100.7)", "MacBook (***.**.***.*)" },
        { "mac 00:1a:2b:3c:4d:5e ok", "mac **:**:**:**:**:** ok" },
        { "AA-BB-CC-DD-EE-FF", "**-**-**-**-**-**" },
        // Not addresses: left exactly as they are.
        { "dsterm v1.2.3", "dsterm v1.2.3" },
        { "1.2.3.4.5", "1.2.3.4.5" },
        { "999.1.1.1", "999.1.1.1" },
        { "12:30 to 13:45", "12:30 to 13:45" },
        { "42x24  14ms", "42x24  14ms" },
        { "", "" },
    };
    char buf[64];
    privacy_set_hide(0);
    CHECK(strcmp(privacy_text("tcp: 192.0.2.10:8347"), "tcp: 192.0.2.10:8347") == 0,
          "addresses were hidden with the setting off");
    privacy_set_hide(1);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(buf, sizeof(buf), "%s", cases[i].in);
        privacy_mask(buf);
        CHECK(strcmp(buf, cases[i].out) == 0, "masked '%s' as '%s', wanted '%s'",
              cases[i].in, buf, cases[i].out);
        CHECK(strcmp(privacy_text(cases[i].in), cases[i].out) == 0,
              "privacy_text('%s') differs from privacy_mask", cases[i].in);
    }
    // Two in one expression: both survive (rotating buffers).
    const char *a = privacy_text("192.0.2.1");
    const char *b = privacy_text("192.0.2.2");
    CHECK(strcmp(a, "***.*.*.*") == 0 && strcmp(b, "***.*.*.*") == 0 && a != b,
          "two masked strings shared a buffer");
    CHECK(privacy_text(NULL) == NULL, "NULL was not passed through");
    privacy_set_hide(0);
}

static void test_clip_wire(void)
{
    // The clipboard's staging machine, and the invariant that is the whole
    // point of the wire's shape: the bytes received must be exactly the
    // converted length, or that length capped.  Everything else here is a
    // refusal, because the failure this protocol's history says to expect is
    // the two encoders drifting apart while both suites stay green.
    static clip_t clip;
    clip_init(&clip);
    CHECK(!clip_have(&clip), "a fresh clipboard claimed to have something");
    CHECK(!clip_pastable(&clip), "a fresh clipboard offered a paste");

    // A one-chunk snapshot, arriving whole.
    CHECK(clip_wire_chunk(&clip, 1, 0, 7, 0, 5, "hello", 5),
          "a complete one-chunk clipboard did not commit");
    CHECK(clip_have(&clip) && clip.len == 5 &&
          strcmp(clip.text, "hello") == 0, "the text did not arrive intact");
    CHECK(clip_pastable(&clip), "five bytes were not pastable");

    // Two chunks, in order, committing only on the second.
    CHECK(!clip_wire_chunk(&clip, 2, 0, 8, 0, 6, "abc", 3),
          "a partial clipboard committed early");
    CHECK(strcmp(clip.text, "hello") == 0,
          "a partial snapshot overwrote the last complete one");
    CHECK(clip_wire_chunk(&clip, 2, 1, 8, 0, 6, "def", 3),
          "the completing chunk did not commit");
    CHECK(strcmp(clip.text, "abcdef") == 0, "the chunks were not joined");

    // The invariant. A snapshot whose bytes do not match its declared length
    // is thrown away whole rather than shown with a caveat -- this is the one
    // assertion that catches the encoders disagreeing.
    CHECK(!clip_wire_chunk(&clip, 1, 0, 9, 0, 99, "short", 5),
          "a chunk whose total disagreed with its bytes was accepted");
    CHECK(strcmp(clip.text, "abcdef") == 0,
          "a rejected snapshot took away the one already held");

    // Truncation has to be both stated and true.
    CHECK(!clip_wire_chunk(&clip, 1, 0, 10, CLIP_FLAG_TRUNCATED, 5, "hello", 5),
          "a snapshot that fits claimed to be shortened");
    CHECK(!clip_wire_chunk(&clip, 1, 0, 11, 0, CLIP_TEXT_MAX + 1, "hi", 2),
          "a snapshot over the cap did not have to say so");

    // A full-length truncated snapshot: six chunks of 192 is exactly 1024,
    // which is the only shape a truncated clipboard can have.
    {
        static char big[CLIP_CHUNK_MAX];
        memset(big, 'x', sizeof(big));
        clip_init(&clip);
        int committed = 0;
        for (unsigned i = 0; i < CLIP_CHUNKS_MAX; i++) {
            const unsigned len = (i == CLIP_CHUNKS_MAX - 1)
                ? CLIP_TEXT_MAX - CLIP_CHUNK_MAX * (CLIP_CHUNKS_MAX - 1)
                : CLIP_CHUNK_MAX;
            committed = clip_wire_chunk(&clip, CLIP_CHUNKS_MAX, i, 12,
                                        CLIP_FLAG_TRUNCATED, 70000u, big, len);
        }
        CHECK(committed, "a full-length truncated clipboard did not commit");
        CHECK(clip.len == CLIP_TEXT_MAX, "the cap was not filled exactly");
        CHECK(clip.total == 70000u, "the true length was not kept");
    }

    // Out of order, wrong seq, and a stale continuation all discard rather
    // than splice. A clipboard assembled around a hole is worse than none.
    clip_init(&clip);
    CHECK(!clip_wire_chunk(&clip, 2, 0, 20, 0, 6, "abc", 3), "staged");
    CHECK(!clip_wire_chunk(&clip, 2, 1, 21, 0, 6, "def", 3),
          "a chunk from a newer copy was spliced onto an older one");
    CHECK(!clip_have(&clip), "the spliced snapshot committed");

    clip_init(&clip);
    CHECK(!clip_wire_chunk(&clip, 3, 0, 22, 0, 9, "abc", 3), "staged");
    CHECK(!clip_wire_chunk(&clip, 3, 2, 22, 0, 9, "ghi", 3),
          "a chunk out of order was accepted");
    CHECK(!clip_have(&clip), "an out-of-order snapshot committed");

    clip_init(&clip);
    CHECK(!clip_wire_chunk(&clip, 2, 1, 23, 0, 6, "def", 3),
          "a continuation with nothing to continue was accepted");

    // Bounds, all of them refused rather than clamped.
    clip_init(&clip);
    CHECK(!clip_wire_chunk(&clip, CLIP_CHUNKS_MAX + 1, 0, 1, 0, 3, "abc", 3),
          "more chunks than the format allows");
    CHECK(!clip_wire_chunk(&clip, 1, 1, 1, 0, 3, "abc", 3),
          "an index at its own count");
    CHECK(!clip_wire_chunk(&clip, 1, 0, 1, 0, 0, "abc", 0),
          "an empty chunk inside a non-empty snapshot");
    CHECK(!clip_wire_chunk(&clip, 1, 0, 1, 0, 3, NULL, 3), "a null body");
    {
        static char over[CLIP_CHUNK_MAX + 1];
        memset(over, 'x', sizeof(over));
        CHECK(!clip_wire_chunk(&clip, 1, 0, 1, 0, sizeof(over), over,
                               sizeof(over)),
              "a chunk over the chunk cap");
    }
    // Control bytes are refused, not replaced. Every other decoder in this
    // program sanitizes; this one must not, because a clipboard silently
    // altered is a paste that does something else.
    CHECK(!clip_wire_chunk(&clip, 1, 0, 1, 0, 3, "a\tb", 3),
          "a tab reached the clipboard");
    CHECK(!clip_wire_chunk(&clip, 1, 0, 1, 0, 3, "a\x1b" "b", 3),
          "an escape reached the clipboard");
    CHECK(clip_wire_chunk(&clip, 1, 0, 1, 0, 3, "a\nb", 3),
          "the one control byte that should survive did not");

    // Empty and withheld are different answers and must stay different.
    clip_init(&clip);
    CHECK(clip_wire_chunk(&clip, 0, 0, 0, 0, 0, NULL, 0),
          "an empty frame did not commit");
    CHECK(clip_have(&clip) && !clip_pastable(&clip) && !clip_withheld(&clip),
          "an empty clipboard did not read as empty");
    CHECK(clip_wire_chunk(&clip, 0, 0, 0, CLIP_FLAG_WITHHELD, 0, NULL, 0),
          "a withheld frame did not commit");
    CHECK(clip_withheld(&clip),
          "a withheld copy read as an ordinary empty clipboard -- which is "
          "the state that invites copying the password again");

    // Losing the link forgets the clipboard. It is the most perishable thing
    // this device shows, and a stale one offered for pasting is worse than
    // none at all.
    clip_init(&clip);
    (void)clip_wire_chunk(&clip, 1, 0, 1, 0, 5, "hello", 5);
    clip_wire_reset(&clip);
    CHECK(!clip_have(&clip), "a dead link left a clipboard behind");
}

static void test_clip_wire_decoder(void)
{
    // proto_apply_clip, on its own.
    //
    // This exists because the first pass at these tests exercised clip.c's
    // checks and never this decoder's, and the two have independent copies of
    // the same rules -- deliberately, because the card must refuse a bad frame
    // before it reaches a buffer as well as after. Removing the control-byte
    // refusal here passed the whole suite, which is the same shape of gap as
    // testing the pieces and never the seam.
    unsigned count = 9, index = 9, seq = 9, flags = 9, body_len = 9;
    uint32_t total = 9;
    char body[CLIP_CHUNK_MAX + 1];
    proto_result_t r;

    // The empty frame, and the withheld one, which must not decode alike.
    const uint8_t empty[] = { 0, 0 };
    r = proto_apply_clip(&count, &index, &seq, &flags, &total,
                         body, sizeof(body), &body_len, empty, sizeof(empty));
    CHECK(r == PROTO_OK, "the empty frame was rejected: %s", proto_strerror(r));
    CHECK(count == 0 && flags == 0 && body_len == 0, "empty decoded wrong");

    const uint8_t withheld[] = { 0, CLIP_FLAG_WITHHELD };
    r = proto_apply_clip(&count, &index, &seq, &flags, &total, body,
                         sizeof(body), &body_len, withheld, sizeof(withheld));
    CHECK(r == PROTO_OK && count == 0 && flags == CLIP_FLAG_WITHHELD,
          "the withheld frame lost its flag, which is what makes it "
          "distinguishable from an empty clipboard");

    // A one-byte "empty" is not the empty frame: the flag byte is required, so
    // trailing-junk tolerance cannot let a later revision be misread.
    const uint8_t short_empty[] = { 0 };
    CHECK(proto_apply_clip(&count, &index, &seq, &flags, &total, body,
                           sizeof(body), &body_len, short_empty, 1) != PROTO_OK,
          "a one-byte empty frame was accepted");

    // A well-formed chunk, with the newline that has to survive.
    const uint8_t good[] = { 2, 1, 0x2A, 0x03, 0x00, 0x01, 0x11, 0x70,
                             3, 'a', CLIP_LF, 'b' };
    r = proto_apply_clip(&count, &index, &seq, &flags, &total, body,
                         sizeof(body), &body_len, good, sizeof(good));
    CHECK(r == PROTO_OK, "a good chunk was rejected: %s", proto_strerror(r));
    CHECK(count == 2 && index == 1 && seq == 0x2A && flags == 0x03,
          "the header fields did not decode");
    CHECK(total == 70000u, "u32 total decoded as %lu", (unsigned long)total);
    CHECK(body_len == 3 && body[0] == 'a' && body[1] == CLIP_LF &&
          body[2] == 'b', "the body did not survive the decoder");

    // Control bytes are refused, not replaced. Every other decoder in proto.c
    // sanitizes a hostile byte so a display buffer stays printable; this one
    // must not, because these bytes are what somebody is about to paste into a
    // prompt, and swapping one quietly produces text that does something else.
    static const uint8_t BAD[] = { '\t', 0x1b, 0x00, 0x7F, 0x80, 0xFF, '\r' };
    for (unsigned i = 0; i < sizeof(BAD); i++) {
        uint8_t frame[] = { 1, 0, 0, 0, 0, 0, 0, 3, 3, 'a', 0, 'b' };
        frame[10] = BAD[i];
        CHECK(proto_apply_clip(&count, &index, &seq, &flags, &total, body,
                               sizeof(body), &body_len,
                               frame, sizeof(frame)) != PROTO_OK,
              "byte 0x%02X reached the clipboard", BAD[i]);
    }

    // Structure, all refused rather than clamped.
    struct { const char *what; uint8_t frame[13]; uint16_t len; } bad[] = {
        { "a body length that does not match the payload",
          { 1, 0, 0, 0, 0, 0, 0, 3, 9, 'a', 'b', 'c' }, 12 },
        { "an index at its own count",
          { 1, 1, 0, 0, 0, 0, 0, 3, 3, 'a', 'b', 'c' }, 12 },
        { "more chunks than the format allows",
          { CLIP_CHUNKS_MAX + 1, 0, 0, 0, 0, 0, 0, 3, 3, 'a', 'b', 'c' }, 12 },
        { "a zero-length body inside a non-empty frame",
          { 1, 0, 0, 0, 0, 0, 0, 0, 0 }, 9 },
        { "a truncated header",
          { 1, 0, 0, 0, 0, 0, 0, 3 }, 8 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(proto_apply_clip(&count, &index, &seq, &flags, &total, body,
                               sizeof(body), &body_len,
                               bad[i].frame, bad[i].len) != PROTO_OK,
              "accepted %s", bad[i].what);
    }

    // A body larger than the caller's buffer is refused rather than written.
    // There is no MMU here; this is the check that matters most.
    {
        static uint8_t big[PROTO_CLIP_CHUNK_SIZE + CLIP_CHUNK_MAX];
        memset(big, 'x', sizeof(big));
        big[0] = 1; big[1] = 0; big[2] = 0; big[3] = 0;
        big[4] = big[5] = big[6] = 0; big[7] = CLIP_CHUNK_MAX;
        big[8] = CLIP_CHUNK_MAX;
        char small[8];
        CHECK(proto_apply_clip(&count, &index, &seq, &flags, &total, small,
                               sizeof(small), &body_len,
                               big, (uint16_t)sizeof(big)) != PROTO_OK,
              "a body larger than the destination was accepted");
    }
}

static void test_clip_presentation(void)
{
    static clip_t clip;
    static char lines[8][CLIP_LINE_COLS + 1];
    char summary[CLIP_LINE_COLS + 1];

    clip_init(&clip);
    clip_summary(&clip, summary, sizeof(summary));
    CHECK(strstr(summary, "nothing") != NULL,
          "an unreceived clipboard did not say so: %s", summary);

    // A withheld copy is named. Not blank, not "empty".
    (void)clip_wire_chunk(&clip, 0, 0, 0, CLIP_FLAG_WITHHELD, 0, NULL, 0);
    clip_summary(&clip, summary, sizeof(summary));
    CHECK(strstr(summary, "withheld") != NULL,
          "a withheld copy was not named: %s", summary);

    // A truncated one says both numbers, because "1.0K" alone reads as the
    // whole clipboard.
    clip_init(&clip);
    {
        static char big[CLIP_CHUNK_MAX];
        memset(big, 'x', sizeof(big));
        for (unsigned i = 0; i < CLIP_CHUNKS_MAX; i++) {
            const unsigned len = (i == CLIP_CHUNKS_MAX - 1)
                ? CLIP_TEXT_MAX - CLIP_CHUNK_MAX * (CLIP_CHUNKS_MAX - 1)
                : CLIP_CHUNK_MAX;
            (void)clip_wire_chunk(&clip, CLIP_CHUNKS_MAX, i, 1,
                                  CLIP_FLAG_TRUNCATED, 70000u, big, len);
        }
    }
    clip_summary(&clip, summary, sizeof(summary));
    CHECK(strstr(summary, "1.0K") != NULL && strstr(summary, "68.3K") != NULL,
          "a truncated clipboard did not say how much was left: %s", summary);
    CHECK(strlen(summary) <= CLIP_LINE_COLS, "the summary overran its row");

    // Wrapping: newlines break, over-long lines hard-wrap, and nothing is
    // invented -- what you read is what you would paste.
    clip_init(&clip);
    (void)clip_wire_chunk(&clip, 1, 0, 1, 0, 11, "one\ntwo\nabc", 11);
    unsigned n = clip_wrap(&clip, lines, 8);
    CHECK(n == 3, "three lines of text wrapped to %u", n);
    CHECK(strcmp(lines[0], "one") == 0 && strcmp(lines[1], "two") == 0 &&
          strcmp(lines[2], "abc") == 0, "the lines did not survive wrapping");

    clip_init(&clip);
    {
        static char wide[100];
        memset(wide, 'w', sizeof(wide));
        (void)clip_wire_chunk(&clip, 1, 0, 1, 0, sizeof(wide), wide,
                              sizeof(wide));
    }
    n = clip_wrap(&clip, lines, 8);
    CHECK(n == 3, "100 columns should be three lines at 42, got %u", n);
    CHECK(strlen(lines[0]) == CLIP_LINE_COLS &&
          strlen(lines[1]) == CLIP_LINE_COLS && strlen(lines[2]) == 100 - 84,
          "the hard wrap did not fall on the column boundary");
    // No line may ever overrun its row: that corrupts the screen it was added
    // to explain, which is the rule status.c is written under.
    for (unsigned i = 0; i < n; i++) {
        CHECK(strlen(lines[i]) <= CLIP_LINE_COLS, "line %u overran", i);
    }

    // The cap is honoured and the preview does not run off the end of the
    // caller's array.
    n = clip_wrap(&clip, lines, 2);
    CHECK(n == 2, "the line cap was not honoured: %u", n);

    // Which clipboards may be typed straight at the PTY.
    //
    // This is the predicate that keeps L-to-paste inside the rule rather than
    // an exception to it: text with no newline lands on the prompt line and
    // waits for a separate submit, so it has not executed on its own. Text
    // with a newline would submit at that newline, unread, so it has to go
    // through the composer and be looked at.
    clip_init(&clip);
    CHECK(!clip_paste_is_inert(&clip),
          "an empty clipboard offered itself for a direct paste");
    (void)clip_wire_chunk(&clip, 0, 0, 0, CLIP_FLAG_WITHHELD, 0, NULL, 0);
    CHECK(!clip_paste_is_inert(&clip), "a withheld copy offered itself");

    clip_init(&clip);
    (void)clip_wire_chunk(&clip, 1, 0, 1, 0, 11, "git status ", 11);
    CHECK(clip_paste_is_inert(&clip),
          "a single-line clipboard was refused a direct paste");

    clip_init(&clip);
    (void)clip_wire_chunk(&clip, 1, 0, 1, 0, 8, "one\ntwo!", 8);
    CHECK(!clip_paste_is_inert(&clip),
          "a clipboard with a newline in it would have been typed straight at "
          "the prompt, and would have submitted itself unread");

    // A trailing newline counts. It is the most likely shape -- copying a line
    // out of a terminal usually takes the newline with it -- and it is the one
    // that submits the instant it arrives.
    clip_init(&clip);
    (void)clip_wire_chunk(&clip, 1, 0, 1, 0, 4, "ls\n!", 4);
    CHECK(!clip_paste_is_inert(&clip), "a trailing newline was treated as inert");
}

static void test_project_page(void)
{
    // The page is composed into an ordinary grid and drawn with the terminal's
    // own painter.  That is what lets an extension draw on it, and it is why the
    // things worth asserting here are the same two things the terminal has to
    // get right: never write outside the grid, and never leave the last page
    // showing through the gaps in this one.
    static project_t project;
    static grid_t g;

    project_init(&project);
    CHECK(project_tab(&project) == PROJECT_TAB_NOW,
          "the page did not open on the tab you glance at");

    // Every tab has a name that fits a tile, and no two are the same.
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        const char *name = project_tab_name((project_tab_t)i);
        CHECK(name && name[0], "tab %d has no name", i);
        CHECK(strlen(name) <= 6,
              "tab %d's name \"%s\" does not fit its tile", i, name);
        for (int k = 0; k < i; k++) {
            CHECK(strcmp(name, project_tab_name((project_tab_t)k)) != 0,
                  "two tabs are both called \"%s\"", name);
        }
    }

    // Stepping wraps both ways and visits everything.
    int seen[PROJECT_TAB_COUNT];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        seen[project_tab(&project)] = 1;
        project_tab_step(&project, 1);
    }
    CHECK(project_tab(&project) == PROJECT_TAB_NOW, "stepping did not wrap");
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        CHECK(seen[i], "stepping never reached tab %d", i);
    }
    project_tab_step(&project, -1);
    CHECK(project_tab(&project) == PROJECT_TAB_COUNT - 1,
          "stepping backwards off the front did not wrap to the end");
    project_tab_step(&project, 0);
    CHECK(project_tab(&project) == PROJECT_TAB_COUNT - 1,
          "a zero step moved the tab");

    // The tab strip's rects are what both the drawing and the touch test use,
    // so they have to tile the screen without overlapping or running off it.
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        int x, y, w, h;
        CHECK(project_tab_rect((project_tab_t)i, &x, &y, &w, &h),
              "tab %d has no rect", i);
        CHECK(x >= 0 && x + w <= 256 && y >= 0 && y + h <= 192,
              "tab %d's rect leaves the screen", i);
        CHECK(project_tab_hit(x + w / 2, y + h / 2) == (project_tab_t)i,
              "a touch in the middle of tab %d hit something else", i);
        if (i > 0) {
            int px, py, pw, ph;
            (void)project_tab_rect((project_tab_t)(i - 1), &px, &py, &pw, &ph);
            CHECK(x >= px + pw, "tabs %d and %d overlap", i - 1, i);
        }
    }
    CHECK(project_tab_hit(-1, -1) == PROJECT_TAB_COUNT,
          "a touch off the screen picked a tab");
    CHECK(project_tab_hit(128, 191) == PROJECT_TAB_COUNT,
          "a touch on the extension's floor picked a tab");

    // Rendering: a long value is clipped, never written past the row, and the
    // page before it does not show through.
    project_tab_set(&project, PROJECT_TAB_NOW);
    char wide[PROJECT_VALUE_MAX + 1];
    memset(wide, 'x', sizeof(wide) - 1);
    wide[sizeof(wide) - 1] = '\0';
    for (unsigned i = 0; i < PROJECT_ROWS_MAX + 4u; i++) {
        project_add(&project, PROJECT_TAB_NOW, "label", wide, 9);
    }
    memset(&g, 0xAB, sizeof(g));
    project_render(&project, &g);
    // One claim, one assertion: the first cell that broke it, not one CHECK
    // per cell.  A thousand assertions restating the same thing inflate the
    // suite's count into something that looks like evidence and is not.
    int unprintable_at = -1;
    uint8_t unprintable = 0;
    for (int i = 0; i < PROTO_CELLS && unprintable_at < 0; i++) {
        if (g.cells[i].glyph < 0x20 || g.cells[i].glyph > 0x7E) {
            unprintable_at = i;
            unprintable = g.cells[i].glyph;
        }
    }
    CHECK(unprintable_at < 0,
          "cell %d of the rendered page is not printable (0x%02X) -- an "
          "unwritten cell renders as '?'", unprintable_at, unprintable);
    // The last row stays clear: it is the one an extension is most likely to
    // be drawing on, and a page that fills it every second would evict it.
    int last_row_blank = 1;
    for (int c = 0; c < PROTO_COLS; c++) {
        if (g.cells[(PROTO_ROWS - 1) * PROTO_COLS + c].glyph != ' ') {
            last_row_blank = 0;
        }
    }
    CHECK(last_row_blank, "the page wrote into its own bottom row");

    // And switching tabs really does blank what the last one left.
    project_tab_set(&project, PROJECT_TAB_WATCH);
    project_render(&project, &g);
    int leftovers = 0;
    for (int i = 0; i < PROTO_CELLS; i++) {
        if (g.cells[i].glyph == 'x') leftovers++;
    }
    CHECK(leftovers == 0,
          "%d cells of the previous tab showed through the next one",
          leftovers);
}

static void test_project_actions(void)
{
    // The DO tab's buttons.  Two claims matter more than the rest: the
    // client's own four are always reachable, and a placeholder row from a
    // server that is not reporting must never become a button -- typing "this
    // server does not report it" at a shell would be a joke at somebody's
    // expense.
    static project_t project;
    project_action_t actions[PROJECT_ACTIONS_MAX];

    project_init(&project);
    unsigned n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    CHECK(n >= 4u, "the client's own actions were not offered (%u)", n);
    for (unsigned i = 0; i < n; i++) {
        CHECK(actions[i].label[0] && actions[i].body[0],
              "action %u has an empty label or body", i);
        CHECK(!actions[i].from_server,
              "a fresh page attributed action %u to the server", i);
    }
    const unsigned generic_count = n;

    // A page the server has not claimed carries a placeholder row.  It is not
    // an action, and the merge must not treat it as one.
    project_clear(&project, PROJECT_TAB_DO);
    project_add(&project, PROJECT_TAB_DO, "actions",
                "this server does not report it", 0);
    n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    CHECK(n == generic_count,
          "an unclaimed tab's placeholder became a button");
    for (unsigned i = 0; i < n; i++) {
        CHECK(strcmp(actions[i].body, "this server does not report it") != 0,
              "the placeholder text was offered as a command to type");
    }

    // Once the server claims the tab, its rows are the actions, and they come
    // first -- they are the ones that know what you are working on.
    project_init(&project);
    CHECK(project_wire_row(&project, 2, 0, PROJECT_TAB_DO, 9, "test",
                           "npm run test") == 0, "snapshot committed early");
    CHECK(project_wire_row(&project, 2, 1, PROJECT_TAB_DO, 9, "build",
                           "make build") == 1, "snapshot did not commit");
    n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    CHECK(n == generic_count + 2u,
          "the merged list has %u actions, expected %u", n, generic_count + 2u);
    CHECK(actions[0].from_server && strcmp(actions[0].body, "npm run test") == 0,
          "the server's actions did not come first");
    CHECK(!actions[n - 1].from_server,
          "the client's own actions were pushed out of the list");

    // Overflow is reported, not swallowed.
    project_init(&project);
    const unsigned many = PROJECT_ACTIONS_MAX + 3u;
    for (unsigned i = 0; i < many; i++) {
        char label[8];
        (void)snprintf(label, sizeof(label), "a%u", i);
        const int done = project_wire_row(&project, many, i, PROJECT_TAB_DO, 0,
                                          label, "echo x");
        CHECK(done == (i == many - 1u), "snapshot completed at the wrong row");
    }
    n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    CHECK(n == PROJECT_ACTIONS_MAX, "the merge overran its own bound (%u)", n);
    CHECK(project_actions_dropped(&project) > 0,
          "a truncated action list did not say so");

    // Selection wraps over whatever the merge produced, never past it.
    project_init(&project);
    n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    for (unsigned i = 0; i < n + 2u; i++) {
        project_action_select(&project, 1);
        CHECK(project_action_selected(&project) < n,
              "the selection left the list");
    }
    project_action_select(&project, -1);
    CHECK(project_action_selected(&project) < n,
          "stepping backwards left the list");

    // The buttons tile, do not overlap, and end above the extension's floor.
    for (unsigned i = 0; i < PROJECT_ACTIONS_MAX; i++) {
        int x, y, w, h;
        CHECK(project_action_rect(i, &x, &y, &w, &h), "button %u has no rect", i);
        CHECK(x >= 0 && x + w <= 256 && y >= PROJECT_TAB_STRIP_Y +
              PROJECT_TAB_STRIP_H && y + h <= 160,
              "button %u overlaps the tab strip or the extension's floor", i);
        CHECK(project_action_hit(x + w / 2, y + h / 2) == i,
              "a touch in the middle of button %u hit something else", i);
        for (unsigned k = 0; k < i; k++) {
            int px, py, pw, ph;
            (void)project_action_rect(k, &px, &py, &pw, &ph);
            const int apart = x >= px + pw || px >= x + w ||
                              y >= py + ph || py >= y + h;
            CHECK(apart, "buttons %u and %u overlap", k, i);
        }
    }
    CHECK(project_action_hit(128, 180) == PROJECT_ACTIONS_MAX,
          "a touch on the extension's floor hit a button");
}

static void test_project_wire(void)
{
    // Everything here is attacker-influenced: the link has no per-message
    // integrity, so anyone who can inject can send a row. A row therefore has
    // to be unable to do anything but be drawn -- and a malformed one has to
    // be unable to take the terminal down with it.
    static project_t project;
    unsigned count = 0, index = 0, tab = 0, fg = 0;
    char label[PROJECT_LABEL_MAX + 1];
    char value[PROJECT_VALUE_MAX + 1];
    uint8_t buf[128];

    // The empty snapshot: exactly one byte, and only one byte.
    buf[0] = 0;
    CHECK(proto_apply_project(&count, &index, &tab, &fg, label, sizeof(label),
                              value, sizeof(value), buf, 1) == PROTO_OK,
          "the empty project snapshot was refused");
    CHECK(count == 0, "the empty snapshot decoded to a count of %u", count);
    buf[1] = 0;
    CHECK(proto_apply_project(&count, &index, &tab, &fg, label, sizeof(label),
                              value, sizeof(value), buf, 2) != PROTO_OK,
          "trailing junk after an empty snapshot was accepted");

    // A good row.
    const uint8_t good[] = { 2, 0, PROJECT_TAB_NOW, 9, 6, 4,
                             'b','r','a','n','c','h', 'm','a','i','n' };
    memcpy(buf, good, sizeof(good));
    CHECK(proto_apply_project(&count, &index, &tab, &fg, label, sizeof(label),
                              value, sizeof(value), buf,
                              (uint16_t)sizeof(good)) == PROTO_OK,
          "a well formed project row was refused");
    CHECK(count == 2 && index == 0 && tab == PROJECT_TAB_NOW && fg == 9 &&
          strcmp(label, "branch") == 0 && strcmp(value, "main") == 0,
          "a project row decoded to the wrong fields");

    // Every structural lie, refused.  Each is a distinct claim: a length that
    // does not match the payload, an index past the count, a colour that is
    // not a palette entry, and a truncated header.
    struct { const char *what; uint8_t b[16]; uint16_t len; } BAD[] = {
        { "a label length longer than the payload",
          { 2, 0, 0, 9, 200, 4, 'b','r','a','n','c','h','m','a','i','n' }, 16 },
        { "a value length longer than the payload",
          { 2, 0, 0, 9, 6, 200, 'b','r','a','n','c','h','m','a','i','n' }, 16 },
        { "an index at or past the count",
          { 2, 2, 0, 9, 6, 4, 'b','r','a','n','c','h','m','a','i','n' }, 16 },
        { "a colour outside the palette",
          { 2, 0, 0, 99, 6, 4, 'b','r','a','n','c','h','m','a','i','n' }, 16 },
        { "a count past the wire maximum",
          { 250, 0, 0, 9, 6, 4, 'b','r','a','n','c','h','m','a','i','n' }, 16 },
        { "a header cut short",
          { 2, 0, 0, 9, 6 }, 5 },
    };
    for (unsigned i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        CHECK(proto_apply_project(&count, &index, &tab, &fg,
                                  label, sizeof(label), value, sizeof(value),
                                  BAD[i].b, BAD[i].len) != PROTO_OK,
              "a project row with %s was accepted", BAD[i].what);
    }

    // An unprintable byte is a presentation problem, not a reason to drop a
    // working terminal -- same judgement the session name gets.
    const uint8_t nasty[] = { 1, 0, 0, 0, 3, 3, 'a', 0x01, 'c', 0x7F, 'x', 'y' };
    CHECK(proto_apply_project(&count, &index, &tab, &fg, label, sizeof(label),
                              value, sizeof(value), nasty,
                              (uint16_t)sizeof(nasty)) == PROTO_OK,
          "an unprintable byte tore down the connection instead of being drawn");
    CHECK(strcmp(label, "a?c") == 0 && strcmp(value, "?xy") == 0,
          "unprintable bytes were not sanitized (label %s value %s)",
          label, value);

    // Staging: a snapshot is committed whole or not at all.
    project_init(&project);
    CHECK(!project_tab_from_server(&project, PROJECT_TAB_NOW),
          "a fresh page already thought the server owned a tab");
    CHECK(project_wire_row(&project, 2, 0, PROJECT_TAB_NOW, 9, "branch",
                           "main") == 0,
          "an incomplete snapshot was committed");
    CHECK(!project_tab_from_server(&project, PROJECT_TAB_NOW),
          "half a snapshot handed the tab to the server");
    CHECK(project_wire_row(&project, 2, 1, PROJECT_TAB_DIFF, 0, "working",
                           "clean") == 1,
          "a complete snapshot did not commit");
    CHECK(project_tab_from_server(&project, PROJECT_TAB_NOW) &&
          project_tab_from_server(&project, PROJECT_TAB_DIFF),
          "a committed snapshot did not claim its tabs");
    // ...and only the tabs it spoke about.  The rest stay local.
    CHECK(!project_tab_from_server(&project, PROJECT_TAB_HEALTH),
          "a snapshot claimed a tab it said nothing about");

    // A row out of order discards the snapshot rather than applying half of it.
    CHECK(project_wire_row(&project, 3, 0, PROJECT_TAB_NOW, 0, "a", "1") == 0,
          "the first row of a three-row snapshot committed early");
    CHECK(project_wire_row(&project, 3, 2, PROJECT_TAB_NOW, 0, "c", "3") == 0,
          "a row that skipped its predecessor was accepted");
    CHECK(project_wire_row(&project, 3, 1, PROJECT_TAB_NOW, 0, "b", "2") == 0,
          "a discarded snapshot carried on being staged");

    // And the link going away gives the tabs back, because a branch name from
    // a server that stopped talking is worse than saying nobody is reporting.
    project_wire_reset(&project);
    CHECK(!project_tab_from_server(&project, PROJECT_TAB_NOW) &&
          !project_tab_from_server(&project, PROJECT_TAB_DIFF),
          "a dead link left the server owning the page");
}

static void test_status_mode_toggle(void)
{
    status_mode_set(SCREEN_MODE_CONTROL);
    CHECK(status_mode_get() == SCREEN_MODE_CONTROL, "CONTROL should be selectable");
    status_mode_next();
    CHECK(status_mode_get() == SCREEN_MODE_KEYS, "CONTROL -> KEYS");
    status_mode_next();
    CHECK(status_mode_get() == SCREEN_MODE_STATUS, "KEYS -> STATUS");
    status_mode_next();
    CHECK(status_mode_get() == SCREEN_MODE_PROJECT, "STATUS -> PROJECT");

    // The rest of the cycle is checked against the enum rather than written
    // out, because the failure this guards against is a mode being added to
    // the cycle and then never reachable by the one button that walks it.
    // Every cycled value must be visited exactly once and it must come back to
    // where it started.
    status_mode_set(SCREEN_MODE_CONTROL);
    int seen[SCREEN_MODE_COUNT];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < SCREEN_MODE_CYCLE_COUNT; i++) {
        const int mode = (int)status_mode_get();
        CHECK(mode >= 0 && mode < SCREEN_MODE_CYCLE_COUNT,
              "SELECT reached mode %d, which is not on the cycle", mode);
        CHECK(!seen[mode], "SELECT visited mode %d twice in one cycle", mode);
        seen[mode] = 1;
        status_mode_next();
    }
    CHECK(status_mode_get() == SCREEN_MODE_CONTROL,
          "a full cycle of SELECT did not come back to CONTROL");
    for (int i = 0; i < SCREEN_MODE_CYCLE_COUNT; i++) {
        CHECK(seen[i], "SELECT never reached mode %d", i);
    }

    // SETTINGS is deliberately off the cycle: it has its own tile, because
    // four presses of a cycling button is not a way to go somewhere on
    // purpose.  Asserted rather than implied -- if a later change puts it back
    // on, that is a decision somebody should have to make here first.
    CHECK(SCREEN_MODE_CYCLE_COUNT == SCREEN_MODE_SETTINGS,
          "the cycle should stop before SETTINGS");
    status_mode_set(SCREEN_MODE_SETTINGS);
    status_mode_next();
    CHECK(status_mode_get() == SCREEN_MODE_CONTROL,
          "SELECT from an off-cycle mode should land on CONTROL, got %d",
          (int)status_mode_get());

    // X backs out, from every mode, in one press -- and reports whether it
    // had anything to do, which is what keeps it from eating Escape on the
    // screen you dismiss permission prompts from.
    for (int i = 0; i < SCREEN_MODE_COUNT; i++) {
        status_mode_set((screen_mode_t)i);
        // And every mode is settable, so a jump-straight-to helper cannot be
        // left behind by a widened enum either.
        CHECK((int)status_mode_get() == i, "mode %d was not settable", i);
        const int backed = status_mode_back();
        CHECK(status_mode_get() == SCREEN_MODE_CONTROL,
              "X from mode %d did not reach CONTROL", i);
        CHECK(backed == (i != SCREEN_MODE_CONTROL),
              "X from mode %d reported %d", i, backed);
        CHECK(!status_mode_back(),
              "a second X on CONTROL claimed it navigated");
    }
    status_mode_set(SCREEN_MODE_CONTROL);
}

static void test_status_view_position(void)
{
    char line[LOG_COLS + 1];

    // At the live edge, or with nothing to scroll, the line says nothing.
    status_view_pos_clear();
    status_format_view_pos(line);
    CHECK(strlen(line) == LOG_COLS, "unknown view line not padded");
    CHECK(line[0] == ' ', "an unknown position printed something: '%s'", line);

    status_view_pos_set(0, 0);
    status_format_view_pos(line);
    CHECK(line[0] == ' ', "no history printed something: '%s'", line);
    unsigned back = 99, available = 99;
    CHECK(status_view_pos_get(&back, &available), "known position getter failed");
    CHECK(back == 0 && available == 0, "zero position getter wrong");

    status_view_pos_set(0, 40);
    status_format_view_pos(line);
    CHECK(line[0] == ' ', "the live edge printed something: '%s'", line);

    status_view_pos_set(12, 40);
    status_format_view_pos(line);
    CHECK(strlen(line) == LOG_COLS, "known view line not padded");
    CHECK(strstr(line, "12 of 40 lines up") && strstr(line, "LIVE"),
          "a scrolled-back view does not say how to return: '%s'", line);

    status_view_pos_set(65535, 65535);
    status_format_view_pos(line);
    CHECK(strlen(line) == LOG_COLS, "maximum view line overran");
    status_view_pos_clear();
}

static void test_status_scroll_regime(void)
{
    char line[LOG_COLS + 1];

    status_view_pos_clear();
    status_scroll_regime_clear();
    status_scroll_regime_set(1);
    status_view_pos_set(12, 40);
    status_format_view_pos(line);
    CHECK(line[0] == ' ',
          "an application scrolling itself still showed a position: '%s'", line);
    CHECK(strlen(line) == LOG_COLS, "app regime line not padded");

    status_scroll_regime_clear();
    status_format_view_pos(line);
    CHECK(strstr(line, "12 of 40") != NULL,
          "regime clear should return the position: '%s'", line);
    status_view_pos_clear();
}

static void test_status_counters(void)
{
    char l1[LOG_COLS + 1], l2[LOG_COLS + 1];
    status_format_counters(123456, 7, 8, 99, 0x0A5, 0x1FF, l1, l2);

    CHECK(strlen(l1) == LOG_COLS, "line1 not padded to LOG_COLS (len %zu)", strlen(l1));
    CHECK(strlen(l2) == LOG_COLS, "line2 not padded to LOG_COLS (len %zu)", strlen(l2));
    CHECK(strstr(l1, "in 123456 B") != NULL, "line1 missing bytes in: '%s'", l1);
    CHECK(strstr(l1, "7 msgs") != NULL, "line1 missing msgs: '%s'", l1);
    CHECK(strstr(l1, "sent 8") != NULL, "line1 missing sent: '%s'", l1);
    CHECK(strncmp(l2, "loop 99", 7) == 0, "line2 missing loop counter: '%s'", l2);
    CHECK(strstr(l2, "raw 0A5") != NULL, "line2 missing raw: '%s'", l2);
    CHECK(strstr(l2, "lib 1FF") != NULL, "line2 missing lib: '%s'", l2);

    // A huge counter must be truncated, never overflow the 43-byte buffers.
    status_format_counters(0xFFFFFFFFul, 0xFFFFFFFFul, 0xFFFFFFFFul,
                           0xFFFFFFFFul, 0xFFF, 0xFFF, l1, l2);
    CHECK(strlen(l1) == LOG_COLS, "big line1 overran: len %zu", strlen(l1));
    CHECK(strlen(l2) == LOG_COLS, "big line2 overran: len %zu", strlen(l2));
}

static void test_status_link(void)
{
    char line[LOG_COLS + 1];
    // DSi mode: -55 dBm arrives as the unsigned byte 201.
    status_link_host("MacBook");
    status_link_set(23, 1, 12, 1, 1, 1, 201, 1, 0);
    status_format_link(line);
    CHECK(strncmp(line, "MacBook", 7) == 0, "host not first: %s", line);
    CHECK(strstr(line, "23ms") != NULL, "RTT missing: %s", line);
    CHECK(strstr(line, "bat 3/4+") != NULL, "DSi battery missing: %s", line);
    CHECK(status_battery_bars(15) == 4 && status_battery_bars(11) == 3 &&
              status_battery_bars(7) == 2 && status_battery_bars(3) == 1 &&
              status_battery_bars(1) == 0 && status_battery_bars(0) == 0,
          "battery readings are not the DSi menu's bars");
    CHECK(strstr(line, "wifi 4/4") != NULL, "-55 dBm is not full bars: %s", line);
    CHECK(strstr(line, "MUTE") == NULL, "MUTE shown while unmuted: %s", line);
    CHECK(status_wifi_bars(256 - 80, 1) == 1 && status_wifi_bars(256 - 70, 1) == 2 &&
              status_wifi_bars(256 - 60, 1) == 3,
          "the dBm thresholds are wrong");
    CHECK(status_wifi_bars(148, 0) == -1, "DS mode invented bars");

    status_link_host("");
    status_link_set(0, 0, 3, 1, 0, 0, 0, 0, 1);
    status_format_link(line);
    CHECK(strncmp(line, "--ms", 4) == 0, "unknown RTT missing: %s", line);
    CHECK(strstr(line, "bat LOW") != NULL, "DS fallback missing: %s", line);
    CHECK(strstr(line, "wifi --") != NULL, "unknown RSSI missing: %s", line);
    CHECK(strstr(line, "MUTE") != NULL && status_muted(), "mute missing: %s", line);
    status_link_host("A-HOST-NAME-THAT-IS-LONG");
    status_link_set(65535, 1, 15, 1, 1, 1, 201, 1, 1);
    status_format_link(line);
    CHECK(strlen(line) == LOG_COLS, "a long link line overran: %zu", strlen(line));
    status_link_host("");
}

// ------------------------------------------------------------- composer ----

static void test_composer_bounds_and_editing(void)
{
    composer_t c;
    composer_init(&c);
    composer_open(&c);
    CHECK(c.mode == COMPOSER_EDIT && c.len == 0, "composer did not open empty");
    CHECK(!composer_append(&c, 0x1b), "escape entered local body");
    CHECK(composer_append(&c, 'a'), "printable byte rejected");
    CHECK(composer_append(&c, '\n'), "newline rejected");
    CHECK(composer_backspace(&c) && c.len == 1, "backspace failed");
    for (size_t i = c.len; i < COMPOSER_MAX; i++) CHECK(composer_append(&c, 'x'), "append %zu failed", i);
    CHECK(c.len == COMPOSER_MAX && c.body[COMPOSER_MAX] == 0, "full buffer not terminated");
    CHECK(!composer_append(&c, 'y'), "byte 256 was accepted");
    composer_cancel(&c);
    CHECK(c.mode == COMPOSER_CLOSED, "cancel failed");
}

static void test_composer_send_frames(void)
{
    const size_t lengths[] = {0, 1, 63, 64, 65, 128, 255};
    for (size_t k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        composer_t c;
        composer_open(&c);
        for (size_t i = 0; i < lengths[k]; i++) composer_append(&c, (uint8_t)('a' + i % 26));
        const size_t frames = composer_frame_count(&c);
        size_t body_seen = 0;
        for (size_t i = 0; i < frames; i++) {
            const uint8_t *data = NULL;
            size_t n = 0;
            CHECK(composer_frame(&c, i, &data, &n), "missing frame %zu", i);
            if (i + 1 == frames) CHECK(n == 1 && data[0] == '\r', "final frame is not lone CR");
            else {
                CHECK(n >= 1 && n <= PROTO_MAX_KEY_PAYLOAD, "body frame length %zu", n);
                body_seen += n;
            }
        }
        CHECK(body_seen == lengths[k], "planned %zu body bytes, saw %zu", lengths[k], body_seen);
    }
    // A completely full buffer, chunked. Derived from COMPOSER_MAX rather
    // than written out, because the buffer grew from 255 to 1024 to hold a
    // pasted clipboard and a test that hardcodes the old arithmetic fails for
    // the wrong reason -- it says the chunking broke when what changed was the
    // capacity.
    composer_t full;
    composer_open(&full);
    for (size_t i = 0; i < COMPOSER_MAX; i++) composer_append(&full, 'x');
    const size_t whole = COMPOSER_MAX / PROTO_MAX_KEY_PAYLOAD;
    const size_t tail = COMPOSER_MAX % PROTO_MAX_KEY_PAYLOAD;
    const size_t expect = whole + (tail ? 1 : 0) + 1;   // + the lone CR
    CHECK(composer_frame_count(&full) == expect,
          "%d bytes should use %zu frames, got %zu", COMPOSER_MAX, expect,
          composer_frame_count(&full));
    size_t seen = 0;
    for (size_t i = 0; i < expect; i++) {
        const uint8_t *data;
        size_t n;
        CHECK(composer_frame(&full, i, &data, &n), "missing frame %zu", i);
        if (i + 1 == expect) {
            CHECK(n == 1 && data[0] == '\r', "the last frame is not a lone CR");
        } else {
            CHECK(n >= 1 && n <= PROTO_MAX_KEY_PAYLOAD,
                  "frame %zu is %zu bytes", i, n);
            seen += n;
        }
    }
    CHECK(seen == COMPOSER_MAX, "a full buffer planned %zu of %d bytes", seen,
          COMPOSER_MAX);
}

static void test_snippets_parser(void)
{
    snippets_t snippets;
    char err[96] = "";
    const char *text = "# local prompt text only\r\nreview=Review this change.\r\n"
                       "multi=first\\nsecond\\t\\\\done\n";
    CHECK(snippets_parse(text, &snippets, err, sizeof(err)), "parse failed: %s", err);
    CHECK(snippets.count == 2, "snippet count %d", snippets.count);
    CHECK(strcmp(snippets.items[0].label, "review") == 0, "label wrong");
    CHECK(strcmp(snippets.items[1].body, "first\nsecond\t\\done") == 0, "escapes wrong");
    CHECK(!snippets_parse("dup=a\ndup=b\n", &snippets, err, sizeof(err)), "duplicate accepted");
    CHECK(!snippets_parse("missing separator\n", &snippets, err, sizeof(err)), "malformed accepted");
}

// The example we ship must parse. It is the only documentation of the format
// that anyone actually copies, and a malformed one sends whoever tries it
// straight to a parse error on a handheld with no keyboard. run_tests.sh cds
// to dsclient/, so the repo root is one level up.
static void test_shipped_snippets_example_parses(void)
{
    snippets_t snippets;
    char err[96] = "";
    CHECK(snippets_load("../dsterm.snippets.example", &snippets, err, sizeof(err)),
          "shipped example does not parse: %s", err);
    // Copied as it is, it adds nothing: the examples are commented out.
    CHECK(snippets.count == 0, "the example enables %d snippets", snippets.count);

    // Each example, uncommented the way the file says to ("remove the #"),
    // has to be a snippet the card accepts.
    FILE *in = fopen("../dsterm.snippets.example", "r");
    char path[64];
    snprintf(path, sizeof(path), "/tmp/dsterm-snippets-%ld", (long)getpid());
    FILE *out = fopen(path, "w");
    CHECK(in && out, "cannot copy the example");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return;
    }
    char line[512];
    int examples = 0;
    while (fgets(line, sizeof(line), in)) {
        const char *eq = strchr(line, '=');
        if (strncmp(line, "# ", 2) == 0 && eq && strchr(line, ' ') == line + 1
            && strcspn(line + 2, " =") == (size_t)(eq - line - 2)) {
            fputs(line + 2, out);
            examples++;
        }
    }
    fclose(in);
    fclose(out);
    CHECK(examples > 0, "the example has no examples");
    CHECK(snippets_load(path, &snippets, err, sizeof(err)),
          "an uncommented example does not parse: %s", err);
    CHECK(snippets.count == examples, "%d of %d examples loaded",
          snippets.count, examples);
    for (int i = 0; i < snippets.count; i++) {
        CHECK(snippets.items[i].label[0] && snippets.items[i].body[0],
              "example %d is missing a label or a body", i);
    }
    remove(path);
}

// ------------------------------------------------------- pushed snippets --

// One chunk of a pushed set: u8 index, u8 total, u8 label_len, label,
// u16 body_len, body (docs/design/SNIPPETS_PUSH.md). Every field is off the wire,
// so every limit is asserted before anything is copied.
static void test_snippet_proto_apply(void)
{
    unsigned index = 99, total = 99;
    char label[SNIPPET_LABEL_MAX + 1] = "";
    char body[SNIPPET_BODY_MAX + 1] = "";
    size_t label_len = 0, body_len = 0;

    // A valid chunk round-trips: index 0 of 1, label "yes", body "hi".
    const uint8_t ok[] = { 0x00, 0x01, 0x03, 'y', 'e', 's', 0x00, 0x02, 'h', 'i' };
    CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                              body, sizeof(body), &body_len, ok, sizeof(ok)) == PROTO_OK,
          "valid chunk rejected");
    CHECK(index == 0 && total == 1, "index/total wrong: %u/%u", index, total);
    CHECK(label_len == 3 && memcmp(label, "yes", 3) == 0, "label wrong");
    CHECK(body_len == 2 && memcmp(body, "hi", 2) == 0, "body wrong");

    // Too short to hold the five-byte length header.
    CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                              body, sizeof(body), &body_len, ok, 4) == PROTO_ERR_PAYLOAD,
          "short payload accepted");

    // Label length 0 and 16 are both out of contract (1..15).
    {
        const uint8_t b0[] = { 0x00, 0x01, 0x00, 0x00, 0x00 };
        const uint8_t b16[] = { 0x00, 0x01, 0x10, 0x00, 0x00 };
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, b0, sizeof(b0)) == PROTO_ERR_PAYLOAD,
              "empty label accepted");
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, b16, sizeof(b16)) == PROTO_ERR_PAYLOAD,
              "16-char label accepted");
    }

    // Body length 256 exceeds COMPOSER_MAX.
    {
        const uint8_t b[] = { 0x00, 0x01, 0x01, 'x', 0x01, 0x00 };
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, b, sizeof(b)) == PROTO_ERR_PAYLOAD,
              "256-byte body accepted");
    }

    // Declared lengths must equal the payload length exactly.
    {
        const uint8_t b[] = { 0x00, 0x01, 0x03, 'y', 'e', 's', 0x00, 0x02, 'h' };
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, b, sizeof(b)) == PROTO_ERR_PAYLOAD,
              "length mismatch accepted");
    }

    // index >= total; total 0 and total 9.
    {
        const uint8_t bad[] = { 0x01, 0x01, 0x01, 'x', 0x00, 0x00 };
        const uint8_t t0[] = { 0x00, 0x00, 0x01, 'x', 0x00, 0x00 };
        const uint8_t t9[] = { 0x00, 0x09, 0x01, 'x', 0x00, 0x00 };
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, bad, sizeof(bad)) == PROTO_ERR_PAYLOAD,
              "index >= total accepted");
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, t0, sizeof(t0)) == PROTO_ERR_PAYLOAD,
              "total 0 accepted");
        CHECK(proto_apply_snippet(&index, &total, label, sizeof(label), &label_len,
                                  body, sizeof(body), &body_len, t9, sizeof(t9)) == PROTO_ERR_PAYLOAD,
              "total 9 accepted");
    }

    // The caller's buffer caps are respected as an additional bound.
    {
        char small[2];
        const uint8_t b[] = { 0x00, 0x01, 0x03, 'y', 'e', 's', 0x00, 0x00 };
        CHECK(proto_apply_snippet(&index, &total, small, sizeof(small), &label_len,
                                  body, sizeof(body), &body_len, b, sizeof(b)) == PROTO_ERR_PAYLOAD,
              "label over caller cap accepted");
    }
}

static void test_snippet_frame_accepted_by_reader(void)
{
    // Same family as VIEW_POS and MODE: the server sends MSG_SNIPPETS only
    // to a card that announced "snippets" in its HELLO, so an unknown type
    // here halts the card on the first chunk. It must reach the application.
    const uint8_t raw[] = { 'D', 'S', 1, MSG_SNIPPETS, 0x00, 0x0A,
                            0x00, 0x01, 0x03, 'y', 'e', 's', 0x00, 0x02, 'h', 'i' };
    framereader_t fr;
    framereader_init(&fr);
    CHECK(framereader_feed(&fr, raw, sizeof(raw)) == PROTO_OK, "SNIPPETS feed failed");

    msg_type_t t;
    const uint8_t *p;
    uint16_t len;
    const proto_result_t r = framereader_next(&fr, &t, &p, &len);
    CHECK(r == PROTO_OK, "SNIPPETS frame rejected: %s", proto_strerror(r));
    if (r != PROTO_OK) return;
    CHECK(t == MSG_SNIPPETS, "wrong type %d", t);
    CHECK(len == 10 && p[0] == 0x00 && p[3] == 'y' && p[5] == 's',
          "SNIPPETS payload mangled");
}

// The partial-set rule: apply only on completion, replace only then, and let
// a bad chunk discard the whole in-flight push -- state included, so the next
// push can accumulate from its first chunk.
static void test_snippets_apply_pushed(void)
{
    snippets_t s;
    char err[96] = "";

    // Complete, out of order: the card list stands until the last chunk.
    CHECK(snippets_parse("one=card one\ntwo=card two\n", &s, err, sizeof(err)),
          "seed parse failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 1, 2, "b", 1, "second", 6) == 0,
          "first chunk of two should accumulate");
    CHECK(s.count == 2 && strcmp(s.items[0].label, "one") == 0,
          "list changed before the set completed");
    CHECK(snippets_apply_pushed(&s, 0, 2, "a", 1, "first", 5) == 1,
          "set did not complete");
    CHECK(s.count == 2, "count after completion %d", s.count);
    CHECK(strcmp(s.items[0].label, "a") == 0 && strcmp(s.items[0].body, "first") == 0,
          "item 0 not replaced");
    CHECK(strcmp(s.items[1].label, "b") == 0 && strcmp(s.items[1].body, "second") == 0,
          "item 1 not replaced");

    // A single-chunk set completes at once, and an empty body is legal.
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)), "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 1, "empty", 5, "", 0) == 1,
          "single chunk did not complete");
    CHECK(s.count == 1 && strcmp(s.items[0].label, "empty") == 0 && s.items[0].body[0] == 0,
          "empty body not applied");

    // Duplicate index discards the in-flight push, leaves the list alone, and
    // does not poison the next push.
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)), "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 2, "a", 1, "first", 5) == 0, "partial failed");
    CHECK(snippets_apply_pushed(&s, 0, 2, "a", 1, "first", 5) == -1,
          "duplicate chunk accepted");
    CHECK(s.count == 1 && strcmp(s.items[0].label, "card") == 0,
          "list changed after duplicate");
    CHECK(snippets_apply_pushed(&s, 0, 1, "fresh", 5, "new", 3) == 1,
          "push after discard did not accumulate");
    CHECK(strcmp(s.items[0].body, "new") == 0, "post-discard push not applied");

    // A different total mid-flight discards, and the next set can still land.
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)), "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 2, "a", 1, "first", 5) == 0, "partial failed");
    CHECK(snippets_apply_pushed(&s, 0, 3, "x", 1, "other", 5) == -1,
          "total change mid-flight accepted");
    CHECK(s.count == 1 && strcmp(s.items[0].label, "card") == 0,
          "list changed after total mismatch");
    CHECK(snippets_apply_pushed(&s, 0, 2, "a", 1, "first", 5) == 0
          && snippets_apply_pushed(&s, 1, 2, "b", 1, "second", 6) == 1,
          "fresh push after discard failed");

    // Body content: printable ASCII plus \n and \t only; 0x7f is not in it.
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)), "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 1, "ok", 2, "a\nb\tc", 5) == 1,
          "newline/tab body rejected");
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)),
          "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 1, "bad", 3, "a\x7f" "b", 3) == -1,
          "DEL byte accepted");
    CHECK(s.count == 1 && strcmp(s.items[0].label, "card") == 0,
          "list changed after bad body");

    // Label bounds are enforced here too, not only in the decoder.
    CHECK(snippets_parse("card=kept\n", &s, err, sizeof(err)), "re-seed failed: %s", err);
    CHECK(snippets_apply_pushed(&s, 0, 1, "", 0, "b", 1) == -1, "empty label accepted");
    CHECK(snippets_apply_pushed(&s, 0, 9, "x", 1, "b", 1) == -1, "total 9 accepted");
}

static void test_snippet_load_picked(void)
{
    snippets_t s;
    char err[96] = "";
    CHECK(snippets_parse("one=first\n", &s, err, sizeof(err)), "parse failed: %s", err);
    composer_t c;
    composer_init(&c);
    CHECK(snippet_load_picked(&s, 0, &c), "picking a valid snippet failed");
    CHECK(c.mode == COMPOSER_EDIT, "picked snippet not in the composer");
    CHECK(strcmp(c.body, "first") == 0 && c.len == 5, "composer body wrong");
    CHECK(!snippet_load_picked(&s, 1, &c), "pick out of range accepted");
    CHECK(!snippet_load_picked(&s, -1, &c), "negative pick accepted");
    CHECK(!snippet_load_picked(NULL, 0, &c), "NULL list accepted");
}

static void test_snippet_composer_gate_shape(void)
{
    // The gate is the only send path: a picked snippet's bytes leave as the
    // composer's frame plan -- <=64-byte KEY chunks plus a lone CR -- never
    // as anything else. docs/design/SNIPPETS_PUSH.md: picking is not sending.
    snippets_t s;
    char err[96] = "";
    // A snippet's own maximum, which is no longer the composer's -- see
    // SNIPPET_BODY_MAX. The buffer it loads into is four times larger now;
    // what matters here is that the largest snippet still goes through the
    // gate whole.
    char line[4 + SNIPPET_BODY_MAX + 1];
    memset(line, 'q', SNIPPET_BODY_MAX);
    line[SNIPPET_BODY_MAX] = 0;
    memmove(line + 4, line, SNIPPET_BODY_MAX + 1);
    memcpy(line, "big=", 4);
    CHECK(snippets_parse(line, &s, err, sizeof(err)), "parse failed: %s", err);
    CHECK(s.count == 1 && strlen(s.items[0].body) == SNIPPET_BODY_MAX,
          "max-size snippet did not parse");

    composer_t c;
    composer_init(&c);
    CHECK(snippet_load_picked(&s, 0, &c), "pick failed");
    const size_t frames = composer_frame_count(&c);
    size_t body_seen = 0;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *data = NULL;
        size_t n = 0;
        CHECK(composer_frame(&c, i, &data, &n), "missing frame %zu", i);
        if (i + 1 == frames) {
            CHECK(n == 1 && data[0] == '\r', "final frame is not lone CR");
        } else {
            CHECK(n >= 1 && n <= PROTO_MAX_KEY_PAYLOAD, "frame %zu length %zu", i, n);
            body_seen += n;
        }
    }
    CHECK(body_seen == SNIPPET_BODY_MAX, "planned %d body bytes, saw %zu",
          SNIPPET_BODY_MAX, body_seen);
}

static void test_hello_capabilities_fit_the_handshake_buffer(void)
{
    // The regression this exists for: the capability list grew past the
    // AUTH-sized stack buffer handshake() reused for both frames, and the
    // overflow -- the tail of a fourteen-byte token -- was restored into a
    // callee-saved register and dereferenced: a data abort at 0x726F7463
    // ("ctor" read as a little-endian pointer) on real hardware. The string
    // and the buffer size now live in proto.h, where this test can see both.
    uint8_t out[PROTO_HANDSHAKE_BUF_SIZE];
    const size_t n = proto_encode(out, MSG_HELLO,
                                  (const uint8_t *)PROTO_HELLO_CAPS,
                                  (uint16_t)PROTO_HELLO_CAPS_LEN);
    CHECK(n == PROTO_HEADER_SIZE + PROTO_HELLO_CAPS_LEN,
          "HELLO frame length disagrees with its constants");
    CHECK(n <= sizeof(out),
          "HELLO frame (%zu bytes) overflows the handshake buffer (%zu)",
          n, sizeof(out));
    CHECK((size_t)(PROTO_HEADER_SIZE + PROTO_MAC_SIZE) <= sizeof(out),
          "AUTH frame no longer fits the shared handshake buffer");
    CHECK(strlen(PROTO_HELLO_CAPS) == PROTO_HELLO_CAPS_LEN,
          "capability string and its length constant drifted");

    // The exact tokens the server keys features on, verbatim: losing one
    // silently downgrades the card, and MODE/SNIPPETS/PROJECT/CLIP all key
    // on them.
    CHECK(strncmp(PROTO_HELLO_CAPS, "ds-term/1", 9) == 0,
          "the version token must lead the capability list");
    CHECK(strstr(PROTO_HELLO_CAPS, " mode") != NULL &&
          strstr(PROTO_HELLO_CAPS, " snippets") != NULL &&
          strstr(PROTO_HELLO_CAPS, " project") != NULL &&
          strstr(PROTO_HELLO_CAPS, " clip") != NULL,
          "a feature token went missing from the capability list");

    // An extension appends tokens within a bound the buffer is sized for.
    // The bound is the contract; whatever this build's extension sends is
    // checked against it where that extension is tested, not here.
    CHECK((size_t)(PROTO_HEADER_SIZE + PROTO_HELLO_MAX) <= sizeof(out),
          "a full-length extended HELLO overflows the handshake buffer");
    CHECK(strlen(ext_hello_tokens()) <= PROTO_HELLO_EXT_MAX,
          "this build's extension tokens exceed the bound");
    CHECK(ext_hello_tokens()[0] == '\0' || ext_hello_tokens()[0] == ' ',
          "extension tokens must start with a separator or be empty");
}

// --------------------------------------------------------------- sessions --

static session_chunk_t make_session_chunk(unsigned count, unsigned index,
                                          unsigned flags, unsigned idle_s,
                                          const char *name)
{
    session_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    chunk.count = (uint8_t)count;
    chunk.entry.index = (uint8_t)index;
    chunk.entry.flags = (uint8_t)flags;
    chunk.entry.idle_s = (uint16_t)idle_s;
    if (name) {
        size_t n = strlen(name);
        if (n > SESSION_NAME_MAX) n = SESSION_NAME_MAX;
        memcpy(chunk.entry.name, name, n);
        chunk.entry.name[n] = '\0';
        chunk.entry.name_len = (uint8_t)n;
    }
    return chunk;
}

static void test_session_proto_apply(void)
{
    // Two sessions, the second one.  The raw name deliberately exceeds the
    // display cap and contains a control byte: structural wire data remains
    // valid, but nothing unsafe may reach the bitmap renderer.
    uint8_t raw[PROTO_SESSIONS_CHUNK_SIZE + 26];
    memset(raw, 'x', sizeof(raw));
    raw[0] = 2;
    raw[1] = 1;
    raw[2] = SESSION_FLAG_RUNNING | SESSION_FLAG_NEW_OUTPUT | 0x80;
    raw[3] = 0x12;
    raw[4] = 0x34;
    raw[5] = 26;
    memcpy(raw + PROTO_SESSIONS_CHUNK_SIZE, "shell", 5);
    raw[PROTO_SESSIONS_CHUNK_SIZE + 5] = 0x01;

    session_chunk_t chunk;
    CHECK(proto_apply_sessions(&chunk, raw, sizeof(raw)) == PROTO_OK,
          "valid session chunk rejected");
    CHECK(chunk.count == 2 && chunk.entry.index == 1,
          "count/index wrong: %u/%u", chunk.count, chunk.entry.index);
    CHECK(chunk.entry.flags == (SESSION_FLAG_RUNNING | SESSION_FLAG_NEW_OUTPUT),
          "reserved session flag reached client state: 0x%02X", chunk.entry.flags);
    CHECK(chunk.entry.idle_s == 0x1234, "idle seconds wrong: %u", chunk.entry.idle_s);
    CHECK(chunk.entry.name_len == SESSION_NAME_MAX, "long name was not clamped");
    CHECK(chunk.entry.name[5] == '?', "unsafe name byte was not substituted");
    CHECK(chunk.entry.name[SESSION_NAME_MAX] == '\0', "safe name lacks terminator");

    // The count-only empty list is a valid complete snapshot.  It is the one
    // case with no per-entry fixed header; any trailing byte is malformed.
    const uint8_t empty[] = { 0 };
    CHECK(proto_apply_sessions(&chunk, empty, sizeof(empty)) == PROTO_OK,
          "empty session list rejected");
    CHECK(chunk.count == 0 && chunk.entry.name_len == 0, "empty list not zeroed");
    const uint8_t empty_junk[] = { 0, 0 };
    CHECK(proto_apply_sessions(&chunk, empty_junk, sizeof(empty_junk)) == PROTO_ERR_PAYLOAD,
          "empty list with trailing data accepted");

    const uint8_t too_short[] = { 1, 0, 0, 0, 0 };
    const uint8_t too_many[] = { SESSIONS_MAX + 1 };
    const uint8_t bad_index[] = { 1, 1, 0, 0, 0, 0 };
    const uint8_t bad_length[] = { 1, 0, 0, 0, 0, 1 };
    CHECK(proto_apply_sessions(&chunk, too_short, sizeof(too_short)) == PROTO_ERR_PAYLOAD,
          "short session chunk accepted");
    CHECK(proto_apply_sessions(&chunk, too_many, sizeof(too_many)) == PROTO_ERR_PAYLOAD,
          "nine-session list accepted");
    CHECK(proto_apply_sessions(&chunk, bad_index, sizeof(bad_index)) == PROTO_ERR_PAYLOAD,
          "index outside advertised list accepted");
    CHECK(proto_apply_sessions(&chunk, bad_length, sizeof(bad_length)) == PROTO_ERR_PAYLOAD,
          "declared session name length mismatch accepted");
}

static void test_session_frames_and_pick_wire(void)
{
    // The reader must accept SESSIONS rather than treating it as the byte it
    // was reserved as before session switching, and SESSION_PICK must be exactly one server-list index.
    const uint8_t sessions_payload[] = { 1, 0, SESSION_FLAG_RUNNING,
                                         0, 7, 4, 's', 'h', 'e', 'l' };
    uint8_t frame[PROTO_HEADER_SIZE + sizeof(sessions_payload)];
    const size_t frame_len = proto_encode(frame, MSG_SESSIONS,
                                          sessions_payload, sizeof(sessions_payload));
    framereader_t reader;
    framereader_init(&reader);
    CHECK(framereader_feed(&reader, frame, frame_len) == PROTO_OK,
          "SESSIONS frame feed failed");
    msg_type_t type;
    const uint8_t *payload;
    uint16_t len;
    CHECK(framereader_next(&reader, &type, &payload, &len) == PROTO_OK,
          "SESSIONS frame rejected by reader");
    CHECK(type == MSG_SESSIONS && len == sizeof(sessions_payload) && payload[1] == 0,
          "SESSIONS frame changed in reader");

    const uint8_t index = 7;
    uint8_t pick[PROTO_HEADER_SIZE + PROTO_SESSION_PICK_SIZE];
    const size_t pick_len = proto_encode(pick, MSG_SESSION_PICK, &index, sizeof(index));
    CHECK(pick_len == sizeof(pick), "SESSION_PICK encoded length %zu", pick_len);
    CHECK(pick[0] == 'D' && pick[1] == 'S' && pick[2] == PROTO_VERSION &&
          pick[3] == MSG_SESSION_PICK && pick[4] == 0 && pick[5] == 1 && pick[6] == index,
          "SESSION_PICK wire bytes are not one index");
}

static void test_sessions_atomic_state_and_selection(void)
{
    sessions_t sessions;
    sessions_init(&sessions);
    CHECK(!sessions_ready(&sessions), "fresh session state is ready before a list");
    CHECK(sessions_count(&sessions) == 0 && sessions_selection(&sessions) == -1,
          "fresh session state has a pick");

    // Chunks may arrive in any order. Nothing becomes visible until both rows
    // are present, and the initial new-output dot must not make a catch-up
    // noise simply because the client just connected.
    session_chunk_t shell = make_session_chunk(2, 1, SESSION_FLAG_RUNNING,
                                               12, "shell");
    session_chunk_t claude = make_session_chunk(2, 0,
                                                SESSION_FLAG_RUNNING | SESSION_FLAG_ACTIVE,
                                                3, "claude");
    CHECK(sessions_apply_chunk(&sessions, &shell) == 0, "first chunk should stage");
    CHECK(!sessions_ready(&sessions) && sessions_count(&sessions) == 0,
          "partial list escaped staging");
    CHECK(sessions_apply_chunk(&sessions, &claude) == 1, "second chunk did not commit");
    CHECK(sessions_ready(&sessions) && sessions_count(&sessions) == 2,
          "complete list not available");
    CHECK(sessions_selection(&sessions) == 0,
          "picker did not begin on active session");
    CHECK(sessions_new_output_mask(&sessions) == 0,
          "initial list generated a lantern cue");
    CHECK(sessions_item(&sessions, 1) && strcmp(sessions_item(&sessions, 1)->name, "shell") == 0,
          "out-of-order row did not land at its advertised index");

    CHECK(sessions_move_selection(&sessions, 1), "selection did not move down");
    CHECK(sessions_selection(&sessions) == 1, "selection down chose wrong row");
    CHECK(sessions_move_selection(&sessions, 1), "selection did not wrap down");
    CHECK(sessions_selection(&sessions) == 0, "selection did not wrap to first row");
    CHECK(sessions_move_selection(&sessions, -1), "selection did not wrap up");
    CHECK(sessions_selection(&sessions) == 1, "selection up chose wrong row");

    // A background row gaining NEW_OUTPUT is a one-shot transition.  The dot
    // remains in the committed list; consuming the mask only consumes its cue.
    shell.entry.flags |= SESSION_FLAG_NEW_OUTPUT;
    CHECK(sessions_apply_chunk(&sessions, &shell) == 0, "refresh chunk should stage");
    CHECK(sessions_apply_chunk(&sessions, &claude) == 1, "refresh did not commit");
    CHECK(sessions_new_output_mask(&sessions) == 0x02,
          "background new-output transition not reported");
    CHECK(sessions_take_new_output_mask(&sessions) == 0x02,
          "lantern transition not consumable");
    CHECK(sessions_take_new_output_mask(&sessions) == 0,
          "lantern transition repeated after consumption");
    CHECK(sessions_item(&sessions, 1)->flags & SESSION_FLAG_NEW_OUTPUT,
          "consuming cue cleared visible new-output dot");

    // A duplicate or invalid chunk throws away only the in-flight replacement;
    // the last usable list stays valid and may still gate a deliberate pick.
    claude.entry.flags = SESSION_FLAG_RUNNING | SESSION_FLAG_ACTIVE;
    CHECK(sessions_apply_chunk(&sessions, &claude) == 0, "duplicate test did not stage");
    CHECK(sessions_apply_chunk(&sessions, &claude) == -1, "duplicate chunk accepted");
    CHECK(sessions_ready(&sessions) && sessions_count(&sessions) == 2,
          "duplicate changed committed list or readiness");
    session_chunk_t invalid = make_session_chunk(2, 0, 0, 0, "bad");
    invalid.entry.name_len = SESSION_NAME_MAX + 1;
    CHECK(sessions_apply_chunk(&sessions, &invalid) == -1, "oversize staged name accepted");
    CHECK(sessions_count(&sessions) == 2 && sessions_selected(&sessions),
          "invalid chunk removed current selection");

    // Count zero is the complete, capable-but-empty list. It clears a stale
    // choice but keeps readiness true so the UI can say there is nothing to
    // switch to instead of falsely claiming the server has no support.
    session_chunk_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(sessions_apply_chunk(&sessions, &empty) == 1, "empty list did not commit");
    CHECK(sessions_ready(&sessions) && sessions_count(&sessions) == 0 &&
          sessions_selection(&sessions) == -1 && !sessions_selected(&sessions),
          "empty list did not clear selectable rows");
}

static void test_sessions_repeated_snapshot_stability(void)
{
    // The server is allowed to publish the same complete snapshot forever:
    // idle-age refreshes, a burst of background changes, or an over-eager
    // server must never make the client grow staging state or walk past either
    // fixed array.  Exercise the exact one-row payload observed on hardware
    // through both the wire decoder and the stateful commit path.
    const uint8_t payload[] = {
        1, 0, SESSION_FLAG_RUNNING | SESSION_FLAG_ACTIVE,
        0, 11, 6, 'c', 'l', 'a', 'u', 'd', 'e'
    };
    sessions_t sessions;
    sessions_init(&sessions);

    for (unsigned i = 0; i < 1024; i++) {
        session_chunk_t chunk;
        uint8_t tick[sizeof(payload)];
        memcpy(tick, payload, sizeof(tick));
        // Vary only the legal idle value.  It must not become a hidden
        // generation key or cause a new allocation/copy shape.
        tick[3] = (uint8_t)(i >> 8);
        tick[4] = (uint8_t)i;
        CHECK(proto_apply_sessions(&chunk, tick, sizeof(tick)) == PROTO_OK,
              "repeated session payload %u did not decode", i);
        CHECK(sessions_apply_chunk(&sessions, &chunk) == 1,
              "repeated one-row snapshot %u did not commit", i);
        CHECK(sessions_ready(&sessions) && sessions_count(&sessions) == 1,
              "repeated snapshot %u damaged readiness/count", i);
        const session_t *entry = sessions_item(&sessions, 0);
        CHECK(entry && entry->index == 0 &&
              entry->flags == (SESSION_FLAG_RUNNING | SESSION_FLAG_ACTIVE) &&
              entry->idle_s == i && entry->name_len == 6 &&
              strcmp(entry->name, "claude") == 0,
              "repeated snapshot %u damaged the committed row", i);
        CHECK(sessions.new_output_mask == 0 &&
              sessions.staged_count == 0 && sessions.staged_received == 0,
              "repeated snapshot %u left staging state behind", i);
    }
}

static void test_sound_cues_all_terminate(void)
{
    // A cue that never ends holds the PSG channel open for the rest of the
    // session -- a stuck tone on a handheld, with no way to stop it short of a
    // power cycle. Every cue must end on its own.
    for (int c = SOUND_SENT; c <= SOUND_LINK; c++) {
        unsigned seen[8];
        int n = 0;
        const int frames = play_out((sound_cue_t)c, seen, 8, &n);
        CHECK(frames > 0, "cue %d never sounded", c);
        CHECK(frames <= SOUND_MAX_FRAMES,
              "cue %d ran %d frames, over the %d budget", c, frames, SOUND_MAX_FRAMES);
        CHECK(n >= 2, "cue %d is a single beep, not a phrase", c);

        for (int i = 0; i < n; i++) {
            CHECK(seen[i] >= SOUND_HZ_MIN && seen[i] <= SOUND_HZ_MAX,
                  "cue %d note %d at %uHz is outside the audible band", c, i, seen[i]);
        }

        // Silence stays silent once the cue is done.
        unsigned hz = 1;
        sound_tick(&hz);
        CHECK(hz == 0, "cue %d resumed after finishing", c);
    }
}

static void test_sound_cues_rise_or_fall_by_meaning(void)
{
    // The intervals carry the meaning: arriving rises, going away falls. If
    // someone retunes these, the direction is the part that must survive.
    unsigned seen[8];
    int n = 0;

    play_out(SOUND_SENT, seen, 8, &n);
    CHECK(n >= 2 && seen[n - 1] > seen[0], "SENT should rise");
    play_out(SOUND_THINK, seen, 8, &n);
    CHECK(n >= 3 && seen[n - 1] > seen[0], "THINK should rise");
    play_out(SOUND_OUTPUT, seen, 8, &n);
    CHECK(n >= 2 && seen[n - 1] > seen[0], "OUTPUT should rise");
    play_out(SOUND_DONE, seen, 8, &n);
    CHECK(n >= 3 && seen[n - 1] > seen[0], "DONE should rise and resolve");
    play_out(SOUND_LINK, seen, 8, &n);
    CHECK(n >= 3 && seen[n - 1] > seen[0], "LINK should rise");
    play_out(SOUND_DROP, seen, 8, &n);
    CHECK(n >= 2 && seen[n - 1] < seen[0], "DROP should fall");

    // The registers must not blur: high is working, blunt is output, mid is
    // sending -- distinguishable without looking at the screen.
    unsigned lo[8];
    int ln = 0;
    play_out(SOUND_THINK, seen, 8, &n);
    play_out(SOUND_OUTPUT, lo, 8, &ln);
    CHECK(seen[0] > lo[ln - 1],
          "THINK (%u) must sit above OUTPUT (%u) -- the contrast is the cue",
          seen[0], lo[ln - 1]);
    play_out(SOUND_SENT, seen, 8, &n);
    CHECK(seen[0] > lo[ln - 1] && seen[0] < 880,
          "SENT should sit between the blunt and the working registers");
}

static void test_sound_mute_silences_everything(void)
{
    reset_sound();
    sound_set_muted(1);
    CHECK(sound_muted(), "mute did not take");
    for (int c = SOUND_SENT; c <= SOUND_LINK; c++) {
        sound_cue((sound_cue_t)c);
        for (int i = 0; i < SOUND_MAX_FRAMES; i++) {
            unsigned hz = 1;
            sound_tick(&hz);
            CHECK(hz == 0, "cue %d sounded while muted", c);
        }
    }

    // Muting mid-cue abandons it rather than letting it finish silently and
    // then resurface on unmute.
    sound_set_muted(0);
    sound_cue(SOUND_DONE);
    unsigned hz = 0;
    sound_tick(&hz);
    CHECK(hz != 0, "cue did not start after unmute");
    sound_set_muted(1);
    sound_tick(&hz);
    CHECK(hz == 0, "muting mid-cue did not silence it");
    sound_set_muted(0);
    sound_tick(&hz);
    CHECK(hz == 0, "an abandoned cue came back on unmute");
    reset_sound();
}

static void test_sound_tick_reports_only_changes(void)
{
    // The caller kills and restarts a PSG channel on every reported change, so
    // reporting one per frame would restart the tone 60 times a second and
    // turn a warm note into a buzz.
    reset_sound();
    sound_cue(SOUND_LINK);
    int changes = 0;
    for (int i = 0; i < SOUND_MAX_FRAMES * 2; i++) {
        unsigned hz = 0;
        changes += sound_tick(&hz);
    }
    // Three notes plus the return to silence.
    CHECK(changes == 4, "LINK reported %d changes, wanted one per note plus silence",
          changes);
}

static void test_sound_ignores_a_bad_cue(void)
{
    // The sparse public numbering is a compatibility contract: old callers
    // deliberately use SOUND_LINK + 1 as an invalid probe, while local-only
    // cues live in the separate 16..20 extension range.
    CHECK(SOUND_SENT == 1 && SOUND_LINK == 6 && SOUND_CUE_COUNT == 6 &&
          SOUND_LANTERN == 16 && SOUND_FOCUS == 17 && SOUND_SWITCH == 18 &&
          SOUND_PREVIEW == 19 && SOUND_KEY == 20 && SOUND_DENY == 21 &&
          SOUND_BOOT == 22 && SOUND_BLIP == 23 && SOUND_APPROVE == 24 &&
          SOUND_QUESTION == 25 && SOUND_CUE_TOTAL_COUNT == 16,
          "sound cue numbering contract changed");
    reset_sound();
    sound_cue(SOUND_NONE);
    unsigned hz = 1;
    sound_tick(&hz);
    CHECK(hz == 0, "SOUND_NONE sounded");
    sound_cue((sound_cue_t)(SOUND_LINK + 1));
    sound_tick(&hz);
    CHECK(hz == 0, "an out-of-range cue sounded");
}

// Play a single keypress tick to silence.  Returns the frames it sounded and
// reports the pitch it opened on.
static int play_key(int backspace, unsigned *first_hz)
{
    reset_sound();
    key_typed(backspace);
    *first_hz = 0;
    int frames = 0;
    for (int i = 0; i < SOUND_MAX_FRAMES * 4; i++) {
        unsigned hz = 0;
        sound_tick(&hz);
        if (hz == 0 && frames > 0) break;
        if (hz) {
            if (frames == 0) *first_hz = hz;
            frames++;
        }
    }
    return frames;
}

static void test_sound_keypress_answers_the_user(void)
{
    // Typing feedback is the user's own action answering, not the console
    // volunteering, so it survives Studio -- the profile whose whole job is
    // keeping the console quiet.
    //
    // This also pins the cooldown-slot bound.  SOUND_KEY is the highest cue
    // value, and the slot arrays are indexed by that value: if CUE_SLOT_COUNT
    // ever stops clearing it, every tick is dropped by the bounds check with
    // no other symptom -- silent typing, nothing in the log, nothing to grep.
    static const sound_profile_t profiles[] = {
        SOUND_PROFILE_STUDIO, SOUND_PROFILE_COMPANION, SOUND_PROFILE_FIELD,
    };
    for (unsigned p = 0; p < sizeof(profiles) / sizeof(profiles[0]); p++) {
        sound_set_profile(profiles[p]);
        unsigned hz = 0;
        const int frames = play_key(0, &hz);
        CHECK(frames > 0, "%s swallowed the keypress tick",
              sound_profile_name(profiles[p]));
        CHECK(hz >= SOUND_HZ_MIN && hz <= SOUND_HZ_MAX,
              "keypress at %uHz is outside the audible band", hz);
        CHECK(frames <= 4, "keypress ran %d frames -- it is a tick, not a phrase",
              frames);
    }
    sound_set_profile(SOUND_PROFILE_COMPANION);

    // Deleting answers below typing.  The exact pitches can be retuned; the
    // direction is the part that carries the meaning.
    unsigned typed = 0;
    unsigned deleted = 0;
    play_key(0, &typed);
    play_key(1, &deleted);
    CHECK(deleted < typed, "backspace (%uHz) should sit below a typed key (%uHz)",
          deleted, typed);

    // A keypress never talks over a phrase.  Someone typing their next line
    // while DONE resolves should hear DONE finish.
    reset_sound();
    sound_cue(SOUND_DONE);
    sound_voice_t voice;
    sound_tick_voice(&voice);
    key_typed(0);
    sound_tick_voice(&voice);
    CHECK(sound_active_cue() == SOUND_DONE, "a keypress interrupted DONE");

    // A held key calls this every frame.  The cooldown has to thin that out,
    // or sixty ticks a second stops being feedback and becomes a buzz.
    reset_sound();
    int starts = 0;
    int was_silent = 1;
    for (int i = 0; i < 60; i++) {
        key_typed(0);
        unsigned hz = 0;
        sound_tick(&hz);
        if (hz && was_silent) starts++;
        was_silent = (hz == 0);
    }
    CHECK(starts > 0, "a held key never ticked at all");
    CHECK(starts <= 20, "a held key ticked %d times in 60 frames -- that is a buzz",
          starts);

    // Mute covers the user's own keys too, per the header contract.
    reset_sound();
    sound_set_muted(1);
    key_typed(0);
    unsigned muted_hz = 1;
    sound_tick(&muted_hz);
    CHECK(muted_hz == 0, "a keypress sounded while muted");

    sound_set_muted(0);
    sound_set_profile(SOUND_PROFILE_COMPANION);
    reset_sound();
}

static void test_sound_switch_and_focus_profiles(void)
{
    // Revisited design call: a session switch is a deliberate, consequential
    // action, so SWITCH voices in Companion too -- a silent one feels broken
    // in the warm profile.  FOCUS stays Field-only: per-move ticking while
    // walking a list is the tactile distinction Field exists for.  Both stay
    // out of Studio.
    sound_voice_t voice;
    for (int p = SOUND_PROFILE_STUDIO; p < SOUND_PROFILE_COUNT; p++) {
        sound_restore_defaults();
        sound_set_profile((sound_profile_t)p);
        reset_sound();
        sound_cue(SOUND_SWITCH);
        sound_tick_voice(&voice);
        if (p == SOUND_PROFILE_STUDIO) {
            CHECK(voice.hz == 0, "Studio voiced a session switch");
        } else {
            CHECK(voice.hz != 0, "profile %d did not voice a session switch", p);
        }
        reset_sound();
        sound_cue(SOUND_FOCUS);
        sound_tick_voice(&voice);
        if (p == SOUND_PROFILE_FIELD) {
            CHECK(voice.hz != 0, "Field did not voice focus feedback");
        } else {
            CHECK(voice.hz == 0, "profile %d voiced focus feedback", p);
        }
    }
    sound_restore_defaults();
}

static void test_chime_owns_resync_and_history(void)
{
    // A scroll's resync is this client's own doing: it must not sound as
    // output, and output after it must still sound.
    unsigned seen[8];
    int n = 0;

    chime_init();
    reset_sound();
    chime_sent();
    drain_current(seen, 8, &n);
    chime_run(CHIME_THINK_QUIET);
    drain_current(seen, 8, &n);            // THINK fired; now WAITING

    chime_view_sent();
    for (int i = 0; i < CHIME_MIN_CELLS + 2; i++) chime_cells(' ');
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "a resync must not sound as output");

    chime_run(CHIME_SUPPRESS_QUIET + 1);   // suppression expires
    for (int i = 0; i < CHIME_MIN_CELLS; i++) chime_cells('a');
    n = 0;
    CHECK(drain_current(seen, 8, &n) > 0 && seen[0] == 220,
          "output after a resync must still sound");

    // Reading history is equally ours: no murmurs, no DONE while reading,
    // and the quiet clock restarts on return so a long read cannot chime.
    chime_run(CHIME_MURMUR_GAP);
    chime_cells('x');                      // one stream blip
    drain_current(seen, 8, &n);
    chime_history(1);
    for (int i = 0; i < 6; i++) chime_cells('z');
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "history redraws must be silent");

    chime_run(CHIME_DONE_QUIET);
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "no DONE while reading history");

    chime_history(0);
    chime_run(CHIME_DONE_QUIET - 1);
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "DONE early after returning to live");
    chime_run(1);
    n = 0;
    CHECK(drain_current(seen, 8, &n) > 0 && seen[2] == 784,
          "DONE after the full quiet since returning to live");
    reset_sound();
}

static void test_chime_mute_covers_everything(void)
{
    unsigned seen[8];
    int n = 0;

    chime_init();
    reset_sound();
    sound_set_muted(1);
    chime_sent();
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "SEND sounded while muted");
    chime_cells('a');
    n = 0;
    CHECK(drain_current(seen, 8, &n) == 0, "murmur sounded while muted");
    sound_set_muted(0);
    reset_sound();
}

// ------------------------------------------------------------- keyboard ----

static int kbd_id_for_char(char c, kbd_layer_t layer)
{
    for (int id = 0; id < kbd_count(); id++) {
        if (kbd_action(id) == KBD_CHAR && kbd_label(id, layer)[0] == c) return id;
    }
    return -1;
}

static void test_kbd_every_key_maps_to_bytes(void)
{
    // Every key in every layer is either a defined byte-producing key or a
    // defined local action (SHFT cycles, CTRL arms) -- nothing undefined.
    uint8_t out[8];
    for (int id = 0; id < kbd_count(); id++) {
        for (int l = KBD_LAYER_BASE; l <= KBD_LAYER_SYM; l++) {
            kbd_reset();
            kbd_layer_set((kbd_layer_t)l);
            const int n = kbd_press(id, out, sizeof(out));
            const kbd_action_t a = kbd_action(id);
            if (a == KBD_SHFT || a == KBD_CTRL) {
                CHECK(n == -1, "key %d layer %d (action %d) should emit nothing", id, l, a);
            } else {
                CHECK(n >= 1, "key %d layer %d emits no bytes", id, l);
                CHECK(kbd_action(id) != KBD_NONE, "key %d undefined", id);
            }
        }
    }
}

static void test_kbd_geometry(void)
{
    // Every key inside 256x192, and no two keys overlap.
    for (int a = 0; a < kbd_count(); a++) {
        int ax, ay, aw, ah;
        kbd_key_rect(a, KBD_LAYER_BASE, &ax, &ay, &aw, &ah);
        CHECK(ax >= 0 && ay >= 0 && ax + aw <= 256 && ay + ah <= 192,
              "key %d at %d,%d %dx%d outside 256x192", a, ax, ay, aw, ah);
        for (int b = a + 1; b < kbd_count(); b++) {
            int bx, by, bw, bh;
            kbd_key_rect(b, KBD_LAYER_BASE, &bx, &by, &bw, &bh);
            CHECK(ax + aw <= bx || bx + bw <= ax || ay + ah <= by || by + bh <= ay,
                  "keys %d and %d overlap", a, b);
        }
    }

    // The three layers have identical geometry: same rects for every key.
    for (int id = 0; id < kbd_count(); id++) {
        int x0, y0, w0, h0;
        kbd_key_rect(id, KBD_LAYER_BASE, &x0, &y0, &w0, &h0);
        for (int l = KBD_LAYER_SHIFT; l <= KBD_LAYER_SYM; l++) {
            int x, y, w, h;
            kbd_key_rect(id, (kbd_layer_t)l, &x, &y, &w, &h);
            CHECK(x == x0 && y == y0 && w == w0 && h == h0,
                  "key %d geometry differs in layer %d", id, l);
        }

        // Every label fits its key: six pixels per glyph, and the key is
        // KBD_W wide. A wider label would overflow into the next key.
        for (int l = KBD_LAYER_BASE; l <= KBD_LAYER_SYM; l++) {
            const int len = (int)strlen(kbd_label(id, (kbd_layer_t)l));
            const int w = (id == 44) ? 2 * KBD_STEP_X : KBD_W;
            CHECK(len * 6 <= w, "key %d layer %d label '%s' too wide (%d px in %d)",
                  id, l, kbd_label(id, (kbd_layer_t)l), len * 6, w);
        }
    }
}

static void test_kbd_hit(void)
{
    // The centre of every key hits that key.
    for (int id = 0; id < kbd_count(); id++) {
        int x, y, w, h;
        kbd_key_rect(id, KBD_LAYER_BASE, &x, &y, &w, &h);
        CHECK(kbd_hit(x + w / 2, y + h / 2) == id,
              "centre of key %d at %d,%d hits %d", id, x + w / 2, y + h / 2,
              kbd_hit(x + w / 2, y + h / 2));
    }

    // Outside the grid misses.
    CHECK(kbd_hit(0, 0) == -1, "origin should miss");
    CHECK(kbd_hit(3, 100) == -1, "left of grid should miss");
    CHECK(kbd_hit(100, 10) == -1, "above grid should miss");
    CHECK(kbd_hit(255, 191) == -1, "bottom-right should miss");

    // Both halves of the double-width space hit the space key.
    int sx, sy, sw, sh;
    kbd_key_rect(44, KBD_LAYER_BASE, &sx, &sy, &sw, &sh);
    CHECK(kbd_hit(sx + 10, sy + 10) == 44, "space left half");
    CHECK(kbd_hit(sx + sw - 10, sy + 10) == 44, "space right half");

    // A pixel on the boundary between keys belongs to the left/upper key.
    CHECK(kbd_hit(KBD_ORIGIN_X + 24, KBD_ORIGIN_Y + 10) == 0, "boundary gap -> key 0");
}

// The Control screen's tiles. Tested here for the same reason the keyboard's
// geometry is: subscreen.c draws from this table and main.c hit-tests against
// it, and a tile whose touch target has drifted off it is a button that does
// something other than what it says. On the approval pair -- accept next to
// reject -- that is not a cosmetic failure.
// No touch control duplicates a physical button.
//
// This is the rule the whole subtraction pass turns on: a physical button means
// the same thing everywhere, and the touchscreen carries only screen changes
// and local utilities that no button can do. Old button-duplication tiles were
// removed because two of their labels contradicted the real X/Escape behavior.
//
// The six destinations have no single physical-key equivalent: KEYS,
// SESSIONS, LIVE, COMPOSE, the extension's tile, and SETTINGS. RELINK is the
// deliberately small touch-only recovery utility. If a future tile can be
// named "A something", it does not belong here.
static void test_kbd_id_at_grid(void)
{
    CHECK(kbd_id_at_grid(0, 0) == 0, "grid (0,0) should be key 0");
    CHECK(kbd_id_at_grid(3, 9) == 39, "grid (3,9) should be key 39");
    CHECK(kbd_id_at_grid(4, 0) == 40, "grid (4,0) should be SHFT");
    CHECK(kbd_id_at_grid(4, 3) == 43, "grid (4,3) should be TAB");
    CHECK(kbd_id_at_grid(4, 4) == 44, "space left half");
    CHECK(kbd_id_at_grid(4, 5) == 44, "space right half");
    CHECK(kbd_id_at_grid(4, 9) == 48, "grid (4,9) should be BKSP");
    CHECK(kbd_id_at_grid(-1, 0) == -1, "row -1 accepted");
    CHECK(kbd_id_at_grid(5, 0) == -1, "row 5 accepted");
    CHECK(kbd_id_at_grid(0, -1) == -1, "col -1 accepted");
    CHECK(kbd_id_at_grid(0, 10) == -1, "col 10 accepted");

    // The grid mapping and the touch mapping must agree cell for cell: d-pad
    // focus and touch are two ways of pressing the same key.
    for (int r = 0; r < KBD_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            const int id = kbd_id_at_grid(r, c);
            int x, y, w, h;
            kbd_key_rect(id, KBD_LAYER_BASE, &x, &y, &w, &h);
            CHECK(kbd_hit(x + w / 2, y + h / 2) == id,
                  "grid (%d,%d) -> id %d disagrees with touch", r, c, id);
        }
    }
}

static void test_focus_control_grid(void)
{
    focus_init();
    CHECK(!focus_active(), "focus should start off");

    focus_on(FOCUS_CONTROL);
    CHECK(focus_active(), "focus_on failed");
    CHECK(focus_place() == FOCUS_CONTROL, "wrong place");
    CHECK(focus_at_top(), "home should be on the top row");
    CHECK(focus_control_id() == KBD_CONTROL_KEYS,
          "home should be the KEYS tile, got %d", focus_control_id());

    // Row 0: KEYS | SESSIONS | LIVE, stopping at the right edge.
    focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_QUICK,
          "right from KEYS should be SESSIONS");
    focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_PHOTO,
          "right from SESSIONS should be LIVE");
    focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_PHOTO, "right edge should stop");
    focus_move(-1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_QUICK,
          "left should return to SESSIONS");

    // Row 1: COMPOSE | (extension) | SETTINGS. With nothing attached the middle
    // slot has no rect, so the row is two tiles wide. The small RELINK
    // utility deliberately is not part of this d-pad grid.
    focus_move(1, 0);   // back to LIVE (col 2)
    focus_move(0, 1);
    CHECK(focus_control_id() == KBD_CONTROL_SETTINGS,
          "down from LIVE should reach SETTINGS, got %d", focus_control_id());
    focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_SETTINGS, "bottom row right edge should stop");
    focus_move(0, 1);
    CHECK(focus_control_id() == KBD_CONTROL_SETTINGS, "below the bottom row should stop");
    // Up keeps the column *within the row it is leaving*: with REMOTE in the
    // middle of the second row, SETTINGS is column 2 and returns to LIVE.
    focus_move(0, -1);
    CHECK(focus_control_id() == KBD_CONTROL_PHOTO,
          "up should keep the column and land on LIVE");
    CHECK(focus_at_top(), "back on the top row");

    // Down from KEYS lands on COMPOSE, which took the slot the snippets tile
    // gave up when snippets became a pane of the composer rather than a
    // seventh destination.
    focus_on(FOCUS_CONTROL);
    focus_move(0, 1);
    CHECK(focus_control_id() == KBD_CONTROL_COMPOSE,
          "down from KEYS should be COMPOSE, got %d", focus_control_id());
    focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_REMOTE,
          "right from COMPOSE should be REMOTE, the sixth tile");
    focus_move(0, -1);
    CHECK(focus_control_id() == KBD_CONTROL_QUICK,
          "up from REMOTE keeps the column and returns to SESSIONS");

    focus_off();
    CHECK(!focus_active(), "focus_off failed");
    CHECK(focus_control_id() == KBD_CONTROL_NONE, "off should have no control");
}

static void test_focus_keyboard_grid(void)
{
    focus_on(FOCUS_KEYS);
    CHECK(focus_active() && focus_place() == FOCUS_KEYS, "keyboard focus failed");
    CHECK(focus_at_top() == 0, "home at 'a' is not the top row");
    CHECK(focus_key_id() == 10, "home should be 'a' (id 10), got %d", focus_key_id());

    // Words start at 'a'; moving right walks the home row and wraps at its end.
    for (int i = 0; i < 9; i++) focus_move(1, 0);
    CHECK(focus_key_id() == 19, "nine rights should reach 'p', got %d", focus_key_id());
    focus_move(1, 0);
    CHECK(focus_key_id() == 10, "right past the row end should wrap to 'a'");
    focus_move(-1, 0);
    CHECK(focus_key_id() == 19, "left from 'a' should wrap to 'p'");

    // Vertical: rows 0-3 are ten keys each; the top row's Up is the exit.
    focus_on(FOCUS_KEYS);
    focus_move(0, -1);
    CHECK(focus_key_id() == 0, "up from 'a' should reach '1', got %d", focus_key_id());
    CHECK(focus_at_top(), "row 0 is the top");
    focus_move(0, -1);
    CHECK(focus_key_id() == 0, "up past the top should stop");

    // Row 4: the special keys, with space owning columns 4-5 as one item.
    focus_on(FOCUS_KEYS);
    for (int i = 0; i < 3; i++) focus_move(0, 1);   // 'a' -> row 4
    CHECK(focus_key_id() == 40, "row 4 home should be SHFT, got %d", focus_key_id());
    focus_move(1, 0); CHECK(focus_key_id() == 41, "SHFT -> CTRL");
    focus_move(1, 0); CHECK(focus_key_id() == 42, "CTRL -> ESC");
    focus_move(1, 0); CHECK(focus_key_id() == 43, "ESC -> TAB");
    focus_move(1, 0); CHECK(focus_key_id() == 44, "TAB -> SPACE");
    focus_move(1, 0); CHECK(focus_key_id() == 45, "SPACE -> '-' (skip duplicate cell)");
    focus_move(-1, 0); CHECK(focus_key_id() == 44, "'-' -> SPACE");
    focus_move(-1, 0); CHECK(focus_key_id() == 43, "SPACE -> TAB (skip duplicate cell)");
    focus_move(0, 1);
    CHECK(focus_key_id() == 43, "below row 4 should stop");
    focus_move(0, -1);
    CHECK(focus_key_id() == 33, "up from TAB should reach 'v' (row 3 col 3)");

    focus_off();
    CHECK(focus_key_id() == -1, "off should have no key");
}

static void test_focus_keyboard_shortcuts(void)
{
    focus_init();
    CHECK(!focus_b_is_space(), "off should not map B to space");
    CHECK(!focus_y_is_backspace(), "off should not map Y to backspace");
    CHECK(!focus_r_is_enter(), "off should not map R to enter");

    focus_on(FOCUS_KEYS);
    // Focus keyboard shortcuts deliberately differ from the live keyboard:
    // B is the easy-to-reach space key, Y is delete, and R submits.  This
    // catches the old mapping before the DS-only input loop is involved.
    CHECK(focus_b_is_space(), "on a key, B must be space");
    CHECK(focus_y_is_backspace(), "on a key, Y must be backspace");
    CHECK(focus_r_is_enter(), "on a key, R must be enter");

    // Up onto the number row is still the keyboard.
    focus_move(0, -1);
    CHECK(focus_b_is_space(), "the number row must keep B as space");
    CHECK(focus_y_is_backspace(), "the number row must keep Y as backspace");
    CHECK(focus_r_is_enter(), "the number row must keep R as enter");

    focus_off();
    focus_on(FOCUS_CONTROL);
    CHECK(!focus_b_is_space(), "on a tile, B must remain back, not space");
    CHECK(!focus_y_is_backspace(), "on a tile, Y must not delete");
    CHECK(!focus_r_is_enter(), "on a tile, R must not submit");

    // Walking between tiles is never a keyboard surface.
    focus_move(0, 1);
    CHECK(!focus_b_is_space(), "still the tile grid");
    CHECK(!focus_y_is_backspace(), "still the tile grid");
    CHECK(!focus_r_is_enter(), "still the tile grid");

    focus_off();
    CHECK(!focus_b_is_space() && !focus_y_is_backspace() && !focus_r_is_enter(),
          "all focus keyboard shortcuts must turn off together");
}

static void test_focus_grid_survives_rebuild(void)
{
    // Regression: build_control_grid kept its row table in static state and
    // focus_init() cleared only the "built" flag, so a second build appended
    // the five controls to the rows left by the first -- counts grew past
    // the table (an out-of-bounds write UBSAN caught on WSL) and every
    // control appeared twice. The grid is derived from a fixed rect table,
    // so every rebuild must produce the identical grid.
    //
    // The walk visits every control reachable from the home tile by moving
    // right, then down when right is blocked, until both are blocked. Each
    // round must see the same controls in the same order, none twice.
    int reference[16], ref_n = 0;

    for (int round = 0; round < 3; round++) {
        int seq[16], n = 0;
        int seen[KBD_CONTROL_COUNT + 1] = {0};

        focus_init();
        focus_on(FOCUS_CONTROL);

        int id = focus_control_id();
        CHECK(id == KBD_CONTROL_KEYS,
              "round %d: home must be KEYS, got %d", round, id);

        int guard = 0;
        while (guard++ < 16) {
            CHECK(id > KBD_CONTROL_NONE && id <= KBD_CONTROL_COUNT,
                  "round %d: id %d out of range", round, id);
            CHECK(!seen[id],
                  "round %d: control %d appears twice -- the rebuild left "
                  "stale rows behind", round, id);
            seen[id] = 1;
            if (n < 16) seq[n++] = id;

            focus_move(1, 0);
            const int next = focus_control_id();
            if (next != id) { id = next; continue; }
            focus_move(0, 1);
            if (focus_control_id() == id) break;   // both directions blocked
            id = focus_control_id();
        }

        if (round == 0) {
            ref_n = n;
            for (int i = 0; i < n; i++) reference[i] = seq[i];
        } else {
            CHECK(n == ref_n,
                  "round %d: walk visited %d controls, round 0 visited %d",
                  round, n, ref_n);
            const int cmp = n < ref_n ? n : ref_n;
            for (int i = 0; i < cmp; i++) {
                CHECK(seq[i] == reference[i],
                      "round %d: position %d is control %d, round 0 had %d",
                      round, i, seq[i], reference[i]);
            }
        }
    }
}

static void test_kbd_keyword_assembly(void)
{
    // Regression: the key word used to be built from REG_KEYINPUT alone --
    // ten bits, no X or Y -- so focus_input's X and Y checks were always
    // false and Y-space was dead on hardware. Assembly is a pure function
    // now so this class of bug fails on the host: a set X bit in the libnds
    // word must arrive as KEY_X in the combined word.

    // The structural property that makes OR the right combine: X and Y sit
    // above the ten register bits and share none of them.
    CHECK((KBD_KEY_X & KBD_REG_MASK) == 0, "X overlaps the register bits");
    CHECK((KBD_KEY_Y & KBD_REG_MASK) == 0, "Y overlaps the register bits");

    // The ten register bits pass through unmangled; nothing in, nothing out.
    CHECK(kbd_held_keyword(0x3FFu, 0) == 0x3FFu, "register bits mangled");
    CHECK(kbd_held_keyword(0, 0) == 0, "no inputs should produce no keys");
    CHECK(kbd_held_keyword(0x1000u, 0) == 0, "a stray high register bit leaked");

    // X and Y from the libnds word arrive at their own bit positions.
    CHECK(kbd_held_keyword(0, KBD_KEY_X) == KBD_KEY_X,
          "an X bit in the libnds word did not arrive as KEY_X");
    CHECK(kbd_held_keyword(0, KBD_KEY_Y) == KBD_KEY_Y,
          "a Y bit in the libnds word did not arrive as KEY_Y");
    CHECK(kbd_held_keyword(0, KBD_KEY_X | KBD_KEY_Y) == (KBD_KEY_X | KBD_KEY_Y),
          "X and Y together did not arrive");
    CHECK(kbd_held_keyword(0x1u, KBD_KEY_X) == (0x1u | KBD_KEY_X),
          "register and libnds bits did not combine");

    // The edge word: newly-pressed only.
    CHECK(kbd_edge_keyword(0x1u, 0) == 0x1u, "edge of a fresh press");
    CHECK(kbd_edge_keyword(0x1u, 0x1u) == 0, "no edge while held steady");
    CHECK(kbd_edge_keyword(0, 0x1u) == 0, "no edge on release");
    CHECK(kbd_edge_keyword(0x3u, 0x1u) == 0x2u, "edge of a second button");

    // The exact regression: a Y that arrives must edge.
    const unsigned y_held = kbd_held_keyword(0, KBD_KEY_Y);
    CHECK(kbd_edge_keyword(y_held, 0) & KBD_KEY_Y,
          "a fresh Y must appear in the edge word");
    // And a held-then-released-then-pressed X must edge again.
    unsigned held = kbd_held_keyword(0, KBD_KEY_X);
    CHECK(kbd_edge_keyword(held, 0) == KBD_KEY_X, "X edges when it arrives");
    held = kbd_held_keyword(0, 0);
    CHECK(kbd_edge_keyword(held, KBD_KEY_X) == 0, "released X must not edge");
    held = kbd_held_keyword(0, KBD_KEY_X);
    CHECK(kbd_edge_keyword(held, 0) == KBD_KEY_X, "re-pressed X must edge again");
}

static void test_no_control_tile_duplicates_a_button(void)
{
    CHECK(KBD_CONTROL_COUNT == 6, "expected six primary touch-only controls, got %d",
          KBD_CONTROL_COUNT);
    // The six screen destinations are dense.  RELINK is intentionally outside
    // that count so it cannot join d-pad focus traversal by accident.
    CHECK(KBD_CONTROL_SETTINGS == KBD_CONTROL_COUNT,
          "primary control ids must stay dense and end at SETTINGS");
    CHECK(KBD_CONTROL_RECONNECT == KBD_CONTROL_COUNT + 1,
          "RELINK must remain the touch-only utility after the primary grid");
}

// A design requirement, encoded as a regression test: typing happens live on
// the keyboard, and the composer leaves the default path. The KEYS tile owns
// the primary (top-left) slot, and the composer's tile sits on the row below
// it. A future reshuffle that puts the composer back on the primary path
// fails here.
//
// The rest of the row 1 order is deliberately not asserted -- which of
// COMPOSE, the extension's tile and SETTINGS sits where is a matter of taste,
// and a test that pins taste is a test somebody has to edit to change their
// mind. What is pinned is the requirement itself: the keyboard is first, and
// the buffer is not on the top row.
static void test_keys_is_the_primary_typing_control(void)
{
    int kx, ky, kw, kh, cx, cy, cw, ch;
    kbd_control_rect(KBD_CONTROL_KEYS, &kx, &ky, &kw, &kh);
    kbd_control_rect(KBD_CONTROL_COMPOSE, &cx, &cy, &cw, &ch);
    CHECK(kx == 6 && ky == 40, "KEYS must own the primary tile: %d,%d", kx, ky);
    CHECK(cy > ky + kh, "COMPOSE must be demoted below KEYS: %d vs %d", cy, ky + kh);
    CHECK(KBD_CONTROL_KEYS == 1, "the keyboard must be the first control");
}

static void test_kbd_control_geometry(void)
{
    for (int id = KBD_CONTROL_KEYS; id <= KBD_CONTROL_COUNT; id++) {
        int x, y, w, h;
        kbd_control_rect((kbd_control_t)id, &x, &y, &w, &h);
        CHECK(w > 0 && h > 0, "control %d has an empty rect", id);
        CHECK(x >= 0 && y >= 0 && x + w <= 256 && y + h <= 192,
              "control %d outside screen: %d,%d %dx%d", id, x, y, w, h);
        // Thumb-sized, not stylus-sized: these are pressed while holding the
        // console to read.
        CHECK(w >= KBD_W * 2 && h >= KBD_H, "control %d is not thumb-sized", id);
        CHECK(kbd_control_hit(x + w / 2, y + h / 2) == (kbd_control_t)id,
              "control %d centre does not hit itself", id);
        CHECK(kbd_control_hit(x, y) == (kbd_control_t)id,
              "control %d top-left does not hit itself", id);
        CHECK(kbd_control_hit(x + w, y) != (kbd_control_t)id,
              "control %d right edge is inclusive", id);
        CHECK(kbd_control_hit(x, y + h) != (kbd_control_t)id,
              "control %d bottom edge is inclusive", id);

        for (int other = id + 1; other <= KBD_CONTROL_COUNT; other++) {
            int ox, oy, ow, oh;
            kbd_control_rect((kbd_control_t)other, &ox, &oy, &ow, &oh);
            CHECK(x + w <= ox || ox + ow <= x || y + h <= oy || oy + oh <= y,
                  "controls %d and %d overlap", id, other);
        }
    }

    // The two status lines at the top of the Control screen are 6x8 cells on
    // rows 0 and 1, and the two hint lines are rows 22 and 23. A tile covering
    // either would be drawn over by the next repaint.
    for (int id = KBD_CONTROL_KEYS; id <= KBD_CONTROL_COUNT; id++) {
        int x, y, w, h;
        kbd_control_rect((kbd_control_t)id, &x, &y, &w, &h);
        CHECK(y >= 16, "control %d covers a readout line", id);
        CHECK(y + h <= 176, "control %d covers a hint line", id);
    }

    CHECK(kbd_control_hit(0, 0) == KBD_CONTROL_NONE, "screen origin should miss");
    CHECK(kbd_control_hit(128, 30) == KBD_CONTROL_NONE, "gap above the top row should miss");
    CHECK(kbd_control_hit(128, 95) == KBD_CONTROL_NONE, "gap between rows should miss");

    // A bad id fails closed: an all-zero rect can never be hit.
    int zx = 1, zy = 1, zw = 1, zh = 1;
    kbd_control_rect(KBD_CONTROL_NONE, &zx, &zy, &zw, &zh);
    CHECK(zx == 0 && zy == 0 && zw == 0 && zh == 0, "NONE should have an empty rect");
    kbd_control_rect((kbd_control_t)(KBD_CONTROL_EXTENSION + 1), &zx, &zy, &zw, &zh);
    CHECK(zw == 0 && zh == 0, "out-of-range control should have an empty rect");

    // No extension, no tile: the one rule that keeps the drawing and the hit
    // test agreeing about whether it exists.  With one, it is a small
    // touch-only utility that overlaps neither RELINK nor the grid.
    kbd_control_rect(KBD_CONTROL_EXTENSION, &zx, &zy, &zw, &zh);
    if (!ext_control_label()) {
        CHECK(zw == 0 && zh == 0, "an absent extension has a tile");
    } else {
        int rx, ry, rw, rh;
        kbd_control_rect(KBD_CONTROL_RECONNECT, &rx, &ry, &rw, &rh);
        CHECK(zx + zw <= rx && zy + zh <= 40,
              "the extension tile overlaps RELINK or the grid");
    }
    // REMOTE fills the sixth tile, the one that used to stand empty.
    int qx, qy, qw, qh;
    kbd_control_rect(KBD_CONTROL_REMOTE, &qx, &qy, &qw, &qh);
    CHECK(qx == 90 && qy == 100 && qw == 76 && qh == 52,
          "REMOTE is not the middle tile of the second row");
}

static void test_kbd_relink_hit(void)
{
    // RELINK is deliberately a small touch target, separate from the six
    // focusable screen destinations.  Its rendered rectangle and hit target
    // still have to be the same source of truth.
    int x, y, w, h;
    kbd_control_rect(KBD_CONTROL_RECONNECT, &x, &y, &w, &h);
    CHECK(w > 0 && h > 0 && w < 76 && h < 52,
          "RELINK should be a small recovery utility: %d,%d %dx%d", x, y, w, h);
    CHECK(kbd_control_hit(x + w / 2, y + h / 2) == KBD_CONTROL_RECONNECT,
          "RELINK centre does not hit RELINK");
    CHECK(kbd_control_hit(x, y) == KBD_CONTROL_RECONNECT,
          "RELINK top-left does not hit RELINK");
    CHECK(kbd_control_hit(x + w, y) != KBD_CONTROL_RECONNECT &&
          kbd_control_hit(x, y + h) != KBD_CONTROL_RECONNECT,
          "RELINK right or bottom edge is inclusive");

    focus_init();
    focus_on(FOCUS_CONTROL);
    for (int i = 0; i < 4; i++) focus_move(1, 0);
    CHECK(focus_control_id() == KBD_CONTROL_PHOTO,
          "touch-only RELINK leaked into the focused top row");
}

static void test_kbd_layers(void)
{
    kbd_reset();
    CHECK(kbd_layer() == KBD_LAYER_BASE, "not base initially");

    kbd_layer_cycle();
    CHECK(kbd_layer() == KBD_LAYER_SHIFT, "not shift after 1 cycle");
    kbd_layer_cycle();
    CHECK(kbd_layer() == KBD_LAYER_SYM, "not sym after 2 cycles");
    kbd_layer_cycle();
    CHECK(kbd_layer() == KBD_LAYER_BASE, "not base after 3 cycles");

    // The SHFT key drives the same cycle.
    uint8_t out[8];
    kbd_reset();
    CHECK(kbd_press(40, out, sizeof(out)) == -1, "SHFT press emits nothing");
    CHECK(kbd_layer() == KBD_LAYER_SHIFT, "SHFT key should go to shift");
}

static void test_kbd_shift_and_sym_bytes(void)
{
    uint8_t out[8];

    // '1' in the shift layer sends '!'.
    kbd_reset();
    kbd_layer_set(KBD_LAYER_SHIFT);
    const int id1 = kbd_id_for_char('1', KBD_LAYER_BASE);
    CHECK(id1 >= 0, "no key for '1'");
    CHECK(kbd_press(id1, out, sizeof(out)) == 1 && out[0] == '!',
          "shift '1' should send '!', got %d", out[0]);

    // '1' in the sym layer sends '`'.
    kbd_reset();
    kbd_layer_set(KBD_LAYER_SYM);
    CHECK(kbd_press(id1, out, sizeof(out)) == 1 && out[0] == '`',
          "sym '1' should send '`', got %d", out[0]);

    // Shift-tab on the TAB key in the shift layer.
    kbd_reset();
    kbd_layer_set(KBD_LAYER_SHIFT);
    CHECK(kbd_press(43, out, sizeof(out)) == 3, "S-TAB should be 3 bytes");
    CHECK(out[0] == 0x1b && out[1] == '[' && out[2] == 'Z', "S-TAB bytes wrong");

    // Tab in the base layer is a plain tab.
    kbd_reset();
    CHECK(kbd_press(43, out, sizeof(out)) == 1 && out[0] == '\t', "TAB should be tab");
}

static void test_kbd_ctrl_folding(void)
{
    uint8_t out[8];
    kbd_reset();

    // L (Ctrl) then 'c' -> 0x03, and the arm is consumed.
    kbd_ctrl_arm();
    CHECK(kbd_ctrl_armed(), "ctrl should be armed");
    kbd_layer_set(KBD_LAYER_BASE);
    const int idc = kbd_id_for_char('c', KBD_LAYER_BASE);
    CHECK(kbd_press(idc, out, sizeof(out)) == 1 && out[0] == 0x03,
          "L then c should fold to 0x03, got %d", out[0]);
    CHECK(!kbd_ctrl_armed(), "ctrl should disarm after a character key");

    // L then '[' (sym layer) -> 0x1b.
    kbd_ctrl_arm();
    kbd_layer_set(KBD_LAYER_SYM);
    const int idlb = kbd_id_for_char('[', KBD_LAYER_SYM);
    CHECK(kbd_press(idlb, out, sizeof(out)) == 1 && out[0] == 0x1b,
          "L then [ should fold to 0x1b, got %d", out[0]);
    CHECK(!kbd_ctrl_armed(), "ctrl should disarm after '['");

    // Arming twice disarms (the doc's "pressing L twice disarms").
    kbd_reset();
    kbd_ctrl_arm();
    kbd_ctrl_arm();
    CHECK(!kbd_ctrl_armed(), "double arm should disarm");

    // A non-character key leaves the arm alone: Ctrl then Enter keeps it armed.
    kbd_reset();
    kbd_ctrl_arm();
    CHECK(kbd_press(47, out, sizeof(out)) == 1 && out[0] == '\r', "ENTER should send CR");
    CHECK(kbd_ctrl_armed(), "Enter must not consume the Ctrl arm");
    kbd_reset();
}

static void test_kbd_shift_one_shot(void)
{
    uint8_t out[8];
    kbd_reset();

    // R (Shift) then 'a' -> 'A', and the arm is consumed.
    kbd_shift_arm();
    CHECK(kbd_shift_armed(), "shift should be armed");
    kbd_layer_set(KBD_LAYER_BASE);
    const int ida = kbd_id_for_char('a', KBD_LAYER_BASE);
    CHECK(kbd_press(ida, out, sizeof(out)) == 1 && out[0] == 'A',
          "R then a should send 'A', got %d", out[0]);
    CHECK(!kbd_shift_armed(), "shift should disarm after a key");

    // R then '1' -> '!' (the shifted byte, not the base or sym byte).
    kbd_shift_arm();
    const int id1 = kbd_id_for_char('1', KBD_LAYER_BASE);
    CHECK(kbd_press(id1, out, sizeof(out)) == 1 && out[0] == '!',
          "R then 1 should send '!', got %d", out[0]);
    CHECK(!kbd_shift_armed(), "shift should disarm after '1'");

    // Arming twice disarms.
    kbd_reset();
    kbd_shift_arm();
    kbd_shift_arm();
    CHECK(!kbd_shift_armed(), "double arm should disarm");

    // R then Tab (touch TAB key) -> Shift-Tab, and the arm is consumed.
    kbd_reset();
    kbd_shift_arm();
    CHECK(kbd_press(43, out, sizeof(out)) == 3, "R then TAB should be 3 bytes");
    CHECK(out[0] == 0x1b && out[1] == '[' && out[2] == 'Z', "R+TAB bytes wrong");
    CHECK(!kbd_shift_armed(), "R+TAB should consume the arm");

    // R then Enter sends Enter and consumes the arm.
    kbd_reset();
    kbd_shift_arm();
    CHECK(kbd_press(47, out, sizeof(out)) == 1 && out[0] == '\r', "R+ENT should send CR");
    CHECK(!kbd_shift_armed(), "R+ENT should consume the arm");
    kbd_reset();
}

static void test_kbd_ctrl_and_shift_together(void)
{
    // L and R together: a shifted byte is folded through Ctrl. Shift applies
    // first ('c' -> 'C'), then Ctrl folds (0x43 & 0x1f == 0x03); both disarm.
    uint8_t out[8];
    kbd_reset();
    kbd_ctrl_arm();
    kbd_shift_arm();
    kbd_layer_set(KBD_LAYER_BASE);
    const int idc = kbd_id_for_char('c', KBD_LAYER_BASE);
    CHECK(kbd_press(idc, out, sizeof(out)) == 1 && out[0] == 0x03,
          "L+R+c should fold to 0x03, got %d", out[0]);
    CHECK(!kbd_ctrl_armed(), "ctrl should disarm");
    CHECK(!kbd_shift_armed(), "shift should disarm");
    kbd_reset();
}

static void test_kbd_repeat_timing(void)
{
    for (unsigned f = 0; f < KBD_REPEAT_DELAY; f++) {
        CHECK(!kbd_repeat_should_fire(f), "should stay quiet through frame %u", f);
    }
    CHECK(kbd_repeat_should_fire(KBD_REPEAT_DELAY), "first repeat at the delay");
    for (unsigned f = KBD_REPEAT_DELAY + 1; f < KBD_REPEAT_DELAY + KBD_REPEAT_INTERVAL; f++) {
        CHECK(!kbd_repeat_should_fire(f), "no repeat between intervals (frame %u)", f);
    }
    CHECK(kbd_repeat_should_fire(KBD_REPEAT_DELAY + KBD_REPEAT_INTERVAL),
          "repeat again one interval later");
}

static void test_kbd_repeatable(void)
{
    CHECK(kbd_repeatable(kbd_id_for_char('c', KBD_LAYER_BASE)), "letter should repeat");
    CHECK(kbd_repeatable(48), "backspace should repeat");
    CHECK(kbd_repeatable(44), "space should repeat");
    CHECK(!kbd_repeatable(47), "enter must not repeat");
    CHECK(!kbd_repeatable(42), "escape must not repeat");
    CHECK(!kbd_repeatable(40), "SHFT must not repeat");
    CHECK(!kbd_repeatable(43), "TAB must not repeat");
}

// ---------------------------------------------------------- key sequences ---

static void test_page_keys_are_complete_sequences(void)
{
    // The bug this exists for: the scroll gesture passed a hand-written length
    // of 3 for a 4-byte sequence, so the card sent ESC [ 5 and never the final
    // '~'. It reached the flashed ROM because nothing in this suite could see
    // a constant that lived in main.c.
    CHECK(SEQ_PAGE_LEN == 4, "a page key is four bytes, got %u",
          (unsigned)SEQ_PAGE_LEN);
    CHECK(strlen(SEQ_PAGE_UP) == SEQ_PAGE_LEN,
          "SEQ_PAGE_LEN must equal the length of the literal it is taken from");
    CHECK(strlen(SEQ_PAGE_DOWN) == SEQ_PAGE_LEN,
          "both page keys must be the length SEQ_PAGE_LEN claims");
}

static void test_page_keys_are_the_bytes_a_keyboard_sends(void)
{
    // Pinned as bytes rather than as a string, because the failure mode is a
    // missing byte and comparing a truncated CSI against a truncated
    // expectation would agree with itself.
    const unsigned char up[]   = {0x1b, '[', '5', '~'};
    const unsigned char down[] = {0x1b, '[', '6', '~'};
    CHECK(memcmp(SEQ_PAGE_UP, up, sizeof(up)) == 0, "PageUp is ESC [ 5 ~");
    CHECK(memcmp(SEQ_PAGE_DOWN, down, sizeof(down)) == 0, "PageDown is ESC [ 6 ~");

    // A final byte in the CSI terminator range is what makes the sequence
    // complete. Without it the receiving terminal stays mid-sequence and eats
    // the next keystroke, which is why a truncation here reads as a dead
    // keyboard rather than as a scroll that did nothing.
    const char last_up = SEQ_PAGE_UP[SEQ_PAGE_LEN - 1];
    const char last_down = SEQ_PAGE_DOWN[SEQ_PAGE_LEN - 1];
    CHECK(last_up >= 0x40 && last_up <= 0x7e, "PageUp ends with a final byte");
    CHECK(last_down >= 0x40 && last_down <= 0x7e, "PageDown ends with a final byte");
}

static void test_a_page_key_survives_being_framed(void)
{
    // The whole path rather than the constant alone: what reaches the wire has
    // to be the four bytes, with the header's length agreeing.
    uint8_t out[PROTO_HEADER_SIZE + PROTO_MAX_KEY_PAYLOAD];
    const size_t n = proto_encode(out, MSG_KEY, (const uint8_t *)SEQ_PAGE_UP,
                                  (uint16_t)SEQ_PAGE_LEN);
    CHECK(n == PROTO_HEADER_SIZE + SEQ_PAGE_LEN, "framed length is header plus four");
    CHECK(out[4] == 0 && out[5] == SEQ_PAGE_LEN, "header declares four payload bytes");
    CHECK(memcmp(out + PROTO_HEADER_SIZE, "\x1b[5~", 4) == 0,
          "the payload on the wire is the complete sequence");
}

// ------------------------------------------------------------- backoff -----

static void test_backoff_starts_short(void)
{
    // A restarted server is back in about a second. Waiting longer than that on
    // the first attempt makes a routine  look like a crash.
    CHECK(backoff_frames(1) == BACKOFF_FIRST, "first wait is BACKOFF_FIRST");
    CHECK(backoff_frames(1) <= 60, "first wait is at most a second");
}

static void test_backoff_doubles(void)
{
    CHECK(backoff_frames(2) == BACKOFF_FIRST * 2, "second doubles");
    CHECK(backoff_frames(3) == BACKOFF_FIRST * 4, "third doubles again");
}

static void test_backoff_is_capped(void)
{
    // Without a ceiling the wait doubles into hours and the handheld never
    // notices the PC coming back.
    for (unsigned a = 1; a < 64; a++) {
        CHECK(backoff_frames(a) <= BACKOFF_MAX, "never exceeds the cap");
    }
    CHECK(backoff_frames(20) == BACKOFF_MAX, "reaches the cap and holds");
}

static void test_backoff_never_returns_zero(void)
{
    // A zero wait is a spin: it would hammer a dead address sixty times a
    // second and flood the log ring with reconnect lines.
    for (unsigned a = 0; a < 64; a++) {
        CHECK(backoff_frames(a) > 0, "always waits at least one frame");
    }
}

static void test_backoff_is_monotonic(void)
{
    for (unsigned a = 1; a < 40; a++) {
        CHECK(backoff_frames(a + 1) >= backoff_frames(a), "never shrinks");
    }
}

// ------------------------------------------------------------- interop -----

static void print_interop_vectors(void)
{
    // Printed for tools/crosscheck.py to verify against the Python server, so
    // the two implementations are known to agree rather than assumed to.
    uint8_t key[16];
    uint8_t nonce[32];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) nonce[i] = (uint8_t)(0xF0 - i);

    uint8_t mac[32];
    char h[65];
    hmac_sha256(key, sizeof(key), nonce, sizeof(nonce), mac);
    hex(mac, 32, h);
    printf("INTEROP hmac %s\n", h);

    uint8_t frame[16];
    const size_t n = proto_encode(frame, MSG_AUTH, (const uint8_t *)"xy", 2);
    char fh[33];
    hex(frame, n, fh);
    printf("INTEROP frame %s\n", fh);

    // One PROJECT row, laid out the way this decoder expects to receive it.
    //
    // This is the lockstep check the pack format never had: the layout lives
    // in two implementations and nothing but a comment has ever held them
    // together.  Building the frame here and having crosscheck.py rebuild it
    // with the server's own encoder makes a divergence a failed test rather
    // than a page of question marks on a handheld.
    static const uint8_t PROJECT_ROW[] = {
        2,          // count
        0,          // index
        0,          // tab: NOW
        9,          // fg: orange
        6,          // label_len
        4,          // value_len
        'b', 'r', 'a', 'n', 'c', 'h',
        'm', 'a', 'i', 'n',
    };
    uint8_t pframe[32];
    const size_t pn = proto_encode(pframe, MSG_PROJECT, PROJECT_ROW,
                                   (uint16_t)sizeof(PROJECT_ROW));
    char ph[65];
    hex(pframe, pn, ph);
    printf("INTEROP project %s\n", ph);

    // One CLIP chunk, the same lockstep check for the same reason.  The
    // interesting fields are the ones the two of us disagreed about while
    // designing it: `total` is a u32 rather than a saturating u16 -- a 200 KB
    // clipboard reported as 65.5K is a wrong number, not a coarse one -- and
    // it counts the CONVERTED text, after the server's ASCII fold and newline
    // normalisation, which is what makes the received bytes exactly
    // min(total, CLIP_TEXT_MAX).
    //
    // The body carries a newline on purpose.  It is the one control byte that
    // survives the fold, and a vector made only of letters would not notice a
    // decoder that had quietly stopped allowing it.
    static const uint8_t CLIP_CHUNK[] = {
        2,                       // count
        1,                       // index: the second and last chunk
        0x2A,                    // seq
        0x03,                    // flags: truncated | transliterated
        0x00, 0x01, 0x11, 0x70,  // total: 70000, big-endian
        3,                       // len
        'a', CLIP_LF, 'b',
    };
    uint8_t cframe[32];
    const size_t cn = proto_encode(cframe, MSG_CLIP, CLIP_CHUNK,
                                   (uint16_t)sizeof(CLIP_CHUNK));
    char ch[65];
    hex(cframe, cn, ch);
    printf("INTEROP clip %s\n", ch);

    // The two-byte empty frames, both of them, because the whole reason the
    // empty frame carries a flag byte is that these two must not be the same
    // bytes.  An empty pane after a concealed copy invites the user to copy
    // the password again.
    static const uint8_t CLIP_EMPTY[] = { 0, 0 };
    static const uint8_t CLIP_WITHHELD[] = { 0, CLIP_FLAG_WITHHELD };
    uint8_t eframe[16];
    size_t en = proto_encode(eframe, MSG_CLIP, CLIP_EMPTY,
                             (uint16_t)sizeof(CLIP_EMPTY));
    char eh[33];
    hex(eframe, en, eh);
    printf("INTEROP clip-empty %s\n", eh);
    en = proto_encode(eframe, MSG_CLIP, CLIP_WITHHELD,
                      (uint16_t)sizeof(CLIP_WITHHELD));
    hex(eframe, en, eh);
    printf("INTEROP clip-withheld %s\n", eh);

    // An Enter tied to question 0xC0FFEE01: the server must read the same id
    // and the same key byte out of it.
    uint8_t aframe[PROTO_HEADER_SIZE + PROTO_ANSWER_MAX];
    const size_t an = proto_encode_answer(aframe, 0xC0FFEE01u,
                                          (const uint8_t *)"\r", 1);
    char ah[2 * sizeof(aframe) + 1];
    hex(aframe, an, ah);
    printf("INTEROP answer %s\n", ah);

    // Voice ADPCM and wire vectors, pinned against dsterm/voice.py and
    // dsterm/protocol.py.
    {
        int16_t pcm[16], decoded[16];
        uint8_t adpcm[8];
        for (int i = 0; i < 16; i++) pcm[i] = (int16_t)(i * 2000 - 15000);
        voice_adpcm_state_t st;
        voice_adpcm_init(&st);
        CHECK(voice_adpcm_encode(&st, pcm, 16, adpcm) == 8, "adpcm encode len");
        voice_adpcm_init(&st);
        CHECK(voice_adpcm_decode(&st, adpcm, 8, decoded) == 16, "adpcm decode len");
        uint8_t pcm_le[32];
        for (int i = 0; i < 16; i++) {
            const uint16_t u = (uint16_t)decoded[i];
            pcm_le[2 * i] = (uint8_t)(u & 0xFF);
            pcm_le[2 * i + 1] = (uint8_t)(u >> 8);
        }
        char h_adpcm[17], h_pcm[65];
        hex(adpcm, 8, h_adpcm);
        hex(pcm_le, 32, h_pcm);
        printf("INTEROP voice-adpcm %s:%s\n", h_adpcm, h_pcm);

        uint8_t wbuf[128];
        size_t wlen = 0;
        wlen += proto_encode_voice_start(wbuf + wlen, VOICE_FORMAT_IMA_ADPCM_8K, "es");
        uint8_t chunk[32];
        for (int i = 0; i < 32; i++) chunk[i] = (uint8_t)((i * 17) & 0xFF);
        wlen += proto_encode_voice_chunk(wbuf + wlen, 480, chunk, 32);
        wlen += proto_encode_voice_end(wbuf + wlen, 512, 0xDEADBEEFu);
        char h_wire[257];
        hex(wbuf, wlen, h_wire);
        printf("INTEROP voice-wire %s\n", h_wire);
    }
}

static void test_lang_and_custom_packs(void)
{
    lang_init();
    CHECK(lang_count() == LANG_BUILTIN_COUNT && lang_current() == LANG_EN,
          "default language is not English");
    CHECK(strcmp(lang_code(LANG_EN), "en") == 0 &&
              strcmp(lang_code(LANG_ES), "es") == 0,
          "built-in language codes do not match en/es");
    CHECK(strcmp(tr(TR_CONTROL_KEYS), "KEYS") == 0, "EN control.keys wrong");

    lang_set_current(LANG_ES);
    CHECK(strcmp(tr(TR_CONTROL_KEYS), "TECLAS") == 0, "ES control.keys wrong");
    CHECK(lang_display_cols("键盘") == 4, "CJK 2-char string is not 4 cols");
    lang_set_current(LANG_ZH);
    CHECK(strcmp(tr(TR_CONTROL_KEYS), "键盘") == 0, "ZH control.keys wrong");
    lang_set_current(LANG_JA);
    CHECK(strcmp(tr(TR_CONTROL_KEYS), "キー") == 0, "JA control.keys wrong");
    CHECK(lang_count() == 4 && lang_find_code("zh") == LANG_ZH &&
              lang_find_code("ja") == LANG_JA,
          "Chinese and Japanese are not offered");

    // Every non-ASCII codepoint in every built-in language must have a glyph
    // -- composed (lang_latin_glyph) or in ui_font_table -- so nothing
    // renders as '?' on screen.
    int missing_glyphs = 0;
    for (unsigned l = 0; l < LANG_BUILTIN_COUNT; l++) {
        for (unsigned id = 0; id < TR_COUNT; id++) {
            const char *s = lang_tr_at(l, (tr_id_t)id);
            uint32_t cp;
            while ((cp = lang_utf8_next(&s)) != 0) {
                uint8_t composed[8];
                if (cp >= 0x80 && !ui_font_lookup(cp) &&
                    !lang_latin_glyph(cp, font6x8, composed)) {
                    missing_glyphs++;
                }
            }
        }
    }
    CHECK(missing_glyphs == 0, "%d built-in UI codepoints missing from ui_font.h",
          missing_glyphs);

    // Custom .lang import: adds a chip, translates set keys, falls back to EN
    // for unset keys, and persists in settings.cfg.
    char err[64] = "";
    const int fr = lang_load_text(
        "code = fr\nlabel = FR\nname = Français\ncontrol.keys = CLAVIER\n",
        NULL, err, sizeof(err));
    CHECK(fr == LANG_BUILTIN_COUNT && lang_count() == LANG_BUILTIN_COUNT + 1,
          "custom language pack did not register: %s", err);
    lang_set_current((unsigned)fr);
    CHECK(strcmp(tr(TR_CONTROL_KEYS), "CLAVIER") == 0 &&
              strcmp(tr(TR_CONTROL_REMOTE), "REMOTE") == 0,
          "custom language did not override control.keys and fall back to EN");

    settings_t st, back;
    settings_ui_t ui;
    settings_defaults(&st);
    settings_ui_init(&ui);
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_LANGUAGE, (unsigned)fr, NULL) ==
              SETTINGS_PRESS_CHANGED &&
          st.language == (unsigned)fr,
          "SETTINGS_ROW_LANGUAGE could not select imported pack");
    static const char lpath[] = "/tmp/dsterm-lang-settings.cfg";
    CHECK(settings_save(lpath, &st, err, sizeof(err)) &&
              settings_load(lpath, &back, err, sizeof(err)) &&
              back.language == (unsigned)fr,
          "custom language did not survive settings save/load: %s", err);
    remove(lpath);

    // Reset back to the built-ins for subsequent tests.
    lang_init();
}

static void test_latin_glyphs_are_letters_with_marks(void)
{
    // The atlas drew a, á, à and â as near-identical blobs.  Composed from
    // the terminal font, each accented letter is its own letter exactly
    // (lower case) or with one repeated row given up (upper case), plus a
    // mark that no other mark shares, clear of the letter's own pixels.
    uint8_t g[8], h[8];
    static const struct { uint32_t cp; char base; } LOWER[] = {
        { 0xE1, 'a' }, { 0xE9, 'e' }, { 0xF3, 'o' }, { 0xFA, 'u' },
        { 0xF1, 'n' }, { 0xE0, 'a' }, { 0xFC, 'u' }, { 0xFF, 'y' },
    };
    for (unsigned i = 0; i < sizeof(LOWER) / sizeof(LOWER[0]); i++) {
        CHECK(lang_latin_glyph(LOWER[i].cp, font6x8, g),
              "U+%04X was not composed", (unsigned)LOWER[i].cp);
        const uint8_t *base = font6x8[LOWER[i].base - 0x20];
        CHECK(memcmp(g + 2, base + 2, 6) == 0,
              "U+%04X changed its letter", (unsigned)LOWER[i].cp);
        CHECK((g[0] | g[1]) != 0, "U+%04X has no mark", (unsigned)LOWER[i].cp);
    }
    // i loses its dot to the mark rather than wearing both.
    CHECK(lang_latin_glyph(0xED, font6x8, g) && g[1] == 0x08 && g[0] == 0x04,
          "í kept its dot under the accent");
    // Marks are told apart: á, à, â, ã, ä all differ, and differ from a.
    uint8_t seen[5][8];
    for (unsigned m = 0; m < 5; m++) {
        CHECK(lang_latin_glyph(0xE0 + m, font6x8, seen[m]), "a-variant %u", m);
        CHECK(memcmp(seen[m], font6x8['a' - 0x20], 8) != 0, "a-variant %u is plain a", m);
        for (unsigned k = 0; k < m; k++) {
            CHECK(memcmp(seen[m], seen[k], 8) != 0,
                  "a-variants %u and %u draw the same glyph", k, m);
        }
    }
    // Upper case: mark on row 0, a clear row under it, the letter below.
    CHECK(lang_latin_glyph(0xC1, font6x8, g) && g[0] != 0 && g[1] == 0 &&
              g[7] == 0 && g[2] == font6x8['A' - 0x20][1],
          "Á is not mark, gap, A");
    CHECK(lang_latin_glyph(0xD1, font6x8, h) && memcmp(g, h, 8) != 0,
          "Ñ and Á are the same glyph");
    // Cedilla hangs below.
    CHECK(lang_latin_glyph(0xE7, font6x8, g) && g[7] != 0 &&
              memcmp(g, font6x8['c' - 0x20], 7) == 0,
          "ç is not c with a hanging mark");
    // Not letters with marks: left to the atlas.
    CHECK(!lang_latin_glyph(0xD7, font6x8, g) && !lang_latin_glyph(0xDF, font6x8, g) &&
              !lang_latin_glyph(0xBF, font6x8, g) && !lang_latin_glyph('a', font6x8, g) &&
              !lang_latin_glyph(0x4E2D, font6x8, g),
          "something that is not an accented letter was composed");
}

static void test_settings_unknown_language_keeps_the_rest(void)
{
    // A language code nothing answers to -- a custom pack taken off the SD,
    // or a code from some other build -- shows English and keeps every other
    // setting.
    lang_init();
    settings_t st;
    char err[64] = "";
    static const char *const CODES[] = { "xx-gone", "fr", "" };
    for (unsigned i = 0; i < sizeof(CODES) / sizeof(CODES[0]); i++) {
        char text[160];
        (void)snprintf(text, sizeof(text),
                       "version=4\ngod_mode=on\ntyping=off\nlang=%s\n"
                       "button_b=space\n", CODES[i]);
        settings_t defaults;
        settings_defaults(&defaults);
        CHECK(settings_parse(text, &st, err, sizeof(err)),
              "lang=%s threw the whole file away: %s", CODES[i], err);
        CHECK(st.language == LANG_EN, "lang=%s did not fall back to English",
              CODES[i]);
        CHECK(st.god_mode == 1 && st.typing == 0 &&
                  st.buttons.action[BUTTON_B] != defaults.buttons.action[BUTTON_B],
              "lang=%s reset the settings beside it", CODES[i]);
    }
    CHECK(settings_parse("version=4\nlang=es\n", &st, err, sizeof(err)) &&
              st.language == LANG_ES,
          "a known language no longer loads");
    CHECK(settings_parse("version=4\nlang=zh\n", &st, err, sizeof(err)) &&
              st.language == LANG_ZH &&
              settings_parse("version=4\nlang=ja\n", &st, err, sizeof(err)) &&
              st.language == LANG_JA,
          "Chinese or Japanese does not load from the settings file");
}

static void test_chime_headline_every_language(void)
{
    // English is the bytes it always was; Spanish covers every state English
    // has (it used to collapse the lantern, SENT, STREAMING and DONE into
    // two lines); and the approval line follows a language change without
    // being rebuilt out of its own buffers.
    lang_init();
    chime_init();
    reset_sound();
    chime_set_answer_buttons("A", "X");
    CHECK(strcmp(chime_headline(), "Ready") == 0, "EN idle: '%s'", chime_headline());
    chime_sent();
    CHECK(strcmp(chime_headline(), "Sent -- waiting for Claude") == 0,
          "EN sent: '%s'", chime_headline());
    lang_set_current(LANG_ES);
    CHECK(strcmp(chime_headline(), "Enviado -- esperando a Claude") == 0,
          "ES sent: '%s'", chime_headline());
    lang_set_current(LANG_EN);
    chime_session_lantern();
    CHECK(strcmp(chime_headline(), "Another session has new output") == 0,
          "EN lantern: '%s'", chime_headline());
    lang_set_current(LANG_ES);
    CHECK(strcmp(chime_headline(), "Otra sesión tiene salida nueva") == 0,
          "ES lantern: '%s'", chime_headline());
    lang_set_current(LANG_EN);
    chime_init();                          // the lantern's moment is over

    chime_claude_state(2);
    CHECK(strcmp(chime_headline(), "Claude is working") == 0, "EN working");
    chime_claude_state(4);
    CHECK(strcmp(chime_headline(), "Claude asked you something") == 0, "EN asked");
    chime_claude_state(3);
    CHECK(strcmp(chime_headline(), "Claude needs your OK:  A yes   X no") == 0,
          "EN approval: '%s'", chime_headline());
    lang_set_current(LANG_ES);
    CHECK(strcmp(chime_headline(), "Claude necesita tu OK:  A sí   X no") == 0,
          "ES approval: '%s'", chime_headline());
    chime_set_answer_buttons("B", NULL);
    CHECK(strcmp(chime_headline(), "Claude necesita tu OK:  B sí") == 0,
          "ES approval, one button: '%s'", chime_headline());
    lang_set_current(LANG_EN);
    CHECK(strcmp(chime_headline(), "Claude needs your OK:  B yes") == 0,
          "EN approval after switching back: '%s'", chime_headline());
    chime_claude_state(1);
    CHECK(strcmp(chime_headline(), "Done -- your turn") == 0,
          "EN done: '%s'", chime_headline());
    lang_set_current(LANG_ES);
    CHECK(strcmp(chime_headline(), "Hecho -- tu turno") == 0,
          "ES done: '%s'", chime_headline());

    // A custom pack with none of the headline keys reads English lines, not
    // another state's wording.
    char err[64] = "";
    const int fr = lang_load_text("code = fr\ncontrol.keys = CLAVIER\n",
                                  NULL, err, sizeof(err));
    CHECK(fr >= 0, "custom pack refused: %s", err);
    lang_set_current((unsigned)fr);
    CHECK(strcmp(chime_headline(), "Done -- your turn") == 0,
          "a pack without chime.done did not fall back to English");

    chime_set_answer_buttons("A", "X");
    lang_init();
    chime_init();
    reset_sound();
}

static void test_custom_pack_labels_never_push_off_screen(void)
{
    // A custom pack may name a tab or a row anything up to 63 bytes. The tab
    // chips still fit the row, and no option column runs past its cap.
    lang_init();
    char err[64] = "";
    const int xx = lang_load_text(
        "code = xx\n"
        "settings.tab.session = AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"
        "settings.tab.sound = BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n"
        "settings.row.sound = CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\n"
        "settings.row.mic = DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD\n",
        NULL, err, sizeof(err));
    CHECK(xx >= 0, "custom pack refused: %s", err);
    lang_set_current((unsigned)xx);
    int x, y, w, h;
    CHECK(settings_tab_rect(SETTINGS_TAB_COUNT - 1, &x, &y, &w, &h) &&
              x + w <= 256 - 20,
          "a long custom tab name pushed the tabs to %d px", x + w);
    for (unsigned r = 0; r < SETTINGS_ROW_COUNT; r++) {
        CHECK(settings_row_option_x((settings_row_t)r) <= 124,
              "row %u's chips start at %d px", r,
              settings_row_option_x((settings_row_t)r));
    }
    lang_init();
}

// Every key in every built-in language, and every string that used to be
// English-only measured against the box it is drawn in, with the same 6 px
// per narrow / 12 px per wide character the renderer advances by.
static int tr_px(const char *s) { return (int)lang_display_cols(s) * 6; }

static void test_lang_coverage_and_budgets(void)
{
    lang_init();
    for (unsigned l = 0; l < LANG_BUILTIN_COUNT; l++) {
        for (unsigned id = 0; id < TR_COUNT; id++) {
            const char *s = lang_builtin_string(l, (tr_id_t)id);
            CHECK(s != NULL, "lang %s has no string for %s", lang_code(l),
                  lang_key_name((tr_id_t)id));
            CHECK(lang_key_name((tr_id_t)id) != NULL, "key %u has no name", id);
        }
    }

    // English is the bytes the screens showed before these were keys.
    static const struct { tr_id_t id; const char *en; } PINNED[] = {
        { TR_QUICK_FOOT,          "the tile shows Claude's mode" },
        { TR_QUICK_ADD,           "+ ADD" },
        { TR_QUICK_CMD_HEAD,      "COMMANDS   A compose   X back" },
        { TR_QUICK_CMD_HEAD_SAVED,"COMMANDS   A compose   Y forget   X back" },
        { TR_QUICK_CMD_HEAD_SURE, "COMMANDS   A compose   Y SURE?   X back" },
        { TR_QUICK_CMD_HEAD_ADD,  "COMMANDS   A add command   X back" },
        { TR_QUICK_CMD_SUB,       "built in, saved (+ ADD), then computer's" },
        { TR_QUICK_AGAIN_HEAD,    "AGAIN   A compose   Y forget   X back" },
        { TR_QUICK_AGAIN_SUB,     "what you sent lately, newest first" },
        { TR_LEGEND_SHOULDERS,    "L ctrl  R shift  L+R pick a tile" },
        { TR_SETTINGS_RESET_SURE, "SURE?" },
        { TR_COMPOSE_ADD_HEAD,    "ADD QUICK COMMAND: A saves on card" },
        { TR_COMPOSE_ADD_KEYS,    "A saves   B deletes   X cancels" },
        { TR_COMPOSE_ADD_FOCUS_KEYS, "START saves   B deletes   X leaves focus" },
        { TR_QUICK_NOTE_NOT_HERE, "not on this computer" },
        { TR_QUICK_NOTE_NO_STT,   "no speech-to-text on server" },
        { TR_QUICK_NOTE_NO_SAVE,  "cannot save pictures here" },
    };
    for (size_t i = 0; i < sizeof(PINNED) / sizeof(PINNED[0]); i++) {
        CHECK(strcmp(lang_tr_at(LANG_EN, PINNED[i].id), PINNED[i].en) == 0,
              "EN %s changed: '%s'", lang_key_name(PINNED[i].id),
              lang_tr_at(LANG_EN, PINNED[i].id));
    }

    char buf[160];
    // The MIC meter, filled, is the bytes snprintf used to make.
    lang_set_current(LANG_EN);
    for (unsigned bars = 0; bars <= 8; bars++) {
        static const tr_id_t METER[] = { TR_COMPOSE_METER_LATCHED,
                                         TR_COMPOSE_METER_LATCH,
                                         TR_COMPOSE_METER_HOLD };
        static const char *const OLD[] = { "MIC [%-8.*s]  R/L stop  X cancel",
                                           "MIC [%-8.*s]  L latch  X cancel",
                                           "MIC [%-8.*s]  X cancel" };
        char level[9], want[64];
        snprintf(level, sizeof(level), "%-8.*s", (int)bars, "########");
        for (size_t m = 0; m < 3; m++) {
            snprintf(want, sizeof(want), OLD[m], (int)bars, "########");
            lang_fill(buf, sizeof(buf), tr(METER[m]), level);
            CHECK(!strcmp(buf, want), "EN meter changed: '%s'", buf);
        }
    }
    CHECK(strcmp(lang_fill(buf, sizeof(buf), "d-pad picks  %s confirms", "A"),
                 "d-pad picks  A confirms") == 0, "fill: '%s'", buf);
    CHECK(strcmp(lang_fill(buf, sizeof(buf), "%d %s %s", "X"), "%d X %s") == 0,
          "fill read more than one %%s, or a format: '%s'", buf);
    CHECK(strcmp(lang_fill(buf, 5, "ab選択", NULL), "ab") == 0,
          "fill cut a character in half: '%s'", buf);

    int tx, ty, tile_w, th, relink_w;
    kbd_control_rect(KBD_CONTROL_KEYS, &tx, &ty, &tile_w, &th);
    kbd_control_rect(KBD_CONTROL_RECONNECT, &tx, &ty, &relink_w, &th);
    static const tr_id_t TILE[] = {
        TR_CONTROL_KEYS, TR_CONTROL_KEYS_SUB, TR_CONTROL_QUICK,
        TR_CONTROL_QUICK_SUB, TR_CONTROL_PHOTO, TR_CONTROL_PHOTO_SUB,
        TR_CONTROL_AGAIN, TR_CONTROL_AGAIN_SUB, TR_CONTROL_COMPOSE,
        TR_CONTROL_COMPOSE_SUB, TR_CONTROL_REMOTE, TR_CONTROL_REMOTE_SUB,
        TR_CONTROL_SETTINGS, TR_CONTROL_SETTINGS_SUB,
    };
    static const tr_id_t LINE[] = {
        TR_QUICK_FOOT, TR_QUICK_CMD_HEAD, TR_QUICK_CMD_HEAD_SAVED,
        TR_QUICK_CMD_HEAD_SURE, TR_QUICK_CMD_HEAD_ADD, TR_QUICK_CMD_SUB,
        TR_QUICK_AGAIN_HEAD, TR_QUICK_AGAIN_SUB, TR_LEGEND_SHOULDERS,
        TR_COMPOSE_ADD_HEAD, TR_COMPOSE_ADD_KEYS, TR_COMPOSE_ADD_FOCUS_KEYS,
    };
    static const char *const MODES[] = { "normal", "manual", "bypass", "edits",
                                         "plan", "auto" };
    for (unsigned l = 0; l < LANG_BUILTIN_COUNT; l++) {
        lang_set_current(l);
        const char *code = lang_code(l);
        // Control tiles: two pixels clear on each side.
        for (size_t i = 0; i < sizeof(TILE) / sizeof(TILE[0]); i++) {
            CHECK(tr_px(tr(TILE[i])) <= tile_w - 4, "%s %s is %dpx, tile %dpx: '%s'",
                  code, lang_key_name(TILE[i]), tr_px(tr(TILE[i])), tile_w,
                  tr(TILE[i]));
        }
        for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
            lang_fill(buf, sizeof(buf), tr(TR_CONTROL_QUICK_MODE), MODES[i]);
            CHECK(tr_px(buf) <= tile_w - 4, "%s QUICK tile '%s' is %dpx",
                  code, buf, tr_px(buf));
        }
        CHECK(tr_px(tr(TR_CONTROL_RELINK)) <= relink_w - 4,
              "%s RELINK '%s' is %dpx, tile %dpx", code, tr(TR_CONTROL_RELINK),
              tr_px(tr(TR_CONTROL_RELINK)), relink_w);
        // Row 3's hints stop at column 30, where RELINK starts.
        lang_fill(buf, sizeof(buf), tr(TR_CONTROL_HINT_CONFIRM), "A");
        CHECK(tr_px(buf) <= 180, "%s confirm hint '%s' is %dpx", code, buf, tr_px(buf));
        lang_fill(buf, sizeof(buf), tr(TR_CONTROL_HINT_UNDO), "A");
        CHECK(tr_px(buf) <= 180, "%s undo hint '%s' is %dpx", code, buf, tr_px(buf));
        CHECK(tr_px(tr(TR_CONTROL_HINT_SCROLL)) <= 180, "%s scroll hint is %dpx",
              code, tr_px(tr(TR_CONTROL_HINT_SCROLL)));
        CHECK(tr_px(tr(TR_CONTROL_HINT_FOCUS)) <= 180, "%s focus hint is %dpx",
              code, tr_px(tr(TR_CONTROL_HINT_FOCUS)));
        // Full-width lines: 42 columns, 252 px.
        for (size_t i = 0; i < sizeof(LINE) / sizeof(LINE[0]); i++) {
            CHECK(tr_px(tr(LINE[i])) <= 252, "%s %s is %dpx: '%s'", code,
                  lang_key_name(LINE[i]), tr_px(tr(LINE[i])), tr(LINE[i]));
        }
        snprintf(buf, sizeof(buf), " %s", tr(TR_QUICK_AGAIN_EMPTY_0));
        CHECK(tr_px(buf) <= 252, "%s empty AGAIN line 0 is %dpx", code, tr_px(buf));
        snprintf(buf, sizeof(buf), " %s", tr(TR_QUICK_AGAIN_EMPTY_1));
        CHECK(tr_px(buf) <= 252, "%s empty AGAIN line 1 is %dpx", code, tr_px(buf));
        snprintf(buf, sizeof(buf), " %s", tr(TR_QUICK_ADD));
        CHECK(tr_px(buf) <= 252, "%s + ADD row is %dpx", code, tr_px(buf));
        // The meter, full, on its one line.
        static const tr_id_t METERS[] = { TR_COMPOSE_METER_LATCHED,
                                          TR_COMPOSE_METER_LATCH,
                                          TR_COMPOSE_METER_HOLD };
        for (size_t m = 0; m < 3; m++) {
            lang_fill(buf, sizeof(buf), tr(METERS[m]), "########");
            CHECK(tr_px(buf) <= 252 && strstr(buf, "########"),
                  "%s meter '%s' is %dpx", code, buf, tr_px(buf));
        }
        // The connection's notes beside QUICK's VOICE and PHOTO rows: the
        // note column is what the widest label leaves of 42 columns.
        unsigned label_cols = 9u;
        for (int i = 0; i < (int)QUICK_ROW_COUNT; i++) {
            const unsigned c = lang_display_cols(quick_row_label((quick_row_t)i));
            if (c > label_cols) label_cols = c;
        }
        static const tr_id_t NOTES[] = { TR_QUICK_NOTE_NOT_HERE,
                                         TR_QUICK_NOTE_NO_STT,
                                         TR_QUICK_NOTE_NO_SAVE };
        for (size_t n = 0; n < 3; n++) {
            CHECK(lang_display_cols(tr(NOTES[n])) <= 42u - 2u - label_cols,
                  "%s QUICK note '%s' is cut", code, tr(NOTES[n]));
        }
        // DEFAULTS' chip holds SURE? whole.
        settings_set_camera_available(1);
        {
            int cx, cy, cw, ch;
            settings_ui_t sui;
            settings_ui_init(&sui);
            unsigned scroll = 0;
            while (!settings_option_rect(SETTINGS_ROW_DEFAULTS, 0, scroll,
                                         &cx, &cy, &cw, &ch) && scroll < 16) {
                scroll++;
            }
            CHECK((int)lang_display_cols(tr(TR_SETTINGS_RESET_SURE)) * 6 + 4 <= cw &&
                      cx + cw <= 256,
                  "%s SURE? '%s' does not fit its %dpx chip", code,
                  tr(TR_SETTINGS_RESET_SURE), cw);
        }
        // The Control legend, as subscreen.c assembles it, with the longest
        // button name on the second line.
        button_map_t m;
        button_map_defaults(&m);
        button_map_legend(&m, buf, sizeof(buf));
        CHECK(tr_px(buf) <= 252, "%s legend '%s' is %dpx", code, buf, tr_px(buf));
        CHECK(l != LANG_EN || strcmp(buf, "A enter  B delete  X esc  Y tab") == 0,
              "EN legend changed: '%s'", buf);
        snprintf(buf, sizeof(buf), "START %s  R+Y %s  SELECT %s", tr(TR_BUTTON_STOP),
                 tr(TR_LEGEND_MODE), tr(TR_LEGEND_SCREENS));
        CHECK(tr_px(buf) <= 252, "%s legend line 2 '%s' is %dpx", code, buf, tr_px(buf));
        CHECK(l != LANG_EN || strcmp(buf, "START stop  R+Y mode  SELECT screens") == 0,
              "EN legend line 2 changed: '%s'", buf);
    }
    lang_init();
}

static void test_layout_widths_every_language(void)
{
    lang_init();
    settings_set_camera_available(1);

    for (unsigned l = 0; l < LANG_BUILTIN_COUNT; l++) {
        lang_set_current(l);

        // 1. Settings tab chips: never overlap and never pass 256 px.
        int prev_tab_right = 0;
        for (unsigned t = 0; t < SETTINGS_TAB_COUNT; t++) {
            int tx = 0, ty = 0, tw = 0, th = 0;
            const char *tlabel = settings_tab_label(t);
            const int text_px = (int)lang_display_cols(tlabel) * 6;
            CHECK(settings_tab_rect(t, &tx, &ty, &tw, &th),
                  "lang %u tab %u has no rect", l, t);
            CHECK(tx > prev_tab_right,
                  "lang %u tab %u overlaps previous tab (%d <= %d)",
                  l, t, tx, prev_tab_right);
            CHECK(tw >= text_px + 4,
                  "lang %u tab %u chip (%dpx) narrower than '%s' (%dpx)",
                  l, t, tw, tlabel, text_px);
            CHECK(tx + tw <= 256,
                  "lang %u tab %u ends past 256px (%d)", l, t, tx + tw);
            prev_tab_right = tx + tw;
        }

        // 2. Settings rows & chips: no label's pixel width passes its first
        // chip's x, chips do not overlap, and no row's chips pass 256 px.
        for (int r = 0; r < SETTINGS_ROW_COUNT; r++) {
            const settings_row_t row = (settings_row_t)r;
            const char *label = settings_row_label(row);
            const int label_end = SETTINGS_LABEL_X +
                                  (int)lang_display_cols(label) * 6;
            int prev_right = label_end;
            const unsigned count = settings_option_count(row);
            for (unsigned o = 0; o < count; o++) {
                int cx = 0, cy = 0, cw = 0, ch = 0;
                CHECK(settings_option_rect(row, o, 0, &cx, &cy, &cw, &ch),
                      "lang %u row %d option %u has no rect", l, r, o);
                CHECK(cx > prev_right,
                      "lang %u row %d ('%s') option %u at x=%d overlaps %d",
                      l, r, label, o, cx, prev_right);
                CHECK(cx + cw <= 256,
                      "lang %u row %d option %u ends past 256px (%d)",
                      l, r, o, cx + cw);
                prev_right = cx + cw;
            }
        }

        // 3. QUICK rows: formatted with lang_pad_cols, never pass 256 px (42 cols).
        unsigned label_cols = 9u;
        for (int i = 0; i < (int)QUICK_ROW_COUNT; i++) {
            const unsigned c = lang_display_cols(quick_row_label((quick_row_t)i));
            if (c > label_cols) label_cols = c;
        }
        CHECK(label_cols <= 12u, "lang %u QUICK label column too wide (%u)",
              l, label_cols);
        for (int i = 0; i < (int)QUICK_ROW_COUNT; i++) {
            const quick_row_t r = (quick_row_t)i;
            // Room for every part at its largest (48 + 16 + 96 + separators):
            // gcc's -Wformat-truncation proves the sum, not the typical row.
            char lbl[48], note[96], row[176];
            (void)lang_pad_cols(quick_row_label(r), label_cols, label_cols,
                                lbl, sizeof(lbl));
            if (r == QUICK_ROW_MODE) {
                char mode_col[16];
                (void)lang_pad_cols("normal", 8u, 8u, mode_col, sizeof(mode_col));
                const unsigned rem = 42u - 1u - label_cols - 1u - 8u - 1u;
                (void)lang_pad_cols(quick_row_note(r), 0u, rem, note, sizeof(note));
                snprintf(row, sizeof(row), " %s %s %s", lbl, mode_col, note);
            } else if (r == QUICK_ROW_COMMANDS || r == QUICK_ROW_AGAIN) {
                const unsigned note_w = 42u - 1u - label_cols - 1u - 1u - 2u;
                (void)lang_pad_cols(quick_row_note(r), note_w, note_w,
                                    note, sizeof(note));
                snprintf(row, sizeof(row), " %s %s %2u", lbl, note, 12u);
            } else {
                const unsigned rem = 42u - 1u - label_cols - 1u;
                (void)lang_pad_cols(quick_row_note(r), 0u, rem, note, sizeof(note));
                snprintf(row, sizeof(row), " %s %s", lbl, note);
            }
            const int px = (int)lang_display_cols(row) * 6;
            CHECK(px <= 256, "lang %u QUICK row %d passes 256px (%dpx: '%s')",
                  l, i, px, row);
        }

        // 4. Every translated string (composer bar, headlines, tiles, hints):
        // fits its container (VOICE_BAR_W = 248px for mic bar, 76px for tile
        // labels, 252px / 42 cols for full lines).
        for (unsigned id = 0; id < TR_COUNT; id++) {
            const char *s = lang_tr_at(l, (tr_id_t)id);
            const int px = (int)lang_display_cols(s) * 6;
            // The guide's pop-up text is wrapped to two lines of its box;
            // test_guide_text_fits measures those against the box instead.
            if (id == TR_GUIDE_INTRO ||
                (id >= TR_GUIDE_DO_ENTER && id <= TR_GUIDE_DO_CLOSE_MENU)) {
                continue;
            }
            CHECK(px <= 252,
                  "lang %u key %s is %dpx (> 252px): '%s'",
                  l, lang_key_name((tr_id_t)id), px, s);
            if (id == TR_COMPOSE_MIC_IDLE || id == TR_COMPOSE_MIC_REC ||
                id == TR_COMPOSE_MIC_LATCH || id == TR_COMPOSE_MIC_WAIT) {
                CHECK(px <= VOICE_BAR_W,
                      "lang %u mic bar key %s is %dpx (> %dpx): '%s'",
                      l, lang_key_name((tr_id_t)id), px, VOICE_BAR_W, s);
            }
        }
        // And the approval headline with both buttons appended fits 252 px.
        chime_set_answer_buttons("START", "X");
        chime_claude_state(3);
        const int hpx = (int)lang_display_cols(chime_headline()) * 6;
        CHECK(hpx <= 252,
              "lang %u approval headline is %dpx (> 252px): '%s'",
              l, hpx, chime_headline());
    }

    settings_set_camera_available(0);
    chime_set_answer_buttons("A", "X");
    chime_init();
    lang_init();
    reset_sound();
}

static void test_voice_adpcm_and_wire(void)
{
    voice_upload_t u;
    voice_upload_init(&u);
    static uint8_t buf[VOICE_BYTES_MAX];
    int16_t pcm[1000];
    for (int i = 0; i < 1000; i++) pcm[i] = (int16_t)((i * 300) % 12000 - 6000);

    // 1000 samples -> 500 ADPCM bytes (> VOICE_CHUNK_MAX = 480).
    CHECK(voice_upload_append(&u, buf, sizeof(buf), pcm, 1000) == 500 &&
              u.total == 500 && u.peak_bars > 0,
          "voice_upload_append did not produce 500 ADPCM bytes");

    uint32_t offset = 999;
    const uint8_t *data = NULL;
    uint16_t len = 0;
    // While recording=1, only full 480-byte chunks leave; the 20-byte tail waits.
    CHECK(voice_upload_next_chunk(&u, buf, 1, &offset, &data, &len) &&
              offset == 0 && len == VOICE_CHUNK_MAX,
          "first voice chunk was not 480 bytes at offset 0");
    CHECK(!voice_upload_next_chunk(&u, buf, 1, &offset, &data, &len),
          "partial tail was sent while recording=1");
    // Once recording=0, the 20-byte tail drains.
    CHECK(voice_upload_next_chunk(&u, buf, 0, &offset, &data, &len) &&
              offset == 480 && len == 20 && u.sent == 500,
          "final 20-byte tail did not drain on stop");
    CHECK(voice_upload_ack(&u, 480) && voice_upload_ack(&u, 500) &&
              !voice_upload_ack(&u, 400) && !voice_upload_ack(&u, 501),
          "voice_upload_ack did not enforce [acked, sent]");

    // Wire codecs.
    int can_stt = 0;
    const uint8_t mr_ok[1] = { PROTO_MIC_CAN_TRANSCRIBE };
    const uint8_t mr_bad[1] = { 0x02 };
    CHECK(proto_decode_mic_ready(&can_stt, mr_ok, 1) == PROTO_OK && can_stt == 1 &&
              proto_decode_mic_ready(&can_stt, mr_bad, 1) != PROTO_OK,
          "proto_decode_mic_ready failed");

    voice_text_t vt;
    const uint8_t vt_ok[] = { VOICE_STATUS_OK, 0, 5, 'h', 'e', 'l', 'l', 'o' };
    const uint8_t vt_bad[] = { VOICE_STATUS_OK, 0, 5, 'h', 0x01, 'l', 'l', 'o' };
    CHECK(proto_decode_voice_text(&vt, vt_ok, sizeof(vt_ok)) == PROTO_OK &&
              vt.status == VOICE_STATUS_OK && strcmp(vt.text, "hello") == 0,
          "proto_decode_voice_text rejected valid text");
    CHECK(proto_decode_voice_text(&vt, vt_bad, sizeof(vt_bad)) != PROTO_OK,
          "proto_decode_voice_text accepted control byte");
}

static void test_voice_late_replies_are_tagged(void)
{
    voice_owed_t o;
    voice_owed_init(&o);

    // Nothing sent: an ack or a text answers nothing, and is no reason to
    // drop anything.
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_UNEXPECTED &&
              voice_owed_text(&o) == VOICE_REPLY_UNEXPECTED,
          "a reply to nothing was taken as live");

    // Utterance 1: three chunks, one acked, then X.
    voice_owed_chunk(&o);
    voice_owed_chunk(&o);
    voice_owed_chunk(&o);
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_LIVE, "utterance 1's first ack");
    voice_owed_abandon(&o);
    CHECK(voice_owed_can_start(&o), "a cancel with no END blocked the next one");

    // Utterance 2 starts and sends a chunk before the old acks arrive: the
    // first two acks on the stream are utterance 1's and must not credit 2.
    voice_owed_chunk(&o);
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_STALE &&
              voice_owed_ack(&o) == VOICE_REPLY_STALE,
          "a cancelled utterance's ack was credited to the next one");
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_LIVE, "utterance 2's own ack");
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_UNEXPECTED, "an extra ack was live");

    // Utterance 2 ends and times out waiting: its text is owed, stale, and
    // nothing new starts until it has arrived (a BUSY for a newer one could
    // otherwise overtake it).
    voice_owed_end(&o);
    voice_owed_abandon(&o);
    CHECK(!voice_owed_can_start(&o), "a new utterance raced an owed transcription");
    CHECK(voice_owed_text(&o) == VOICE_REPLY_STALE, "the late text was live");
    CHECK(voice_owed_can_start(&o), "the paid-off text still blocks");

    // Utterance 3 runs to the end: its text is the live one, exactly once.
    voice_owed_chunk(&o);
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_LIVE, "utterance 3's ack");
    voice_owed_end(&o);
    CHECK(voice_owed_text(&o) == VOICE_REPLY_LIVE, "utterance 3's text");
    CHECK(voice_owed_text(&o) == VOICE_REPLY_UNEXPECTED, "a second text was live");

    // Abandoning with nothing in flight changes nothing.
    voice_owed_abandon(&o);
    CHECK(o.acks_stale == 0 && o.texts_stale == 0 && voice_owed_can_start(&o),
          "an idle cancel booked a debt");

    // Dictation lands after what is written, one space apart, and only the
    // bytes the keyboard could have typed.
    composer_t c;
    composer_init(&c);
    CHECK(composer_append_text(&c, "hi", 2) == 0, "dictated into a closed composer");
    composer_open(&c);
    (void)composer_load(&c, "fix");
    CHECK(composer_append_text(&c, "the bug", 7) == 7 &&
              strcmp(c.body, "fix the bug") == 0,
          "dictation was not joined with one space: '%s'", c.body);
    const char hostile[] = { 'o', 'k', (char)0x9B, '2', 'J' };
    CHECK(composer_append_text(&c, hostile, sizeof(hostile)) == 4 &&
              memchr(c.body, 0x9B, c.len) == NULL,
          "dictation put a C1 byte in the composer");
}

// Release 1.2's fixes, each against the pure half that decides it.
static void test_voice_language_is_not_the_ui_language(void)
{
    // The server folds transcripts to ASCII: a CJK hint came back empty.
    lang_init();
    CHECK(!strcmp(lang_voice_code(LANG_EN), "en") &&
              !strcmp(lang_voice_code(LANG_ES), "es"),
          "a Latin-script language lost its own voice code");
    CHECK(!strcmp(lang_voice_code(LANG_ZH), "en") &&
              !strcmp(lang_voice_code(LANG_JA), "en"),
          "ZH or JA asked the server for speech it cannot return");
    char err[64];
    const int fr = lang_load_text("code = fr\ncontrol.keys = CLAVIER\n", NULL,
                                  err, sizeof(err));
    const int tw = lang_load_text("code = zh-TW\ncontrol.keys = KEYS\n", NULL,
                                  err, sizeof(err));
    CHECK(fr >= 0 && tw >= 0, "packs did not load: %s", err);
    CHECK(fr >= 0 && !strcmp(lang_voice_code((unsigned)fr), "fr"),
          "a Latin custom pack lost its code");
    CHECK(tw >= 0 && !strcmp(lang_voice_code((unsigned)tw), "en"),
          "a zh-TW pack asked for Chinese speech");
    lang_init();
    const int ko = lang_load_text("code = ko\ncontrol.keys = KEYS\n", NULL, err,
                                  sizeof(err));
    const int zhx = lang_load_text("code = zhx\ncontrol.keys = KEYS\n", NULL, err,
                                   sizeof(err));
    CHECK(ko >= 0 && !strcmp(lang_voice_code((unsigned)ko), "en"),
          "a Korean pack asked for Korean speech");
    CHECK(zhx >= 0 && !strcmp(lang_voice_code((unsigned)zhx), "zhx"),
          "a code merely starting zh was taken for Chinese");
    CHECK(!strcmp(lang_voice_code(99), "en"), "an index past the end");
    lang_init();
}

static void test_a_malformed_voice_reply_still_pays(void)
{
    // mic.c pays the ledger for a reply it could not decode exactly as for
    // one it could: one VOICE_TEXT per VOICE_END, one ACK per CHUNK.
    voice_owed_t o;
    voice_owed_init(&o);
    voice_owed_chunk(&o);
    voice_owed_end(&o);
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_LIVE, "the live chunk's ack");
    // The text arrives malformed: mic_text_malformed() books it all the same.
    CHECK(voice_owed_text(&o) == VOICE_REPLY_LIVE && voice_owed_can_start(&o),
          "a damaged transcription left the next recording refused");
    // An abandoned utterance whose late text is malformed: still paid.
    voice_owed_chunk(&o);
    voice_owed_end(&o);
    voice_owed_abandon(&o);
    CHECK(!voice_owed_can_start(&o), "a stale text owed did not block");
    CHECK(voice_owed_ack(&o) == VOICE_REPLY_STALE, "stale ack");
    CHECK(voice_owed_text(&o) == VOICE_REPLY_STALE && voice_owed_can_start(&o),
          "a damaged stale text did not unblock the microphone");
}

static void test_settings_values_fall_back_alone(void)
{
    // A file with an unknown value on most keys at once: each of those keys
    // is its default, and every readable line beside them survives.
    lang_init();
    settings_t s;
    char err[64];
    const char file[] =
        "version=4\n"
        "model=gpt\nboot=keys\nsound=loud\nvolume=high\nblips=lots\n"
        "typing=off\ntheme=purple\nlang=fr\nautoconnect=maybe\n"
        "addresses=blur\nviewtaps=on\ngod_mode=perhaps\nbutton_a=jump\n"
        "button_b=tab\nshutter=x\npicture=huge\nliveview=sepia\nmic_mode=latch\n";
    CHECK(settings_parse(file, &s, err, sizeof(err)), "refused: %s", err);
    settings_t d;
    settings_defaults(&d);
    CHECK(s.model == d.model && s.sound == d.sound && s.blips == d.blips &&
              s.theme == d.theme && s.language == LANG_EN &&
              s.autoconnect == d.autoconnect &&
              s.hide_addresses == d.hide_addresses && s.god_mode == 0 &&
              s.buttons.action[BUTTON_A] == d.buttons.action[BUTTON_A] &&
              s.camera_shutter == d.camera_shutter && s.picture == d.picture &&
              s.liveview == d.liveview,
          "an unknown value did not fall back to its own default");
    CHECK(s.boot == SETTINGS_BOOT_KEYS && s.volume == SETTINGS_VOLUME_HIGH &&
              s.typing == 0 && s.view_taps == 1 &&
              s.buttons.action[BUTTON_B] == BUTTON_TAB &&
              s.mic_mode == SETTINGS_MIC_LATCH,
          "an unknown value elsewhere cost a good line");

    // Saved with a custom pack chosen, read back after the pack is gone.
    const int fr = lang_load_text("code = fr\ncontrol.keys = CLAVIER\n", NULL,
                                  err, sizeof(err));
    CHECK(fr >= 0, "pack: %s", err);
    settings_defaults(&s);
    s.language = (unsigned)fr;
    s.theme = SETTINGS_THEME_MOSS;
    s.buttons.action[BUTTON_Y] = BUTTON_STOP;
    static const char path[] = "/tmp/dsterm-removed-pack.cfg";
    CHECK(settings_save(path, &s, err, sizeof(err)), "save: %s", err);
    lang_init();    // the SD card no longer has fr.lang
    settings_t back;
    CHECK(settings_load(path, &back, err, sizeof(err)) &&
              back.language == LANG_EN && back.theme == SETTINGS_THEME_MOSS &&
              back.buttons.action[BUTTON_Y] == BUTTON_STOP,
          "a removed pack's lang= cost more than the language: %s", err);
    remove(path);
}

static void test_custom_pack_cut_on_a_character(void)
{
    // 21 three-byte characters is 63 bytes, LANG_STR_MAX exactly; one more
    // ASCII byte before them leaves the 21st cut in half.
    lang_init();
    static char text[256];
    char val[80];
    size_t k = 0;
    val[k++] = 'x';
    for (int i = 0; i < 21; i++) {
        memcpy(val + k, "\xe9\x80\x81", 3);     // 送
        k += 3;
    }
    val[k] = '\0';
    snprintf(text, sizeof(text), "code = qq\ncontrol.keys = %s\n", val);
    char err[64];
    const int qq = lang_load_text(text, NULL, err, sizeof(err));
    CHECK(qq >= 0, "pack: %s", err);
    const char *got = lang_tr_at((unsigned)qq, TR_CONTROL_KEYS);
    CHECK(strlen(got) == 61 && !strncmp(got, val, 61),
          "a long value was cut inside a character (%zu bytes)", strlen(got));
    // A value that fits keeps every byte.
    snprintf(text, sizeof(text), "code = qr\ncontrol.keys = %s\n", val + 1);
    const int qr = lang_load_text(text, NULL, err, sizeof(err));
    CHECK(qr >= 0 && strlen(lang_tr_at((unsigned)qr, TR_CONTROL_KEYS)) == 63,
          "a value of exactly LANG_STR_MAX bytes was cut");
    lang_init();
}

static void test_voice_bar_edges_and_guide_context(void)
{
    CHECK(voice_bar_hit(VOICE_BAR_X, VOICE_BAR_Y) &&
              voice_bar_hit(VOICE_BAR_X + VOICE_BAR_W - 1,
                            VOICE_BAR_Y + VOICE_BAR_H - 1),
          "the MIC bar's corners are not the bar");
    CHECK(!voice_bar_hit(VOICE_BAR_X - 1, VOICE_BAR_Y) &&
              !voice_bar_hit(VOICE_BAR_X + VOICE_BAR_W, VOICE_BAR_Y) &&
              !voice_bar_hit(VOICE_BAR_X, VOICE_BAR_Y - 1) &&
              !voice_bar_hit(VOICE_BAR_X, VOICE_BAR_Y + VOICE_BAR_H),
          "a pixel just outside the MIC bar hit it");
    CHECK(guide_ctx_for(0, 0, 0) == GUIDE_CTX_CONTROL &&
              guide_ctx_for(1, 0, 0) == GUIDE_CTX_KEYS &&
              guide_ctx_for(0, 1, 0) == GUIDE_CTX_CONTROL &&
              guide_ctx_for(1, 1, 0) == GUIDE_CTX_KEYS,
          "the guide opened on the wrong screen's tab");
    CHECK(guide_ctx_for(0, 0, 1) == GUIDE_CTX_QUICK &&
              guide_ctx_for(1, 1, 1) == GUIDE_CTX_QUICK,
          "QUICK open did not open the guide on QUICK");

    // The pop-up names a mapped button's action as a whole word; the drawing
    // keeps English's "ENT" and shows a translation's own word.
    lang_init();
    button_map_t map;
    button_map_defaults(&map);
    guide_config_t cfg = { &map, SHUTTER_R, 1, 1, 1 };
    const guide_entry_t e = guide_entry(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A, 0);
    char name[GUIDE_LINE_MAX];
    guide_entry_texts(&e, NULL, 0, name, sizeof(name), NULL, 0);
    CHECK(!strcmp(name, "ENTER"), "EN pop-up name '%s'", name);
    lang_set_current(LANG_ES);
    guide_entry_texts(&e, NULL, 0, name, sizeof(name), NULL, 0);
    CHECK(!strcmp(name, "INTRO") &&
              !strcmp(guide_button_label(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A),
                      "INTRO"),
          "ES pop-up name '%s'", name);
    lang_set_current(LANG_JA);
    CHECK(!strcmp(guide_button_label(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A),
                  "決定"),
          "JA drawing kept the English abbreviation");
    lang_init();
}

static void test_quick_files_reject_what_they_cannot_read(void)
{
    static quick_saved_t s;
    // Unknown and dangling escapes skip their line, and only theirs.
    quick_saved_parse(&s, "one\nbad \\t escape\ntwo\\\\ok\nend\\");
    CHECK(s.count == 2 && !strcmp(s.items[0], "one") &&
              !strcmp(s.items[1], "two\\ok"),
          "bad escapes: %u items", s.count);
    // CRLF reads like LF; "\\n" is a newline inside an entry.
    quick_saved_parse(&s, "first\r\nsecond\\nline\r\n");
    CHECK(s.count == 2 && !strcmp(s.items[0], "first") &&
              !strcmp(s.items[1], "second\nline"),
          "CRLF: %u items, '%s'", s.count, s.items[0]);
    // A line longer than an entry may be is skipped whole, not cut.
    static char big[QUICK_SAVED_TEXT_MAX + 40];
    memset(big, 'a', QUICK_SAVED_TEXT_MAX + 1);
    memcpy(big + QUICK_SAVED_TEXT_MAX + 1, "\nshort\n", 8);
    quick_saved_parse(&s, big);
    CHECK(s.count == 1 && !strcmp(s.items[0], "short"),
          "an over-long saved line: %u items", s.count);
    // Exactly the limit is kept.
    big[QUICK_SAVED_TEXT_MAX] = '\n';
    big[QUICK_SAVED_TEXT_MAX + 1] = '\0';
    quick_saved_parse(&s, big);
    CHECK(s.count == 1 && strlen(s.items[0]) == QUICK_SAVED_TEXT_MAX,
          "a saved line of exactly the limit was refused");
    // And format writes what parse reads, escapes included.
    static char out[QUICK_SAVED_MAX * (2 * QUICK_SAVED_TEXT_MAX + 2) + 1];
    quick_saved_init(&s);
    (void)quick_saved_add(&s, "a\\b\nc");
    CHECK(quick_saved_format(&s, out, sizeof(out)) && !strcmp(out, "a\\\\b\\nc\n"),
          "format: '%s'", out);
    CHECK(quick_saved_format(&s, out, 8) == 0, "a format that did not fit said it did");

    static quick_recent_t r;
    memset(big, 'b', QUICK_RECENT_TEXT_MAX + 1);
    memcpy(big + QUICK_RECENT_TEXT_MAX + 1, "\r\nkept\r\n", 9);
    quick_recent_parse(&r, big);
    CHECK(r.count == 1 && !strcmp(r.items[0], "kept"),
          "an over-long AGAIN line: %u items", r.count);
}

static void test_an_empty_enter_is_not_a_send(void)
{
    // Pressing A (Enter) on nothing must not sound SENT or start the thinking
    // arc: that played whole "requests" that never were.
    chime_init();
    CHECK(!chime_typed("\r", 1) && !chime_typed("\r", 1),
          "an Enter on an empty line counted as a send");
    CHECK(!chime_typed("fix it", 6) && chime_typed("\r", 1),
          "a typed line's Enter was not a send");
    CHECK(!chime_typed("\r", 1), "a second Enter after a send counted again");
    CHECK(!chime_typed("a", 1) && !chime_typed("\x7f", 1) && !chime_typed("\r", 1),
          "a line typed and deleted again counted as a send");
    CHECK(!chime_typed("\x1b[C", 3) && !chime_typed("\x1b", 1) &&
              !chime_typed("\x1b[D", 3) && !chime_typed("\r", 1),
          "Left, Right or Escape counted as typed text");
    // Up and Down recall history into the line: Enter after one sends it.
    CHECK(!chime_typed("\x1b[A", 3) && chime_typed("\r", 1),
          "Enter after Up-arrow recall was not a send");
    CHECK(!chime_typed("\x1bOB", 3) && chime_typed("\r", 1),
          "Enter after an application-mode Down was not a send");
    // LF is a newline inside Claude Code's prompt, not a submit: a composer
    // body ending in one (sent as its own frame, or with the text) still
    // cues SENT on the CR that follows.
    CHECK(!chime_typed("two\nlines\n", 10) && chime_typed("\r", 1),
          "a body ending in LF was not a send");
    CHECK(!chime_typed("x", 1) && !chime_typed("\n", 1) && chime_typed("\r", 1),
          "an LF frame cleared the line");
    CHECK(!chime_typed("\n", 1) && !chime_typed("\r", 1),
          "a body of nothing but LF counted as a send");
    CHECK(!chime_typed("half", 4) && !chime_typed("\x03", 1) && !chime_typed("\r", 1),
          "Ctrl-C did not clear the line");
    // Answering a question Claude asked is a send even with nothing typed.
    chime_claude_state(3);
    CHECK(chime_typed("\r", 1), "Enter on an approval was not a send");
    chime_claude_state(1);
    // A new connection starts with nothing typed.
    (void)chime_typed("x", 1);
    chime_claude_reset();
    CHECK(!chime_typed("\r", 1), "typed text survived a reconnect");
    chime_init();
}

static void test_voice_ptt_focus_order(void)
{
    // Which shoulder came first decides what L+R means in the composer, so
    // focus and the microphone never collide.
    for (int latch = 0; latch <= 1; latch++) {
        voice_ptt_t p;
        voice_ptt_init(&p);
        // L first, held a while, then R: focus, in either mic mode.
        CHECK(voice_ptt_step(&p, latch, 0, 1, 0, 1, 0, 0, 0, 0) == VOICE_PTT_NONE &&
                  voice_ptt_step(&p, latch, 0, 1, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
              "L alone did something (latch=%d)", latch);
        CHECK(voice_ptt_step(&p, latch, 1, 1, 1, 0, 0, 0, 0, 0) == VOICE_PTT_FOCUS &&
                  !p.chord_notice,
              "L then R was not focus (latch=%d)", latch);
        // Holding both on, then L let go with R still down: never the mic.
        for (unsigned i = 0; i < 2 * VOICE_HOLD_FRAMES; i++) {
            CHECK(voice_ptt_step(&p, latch, 1, i < 3, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
                  "the focus chord's R started the mic at frame %u (latch=%d)", i, latch);
        }
        CHECK(voice_ptt_step(&p, latch, 0, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE &&
                  !p.wait_release,
              "releasing after the focus chord did not settle (latch=%d)", latch);

        // Both landing in one frame: focus too -- the mic needs R alone.
        CHECK(voice_ptt_step(&p, latch, 1, 1, 1, 1, 0, 0, 0, 0) == VOICE_PTT_FOCUS,
              "a simultaneous L+R was not focus (latch=%d)", latch);
        (void)voice_ptt_step(&p, latch, 0, 0, 0, 0, 0, 0, 0, 0);

        // R first (not yet recording), then L: not focus -- the notice says
        // which order is.
        CHECK(voice_ptt_step(&p, latch, 1, 0, 1, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
              "R's first frame did something (latch=%d)", latch);
        CHECK(voice_ptt_step(&p, latch, 1, 1, 0, 1, 0, 0, 0, 0) == VOICE_PTT_NONE &&
                  p.chord_notice,
              "R then L without a recording was not the notice (latch=%d)", latch);
        (void)voice_ptt_step(&p, latch, 0, 0, 0, 0, 0, 0, 0, 0);
    }
}

// ------------------------------------------------------------ CONTROLS ---

// Feed guide_step() a run of held key words, one per frame, as poll_input
// does: the edge is what was not held the frame before.
static guide_step_t guide_feed(guide_t *g, const guide_config_t *cfg,
                               const unsigned *held, unsigned n)
{
    unsigned prev = 0;
    guide_step_t last = GUIDE_STEP_NONE;
    for (unsigned i = 0; i < n; i++) {
        const guide_step_t s = guide_step(g, cfg, held[i], held[i] & ~prev);
        if (s != GUIDE_STEP_NONE) last = s;
        prev = held[i];
    }
    return last;
}

static void test_guide_every_combo_in_every_context(void)
{
    lang_init();
    button_map_t map;
    button_map_defaults(&map);
    guide_config_t cfg = { &map, SHUTTER_R, 0, 1, 1 };
    for (int c = 0; c < GUIDE_CTX_COUNT; c++) {
        for (int k = GUIDE_COMBO_A; k < GUIDE_COMBO_COUNT; k++) {
            for (int rec = 0; rec < 2; rec++) {
                const guide_entry_t e = guide_entry((guide_ctx_t)c, &cfg,
                                                    (guide_combo_t)k, rec);
                CHECK(e.what < TR_COUNT, "ctx %d combo %d has no text", c, k);
                CHECK(e.name < TR_COUNT || e.action < BUTTON_ACTION_COUNT,
                      "ctx %d combo %d has no name", c, k);
                // A mapped name only where the mapping applies.
                CHECK(e.name != TR_COUNT ||
                          c == GUIDE_CTX_CONTROL || c == GUIDE_CTX_KEYS,
                      "ctx %d combo %d reads the face map outside CONTROL/KEYS", c, k);
                char combo[GUIDE_LINE_MAX], name[GUIDE_LINE_MAX], what[GUIDE_LINE_MAX];
                guide_entry_texts(&e, combo, sizeof(combo), name, sizeof(name),
                                  what, sizeof(what));
                CHECK(combo[0] && name[0] && what[0],
                      "ctx %d combo %d has an empty line", c, k);
            }
        }
    }

    // Never an action for a context it does not apply to.  The composer's
    // shoulder combos are the composer's; the shutter is the camera's;
    // Shift-Tab is the Control and KEYS screens'.
    static const guide_combo_t COMPOSER_ONLY[] = {
        GUIDE_COMBO_HOLD_R, GUIDE_COMBO_R_THEN_L, GUIDE_COMBO_L_THEN_R,
    };
    static const guide_ctx_t NOT_COMPOSER[] = {
        GUIDE_CTX_CONTROL, GUIDE_CTX_KEYS, GUIDE_CTX_CAMERA, GUIDE_CTX_QUICK,
    };
    for (size_t i = 0; i < sizeof(NOT_COMPOSER) / sizeof(NOT_COMPOSER[0]); i++) {
        for (size_t j = 0; j < sizeof(COMPOSER_ONLY) / sizeof(COMPOSER_ONLY[0]); j++) {
            const guide_entry_t e = guide_entry(NOT_COMPOSER[i], &cfg, COMPOSER_ONLY[j], 1);
            CHECK(e.what == TR_GUIDE_DO_NOTHING,
                  "composer combo %d acts in ctx %d", COMPOSER_ONLY[j], NOT_COMPOSER[i]);
        }
    }
    static const guide_ctx_t NOT_CAMERA[] = {
        GUIDE_CTX_CONTROL, GUIDE_CTX_KEYS, GUIDE_CTX_COMPOSE, GUIDE_CTX_FOCUS,
        GUIDE_CTX_QUICK,
    };
    for (size_t i = 0; i < sizeof(NOT_CAMERA) / sizeof(NOT_CAMERA[0]); i++) {
        CHECK(guide_entry(NOT_CAMERA[i], &cfg, GUIDE_COMBO_SHUTTER, 0).what ==
                  TR_GUIDE_DO_NOTHING,
              "the shutter acts in ctx %d", NOT_CAMERA[i]);
    }
    for (int c = GUIDE_CTX_COMPOSE; c < GUIDE_CTX_COUNT; c++) {
        CHECK(guide_entry((guide_ctx_t)c, &cfg, GUIDE_COMBO_SHIFT_TAB, 0).what ==
                  TR_GUIDE_DO_NOTHING,
              "Shift-Tab acts in ctx %d", c);
    }
    // No camera: no CAMERA context, and nothing in it if asked anyway.
    cfg.camera = 0;
    CHECK(!guide_ctx_present(GUIDE_CTX_CAMERA, &cfg) &&
              guide_ctx_step(GUIDE_CTX_FOCUS, 1, &cfg) == GUIDE_CTX_QUICK &&
              guide_ctx_step(GUIDE_CTX_QUICK, -1, &cfg) == GUIDE_CTX_FOCUS,
          "a card without a camera still steps onto CAMERA");
    for (int k = GUIDE_COMBO_A; k < GUIDE_COMBO_INTRO; k++) {
        CHECK(guide_entry(GUIDE_CTX_CAMERA, &cfg, (guide_combo_t)k, 0).what ==
                  TR_GUIDE_DO_NOTHING,
              "CAMERA without a camera explains combo %d", k);
    }
    int x, y, w, h;
    CHECK(!guide_tab_rect(GUIDE_CTX_CAMERA, &cfg, &x, &y, &w, &h),
          "a card without a camera has a CAMERA tab");
    guide_t g;
    guide_init(&g, GUIDE_CTX_CAMERA, &cfg);
    CHECK(g.ctx == GUIDE_CTX_CONTROL, "the guide opened on an absent CAMERA");
    cfg.camera = 1;

    // The real keys, context by context.
    CHECK(guide_entry(GUIDE_CTX_KEYS, &cfg, GUIDE_COMBO_X, 0).what == TR_GUIDE_DO_X_BACK,
          "KEYS' X is not back (status_mode_back swallows it)");
    CHECK(guide_entry(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_X, 0).what == TR_GUIDE_DO_ESC,
          "Control's X does not send its mapped Esc");
    CHECK(guide_entry(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_START, 0).what == TR_GUIDE_DO_STOP,
          "START is not stop, as mapped");
    CHECK(guide_entry(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_SELECT, 0).what == TR_GUIDE_DO_SELECT &&
              guide_entry(GUIDE_CTX_QUICK, &cfg, GUIDE_COMBO_SELECT, 0).what == TR_GUIDE_DO_NOTHING,
          "SELECT cycles where it does, and only there");
    CHECK(guide_entry(GUIDE_CTX_COMPOSE, &cfg, GUIDE_COMBO_A, 0).what == TR_GUIDE_DO_SEND &&
              guide_entry(GUIDE_CTX_COMPOSE, &cfg, GUIDE_COMBO_Y, 0).what == TR_GUIDE_DO_SNIPPETS &&
              guide_entry(GUIDE_CTX_COMPOSE, &cfg, GUIDE_COMBO_X, 0).what == TR_GUIDE_DO_CLOSE &&
              guide_entry(GUIDE_CTX_COMPOSE, &cfg, GUIDE_COMBO_X, 1).what == TR_GUIDE_DO_MIC_CANCEL,
          "the composer's A, Y and X are wrong");
    CHECK(guide_entry(GUIDE_CTX_FOCUS, &cfg, GUIDE_COMBO_A, 0).what == TR_GUIDE_DO_TYPE &&
              guide_entry(GUIDE_CTX_FOCUS, &cfg, GUIDE_COMBO_START, 0).what == TR_GUIDE_DO_FOCUS_SEND &&
              guide_entry(GUIDE_CTX_FOCUS, &cfg, GUIDE_COMBO_X, 0).what == TR_GUIDE_DO_FOCUS_OUT &&
              guide_entry(GUIDE_CTX_FOCUS, &cfg, GUIDE_COMBO_DPAD, 0).what == TR_GUIDE_DO_MOVE_KEYS,
          "FOCUS is not A types, START sends, X leaves, the d-pad moves");
    CHECK(guide_entry(GUIDE_CTX_CAMERA, &cfg, GUIDE_COMBO_LR, 0).what == TR_GUIDE_DO_CAM_LEAVE &&
              guide_entry(GUIDE_CTX_CAMERA, &cfg, GUIDE_COMBO_R, 0).combo == GUIDE_COMBO_SHUTTER,
          "the camera's L+R and shutter are wrong");
    lang_init();
}

static void test_guide_mapping_and_order(void)
{
    lang_init();
    button_map_t map;
    button_map_defaults(&map);
    guide_config_t cfg = { &map, SHUTTER_R, 0, 1, 1 };
    guide_t g;

    // A remapped A changes the A entry, and the drawing's label, at once.
    CHECK(!strcmp(guide_button_label(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A), "ENT"),
          "A's label is not ENT by default");
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    static const unsigned press_a[] = { GUIDE_KEY_A };
    CHECK(guide_feed(&g, &cfg, press_a, 1) == GUIDE_STEP_SHOW &&
              g.entry.action == BUTTON_ENTER && g.entry.what == TR_GUIDE_DO_ENTER,
          "A does not show Enter");
    // Enter has to stay on some button: B takes it first.
    CHECK(button_map_set(&map, BUTTON_B, BUTTON_ENTER) &&
              button_map_set(&map, BUTTON_A, BUTTON_SPACE), "could not remap A");
    CHECK(!strcmp(guide_button_label(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A), "SPC"),
          "a remapped A still says '%s'",
          guide_button_label(GUIDE_CTX_CONTROL, &cfg, GUIDE_COMBO_A));
    guide_init(&g, GUIDE_CTX_KEYS, &cfg);
    (void)guide_feed(&g, &cfg, press_a, 1);
    CHECK(g.entry.action == BUTTON_SPACE && g.entry.what == TR_GUIDE_DO_SPACE,
          "a remapped A does not show Space");
    // ...but not in the composer, which keeps its own keys.
    CHECK(guide_entry(GUIDE_CTX_COMPOSE, &cfg, GUIDE_COMBO_A, 0).what == TR_GUIDE_DO_SEND,
          "the mapping reached the composer");
    button_map_defaults(&map);

    // R, then the Tab button: Shift-Tab, named after whichever button has Tab.
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    static const unsigned r_then_y[] = { GUIDE_KEY_R, 0, GUIDE_KEY_Y };
    (void)guide_feed(&g, &cfg, r_then_y, 3);
    char combo[GUIDE_LINE_MAX], name[GUIDE_LINE_MAX], what[GUIDE_LINE_MAX];
    guide_entry_texts(&g.entry, combo, sizeof(combo), name, sizeof(name),
                      what, sizeof(what));
    CHECK(g.entry.combo == GUIDE_COMBO_SHIFT_TAB && !strcmp(combo, "R, then Y"),
          "R then Y is '%s'", combo);
    static const unsigned y_alone[] = { GUIDE_KEY_Y };
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    (void)guide_feed(&g, &cfg, y_alone, 1);
    CHECK(g.entry.combo == GUIDE_COMBO_Y && g.entry.action == BUTTON_TAB,
          "Y alone is not Tab");
    (void)button_map_set(&map, BUTTON_Y, BUTTON_SPACE);
    (void)button_map_set(&map, BUTTON_B, BUTTON_TAB);
    static const unsigned r_then_b[] = { GUIDE_KEY_R, 0, GUIDE_KEY_B };
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    (void)guide_feed(&g, &cfg, r_then_b, 3);
    guide_entry_texts(&g.entry, combo, sizeof(combo), NULL, 0, NULL, 0);
    CHECK(g.entry.combo == GUIDE_COMBO_SHIFT_TAB && !strcmp(combo, "R, then B"),
          "Tab moved to B, but R then B is '%s'", combo);
    button_map_defaults(&map);

    // L+R on Control: focus, whichever lands first.
    static const unsigned l_r[] = { GUIDE_KEY_L, GUIDE_KEY_L | GUIDE_KEY_R };
    static const unsigned r_l[] = { GUIDE_KEY_R, GUIDE_KEY_L | GUIDE_KEY_R };
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    (void)guide_feed(&g, &cfg, l_r, 2);
    CHECK(g.entry.combo == GUIDE_COMBO_LR && g.entry.what == TR_GUIDE_DO_FOCUS_TILES,
          "L+R on Control is not focus");
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    (void)guide_feed(&g, &cfg, r_l, 2);
    CHECK(g.entry.combo == GUIDE_COMBO_LR, "R+L on Control is not focus");

    // COMPOSE: which shoulder came first is voice_ptt_step's decision.
    guide_init(&g, GUIDE_CTX_COMPOSE, &cfg);
    (void)guide_feed(&g, &cfg, l_r, 2);
    CHECK(g.entry.combo == GUIDE_COMBO_L_THEN_R && g.entry.what == TR_GUIDE_DO_FOCUS,
          "L then R in the composer is not focus");
    guide_init(&g, GUIDE_CTX_COMPOSE, &cfg);
    (void)guide_feed(&g, &cfg, r_l, 2);
    CHECK(g.entry.combo == GUIDE_COMBO_R_THEN_L && g.entry.what == TR_GUIDE_DO_ORDER,
          "R then L, nothing recording, is not the order notice");
    // Hold R past VOICE_HOLD_FRAMES: talk; then L latches or cancels.
    unsigned hold[VOICE_HOLD_FRAMES + 2];
    for (unsigned i = 0; i < VOICE_HOLD_FRAMES + 1; i++) hold[i] = GUIDE_KEY_R;
    hold[VOICE_HOLD_FRAMES + 1] = GUIDE_KEY_R | GUIDE_KEY_L;
    guide_init(&g, GUIDE_CTX_COMPOSE, &cfg);
    (void)guide_feed(&g, &cfg, hold, VOICE_HOLD_FRAMES + 1);
    CHECK(g.entry.combo == GUIDE_COMBO_HOLD_R && g.entry.what == TR_GUIDE_DO_TALK &&
              g.recording,
          "holding R is not talk");
    guide_init(&g, GUIDE_CTX_COMPOSE, &cfg);
    (void)guide_feed(&g, &cfg, hold, VOICE_HOLD_FRAMES + 2);
    CHECK(g.entry.combo == GUIDE_COMBO_R_THEN_L && g.entry.what == TR_GUIDE_DO_HOLD_CANCEL &&
              !g.recording,
          "R then L while recording (HOLD) is not cancel");
    cfg.latch = 1;
    guide_init(&g, GUIDE_CTX_FOCUS, &cfg);
    (void)guide_feed(&g, &cfg, hold, VOICE_HOLD_FRAMES + 2);
    CHECK(g.entry.combo == GUIDE_COMBO_R_THEN_L && g.entry.what == TR_GUIDE_DO_LATCH &&
              g.recording,
          "R then L while recording (LATCH) is not the latch");
    cfg.latch = 0;
    // No speech-to-text: holding R says so, and R then L is only the notice.
    cfg.talk = 0;
    guide_init(&g, GUIDE_CTX_COMPOSE, &cfg);
    (void)guide_feed(&g, &cfg, hold, VOICE_HOLD_FRAMES + 1);
    CHECK(g.entry.what == TR_GUIDE_DO_TALK_OFF && !g.recording,
          "talk started without speech-to-text");
    cfg.talk = 1;

    // CAMERA: the shutter follows the setting; L+R leaves.
    cfg.shutter = SHUTTER_A;
    guide_init(&g, GUIDE_CTX_CAMERA, &cfg);
    (void)guide_feed(&g, &cfg, press_a, 1);
    CHECK(g.entry.combo == GUIDE_COMBO_SHUTTER, "A as the shutter is not the shutter");
    static const unsigned press_r[] = { GUIDE_KEY_R };
    guide_init(&g, GUIDE_CTX_CAMERA, &cfg);
    (void)guide_feed(&g, &cfg, press_r, 1);
    CHECK(g.entry.what == TR_GUIDE_DO_NOTHING, "R took a picture with A as the shutter");
    cfg.shutter = SHUTTER_R;
    guide_init(&g, GUIDE_CTX_CAMERA, &cfg);
    (void)guide_feed(&g, &cfg, r_l, 2);
    CHECK(g.entry.combo == GUIDE_COMBO_LR && g.entry.what == TR_GUIDE_DO_CAM_LEAVE,
          "L+R on the camera does not leave");

    // The ways out, from any context; and left/right pick the context.
    for (int c = 0; c < GUIDE_CTX_COUNT; c++) {
        static const unsigned sel_start[] = { GUIDE_KEY_SELECT,
                                              GUIDE_KEY_SELECT | GUIDE_KEY_START };
        guide_init(&g, (guide_ctx_t)c, &cfg);
        CHECK(guide_feed(&g, &cfg, sel_start, 2) == GUIDE_STEP_LEAVE,
              "SELECT+START does not leave ctx %d", c);
        guide_init(&g, (guide_ctx_t)c, &cfg);
        unsigned prev = 0;
        guide_step_t s = GUIDE_STEP_NONE;
        unsigned frames = 0;
        while (s != GUIDE_STEP_LEAVE && frames < 200) {
            s = guide_step(&g, &cfg, GUIDE_KEY_B, GUIDE_KEY_B & ~prev);
            prev = GUIDE_KEY_B;
            frames++;
        }
        CHECK(frames == GUIDE_B_HOLD_FRAMES, "B held left ctx %d after %u frames", c, frames);
    }
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    static const unsigned right[] = { GUIDE_KEY_RIGHT };
    CHECK(guide_feed(&g, &cfg, right, 1) == GUIDE_STEP_CONTEXT && g.ctx == GUIDE_CTX_KEYS,
          "RIGHT did not move to KEYS");

    // The pop-up: the opening one waits, the rest go after three seconds.
    guide_init(&g, GUIDE_CTX_CONTROL, &cfg);
    for (unsigned i = 0; i < GUIDE_POPUP_FRAMES * 2; i++) (void)guide_tick(&g);
    CHECK(g.shown && g.entry.combo == GUIDE_COMBO_INTRO, "the opening pop-up timed out");
    (void)guide_feed(&g, &cfg, press_a, 1);
    unsigned t = 0;
    while (!guide_tick(&g) && t < 1000) t++;
    CHECK(!g.shown && t + 1 == GUIDE_POPUP_FRAMES, "a pop-up stayed %u frames", t + 1);
    lang_init();
}

static void test_guide_text_and_geometry_fit(void)
{
    lang_init();
    button_map_t map;
    button_map_defaults(&map);
    guide_config_t cfg = { &map, SHUTTER_R, 1, 1, 1 };

    char lines[2][GUIDE_LINE_MAX];
    CHECK(guide_wrap("aaa bbb", 3, lines) == 2 && !strcmp(lines[0], "aaa") &&
              !strcmp(lines[1], "bbb"),
          "wrap: '%s' '%s'", lines[0], lines[1]);
    CHECK(guide_wrap("aaaa bbbb cccc", 4, lines) == 0, "three lines fit in two");
    CHECK(guide_wrap("按住录音", 4, lines) == 2 && !strcmp(lines[0], "按住"),
          "CJK did not break between characters: '%s'", lines[0]);

    for (unsigned l = 0; l < LANG_BUILTIN_COUNT; l++) {
        lang_set_current(l);
        const char *code = lang_code(l);
        // Every pop-up, in every context: three or four lines in the box.
        for (int c = 0; c < GUIDE_CTX_COUNT; c++) {
            for (int k = GUIDE_COMBO_A; k < GUIDE_COMBO_COUNT; k++) {
                for (int rec = 0; rec < 2; rec++) {
                    const guide_entry_t e = guide_entry((guide_ctx_t)c, &cfg,
                                                        (guide_combo_t)k, rec);
                    char combo[GUIDE_LINE_MAX], name[GUIDE_LINE_MAX], what[GUIDE_LINE_MAX];
                    guide_entry_texts(&e, combo, sizeof(combo), name, sizeof(name),
                                      what, sizeof(what));
                    CHECK(lang_display_cols(combo) <= GUIDE_POPUP_COLS &&
                              lang_display_cols(name) <= GUIDE_POPUP_COLS,
                          "%s ctx %d combo %d: '%s' / '%s' wider than the box",
                          code, c, k, combo, name);
                    CHECK(guide_wrap(what, GUIDE_POPUP_COLS, lines) != 0,
                          "%s ctx %d combo %d: '%s' is more than two lines",
                          code, c, k, what);
                }
            }
            // The drawing's labels: face legend "X label" in 11 columns,
            // the d-pad's in 11, START's and SELECT's in 10, the shoulders'
            // "L label" in the 92-pixel tab.
            static const guide_combo_t FACE[] = {
                GUIDE_COMBO_A, GUIDE_COMBO_B, GUIDE_COMBO_X, GUIDE_COMBO_Y,
            };
            for (size_t i = 0; i < 4; i++) {
                const char *s = guide_button_label((guide_ctx_t)c, &cfg, FACE[i]);
                CHECK(lang_display_cols(s) <= 9, "%s ctx %d face label '%s' too wide",
                      code, c, s);
            }
            const char *s = guide_button_label((guide_ctx_t)c, &cfg, GUIDE_COMBO_DPAD);
            CHECK(lang_display_cols(s) <= 11, "%s ctx %d d-pad label '%s'", code, c, s);
            s = guide_button_label((guide_ctx_t)c, &cfg, GUIDE_COMBO_START);
            CHECK(lang_display_cols(s) <= 10, "%s ctx %d START label '%s'", code, c, s);
            s = guide_button_label((guide_ctx_t)c, &cfg, GUIDE_COMBO_SELECT);
            CHECK(lang_display_cols(s) <= 10, "%s ctx %d SELECT label '%s'", code, c, s);
            s = guide_button_label((guide_ctx_t)c, &cfg, GUIDE_COMBO_L);
            CHECK(lang_display_cols(s) + 2 <= (92 - 8) / 6, "%s ctx %d L label '%s'", code, c, s);
            s = guide_button_label((guide_ctx_t)c, &cfg, GUIDE_COMBO_R);
            CHECK(lang_display_cols(s) + 2 <= (92 - 8) / 6, "%s ctx %d R label '%s'", code, c, s);
        }
        // Tabs: in order, apart, each wide enough for its label, on screen.
        for (int cam = 0; cam < 2; cam++) {
            cfg.camera = cam;
            int prev_right = -1;
            for (int c = 0; c < GUIDE_CTX_COUNT; c++) {
                int x, y, w, h;
                if (!guide_tab_rect((guide_ctx_t)c, &cfg, &x, &y, &w, &h)) continue;
                const int need = (int)lang_display_cols(tr(guide_ctx_label((guide_ctx_t)c))) * 6 + 4;
                CHECK(x > prev_right && w >= need && x + w <= 256,
                      "%s tab %d at %d+%d (needs %d, previous ends %d)",
                      code, c, x, w, need, prev_right);
                CHECK(guide_tab_hit(&cfg, x + w / 2, y + h / 2) == (guide_ctx_t)c,
                      "%s tab %d is not where it is drawn", code, c);
                prev_right = x + w - 1;
            }
        }
        cfg.camera = 1;
        CHECK(lang_display_cols(tr(TR_GUIDE_PRESS)) <= 16 &&
                  lang_display_cols(tr(TR_GUIDE_TABS_HINT)) <= 16,
              "%s the drawn screen's lines are wider than it", code);
        CHECK((int)lang_display_cols(tr(TR_GUIDE_DONE)) * 6 <= GUIDE_DONE_W - 4,
              "%s DONE is wider than its tile", code);
        CHECK(lang_display_cols(tr(TR_GUIDE_FOOT)) <= 42 &&
                  lang_display_cols(tr(TR_LEGEND_GUIDE)) <= 42 &&
                  lang_display_cols(tr(TR_COMPOSE_HINT_EDIT)) <= 42,
              "%s a guide line is wider than the screen", code);
        char both[160];
        snprintf(both, sizeof(both), "%s   %s", tr(TR_COMPOSE_HINT_FOCUS),
                 tr(TR_COMPOSE_HINT_TALK));
        CHECK(lang_display_cols(both) <= 42, "%s composer hint '%s' too wide", code, both);
    }
    lang_init();

    // DONE is inside the drawn screen and clear of the tabs.
    CHECK(guide_done_hit(GUIDE_DONE_X + 1, GUIDE_DONE_Y + 1) &&
              !guide_done_hit(GUIDE_DONE_X - 1, GUIDE_DONE_Y) &&
              GUIDE_DONE_Y > GUIDE_TAB_Y + GUIDE_TAB_H,
          "DONE is not where it is drawn");
    CHECK(guide_console(CONSOLE_UNKNOWN) == CONSOLE_DSI &&
              guide_console(CONSOLE_3DS) == CONSOLE_3DS,
          "an unknown console is not drawn as a DSi");

    // The pop-up never covers the input line, at any size.
    for (unsigned n = 1; n <= GUIDE_POPUP_LINES; n++) {
        int x, y, w, h;
        guide_popup_rect(n, &x, &y, &w, &h);
        CHECK(y >= 0 && x >= 0 && x + w <= 256 && y + h <= guide_input_px() - 6,
              "a %u-line pop-up (%d..%d) reaches the input line at %d",
              n, y, y + h, guide_input_px());
    }

    // The still session: the input box where guide_input_row() says, and
    // nothing from anywhere else.
    static grid_t s1;
    guide_session_fill(&s1);
    const int r = guide_input_row();
    CHECK(s1.cells[r * PROTO_COLS].glyph == 0x8b &&
              s1.cells[(r + 1) * PROTO_COLS + 2].glyph == '>' &&
              s1.cells[(r + 2) * PROTO_COLS].glyph == 0x8e,
          "the input box is not at row %d", r);
}

static void test_voice_ptt_latch(void)
{
    voice_ptt_t p;
    voice_ptt_init(&p);

    // 1. HOLD mode (the default): 6 frames of R starts, L+R cancels and sets
    // chord_notice.
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        CHECK(voice_ptt_step(&p, 0, 1, 0, i == 1, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
              "started before frame 6 at frame %u", i);
    }
    CHECK(voice_ptt_step(&p, 0, 1, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_START &&
              p.active && !p.latched,
          "6th frame of R did not start recording");
    // With LATCH off, pressing L while R is held cancels and shows focus notice.
    CHECK(voice_ptt_step(&p, 0, 1, 1, 0, 1, 0, 1, 0, 0) == VOICE_PTT_CANCEL &&
              p.chord_notice && !p.latched && !p.active,
          "L+R with LATCH off did not cancel with chord_notice");
    // Holding R after that cancel cannot start a new recording until released.
    for (unsigned i = 0; i < 10; i++) {
        CHECK(voice_ptt_step(&p, 0, 1, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
              "restarted while R still held after L+R cancel");
    }
    CHECK(voice_ptt_step(&p, 0, 0, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE &&
              !p.wait_release,
          "release after cancel did not clear wait_release");

    // 2. Normal HOLD release: start, hold, let go -> STOP.
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        (void)voice_ptt_step(&p, 0, 1, 0, i == 1, 0, 0, 0, 0, 0);
    }
    CHECK(voice_ptt_step(&p, 0, 1, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_START,
          "second start failed");
    CHECK(voice_ptt_step(&p, 0, 0, 0, 0, 0, 0, 1, 0, 0) == VOICE_PTT_STOP,
          "releasing R in HOLD mode did not stop");

    // 3. LATCH mode: start, press L while holding R -> LATCH, release both ->
    // keeps recording, tap R (or L) on press edge -> STOP, and holding/releasing
    // that stop tap does not start a new recording.
    voice_ptt_init(&p);
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        (void)voice_ptt_step(&p, 1, 1, 0, i == 1, 0, 0, 0, 0, 0);
    }
    CHECK(voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_START,
          "LATCH mode start failed");
    CHECK(voice_ptt_step(&p, 1, 1, 1, 0, 1, 0, 1, 0, 0) == VOICE_PTT_LATCH &&
              p.latched && !p.chord_notice,
          "pressing L while holding R in LATCH mode did not latch");
    // Releasing L, then releasing R, then sitting hands-free keeps recording.
    CHECK(voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 1, 0, 0) == VOICE_PTT_NONE &&
              voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 0) == VOICE_PTT_NONE &&
              voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 0) == VOICE_PTT_NONE &&
              p.latched,
          "releasing R and L while latched stopped the recording");
    // Tap R: stops on the press edge.
    CHECK(voice_ptt_step(&p, 1, 1, 0, 1, 0, 0, 1, 0, 0) == VOICE_PTT_STOP &&
              !p.latched && p.wait_release,
          "tapping R while latched did not stop on press edge");
    // Holding that stop tap for >= 6 frames and then releasing it must not
    // start a new recording.
    for (unsigned i = 0; i < 10; i++) {
        CHECK(voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE,
              "holding the stop tap started a new recording at frame %u", i);
    }
    CHECK(voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 0, 0, 0) == VOICE_PTT_NONE &&
              !p.wait_release,
          "releasing the stop tap acted as a trigger");

    // 4. Tap L to stop a latched recording, X to cancel, and 60 KB auto-stop.
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        (void)voice_ptt_step(&p, 1, 1, 0, i == 1, 0, 0, 0, 0, 0);
    }
    (void)voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    (void)voice_ptt_step(&p, 1, 1, 1, 0, 1, 0, 1, 0, 0);
    (void)voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 0);
    CHECK(voice_ptt_step(&p, 1, 0, 1, 0, 1, 0, 1, 0, 0) == VOICE_PTT_STOP &&
              !p.latched,
          "tapping L while latched did not stop");
    (void)voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 0, 0, 0);

    // X cancels a latched recording.
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        (void)voice_ptt_step(&p, 1, 1, 0, i == 1, 0, 0, 0, 0, 0);
    }
    (void)voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    (void)voice_ptt_step(&p, 1, 1, 1, 0, 1, 0, 1, 0, 0);
    (void)voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 0);
    CHECK(voice_ptt_step(&p, 1, 0, 0, 0, 0, 1, 1, 0, 0) == VOICE_PTT_CANCEL &&
              !p.latched,
          "X did not cancel a latched recording");

    // 60 KB cap (full=1) stops a latched recording automatically.
    for (unsigned i = 1; i < VOICE_HOLD_FRAMES; i++) {
        (void)voice_ptt_step(&p, 1, 1, 0, i == 1, 0, 0, 0, 0, 0);
    }
    (void)voice_ptt_step(&p, 1, 1, 0, 0, 0, 0, 0, 0, 0);
    (void)voice_ptt_step(&p, 1, 1, 1, 0, 1, 0, 1, 0, 0);
    (void)voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 0);
    CHECK(voice_ptt_step(&p, 1, 0, 0, 0, 0, 0, 1, 0, 1) == VOICE_PTT_AUTO_STOP &&
              !p.latched && !p.active,
          "60 KB cap did not auto-stop a latched recording");

    // 5. Settings row and file persistence: mic_mode=hold|latch, missing or
    // unknown values default to hold and never reset other settings, and a
    // 1.0.1 settings file loads intact.
    settings_t st, back;
    settings_ui_t ui;
    char err[64] = "";
    settings_defaults(&st);
    settings_ui_init(&ui);
    CHECK(st.mic_mode == SETTINGS_MIC_HOLD &&
              settings_option_current(&st, SETTINGS_ROW_MIC) == SETTINGS_MIC_HOLD,
          "default mic_mode is not HOLD");
    CHECK(settings_press(&st, &ui, SETTINGS_ROW_MIC, SETTINGS_MIC_LATCH, NULL) ==
              SETTINGS_PRESS_CHANGED &&
              st.mic_mode == SETTINGS_MIC_LATCH &&
              settings_option_current(&st, SETTINGS_ROW_MIC) == SETTINGS_MIC_LATCH,
          "SETTINGS_ROW_MIC did not switch to LATCH");
    static const char mpath[] = "/tmp/dsterm-mic-settings.cfg";
    CHECK(settings_save(mpath, &st, err, sizeof(err)) &&
              settings_load(mpath, &back, err, sizeof(err)) &&
              back.mic_mode == SETTINGS_MIC_LATCH,
          "mic_mode=latch did not survive save/load: %s", err);
    remove(mpath);

    // A file written by 1.0.1 (no mic_mode, no lang) loads with mic_mode=hold
    // and keeps every setting 1.0.1 wrote.
    static const char v101_file[] =
        "# DS Claude Terminal settings\n"
        "version=4\n"
        "model=sonnet\n"
        "boot=keys\n"
        "sound=snes\n"
        "volume=high\n"
        "blips=low\n"
        "typing=off\n"
        "theme=sea\n"
        "autoconnect=off\n"
        "addresses=hide\n"
        "viewtaps=on\n"
        "god_mode=on\n"
        "button_a=enter\n"
        "button_b=space\n"
        "button_x=esc\n"
        "button_y=tab\n"
        "button_start=stop\n"
        "shutter=l\n"
        "picture=small\n"
        "liveview=colour\n";
    CHECK(settings_parse(v101_file, &back, err, sizeof(err)) &&
              back.model == SETTINGS_MODEL_SONNET &&
              back.boot == SETTINGS_BOOT_KEYS &&
              back.sound == SETTINGS_SOUND_SNES &&
              back.volume == SETTINGS_VOLUME_HIGH &&
              back.blips == SETTINGS_WRITING_LOW &&
              back.typing == 0 &&
              back.theme == SETTINGS_THEME_SEA &&
              back.autoconnect == 0 &&
              back.hide_addresses == 1 &&
              back.view_taps == 1 &&
              back.god_mode == 1 &&
              back.buttons.action[BUTTON_B] == BUTTON_SPACE &&
              back.camera_shutter == SHUTTER_L &&
              back.picture == SETTINGS_PICTURE_SMALL &&
              back.liveview == SETTINGS_LIVEVIEW_COLOUR &&
              back.mic_mode == SETTINGS_MIC_HOLD &&
              back.language == LANG_EN,
          "a 1.0.1 settings file did not load intact: %s", err);

    // Unknown or empty mic_mode falls back to hold and never resets other settings.
    static const char bad_mic[] =
        "version=4\ngod_mode=on\ntyping=off\nmic_mode=telepathy\n"
        "future_key=hello\n";
    CHECK(settings_parse(bad_mic, &back, err, sizeof(err)) &&
              back.mic_mode == SETTINGS_MIC_HOLD &&
              back.god_mode == 1 &&
              back.typing == 0,
          "unknown mic_mode or unknown key reset other settings: %s", err);
}

static void test_project_glyph_rows_never_type(void)
{
    // A DO row may carry 0x80..0xBF now (display glyphs, `project64`), and on
    // a PTY 0x9B is a one-byte CSI.  The row decodes and draws; it must never
    // become keys, in the composer or in GOD MODE.
    static project_t project;
    project_action_t actions[PROJECT_ACTIONS_MAX];
    unsigned count = 0, index = 0, tab = 0, fg = 0;
    char label[PROJECT_LABEL_MAX + 1];
    char value[PROJECT_VALUE_MAX + 1];
    const uint8_t row[] = { 1, 0, PROJECT_TAB_DO, 9, 4, 5,
                            'w', 'i', 'p', 'e', 'e', 'c', 0x9B, '2', 'J' };
    CHECK(proto_apply_project(&count, &index, &tab, &fg, label, sizeof(label),
                              value, sizeof(value), row,
                              (uint16_t)sizeof(row)) == PROTO_OK,
          "a row with a glyph byte was refused outright");
    CHECK((uint8_t)value[2] == 0x9B, "the glyph byte did not survive for display");

    project_init(&project);
    CHECK(project_wire_row(&project, count, index, (project_tab_t)tab, fg,
                           label, value) == 1, "the snapshot did not commit");
    const unsigned n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    CHECK(n > 0 && actions[0].from_server && (uint8_t)actions[0].body[2] == 0x9B,
          "the server's action did not come through as sent");

    // The gate every row-to-keys path takes (main.c, project_action_press).
    CHECK(!project_action_keyable(actions[0].body),
          "an action carrying 0x9B is typeable");
    // And the composer refuses it whole as a second line, rather than
    // loading it with the byte dropped.
    composer_t c;
    composer_init(&c);
    CHECK(!composer_load(&c, actions[0].body) && c.mode == COMPOSER_CLOSED,
          "the composer loaded an action carrying 0x9B");
    for (unsigned i = 0; i < n; i++) {
        if (!project_action_keyable(actions[i].body)) continue;
        CHECK(strchr(actions[i].body, (char)0x9B) == NULL,
              "action %u is typeable and carries 0x9B", i);
    }
    // The client's own actions, and plain ASCII ones, still type.
    CHECK(project_action_keyable("npm run test") && !project_action_keyable("") &&
              !project_action_keyable("a\tb") && !project_action_keyable("x\x7f"),
          "the keyable gate is not exactly printable ASCII");
    CHECK(strstr(PROTO_HELLO_CAPS, " project64 ") != NULL,
          "the card does not announce project64");
    CHECK(PROTO_PROJECT_ROWS_MAX == 64, "the card no longer takes 64 rows");
}

int main(void)
{
    // The square-wave voice for every PSG check below; the SNES instrument
    // has its own tests, which select it themselves.
    sound_set_style(SOUND_STYLE_CHIP);

    printf("== sha256 ==\n");
    test_sha256_vectors();
    test_sha256_streaming_matches_oneshot();
    test_hmac_rfc4231();
    test_ct_equal();

    printf("== framing ==\n");
    test_frame_roundtrip();
    test_payload_survives_until_next_feed();
    test_byte_at_a_time();
    test_many_messages_one_feed();
    test_hostile_headers();
    test_hello_capabilities_fit_the_handshake_buffer();

    test_connect_all();

    printf("== grid ==\n");
    test_full_frame();
    test_diff_bounds();
    test_diff_validates_before_writing();
    test_scroll();

    printf("== cursor ==\n");
    test_cursor_apply();
    test_cursor_frame_accepted_by_reader();
    test_view_pos_apply();
    test_view_pos_frame_accepted_by_reader();
    test_mode_apply();
    test_mode_frame_accepted_by_reader();

    printf("== config ==\n");
    test_config_valid();
    test_config_psk_grouped();
    test_config_missing_fields();
    test_config_bad_psk();
    test_config_robustness();

    printf("== log ==\n");
    test_log_ring();
    test_log_truncation();
    test_log_vprintf_forwards();
    test_log_blank();

    printf("== status ==\n");
    test_lang_and_custom_packs();
    test_latin_glyphs_are_letters_with_marks();
    test_settings_unknown_language_keeps_the_rest();
    test_chime_headline_every_language();
    test_layout_widths_every_language();
    test_custom_pack_labels_never_push_off_screen();
    test_lang_coverage_and_budgets();
    test_voice_adpcm_and_wire();
    test_voice_late_replies_are_tagged();
    test_an_empty_enter_is_not_a_send();
    test_voice_language_is_not_the_ui_language();
    test_a_malformed_voice_reply_still_pays();
    test_settings_values_fall_back_alone();
    test_custom_pack_cut_on_a_character();
    test_voice_bar_edges_and_guide_context();
    test_quick_files_reject_what_they_cannot_read();
    test_voice_ptt_focus_order();
    test_voice_ptt_latch();
    test_guide_every_combo_in_every_context();
    test_guide_mapping_and_order();
    test_guide_text_and_geometry_fit();
    test_project_glyph_rows_never_type();
    test_settings_page();
    test_privacy_mask();
    test_quick();
    test_mirror();
    test_buttons_and_their_tab();
    test_shutter_setting();
    test_picture_setting();
    test_clip_wire();
    test_clip_wire_decoder();
    test_clip_presentation();
    test_project_page();
    test_project_wire();
    test_project_actions();
    test_status_mode_toggle();
    test_status_view_position();
    test_status_scroll_regime();
    test_status_counters();
    test_status_link();

    printf("== composer ==\n");
    test_composer_bounds_and_editing();
    test_composer_send_frames();
    test_snippets_parser();
    test_shipped_snippets_example_parses();
    test_snippet_proto_apply();
    test_snippet_frame_accepted_by_reader();
    test_snippets_apply_pushed();
    test_snippet_load_picked();
    test_snippet_composer_gate_shape();

    printf("== sessions ==\n");
    test_session_proto_apply();
    test_session_frames_and_pick_wire();
    test_sessions_atomic_state_and_selection();
    test_sessions_repeated_snapshot_stability();

    test_sound_snes_all();
    test_remote_all();
    test_seal_all();
    test_camera_all();
    test_fuzz_all();

    printf("== sound ==\n");
    test_sound_cues_all_terminate();
    test_sound_cues_rise_or_fall_by_meaning();
    test_sound_mute_silences_everything();
    test_sound_tick_reports_only_changes();
    test_sound_ignores_a_bad_cue();
    test_sound_keypress_answers_the_user();
    test_sound_switch_and_focus_profiles();
    test_chime_owns_resync_and_history();
    test_chime_mute_covers_everything();

    printf("== keyboard ==\n");
    test_kbd_every_key_maps_to_bytes();
    test_kbd_geometry();
    test_kbd_hit();
    test_kbd_id_at_grid();
    test_kbd_keyword_assembly();
    test_focus_control_grid();
    test_focus_keyboard_grid();
    test_focus_keyboard_shortcuts();
    test_focus_grid_survives_rebuild();
    test_kbd_control_geometry();
    test_kbd_relink_hit();
    test_no_control_tile_duplicates_a_button();
    test_keys_is_the_primary_typing_control();
    test_kbd_layers();
    test_kbd_shift_and_sym_bytes();
    test_kbd_ctrl_folding();
    test_kbd_shift_one_shot();
    test_kbd_ctrl_and_shift_together();
    test_kbd_repeat_timing();
    test_kbd_repeatable();

    printf("== backoff ==\n");
    test_backoff_starts_short();
    test_backoff_doubles();
    test_backoff_is_capped();
    test_backoff_never_returns_zero();
    test_backoff_is_monotonic();

    test_page_keys_are_complete_sequences();
    test_page_keys_are_the_bytes_a_keyboard_sends();
    test_a_page_key_survives_being_framed();

    print_interop_vectors();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
