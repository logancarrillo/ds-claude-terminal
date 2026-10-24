"""Test-suite guards that cannot be forgotten.

The production tmux socket is named by a default argument -- `serve.tmux_command`
takes `socket: str = TMUX_SOCKET` -- so a caller that simply omits the parameter
gets the live server that the shipped service runs on. A test that does this does
not fail; it succeeds against production and takes the session with it.

That is not hypothetical. tests/test_mode.py built its command lines on the
default socket, created and killed sessions there, and destroyed a running Claude
Code conversation. The agreed remedy was a habit: always name the socket. This
file is that habit made mechanical, because a habit protects only the people who
remember it and the default is still production.

Both routes are covered, since the damage was done by the second one:

  - `serve.tmux_command(...)` without an explicit socket.
  - A hand-built argv passed to Popen or run, which is how a Session starts its
    child and how the incident actually happened.
"""

import subprocess

import pytest

from dsterm import serve

PRODUCTION_SOCKET = serve.TMUX_SOCKET

_MESSAGE = (
    f"a test tried to drive the tmux socket {PRODUCTION_SOCKET!r}, which is the "
    "one the shipped service runs on. Pass an explicit test socket: "
    "tmux_command(..., socket=SOME_TEST_SOCKET). This guard exists because "
    "omitting it once destroyed a live Claude Code session."
)


def _names_production(argv) -> bool:
    """True for a tmux command line addressing the production socket.

    Matched positionally on the `-L name` pair rather than by searching for the
    bare string, so a session or window that happens to be called "dsterm" does
    not trip it.
    """
    try:
        parts = [str(a) for a in argv]
    except TypeError:
        return False
    if not parts or parts[0].rsplit("/", 1)[-1] != "tmux":
        return False
    for i, part in enumerate(parts[:-1]):
        if part == "-L":
            return parts[i + 1] == PRODUCTION_SOCKET
    # No -L at all is the default socket, which is also not ours to touch.
    return True


@pytest.fixture(autouse=True, scope="session")
def _never_touch_the_production_tmux_socket():
    real_popen, real_run = subprocess.Popen, subprocess.run

    class GuardedPopen(real_popen):
        def __init__(self, args, *a, **kw):
            if _names_production(args):
                raise RuntimeError(_MESSAGE)
            super().__init__(args, *a, **kw)

    def guarded_run(args, *a, **kw):
        if _names_production(args):
            raise RuntimeError(_MESSAGE)
        return real_run(args, *a, **kw)

    subprocess.Popen, subprocess.run = GuardedPopen, guarded_run
    try:
        yield
    finally:
        subprocess.Popen, subprocess.run = real_popen, real_run


@pytest.fixture(autouse=True)
def _no_personal_buttons(monkeypatch, tmp_path_factory):
    """The DO tab reads ~/.config/ds-claude-terminal/buttons; a developer's own
    buttons must not leak into the suite's expectations."""
    from dsterm import actions

    monkeypatch.setattr(actions, "USER_BUTTONS",
                        tmp_path_factory.mktemp("home") / "buttons")


@pytest.fixture(autouse=True)
def _no_real_speech_to_text(request, monkeypatch):
    """Servers built by the suite never find this machine's own speech-to-text.

    A developer with mlx-whisper or faster-whisper installed would otherwise
    have every test Server pick it up, and the suite builds hundreds: one
    worker process each, every one loading a model, crashed a Mac outright.
    tests/test_voice.py chooses its backends itself and is left alone."""
    if request.node.fspath.basename == "test_voice.py":
        return
    from dsterm import voice

    monkeypatch.setattr(voice, "choose_backend",
                        lambda env=None, cloud=False: (None, "off (tests)"))
