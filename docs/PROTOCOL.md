# Wire protocol

This is the reference for everything that crosses the network between the
handheld and the server. There are two implementations, and both are
authoritative only together: `dsclient/source/proto.h` and `proto.c` on the
card, and `dsterm/protocol.py` on the server. The last section explains how
they are kept in agreement. For how the pieces fit together, see
[ARCHITECTURE.md](ARCHITECTURE.md).

## Principles

- **The handheld gets the PTY and nothing else.** No message runs a command
  or reads a file, and no message from the card names a path. (`PHOTO_SAVED`,
  from the server, carries the path of a picture the server itself chose; it
  lands only in the card's composer.) A client fully under an attacker's
  control can type only what a person at the keyboard could type. `KEY` is
  raw bytes for the PTY; every other client message is an intent the server
  resolves against state it already holds (a line count, an index into a
  list it sent), or pixels the server stores under a name it picks.
- **The handshake is one-way.** The card announces capability tokens in
  `HELLO`; the server announces nothing back. Everything optional is either
  gated on a token or proved by the server sending it first.
- **An unknown type is fatal; an unknown token is inert.** Both frame readers
  drop the connection on a type they do not know, because a desynchronised
  stream cannot be recovered. Tokens are ignored by whoever does not know them.
- **Length fields are never trusted.** The DS has no MMU, so a length taken on
  trust is memory corruption with no backstop. Every length is checked before
  anything is buffered or written.
- **Authenticated, then encrypted and signed.** After the handshake every
  frame is encrypted with ChaCha20 and tagged (SEAL, below). What still
  crosses in the clear: the handshake, and each frame's type and length.

## Transport

| | |
|---|---|
| Protocol | TCP, server port 8347, `TCP_NODELAY` on the server |
| Discovery | UDP 8346, outside the framed protocol (see [Discovery](#discovery)) |
| Connections | At most two at once: the live session and one challenger. A third gets `ERROR "busy"` and is closed |
| Authentication timeout | 30 seconds from accept, then the connection is dropped (a connection mid-pairing is held while its pairing window is open instead) |
| Message size on the wire | Every server-to-client message fits within 536 bytes sealed, the TCP MSS the DS reports; a 3 KB frame was observed never to be acknowledged. `tests/test_protocol.py` builds each type at its largest legal size and checks it, and `tests/test_server.py` checks a real repaint's frames, sealed. An extension's `EXTENSION` body is the extension's to keep inside the same limit |

## Framing

Every message, in both directions, is one frame:

```
+--------+---------+------+--------+---------+
| magic  | version | type | length | payload |
| 2 (DS) |    1    |  1   |   2    |  0..N   |
+--------+---------+------+--------+---------+
```

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | magic | ASCII `DS` (0x44 0x53) |
| 2 | 1 | version | 1 |
| 3 | 1 | type | see [Messages](#messages) |
| 4 | 2 | length | payload length, big-endian, at most 8192 |
| 6 | length | payload | |

All integers are big-endian. Readers obey these rules:

- One `recv()` is not one message. Both sides accumulate bytes and pop whole
  frames (`FrameReader` in Python, `framereader_t` in C).
- A length over 8192 is refused from the header alone, before the body is
  waited for, so six hostile bytes cannot pin the receive buffer.
- Bad magic, an unsupported version, an unknown type or an oversized length
  ends the connection. Neither side tries to resynchronise mid-stream; the
  card reconnects, which costs about a second.
- A malformed payload of a known type is handled differently by each side.
  The server treats it as a protocol error and drops the connection. The card
  logs it, discards that message, and keeps the link, since the framing itself
  is still intact.

## Cells

The screen is `COLS x ROWS` cells, 42x24 by default. Both ends are built or
configured for the same geometry (`PROTO_COLS`/`PROTO_ROWS` on the card,
`DSTERM_COLS`/`DSTERM_ROWS` on the server); there is no runtime negotiation of
size. A cell is three bytes:

| Byte | Meaning |
|---|---|
| glyph | `0x20`-`0x7E`: ASCII. `0x80` upward (currently `0x80`-`0xB9`): the extended table in `tools/extglyphs.py` (box drawing, blocks, marks). Anything the server cannot represent is sent as `?` |
| attr | foreground palette index in the high nibble, background in the low nibble |
| flags | bit 0 bold, bit 1 reverse |

Palette indices, which are the wire encoding and are only ever appended to:
0 default, 1 black, 2 red, 3 green, 4 yellow, 5 blue, 6 magenta, 7 cyan,
8 white, 9 orange.

## Messages

`Gate` is what must be true before the message may be sent: a HELLO token the
card announced, or proof that the server understands it (see
[the capability proof rule](#the-capability-proof-rule)).

### Client to server

| Code | Name | Gate | Payload |
|---|---|---|---|
| 0x01 | `HELLO` | allowed before auth | ASCII capability tokens, space-separated |
| 0x02 | `AUTH` | allowed before auth | `HMAC-SHA256(key, nonce)`, 32 bytes |
| 0x10 | `KEY` | | 1 to 64 raw bytes for the PTY |
| 0x11 | `RESYNC` | | empty: send me the whole screen |
| 0x12 | `PING` | | empty; answered with `PONG` |
| 0x13 | `VIEW` | | int16: move my view this many lines (negative is back) |
| 0x15 | `SESSION_PICK` | a `SESSIONS` list received | u8 index into the last list |
| 0x16 | `PAIR_HELLO` | allowed before auth | device id (4) and X25519 public key (32) |
| 0x17 | `PAIR_CONFIRM` | allowed before auth | the card's 16-byte nonce |
| 0x18 | `REMOTE_OPEN` | `remote`, and a `REMOTE` frame received | u8: 1 the screen opened, 0 it closed |
| 0x19 | `REMOTE_PICK` | `remote`, and a `REMOTE` frame received | u8 row index, u8 action, u16 check of the row's name and detail |
| 0x1A | `PAIR_ACCEPT` | allowed before auth | 32-byte MAC: the person at the DS said the codes match |
| 0x1B | `CAMERA` | `camera`, and a `CAMERA_READY` received | u8: 0 off, 1 inner, 2 outer |
| 0x1C | `CAMERA_FRAME` | `camera`, and a `CAMERA_READY` received | format, flags, seq, then the live view: format 2, 128x96 in 16 greys two pixels a byte (6,147 bytes), or format 3, 96x72 RGB332 (6,915 bytes) |
| 0x1D | `PHOTO_CHUNK` | `camera`, and a `CAMERA_READY` received | u32 offset, 1..1024 bytes of picture |
| 0x1E | `PHOTO_END` | `camera`, and a `CAMERA_READY` received | u8 format, u8 camera, u32 total, u32 CRC-32 |
| 0x1F | `PHOTO_DISCARD` | `camera`, and a `CAMERA_READY` received | u16 id the server issued in `PHOTO_SAVED` |
| 0x20 | `SUBSCREEN` | `mirror`, and a `MIRROR` saying a view is watching | the bottom screen as text: u8 cols, u8 rows, u8 flags, then 42x24 cells of 6 bytes (6,051 bytes) |
| 0x21 | `ANSWER` | `ask`, and a `STATE` carrying an ask id received | u32 ask id, then 1 to 64 key bytes: [Answers](#answers) |
| 0x22 | `VOICE_START` | `mic`, and a `MIC_READY` received | u8 format (1: IMA ADPCM, 8 kHz), u8 language length, language hint: [Voice](#voice) |
| 0x23 | `VOICE_CHUNK` | `mic`, and a `MIC_READY` received | u32 offset, 1..480 bytes of ADPCM |
| 0x24 | `VOICE_END` | `mic`, and a `MIC_READY` received | u32 total, u32 CRC-32 |

Before authentication the server accepts only `HELLO`, `AUTH`, `PAIR_HELLO`,
`PAIR_CONFIRM` and `PAIR_ACCEPT`; anything else drops the connection, so an
unauthenticated peer can never reach the PTY.

### Server to client

| Code | Name | Gate | Payload |
|---|---|---|---|
| 0x81 | `NONCE` | sent on accept | 32 random bytes, fresh per connection |
| 0x82 | `AUTH_OK` | | `seal3` (5 bytes): SEAL starts with the next frame |
| 0x83 | `AUTH_FAIL` | | empty; the connection then closes |
| 0x90 | `FRAME_FULL` | | every cell, row-major, `COLS*ROWS*3` bytes |
| 0x91 | `FRAME_DIFF` | | changed cells, 5 bytes each |
| 0x92 | `SCROLL` | | top, bottom, lines (3 bytes) |
| 0x93 | `PONG` | | empty |
| 0x94 | `CURSOR` | | row, col, visible |
| 0x95 | `VIEW_POS` | the client has sent `VIEW` | u16 lines back, u16 lines available |
| 0x96 | `MODE` | `mode` | 1 byte of flags |
| 0x97 | `SESSIONS` | none (see below) | one entry of the session list |
| 0x98 | `SNIPPETS` | `snippets` | one snippet of a pushed set |
| 0x99 | `EXTENSION` | the extension's own token | opaque; the terminal never reads it |
| 0x9A | `PROJECT` | `project` (64 rows and glyph bytes only with `project64`) | one row of the PROJECT page |
| 0x9B | `CLIP` | `clip`, and the server started with `--clip` | one chunk of the desktop clipboard |
| 0x9C | `PAIR_REPLY` | answers `PAIR_HELLO` | X25519 public key (32), commitment to the server's nonce (32) |
| 0x9D | `PAIR_OK` | answers `PAIR_CONFIRM` | the server's 16-byte nonce, which must open the commitment |
| 0x9E | `PAIR_FAIL` | | ASCII reason, at most 64 bytes; the connection closes |
| 0x9F | `REMOTE` | `remote`, unless the server runs with `--no-remote` | one row of the REMOTE list |
| 0xA0 | `REMOTE_NOTE` | `remote` | 1 to 40 printable ASCII bytes: how a pick went |
| 0xA1 | `STATE` | `state` | what the session on screen is doing |
| 0xA2 | `PAIR_DONE` | answers `PAIR_ACCEPT`, once the computer said yes too | 32-byte MAC: both people accepted |
| 0xA3 | `COMMANDS` | `commands` | the computer's slash command names for QUICK, newline-separated: at most 18, each `/` and up to 30 of letters, digits, `_ - . :` |
| 0xA4 | `CAMERA_READY` | `camera`, unless the server runs `--no-camera` | u8 flags: bit 0, pictures can be saved |
| 0xA5 | `CAMERA_ACK` | a `CAMERA_FRAME` or `PHOTO_CHUNK` received | u8 kind (0 preview, 1 photo), u32 value |
| 0xA6 | `PHOTO_SAVED` | answers `PHOTO_END` | u8 status, u16 id, u8 path length, path (status 0 only) |
| 0xA7 | `MIRROR` | `mirror` | u8: 1 a `dsterm view` is watching, 0 none is |
| 0xA8 | `TOUCH` | `mirror`, while a view is watching | u8 x (0-255), u8 y (0-191): a tap on the bottom screen |
| 0xA9 | `MIC_READY` | `mic`, unless the server runs `--no-mic` | u8 flags: bit 0, speech-to-text is available |
| 0xAA | `VOICE_ACK` | a `VOICE_CHUNK` received | u32 bytes of this utterance the server holds |
| 0xAB | `VOICE_TEXT` | answers `VOICE_END` | u8 status, u16 length, text (status 0 only: 1-480 printable ASCII) |
| 0xFF | `ERROR` | | ASCII reason, for example `busy` or `no such session` |

`tests/test_protocol.py` pins this list. Each type in it carries a written
argument for why it cannot run a command or read a file, and why nothing in
it from the card can name a path (`PHOTO_SAVED`'s path is the server's own,
and the argument says why it is harmless).

## Capability tokens

`HELLO` is ASCII: a version token first, then feature tokens, separated by
spaces. The card sends, in order, its own tokens, ` id=<hex>` if it has been
paired, and an extension's tokens (at most 32 bytes, left out whole if they do
not fit).

| Token | Meaning |
|---|---|
| `ds-term/1` | Protocol version. Always first |
| `mode` | Send `MODE` |
| `snippets` | Push the snippet set after `AUTH_OK` |
| `project` | Send `PROJECT` rows |
| `clip` | Send `CLIP` (only if the server was also started with `--clip`) |
| `remote` | Send `REMOTE` and accept `REMOTE_OPEN` / `REMOTE_PICK` |
| `state` | Send `STATE` |
| `ask` | Add the open question's id to `STATE`, and accept `ANSWER` ([Answers](#answers)) |
| `seal` | Sent alongside `seal3`, so that a server from before encryption answers `AUTH_OK` `seal`, which the card can name and refuse |
| `seal3` | Required. A `HELLO` without it is refused with `ERROR` before `AUTH_OK` |
| `cn=<hex>` | Required: the card's nonce for this connection, 16 bytes as 32 lowercase hex digits, fresh every time. It goes into the SEAL keys ([SEAL.md](SEAL.md)) |
| `commands` | Send `COMMANDS` after `AUTH_OK` and whenever the list changes |
| `camera` | The card has a camera (a DSi): send `CAMERA_READY`, accept the camera and photo messages, and answer their ACKs |
| `mirror` | Send `MIRROR` after `AUTH_OK` and whenever a view comes or goes; accept `SUBSCREEN` |
| `mic` | The card has a microphone and knows the voice types: send `MIC_READY` after `AUTH_OK` (not under `--no-mic`), accept `VOICE_START`, `VOICE_CHUNK` and `VOICE_END`, and answer them ([Voice](#voice)) |
| `project64` | The card takes `PROJECT` snapshots of up to 64 rows with the DS glyph bytes 0x80-0xBF in their text. Without it (a 1.0.x card refuses more than 48 rows and draws those bytes as `?`) the server sends at most 48 rows -- NOW, DIFF and DO first, then WATCH, HEALTH and LOG while they fit -- folded to printable ASCII |
| `grid=COLSxROWS` | The card's build geometry, so a server set up for another size can refuse the card by name rather than stream cells into the wrong shape |
| `id=<8 hex>` | This card's device id, lowercase. Selects the per-device key `AUTH` is checked against. Grants nothing |

Tokens select which optional messages the server may send. None of them grants
anything, and a malformed token from an unauthenticated peer is ignored rather
than allowed to fail the `HELLO`.

## The capability proof rule

A client never sends a new client-to-server type until the server has sent the
matching server-to-client message on the same connection.

The reason is the asymmetry above. A token is inert, so announcing one to an
old server costs nothing. A type is not: an older server's frame reader raises
on a type it has never heard of and drops the connection, and a card that
reconnects automatically would turn that into a loop. So the receipt of the
server's message is the proof:

| Client sends | Only after receiving |
|---|---|
| `SESSION_PICK` | a complete `SESSIONS` list |
| `REMOTE_OPEN`, `REMOTE_PICK` | a `REMOTE` frame (the server sends an empty "not asked yet" set right after `AUTH_OK` to a card that announced `remote`) |
| `CAMERA`, `CAMERA_FRAME`, `PHOTO_CHUNK`, `PHOTO_END`, `PHOTO_DISCARD` | `CAMERA_READY` (sent right after `AUTH_OK` to a card that announced `camera`) |
| `SUBSCREEN` | a `MIRROR` whose byte is 1 (sent to a card that announced `mirror` when a view is open) |
| `VOICE_START`, `VOICE_CHUNK`, `VOICE_END` | `MIC_READY` (sent right after `AUTH_OK` to a card that announced `mic`, unless the server runs `--no-mic`); and a card records only when its bit 0 says speech-to-text is available |

The server applies the same rule in the other direction for one message: it
sends `VIEW_POS` only to a client that has sent `VIEW` (or picked a session),
because only a build that knows `VIEW_POS` can do either.

Proof belongs to a connection: `reconnect()` clears the session list and the
REMOTE list, so a card that moves to an older server never carries a proof
across. `SESSIONS` itself has no token and was a coordinated rollout of both
halves. `PAIR_HELLO` is the one exception: pairing is started by a person, and
an old server dropping it is reported as "this server cannot pair", not
retried.

## Connection lifecycle

```
card                                         server
  | ---- TCP connect -----------------------> |
  | ---- HELLO "ds-term/1 ... seal3 id=... cn=..." -> |  sent at once
  | <--- NONCE (32 bytes) ------------------- |  sent on accept
  | ---- AUTH  HMAC-SHA256(key, nonce) -----> |
  | <--- AUTH_OK "seal3" -------------------- |  or AUTH_FAIL, then close
  |      (every frame encrypted and sealed from here, both ways)
  | <--- MODE, SNIPPETS*, FRAME_DIFF*, CURSOR, SESSIONS*, CLIP*,
  |      REMOTE (empty), STATE, COMMANDS, MIRROR, CAMERA_READY,  |  each only if
  |      MIC_READY                                |
  |                                              |  negotiated; order not fixed
  | <==> KEY, VIEW, PING / screen updates ==> |
```

- `HELLO` and `NONCE` cross in flight: the server sends `NONCE` as soon as it
  accepts, and the card sends `HELLO` without waiting for it. A card that
  sends no `HELLO` cannot authenticate: SEAL is keyed to the `HELLO`, and an
  `AUTH` without one is refused.
- `key` is the card's per-device key when `HELLO` carried an `id=` the server
  has paired, and the legacy shared key otherwise. The MAC is compared in
  constant time. A fresh nonce per connection means a captured exchange cannot
  be replayed.
- A verified `AUTH` takes over the session: any other authenticated
  connection is dropped. A handheld switched off without closing its socket
  leaves a connection the server cannot tell from a live one, and this is how
  the returning device displaces it. Only a verified MAC can do this.
- After `AUTH_OK` the server starts, only where negotiated: `MODE` (even when
  the flag is clear), the pushed snippet set, a full repaint, a forced
  `SESSIONS` snapshot, the clipboard, an empty `REMOTE` set with status 0, the
  current `STATE`, `COMMANDS`, `MIRROR`, `CAMERA_READY` and `MIC_READY`. `MODE`, the snippet
  set and the other small messages go to the socket at once; the repaint and
  the `SESSIONS`, clipboard and `REMOTE` lists are released through the paced
  outbox, four messages a tick, so they arrive later and interleaved. A card
  must not depend on the order. `PROJECT` rows follow when the server's
  collector publishes.
- A reconnect is a full repaint. The PTY outlives the connection, so a
  returning client gets the whole screen, never a diff against a state it did
  not hold.

## Payload reference

### Screen

- **`FRAME_DIFF`**: a sequence of `row, col, glyph, attr, flags`. The card
  validates every position before writing any cell, so a bad diff cannot leave
  the screen half-updated. The server sends at most 90 cells per message
  (456 bytes), and a full repaint as a series of such diffs.
- **`FRAME_FULL`**: every cell in row-major order. The card still decodes it,
  but the current server never sends it: a single 3030-byte frame was
  retransmitted past 10 KB without one acknowledgement from the DS, so a
  repaint is a series of diffs instead.
- **`SCROLL`**: u8 top, u8 bottom, i8 lines. The server sends only whole-screen
  upward scrolls (`0, ROWS-1, n` with `n > 0`): rows move up by `n`, the
  vacated rows are blanked, and a residual `FRAME_DIFF` fills them. The card
  refuses any other region. One line of scroll changes about 900 of 1008
  cells, so this replaces most of a repaint with three bytes.
- **`CURSOR`**: u8 row, u8 col, u8 visible. Hidden while the view is scrolled
  back.

### View and mode

- **`VIEW`**: int16, negative is back. The server clamps to 5000 lines (its
  history depth) rather than refusing; the card clamps as well, because a value
  above 32767 would encode as negative. Any `KEY` returns the view to live.
- **`VIEW_POS`**: u16 lines back, u16 lines available. Sent in answer to
  `VIEW`, and while the view is live at most once a second (or after a screen
  of new history) so the card learns that history has grown.
- **`MODE`**: bit 0 `APP_SCROLLS`: the program on screen owns its scrollback
  (it is on the alternate screen), so the card's scroll gestures become Page Up
  / Page Down keys instead of `VIEW`. Sent once after `AUTH_OK` and then on
  every change. Reserved bits are sent as zero and masked off by the card.

### Lists

Lists are sent one entry per frame, to stay within the 536-byte ceiling. The
card stages entries and replaces its visible list only when every index of a
set has arrived and validated.

| Message | Entry layout | Empty form | Limits |
|---|---|---|---|
| `SESSIONS` | count, index, flags, idle seconds (u16), name length, name | 1 byte: `0` | 8 entries, names 1-24 printable ASCII. Flags: 0x01 running, 0x02 active, 0x04 new output |
| `SNIPPETS` | index, total, label length, label, body length (u16), body | none; nothing is sent | 8 snippets, label 1-15, body up to 255 |
| `PROJECT` | count, index, tab, colour, label length, value length, label, value | 1 byte: `0` | 64 rows to a `project64` card, 48 otherwise; tab 0-5 (NOW, DIFF, DO, WATCH, HEALTH, LOG), colour 0-15, label 1-12, value up to 40; printable ASCII, plus the glyph bytes 0x80-0xBF (`dsterm/extmap.py`) to a `project64` card only |
| `CLIP` | count, index, seq, flags, total (u32), length, bytes | 2 bytes: `0`, flags | 6 chunks of up to 192 bytes, 1024 bytes of text, printable ASCII and newline only. Flags: 0x01 truncated, 0x02 transliterated, 0x04 withheld (empty only) |
| `REMOTE` | count, index, kind, state, flags, age (u16), name length, name, detail length, detail | 2 bytes: `0`, status | See [REMOTE.md](REMOTE.md) |

The two empty forms with a second byte exist because their meanings must not
look alike: an empty clipboard is not a clipboard withheld because a password
manager marked it, and an empty REMOTE list has to say whether there are no
sessions, `claude` is not installed, or the query failed.

`SESSION_PICK` is one byte: an index into the list this server last sent. An
index that no longer exists gets `ERROR "no such session"`, never the nearest
surviving row.

Snippets, the clipboard and PROJECT's DO buttons are text that could become
keystrokes. None of them reaches the PTY on arrival: the card loads them into
its composer, shows them in full, and sends only on a second deliberate press
([design/SNIPPETS_PUSH.md](design/SNIPPETS_PUSH.md)). The one exception is god mode, a
card-only setting that is off by default and that the server cannot set: when
armed, a DO button types its command directly. It never applies to snippets or
the clipboard. There is no request message for `PROJECT` or `CLIP`; a DO
button works by typing ordinary `KEY` bytes.

### REMOTE and STATE

`REMOTE` (0x9F) lists the Claude Code sessions on the server's machine as
`claude agents --json` reports them, with a kind (background or interactive),
a state, flags (HERE, LIVE, COPY), an age, a name and a detail. `REMOTE_NOTE`
(0xA0) is one line saying how a pick went. `REMOTE_OPEN` (0x18) tells the
server the screen is up, which is the only time it polls for the list.
`REMOTE_PICK` (0x19) is a row index, an action (0 open, 1 open a copy,
2 new session, with index 0xFF, which no row can have) and a 16-bit check:
FNV-1a over the row's name, a NUL and its detail, as they were sent, folded to
16 bits (0 for a new session). The server maps the index back to the row it
sent, refuses the pick with "the list changed; pick again" if that row's check
differs -- the list re-sorts as sessions change, so an index alone can name
the wrong one -- and re-checks the action against the row's flags; it never
takes an id, a path or a command from the wire, and it accepts one pick per
second.

`STATE` (0xA1) is `state (u8), detail length (u8), detail`: 0 unknown, 1 idle,
2 working, 3 needs approval, 4 needs input, with Claude Code's own
`waitingFor` text (at most 24 bytes) while it waits. It is sent once after
authentication and then only on change. It names nothing and grants nothing;
the worst a forged one can do is play the wrong sound. To a card that
announced `ask`, it carries five more bytes: the open question's id (u32) and
a flags byte (bit 0: this card's last Enter was not delivered). See
[Answers](#answers).

### Answers

The card has no "approve" message. A permission prompt belongs to Claude
Code, and the card answers it the way a keyboard does: A sends Enter, X sends
Escape. What `ask` adds is that an Enter can only reach the prompt it was
meant for.

Every permission prompt is a question with an id. The server opens one when
a `claude agents` reading says the session needs approval, and gives it a new
id when the selected window changes, or when the screen changes under it with
no arrow from the card in the last half second to explain it (someone answered
at the computer, and what is on screen is no longer what the card was looking
at). A question closes when an answer reaches it; a reading taken before that
answer cannot reopen it.

While `STATE` says a question is open, the card sends any key frame with an
Enter in it as `ANSWER`, naming the id it was last sent. The server writes
the bytes only if that question is still the one open, and closes it.
Otherwise it drops them, and the next `STATE` sets the flag, so the card can
say "not sent: the screen changed -- look, then press again". From an `ask`
card, a plain `KEY` with an Enter in it is held back while a question is
open: the card sends Enter as `ANSWER` once it knows of the question, so a
plain one was sent before it did. The same holds for a moment after any Enter
or Escape from the card reaches the session, until a reading taken since then
says what is on screen (at most three seconds): the next prompt may already
be up with no question open for it. That is what makes a double press safe.

So an Enter pressed for one prompt, and delayed for any reason, never answers
the next one: a double press, a frame held up on the way, or a prompt
answered at the computer while the press was in flight. Escape, arrows and
ordinary typing are never held back: declining is safe wherever it lands, and
an arrow only moves the highlight. A digit typed on the keyboard still picks
a numbered choice, as it would on any keyboard. One case is out of reach: if
someone at the computer answers a prompt and the next one appears within half
a second of an arrow pressed on the DS, the server takes that change for the
arrow moving the highlight, and both prompts share an id.
`tests/test_answers_random.py` runs the rule against random interleavings of
prompts, presses, late readings and answers at the computer, and checks that
every Enter lands on the prompt it was pressed for, outside that one case.

There is no timeout anywhere in this. A prompt waits in Claude Code until a
key reaches it, so a lost link never approves or declines anything.

The full contract, including every field table and the polling cadence, is in
[REMOTE.md](REMOTE.md).

### Camera

The camera messages cross only where a `camera` token was announced and
`CAMERA_READY` answered on this connection. The card sends pixels and intents;
the server alone picks where a picture goes.

- **`CAMERA`**: u8 state — 0 off, 1 inner, 2 outer. A change sets the
  server-wide live view and the feed the popups and `dsterm camera` read.
- **`CAMERA_FRAME`**: `format, flags, seq`, then 128x96 pixels in sixteen
  greys, two a byte with the left one in the high nibble — a 6,147-byte
  payload. `format` is 2 (4-bit grey; format 1, the first version's 64x48
  RGB332, is refused); `flags` bit 0 is STILL, the one frame that is a
  captured picture's preview rather than live view; `seq` is the byte the
  server's `CAMERA_ACK` answers. Each pixel is the 256x192 viewfinder's 2x2
  block averaged per channel in 5-bit space (`(sum + 2) >> 2`), then
  `y = (77r + 150g + 29b + 128) >> 8` and `level = (y*15 + 15) / 31`, all
  integer so both ends produce identical bytes. Format 3 is the colour view
  (SETTINGS → LIVE VIEW → COLOUR): 96x72, one RGB332 byte a pixel (R in bits
  7-5, G 4-2, B 1-0), each the area box over `[dx*256/96, (dx+1)*256/96) x
  [dy*192/72, (dy+1)*192/72)` averaged per channel in 5-bit space and
  quantized without dither, `(a*7 + 15) / 31` for R and G and `(a*3 + 15) / 31`
  for B. Either view is for aiming; the picture itself is the full-colour
  still.
- **`PHOTO_CHUNK`**: u32 offset, then 1 to 1024 bytes. Offsets must be
  contiguous and bounded by 460,800; a gap drops the connection — except
  offset 0, which restarts the picture and discards what was held.
- **`PHOTO_END`**: u8 format, u8 camera, u32 total, u32 CRC-32. Format 1,
  FULL, is 640x480 4:2:0 planar, 460,800 bytes: the YUYV luma plane
  unchanged, then a 320x240 Cb plane and a 320x240 Cr plane, each chroma the
  average of the pair's two rows `((a+b+1)>>1)`. Format 2, SMALL, is
  320x240 4:2:0 planar, 115,200 bytes: each Y the 2x2 luma block average
  `((sum+2)>>2)`, each chroma of its 160x120 planes the average of the four
  samples under it. `total` must equal the format's size, and the CRC-32 is
  the reflected polynomial 0xEDB88320 over the packed bytes, initial 0 and
  final inversion — the value `camera_crc32_update` / the server's assembler
  compute. A bad CRC answers `PHOTO_SAVED` INTEGRITY; it is a status, not a
  drop.
- **`CAMERA_ACK`**: u8 kind, u32 value. Kind 0 answers a preview and carries
  its `seq` byte; kind 1 answers chunks and carries the byte count the server
  holds — the credit the card's 8 KB window slides on. A credit outside
  `[acked, sent]` is a protocol fault.
- **`PHOTO_SAVED`**: u8 status, and on status 0 also u16 id, u8 path length,
  and the path — printable ASCII, at most 200 bytes, relative to the project
  (`.dsterm/photos/...`) or absolute when `--photo-dir` points outside it.
  Statuses: 0 OK, 1 the server is busy saving another, 2 the picture arrived
  damaged (bad CRC), 3 the folder could not be written, 4 sooner than three
  seconds after the last, 5 the folder's name is not plain ASCII.
- **`PHOTO_DISCARD`**: u16 id — the id `PHOTO_SAVED` issued, forgotten per
  connection. The composer sends it when a saved picture is cancelled instead
  of sent.
- **`CAMERA_READY`**: u8 flags; bit 0 means this server can save pictures.
  Its receipt is the proof that lets every camera message be sent at all.

Flow control: at most one preview in flight and at most one every twenty
frames — 3 fps, about 18 KB/s — so a stalled ACK costs a dropped frame, never
a queue.
Photo chunks fly 1 KB at a time under an 8 KB window, kept inside lwIP's
11,680-byte TCP send buffer so a `KEY` never waits behind more than one
window. The server ACKs at once, draws at most ten previews a second, accepts
one picture per connection every three seconds, and saves one at a time.
A camera message from a card that never negotiated it, or a preview while
the camera is off, drops the connection.

### Voice

Push-to-Talk: the card records while the person holds **R** in the composer,
and the transcript comes back into the composer. The messages cross only
where a `mic` token was announced and `MIC_READY` answered on this
connection. The card sends samples and a language hint; the server alone
decides what transcribes them, and where.

- **`MIC_READY`**: u8 flags; bit 0 means this server can transcribe. Clear
  when no local speech-to-text is installed and the cloud was not opted in
  (below), so the card says so instead of recording. Its receipt is the
  proof that lets the voice types be sent at all.
- **`VOICE_START`**: u8 format, which must be 1 (4-bit IMA ADPCM, 8,000 Hz
  mono, low nibble first), u8 length and a language hint of at most seven of
  `[A-Za-z0-9_-]`. The server keeps only two or three letters of it (`en`,
  `es`, `pt` from `pt-BR`) and drops anything else, in which case the
  language is detected. It also resets the utterance being assembled.
- **`VOICE_CHUNK`**: u32 offset, then 1 to 480 bytes of ADPCM. Offsets must
  be contiguous from 0 and the utterance stays under 60,000 bytes (15
  seconds); a gap or an overrun drops the connection. Offset 0 restarts.
- **`VOICE_ACK`**: u32, the bytes of this utterance the server holds -- the
  credit the card's upload window slides on.
- **`VOICE_END`**: u32 total, u32 CRC-32 (the camera's: reflected
  0xEDB88320, initial 0, final inversion). A total that does not match what
  arrived drops the connection; a bad CRC answers `VOICE_TEXT` INTEGRITY.
- **`VOICE_TEXT`**: u8 status, u16 length, then on status 0 only 1 to 480
  bytes of printable ASCII. Statuses: 0 OK, 1 busy (this card's last
  utterance is still being transcribed, or another card's is), 2 the audio
  arrived damaged, 3 no transcript (no speech-to-text, or it failed or ran
  out of time -- the server's log says which), 4 nothing was heard. The text
  lands in the composer, and nothing is sent until the person presses **A**.

One utterance per card is transcribed at a time, and every transcription has
a deadline: `DSTERM_STT_TIMEOUT` (15 s by default), at which a backend
process is killed, and two seconds after it the server answers the card 3
whatever the backend is doing -- inside the 20 s the card itself waits. A
Python library backend loading its model (on a first run, perhaps
downloading it) is allowed five minutes before it is killed; the card is
still answered at the usual deadline, and is answered 1, busy, until the
load ends. A transcript that arrives after the card was answered, or after it
left, is dropped rather than delivered.

Where the audio goes is fixed when the server starts, and printed: a local
backend (`DSTERM_STT_CMD`, then `mlx-whisper`, `faster-whisper`,
`openai-whisper`, then `whisper-cli`), or -- only with `dsterm serve
--stt-cloud` or `DSTERM_STT_CLOUD=1`, and only when nothing local is
installed -- OpenAI, Groq or `DSTERM_STT_URL` over https. An API key in the
environment alone never sends anything, and a failed local backend is never
followed by an upload.

### The view: SUBSCREEN, MIRROR and TOUCH

`dsterm view` shows the card's bottom screen on the computer. The card keeps
a text copy of it as it draws (`mirror.c`), and sends it only while the
server says a view is watching.

- **`MIRROR`**: u8, 1 when the first view opens and 0 when the last one
  closes; sent after `AUTH_OK` to a card that announced `mirror`, and again
  whenever that changes. A card that hears 1 sends the screen at once.
- **`SUBSCREEN`**: u8 cols (42), u8 rows (24), u8 flags (bit 0: the card's
  VIEW TAPS is on, so a click will be acted on), then each cell row-major:
  u8 glyph (a byte of the card's font, as in `FRAME_FULL`), u8 flags (bit 0:
  the camera's viewfinder is under this cell), u16 foreground and u16
  background, big-endian RGB15 with red in bits 0-4. A cell is the 6x8 block
  whose centre a glyph or fill covers. The card sends one when the screen
  changes, at most every fifteen frames (once a second while a picture
  uploads), and once every two seconds whatever happens. Any other size, shape or flag drops the connection.
- **`TOUCH`**: u8 x, u8 y, a point on the 256x192 bottom screen. The card
  holds it as a touch for four frames, exactly as a stylus tap, and only
  with its VIEW TAPS setting on; with it off, it shows a line saying so. A
  tap never arms GOD MODE.

The view socket itself, `<config dir>/run/view.sock`, is not part of the
wire: one JSON object a line, documented in `dsterm/view.py`.

### Other messages

- **`KEY`**: 1 to 64 bytes, written to the PTY unchanged. Longer text (the
  composer's 1024-byte buffer) is sent as several `KEY` frames, and a prompt's
  final CR travels in its own frame.
- **`PING` / `PONG`**: both empty, with no correlation id. The card sends one
  a second and shows the round trip on its Control screen.
- **`EXTENSION`**: server-to-client only, sent only to a card whose `HELLO`
  carried the extension's token. The card hands the body to the extension; a
  body it does not want is dropped, never a connection error.
- **`ERROR`**: an ASCII reason. The card logs it.

## SEAL

SEAL encrypts every frame after the handshake and signs it. Without it, anyone
who can see the TCP stream could read the session, or splice a `KEY` frame into
it. The contract is [SEAL.md](SEAL.md); in summary:

- It is not optional. The card announces `seal`, `seal3` and `cn=<hex>`, its
  nonce for this connection, and the server answers `AUTH_OK` with the payload
  `seal3`. A server refuses a `HELLO` without `seal3` and a well-formed `cn`
  with an `ERROR` naming the card's ROM as the one to update, never with
  `AUTH_FAIL`. A card refuses an `AUTH_OK` other than `seal3`, naming the
  computer as the one to update when it says `seal`. There is no fallback: on
  an open network, "too old" and "someone in the middle removed the word" look
  the same, so both are refused.
- `base = HMAC-SHA256(key, "dsterm seal v3" || nonce || SHA-256(HELLO payload))`,
  using the key `AUTH` was computed with, this connection's nonce, and the
  `HELLO` payload exactly as the card sent it (card nonce included). Then
  `k_mac = HMAC(base, "mac")`, `k_cs = HMAC(base, "enc C")` and
  `k_sc = HMAC(base, "enc S")`. A `HELLO` edited on the way leaves the two ends
  with different keys, so the first sealed frame ends the connection; a server
  session replayed at the card meets a new card nonce, and so new keys.
- A sealed frame's payload is encrypted, its length counts eight more bytes,
  and the tag follows:
  `ciphertext = payload XOR ChaCha20(k_dir, 0x00 * 8 || seq, counter 0)` and
  `tag = HMAC-SHA256(k_mac, dir || seq || header || ciphertext)[0:8]`, where
  `k_dir` is `k_cs` or `k_sc`, `dir` is `C` (card to server) or `S` (server to
  card), `seq` is a 32-bit counter per direction that starts at 0 after
  `AUTH_OK` and is never sent, and `header` is the six bytes as sent.
- A wrong, missing, replayed, reordered or dropped frame ends the connection.
  The tag is compared in constant time before anything is decrypted, and
  stripped, so everything above the framing sees the same payloads.
- Everything up to and including `AUTH_OK` travels in the clear: `HELLO`,
  `NONCE`, `AUTH`, `AUTH_OK` or `AUTH_FAIL`, and the pairing messages. The
  `HELLO` is covered after the fact, through the key. After `AUTH_OK` nothing
  is exempt by type, and a second `AUTH` on a sealed connection ends it.
- Each frame's header (type and length) stays in the clear, which shows when
  something is typed or changes on screen, but not what.

## Pairing

Pairing makes a 16-byte per-device key from an X25519 exchange, and two people
(usually one person at two screens) confirm it by comparing a six-digit code.
It is the numeric comparison of Bluetooth's Secure Connections. No key and no
secret ever crosses the link. The construction and the argument are in
`dsterm/pairing.py`; the card's half is `dsclient/source/pair.c`.

```
card   -> PAIR_HELLO    device id (4), X25519 public key A (32)
server -> PAIR_REPLY    X25519 public key B (32),
                        C = SHA-256("dsterm pair v2 commit" | A | B | Ns) (32)
card   -> PAIR_CONFIRM  card nonce Nc (16)
server -> PAIR_OK       server nonce Ns (16); the card checks it opens C
both:     code   = SHA-256("dsterm pair v2 code" | A | B | Nc | Ns),
                   first 4 bytes big-endian, mod 1 000 000, as six digits
          shared = X25519(own private, peer public)
          okm    = HKDF-SHA256(salt = Nc | Ns, shared,
                               info = "dsterm pair v2" | id | A | B)
          confirm key = okm[0:32], device key = okm[32:48]
person:   the same code on both screens?  A on the DS, y on the computer
card   -> PAIR_ACCEPT   HMAC-SHA256(confirm key, "dsterm pair v2 accept ds"  | A | B)
server -> PAIR_DONE     HMAC-SHA256(confirm key, "dsterm pair v2 accept srv" | A | B)
```

- The card connects, waits for `NONCE` (which proves it reached a server), and
  sends `PAIR_HELLO` without `HELLO` or `AUTH`. It closes the connection after
  `PAIR_DONE` and then connects normally with the new key.
- `dsterm pair` opens a 120-second window by writing a mode-600 file the
  server reads; there is no listening endpoint for it. The server writes the
  code into that file, `dsterm pair` shows it and asks, and the answer comes
  back the same way. One console pairs at a time.
- Nothing is saved until both people said yes. A no on either side, a broken
  commitment, an expired window or no window gets `PAIR_FAIL` with a reason
  the card shows, and the window closes.
- The commitment is what makes six digits enough. The server is bound to `Ns`
  before it sees `Nc`, and the card sends `Nc` before it sees `Ns`, so someone
  in the middle cannot steer either code toward the other. Each attempt has
  one chance in a million of matching, and there is nothing to guess offline.
- An all-zero shared secret (a low-order public key) is refused on both sides.

## Discovery

Discovery is plain UDP on port 8346, outside the framed protocol. Both
datagrams are ASCII, single-line, at most 96 bytes.

```
card   -> 255.255.255.255:8346   DSTM probe 1 [id=<hex>]
server -> card (unicast)         DSTM here 1 name=<host> port=<tcp> pair=<0|1>
```

- `name` is the short hostname reduced to letters, digits, `-` and `_`, at
  most 20 characters. `pair=1` means a pairing window is open. Nothing in the
  answer is secret, since anyone on the LAN can send the probe.
- The server ignores anything that is not a probe, and answers at most 20 a
  second so it cannot be used as a reflector. `dsterm serve
  --discovery-port 0` turns it off.
- The card parses answers strictly and reduces the name to its own alphabet
  before storing it.
- Broadcast does not cross WSL2's NAT mode, so the card can also sweep its /24
  with TCP connects to port 8347. A peer that answers with a `NONCE` frame is a
  server.

## Keeping the C and Python codecs in agreement

Two implementations of one protocol drift, and on this hardware a drift shows
up as "authentication rejected" or a garbled screen on a device that must be
reflashed to fix. Several mechanisms hold the two halves together:

| Mechanism | What it pins |
|---|---|
| `tools/crosscheck.py` | The C suite prints `INTEROP <name> <hex>` lines; the script recomputes each with the server's own code and fails on any difference. Vectors: the AUTH HMAC, a framed message, a `PROJECT` row, `CLIP` chunks (including both empty forms), the whole pairing exchange (both public keys, the device key, both MACs), the `HELLO` payload with and without `camera` (both carrying `mic` and `project64`), the discovery
probe, the voice set -- IMA ADPCM encoded and decoded on both sides, and `VOICE_START`, `VOICE_CHUNK` and `VOICE_END` as the card sends them -- and the camera set — `CAMERA`, a `CAMERA_FRAME`, a `PHOTO_CHUNK`,
`PHOTO_END`, `PHOTO_DISCARD`, the 128x96 grey preview, the 96x72 colour
preview, the YUYV conversion, the CRC-32, and both 4:2:0 packs as SHA-256;
and a `SUBSCREEN`'s head and tail |
| `dsclient/tests/test_remote.c` | `REMOTE`, `REMOTE_NOTE` and `STATE` frames encoded by `dsterm/protocol.py`, pinned as hex and decoded by the card's code; and the card's `REMOTE_PICK` compared with the server's bytes |
| `dsclient/tests/test_seal.c`, `tests/test_seal.py` | The SEAL keys, cipher and tag, computed the long way from [SEAL.md](SEAL.md), on each side; ChaCha20 against RFC 8439's vectors; and the server's own sealed frames checked byte for byte by the card's code |
| `tests/test_encryption.py` | SEAL from where an eavesdropper stands: a recording proxy on the wire finds nothing typed or shown, and a recorded session replayed at a card fails its first tag |
| `tests/test_wire.py` | The whole connection, end to end: `dsclient/tests/wire_client.c`, built from the card's own sources, logs in to a live server, turns on SEAL, reads every message through the card's frame reader and decoders, types a line and finds it on screen; and a card built for the other grid is refused by name. This is the test that sees a type the frame reader refuses, which payload-level tests cannot |
| `tests/test_snippets.py` | Reads the C header to check the snippet limits match |
| `tests/test_sounds.py` | The cue names in `dsterm/sounds.py` match the banks in `tools/sfx_build.py` |
| `tools/extglyphs.py` | One table generates both the C font and `dsterm/extmap.py`, so a glyph byte means the same thing at both ends |
| `tools/geomcheck.py` | A candidate geometry, measured on the wire before any card is flashed for it |

`dsclient/run_tests.sh` runs the C suite under sanitizers at 42x24 and 64x32
and pipes its output into `tools/crosscheck.py`; CI runs it on every push.

### Adding a message

1. Choose a code in the right range: below 0x80 for client to server, 0x80 and
   above for server to client.
2. Gate it. A server-to-client type needs a `HELLO` token; a client-to-server
   type needs a server-to-client message that proves support.
3. C: add it to `msg_type_t` in `proto.h`, to `type_is_known()` in `proto.c`
   (the card's frame reader refuses anything not listed there), a decoder or
   encoder in `proto.c`, and a case in `handle_message()` in `main.c`.
4. Python: add it to `MsgType` and write its encoder and decoder in
   `dsterm/protocol.py`, re-checking every bound on the way in; route it in
   `_handle()` (`dsterm/server.py`) to an `_on_<message>()` in the feature's
   `server_*.py`, which owns the state it touches (`tests/test_layout.py`).
5. Add it to the type list in `tests/test_protocol.py` with the argument for
   why it cannot run a command, read a file or take a path from the card. A
   server-to-client type also goes in the segment table there, at its largest
   legal size.
6. Pin it across the boundary: an `INTEROP` line and a `tools/crosscheck.py`
   entry, or a server-encoded vector decoded in a C test.
7. Document it here.
