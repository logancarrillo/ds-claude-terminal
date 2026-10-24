"""Wire format.

Every frame is length-prefixed. Nothing in this module assumes that one recv()
returns one message, or that a length field is honest -- both are decoded from
an accumulating buffer and bounds-checked before any allocation happens.

    +--------+---------+------+--------+---------+
    | magic  | version | type | length | payload |
    | 2 (DS) |    1    |  1   |   2    |  0..N   |
    +--------+---------+------+--------+---------+

All integers are big-endian. The ARM9 is little-endian, so the client must byte
swap; that is deliberate -- network order keeps the format unambiguous on paper
and the swap is three instructions.
"""

from __future__ import annotations

import functools
import hmac
import re
import struct
from dataclasses import dataclass
from enum import IntEnum
from hashlib import sha256
from typing import Iterator

from . import chacha20
from .grid import COLS, ROWS, Cell, CellChange, Grid

MAGIC = b"DS"
VERSION = 1

HEADER = struct.Struct(">2sBBH")
HEADER_SIZE = HEADER.size  # 6

# A DS has 4 MB of RAM. The largest legitimate payload is a full grid
# (1008 cells x 3 bytes = 3024), so anything approaching this cap is hostile or
# broken. The client must be able to reject on the header alone, before it has
# committed to reading the body.
MAX_PAYLOAD = 8192

# Bytes on the wire per cell, and per diff entry (which carries its position).
CELL_SIZE = 3
DIFF_ENTRY_SIZE = 5

NONCE_SIZE = 32
MAC_SIZE = 32


class MsgType(IntEnum):
    # client -> server
    HELLO = 0x01
    AUTH = 0x02
    KEY = 0x10
    RESYNC = 0x11
    PING = 0x12
    # "Move my view N lines up or down." An intent, not content: it cannot name
    # a file, a path or a command, so it does not widen what a compromised
    # client can reach. The server owns the history and decides what is
    # representable.
    VIEW = 0x13
    # "Switch me to the session at this index." One byte, and the index is only
    # meaningful against the list the server last advertised -- it is not a
    # tmux target, a name, a path or a persistent identity, and the server
    # resolves it or refuses. An index that no longer exists produces an ERROR
    # rather than the nearest surviving session, because silently switching
    # someone to a different session than the one they touched is worse than
    # telling them the row went away.
    #
    # No capability token gates this one, and it must not have one: an unknown
    # CLIENT->SERVER type makes the frame reader raise and drop the connection,
    # which a reconnecting card turns into a loop. The card therefore sends this
    # only after it has received a complete SESSIONS list, and receiving that
    # list is itself the proof that this server understands the pick.
    SESSION_PICK = 0x15
    # Pairing, the one exchange a card makes before it holds a key. All three
    # are allowed before authentication, and none can reach the PTY: they
    # touch nothing but the pairing window and the device key directory, and
    # the connection that carries them is dropped when the exchange ends. See
    # dsterm/pairing.py for the construction (a code compared on both screens).
    #
    # An old server raises on these and drops the connection, which the card
    # reports as "this server cannot pair". It never loops: pairing is a
    # person pressing a button, not a reconnect.
    PAIR_HELLO = 0x16       # device id (4) + the card's X25519 public key (32)
    PAIR_CONFIRM = 0x17     # the card's nonce (16)
    PAIR_ACCEPT = 0x1A      # HMAC: the codes matched on the card, same key
    # The REMOTE screen (docs/REMOTE.md). Both are intents in the shape of
    # SESSION_PICK: OPEN is one byte, "the screen is up / gone"; PICK is a row
    # index into the list this server last sent, an action number, and a
    # 16-bit check of the row's name and detail as the card drew them. None
    # can carry an id, a path or a command -- the server maps the index back to
    # the session IT listed, refuses the pick if that row's check differs, and
    # re-checks the action against that row's flags.
    #
    # Gated twice, and both gates matter. The card announces "remote" in its
    # HELLO, and it sends neither type until it has RECEIVED a REMOTE frame,
    # because an older server raises on an unknown client->server type and
    # drops the connection. A server that was not asked, or was started with
    # --no-remote, sends no REMOTE frame and so is never sent these.
    REMOTE_OPEN = 0x18      # 1 byte: 1 screen opened, 0 closed
    REMOTE_PICK = 0x19      # 4 bytes: row index, action, row check (u16be)
    # The DSi camera (see docs/design and the camera section below). Pixels
    # and intents, all bounded: a camera state, a 128x96 grey preview in one
    # fixed format, photo bytes at offsets the server checks against its own
    # count, a CRC, and an id the server itself issued. None names a file, a
    # path or a command -- the server alone chooses where a picture goes.
    #
    # Gated like REMOTE: the card announces "camera" in its HELLO and sends
    # none of these until CAMERA_READY has arrived on this connection. A
    # server run with --no-camera never sends one.
    CAMERA = 0x1B           # u8: 0 off, 1 inner, 2 outer
    CAMERA_FRAME = 0x1C     # format, flags, seq, 3072 pixel bytes
    PHOTO_CHUNK = 0x1D      # u32be offset, 1..1024 bytes
    PHOTO_END = 0x1E        # format, camera, u32be total, u32be CRC-32
    PHOTO_DISCARD = 0x1F    # u16be id, one this server issued
    # `dsterm view`: the card's bottom screen as text (see SUBSCREEN below),
    # sent only while this server's MIRROR says a view is watching.
    SUBSCREEN = 0x20
    # A key tied to the question it answers: u32be the ask id from the STATE
    # the card last heard, then 1..MAX_KEY_PAYLOAD key bytes. Sent only by a
    # card that announced "ask", and only after a STATE carrying an ask id
    # arrived on this connection (the proof rule). The server writes the
    # bytes only if that question is still the one open, so an Enter pressed
    # for one permission prompt can never answer the next. Like KEY it is key
    # bytes and nothing else: it names no command and no file.
    ANSWER = 0x21
    # Push-to-Talk microphone (dsterm/voice.py).  Audio samples (4-bit IMA
    # ADPCM at 8 kHz) and a short ISO language code hint; none names a file, a
    # path or a command.  Gated like CAMERA: the card announces "mic" in HELLO
    # and sends none of these until MIC_READY has arrived on this connection.
    VOICE_START = 0x22      # u8 format (1=IMA ADPCM 8kHz), u8 lang_len, lang
    VOICE_CHUNK = 0x23      # u32be offset, 1..480 ADPCM bytes
    VOICE_END = 0x24        # u32be total_bytes, u32be CRC-32

    # server -> client
    NONCE = 0x81
    AUTH_OK = 0x82
    AUTH_FAIL = 0x83
    FRAME_FULL = 0x90
    FRAME_DIFF = 0x91
    SCROLL = 0x92
    PONG = 0x93
    CURSOR = 0x94
    # Where the client's view sits in history: offset back, and how much
    # exists. Display state, like CURSOR -- it names nothing and grants nothing.
    VIEW_POS = 0x95
    # Which scrolling regime the child is in, so the handheld's scroll button
    # means the right thing. It names nothing and grants nothing -- one byte of
    # flags describing how the screen behaves, like CURSOR and VIEW_POS describe
    # where things are. It cannot be inferred client-side, and getting it wrong
    # makes a physical button do the wrong thing, which is worse than a stale
    # readout.
    MODE = 0x96
    # One entry of the session list, one entry per frame. Display state about
    # what the server is already running: it names windows the server chose to
    # offer, and the client can only answer with an index into it. Chunked for
    # the same reason SNIPPETS is -- 536 bytes is the ceiling on real traffic.
    #
    # A coordinated rollout, deliberately: a card older than the client half
    # cannot parse 0x97, and there is no speculative capability token to hide
    # behind. Server and card ship together for this one.
    SESSIONS = 0x97
    #
    # One chunk of a pushed snippet set. Content, not a handle: it names no
    # file, path or command, and the client cannot ask for it -- the server
    # sends what it has, once, to a client that announced it understands them.
    # What makes carrying content acceptable here is that a snippet cannot
    # reach the PTY without being displayed in the composer and committed by a
    # second deliberate press. That gate is the safety case, not a nicety:
    # a card without SEAL (docs/SEAL.md) has no per-message integrity, so
    # anyone who can inject can plant a snippet, and the only thing standing
    # between a planted snippet and a keystroke is the user reading it. SEAL
    # closes the injection for a card that has it; the gate stays, because
    # an older card still connects. See docs/design/SNIPPETS_PUSH.md.
    SNIPPETS = 0x98
    # Reserved for an extension: server-to-client only, and sent only to a
    # card whose HELLO carried the extension's own token. The terminal never
    # reads the body -- the card hands it to whatever asked for it, and a
    # body it does not want is dropped, never a connection-level error.
    EXTENSION = 0x99
    #
    # One row of the PROJECT page. Facts and labels, chunked one row per frame
    # for the same 536-byte reason SESSIONS and SNIPPETS are.
    #
    # Capability-gated on FEATURE_PROJECT, so an older card never sees a type
    # it cannot frame. Note what is NOT here: there is no matching request or
    # command message, and there never should be. The page's buttons work by
    # typing into the PTY as ordinary KEY bytes, which is what a person does,
    # and it keeps this protocol's promise that no message names a command.
    #
    # A row carries a label and a value the card will draw, so the same rule
    # snippets are under applies: this content reaches a screen, not a shell.
    PROJECT = 0x9A
    #
    # One chunk of the desktop clipboard, pushed down. Gated on FEATURE_CLIP and
    # off unless the server was started with --clip, because this widens the
    # blast radius of every machine whose clipboard syncs to this one: every
    # copy made on any of them reaches the DS. (The link itself is encrypted,
    # SEAL v3; it was not when this was first written.)
    #
    # Strictly one-way, like PROJECT. There is no request, no ack, and
    # there must not be one: the handshake is one-way -- the card announces its
    # tokens and the server announces nothing back -- so a new client->server
    # type would break a new card against an old server.
    CLIP = 0x9B
    # Pairing replies. Sent only in answer to PAIR_HELLO / PAIR_CONFIRM, so a
    # card that never asked to pair -- including every card older than these
    # types -- never receives one.
    PAIR_REPLY = 0x9C       # the server's X25519 public key (32) + commitment (32)
    PAIR_OK = 0x9D          # the server's nonce (16), opening the commitment
    PAIR_FAIL = 0x9E        # ASCII reason; the connection closes after it
    # The other Claude sessions on this machine, as `claude agents --json`
    # reports them: one row per frame, committed by the card when the last row
    # of a set arrives -- the SESSIONS discipline, for the same 536-byte
    # reason. Facts and labels only, sent only to a card that announced
    # "remote". REMOTE_NOTE is one line saying how a pick went.
    REMOTE = 0x9F
    REMOTE_NOTE = 0xA0
    # What the Claude session on screen is doing -- idle, working, or waiting
    # for an approval or an answer -- from the same `claude agents` reading.
    # One state byte and its waitingFor text; sent after authentication and
    # then only on change, and only to a card that announced "state". The
    # card's sounds play from its edges. See docs/REMOTE.md, "STATE".
    STATE = 0xA1
    # Pairing's last word: the person at the computer saw the same code, and
    # the device is saved (HMAC under the derived key).
    PAIR_DONE = 0xA2
    # The computer's own slash commands for QUICK's list: names only, one
    # frame, newline-separated. Sent after authentication and on change, to a
    # card that announced "commands". The card loads one into its composer,
    # so a name reaches Claude only when the person presses A.
    COMMANDS = 0xA3
    # Sent only to a card that announced "camera", and only in answer to the
    # card's own camera messages or, for READY, right after AUTH_OK.
    CAMERA_READY = 0xA4     # u8 flags: bit0 pictures can be saved
    CAMERA_ACK = 0xA5       # u8 kind (0 preview, 1 photo), u32be value
    PHOTO_SAVED = 0xA6      # status, u16be id, u8 len, path (printable ASCII)
    # `dsterm view`: whether a view is watching the card's bottom screen
    # (gated by "mirror"), and a click on it as a tap at a pixel, which the
    # card acts on only with its VIEW TAPS setting on.
    MIRROR = 0xA7           # u8 flags: bit0 a view is watching
    TOUCH = 0xA8            # u8 x (0..255), u8 y (0..191)
    # Sent only to a card that announced "mic": the capability proof right
    # after AUTH_OK, the credit window updates, and the transcribed text that
    # lands in the card's local composer for a second deliberate press of A.
    MIC_READY = 0xA9        # u8 flags: bit0 speech-to-text is available
    VOICE_ACK = 0xAA        # u32be received ADPCM bytes
    VOICE_TEXT = 0xAB       # u8 status, u16be len, text (printable ASCII/LF)
    ERROR = 0xFF


# Messages a client may send before it has authenticated. Everything else is
# refused, so an unauthenticated peer can never reach the PTY.
PRE_AUTH_ALLOWED = frozenset({MsgType.HELLO, MsgType.AUTH,
                              MsgType.PAIR_HELLO, MsgType.PAIR_CONFIRM,
                              MsgType.PAIR_ACCEPT})


class ProtocolError(Exception):
    """A frame was malformed. Always fatal for the connection -- never resync
    mid-stream, because a desynchronised length prefix cannot be recovered from."""


@dataclass(frozen=True, slots=True)
class Message:
    type: MsgType
    payload: bytes = b""


# -- framing ---------------------------------------------------------------


def encode(msg_type: MsgType, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError(f"payload {len(payload)} exceeds {MAX_PAYLOAD}")
    return HEADER.pack(MAGIC, VERSION, int(msg_type), len(payload)) + payload


class FrameReader:
    """Accumulates bytes and yields whole messages.

    TCP gives a byte stream, not messages: a single recv() may return half a
    header, three frames, or one byte. This is the only place that is allowed to
    care.

    It is also where a SEAL tag is checked and stripped (see start_sealing), so
    everything above the framing sees the same payloads whether or not the
    connection is sealed.
    """

    __slots__ = ("_buf", "_seal")

    def __init__(self) -> None:
        self._buf = bytearray()
        self._seal: Seal | None = None

    def start_sealing(self, seal: "Seal") -> None:
        """Require ``seal``'s tag on every frame from the next one on.

        Takes effect at the next frame boundary, including in the middle of a
        feed() that is already being iterated -- which is the point. feed() is
        a generator that re-reads this per frame, and the frame that turns
        sealing on (AUTH, on the server; AUTH_OK, on a card) commonly shares
        one recv() with the first sealed frame behind it. Switching after the
        whole buffer had been parsed would read that frame's tag as payload.
        """
        self._seal = seal

    @property
    def sealed(self) -> bool:
        return self._seal is not None

    def feed(self, data: bytes) -> Iterator[Message]:
        self._buf.extend(data)
        while True:
            if len(self._buf) < HEADER_SIZE:
                return
            magic, version, raw_type, length = HEADER.unpack_from(self._buf, 0)
            if magic != MAGIC:
                raise ProtocolError(f"bad magic {magic!r}")
            if version != VERSION:
                raise ProtocolError(f"unsupported version {version}")
            if length > MAX_PAYLOAD:
                # Checked before we wait for the body, so a hostile length
                # cannot make us buffer unboundedly.
                raise ProtocolError(f"declared length {length} exceeds {MAX_PAYLOAD}")
            seal = self._seal
            if seal is None:
                msg_type = _msg_type(raw_type)
            if len(self._buf) < HEADER_SIZE + length:
                return
            header = bytes(self._buf[:HEADER_SIZE]) if seal is not None else b""
            payload = bytes(self._buf[HEADER_SIZE : HEADER_SIZE + length])
            del self._buf[: HEADER_SIZE + length]
            if seal is not None:
                # The tag before the type: a frame that is not the peer's own
                # is refused as forged, never as merely unknown, and nothing
                # in it -- not even which message it claims to be -- is acted
                # on. Raises SealError, which ends the connection.
                payload = seal.open(header, payload)
                msg_type = _msg_type(raw_type)
            yield Message(msg_type, payload)

    @property
    def buffered(self) -> int:
        return len(self._buf)


def _msg_type(raw_type: int) -> MsgType:
    try:
        return MsgType(raw_type)
    except ValueError as exc:
        raise ProtocolError(f"unknown message type 0x{raw_type:02x}") from exc


# -- integrity (SEAL) --------------------------------------------------------
#
# docs/SEAL.md is the contract, and the card's half (dsclient/source/seal.c) is
# written against it byte for byte. Everything here is a few lines of HMAC; the
# care is all in WHICH bytes are covered and in what order the counters move.
#
# Authentication proved the card held the key once, at the start. Before SEAL
# nothing after that was protected: anyone who could see the TCP stream --
# anyone in radio range, on the open networks a DS Lite is limited to -- could
# splice a KEY frame into it and type into the Claude session. A sealed frame
# carries an 8-byte tag that only a holder of this connection's key can
# compute, over a counter that only the two ends know, so an injected,
# replayed, reordered or dropped frame fails the next check and ends the
# connection.
#
# SEAL v3 adds secrecy: every sealed payload is encrypted with ChaCha20 before
# it is tagged, so the terminal's text and the keys typed into it no longer
# cross the air in the clear. The tag covers what was encrypted, and is checked
# before a byte is decrypted.

# The HELLO tokens a card announces, and the AUTH_OK payload that turns sealing
# on. A current card announces both "seal" and "seal3": the old word is only
# there so that a server from before v3 answers with an AUTH_OK the card can
# name ("update dsterm on the computer") instead of an unexplained refusal.
# This server requires "seal3" and a card nonce, and answers "seal3"; a card
# requires "seal3" back. There is no fallback in either direction, because on
# an open network "too old" and "someone in the middle took the word out" look
# exactly the same.
FEATURE_SEAL = "seal"
FEATURE_SEAL3 = "seal3"
SEAL_AUTH_OK = b"seal3"
SEAL_AUTH_OK_V2 = b"seal"

SEAL_TAG_SIZE = 8
SEAL_KEY_SIZE = 32
SEAL_LABEL = b"dsterm seal v3"

# The card's own nonce for one connection, in its HELLO as "cn=" and 32
# lowercase hex digits. It is in the HELLO, and the HELLO is in the key, so a
# NONCE and the server frames after it, recorded and replayed at the card,
# give it different keys from the ones it had then -- never the same
# keystream twice.
CARD_NONCE_SIZE = 16
_CARD_NONCE_PREFIX = "cn="

# One byte of direction inside every tag, so a frame cannot be reflected back
# at the side that sent it and pass: the card's frame 7 and the server's frame
# 7 are tagged differently under the same key.
SEAL_FROM_CARD = b"C"
SEAL_FROM_SERVER = b"S"
_SEAL_DIRECTIONS = (SEAL_FROM_CARD, SEAL_FROM_SERVER)

# The counter is 32 bits and never sent. Four billion frames is years of a
# connection at the server's release rate; one that got there is ended rather
# than wrapped, because a wrapped counter is a replay window.
SEAL_SEQ_LIMIT = 1 << 32


class SealError(ProtocolError):
    """A sealed frame's tag was wrong or missing. Fatal, like every
    ProtocolError -- but named separately so the log says "integrity check
    failed", which is a different conversation from a framing bug."""


def hello_card_nonce(features: frozenset[str]) -> bytes | None:
    """The card nonce a HELLO carried, or None for none, or a malformed one."""
    for token in features:
        if token.startswith(_CARD_NONCE_PREFIX):
            text = token[len(_CARD_NONCE_PREFIX):]
            if len(text) == 2 * CARD_NONCE_SIZE and all(c in "0123456789abcdef" for c in text):
                return bytes.fromhex(text)
            return None
    return None


def seal_key(key: bytes, nonce: bytes, hello: bytes) -> bytes:
    """base = HMAC-SHA256(key, "dsterm seal v3" || nonce || SHA-256(hello)).

    ``key`` is the one the AUTH was checked against -- the device's own for a
    paired card, the legacy PSK otherwise -- ``nonce`` is this connection's
    NONCE, and ``hello`` is the HELLO payload exactly as the card sent it,
    card nonce included. A fresh nonce from each side per connection is a
    fresh base; and a HELLO edited on the way -- a capability taken out, a
    grid changed, the card nonce swapped -- gives the two ends different keys,
    so the first sealed frame ends the connection. seal_keys() turns the base
    into the three keys a connection uses.
    """
    if len(nonce) != NONCE_SIZE:
        raise ProtocolError(f"nonce must be {NONCE_SIZE} bytes, got {len(nonce)}")
    return hmac.new(key, SEAL_LABEL + nonce + sha256(hello).digest(), sha256).digest()


@functools.lru_cache(maxsize=64)
def seal_keys(base: bytes) -> tuple[bytes, bytes, bytes]:
    """(tag key, card-to-server cipher key, server-to-card cipher key).

    One key per direction for the cipher: the card's frame 7 and the
    server's frame 7 share a sequence number, and must never share a
    keystream.
    """
    def derive(label: bytes) -> bytes:
        return hmac.new(base, label, sha256).digest()
    return derive(b"mac"), derive(b"enc C"), derive(b"enc S")


def _cipher_key(base: bytes, direction: bytes) -> bytes:
    _, from_card, from_server = seal_keys(base)
    return from_card if direction == SEAL_FROM_CARD else from_server


def seal_nonce(seq: int) -> bytes:
    """The cipher's nonce for frame ``seq``: eight zero bytes and the count.
    Each direction has its own key and its own count, so no (key, nonce)
    pair is used twice."""
    return bytes(8) + seq.to_bytes(4, "big")


def seal_tag(k: bytes, direction: bytes, seq: int, header: bytes,
             payload: bytes) -> bytes:
    """tag = HMAC-SHA256(k_mac, dir || seq || header || payload)[0:8].

    ``k`` is the connection's base key; ``payload`` is the bytes on the wire,
    which are encrypted. ``header`` is the six bytes exactly as they cross
    the wire, with the length that already counts the tag -- so a length
    cannot be edited without breaking the tag either.
    """
    if direction not in _SEAL_DIRECTIONS:
        raise ValueError(f"direction must be one of {_SEAL_DIRECTIONS}, got {direction!r}")
    if not 0 <= seq < SEAL_SEQ_LIMIT:
        raise ProtocolError(f"seal sequence {seq} out of range")
    if len(header) != HEADER_SIZE:
        raise ValueError(f"header must be {HEADER_SIZE} bytes, got {len(header)}")
    mac = hmac.new(seal_keys(k)[0], direction + seq.to_bytes(4, "big") + header + payload,
                   sha256)
    return mac.digest()[:SEAL_TAG_SIZE]


def seal_frame(k: bytes, direction: bytes, seq: int, frame: bytes) -> bytes:
    """Seal ONE complete unsealed frame: its payload is encrypted, its length
    grows by eight and the tag over the encrypted payload is appended.

    Refuses anything that is not exactly one frame. Sealing half a frame, or
    two as one, would put a tag where the peer expects a header and desync the
    stream -- better to fail here, loudly, than on a handheld.
    """
    if len(frame) < HEADER_SIZE:
        raise ProtocolError(f"not a frame: {len(frame)} bytes")
    magic, version, raw_type, length = HEADER.unpack_from(frame, 0)
    if magic != MAGIC or version != VERSION:
        raise ProtocolError("not a frame: bad magic or version")
    if HEADER_SIZE + length != len(frame):
        raise ProtocolError(
            f"not one frame: header says {length}, have {len(frame) - HEADER_SIZE}")
    if not 0 <= seq < SEAL_SEQ_LIMIT - 1:
        raise ProtocolError(f"seal sequence {seq} out of range")
    # The sealed length must still fit the reader's cap on BOTH ends: the
    # card's reader refuses a declared length over PROTO_MAX_PAYLOAD before it
    # reads the body, exactly as FrameReader does.
    wire = length + SEAL_TAG_SIZE
    if wire > MAX_PAYLOAD:
        raise ProtocolError(f"sealed length {wire} exceeds {MAX_PAYLOAD}")
    header = HEADER.pack(MAGIC, VERSION, raw_type, wire)
    ciphertext = chacha20.xor(_cipher_key(k, direction), seal_nonce(seq),
                              frame[HEADER_SIZE:])
    return header + ciphertext + seal_tag(k, direction, seq, header, ciphertext)


def unseal(k: bytes, direction: bytes, seq: int, header: bytes, body: bytes) -> bytes:
    """Check a received frame's tag, then decrypt, and return the payload.

    ``header`` is the six bytes as received and ``body`` everything its length
    counted, tag included. Raises SealError for a frame too short to carry a
    tag or whose tag is not this one -- before a byte of it is decrypted. The
    comparison is constant-time: a timing-variable check would hand the tag
    over a byte at a time to anyone who can keep reconnecting.
    """
    if len(body) < SEAL_TAG_SIZE:
        raise SealError(f"frame {seq} carries no tag ({len(body)} bytes)")
    if not 0 <= seq < SEAL_SEQ_LIMIT - 1:
        raise SealError(f"seal sequence {seq} out of range")
    ciphertext, presented = body[:-SEAL_TAG_SIZE], body[-SEAL_TAG_SIZE:]
    expected = seal_tag(k, direction, seq, header, ciphertext)
    if not hmac.compare_digest(expected, presented):
        raise SealError(f"frame {seq} has a bad tag")
    return chacha20.xor(_cipher_key(k, direction), seal_nonce(seq), ciphertext)


class Seal:
    """One connection's sealing state: the key and a counter each way.

    Created at AUTH_OK and discarded with the connection, never reused: the
    counters are what make a dropped, repeated or reordered frame fail, so
    they must start at zero exactly when the other side's do.
    """

    __slots__ = ("_k", "_sends", "_receives", "sent", "received")

    def __init__(self, k: bytes, *, sends: bytes, receives: bytes) -> None:
        if len(k) != SEAL_KEY_SIZE:
            raise ValueError(f"seal key must be {SEAL_KEY_SIZE} bytes, got {len(k)}")
        if sends not in _SEAL_DIRECTIONS or receives not in _SEAL_DIRECTIONS \
                or sends == receives:
            raise ValueError("a seal sends one direction and receives the other")
        self._k = k
        self._sends = sends
        self._receives = receives
        # Frames sealed / verified so far, which is also the next one's seq.
        self.sent = 0
        self.received = 0

    @classmethod
    def for_server(cls, k: bytes) -> "Seal":
        return cls(k, sends=SEAL_FROM_SERVER, receives=SEAL_FROM_CARD)

    @classmethod
    def for_card(cls, k: bytes) -> "Seal":
        return cls(k, sends=SEAL_FROM_CARD, receives=SEAL_FROM_SERVER)

    def seal(self, data: bytes) -> bytes:
        """Seal every frame in ``data``, in order, one counter step each.

        ``data`` may be several frames back to back -- a caller handing over a
        batch need not split it -- but it must be WHOLE frames: a remainder
        raises rather than going out unsealed. Nothing is counted unless the
        whole of ``data`` sealed, so a refusal leaves the counter where the
        peer's is.
        """
        out = bytearray()
        seq = self.sent
        offset = 0
        while offset < len(data):
            if len(data) - offset < HEADER_SIZE:
                raise ProtocolError(f"{len(data) - offset} trailing bytes are not a frame")
            length = HEADER.unpack_from(data, offset)[3]
            end = offset + HEADER_SIZE + length
            if end > len(data):
                raise ProtocolError("a frame runs past the end of the data")
            out += seal_frame(self._k, self._sends, seq, bytes(data[offset:end]))
            seq += 1
            offset = end
        self.sent = seq
        return bytes(out)

    def open(self, header: bytes, body: bytes) -> bytes:
        """Verify the next received frame and return its payload."""
        payload = unseal(self._k, self._receives, self.received, header, body)
        self.received += 1
        return payload


# -- cell encoding ---------------------------------------------------------

# Index order is the wire encoding; never reorder, only append.
_PALETTE_INDEX = {
    "default": 0,
    "black": 1,
    "red": 2,
    "green": 3,
    "yellow": 4,
    "blue": 5,
    "magenta": 6,
    "cyan": 7,
    "white": 8,
    "orange": 9,
}
_INDEX_PALETTE = {v: k for k, v in _PALETTE_INDEX.items()}

# The DS font holds ASCII 0x20..0x7E plus an extended table above 0x7F. Both
# sides are generated from tools/extglyphs.py, so a wire byte means exactly one
# thing at each end.
from .extmap import FROM_WIRE, TO_WIRE

_FALLBACK_BYTE = ord("?")

_FLAG_BOLD = 0x01
_FLAG_REVERSE = 0x02


def _glyph_byte(char: str) -> int:
    """ASCII passes through; known extended glyphs get their wire byte.

    Claude Code frames its entire UI in box drawing. An earlier version mapped
    everything outside ASCII to '?', which the grid had deliberately preserved
    -- the screen came back as rows of question marks.
    """
    if not char:
        return 0x20
    code = ord(char[0])
    if 0x20 <= code <= 0x7E:
        return code
    wire = TO_WIRE.get(code)
    if wire is not None:
        return wire
    return _FALLBACK_BYTE


def pack_cell(cell: Cell) -> bytes:
    attr = (_PALETTE_INDEX.get(cell.fg, 0) << 4) | _PALETTE_INDEX.get(cell.bg, 0)
    flags = (_FLAG_BOLD if cell.bold else 0) | (_FLAG_REVERSE if cell.reverse else 0)
    return bytes((_glyph_byte(cell.char), attr, flags))


def _glyph_char(glyph: int) -> str:
    if 0x20 <= glyph <= 0x7E:
        return chr(glyph)
    code = FROM_WIRE.get(glyph)
    return chr(code) if code is not None else "?"


def unpack_cell(raw: bytes, offset: int = 0) -> Cell:
    glyph, attr, flags = raw[offset], raw[offset + 1], raw[offset + 2]
    return Cell(
        char=_glyph_char(glyph),
        fg=_INDEX_PALETTE.get(attr >> 4, "default"),
        bg=_INDEX_PALETTE.get(attr & 0x0F, "default"),
        bold=bool(flags & _FLAG_BOLD),
        reverse=bool(flags & _FLAG_REVERSE),
    )


def encode_full_frame(grid: Grid) -> bytes:
    return encode(MsgType.FRAME_FULL, b"".join(pack_cell(c) for c in grid.cells))


def decode_full_frame(payload: bytes) -> Grid:
    expected = COLS * ROWS * CELL_SIZE
    if len(payload) != expected:
        raise ProtocolError(f"full frame must be {expected} bytes, got {len(payload)}")
    cells = tuple(
        unpack_cell(payload, i * CELL_SIZE) for i in range(COLS * ROWS)
    )
    return Grid(cells)


def encode_diff(changes: list[CellChange]) -> bytes:
    if len(changes) * DIFF_ENTRY_SIZE > MAX_PAYLOAD:
        raise ProtocolError("diff too large; send a full frame instead")
    parts = bytearray()
    for ch in changes:
        parts.append(ch.row)
        parts.append(ch.col)
        parts.extend(pack_cell(ch.cell))
    return encode(MsgType.FRAME_DIFF, bytes(parts))


def decode_diff(payload: bytes) -> list[CellChange]:
    if len(payload) % DIFF_ENTRY_SIZE:
        raise ProtocolError(f"diff payload {len(payload)} is not a multiple of {DIFF_ENTRY_SIZE}")
    out: list[CellChange] = []
    for off in range(0, len(payload), DIFF_ENTRY_SIZE):
        row, col = payload[off], payload[off + 1]
        # A row/col past the grid would let a peer write outside the client's
        # cell array. The DS has no MMU to catch that.
        if row >= ROWS or col >= COLS:
            raise ProtocolError(f"cell position ({row},{col}) outside {COLS}x{ROWS}")
        out.append(CellChange(row, col, unpack_cell(payload, off + 2)))
    return out


# -- scroll ----------------------------------------------------------------
#
# Measured (tools/measure_cells.py): a scroll changes ~900 of 1008 cells, so
# sending it as a diff costs ~20x what the operation is worth. This says
# "shift, then expect the uncovered rows as a diff" in 3 bytes.

_SCROLL = struct.Struct(">BBb")


def encode_scroll(top: int, bottom: int, lines: int) -> bytes:
    if not (0 <= top < ROWS) or not (0 <= bottom < ROWS) or top > bottom:
        raise ProtocolError(f"invalid scroll region {top}..{bottom}")
    if not (-ROWS < lines < ROWS) or lines == 0:
        raise ProtocolError(f"invalid scroll distance {lines}")
    return encode(MsgType.SCROLL, _SCROLL.pack(top, bottom, lines))


def decode_scroll(payload: bytes) -> tuple[int, int, int]:
    if len(payload) != _SCROLL.size:
        raise ProtocolError(f"scroll payload must be {_SCROLL.size} bytes")
    top, bottom, lines = _SCROLL.unpack(payload)
    if top >= ROWS or bottom >= ROWS or top > bottom or lines == 0:
        raise ProtocolError(f"invalid scroll region {top}..{bottom} by {lines}")
    return top, bottom, lines


# -- cursor ----------------------------------------------------------------
#
# The emulated caret. Typing blind is unacceptable once the DS has a keyboard,
# and there was no way to show where the caret is: the client has only cells,
# and the server has a cursor that moves. CURSOR is display state -- the row,
# column and visibility of a caret -- which is why it is allowed where a message
# that named a file or a command would not be.
#
#   0.94 + 3 bytes: row (0..ROWS-1), col (0..COLS-1), visible (0 or 1)

CURSOR_SIZE = 3


# Signed, so one message covers both directions, and bounded so a hostile
# client cannot ask the server to walk an enormous history in one step.
# Matches Session.HISTORY_LINES: "return to live" has to be able to reach live
# from the deepest point the history holds, and a smaller ceiling meant a client
# scrolled further back than this could not get home in one request. Still a
# clamp rather than a refusal -- asking to go further than the history reaches
# means "as far as you can" -- and still the guard against being asked to walk
# an unbounded number of lines.
VIEW_MAX_DELTA = 5000


def encode_view(delta: int) -> bytes:
    """Scroll the view by `delta` lines: negative is back, positive is forward."""
    if not (-VIEW_MAX_DELTA <= delta <= VIEW_MAX_DELTA):
        raise ProtocolError(f"view delta {delta} exceeds {VIEW_MAX_DELTA}")
    return encode(MsgType.VIEW, struct.pack(">h", delta))


def decode_view(payload: bytes) -> int:
    if len(payload) != 2:
        raise ProtocolError(f"view payload must be 2 bytes, got {len(payload)}")
    delta = struct.unpack(">h", payload)[0]
    # Clamped rather than rejected: a client that asks for more than it may have
    # is asking for "as far as possible", which is a reasonable thing to mean.
    return max(-VIEW_MAX_DELTA, min(VIEW_MAX_DELTA, delta))


def encode_view_pos(offset: int, available: int) -> bytes:
    """Tell the client where its view sits: `offset` lines back of `available`.

    Without this the handheld is scrolling blind -- it has no way to know
    whether a press did anything, whether it has reached the top, or how far it
    has come. The server is the only side that knows, because the history is
    deliberately kept there.
    """
    offset = max(0, min(0xFFFF, offset))
    available = max(0, min(0xFFFF, available))
    return encode(MsgType.VIEW_POS, struct.pack(">HH", offset, available))


def decode_view_pos(payload: bytes) -> tuple[int, int]:
    if len(payload) != 4:
        raise ProtocolError(f"view_pos payload must be 4 bytes, got {len(payload)}")
    return struct.unpack(">HH", payload)


# -- capabilities and mode ---------------------------------------------------

# The HELLO payload is ASCII, whitespace-separated: a version token first, then
# zero or more feature tokens. Unknown tokens are ignored by both sides, always.
# That is what makes it safe in both directions and why no new client->server
# type is needed to negotiate anything again: an old server ignores a token it
# has never heard of, and an old card never sends one.
#
# It has to be this way round. An unknown *type* is fatal in both directions --
# the card halts on it, and this server raises inside the frame reader and drops
# the connection, which a reconnecting card turns into a loop. A token is inert.
HELLO_VERSION = "ds-term/1"
FEATURE_MODE = "mode"
FEATURE_SNIPPETS = "snippets"
FEATURE_PROJECT = "project"
FEATURE_CLIP = "clip"
FEATURE_REMOTE = "remote"
FEATURE_STATE = "state"
FEATURE_COMMANDS = "commands"
FEATURE_CAMERA = "camera"
FEATURE_MIRROR = "mirror"
# STATE carries the open question's id and the card ties its Enter to it
# (ANSWER). See docs/PROTOCOL.md, "Answers".
FEATURE_ASK = "ask"
# The card has a microphone and knows the voice types: send MIC_READY after
# AUTH_OK (unless --no-mic), and only then may it send VOICE_*.
FEATURE_MIC = "mic"
# The card takes PROJECT snapshots of up to PROJECT_ROWS_MAX (64) rows and the
# DS glyph bytes 0x80-0xBF in their text. A 1.0.x card refuses a count above
# PROJECT_ROWS_MAX_V1 and draws those bytes as '?', so without this token the
# server sends it at most 48 rows of printable ASCII (project.encode).
FEATURE_PROJECT64 = "project64"


def encode_hello(features: tuple[str, ...] = ()) -> bytes:
    return encode(MsgType.HELLO, " ".join((HELLO_VERSION, *features)).encode("ascii"))


def hello_features(payload: bytes) -> frozenset[str]:
    """Feature tokens in a HELLO payload. Never raises: a malformed hello from a
    client we have not authenticated yet must not be able to drop the connection,
    and an undecodable token is simply a capability we do not have."""
    try:
        text = payload.decode("ascii")
    except UnicodeDecodeError:
        return frozenset()
    return frozenset(text.split()[1:])


# Bit 0 of the MODE flags byte. Bits 1-7 are reserved and are sent as zero; the
# client masks, so claiming one later is not a flag day.
#
# Named for what the card must DO, not for the mechanism that causes it. It was
# very nearly MODE_ALT_SCREEN, and that would have been wrong in the only
# configuration we ship: with a tmux-wrapped child the alternate screen never
# shows up out here, yet the history is still empty. Two different causes, one
# consequence -- the server has nothing to scroll, so the application must be
# asked instead. The consequence is the thing worth naming.
MODE_APP_SCROLLS = 0x01


def encode_mode(app_scrolls: bool) -> bytes:
    """Tell the client which scrolling regime the child is in.

    Sent once after AUTH_OK even when false -- "not on the alternate screen" and
    "the server has not said yet" call for different behaviour on the card -- and
    then on every change, immediately. Deliberately not rate-limited and
    deliberately not folded into VIEW_POS: this decides what a physical button
    does, and VIEW_POS is gated on the client having sent a VIEW, which a card in
    alt-screen mode stops doing. That gate would be circular.
    """
    return encode(MsgType.MODE, bytes((MODE_APP_SCROLLS if app_scrolls else 0,)))


def decode_mode(payload: bytes) -> int:
    if len(payload) != 1:
        raise ProtocolError(f"mode payload must be 1 byte, got {len(payload)}")
    return payload[0]


# One chunk per snippet, never one message for the set. Eight snippets at the
# maximum body length is over 2 KiB, and every server->client message has to
# clear 536 bytes -- a limit test_server.py asserts on real traffic. One
# snippet at maximum size is 5 + 15 + 2 + 255 = 277 bytes of payload.
SNIPPET_LABEL_MAX = 15
SNIPPET_BODY_MAX = 255
SNIPPETS_MAX = 8


def encode_snippet(index: int, total: int, label: str, body: str) -> bytes:
    """One chunk: u8 index, u8 total, u8 label_len, label, u16 body_len, body.

    The body length field sits AFTER the label, so its offset depends on
    label_len. That caught the client's first decoder, which read it from a
    fixed offset and got the label's first two bytes instead.
    """
    lb = label.encode("ascii")
    bb = body.encode("ascii")
    if not 1 <= total <= SNIPPETS_MAX:
        raise ProtocolError(f"total {total} outside 1..{SNIPPETS_MAX}")
    if not 0 <= index < total:
        raise ProtocolError(f"index {index} outside 0..{total - 1}")
    if not 1 <= len(lb) <= SNIPPET_LABEL_MAX:
        raise ProtocolError(f"label must be 1..{SNIPPET_LABEL_MAX} bytes")
    if len(bb) > SNIPPET_BODY_MAX:
        raise ProtocolError(f"body exceeds {SNIPPET_BODY_MAX} bytes")
    payload = (bytes((index, total, len(lb))) + lb
               + struct.pack(">H", len(bb)) + bb)
    return encode(MsgType.SNIPPETS, payload)


def decode_snippet(payload: bytes) -> tuple[int, int, str, str]:
    """Inverse of encode_snippet, bounds-checked the way the card checks it."""
    if len(payload) < 6:
        raise ProtocolError(f"snippet payload too short: {len(payload)}")
    index, total, label_len = payload[0], payload[1], payload[2]
    if not 1 <= total <= SNIPPETS_MAX:
        raise ProtocolError(f"total {total} outside 1..{SNIPPETS_MAX}")
    if index >= total:
        raise ProtocolError(f"index {index} not below total {total}")
    if not 1 <= label_len <= SNIPPET_LABEL_MAX:
        raise ProtocolError(f"label_len {label_len} outside 1..{SNIPPET_LABEL_MAX}")
    at = 3 + label_len
    if len(payload) < at + 2:
        raise ProtocolError("truncated before body length")
    label = payload[3:at].decode("ascii", errors="replace")
    body_len = struct.unpack(">H", payload[at:at + 2])[0]
    if body_len > SNIPPET_BODY_MAX:
        raise ProtocolError(f"body_len {body_len} exceeds {SNIPPET_BODY_MAX}")
    if len(payload) != at + 2 + body_len:
        raise ProtocolError("declared lengths do not match the payload")
    body = payload[at + 2:].decode("ascii", errors="replace")
    return index, total, label, body


# -- sessions -----------------------------------------------------------------

SESSIONS_MAX = 8
SESSION_NAME_MAX = 24

# Flags byte. Bits 3-7 are reserved, sent as zero, and rejected by strict
# reference decoding.
SESSION_RUNNING = 0x01
SESSION_ACTIVE = 0x02
SESSION_NEW_OUTPUT = 0x04
SESSION_FLAGS = SESSION_RUNNING | SESSION_ACTIVE | SESSION_NEW_OUTPUT


def encode_sessions_empty() -> bytes:
    """The list, when there is nothing to offer. One byte: count = 0.

    A distinct shape rather than an omitted message, because "no sessions" and
    "the server has not told me" need to look different on the handheld -- the
    same reason MODE is sent even when false.
    """
    return encode(MsgType.SESSIONS, bytes((0,)))


def encode_session(index: int, count: int, flags: int, idle_s: int,
                   name: str) -> bytes:
    """One entry: count, index, flags, idle_s (u16be), name_len, name."""
    try:
        nb = name.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ProtocolError("session name is not ASCII") from exc
    nb = nb[:SESSION_NAME_MAX]
    if not 1 <= count <= SESSIONS_MAX:
        raise ProtocolError(f"count {count} outside 1..{SESSIONS_MAX}")
    if not 0 <= index < count:
        raise ProtocolError(f"index {index} outside 0..{count - 1}")
    if not 0 <= flags <= 0xFF or flags & ~SESSION_FLAGS:
        raise ProtocolError(f"flags {flags} contain reserved session bits")
    if not nb:
        raise ProtocolError("empty session name")
    if any(b < 0x20 or b > 0x7E for b in nb):
        raise ProtocolError("session name must be printable ASCII")
    idle_s = max(0, min(0xFFFF, idle_s))
    payload = (bytes((count, index, flags)) + struct.pack(">H", idle_s)
               + bytes((len(nb),)) + nb)
    return encode(MsgType.SESSIONS, payload)


def decode_session(payload: bytes) -> tuple[int, int, int, int, str]:
    """Inverse of the pair above. Returns (count, index, flags, idle_s, name);
    an empty list is (0, 0, 0, 0, "")."""
    if len(payload) == 1 and payload[0] == 0:
        return (0, 0, 0, 0, "")
    if len(payload) < 6:
        raise ProtocolError(f"session payload too short: {len(payload)}")
    count, index, flags = payload[0], payload[1], payload[2]
    if not 1 <= count <= SESSIONS_MAX:
        raise ProtocolError(f"count {count} outside 1..{SESSIONS_MAX}")
    if index >= count:
        raise ProtocolError(f"index {index} not below count {count}")
    if flags & ~SESSION_FLAGS:
        raise ProtocolError(f"reserved session flags set: 0x{flags:02x}")
    idle_s = struct.unpack(">H", payload[3:5])[0]
    name_len = payload[5]
    if not 1 <= name_len <= SESSION_NAME_MAX:
        raise ProtocolError(f"name_len {name_len} outside 1..{SESSION_NAME_MAX}")
    if len(payload) != 6 + name_len:
        raise ProtocolError("declared name length does not match the payload")
    name_bytes = payload[6:]
    if any(b < 0x20 or b > 0x7E for b in name_bytes):
        raise ProtocolError("session name must be printable ASCII")
    return count, index, flags, idle_s, name_bytes.decode("ascii")


# The page's tabs, and how many rows one snapshot may carry. Both are mirrored
# in dsclient/source/project.h; a value here that the card does not share is a
# row it will refuse, which is the safe direction for a mismatch to fail in.
PROJECT_TAB_NOW = 0
PROJECT_TAB_DIFF = 1
PROJECT_TAB_DO = 2
PROJECT_TAB_WATCH = 3
PROJECT_TAB_HEALTH = 4
PROJECT_TAB_LOG = 5
PROJECT_TAB_COUNT = 6
PROJECT_ROWS_MAX = 64
# What a card without "project64" (1.0.x) accepts. See FEATURE_PROJECT64.
PROJECT_ROWS_MAX_V1 = 48
PROJECT_LABEL_MAX = 12
PROJECT_VALUE_MAX = 40


def _encode_project_str(text: str, limit: int, glyphs: bool = True) -> bytes:
    """Printable ASCII, plus -- for a "project64" card only -- the DS glyphs
    of extmap.TO_WIRE as their wire bytes. Anything else is refused, not
    replaced: what to show instead is project.py's decision, made before."""
    out = bytearray()
    for ch in text:
        code = ord(ch)
        if 0x20 <= code <= 0x7E:
            out.append(code)
        elif glyphs and code in TO_WIRE:
            out.append(TO_WIRE[code])
        elif glyphs:
            raise ProtocolError("project text is not ASCII or extended DS glyph")
        else:
            raise ProtocolError("project text is not printable ASCII")
        if len(out) >= limit:
            break
    return bytes(out)


def _decode_project_bytes(raw: bytes) -> str:
    chars: list[str] = []
    for b in raw:
        if 0x20 <= b <= 0x7E:
            chars.append(chr(b))
        elif b in FROM_WIRE:
            chars.append(chr(FROM_WIRE[b]))
        else:
            raise ProtocolError("project text must be printable ASCII or extended DS glyph")
    return "".join(chars)


def encode_project_empty() -> bytes:
    """The page, when the server has nothing to say about it.

    A distinct shape rather than silence, for the same reason the empty session
    list is: "nothing to report" and "this server does not report it" are
    different answers, and the handheld shows them differently.
    """
    return encode(MsgType.PROJECT, bytes((0,)))


def encode_project_row(index: int, count: int, tab: int, fg: int,
                       label: str, value: str, project64: bool = True) -> bytes:
    """One row: count, index, tab, fg, label_len, value_len, label, value.

    ``project64=False`` is the 1.0.x card's contract: a count of at most
    PROJECT_ROWS_MAX_V1 and printable ASCII only."""
    rows_max = PROJECT_ROWS_MAX if project64 else PROJECT_ROWS_MAX_V1
    if not 1 <= count <= rows_max:
        raise ProtocolError(f"count {count} outside 1..{rows_max}")
    if not 0 <= index < count:
        raise ProtocolError(f"index {index} outside 0..{count - 1}")
    if not 0 <= tab < PROJECT_TAB_COUNT:
        raise ProtocolError(f"tab {tab} outside 0..{PROJECT_TAB_COUNT - 1}")
    if not 0 <= fg <= 15:
        raise ProtocolError(f"colour {fg} is not a palette index")
    lb = _encode_project_str(label, PROJECT_LABEL_MAX, project64)
    vb = _encode_project_str(value, PROJECT_VALUE_MAX, project64)
    if not lb:
        raise ProtocolError("empty project label")
    payload = (bytes((count, index, tab, fg, len(lb), len(vb))) + lb + vb)
    return encode(MsgType.PROJECT, payload)


def decode_project_row(payload: bytes) -> tuple[int, int, int, int, str, str]:
    """Inverse of the pair above. An empty snapshot is (0, 0, 0, 0, "", "")."""
    if len(payload) == 1 and payload[0] == 0:
        return (0, 0, 0, 0, "", "")
    if len(payload) < 6:
        raise ProtocolError(f"project payload too short: {len(payload)}")
    count, index, tab, fg, label_len, value_len = payload[:6]
    if not 1 <= count <= PROJECT_ROWS_MAX:
        raise ProtocolError(f"count {count} outside 1..{PROJECT_ROWS_MAX}")
    if index >= count:
        raise ProtocolError(f"index {index} not below count {count}")
    if tab >= PROJECT_TAB_COUNT:
        raise ProtocolError(f"tab {tab} outside 0..{PROJECT_TAB_COUNT - 1}")
    if fg > 15:
        raise ProtocolError(f"colour {fg} is not a palette index")
    if not 1 <= label_len <= PROJECT_LABEL_MAX:
        raise ProtocolError(f"label_len {label_len} outside 1..{PROJECT_LABEL_MAX}")
    if value_len > PROJECT_VALUE_MAX:
        raise ProtocolError(f"value_len {value_len} above {PROJECT_VALUE_MAX}")
    if len(payload) != 6 + label_len + value_len:
        raise ProtocolError("declared lengths do not match the payload")
    body = payload[6:]
    label = _decode_project_bytes(body[:label_len])
    value = _decode_project_bytes(body[label_len:])
    return count, index, tab, fg, label, value


# One chunk of the desktop clipboard, mirrored in dsclient/source/clip.h.
#
# CLIP_CHUNK_MAX is 192 because the card sized its staging buffer for it, not
# because the ceiling demands it: the header is 6 bytes and the fixed fields
# below are 9, so a chunk could be 521 and still clear 536. Left at 192 because
# a number one half already designed to is not worth changing unilaterally --
# that is how two halves drift.
CLIP_CHUNK_MAX = 192
CLIP_TEXT_MAX = 1024
CLIP_CHUNKS_MAX = 6            # ceil(CLIP_TEXT_MAX / CLIP_CHUNK_MAX)

# Flags. TRUNCATED and TRANSLITERATED say the text is not what was copied;
# WITHHELD appears only on an empty snapshot and says why it is empty.
CLIP_FLAG_TRUNCATED = 0x01
CLIP_FLAG_TRANSLITERATED = 0x02
CLIP_FLAG_WITHHELD = 0x04

# Newline is the one control byte that survives; the composer holds it via the
# on-screen ENT key. Everything else is folded or dropped before it gets here.
_CLIP_LF = 0x0A


def encode_clip_empty(flags: int = 0) -> bytes:
    """An empty clipboard: count = 0, and a flag byte saying why.

    PROJECT's empty frame is one byte because "nothing to report" is the only
    thing it can mean. This one carries flags because it has two meanings that
    must not look alike: the clipboard is genuinely empty, or it was WITHHELD
    because a password manager marked it. Showing an empty pane for a concealed
    copy invites the user to copy it again, which is the one outcome this
    feature must never encourage.
    """
    if not 0 <= flags <= 0xFF:
        raise ProtocolError(f"flags {flags} is not a byte")
    return encode(MsgType.CLIP, bytes((0, flags)))


def encode_clip_chunk(index: int, count: int, seq: int, flags: int,
                      total: int, body: bytes) -> bytes:
    """One chunk: count, index, seq, flags, total (u32be), len, bytes.

    ``total`` is the length of the CONVERTED text -- ASCII-folded and newline
    normalised -- before truncation, not the raw clipboard length. That is what
    makes ``sum(len) == min(total, CLIP_TEXT_MAX)`` an invariant both ends can
    assert, and an invariant is the only thing that stops two encoders agreeing
    in testing and disagreeing on the wire.

    u32be rather than u16be because a saturating total is a wrong number, not a
    coarse one: a 200 KB clipboard would report "1.0K of 65.5K" and the card
    would draw it as fact.
    """
    if not 1 <= count <= CLIP_CHUNKS_MAX:
        raise ProtocolError(f"count {count} outside 1..{CLIP_CHUNKS_MAX}")
    if not 0 <= index < count:
        raise ProtocolError(f"index {index} outside 0..{count - 1}")
    if not 0 <= seq <= 0xFF:
        raise ProtocolError(f"seq {seq} is not a byte")
    if not 0 <= flags <= 0xFF:
        raise ProtocolError(f"flags {flags} is not a byte")
    if not 0 <= total <= 0xFFFFFFFF:
        raise ProtocolError(f"total {total} does not fit u32")
    if not 1 <= len(body) <= CLIP_CHUNK_MAX:
        raise ProtocolError(f"chunk length {len(body)} outside 1..{CLIP_CHUNK_MAX}")
    if any(b != _CLIP_LF and (b < 0x20 or b > 0x7E) for b in body):
        raise ProtocolError("clip text must be printable ASCII or newline")
    payload = (bytes((count, index, seq, flags)) + struct.pack(">I", total)
               + bytes((len(body),)) + body)
    return encode(MsgType.CLIP, payload)


def decode_clip_chunk(payload: bytes) -> tuple[int, int, int, int, int, bytes]:
    """Inverse of the pair above. An empty snapshot is (0, 0, 0, flags, 0, b"").

    Every bound the encoder enforces is re-checked here rather than trusted,
    because this is also what the interop vector exercises: the card's decoder
    and this one have to refuse the same frames, not merely accept the same ones.
    """
    if len(payload) == 2 and payload[0] == 0:
        return (0, 0, 0, payload[1], 0, b"")
    if len(payload) < 9:
        raise ProtocolError(f"clip payload too short: {len(payload)}")
    count, index, seq, flags = payload[:4]
    total = struct.unpack_from(">I", payload, 4)[0]
    body_len = payload[8]
    if not 1 <= count <= CLIP_CHUNKS_MAX:
        raise ProtocolError(f"count {count} outside 1..{CLIP_CHUNKS_MAX}")
    if index >= count:
        raise ProtocolError(f"index {index} not below count {count}")
    if not 1 <= body_len <= CLIP_CHUNK_MAX:
        raise ProtocolError(f"chunk length {body_len} outside 1..{CLIP_CHUNK_MAX}")
    if len(payload) != 9 + body_len:
        raise ProtocolError("declared length does not match the payload")
    body = payload[9:]
    if any(b != _CLIP_LF and (b < 0x20 or b > 0x7E) for b in body):
        raise ProtocolError("clip text must be printable ASCII or newline")
    return count, index, seq, flags, total, body


def encode_session_pick(index: int) -> bytes:
    if not 0 <= index < SESSIONS_MAX:
        raise ProtocolError(f"index {index} outside 0..{SESSIONS_MAX - 1}")
    return encode(MsgType.SESSION_PICK, bytes((index,)))


def decode_session_pick(payload: bytes) -> int:
    if len(payload) != 1:
        raise ProtocolError(f"session pick must be 1 byte, got {len(payload)}")
    return payload[0]


# -- remote -------------------------------------------------------------------
#
# Mirrored in the card's REMOTE screen; docs/REMOTE.md is the contract and the
# numbers below are its tables. Every bound is checked on the way out AND on
# the way in, because the decoder is also what an interop vector exercises: the
# card and this module have to refuse the same frames, not merely accept the
# same ones.

REMOTE_ROWS_MAX = 8
REMOTE_NAME_MAX = 24
REMOTE_DETAIL_MAX = 28
REMOTE_NOTE_MAX = 40

# Byte 1 of an empty set. "Not asked yet" is what a card is told right after
# authentication, so it can tell "this server speaks REMOTE" from silence.
REMOTE_STATUS_NOT_ASKED = 0
REMOTE_STATUS_NONE = 1
REMOTE_STATUS_NO_CLAUDE = 2
REMOTE_STATUS_FAILED = 3
_REMOTE_STATUS_MAX = REMOTE_STATUS_FAILED

REMOTE_KIND_BACKGROUND = 0
REMOTE_KIND_INTERACTIVE = 1

REMOTE_STATE_UNKNOWN = 0
REMOTE_STATE_WORKING = 1
REMOTE_STATE_NEEDS_YOU = 2
REMOTE_STATE_DONE = 3
REMOTE_STATE_IDLE = 4
REMOTE_STATE_FAILED = 5
REMOTE_STATE_STOPPED = 6
_REMOTE_STATE_MAX = REMOTE_STATE_STOPPED

# Flags byte. Bits 3-7 are reserved, sent as zero and refused on decode.
REMOTE_HERE = 0x01
REMOTE_LIVE = 0x02
REMOTE_COPY = 0x04
REMOTE_FLAGS = REMOTE_HERE | REMOTE_LIVE | REMOTE_COPY

# REMOTE_PICK actions. NEW takes no row, so it travels with index 0xFF -- a
# value no row can have, which keeps "open row 0" and "open nothing in
# particular" from ever being confused.
REMOTE_ACTION_OPEN = 0
REMOTE_ACTION_COPY = 1
REMOTE_ACTION_NEW = 2
REMOTE_NEW_INDEX = 0xFF

# A pick names its row twice: by position, and by a 16-bit check of what that
# row SAID. Position alone was not enough. The list is re-sorted on every
# answer -- NEEDS YOU first, then WORKING -- so a session that started waiting
# moves to the top while a thumb is on its way to the A button, and "row 1"
# then names a different session than the one the person read. The server
# resolves the index against the rows it last sent this connection, and if
# that row's check is not the one the card drew, it acts on nothing and says
# "the list changed; pick again".
#
# The check covers the name and detail bytes exactly as they were sent (after
# this server's clamping), joined by one NUL that neither may contain, so
# ("ab", "c") and ("a", "bc") differ. FNV-1a because the card computes the same
# thing in a dozen lines of C with no table, folded to 16 bits because this is
# a guard against a moving list, not against an adversary: a client that wants
# to pick row 1 can always just send row 1's check. NEW names no row, so its
# check is 0.
REMOTE_PICK_SIZE = 4
_FNV32_OFFSET = 0x811C9DC5
_FNV32_PRIME = 0x01000193

_REMOTE_ROW = struct.Struct(">BBBBBH")    # count, index, kind, state, flags, age


def _remote_text(text: str, limit: int, what: str) -> bytes:
    """Printable ASCII at most ``limit`` bytes, or a ProtocolError.

    Refused rather than clamped: clamping and replacement are the caller's job
    (dsterm/remote.py), and an encoder that silently rewrote its input would let
    a caller that forgot to sanitize ship something nobody chose.
    """
    try:
        raw = text.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ProtocolError(f"remote {what} is not ASCII") from exc
    if len(raw) > limit:
        raise ProtocolError(f"remote {what} exceeds {limit} bytes")
    if any(b < 0x20 or b > 0x7E for b in raw):
        raise ProtocolError(f"remote {what} must be printable ASCII")
    return raw


def encode_remote_empty(status: int) -> bytes:
    """An empty set: count = 0, then why it is empty.

    Two bytes rather than PROJECT's one, for CLIP's reason: "there are none",
    "claude is not installed here" and "the query failed" call for different
    words on the handheld, and all of them differ from "not asked yet".
    """
    if not 0 <= status <= _REMOTE_STATUS_MAX:
        raise ProtocolError(f"remote status {status} outside 0..{_REMOTE_STATUS_MAX}")
    return encode(MsgType.REMOTE, bytes((0, status)))


def encode_remote_row(index: int, count: int, kind: int, state: int, flags: int,
                      age_min: int, name: str, detail: str) -> bytes:
    """One row: count, index, kind, state, flags, age (u16be), name_len, name,
    detail_len, detail.

    The detail length sits AFTER the name, so its offset depends on name_len --
    the same shape that caught the card's first snippet decoder reading a
    length from a fixed offset.
    """
    if not 1 <= count <= REMOTE_ROWS_MAX:
        raise ProtocolError(f"count {count} outside 1..{REMOTE_ROWS_MAX}")
    if not 0 <= index < count:
        raise ProtocolError(f"index {index} outside 0..{count - 1}")
    if kind not in (REMOTE_KIND_BACKGROUND, REMOTE_KIND_INTERACTIVE):
        raise ProtocolError(f"remote kind {kind} is not 0 or 1")
    if not 0 <= state <= _REMOTE_STATE_MAX:
        raise ProtocolError(f"remote state {state} outside 0..{_REMOTE_STATE_MAX}")
    if not 0 <= flags <= 0xFF or flags & ~REMOTE_FLAGS:
        raise ProtocolError(f"flags {flags} contain reserved remote bits")
    nb = _remote_text(name, REMOTE_NAME_MAX, "name")
    db = _remote_text(detail, REMOTE_DETAIL_MAX, "detail")
    age_min = max(0, min(0xFFFF, age_min))
    payload = (_REMOTE_ROW.pack(count, index, kind, state, flags, age_min)
               + bytes((len(nb),)) + nb + bytes((len(db),)) + db)
    return encode(MsgType.REMOTE, payload)


def decode_remote(payload: bytes) -> tuple[int, int, int, int, int, int, str, str]:
    """Inverse of the pair above: (count, index, kind, state, flags, age_min,
    name, detail).

    An empty set is (0, status, 0, 0, 0, 0, "", ""): byte 1 is the status there
    and the index everywhere else, and the tuple says exactly what the wire
    says rather than inventing a second shape.
    """
    if len(payload) == 2 and payload[0] == 0:
        status = payload[1]
        if status > _REMOTE_STATUS_MAX:
            raise ProtocolError(f"remote status {status} outside 0..{_REMOTE_STATUS_MAX}")
        return (0, status, 0, 0, 0, 0, "", "")
    if len(payload) < _REMOTE_ROW.size + 2:
        raise ProtocolError(f"remote payload too short: {len(payload)}")
    count, index, kind, state, flags, age = _REMOTE_ROW.unpack_from(payload, 0)
    if not 1 <= count <= REMOTE_ROWS_MAX:
        raise ProtocolError(f"count {count} outside 1..{REMOTE_ROWS_MAX}")
    if index >= count:
        raise ProtocolError(f"index {index} not below count {count}")
    if kind > REMOTE_KIND_INTERACTIVE:
        raise ProtocolError(f"remote kind {kind} is not 0 or 1")
    if state > _REMOTE_STATE_MAX:
        raise ProtocolError(f"remote state {state} outside 0..{_REMOTE_STATE_MAX}")
    if flags & ~REMOTE_FLAGS:
        raise ProtocolError(f"reserved remote flags set: 0x{flags:02x}")
    at = _REMOTE_ROW.size
    name_len = payload[at]
    if name_len > REMOTE_NAME_MAX:
        raise ProtocolError(f"name_len {name_len} above {REMOTE_NAME_MAX}")
    at += 1
    if len(payload) < at + name_len + 1:
        raise ProtocolError("truncated before detail length")
    name = payload[at:at + name_len]
    at += name_len
    detail_len = payload[at]
    if detail_len > REMOTE_DETAIL_MAX:
        raise ProtocolError(f"detail_len {detail_len} above {REMOTE_DETAIL_MAX}")
    at += 1
    if len(payload) != at + detail_len:
        raise ProtocolError("declared lengths do not match the payload")
    detail = payload[at:]
    if any(b < 0x20 or b > 0x7E for b in name + detail):
        raise ProtocolError("remote text must be printable ASCII")
    return (count, index, kind, state, flags, age,
            name.decode("ascii"), detail.decode("ascii"))


def encode_remote_note(text: str) -> bytes:
    """The outcome of a pick, one line: 1..40 bytes of printable ASCII."""
    raw = _remote_text(text, REMOTE_NOTE_MAX, "note")
    if not raw:
        raise ProtocolError("empty remote note")
    return encode(MsgType.REMOTE_NOTE, raw)


def decode_remote_note(payload: bytes) -> str:
    if not 1 <= len(payload) <= REMOTE_NOTE_MAX:
        raise ProtocolError(f"remote note length {len(payload)} outside 1..{REMOTE_NOTE_MAX}")
    if any(b < 0x20 or b > 0x7E for b in payload):
        raise ProtocolError("remote note must be printable ASCII")
    return payload.decode("ascii")


def encode_remote_open(is_open: bool) -> bytes:
    return encode(MsgType.REMOTE_OPEN, bytes((1 if is_open else 0,)))


def decode_remote_open(payload: bytes) -> bool:
    """One byte, 0 or 1. Anything else is a malformed frame, not a guess."""
    if len(payload) != 1:
        raise ProtocolError(f"remote open must be 1 byte, got {len(payload)}")
    if payload[0] > 1:
        raise ProtocolError(f"remote open must be 0 or 1, got {payload[0]}")
    return payload[0] == 1


def remote_row_check(name: str | bytes, detail: str | bytes) -> int:
    """The 16-bit check a REMOTE_PICK carries for the row it names.

    fold16(FNV-1a-32(name + NUL + detail)), where fold16(h) is the high half
    XOR the low half. ``name`` and ``detail`` are the row's text exactly as it
    went on the wire; a str is taken as the ASCII the encoder sent.
    """
    nb = name.encode("ascii") if isinstance(name, str) else bytes(name)
    db = detail.encode("ascii") if isinstance(detail, str) else bytes(detail)
    h = _FNV32_OFFSET
    for b in nb + b"\x00" + db:
        h = ((h ^ b) * _FNV32_PRIME) & 0xFFFFFFFF
    return ((h >> 16) ^ (h & 0xFFFF)) & 0xFFFF


def encode_remote_pick(index: int, action: int, check: int = 0) -> bytes:
    """index (u8), action (u8), check (u16be): four bytes.

    ``check`` is remote_row_check() of the row at ``index`` as the card was
    sent it; NEW names no row, so it travels as index 0xFF with check 0.
    """
    if action == REMOTE_ACTION_NEW:
        if index != REMOTE_NEW_INDEX:
            raise ProtocolError("a new session is picked with index 0xFF")
        if check != 0:
            raise ProtocolError("a new session is picked with check 0")
    elif action in (REMOTE_ACTION_OPEN, REMOTE_ACTION_COPY):
        if not 0 <= index < REMOTE_ROWS_MAX:
            raise ProtocolError(f"index {index} outside 0..{REMOTE_ROWS_MAX - 1}")
        if not 0 <= check <= 0xFFFF:
            raise ProtocolError(f"row check {check} outside 0..0xFFFF")
    else:
        raise ProtocolError(f"remote action {action} is not 0, 1 or 2")
    return encode(MsgType.REMOTE_PICK, struct.pack(">BBH", index, action, check))


# -- state --------------------------------------------------------------------

STATE_UNKNOWN = 0
STATE_IDLE = 1
STATE_WORKING = 2
STATE_NEEDS_APPROVAL = 3
STATE_NEEDS_INPUT = 4
_STATE_MAX = STATE_NEEDS_INPUT
STATE_DETAIL_MAX = 24


# STATE's tail for a card that announced "ask": the open question's id
# (u32be) and one flags byte.
ASK_TAIL = 5
# Flag: the last answer or Enter from this card was not delivered, because
# the question it was meant for is gone or had not reached the card yet.
ASK_FLAG_DROPPED = 0x01


def encode_state(state: int, detail: str = "", ask: tuple[int, int] | None = None) -> bytes:
    """state (u8), detail_len (u8), detail: what the session on screen is doing.

    ``detail`` is Claude Code's own ``waitingFor`` while it waits, and empty
    otherwise; printable ASCII, at most 24 bytes, refused rather than clamped
    for the reason _remote_text gives.

    ``ask`` is (ask id, flags), only for a card that announced "ask": the id
    of the question an ANSWER must name, and ASK_FLAG_DROPPED.
    """
    if not 0 <= state <= _STATE_MAX:
        raise ProtocolError(f"state {state} outside 0..{_STATE_MAX}")
    raw = _remote_text(detail, STATE_DETAIL_MAX, "state detail")
    body = bytes((state, len(raw))) + raw
    if ask is not None:
        ask_id, flags = ask
        if not 0 <= ask_id <= 0xFFFFFFFF or not 0 <= flags <= 0xFF:
            raise ProtocolError(f"ask id {ask_id} or flags {flags} out of range")
        body += struct.pack(">IB", ask_id, flags)
    return encode(MsgType.STATE, body)


def decode_state(payload: bytes) -> tuple[int, str]:
    state, detail, _ = decode_state_ask(payload)
    return state, detail


def decode_state_ask(payload: bytes) -> tuple[int, str, tuple[int, int] | None]:
    """(state, detail, (ask id, flags) or None)."""
    if len(payload) < 2:
        raise ProtocolError(f"state payload too short: {len(payload)}")
    state, length = payload[0], payload[1]
    if state > _STATE_MAX:
        raise ProtocolError(f"state {state} outside 0..{_STATE_MAX}")
    if length > STATE_DETAIL_MAX:
        raise ProtocolError(f"state detail_len {length} above {STATE_DETAIL_MAX}")
    if len(payload) not in (2 + length, 2 + length + ASK_TAIL):
        raise ProtocolError("declared state detail length does not match the payload")
    detail = payload[2:2 + length]
    if any(b < 0x20 or b > 0x7E for b in detail):
        raise ProtocolError("state detail must be printable ASCII")
    ask = None
    if len(payload) == 2 + length + ASK_TAIL:
        ask = struct.unpack(">IB", payload[2 + length:])
    return state, detail.decode("ascii"), ask


# Every frame the server sends fits one TCP segment of the DS's 536-byte MSS
# once sealed: frames spanning full-size segments did not survive its stack
# (server.py, CELLS_PER_MESSAGE).
SEGMENT_MAX = 536

# COMMANDS: at most this many names, each "/" and up to 30 characters from a
# narrow set, and no more of them than fit in one sealed segment -- eighteen
# of the longest would not. The card keeps six of its own beside them.
COMMANDS_MAX = 18
COMMAND_NAME_MAX = 31
COMMANDS_PAYLOAD_MAX = SEGMENT_MAX - HEADER_SIZE - SEAL_TAG_SIZE   # 522
_COMMAND_NAME = re.compile(r"^/[A-Za-z0-9_][A-Za-z0-9_.:-]*$")


def encode_commands(names) -> bytes:
    """The names, newline-separated, in one frame. Refused rather than
    trimmed: commands.py only ever offers names that pass."""
    names = list(names)
    if len(names) > COMMANDS_MAX:
        raise ProtocolError(f"{len(names)} commands, at most {COMMANDS_MAX}")
    for name in names:
        if not isinstance(name, str) or len(name) > COMMAND_NAME_MAX or \
                not _COMMAND_NAME.match(name):
            raise ProtocolError(f"not a command name: {name!r}")
    payload = "\n".join(names).encode("ascii")
    if len(payload) > COMMANDS_PAYLOAD_MAX:
        raise ProtocolError(f"commands payload {len(payload)} exceeds "
                            f"{COMMANDS_PAYLOAD_MAX}: it would not fit one segment")
    return encode(MsgType.COMMANDS, payload)


def decode_commands(payload: bytes) -> tuple[str, ...]:
    if not payload:
        return ()
    try:
        names = payload.decode("ascii").split("\n")
    except UnicodeDecodeError as exc:
        raise ProtocolError("commands must be ASCII") from exc
    if len(names) > COMMANDS_MAX:
        raise ProtocolError(f"{len(names)} commands, at most {COMMANDS_MAX}")
    for name in names:
        if len(name) > COMMAND_NAME_MAX or not _COMMAND_NAME.match(name):
            raise ProtocolError(f"not a command name: {name!r}")
    return tuple(names)


def decode_remote_pick(payload: bytes) -> tuple[int, int, int]:
    """(index, action, check), checked for length only.

    Deliberately not more, like decode_session_pick: whether an index names a
    row, whether the check is that row's, and whether that row allows the
    action are questions about what THIS connection was last sent, and the
    server answers them with a REMOTE_NOTE. A well-formed pick of a row that
    has gone away, or moved, is ordinary, not hostile.

    The old two-byte pick is a malformed frame, not a legacy to honour: the
    card and the server ship together and nothing older speaks REMOTE, and a
    pick without a check is exactly the pick that can open the wrong session.
    """
    if len(payload) != REMOTE_PICK_SIZE:
        raise ProtocolError(
            f"remote pick must be {REMOTE_PICK_SIZE} bytes, got {len(payload)}")
    index, action, check = struct.unpack(">BBH", payload)
    return index, action, check


def encode_cursor(row: int, col: int, visible: bool) -> bytes:
    if not (0 <= row < ROWS) or not (0 <= col < COLS):
        raise ProtocolError(f"cursor position ({row},{col}) outside {COLS}x{ROWS}")
    return encode(MsgType.CURSOR, bytes((row, col, 1 if visible else 0)))


def decode_cursor(payload: bytes) -> tuple[int, int, bool]:
    if len(payload) != CURSOR_SIZE:
        raise ProtocolError(f"cursor payload must be {CURSOR_SIZE} bytes")
    row, col, visible = payload
    if row >= ROWS or col >= COLS:
        raise ProtocolError(f"cursor position ({row},{col}) outside {COLS}x{ROWS}")
    return (row, col, bool(visible))


# -- input -----------------------------------------------------------------

# The DS sends raw key bytes and nothing else. There is deliberately no message
# type that names a file, a command, or a path: the PTY is the entire interface,
# so a compromised client can only type what a person sitting at the keyboard
# could type.
MAX_KEY_PAYLOAD = 64


def encode_key(data: bytes) -> bytes:
    if not data:
        raise ProtocolError("empty key payload")
    if len(data) > MAX_KEY_PAYLOAD:
        raise ProtocolError(f"key payload {len(data)} exceeds {MAX_KEY_PAYLOAD}")
    return encode(MsgType.KEY, data)


def decode_key(payload: bytes) -> bytes:
    if not payload:
        raise ProtocolError("empty key payload")
    if len(payload) > MAX_KEY_PAYLOAD:
        raise ProtocolError(f"key payload {len(payload)} exceeds {MAX_KEY_PAYLOAD}")
    return payload


def encode_answer(ask_id: int, data: bytes) -> bytes:
    """ANSWER: u32be ask id, then the key bytes, bounded as KEY's are."""
    if not 0 <= ask_id <= 0xFFFFFFFF:
        raise ProtocolError(f"ask id {ask_id} out of range")
    return encode(MsgType.ANSWER, struct.pack(">I", ask_id) + encode_key(data)[HEADER.size:])


def decode_answer(payload: bytes) -> tuple[int, bytes]:
    if len(payload) < 5:
        raise ProtocolError(f"answer payload too short: {len(payload)}")
    return struct.unpack(">I", payload[:4])[0], decode_key(payload[4:])


# -- device identity and pairing ----------------------------------------------

# A card that has been paired announces which device it is with an "id=" token
# in HELLO, eight lowercase hex digits. Like every token it is inert to a
# server that does not know it, and it grants nothing: it only says which
# per-device key the AUTH that follows should be checked against.
DEVICE_ID_BYTES = 4
_DEVICE_TOKEN = "id="


def is_device_id(text: str) -> bool:
    return (len(text) == DEVICE_ID_BYTES * 2 and
            all(c in "0123456789abcdef" for c in text))


# `grid=COLSxROWS`: the geometry the card was built for.  A server set up for
# the other size refuses it by name rather than streaming cells into a grid
# of the wrong shape.  Absent from cards older than the token, which are
# taken on trust as they always were.
_GRID_TOKEN = "grid="


def hello_grid(features: frozenset[str]) -> tuple[int, int] | None:
    """The (cols, rows) a HELLO announced, or None.  Malformed reads as
    absent, never as a reason to fail the HELLO."""
    for token in features:
        if token.startswith(_GRID_TOKEN):
            cols, _, rows = token[len(_GRID_TOKEN):].partition("x")
            if cols.isdigit() and rows.isdigit() and len(cols) <= 3 and len(rows) <= 3:
                return int(cols), int(rows)
    return None


def hello_device_id(features: frozenset[str]) -> str | None:
    """The device id a HELLO announced, or None. A malformed id is ignored
    rather than refused, for the same reason every token is: an unauthenticated
    peer must not be able to make the HELLO itself fail."""
    for token in features:
        if token.startswith(_DEVICE_TOKEN):
            candidate = token[len(_DEVICE_TOKEN):]
            if is_device_id(candidate):
                return candidate
    return None


PAIR_PUBLIC_SIZE = 32
PAIR_NONCE_SIZE = 16
PAIR_COMMIT_SIZE = 32
PAIR_MAC_SIZE = 32


def encode_pair_hello(device_id: bytes, public: bytes) -> bytes:
    if len(device_id) != DEVICE_ID_BYTES or len(public) != PAIR_PUBLIC_SIZE:
        raise ProtocolError("pair hello is a 4-byte id and a 32-byte key")
    return encode(MsgType.PAIR_HELLO, device_id + public)


def decode_pair_hello(payload: bytes) -> tuple[bytes, bytes]:
    if len(payload) != DEVICE_ID_BYTES + PAIR_PUBLIC_SIZE:
        raise ProtocolError(f"pair hello must be 36 bytes, got {len(payload)}")
    return payload[:DEVICE_ID_BYTES], payload[DEVICE_ID_BYTES:]


def encode_pair_reply(public: bytes, commit: bytes) -> bytes:
    if len(public) != PAIR_PUBLIC_SIZE or len(commit) != PAIR_COMMIT_SIZE:
        raise ProtocolError("pair reply is a 32-byte key and a 32-byte commitment")
    return encode(MsgType.PAIR_REPLY, public + commit)


def decode_pair_reply(payload: bytes) -> tuple[bytes, bytes]:
    if len(payload) != PAIR_PUBLIC_SIZE + PAIR_COMMIT_SIZE:
        raise ProtocolError(f"pair reply must be 64 bytes, got {len(payload)}")
    return payload[:PAIR_PUBLIC_SIZE], payload[PAIR_PUBLIC_SIZE:]


def decode_pair_nonce(payload: bytes) -> bytes:
    if len(payload) != PAIR_NONCE_SIZE:
        raise ProtocolError(f"pair nonce must be 16 bytes, got {len(payload)}")
    return payload


def decode_pair_mac(payload: bytes) -> bytes:
    if len(payload) != PAIR_MAC_SIZE:
        raise ProtocolError(f"pair MAC must be 32 bytes, got {len(payload)}")
    return payload


# -- camera ------------------------------------------------------------------
#
# Mirrored in dsclient/source/proto.c and dsclient/source/camera.c. The card
# sends pixels and intents -- a camera state, frames in a fixed format and
# size, chunks at offsets the server checks against its own count, a CRC, and
# an id the server issued. No client message names a file, a path or a
# command: the server alone chooses the folder and the name, and PHOTO_SAVED's
# path is COMMANDS' argument -- server text that lands only in the composer.
# Everything is bounded, and every bound below is re-checked by the decoders,
# never trusted.

# The preview: 128x96 in sixteen greys, two pixels a byte, the left one in
# the high nibble (camera.h has the exact rule). Format 1 was the first
# version's 64x48 RGB332 and is refused.
PREVIEW_W = 128
PREVIEW_H = 96
PREVIEW_FORMAT_GRAY4 = 2
PREVIEW_FLAG_STILL = 0x01
PREVIEW_FLAGS = PREVIEW_FLAG_STILL
PREVIEW_PIXELS = PREVIEW_W * PREVIEW_H            # 12288
PREVIEW_BYTES = PREVIEW_PIXELS // 2               # 6144
# The other live view, SETTINGS -> LIVE VIEW -> COLOUR: 96x72, one RGB332
# byte a pixel, no dither (camera.h).
PREVIEW_FORMAT_RGB332 = 3
COLOUR_W = 96
COLOUR_H = 72
COLOUR_BYTES = COLOUR_W * COLOUR_H                # 6912
# format -> (width, height, payload bytes)
PREVIEW_FORMATS = {
    PREVIEW_FORMAT_GRAY4: (PREVIEW_W, PREVIEW_H, PREVIEW_BYTES),
    PREVIEW_FORMAT_RGB332: (COLOUR_W, COLOUR_H, COLOUR_BYTES),
}
CAMERA_FRAME_SIZE = 3 + COLOUR_BYTES              # 6915, the larger of the two

PHOTO_W = 640
PHOTO_H = 480
# The wire never carries the sensor's YUYV: the card packs it into 4:2:0
# planar (Y, then Cb, then Cr), full size or a quarter of it.
PHOTO_FORMAT_FULL = 1                             # 640x480
PHOTO_FORMAT_SMALL = 2                            # 320x240
PHOTO_FORMATS = (PHOTO_FORMAT_FULL, PHOTO_FORMAT_SMALL)
PHOTO_BYTES_FULL = PHOTO_W * PHOTO_H * 3 // 2                     # 460800
PHOTO_BYTES_SMALL = (PHOTO_W // 2) * (PHOTO_H // 2) * 3 // 2      # 115200
PHOTO_BYTES_MAX = PHOTO_BYTES_FULL
PHOTO_BYTES = PHOTO_BYTES_MAX                     # chunks are bounded by it
PHOTO_CHUNK_MAX = 1024
PHOTO_PATH_MAX = 200


def photo_bytes(fmt: int) -> int:
    """The packed byte count of a wire format, or 0 for one that is not."""
    return {PHOTO_FORMAT_FULL: PHOTO_BYTES_FULL,
            PHOTO_FORMAT_SMALL: PHOTO_BYTES_SMALL}.get(fmt, 0)

CAMERA_OFF = 0
CAMERA_INNER = 1
CAMERA_OUTER = 2

ACK_PREVIEW = 0
ACK_PHOTO = 1

READY_FLAG_CAN_SAVE = 0x01

PHOTO_SAVED_OK = 0          # saved; id and path are set
PHOTO_SAVED_BUSY = 1        # a picture is already being saved
PHOTO_SAVED_INTEGRITY = 2   # the bytes did not match the CRC
PHOTO_SAVED_DISK = 3        # the write failed
PHOTO_SAVED_TOO_SOON = 4    # inside the rate limit
PHOTO_SAVED_BAD_NAME = 5    # a name was not printable ASCII
_PHOTO_SAVED_MAX = PHOTO_SAVED_BAD_NAME


def encode_camera(state: int) -> bytes:
    """The camera screen's one byte: 0 off, 1 inner, 2 outer."""
    if state not in (CAMERA_OFF, CAMERA_INNER, CAMERA_OUTER):
        raise ProtocolError(f"camera state {state} is not 0, 1 or 2")
    return encode(MsgType.CAMERA, bytes((state,)))


def decode_camera(payload: bytes) -> int:
    if len(payload) != 1:
        raise ProtocolError(f"camera payload must be 1 byte, got {len(payload)}")
    if payload[0] > CAMERA_OUTER:
        raise ProtocolError(f"camera state {payload[0]} is not 0, 1 or 2")
    return payload[0]


def encode_camera_frame(flags: int, seq: int, pixels: bytes,
                        fmt: int = PREVIEW_FORMAT_GRAY4) -> bytes:
    """One preview frame: format, flags, seq, then the view in that format."""
    if fmt not in PREVIEW_FORMATS:
        raise ProtocolError(f"preview format {fmt} is not one we send")
    size = PREVIEW_FORMATS[fmt][2]
    if not 0 <= flags <= 0xFF or flags & ~PREVIEW_FLAGS:
        raise ProtocolError(f"preview flags {flags} contain reserved bits")
    if not 0 <= seq <= 0xFF:
        raise ProtocolError(f"preview seq {seq} is not a byte")
    if len(pixels) != size:
        raise ProtocolError(f"preview format {fmt} must hold {size} bytes")
    return encode(MsgType.CAMERA_FRAME, bytes((fmt, flags, seq)) + pixels)


def decode_camera_frame(payload: bytes) -> tuple[int, int, int, bytes]:
    """(format, flags, seq, pixels). Only a format we know, at its exact size,
    is a frame."""
    if len(payload) < 3:
        raise ProtocolError(f"camera frame too short: {len(payload)}")
    fmt, flags, seq = payload[0], payload[1], payload[2]
    if fmt not in PREVIEW_FORMATS:
        raise ProtocolError(f"preview format {fmt} is not one we know")
    size = PREVIEW_FORMATS[fmt][2]
    if len(payload) != 3 + size:
        raise ProtocolError(
            f"camera frame format {fmt} must be {3 + size} bytes, got {len(payload)}")
    if flags & ~PREVIEW_FLAGS:
        raise ProtocolError(f"preview flags {flags} contain reserved bits")
    return fmt, flags, seq, payload[3:]


def encode_photo_chunk(offset: int, data: bytes) -> bytes:
    """u32be offset, then 1..1024 bytes. The offset must be where the
    receiver's count already is; this encoder only enforces the bounds."""
    if not 0 <= offset < PHOTO_BYTES:
        raise ProtocolError(f"photo offset {offset} outside 0..{PHOTO_BYTES - 1}")
    if not 1 <= len(data) <= PHOTO_CHUNK_MAX:
        raise ProtocolError(f"chunk length {len(data)} outside 1..{PHOTO_CHUNK_MAX}")
    if offset + len(data) > PHOTO_BYTES:
        raise ProtocolError("chunk runs past the picture")
    return encode(MsgType.PHOTO_CHUNK, struct.pack(">I", offset) + data)


def decode_photo_chunk(payload: bytes) -> tuple[int, bytes]:
    if not 5 <= len(payload) <= 4 + PHOTO_CHUNK_MAX:
        raise ProtocolError(f"photo chunk payload {len(payload)} outside 5..1028")
    offset = struct.unpack(">I", payload[:4])[0]
    data = payload[4:]
    if offset + len(data) > PHOTO_BYTES:
        raise ProtocolError("chunk runs past the picture")
    return offset, data


def encode_photo_end(fmt: int, camera: int, crc: int) -> bytes:
    """The picture is complete: format, which camera, total bytes, CRC-32."""
    total = photo_bytes(fmt)
    if not total:
        raise ProtocolError(f"photo format {fmt} is not 1 or 2")
    if camera not in (CAMERA_INNER, CAMERA_OUTER):
        raise ProtocolError(f"camera {camera} is not 1 or 2")
    if not 0 <= crc <= 0xFFFFFFFF:
        raise ProtocolError(f"crc {crc} does not fit u32")
    payload = bytes((fmt, camera)) + struct.pack(">II", total, crc)
    return encode(MsgType.PHOTO_END, payload)


def decode_photo_end(payload: bytes) -> tuple[int, int, int]:
    """(format, camera, crc). The total must be the format's exact size."""
    if len(payload) != 10:
        raise ProtocolError(f"photo end must be 10 bytes, got {len(payload)}")
    fmt, camera = payload[0], payload[1]
    total, crc = struct.unpack(">II", payload[2:10])
    want = photo_bytes(fmt)
    if not want:
        raise ProtocolError(f"photo format {fmt} is not 1 or 2")
    if camera not in (CAMERA_INNER, CAMERA_OUTER):
        raise ProtocolError(f"camera {camera} is not 1 or 2")
    if total != want:
        raise ProtocolError(f"photo total {total} is not {want}")
    return fmt, camera, crc


def encode_photo_discard(photo_id: int) -> bytes:
    """Forget one: the u16be id this server issued on this connection."""
    if not 1 <= photo_id <= 0xFFFF:
        raise ProtocolError(f"photo id {photo_id} outside 1..0xFFFF")
    return encode(MsgType.PHOTO_DISCARD, struct.pack(">H", photo_id))


def decode_photo_discard(payload: bytes) -> int:
    if len(payload) != 2:
        raise ProtocolError(f"photo discard must be 2 bytes, got {len(payload)}")
    photo_id = struct.unpack(">H", payload)[0]
    if photo_id == 0:
        raise ProtocolError("photo id 0 is not one this server issued")
    return photo_id


def encode_camera_ready(can_save: bool) -> bytes:
    """Bit0: pictures can be saved on this server. The rest are zero, and the
    decoder refuses them, so a flag added later is a coordinated change."""
    return encode(MsgType.CAMERA_READY,
                  bytes((READY_FLAG_CAN_SAVE if can_save else 0,)))


def decode_camera_ready(payload: bytes) -> bool:
    if len(payload) != 1:
        raise ProtocolError(f"camera ready must be 1 byte, got {len(payload)}")
    if payload[0] & ~READY_FLAG_CAN_SAVE:
        raise ProtocolError(f"camera ready flags 0x{payload[0]:02x} are reserved")
    return bool(payload[0] & READY_FLAG_CAN_SAVE)


def encode_camera_ack(kind: int, value: int) -> bytes:
    """kind, u32be value: the preview seq, or photo bytes received so far."""
    if kind not in (ACK_PREVIEW, ACK_PHOTO):
        raise ProtocolError(f"ack kind {kind} is not 0 or 1")
    if kind == ACK_PREVIEW and not 0 <= value <= 0xFF:
        raise ProtocolError(f"preview ack value {value} is not a seq byte")
    if not 0 <= value <= 0xFFFFFFFF:
        raise ProtocolError(f"ack value {value} does not fit u32")
    return encode(MsgType.CAMERA_ACK, bytes((kind,)) + struct.pack(">I", value))


def decode_camera_ack(payload: bytes) -> tuple[int, int]:
    if len(payload) != 5:
        raise ProtocolError(f"camera ack must be 5 bytes, got {len(payload)}")
    kind = payload[0]
    value = struct.unpack(">I", payload[1:5])[0]
    if kind not in (ACK_PREVIEW, ACK_PHOTO):
        raise ProtocolError(f"ack kind {kind} is not 0 or 1")
    # A preview ack names a seq, which is one byte on the wire it answers.
    if kind == ACK_PREVIEW and value > 0xFF:
        raise ProtocolError(f"preview ack value {value} is not a seq byte")
    return kind, value


def encode_photo_saved(status: int, photo_id: int, path: str) -> bytes:
    """The server's answer to a completed picture.

    Status 0 carries the id this connection was given and the path the file
    was saved under -- server text that lands only in the composer, checked
    printable here and again by the card. Every other status is a bare
    status: no id, no path.
    """
    if not 0 <= status <= _PHOTO_SAVED_MAX:
        raise ProtocolError(f"photo saved status {status} outside 0..{_PHOTO_SAVED_MAX}")
    if status == PHOTO_SAVED_OK:
        if not 1 <= photo_id <= 0xFFFF:
            raise ProtocolError("a saved photo needs the id it was issued")
        try:
            pb = path.encode("ascii")
        except (UnicodeEncodeError, AttributeError) as exc:
            raise ProtocolError("photo path is not ASCII") from exc
        if not 1 <= len(pb) <= PHOTO_PATH_MAX:
            raise ProtocolError(f"photo path length {len(pb)} outside 1..{PHOTO_PATH_MAX}")
        if any(b < 0x20 or b > 0x7E for b in pb):
            raise ProtocolError("photo path must be printable ASCII")
        return encode(MsgType.PHOTO_SAVED,
                      bytes((status,)) + struct.pack(">H", photo_id)
                      + bytes((len(pb),)) + pb)
    if photo_id != 0 or path != "":
        raise ProtocolError("an unsaved photo carries no id and no path")
    return encode(MsgType.PHOTO_SAVED, bytes((status, 0, 0, 0)))


def decode_photo_saved(payload: bytes) -> tuple[int, int, str]:
    """(status, photo_id, path)."""
    if len(payload) < 4:
        raise ProtocolError(f"photo saved payload too short: {len(payload)}")
    status = payload[0]
    photo_id = struct.unpack(">H", payload[1:3])[0]
    path_len = payload[3]
    if status > _PHOTO_SAVED_MAX:
        raise ProtocolError(f"photo saved status {status} outside 0..{_PHOTO_SAVED_MAX}")
    if len(payload) != 4 + path_len:
        raise ProtocolError("declared path length does not match the payload")
    body = payload[4:]
    if status == PHOTO_SAVED_OK:
        if photo_id == 0:
            raise ProtocolError("a saved photo carries no id")
        if not 1 <= path_len <= PHOTO_PATH_MAX:
            raise ProtocolError(f"photo path length {path_len} outside 1..{PHOTO_PATH_MAX}")
        if any(b < 0x20 or b > 0x7E for b in body):
            raise ProtocolError("photo path must be printable ASCII")
        return status, photo_id, body.decode("ascii")
    if photo_id != 0 or path_len != 0:
        raise ProtocolError("an unsaved photo carries no id and no path")
    return status, 0, ""


# -- the bottom screen, for `dsterm view` -------------------------------------
#
# SUBSCREEN: cols, rows, flags (bit0: the card's VIEW TAPS is on), then each
# cell row-major as glyph, flags (bit0: the camera's viewfinder is under it),
# fg and bg as RGB15 (u16be). The card records it as it draws (mirror.h); the
# server only ever shows it to a view on this machine, and nothing in it is
# acted on -- it is pixels described as text.

SUBSCREEN_COLS = 42
SUBSCREEN_ROWS = 24
SUBSCREEN_CELL_BYTES = 6
SUBSCREEN_SIZE = 3 + SUBSCREEN_COLS * SUBSCREEN_ROWS * SUBSCREEN_CELL_BYTES  # 6051
SUBSCREEN_FLAG_TAPS = 0x01
MIRROR_FLAG_CAMERA = 0x01


@dataclass(frozen=True, slots=True)
class SubCell:
    char: str
    fg: int          # RGB15
    bg: int          # RGB15
    camera: bool


@dataclass(frozen=True, slots=True)
class SubScreen:
    taps: bool
    cells: tuple[tuple[SubCell, ...], ...]


def rgb15_to_rgb(v: int) -> tuple[int, int, int]:
    """The DS's RGB15 (R in bits 0-4) as 8-bit RGB."""
    r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def encode_subscreen_payload(cells, flags: int = 0) -> bytes:
    """For tests and the cross-check: rows of (glyph, flags, fg, bg)."""
    out = bytearray((SUBSCREEN_COLS, SUBSCREEN_ROWS, flags))
    for row in cells:
        for glyph, cflags, fg, bg in row:
            out += bytes((glyph, cflags, fg >> 8, fg & 0xFF, bg >> 8, bg & 0xFF))
    return bytes(out)


def decode_subscreen(payload: bytes) -> SubScreen:
    if len(payload) != SUBSCREEN_SIZE:
        raise ProtocolError(f"subscreen must be {SUBSCREEN_SIZE} bytes, got {len(payload)}")
    if payload[0] != SUBSCREEN_COLS or payload[1] != SUBSCREEN_ROWS:
        raise ProtocolError(f"subscreen is {payload[0]}x{payload[1]}, not "
                            f"{SUBSCREEN_COLS}x{SUBSCREEN_ROWS}")
    if payload[2] & ~SUBSCREEN_FLAG_TAPS:
        raise ProtocolError(f"subscreen flags {payload[2]} contain reserved bits")
    rows = []
    at = 3
    for _ in range(SUBSCREEN_ROWS):
        row = []
        for _ in range(SUBSCREEN_COLS):
            glyph, cflags = payload[at], payload[at + 1]
            fg = (payload[at + 2] << 8 | payload[at + 3]) & 0x7FFF
            bg = (payload[at + 4] << 8 | payload[at + 5]) & 0x7FFF
            row.append(SubCell(_glyph_char(glyph), fg, bg,
                               bool(cflags & MIRROR_FLAG_CAMERA)))
            at += SUBSCREEN_CELL_BYTES
        rows.append(tuple(row))
    return SubScreen(bool(payload[2] & SUBSCREEN_FLAG_TAPS), tuple(rows))


def encode_mirror(watching: bool) -> bytes:
    return encode(MsgType.MIRROR, bytes((1 if watching else 0,)))


def encode_touch(x: int, y: int) -> bytes:
    if not (0 <= x <= 255 and 0 <= y <= 191):
        raise ProtocolError(f"a tap at ({x}, {y}) is off the bottom screen")
    return encode(MsgType.TOUCH, bytes((x, y)))


# -- Push-to-Talk microphone (voice.py) ---------------------------------------

VOICE_FORMAT_IMA_ADPCM_8K = 1
VOICE_SAMPLE_RATE = 8000
VOICE_BYTES_MAX = 60000
VOICE_CHUNK_MAX = 480
VOICE_LANG_MAX = 7
VOICE_TEXT_MAX = 480

MIC_READY_CAN_TRANSCRIBE = 0x01

VOICE_STATUS_OK = 0
VOICE_STATUS_BUSY = 1
VOICE_STATUS_INTEGRITY = 2
VOICE_STATUS_NO_STT = 3
VOICE_STATUS_EMPTY = 4
_VOICE_STATUS_MAX = VOICE_STATUS_EMPTY

# fullmatch, not match: "$" also matches before a final newline, so "en\n"
# would pass a match against "^...$".
_VOICE_LANG_RE = re.compile(r"[A-Za-z0-9_-]{0,7}")


def encode_voice_start(fmt: int = VOICE_FORMAT_IMA_ADPCM_8K, lang: str = "en") -> bytes:
    if fmt != VOICE_FORMAT_IMA_ADPCM_8K:
        raise ProtocolError(f"voice format {fmt} is not 1")
    if not isinstance(lang, str) or not _VOICE_LANG_RE.fullmatch(lang):
        raise ProtocolError(f"invalid voice language {lang!r}")
    lb = lang.encode("ascii")
    return encode(MsgType.VOICE_START, bytes((fmt, len(lb))) + lb)


def decode_voice_start(payload: bytes) -> tuple[int, str]:
    if len(payload) < 2:
        raise ProtocolError(f"voice start payload too short: {len(payload)}")
    fmt, lang_len = payload[0], payload[1]
    if fmt != VOICE_FORMAT_IMA_ADPCM_8K:
        raise ProtocolError(f"voice format {fmt} is not 1")
    if lang_len > VOICE_LANG_MAX or len(payload) != 2 + lang_len:
        raise ProtocolError("voice start length mismatch")
    try:
        lang = payload[2:].decode("ascii")
    except UnicodeDecodeError as exc:
        raise ProtocolError("voice language must be ASCII") from exc
    if not _VOICE_LANG_RE.fullmatch(lang):
        raise ProtocolError(f"invalid voice language {lang!r}")
    return fmt, lang.lower()


def encode_voice_chunk(offset: int, data: bytes) -> bytes:
    if not 0 <= offset < VOICE_BYTES_MAX:
        raise ProtocolError(f"voice offset {offset} outside 0..{VOICE_BYTES_MAX - 1}")
    if not 1 <= len(data) <= VOICE_CHUNK_MAX:
        raise ProtocolError(f"voice chunk {len(data)} outside 1..{VOICE_CHUNK_MAX}")
    if offset + len(data) > VOICE_BYTES_MAX:
        raise ProtocolError("voice chunk runs past the maximum utterance")
    return encode(MsgType.VOICE_CHUNK, struct.pack(">I", offset) + data)


def decode_voice_chunk(payload: bytes) -> tuple[int, bytes]:
    if not 5 <= len(payload) <= 4 + VOICE_CHUNK_MAX:
        raise ProtocolError(f"voice chunk payload {len(payload)} outside 5..{4 + VOICE_CHUNK_MAX}")
    offset = struct.unpack(">I", payload[:4])[0]
    data = payload[4:]
    if offset + len(data) > VOICE_BYTES_MAX:
        raise ProtocolError("voice chunk runs past the maximum utterance")
    return offset, data


def encode_voice_end(total: int, crc: int) -> bytes:
    if not 1 <= total <= VOICE_BYTES_MAX:
        raise ProtocolError(f"voice total {total} outside 1..{VOICE_BYTES_MAX}")
    if not 0 <= crc <= 0xFFFFFFFF:
        raise ProtocolError(f"voice crc {crc} does not fit u32")
    return encode(MsgType.VOICE_END, struct.pack(">II", total, crc))


def decode_voice_end(payload: bytes) -> tuple[int, int]:
    if len(payload) != 8:
        raise ProtocolError(f"voice end must be 8 bytes, got {len(payload)}")
    total, crc = struct.unpack(">II", payload)
    if not 1 <= total <= VOICE_BYTES_MAX:
        raise ProtocolError(f"voice total {total} outside 1..{VOICE_BYTES_MAX}")
    return total, crc


def encode_mic_ready(can_transcribe: bool) -> bytes:
    return encode(MsgType.MIC_READY,
                  bytes((MIC_READY_CAN_TRANSCRIBE if can_transcribe else 0,)))


def decode_mic_ready(payload: bytes) -> bool:
    if len(payload) != 1:
        raise ProtocolError(f"mic ready must be 1 byte, got {len(payload)}")
    if payload[0] & ~MIC_READY_CAN_TRANSCRIBE:
        raise ProtocolError(f"mic ready flags 0x{payload[0]:02x} are reserved")
    return bool(payload[0] & MIC_READY_CAN_TRANSCRIBE)


def encode_voice_ack(acked: int) -> bytes:
    if not 0 <= acked <= VOICE_BYTES_MAX:
        raise ProtocolError(f"voice ack {acked} outside 0..{VOICE_BYTES_MAX}")
    return encode(MsgType.VOICE_ACK, struct.pack(">I", acked))


def decode_voice_ack(payload: bytes) -> int:
    if len(payload) != 4:
        raise ProtocolError(f"voice ack must be 4 bytes, got {len(payload)}")
    acked = struct.unpack(">I", payload)[0]
    if acked > VOICE_BYTES_MAX:
        raise ProtocolError(f"voice ack {acked} exceeds {VOICE_BYTES_MAX}")
    return acked


def encode_voice_text(status: int, text: str = "") -> bytes:
    if not 0 <= status <= _VOICE_STATUS_MAX:
        raise ProtocolError(f"voice status {status} outside 0..{_VOICE_STATUS_MAX}")
    if status == VOICE_STATUS_OK:
        try:
            tb = text.encode("ascii")
        except UnicodeEncodeError as exc:
            raise ProtocolError("voice text is not ASCII") from exc
        if not 1 <= len(tb) <= VOICE_TEXT_MAX:
            raise ProtocolError(f"voice text length {len(tb)} outside 1..{VOICE_TEXT_MAX}")
        if any(b != 0x0A and (b < 0x20 or b > 0x7E) for b in tb):
            raise ProtocolError("voice text must be printable ASCII or newline")
        return encode(MsgType.VOICE_TEXT, bytes((status,)) + struct.pack(">H", len(tb)) + tb)
    if text != "":
        raise ProtocolError("non-OK voice status carries no text")
    return encode(MsgType.VOICE_TEXT, bytes((status, 0, 0)))


def decode_voice_text(payload: bytes) -> tuple[int, str]:
    if len(payload) < 3:
        raise ProtocolError(f"voice text payload too short: {len(payload)}")
    status = payload[0]
    text_len = struct.unpack(">H", payload[1:3])[0]
    if status > _VOICE_STATUS_MAX:
        raise ProtocolError(f"voice status {status} outside 0..{_VOICE_STATUS_MAX}")
    if len(payload) != 3 + text_len:
        raise ProtocolError("declared voice text length does not match payload")
    body = payload[3:]
    if status == VOICE_STATUS_OK:
        if not 1 <= text_len <= VOICE_TEXT_MAX:
            raise ProtocolError(f"voice text length {text_len} outside 1..{VOICE_TEXT_MAX}")
        if any(b != 0x0A and (b < 0x20 or b > 0x7E) for b in body):
            raise ProtocolError("voice text must be printable ASCII or newline")
        return status, body.decode("ascii")
    if text_len != 0:
        raise ProtocolError("non-OK voice status carries no text")
    return status, ""

