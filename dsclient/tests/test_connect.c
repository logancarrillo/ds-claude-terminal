// Host tests for getting connected: the pairing crypto, the card's profile
// store, discovery, and the boot menu's pure half.
//
// Run from test_core.c's main() as part of the terminal's suite.  The
// INTEROP lines it prints are read by tools/crosscheck.py, which recomputes
// them with the server's Python and fails if one byte differs.

#include <sys/types.h>   // before dirent.h: newlib's needs off_t
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "support.h"

#include "../source/bootui.h"
#include "../source/console.h"
#include "../source/discover.h"
#include "../source/entropy.h"
#include "../source/pair.h"
#include "../source/profiles.h"
#include "../source/proto.h"
#include "../source/sha256.h"
#include "../source/x25519.h"

static void unhex(const char *h, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void tohex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

// ----------------------------------------------------------------- crypto ---

static void test_x25519_rfc7748(void)
{
    uint8_t k[32], u[32], out[32], want[32];
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    unhex("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", want, 32);
    x25519(out, k, u);
    CHECK(memcmp(out, want, 32) == 0, "RFC 7748 vector 1");

    uint8_t alice[32], bob[32], apub[32], bpub[32], s1[32], s2[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", bob, 32);
    x25519_public(apub, alice);
    x25519_public(bpub, bob);
    unhex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", want, 32);
    CHECK(memcmp(apub, want, 32) == 0, "Alice's public key");
    unhex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", want, 32);
    CHECK(memcmp(bpub, want, 32) == 0, "Bob's public key");
    x25519(s1, alice, bpub);
    x25519(s2, bob, apub);
    unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", want, 32);
    CHECK(memcmp(s1, want, 32) == 0 && memcmp(s2, want, 32) == 0, "shared secret");

    // The point the pairing code refuses: u = 0 gives zero whatever the key.
    static const uint8_t zero[32] = {0};
    x25519(out, alice, zero);
    CHECK(memcmp(out, zero, 32) == 0, "low-order point does not give zero");
}

static void test_hkdf_rfc5869(void)
{
    uint8_t ikm[22], salt[13], info[10], okm[42], want[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    unhex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865", want, 42);
    CHECK(hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, 42), "hkdf refused");
    CHECK(memcmp(okm, want, 42) == 0, "RFC 5869 case 1");
    uint8_t big[HKDF_INFO_MAX + 1] = {0};
    CHECK(!hkdf_sha256(salt, 13, ikm, 22, big, sizeof(big), okm, 32),
          "an over-long info must be refused, not truncated");
    CHECK(!hkdf_sha256(salt, 13, ikm, 22, info, 10, okm, HKDF_OUT_MAX + 1),
          "an over-long output must be refused");
}

// The fixed exchange crosscheck.py recomputes.
static void vector_keys(uint8_t card_priv[32], uint8_t srv_priv[32],
                        uint8_t card_nonce[16], uint8_t srv_nonce[16],
                        uint8_t id[4])
{
    for (int i = 0; i < 32; i++) {
        card_priv[i] = (uint8_t)i;
        srv_priv[i] = (uint8_t)(32 + i);
    }
    for (int i = 0; i < 16; i++) {
        card_nonce[i] = (uint8_t)(0xa0 + i);
        srv_nonce[i] = (uint8_t)(0xb0 + i);
    }
    id[0] = 0xde; id[1] = 0xad; id[2] = 0xbe; id[3] = 0xef;
}

static void test_pairing_both_sides_agree(void)
{
    uint8_t card_priv[32], srv_priv[32], nc[16], ns[16], id[4];
    vector_keys(card_priv, srv_priv, nc, ns, id);
    uint8_t card_pub[32], srv_pub[32];
    x25519_public(card_pub, card_priv);
    x25519_public(srv_pub, srv_priv);

    // The server commits to its nonce before it sees the card's.
    uint8_t commit[PAIR_COMMIT_SIZE];
    pair_commitment(card_pub, srv_pub, ns, commit);
    CHECK(pair_commitment_kept(card_pub, srv_pub, ns, sizeof(ns), commit),
          "the server's own nonce does not open its commitment");
    uint8_t other[16];
    memcpy(other, ns, 16);
    other[15] ^= 1;
    CHECK(!pair_commitment_kept(card_pub, srv_pub, other, sizeof(other), commit),
          "a different nonce opened the commitment");
    CHECK(!pair_commitment_kept(card_pub, srv_pub, ns, sizeof(ns) - 1, commit),
          "a short nonce opened the commitment");
    CHECK(!pair_commitment_kept(srv_pub, card_pub, ns, sizeof(ns), commit),
          "the commitment does not bind the keys");

    // Both halves, each from its own side of the exchange.
    uint8_t srv_shared[32], card_shared[32];
    x25519(srv_shared, srv_priv, card_pub);
    x25519(card_shared, card_priv, srv_pub);
    uint8_t srv_confirm[32], srv_key[16], confirm[32], key[16];
    CHECK(pair_derive(srv_shared, nc, ns, id, card_pub, srv_pub, srv_confirm, srv_key),
          "server derive refused");
    CHECK(pair_derive(card_shared, nc, ns, id, card_pub, srv_pub, confirm, key),
          "card derive refused");
    CHECK(memcmp(key, srv_key, 16) == 0, "the two device keys differ");
    CHECK(memcmp(confirm, srv_confirm, 32) == 0, "the two confirm keys differ");

    const uint32_t code = pair_code(card_pub, srv_pub, nc, ns);
    CHECK(code < 1000000u, "the code is more than six digits");
    CHECK(code == pair_code(card_pub, srv_pub, nc, ns), "the code is not stable");
    CHECK(code != pair_code(card_pub, srv_pub, ns, nc), "the nonces' order is lost");

    uint8_t card_mac[32], srv_mac[32];
    pair_accept_mac(confirm, PAIR_ROLE_CARD, card_pub, srv_pub, card_mac);
    pair_accept_mac(srv_confirm, PAIR_ROLE_SERVER, card_pub, srv_pub, srv_mac);
    CHECK(pair_server_done(confirm, card_pub, srv_pub, srv_mac, 32),
          "the server's acceptance was rejected");
    CHECK(!ct_equal(srv_mac, card_mac, 32), "the two roles' MACs must differ");
    CHECK(!pair_server_done(confirm, card_pub, srv_pub, card_mac, 32),
          "the card's own MAC echoed back must not prove the server");
    CHECK(!pair_server_done(confirm, card_pub, srv_pub, srv_mac, 31),
          "a short proof must be rejected");

    uint8_t all[32 + 32 + 32 + 4 + 16 + 32 + 32];
    size_t n = 0;
    memcpy(all + n, card_pub, 32); n += 32;
    memcpy(all + n, srv_pub, 32); n += 32;
    memcpy(all + n, commit, 32); n += 32;
    all[n++] = (uint8_t)(code >> 24);
    all[n++] = (uint8_t)(code >> 16);
    all[n++] = (uint8_t)(code >> 8);
    all[n++] = (uint8_t)code;
    memcpy(all + n, srv_key, 16); n += 16;
    memcpy(all + n, card_mac, 32); n += 32;
    memcpy(all + n, srv_mac, 32); n += 32;
    char line[2 * sizeof(all) + 1];
    tohex(all, n, line);
    printf("INTEROP pair %s\n", line);
}

// Someone in the middle runs one exchange with each side.  Each half is
// sound; what gives it away is that the codes differ.
static void test_pairing_in_the_middle(void)
{
    uint8_t card_priv[32], srv_priv[32], nc[16], ns[16], id[4];
    vector_keys(card_priv, srv_priv, nc, ns, id);
    uint8_t card_pub[32], srv_pub[32], mitm_priv[32], mitm_pub[32];
    x25519_public(card_pub, card_priv);
    x25519_public(srv_pub, srv_priv);
    for (int i = 0; i < 32; i++) mitm_priv[i] = (uint8_t)(0x40 + 3 * i);
    x25519_public(mitm_pub, mitm_priv);

    uint8_t mitm_ns[16], mitm_nc[16];
    for (int i = 0; i < 16; i++) {
        mitm_ns[i] = (uint8_t)(0xc0 + i);
        mitm_nc[i] = (uint8_t)(0xd0 + i);
    }
    const uint32_t card_sees = pair_code(card_pub, mitm_pub, nc, mitm_ns);
    const uint32_t server_sees = pair_code(mitm_pub, srv_pub, mitm_nc, ns);
    CHECK(card_sees != server_sees, "the two exchanges showed the same code");

    uint8_t a_shared[32], b_shared[32], ca[32], ka[16], cb[32], kb[16];
    x25519(a_shared, card_priv, mitm_pub);
    x25519(b_shared, srv_priv, mitm_pub);
    CHECK(pair_derive(a_shared, nc, mitm_ns, id, card_pub, mitm_pub, ca, ka), "derive");
    CHECK(pair_derive(b_shared, mitm_nc, ns, id, mitm_pub, srv_pub, cb, kb), "derive");
    CHECK(memcmp(ka, kb, 16) != 0, "the two halves share a key");
}

static void test_pairing_refusals(void)
{
    uint8_t card_priv[32], srv_priv[32], nc[16], ns[16], id[4];
    vector_keys(card_priv, srv_priv, nc, ns, id);
    uint8_t card_pub[32], confirm[32] = {0}, key[16] = {0};
    x25519_public(card_pub, card_priv);

    // A low-order server key makes the shared secret all zero.
    uint8_t zero[32] = {0}, shared[32];
    x25519(shared, card_priv, zero);
    CHECK(!pair_derive(shared, nc, ns, id, card_pub, zero, confirm, key),
          "a low-order server key must be refused");
    static const uint8_t untouched[16] = {0};
    CHECK(memcmp(key, untouched, 16) == 0, "a refused derive wrote a key");

    uint8_t commit[32];
    pair_commitment(card_pub, zero, ns, commit);
    CHECK(!pair_commitment_kept(card_pub, zero, NULL, 16, commit), "no nonce");
    CHECK(!pair_server_done(confirm, card_pub, zero, NULL, 32), "no proof");
}

static void test_entropy_pool(void)
{
    entropy_t a, b;
    entropy_init(&a);
    entropy_init(&b);
    entropy_add_u32(&a, 1);
    entropy_add_u32(&b, 2);
    uint8_t x[32], y[32], z[32];
    entropy_take(&a, x);
    entropy_take(&b, y);
    CHECK(memcmp(x, y, 32) != 0, "different input, same output");
    entropy_take(&a, z);
    CHECK(memcmp(x, z, 32) != 0, "two takes returned the same bytes");
    CHECK(a.samples == 1 && a.taken == 2, "counters wrong");

    entropy_t c;
    entropy_init(&c);
    entropy_add_u32(&c, 1);
    uint8_t w[32];
    entropy_take(&c, w);
    CHECK(memcmp(w, x, 32) == 0, "the pool must be a function of its inputs");
}

// --------------------------------------------------------------- the hello ---

static void test_hello_payload(void)
{
    char out[PROTO_HELLO_MAX + 1];
    int n = proto_hello_payload(out, sizeof(out), NULL, NULL, NULL, 0);
    CHECK(n == (int)PROTO_HELLO_CAPS_LEN && strcmp(out, PROTO_HELLO_CAPS) == 0,
          "core only: '%s'", out);

    n = proto_hello_payload(out, sizeof(out), "deadbeef", NULL, " ext-a ext-b", 0);
    CHECK(strcmp(out, PROTO_HELLO_CAPS " id=deadbeef ext-a ext-b") == 0,
          "with id and extension: '%s'", out);
    CHECK(n == (int)strlen(out), "length disagrees");
    uint8_t frame[PROTO_HANDSHAKE_BUF_SIZE];
    CHECK(proto_encode(frame, MSG_HELLO, (const uint8_t *)out, (uint16_t)n) <=
              sizeof(frame), "HELLO with id overflows the handshake buffer");

    // The camera token sits with the core tokens, before the id.
    n = proto_hello_payload(out, sizeof(out), NULL, NULL, NULL, 1);
    CHECK(strcmp(out, PROTO_HELLO_CAPS " camera") == 0, "with camera: '%s'", out);

    // The card nonce follows the id, as 32 lowercase hex digits.
    uint8_t cn[PROTO_CARD_NONCE_BYTES];
    for (int i = 0; i < PROTO_CARD_NONCE_BYTES; i++) cn[i] = (uint8_t)(0x11 * i);
    n = proto_hello_payload(out, sizeof(out), "deadbeef", cn, NULL, 0);
    CHECK(strcmp(out, PROTO_HELLO_CAPS
                 " id=deadbeef cn=00112233445566778899aabbccddeeff") == 0,
          "with id and card nonce: '%s'", out);
    CHECK(strstr(PROTO_HELLO_CAPS, " seal seal3 ") != NULL,
          "the card does not ask for both seal and seal3");

    // The worst case the bounds promise: the camera token, the id, the card
    // nonce and a full extension string.
    char ext[PROTO_HELLO_EXT_MAX + 1];
    memset(ext, 'x', PROTO_HELLO_EXT_MAX);
    ext[0] = ' ';
    ext[PROTO_HELLO_EXT_MAX] = '\0';
    n = proto_hello_payload(out, sizeof(out), "deadbeef", cn, ext, 1);
    CHECK(n == (int)PROTO_HELLO_MAX, "the worst case is not PROTO_HELLO_MAX");
    CHECK(proto_encode(frame, MSG_HELLO, (const uint8_t *)out, (uint16_t)n) <=
              sizeof(frame), "the worst-case HELLO overflows the handshake buffer");

    // Too long an extension string is left out whole, never cut.
    char longer[PROTO_HELLO_EXT_MAX + 2];
    memset(longer, 'y', sizeof(longer) - 1);
    longer[0] = ' ';
    longer[sizeof(longer) - 1] = '\0';
    proto_hello_payload(out, sizeof(out), "deadbeef", NULL, longer, 0);
    CHECK(strchr(out, 'y') == NULL, "an over-long extension was truncated in");

    for (const char *bad = "DEADBEEF"; bad; bad = NULL) {
        proto_hello_payload(out, sizeof(out), bad, NULL, NULL, 0);
        CHECK(strstr(out, " id=") == NULL, "an invalid id was sent");
    }
    proto_hello_payload(out, sizeof(out), "deadbee", NULL, NULL, 0);
    CHECK(strstr(out, " id=") == NULL, "a short id was sent");
    CHECK(proto_hello_payload(out, 4, NULL, NULL, NULL, 0) == -1, "tiny buffer accepted");

    proto_hello_payload(out, sizeof(out), "deadbeef", NULL, NULL, 0);
    char hex[2 * sizeof(out) + 1];
    tohex((const uint8_t *)out, strlen(out), hex);
    printf("INTEROP hello %s\n", hex);
    proto_hello_payload(out, sizeof(out), "deadbeef", NULL, NULL, 1);
    tohex((const uint8_t *)out, strlen(out), hex);
    printf("INTEROP hello-camera %s\n", hex);
    proto_hello_payload(out, sizeof(out), "deadbeef", cn, NULL, 0);
    tohex((const uint8_t *)out, strlen(out), hex);
    printf("INTEROP hello-cn %s\n", hex);
}

// -------------------------------------------------------- the camera wire ---

// Encoders, bounds and refusals, and the frames printed for crosscheck.py to
// rebuild with the server's own encoders.
static void test_camera_encoders(void)
{
    uint8_t out[PROTO_HEADER_SIZE + PROTO_MAX_CARD_PAYLOAD + 16];
    char hexline[2 * sizeof(out) + 1];

    CHECK(proto_encode_camera(out, PROTO_CAMERA_INNER) == PROTO_HEADER_SIZE + 1 &&
              out[3] == MSG_CAMERA && out[6] == 1, "CAMERA inner");
    CHECK(proto_encode_camera(out, 3) == 0 && proto_encode_camera(out, -1) == 0,
          "a state that is not 0/1/2 was encoded");
    tohex(out, proto_encode_camera(out, PROTO_CAMERA_OUTER), hexline);
    printf("INTEROP camera %s\n", hexline);

    // Deterministic pixels, so both sides compute the same frame.
    static uint8_t px[PROTO_PREVIEW_BYTES];
    for (int i = 0; i < PROTO_PREVIEW_BYTES; i++) px[i] = (uint8_t)(i * 31);
    CHECK(proto_encode_camera_frame(out, PROTO_PREVIEW_FORMAT_GRAY4,
                                    PROTO_PREVIEW_FLAG_STILL, 0xAB, px) ==
              PROTO_HEADER_SIZE + 3 + PROTO_PREVIEW_BYTES &&
              out[3] == MSG_CAMERA_FRAME && out[6] == PROTO_PREVIEW_FORMAT_GRAY4,
          "CAMERA_FRAME");
    tohex(out, PROTO_HEADER_SIZE + 3 + PROTO_PREVIEW_BYTES, hexline);
    printf("INTEROP camera-frame %s\n", hexline);
    CHECK(proto_encode_camera_frame(out, PROTO_PREVIEW_FORMAT_GRAY4, 0x02, 0, px) == 0 &&
              proto_encode_camera_frame(out, PROTO_PREVIEW_FORMAT_GRAY4, 0, 0, NULL) == 0 &&
              proto_encode_camera_frame(out, 1, 0, 0, px) == 0,
          "reserved flag bits and a NULL picture were encoded");

    uint8_t data[PROTO_PHOTO_CHUNK_MAX];
    for (int i = 0; i < PROTO_PHOTO_CHUNK_MAX; i++) data[i] = (uint8_t)i;
    CHECK(proto_encode_photo_chunk(out, 1024, data, sizeof(data)) ==
              PROTO_HEADER_SIZE + 4 + PROTO_PHOTO_CHUNK_MAX &&
              out[6] == 0 && out[7] == 0 && out[8] == 4 && out[9] == 0,
          "PHOTO_CHUNK offset");
    tohex(out, proto_encode_photo_chunk(out, 1024, data, sizeof(data)), hexline);
    printf("INTEROP photo-chunk %s\n", hexline);
    CHECK(proto_encode_photo_chunk(out, 0, data, 0) == 0, "an empty chunk");
    CHECK(proto_encode_photo_chunk(out, 0, data, PROTO_PHOTO_CHUNK_MAX + 1) == 0,
          "a chunk bigger than a KB");
    CHECK(proto_encode_photo_chunk(out, PROTO_PHOTO_BYTES_MAX - 4, data, 5) == 0,
          "a chunk running past the picture");
    CHECK(proto_encode_photo_chunk(out, PROTO_PHOTO_BYTES_MAX, data, 1) == 0,
          "an offset past the picture");

    CHECK(proto_encode_photo_end(out, PROTO_PHOTO_FORMAT_FULL,
                                 PROTO_CAMERA_OUTER, 0xCBF43926) ==
              PROTO_HEADER_SIZE + 10 && out[3] == MSG_PHOTO_END &&
              out[6] == PROTO_PHOTO_FORMAT_FULL && out[7] == PROTO_CAMERA_OUTER,
          "PHOTO_END");
    tohex(out, PROTO_HEADER_SIZE + 10, hexline);
    printf("INTEROP photo-end %s\n", hexline);
    CHECK(proto_encode_photo_end(out, PROTO_PHOTO_FORMAT_FULL,
                                 PROTO_CAMERA_OFF, 0) == 0,
          "off is not a camera");
    CHECK(proto_encode_photo_end(out, 0, PROTO_CAMERA_OUTER, 0) == 0 &&
              proto_encode_photo_end(out, 3, PROTO_CAMERA_OUTER, 0) == 0,
          "a format that is not 1 or 2");
    CHECK(proto_encode_photo_end(out, PROTO_PHOTO_FORMAT_SMALL,
                                 PROTO_CAMERA_OUTER, 0) ==
              PROTO_HEADER_SIZE + 10 && out[9] == 0x01 && out[10] == 0xC2 &&
              out[11] == 0,
          "SMALL's total is 115200");

    CHECK(proto_encode_photo_discard(out, 7) == PROTO_HEADER_SIZE + 2 &&
              out[6] == 0 && out[7] == 7, "PHOTO_DISCARD");
    tohex(out, PROTO_HEADER_SIZE + 2, hexline);
    printf("INTEROP photo-discard %s\n", hexline);
    CHECK(proto_encode_photo_discard(out, 0) == 0, "id 0 is not one issued");
}

// The server's own encodings (dsterm/protocol.py, printed once and pinned
// here byte for byte), decoded by the card's decoders.
static void test_camera_decoders_against_the_server(void)
{
    uint8_t f[260];
    int can_save = -1;
    size_t n = sizeof("445301a4000101") / 2;
    unhex("445301a4000101", f, n);
    CHECK(f[3] == MSG_CAMERA_READY &&
              proto_decode_camera_ready(&can_save, f + 6, (uint16_t)(n - 6)) ==
                  PROTO_OK && can_save == 1,
          "the server's READY (can save) did not decode");
    n = sizeof("445301a4000100") / 2;
    unhex("445301a4000100", f, n);
    CHECK(proto_decode_camera_ready(&can_save, f + 6, (uint16_t)(n - 6)) ==
              PROTO_OK && can_save == 0, "READY without save");
    CHECK(proto_decode_camera_ready(&can_save, f + 6, (uint16_t)(n - 7)) !=
              PROTO_OK, "a truncated READY");
    const uint8_t reserved[1] = { 0x03 };
    CHECK(proto_decode_camera_ready(&can_save, reserved, 1) != PROTO_OK,
          "reserved READY bits were accepted");

    uint8_t kind = 9;
    uint32_t value = 0;
    n = sizeof("445301a500050000000007") / 2;
    unhex("445301a500050000000007", f, n);
    CHECK(f[3] == MSG_CAMERA_ACK &&
              proto_decode_camera_ack(&kind, &value, f + 6, (uint16_t)(n - 6)) ==
                  PROTO_OK && kind == PROTO_ACK_PREVIEW && value == 7,
          "the server's preview ack did not decode");
    n = sizeof("445301a500050100070800") / 2;
    unhex("445301a500050100070800", f, n);
    CHECK(proto_decode_camera_ack(&kind, &value, f + 6, (uint16_t)(n - 6)) ==
              PROTO_OK && kind == PROTO_ACK_PHOTO &&
              value == PROTO_PHOTO_BYTES_MAX,
          "the server's photo ack did not decode");
    const uint8_t badkind[5] = { 2, 0, 0, 0, 0 };
    CHECK(proto_decode_camera_ack(&kind, &value, badkind, 5) != PROTO_OK,
          "an ack kind that is not 0 or 1");
    const uint8_t bigseq[5] = { 0, 0, 0, 1, 0 };
    CHECK(proto_decode_camera_ack(&kind, &value, bigseq, 5) != PROTO_OK,
          "a preview ack above a seq byte");
    CHECK(proto_decode_camera_ack(&kind, &value, bigseq, 4) != PROTO_OK,
          "a short ack");

    photo_saved_t saved;
    n = sizeof("445301a60015000007112f702f64732d70686f746f2d312e706e67") / 2;
    unhex("445301a60015000007112f702f64732d70686f746f2d312e706e67", f, n);
    CHECK(f[3] == MSG_PHOTO_SAVED &&
              proto_decode_photo_saved(&saved, f + 6, (uint16_t)(n - 6)) ==
                  PROTO_OK && saved.status == PROTO_SAVED_OK && saved.id == 7 &&
              strcmp(saved.path, "/p/ds-photo-1.png") == 0 &&
              saved.path_len == 17,
          "the server's PHOTO_SAVED did not decode");
    n = sizeof("445301a6000403000000") / 2;
    unhex("445301a6000403000000", f, n);
    CHECK(proto_decode_photo_saved(&saved, f + 6, (uint16_t)(n - 6)) == PROTO_OK &&
              saved.status == PROTO_SAVED_DISK && saved.id == 0 &&
              saved.path_len == 0,
          "the server's disk-error PHOTO_SAVED did not decode");
    const uint8_t badstatus[4] = { 6, 0, 0, 0 };
    CHECK(proto_decode_photo_saved(&saved, badstatus, 4) != PROTO_OK,
          "a status past the range");
    const uint8_t zeroid[5] = { 0, 0, 0, 1, 'x' };
    CHECK(proto_decode_photo_saved(&saved, zeroid, 5) != PROTO_OK,
          "a saved photo with id 0");
    const uint8_t nopath[4] = { 0, 0, 7, 0 };
    CHECK(proto_decode_photo_saved(&saved, nopath, 4) != PROTO_OK,
          "a saved photo with no path");
    const uint8_t ctrl[5] = { 0, 0, 7, 1, 0x01 };
    CHECK(proto_decode_photo_saved(&saved, ctrl, 5) != PROTO_OK,
          "a path that is not printable");
    const uint8_t ghostid[4] = { 1, 0, 7, 0 };
    CHECK(proto_decode_photo_saved(&saved, ghostid, 4) != PROTO_OK,
          "an unsaved photo claimed an id");
    const uint8_t shortlen[4] = { 0, 0, 7, 2 };
    CHECK(proto_decode_photo_saved(&saved, shortlen, 4) != PROTO_OK,
          "a declared length that does not match the payload");
}

// --------------------------------------------------------------- profiles ---

static void test_addresses(void)
{
    CHECK(addr_valid("192.168.0.10"), "plain");
    CHECK(addr_valid("0.0.0.0") && addr_valid("255.255.255.255"), "ends");
    CHECK(!addr_valid("256.1.1.1"), "octet over 255");
    CHECK(!addr_valid("1.2.3"), "three parts");
    CHECK(!addr_valid("1.2.3.4.5"), "five parts");
    CHECK(!addr_valid("1.2.3.4 "), "trailing space");
    CHECK(!addr_valid("1..3.4") && !addr_valid("") && !addr_valid(NULL), "empty parts");
    CHECK(!addr_valid("1234.1.1.1"), "four digits");

    char host[CFG_ADDR_MAX];
    uint16_t port = 0;
    CHECK(host_port_parse("10.0.0.2", host, &port, 8347) &&
          strcmp(host, "10.0.0.2") == 0 && port == 8347, "default port");
    CHECK(host_port_parse("10.0.0.2:9000", host, &port, 8347) && port == 9000,
          "explicit port");
    CHECK(!host_port_parse("10.0.0.2:0", host, &port, 8347), "port 0");
    CHECK(!host_port_parse("10.0.0.2:65536", host, &port, 8347), "port too big");
    CHECK(!host_port_parse("10.0.0.2:", host, &port, 8347), "empty port");
    CHECK(!host_port_parse("10.0.0.2:8a", host, &port, 8347), "port with a letter");
    CHECK(!host_port_parse("host:8347", host, &port, 8347), "names are not addresses");

    char name[PROFILE_NAME_MAX];
    profile_name_from("wsl-desktop", name);
    CHECK(strcmp(name, "wsl-desktop") == 0, "a good name changed");
    profile_name_from("../../etc/passwd", name);
    CHECK(strcmp(name, "....etcpasswd") == 0, "slashes survived: %s", name);
    profile_name_from("..", name);
    CHECK(strcmp(name, "server") == 0, "a dots-only name must not become ..");
    profile_name_from("a very long name that goes on", name);
    CHECK(strlen(name) == PROFILE_NAME_MAX - 1, "not clipped: %s", name);
    profile_name_from("", name);
    CHECK(strcmp(name, "server") == 0, "empty name");

    uint8_t b[4] = {0xde, 0xad, 0x00, 0x0f}, back[4];
    char id[CFG_DEVICE_ID_MAX];
    device_id_from_bytes(b, id);
    CHECK(strcmp(id, "dead000f") == 0, "device id: %s", id);
    CHECK(device_id_to_bytes(id, back) && memcmp(b, back, 4) == 0, "round trip");
    CHECK(!device_id_valid("DEAD000F") && !device_id_valid("dead000") &&
          !device_id_valid("dead000f0"), "ids are exactly 8 lowercase hex");
}

static void test_profile_formats(void)
{
    wifi_profile_t w, w2;
    char err[64], text[PROFILE_FILE_MAX];
    CHECK(wifi_profile_parse("ssid = Example Network\nwifi_key = passphrase1\n"
                             "ip = dhcp\n", &w, err, sizeof(err)), "%s", err);
    CHECK(w.dhcp && strcmp(w.ssid, "Example Network") == 0, "dhcp profile");
    CHECK(wifi_profile_format(&w, text, sizeof(text)) > 0, "format");
    CHECK(wifi_profile_parse(text, &w2, err, sizeof(err)) &&
          memcmp(&w, &w2, sizeof(w)) == 0, "dhcp round trip");

    CHECK(wifi_profile_parse("ssid=Lab\nwifi_key=\nip=192.0.2.60\n"
                             "gateway=192.0.2.1\n", &w, err, sizeof(err)), "%s", err);
    CHECK(!w.dhcp && strcmp(w.netmask, "255.255.255.0") == 0, "static defaults");
    CHECK(wifi_profile_format(&w, text, sizeof(text)) > 0 &&
          wifi_profile_parse(text, &w2, err, sizeof(err)) &&
          memcmp(&w, &w2, sizeof(w)) == 0, "static round trip");

    CHECK(!wifi_profile_parse("wifi_key=passphrase1\n", &w, err, sizeof(err)) &&
          strstr(err, "ssid"), "missing ssid: %s", err);
    CHECK(!wifi_profile_parse("ssid=x\nwifi_key=short\n", &w, err, sizeof(err)),
          "a 5-character passphrase");
    CHECK(!wifi_profile_parse("ssid=x\nip=192.0.2.1\ngateway=nope\n", &w, err,
                              sizeof(err)), "a bad gateway");
    CHECK(!wifi_profile_parse("ssid=x\njunk line\n", &w, err, sizeof(err)),
          "a line without =");

    server_profile_t s, s2;
    CHECK(server_profile_parse("name=wsl-desktop\nhost=192.0.2.10\nport=8347\n"
                               "psk=00112233445566778899aabbccddeeff\n",
                               &s, err, sizeof(err)), "%s", err);
    CHECK(server_profile_format(&s, text, sizeof(text)) > 0 &&
          server_profile_parse(text, &s2, err, sizeof(err)) &&
          memcmp(&s, &s2, sizeof(s)) == 0, "server round trip");
    CHECK(!server_profile_parse("host=192.0.2.10\n", &s, err, sizeof(err)) &&
          strstr(err, "psk"), "missing psk");
    CHECK(!server_profile_parse("host=example.com\npsk=00112233445566778899aabbccddeeff\n",
                                &s, err, sizeof(err)), "a hostname");
    CHECK(server_profile_parse("host=192.0.2.10\npsk=0011 2233 4455 6677 8899 aabb ccdd eeff\n",
                               &s, err, sizeof(err)) &&
          strcmp(s.name, "192.0.2.10") == 0 && s.port == 8347,
          "defaults: name from host, port 8347");

    // The legacy file splits into the same two profiles.
    dsterm_config_t legacy;
    CHECK(config_parse("ssid=Example Network\nwifi_key=passphrase1\nip=192.0.2.60\n"
                       "gateway=192.0.2.1\nserver=192.0.2.10\nport=9000\n"
                       "psk=00112233445566778899aabbccddeeff\n",
                       &legacy, err, sizeof(err)), "%s", err);
    profiles_from_legacy(&legacy, &w, &s);
    CHECK(!w.dhcp && strcmp(w.ip, "192.0.2.60") == 0 && s.port == 9000 &&
          strcmp(s.name, "192.0.2.10") == 0 && s.psk[15] == 0xff, "migration");

    dsterm_config_t out;
    profiles_resolve(&w, &s, "deadbeef", &out);
    CHECK(strcmp(out.server, "192.0.2.10") == 0 && out.port == 9000 &&
          out.have_psk && strcmp(out.device_id, "deadbeef") == 0 &&
          strcmp(out.ip, "192.0.2.60") == 0, "resolve");
    w.dhcp = 1;
    profiles_resolve(&w, &s, "BAD", &out);
    CHECK(out.ip[0] == '\0' && out.device_id[0] == '\0',
          "dhcp leaves ip empty; a bad id is not carried");
}

static int count_files(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') n++;
    }
    closedir(d);
    return n;
}

static void remove_tree(const char *root)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/servers", root);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char f[sizeof(path) + 1 + sizeof(e->d_name)];
            snprintf(f, sizeof(f), "%s/%s", path, e->d_name);
            remove(f);
        }
        closedir(d);
        rmdir(path);
    }
    const char *files[] = {"wifi.cfg", "wifi.cfg.tmp", "last", "id", "probe"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i]);
        remove(path);
    }
    rmdir(root);
}

static void test_profiles_find_moved(void)
{
    profiles_t p;
    memset(&p, 0, sizeof(p));
    p.count = 2;
    snprintf(p.servers[0].name, sizeof(p.servers[0].name), "StudyPC");
    snprintf(p.servers[0].host, sizeof(p.servers[0].host), "192.168.0.30");
    p.servers[0].port = 8347;
    snprintf(p.servers[1].name, sizeof(p.servers[1].name), "192.168.0.20");
    snprintf(p.servers[1].host, sizeof(p.servers[1].host), "192.168.0.20");
    p.servers[1].port = 8347;

    // The router moved it: same name (any case), same port, a new address.
    CHECK(profiles_find_moved(&p, "studypc", "192.168.0.31", 8347) == 0,
          "a saved computer at a new address was not recognised");
    // Where it is saved, it has not moved.
    CHECK(profiles_find_moved(&p, "StudyPC", "192.168.0.30", 8347) == -1,
          "a computer at its saved address counted as moved");
    // Another port is another server.
    CHECK(profiles_find_moved(&p, "StudyPC", "192.168.0.31", 9000) == -1,
          "a different port counted as the same server");
    // An address something else is saved at is not up for grabs.
    CHECK(profiles_find_moved(&p, "StudyPC", "192.168.0.20", 8347) == -1,
          "a saved computer's address was taken for a moved one");
    CHECK(profiles_find_moved(&p, "", "192.168.0.31", 8347) == -1 &&
              profiles_find_moved(&p, "Other", "192.168.0.31", 8347) == -1,
          "a computer with no or another name counted as moved");

    CHECK(profile_name_is_address("192.168.0.20") && !profile_name_is_address("StudyPC") &&
              !profile_name_is_address("") && !profile_name_is_address("DESKTOP-ABC1234"),
          "address-like names are not told apart from hostnames");
}

static void test_profile_store(void)
{
    // Not mkdtemp: macOS hides it under strict C11 feature macros and glibc
    // hides it without them.  A per-process name is enough for a test.
    char base[64];
    snprintf(base, sizeof(base), "/tmp/dsterm-profiles-%ld", (long)getpid());
    remove_tree(base);
    if (mkdir(base, 0700) != 0) {
        CHECK(0, "cannot create %s", base);
        return;
    }
    char root[sizeof(base) + 8];
    snprintf(root, sizeof(root), "%s/dsterm", base);

    profiles_t p;
    CHECK(profiles_load(root, &p) && !p.have_wifi && p.count == 0,
          "a fresh card has nothing, and that is not an error");

    wifi_profile_t w;
    char err[64];
    wifi_profile_parse("ssid=Example Network\nwifi_key=passphrase1\n", &w, err, sizeof(err));
    CHECK(profiles_save_wifi(root, &w, err, sizeof(err)), "%s", err);

    server_profile_t s;
    server_profile_parse("name=desk\nhost=192.0.2.10\n"
                         "psk=00112233445566778899aabbccddeeff\n", &s, err, sizeof(err));
    CHECK(profiles_save_server(root, &s, err, sizeof(err)), "%s", err);
    strcpy(s.name, "laptop");
    strcpy(s.host, "192.0.2.11");
    CHECK(profiles_save_server(root, &s, err, sizeof(err)), "%s", err);
    CHECK(profiles_save_last(root, "laptop"), "last");
    CHECK(profiles_save_device_id(root, "0a1b2c3d"), "id");
    CHECK(!profiles_save_device_id(root, "nothex!!"), "a bad id was saved");

    CHECK(profiles_load(root, &p), "load");
    CHECK(p.have_wifi && strcmp(p.wifi.ssid, "Example Network") == 0, "wifi");
    CHECK(p.count == 2, "two servers, got %u", p.count);
    CHECK(strcmp(p.last, "laptop") == 0, "last: %s", p.last);
    CHECK(strcmp(p.device_id, "0a1b2c3d") == 0, "id: %s", p.device_id);
    CHECK(profiles_find(&p, "desk") >= 0 && profiles_find(&p, "nope") < 0, "find");
    CHECK(profiles_find_host(&p, "192.0.2.11", 8347) >= 0 &&
          profiles_find_host(&p, "192.0.2.11", 9) < 0, "find by host");

    // Saving under the same name replaces, rather than adding a second.
    s.port = 9000;
    CHECK(profiles_save_server(root, &s, err, sizeof(err)), "%s", err);
    profiles_load(root, &p);
    CHECK(p.count == 2 && p.servers[profiles_find(&p, "laptop")].port == 9000,
          "replace by name");

    // A broken file is skipped and counted, never fatal.
    char path[256];
    snprintf(path, sizeof(path), "%s/servers/broken.cfg", root);
    FILE *fh = fopen(path, "wb");
    if (fh) { fputs("host=nope\n", fh); fclose(fh); }
    profiles_load(root, &p);
    CHECK(p.count == 2 && p.skipped == 1, "broken file: %u/%u", p.count, p.skipped);
    remove(path);

    CHECK(profiles_forget_server(root, "desk"), "forget");
    profiles_load(root, &p);
    CHECK(p.count == 1 && profiles_find(&p, "desk") < 0, "forgotten");
    snprintf(path, sizeof(path), "%s/servers", root);
    CHECK(count_files(path) == 1, "forget left a file behind");

    // A save interrupted between remove and rename leaves only the .tmp; the
    // next read must recover it rather than lose the Wi-Fi settings.
    char real[256], tmp[260];
    snprintf(real, sizeof(real), "%s/wifi.cfg", root);
    snprintf(tmp, sizeof(tmp), "%s.tmp", real);
    CHECK(rename(real, tmp) == 0, "could not stage the interrupted save");
    profiles_load(root, &p);
    CHECK(p.have_wifi, "the .tmp was not recovered");

    // The cap: a ninth server is skipped, not written past the array.
    for (int i = 0; i < PROFILES_MAX + 1; i++) {
        snprintf(s.name, sizeof(s.name), "s%d", i);
        profiles_save_server(root, &s, err, sizeof(err));
    }
    profiles_load(root, &p);
    CHECK(p.count == PROFILES_MAX && p.skipped >= 1, "cap: %u/%u", p.count, p.skipped);

    remove_tree(root);
    rmdir(base);
}

// -------------------------------------------------------------- discovery ---

static void test_discovery_datagrams(void)
{
    char probe[DISCOVER_DATAGRAM_MAX];
    CHECK(discover_probe(probe, sizeof(probe), "deadbeef") > 0 &&
          strcmp(probe, "DSTM probe 1 id=deadbeef") == 0, "probe: %s", probe);
    CHECK(discover_probe(probe, sizeof(probe), NULL) > 0 &&
          strcmp(probe, "DSTM probe 1") == 0, "probe without id");
    discover_probe(probe, sizeof(probe), "deadbeef");
    char hex[2 * DISCOVER_DATAGRAM_MAX + 1];
    tohex((const uint8_t *)probe, strlen(probe), hex);
    printf("INTEROP probe %s\n", hex);

    discover_entry_t e;
    // Exactly the server's encode_answer() output (tests/test_discovery.py).
    const char *answer = "DSTM here 1 name=host-1 port=8347 pair=0";
    CHECK(discover_parse_answer((const uint8_t *)answer, strlen(answer), &e) &&
          strcmp(e.name, "host-1") == 0 && e.port == 8347 && !e.pairing,
          "the server's answer");
    answer = "DSTM here 1 name=x port=9000 pair=1";
    CHECK(discover_parse_answer((const uint8_t *)answer, strlen(answer), &e) &&
          e.pairing && e.port == 9000, "pairing open");

    const char *bad[] = {
        "DSTM here 1 port=8347 pair=0",          // no name
        "DSTM here 1 name=x port=0 pair=0",      // port 0
        "DSTM here 1 name=x port=70000 pair=0",  // port too big
        "DSTM here 1 name=x port=12ab pair=0",   // junk port
        "DSTM probe 1",                          // a probe, not an answer
        "XXXX here 1 name=x port=1 pair=0",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!discover_parse_answer((const uint8_t *)bad[i], strlen(bad[i]), &e),
              "accepted: %s", bad[i]);
    }
    const uint8_t control[] = "DSTM here 1 name=a\x1b[2J port=1 pair=0";
    CHECK(!discover_parse_answer(control, sizeof(control) - 1, &e),
          "an escape sequence in an answer must be refused");
    uint8_t huge[DISCOVER_DATAGRAM_MAX + 1];
    memset(huge, 'a', sizeof(huge));
    CHECK(!discover_parse_answer(huge, sizeof(huge), &e), "oversized datagram");
    answer = "DSTM here 1 name=../../x;rm port=1 pair=0";
    CHECK(discover_parse_answer((const uint8_t *)answer, strlen(answer), &e) &&
          strcmp(e.name, "....xrm") == 0, "a hostile name is reduced: %s", e.name);
}

static void test_discovery_list(void)
{
    discover_list_t l;
    discover_list_init(&l);
    discover_entry_t e = {0};
    strcpy(e.host, "192.0.2.10");
    e.port = 8347;
    e.swept = 1;
    CHECK(discover_list_add(&l, &e) && l.count == 1, "add swept");
    strcpy(e.name, "desk");
    e.swept = 0;
    CHECK(discover_list_add(&l, &e) && l.count == 1 &&
          strcmp(l.entries[0].name, "desk") == 0, "a broadcast answer upgrades");
    e.swept = 1;
    e.name[0] = '\0';
    discover_list_add(&l, &e);
    CHECK(strcmp(l.entries[0].name, "desk") == 0, "a sweep must not downgrade");
    for (int i = 0; i < DISCOVER_MAX + 2; i++) {
        snprintf(e.host, sizeof(e.host), "192.0.2.%u", (unsigned)(uint8_t)(100 + i));
        discover_list_add(&l, &e);
    }
    CHECK(l.count == DISCOVER_MAX, "list overran: %u", l.count);
}

static void test_discovery_sweep(void)
{
    uint32_t self, mask, a;
    char text[CFG_ADDR_MAX];
    CHECK(ipv4_parse("192.0.2.60", &self) && self == 0xC000023Cu, "parse");
    ipv4_format(self, text);
    CHECK(strcmp(text, "192.0.2.60") == 0, "format: %s", text);

    ipv4_parse("255.255.255.0", &mask);
    discover_sweep_t s;
    CHECK(discover_sweep_init(&s, self, mask), "init /24");
    CHECK(s.total == 253, "a /24 has 253 peers, got %u", s.total);
    unsigned n = 0;
    uint32_t first = 0, last = 0;
    int saw_self = 0;
    while (discover_sweep_next(&s, &a)) {
        if (n == 0) first = a;
        last = a;
        if (a == self) saw_self = 1;
        n++;
    }
    CHECK(n == 253 && first == 0xC0000201u && last == 0xC00002FEu && !saw_self,
          "sweep: %u from %08x to %08x", n, (unsigned)first, (unsigned)last);
    CHECK(s.done == 253, "progress count");

    ipv4_parse("255.255.0.0", &mask);
    discover_sweep_init(&s, self, mask);
    CHECK(s.total == 253, "a /16 is narrowed to the /24");

    // 192.0.2.61 in 192.0.2.60/30: hosts .61 and .62, so one peer.
    ipv4_parse("255.255.255.252", &mask);
    CHECK(discover_sweep_init(&s, self + 1, mask), "/30");
    CHECK(s.total == 1, "a /30 has one peer, got %u", s.total);

    ipv4_parse("255.255.255.255", &mask);
    CHECK(!discover_sweep_init(&s, self, mask), "/32 has nobody to find");
    ipv4_parse("255.0.255.0", &mask);
    CHECK(discover_sweep_init(&s, self, mask) && s.total == 253,
          "a non-contiguous mask falls back to /24");
}

// ---------------------------------------------------------------- boot UI ---

static void type(field_t *f, const char *s)
{
    for (; *s; s++) field_input(f, (uint8_t)*s);
}

static void test_fields(void)
{
    field_t f;
    field_init(&f, FIELD_ADDRESS, NULL);
    type(&f, "19x2.0.2.1g0");
    CHECK(strcmp(f.buf, "192.0.2.10") == 0, "address filtering: %s", f.buf);
    CHECK(field_input(&f, '\r') == FIELD_DONE, "a complete address");
    for (int i = 0; i < 20; i++) field_input(&f, 0x7f);
    CHECK(field_input(&f, '\r') == FIELD_INCOMPLETE, "an empty address is not done");
    CHECK(field_input(&f, 0x1b) == FIELD_CANCEL, "escape cancels");

    field_init(&f, FIELD_ADDRESS, "192.0.2.10");
    CHECK(field_complete(&f), "initial address");
    type(&f, "x:9000");
    CHECK(strcmp(f.buf, "192.0.2.10:9000") == 0 && field_complete(&f),
          "address with port: %s", f.buf);
    for (int i = 0; i < 40; i++) field_input(&f, 0x7f);
    CHECK(f.len == 0 && field_input(&f, 0x7f) == FIELD_IGNORED, "delete past empty");

    field_init(&f, FIELD_PASSPHRASE, NULL);
    CHECK(field_complete(&f), "an empty passphrase is an open network");
    type(&f, "short");
    CHECK(!field_complete(&f), "5 characters");
    type(&f, " pass");
    CHECK(field_complete(&f), "10 characters with a space");
    for (int i = 0; i < 100; i++) field_input(&f, 'z');
    CHECK(f.len == 63, "passphrase cap: %u", f.len);
    char shown[BOOTUI_COLS + 1];
    field_display(&f, shown);
    CHECK(strlen(shown) == BOOTUI_COLS && shown[BOOTUI_COLS - 1] == '_' &&
          shown[BOOTUI_COLS - 2] == 'z', "a long field shows its tail");

    field_init(&f, FIELD_KEY, NULL);
    type(&f, "0011 2233 4455 6677 8899 aabb ccdd eeff");
    CHECK(field_complete(&f), "grouped key: %s", f.buf);
    field_input(&f, 0x7f);
    CHECK(!field_complete(&f), "31 hex digits");
    field_init(&f, FIELD_TEXT, NULL);
    CHECK(!field_complete(&f), "an SSID cannot be empty");
    CHECK(field_input(&f, 0x80) == FIELD_IGNORED && field_input(&f, 0x07) == FIELD_IGNORED,
          "non-printable bytes");
    for (int k = FIELD_TEXT; k <= FIELD_KEY; k++) {
        CHECK(strlen(field_prompt((field_kind_t)k)) <= BOOTUI_COLS, "prompt %d too wide", k);
    }
}

static void test_lists_and_countdown(void)
{
    menu_list_t l = {0};
    menu_list_set(&l, 30);
    menu_list_move(&l, -5, 10);
    CHECK(l.selected == 0, "clamped at the top");
    menu_list_move(&l, 12, 10);
    CHECK(l.selected == 12 && l.top == 3, "window follows: sel %u top %u",
          l.selected, l.top);
    menu_list_move(&l, 100, 10);
    CHECK(l.selected == 29 && l.top == 20, "bottom: %u %u", l.selected, l.top);
    menu_list_move(&l, -29, 10);
    CHECK(l.selected == 0 && l.top == 0, "back to the top");
    menu_list_set(&l, 3);
    CHECK(l.selected == 0, "shrink");
    l.selected = 2;
    menu_list_set(&l, 1);
    CHECK(l.selected == 0, "selection clamped on shrink");
    menu_list_set(&l, 0);
    menu_list_move(&l, 1, 10);
    CHECK(l.selected == 0, "empty list");

    countdown_t c;
    countdown_start(&c, 180);
    CHECK(countdown_seconds(&c) == 3, "3 s");
    int fired = 0;
    for (int i = 0; i < 179; i++) fired |= countdown_tick(&c);
    CHECK(!fired && countdown_seconds(&c) == 1, "not yet");
    CHECK(countdown_tick(&c) == 1, "fires on the last frame");
    CHECK(countdown_tick(&c) == 0 && !c.active, "fires once");
    countdown_start(&c, 180);
    countdown_cancel(&c);
    CHECK(countdown_tick(&c) == 0 && countdown_seconds(&c) == 0, "cancelled");
}

static void test_main_menu_rows(void)
{
    profiles_t p;
    memset(&p, 0, sizeof(p));
    boot_row_t rows[BOOT_MAIN_ROWS];
    char labels[BOOT_MAIN_ROWS][BOOTUI_COLS + 1];

    unsigned n = bootui_main_rows(&p, 3, rows, labels);
    CHECK(n == 4 && rows[0] == BOOT_ROW_FIND && rows[3] == BOOT_ROW_WIFI,
          "a fresh card has no Connect row");
    CHECK(strstr(labels[3], "not set up") != NULL, "wifi row: %s", labels[3]);

    p.have_wifi = 1;
    strcpy(p.wifi.ssid, "Example Network");
    p.wifi.dhcp = 1;
    p.count = 1;
    strcpy(p.servers[0].name, "wsl-desktop");
    strcpy(p.last, "wsl-desktop");
    n = bootui_main_rows(&p, 3, rows, labels);
    CHECK(n == 5 && rows[0] == BOOT_ROW_CONNECT_LAST, "connect row");
    CHECK(strncmp(labels[0], "Connect to wsl-desktop", 22) == 0 &&
          labels[0][strlen(labels[0]) - 1] == ')', "label: %s", labels[0]);
    CHECK(strstr(labels[3], "(1)") != NULL, "saved count: %s", labels[3]);
    for (unsigned i = 0; i < n; i++) {
        CHECK(strlen(labels[i]) <= BOOTUI_COLS, "row %u too wide", i);
    }

    strcpy(p.last, "gone");
    n = bootui_main_rows(&p, 3, rows, labels);
    CHECK(n == 4, "a last server that was forgotten is not offered");

    char row[BOOTUI_COLS + 1];
    bootui_server_row("a-very-long-server-name", "192.168.100.200", 65535, "pair", row);
    CHECK(strlen(row) <= BOOTUI_COLS && strstr(row, "pair") != NULL &&
          strstr(row, "192.168.100.200:65535") != NULL, "server row: %s", row);
}

static void test_access_points(void)
{
    // In DSi mode: dBm, every network compatible.  The row is ssid, rssi,
    // security, compatible, wpa, auth, dsi_mode.
    bootui_ap_t aps[6] = {
        {"Weak", -85, AP_WPA2, 1, 1, 2, 1},
        {"Home", -60, AP_WPA2, 1, 1, 2, 1},
        {"", -30, AP_WPA2, 1, 1, 2, 1},           // hidden
        {"Home", -45, AP_WPA2, 1, 1, 2, 1},       // the same network, closer
        {"Cafe", -50, AP_OPEN, 1, 0, 0, 1},
        {"Old", -55, AP_WEP, 1, 0, 0, 1},
    };
    const unsigned n = bootui_aps_tidy(aps, 6);
    CHECK(n == 4, "tidy count %u", n);
    CHECK(strcmp(aps[0].ssid, "Home") == 0 && aps[0].rssi == -45, "strongest first");
    // WEP cannot be joined, so it sorts below every network that can.
    CHECK(strcmp(aps[3].ssid, "Old") == 0, "an unjoinable network is not last: %s",
          aps[3].ssid);
    CHECK(strcmp(aps[2].ssid, "Weak") == 0, "weakest joinable last among them");
    CHECK(bootui_ap_supported(AP_WPA2) && bootui_ap_supported(AP_OPEN) &&
          !bootui_ap_supported(AP_WEP) && !bootui_ap_supported(AP_WPA),
          "what a DSi can join");

    bootui_ap_t long_name = {"A network with a thirty-two-char", -40, AP_WPA2,
                             1, 1, 2, 1};
    char row[BOOTUI_COLS + 1];
    bootui_ap_row(&long_name, row);
    CHECK(strlen(row) <= BOOTUI_COLS && strstr(row, "WPA2 ####") != NULL,
          "ap row: %s", row);
    bootui_ap_t far = {"x", -95, AP_OPEN, 1, 0, 0, 1};
    bootui_ap_row(&far, row);
    CHECK(strstr(row, "open ....") != NULL, "no bars: %s", row);

    // DS mode: dswifi calls every WPA2 network "WPA", and the flags decide.
    // The verdict must be "needs DSi mode", never "set the router to WPA2".
    bootui_ap_t ds_home = {"Home", 140, AP_WPA, 0, 1, 0, 0};
    CHECK(bootui_ap_verdict(&ds_home) == AP_VERDICT_NEEDS_DSI,
          "a WPA2 router in DS mode is not 'needs DSi mode'");
    bootui_ap_row(&ds_home, row);
    CHECK(strstr(row, "needs DSi mode") != NULL && strstr(row, "#") == NULL,
          "DS-mode row: %s", row);
    bootui_ap_t ds_open = {"Hotspot", 140, AP_OPEN, 1, 0, 0, 0};
    CHECK(bootui_ap_verdict(&ds_open) == AP_VERDICT_OK, "an open network refused");
    bootui_ap_row(&ds_open, row);
    CHECK(strstr(row, "open") != NULL && strstr(row, "#") == NULL &&
              strstr(row, ".") == NULL,
          "DS mode drew bars it cannot measure: %s", row);
    bootui_ap_t g_only = {"Modern", 140, AP_OPEN, 0, 0, 0, 0};
    CHECK(bootui_ap_verdict(&g_only) == AP_VERDICT_NO_11B,
          "an 11g-only router was offered to a DS");
    // DSi mode refusals.
    bootui_ap_t ent = {"Office", -40, AP_WPA2, 1, 1, 1, 1};
    CHECK(bootui_ap_verdict(&ent) == AP_VERDICT_NOT_PSK, "Enterprise accepted");
    // A transition-mode router may report its SAE suite; it is tried, not
    // refused (the DSi happy path must not regress on such a router).
    bootui_ap_t mixed = {"Home", -40, AP_WPA2, 1, 1, 8, 1};
    CHECK(bootui_ap_verdict(&mixed) == AP_VERDICT_OK,
          "a WPA2/WPA3 transition router was refused before trying");
    bootui_ap_t wpa1 = {"Older", -40, AP_WPA, 1, 1, 2, 1};
    CHECK(bootui_ap_verdict(&wpa1) == AP_VERDICT_WPA1, "WPA1 accepted in DSi mode");
    for (int v = AP_VERDICT_NEEDS_DSI; v <= AP_VERDICT_NO_11B; v++) {
        const char *text = bootui_verdict_text((ap_verdict_t)v, "advice");
        CHECK(text && text[0], "verdict %d has no message", v);
    }
}

static void test_console_detection(void)
{
    CHECK(console_classify(0xFF, 0x00, 0, 0) == CONSOLE_DS, "a DS");
    CHECK(console_classify(0x20, 0x00, 0, 0) == CONSOLE_DS_LITE, "a DS Lite");
    CHECK(console_classify(0x63, 0x00, 0, 0) == CONSOLE_DS_LITE, "an iQue DS Lite");
    CHECK(console_classify(0x57, 0x01, 1, 0) == CONSOLE_DSI, "a DSi in DSi mode");
    CHECK(console_classify(0x57, 0x02, 0, 0) == CONSOLE_DSI, "a DSi in DS mode");
    CHECK(console_classify(0x57, 0x03, 0, 0) == CONSOLE_3DS, "a 3DS in DS mode");
    CHECK(console_classify(0x57, 0x00, 1, 1) == CONSOLE_3DS,
          "32 MB in DSi mode is a 3DS");
    CHECK(console_classify(0x00, 0x00, 1, 0) == CONSOLE_DSI,
          "only a DSi-family console can be in DSi mode");
    CHECK(console_classify(0x00, 0x00, 0, 0) == CONSOLE_UNKNOWN, "unknown");

    const console_t ds = { CONSOLE_DS_LITE, 0 }, dsi = { CONSOLE_DSI, 0 },
                    n3ds = { CONSOLE_3DS, 0 };
    CHECK(strstr(console_wpa_advice(&ds), "can't join WPA") != NULL,
          "a DS Lite is told to relaunch in a mode it does not have");
    CHECK(strstr(console_wpa_advice(&dsi), "Unlaunch") != NULL,
          "a DSi in DS mode is not told how to start in DSi mode");
    CHECK(strstr(console_wpa_advice(&n3ds), "TWiLight") != NULL,
          "a 3DS in DS mode is not told about TWiLight Menu++");
    // Every advice line fits the top screen.
    const console_t all[] = { ds, dsi, n3ds, { CONSOLE_DS, 0 },
                              { CONSOLE_UNKNOWN, 0 } };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *texts[2] = { console_wpa_advice(&all[i]),
                                 console_silent_advice(&all[i]) };
        for (int t = 0; t < 2; t++) {
            const char *line = texts[t];
            while (line && *line) {
                const char *end = strchr(line, '\n');
                const size_t len = end ? (size_t)(end - line) : strlen(line);
                CHECK(len <= 40, "console %u advice has a %zu-column line", i, len);
                line = end ? end + 1 : NULL;
            }
        }
    }
}

void test_connect_all(void)
{
    printf("== pairing ==\n");
    test_x25519_rfc7748();
    test_hkdf_rfc5869();
    test_pairing_both_sides_agree();
    test_pairing_in_the_middle();
    test_pairing_refusals();
    test_entropy_pool();
    test_hello_payload();
    test_camera_encoders();
    test_camera_decoders_against_the_server();

    printf("== profiles ==\n");
    test_addresses();
    test_profile_formats();
    test_profile_store();
    test_profiles_find_moved();

    printf("== discovery ==\n");
    test_discovery_datagrams();
    test_discovery_list();
    test_discovery_sweep();

    printf("== boot menu ==\n");
    test_fields();
    test_lists_and_countdown();
    test_main_menu_rows();
    test_console_detection();
    test_access_points();
}
