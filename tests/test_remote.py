"""REMOTE: the other Claude sessions on this machine, against docs/REMOTE.md.

What is pinned here, in the order the contract states it:

* what ``claude agents --json`` is allowed to look like, and that nothing it
  can print -- junk included -- takes the server down;
* how each entry becomes a row: state, flags, sort order, clamping;
* the frames, byte for byte, because a C decoder is being written against the
  same table in parallel and the hex here is the interop vector;
* the gates: no REMOTE frame to a card that did not ask, a "not asked yet"
  frame to one that did, and the client->server types refused on a connection
  that never negotiated them;
* picks: the index means "the row I last sent you", the action is re-checked
  against that row's flags, one pick a second, and every argv is the server's;
* the poller: `claude agents` is asked only while a card has the screen open.

Nothing here runs a real ``claude`` or a real tmux. The runner, ``which``, the
tmux window list and ``new-window`` are all fakes, so the suite cannot open a
window in -- or even list -- anyone's live session.
"""

from __future__ import annotations

import json
import socket
import subprocess
import time

import pytest

from dsterm import remote, sessions
from dsterm.auth import generate_key
from dsterm.client import Client
from dsterm.grid import Grid
from dsterm.protocol import (
    FEATURE_REMOTE,
    HEADER_SIZE,
    REMOTE_ACTION_COPY,
    REMOTE_ACTION_NEW,
    REMOTE_ACTION_OPEN,
    REMOTE_COPY,
    REMOTE_HERE,
    REMOTE_KIND_BACKGROUND,
    REMOTE_KIND_INTERACTIVE,
    REMOTE_LIVE,
    REMOTE_NEW_INDEX,
    REMOTE_STATE_DONE,
    REMOTE_STATE_FAILED,
    REMOTE_STATE_IDLE,
    REMOTE_STATE_NEEDS_YOU,
    REMOTE_STATE_STOPPED,
    REMOTE_STATE_UNKNOWN,
    REMOTE_STATE_WORKING,
    REMOTE_STATUS_FAILED,
    REMOTE_STATUS_NO_CLAUDE,
    REMOTE_STATUS_NONE,
    REMOTE_STATUS_NOT_ASKED,
    Message,
    MsgType,
    ProtocolError,
    decode_remote,
    decode_remote_note,
    decode_remote_open,
    decode_remote_pick,
    encode_remote_empty,
    encode_remote_note,
    encode_remote_open,
    encode_remote_pick,
    encode_remote_row,
    remote_row_check,
)
from dsterm import server_agents as remote_server
from dsterm.server import ClientState, Server

from test_server import ServerHarness

CLAUDE = "/opt/fake/bin/claude"
SID_A = "eb5f4971-356e-4a22-9708-511d32724a4f"
SID_B = "2b7e1516-28ae-4d2a-a6f7-15884f3c4f3c"
NOW_MS = 1_800_000_000_000


# -- fakes ---------------------------------------------------------------------


class FakeRunner:
    """Answers the three commands observe() runs, and records every argv."""

    def __init__(self, agents=None, *, agents_rc=0, agents_text=None,
                 panes="", panes_rc=0, ps="", ps_rc=0):
        self.agents = agents if agents is not None else []
        self.agents_rc = agents_rc
        self.agents_text = agents_text
        self.panes, self.panes_rc = panes, panes_rc
        self.ps, self.ps_rc = ps, ps_rc
        self.calls: list[list[str]] = []

    def __call__(self, argv, timeout):
        self.calls.append(list(argv))
        if argv[1:] == ["agents", "--json"]:
            text = (self.agents_text if self.agents_text is not None
                    else json.dumps(self.agents))
            return subprocess.CompletedProcess(argv, self.agents_rc, text, "")
        if argv[0] == "tmux" and "list-panes" in argv:
            return subprocess.CompletedProcess(argv, self.panes_rc, self.panes, "")
        if argv[0] == "ps":
            return subprocess.CompletedProcess(argv, self.ps_rc, self.ps, "")
        raise AssertionError(f"unexpected command {argv}")

    def asked_claude(self) -> int:
        return sum(1 for argv in self.calls if argv[1:] == ["agents", "--json"])


def _bg(agent_id="bg1", state="working", **extra):
    item = {"cwd": "/home/user/app", "kind": "background", "startedAt": NOW_MS - 60_000,
            "id": agent_id, "state": state}
    item.update(extra)
    return item


def _ia(session_id=SID_A, status="idle", **extra):
    item = {"cwd": "/home/user/web", "kind": "interactive", "startedAt": NOW_MS - 120_000,
            "sessionId": session_id, "status": status, "pid": 4242}
    item.update(extra)
    return item


def _agents(*items):
    parsed = remote.parse_agents(json.dumps(list(items)))
    assert parsed is not None
    return parsed


def _epoch(pid=100):
    return sessions.TmuxEpoch(pid, 200, "$0")


def _listed(server, epoch, *window_ids):
    """Make ``window_ids`` the tmux listing the server last saw, as a
    successful _observe_windows would: the card's session list, the epoch,
    and the lifetime the attach-window map belongs to."""
    server._tmux_epoch = epoch
    server._remote_windows_epoch = epoch
    server._last_windows = sessions.WindowSnapshot(epoch, tuple(
        sessions.Window(w, i, f"w{i}", i == 0, 0) for i, w in enumerate(window_ids)))


def _server_client(*, knows_remote=True, **server_kwargs):
    left, right = socket.socketpair()
    left.setblocking(False)
    right.setblocking(False)
    server = Server(["true"], generate_key(), host="127.0.0.1", port=0, **server_kwargs)
    client = ClientState(left, ("test", 1), reader=None, nonce=b"n", authed=True,
                         mirror=Grid.blank())
    client.caps.mode = True
    client.caps.remote = knows_remote
    server._clients.append(client)
    server._tmux_target = lambda: ("scratch", "rm")  # type: ignore[method-assign]
    return server, client, right


def _close(server, client, peer):
    try:
        client.sock.close()
    finally:
        peer.close()
        server._agents.stop()
        server._sel.close()


def _decoded(messages):
    out = []
    for data in messages:
        if data[3] == MsgType.REMOTE:
            out.append(("row", decode_remote(data[HEADER_SIZE:])))
        elif data[3] == MsgType.REMOTE_NOTE:
            out.append(("note", decode_remote_note(data[HEADER_SIZE:])))
    return out


def _row(*, kind=REMOTE_KIND_BACKGROUND, state=REMOTE_STATE_WORKING, flags=REMOTE_LIVE,
         name="fix-tests", agent_id="bg1", session_id=None, cwd="", here=None,
         here_epoch=None):
    return remote.Row(kind, state, flags, 3, name, "app", agent_id=agent_id,
                      session_id=session_id, cwd=cwd, here_window=here,
                      here_epoch=here_epoch)


def _wait_harvest(poller, wanted=True, target=None, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = poller.poll(wanted, target)
        if got is not None:
            return got
        time.sleep(0.01)
    raise AssertionError("the worker never finished")


# -- parsing ----------------------------------------------------------------------


class TestParsing:
    def test_both_documented_field_sets(self):
        got = _agents(
            # A live background session: both field sets at once.
            _bg("a1b2c3", "blocked", pid=11, status="waiting",
                waitingFor="permission prompt", sessionId=SID_A, name="fix tests"),
            # A background session whose process exited: state, no status/pid.
            _bg("d4e5", "done"),
            # An interactive session: status, no state, no id.
            _ia(SID_B, "busy", name="web"),
        )
        live, exited, interactive = got
        assert live == remote.Agent(
            kind="background", cwd="/home/user/app", started_at_ms=NOW_MS - 60_000,
            agent_id="a1b2c3", state="blocked", pid=11, status="waiting",
            waiting_for="permission prompt", session_id=SID_A, name="fix tests",
            waiting_raw="permission prompt")
        assert (exited.state, exited.pid, exited.status) == ("done", None, None)
        assert (interactive.kind, interactive.state, interactive.status,
                interactive.agent_id) == ("interactive", None, "busy", None)

    @pytest.mark.parametrize("text", [
        "", "not json", "{", "{}", '{"agents": []}', "null", "42", '"[]"',
        "[" * 100_000,                       # deep enough to recurse out
    ])
    def test_anything_but_a_list_is_a_failed_query(self, text):
        assert remote.parse_agents(text) is None

    def test_junk_entries_and_values_are_ignored_not_fatal(self):
        got = remote.parse_agents(json.dumps([
            "a string", 7, None, [],
            {"kind": "daemon", "cwd": "/x"},           # unknown kind: no row
            {"cwd": "/x"},                              # no kind at all
            {"kind": "background", "id": "-rf", "state": "exploded",
             "pid": "12", "startedAt": True, "cwd": 5, "name": 9,
             "sessionId": "not-a-uuid", "status": "dancing",
             "waitingFor": "a sign", "extra": {"nested": [1, 2]}},
            {"kind": "interactive", "pid": -3, "startedAt": -5,
             "sessionId": SID_A.upper()},
        ]))
        assert got is not None and len(got) == 2
        junk, upper = got
        # Every field refused -- except the raw waitingFor, which only ever
        # becomes clamped display text (see agent_state).
        assert junk == remote.Agent(kind="background", waiting_raw="a sign")
        assert upper.pid is None and upper.started_at_ms is None
        assert upper.session_id == SID_A.upper()           # a UUID in capitals is still one

    def test_non_finite_numbers_are_absent(self):
        (agent,) = remote.parse_agents(
            '[{"kind": "background", "startedAt": NaN, "pid": Infinity}]')
        assert agent.started_at_ms is None and agent.pid is None

    @pytest.mark.parametrize("agent_id", [
        "a" * 65, "", "-x", "a b", "a;b", "a/b", "$(id)", "id\n", "été"])
    def test_an_id_that_could_be_anything_but_an_id_is_dropped(self, agent_id):
        (agent,) = _agents(_bg(agent_id))
        assert agent.agent_id is None


class TestObserve:
    def test_claude_not_on_path_is_status_2(self):
        runner = FakeRunner()
        obs = remote.observe(None, runner=runner, which=lambda: None)
        assert obs.status == REMOTE_STATUS_NO_CLAUDE
        assert runner.calls == []

    @pytest.mark.parametrize("runner", [
        FakeRunner(agents_rc=1),
        FakeRunner(agents_text="{oops"),
        FakeRunner(agents_text='{"not": "a list"}'),
    ])
    def test_a_failing_or_malformed_answer_is_status_3(self, runner):
        obs = remote.observe(None, runner=runner, which=lambda: CLAUDE)
        assert obs.status == REMOTE_STATUS_FAILED

    def test_a_timeout_is_status_3(self):
        obs = remote.observe(None, runner=lambda argv, timeout: None,
                             which=lambda: CLAUDE)
        assert obs.status == REMOTE_STATUS_FAILED

    def test_claude_is_asked_with_the_resolved_path_and_a_timeout(self):
        seen = []

        def runner(argv, timeout):
            seen.append((argv, timeout))
            return subprocess.CompletedProcess(argv, 0, "[]", "")

        obs = remote.observe(None, runner=runner, which=lambda: CLAUDE)
        assert obs.status == REMOTE_STATUS_NONE and obs.agents == ()
        assert seen == [([CLAUDE, "agents", "--json"], remote.QUERY_TIMEOUT)]

    def test_here_is_found_through_the_process_tree(self):
        runner = FakeRunner(
            [_ia(pid=500), _ia(SID_B, pid=900)],
            panes="77 300 @0\n77 301 @4\n",
            # 500 -> 400 -> 301 (a pane of @4); 900 -> 1 (elsewhere).
            ps="  1     0\n301     77\n400   301\n500   400\n900     1\n")
        obs = remote.observe(("scratch", "rm"), runner=runner, which=lambda: CLAUDE)
        assert obs.here_by_pid == {500: "@4"}
        assert obs.windows == frozenset({"@0", "@4"})
        assert obs.tmux_pid == 77
        tmux = next(argv for argv in runner.calls if argv[0] == "tmux")
        assert tmux[:7] == ["tmux", "-L", "scratch", "list-panes", "-s", "-t", "rm"]

    @pytest.mark.parametrize("runner", [
        FakeRunner([_ia(pid=500)], panes_rc=1),
        FakeRunner([_ia(pid=500)], panes="77 300 @0\n", ps_rc=1),
        FakeRunner([_ia(pid=500)], panes="77 300 @0\n78 301 @1\n", ps="500 300\n"),
    ])
    def test_a_failed_detection_costs_here_and_nothing_else(self, runner):
        obs = remote.observe(("scratch", "rm"), runner=runner, which=lambda: CLAUDE)
        assert obs.status == REMOTE_STATUS_NONE
        assert len(obs.agents) == 1
        assert obs.here_by_pid == {}

    def test_an_ancestry_cycle_terminates(self):
        found = remote.here_by_ancestry([10], {99: "@1"}, {10: 11, 11: 10})
        assert found == {}


# -- rows ----------------------------------------------------------------------------


class TestRows:
    @pytest.mark.parametrize("state,wire,live", [
        ("working", REMOTE_STATE_WORKING, True),
        ("blocked", REMOTE_STATE_NEEDS_YOU, True),
        ("done", REMOTE_STATE_DONE, True),
        ("failed", REMOTE_STATE_FAILED, False),
        ("stopped", REMOTE_STATE_STOPPED, True),     # `claude attach` restarts it
        ("mystery", REMOTE_STATE_UNKNOWN, True),
    ])
    def test_background_states_and_live(self, state, wire, live):
        (row,) = remote.build_rows(_agents(_bg(state=state)), now_ms=NOW_MS)
        assert row.kind == REMOTE_KIND_BACKGROUND
        assert row.state == wire
        assert row.flags == (REMOTE_LIVE if live else 0)

    def test_a_background_row_without_an_id_is_not_live(self):
        (row,) = remote.build_rows(_agents(_bg(agent_id="-bad")), now_ms=NOW_MS)
        assert row.flags == 0

    @pytest.mark.parametrize("status,wire", [
        ("busy", REMOTE_STATE_WORKING),
        ("waiting", REMOTE_STATE_NEEDS_YOU),
        ("idle", REMOTE_STATE_IDLE),
        (None, REMOTE_STATE_UNKNOWN),
    ])
    def test_interactive_statuses(self, status, wire):
        (row,) = remote.build_rows(_agents(_ia(status=status)), now_ms=NOW_MS)
        assert row.kind == REMOTE_KIND_INTERACTIVE
        assert row.state == wire
        assert row.flags == REMOTE_COPY

    def test_an_exited_background_session_falls_back_across_fields(self):
        # state without status, and status without state: each kind reads its
        # own field first and the other only when its own is missing.
        a, b = remote.build_rows(_agents(
            _bg(state=None, status="busy"),
            _ia(status=None, state="done", startedAt=0)), now_ms=NOW_MS)
        assert a.state == REMOTE_STATE_WORKING
        assert b.state == REMOTE_STATE_DONE

    def test_waiting_names_what_it_is_waiting_for(self):
        (row,) = remote.build_rows(_agents(
            _ia(status="waiting", waitingFor="input needed")), now_ms=NOW_MS)
        assert row.detail == "web - input needed"
        (row,) = remote.build_rows(_agents(
            _ia(status="waiting", waitingFor="something new")), now_ms=NOW_MS)
        assert row.detail == "web"

    def test_interactive_without_a_session_id_cannot_be_copied(self):
        item = _ia()
        del item["sessionId"]
        (row,) = remote.build_rows(_agents(item), now_ms=NOW_MS)
        assert row.flags == 0

    def test_here_by_process_tree_replaces_copy(self):
        # Inside dsterm's own tmux, a session is shown, never forked.
        epoch = _epoch()
        (row,) = remote.build_rows(_agents(_ia(pid=500)), now_ms=NOW_MS,
                                   here_by_pid={500: "@3"}, here_epoch=epoch)
        assert row.flags == REMOTE_HERE
        assert (row.here_window, row.here_epoch) == ("@3", epoch)

    def test_here_by_attach_map_keeps_live(self):
        (row,) = remote.build_rows(_agents(_bg("bg1")), now_ms=NOW_MS,
                                   here_by_id={"bg1": "@2"})
        assert row.flags == REMOTE_HERE | REMOTE_LIVE
        assert row.here_window == "@2"

    def test_nowhere_to_open_means_no_flag_claims_otherwise(self):
        rows = remote.build_rows(_agents(_bg(), _ia(pid=500)), now_ms=NOW_MS,
                                 here_by_pid={500: "@3"}, can_open=False)
        assert [r.flags for r in rows] == [0, 0]
        assert all(r.here_window is None for r in rows)

    def test_sorted_needs_you_then_working_then_most_recent(self):
        rows = remote.build_rows(_agents(
            _bg("old-idle", "done", startedAt=NOW_MS - 10_000_000),
            _ia(SID_A, "busy", startedAt=NOW_MS - 9_000_000, name="w-old"),
            _bg("new-done", "done", startedAt=NOW_MS - 1_000),
            _bg("blocked", "blocked", startedAt=NOW_MS - 50_000_000),
            _ia(SID_B, "busy", startedAt=NOW_MS - 1_000, name="w-new"),
            _ia(SID_B, "idle", name="no-start", startedAt=None),
        ), now_ms=NOW_MS)
        keys = [r.agent_id or r.name for r in rows]
        assert keys == ["blocked", "w-new", "w-old", "new-done", "old-idle", "no-start"]

    def test_at_most_eight_and_the_urgent_ones_survive(self):
        items = [_bg(f"done{i}", "done", startedAt=NOW_MS - i) for i in range(12)]
        items.append(_bg("late-blocker", "blocked", startedAt=0))
        rows = remote.build_rows(_agents(*items), now_ms=NOW_MS)
        assert len(rows) == 8
        assert rows[0].agent_id == "late-blocker"

    def test_names_and_details_are_clamped_and_replaced(self):
        (row,) = remote.build_rows(_agents(_ia(
            name="café \x1b[31m" + "x" * 40,
            cwd="/home/user/a-directory-name-that-is-far-too-long",
            status="waiting", waitingFor="permission prompt")), now_ms=NOW_MS)
        assert row.name == ("caf? ?[31m" + "x" * 40)[:24]
        assert len(row.name) == 24
        assert row.detail == "a-directory-name-that-is-far"
        assert all(0x20 <= ord(ch) <= 0x7E for ch in row.name + row.detail)

    def test_a_missing_name_is_untitled_and_age_is_minutes(self):
        item = _bg(startedAt=NOW_MS - 75 * 60_000 - 59_000)
        (row,) = remote.build_rows(_agents(item), now_ms=NOW_MS)
        assert row.name == "untitled"
        assert row.age_min == 75
        (row,) = remote.build_rows(_agents(_bg(startedAt=0)), now_ms=NOW_MS)
        assert row.age_min == 0xFFFF

    def test_a_root_cwd_and_no_cwd(self):
        a, b = remote.build_rows(_agents(_bg(cwd="/"), _bg("x2", cwd=None)),
                                 now_ms=NOW_MS)
        assert {a.detail, b.detail} == {"/", ""}


# -- wire -------------------------------------------------------------------------


class TestWire:
    def test_empty_set_status_2_byte_exact(self):
        assert encode_remote_empty(REMOTE_STATUS_NO_CLAUDE) == bytes.fromhex(
            "4453019f0002" "00" "02")
        assert decode_remote(b"\x00\x02") == (0, 2, 0, 0, 0, 0, "", "")

    def test_not_asked_yet_is_its_own_frame(self):
        assert encode_remote_empty(REMOTE_STATUS_NOT_ASKED) == bytes.fromhex(
            "4453019f00020000")
        assert remote.NOT_ASKED.messages == (bytes.fromhex("4453019f00020000"),)

    def test_full_row_byte_exact(self):
        blob = encode_remote_row(1, 2, REMOTE_KIND_BACKGROUND, REMOTE_STATE_NEEDS_YOU,
                                 REMOTE_LIVE, 75, "fix-tests", "app - permission prompt")
        assert blob == bytes.fromhex(
            "4453019f0029"      # magic, version, 0x9F, length 41
            "02" "01" "00" "02" "02" "004b"
            "09") + b"fix-tests" + bytes.fromhex("17") + b"app - permission prompt"
        assert decode_remote(blob[HEADER_SIZE:]) == (
            2, 1, 0, 2, 0x02, 75, "fix-tests", "app - permission prompt")

    def test_empty_name_and_detail_are_legal(self):
        blob = encode_remote_row(0, 1, 1, 0, 0, 0, "", "")
        assert blob == bytes.fromhex("4453019f0009" "01000100000000" "00" "00")
        assert decode_remote(blob[HEADER_SIZE:]) == (1, 0, 1, 0, 0, 0, "", "")

    def test_the_largest_row_clears_the_ceiling(self):
        blob = encode_remote_row(7, 8, 1, 6, 0x07, 0xFFFF, "n" * 24, "d" * 28)
        assert len(blob) == HEADER_SIZE + 7 + 1 + 24 + 1 + 28
        assert len(blob) < 536

    def test_age_saturates(self):
        blob = encode_remote_row(0, 1, 0, 0, 0, 10**9, "n", "")
        assert decode_remote(blob[HEADER_SIZE:])[5] == 0xFFFF

    def test_note_byte_exact(self):
        assert encode_remote_note("opening fix-tests") == bytes.fromhex(
            "445301a00011") + b"opening fix-tests"
        assert decode_remote_note(b"opening fix-tests") == "opening fix-tests"

    def test_pick_and_open_byte_exact(self):
        # The interop vector for the card's encoder (dsclient/tests): row 1 of
        # test_full_row_byte_exact's row, opened. The check is computed from
        # that row's name and detail bytes exactly as they were sent.
        check = remote_row_check("fix-tests", "app - permission prompt")
        assert check == 0x10BB
        assert encode_remote_pick(1, REMOTE_ACTION_OPEN, check) == bytes.fromhex(
            "445301190004" "01" "00" "10bb")
        assert encode_remote_pick(REMOTE_NEW_INDEX, REMOTE_ACTION_NEW) == bytes.fromhex(
            "445301190004" "ff" "02" "0000")
        assert encode_remote_open(True) == bytes.fromhex("44530118000101")
        assert encode_remote_open(False) == bytes.fromhex("44530118000100")
        assert decode_remote_pick(b"\x01\x00\x10\xbb") == (1, 0, 0x10BB)
        assert decode_remote_open(b"\x01") is True
        assert decode_remote_open(b"\x00") is False

    def test_the_row_check_is_fnv1a_folded_over_name_nul_detail(self):
        # Spelled out independently of the module, so a change to either the
        # hash or the join shows up here and not first on a console.
        def reference(data: bytes) -> int:
            h = 0x811C9DC5
            for b in data:
                h = ((h ^ b) * 0x01000193) % 2**32
            return (h >> 16) ^ (h & 0xFFFF)

        assert remote_row_check("", "") == reference(b"\x00")
        assert remote_row_check("web", "x") == reference(b"web\x00x")
        # The NUL keeps the boundary: these two rows draw the same 3 bytes.
        assert remote_row_check("ab", "c") != remote_row_check("a", "bc")
        assert remote_row_check(b"fix-tests", b"app") == remote_row_check("fix-tests", "app")
        row = _row(name="fix-tests")
        assert row.check == remote_row_check("fix-tests", row.detail)

    @pytest.mark.parametrize("args", [
        (0, 0, 0, 0, 0, 0, "n", ""),            # count 0
        (0, 9, 0, 0, 0, 0, "n", ""),            # count above 8
        (2, 2, 0, 0, 0, 0, "n", ""),            # index not below count
        (0, 1, 2, 0, 0, 0, "n", ""),            # kind
        (0, 1, 0, 7, 0, 0, "n", ""),            # state
        (0, 1, 0, 0, 0x08, 0, "n", ""),         # reserved flag
        (0, 1, 0, 0, 0, 0, "n" * 25, ""),       # name too long
        (0, 1, 0, 0, 0, 0, "n", "d" * 29),      # detail too long
        (0, 1, 0, 0, 0, 0, "café", ""),    # not ASCII
        (0, 1, 0, 0, 0, 0, "a\tb", ""),         # not printable
    ])
    def test_encode_refuses_what_the_card_would(self, args):
        index, count, *rest = args
        with pytest.raises(ProtocolError):
            encode_remote_row(index, count, *rest)

    @pytest.mark.parametrize("payload", [
        b"", b"\x00", b"\x00\x04", b"\x00\x00\x00",
        b"\x01\x00\x00\x00\x00\x00",                       # truncated before name_len
        b"\x01\x00\x00\x00\x00\x00\x00\x01",                # name shorter than declared
        b"\x01\x00\x00\x00\x00\x00\x00\x01n",               # no detail length
        b"\x01\x00\x00\x00\x00\x00\x00\x01n\x02d",          # detail shorter than declared
        b"\x01\x00\x00\x00\x00\x00\x00\x01n\x00x",          # trailing byte
        b"\x09\x00\x00\x00\x00\x00\x00\x00\x00",            # count above 8
        b"\x01\x01\x00\x00\x00\x00\x00\x00\x00",            # index not below count
        b"\x01\x00\x02\x00\x00\x00\x00\x00\x00",            # kind
        b"\x01\x00\x00\x07\x00\x00\x00\x00\x00",            # state
        b"\x01\x00\x00\x00\x10\x00\x00\x00\x00",            # reserved flag
        b"\x01\x00\x00\x00\x00\x00\x00\x19" + b"n" * 25 + b"\x00",
        b"\x01\x00\x00\x00\x00\x00\x00\x00\x1d" + b"d" * 29,
        b"\x01\x00\x00\x00\x00\x00\x00\x01\x7f\x00",       # DEL is not printable
    ])
    def test_decode_rejects_malformed(self, payload):
        with pytest.raises(ProtocolError):
            decode_remote(payload)

    @pytest.mark.parametrize("text", ["", "x" * 41, "a\nb", "é"])
    def test_note_bounds(self, text):
        with pytest.raises(ProtocolError):
            encode_remote_note(text)
        with pytest.raises(ProtocolError):
            decode_remote_note(text.encode("utf-8"))

    @pytest.mark.parametrize("payload", [b"", b"\x01\x00", b"\x02"])
    def test_open_is_one_byte_zero_or_one(self, payload):
        with pytest.raises(ProtocolError):
            decode_remote_open(payload)

    # b"\x01\x00" is the old two-byte pick: refused like any malformed frame,
    # because a pick without a check is the one that can open the wrong row.
    @pytest.mark.parametrize("payload", [b"", b"\x01", b"\x01\x00", b"\x01\x00\x00",
                                         b"\x01\x00\x00\x00\x00"])
    def test_pick_is_exactly_four_bytes(self, payload):
        with pytest.raises(ProtocolError):
            decode_remote_pick(payload)

    @pytest.mark.parametrize("index,action,check", [
        (8, 0, 0), (0xFF, 0, 0), (0, 2, 0), (0, 3, 0),
        (0xFF, 2, 1),                 # NEW names no row, so no check either
        (0, 0, 0x10000), (0, 1, -1),
    ])
    def test_pick_encoder_refuses_impossible_picks(self, index, action, check):
        with pytest.raises(ProtocolError):
            encode_remote_pick(index, action, check)

    def test_notes_are_clamped_before_they_are_framed(self):
        long = remote.note_text("opened a copy of " + "y" * 24)
        assert len(long) == 40
        encode_remote_note(long)
        assert remote.note_text("") == "?"
        assert remote.note_text("café") == "caf?"

    def test_a_set_with_rows_is_status_1_and_numbered(self):
        rows = remote.build_rows(_agents(_bg("a"), _bg("b")), now_ms=NOW_MS)
        built = remote.make_set(REMOTE_STATUS_FAILED, rows)
        assert built.status == REMOTE_STATUS_NONE
        decoded = [decode_remote(m[HEADER_SIZE:]) for m in built.messages]
        assert [(d[0], d[1]) for d in decoded] == [(2, 0), (2, 1)]


# -- gates, over a real socket ----------------------------------------------------


def _harness(**kwargs):
    runner = kwargs.pop("runner", None) or FakeRunner([_bg()])
    poller = remote.RemotePoller(interval=0.2, runner=runner, which=lambda: CLAUDE)
    return ServerHarness(remote_poller=poller, **kwargs), runner


def _remote_frames(client, seconds=1.0, until=None):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        client.poll(0.1)
        if until is not None and until(list(client.remote_frames)):
            break
    return list(client.remote_frames)


class TestGates:
    def test_nothing_remote_reaches_a_card_that_did_not_ask(self):
        h, runner = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key) as c:
                assert _remote_frames(c, 1.0) == []
                assert h.server._clients[0].caps.remote is False
        assert runner.calls == []

    def test_a_card_that_asked_is_told_not_asked_yet(self):
        h, runner = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                frames = _remote_frames(c, 3.0, until=bool)
                assert frames[0] == ("row", (0, REMOTE_STATUS_NOT_ASKED, 0, 0, 0, 0, "", ""))
                # Told, but not asked about: the screen is not open.
                time.sleep(0.5)
                assert runner.asked_claude() == 0

    def test_with_no_remote_a_card_that_asked_is_told_nothing(self):
        with ServerHarness(remote_enabled=False) as h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                assert _remote_frames(c, 1.0) == []
            assert h.server.remote_enabled is False
            # Nobody asked for STATE either, so nothing was ever asked.
            assert h.server._agents.asks == 0

    def test_opening_the_screen_asks_and_sends_a_set(self):
        h, runner = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                _remote_frames(c, 3.0, until=bool)
                c.send_remote_open(True)
                frames = _remote_frames(
                    c, 5.0, until=lambda f: any(k == "row" and v[0] == 1 for k, v in f))
                row = next(v for k, v in frames if k == "row" and v[0] == 1)
                # Not in tmux: listed, but nothing claims it can be opened.
                assert row[:5] == (1, 0, REMOTE_KIND_BACKGROUND, REMOTE_STATE_WORKING, 0)
                assert row[6:] == ("untitled", "app")
                asked = runner.asked_claude()
                assert asked >= 1
                c.send_remote_open(False)
                assert h.wait_for_event("remote screen closed")
                settled = runner.asked_claude()
                time.sleep(0.8)          # four intervals of the 0.2 s poller
                assert runner.asked_claude() <= settled + 1

    @pytest.mark.parametrize("frame", [encode_remote_open(True),
                                       encode_remote_pick(0, REMOTE_ACTION_OPEN)])
    def test_unnegotiated_remote_messages_drop_only_that_connection(self, frame):
        h, _ = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key) as c:
                c._send(frame)
                assert h.wait_for_event("remote was not negotiated")
            # The server is still serving the next card.
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c2:
                assert _remote_frames(c2, 3.0, until=bool)

    def test_a_malformed_remote_open_is_a_protocol_error(self):
        h, _ = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                _remote_frames(c, 3.0, until=bool)
                c._send(bytes.fromhex("44530118000102"))
                assert h.wait_for_event("bad remote open")


# -- server state machine -------------------------------------------------------


class TestServerState:
    def test_hello_negotiates_and_a_later_hello_withdraws(self):
        server, client, peer = _server_client(knows_remote=False)
        try:
            server._handle(client, Message(MsgType.HELLO, b"ds-term/1 remote"))
            assert client.caps.remote
            client.agents.target = remote.NOT_ASKED
            server._advertise_remote(client)
            client.agents.note = encode_remote_note("x")
            client.outbox.append(encode_remote_note("queued"))
            assert len(client.outbox) == 2       # the set, and the note
            server._handle(client, Message(MsgType.HELLO, b"ds-term/1"))
            assert not client.caps.remote
            assert client.agents.target is None and client.agents.note is None
            assert not client.outbox
        finally:
            _close(server, client, peer)

    def test_no_remote_server_never_negotiates(self):
        server, client, peer = _server_client(knows_remote=False, remote_enabled=False)
        try:
            server._handle(client, Message(MsgType.HELLO, b"ds-term/1 remote"))
            assert not client.caps.remote
        finally:
            _close(server, client, peer)

    def test_unnegotiated_open_drops_the_connection(self):
        server, client, peer = _server_client(knows_remote=False)
        events = []
        server._on_event = events.append
        try:
            server._handle(client, Message(MsgType.REMOTE_OPEN, b"\x01"))
            assert client not in server._clients
            assert any("remote was not negotiated" in e for e in events)
        finally:
            _close(server, client, peer)

    def test_a_set_is_promoted_only_when_its_last_row_has_left(self, monkeypatch):
        monkeypatch.setattr("dsterm.server.MESSAGES_PER_TICK", 1)
        server, client, peer = _server_client()
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            old = [_row(agent_id="old")]
            client.agents.rows = list(old)
            rows = remote.build_rows(_agents(_bg("a"), _bg("b")), now_ms=NOW_MS)
            client.agents.target = remote.make_set(REMOTE_STATUS_NONE, rows)
            server._advertise_remote(client)
            server._flush_outbox()
            assert client.agents.rows == old        # one of two rows sent
            server._advertise_remote(client)         # nothing doubles up mid-set
            server._flush_outbox()
            assert [r.agent_id for r in client.agents.rows] == ["a", "b"]
            assert [d[1][1] for d in _decoded(sent)] == [0, 1]
            server._advertise_remote(client)
            server._flush_outbox()
            assert len(sent) == 2                    # unchanged: not resent
        finally:
            _close(server, client, peer)

    def test_a_set_cut_off_by_a_resync_is_sent_again_from_row_0(self, monkeypatch):
        monkeypatch.setattr("dsterm.server.MESSAGES_PER_TICK", 1)
        server, client, peer = _server_client()
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            rows = remote.build_rows(_agents(_bg("a"), _bg("b")), now_ms=NOW_MS)
            client.agents.target = remote.make_set(REMOTE_STATUS_NONE, rows)
            server._advertise_remote(client)
            server._flush_outbox()
            server._clear_unsent_outbox(client)
            assert client.agents.batch is None and client.agents.rows == []
            server._advertise_remote(client)
            server._flush_outbox()
            server._flush_outbox()
            assert [d[1][1] for d in _decoded(sent)] == [0, 0, 1]
            assert [r.agent_id for r in client.agents.rows] == ["a", "b"]
        finally:
            _close(server, client, peer)

    def test_ages_alone_refresh_at_most_once_a_minute(self):
        server, client, peer = _server_client()
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            first = remote.build_rows(_agents(_bg()), now_ms=NOW_MS)
            later = remote.build_rows(_agents(_bg()), now_ms=NOW_MS + 5 * 60_000)
            client.agents.target = remote.make_set(REMOTE_STATUS_NONE, first)
            server._advertise_remote(client)
            server._flush_outbox()
            assert len(sent) == 1
            client.agents.target = remote.make_set(REMOTE_STATUS_NONE, later)
            server._advertise_remote(client, now=client.agents.sent_at + 10)
            assert not client.outbox
            server._advertise_remote(client, now=client.agents.sent_at + 61)
            server._flush_outbox()
            assert len(sent) == 2
        finally:
            _close(server, client, peer)

    def test_here_ignores_a_pane_list_from_another_tmux_lifetime(self):
        server, client, peer = _server_client()
        try:
            _listed(server, _epoch(100), "@0", "@3")
            obs = remote.Observation(REMOTE_STATUS_NONE, _agents(_ia(pid=500)),
                                     CLAUDE, frozenset({"@3"}), {500: "@3"}, 999, 0.0)
            (row,) = server._remote_set(obs).rows
            assert row.flags == REMOTE_COPY
            obs = remote.Observation(REMOTE_STATUS_NONE, _agents(_ia(pid=500)),
                                     CLAUDE, frozenset({"@3"}), {500: "@3"}, 100, 0.0)
            (row,) = server._remote_set(obs).rows
            assert row.flags == REMOTE_HERE and row.here_epoch == _epoch(100)
        finally:
            _close(server, client, peer)

    def test_a_closed_attach_window_stops_being_here(self):
        server, client, peer = _server_client()
        try:
            _listed(server, _epoch(100), "@0", "@7")
            server._remote_windows["@7"] = ("bg1", 5.0)
            listed = remote.Observation(REMOTE_STATUS_NONE, _agents(_bg("bg1")), CLAUDE,
                                        frozenset({"@0", "@7"}), {}, 100, 10.0)
            assert server._remote_set(listed).rows[0].flags == REMOTE_HERE | REMOTE_LIVE
            unknown = remote.Observation(REMOTE_STATUS_NONE, _agents(_bg("bg1")), CLAUDE,
                                         None, {}, None, 20.0)
            assert server._remote_set(unknown).rows[0].flags & REMOTE_HERE
            closed = remote.Observation(REMOTE_STATUS_NONE, _agents(_bg("bg1")), CLAUDE,
                                        frozenset({"@0"}), {}, 100, 20.0)
            assert server._remote_set(closed).rows[0].flags == REMOTE_LIVE
            assert server._remote_windows == {}
        finally:
            _close(server, client, peer)

    def test_a_note_waits_for_a_pending_switch_then_precedes_the_set(self):
        server, client, peer = _server_client()
        try:
            client.agents.target = remote.NOT_ASKED
            client.agents.note = encode_remote_note("opening x")
            client.sessions.switch_pending = True
            server._release_remote(client)
            assert not client.outbox and client.agents.note is not None
            client.sessions.switch_pending = False
            server._release_remote(client)
            assert client.outbox[0] == encode_remote_note("opening x")
            assert client.outbox[1].messages == list(remote.NOT_ASKED.messages)
        finally:
            _close(server, client, peer)


    def test_here_only_for_a_window_the_cards_session_list_holds(self):
        # The pane walk covers every window; the card's list holds eight. A
        # session in a ninth window is not HERE -- a pick could not select it
        # -- so an interactive one is offered as a copy instead.
        server, client, peer = _server_client()
        try:
            first_eight = [f"@{i}" for i in range(sessions.MAX_SESSIONS)]
            _listed(server, _epoch(100), *first_eight)
            server._remote_windows["@9"] = ("bg1", 5.0)
            obs = remote.Observation(REMOTE_STATUS_NONE,
                                     _agents(_ia(pid=500), _bg("bg1")), CLAUDE,
                                     frozenset(first_eight + ["@9", "@10"]),
                                     {500: "@10"}, 100, 10.0)
            rows = {r.kind: r for r in server._remote_set(obs).rows}
            assert rows[REMOTE_KIND_INTERACTIVE].flags == REMOTE_COPY
            assert rows[REMOTE_KIND_BACKGROUND].flags == REMOTE_LIVE
            # The same windows inside the list are HERE.
            obs = remote.Observation(REMOTE_STATUS_NONE, _agents(_ia(pid=500)), CLAUDE,
                                     frozenset(first_eight), {500: "@5"}, 100, 10.0)
            assert server._remote_set(obs).rows[0].flags == REMOTE_HERE
        finally:
            _close(server, client, peer)


# -- the poller ------------------------------------------------------------------


class TestPoller:
    def test_it_never_asks_while_nobody_is_looking(self):
        runner = FakeRunner([_bg()])
        poller = remote.RemotePoller(interval=0.0, runner=runner, which=lambda: CLAUDE)
        try:
            for i in range(20):
                assert poller.poll(False, None, now=float(i)) is None
            poller.refresh()
            assert poller.poll(False, None, now=100.0) is None
            assert poller.asks == 0 and runner.calls == []
            obs = _wait_harvest(poller)
            assert obs.status == REMOTE_STATUS_NONE and poller.asks >= 1
            assert poller.latest is obs
        finally:
            poller.stop()

    def test_one_ask_at_a_time_and_on_the_interval(self):
        runner = FakeRunner([_bg()])
        poller = remote.RemotePoller(interval=3.0, runner=runner, which=lambda: CLAUDE)
        try:
            poller.poll(True, None, now=0.0)
            # In flight, or finished and harvested here -- either way not due.
            harvested = poller.poll(True, None, now=0.1)
            assert poller.asks == 1
            deadline = time.monotonic() + 5
            while harvested is None:
                assert time.monotonic() < deadline
                time.sleep(0.01)
                harvested = poller.poll(True, None, now=1.0)
                time.sleep(0.01)
            assert poller.asks == 1                   # harvested, not due until 3.0
            poller.poll(True, None, now=3.0)
            assert poller.asks == 2
        finally:
            poller.stop()

    def test_a_crashing_runner_is_a_failed_answer(self):
        def runner(argv, timeout):
            raise RuntimeError("boom")

        poller = remote.RemotePoller(runner=runner, which=lambda: CLAUDE)
        try:
            assert _wait_harvest(poller).status == REMOTE_STATUS_FAILED
        finally:
            poller.stop()

    def test_an_answer_older_than_max_age_is_never_harvested(self):
        # The ask that was in flight when the last card left lands with
        # nobody polling; when somebody polls again, much later, it must not
        # come back as the present.
        runner = FakeRunner([_bg()])
        poller = remote.RemotePoller(interval=0.0, runner=runner, which=lambda: CLAUDE)
        try:
            poller.poll(True, None)
            deadline = time.monotonic() + 5
            while poller._result is None:
                assert time.monotonic() < deadline
                time.sleep(0.01)
            landed = poller._result
            later = landed.started_at + 31.0
            assert poller.poll(False, None, now=later, max_age=30.0) is None
            assert poller.latest is None and poller._result is None
            # A fresh one is handed over as before.
            deadline = time.monotonic() + 5
            fresh = None
            while fresh is None:
                assert time.monotonic() < deadline
                fresh = poller.poll(True, None, max_age=30.0)
                time.sleep(0.01)
            assert poller.latest is fresh
        finally:
            poller.stop()

    def test_a_reading_that_landed_after_the_last_card_left_is_not_published(self):
        server, client, peer = _server_client()
        client.caps.state = True
        try:
            stale = remote.Observation(
                REMOTE_STATUS_NONE, _agents(_ia(status="busy", pid=500)), CLAUDE,
                frozenset({"@0"}), {500: "@0"}, 100,
                started_at=time.monotonic() - 3600)
            server._agents._result = stale
            server._poll_agents(time.monotonic())
            assert server._agents.latest is None
            assert client.agents.target is None
            assert server._state_reading == (STATE_UNKNOWN, "")
        finally:
            _close(server, client, peer)

    def test_a_remote_card_does_not_inherit_a_stale_reading_either(self):
        # Not only a STATE card: a REMOTE card's first pick republishes
        # `latest` as its list, so an hour-old one must be gone by then.
        h, _ = _harness()
        with h:
            h.server._agents._latest = remote.Observation(
                REMOTE_STATUS_NONE, _agents(_bg()), CLAUDE,
                started_at=time.monotonic() - 3600)
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                assert _remote_frames(c, 3.0, until=bool)
                assert h.server._agents.latest is None

    def test_the_server_asks_only_while_a_card_has_the_screen_open(self):
        runner = FakeRunner([_bg()])
        poller = remote.RemotePoller(interval=0.0, runner=runner, which=lambda: CLAUDE)
        server, client, peer = _server_client(remote_poller=poller)
        try:
            for i in range(10):
                server._poll_agents(float(i))
            assert poller.asks == 0

            server._set_remote_open(client, True)
            deadline = time.monotonic() + 5
            while client.agents.target is None:
                assert time.monotonic() < deadline
                server._poll_agents(time.monotonic())
                time.sleep(0.01)
            assert [r.agent_id for r in client.agents.target.rows] == ["bg1"]
            # Within reach of tmux (the fake target), so the row is live.
            assert client.agents.target.rows[0].flags == REMOTE_LIVE

            # Gone: the card disconnected. Nothing is asked for nobody.
            server._drop(client, "test")
            time.sleep(0.05)
            server._poll_agents(time.monotonic())     # harvest anything in flight
            asks = poller.asks
            for i in range(10):
                server._poll_agents(time.monotonic() + i)
            assert poller.asks == asks
        finally:
            _close(server, client, peer)


# -- picks ------------------------------------------------------------------------


class _Tmux:
    """A fake tmux behind sessions.list_windows/select and remote.new_window."""

    def __init__(self, monkeypatch, windows=("@0",)):
        self.epoch = _epoch()
        self.windows = list(windows)
        self.selected: list[str] = []
        self.opened: list[tuple] = []
        monkeypatch.setattr(sessions, "list_windows", self._list)
        monkeypatch.setattr(sessions, "select", self._select)
        monkeypatch.setattr(remote, "new_window", self._new_window)
        monkeypatch.setattr(remote, "find_claude", lambda: CLAUDE)

    def _list(self, *_):
        return sessions.WindowSnapshot(self.epoch, tuple(
            sessions.Window(w, i, f"w{i}", i == 0, 0) for i, w in enumerate(self.windows)))

    def _select(self, socket_name, target, window_id):
        self.selected.append(window_id)
        return window_id in self.windows

    def _new_window(self, socket_name, session, name, cwd, argv):
        self.opened.append((socket_name, session, name, cwd, list(argv)))
        window_id = f"@{len(self.windows) + 10}"
        self.windows.append(window_id)
        return window_id


def _note(client) -> str:
    assert client.agents.note is not None
    return decode_remote_note(client.agents.note[HEADER_SIZE:])


class TestPicks:
    @pytest.fixture
    def env(self, monkeypatch, tmp_path):
        tmux = _Tmux(monkeypatch)
        server, client, peer = _server_client(child_command=["claude", "--model", "x"],
                                              cwd=str(tmp_path))
        events = []
        server._on_event = events.append
        yield server, client, tmux, tmp_path, events
        _close(server, client, peer)

    def _pick(self, server, client, index, action, check=None):
        """Pick as a card would: with the check of the row it was sent."""
        if check is None:
            check = (client.agents.rows[index].check
                     if 0 <= index < len(client.agents.rows) else 0)
        client.agents.last_pick_at = float("-inf")
        server._handle_remote_pick(client, index, action, check)
        return _note(client)

    def test_live_attaches_in_a_new_window_and_switches_to_it(self, env):
        server, client, tmux, tmp_path, _ = env
        client.agents.rows = [_row(name="fix-tests", agent_id="bg1", cwd=str(tmp_path))]
        # "starting": tmux made a window, which says nothing yet about whether
        # the attach found its session.
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "starting fix-tests"
        assert tmux.opened == [("scratch", "rm", "fix-tests", str(tmp_path),
                                [CLAUDE, "attach", "bg1"])]
        new = tmux.windows[-1]
        assert tmux.selected == [new]
        assert client.sessions.switch_pending
        assert server._remote_windows[new][0] == "bg1"

    def test_copy_forks_and_never_resumes_plainly(self, env):
        server, client, tmux, tmp_path, _ = env
        client.agents.rows = [_row(kind=REMOTE_KIND_INTERACTIVE, flags=REMOTE_COPY,
                                   agent_id=None, session_id=SID_A, name="web",
                                   cwd="/no/such/dir")]
        assert self._pick(server, client, 0, REMOTE_ACTION_COPY) == "starting a copy of web"
        ((_, _, _, cwd, argv),) = tmux.opened
        assert argv == [CLAUDE, "--resume", SID_A, "--fork-session"]
        assert cwd == str(tmp_path)          # a vanished directory: the server's own
        assert server._remote_windows == {}  # a fork is a new session, found by pid

    def test_here_selects_the_window_it_is_already_in(self, env):
        server, client, tmux, _, _ = env
        tmux.windows.append("@3")
        client.agents.rows = [_row(kind=REMOTE_KIND_INTERACTIVE, flags=REMOTE_HERE,
                                   agent_id=None, here="@3", here_epoch=tmux.epoch)]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "opening fix-tests"
        assert tmux.opened == [] and tmux.selected == ["@3"]
        assert client.sessions.switch_pending

    def test_here_from_another_tmux_lifetime_is_not_selected(self, env):
        server, client, tmux, _, _ = env
        tmux.windows.append("@3")
        client.agents.rows = [_row(kind=REMOTE_KIND_INTERACTIVE, flags=REMOTE_HERE,
                                   agent_id=None, here="@3", here_epoch=_epoch(55))]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "that window is gone"
        assert tmux.selected == [] and not client.sessions.switch_pending

    def test_new_session_runs_the_servers_own_command_in_its_cwd(self, env):
        server, client, tmux, tmp_path, _ = env
        assert self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW) == (
            "starting a new session")
        assert tmux.opened == [("scratch", "rm", "claude", str(tmp_path),
                                ["claude", "--model", "x"])]

    def test_new_session_without_a_child_command_is_bare_claude(self, env):
        server, client, tmux, _, _ = env
        server.child_command = None
        self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW)
        assert tmux.opened[0][4] == [CLAUDE]

    @pytest.mark.parametrize("index,action,rows,expected", [
        (1, REMOTE_ACTION_OPEN, 1, "that session is gone"),
        (0, REMOTE_ACTION_OPEN, 0, "that session is gone"),
        (0xFF, REMOTE_ACTION_OPEN, 1, "that session is gone"),
        (3, REMOTE_ACTION_NEW, 1, "that is not a row"),
        (0, 7, 1, "that is not something a row can do"),
    ])
    def test_bad_indexes_and_actions_are_refused(self, env, index, action, rows, expected):
        server, client, tmux, _, _ = env
        client.agents.rows = [_row()] * rows
        assert self._pick(server, client, index, action) == expected
        assert tmux.opened == [] and tmux.selected == []

    @pytest.mark.parametrize("flags,action,expected", [
        (REMOTE_COPY, REMOTE_ACTION_OPEN, "that row can't be opened live"),
        (0, REMOTE_ACTION_OPEN, "that row can't be opened live"),
        (REMOTE_LIVE, REMOTE_ACTION_COPY, "that row can't be opened as a copy"),
        (REMOTE_HERE, REMOTE_ACTION_COPY, "that row can't be opened as a copy"),
    ])
    def test_the_action_is_rechecked_against_the_rows_flags(self, env, flags, action,
                                                            expected):
        server, client, tmux, _, _ = env
        client.agents.rows = [_row(flags=flags, session_id=SID_A, here="@0")]
        assert self._pick(server, client, 0, action) == expected
        assert tmux.opened == [] and tmux.selected == []

    def test_a_row_missing_from_the_newest_answer_is_gone(self, env):
        server, client, tmux, _, _ = env
        server._agents._latest = remote.Observation(
            REMOTE_STATUS_NONE, _agents(_bg("someone-else")), CLAUDE)
        client.agents.rows = [_row(agent_id="bg1")]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "that session is gone"
        assert tmux.opened == []

    def test_an_id_that_slipped_past_parsing_is_still_refused(self, env):
        server, client, tmux, _, _ = env
        client.agents.rows = [_row(agent_id="--dangerous")]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "that row can't be opened live"
        client.agents.rows = [_row(kind=REMOTE_KIND_INTERACTIVE, flags=REMOTE_COPY,
                                   agent_id=None, session_id="$(rm -rf ~)")]
        assert self._pick(server, client, 0, REMOTE_ACTION_COPY) == (
            "that row can't be opened as a copy")
        assert tmux.opened == []

    def test_no_claude_no_window(self, env, monkeypatch):
        server, client, tmux, _, _ = env
        monkeypatch.setattr(remote, "find_claude", lambda: None)
        client.agents.rows = [_row()]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "claude is not installed here"
        assert tmux.opened == []

    def test_not_in_tmux_nothing_opens(self, env):
        server, client, tmux, _, _ = env
        server._tmux_target = lambda: None  # type: ignore[method-assign]
        client.agents.rows = [_row()]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == (
            "no tmux session to open it in")
        assert self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW) == (
            "no tmux session to open it in")
        assert tmux.opened == []

    def test_a_ninth_window_is_refused(self, env):
        server, client, tmux, _, _ = env
        tmux.windows[:] = [f"@{i}" for i in range(sessions.MAX_SESSIONS)]
        assert self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW) == (
            "8 windows open: close one first")
        assert tmux.opened == []

    def test_one_pick_a_second(self, env):
        server, client, tmux, _, events = env
        client.agents.rows = [_row()]
        server._handle_remote_pick(client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW)
        server._handle_remote_pick(client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW)
        assert len(tmux.opened) == 1
        assert any("ignored" in e for e in events)
        client.agents.last_pick_at -= 1.0
        server._handle_remote_pick(client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW)
        assert len(tmux.opened) == 2

    def test_any_pick_asks_again_and_refreshes_sessions(self, env):
        server, client, _, _, _ = env
        server._sessions_polled_at = 123.0
        client.agents.rows = []
        self._pick(server, client, 0, REMOTE_ACTION_OPEN)
        assert server._sessions_polled_at == 0.0
        assert server._agents._again

    def test_a_pick_arrives_through_handle(self, env):
        server, client, tmux, _, _ = env
        server._handle(client, Message(MsgType.REMOTE_PICK, bytes((0xFF, 2, 0, 0))))
        assert _note(client) == "starting a new session" and len(tmux.opened) == 1
        server._handle(client, Message(MsgType.REMOTE_PICK, b"\x00"))
        assert client not in server._clients

    def test_a_two_byte_pick_is_a_malformed_frame(self, env):
        # The card and the server ship together; a pick without a check is
        # refused the way any malformed payload is, never acted on as row N.
        server, client, tmux, _, events = env
        client.agents.rows = [_row()]
        server._handle(client, Message(MsgType.REMOTE_PICK, bytes((0, 0))))
        assert client not in server._clients
        assert any("bad remote pick" in e for e in events)
        assert tmux.opened == [] and tmux.selected == []

    def test_a_row_that_moved_under_the_thumb_is_not_opened(self, env):
        # The card drew "fix-tests" at row 0; the newest set, already promoted
        # here, re-sorted a session that needs you to the top. The index alone
        # would attach the wrong one.
        server, client, tmux, tmp_path, _ = env
        drawn = _row(name="fix-tests", agent_id="bg1")
        client.agents.rows = [_row(name="web", agent_id="bg2",
                                   state=REMOTE_STATE_NEEDS_YOU), drawn]
        client.agents.signature = ("whatever", "was sent")
        server._agents._again = False
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN, check=drawn.check) == (
            "the list changed; pick again")
        assert tmux.opened == [] and tmux.selected == []
        assert not client.sessions.switch_pending
        # The list goes again, and `claude agents` is asked again.
        assert client.agents.signature is None and server._agents._again
        # The same pick with the row's own check goes through.
        assert self._pick(server, client, 1, REMOTE_ACTION_OPEN) == "starting fix-tests"

    def test_a_new_session_with_a_check_is_not_a_pick(self, env):
        server, client, tmux, _, _ = env
        assert self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW,
                          check=0x10BB) == "that is not a row"
        assert tmux.opened == []

    def test_a_window_that_died_at_once_is_reported(self, env):
        server, client, tmux, tmp_path, _ = env
        client.agents.rows = [_row(name="fix-tests", agent_id="bg1", cwd=str(tmp_path))]
        assert self._pick(server, client, 0, REMOTE_ACTION_OPEN) == "starting fix-tests"
        new = tmux.windows[-1]
        client.agents.note = None
        # Checked too early: nothing to say yet, whatever tmux lists.
        tmux.windows.remove(new)
        server._observe_windows()
        assert client.agents.note is None
        what, opened_at = server._remote_starting[new][1:]
        server._remote_starting[new] = (client, what, opened_at - 10)
        server._observe_windows()
        assert _note(client) == "fix-tests exited at once"
        assert new not in server._remote_starting

    def test_a_window_still_there_is_not_reported(self, env):
        server, client, tmux, _, _ = env
        self._pick(server, client, REMOTE_NEW_INDEX, REMOTE_ACTION_NEW)
        new = tmux.windows[-1]
        client.agents.note = None
        c, what, opened_at = server._remote_starting[new]
        server._remote_starting[new] = (c, what, opened_at - 10)
        server._observe_windows()
        assert client.agents.note is None and not server._remote_starting

    def test_one_failed_tmux_query_does_not_forget_attach_windows(self, env, monkeypatch):
        server, client, tmux, tmp_path, _ = env
        client.agents.rows = [_row(name="fix-tests", agent_id="bg1", cwd=str(tmp_path))]
        self._pick(server, client, 0, REMOTE_ACTION_OPEN)
        new = tmux.windows[-1]
        assert server._remote_windows[new][0] == "bg1"
        listing = sessions.list_windows
        monkeypatch.setattr(sessions, "list_windows", lambda *_: None)
        assert server._observe_windows() is None          # tmux timed out, once
        assert server._remote_windows[new][0] == "bg1"
        obs = remote.Observation(REMOTE_STATUS_NONE, _agents(_bg("bg1")), CLAUDE,
                                 None, {}, None, time.monotonic())
        # Unknown tmux: no HERE badge meanwhile, but nothing forgotten.
        assert server._remote_set(obs).rows[0].flags == REMOTE_LIVE
        monkeypatch.setattr(sessions, "list_windows", listing)
        server._observe_windows()                         # and it answers again
        assert server._remote_set(obs).rows[0].flags == REMOTE_HERE | REMOTE_LIVE
        # Gone from a later complete listing: gone.
        tmux.windows.remove(new)
        server._observe_windows()
        assert new not in server._remote_windows

    def test_another_tmux_lifetime_forgets_attach_windows(self, env, monkeypatch):
        server, client, tmux, tmp_path, _ = env
        client.agents.rows = [_row(name="fix-tests", agent_id="bg1", cwd=str(tmp_path))]
        self._pick(server, client, 0, REMOTE_ACTION_OPEN)
        assert server._remote_windows
        listing = sessions.list_windows
        monkeypatch.setattr(sessions, "list_windows", lambda *_: None)
        server._observe_windows()
        monkeypatch.setattr(sessions, "list_windows", listing)
        # Same ids, new server: they name other windows now.
        tmux.epoch = _epoch(999)
        server._observe_windows()
        assert server._remote_windows == {}


class TestOpeningWindows:
    def test_new_window_argv_is_exact_and_shell_free(self):
        seen = []

        def runner(argv, timeout):
            seen.append(argv)
            return subprocess.CompletedProcess(argv, 0, "@12\n", "")

        got = remote.new_window("scratch", "rm", "fix #{pane_pid}; rm", "/tmp",
                                [CLAUDE, "attach", "bg1"], runner=runner)
        assert got == "@12"
        assert seen == [["tmux", "-L", "scratch", "new-window", "-d", "-P",
                         "-F", "#{window_id}", "-t", "rm:",
                         "-n", "fix --pane_pid-- rm", "-c", "/tmp",
                         "--", CLAUDE, "attach", "bg1"]]

    def test_what_tmux_would_reread_is_escaped_for_it(self):
        seen = []

        def runner(argv, timeout):
            seen.append(argv)
            return subprocess.CompletedProcess(argv, 0, "@12\n", "")

        # '#' in the start directory is a format to tmux; a trailing ';' on
        # any word ends tmux's command there.
        remote.new_window("scratch", "rm", "n", "/home/user/#S;", [CLAUDE, "--resume;"],
                          runner=runner)
        (argv,) = seen
        assert argv[argv.index("-c") + 1] == "/home/user/##S\\;"
        assert argv[argv.index("--") + 1:] == [CLAUDE, "--resume\\;"]

    def test_a_one_word_command_never_reaches_a_shell_as_text(self):
        # tmux hands ONE word to `$SHELL -c`; this keeps it a parameter.
        seen = []

        def runner(argv, timeout):
            seen.append(argv)
            return subprocess.CompletedProcess(argv, 0, "@12\n", "")

        remote.new_window("scratch", "rm", "n", "/tmp", ["/opt/My Tools/claude"],
                          runner=runner)
        assert seen[0][seen[0].index("--") + 1:] == [
            "/bin/sh", "-c", 'exec "$0"', "/opt/My Tools/claude"]

    @pytest.mark.parametrize("out", [None,
                                     subprocess.CompletedProcess([], 1, "", "no"),
                                     subprocess.CompletedProcess([], 0, "garbage", "")])
    def test_a_window_tmux_did_not_confirm_is_none(self, out):
        assert remote.new_window("s", "t", "n", "/", ["x"],
                                 runner=lambda argv, timeout: out) is None


class TestServeOption:
    def test_no_remote_reaches_the_server_and_the_child_command_is_unwrapped(
            self, monkeypatch, tmp_path):
        from dsterm import serve

        made = {}

        class FakeServer:
            voice_where = "off (a test's server)"

            def __init__(self, command, key, **kwargs):
                made.update(kwargs, command=command)

            def register(self, extension):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def warm_voice(self):
                pass

            def serve_forever(self):
                pass

        monkeypatch.setattr(serve, "Server", FakeServer)
        monkeypatch.setattr(serve, "load_key", lambda path: b"k" * 32)
        monkeypatch.setattr(serve, "load_extensions", lambda: [])
        # main() fits an existing session to the grid; on the real socket that
        # would be the shipped service's, which no test may touch.
        fitted = []
        monkeypatch.setattr(serve, "fit_session",
                            lambda name, cols, rows: fitted.append((name, cols, rows)))
        common = ["--host", "127.0.0.1", "--api-key-file", str(tmp_path / "none"),
                  "--env-file", str(tmp_path / "none"), "--tmux", "rmtest"]
        assert serve.main(common + ["--no-remote", "--", "claude", "-c"]) == 0
        assert made["remote_enabled"] is False
        assert made["child_command"] == ["claude", "-c"]
        assert made["command"][0] == "tmux"
        from dsterm.grid import COLS, ROWS
        assert fitted == [("rmtest", COLS, ROWS)]
        made.clear()
        assert serve.main(common) == 0
        assert made["remote_enabled"] is True
        assert made["child_command"] == ["claude"]


# -- STATE: what the session on screen is doing ----------------------------------

from dsterm.protocol import (  # noqa: E402 -- grouped with the section it serves
    FEATURE_STATE,
    STATE_IDLE,
    STATE_NEEDS_APPROVAL,
    STATE_NEEDS_INPUT,
    STATE_UNKNOWN,
    STATE_WORKING,
    decode_state,
    encode_state,
)


class TestStateWire:
    def test_needs_approval_byte_exact(self):
        assert encode_state(STATE_NEEDS_APPROVAL, "permission prompt") == bytes.fromhex(
            "445301a10013" "03" "11") + b"permission prompt"
        assert decode_state(b"\x03\x11permission prompt") == (3, "permission prompt")

    def test_idle_byte_exact(self):
        assert encode_state(STATE_IDLE, "") == bytes.fromhex("445301a10002" "01" "00")
        assert decode_state(b"\x01\x00") == (1, "")

    @pytest.mark.parametrize("state,detail", [(5, ""), (-1, ""), (1, "x" * 25),
                                              (1, "café"), (1, "a\nb")])
    def test_encode_refuses(self, state, detail):
        with pytest.raises(ProtocolError):
            encode_state(state, detail)

    @pytest.mark.parametrize("payload", [b"", b"\x01", b"\x05\x00", b"\x01\x01",
                                         b"\x01\x00x", b"\x01\x19" + b"x" * 25,
                                         b"\x01\x01\x07"])
    def test_decode_refuses(self, payload):
        with pytest.raises(ProtocolError):
            decode_state(payload)


class TestStateMapping:
    @pytest.mark.parametrize("item,expected", [
        (dict(status="busy"), (STATE_WORKING, "")),
        (dict(status="idle"), (STATE_IDLE, "")),
        (dict(status="waiting", waitingFor="permission prompt"),
         (STATE_NEEDS_APPROVAL, "permission prompt")),
        (dict(status="waiting", waitingFor="sandbox request"),
         (STATE_NEEDS_APPROVAL, "sandbox request")),
        (dict(status="waiting", waitingFor="input needed"), (STATE_NEEDS_INPUT, "input needed")),
        (dict(status="waiting", waitingFor="dialog open"), (STATE_NEEDS_INPUT, "dialog open")),
        (dict(status="waiting", waitingFor="worker request"),
         (STATE_NEEDS_INPUT, "worker request")),
        # "Anything else" is a question, shown as Claude Code reported it.
        (dict(status="waiting", waitingFor="a brand new reason, quite long"),
         (STATE_NEEDS_INPUT, "a brand new reason, quit")),
        (dict(status="waiting"), (STATE_NEEDS_INPUT, "")),
        (dict(status="dancing"), (STATE_UNKNOWN, "")),
        (dict(status=None, state="working"), (STATE_WORKING, "")),
        (dict(status=None, state="blocked"), (STATE_NEEDS_INPUT, "")),
        (dict(status=None, state="done"), (STATE_IDLE, "")),
        (dict(status=None), (STATE_UNKNOWN, "")),
    ])
    def test_the_contracts_table(self, item, expected):
        (agent,) = _agents(_ia(**item))
        assert remote.agent_state(agent) == expected

    def _observation(self):
        # Window @0's pane is 300, window @4's is 301. Session 500 lives under
        # @4 (500 -> 400 -> 301); session 600 lives directly in @0's pane.
        runner = FakeRunner(
            [_ia(SID_A, "waiting", pid=500, waitingFor="permission prompt"),
             _ia(SID_B, "busy", pid=600),
             _bg("bg9", "working", pid=None)],
            panes="77 300 @0\n77 301 @4\n",
            ps="300 77\n301 77\n400 301\n500 400\n600 300\n")
        return remote.observe(("scratch", "rm"), runner=runner, which=lambda: CLAUDE)

    def test_the_session_in_the_selected_window_speaks(self):
        obs = self._observation()
        assert remote.state_reading(obs.agents, obs.here_by_pid, "@4") == (
            STATE_NEEDS_APPROVAL, "permission prompt")
        assert remote.state_reading(obs.agents, obs.here_by_pid, "@0") == (STATE_WORKING, "")
        assert remote.state_reading(obs.agents, obs.here_by_pid, "@9") == (STATE_UNKNOWN, "")
        assert remote.state_reading(obs.agents, obs.here_by_pid, None) == (STATE_UNKNOWN, "")

    def test_an_attached_background_session_speaks_for_its_window(self):
        obs = self._observation()
        assert remote.state_reading(obs.agents, obs.here_by_pid, "@7",
                                    attached={"@7": "bg9"}) == (STATE_WORKING, "")

    def test_two_sessions_in_one_window_the_one_that_needs_you_wins(self):
        agents = _agents(_ia(SID_A, "idle", pid=1), _ia(SID_B, "waiting", pid=2,
                                                        waitingFor="input needed"))
        assert remote.state_reading(agents, {1: "@0", 2: "@0"}, "@0") == (
            STATE_NEEDS_INPUT, "input needed")

    @pytest.mark.parametrize("remote_open,state_clients,reading,typed_ago,expected", [
        (False, False, STATE_WORKING, 1.0, None),          # nobody owed an answer
        (True, False, STATE_UNKNOWN, None, remote.POLL_SECONDS),
        (False, True, STATE_IDLE, None, remote.STATE_SLOW_SECONDS),
        (False, True, STATE_UNKNOWN, 31.0, remote.STATE_SLOW_SECONDS),
        (False, True, STATE_IDLE, 29.0, remote.STATE_FAST_SECONDS),
        (False, True, STATE_WORKING, None, remote.STATE_FAST_SECONDS),
        (False, True, STATE_NEEDS_APPROVAL, None, remote.STATE_FAST_SECONDS),
        (False, True, STATE_NEEDS_INPUT, None, remote.STATE_FAST_SECONDS),
        (True, True, STATE_IDLE, None, remote.POLL_SECONDS),   # the faster of the two
        (True, True, STATE_WORKING, None, remote.STATE_FAST_SECONDS),
    ])
    def test_cadence(self, remote_open, state_clients, reading, typed_ago, expected):
        assert remote.poll_interval(remote_open=remote_open, state_clients=state_clients,
                                    reading=reading, typed_ago=typed_ago) == expected

    @pytest.mark.parametrize("remote_open,reading,typed_ago,expected", [
        (False, STATE_WORKING, 1.0, remote.STATE_SLOW_SECONDS),
        (False, STATE_NEEDS_INPUT, None, remote.STATE_SLOW_SECONDS),
        (True, STATE_IDLE, 1.0, remote.POLL_SECONDS),     # someone is looking
    ])
    def test_a_failing_cli_is_asked_slowly(self, remote_open, reading, typed_ago,
                                           expected):
        assert remote.poll_interval(remote_open=remote_open, state_clients=True,
                                    reading=reading, typed_ago=typed_ago,
                                    failing=True) == expected


class TestStateServer:
    def _armed(self, monkeypatch):
        """A server whose last reading is _observation(), with @4 selected."""
        tmux = _Tmux(monkeypatch, windows=("@4", "@0"))
        tmux.epoch = _epoch(77)
        server, client, peer = _server_client(knows_remote=False)
        client.caps.state = True
        server._agents._latest = TestStateMapping()._observation()
        server._observe_windows()           # @4 is index 0, so active
        return server, client, peer, tmux

    def test_the_selected_window_decides_and_a_switch_re_evaluates_at_once(self, monkeypatch):
        server, client, peer, tmux = self._armed(monkeypatch)
        try:
            assert server._state_reading == (STATE_NEEDS_APPROVAL, "permission prompt")
            server._agents._again = False
            assert server._switch_to_window(client, "@0") is None
            # No new ask has landed; the reading for @0 comes from the same one.
            assert server._state_reading == (STATE_WORKING, "")
            assert server._agents._again              # and a fresh ask is due now
        finally:
            _close(server, client, peer)

    def test_a_reading_from_another_tmux_lifetime_says_unknown(self, monkeypatch):
        server, client, peer, tmux = self._armed(monkeypatch)
        try:
            tmux.epoch = _epoch(78)
            server._observe_windows()
            assert server._state_reading == (STATE_UNKNOWN, "")
        finally:
            _close(server, client, peer)

    def test_sent_once_then_only_on_change(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            for _ in range(3):
                server._advertise_state(client)
            assert [decode_state(d[HEADER_SIZE:]) for d in sent] == [
                (STATE_NEEDS_APPROVAL, "permission prompt")]
            server._state_reading = (STATE_IDLE, "")
            server._advertise_state(client)
            server._advertise_state(client)
            assert len(sent) == 2 and decode_state(sent[1][HEADER_SIZE:]) == (1, "")
        finally:
            _close(server, client, peer)

    def test_never_inside_a_staged_list_or_a_switch(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            for part, attr, value in (("sessions", "switch_pending", True),
                                      ("sessions", "switch_barrier", object()),
                                      ("sessions", "batch", object()),
                                      ("agents", "batch", object())):
                holder = getattr(client, part)
                default = getattr(type(holder)(), attr)
                setattr(holder, attr, value)
                server._advertise_state(client)
                assert sent == []
                setattr(holder, attr, default)
            server._advertise_state(client)
            assert len(sent) == 1
        finally:
            _close(server, client, peer)

    def test_nothing_to_a_card_that_did_not_ask(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        sent = []
        server._send_to = lambda _c, data: sent.append(data)  # type: ignore[method-assign]
        try:
            client.caps.state = False
            server._advertise_state(client)
            assert sent == []
        finally:
            _close(server, client, peer)

    def test_a_failed_ask_keeps_the_reading_and_no_claude_is_unknown(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        try:
            server._take_observation(remote.Observation(REMOTE_STATUS_FAILED))
            server._agents._latest = remote.Observation(REMOTE_STATUS_FAILED)
            server._refresh_state()
            assert server._state_reading[0] == STATE_NEEDS_APPROVAL
            server._agents._latest = remote.Observation(REMOTE_STATUS_NO_CLAUDE)
            server._refresh_state()
            assert server._state_reading == (STATE_UNKNOWN, "")
        finally:
            _close(server, client, peer)

    def test_failing_for_long_enough_is_unknown_and_slow(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        try:
            client.last_key_at = time.monotonic()            # typing: fast cadence
            server._agents._latest = remote.Observation(REMOTE_STATUS_FAILED)
            server._take_observation(server._agents._latest)
            assert server._state_reading[0] == STATE_NEEDS_APPROVAL   # one failure
            assert server._poll_interval(time.monotonic()) == remote.STATE_FAST_SECONDS
            # Half a minute of nothing but failures: the old reading is a guess.
            server._agents_failing_since -= remote_server.AGENTS_FAILING_SECONDS + 1
            server._take_observation(server._agents._latest)
            assert server._state_reading == (STATE_UNKNOWN, "")
            assert server._poll_interval(time.monotonic()) == remote.STATE_SLOW_SECONDS
            # One answer ends the run.
            server._agents._latest = TestStateMapping()._observation()
            server._take_observation(server._agents._latest)
            assert server._agents_failing_since is None
            assert server._state_reading == (STATE_NEEDS_APPROVAL, "permission prompt")
        finally:
            _close(server, client, peer)

    def test_a_silent_card_is_not_owed_readings(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        try:
            now = time.monotonic()
            client.caps.remote = True
            client.agents.remote_open = True
            client.last_heard_at = now - 1
            assert server._poll_interval(now) == remote.STATE_FAST_SECONDS
            # Switched off on a half-open socket: ten pings missed.
            client.last_heard_at = now - remote_server.SILENT_CARD_SECONDS - 1
            assert server._poll_interval(now) is None
            assert not server._remote_wanted(now)
            assert client in server._clients                  # not dropped
        finally:
            _close(server, client, peer)

    def test_a_stale_reading_is_not_told_to_a_new_card(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        try:
            server._agents._latest = remote.Observation(
                REMOTE_STATUS_NONE, started_at=time.monotonic() - 3600)
            server._state_reading = (STATE_WORKING, "")
            server._state_for_new_card()
            assert server._state_reading == (STATE_UNKNOWN, "")
            assert server._agents.latest is None and server._agents._again
        finally:
            _close(server, client, peer)

    def test_typing_and_activity_choose_the_cadence(self, monkeypatch):
        server, client, peer, _ = self._armed(monkeypatch)
        try:
            now = time.monotonic()
            server._state_reading = (STATE_IDLE, "")
            assert server._poll_interval(now) == remote.STATE_SLOW_SECONDS
            client.last_key_at = now - 10
            assert server._poll_interval(now) == remote.STATE_FAST_SECONDS
            client.last_key_at = now - 40
            assert server._poll_interval(now) == remote.STATE_SLOW_SECONDS
            server._state_reading = (STATE_NEEDS_INPUT, "")
            assert server._poll_interval(now) == remote.STATE_FAST_SECONDS
            client.caps.state = False
            assert server._poll_interval(now) is None
        finally:
            _close(server, client, peer)

    def test_hello_negotiates_state_and_forgets_what_was_sent(self):
        server, client, peer = _server_client(knows_remote=False)
        try:
            client.agents.state_sent = (STATE_IDLE, "")
            server._handle(client, Message(MsgType.HELLO, b"ds-term/1 state"))
            assert client.caps.state and client.agents.state_sent is None
            server._handle(client, Message(MsgType.HELLO, b"ds-term/1"))
            assert not client.caps.state
        finally:
            _close(server, client, peer)

    def test_a_key_marks_the_card_as_typing(self):
        server, client, peer = _server_client(knows_remote=False)
        try:
            server._handle(client, Message(MsgType.KEY, b"x"))
            assert client.last_key_at is not None
        finally:
            _close(server, client, peer)


class TestStateGates:
    def test_anything_the_card_sends_counts_as_being_there(self):
        h, _ = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_STATE,)) as c:
                deadline = time.monotonic() + 3
                while not c.states and time.monotonic() < deadline:
                    c.poll(0.1)
                state = h.server._clients[0]
                state.last_heard_at = 0.0
                c.send_ping()
                deadline = time.monotonic() + 3
                while state.last_heard_at == 0.0 and time.monotonic() < deadline:
                    c.poll(0.05)
                assert state.last_heard_at > 0.0

    def test_a_card_that_asked_hears_the_reading_right_after_auth(self):
        h, runner = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_STATE,)) as c:
                deadline = time.monotonic() + 3
                while not c.states and time.monotonic() < deadline:
                    c.poll(0.1)
                # Not in tmux: there is no selected window, so no session on it.
                assert list(c.states)[:1] == [(STATE_UNKNOWN, "")]
                # A state card is owed readings, so the shared reading runs.
                deadline = time.monotonic() + 3
                while runner.asked_claude() == 0 and time.monotonic() < deadline:
                    c.poll(0.1)
                assert runner.asked_claude() >= 1
                c.poll(0.5)
                assert list(c.states) == [(STATE_UNKNOWN, "")]   # no change, no frame

    def test_nothing_reaches_a_card_that_did_not_ask(self):
        h, runner = _harness()
        with h:
            with Client("127.0.0.1", h.port, h.key, features=(FEATURE_REMOTE,)) as c:
                _remote_frames(c, 1.0)
                assert list(c.states) == []
            assert runner.asked_claude() == 0


def test_a_failed_ask_does_not_carry_one_windows_state_to_another(monkeypatch):
    server, client, peer, tmux = TestStateServer()._armed(monkeypatch)
    try:
        assert server._state_reading[0] == STATE_NEEDS_APPROVAL
        server._agents._latest = remote.Observation(REMOTE_STATUS_FAILED)
        server._refresh_state()
        assert server._state_reading[0] == STATE_NEEDS_APPROVAL   # same window: kept
        assert server._switch_to_window(client, "@0") is None
        assert server._state_reading == (STATE_UNKNOWN, "")
    finally:
        _close(server, client, peer)
