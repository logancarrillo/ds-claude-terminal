"""The PROJECT page's server half: what it gathers, and what it puts on the wire.

Two separable concerns, tested separately. The encoding has to survive a hostile
value in every field, because the page is drawn on a device that halts on a
malformed frame. The gathering has to be honest: read-only, factual, and silent
when it does not know -- a stale branch name on a status page is worse than an
admission that nobody is reporting.
"""

from __future__ import annotations

import os
import subprocess
import threading
import time

import pytest

from dsterm import project
from dsterm.protocol import (
    PROJECT_LABEL_MAX,
    PROJECT_ROWS_MAX,
    PROJECT_ROWS_MAX_V1,
    PROJECT_TAB_COUNT,
    PROJECT_TAB_DIFF,
    PROJECT_TAB_NOW,
    PROJECT_TAB_WATCH,
    PROJECT_VALUE_MAX,
    FrameReader,
    MsgType,
    ProtocolError,
    decode_project_row,
    encode_project_empty,
    encode_project_row,
)


# The real ceiling on a server-to-client message: the DS's IPv4 minimum MSS.
# Anything above this is not slow, it is broken.
MSS_CEILING = 536


def only(frame: bytes):
    """The one message in a frame, for a test that just encoded it."""
    msgs = list(FrameReader().feed(frame))
    assert len(msgs) == 1
    return msgs[0]


class TestEncoding:
    def test_round_trip(self):
        frame = encode_project_row(0, 2, PROJECT_TAB_NOW, 9, "branch", "main")
        msg = only(frame)
        assert msg.type is MsgType.PROJECT
        assert decode_project_row(msg.payload) == (
            2, 0, PROJECT_TAB_NOW, 9, "branch", "main")

    def test_empty_snapshot_is_its_own_shape(self):
        # Distinct from silence: "nothing to report" and "this server does not
        # report it" are different answers and the handheld shows them apart.
        msg = only(encode_project_empty())
        assert msg.payload == b"\x00"
        assert decode_project_row(msg.payload) == (0, 0, 0, 0, "", "")

    def test_a_full_snapshot_fits_the_link(self):
        # Every row at its maximum width, which is the worst case that can
        # legitimately be sent.
        label = "x" * PROJECT_LABEL_MAX
        value = "y" * PROJECT_VALUE_MAX
        for i in range(PROJECT_ROWS_MAX):
            frame = encode_project_row(i, PROJECT_ROWS_MAX,
                                       i % PROJECT_TAB_COUNT, 15, label, value)
            assert len(frame) <= MSS_CEILING

    @pytest.mark.parametrize("kwargs", [
        dict(index=2, count=2),                       # index at the count
        dict(index=-1, count=2),                      # negative index
        dict(count=0),                                # zero count via the row form
        dict(count=PROJECT_ROWS_MAX + 1),             # past the wire maximum
        dict(tab=PROJECT_TAB_COUNT),                  # a tab that does not exist
        dict(fg=16),                                  # not a palette index
        dict(fg=-1),
        dict(label=""),                               # a row with no label
    ])
    def test_refuses_a_bad_field(self, kwargs):
        args = dict(index=0, count=2, tab=PROJECT_TAB_NOW, fg=0,
                    label="branch", value="main")
        args.update(kwargs)
        with pytest.raises(ProtocolError):
            encode_project_row(**args)

    def test_non_ascii_is_refused_not_mangled(self):
        with pytest.raises(ProtocolError):
            encode_project_row(0, 1, PROJECT_TAB_NOW, 0, "branch", "café")

    def test_over_long_text_is_clipped_rather_than_refused(self):
        # A long branch name is not an attack and should not cost the page.
        frame = encode_project_row(0, 1, PROJECT_TAB_NOW, 0,
                                   "x" * 40, "y" * 200)
        _, _, _, _, label, value = decode_project_row(only(frame).payload)
        assert len(label) == PROJECT_LABEL_MAX
        assert len(value) == PROJECT_VALUE_MAX

    @pytest.mark.parametrize("payload", [
        b"",
        b"\x00\x00",                                  # empty plus junk
        b"\x02\x00\x00\x00\x06",                      # header cut short
        b"\x02\x02\x00\x00\x06\x04branchmain",        # index at the count
        b"\x02\x00\x63\x00\x06\x04branchmain",        # tab out of range
        b"\x02\x00\x00\x63\x06\x04branchmain",        # colour out of range
        b"\x02\x00\x00\x00\xc8\x04branchmain",        # label longer than payload
        b"\x02\x00\x00\x00\x06\x04branchmain!",       # declared lengths short
        b"\x01\x00\x00\x00\x03\x00a\x01c",            # unprintable label byte
    ])
    def test_decoder_refuses_malformed(self, payload):
        with pytest.raises(ProtocolError):
            decode_project_row(payload)


class TestGathering:
    @staticmethod
    def _repo(tmp_path):
        def git(*args, env=None):
            subprocess.run(["git", *args], cwd=tmp_path, check=True,
                           capture_output=True, env=env)
        git("init", "-q")
        git("config", "user.email", "t@example.invalid")
        git("config", "user.name", "T")
        (tmp_path / "a.txt").write_text("one\n")
        git("add", "a.txt")
        # Committed NOW, deliberately.
        #
        # This was briefly pinned to a fixed 2024 date to stop a flake, and that
        # was the wrong fix: it made the fixture stable while leaving the
        # product ticking. A two-year-old commit reads "2 years ago", a bucket
        # nothing moves out of on any timescale a test can reach -- so the exact
        # class of bug that caused the flake could never recur in the suite
        # while remaining live on the device. The fix belonged in collect(),
        # and a fresh commit is what keeps the tests honest about it.
        git("commit", "-qm", "first commit")
        return tmp_path

    def test_a_clean_repo_reports_itself(self, tmp_path):
        repo = self._repo(tmp_path)
        rows = project.collect(str(repo))
        by_label = {r.label: r.value for r in rows}
        assert by_label["repo"] == repo.name
        assert by_label["state"] == "clean"
        assert by_label["last commit"] == "first commit"
        assert all(r.tab in range(PROJECT_TAB_COUNT) for r in rows)

    def test_a_dirty_repo_counts_what_changed(self, tmp_path):
        repo = self._repo(tmp_path)
        (repo / "a.txt").write_text("one\ntwo\n")
        (repo / "b.txt").write_text("new\n")
        rows = project.collect(str(repo))
        by_label = {r.label: r.value for r in rows}
        assert "1 changed" in by_label["state"]
        assert "1 new" in by_label["state"]
        diff = [r for r in rows if r.tab == PROJECT_TAB_DIFF]
        assert diff and "1 files" in diff[0].value

    def test_a_directory_that_is_not_a_repo_says_nothing(self, tmp_path):
        # Silence, not a guess. The handheld then says the server does not
        # report it, which is true.
        assert project.collect(str(tmp_path)) == ()

    def test_every_value_fits_the_wire(self, tmp_path):
        # A branch name can be any length; the page cannot.
        repo = self._repo(tmp_path)
        subprocess.run(["git", "checkout", "-qb", "b" * 200], cwd=repo,
                       check=True, capture_output=True)
        rows = project.collect(str(repo))
        for row in rows:
            assert len(row.label) <= PROJECT_LABEL_MAX
            assert len(row.value) <= PROJECT_VALUE_MAX
            # And encoding must not then raise, which is the property that
            # actually keeps the page on screen.
            encode_project_row(0, 1, row.tab, row.fg, row.label, row.value)

    def test_a_snapshot_stays_within_the_row_budget(self, tmp_path):
        repo = self._repo(tmp_path)
        for i in range(60):
            (repo / f"f{i}.txt").write_text("x\n")
        subprocess.run(["git", "add", "-A"], cwd=repo, check=True,
                       capture_output=True)
        rows = project.collect(str(repo))
        assert len(rows) <= PROJECT_ROWS_MAX

    def test_refresh_is_on_its_own_slow_clock(self, tmp_path):
        # git is forked to answer, so it must not be asked every tick.
        repo = self._repo(tmp_path)
        state = project.ProjectState()
        base = time.monotonic()
        assert project.refresh(state, str(repo), now=base) is True
        first = state.rows
        (repo / "a.txt").write_text("changed\n")
        assert project.refresh(state, str(repo), now=base + 0.5) is False
        assert state.rows == first
        assert project.refresh(state, str(repo),
                               now=base + project.POLL_INTERVAL_S + 0.1) is True
        assert state.rows != first

    def test_unchanged_rows_do_not_report_a_change(self, tmp_path):
        # What keeps a quiet repository off the wire entirely.
        repo = self._repo(tmp_path)
        state = project.ProjectState()
        base = time.monotonic()
        assert project.refresh(state, str(repo), now=base) is True
        assert project.refresh(state, str(repo),
                               now=base + project.POLL_INTERVAL_S + 0.1) is False

    def test_a_quiet_repo_still_looks_the_same_a_second_later(self, tmp_path):
        # The property the older test assumed and never checked: that a
        # snapshot of an untouched repository does not move on its own.
        #
        # It did. `git log -1 --pretty=%cr` returns "3 seconds ago", then "6
        # seconds ago", so _advertise_project's signature changed on every poll
        # and the whole six-frame page went back out every three seconds,
        # forever, on a repository nobody had touched. Measured: 6 of 6 polls
        # re-sent it. refresh()'s docstring promises that returning "changed"
        # rather than "collected" is what keeps a quiet page off the wire, and
        # that promise was never once kept.
        #
        # The fixture commits NOW, so this exercises the live path.
        repo = self._repo(tmp_path)
        first = project.collect(str(repo))
        time.sleep(1.2)
        second = project.collect(str(repo))
        moved = [(a.label, a.value, b.value)
                 for a, b in zip(first, second) if a != b]
        assert not moved, f"a row changed with no repository change: {moved}"



    def test_encode_produces_one_frame_per_row(self, tmp_path):
        repo = self._repo(tmp_path)
        rows = project.collect(str(repo))
        frames = project.encode(rows)
        assert len(frames) == len(rows)
        assert all(len(f) <= MSS_CEILING for f in frames)
        # And the whole snapshot decodes back to the rows that went in.
        seen = [decode_project_row(only(f).payload) for f in frames]
        assert [(t, l, v, g) for (_, _, t, g, l, v) in seen] == \
               [(r.tab, r.label, r.value, r.fg) for r in rows]

    def test_no_rows_encodes_as_the_empty_snapshot(self):
        frames = project.encode(())
        assert len(frames) == 1
        assert decode_project_row(only(frames[0]).payload)[0] == 0


class TestTheCommitAgeDoesNotTick:
    """A time display has to move; it does not have to move every three seconds.

    The honest answer is not "nothing ever changes" -- an age that never changed
    would be a lie within the minute. It is that the value changes when the
    answer meaningfully changes and holds still in between, so a quiet
    repository stays off the wire for as long as it is actually quiet.

    The tilde is the same admission the "quiet for" row already makes: the
    number is coarse and says so, rather than implying a precision it does not
    have.
    """

    @staticmethod
    def _at(delta: int) -> str:
        return project._ago(1_000_000, 1_000_000 + delta)

    def test_the_first_minute_is_just_now(self):
        # The window somebody is most likely to be watching, and the one where
        # per-second ticking cost the most: six frames every three seconds.
        assert self._at(0) == "just now"
        assert self._at(59) == "just now"

    def test_minutes_then_hours_then_days(self):
        assert self._at(60) == "~1m ago"
        assert self._at(3599) == "~59m ago"
        assert self._at(3600) == "~1h ago"
        assert self._at(86_399) == "~23h ago"
        assert self._at(86_400) == "~1d ago"

    def test_it_holds_still_between_boundaries(self):
        # The property that keeps the page off the wire.
        assert self._at(61) == self._at(119)
        assert self._at(3601) == self._at(7199)

    def test_it_does_change_at_a_boundary(self):
        # Stated so nobody later "fixes" the ticking by freezing it outright:
        # an age that never changed would be wrong within the minute.
        assert self._at(59) != self._at(60)
        assert self._at(3599) != self._at(3600)

    def test_a_clock_skew_into_the_future_is_not_negative(self):
        assert project._ago(1_000_000, 999_000) == "just now"


class TestThePageNeverStallsTheFrameLoop:
    """The page is gathered by forking git, and the frame loop cannot wait for it.

    Measured on the deployment this server actually runs on, whose working
    directory is a Windows path reached over WSL's drvfs: one snapshot costs
    1.59 s of wall time -- 0.70 s for ``status --porcelain`` and 0.56 s for
    ``diff --numstat`` alone -- against a repository of only 269 tracked files.
    ``POLL_INTERVAL_S`` is 3.0, so gathering inline spends over half of every
    three seconds sending the handheld nothing, and the terminal visibly
    stutters. Eight queries each hitting ``_GIT_TIMEOUT`` would be a 24 s freeze.

    None of that reproduces on a developer's ext4 checkout, where the same
    snapshot is an order of magnitude cheaper, which is exactly why the property
    belongs in a test rather than in someone's judgement.

    ``collect`` is patched throughout: the cost being guarded against is the
    forking, so a test that forks git to prove it would be measuring the thing
    it is supposed to make irrelevant.
    """

    # Far longer than a 15 fps tick, far shorter than _GIT_TIMEOUT.
    SLOW = 0.75

    def _blocking_collect(self, release, calls, rows=()):
        """A collect() that hangs until the test lets it finish."""
        def fake(cwd, quiet_s=None, ci=False):
            calls.append(cwd)
            release.wait(timeout=5.0)
            return rows
        return fake

    def test_polling_returns_at_once_while_a_collection_is_running(
            self, monkeypatch):
        release = threading.Event()
        calls: list[str] = []
        monkeypatch.setattr(project, "collect",
                            self._blocking_collect(release, calls))
        collector = project.BackgroundCollector("/anywhere")
        try:
            start = time.monotonic()
            for _ in range(5):
                collector.poll()
            elapsed = time.monotonic() - start
            # The whole point: the caller is the frame loop.
            assert elapsed < 0.1, f"polling blocked for {elapsed:.3f}s"
            assert calls, "no collection was ever started"
        finally:
            release.set()
            collector.stop()

    def test_one_collection_runs_at_a_time(self, monkeypatch):
        # Polling faster than git can answer must not pile up workers, or a slow
        # repository becomes an unbounded fork bomb on a 15 fps clock.
        release = threading.Event()
        calls: list[str] = []
        monkeypatch.setattr(project, "collect",
                            self._blocking_collect(release, calls))
        collector = project.BackgroundCollector("/anywhere")
        try:
            for _ in range(50):
                collector.poll()
            assert len(calls) == 1
        finally:
            release.set()
            collector.stop()

    def test_the_rows_arrive_once_the_worker_finishes(self, monkeypatch):
        rows = (project.Row(PROJECT_TAB_NOW, "branch", "main"),)
        release = threading.Event()
        calls: list[str] = []
        monkeypatch.setattr(project, "collect",
                            self._blocking_collect(release, calls, rows))
        collector = project.BackgroundCollector("/anywhere")
        try:
            assert collector.poll() is False
            assert collector.rows == ()
            release.set()
            changed = self._poll_until_change(collector)
            assert changed is True
            assert collector.rows == rows
        finally:
            release.set()
            collector.stop()

    def test_a_finished_snapshot_is_announced_exactly_once(self, monkeypatch):
        # The same rule the synchronous refresh follows: "changed" is what keeps
        # a quiet repository off the wire, so an unchanged re-collection is
        # silence, not a repeat.
        rows = (project.Row(PROJECT_TAB_NOW, "branch", "main"),)
        monkeypatch.setattr(project, "collect", lambda cwd, quiet_s=None, ci=False: rows)
        collector = project.BackgroundCollector("/anywhere", interval=0.0)
        try:
            assert self._poll_until_change(collector) is True
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                assert collector.poll() is False
                time.sleep(0.01)
        finally:
            collector.stop()

    def test_a_collection_that_raises_leaves_the_last_snapshot_alone(
            self, monkeypatch):
        # Failure is silence, and silence must not also mean a dead poller: the
        # page keeps its last honest answer and the next tick still works.
        rows = (project.Row(PROJECT_TAB_NOW, "branch", "main"),)
        monkeypatch.setattr(project, "collect", lambda cwd, quiet_s=None, ci=False: rows)
        collector = project.BackgroundCollector("/anywhere", interval=0.0)
        try:
            assert self._poll_until_change(collector) is True

            def boom(cwd, quiet_s=None):
                raise OSError("git fell over")

            monkeypatch.setattr(project, "collect", boom)
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline:
                collector.poll()
                time.sleep(0.01)
            assert collector.rows == rows
        finally:
            collector.stop()

    def test_stop_is_idempotent_and_polling_after_it_is_inert(self, monkeypatch):
        monkeypatch.setattr(project, "collect", lambda cwd, quiet_s=None, ci=False: ())
        collector = project.BackgroundCollector("/anywhere", interval=0.0)
        collector.poll()
        collector.stop()
        collector.stop()
        assert collector.poll() is False

    @staticmethod
    def _poll_until_change(collector, timeout=5.0):
        """Poll on a tick-like cadence until the worker publishes."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if collector.poll():
                return True
            time.sleep(0.01)
        return False


class TestTheQuietRowIsActuallyWired:
    """The "quiet for" row exists, is rendered, and was never once sent.

    ``collect`` grew a ``quiet_s`` parameter and a row to render it; the tick
    loop then called the collector without it, so the row could not appear on
    any page, ever. Both halves were tested -- ``collect`` directly, the encoder
    directly -- and neither test ran the assembly, so a green suite held an
    unreachable feature. These tests run the seam instead: what the server
    computes, and that the server hands it over.
    """

    @staticmethod
    def _server():
        from dsterm.server import Server
        return Server(["true"], b"k" * 32, host="127.0.0.1", port=0)

    @staticmethod
    def _with_active_window(server, idle_for: float, now: float):
        from dsterm import sessions as dssessions
        from dsterm.connection import OutputRevision
        epoch = ("socket", "0")
        window = dssessions.Window(window_id="@1", index=0, name="main",
                                   active=True, activity=0)
        server._last_windows = dssessions.WindowSnapshot(epoch=epoch,
                                                         windows=(window,))
        server._window_output[(epoch, "@1")] = OutputRevision(
            activity_marker=0, revision=1,
            last_output_monotonic=now - idle_for)
        return epoch

    def test_it_is_the_same_number_sessions_already_reports(self):
        # Not a second measurement that can disagree with idle_s -- the same
        # record, through the same helper.
        from dsterm import sessions as dssessions
        server = self._server()
        now = time.monotonic()
        self._with_active_window(server, idle_for=40.0, now=now)
        assert server._active_idle_seconds(now) == \
            dssessions.idle_seconds(now - 40.0, now) == 40

    def test_no_active_window_means_no_claim(self):
        # Silence rather than a zero, which would read as "busy right now".
        server = self._server()
        assert server._active_idle_seconds(time.monotonic()) is None

    def test_the_tick_hands_it_to_the_collector(self, monkeypatch):
        # The assertion the original wiring needed and did not have.
        server = self._server()
        now = time.monotonic()
        self._with_active_window(server, idle_for=40.0, now=now)

        seen: list[int | None] = []

        class Recorder:
            rows = ()

            def poll(self, quiet_s=None, now=None):
                seen.append(quiet_s)
                return False

        server._project = Recorder()
        server._poll_project(now)
        assert seen == [40], "the tick did not pass the active window's idle"


class TestTheQuietRowReadsHonestly:
    """A number sampled one collection ago must not look precise.

    ``quiet_s`` is sampled when a collection starts, so by the time the row is
    drawn it can be a snapshot old on top of the poll interval it was already
    coarse by. Rendering a bare "43s" invites precision the value does not have.
    """

    def test_it_is_rounded_and_marked_approximate(self, monkeypatch):
        monkeypatch.setattr(project, "_git", lambda cwd, *a: "/tmp/repo")
        rows = project.collect("/tmp/repo", quiet_s=43)
        quiet = [r for r in rows if r.label == "quiet for"]
        assert quiet and quiet[0].value == "~45s"

    def test_a_few_seconds_does_not_pretend_to_be_zero(self, monkeypatch):
        monkeypatch.setattr(project, "_git", lambda cwd, *a: "/tmp/repo")
        rows = project.collect("/tmp/repo", quiet_s=2)
        quiet = [r for r in rows if r.label == "quiet for"]
        assert quiet and quiet[0].value == "<5s"

    def test_absent_when_nothing_measured_it(self, monkeypatch):
        monkeypatch.setattr(project, "_git", lambda cwd, *a: "/tmp/repo")
        rows = project.collect("/tmp/repo")
        assert not [r for r in rows if r.label == "quiet for"]


class TestDeclarativeWatchAndSparklines:
    def test_sparkline_and_progress_bar_round_trip_on_wire(self):
        sp = project.sparkline([0, 1, 2, 3, 4, 5, 6, 7], width=8)
        assert sp == "▁▂▃▄▅▆▇█"
        pb = project.progress_bar(50.0, width=8)
        assert pb == "████░░░░"
        frame = encode_project_row(0, 1, PROJECT_TAB_WATCH, 3,
                                   "latency", f"{sp} 12ms")
        assert decode_project_row(only(frame).payload) == (
            1, 0, PROJECT_TAB_WATCH, 3, "latency", f"{sp} 12ms"
        )

    def test_declarative_watch_json_and_timer_populate_watch_tab(self, tmp_path):
        import json

        repo = TestGathering._repo(tmp_path)
        dsterm_dir = repo / ".dsterm"
        dsterm_dir.mkdir()
        (dsterm_dir / "watch.json").write_text(json.dumps([
            {"label": "gateway", "value": "11ms", "spark": [9, 10, 12, 11], "color": "green"},
            {"label": "deploy", "value": "staging", "progress": 75, "color": "orange"},
        ]), encoding="utf-8")
        (dsterm_dir / "timer.json").write_text(json.dumps({
            "label": "pomodoro",
            "duration_s": 1500,
            "end_ts": time.time() + 600,
        }), encoding="utf-8")

        rows = project.collect(str(repo))
        watch_rows = {r.label: r for r in rows if r.tab == PROJECT_TAB_WATCH}
        assert "gateway" in watch_rows
        assert "11ms" in watch_rows["gateway"].value
        assert "deploy" in watch_rows
        assert "pomodoro" in watch_rows
        assert "left" in watch_rows["pomodoro"].value


    def test_one_bad_spark_or_progress_costs_its_row_not_the_file(self, tmp_path):
        import json

        repo = TestGathering._repo(tmp_path)
        (repo / ".dsterm").mkdir()
        (repo / ".dsterm" / "watch.json").write_text(json.dumps([
            {"label": "good", "value": "1", "spark": [1, 2, 3]},
            {"label": "words", "value": "2", "spark": ["a", "b"]},
            {"label": "nan", "value": "3", "progress": "NaN"},
            {"label": "inf", "value": "4", "progress": 1e400},
            {"label": "also good", "value": "5", "progress": 40},
        ]), encoding="utf-8")
        labels = [r.label for r in project.collect(str(repo))
                  if r.tab == PROJECT_TAB_WATCH]
        assert labels == ["good", "also good"]

    @pytest.mark.parametrize("body", [
        "[]", "null", '"x"', "3", "true",
        '{"end_ts": 1e400}', '{"end_ts": "soon"}', '{"end_ts": NaN}',
        '{"end_ts": [1]}', "{not json",
    ])
    def test_a_bad_timer_file_costs_its_row_not_the_page(self, tmp_path, body):
        # Valid JSON of the wrong shape once raised AttributeError out of
        # collect(), and BackgroundCollector kept the last snapshot forever.
        import json

        repo = TestGathering._repo(tmp_path)
        (repo / ".dsterm").mkdir()
        (repo / ".dsterm" / "timer.json").write_text(body, encoding="utf-8")
        (repo / ".dsterm" / "watch.json").write_text(json.dumps([
            {"label": "still here", "value": "1"},
        ]), encoding="utf-8")
        rows = project.collect(str(repo))
        labels = [r.label for r in rows if r.tab == PROJECT_TAB_WATCH]
        assert labels == ["still here"]
        assert any(r.tab != PROJECT_TAB_WATCH for r in rows)

    @pytest.mark.parametrize("body", ["null", '"x"', "3", "{}"])
    def test_a_watch_file_of_the_wrong_shape_is_no_rows(self, tmp_path, body):
        repo = TestGathering._repo(tmp_path)
        (repo / ".dsterm").mkdir()
        (repo / ".dsterm" / "watch.json").write_text(body, encoding="utf-8")
        rows = project.collect(str(repo))
        assert [r for r in rows if r.tab == PROJECT_TAB_WATCH] == []


class TestCiIsOptIn:
    def test_without_project_ci_gh_is_never_run(self, tmp_path, monkeypatch):
        repo = TestGathering._repo(tmp_path)
        subprocess.run(["git", "remote", "add", "origin",
                        "https://github.com/example/example.git"],
                       cwd=repo, check=True)
        project._CI_CACHE.clear()
        ran = []
        real_run = subprocess.run

        def spy(argv, *a, **k):
            if argv and str(argv[0]).endswith("gh"):
                ran.append(argv)
                return subprocess.CompletedProcess(argv, 0, "[]", "")
            return real_run(argv, *a, **k)

        monkeypatch.setattr(project.shutil, "which", lambda name: "/usr/bin/gh")
        monkeypatch.setattr(project.subprocess, "run", spy)
        project.collect(str(repo))
        assert ran == []
        project.collect(str(repo), ci=True)
        assert ran and ran[0][1:3] == ["run", "list"]
        project._CI_CACHE.clear()

    @pytest.mark.parametrize("answer", [
        "{}", "null", '"x"', '["a", 1, null]', '{"name": "ci"}',
        '[{"name": "build", "status": "completed", "conclusion": "success"}, 7]',
    ])
    def test_a_strange_gh_answer_never_freezes_the_page(
            self, tmp_path, monkeypatch, answer):
        repo = TestGathering._repo(tmp_path)
        subprocess.run(["git", "remote", "add", "origin",
                        "https://github.com/example/example.git"],
                       cwd=repo, check=True)
        project._CI_CACHE.clear()
        real_run = subprocess.run

        def fake(argv, *a, **k):
            if argv and str(argv[0]).endswith("gh"):
                return subprocess.CompletedProcess(argv, 0, answer, "")
            return real_run(argv, *a, **k)

        monkeypatch.setattr(project.shutil, "which", lambda name: "/usr/bin/gh")
        monkeypatch.setattr(project.subprocess, "run", fake)
        try:
            rows = project.collect(str(repo), ci=True)
        finally:
            project._CI_CACHE.clear()
        labels = [r.label for r in rows if r.tab == PROJECT_TAB_WATCH]
        assert labels in ([], ["build"])


def _rows_of_every_tab(n_each: int) -> tuple:
    """n_each rows on each of the six tabs, with glyphs in them."""
    rows = []
    for tab in range(PROJECT_TAB_COUNT):
        for i in range(n_each):
            rows.append(project.Row(tab, f"t{tab}r{i}", f"{project.sparkline([1, 5, 9])} ✓ {i}"))
    return tuple(rows)


class TestOldCardsGetWhatTheyAccept:
    """A 1.0.x card (no "project64") refuses a snapshot over 48 rows and
    draws the glyph bytes as '?'. It is sent at most 48 rows, NOW, DIFF and
    DO first, in printable ASCII."""

    def _decoded(self, frames):
        return [decode_project_row(only(f).payload) for f in frames]

    def test_a_project64_card_gets_every_row_and_its_glyphs(self):
        rows = _rows_of_every_tab(11)[:PROJECT_ROWS_MAX]
        got = self._decoded(project.encode(rows, project64=True))
        assert len(got) == PROJECT_ROWS_MAX
        assert any("✓" in value for *_, value in got)

    def test_an_old_card_gets_48_rows_the_old_tabs_first(self):
        rows = _rows_of_every_tab(11)[:PROJECT_ROWS_MAX]   # 64: LOG has 9
        frames = project.encode(rows, project64=False)
        got = self._decoded(frames)
        assert len(got) == PROJECT_ROWS_MAX_V1
        assert all(count == PROJECT_ROWS_MAX_V1 for count, *_ in got)
        tabs = [tab for _, _, tab, *_ in got]
        # NOW, DIFF, DO whole (33), WATCH whole (11), then HEALTH's first 4.
        assert tabs.count(0) == tabs.count(1) == tabs.count(2) == 11
        assert tabs.count(3) == 11 and tabs.count(4) == 4 and 5 not in tabs
        assert tabs == sorted(tabs)                          # original order kept
        for frame in frames:
            assert all(b < 0x80 for b in only(frame).payload[6:])

    def test_an_old_card_sees_glyphs_folded_to_ascii(self):
        rows = (project.Row(PROJECT_TAB_WATCH, "build",
                            f"{project.sparkline([0, 1, 2, 3, 4, 5, 6, 7], 8)} ✓ pass"),
                project.Row(PROJECT_TAB_WATCH, "deploy",
                            f"{project.progress_bar(50, 8)} → staging"))
        got = self._decoded(project.encode(rows, project64=False))
        assert got[0][5] == "_.,-=+*# + pass"
        assert got[1][5] == "####.... -> staging"

    def test_a_small_snapshot_is_unchanged_but_for_the_folding(self):
        rows = (project.Row(PROJECT_TAB_NOW, "branch", "main"),)
        assert project.encode(rows, project64=False) == project.encode(rows)

    def test_the_old_contract_is_enforced_by_the_encoder_too(self):
        with pytest.raises(ProtocolError):
            encode_project_row(0, PROJECT_ROWS_MAX_V1 + 1, 0, 0, "a", "b",
                               project64=False)
        with pytest.raises(ProtocolError):
            encode_project_row(0, 1, 0, 0, "a", "█", project64=False)
        encode_project_row(0, 1, 0, 0, "a", "█", project64=True)

    def test_the_server_honours_the_token(self):
        from dsterm.connection import ClientState
        from dsterm.protocol import FEATURE_PROJECT64

        assert FEATURE_PROJECT64 == "project64"
        assert ClientState(None, ("t", 1), None, b"").caps.project64 is False

    def test_the_server_sends_each_card_its_own_contract(self):
        from dsterm.connection import ClientState
        from dsterm.server import Server

        srv = Server(["true"], b"k" * 16, host="127.0.0.1", port=0,
                     voice_enabled=False)
        rows = _rows_of_every_tab(11)[:PROJECT_ROWS_MAX]
        srv._project._rows = rows
        old = ClientState(None, ("old", 1), None, b"")
        new = ClientState(None, ("new", 1), None, b"")
        for c, wide in ((old, False), (new, True)):
            c.caps.project = True
            c.caps.project64 = wide
            srv._clients.append(c)
            srv._advertise_project(c)
        assert len(old.outbox) == PROJECT_ROWS_MAX_V1
        assert len(new.outbox) == PROJECT_ROWS_MAX
        assert all(b < 0x80 for f in old.outbox for b in only(f).payload[6:])
