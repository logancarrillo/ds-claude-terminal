"""The small pushed feeds: MODE, snippets, slash commands, the PROJECT page and
the clipboard.

Part of the server, split out by feature: the methods below are
mixed into Server (server.py) and share its state through `self`.
"""

from __future__ import annotations

import os

from . import commands as dscommands
from . import clip as dsclip
from . import project as dsproject
from . import snippets
from .protocol import (
    MODE_APP_SCROLLS,
    ProtocolError,
    encode_mode,
    encode_snippet,
    encode_commands,
)
from .connection import ClientState


# The computer's slash commands (commands.py) are looked for again this often:
# a few directory listings, cheap, and a command you just wrote shows up on
# the handheld without reconnecting.
COMMANDS_POLL_SECONDS = 15.0


class FeedsMixin:
    """Owns the project and clipboard pollers and the commands' clock
    (_init_feeds); per connection, ClientState.feeds."""

    def _advertise_commands(self, c: ClientState) -> None:
        """Send this card the computer's slash commands (commands.py) when it
        asked for them and the list is not what it was last sent -- which,
        just after AUTH_OK, is nothing, so it always goes then."""
        if not c.caps.commands or not c.authed:
            return
        names = dscommands.discover(self.cwd or os.getcwd())
        if names == c.feeds.commands:
            return
        try:
            frame = encode_commands(names)
        except ProtocolError as exc:
            self._emit(f"commands not sent: {exc}")
            return
        c.feeds.commands = names
        self._send_to(c, frame)

    def _push_snippets(self, c: ClientState) -> None:
        """Send this client the server's snippet set, once, after AUTH_OK.

        Re-read from disk on every connect rather than cached at startup, so
        editing the file is the whole workflow: change it, reconnect, it is
        there. That is the entire point of the feature -- the alternative is
        carrying the card to another machine.

        Validated before anything is encoded. The card discards an entire push
        on one bad chunk, so a set that would be refused must never leave here;
        a snippet that silently vanished on the handheld would look like a wire
        fault rather than a file the server should have rejected.

        Sent whole or not at all. A partial push leaves the card holding its
        card-file list, which is the right fallback: a half-menu is worse than
        an old one.
        """
        if not c.caps.snippets or self.snippets_path is None:
            return
        try:
            items = snippets.load(self.snippets_path)
            snippets.validate(items)
        except snippets.SnippetError as exc:
            self._emit(f"snippets not sent: {exc}")
            return
        if not items:
            return
        for i, s in enumerate(items):
            self._send_to(c, encode_snippet(i, len(items), s.label, s.body))
            if c not in self._clients:
                return
        self._emit(f"client {c.addr} sent {len(items)} snippets")

    def _advertise_project(self, c: ClientState) -> None:
        """Queue a complete PROJECT snapshot, if the repo said anything new.

        Unsolicited, like SESSIONS and for the same reason: there is no request
        message and there should not be one. The card asked to be told, by
        advertising the token, and it is told when there is something to tell.
        """
        if c not in self._clients or not c.caps.project:
            return
        rows = self._project.rows
        signature = tuple((r.tab, r.label, r.value, r.fg) for r in rows)
        if signature == c.feeds.project_signature:
            return
        c.feeds.project_signature = signature
        # A 1.0.x card (no "project64") gets the 48 rows it can hold, in the
        # ASCII it can draw; see project.encode.
        for frame in dsproject.encode(rows, project64=c.caps.project64):
            c.outbox.append(frame)

    def _advertise_clip(self, c: ClientState) -> None:
        """Queue the current clipboard, if this client has not been sent it.

        Four gates, all of which must hold: the server was started with --clip,
        the card advertised the token, the client is authenticated, and there is
        a reading to send. Unsolicited like SESSIONS and PROJECT, and for the
        same reason -- there is no request message and there must not be one.

        Never logs the text. The event says how much and how many, because a
        clipboard in the journal is permanent and at rest, which is a different
        bargain from the transient LAN exposure that was actually accepted.
        """
        if self._clip is None or not c.caps.clip or not c.authed:
            return
        if c not in self._clients:
            return
        snapshot = self._clip.snapshot
        if snapshot is None:
            return
        seq = self._clip.seq
        if c.feeds.clip_seq == seq:
            return
        c.feeds.clip_seq = seq
        for frame in dsclip.chunks(snapshot, seq):
            c.outbox.append(frame)
        if snapshot.withheld:
            self._emit(f"client {c.addr} clip withheld (concealed)")
        else:
            self._emit(f"client {c.addr} clip {snapshot.total}B "
                       f"flags={snapshot.flags:#04x}")

    def _mode_frame(self, c: ClientState, *, force: bool = False) -> bytes | None:
        """Return a fresh MODE frame and record it, or ``None`` if unnecessary."""
        if not c.caps.mode or self._session is None:
            return None
        flags = MODE_APP_SCROLLS if self._session.app_scrolls else 0
        if flags == c.mode_flags and not force:
            return None
        c.mode_flags = flags
        self._emit(f"client {c.addr} mode app_scrolls={bool(flags)}")
        return encode_mode(bool(flags))

    def _send_mode(self, c: ClientState, *, force: bool = False) -> None:
        """Send MODE immediately unless a switch barrier owns the ordering."""
        data = self._mode_frame(c, force=force)
        if data is not None:
            # Do not insert a different state message between the entries of a
            # staged list. The snapshot itself is FIFO; this preserves that
            # simple atomicity even when MODE changes at the same instant.
            if (self._switch_in_progress(c)
                    or self._session_batch_queued_or_active(c)
                    or self._remote_batch_queued_or_active(c)):
                c.outbox.append(data)
            else:
                self._send_to(c, data)

    def _poll_project(self, now: float) -> None:
        """One project tick: publish a finished snapshot, start the next.

        Never blocks -- the collection runs on its own thread. Split out of
        ``_tick`` so that the wiring is testable without a live session, which
        is precisely the gap that let ``quiet_s`` be added, rendered, tested,
        and then never passed by anyone.
        """
        if self._project.poll(quiet_s=self._active_idle_seconds(now), now=now):
            for peer in tuple(self._clients):
                self._advertise_project(peer)

    def _poll_clip(self, now: float) -> None:
        """One clipboard tick: publish a new reading, start the next.

        Never blocks -- the read runs on its own thread. Split out of ``_tick``
        for the same reason ``_poll_project`` is: the wiring itself has to be
        testable without a live session, because the failure this project keeps
        repeating is a feature that is built, rendered, tested, and then never
        actually called.
        """
        if self._clip is None:
            return
        # Nobody to tell, nothing to read. Reading spawns PowerShell and costs
        # ~0.2 s of a worker thread every interval; doing that when no card has
        # advertised the token is a process every three seconds forever in
        # exchange for frames that can never be sent. It also means the
        # clipboard is not being read at all until something asks for it, which
        # is the better default for a feature that handles what people copy.
        if not any(c.authed and c.caps.clip for c in self._clients):
            return
        if self._clip.poll(now=now):
            for peer in tuple(self._clients):
                self._advertise_clip(peer)

    # -- what the core calls (server.py) -------------------------------------

    def _init_feeds(self, cwd, snippets_path, clip_enabled: bool,
                    project_ci: bool = False) -> None:
        """The snippets file, the project and clipboard pollers, and the
        slash commands' clock."""
        # Read on every connect, not here: editing the file and reconnecting is
        # the whole workflow this exists for.
        self.snippets_path = snippets_path
        # Gathered on a worker thread, not inline: git is forked to answer, and
        # against a large working directory one snapshot measured 1.59 s.
        # Polled inline that is over half of every interval with no frames
        # reaching the handheld. See BackgroundCollector for the measurements.
        # project_ci (--project-ci) adds GitHub Actions runs to WATCH: `gh run
        # list`, a network call every 30 s, so off unless asked for.
        self._project = dsproject.BackgroundCollector(cwd or ".", ci=project_ci)
        # Off unless --clip was passed. The link is encrypted (SEAL v3), but
        # bridging a clipboard still widens where every copy goes: anything
        # copied on any machine synced to this one reaches the DS. A feature
        # that does that does not get to be on by default. None when disabled, so there is no reader
        # to accidentally start.
        self.clip_enabled = clip_enabled
        # on_problem goes to the ordinary event log so a bridge that cannot read
        # the clipboard says so once, rather than looking identical to a bridge
        # nobody has copied anything into. It carries no clipboard text.
        self._clip = (dsclip.ClipReader(on_problem=self._emit)
                      if clip_enabled else None)
        # When commands.py last looked (COMMANDS_POLL_SECONDS).
        self._commands_polled_at = 0.0

    def _stop_feeds(self) -> None:
        # Before the sockets: a collection in flight holds nothing the shutdown
        # needs, and the worker is a daemon bounded by _GIT_TIMEOUT, so this
        # cannot be what keeps the process alive.
        self._project.stop()
        if self._clip is not None:
            self._clip.stop()

    def _feeds_tick(self, c: ClientState, now: float) -> None:
        # The repository, on its own much slower clock and on its own thread.
        self._poll_project(now)

        # QUICK's commands: a few directory listings, every few seconds.
        if now - self._commands_polled_at >= COMMANDS_POLL_SECONDS:
            self._commands_polled_at = now
            self._advertise_commands(c)

        # The clipboard, likewise: reading it spawns PowerShell and costs about
        # 0.2 s, which is three frames at 15 fps and must never be spent here.
        self._poll_clip(now)
