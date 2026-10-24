"""TCP server: one PTY, one client at a time.

A card is given the terminal and nothing else: it receives cells and sends
key bytes, and the rest of what it can say is intents the server resolves
against state it already holds (a line count, a row it sent) or pixels the
server stores under a name it picks. No message from a card runs a command,
reads a file or names a path, so a card that is entirely under an attacker's
control can still only do what a person at the keyboard could do.

This module is the core: the select loop, accepting and authenticating
connections, the paced outbox, dispatching each message, and the tick that
turns the session into frames. Each feature lives in its own module, mixed
into Server: server_camera, server_pairing, server_scrollback, server_mirror,
server_sessions, server_agents (REMOTE and STATE), server_feeds and
server_ext. connection.py holds the per-connection state they share.
"""

from __future__ import annotations

import selectors
import socket
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .auth import (
    generate_nonce,
    verify_mac,
)
from .grid import (
    COLS,
    ROWS,
    CellChange,
    Grid,
)
from . import awake
from . import view as dsview
from . import discovery
from . import pairing
from . import remote
from .protocol import (
    FEATURE_CAMERA,
    FEATURE_MODE,
    FEATURE_CLIP,
    FEATURE_PROJECT,
    FEATURE_REMOTE,
    FEATURE_SEAL3,
    FEATURE_SNIPPETS,
    FEATURE_STATE,
    FEATURE_COMMANDS,
    FEATURE_MIRROR,
    FEATURE_ASK,
    FEATURE_MIC,
    FEATURE_PROJECT64,
    MAX_PAYLOAD,
    PRE_AUTH_ALLOWED,
    FrameReader,
    Message,
    MsgType,
    ProtocolError,
    SEAL_AUTH_OK,
    Seal,
    SealError,
    seal_key,
    decode_answer,
    decode_key,
    encode,
    encode_cursor,
    encode_camera_ready,
    encode_mic_ready,
    hello_features,
    hello_card_nonce,
    hello_device_id,
    hello_grid,
    encode_diff,
    encode_scroll,
)
from .session import Session
from .server_camera import CameraMixin
from .server_pairing import PairingMixin
from .server_scrollback import ScrollbackMixin
from .server_mirror import MirrorMixin
from .server_sessions import SessionsMixin
from .server_agents import AgentsMixin
from .server_feeds import FeedsMixin
from .server_ext import ExtensionsMixin
from .server_voice import VoiceMixin
from .connection import (
    ClientState,
    ExtensionFrame,
    OutboundBatch,
    RemoteSnapshotBatch,
    SessionSnapshotBatch,
    log,
)
from .server_camera import FeedViewer


DEFAULT_PORT = 8347

# One live session, plus room for one challenger.
#
# The handheld can vanish without closing its socket -- switched off, battery
# out, carried out of range -- and TCP on this side has no way to notice. The
# connection stays ESTABLISHED indefinitely, and with a single slot the server
# then turns away the very device that owns the session: "server refused: busy",
# with no way through short of restarting the server by hand.
#
# So a second connection is accepted and allowed to attempt the handshake, and a
# client that proves it holds the key takes over from whatever was there before.
# The takeover happens only on a verified MAC, so an unauthenticated stranger can
# occupy the spare slot but can never displace a live session.
MAX_CONNECTIONS = 2

# A client that connects and then says nothing holds the single slot forever.
# Generous rather than tight: a handheld printing diagnostics between steps is
# slow, and hanging up mid-handshake truncates the very output that explains why
# the handshake is failing.
AUTH_TIMEOUT = 30.0

# Frame coalescing. pyte measured ~1.3 ms/snapshot against this budget, so
# the rate is chosen for latency smoothing, not because emulation is slow.
DEFAULT_FPS = 15.0

# Below this, a diff is cheaper than the full grid it would replace.
FULL_FRAME_THRESHOLD = (COLS * ROWS) * 2 // 3

# Cells per FRAME_DIFF message.
#
# Measured against the handheld, not chosen for tidiness: every small message was
# acknowledged, and a single 3030-byte FRAME_FULL was retransmitted past 10 KB
# without one ACK while the connection stayed ESTABLISHED. The DS reports an MSS
# of 536 -- the IPv4 minimum -- so anything spanning full-size segments does not
# survive its stack. 90 cells is 450 bytes plus a 6 byte header, comfortably
# inside one small segment.
CELLS_PER_MESSAGE = 90

# A whole screen therefore arrives as a sequence of diffs rather than one
# FRAME_FULL. The client's grid starts blank and every cell is sent, so the
# result is identical; FRAME_FULL remains in the protocol but is no longer used
# on the wire.
FULL_RESYNC_MESSAGES = (COLS * ROWS + CELLS_PER_MESSAGE - 1) // CELLS_PER_MESSAGE

# Messages released per tick, rather than all at once.
#
# Not the fix for the stall this was written for: the handheld's counter came
# back "sel 0, rx 0, msg 0", meaning it never polled the socket at all, so no
# sending pattern could have reached it. The cause was a zero select() timeout
# on the client. Burst size was never the problem.
#
# It stays because the underlying constraint is still real -- the DS drains its
# Wi-Fi receive buffer once per VBlank, and that buffer is small -- and because
# the rate follows from that rather than from taste: release no more messages
# per tick than the client has drains within a tick, i.e. its 60 Hz refresh over
# this server's 15 Hz tick. A typical diff is one or two messages and never
# reaches the cap; only a whole-screen repaint is paced, and that costs it about
# 200 ms.
MESSAGES_PER_TICK = 4

# Above this the queue is discarded and replaced with a fresh resync, which is
# self-contained and therefore safe to substitute for any backlog of diffs.
#
# The threshold is the cost of the resync itself rather than a tuned number:
# once the backlog would take more messages to deliver than simply resending the
# whole screen, the backlog is strictly worse -- the same bytes, describing
# older frames. Without a bound, output faster than the release rate grows the
# queue without limit and the screen falls further behind the longer it runs.
OUTBOX_LIMIT = FULL_RESYNC_MESSAGES

# Unsent bytes tolerated in a client's kernel-refused tail. A whole screen is
# about 22 KiB of diffs, so this is several screens' worth: enough to ride out
# a stalled Wi-Fi moment, far too little to hide a client that has stopped
# reading. Beyond it the connection is not slow, it is gone.
PENDING_LIMIT = 128 * 1024



# How many recent events the dashboard can show. Bounded because this runs for
# days: an unbounded log of every connect and drop is a slow memory leak.
EVENT_HISTORY = 200

# A child that exits sooner than this did not run; it failed to start.
CHILD_MIN_LIFETIME = 5.0

# After this many failures in a row, stop respawning.
#
# Restarting a child that dies instantly is an unthrottled loop: it burns a core,
# floods the journal, and -- worst of all -- wipes the screen each time, so the
# error message explaining the failure is destroyed by the next attempt before
# anyone can read it. Giving up leaves that message on screen for the handheld.
CHILD_MAX_RAPID_FAILURES = 3


@dataclass
class Counters:
    """What the server has actually done, for the dashboard.

    Bytes rather than only messages: the two failures that cost the most time
    this project were both cases where messages were being sent and no bytes
    were being acknowledged, and a message-only view cannot show that.
    """
    connects: int = 0
    drops: int = 0
    auth_ok: int = 0
    auth_failed: int = 0
    bytes_out: int = 0
    bytes_in: int = 0
    messages_out: int = 0
    messages_in: int = 0
    resyncs: int = 0
    child_restarts: int = 0


class Server(CameraMixin, PairingMixin, ScrollbackMixin, MirrorMixin, SessionsMixin, AgentsMixin, FeedsMixin, ExtensionsMixin, VoiceMixin):
    def __init__(
        self,
        command: list[str],
        key: bytes,
        host: str = "0.0.0.0",
        port: int = DEFAULT_PORT,
        fps: float = DEFAULT_FPS,
        cwd: str | None = None,
        snippets_path=None,
        on_event: Callable[[str], None] | None = None,
        auth_timeout: float = AUTH_TIMEOUT,
        env: dict | None = None,
        clip_enabled: bool = False,
        discovery_port: int | None = None,
        config_dir: Path | None = None,
        remote_enabled: bool = True,
        child_command: list[str] | None = None,
        remote_poller: "remote.RemotePoller | None" = None,
        keep_awake: "bool | awake.KeepAwake" = False,
        camera_enabled: bool = True,
        photo_dir: "str | Path | None" = None,
        photo_keep: int = 20,
        camera_popups: bool = True,
        voice_enabled: bool = True,
        voice_transcriber: Callable[[bytes, str | None], tuple[int, str]] | None = None,
        voice_cloud: bool = False,
        project_ci: bool = False,
    ) -> None:
        self.command = command
        # The command the child runs INSIDE tmux, before serve.py wrapped it:
        # what REMOTE's "+ New Claude session" opens in a new window. None
        # falls back to `claude` with no arguments, which is the default child.
        self.child_command = list(child_command) if child_command else None
        self.key = key
        self.host = host
        self.port = port
        self.interval = 1.0 / fps
        self.cwd = cwd
        self._init_feeds(cwd, snippets_path, clip_enabled, project_ci)
        self._init_sessions()
        self._init_agents(remote_enabled, remote_poller)
        # Off unless asked: `dsterm serve` asks (--no-keep-awake declines), and
        # a test hands in its own so nothing it starts touches the machine.
        self._awake = (keep_awake if isinstance(keep_awake, awake.KeepAwake)
                       else awake.KeepAwake(enabled=bool(keep_awake),
                                            on_event=self._emit))
        # Handed to the child, and to every replacement child. None means
        # inherit this process's environment.
        self.env = env
        # Set by request_restart() on the dashboard's thread, acted on by the
        # loop.
        self._restart_requested = threading.Event()
        self._init_mirror()
        # Where paired devices' keys and the pairing window live. A parameter
        # so tests can pair against a temporary directory.
        self.config_dir = config_dir or pairing.CONFIG_DIR
        # UDP discovery. None or 0 leaves it off, which is what tests and
        # anything embedding the server get unless they ask.
        self.discovery_port = discovery_port
        self._discovery: discovery.Responder | None = None
        self.auth_timeout = auth_timeout
        self._on_event = on_event or (lambda msg: log.info(msg))
        self.counters = Counters()
        # Recent events, kept so the dashboard can show what happened without
        # anyone having to be tailing the journal at the time.
        self.events: deque[tuple[float, str]] = deque(maxlen=EVENT_HISTORY)
        self.started_at = time.time()
        self._child_started_at = 0.0
        self._rapid_failures = 0
        # Set once respawning has been abandoned, so the failure stays on screen.
        self.child_abandoned = False
        self._sel = selectors.DefaultSelector()
        self._listener: socket.socket | None = None
        self._clients: list[ClientState] = []
        self._session: Session | None = None
        # queue_extension() is intentionally usable by a trusted in-process
        # caller outside the selector thread. The lock covers only the deferred
        # message's ownership and never holds around socket or tmux work.
        self._ext_lock = threading.RLock()
        self._init_ext()
        self._running = False
        self._init_camera(camera_enabled, photo_dir, photo_keep, camera_popups)
        self._init_voice(voice_enabled, voice_transcriber, voice_cloud)
        self._init_scrollback()

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        self._session = Session(self.command, cwd=self.cwd, env=self.env)
        self._session.start()
        self._child_started_at = time.monotonic()

        lst = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Binding 0.0.0.0 is required: WSL2 mirrored mode means a loopback bind
        # is unreachable from the LAN even with the firewall open.
        lst.bind((self.host, self.port))
        lst.listen(1)
        lst.setblocking(False)
        self._listener = lst
        self._sel.register(lst, selectors.EVENT_READ, "listen")
        self._sel.register(self._session.fd, selectors.EVENT_READ, "pty")
        if self.discovery_port:
            # A convenience, never a reason not to serve: a port already in use
            # (a second server, or something else entirely) is a line in the
            # log, and a card can still be pointed at an address by hand.
            try:
                self._discovery = discovery.Responder(
                    self._discovery_answer, host=self.host, port=self.discovery_port)
                self._sel.register(self._discovery.sock, selectors.EVENT_READ,
                                   "discovery")
                self._emit(f"answering discovery on udp {self.host}:"
                           f"{self._discovery.port}")
            except OSError as exc:
                self._discovery = None
                self._emit(f"discovery off: cannot bind udp {self.discovery_port}: {exc}")
        self._start_mirror()
        self._start_camera()
        self._running = True
        self._emit(f"listening on {self.host}:{self.port}")

    def stop(self) -> None:
        self._running = False
        # In this order: the capture worker woken, the pollers stopped, then
        # the camera's and the view's sockets closed -- all before the cards
        # are dropped and the session ends.
        self._stop_scrollback()
        self._stop_feeds()
        self._stop_agents()
        self._stop_camera()
        self._stop_voice()
        self._stop_mirror()
        for c in list(self._clients):
            self._drop(c, "server shutting down")
        self._awake.release()
        if self._listener is not None:
            try:
                self._sel.unregister(self._listener)
            except KeyError:
                pass
            self._listener.close()
            self._listener = None
        if self._discovery is not None:
            try:
                self._sel.unregister(self._discovery.sock)
            except (KeyError, ValueError):
                pass
            self._discovery.close()
            self._discovery = None
        if self._session is not None:
            try:
                self._sel.unregister(self._session.fd)
            except (KeyError, ValueError):
                pass
            self._session.stop()
            self._session = None
        self._sel.close()

    def __enter__(self) -> "Server":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    @property
    def bound_port(self) -> int:
        """The real port, which differs from `port` when 0 was requested."""
        if self._listener is None:
            raise RuntimeError("server is not started")
        return self._listener.getsockname()[1]

    def _emit(self, msg: str) -> None:
        self.events.append((time.time(), msg))
        self._on_event(msg)

    # -- main loop ---------------------------------------------------------

    def serve_forever(self, max_seconds: float | None = None) -> None:
        started = time.monotonic()
        next_tick = started
        while self._running:
            if max_seconds is not None and time.monotonic() - started >= max_seconds:
                break
            if self._restart_requested.is_set():
                self._restart_requested.clear()
                self.child_abandoned = False
                self._rapid_failures = 0
                self._emit("child restart requested from the dashboard")
                self._restart_session()
            elif (self._session is not None and not self._session.alive()
                    and not self.child_abandoned):
                self._restart_session()

            timeout = max(0.0, next_tick - time.monotonic())
            # Output read but not yet emulated (Session.pump's budget): no
            # sleeping in select while it waits.
            if self._session is not None and self._session.backlog:
                timeout = 0.0
            try:
                events = self._sel.select(timeout=min(timeout, 0.05))
            except (OSError, ValueError):
                # stop() runs on another thread and closes the selector. It can
                # land between the `while self._running` check above and this
                # call, and select() on a closed selector raises EBADF -- which
                # surfaced as an unhandled thread exception on every single
                # shutdown. That is the loop ending on request, not a fault, so
                # it is only swallowed when a stop is actually in progress.
                if not self._running:
                    break
                raise
            pumped = False
            for key, mask in events:
                if key.data == "listen":
                    self._accept()
                elif key.data == "pty":
                    self._session.pump()  # type: ignore[union-attr]
                    pumped = True
                elif key.data == "discovery":
                    self._discovery.readable()  # type: ignore[union-attr]
                elif key.data == "camera-feed":
                    self._accept_feed()
                elif key.data == "view-hub" or isinstance(key.data, dsview.ViewClient):
                    self._mirror_event(key.data, mask)
                elif isinstance(key.data, FeedViewer):
                    self._camera_event(key.data, mask)
                elif isinstance(key.data, ClientState):
                    self._read_client(key.data)

            if not pumped and self._session is not None and self._session.backlog:
                self._session.pump()
            if self._session is not None and self._session.input_pending:
                self._session.flush_input()
            self._expire_unauthed()
            self._poll_pairing()

            now = time.monotonic()
            if now >= next_tick:
                self._tick()
                next_tick += self.interval
                if next_tick < now:
                    next_tick = now + self.interval

    # -- session -----------------------------------------------------------

    def request_restart(self) -> None:
        """Ask for a fresh child from another thread -- the dashboard's. The
        loop does it on its next pass, so the session is only ever replaced
        by the thread that reads it. A restart asked for by a person also
        forgives a child given up on: they have presumably fixed something."""
        self._restart_requested.set()

    def _restart_session(self) -> None:
        """Replace the child process without dropping the client.

        The server used to exit when the child did, which meant a handheld lost
        its connection the moment anyone typed `exit` -- and the only way back
        was walking to the PC to restart the server. That is precisely the trip
        this project exists to avoid. Observed on hardware: the DS answered a
        prompt, the child later exited, and the console halted with
        "server closed (errno=128)".

        The client keeps its connection and is resynchronised, so from the
        handheld this looks like the screen being replaced rather than the link
        failing.

        Rapid failures are not retried forever. A child that cannot start at all
        would otherwise spin: each attempt clears the screen, so the error
        explaining the failure is wiped by the next attempt and never readable.
        Abandoning leaves that message on the handheld, which is the only place
        anyone is looking.
        """
        if self._child_started_at:
            lived = time.monotonic() - self._child_started_at
            self._rapid_failures = (
                self._rapid_failures + 1 if lived < CHILD_MIN_LIFETIME else 0
            )
            if self._rapid_failures >= CHILD_MAX_RAPID_FAILURES:
                self.child_abandoned = True
                self._emit(
                    f"child died {self._rapid_failures} times in under "
                    f"{CHILD_MIN_LIFETIME:.0f}s each; not restarting again. "
                    f"The last screen is left as-is: read it."
                )
                return

        old = self._session
        if old is not None:
            try:
                self._sel.unregister(old.fd)
            except (KeyError, ValueError):
                pass
            old.stop()

        self._session = Session(self.command, cwd=self.cwd, env=self.env)
        self._session.start()
        self._child_started_at = time.monotonic()
        self._sel.register(self._session.fd, selectors.EVENT_READ, "pty")
        self.counters.child_restarts += 1
        self._emit("child process exited; restarted")

        # The new child's screen has nothing in common with the old one, so a
        # diff against the mirror would be meaningless.
        c = self._authed()
        if c is not None:
            c.mirror = Grid.blank()
            self._view_to_live(c)
            self._resync()

    # -- connections -------------------------------------------------------

    def _authed(self) -> ClientState | None:
        """The client that owns the session. At most one is ever authenticated."""
        for c in self._clients:
            if c.authed:
                return c
        return None

    def _accept(self) -> None:
        try:
            sock, addr = self._listener.accept()  # type: ignore[union-attr]
        except OSError:
            return
        if len(self._clients) >= MAX_CONNECTIONS:
            # A live session plus a challenger already. Refusing here bounds the
            # work an unauthenticated peer can create; the challenger slot frees
            # itself when the auth window expires.
            self._emit(f"refused {addr}: no free slot")
            try:
                sock.sendall(encode(MsgType.ERROR, b"busy"))
            except OSError:
                pass
            sock.close()
            return

        sock.setblocking(False)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        nonce = generate_nonce()
        c = ClientState(
            sock=sock,
            addr=addr,
            reader=FrameReader(),
            nonce=nonce,
            connected_at=time.monotonic(),
            mirror=Grid.blank(),
        )
        self._clients.append(c)
        self.counters.connects += 1
        self._sel.register(sock, selectors.EVENT_READ, c)
        self._emit(f"client {addr} connected; challenging")
        self._send_to(c, encode(MsgType.NONCE, nonce))

    def _drop(self, c: ClientState, why: str) -> None:
        # Remove first, and let that decide whether this call owns the drop.
        # Check-then-remove loses a race that stop() creates: it runs on the
        # caller's thread while serve_forever() may still be dropping clients on
        # its own, so the entry can vanish between the check and the removal and
        # list.remove() then raises. Removal is a single interpreter step, so
        # exactly one caller does the work and the loser returns quietly.
        try:
            self._clients.remove(c)
        except ValueError:
            return
        if c.pair is not None:
            # A console that left mid-pairing ends the window, and `dsterm
            # pair` says so rather than waiting on a code nobody will see.
            pairing.end_window("the console left before pairing finished",
                               self.config_dir)
            c.pair = None
        # A deferred message is connection-scoped, never a queue for whichever
        # card authenticates next.
        with self._ext_lock:
            c.ext.pending = None
            c.ext.queued = None
        try:
            self._sel.unregister(c.sock)
        except (KeyError, ValueError):
            pass
        try:
            c.sock.close()
        except OSError:
            pass
        self.counters.drops += 1
        self._emit(f"client {c.addr} dropped: {why}")
        if c.authed:
            self._camera_card_gone(c)
            self._voice_card_gone(c)
            self._mirror_card_gone()
            if not any(other.authed for other in self._clients):
                self._awake.release()

    def _expire_unauthed(self) -> None:
        now = time.monotonic()
        for c in list(self._clients):
            if c.authed or now - c.connected_at <= self.auth_timeout:
                continue
            # A card mid-pairing waits on a person comparing codes; the
            # window's own expiry bounds it, not the auth timeout.
            if c.pair is not None and pairing.current_window(self.config_dir) is not None:
                continue
            self._drop(c, "pairing window closed" if c.pair is not None
                       else "authentication timed out")

    def _send_to(self, c: ClientState, data: bytes) -> None:
        """Queue bytes for this client and push as much as the kernel accepts.

        Never sendall(). The socket is non-blocking, so sendall() raises
        BlockingIOError the moment the send buffer fills -- and it may already
        have written part of the data before raising, which leaves no safe way
        to retry. Treating that as a failure dropped the handheld mid-burst:
        input triggers a resync, the resync fills the buffer, the client is hung
        up on and reconnects. From the sofa that is "it disconnects when I type".

        A full buffer is backpressure, not an error. The unsent tail waits in
        c.pending and goes out on the next push.

        This is also where a sealed connection's frames are sealed, and it is
        the only place that can be right. Every byte bound for a client passes
        through here, and c.pending is a strict FIFO onto the socket: appended
        only here, consumed only from the front by _push, never withdrawn. So
        the order frames are sealed in is the order the card reads them, which
        is exactly what its counter assumes. Anywhere earlier is wrong: the
        outbox is reordered and discarded from -- a resync clears it, a switch
        installs a barrier, a withdrawn extension is filtered out -- and a
        frame sealed at enqueue time and then thrown away would leave a gap in
        the counter that ends the connection on the next frame.

        ``data`` may be several frames back to back; each is sealed on its own
        with its own counter step, which is what the card verifies.
        """
        if c.seal is not None:
            try:
                data = c.seal.seal(data)
            except ProtocolError as exc:
                # Something here handed over bytes that are not whole frames,
                # or the counter ran out. Neither may go out unsealed, and
                # neither can be skipped without desynchronising the counter,
                # so the connection ends; the card reconnects in a second.
                self._drop(c, f"could not seal an outgoing frame: {exc}")
                return
        c.pending += data
        self.counters.messages_out += 1
        self._push(c)

    def _push(self, c: ClientState) -> None:
        while c.pending:
            try:
                sent = c.sock.send(c.pending)
            except (BlockingIOError, InterruptedError):
                break          # buffer full or interrupted: the tail waits
            except OSError as exc:
                self._drop(c, f"send failed: {exc}")
                return
            if sent <= 0:
                break
            del c.pending[:sent]
            self.counters.bytes_out += sent

        # A tail this large means the client has stopped draining for long
        # enough that everything queued behind it is stale anyway. Dropping is
        # honest here in a way it is not for a momentarily full buffer.
        if len(c.pending) > PENDING_LIMIT:
            self._drop(c, f"send backlog {len(c.pending)} bytes; client not draining")

    def _send(self, data: bytes) -> None:
        """Send to whoever owns the session."""
        c = self._authed()
        if c is not None:
            self._send_to(c, data)

    def _queue(self, data: bytes) -> None:
        """Hold a screen update for paced release. See MESSAGES_PER_TICK."""
        c = self._authed()
        if c is not None:
            c.outbox.append(data)

    def _queue_batch(self, c: ClientState, messages: list[bytes]) -> None:
        """Append a non-interleavable FIFO batch for one client."""
        if messages and c in self._clients:
            c.outbox.append(OutboundBatch(messages))

    def _complete_session_snapshot(self, c: ClientState,
                                   batch: SessionSnapshotBatch) -> None:
        """Make a fully released snapshot this client's current row map."""
        if c.outbox and c.outbox[0] is batch:
            c.outbox.popleft()
        c.sessions.rows = list(batch.rows)
        c.sessions.signature = batch.signature
        if c.sessions.batch is batch:
            c.sessions.batch = None
        self._start_coalesced_session_snapshot(c)

    def _complete_remote_snapshot(self, c: ClientState,
                                  batch: RemoteSnapshotBatch) -> None:
        """Make a fully released REMOTE set this client's pick mapping."""
        if c.outbox and c.outbox[0] is batch:
            c.outbox.popleft()
        c.agents.rows = list(batch.rows)
        c.agents.signature = batch.signature
        c.agents.ages = batch.ages
        c.agents.sent_at = time.monotonic()
        if c.agents.batch is batch:
            c.agents.batch = None

    def _complete_outbound_batch(self, c: ClientState,
                                 batch: OutboundBatch) -> None:
        """Finish a released generic batch, including a switch barrier."""
        if c.outbox and c.outbox[0] is batch:
            c.outbox.popleft()
        if c.sessions.switch_barrier is batch:
            c.sessions.switch_barrier = None
        if batch.promote_rows is not None:
            c.sessions.rows = list(batch.promote_rows)
            c.sessions.signature = batch.promote_signature

    def _finish_drained_outbox_head(self, c: ClientState) -> None:
        """Promote any final frame whose non-blocking tail just drained."""
        if c.pending:
            return
        batch = c.sessions.batch
        if batch is not None and batch.offset == len(batch.messages):
            self._complete_session_snapshot(c, batch)
        rbatch = c.agents.batch
        if rbatch is not None and rbatch.offset == len(rbatch.messages):
            self._complete_remote_snapshot(c, rbatch)
        if not c.outbox:
            return
        item = c.outbox[0]
        if isinstance(item, SessionSnapshotBatch) and item.offset == len(item.messages):
            self._complete_session_snapshot(c, item)
        elif isinstance(item, RemoteSnapshotBatch) and item.offset == len(item.messages):
            self._complete_remote_snapshot(c, item)
        elif isinstance(item, OutboundBatch) and item.offset == len(item.messages):
            self._complete_outbound_batch(c, item)

    @staticmethod
    def _keep_final_session_tail(c: ClientState) -> bool:
        """Whether ``c.sessions.batch`` is a complete frame still in ``pending``.

        Bytes already handed to a non-blocking socket cannot be withdrawn.  If
        the final row is in that tail, retain the batch object until the tail
        drains so an old mapping cannot outlive a complete wire snapshot.
        """
        batch = c.sessions.batch
        return (batch is not None and batch.offset == len(batch.messages)
                and bool(c.pending))

    @staticmethod
    def _keep_final_remote_tail(c: ClientState) -> bool:
        """``_keep_final_session_tail`` for the REMOTE set, for the same reason."""
        batch = c.agents.batch
        return (batch is not None and batch.offset == len(batch.messages)
                and bool(c.pending))

    def _clear_unsent_outbox(self, c: ClientState) -> None:
        """Discard queued work without forgetting a final in-flight row list."""
        keep_tail = self._keep_final_session_tail(c)
        keep_remote_tail = self._keep_final_remote_tail(c)
        c.outbox.clear()
        if not keep_tail:
            c.sessions.batch = None
        # A REMOTE set cut off here is not lost: its target is unchanged and
        # still differs from what was last fully sent, so the next tick queues
        # it again from row 0, which the card treats as a new set.
        if not keep_remote_tail:
            c.agents.batch = None
        c.sessions.next_snapshot = None

    @staticmethod
    def _discard_ext_locked(c: ClientState) -> None:
        """Drop every unsent extension message while ``_ext_lock`` is held."""
        c.ext.pending = None
        c.ext.queued = None
        c.outbox = deque(item for item in c.outbox
                         if not isinstance(item, ExtensionFrame))

    @staticmethod
    def _discard_ext_withdrawn_locked(c: ClientState) -> None:
        """Drop unsent extension messages whose token the card no longer grants."""
        if c.ext.pending is not None and c.ext.pending.token not in c.caps.tokens:
            c.ext.pending = None
        if c.ext.queued is not None and c.ext.queued.token not in c.caps.tokens:
            c.ext.queued = None
        c.outbox = deque(item for item in c.outbox
                         if not (isinstance(item, ExtensionFrame) and
                                 item.token not in c.caps.tokens))

    @staticmethod
    def _queued_message_count(c: ClientState) -> int:
        """Count frames, not container objects, for backpressure decisions."""
        total = 0
        for item in c.outbox:
            if isinstance(item, bytes) or isinstance(item, ExtensionFrame):
                total += 1
            else:
                total += len(item.messages) - item.offset
        return total

    def _flush_outbox(self) -> None:
        c = self._authed()
        if c is None:
            return
        # Drain last tick's tail first, and stop there if it is still not gone:
        # piling more on a socket that has not accepted what it already has is
        # how the backlog runs away.
        self._push(c)
        if c not in self._clients or c.pending:
            return
        # A final state frame may have been retained only as a socket tail.
        # Promote its mapping/barrier before beginning another queued message.
        self._finish_drained_outbox_head(c)
        for _ in range(MESSAGES_PER_TICK):
            if not c.outbox or c not in self._clients or c.pending:
                return
            item = c.outbox[0]
            if isinstance(item, bytes):
                c.outbox.popleft()
                self._send_to(c, item)
                continue
            if isinstance(item, SessionSnapshotBatch):
                # Keep the prior complete mapping valid while this new list is
                # staged. A client cannot legitimately pick the new rows until
                # it has received every index exactly once.
                # A non-blocking send can retain the tail of the final entry.
                # Do not promote the rows until that tail has drained; until
                # then an already-received old list remains the only one a
                # client can legitimately pick from.
                if item.offset == len(item.messages):
                    self._complete_session_snapshot(c, item)
                    continue
                if item.offset == 0:
                    c.sessions.batch = item
                data = item.messages[item.offset]
                item.offset += 1
                self._send_to(c, data)
                if c not in self._clients:
                    return
                if item.offset == len(item.messages) and not c.pending:
                    self._complete_session_snapshot(c, item)
                elif item.offset == len(item.messages):
                    # The last entry is now a TCP tail. Pop its completed FIFO
                    # batch, but retain ``c.sessions.batch`` until _push drains it.
                    c.outbox.popleft()
                continue
            if isinstance(item, RemoteSnapshotBatch):
                # SessionSnapshotBatch's handling, row for row. Checked again
                # at serialization: a later HELLO may have withdrawn "remote",
                # and an older card halts on a type it does not know.
                if not c.caps.remote:
                    c.outbox.popleft()
                    if c.agents.batch is item:
                        c.agents.batch = None
                    continue
                if item.offset == len(item.messages):
                    self._complete_remote_snapshot(c, item)
                    continue
                if item.offset == 0:
                    c.agents.batch = item
                data = item.messages[item.offset]
                item.offset += 1
                self._send_to(c, data)
                if c not in self._clients:
                    return
                if item.offset == len(item.messages) and not c.pending:
                    self._complete_remote_snapshot(c, item)
                elif item.offset == len(item.messages):
                    c.outbox.popleft()
                continue
            if isinstance(item, ExtensionFrame):
                # The public API may replace this message until the instant it
                # is serialized. Once queued to the socket it is no longer
                # pending, so a subsequent call starts the next one.
                c.outbox.popleft()
                with self._ext_lock:
                    if c.ext.queued is item:
                        c.ext.queued = None
                    payload = item.payload
                    compatible = c.authed and item.token in c.caps.tokens
                if not compatible:
                    continue
                self._send_to(c, encode(MsgType.EXTENSION, payload))
                continue
            if item.offset == len(item.messages):
                self._complete_outbound_batch(c, item)
                continue
            data = item.messages[item.offset]
            item.offset += 1
            self._send_to(c, data)
            if c not in self._clients:
                return
            if item.offset == len(item.messages) and not c.pending:
                self._complete_outbound_batch(c, item)

    # -- client input ------------------------------------------------------

    def _read_client(self, c: ClientState) -> None:
        # If the peer has a readable pick exactly while the final advertised
        # frame is a non-blocking socket tail, finish pushing that tail and
        # promote its row map before resolving the incoming index.
        if c.pending:
            self._push(c)
            if c not in self._clients or c.pending:
                return
        self._finish_drained_outbox_head(c)
        try:
            data = c.sock.recv(MAX_PAYLOAD)
        except BlockingIOError:
            return
        except OSError as exc:
            self._drop(c, f"recv failed: {exc}")
            return
        if not data:
            self._drop(c, "peer closed")
            return
        c.last_heard_at = time.monotonic()
        self.counters.bytes_in += len(data)
        try:
            for msg in c.reader.feed(data):
                self.counters.messages_in += 1
                self._handle(c, msg)
                if c not in self._clients:
                    return
        except SealError as exc:
            # A sealed connection received a frame whose tag is wrong or
            # missing: injected, replayed, reordered, or with a frame dropped
            # before it. Nothing in it was acted on -- the reader checks the
            # tag before the type -- and nothing after it can be trusted.
            self._drop(c, f"integrity check failed: {exc}")
        except ProtocolError as exc:
            # Never try to resynchronise: a bad length prefix means every
            # subsequent byte offset is suspect.
            self._drop(c, f"protocol error: {exc}")

    def _handle(self, c: ClientState, msg: Message) -> None:
        if c.paired:
            # The card closes a pairing connection itself; a peer that goes on
            # -- an AUTH with the key it was just given, say -- does so on a
            # connection that never authenticates, as SECURITY.md says.
            self._drop(c, f"{msg.type.name} after pairing: authenticate on a new connection")
            return
        if not c.authed and msg.type not in PRE_AUTH_ALLOWED:
            self._drop(c, f"{msg.type.name} before authentication")
            return

        if msg.type is MsgType.HELLO:
            self._on_hello(c, msg)
            return

        if msg.type is MsgType.AUTH:
            self._on_auth(c, msg)
            return

        if msg.type is MsgType.PAIR_HELLO:
            self._pair_hello(c, msg.payload)
            return

        if msg.type is MsgType.PAIR_CONFIRM:
            self._pair_confirm(c, msg.payload)
            return

        if msg.type is MsgType.PAIR_ACCEPT:
            self._pair_accept(c, msg.payload)
            return

        if msg.type is MsgType.KEY:
            self._on_key(c, msg)
            return

        if msg.type is MsgType.ANSWER:
            self._on_answer(c, msg)
            return

        if msg.type is MsgType.SESSION_PICK:
            self._on_session_pick(c, msg)
            return

        if msg.type in (MsgType.REMOTE_OPEN, MsgType.REMOTE_PICK):
            self._on_remote(c, msg)
            return

        if msg.type is MsgType.VIEW:
            self._on_view(c, msg)
            return

        if msg.type is MsgType.RESYNC:
            self._resync()
            return

        if msg.type is MsgType.PING:
            self._send_to(c, encode(MsgType.PONG))
            return

        if msg.type is MsgType.SUBSCREEN:
            self._on_subscreen(c, msg)
            return

        if msg.type in (MsgType.CAMERA, MsgType.CAMERA_FRAME,
                        MsgType.PHOTO_CHUNK, MsgType.PHOTO_END,
                        MsgType.PHOTO_DISCARD):
            self._on_camera(c, msg)
            return

        if msg.type in (MsgType.VOICE_START, MsgType.VOICE_CHUNK,
                        MsgType.VOICE_END):
            self._on_voice(c, msg)
            return

        self._drop(c, f"unexpected message {msg.type.name}")

    def _on_hello(self, c: ClientState, msg: Message) -> None:
        """HELLO: the card's capability tokens, its grid and its device id."""
        # Feature tokens only. Nothing here grants anything: it selects which
        # optional messages this client is safe to be sent, and an unknown
        # token is ignored so neither side needs the other to be current.
        features = hello_features(msg.payload)
        # A card built for the other grid would be sent cells it has no
        # room for.  Say which size each side is, in words the card shows
        # on its menu, and end it there: the fix is a different ROM or a
        # `--grid` line in serve.conf, not anything the link can do.
        grid = hello_grid(features)
        if grid is not None and grid != (COLS, ROWS):
            why = (f"grid: this server is {COLS}x{ROWS}, this card is "
                   f"{grid[0]}x{grid[1]}").encode("ascii")
            self._send_to(c, encode(MsgType.ERROR, why))
            self._emit(f"client {c.addr} refused: {why.decode()}")
            self._drop(c, "grid mismatch")
            return
        c.caps.mode = FEATURE_MODE in features
        c.caps.snippets = FEATURE_SNIPPETS in features
        c.caps.tokens = frozenset(features)
        c.caps.project = FEATURE_PROJECT in features
        c.caps.project64 = FEATURE_PROJECT64 in features
        c.caps.clip = FEATURE_CLIP in features
        # Asked for AND offered: with --no-remote this stays False, so the
        # card is never sent a REMOTE frame and therefore never sends one.
        c.caps.remote = FEATURE_REMOTE in features and self._remote_offered()
        c.caps.state = FEATURE_STATE in features
        c.caps.commands = FEATURE_COMMANDS in features
        c.caps.mirror = FEATURE_MIRROR in features
        c.caps.ask = FEATURE_ASK in features
        # Asked for AND offered: under --no-camera this stays False, so
        # no READY is sent and any camera frame is a protocol error.
        c.caps.camera = FEATURE_CAMERA in features and self._camera_offered()
        c.caps.mic = FEATURE_MIC in features and self._voice_offered()
        # Only read at AUTH. A HELLO after that -- sealed, like every
        # frame then -- cannot turn sealing off or on again: the decision
        # was AUTH_OK's, and both counters are already running.
        # SEAL v3 needs both the word and this connection's card nonce: a
        # HELLO without the nonce could be a replay's, and the nonce is what
        # keeps the card from reusing an old keystream (docs/SEAL.md).
        c.wants_seal = (FEATURE_SEAL3 in features
                        and hello_card_nonce(features) is not None)
        if not c.authed:
            c.hello = bytes(msg.payload)
        c.device_id = hello_device_id(features)
        # Say what the card asked for. The handshake is one-way and this
        # server announces nothing back, so without this line the only
        # record of what a connection negotiated is the behaviour that
        # follows from it -- and when the clipboard bridge did not light
        # up, the only way to establish that the card was not advertising
        # "clip" was to reason from the absence of other log lines. One
        # line would have said it. Tokens are capability names, never
        # content, so there is nothing here worth withholding.
        if features:
            c.caps.announced = " ".join(sorted(features))
            self._emit(f"client {c.addr} asked for: {c.caps.announced}")
        else:
            self._emit(f"client {c.addr} asked for nothing optional")
        # A reconnecting card has been told nothing yet, whatever this
        # object remembers.
        c.feeds.project_signature = ()
        c.feeds.clip_seq = -1
        self._reset_remote(c)
        c.agents.state_sent = None
        # A later HELLO may withdraw an optional capability. Do not retain
        # a deferred message for a connection that no longer grants the
        # token it needs.
        with self._ext_lock:
            self._discard_ext_withdrawn_locked(c)

    def _on_auth(self, c: ClientState, msg: Message) -> None:
        """AUTH: check the MAC, start SEAL, take over the session, and send what
        the card asked for."""
        if c.seal is not None:
            # Sealing is decided once. A second AUTH on a sealed connection
            # would answer with another AUTH_OK and restart both counters
            # mid-stream, which neither side could follow; no card sends
            # one, so a connection that does is not following the contract.
            self._drop(c, "AUTH on a connection that is already sealed")
            return
        if not c.wants_seal or c.hello is None:
            # Sealing is not optional: a card that does not ask for it is
            # either one built before it existed, or a HELLO with the word
            # taken out on the way -- and the second is the reason to
            # refuse the first. ERROR, never AUTH_FAIL: the card's key is
            # fine, and a card must not take this for a revoked pairing.
            self._send_to(c, encode(
                MsgType.ERROR,
                b"this card's dsterm.nds is older than the computer's: "
                b"copy the current one to the card (dsterm card)"))
            self._drop(c, "refused: the card did not ask for encrypted frames")
            return
        # Looked up once: the key the MAC is checked against is the key
        # the seal is derived from, and _key_for logs when it falls back.
        key = self._key_for(c)
        if verify_mac(key, c.nonce, msg.payload):
            # Proof of the key is what earns the session, so the takeover
            # happens here and nowhere earlier. A handheld that was switched
            # off still holds an ESTABLISHED socket this side cannot tell
            # from a live one; the reconnecting device displaces it.
            for other in list(self._clients):
                if other is not c and other.authed:
                    self._drop(other, "replaced by a newer authenticated client")
            c.authed = True
            self.counters.auth_ok += 1
            # SEAL: "seal" in the AUTH_OK is the decision, and the AUTH_OK
            # itself is the last frame either side sends unsealed. It goes
            # into c.pending before c.seal exists, so _send_to leaves it
            # alone; everything after it -- MODE, snippets, the resync
            # below -- is sealed from S0 on. The reader is switched here,
            # inside the loop still iterating it, so any bytes already
            # buffered behind this AUTH are held to the seal as C0: a card
            # sends nothing until it has read AUTH_OK, so anything there
            # was spliced in, and must not ride through unsealed.
            self._send_to(c, encode(MsgType.AUTH_OK, SEAL_AUTH_OK))
            c.seal = Seal.for_server(seal_key(key, c.nonce, c.hello))
            c.reader.start_sealing(c.seal)
            self._awake.hold()
            self._emit(f"client {c.addr} authenticated; every frame encrypted and sealed")
            self._send_mode(c)
            self._push_snippets(c)
            # Visual recovery is a paced batch. Put the first complete
            # session snapshot after it so resync cannot discard half of
            # an advertisement during authentication.
            self._resync(c)
            self._advertise_sessions(c, force=True)
            # After the resync, like the session snapshot: a card that has
            # just reconnected should find the current clipboard waiting
            # rather than the one it had before it went away.
            self._advertise_clip(c)
            # "Not asked yet", to a card that announced "remote" and
            # nothing to one that did not. Receiving it is the card's
            # proof that REMOTE_OPEN and REMOTE_PICK are safe to send.
            if c.caps.remote:
                c.agents.target = remote.NOT_ASKED
                self._advertise_remote(c)
            # STATE: the current reading, or 0. A reading from before the
            # last card left says nothing about now, so it is dropped and
            # this card hears "unknown" until a fresh ask lands -- never a
            # stale "working" that the next answer turns into a DONE cue.
            # A REMOTE card is owed the same: its first pick republishes
            # the last reading as its list.
            if c.caps.state or c.caps.remote:
                self._state_for_new_card()
            if c.caps.state:
                self._advertise_state(c)
            c.feeds.commands = None
            self._advertise_commands(c)
            c.mirror_told = None
            self._tell_mirror(c)
            # PHOTO: the card's cue that camera messages are now legal --
            # sent after the rest so nothing camera-typed precedes the
            # state the card needs to read first.
            if c.caps.camera:
                self._send_to(c, encode_camera_ready(self._camera_can_save()))
            if c.caps.mic:
                self._send_to(c, encode_mic_ready(self._voice_can_transcribe()))
        else:
            self.counters.auth_failed += 1
            self._send_to(c, encode(MsgType.AUTH_FAIL))
            self._drop(c, "bad MAC")

    def _on_key(self, c: ClientState, msg: Message) -> None:
        """KEY: bytes for the PTY, the one thing a card can do to the session."""
        try:
            keys = decode_key(msg.payload)
        except ProtocolError as exc:
            self._drop(c, f"bad key payload: {exc}")
            return
        self._type(c, keys, None)

    def _on_answer(self, c: ClientState, msg: Message) -> None:
        """ANSWER: key bytes tied to the question they answer."""
        if not c.caps.ask:
            # Only a card that announced "ask" is ever sent an ask id, and
            # only one that has been sent one may send this.
            self._drop(c, "ANSWER from a card that did not announce ask")
            return
        try:
            ask_id, keys = decode_answer(msg.payload)
        except ProtocolError as exc:
            self._drop(c, f"bad answer payload: {exc}")
            return
        self._type(c, keys, ask_id)

    def _type(self, c: ClientState, keys: bytes, ask_id: int | None) -> None:
        # Typing returns to the live screen. Otherwise the keystroke lands
        # somewhere the user cannot see, which is how a terminal loses
        # someone's trust fastest.
        c.last_key_at = time.monotonic()
        if not self._gate_keys(c, keys, ask_id):
            return
        if self._view_to_live(c):
            self._resync()
        if self._session is not None:
            if not self._session.write(keys):
                self._emit("input dropped: the session has stopped reading "
                           "what is typed")
            else:
                self._ask_keys_sent(keys)
                self._answered(c, keys)

    # -- helpers the features share --------------------------------------

    def _sel_forget(self, sock) -> None:
        try:
            self._sel.unregister(sock)
        except (KeyError, ValueError):
            pass

    def _tmux_target(self) -> tuple[str, str] | None:
        return getattr(self._session, "_tmux_target", None) if self._session else None

    # -- frames ------------------------------------------------------------

    def _resync_messages(self, c: ClientState) -> list[bytes]:
        """Build a complete bounded repaint plus cursor for one client."""
        if self._session is None:
            return []
        grid = self._view_for(c)
        every_cell = [
            CellChange(i // COLS, i % COLS, cell)
            for i, cell in enumerate(grid.cells)
        ]
        messages = []
        for start in range(0, len(every_cell), CELLS_PER_MESSAGE):
            messages.append(encode_diff(every_cell[start : start + CELLS_PER_MESSAGE]))
        c.mirror = grid
        # The client's caret starts wherever the current one is; a resync leaves
        # nothing to diff against, so the cursor is stated rather than inferred.
        #
        # Hidden while scrolled back: the caret belongs to the live screen, and
        # drawing it over history would mark a cell the user is not typing into.
        cur = self._session.cursor if c.scroll.offset == 0 else (0, 0, False)
        c.cursor = cur
        messages.append(encode_cursor(*cur))
        return messages

    def _resync(self, c: ClientState | None = None) -> None:
        """Replace stale visual work with one self-contained bounded repaint."""
        c = self._authed() if c is None else c
        if c is None or c not in self._clients or self._session is None:
            return
        # A picked window has one required ordered recovery sequence.  A
        # concurrent RESYNC/scroll/key event must not clear that barrier and
        # leave the handheld with a repaint but no fresh metadata.
        if c.sessions.switch_pending or c.sessions.switch_barrier is not None:
            return
        # A resync is standalone, so any older diffs are useless. The current
        # session row mapping remains valid until a later complete snapshot is
        # promoted; an interrupted staged snapshot is simply retried on poll.
        self._clear_unsent_outbox(c)
        c.sessions.switch_barrier = None
        with self._ext_lock:
            c.ext.queued = None
        self.counters.resyncs += 1
        self._queue_batch(c, self._resync_messages(c))

    def _tick(self) -> None:
        if self._session is None:
            return
        # Always snapshot: the emulator must stay current whether or not anyone
        # is looking at the live screen, or scrolling back would freeze history
        # itself and the view would be wrong on the way down.
        live, _, stats = self._session.snapshot()

        # Housekeeping that does not need a card: the camera's worker results
        # and its feed's viewers, scrollback's captures, `dsterm view`.
        self._camera_tick()
        self._voice_tick()
        self._collect_view_results()
        self._view_tick(live)

        c = self._authed()
        if c is None:
            return

        now = time.monotonic()
        self._ask_tick(live.cells, c, now)
        snapshot = self._sessions_tick(now, stats.bytes_in)
        self._feeds_tick(c, now)

        # The other Claude sessions and the one on screen, likewise: `claude
        # agents` starts the whole CLI, so it is asked on a worker thread and
        # only while a card is owed an answer.
        self._poll_agents(now)

        if c.sessions.switch_pending:
            # Yield one ordinary render update after select-window. tmux normally
            # redraws immediately, but this bounded delay avoids publishing a
            # stale screen when its PTY notification arrives one event later.
            if c.sessions.switch_wait_ticks:
                c.sessions.switch_wait_ticks -= 1
                self._flush_outbox()
                return
            self._install_switch_barrier(c)
            # A REMOTE pick's note and any set it changed go BEHIND the
            # barrier: the card hears "opening x" after it can see x.
            self._release_remote(c, now)
            # Do not release an extension's message until the switch barrier
            # itself is drained.
            self._queue_pending_extension(c)
            self._flush_outbox()
            return

        # Before ordinary screen work: a child entering or leaving the alternate
        # screen changes what the handheld's scroll button means, so it remains
        # immediate except while a switch barrier owns that ordering.
        self._send_mode(c)
        if snapshot is not None:
            self._advertise_sessions(c, snapshot=snapshot)

        grid = live if c.scroll.offset == 0 else self._view_for(c)

        if grid.cells != c.mirror.cells:
            # Scrolling changes ~90% of cells but is describable in three bytes.
            lines = grid.detect_scroll(c.mirror)
            if lines:
                shifted = c.mirror.shifted(lines)
                residual = grid.diff(shifted)
                self._queue(encode_scroll(0, ROWS - 1, lines))
                if residual:
                    self._send_changes(residual, grid)
            else:
                self._send_changes(grid.diff(c.mirror), grid)
            c.mirror = grid

            queued = self._queued_message_count(c)
            if queued > OUTBOX_LIMIT:
                # Output is outrunning the release rate. A resync stands alone,
                # so replacing the backlog with one loses nothing but the
                # intermediate frames -- which the screen could not have shown
                # in time anyway.
                self._emit(f"outbox backlog {queued}; collapsing to a resync")
                self._resync(c)
                if snapshot is None:
                    snapshot = self._current_windows()
                self._advertise_sessions(c, force=True, snapshot=snapshot)

        # The caret moves on its own schedule -- typing, prompts, any cursor
        # addressing -- independent of the cell diff, so it is sent whenever it
        # changes rather than whenever a cell does. Hidden while scrolled back,
        # where it would mark a cell nobody is typing into.
        cur = self._session.cursor if c.scroll.offset == 0 else (0, 0, False)
        if cur != c.cursor:
            c.cursor = cur
            self._queue(encode_cursor(*cur))

        # The amount of history grows as output scrolls, so a client sitting
        # still still needs telling that there is more behind it now.
        self._send_view_pos(c)
        self._release_remote(c, now)
        self._advertise_state(c)
        self._queue_pending_extension(c)
        self._flush_outbox()

    def _send_changes(self, changes: list, grid: Grid) -> None:
        """Emit changes as diffs small enough to cross the handheld's stack.

        A large repaint is many small messages rather than one FRAME_FULL. That
        costs a few extra headers and is the difference between a screen that
        updates and one that never does.
        """
        if not changes:
            return
        for start in range(0, len(changes), CELLS_PER_MESSAGE):
            self._queue(encode_diff(changes[start : start + CELLS_PER_MESSAGE]))
