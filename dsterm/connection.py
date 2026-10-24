"""What the server keeps per connection, and the records its outbox carries.

ClientState is one card's connection: its socket and framing, how far it has
got through authentication, what it announced it understands, and the state
each feature keeps for it. The features (server_*.py) each read and write
their own part of it; server.py owns the rest.
"""

from __future__ import annotations

import logging
import socket
import time
from collections import deque
from dataclasses import (
    dataclass,
    field,
)
from pathlib import Path
from .grid import Grid
from . import camera as dscamera
from . import pairing
from . import remote
from . import sessions
from . import voice as dsvoice
from .protocol import (
    FrameReader,
    Seal,
    CAMERA_OFF,
)


log = logging.getLogger("dsterm.server")


@dataclass(frozen=True, slots=True)
class AdvertisedSession:
    """A row mapping that belongs to one exact client snapshot."""

    epoch: sessions.TmuxEpoch
    window_id: str
    # Kept alongside the stable identity for diagnostics and snapshot-change
    # detection only. It is never accepted back as a tmux selection target.
    display_index: int


@dataclass(slots=True)
class OutputRevision:
    """Observed output state for one tmux-window identity."""

    activity_marker: int
    revision: int
    last_output_monotonic: float


@dataclass(slots=True)
class OutboundBatch:
    """A FIFO group whose messages may not be interleaved with another batch."""

    messages: list[bytes]
    offset: int = 0
    # Switch barriers carry a complete SESSIONS snapshot after their repaint
    # metadata. Keep the new mapping dormant until every byte of that barrier
    # has been handed to the socket, so a client can never pick a half-list.
    promote_rows: tuple[AdvertisedSession, ...] | None = None
    promote_signature: tuple = ()


@dataclass(slots=True)
class ExtensionFrame:
    """One extension message, mutable until sent so the latest offer wins.

    ``token`` is the HELLO capability that gates it; ``payload`` is the body,
    framed as MsgType.EXTENSION at the instant it is serialized.
    """

    token: str
    payload: bytes


@dataclass(slots=True)
class SessionSnapshotBatch:
    """One atomic SESSIONS snapshot and the mapping it makes pickable."""

    messages: list[bytes]
    rows: tuple[AdvertisedSession, ...]
    signature: tuple
    epoch: sessions.TmuxEpoch | None
    offset: int = 0


@dataclass(slots=True)
class RemoteSnapshotBatch:
    """One atomic REMOTE set and the row map it makes pickable.

    The SESSIONS discipline exactly: the card commits a set when its last row
    arrives, so the server promotes ``rows`` -- what a REMOTE_PICK index means
    -- only once every frame has been handed to the socket and its tail has
    drained. Until then a pick resolves against the previous complete set.
    """

    messages: list[bytes]
    rows: tuple[remote.Row, ...]
    signature: tuple
    ages: tuple[int, ...]
    offset: int = 0


@dataclass(slots=True)
class Caps:
    """What the card's HELLO announced, and the one thing proved by use.

    Every gate on an optional message reads here. Held per connection, so a
    reconnecting older card resets them all and is never sent a type it would
    halt on -- the framing layer refuses unknown types, and a card can only
    be reflashed from another machine.
    """

    # Set once this client has sent a VIEW, which only a build that also knows
    # VIEW_POS can do. Inferred rather than announced: sending VIEW_POS to an
    # older card would halt a working handheld the moment it connected. A
    # client that never scrolls never needs the message.
    view_pos: bool = False
    # Announced in the HELLO ("mode", "snippets", "project", "clip").
    mode: bool = False
    snippets: bool = False
    project: bool = False
    clip: bool = False
    # REMOTE (docs/REMOTE.md) and STATE.
    remote: bool = False
    state: bool = False
    # QUICK's slash commands, and `dsterm view` (view.py).
    commands: bool = False
    mirror: bool = False
    # STATE carries the open question's id, and this card ties its Enter to
    # it with ANSWER (server_agents.py, "questions").
    ask: bool = False
    # The camera: HELLO-announced AND offered by this server. Without it no
    # camera message from this connection is legitimate.
    camera: bool = False
    # Push-to-Talk microphone: HELLO-announced ("mic") AND offered (not
    # --no-mic). Without it no voice message from this connection is
    # legitimate, and no MIC_READY is sent.
    mic: bool = False
    # "project64": this card takes up to 64 PROJECT rows and the DS glyph
    # bytes 0x80-0xBF in them. A card without it (1.0.x) refuses a snapshot
    # of more than 48 rows and draws those bytes as '?', so it is sent at most
    # 48 rows of printable ASCII (project.encode).
    project64: bool = False
    # Every token the HELLO carried, for an extension's gate.
    tokens: frozenset[str] = frozenset()
    # What this connection advertised, for the log and the dashboard: a record
    # of the negotiation, which the wire itself does not keep.
    announced: str = ""


@dataclass(slots=True)
class FeedsSent:
    """What each small feed last sent this card (server_feeds.py), so each
    goes on change rather than every tick."""

    # The last clipboard snapshot's sequence. -1 means "nothing yet", which a
    # reconnect restores so the card is told the current clipboard rather than
    # assumed to still have it.
    clip_seq: int = -1
    # The last project snapshot, so an unchanged repo does not put a dozen
    # frames on the wire every three seconds.
    project_signature: tuple = ()
    # QUICK's COMMANDS list as last sent; None until the first.
    commands: tuple[str, ...] | None = None


@dataclass(slots=True)
class SessionsState:
    """The SESSIONS list as this card has it, and a switch in progress
    (server_sessions.py)."""

    # The exact tmux identity for each wire row, installed only after its
    # complete snapshot has entered the socket FIFO. A pick never resolves
    # through a current tmux index or a window name.
    rows: list[AdvertisedSession] = field(default_factory=list)
    signature: tuple = ()
    seen: dict[tuple[sessions.TmuxEpoch, str], int] = field(default_factory=dict)
    initialized: bool = False
    batch: SessionSnapshotBatch | None = None
    next_snapshot: SessionSnapshotBatch | None = None
    # A select is completed by the ordinary tick after tmux has had an
    # opportunity to repaint. This avoids blocking while still imposing the
    # required paint/cursor -> VIEW_POS -> MODE -> SESSIONS order.
    switch_pending: bool = False
    switch_wait_ticks: int = 0
    switch_barrier: OutboundBatch | None = None


@dataclass(slots=True)
class ExtQueue:
    """An extension's one deferred message for this card (server_ext.py):
    offered, then queued behind the switch barrier, then serialized. The
    latest offer wins at every stage."""

    pending: ExtensionFrame | None = None
    queued: ExtensionFrame | None = None


@dataclass(slots=True)
class ScrollState:
    """How far back this card is reading, and the capture behind it
    (server_scrollback.py)."""

    # Lines above the live screen this client is looking at. 0 is live. Held
    # per client rather than per session so scrolling back is a private act:
    # it must not move the view for anyone else, nor disturb the emulator.
    offset: int = 0
    # Multiplexed views are tmux captures taken by a worker. The anchor is the
    # absolute history line at the top of the view (history_size - offset), so
    # what the user is reading stays put while new output arrives; the grid is
    # the frozen capture; the gen makes a superseded worker result a no-op.
    anchor: int | None = None
    grid: Grid | None = None
    gen: int = 0
    # Presses that arrive while a capture is in flight accumulate here as a
    # signed sum and are dispatched as one job when the result frees the gate:
    # the worker reads only the job tuple, so a burst can never race the anchor.
    pending: int = 0
    inflight: bool = False
    # Rate-limit live VIEW_POS updates. Explicit VIEW requests bypass this so a
    # button press always receives an answer, even at 0 of 0.
    live_report_at: float = 0.0
    live_available: int = 0


@dataclass(slots=True)
class AgentsState:
    """REMOTE and STATE as this card has them (server_agents.py)."""

    # The card has the REMOTE screen up. While any card does, `claude agents`
    # is asked every few seconds; while none does, it is not asked at all.
    remote_open: bool = False
    # The set this client should have: NOT_ASKED right after auth, then the
    # latest answer. Reconciled against what was last fully sent every tick,
    # so a set lost to a resync's outbox clear is simply sent again.
    target: remote.RemoteSet | None = None
    # What a REMOTE_PICK index means: the rows of the last set whose every
    # frame left this server, and that set's identity and ages. None for the
    # signature means "never sent one", which differs from every real set.
    rows: list[remote.Row] = field(default_factory=list)
    signature: tuple | None = None
    ages: tuple[int, ...] = ()
    sent_at: float = 0.0
    # The set being staged frame by frame, kept through a final socket tail
    # exactly as the SESSIONS batch is.
    batch: RemoteSnapshotBatch | None = None
    # One REMOTE_NOTE waiting for the tick: released only once no switch is
    # pending, so the switch that a pick starts cannot clear it from the
    # outbox before it is sent. The latest note wins.
    note: bytes | None = None
    last_pick_at: float = float("-inf")
    # The STATE this card last heard -- (state, detail) and, for an "ask"
    # card, the ask id and flags; None until the first one after auth, so it
    # is sent once and then only on change.
    state_sent: tuple | None = None
    # An answer or Enter from this card was dropped; the next STATE says so.
    ask_dropped: bool = False


@dataclass(slots=True)
class CameraLink:
    """This card's camera and its pictures (server_camera.py)."""

    # What the card last said: the gate that makes a preview while the screen
    # is closed a protocol error rather than a stray frame.
    state: int = CAMERA_OFF
    # The upload in flight, made lazily (460 KB is not worth holding for every
    # card), the rate-limit timestamp of the last saved picture, and this
    # connection's issued ids -> saved path. The id map dies with the
    # connection, which is what keeps a foreign id unable to discard a file.
    assembler: dscamera.PhotoAssembler | None = None
    last_photo_at: float = 0.0
    photo_ids: dict[int, Path] = field(default_factory=dict)
    next_photo_id: int = 0
    # A save is on the worker for this connection, and whether the camera was
    # closed before it finished: a picture the person walked away from is
    # deleted rather than kept, since nothing will ever reference it.
    saving: bool = False
    cancelled: bool = False
    # Preview storage is capped (CAMERA_STORE_MIN_INTERVAL); acked either way.
    last_preview_stored_at: float = 0.0


@dataclass(slots=True)
class VoiceLink:
    """This card's microphone upload and transcription state (server_voice.py)."""

    assembler: dsvoice.VoiceAssembler | None = None
    # One transcription at a time per card: VOICE_END while this is set
    # answers BUSY.
    transcribing: bool = False
    # The job the card is waiting for (0: none). A result carrying any other
    # number -- abandoned at its deadline, or from before a disconnect -- is
    # dropped rather than delivered.
    job: int = 0
    # time.monotonic() past which the watchdog abandons the job.
    deadline: float = 0.0


# eq=False so identity decides. A dataclass __eq__ would compare every field,
# which is both wrong and dangerous here: two connections are never the same
# connection however similar their state, and list.remove() would then be free
# to delete a different client that merely compared equal.
#
# slots=True so a misspelt or retired field name is an AttributeError rather
# than a new attribute that silently diverges from the real one.
@dataclass(eq=False, slots=True)
class ClientState:
    """One card's connection: its socket and framing, how far it has got
    through authentication, and what it has been sent. What each feature
    keeps for it lives in the parts below the connection's own fields."""

    sock: socket.socket
    addr: tuple
    reader: FrameReader
    nonce: bytes
    authed: bool = False
    connected_at: float = 0.0
    # What we believe the client's screen holds. Reset on connect so a
    # reconnecting client is resynchronised with a full frame rather than a diff
    # against a grid it never received.
    mirror: Grid = None  # type: ignore[assignment]
    # Last cursor sent to this client, as (row, col, visible). None means the
    # client does not know where the caret is yet, so the first change -- even
    # to a hidden cursor -- must be sent.
    cursor: tuple[int, int, bool] | None = None
    # Screen updates awaiting release. Handshake replies bypass this entirely:
    # they are proven to cross, and delaying them would stall the handshake.
    outbox: deque[bytes | OutboundBatch | SessionSnapshotBatch | RemoteSnapshotBatch
                  | ExtensionFrame] = field(default_factory=deque)
    # Bytes handed to the kernel but not yet accepted by it. The socket is
    # non-blocking, so a send can be partial or refused outright when the
    # buffer is full; the remainder waits here rather than being lost or
    # retried from the start. Empty in the ordinary case, because the DS drains
    # far faster than a 42x24 screen fills.
    pending: bytearray = field(default_factory=bytearray)
    # Last (offset, available) VIEW_POS told to this client, so it is sent on
    # change rather than every tick. None means it has never been told.
    view_pos: tuple[int, int] | None = None
    # Last MODE flags byte sent, so it goes on change rather than every tick.
    # None means never told, which is why the first send happens even when the
    # flags are zero: the card must be able to tell "not alt screen" from "not
    # heard from the server yet".
    mode_flags: int | None = None
    # The device id this card announced in HELLO, which selects the key its
    # AUTH is checked against. None for a card that sends none: it proves the
    # one legacy key, exactly as every card did before pairing existed.
    device_id: str | None = None
    # A pairing attempt in flight on this connection, between PAIR_REPLY and
    # PAIR_CONFIRM. One per connection; the connection never authenticates.
    pair: pairing.ServerHalf | None = None
    # PAIR_DONE went out: this connection's work is finished, and the card
    # authenticates on a new one. Anything more on it is refused.
    paired: bool = False
    # When this connection last sent anything at all. A card PINGs every
    # second, so silence is a card that has gone (SILENT_CARD_SECONDS).
    last_heard_at: float = field(default_factory=time.monotonic)
    # When this card last typed. Typing is the thing that starts a session
    # working, so STATE asks faster for a while after it.
    last_key_at: float | None = None
    # SEAL (docs/SEAL.md). ``wants_seal`` is whether the HELLO carried the
    # token, which every card must; ``seal`` is made once at AUTH_OK and never
    # revisited, holding the key and both counters. While it is set every
    # byte this server sends the client is sealed in _send_to, and the reader
    # refuses any frame without its tag. Per connection like every
    # capability, so a reconnect -- a new nonce, therefore a new key -- starts
    # unsealed until its own AUTH_OK. ``hello`` is the last HELLO payload
    # before AUTH, exactly as it arrived: it goes into the seal's key, so a
    # HELLO edited on the way leaves the two ends unable to talk.
    wants_seal: bool = False
    seal: Seal | None = None
    hello: bytes | None = None
    # `dsterm view` (view.py): whether this card was last told a view is
    # watching its bottom screen.
    mirror_told: bool | None = None

    # What each feature keeps for this card.
    caps: Caps = field(default_factory=Caps)
    feeds: FeedsSent = field(default_factory=FeedsSent)
    sessions: SessionsState = field(default_factory=SessionsState)
    ext: ExtQueue = field(default_factory=ExtQueue)
    scroll: ScrollState = field(default_factory=ScrollState)
    agents: AgentsState = field(default_factory=AgentsState)
    camera: CameraLink = field(default_factory=CameraLink)
    voice: VoiceLink = field(default_factory=VoiceLink)
