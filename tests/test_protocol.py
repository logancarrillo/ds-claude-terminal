"""Framing tests.

The client is a C program on a device with no MMU: a length field it trusts is a
buffer overflow. These tests exist to prove the decoder never trusts one.
"""

import pytest

from dsterm.grid import COLS, ROWS, Cell, CellChange, Grid
from dsterm.protocol import (
    HEADER_SIZE,
    MAGIC,
    MAX_KEY_PAYLOAD,
    MAX_PAYLOAD,
    VERSION,
    FrameReader,
    MsgType,
    ProtocolError,
    decode_cursor,
    decode_diff,
    decode_full_frame,
    decode_key,
    decode_scroll,
    encode,
    encode_cursor,
    encode_diff,
    encode_full_frame,
    encode_key,
    encode_scroll,
    pack_cell,
    unpack_cell,
)


class TestFraming:
    def test_round_trip(self):
        raw = encode(MsgType.KEY, b"hello")
        msgs = list(FrameReader().feed(raw))
        assert len(msgs) == 1
        assert msgs[0].type is MsgType.KEY
        assert msgs[0].payload == b"hello"

    def test_empty_payload(self):
        msgs = list(FrameReader().feed(encode(MsgType.PING)))
        assert msgs[0].payload == b""

    def test_one_recv_many_messages(self):
        blob = encode(MsgType.PING) + encode(MsgType.KEY, b"ab") + encode(MsgType.PONG)
        msgs = list(FrameReader().feed(blob))
        assert [m.type for m in msgs] == [MsgType.PING, MsgType.KEY, MsgType.PONG]

    def test_byte_at_a_time_delivery(self):
        # The failure this guards against: assuming one recv() == one message.
        raw = encode(MsgType.KEY, b"fragmented across every possible boundary")
        reader = FrameReader()
        collected = []
        for i in range(len(raw)):
            collected.extend(reader.feed(raw[i : i + 1]))
        assert len(collected) == 1
        assert collected[0].payload == b"fragmented across every possible boundary"

    def test_split_mid_header(self):
        raw = encode(MsgType.KEY, b"xyz")
        reader = FrameReader()
        assert list(reader.feed(raw[:3])) == []
        msgs = list(reader.feed(raw[3:]))
        assert len(msgs) == 1

    def test_partial_message_leaves_buffer_intact(self):
        raw = encode(MsgType.KEY, b"abcdef")
        reader = FrameReader()
        assert list(reader.feed(raw[:-2])) == []
        assert reader.buffered == len(raw) - 2
        assert len(list(reader.feed(raw[-2:]))) == 1
        assert reader.buffered == 0

    def test_trailing_partial_after_complete(self):
        raw = encode(MsgType.PING) + encode(MsgType.KEY, b"zz")[:4]
        msgs = list(FrameReader().feed(raw))
        assert len(msgs) == 1


class TestHostileFrames:
    def test_bad_magic_rejected(self):
        bad = b"XX" + bytes([VERSION, int(MsgType.PING), 0, 0])
        with pytest.raises(ProtocolError, match="magic"):
            list(FrameReader().feed(bad))

    def test_wrong_version_rejected(self):
        bad = MAGIC + bytes([VERSION + 1, int(MsgType.PING), 0, 0])
        with pytest.raises(ProtocolError, match="version"):
            list(FrameReader().feed(bad))

    def test_unknown_type_rejected(self):
        bad = MAGIC + bytes([VERSION, 0x7A, 0, 0])
        with pytest.raises(ProtocolError, match="unknown message type"):
            list(FrameReader().feed(bad))

    def test_oversize_length_rejected_before_body(self):
        # 0xFFFF declared. A decoder that waits for the body would buffer 64 KB
        # on a 4 MB device on one hostile header.
        bad = MAGIC + bytes([VERSION, int(MsgType.KEY), 0xFF, 0xFF])
        reader = FrameReader()
        with pytest.raises(ProtocolError, match="exceeds"):
            list(reader.feed(bad))

    def test_encode_refuses_oversize_payload(self):
        with pytest.raises(ProtocolError):
            encode(MsgType.KEY, b"x" * (MAX_PAYLOAD + 1))

    def test_header_size_is_stable(self):
        # The C client hardcodes this. If it changes, that client breaks silently.
        assert HEADER_SIZE == 6


class TestCells:
    def test_cell_round_trip(self):
        cell = Cell(char="Q", fg="red", bg="blue", bold=True, reverse=True)
        assert unpack_cell(pack_cell(cell)) == cell

    def test_cell_is_three_bytes(self):
        assert len(pack_cell(Cell(char="a"))) == 3

    def test_box_drawing_round_trips(self):
        # Regression: these used to encode as '?'. The grid preserved them and
        # the encoder destroyed them, so Claude Code's UI -- which is boxes all
        # the way down -- arrived as rows of question marks.
        for ch in "─│┌┐└┘├┤┬┴┼":
            assert unpack_cell(pack_cell(Cell(char=ch))).char == ch, ch

    def test_rounded_corners_substitute_to_square(self):
        # Spleen has no rounded corners; at 6x8 the square ones are visually
        # equivalent and vastly better than '?'. They keep their own wire byte,
        # so they round-trip as themselves.
        for ch in "╭╮╯╰":
            assert unpack_cell(pack_cell(Cell(char=ch))).char == ch, ch

    def test_status_marks_round_trip(self):
        for ch in "●○•✓⏸❯…":
            assert unpack_cell(pack_cell(Cell(char=ch))).char == ch, ch

    def test_truly_unknown_glyph_still_becomes_placeholder(self):
        assert unpack_cell(pack_cell(Cell(char="漢"))).char == "?"

    def test_extended_glyphs_do_not_collide_with_ascii(self):
        from dsterm.extmap import TO_WIRE
        assert all(v > 0x7E for v in TO_WIRE.values())
        assert len(set(TO_WIRE.values())) == len(TO_WIRE)

    def test_unknown_colour_degrades_to_default(self):
        assert unpack_cell(pack_cell(Cell(char="a", fg="chartreuse"))).fg == "default"


class TestFullFrame:
    def test_round_trip(self):
        cells = list(Grid.blank().cells)
        cells[5] = Cell(char="Z", fg="green")
        original = Grid(tuple(cells))
        raw = encode_full_frame(original)
        payload = raw[HEADER_SIZE:]
        assert decode_full_frame(payload).cells == original.cells

    def test_truncated_full_frame_rejected(self):
        payload = encode_full_frame(Grid.blank())[HEADER_SIZE:]
        with pytest.raises(ProtocolError, match="must be"):
            decode_full_frame(payload[:-1])

    def test_full_frame_fits_one_message(self):
        assert COLS * ROWS * 3 <= MAX_PAYLOAD


class TestDiff:
    def test_round_trip(self):
        changes = [
            CellChange(0, 0, Cell(char="a", fg="red")),
            CellChange(23, 41, Cell(char="b", bold=True)),
        ]
        payload = encode_diff(changes)[HEADER_SIZE:]
        decoded = decode_diff(payload)
        assert [(c.row, c.col, c.cell.char) for c in decoded] == [
            (0, 0, "a"),
            (23, 41, "b"),
        ]

    def test_ragged_payload_rejected(self):
        with pytest.raises(ProtocolError, match="multiple"):
            decode_diff(b"\x00\x01\x02")

    def test_row_out_of_bounds_rejected(self):
        # Writing row 200 into a ROWS-tall array is the exact bug this catches.
        payload = bytes([200, 0, ord("x"), 0, 0])
        with pytest.raises(ProtocolError, match="outside"):
            decode_diff(payload)

    def test_col_out_of_bounds_rejected(self):
        payload = bytes([0, 200, ord("x"), 0, 0])
        with pytest.raises(ProtocolError, match="outside"):
            decode_diff(payload)

    def test_boundary_position_accepted(self):
        payload = bytes([ROWS - 1, COLS - 1, ord("x"), 0, 0])
        assert len(decode_diff(payload)) == 1

    def test_empty_diff_decodes_empty(self):
        assert decode_diff(b"") == []


class TestScroll:
    def test_round_trip(self):
        payload = encode_scroll(0, ROWS - 1, 3)[HEADER_SIZE:]
        assert decode_scroll(payload) == (0, ROWS - 1, 3)

    def test_negative_scroll_round_trips(self):
        payload = encode_scroll(0, ROWS - 1, -2)[HEADER_SIZE:]
        assert decode_scroll(payload) == (0, ROWS - 1, -2)

    def test_zero_distance_rejected(self):
        with pytest.raises(ProtocolError):
            encode_scroll(0, ROWS - 1, 0)

    def test_region_past_grid_rejected(self):
        with pytest.raises(ProtocolError):
            encode_scroll(0, ROWS + 5, 1)

    def test_inverted_region_rejected(self):
        with pytest.raises(ProtocolError):
            encode_scroll(10, 2, 1)

    def test_malformed_payload_rejected(self):
        with pytest.raises(ProtocolError):
            decode_scroll(b"\x00")

    def test_scroll_is_three_bytes(self):
        # The point of the primitive: ~900 changed cells for 3 bytes.
        assert len(encode_scroll(0, ROWS - 1, 1)) - HEADER_SIZE == 3


class TestCursor:
    def test_round_trip(self):
        payload = encode_cursor(5, 10, True)[HEADER_SIZE:]
        assert decode_cursor(payload) == (5, 10, True)

    def test_hidden_cursor_round_trips(self):
        payload = encode_cursor(0, 0, False)[HEADER_SIZE:]
        assert decode_cursor(payload) == (0, 0, False)

    def test_cursor_is_three_bytes(self):
        assert len(encode_cursor(1, 2, True)) - HEADER_SIZE == 3

    def test_boundary_position_accepted(self):
        assert decode_cursor(encode_cursor(ROWS - 1, COLS - 1, True)[HEADER_SIZE:]) == (
            ROWS - 1,
            COLS - 1,
            True,
        )

    def test_row_out_of_bounds_rejected(self):
        # Row 200 would write outside the client's cell array on a device with
        # no MMU -- the same guard the diff decoder has.
        with pytest.raises(ProtocolError, match="outside"):
            encode_cursor(ROWS + 5, 0, True)

    def test_col_out_of_bounds_rejected(self):
        with pytest.raises(ProtocolError, match="outside"):
            encode_cursor(0, COLS + 5, True)

    def test_decode_rejects_out_of_bounds(self):
        with pytest.raises(ProtocolError, match="outside"):
            decode_cursor(bytes((ROWS, 0, 1)))
        with pytest.raises(ProtocolError, match="outside"):
            decode_cursor(bytes((0, COLS, 1)))

    def test_malformed_payload_rejected(self):
        with pytest.raises(ProtocolError, match="must be"):
            decode_cursor(b"\x00\x01")

    def test_unknown_type_rejected_is_still_true(self):
        # CURSOR is now a known type; a genuinely unknown one still drops the
        # connection rather than being guessed at.
        bad = MAGIC + bytes([VERSION, 0x7B, 0, 0])
        with pytest.raises(ProtocolError, match="unknown message type"):
            list(FrameReader().feed(bad))


class TestKeys:
    def test_round_trip(self):
        assert decode_key(encode_key(b"\x1b[A")[HEADER_SIZE:]) == b"\x1b[A"

    def test_empty_rejected(self):
        with pytest.raises(ProtocolError):
            encode_key(b"")
        with pytest.raises(ProtocolError):
            decode_key(b"")

    def test_oversize_rejected(self):
        with pytest.raises(ProtocolError):
            encode_key(b"x" * (MAX_KEY_PAYLOAD + 1))
        with pytest.raises(ProtocolError):
            decode_key(b"x" * (MAX_KEY_PAYLOAD + 1))

    def test_control_bytes_pass_through(self):
        # Esc, Ctrl-C, Enter, Tab must survive verbatim.
        for seq in [b"\x1b", b"\x03", b"\r", b"\t", b"\x1b[B"]:
            assert decode_key(encode_key(seq)[HEADER_SIZE:]) == seq


class TestNoSideChannels:
    def test_message_types_are_only_screen_and_keys(self):
        # The DS gets the PTY and nothing else. If a future change adds a type
        # that names a file or a command, this fails and forces the argument.
        #
        # Six types have been added since, and each had to earn it:
        #
        # CURSOR is display state -- where a caret is -- which is why it is
        # allowed while a message naming a path would not be.
        #
        # VIEW carries a signed line count and nothing else. It moves a window
        # over history the server already holds and already decided to keep; it
        # cannot address anything the server was not going to render anyway,
        # cannot name a source, and cannot reach outside the session. The
        # alternative -- letting the client hold the scrollback -- would have
        # put knowledge of what a line is on the handheld, which is the thing
        # this whole design keeps on the server.
        #
        # MODE is one byte of flags describing how the child's screen behaves --
        # at present only "it is on the alternate screen". Same kind of thing as
        # CURSOR and VIEW_POS: a fact about the display, not a handle on
        # anything. The client cannot set it, cannot address anything with it,
        # and the server derives it from the emulator rather than from any
        # request. It has to exist because the alternate screen has no
        # scrollback, so the scroll control must mean something different there,
        # and the card cannot work that out for itself.
        #
        # SNIPPETS is the first type that carries CONTENT rather than display
        # state, and it had to earn that separately. It names no file, path or
        # command, and the client cannot request it -- but it does carry text
        # the handheld will later type into the PTY, and this link has no
        # per-message integrity, so anyone who can inject can plant a snippet.
        #
        # What makes it admissible is that a snippet cannot reach the PTY
        # without being displayed in full in the composer and committed by a
        # second deliberate press. That gate is the entire safety case, not a UI
        # nicety: a version that sent a chosen snippet directly would turn "can
        # deceive the display" into "can stage an action", and must not be
        # built. See docs/design/SNIPPETS_PUSH.md, and the client-side test that pins
        # the gate.
        #
        # SESSIONS and SESSION_PICK are the session-switching pair, and earn their place
        # the same way VIEW did: by carrying an intent rather than content.
        #
        # SESSIONS advertises what the server is already running -- names it
        # chose to offer, with flags and an idle time. SESSION_PICK answers with
        # one byte: a position in the list the client was just shown. Not a tmux
        # target, not a window number, not a name, not a path, and not an
        # identity that survives the advertisement. The server resolves it
        # against that client's last list or refuses, and a row that has gone
        # away produces an ERROR rather than the nearest survivor -- switching
        # someone to a session they did not touch is worse than telling them it
        # vanished.
        #
        # So a client entirely under an attacker's control can select among
        # sessions a person at the keyboard could already reach with Ctrl-B n,
        # and can name nothing that was not offered to it first.
        #
        # EXTENSION is a reserved server-to-client type the terminal never
        # reads. It is sent only to a card whose HELLO carried the extension's
        # own token, and the card hands the body to whatever asked for it --
        # which is free to ignore it. It cannot carry a key, a command, a
        # file, a path or a credential any more than any other type here can:
        # nothing on the card interprets a body as any of those.
        #
        # PROJECT is an optional server-to-client page of facts: a tab number,
        # a palette index, a label and a value, all of which the handheld draws
        # and none of which it interprets. It is worth being precise about what
        # makes it safe, because it is the newest and widest of these.
        #
        # It carries content, so it inherits the snippet argument: this link has
        # no per-message integrity, anyone who can inject can plant a row, and a
        # planted row therefore has to be harmless. It is, because a row reaches
        # a screen and nothing else. The card has no code that can execute one,
        # no code that can open a path named in one, and the strings are clipped
        # and sanitized to printable ASCII by the decoder before they reach a
        # display buffer.
        #
        # And note what is deliberately absent: there is no PROJECT_REQUEST, no
        # command message, and no way for the card to ask this server to do
        # anything with a path. The page's one-tap actions work by typing into
        # the PTY as ordinary KEY bytes -- exactly what a person at the keyboard
        # does -- which keeps the property the rest of this file is about: no
        # message in this protocol names a file, a command, or a path.
        # CLIP is the desktop clipboard, pushed down. It is the widest of these
        # and the argument for it is different in kind, so it is worth writing
        # out rather than adding the name and moving on.
        #
        # It inherits the snippet argument like PROJECT does -- the text reaches
        # a screen and the composer, never a shell, and the decoder refuses
        # anything but printable ASCII and newline, so an injected chunk cannot
        # carry an escape sequence into a terminal. There is no CLIP_REQUEST and
        # there must not be one: the handshake is one-way, so a new
        # client-to-server type would break a new card against an old server.
        # A paste still needs the same deliberate second press every piece of
        # server-supplied text needs before it is sent.
        #
        # What is genuinely new is the direction of the risk. Every other
        # message here carries something the server already shows on the DS's
        # own screen. This one carries something from a THIRD machine -- whatever
        # was copied on a synced machine -- onto a link with no encryption, when
        # a sync tool encrypts precisely so a clipboard is not readable on the LAN.
        # That is why it is off unless --clip is passed, why it is gated on a
        # token the card must advertise, why concealed content is refused using
        # Windows' own markers rather than a guess about what looks secret,
        # and why the text is never written to the journal or the dashboard.
        # None of those belong in the wire format, which is why they are tested
        # in test_clip.py instead -- but the reason the format is allowed to
        # exist at all is that they hold.
        #
        # PAIR_HELLO, PAIR_REPLY, PAIR_CONFIRM, PAIR_OK, PAIR_ACCEPT, PAIR_DONE
        # and PAIR_FAIL are the pairing exchange, and they are the only types
        # here allowed before authentication besides HELLO and AUTH. That is
        # the part that needed arguing, so here it is.
        #
        # They carry key material and nothing else: a device id, two X25519
        # public keys, a commitment, two nonces, two MACs and a short reason.
        # No path, no command, no text that reaches a screen or a PTY. Their
        # whole effect on the server is one file under the device-key
        # directory, written only while a person has a pairing window open with
        # `dsterm pair`, and only after that person AND the one at the DS have
        # both said the six-digit codes on their screens match -- a no from
        # either, or a broken commitment, closes the window. The connection
        # that carries them never authenticates: anything but the next pairing
        # message is a drop, which tests/test_pairing.py pins with a KEY sent
        # right after a PAIR_HELLO. And nothing secret is on the wire -- the
        # device key is derived on both ends from a Diffie-Hellman exchange,
        # and someone in the middle shows up as two different codes
        # (dsterm/pairing.py).
        #
        # REMOTE, REMOTE_NOTE, REMOTE_OPEN and REMOTE_PICK are the REMOTE screen
        # (docs/REMOTE.md), and they are the first pair here whose answer can
        # START something: a pick opens a tmux window. So the argument has to
        # be about what can be started, and it is the SESSION_PICK argument
        # with the list widened from "windows" to "Claude sessions".
        #
        # REMOTE and REMOTE_NOTE are facts and one line of outcome, printable
        # ASCII, drawn and never interpreted. REMOTE_OPEN is one bit. REMOTE_PICK
        # is a row index and an action number, and the server resolves the
        # index against the rows IT last sent this connection, re-checks the
        # action against that row's flags, and refuses otherwise. Every
        # argument that reaches tmux comes from the server: the id or session
        # id `claude agents` printed on this machine (validated against narrow
        # patterns), the `claude` on its PATH, and its own child command. The
        # three commands are fixed -- `claude attach <id>`, `claude --resume
        # <id> --fork-session`, and the server's own -- and run as argv, never
        # through a shell. That is what a person at the keyboard could already
        # do with Ctrl-B c, and a client under an attacker's control can name
        # nothing that was not offered to it first. Picks are rate-limited to
        # one a second, so it cannot open a window per frame either.
        #
        # STATE is the narrowest of these: one byte of what the session on
        # screen is doing and Claude Code's own waitingFor text, display state
        # like MODE, sent only to a card that announced "state". There is no
        # request for it -- it follows the same reading REMOTE uses.
        #
        # COMMANDS is a list of names for QUICK, and it is snippets' argument
        # again with less in it: server-chosen text that lands in the
        # composer, where nothing is sent until the person holding the DS
        # presses A. Only names -- "/" and up to thirty letters, digits and
        # _ - . : -- found as file names under .claude/commands and
        # .claude/skills (dsterm/commands.py); never a file's contents, never
        # a path, refused rather than escaped when it does not look like a
        # name. The card checks the same rule again (quick.c).
        #
        # CAMERA, CAMERA_FRAME, PHOTO_CHUNK, PHOTO_END, PHOTO_DISCARD,
        # CAMERA_READY, CAMERA_ACK and PHOTO_SAVED are the DSi camera, and
        # they are the widest thing here: the card sends actual pixels. So
        # the argument gets said once, in full.
        #
        # What the card can say is pixels and intents, all of them bounded:
        # a camera state, frames in one fixed format at one fixed size,
        # chunks at offsets the server checks against its own count, a CRC,
        # and an id the server itself issued on this connection. No client
        # message names a file, a path or a command. Only the server picks
        # the folder and the name, writes nowhere else, and deletes only
        # names it made; a discard resolves through a map of ids belonging
        # to this connection, so it cannot name somebody else's file.
        #
        # PHOTO_SAVED carries a path the server just wrote: printable ASCII,
        # at most 200 bytes, checked again by the card. It is COMMANDS'
        # argument -- server text that lands only in the composer, where
        # nothing is sent until the person holding the DS presses A. A
        # forged one can put a wrong path in front of the person, never
        # type it.
        #
        # Everything else about it is a budget, not a capability: one
        # preview frame and 4 KB of photo in flight, one picture being
        # assembled, one saved per three seconds. The types are gated like
        # REMOTE's -- the card announces "camera" in its HELLO and sends
        # none of them until CAMERA_READY has arrived, so an old server is
        # never handed a type it must drop the connection over.
        #
        # SUBSCREEN, MIRROR and TOUCH are `dsterm view`. SUBSCREEN is the
        # card's bottom screen as 42x24 cells -- a glyph, a flag and two
        # colours each, one fixed size and shape, every glyph mapped to a
        # printable character (a control byte arrives as "?", so a card can
        # put no escape sequence in the view's terminal) -- the same screen
        # the person holding the DS already sees, sent only while MIRROR
        # says a view is watching. The view itself is
        # a 0600 socket, this user's only (view.py): the people who can open
        # it can already `tmux attach`.
        #
        # TOUCH is the one that acts on the card, and it is the narrowest
        # thing here: an x below 256 and a y below 192, a tap where the
        # person could have tapped. The card ignores it unless its own VIEW
        # TAPS setting is on, which is off until the person holding the DS
        # turns it on, and GOD MODE cannot be armed by one. Gated like
        # CAMERA: "mirror" in the HELLO, nothing sent before MIRROR.
        #
        # ANSWER is KEY with a condition attached, and so narrower than KEY:
        # the same key bytes, bounded the same way, plus the id of the
        # question they answer, which only makes the server write them in
        # fewer cases -- when that question is still the one open -- never
        # more. The id is a number the server issued; it names nothing. For
        # an "ask" card the server also holds back a plain KEY carrying Enter
        # while a question is open, which again only refuses. STATE's tail is
        # that id and one flag byte, display state like the rest of STATE.
        #
        # VOICE_START, VOICE_CHUNK, VOICE_END, MIC_READY, VOICE_ACK and
        # VOICE_TEXT are the DS's Push-to-Talk microphone, and they are the
        # first types here that make the server RUN something on the card's
        # say-so -- so, like the camera, the argument is said in full.
        #
        # What the card can say is samples and one hint. VOICE_START is a
        # format byte that must be 1 (IMA ADPCM, 8 kHz) and a language hint
        # of at most seven of [A-Za-z0-9_-]. VOICE_CHUNK is an offset and
        # 1..480 bytes of ADPCM; offsets must be contiguous from 0 and the
        # whole utterance stays under VOICE_BYTES_MAX (60,000 bytes, 15 s),
        # per connection, or the connection drops. VOICE_END is a total and
        # a CRC-32, both checked against what arrived. None of the three
        # names a file, a path, a program or a destination.
        #
        # VOICE_END is what makes a process run, and everything about that
        # process was fixed before the card connected: which backend (chosen
        # at server start, voice.choose_backend, and printed), its argv (a
        # temporary 0600 WAV the server named, and a language code), never a
        # shell. The language code is the one card value that reaches an
        # argv or a form field, and voice.voice_lang lets through only two
        # or three lowercase letters -- anything else is dropped and the
        # backend auto-detects -- so it cannot be an option, a path or a
        # word with a meaning to a shell. Bounded in time as well as space:
        # one transcription per card in flight (a second VOICE_END answers
        # BUSY), one at a time across cards, every backend under a deadline
        # that kills its process, and a watchdog that clears busy and
        # answers NO_STT if anything outlives it.
        #
        # Where the audio goes is not the card's to choose either. Local
        # speech-to-text, or nothing: a cloud service is used only when the
        # person who started the server opted in (--stt-cloud or
        # DSTERM_STT_CLOUD=1), never because an API key happens to be set,
        # never as a fallback when the local one fails, only over https,
        # and a custom URL is never sent OPENAI_API_KEY.
        #
        # VOICE_TEXT carries the transcript back, and with the opt-in that
        # text came from a third party. It is COMMANDS' and PHOTO_SAVED's
        # argument once more: printable ASCII (the server folds it,
        # voice.fold_transcript, and both decoders refuse anything else), at
        # most 480 bytes, and it lands in the card's composer, where nothing
        # is sent until the person holding the DS reads it and presses A a
        # second time. A wrong or hostile transcript can put words in front
        # of the person, never type them. Its non-OK statuses carry no text.
        #
        # MIC_READY is one flag bit (speech-to-text available or not) and
        # the capability proof: sent right after AUTH_OK only to a card that
        # announced "mic", and never under --no-mic, so an old server is
        # never sent a voice type and a card on a server without a backend
        # is told so before it records. VOICE_ACK is a byte count no larger
        # than VOICE_BYTES_MAX, the credit the card's upload window slides
        # on -- a number, display state for the upload.
        allowed = {
            "HELLO", "AUTH", "KEY", "RESYNC", "PING", "VIEW", "SESSION_PICK",
            "NONCE", "AUTH_OK", "AUTH_FAIL",
            "FRAME_FULL", "FRAME_DIFF", "SCROLL", "PONG", "CURSOR", "VIEW_POS",
            "MODE", "SNIPPETS", "SESSIONS", "EXTENSION", "PROJECT", "CLIP",
            "PAIR_HELLO", "PAIR_REPLY", "PAIR_CONFIRM", "PAIR_OK", "PAIR_FAIL",
            "PAIR_ACCEPT", "PAIR_DONE", "COMMANDS",
            "REMOTE", "REMOTE_NOTE", "REMOTE_OPEN", "REMOTE_PICK", "STATE",
            "CAMERA", "CAMERA_FRAME", "PHOTO_CHUNK", "PHOTO_END",
            "PHOTO_DISCARD", "CAMERA_READY", "CAMERA_ACK", "PHOTO_SAVED",
            "SUBSCREEN", "MIRROR", "TOUCH", "ANSWER",
            "VOICE_START", "VOICE_CHUNK", "VOICE_END",
            "MIC_READY", "VOICE_ACK", "VOICE_TEXT",
            "ERROR",
        }
        assert {m.name for m in MsgType} == allowed

    def test_view_carries_only_a_line_count(self):
        # The guard that matters for VIEW specifically: its payload is two bytes
        # of signed integer. There is no room in it for a name, a path, or an
        # index into anything but the session's own history.
        from dsterm.protocol import encode_view
        for delta in (-1000, -1, 0, 1, 1000):
            assert len(encode_view(delta)) == HEADER_SIZE + 2

    def test_client_ignoring_cursor_still_reconstructs_the_grid(self):
        # The DS build before this feature ignores unknown server->client types,
        # and it must not matter. CURSOR interleaved with diffs is display state
        # that a grid-only client drops without corrupting the screen.
        changes = [
            CellChange(0, 0, Cell(char="a")),
            CellChange(1, 2, Cell(char="b")),
            CellChange(2, 3, Cell(char="c")),
        ]
        blob = (
            encode_diff(changes[:2])
            + encode_cursor(1, 2, True)
            + encode_diff(changes[2:])
        )
        msgs = list(FrameReader().feed(blob))

        grid = Grid.blank()
        for msg in msgs:
            if msg.type is MsgType.CURSOR:
                continue          # the grid-only client never looks at this
            for ch in decode_diff(msg.payload):
                cells = list(grid.cells)
                cells[ch.row * COLS + ch.col] = ch.cell
                grid = Grid(tuple(cells))
        assert grid.at(0, 0).char == "a"
        assert grid.at(1, 2).char == "b"
        assert grid.at(2, 3).char == "c"

class TestOrangeIsCarried:
    """The tenth palette entry rides in the four bits that were already there."""

    def test_orange_round_trips(self):
        from dsterm.grid import Cell, Grid
        from dsterm.protocol import decode_full_frame, encode_full_frame
        cells = [Cell(char="X", fg="orange", bg="orange")] + [
            Cell(char=" ", fg="default", bg="default")
        ] * (COLS * ROWS - 1)
        g = Grid(tuple(cells))
        back = decode_full_frame(encode_full_frame(g)[6:])
        assert back.cells[0].fg == "orange"
        assert back.cells[0].bg == "orange"

    def test_orange_needs_no_protocol_version_bump(self):
        # Four bits per colour, ten values used. Nothing widened, so a card
        # built before orange existed still parses every frame -- it renders
        # index 9 as default rather than mis-colouring it.
        from dsterm.protocol import VERSION, _PALETTE_INDEX
        assert max(_PALETTE_INDEX.values()) <= 0x0F
        assert VERSION == 1


class TestEveryServerFrameFitsOneSegment:
    """The DS reports a 536-byte MSS, and frames spanning full-size segments
    did not survive its stack (server.py, CELLS_PER_MESSAGE). So each type
    the server sends is built here at its largest legal size and checked
    sealed. A new server-to-client type fails the last assertion until it is
    added to the table -- with its own worst case."""

    def _largest(self):
        import dsterm.protocol as P
        from dsterm.server import CELLS_PER_MESSAGE

        wide = "W"
        return {
            P.MsgType.NONCE: P.encode(P.MsgType.NONCE, b"\0" * 32),
            P.MsgType.AUTH_OK: P.encode(P.MsgType.AUTH_OK, P.SEAL_AUTH_OK),
            P.MsgType.AUTH_FAIL: P.encode(P.MsgType.AUTH_FAIL),
            P.MsgType.FRAME_DIFF: P.encode_diff(
                [CellChange(i // COLS, i % COLS, Cell(wide, "orange", "blue", True, True))
                 for i in range(CELLS_PER_MESSAGE)]),
            P.MsgType.SCROLL: P.encode_scroll(0, ROWS - 1, ROWS - 1),
            P.MsgType.PONG: P.encode(P.MsgType.PONG),
            P.MsgType.CURSOR: P.encode_cursor(ROWS - 1, COLS - 1, True),
            P.MsgType.VIEW_POS: P.encode_view_pos(65535, 65535),
            P.MsgType.MODE: P.encode_mode(True),
            P.MsgType.SNIPPETS: P.encode_snippet(
                P.SNIPPETS_MAX - 1, P.SNIPPETS_MAX, "L" * P.SNIPPET_LABEL_MAX,
                "b" * P.SNIPPET_BODY_MAX),
            P.MsgType.SESSIONS: P.encode_session(
                P.SESSIONS_MAX - 1, P.SESSIONS_MAX, P.SESSION_FLAGS, 65535,
                "n" * P.SESSION_NAME_MAX),
            P.MsgType.PROJECT: P.encode_project_row(
                P.PROJECT_ROWS_MAX - 1, P.PROJECT_ROWS_MAX, P.PROJECT_TAB_COUNT - 1,
                15, "l" * P.PROJECT_LABEL_MAX, "v" * P.PROJECT_VALUE_MAX),
            P.MsgType.CLIP: P.encode_clip_chunk(
                0, P.CLIP_CHUNKS_MAX, 255, 0x07, P.CLIP_TEXT_MAX, b"c" * P.CLIP_CHUNK_MAX),
            P.MsgType.PAIR_REPLY: P.encode_pair_reply(b"\0" * 32, b"\0" * 32),
            P.MsgType.PAIR_OK: P.encode(P.MsgType.PAIR_OK, b"\0" * 16),
            P.MsgType.PAIR_FAIL: P.encode(P.MsgType.PAIR_FAIL, b"r" * 64),
            P.MsgType.REMOTE: P.encode_remote_row(
                P.REMOTE_ROWS_MAX - 1, P.REMOTE_ROWS_MAX, 1, 0, P.REMOTE_FLAGS, 65535,
                "n" * P.REMOTE_NAME_MAX, "d" * P.REMOTE_DETAIL_MAX),
            P.MsgType.REMOTE_NOTE: P.encode_remote_note("n" * P.REMOTE_NOTE_MAX),
            P.MsgType.STATE: P.encode_state(1, "s" * P.STATE_DETAIL_MAX),
            P.MsgType.PAIR_DONE: P.encode(P.MsgType.PAIR_DONE, b"\0" * 32),
            P.MsgType.COMMANDS: P.encode_commands(
                ["/" + "x" * 29 + f"{i % 10}" for i in range(16)]),
            P.MsgType.CAMERA_READY: P.encode_camera_ready(True),
            P.MsgType.CAMERA_ACK: P.encode_camera_ack(1, 0xFFFFFFFF),
            P.MsgType.PHOTO_SAVED: P.encode_photo_saved(0, 65535, "p" * P.PHOTO_PATH_MAX),
            P.MsgType.MIRROR: P.encode_mirror(True),
            P.MsgType.TOUCH: P.encode_touch(255, 191),
            P.MsgType.MIC_READY: P.encode_mic_ready(True),
            P.MsgType.VOICE_ACK: P.encode_voice_ack(P.VOICE_BYTES_MAX),
            P.MsgType.VOICE_TEXT: P.encode_voice_text(0, "t" * P.VOICE_TEXT_MAX),
            P.MsgType.ERROR: P.encode(
                P.MsgType.ERROR,
                b"this card's dsterm.nds is older than the computer's: "
                b"copy the current one to the card (dsterm card)"),
        }

    def test_each_type_at_its_largest_is_one_sealed_segment(self):
        from dsterm.protocol import SEAL_TAG_SIZE, SEGMENT_MAX
        for msg_type, frame in self._largest().items():
            assert len(frame) + SEAL_TAG_SIZE <= SEGMENT_MAX, (
                f"{msg_type.name} is {len(frame) + SEAL_TAG_SIZE} bytes sealed")

    def test_no_server_type_is_left_out(self):
        # FRAME_FULL is in the protocol but never sent (a whole screen goes as
        # diffs); EXTENSION's body belongs to an extension, which keeps its
        # own frames inside the segment.
        from dsterm.protocol import MsgType
        sent = {t for t in MsgType if t.value >= 0x80}
        assert set(self._largest()) | {MsgType.FRAME_FULL, MsgType.EXTENSION} == sent
