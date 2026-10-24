"""The server's shape, held: one module a feature, no cycles, no flat state.

server.py was once 3,790 lines and ClientState 73 fields, and nothing failed
while it grew. These tests are the ratchet: a feature module that reaches
back into server.py, a mixin that is not mixed in, or new per-feature state
added to ClientState's top level fails here, with a message saying where it
belongs instead.
"""

import ast
import dataclasses
from pathlib import Path

from dsterm.connection import ClientState
from dsterm.server import Server

DSTERM = Path(__file__).resolve().parent.parent / "dsterm"
FEATURES = sorted(DSTERM.glob("server_*.py"))


def test_there_are_feature_modules():
    assert len(FEATURES) >= 8


def test_each_feature_is_one_mixin_and_server_has_them_all():
    mixins = []
    for path in FEATURES:
        tree = ast.parse(path.read_text())
        found = [n.name for n in tree.body
                 if isinstance(n, ast.ClassDef) and n.name.endswith("Mixin")]
        assert len(found) == 1, f"{path.name}: {found}"
        mixins.append(found[0])
    bases = {b.__name__ for b in Server.__mro__}
    missing = [m for m in mixins if m not in bases]
    assert not missing, f"not mixed into Server: {missing}"


def test_no_feature_imports_the_core():
    # The core imports the features; a feature importing server.py back is a
    # cycle, and the start of everything living in one place again.
    for path in FEATURES + [DSTERM / "connection.py"]:
        tree = ast.parse(path.read_text())
        for node in ast.walk(tree):
            if isinstance(node, ast.ImportFrom) and node.level == 1:
                assert node.module != "server", f"{path.name} imports .server"


def test_client_state_stays_in_parts():
    # The connection's own fields are at the top; what a feature keeps for a
    # card is a part it owns (caps, feeds, sessions, ext, scroll, agents,
    # camera). A new feature's state goes in a new part, not here.
    top = [f.name for f in dataclasses.fields(ClientState)]
    assert len(top) <= 30, (
        f"ClientState has {len(top)} top-level fields; put per-feature state "
        f"in a part (connection.py) rather than at the top")
    c = ClientState(None, ("t", 1), None, b"")
    parts = [f.name for f in dataclasses.fields(ClientState)
             if dataclasses.is_dataclass(getattr(c, f.name))]
    assert parts == ["caps", "feeds", "sessions", "ext", "scroll", "agents", "camera", "voice"]


def test_state_is_slotted():
    # A misspelt field is an AttributeError, never a silent new attribute.
    c = ClientState(None, ("t", 1), None, b"")
    for holder in (c, c.caps, c.feeds, c.sessions, c.ext, c.scroll, c.agents, c.camera, c.voice):
        try:
            holder.no_such_field = 1
        except AttributeError:
            continue
        raise AssertionError(f"{type(holder).__name__} accepts unknown attributes")


def test_handle_is_a_dispatcher():
    # Each message's handling is a method of its own (in its feature's module
    # where it has one); _handle only routes.
    tree = ast.parse((DSTERM / "server.py").read_text())
    server = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "Server")
    handle = next(m for m in server.body if isinstance(m, ast.FunctionDef) and m.name == "_handle")
    assert handle.end_lineno - handle.lineno < 80, "_handle is growing handlers again"


# -- who owns what, and what crosses ------------------------------------------
#
# A feature's server state is whatever its _init_<feature>() sets, and only
# its own module touches it. What one feature asks of another goes through a
# method, and every such crossing is listed here with its reason: a new one
# fails the test until it is added -- coupling is a decision, written down.

SHARED_STATE = {
    ("dashboard", "camera_view"): "the dashboard's CAMERA ON badge",
    ("server_mirror", "camera_view"): "`dsterm view`'s status says whether the camera is on",
}

FEATURE_CALLS = {
    # REMOTE and STATE read SESSIONS' window catalogue and switch windows.
    ("server_agents", "server_sessions", "_listing"),
    ("server_agents", "server_sessions", "_tmux_lifetime"),
    ("server_agents", "server_sessions", "_observe_windows"),
    ("server_agents", "server_sessions", "_list_windows_soon"),
    ("server_agents", "server_sessions", "_switch_to_window"),
    # A listing tells REMOTE which of its windows survive, and STATE which
    # window is on screen; a switch sends the frames the card must see first.
    ("server_sessions", "server_agents", "_note_remote_windows"),
    ("server_sessions", "server_agents", "_active_window_id"),
    ("server_sessions", "server_agents", "_active_window_changed"),
    ("server_sessions", "server_agents", "_refresh_state"),
    ("server_sessions", "server_feeds", "_mode_frame"),
    ("server_sessions", "server_scrollback", "_view_pos_frame"),
    ("server_sessions", "server_scrollback", "_view_to_live"),
    # PROJECT's idle time is SESSIONS' number; a pushed list waits for a
    # SESSIONS or REMOTE set in flight.
    ("server_feeds", "server_sessions", "_active_idle_seconds"),
    ("server_feeds", "server_sessions", "_session_batch_queued_or_active"),
    ("server_feeds", "server_agents", "_remote_batch_queued_or_active"),
    # `dsterm view` shows the top screen as the card has it, scrolled or not,
    # and says how far back.
    ("server_mirror", "server_scrollback", "_view_for"),
    ("server_mirror", "server_scrollback", "_lines_back"),
    # Nothing goes between a window switch's frames: REMOTE, STATE, the feeds
    # and an extension's frame each wait for it, and a switch drops the
    # extension frame queued before it.
    ("server_agents", "server_sessions", "_switch_pending"),
    ("server_agents", "server_sessions", "_switch_in_progress"),
    ("server_agents", "server_sessions", "_session_list_staged"),
    ("server_feeds", "server_sessions", "_switch_in_progress"),
    ("server_ext", "server_sessions", "_switch_in_progress"),
    ("server_sessions", "server_ext", "_forget_queued_extension"),
}

# Each card's state is in parts (connection.py), one a feature. The core may
# touch any of them -- it is the connection -- and `caps` is the HELLO's,
# read by everyone; any other part is touched only by its own feature.
PART_OWNERS = {"feeds": "server_feeds", "sessions": "server_sessions", "ext": "server_ext",
               "scroll": "server_scrollback", "agents": "server_agents", "camera": "server_camera",
               "voice": "server_voice"}


def _ownership():
    files = {p.stem: p for p in [DSTERM / "server.py", DSTERM / "dashboard.py"] + FEATURES}
    owned, methods = {}, {}
    for mod, path in files.items():
        for cls in ast.walk(ast.parse(path.read_text())):
            if isinstance(cls, ast.ClassDef) and (cls.name.endswith("Mixin") or cls.name == "Server"):
                for fn in cls.body:
                    if not isinstance(fn, ast.FunctionDef):
                        continue
                    methods[fn.name] = mod
                    if fn.name.startswith("_init_") and mod.startswith("server_"):
                        for n in ast.walk(fn):
                            if (isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name)
                                    and n.value.id == "self" and isinstance(n.ctx, ast.Store)):
                                owned[n.attr] = mod
    state, calls = set(), set()
    for mod, path in files.items():
        for n in ast.walk(ast.parse(path.read_text())):
            if not (isinstance(n, ast.Attribute) and isinstance(n.value, ast.Name)
                    and n.value.id in ("self", "server")):
                continue
            if n.attr in owned and owned[n.attr] != mod:
                state.add((mod, n.attr))
            elif mod.startswith("server_") and methods.get(n.attr, mod) not in (mod, "server"):
                calls.add((mod, methods[n.attr], n.attr))
    return owned, state, calls


def test_each_feature_owns_its_state():
    owned, _, _ = _ownership()
    for mod in {p.stem for p in FEATURES} - {"server_pairing"}:
        assert mod in owned.values(), f"{mod} has no _init_ owning any state"


def test_feature_state_stays_with_its_owner():
    _, state, _ = _ownership()
    undeclared = state - set(SHARED_STATE)
    assert not undeclared, (
        f"state used outside the feature that owns it: {sorted(undeclared)}; "
        f"ask the owner through a method, or declare it in SHARED_STATE with why")
    assert set(SHARED_STATE) <= state, f"stale SHARED_STATE: {sorted(set(SHARED_STATE) - state)}"


def test_every_crossing_between_features_is_declared():
    _, _, calls = _ownership()
    assert calls - FEATURE_CALLS == set(), (
        f"undeclared calls between features: {sorted(calls - FEATURE_CALLS)}")
    assert FEATURE_CALLS - calls == set(), (
        f"declared but gone: {sorted(FEATURE_CALLS - calls)}")


def test_each_cards_parts_stay_with_their_feature():
    crossings = set()
    for path in FEATURES:
        for n in ast.walk(ast.parse(path.read_text())):
            if (isinstance(n, ast.Attribute) and isinstance(n.value, ast.Attribute)
                    and n.value.attr in PART_OWNERS
                    and not (isinstance(n.value.value, ast.Name) and n.value.value.id == "self")
                    and PART_OWNERS[n.value.attr] != path.stem):
                crossings.add((path.stem, f"{n.value.attr}.{n.attr}"))
    assert not crossings, (
        f"a card's part used outside its feature: {sorted(crossings)}; "
        f"ask the owning feature through a method")
