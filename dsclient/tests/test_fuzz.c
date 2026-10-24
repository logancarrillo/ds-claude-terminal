// Seeded fuzzing of everything the card parses from outside.
//
// Each parser that takes bytes from the network or the SD card -- the frame
// reader, every server-to-client decoder, SEAL, discovery answers, the
// config, profile, snippet, settings and recent-prompt files, a custom WAV,
// the COMMANDS list -- is
// handed random and half-valid input, thousands of times, under the address
// and undefined-behaviour sanitizers this suite always builds with.  The
// sanitizers are most of the test: any read or write outside a buffer, any
// signed overflow, any misaligned access stops the run.  The CHECKs add the
// contracts a caller relies on -- a result that is one of the known codes,
// strings that end inside their buffers, counts inside their limits.
//
// Fixed seeds, so a failure is the same failure every run.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"

#include "../source/config.h"
#include "../source/discover.h"
#include "../source/lang.h"
#include "../source/profiles.h"
#include "../source/proto.h"
#include "../source/quick.h"
#include "../source/remote.h"
#include "../source/seal.h"
#include "../source/sessions.h"
#include "../source/settings.h"
#include "../source/snippets.h"
#include "../source/wav.h"

static uint64_t fz_state;

static void fz_seed(uint64_t seed) { fz_state = seed ? seed : 0x9E3779B97F4A7C15ull; }

static uint32_t fz_next(void)
{
    // xorshift64*: plenty for test data, and the same on every host.
    fz_state ^= fz_state >> 12;
    fz_state ^= fz_state << 25;
    fz_state ^= fz_state >> 27;
    return (uint32_t)((fz_state * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint32_t fz_below(uint32_t n) { return n ? fz_next() % n : 0; }

// A length worth trying: small, one of the protocol's fixed sizes, or the
// limit, so a length check passes and what is behind it gets tested.
static uint16_t fz_len(void)
{
    static const uint16_t sizes[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 17, 32, 33, 36, 40, 48, 64,
        65, 100, 200, 255, 256, 257, 536, 1024, 1028,
        (uint16_t)(PROTO_COLS * PROTO_ROWS * 3), PROTO_MAX_PAYLOAD,
    };
    if (fz_below(4) == 0) return (uint16_t)fz_below(PROTO_MAX_PAYLOAD + 1);
    return sizes[fz_below(sizeof(sizes) / sizeof(sizes[0]))];
}

static void fz_fill(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)fz_next();
}

// Mostly printable text with newlines, '=' and '#', the shape the card's
// files have, and now and then a raw byte.
static void fz_text(char *buf, size_t cap)
{
    static const char shape[] = "abcdefghijklmnopqrstuvwxyz0123456789 =#./:-_\n\t\r\"'\\[]";
    const size_t n = fz_below((uint32_t)cap);
    for (size_t i = 0; i < n; i++) {
        buf[i] = fz_below(16) ? shape[fz_below(sizeof(shape) - 1)]
                              : (char)(fz_below(255) + 1);
    }
    buf[n] = '\0';
}

static int known_result(proto_result_t r)
{
    return r == PROTO_OK || r == PROTO_NEED_MORE || r == PROTO_ERR_MAGIC ||
           r == PROTO_ERR_VERSION || r == PROTO_ERR_TYPE ||
           r == PROTO_ERR_LENGTH || r == PROTO_ERR_PAYLOAD ||
           r == PROTO_ERR_BOUNDS || r == PROTO_ERR_OVERFLOW;
}

static int ends_inside(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

// Each input gets a heap block of exactly its own length, so a read even one
// byte past `len` lands outside the allocation and the address sanitizer
// stops the run -- inside one big static buffer it would go unseen.
static uint8_t *fz_exact(size_t n)
{
    uint8_t *p = malloc(n ? n : 1);
    if (!p) abort();
    fz_fill(p, n);
    return p;
}

static void fuzz_frame_reader(void)
{
    fz_seed(0xF00D0001);
    static framereader_t fr;
    int bad = 0;
    for (int round = 0; round < 400; round++) {
        framereader_init(&fr);
        for (int k = 0; k < 6; k++) {
            const size_t n = fz_below(701);
            uint8_t *chunk = fz_exact(n);
            if (n >= 2 && fz_below(2)) {        // sometimes a plausible header
                chunk[0] = PROTO_MAGIC0;
                chunk[1] = PROTO_MAGIC1;
                if (n >= 3) chunk[2] = PROTO_VERSION;
            }
            const size_t room = framereader_space(&fr);
            proto_result_t r = framereader_feed(&fr, chunk, n < room ? n : room);
            if (!known_result(r)) bad++;
            msg_type_t type;
            const uint8_t *p;
            uint16_t plen;
            for (int m = 0; m < 8; m++) {
                r = framereader_next(&fr, &type, &p, &plen);
                if (!known_result(r)) bad++;
                if (r != PROTO_OK) break;
                if (plen > PROTO_MAX_PAYLOAD) bad++;
            }
            free(chunk);
            if (r != PROTO_OK && r != PROTO_NEED_MORE) break;
        }
    }
    CHECK(bad == 0, "the frame reader gave %d unknown results or lengths", bad);
}

static void fuzz_screen_decoders(void)
{
    fz_seed(0xF00D0002);
    static grid_t g;
    cursor_t cur;
    view_pos_t pos;
    int bad = 0;
    grid_clear(&g);
    for (int i = 0; i < 3000; i++) {
        const uint16_t len = fz_len();
        uint8_t *payload = fz_exact(len);
        int lines = 0;
        uint8_t flags = 0;
        switch (fz_below(6)) {
        case 0: bad += !known_result(proto_apply_full(&g, payload, len)); break;
        case 1: bad += !known_result(proto_apply_diff(&g, payload, len)); break;
        case 2: bad += !known_result(proto_apply_scroll(&g, payload, len, &lines)); break;
        case 3: bad += !known_result(proto_apply_cursor(&cur, payload, len)); break;
        case 4: bad += !known_result(proto_apply_view_pos(&pos, payload, len)); break;
        default: bad += !known_result(proto_apply_mode(&flags, payload, len)); break;
        }
        free(payload);
    }
    CHECK(bad == 0, "%d screen decodes gave an unknown result", bad);
}

static void fuzz_list_decoders(void)
{
    fz_seed(0xF00D0003);
    int bad = 0;
    static remote_chunk_t rc;
    static session_chunk_t sc;
    static photo_saved_t ps;
    char a[256], b[1100];
    for (int i = 0; i < 4000; i++) {
        const uint16_t len = fz_len();
        uint8_t *payload = fz_exact(len);
        memset(a, 'Z', sizeof(a));
        memset(b, 'Z', sizeof(b));
        unsigned u1, u2, u3, u4;
        uint32_t u32;
        size_t s1, s2;
        uint8_t x, y, kind;
        int flag;
        proto_result_t r = PROTO_OK;
        switch (fz_below(16)) {
        case 0: r = proto_apply_remote(&rc, payload, len); break;
        case 1:
            r = proto_apply_remote_note(a, sizeof(a), payload, len);
            if (r == PROTO_OK) bad += !ends_inside(a, sizeof(a));
            break;
        case 2:
            r = proto_apply_state(&x, a, sizeof(a), payload, len);
            if (r == PROTO_OK) bad += !ends_inside(a, sizeof(a));
            break;
        case 3: r = proto_apply_sessions(&sc, payload, len); break;
        case 4:
            r = proto_apply_project(&u1, &u2, &u3, &u4, a, sizeof(a), b,
                                    sizeof(b), payload, len);
            if (r == PROTO_OK) bad += !ends_inside(a, sizeof(a)) + !ends_inside(b, sizeof(b));
            break;
        case 5:
            r = proto_apply_clip(&u1, &u2, &u3, &u4, &u32, b, sizeof(b), &u1,
                                 payload, len);
            if (r == PROTO_OK) bad += u1 > sizeof(b);
            break;
        case 6:
            r = proto_apply_snippet(&u1, &u2, a, sizeof(a), &s1, b, sizeof(b),
                                    &s2, payload, len);
            if (r == PROTO_OK) bad += (s1 >= sizeof(a)) + (s2 >= sizeof(b));
            break;
        case 7: r = proto_decode_mirror(&flag, payload, len); break;
        case 8:
            r = proto_decode_touch(&x, &y, payload, len);
            if (r == PROTO_OK) bad += y >= 192;
            break;
        case 9: r = proto_decode_camera_ready(&flag, payload, len); break;
        case 10: r = proto_decode_camera_ack(&kind, &u32, payload, len); break;
        case 11:
            r = proto_decode_photo_saved(&ps, payload, len);
            if (r == PROTO_OK) bad += !ends_inside(ps.path, sizeof(ps.path));
            break;
        case 12: r = proto_decode_mic_ready(&flag, payload, len); break;
        case 13: r = proto_decode_voice_ack(&u32, payload, len); break;
        case 14: {
            static voice_text_t vt;
            r = proto_decode_voice_text(&vt, payload, len);
            if (r == PROTO_OK) bad += !ends_inside(vt.text, sizeof(vt.text));
            break;
        }
        default: {
            static quick_commands_t qc;
            quick_commands_init(&qc);
            (void)quick_commands_apply(&qc, payload, len);
            break;
        }
        }
        bad += !known_result(r);
        free(payload);
    }
    CHECK(bad == 0, "%d list or camera decodes broke a contract", bad);
}

static void fuzz_seal(void)
{
    fz_seed(0xF00D0004);
    static seal_t s;
    uint8_t key[16], nonce[PROTO_NONCE_SIZE], hello[64];
    fz_fill(key, sizeof(key));
    fz_fill(nonce, sizeof(nonce));
    fz_fill(hello, sizeof(hello));
    int accepted = 0;
    for (int i = 0; i < 2000; i++) {
        seal_start(&s, key, sizeof(key), nonce, hello, sizeof(hello));
        const uint16_t len = fz_len();
        uint8_t *payload = fz_exact(len);
        uint8_t *plain = fz_exact(len);
        if (seal_open(&s, (msg_type_t)fz_below(256), payload, len, plain) >= 0) {
            accepted++;
        }
        free(plain);
        free(payload);
    }
    // A random tag is right one time in 2^64.
    CHECK(accepted == 0, "SEAL accepted %d random frames", accepted);
}

static void fuzz_discovery(void)
{
    fz_seed(0xF00D0005);
    discover_entry_t e;
    int bad = 0;
    for (int i = 0; i < 4000; i++) {
        char text[200];
        uint8_t raw[200];
        size_t n;
        if (fz_below(2)) {
            // Shaped like an answer, with random fields.
            fz_text(text, 60);
            n = (size_t)snprintf((char *)raw, sizeof(raw), "DSTM here %u name=%s port=%u pair=%u",
                                 fz_below(4), text, fz_below(70000), fz_below(3));
            if (n >= sizeof(raw)) n = sizeof(raw) - 1;
        } else {
            n = fz_below(sizeof(raw) + 1);
            fz_fill(raw, n);
        }
        uint8_t *exact = malloc(n ? n : 1);
        if (!exact) abort();
        memcpy(exact, raw, n);
        if (discover_parse_answer(exact, n, &e)) {
            bad += !ends_inside(e.name, sizeof(e.name));
        }
        free(exact);
    }
    CHECK(bad == 0, "%d discovery answers left a name without its end", bad);
}

static void fuzz_card_files(void)
{
    fz_seed(0xF00D0006);
    static char text[4096];
    static dsterm_config_t cfg;
    static snippets_t sn;
    static wifi_profile_t w;
    static server_profile_t sp;
    char err[128];
    int bad = 0;
    static settings_t st;
    static quick_recent_t recent;
    for (int i = 0; i < 4000; i++) {
        fz_text(text, sizeof(text) - 1);
        // An exact-size copy, terminator included: a parser reading past the
        // NUL leaves the allocation and the address sanitizer stops the run.
        const size_t n = strlen(text) + 1;
        char *exact = malloc(n);
        if (!exact) abort();
        memcpy(exact, text, n);
        memset(err, 'Z', sizeof(err));
        switch (fz_below(7)) {
        case 0: (void)config_parse(exact, &cfg, err, sizeof(err)); break;
        case 1: (void)snippets_parse(exact, &sn, err, sizeof(err)); break;
        case 2: (void)wifi_profile_parse(exact, &w, err, sizeof(err)); break;
        case 3: (void)server_profile_parse(exact, &sp, err, sizeof(err)); break;
        case 4: (void)settings_parse(exact, &st, err, sizeof(err)); break;
        case 5:
            lang_init();
            (void)lang_load_text(exact, "fz", err, sizeof(err));
            lang_init();
            break;
        default:
            quick_recent_parse(&recent, exact);
            bad += recent.count > QUICK_RECENT_MAX;
            for (unsigned k = 0; k < recent.count && k < QUICK_RECENT_MAX; k++) {
                bad += !ends_inside(recent.items[k], sizeof(recent.items[k]));
            }
            break;
        }
        free(exact);
        // An error message, when one is written, ends inside its buffer.
        if (err[0] != 'Z') bad += !ends_inside(err, sizeof(err));
    }
    CHECK(bad == 0, "%d parses wrote an error past its buffer's end", bad);
}

// Short texts a person types or a file holds: a key, an address, a host.
static void fuzz_short_texts(void)
{
    fz_seed(0xF00D0009);
    int bad = 0;
    for (int i = 0; i < 6000; i++) {
        char text[80];
        fz_text(text, sizeof(text) - 1);
        const size_t n = strlen(text) + 1;
        char *exact = malloc(n);
        if (!exact) abort();
        memcpy(exact, text, n);
        uint8_t psk[CFG_PSK_BYTES];
        char err[96], host[CFG_ADDR_MAX];
        uint32_t ip;
        uint16_t port = 0;
        memset(host, 'Z', sizeof(host));
        switch (fz_below(3)) {
        case 0: (void)config_parse_psk(exact, psk, err, sizeof(err)); break;
        case 1: (void)ipv4_parse(exact, &ip); break;
        default:
            if (host_port_parse(exact, host, &port, 8347)) {
                bad += !ends_inside(host, sizeof(host));
            }
            break;
        }
        free(exact);
    }
    CHECK(bad == 0, "%d host parses left a host without its end", bad);
}

// What a decoded list frame is staged into: REMOTE's rows, the SESSIONS
// list and a pushed snippet set. Random bytes almost never decode, so these
// payloads have the right shape -- counts, indices and lengths that agree,
// sometimes one over a limit -- and random contents, and the stagers get
// whatever the decoders accept, in any order, sets interleaved.
static size_t fz_small(size_t limit)
{
    // Mostly inside the limit, sometimes exactly at it, now and then past.
    switch (fz_below(8)) {
    case 0: return limit;
    case 1: return limit + 1;
    default: return fz_below((uint32_t)limit + 1);
    }
}

static void fuzz_list_staging(void)
{
    fz_seed(0xF00D000A);
    static remote_list_t rl;
    static sessions_t ss;
    static snippets_t sn;
    static remote_chunk_t rc;
    static session_chunk_t sc;
    uint8_t buf[600];
    remote_init(&rl);
    sessions_init(&ss);
    memset(&sn, 0, sizeof(sn));
    int staged = 0;
    for (int i = 0; i < 20000; i++) {
        size_t n = 0;
        const unsigned kind = fz_below(3);
        if (kind == 0) {                       // REMOTE: count index kind state flags age name detail
            const size_t name = fz_small(REMOTE_NAME_MAX), detail = fz_small(REMOTE_DETAIL_MAX);
            buf[n++] = (uint8_t)fz_small(REMOTE_MAX);
            buf[n++] = (uint8_t)fz_below(REMOTE_MAX + 1);
            for (int k = 0; k < 5; k++) buf[n++] = (uint8_t)fz_next();
            buf[n++] = (uint8_t)name;
            for (size_t k = 0; k < name; k++) buf[n++] = (uint8_t)(' ' + fz_below(96));
            buf[n++] = (uint8_t)detail;
            for (size_t k = 0; k < detail; k++) buf[n++] = (uint8_t)(' ' + fz_below(96));
        } else if (kind == 1) {                // SESSIONS: count index flags idle(2) name_len name
            const size_t name = fz_small(SESSION_NAME_MAX);
            buf[n++] = (uint8_t)fz_small(SESSIONS_MAX);
            buf[n++] = (uint8_t)fz_below(SESSIONS_MAX + 1);
            for (int k = 0; k < 3; k++) buf[n++] = (uint8_t)fz_next();
            buf[n++] = (uint8_t)name;
            for (size_t k = 0; k < name; k++) buf[n++] = (uint8_t)fz_next();
        } else {                               // snippet: index total label_len label body_len(2) body
            const size_t label = fz_small(SNIPPET_LABEL_MAX), body = fz_small(SNIPPET_BODY_MAX);
            buf[n++] = (uint8_t)fz_below(SNIPPETS_MAX + 1);
            buf[n++] = (uint8_t)fz_small(SNIPPETS_MAX);
            buf[n++] = (uint8_t)label;
            for (size_t k = 0; k < label; k++) buf[n++] = (uint8_t)(' ' + fz_below(96));
            buf[n++] = (uint8_t)(body >> 8);
            buf[n++] = (uint8_t)body;
            for (size_t k = 0; k < body; k++) buf[n++] = (uint8_t)fz_next();
        }
        if (fz_below(16) == 0 && n) n -= 1 + fz_below((uint32_t)n);   // now and then cut short
        uint8_t *payload = malloc(n ? n : 1);
        if (!payload) abort();
        memcpy(payload, buf, n);
        if (kind == 0 && proto_apply_remote(&rc, payload, (uint16_t)n) == PROTO_OK) {
            (void)remote_apply_chunk(&rl, &rc);
            staged++;
        } else if (kind == 1 && proto_apply_sessions(&sc, payload, (uint16_t)n) == PROTO_OK) {
            (void)sessions_apply_chunk(&ss, &sc);
            staged++;
        } else if (kind == 2) {
            unsigned idx, total;
            char label[SNIPPET_LABEL_MAX + 1], body[SNIPPET_BODY_MAX + 1];
            size_t ll, bl;
            if (proto_apply_snippet(&idx, &total, label, sizeof(label), &ll,
                                    body, sizeof(body), &bl, payload, (uint16_t)n) == PROTO_OK) {
                (void)snippets_apply_pushed(&sn, idx, total, label, ll, body, bl);
                staged++;
            }
        }
        free(payload);
    }
    CHECK(staged > 5000, "only %d of 20000 shaped frames reached the stagers", staged);
    CHECK(rl.count <= REMOTE_MAX && ss.count <= SESSIONS_MAX && sn.count <= SNIPPETS_MAX,
          "a staged list outgrew its limit");
}

static void fuzz_wav(void)
{
    fz_seed(0xF00D0007);
    static uint8_t data[4096];
    static int16_t pcm[8192];
    char why[96];
    int bad = 0;
    for (int i = 0; i < 3000; i++) {
        size_t n = fz_below(sizeof(data) + 1);
        fz_fill(data, n);
        if (n >= 44 && fz_below(2)) {
            // A real header, then random fields inside it.
            memcpy(data, "RIFF", 4);
            memcpy(data + 8, "WAVEfmt ", 8);
            if (fz_below(2)) memcpy(data + 36, "data", 4);
        }
        uint8_t *exact = malloc(n ? n : 1);
        if (!exact) abort();
        memcpy(exact, data, n);
        uint32_t hz = 0;
        const size_t want = wav_measure(exact, n, &hz, why, sizeof(why));
        if (want && want <= sizeof(pcm) / sizeof(pcm[0])) {
            wav_t out = {pcm, 0, 0};
            if (wav_decode(exact, n, &out, sizeof(pcm) / sizeof(pcm[0]), why, sizeof(why))) {
                bad += out.samples > sizeof(pcm) / sizeof(pcm[0]);
            }
        }
        free(exact);
    }
    CHECK(bad == 0, "%d WAVs decoded past their buffer", bad);
}

static void fuzz_quick_line(void)
{
    fz_seed(0xF00D0008);
    quick_line_t l;
    char out[QUICK_RECENT_TEXT_MAX + 1];
    int bad = 0;
    quick_line_reset(&l);
    for (int i = 0; i < 4000; i++) {
        char bytes[80];
        const size_t n = fz_below(sizeof(bytes));
        for (size_t k = 0; k < n; k++) {
            bytes[k] = fz_below(8) ? (char)(' ' + fz_below(95)) : (char)fz_below(256);
        }
        char *exact = malloc(n ? n : 1);
        if (!exact) abort();
        memcpy(exact, bytes, n);
        if (quick_line_feed(&l, exact, n, out, sizeof(out))) {
            bad += !ends_inside(out, sizeof(out));
        }
        free(exact);
        bad += l.len > QUICK_RECENT_TEXT_MAX;
    }
    CHECK(bad == 0, "%d typed lines broke the line's bounds", bad);
}

void test_fuzz_all(void)
{
    printf("== fuzz ==\n");
    fuzz_frame_reader();
    fuzz_screen_decoders();
    fuzz_list_decoders();
    fuzz_seal();
    fuzz_discovery();
    fuzz_card_files();
    fuzz_short_texts();
    fuzz_list_staging();
    fuzz_wav();
    fuzz_quick_line();
}
