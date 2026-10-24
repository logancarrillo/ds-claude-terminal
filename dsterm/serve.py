#!/usr/bin/env python3
"""Run the DS terminal server.

    dsterm serve                          # serves `claude` in the current directory
    dsterm serve --port 8347 -- bash      # or anything else
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
from pathlib import Path

from dsterm import dashboard
from dsterm import voice
from dsterm.auth import DEFAULT_KEY_PATH, load_key
from dsterm.grid import COLS, ROWS
from dsterm.server import DEFAULT_PORT, Server
from dsterm.session import CHILD_TERM

# An API key for the child, kept beside the pairing key rather than in the
# environment of whichever shell started the server.
#
# The child inherits this process's environment, so exporting the variable in
# another terminal never reaches it, and exporting it in this one puts the key
# into shell history. Reading a file at spawn time also means the key survives
# both a server restart and a child restart without being retyped.
DEFAULT_API_KEY_PATH = Path.home() / ".config" / "ds-claude-terminal" / "anthropic-key"


# Anything else the child needs: a gateway base URL, a model override. Same
# directory, same permissions, KEY=VALUE lines with # comments.
DEFAULT_ENV_PATH = Path.home() / ".config" / "ds-claude-terminal" / "env"

# Snippets pushed to the handheld on connect: the server-side twin of the card's
# /dsterm.snippets. Edit here, reconnect, and they are there -- which is the
# entire point, because the card can only be written from the machine it is
# plugged into, and the card is usually in the console. The card file stays the
# durable store and the fallback; nothing here ever writes to the card.
DEFAULT_SNIPPETS_PATH = Path.home() / ".config" / "ds-claude-terminal" / "snippets"


def load_api_key(path: Path) -> str | None:
    """Read the API key file. Returns None when there is no such file."""
    if not path.exists():
        return None
    if path.stat().st_mode & 0o077:
        raise PermissionError(f"{path} is readable by others; chmod 600 it")
    key = path.read_text(encoding="utf-8").strip()
    if not key:
        raise ValueError(f"{path} is empty")
    return key


def load_env_file(path: Path) -> dict[str, str]:
    """Read KEY=VALUE lines. Returns {} when there is no such file.

    Values are taken literally: no shell expansion, no unescaping. A model name
    or a URL never needs it, and quietly rewriting a secret is worse than not
    supporting the syntax. Surrounding quotes are stripped, because typing them
    is the obvious mistake and a value that genuinely starts and ends with a
    quote does not occur here.
    """
    if not path.exists():
        return {}
    if path.stat().st_mode & 0o077:
        raise PermissionError(f"{path} is readable by others; chmod 600 it")

    out: dict[str, str] = {}
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise ValueError(f"{path}:{lineno}: expected KEY=VALUE, got {line!r}")
        name, _, value = line.partition("=")
        name = name.strip()
        if not name:
            raise ValueError(f"{path}:{lineno}: empty variable name")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        out[name] = value
    return out


# tmux config for the dedicated server. `status off` is not cosmetic: a status
# line would eat one of only 24 rows.
TMUX_SOCKET = "dsterm"
TMUX_CONF = Path(__file__).resolve().parent / "data" / "dsterm.tmux.conf"


def tmux_command(name: str, command: list[str], cols: int, rows: int,
                 socket: str = TMUX_SOCKET, conf: Path = TMUX_CONF) -> list[str]:
    """Wrap `command` so it runs inside a persistent tmux session.

    The point is that the session outlives everything around it: the DS
    disconnecting, this server restarting, and the child-restart path. Only the
    tmux server owns it, so a `dsterm restart` no longer throws away whatever
    Claude Code was in the middle of.

    -A attaches to the session if it exists and creates it otherwise, which is
    what makes reconnecting automatic rather than a separate step.

    The size is pinned at the handheld's geometry and `window-size manual` in the
    config stops tmux renegotiating it. That is the whole reason this is usable:
    tmux was rejected as a multiplexer earlier precisely because clients of
    different sizes negotiate, and a 42x24 client collapses the window for
    everyone attached. Pinning removes the negotiation -- a PC attaching sees a
    42x24 island in a larger terminal, which is the right trade when the
    handheld is the client that matters.

    A dedicated socket (-L) keeps this away from any other tmux server on the
    box, so nothing here can disturb an existing session.
    """
    return [
        "tmux", "-L", socket, "-f", str(conf),
        "new-session", "-A", "-s", name,
        "-x", str(cols), "-y", str(rows),
        *command,
    ]


def fit_session(name: str, cols: int, rows: int, socket: str = TMUX_SOCKET,
                runner=None) -> bool:
    """Make an existing session ``name`` exactly ``cols`` x ``rows``.

    tmux_command's -x/-y size only a session that new-session CREATES. With
    -A and a session already there it attaches instead, and -x/-y are
    ignored; `window-size manual` then holds every window at whatever size it
    had -- which is the point of it, until the size is what changed. So after
    `--grid 64x32` and a restart, the service attached to the old 42x24
    session and the 64x32 card was drawn a 42x24 island. Both halves matter:
    each window is resized (`resize-window` on its id, which also leaves it
    manual), and the session's default-size is set, because tmux sized every
    later window -- Ctrl-B c, a REMOTE pick -- from the one new-session
    recorded, not from the config.

    No session, or no tmux server, is not an error: new-session will make one
    at the right size. Returns whether a session was there to fit. Every
    command runs on the dedicated socket, never through a shell.
    """
    import subprocess

    def call(*args: str) -> subprocess.CompletedProcess | None:
        try:
            return (runner or subprocess.run)(
                ["tmux", "-L", socket, *args], capture_output=True, text=True,
                timeout=5)
        except (OSError, subprocess.SubprocessError):
            return None

    # "=name": the session called exactly that, never a prefix match of it.
    # set-option resolves -t as a pane, where a bare "=name" is looked up as
    # a window and fails (measured, tmux 3.7b: "no such session"); the colon
    # makes it the session part.
    listed = call("list-windows", "-t", f"={name}", "-F", "#{window_id}")
    if listed is None or listed.returncode != 0:
        return False
    size = f"{cols}x{rows}"
    call("set-option", "-t", f"={name}:", "default-size", size)
    for window_id in listed.stdout.split():
        if window_id.startswith("@") and window_id[1:].isdigit():
            call("resize-window", "-t", window_id, "-x", str(cols), "-y", str(rows))
    return True


def load_extensions() -> list:
    """Every extension installed beside this server, instantiated.

    Discovery by entry point rather than by import: the terminal ships
    with none, and a checkout that carries one declares it in its own
    pyproject.  A broken one is reported and skipped, never fatal -- the
    terminal is the thing that must start.
    """
    from importlib.metadata import entry_points
    found = []
    for ep in entry_points(group="dsterm.extensions"):
        try:
            found.append((ep.name, ep.load()()))
        except Exception as exc:  # noqa: BLE001 -- reported, not fatal
            print(f"  extension {ep.name}: not loaded ({exc})", file=sys.stderr)
    return found


def user_path(value: str) -> str:
    """A path option with ``~`` expanded.

    The shell expands ``~`` on a command line, but serve.conf is split by
    shlex, which does not: ``--cwd ~/code`` there reached the child as a
    directory literally named "~", and ``--key-file ~/...`` as a file that
    could never exist. Expanded here, so both roads arrive at the same path.
    """
    return os.path.expanduser(value)


def build_parser() -> argparse.ArgumentParser:
    """`dsterm serve`'s options. Also what dsterm.cli checks serve.conf
    against, so a line there is refused for exactly what this would not take.
    """
    p = argparse.ArgumentParser(prog="dsterm serve", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="0.0.0.0",
                   help="bind address; 0.0.0.0 is required for the DS to reach it")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--fps", type=float, default=15.0)
    # Never parsed here: dsterm.cli.apply_grid takes it off the command line
    # and turns it into DSTERM_COLS/DSTERM_ROWS before anything imports the
    # grid, which this module already has by the time it parses anything.
    # Declared so that --help lists it and serve.conf may name it.
    p.add_argument("--grid", metavar="COLSxROWS", default=None,
                   help=f"terminal size, matching the ROM on the card: 42x24 "
                        f"(the default build) or 64x32 (the smaller-font "
                        f"build). This server is {COLS}x{ROWS}")
    p.add_argument("--cwd", default=None, type=user_path,
                   help="working directory for the child")
    p.add_argument("--key-file", default=None, type=user_path)
    p.add_argument("--api-key-file", default=None, type=user_path,
                   help=f"file holding ANTHROPIC_API_KEY for the child "
                        f"(default: {DEFAULT_API_KEY_PATH})")
    p.add_argument("--env-file", default=None, type=user_path,
                   help=f"KEY=VALUE lines added to the child's environment "
                        f"(default: {DEFAULT_ENV_PATH})")
    p.add_argument("--snippets-file", default=None, type=user_path,
                   help=f"snippets pushed to the handheld on connect "
                        f"(default: {DEFAULT_SNIPPETS_PATH})")
    p.add_argument("--tmux", default=None, metavar="NAME",
                   help="run the child inside a persistent tmux session of this "
                        "name, on a dedicated socket, pinned to the DS geometry")
    p.add_argument("--dashboard-port", type=int, default=None, metavar="PORT",
                   help="serve a read-only status page on 127.0.0.1:PORT")
    # A flag rather than a key in --env-file, deliberately: that file is applied
    # to the CHILD's environment while this is read by this process, so a
    # setting put there would look correct and silently do nothing. Same trap
    # DSTERM_COLS cost two rounds to learn.
    p.add_argument("--clip", action="store_true",
                   help="push the Windows clipboard to a handheld that asked "
                        "for it (WSL only). OFF by default: every copy from "
                        "every machine synced to this one reaches the DS "
                        "(encrypted on the way, like everything else)")
    # On by default, unlike --clip: what it lists is this user's own Claude
    # sessions on this machine, to a card that has proved the key, and what a
    # pick can start is fixed here -- `claude attach`, `claude --resume ...
    # --fork-session`, or this server's own command, in its own tmux session.
    p.add_argument("--no-remote", action="store_true",
                   help="do not offer the handheld's REMOTE screen: never "
                        "list other sessions, never open a window for a pick. "
                        "STATE (what the session on screen is doing) still "
                        "reads `claude agents` for a card that asks for it")
    p.add_argument("--no-camera", action="store_true",
                   help="do not offer the DSi's camera: no CAMERA_READY, no "
                        "viewfinder feed, no photo saving")
    p.add_argument("--photo-dir", default=None, type=user_path,
                   help="save photos here instead of <project>/.dsterm/photos; "
                        "the card is sent the absolute path")
    p.add_argument("--photo-keep", type=int, default=20, metavar="N",
                   help="keep at most N saved photos per folder (1..1000); "
                        "a photo older than 7 days goes regardless")
    p.add_argument("--no-camera-popup", action="store_true",
                   help="do not raise the viewfinder popup on the other tmux "
                        "clients while the camera is on")
    p.add_argument("--no-mic", action="store_true",
                   help="do not offer the DS's Push-to-Talk microphone: no "
                        "MIC_READY, and any voice message drops the card")
    # A flag (or DSTERM_STT_CLOUD=1 in this process's environment), never the
    # env file: that file is the child's, and whether a recording leaves this
    # computer is decided by how the server was started.
    p.add_argument("--stt-cloud", action="store_true",
                   help="allow speech-to-text in the cloud (OpenAI, Groq or "
                        "DSTERM_STT_URL) when no local backend is installed. "
                        "OFF by default: without it a recording is never "
                        "uploaded, whatever API keys are set")
    p.add_argument("--project-ci", action="store_true",
                   help="show GitHub Actions runs on PROJECT -> WATCH, read "
                        "with `gh run list` (a network call every 30 s)")
    p.add_argument("--no-keep-awake", action="store_true",
                   help="let the computer (macOS, Linux) idle to sleep even while a handheld "
                        "is connected (by default its idle sleep is held off "
                        "until the last handheld leaves; a closed lid still "
                        "sleeps it)")
    p.add_argument("--discovery-port", type=int, default=8346, metavar="PORT",
                   help="answer the handheld's UDP discovery probe on this port "
                        "(0 turns it off). The answer is the hostname, the TCP "
                        "port and whether pairing is open -- nothing secret")
    p.add_argument("argv", nargs=argparse.REMAINDER, help="-- command to run (default: claude)")
    return p


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    if args.grid is not None and args.grid.lower() != f"{COLS}x{ROWS}":
        # Only reachable past dsterm.cli, which consumes --grid itself: by the
        # time anything parses it here the grid module was imported long ago,
        # so accepting it would run the old size while appearing to obey.
        print(f"error: --grid {args.grid} cannot take effect here (this process "
              f"is already {COLS}x{ROWS}); run `dsterm serve --grid {args.grid}`",
              file=sys.stderr)
        return 2

    logging.basicConfig(level=logging.INFO, format="%(asctime)s  %(message)s",
                        datefmt="%H:%M:%S")

    command = args.argv[1:] if args.argv and args.argv[0] == "--" else args.argv
    if not command:
        command = ["claude"]

    # Kept before it is wrapped: REMOTE's "+ New Claude session" opens this,
    # the child's own command, in a new window of the same tmux session.
    child_command = list(command)
    if args.tmux:
        command = tmux_command(args.tmux, command, COLS, ROWS)
        # Before the child attaches: a session that outlived a --grid change
        # is still at the old size, and attaching does not resize it.
        fit_session(args.tmux, COLS, ROWS)

    if not 1 <= args.photo_keep <= 1000:
        print("error: --photo-keep must be 1..1000", file=sys.stderr)
        return 2

    try:
        key = load_key(args.key_file)
    except (FileNotFoundError, PermissionError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        print(f"\nexpected key at {DEFAULT_KEY_PATH}", file=sys.stderr)
        return 1

    if args.host not in ("127.0.0.1", "localhost"):
        print("\n  This link is authenticated, encrypted and signed (docs/SEAL.md),")
        print("  but anyone on this network can still see that a DS is connected.")
        print("  Do not port-forward this; use a VPN to reach it from outside.\n")

    api_key_path = Path(args.api_key_file) if args.api_key_file else DEFAULT_API_KEY_PATH
    env_path = Path(args.env_file) if args.env_file else DEFAULT_ENV_PATH
    try:
        api_key = load_api_key(api_key_path)
        extra = load_env_file(env_path)
    except (PermissionError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    # A copy, not a mutation of this process's environment, and the file wins
    # over what was inherited. That matters: an ANTHROPIC_BASE_URL inherited
    # from the host would otherwise silently beat the gateway this is being
    # pointed at. TERM is the emulator's, not the inherited one -- a server
    # started from a TERM=dumb shell would otherwise hand that to Claude --
    # unless the env file says otherwise.
    env = dict(os.environ)
    env["TERM"] = CHILD_TERM
    if api_key is not None:
        env["ANTHROPIC_API_KEY"] = api_key
        # That one was found, never the value and never its length.
        print(f"  API key: {api_key_path}")
    for name, value in extra.items():
        env[name] = value
    # Names only. Values may be secrets and this goes to a log.
    if extra:
        print(f"  env: {env_path} -> {', '.join(sorted(extra))}")

    server = Server(command, key, host=args.host, port=args.port,
                    fps=args.fps, cwd=args.cwd, env=env,
                    clip_enabled=args.clip,
                    remote_enabled=not args.no_remote,
                    keep_awake=not args.no_keep_awake,
                    child_command=child_command,
                    discovery_port=args.discovery_port or None,
                    snippets_path=Path(args.snippets_file) if args.snippets_file else DEFAULT_SNIPPETS_PATH,
                    camera_enabled=not args.no_camera,
                    photo_dir=args.photo_dir,
                    photo_keep=args.photo_keep,
                    camera_popups=not args.no_camera_popup,
                    voice_enabled=not args.no_mic,
                    voice_cloud=voice.cloud_opted_in(args.stt_cloud),
                    project_ci=args.project_ci)
    # Where a DS's recording goes, said at every start: a local backend by
    # name, the cloud host when opted in, or off.
    print(f"  voice: {server.voice_where}")
    for name, extension in load_extensions():
        # Whatever is installed beside this server and registered under the
        # dsterm.extensions entry point attaches here. Named at startup, so
        # a log says what was speaking on the reserved type.
        server.register(extension)
        print(f"  extension: {name} ({extension.token})")
    if args.clip:
        # Said out loud at startup, next to the other warnings, because a
        # feature that widens where every copy goes should never be running
        # without the operator being reminded that it is.
        print("  clipboard bridge: ON -- every copy on this computer (and any "
              "synced to it) goes to the handheld")
    try:
        with server:
            server.warm_voice()
            if args.dashboard_port:
                # Started after the server so the page never shows a half-built
                # one, and inside the context manager so it dies with it.
                dashboard.start(server, args.dashboard_port)
                print(f"  dashboard: http://127.0.0.1:{args.dashboard_port}")
            server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
