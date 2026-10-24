#!/usr/bin/env python3
"""Verify the C client and the Python server agree on the wire.

Two independent implementations of the same protocol will diverge eventually.
The C side prints its computed values; this recomputes them with the server's
own code and compares. A mismatch here would show up on hardware as
"authentication rejected" with no further clue.

Usage:  python tools/crosscheck.py < output-of-test_host
"""

from __future__ import annotations

import binascii
import hashlib
import sys

from dsterm.auth import compute_mac
from dsterm.discovery import encode_probe
from dsterm import camera as pycam
from dsterm.protocol import (
    CAMERA_OUTER,
    CLIP_FLAG_TRANSLITERATED,
    CLIP_FLAG_TRUNCATED,
    CLIP_FLAG_WITHHELD,
    PREVIEW_FLAG_STILL,
    PREVIEW_BYTES,
    PROJECT_TAB_NOW,
    MsgType,
    encode,
    encode_camera,
    encode_camera_frame,
    encode_clip_chunk,
    encode_clip_empty,
    encode_photo_chunk,
    encode_photo_discard,
    encode_photo_end,
    encode_project_row,
)

KEY = bytes(range(16))


def pairing_vector() -> str:
    """The fixed exchange test_connect.c prints: both public keys, the server's
    commitment, the six-digit code, the device key and both acceptance MACs. If
    the two implementations disagree on a single byte of X25519, HKDF or the
    labels, the screens show different codes and a card pairs to nothing."""
    from dsterm import pairing, x25519

    card_private = bytes(range(32))
    server_private = bytes(range(32, 64))
    card_nonce = bytes(range(0xA0, 0xB0))
    server_nonce = bytes(range(0xB0, 0xC0))
    device_id = bytes.fromhex("deadbeef")
    card_public = x25519.public_key(card_private)
    server_public = x25519.public_key(server_private)
    shared = x25519.x25519(server_private, card_public)
    confirm, device_key = pairing.derive(shared, card_nonce, server_nonce, device_id,
                                         card_public, server_public)
    code = int(pairing.code(card_public, server_public, card_nonce, server_nonce))
    return (card_public + server_public +
            pairing.commitment(card_public, server_public, server_nonce) +
            code.to_bytes(4, "big") + device_key +
            pairing.accept_mac(confirm, pairing.ROLE_CARD, card_public, server_public) +
            pairing.accept_mac(confirm, pairing.ROLE_SERVER, card_public, server_public)
            ).hex()


def hello_vector(camera: bool = False, card_nonce: bool = False) -> str:
    """The C side prints its HELLO payload with an id; what matters is that the
    server reads the same id and the same features out of it.  With ``camera``
    the DSi variant is pinned: the token sits with the core tokens, before
    the id.  With ``card_nonce``, the nonce every real connection carries
    (test_connect.c's 0x00, 0x11, ... 0xff) must read back as those bytes."""
    from dsterm.protocol import hello_card_nonce, hello_device_id, hello_features

    payload = (b"ds-term/1 mode snippets project clip remote state seal seal3 "
               b"commands mirror ask mic project64 grid=42x24")
    if camera:
        payload += b" camera"
    payload += b" id=deadbeef"
    if card_nonce:
        payload += b" cn=00112233445566778899aabbccddeeff"
    features = hello_features(payload)
    assert hello_device_id(features) == "deadbeef"
    assert {"mode", "snippets", "project", "clip", "remote", "state", "seal",
            "seal3", "commands", "mirror", "ask", "mic", "project64"} <= features
    assert ("camera" in features) == camera
    assert hello_card_nonce(features) == (
        bytes(0x11 * i for i in range(16)) if card_nonce else None)
    return payload.hex()


def answer_vector() -> str:
    """test_core.c's ANSWER, rebuilt with the server's encoder, after the
    server's decoder has read the same id and key out of it."""
    from dsterm.protocol import HEADER_SIZE, decode_answer, encode_answer

    frame = encode_answer(0xC0FFEE01, b"\r")
    assert decode_answer(frame[HEADER_SIZE:]) == (0xC0FFEE01, b"\r")
    return frame.hex()


def chacha_vector() -> str:
    """test_seal.c's cipher run: every awkward length, each from its own
    block counter, hashed together."""
    from dsterm import chacha20

    key = bytes(0xA0 ^ i for i in range(32))
    nonce = bytes(3 * i + 1 for i in range(12))
    h = hashlib.sha256()
    for counter, n in enumerate((0, 1, 63, 64, 65, 536, 1024, 6147)):
        h.update(chacha20.xor(key, nonce, bytes((i * 7 + counter) & 0xFF
                                                for i in range(n)), counter))
    return h.hexdigest()


def seal_big_vector() -> str:
    """test_seal.c's 1000-byte KEY frame, sealed by the card at seq 3 under
    its fixture key: the server's seal_frame must make the same bytes."""
    from dsterm.protocol import MsgType, encode, seal_frame, seal_key

    hello = (b"ds-term/1 mode seal seal3 grid=42x24 id=deadbeef "
             b"cn=00112233445566778899aabbccddeeff")
    k = seal_key(bytes(range(16)), bytes((0xF0 - i) & 0xFF for i in range(32)), hello)
    frame = encode(MsgType.KEY, bytes((i * 13) & 0xFF for i in range(1000)))
    return hashlib.sha256(seal_frame(k, b"C", 3, frame)).hexdigest()


def subscreen_vectors() -> tuple[str, str]:
    """test_core.c's mirror vector, rebuilt with the server's own encoder and
    decoded back: the first and last 64 bytes, as the card prints them."""
    from dsterm.protocol import SUBSCREEN_COLS, SUBSCREEN_ROWS, MIRROR_FLAG_CAMERA, \
        decode_subscreen, encode_subscreen_payload
    cells = [[(0x20, 0, 0x7FFF, 0x0421) for _ in range(SUBSCREEN_COLS)]
             for _ in range(SUBSCREEN_ROWS)]
    cells[0][0] = (ord("D"), 0, 0x7FFF, 0x0421)
    cells[23][41] = (ord("S"), 0, 0x001F, 0x7C00)
    for c in range(SUBSCREEN_COLS):
        g, f, fg, bg = cells[1][c]
        cells[1][c] = (g, f | MIRROR_FLAG_CAMERA, fg, bg)
    payload = encode_subscreen_payload(cells, 0)
    back = decode_subscreen(payload)
    assert back.cells[23][41].char == "S" and back.cells[1][0].camera
    return payload[:64].hex(), payload[-64:].hex()


def pycam_colour() -> str:
    """The 96x72 RGB332 colour view of the synthetic frame."""
    out = bytearray(pycam.COLOUR_BYTES)
    pycam.downscale_colour(pycam.synth_rgb555_frame(), out)
    return bytes(out).hex()


def pycam_preview() -> str:
    """The 128x96 grey preview of the synthetic frame, computed with the
    server's own reference (dsterm/camera.py)."""
    preview = bytearray(pycam.PREVIEW_BYTES)
    pycam.downscale_preview(pycam.synth_rgb555_frame(), preview)
    return bytes(preview).hex()


def _pack_vector(fmt: int) -> str:
    """sha256 of the packed synthetic picture, then its first 64 bytes --
    exactly what test_camera.c prints."""
    import hashlib

    packed = pycam.photo_pack(pycam.synth_yuyv_frame(), fmt)
    return hashlib.sha256(packed).hexdigest() + ":" + packed[:64].hex()


def commands_vector() -> str:
    """test_core.c decodes this payload with quick_commands_apply() and prints
    the names it kept, joined by "|"."""
    from dsterm.protocol import HEADER_SIZE, decode_commands, encode_commands

    payload = encode_commands(["/deploy", "/frontend:test", "/x.y_z-1"])[HEADER_SIZE:]
    assert payload == b"/deploy\n/frontend:test\n/x.y_z-1"
    return "|".join(decode_commands(payload)).encode().hex()


def voice_adpcm_vector() -> str:
    """test_core.c's 16-sample synthetic PCM encoded to 8 bytes of IMA ADPCM,
    then decoded back to 16-bit PCM: proves both encoders and both decoders
    agree bit for bit."""
    import struct
    from dsterm import voice as dsvoice

    pcm = [i * 2000 - 15000 for i in range(16)]
    encoded = dsvoice.adpcm_encode(pcm)
    decoded = dsvoice.adpcm_decode(encoded)
    return encoded.hex() + ":" + struct.pack(f"<{len(decoded)}h", *decoded).hex()


def voice_wire_vector() -> str:
    """VOICE_START, VOICE_CHUNK, and VOICE_END concatenated as hex."""
    from dsterm.protocol import (
        VOICE_FORMAT_IMA_ADPCM_8K,
        encode_voice_chunk,
        encode_voice_end,
        encode_voice_start,
    )

    f1 = encode_voice_start(VOICE_FORMAT_IMA_ADPCM_8K, "es")
    f2 = encode_voice_chunk(480, bytes((i * 17) & 0xFF for i in range(32)))
    f3 = encode_voice_end(512, 0xDEADBEEF)
    return (f1 + f2 + f3).hex()


NONCE = bytes((0xF0 - i) & 0xFF for i in range(32))


def main() -> int:
    got: dict[str, str] = {}
    for line in sys.stdin:
        parts = line.split()
        if len(parts) == 3 and parts[0] == "INTEROP":
            got[parts[1]] = parts[2]

    if not got:
        print("no INTEROP lines on stdin -- run test_host and pipe it here")
        return 2

    expected = {
        "hmac": compute_mac(KEY, NONCE).hex(),
        "frame": encode(MsgType.AUTH, b"xy").hex(),
        # The PROJECT row layout, built by both implementations. The C side
        # lays the bytes out the way its decoder expects to read them; this
        # rebuilds the same row with the server's encoder. A field that moves
        # on one side and not the other shows up here rather than as an
        # unreadable page on a device you have to reflash to fix.
        "project": encode_project_row(0, 2, PROJECT_TAB_NOW, 9,
                                      "branch", "main").hex(),
        # The CLIP chunk, now that test_host.c emits it. The fields here are
        # the ones the two implementations disagreed about while this was being
        # designed, which is exactly why they are the ones worth pinning: a
        # u32 total rather than a saturating u16, and a total that counts the
        # converted text so that the bytes received are always
        # min(total, CLIP_TEXT_MAX).
        #
        # The body carries a newline deliberately. It is the one control byte
        # that survives the server's fold, and a vector made only of letters
        # would not notice a decoder that had quietly stopped allowing it.
        "clip": encode_clip_chunk(
            1, 2, 0x2A,
            CLIP_FLAG_TRUNCATED | CLIP_FLAG_TRANSLITERATED,
            70_000, b"a\nb").hex(),
        # Both empty frames, because the entire reason the empty frame carries
        # a flag byte is that these two must not be the same bytes on the wire.
        "clip-empty": encode_clip_empty().hex(),
        "clip-withheld": encode_clip_empty(CLIP_FLAG_WITHHELD).hex(),
        # Pairing, end to end: X25519, HKDF, both MACs.
        "pair": pairing_vector(),
        # The HELLO a paired card sends, which the server must read the
        # device id out of -- and the DSi variant carrying "camera".
        "hello": hello_vector(),
        "hello-camera": hello_vector(camera=True),
        "hello-cn": hello_vector(card_nonce=True),
        # SEAL v3: the cipher on its own, and a whole sealed frame.
        "chacha": chacha_vector(),
        "seal-big": seal_big_vector(),
        # The camera messages, rebuilt with the server's own encoders against
        # the C side's deterministic input: CAMERA outer, a preview frame of
        # (i*31)&0xFF pixels, one KB chunk at offset 1024 of (i&0xFF), the
        # photo's end with the known CRC of "123456789", and a discard of id 7.
        "camera": encode_camera(CAMERA_OUTER).hex(),
        # Enter, tied to question 0xC0FFEE01 (test_core.c).
        "answer": answer_vector(),
        "camera-frame": encode_camera_frame(
            PREVIEW_FLAG_STILL, 0xAB,
            bytes((i * 31) & 0xFF for i in range(PREVIEW_BYTES))).hex(),
        "photo-chunk": encode_photo_chunk(
            1024, bytes(i & 0xFF for i in range(1024))).hex(),
        "photo-end": encode_photo_end(1, CAMERA_OUTER, 0xCBF43926).hex(),
        "photo-discard": encode_photo_discard(7).hex(),
        # The pure camera helpers: the card's camera.c and the server's
        # dsterm/camera.py must produce the same bytes for the same input.
        # The preview is computed over the synthetic 256x192 frame both
        # generators build (r=(x*31//255), g=(y*31//191), b=((x+y)&31));
        # the YUYV vector covers the clamp extremes.
        "cam-preview": pycam_preview(),
        "cam-colour": pycam_colour(),
        "subscreen-head": subscreen_vectors()[0],
        "subscreen-tail": subscreen_vectors()[1],
        "cam-yuyv": binascii.hexlify(
            b"".join(
                bytes(pycam.yuyv_to_rgb888(y, cb, cr))
                for y, cb, cr in (
                    (0, -128, -128), (0, 0, 127), (0, 127, 0),
                    (255, 127, 127), (255, -128, -128), (128, 0, 0),
                    (100, 0, -1), (16, 32, -64),
                )
            )
        ).decode(),
        "cam-crc32": "%08x" % pycam.crc32(pycam.synth_4k()),
        # The packed wire forms of the synthetic YUYV capture: sha256 of the
        # whole buffer plus its first 64 bytes, so a packing drift is visible
        # even where a hash would only say "different".
        "cam-pack-full": _pack_vector(pycam.PHOTO_FORMAT_FULL),
        "cam-pack-small": _pack_vector(pycam.PHOTO_FORMAT_SMALL),
        # The discovery probe, byte for byte what the server's parser accepts.
        "probe": encode_probe("deadbeef").hex(),
        # QUICK's COMMANDS list as the card reads it back: the server's frame
        # payload, decoded by the card and printed name by name.
        "commands": commands_vector(),
        "voice-adpcm": voice_adpcm_vector(),
        "voice-wire": voice_wire_vector(),
    }

    failures = 0
    for name, want in expected.items():
        have = got.get(name)
        if have is None:
            print(f"MISSING  {name}: C side printed nothing")
            failures += 1
        elif have != want:
            print(f"MISMATCH {name}")
            print(f"    C      {have}")
            print(f"    Python {want}")
            failures += 1
        else:
            print(f"ok       {name}  {have}")

    print()
    print("C client and Python server agree" if not failures
          else f"{failures} mismatch(es) -- the DS would fail to authenticate")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
