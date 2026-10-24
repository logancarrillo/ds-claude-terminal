"""Facts about the repository the child is working in, for the PROJECT page.

The handheld cannot see a filesystem and must never be able to name one. This
module is the whole of what it learns: a fixed set of read-only questions asked
of ``git`` in the server's own working directory, turned into finished label and
value strings that the card draws and does not interpret.

Three properties are load-bearing and none of them is an accident:

* **Read-only.** Every command below is a query. Nothing here writes, checks
  out, stages, or fetches, and the argument lists are literals -- no value from
  the wire, the page, or the repository is ever spliced into one.
* **Facts, not verdicts.** "quiet for 40s" is a fact. "the agent is stuck" is a
  guess about someone else's state, and this server does not make those; the
  same rule that keeps it from inferring success or failure from the shape of
  the terminal applies to the page built beside it.
* **Failure is silence.** A directory that is not a repository, a git that is
  missing, a command that hangs: all of them produce no rows. The card shows
  "this server does not report it", which is true, rather than a stale answer.

The card renders whatever it is sent, so every string that leaves here is
already clipped to the widths in ``protocol.py``.

Beyond git, the WATCH and HEALTH tabs read two files the person writes in the
project's ``.dsterm/`` (``timer.json`` from ``dsterm timer``, ``watch.json``
by hand), the load average and the disk's free space. One source is off by
default because it is not local: GitHub Actions runs come from ``gh run
list``, a network call every 30 s, only under ``dsterm serve --project-ci``.

A card from before "project64" (1.0.x) refuses a snapshot of more than 48
rows and draws the glyph bytes as '?'; ``encode`` gives it the rows that
matter most, folded to ASCII (``fold_ascii``).
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

from . import actions as dsactions
from . import protocol
from .extmap import TO_WIRE

# Long enough for a cold cache on a large repository, short enough that a
# wedged git cannot stall the frame loop it is polled from.
_GIT_TIMEOUT = 3.0

# Git is forked to answer these, so it is asked on its own slow clock rather
# than on the server's 15 fps tick or even the once-a-second session poll.
# Nothing on this page changes faster than this is worth noticing.
POLL_INTERVAL_S = 3.0

# How many changed files the DIFF tab lists. The page has eighteen rows and the
# rest of the answer is the summary line above them.
_DIFF_FILES_MAX = 12

# The WATCH tab's budgets: at most this many rows from watch.json, this many
# rows in the tab altogether (timer, watch.json and CI), and this many of the
# latest GitHub Actions runs asked for under --project-ci.
_WATCH_FILE_ROWS_MAX = 12
_WATCH_ROWS_MAX = 14
_CI_RUNS = 4

# Palette indices, mirrored from render.h. Named here so the wording and the
# colour of a row are decided in the same place.
_PLAIN = 0
_RED = 2
_GREEN = 3
_YELLOW = 4
_CYAN = 7
_ORANGE = 9

_SPARK_BARS = "▁▂▃▄▅▆▇█"


def sparkline(values: Sequence[float], width: int = 12) -> str:
    """Render a sequence of numeric readings as an 8-level block sparkline."""
    if not values or width <= 0:
        return ""
    tail = [float(v) for v in values[-width:]]
    # A NaN or an infinity has no height; drawing one anyway would be a bar
    # that says something the reading did not. The caller drops the row.
    if not all(math.isfinite(v) for v in tail):
        raise ValueError("a sparkline reading is not a finite number")
    lo, hi = min(tail), max(tail)
    span = hi - lo
    out: list[str] = []
    for v in tail:
        if span <= 1e-9:
            idx = 3
        else:
            idx = int(round(((v - lo) / span) * 7))
            idx = max(0, min(7, idx))
        out.append(_SPARK_BARS[idx])
    return "".join(out)


def progress_bar(pct: float, width: int = 10) -> str:
    """Render a 0..100 percentage as a block progress bar (`█` and `░`)."""
    if width <= 0:
        return ""
    value = float(pct)
    # min/max pass a NaN straight through as "100%": refused instead.
    if not math.isfinite(value):
        raise ValueError("a progress value is not a finite number")
    clamped = max(0.0, min(100.0, value))
    filled = int(round((clamped / 100.0) * width))
    filled = max(0, min(width, filled))
    return ("█" * filled) + ("░" * (width - filled))


@dataclass(slots=True)
class Row:
    """One line of the page: which tab, what it says, and what colour."""

    tab: int
    label: str
    value: str
    fg: int = _PLAIN


@dataclass(slots=True)
class ProjectState:
    """The last snapshot, and when it was taken."""

    rows: tuple[Row, ...] = ()
    taken_at: float = 0.0
    # Set once we know the cwd is not a git worktree, so a non-repository
    # directory costs one git invocation rather than one every three seconds.
    not_a_repo: bool = False
    _pending: list[Row] = field(default_factory=list)


def _git(cwd: str, *args: str) -> str | None:
    """One read-only git query. None on any failure at all.

    ``args`` is always a literal at every call site below; this signature takes
    them varargs so that stays visible rather than being assembled somewhere.
    """
    try:
        out = subprocess.run(
            ["git", "--no-optional-locks", *args],
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=_GIT_TIMEOUT,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    return out.stdout.strip()


def _ago(committed_at: int, now: float) -> str:
    """A commit age that changes when the answer does, not once a second.

    This row used to be ``git log -1 --pretty=%cr`` straight onto the page, and
    %cr is a live clock: "3 seconds ago", then "6 seconds ago". Every poll
    therefore changed the row, which changed the signature ``_advertise_project``
    compares, which re-sent the entire six-frame page -- every three seconds,
    forever, on a repository nobody had touched. Measured: 6 of 6 polls re-sent
    it. ``refresh`` promises that returning "changed" rather than "collected" is
    what keeps a quiet page off the wire, and against any repo with a recent
    commit that promise was never once kept.

    Freezing it was not the answer either: an age that never moved would be
    wrong within the minute. So the value is bucketed. It holds still for the
    whole first minute -- the window somebody is most likely to be watching --
    then changes at a boundary that means something, and the tilde says the
    number is coarse, exactly as the "quiet for" row already does.

    The alternative was to send the absolute timestamp and let the card render
    it, which is the better split and is a wire change. Not taken unilaterally
    while the client half is mid-pass; this needs no wire change at all.
    """
    delta = max(0, int(now - committed_at))
    if delta < 60:
        return "just now"
    if delta < 3600:
        return f"~{delta // 60}m ago"
    if delta < 86_400:
        return f"~{delta // 3600}h ago"
    return f"~{delta // 86_400}d ago"


def _clip(text: str, limit: int) -> str:
    """Printable ASCII or extended DS glyph, clipped. The encoder enforces
    both; doing it here too means the value the page shows is the value this
    module decided on, rather than whatever survived a truncation it could not
    see."""
    safe = "".join(
        c if (0x20 <= ord(c) <= 0x7E or ord(c) in TO_WIRE) else "?"
        for c in text
    )
    return safe[:limit]


_COLOR_MAP = {
    "default": _PLAIN,
    "plain": _PLAIN,
    "red": _RED,
    "green": _GREEN,
    "yellow": _YELLOW,
    "cyan": _CYAN,
    "orange": _ORANGE,
}

# Slow 30s caches so collect() is fast and deterministic across adjacent polls.
_CI_CACHE: dict[str, tuple[float, list[Row]]] = {}
_HEALTH_CACHE: dict[str, tuple[float, list[Row]]] = {}
_LOAD_HISTORY: list[float] = []
_SLOW_CACHE_SECONDS = 30.0


def _watch_row(entry: object, l_max: int, v_max: int) -> Row | None:
    """One watch.json entry as a row, or None when it is not one. Raises on a
    value of the wrong type; the caller skips that entry and keeps the rest."""
    if not isinstance(entry, dict):
        return None
    lbl = _clip(str(entry.get("label", "")).strip(), l_max)
    if not lbl:
        return None
    val = str(entry.get("value", "")).strip()
    if "spark" in entry and isinstance(entry["spark"], list):
        sp = sparkline(entry["spark"], width=10)
        val = f"{sp} {val}".strip()
    elif "progress" in entry:
        pb = progress_bar(float(entry["progress"]), width=8)
        val = f"{pb} {val}".strip()
    fg_raw = entry.get("fg", entry.get("color", "plain"))
    fg = (fg_raw if isinstance(fg_raw, int) and not isinstance(fg_raw, bool)
          and 0 <= fg_raw <= 15
          else _COLOR_MAP.get(str(fg_raw).lower(), _PLAIN))
    return Row(protocol.PROJECT_TAB_WATCH, lbl, _clip(val, v_max), fg)


def _collect_watch(cwd: str, ci: bool = False) -> list[Row]:
    """Rows for the WATCH tab: Pomodoro timer, declarative .dsterm/watch.json,
    and -- with ``ci`` (``--project-ci``) only -- GitHub Actions runs when the
    origin is on github.com and `gh` is installed."""
    watch = protocol.PROJECT_TAB_WATCH
    l_max = protocol.PROJECT_LABEL_MAX
    v_max = protocol.PROJECT_VALUE_MAX
    rows: list[Row] = []

    # 1. Pomodoro / focus timer (.dsterm/timer.json)
    timer_path = Path(cwd) / ".dsterm" / "timer.json"
    if timer_path.is_file():
        try:
            data = json.loads(timer_path.read_text(encoding="utf-8"))
            # Valid JSON that is not an object ([], null, "x") is as bad a
            # file as a truncated one: no row, never an exception that would
            # take the whole page down with it.
            if not isinstance(data, dict):
                raise ValueError("timer.json is not an object")
            end_ts = float(data.get("end_ts", 0))
            duration_s = max(60.0, float(data.get("duration_s", 1500)))
            label = str(data.get("label", "focus"))[:l_max] or "focus"
            rem = end_ts - time.time()
            if rem <= 0:
                rows.append(Row(watch, _clip(label, l_max),
                                _clip(f"{progress_bar(100, 8)} done!", v_max), _GREEN))
            else:
                mins_left = int((rem + 59) // 60)
                pct = 100.0 * (1.0 - (rem / duration_s))
                bar = progress_bar(pct, 8)
                rows.append(Row(watch, _clip(label, l_max),
                                _clip(f"{bar} ~{mins_left}m left", v_max), _ORANGE))
        except (OSError, ValueError, TypeError, AttributeError, OverflowError):
            # One bad file costs its own row, never the page: an end_ts of
            # 1e400 is an infinity, and int() of one raises OverflowError.
            pass

    # 2. Declarative watch rows (.dsterm/watch.json)
    watch_file = Path(cwd) / ".dsterm" / "watch.json"
    if watch_file.is_file():
        try:
            raw = json.loads(watch_file.read_text(encoding="utf-8"))
            items = raw.get("rows", raw) if isinstance(raw, dict) else raw
            if isinstance(items, list):
                for entry in items[:_WATCH_FILE_ROWS_MAX]:
                    # Per entry: one bad "spark" or "progress" (a string, a
                    # NaN, an infinity) costs that row, not the whole file.
                    try:
                        row = _watch_row(entry, l_max, v_max)
                    except (ValueError, TypeError, OverflowError):
                        continue
                    if row is not None:
                        rows.append(row)
        except (OSError, ValueError, TypeError, AttributeError):
            pass

    # 3. GitHub Actions CI status: opt-in (--project-ci), because `gh run
    # list` asks GitHub over the network, every 30 s while a card is
    # connected. Cached 30 s, and only with a github.com origin.
    if not ci:
        return rows[:_WATCH_ROWS_MAX]
    now = time.monotonic()
    cached = _CI_CACHE.get(cwd)
    if cached is not None and now - cached[0] < _SLOW_CACHE_SECONDS:
        rows.extend(cached[1])
    else:
        ci_rows: list[Row] = []
        remote_url = _git(cwd, "config", "--get", "remote.origin.url")
        gh = shutil.which("gh") if remote_url else None
        if gh and remote_url and "github.com" in remote_url:
            try:
                out = subprocess.run(
                    [gh, "run", "list", "--limit", str(_CI_RUNS),
                     "--json", "name,status,conclusion,headBranch"],
                    cwd=cwd, capture_output=True, text=True, timeout=2.5, check=False,
                )
                if out.returncode == 0 and out.stdout.strip():
                    runs = json.loads(out.stdout)
                    # gh answers with a list of objects; anything else (an
                    # object, a null, a list of strings) is no rows, not an
                    # exception that freezes the page.
                    if not isinstance(runs, list):
                        runs = []
                    for r in runs[:_CI_RUNS]:
                        if not isinstance(r, dict):
                            continue
                        status = str(r.get("status", ""))
                        conc = str(r.get("conclusion", ""))
                        wf = str(r.get("name", "ci"))[:l_max] or "ci"
                        br = str(r.get("headBranch", ""))
                        if status != "completed":
                            mark, fg = "●", _YELLOW
                            state_word = status or "running"
                        elif conc == "success":
                            mark, fg = "✓", _GREEN
                            state_word = "pass"
                        else:
                            mark, fg = "✗", _RED
                            state_word = conc or "fail"
                        ci_rows.append(Row(
                            watch,
                            _clip(wf, l_max),
                            _clip(f"{mark} {state_word} ({br})", v_max),
                            fg,
                        ))
            except (OSError, subprocess.SubprocessError, ValueError,
                    TypeError, AttributeError):
                pass
        _CI_CACHE[cwd] = (now, ci_rows)
        rows.extend(ci_rows)

    return rows[:_WATCH_ROWS_MAX]


def _collect_health(cwd: str) -> list[Row]:
    """Rows for the HEALTH tab (cached 30s so adjacent polls stay identical)."""
    now = time.monotonic()
    cached = _HEALTH_CACHE.get(cwd)
    if cached is not None and now - cached[0] < _SLOW_CACHE_SECONDS:
        return list(cached[1])

    health = protocol.PROJECT_TAB_HEALTH
    v_max = protocol.PROJECT_VALUE_MAX
    rows: list[Row] = []

    try:
        load1, _, _ = os.getloadavg()
        cpus = os.cpu_count() or 1
        pct = min(100.0, (load1 / cpus) * 100.0)
        _LOAD_HISTORY.append(load1)
        if len(_LOAD_HISTORY) > 12:
            del _LOAD_HISTORY[:-12]
        sp = sparkline(_LOAD_HISTORY, width=8)
        fg = _RED if pct >= 85.0 else (_ORANGE if pct >= 60.0 else _GREEN)
        rows.append(Row(health, "server cpu",
                        _clip(f"{sp} {load1:.1f} ({cpus}c)", v_max), fg))
    except (AttributeError, OSError):
        pass

    try:
        usage = shutil.disk_usage(cwd)
        used_pct = (usage.used / usage.total) * 100.0 if usage.total else 0.0
        free_gb = usage.free // (1024 ** 3)
        bar = progress_bar(used_pct, width=8)
        fg = _RED if used_pct >= 90.0 else _GREEN
        rows.append(Row(health, "server disk",
                        _clip(f"{bar} {free_gb}G free", v_max), fg))
    except OSError:
        pass

    _HEALTH_CACHE[cwd] = (now, rows)
    return list(rows)


def collect(cwd: str, quiet_s: int | None = None,
            ci: bool = False) -> tuple[Row, ...]:
    """Every row of one snapshot, or () when there is nothing honest to say."""
    toplevel = _git(cwd, "rev-parse", "--show-toplevel")
    if not toplevel:
        return ()

    rows: list[Row] = []
    now = protocol.PROJECT_TAB_NOW
    diff = protocol.PROJECT_TAB_DIFF
    v_max = protocol.PROJECT_VALUE_MAX

    name = toplevel.rsplit("/", 1)[-1] or toplevel
    rows.append(Row(now, "repo", _clip(name, v_max), _ORANGE))

    # A detached HEAD reports the short sha instead, which is the honest answer
    # rather than an empty branch line.
    branch = _git(cwd, "rev-parse", "--abbrev-ref", "HEAD")
    if branch == "HEAD":
        branch = _git(cwd, "rev-parse", "--short", "HEAD")
        if branch:
            branch = f"detached at {branch}"
    if branch:
        rows.append(Row(now, "branch", _clip(branch, v_max), _ORANGE))

    porcelain = _git(cwd, "status", "--porcelain")
    if porcelain is not None:
        lines = [ln for ln in porcelain.splitlines() if ln.strip()]
        if not lines:
            rows.append(Row(now, "state", "clean", _GREEN))
        else:
            untracked = sum(1 for ln in lines if ln.startswith("??"))
            tracked = len(lines) - untracked
            parts = []
            if tracked:
                parts.append(f"{tracked} changed")
            if untracked:
                parts.append(f"{untracked} new")
            rows.append(Row(now, "state", _clip(", ".join(parts), v_max), _RED))

    # Ahead/behind is only meaningful with an upstream, and plenty of branches
    # have none. No upstream is not a problem to report, so it is simply absent.
    counts = _git(cwd, "rev-list", "--left-right", "--count", "@{upstream}...HEAD")
    if counts:
        try:
            behind_s, ahead_s = counts.split()
            behind, ahead = int(behind_s), int(ahead_s)
        except ValueError:
            behind = ahead = 0
        if ahead or behind:
            rows.append(Row(now, "upstream",
                            _clip(f"{ahead} ahead, {behind} behind", v_max),
                            _ORANGE))

    subject = _git(cwd, "log", "-1", "--pretty=%s")
    if subject:
        rows.append(Row(now, "last commit", _clip(subject, v_max)))
    # %ct, the absolute commit time, and bucketed here -- not %cr, which is a
    # clock that re-sent this whole page every three seconds. See _ago.
    when = _git(cwd, "log", "-1", "--pretty=%ct")
    if when:
        try:
            committed_at = int(when)
        except ValueError:
            committed_at = 0
        if committed_at:
            rows.append(Row(now, "committed",
                            _clip(_ago(committed_at, time.time()), v_max)))

    # A fact, deliberately, and not a verdict. How long the terminal has been
    # quiet is something this server measured; whether that means the agent is
    # stuck is a guess about someone else's state, and it does not make those.
    if quiet_s is not None:
        # Rounded to five seconds and marked approximate, because the number is
        # sampled when the collection starts: by the time the row is drawn it
        # can be a whole snapshot older than POLL_INTERVAL_S already made it,
        # and a bare "43s" invites a precision it does not have. Under five
        # seconds says so rather than rounding down to a "0s" that would read
        # as "producing output right now", which is the opposite of the truth.
        quiet_text = ("<5s" if quiet_s < 5
                      else f"~{int(round(quiet_s / 5.0)) * 5}s")
        rows.append(Row(now, "quiet for", _clip(quiet_text, v_max)))

    # DO: what this project can be told to do.  The value is the command
    # itself, deliberately -- that is what puts the exact text on the page
    # beside the button, so it can be read before it is pressed.  See
    # actions.py for why a script name that is not plainly a name is skipped
    # rather than quoted.
    for label, body in dsactions.rows(cwd):
        # Clipped like every other value here even though actions.py already
        # bounds it: the encoder raises on an over-long or non-ASCII value, and
        # that raise would come out inside the tick that queues the snapshot.
        # Nothing should be able to reach the wire without passing through the
        # same gate the rest of the page does.
        rows.append(Row(protocol.PROJECT_TAB_DO, _clip(label,
                                                       protocol.PROJECT_LABEL_MAX),
                        _clip(body, v_max), _ORANGE))

    # DIFF: what the working tree has that HEAD does not.
    numstat = _git(cwd, "diff", "--numstat", "HEAD")
    if numstat is not None:
        entries: list[tuple[str, str, str]] = []
        added = removed = 0
        for line in numstat.splitlines():
            parts = line.split("\t", 2)
            if len(parts) != 3:
                continue
            plus, minus, path = parts
            entries.append((plus, minus, path))
            if plus.isdigit():
                added += int(plus)
            if minus.isdigit():
                removed += int(minus)
        if not entries:
            rows.append(Row(diff, "working", "nothing uncommitted", _GREEN))
        else:
            rows.append(Row(diff, "working",
                            _clip(f"{len(entries)} files  +{added} -{removed}",
                                  v_max),
                            _ORANGE))
            for plus, minus, path in entries[:_DIFF_FILES_MAX]:
                # The path is the label so the numbers line up down the page,
                # and the tail of a long path is the informative half.
                short = path if len(path) <= protocol.PROJECT_LABEL_MAX else \
                    "~" + path[-(protocol.PROJECT_LABEL_MAX - 1):]
                rows.append(Row(diff, _clip(short, protocol.PROJECT_LABEL_MAX),
                                _clip(f"+{plus} -{minus}  {path}", v_max)))
            if len(entries) > _DIFF_FILES_MAX:
                rows.append(Row(diff, "and",
                                f"{len(entries) - _DIFF_FILES_MAX} more"))

    # WATCH: Pomodoro timer, declarative .dsterm/watch.json, and CI runs.
    rows.extend(_collect_watch(toplevel, ci))

    # HEALTH: server CPU load sparkline and disk headroom.
    rows.extend(_collect_health(toplevel))

    # LOG: recent commit history on the repository.
    recent_log = _git(cwd, "log", "-6", "--pretty=%h %s")
    if recent_log:
        for line in recent_log.splitlines()[:6]:
            sha, _, subj = line.partition(" ")
            if sha and subj:
                rows.append(Row(protocol.PROJECT_TAB_LOG,
                                _clip(sha, protocol.PROJECT_LABEL_MAX),
                                _clip(subj, v_max), _PLAIN))

    return tuple(rows[:protocol.PROJECT_ROWS_MAX])


def refresh(state: ProjectState, cwd: str, quiet_s: int | None = None,
            now: float | None = None) -> bool:
    """Re-collect if the slow clock says it is time. True if the rows changed.

    Returning "changed" rather than "collected" is what keeps the page off the
    wire when nothing has happened, which on a repository nobody is touching is
    almost always.
    """
    stamp = time.monotonic() if now is None else now
    if state.not_a_repo:
        return False
    if state.taken_at and stamp - state.taken_at < POLL_INTERVAL_S:
        return False
    state.taken_at = stamp
    rows = collect(cwd, quiet_s)
    if not rows and not state.rows and not state.taken_at:
        state.not_a_repo = True
    changed = rows != state.rows
    state.rows = rows
    return changed


class BackgroundCollector:
    """``collect`` on a worker thread, so the frame loop never waits for git.

    ``refresh`` above is the synchronous answer and remains correct for anything
    that can afford to block. The server's tick loop cannot. Measured against
    the working directory this server actually runs in -- a Windows path reached
    over WSL's drvfs -- one snapshot costs 1.59 s, of which ``status
    --porcelain`` is 0.70 s and ``diff --numstat`` 0.56 s, on a repository of
    only 269 tracked files. Polled inline every ``POLL_INTERVAL_S`` that is over
    half of wall-clock time with no frames reaching the handheld, and eight
    queries each hitting ``_GIT_TIMEOUT`` would be a 24 s freeze. On a developer
    checkout on ext4 the same snapshot is an order of magnitude cheaper, so the
    stall is invisible exactly where the code gets written.

    ``poll`` is the whole interface and it never blocks: it hands back a
    finished snapshot if the worker has one, starts a new one when the slow
    clock says so, and returns immediately either way.

    Two deliberate differences from ``refresh``:

    * **No ``not_a_repo`` latch.** A directory that is not a worktree costs one
      ``rev-parse`` per interval, which is ~16 ms and now off the loop entirely.
      Latching would also mean a git that was briefly missing silences the page
      until the server restarts, and this way it heals by itself.
    * **``quiet_s`` is sampled when the collection starts,** not when it lands,
      so that row can be one collection older than the rest. The value was
      already up to ``POLL_INTERVAL_S`` stale by design; this widens that by the
      length of one snapshot.
    """

    def __init__(self, cwd: str, interval: float = POLL_INTERVAL_S,
                 ci: bool = False) -> None:
        self._cwd = cwd
        # --project-ci: whether WATCH may ask GitHub (see _collect_watch).
        self._ci = ci
        self._interval = interval
        self._lock = threading.Lock()
        self._rows: tuple[Row, ...] = ()
        self._result: tuple[Row, ...] | None = None
        self._worker: threading.Thread | None = None
        self._started_at: float | None = None
        self._stopped = False

    @property
    def rows(self) -> tuple[Row, ...]:
        """The last complete snapshot. Never a half-gathered one."""
        with self._lock:
            return self._rows

    def poll(self, quiet_s: int | None = None,
             now: float | None = None) -> bool:
        """Publish a finished snapshot and start the next. True if rows changed.

        Safe to call every tick; the interval is enforced here, not by the
        caller. Returning "changed" rather than "collected" is what keeps a
        quiet repository off the wire, exactly as ``refresh`` does.
        """
        stamp = time.monotonic() if now is None else now
        changed = False
        worker: threading.Thread | None = None

        with self._lock:
            if self._stopped:
                return False

            # Harvest first: a snapshot that finished during the last tick is
            # already paid for, and publishing it frees the slot for the next.
            if self._result is not None:
                rows, self._result = self._result, None
                self._worker = None
                if rows != self._rows:
                    self._rows = rows
                    changed = True

            due = (self._started_at is None
                   or stamp - self._started_at >= self._interval)
            if self._worker is None and due:
                self._started_at = stamp
                worker = threading.Thread(
                    target=self._run, args=(quiet_s,),
                    name="dsterm-project", daemon=True)
                self._worker = worker

        # Started outside the lock: the worker's first act is to want it.
        if worker is not None:
            worker.start()
        return changed

    def stop(self, timeout: float = 2.0) -> None:
        """Stop polling and wait briefly for a running collection. Idempotent.

        The worker is a daemon and every git call is bounded by ``_GIT_TIMEOUT``,
        so a wedged one cannot hold the process open even if this times out.
        """
        with self._lock:
            self._stopped = True
            worker = self._worker
            self._worker = None
        if worker is not None and worker.is_alive():
            worker.join(timeout)

    def _run(self, quiet_s: int | None) -> None:
        """One collection, on the worker thread. Failure is silence."""
        try:
            # Resolved here rather than captured in __init__ so that patching
            # the module function is honoured -- and so a future caller can.
            rows: tuple[Row, ...] | None = collect(self._cwd, quiet_s, self._ci)
        except Exception:
            # The page keeps its last honest answer, and the next tick tries
            # again. A collector that dies on one bad git is a page that goes
            # silent until the server restarts.
            rows = None
        with self._lock:
            if rows is None:
                self._worker = None
            else:
                self._result = rows


# The DS glyphs as the ASCII a 1.0.x card can draw. The sparkline's eight
# levels stay eight distinct, rising characters; a bar is '#' on '.'.
_ASCII_FOLD = {
    "─": "-", "━": "-", "═": "-",
    "│": "|", "┃": "|", "║": "|",
    "┌": "+", "┐": "+", "└": "+", "┘": "+", "├": "+", "┤": "+",
    "┬": "+", "┴": "+", "┼": "+", "╭": "+", "╮": "+", "╯": "+", "╰": "+",
    "▁": "_", "▂": ".", "▃": ",", "▄": "-", "▅": "=", "▆": "+", "▇": "*",
    "█": "#", "▀": "#", "▌": "#", "▐": "#",
    "░": ".", "▒": ":", "▓": "%",
    "▖": "#", "▗": "#", "▘": "#", "▙": "#", "▚": "#", "▛": "#", "▜": "#",
    "▝": "#", "▞": "#", "▟": "#",
    "\u00a0": " ", "●": "*", "○": "o", "•": "*", "▪": "*", "·": ".",
    "…": "...", "✓": "+", "✗": "x", "✘": "x", "⏸": "||", "✻": "*",
    "✨": "*", "←": "<-", "→": "->", "↑": "^", "↓": "v",
    "❯": ">", "›": ">", "❮": "<", "‹": "<",
}


def fold_ascii(text: str) -> str:
    """``text`` in printable ASCII: each DS glyph folded to its stand-in
    above, anything else non-printable '?'."""
    return "".join(
        ch if 0x20 <= ord(ch) <= 0x7E else _ASCII_FOLD.get(ch, "?")
        for ch in text
    )


# Which rows a 1.0.x card keeps when the snapshot is over its 48: NOW, DIFF
# and DO are the page it was built for, then WATCH, HEALTH and LOG -- the
# newer tabs -- in that order, row by row until there are 48: a tab that does
# not fit whole keeps its first rows, and the tabs after it keep none.
_V1_PRIORITY = (
    (protocol.PROJECT_TAB_NOW, protocol.PROJECT_TAB_DIFF, protocol.PROJECT_TAB_DO),
    (protocol.PROJECT_TAB_WATCH,),
    (protocol.PROJECT_TAB_HEALTH,),
    (protocol.PROJECT_TAB_LOG,),
)


def _for_v1(rows: tuple[Row, ...]) -> tuple[Row, ...]:
    """At most PROJECT_ROWS_MAX_V1 rows, the most important kept, in their
    original order; every string folded to ASCII and re-clipped."""
    limit = protocol.PROJECT_ROWS_MAX_V1
    keep: set[int] = set()
    for tabs in _V1_PRIORITY:
        for i, r in enumerate(rows):
            if r.tab in tabs and len(keep) < limit:
                keep.add(i)
    return tuple(
        Row(r.tab, fold_ascii(r.label)[:protocol.PROJECT_LABEL_MAX] or "?",
            fold_ascii(r.value)[:protocol.PROJECT_VALUE_MAX], r.fg)
        for i, r in enumerate(rows) if i in keep
    )


def encode(rows: tuple[Row, ...], project64: bool = True) -> list[bytes]:
    """The snapshot as wire frames, one row each.

    An empty snapshot is one explicit empty message rather than no messages,
    because the card distinguishes "nothing to report" from "never told".

    ``project64=False`` is for a card that did not announce "project64": it
    gets ``_for_v1``'s rows, at most 48 and ASCII only, which is everything it
    accepts.
    """
    if not project64:
        rows = _for_v1(rows)
    if not rows:
        return [protocol.encode_project_empty()]
    count = len(rows)
    return [
        protocol.encode_project_row(i, count, r.tab, r.fg, r.label, r.value,
                                    project64=project64)
        for i, r in enumerate(rows)
    ]
