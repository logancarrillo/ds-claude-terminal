"""dsterm -- the one command for the DS Claude Terminal server.

Installed by pipx (`dsterm`) or run from a checkout (`bin/dsterm`, which calls
this with the checkout's venv). Everything that used to be a remembered
incantation -- a venv path, a PYTHONPATH, a nohup, a log redirect -- lives
here, and the service owns the process.

    dsterm setup           pairing key, service, and what to do next
    dsterm pair            pair a DS: both screens show a code to compare
    dsterm doctor          everything that has to be true, checked, pasteable
    dsterm --help          the rest

Imports are lazy on purpose: `dsterm setup` has to run from a checkout before
the dependencies it installs exist.
"""

from __future__ import annotations

import filecmp
import getpass
import json
import math
import os
import platform as _platform
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

PACKAGE = Path(__file__).resolve().parent
DATA = PACKAGE / "data"
CONFIG_DIR = Path.home() / ".config" / "ds-claude-terminal"

UNIT = "dsterm.service"
LAUNCHD_LABEL = "dsterm.server"
TMUX_SOCKET = "dsterm"
TMUX_SESSION = "dsterm"
PORT = 8347
DISCOVERY_PORT = 8346
DASHBOARD_PORT = 8348
SERVE_ARGS = ["--tmux", "dsterm", "--dashboard-port", str(DASHBOARD_PORT)]


class Die(SystemExit):
    pass


def die(msg: str) -> "Die":
    print(f"dsterm: {msg}", file=sys.stderr)
    raise Die(1)


def note(msg: str) -> None:
    print(f"dsterm: {msg}", file=sys.stderr)


def run(argv: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(argv, **kw)


# -- where are we ---------------------------------------------------------------


def checkout() -> Path | None:
    """The repository this is running from, if it is a checkout with the DS
    client in it (a pipx install has neither)."""
    root = PACKAGE.parent
    if (root / "dsclient" / "Makefile").is_file():
        return root
    return None


def is_wsl() -> bool:
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


def service_manager() -> str | None:
    if sys.platform == "darwin":
        return "launchd"
    if Path("/run/systemd/system").is_dir():
        return "systemd"
    return None


def lan_addresses() -> list[str]:
    """This machine's IPv4 addresses a handheld could reach, primary first.
    The primary one is found by asking the routing table which source address
    it would use (a UDP connect sends nothing)."""
    found: list[str] = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.0.2.1", 9))
        found.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addr = info[4][0]
            if addr not in found:
                found.append(addr)
    except OSError:
        pass
    return [a for a in found if not a.startswith("127.")]


# -- extensions -----------------------------------------------------------------


@dataclass
class CardRun:
    """One `dsterm card`, as the core and any extension see it."""
    dest: Path
    rom: bool = True            # write the ROM this run (an extension may say no)
    eject: bool = True          # the safe default: a mounted card pulled is a corrupt one
    dirty: bool = False
    repo: Path | None = None
    extra: dict = field(default_factory=dict)


class CliExtension:
    """What an extension may add to this command. Registered under the
    `dsterm.cli` entry point group; every method is optional."""

    name = "extension"
    # name -> (one-line help, handler(argv) -> exit code)
    commands: dict[str, tuple[str, Callable[[list[str]], int]]] = {}
    # flag -> one-line help
    card_flags: dict[str, str] = {}

    def card_option(self, flag: str, card: CardRun) -> bool:
        return False

    def card_check(self, card: CardRun) -> None:
        pass

    def card_write(self, card: CardRun) -> None:
        pass


def extensions() -> list[CliExtension]:
    from importlib.metadata import entry_points

    # entry_points(group=) is 3.10+.  The checkout's front door may run on an
    # older system python3 (macOS ships 3.9) before setup has made a venv, and
    # `dsterm --help` must still answer there rather than crash.
    try:
        eps = entry_points(group="dsterm.cli")
    except TypeError:
        eps = entry_points().get("dsterm.cli", [])
    out = []
    for ep in eps:
        try:
            obj = ep.load()
            out.append(obj() if isinstance(obj, type) else obj)
        except Exception as exc:  # an extension must not take the CLI down
            note(f"extension {ep.name} did not load: {exc}")
    return out


# -- the service ----------------------------------------------------------------


def python_for_service() -> str:
    return sys.executable


def unit_text(workdir: Path) -> str:
    text = (DATA / "dsterm.service").read_text()
    return (text.replace("@PYTHON@", python_for_service())
                .replace("@WORKDIR@", str(workdir)))


def plist_path() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{LAUNCHD_LABEL}.plist"


def plist_text(workdir: Path) -> str:
    text = (DATA / "dsterm.plist").read_text()
    log = Path.home() / "Library" / "Logs" / "dsterm.log"
    return (text.replace("@PYTHON@", python_for_service())
                .replace("@WORKDIR@", str(workdir))
                .replace("@HOME@", str(Path.home()))
                .replace("@LOG@", str(log)))


def default_workdir() -> Path:
    """Where Claude Code runs. A checkout keeps its old behaviour; an install
    starts in the home directory, and --cwd says otherwise."""
    return checkout() or Path.home()


def cmd_install_service(argv: list[str], from_setup: bool = False) -> int:
    workdir = default_workdir()
    if argv[:1] == ["--cwd"] and len(argv) > 1:
        workdir = Path(argv[1]).expanduser().resolve()
    manager = service_manager()
    if manager == "systemd":
        d = Path.home() / ".config" / "systemd" / "user"
        d.mkdir(parents=True, exist_ok=True)
        (d / UNIT).write_text(unit_text(workdir))
        run(["systemctl", "--user", "daemon-reload"])
        run(["systemctl", "--user", "enable", UNIT])
        # Without lingering the service dies with the last WSL shell, so the
        # handheld would find nothing unless a terminal happened to be open.
        if run(["loginctl", "enable-linger", os.environ.get("USER", "")],
               capture_output=True).returncode != 0:
            note("could not enable lingering; the service will stop when your "
                 "last shell exits")
        print(f"installed {d / UNIT}")
    elif manager == "launchd":
        p = plist_path()
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(plist_text(workdir))
        print(f"installed {p}")
    else:
        print("no service manager here (WSL without systemd?): run "
              "`dsterm serve` in a terminal and leave it open")
        return 0
    link_cli()
    print(f"Claude Code will run in {workdir} (change with: dsterm "
          "install-service --cwd PATH)")
    if not from_setup:
        print("start it with: dsterm start")
    return 0


def link_cli() -> None:
    """From a checkout, put `dsterm` on PATH; pipx already has."""
    repo = checkout()
    if repo is None:
        return
    target = Path.home() / ".local" / "bin" / "dsterm"
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        if target.is_symlink() or target.exists():
            target.unlink()
        target.symlink_to(repo / "bin" / "dsterm")
    except OSError as exc:
        note(f"could not link {target}: {exc}")


def launchd_domain() -> str:
    return f"gui/{os.getuid()}"


def cmd_start(argv: list[str]) -> int:
    manager = service_manager()
    if manager == "systemd":
        run(["systemctl", "--user", "start", UNIT], check=False)
    elif manager == "launchd":
        if not plist_path().exists():
            die("not installed -- run: dsterm install-service")
        # bootstrap fails harmlessly if it is already loaded.
        run(["launchctl", "bootstrap", launchd_domain(), str(plist_path())],
            capture_output=True)
        run(["launchctl", "kickstart", f"{launchd_domain()}/{LAUNCHD_LABEL}"],
            capture_output=True)
    else:
        die("no service manager here; run: dsterm serve")
    return cmd_status([])


def cmd_stop(argv: list[str]) -> int:
    manager = service_manager()
    if manager == "systemd":
        return run(["systemctl", "--user", "stop", UNIT]).returncode
    if manager == "launchd":
        run(["launchctl", "bootout", f"{launchd_domain()}/{LAUNCHD_LABEL}"],
            capture_output=True)
        return 0
    die("no service manager here")
    return 1


def cmd_restart(argv: list[str]) -> int:
    if service_manager() == "systemd":
        run(["systemctl", "--user", "restart", UNIT])
        return cmd_status([])
    cmd_stop([])
    return cmd_start([])


def service_state() -> str:
    manager = service_manager()
    if manager == "systemd":
        r = run(["systemctl", "--user", "is-active", UNIT], capture_output=True, text=True)
        return r.stdout.strip() or "unknown"
    if manager == "launchd":
        r = run(["launchctl", "print", f"{launchd_domain()}/{LAUNCHD_LABEL}"],
                capture_output=True, text=True)
        if r.returncode != 0:
            return "not loaded"
        return "running" if "state = running" in r.stdout else "loaded"
    return "no service manager"


def probe_tcp(host: str = "127.0.0.1", port: int = PORT, timeout: float = 2.0) -> bool:
    """True if a dsterm server is listening: it sends a NONCE on accept."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            s.settimeout(timeout)
            head = s.recv(6)
            return len(head) >= 4 and head[:2] == b"DS" and head[3] == 0x81
    except OSError:
        return False


def probe_udp(host: str = "127.0.0.1", port: int = DISCOVERY_PORT,
              timeout: float = 1.0):
    from .discovery import decode_answer, encode_probe

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(encode_probe(), (host, port))
        return decode_answer(s.recvfrom(256)[0])
    except OSError:
        return None
    finally:
        s.close()


def handheld_peer() -> str | None:
    if shutil.which("ss"):
        r = run(["ss", "-tn"], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 5 and parts[3].endswith(f":{PORT}"):
                return parts[4]
    elif shutil.which("lsof"):
        r = run(["lsof", "-nP", f"-iTCP:{PORT}", "-sTCP:ESTABLISHED"],
                capture_output=True, text=True)
        for line in r.stdout.splitlines()[1:]:
            if "->" in line:
                return line.split("->")[-1].split()[0]
    return None


def tmux_size() -> str | None:
    if not shutil.which("tmux"):
        return None
    r = run(["tmux", "-L", TMUX_SOCKET, "display-message", "-p", "-t", TMUX_SESSION,
             "#{window_width}x#{window_height}"], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def cmd_status(argv: list[str]) -> int:
    from . import pairing

    print(f"service    {service_state()}")
    print(f"listening  {'0.0.0.0:%d' % PORT if probe_tcp() else 'no'}")
    print(f"handheld   {handheld_peer() or 'none connected'}")
    size = tmux_size()
    print(f"session    {TMUX_SESSION + ' (' + size + ')' if size else 'none'}")
    print(f"devices    {len(pairing.list_devices())} paired")
    window = pairing.current_window()
    if window:
        print(f"pairing    open for {int(window.remaining())} s")
    print(f"dashboard  http://127.0.0.1:{DASHBOARD_PORT}")
    return 0


def cmd_logs(argv: list[str]) -> int:
    manager = service_manager()
    if manager == "systemd":
        return run(["journalctl", "--user", "-u", UNIT, "--no-pager", *argv]).returncode
    log = Path.home() / "Library" / "Logs" / "dsterm.log"
    if manager == "launchd" and log.exists():
        return run(["tail", "-n", "200", *argv, str(log)]).returncode
    die("no service log here")
    return 1


# Options for `dsterm serve` without editing a service file: the service runs
# `dsterm serve` with a fixed command line (SERVE_ARGS), and this file is
# merged with it.  One or more options per line, as you would type them; `#`
# comments.  `--grid 64x32` matches the server to the smaller-font build of
# the ROM, and `-- claude --model sonnet` changes the command the session runs.
SERVE_CONF = CONFIG_DIR / "serve.conf"


def serve_conf_words(path: Path) -> list[tuple[int, str]]:
    """(line number, word) for every word in `path`, or none if it is absent.

    A line that does not parse is an error worth stopping for: a service that
    silently ignored half of its configuration would be a service doing
    something you did not ask for."""
    import shlex

    try:
        text = path.read_text()
    except OSError:
        return []
    out: list[tuple[int, str]] = []
    for n, line in enumerate(text.splitlines(), 1):
        try:
            words = shlex.split(line, comments=True)
        except ValueError as exc:
            die(f"{path}:{n}: {exc}")
        out.extend((n, w) for w in words)
    return out


def serve_options(path: Path) -> list[str]:
    """The options in `path`, or none."""
    return [w for _, w in serve_conf_words(path)]


def check_serve_conf(path: Path, located: list[tuple[int, str]], parser) -> None:
    """Refuse a serve.conf word, before its `--`, that `parser` would not
    read as an option or an option's value.

    Not pedantry. `dsterm serve` ends in a catch-all for the child command,
    and argparse starts it at the first bare word: a stray `KEY=VALUE` line
    (the env file's syntax, in the wrong file) silently became the command
    the session ran, swallowing every option after it -- the service's own
    `--tmux` and `--dashboard-port` included, so the session was no longer
    even in tmux.  Checked against the serve parser's own table, so the rule
    is exactly what it accepts: a known option (or an unambiguous prefix of
    one, as argparse allows), `--option=value`, and the one word after an
    option that takes a value."""
    takes_value: dict[str, bool] = {}
    for action in parser._actions:          # argparse's table; stable API in practice
        for flag in action.option_strings:
            takes_value[flag] = action.nargs != 0
    longs = [f for f in takes_value if f.startswith("--")]

    def resolve(word: str) -> str | None:
        if word in takes_value:
            return word
        hits = [f for f in longs if f.startswith(word)] if word.startswith("--") else []
        return hits[0] if len(hits) == 1 else None

    expect_value = False
    for n, word in located:
        if expect_value:
            expect_value = False
            continue
        if word == "--":
            return
        if word.startswith("-") and word != "-":
            flag, eq, _ = word.partition("=")
            known = resolve(flag)
            if known is None:
                die(f"{path}:{n}: unknown option {flag!r} -- `dsterm serve --help` "
                    "lists them")
            if eq and not takes_value[known]:
                die(f"{path}:{n}: {known} takes no value")
            expect_value = takes_value[known] and not eq
            continue
        die(f"{path}:{n}: {word!r} is not an option. Options start with --; "
            "a command for the session goes after `--` (`-- claude --model "
            "sonnet`); environment variables go in "
            f"{CONFIG_DIR / 'env'}")
    if expect_value:
        die(f"{path}: the last option has no value")


def merge_serve_args(from_file: list[str], given: list[str]) -> list[str]:
    """The given arguments first, then the file's, so the FILE wins where both
    set an option (argparse keeps the last).

    The given arguments are, in practice, the service's own fixed command line
    -- `--tmux dsterm --dashboard-port 8348` in a unit file nobody edits -- so
    they are defaults, and serve.conf is where a person overrides them.  With
    the file first, a `--dashboard-port` in it was silently beaten by the
    service's.  The child command is the exception: an explicit `-- command`
    on the real command line is a person running `dsterm serve` by hand to
    try something, and it beats the file's."""
    def split(args: list[str]) -> tuple[list[str], list[str] | None]:
        if "--" in args:
            i = args.index("--")
            return args[:i], args[i + 1:]
        return args, None

    file_opts, file_child = split(from_file)
    opts, child = split(given)
    merged = opts + file_opts
    child = child if child is not None else file_child
    return merged + (["--", *child] if child else [])


def apply_grid(args: list[str]) -> list[str]:
    """Turn `--grid COLSxROWS` into the environment the geometry is read from,
    before anything imports it."""
    out: list[str] = []
    it = iter(args)
    for a in it:
        if a == "--":
            out.append(a)
            out.extend(it)
            break
        if a == "--grid" or a.startswith("--grid="):
            value = a.partition("=")[2] if "=" in a else next(it, "")
            cols, _, rows = value.lower().partition("x")
            if not (cols.isdigit() and rows.isdigit()):
                die(f"--grid wants COLSxROWS, like 64x32 (got {value!r})")
            os.environ["DSTERM_COLS"], os.environ["DSTERM_ROWS"] = cols, rows
            continue
        out.append(a)
    return out


def cmd_serve(argv: list[str]) -> int:
    located = serve_conf_words(SERVE_CONF)
    args = apply_grid(merge_serve_args([w for _, w in located], argv))
    # Only now: importing .serve imports the grid, which reads the geometry
    # apply_grid has just set.
    from .serve import build_parser, main

    check_serve_conf(SERVE_CONF, located, build_parser())
    return main(args)


def cmd_screen(argv: list[str]) -> int:
    args = apply_grid(argv)
    from .peek import main

    return main(args)


# -- the camera feed -------------------------------------------------------------

FEED_SOCK = CONFIG_DIR / "run" / "camera.sock"


class _SockFeed:
    """One connection to the server's camera feed. poll() -> bytes, None on
    EOF, b"" on timeout -- the loop decides what to do with them."""

    def __init__(self, path: str):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # Connect first while still blocking: AF_UNIX connect is local and
        # instant, and a failure is the "no server" case the caller reports.
        sock.connect(path)
        sock.setblocking(False)
        self.sock = sock

    def poll(self, timeout: float) -> bytes | None:
        import select
        rl, _, _ = select.select([self.sock], [], [], timeout)
        if not rl:
            return b""
        try:
            return self.sock.recv(65536) or None
        except OSError:
            return None

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def _stdin_quit() -> bool:
    """Whether q or Esc has been pressed. False without a tty."""
    if not sys.stdin.isatty():
        return False
    import select
    rl, _, _ = select.select([sys.stdin], [], [], 0)
    if not rl:
        return False
    try:
        data = os.read(sys.stdin.fileno(), 8)
    except OSError:
        return False
    return b"q" in data or b"\x1b" in data


def camera_watch(sock_path: str, popup: bool, *,
                 feed_factory=_SockFeed, key_fn=_stdin_quit,
                 out=None, err=None, now=time.monotonic,
                 sleep=time.sleep, size_fn=None) -> int:
    """The viewfinder loop. Everything testable is injectable: feed_factory
    supplies the feed, key_fn the quit check, size_fn the terminal size."""
    import shutil

    from .camera import FeedReader, camera_view_lines

    out = out if out is not None else sys.stdout
    err = err if err is not None else sys.stderr
    size_fn = size_fn or shutil.get_terminal_size
    reader = FeedReader()
    feed = None
    off_since = None
    said_no_feed = False

    stdin = sys.stdin
    is_tty = stdin.isatty()
    old_term = None
    if is_tty:
        import termios
        import tty
        fd = stdin.fileno()
        old_term = termios.tcgetattr(fd)
        # Cbreak only: _stdin_quit selects before it reads, so the terminal
        # stays blocking -- a tty left O_NONBLOCK breaks whatever runs next.
        tty.setcbreak(fd)
    out.write("\x1b[?1049h\x1b[?25l")
    out.flush()
    try:
        while True:
            if feed is None:
                try:
                    feed = feed_factory(str(sock_path))
                except OSError:
                    if popup:
                        print("no camera feed: is dsterm serving on this "
                              "computer?", file=err)
                        return 1
                    if not said_no_feed:
                        said_no_feed = True
                        print("no camera feed: is dsterm serving on this "
                              "computer?", file=err)
                    sleep(1.0)
                    if key_fn():
                        return 0
                    continue
            data = feed.poll(0.15)
            if data is None:
                feed.close()
                feed = None
                continue
            if key_fn():
                return 0
            for header, rgb in reader.feed(data):
                size = size_fn()
                lines = camera_view_lines(header, rgb, size.columns, size.lines)
                out.write("\x1b[H" + "\x1b[K\n".join(lines) + "\x1b[J")
                out.flush()
                if header.get("on"):
                    off_since = None
                elif off_since is None:
                    off_since = now()
            if popup and off_since is not None and now() - off_since >= 1.0:
                return 0
    finally:
        out.write("\x1b[?25h\x1b[?1049l")
        out.flush()
        if old_term is not None:
            import termios
            termios.tcsetattr(stdin.fileno(), termios.TCSADRAIN, old_term)


def cmd_camera(argv: list[str]) -> int:
    import argparse
    p = argparse.ArgumentParser(
        prog="dsterm camera",
        description="The handheld's camera, live: the server's feed socket "
                    "carries the viewfinder to whoever holds it. "
                    "`dsterm serve` runs the feed; this only watches.")
    p.add_argument("--socket", default=str(FEED_SOCK), metavar="PATH",
                   help="the feed socket (default: %(default)s)")
    p.add_argument("--popup", action="store_true",
                   help="exit once the camera has been off for about a second "
                        "(the mode the tmux popup uses)")
    p.add_argument("--window", action="store_true",
                   help="show it in a window of its own, real pixels, instead "
                        "of in this terminal")
    args = p.parse_args(argv)
    if args.window:
        from . import camwindow
        return camwindow.run(Path(args.socket).parent.parent)
    return camera_watch(args.socket, args.popup)


def cmd_view(argv: list[str]) -> int:
    """Both of the DS's screens in this terminal, and its camera (viewer.py)."""
    if argv and argv[0] in ("-h", "--help"):
        print(command_help("view"))
        return 0
    if "--window" in argv:
        from . import viewwindow
        return viewwindow.run(CONFIG_DIR)
    from . import viewer
    return viewer.run(CONFIG_DIR)


def cmd_attach(argv: list[str]) -> int:
    size = tmux_size()
    if size is None:
        die("no session -- is the service running? try: dsterm status")
    print(f"attaching; the window stays {size}. detach with C-b d")
    os.execvp("tmux", ["tmux", "-L", TMUX_SOCKET, "attach", "-t", TMUX_SESSION])
    return 0


# -- the ROM --------------------------------------------------------------------


def toolchain_env() -> dict:
    """The toolchain lives outside the repo, and make fails confusingly
    without it ("cannot read spec file"), which looks like a missing toolchain
    rather than a missing variable. Its location differs between machines and
    neither is wrong, so look rather than assume, and say what was tried."""
    # BLOCKSDS set by hand, or BlocksDS's own default location, wins: that is
    # the layout of its installer and its Docker image, with the compiler
    # found through WONDERFUL_TOOLCHAIN or /opt/wonderful as the Makefile does.
    for core in filter(None, [os.environ.get("BLOCKSDS", ""), "/opt/blocksds/core"]):
        if (Path(core) / "sys").is_dir():
            env = dict(os.environ)
            env["BLOCKSDS"] = core
            wonderful = env.get("WONDERFUL_TOOLCHAIN", "/opt/wonderful")
            env.setdefault("WONDERFUL_TOOLCHAIN", wonderful)
            env["PATH"] = f"{wonderful}/toolchain/gcc-arm-none-eabi/bin:" + env.get("PATH", "")
            return env
    candidates = [os.environ.get("WONDERFUL_TOOLCHAIN", ""),
                  str(Path.home() / "wonderful"), "/opt/wonderful",
                  "/usr/local/wonderful"]
    for root in filter(None, candidates):
        if (Path(root) / "thirdparty" / "blocksds" / "core").is_dir():
            env = dict(os.environ)
            env["WONDERFUL_TOOLCHAIN"] = root
            env.setdefault("BLOCKSDS", f"{root}/thirdparty/blocksds/core")
            env.setdefault("BLOCKSDSEXT", f"{root}/thirdparty/blocksds/external")
            env["PATH"] = f"{root}/toolchain/gcc-arm-none-eabi/bin:" + env.get("PATH", "")
            return env
    tried = "\n".join(["  $BLOCKSDS, /opt/blocksds/core"] +
                      [f"  {c}/thirdparty/blocksds/core" for c in filter(None, candidates)])
    die("no BlocksDS toolchain found. Tried:\n" + tried +
        "\nInstall it (https://blocksds.skylyrac.net/docs/setup/), or set BLOCKSDS "
        "to its core directory")
    return {}


def need_checkout(what: str) -> Path:
    repo = checkout()
    if repo is None:
        die(f"{what} needs a checkout of the repository (this is an installed copy)")
    return repo  # type: ignore[return-value]


def cmd_build(argv: list[str]) -> int:
    repo = need_checkout("building the ROM")
    r = run(["make", "-C", str(repo / "dsclient"), *argv], env=toolchain_env())
    rom = repo / "dsclient" / "dsterm.nds"
    if rom.exists():
        print(f"{rom}  {rom.stat().st_size} bytes")
    return r.returncode


def cmd_test(argv: list[str]) -> int:
    repo = need_checkout("the test suites")
    env = dict(os.environ, PYTHONPATH=str(repo))
    r1 = run([sys.executable, "-m", "pytest", "tests/", "-q", *argv], cwd=repo, env=env)
    r2 = run(["bash", "dsclient/run_tests.sh"], cwd=repo)
    return r1.returncode or r2.returncode


# -- the card -------------------------------------------------------------------
#
# `dsterm card` is the whole handheld half in one command: the ROM, whatever an
# extension adds beside it, optionally the Wi-Fi settings and a paired server,
# verification of all of it, and an eject.
#
# It exists because the manual version was seven steps and one of them was
# always skipped. Rules it will not break:
#
#   * A config on the card is never written over without being asked for by
#     name (--wifi, --pair), and the legacy dsterm.cfg never at all. It holds
#     the Wi-Fi passphrase and the pairing key and is not recoverable.
#   * Nothing on the card is deleted. The card also carries TWiLight, the
#     launcher, and a NAND backup, none of which are in any checkout.
#   * Nothing is written until everything has been checked, and everything
#     written is read back and compared after a sync.
#   * The card is never a git working tree: a git clean on one deletes the
#     boot installation and spares the ignored config.


def eject(dest: Path) -> None:
    os.sync()
    if shutil.which("diskutil"):
        # The disk id is re-derived here rather than remembered: unmounting
        # the wrong disk because an identifier went stale is not worth a lookup.
        r = run(["df", str(dest)], capture_output=True, text=True)
        lines = r.stdout.splitlines()
        if len(lines) < 2:
            note(f"could not find the disk for {dest}; eject it yourself")
            return
        node = lines[1].split()[0]
        if run(["diskutil", "unmountDisk", node]).returncode != 0 and \
           run(["diskutil", "unmountDisk", "force", node]).returncode != 0:
            note(f"could not unmount {node}; eject it yourself")
    elif shutil.which("udisksctl"):
        r = run(["df", str(dest)], capture_output=True, text=True)
        node = r.stdout.splitlines()[1].split()[0]
        if run(["udisksctl", "unmount", "-b", node]).returncode != 0:
            note(f"could not unmount {dest}; eject it yourself")
    else:
        note(f"no eject tool here; unmount {dest} yourself before pulling it")


def copy_verified(src: Path, dst: Path, what: str) -> None:
    shutil.copyfile(src, dst)
    os.sync()
    if not filecmp.cmp(src, dst, shallow=False):
        die(f"{what} on the card does not match -- do not boot it")


def write_verified(path: Path, text: str, what: str) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text)
    os.replace(tmp, path)
    os.sync()
    if path.read_text() != text:
        die(f"{what} on the card did not read back -- do not boot it")


def wifi_text(ssid: str, passphrase: str) -> str:
    return ("# DS Claude Terminal: the Wi-Fi network.\n"
            "# Written by `dsterm card --wifi`; safe to edit.\n"
            f"ssid = {ssid}\nwifi_key = {passphrase}\nip = dhcp\n")


def server_text(name: str, host: str, port: int, key: bytes) -> str:
    return ("# DS Claude Terminal: one paired server.\n"
            "# The psk is this console's key for it; keep this file on the card.\n"
            f"name = {name}\nhost = {host}\nport = {port}\npsk = {key.hex()}\n")


def ask_wifi() -> tuple[str, str]:
    ssid = input("Wi-Fi network name (SSID, 2.4 GHz): ").strip()
    if not ssid or len(ssid.encode()) > 32:
        die("an SSID is 1 to 32 bytes")
    prompt = "Wi-Fi password (empty for an open network): "
    if sys.stdin.isatty():
        passphrase = getpass.getpass(prompt)
    else:
        # Piped in -- a script, or a test. getpass reads /dev/tty, not stdin,
        # so with a controlling terminal it waited forever at a prompt nobody
        # could see for a password that was sitting in the pipe.
        print(prompt, end="", flush=True, file=sys.stderr)
        passphrase = sys.stdin.readline().rstrip("\n")
    if passphrase and not 8 <= len(passphrase) <= 63:
        die("a WPA2 password is 8 to 63 characters")
    if passphrase != passphrase.strip():
        die("a password that starts or ends with a space cannot be stored in "
            "the card's file; use the console's own Wi-Fi menu instead")
    return ssid, passphrase


def cmd_card(argv: list[str]) -> int:
    from . import pairing

    exts = extensions()
    card = CardRun(dest=Path(), repo=checkout())
    dest: str | None = None
    rom_file: str | None = None
    want_wifi = want_pair = False
    host_override: str | None = None
    sounds_dir: str | None = None
    lang_files: list[str] = []
    args = list(argv)
    while args:
        a = args.pop(0)
        if a == "--dirty":
            card.dirty = True
        elif a == "--no-eject":
            card.eject = False
        elif a == "--wifi":
            want_wifi = True
        elif a == "--pair":
            want_pair = True
        elif a == "--rom":
            if not args:
                die("--rom needs a file")
            rom_file = args.pop(0)
        elif a == "--host":
            if not args:
                die("--host needs an address")
            host_override = args.pop(0)
        elif a == "--sounds":
            if not args:
                die("--sounds needs a folder of WAV files")
            sounds_dir = args.pop(0)
        elif a == "--lang":
            if not args:
                die("--lang needs a .lang file")
            lang_files.append(args.pop(0))
        elif a.startswith("-"):
            if not any(e.card_option(a, card) for e in exts):
                die(f"unknown option '{a}' -- try: dsterm --help")
        else:
            if dest is not None:
                die("one card path, not two")
            dest = a
    if dest is None:
        die("usage: dsterm card /Volumes/SDCARD [--wifi] [--pair] [--rom FILE] "
            "[--sounds FOLDER] [--dirty] [--no-eject]")
    card.dest = Path(dest)
    if not card.dest.is_dir():
        die(f"{dest} is not mounted")
    if (card.dest / ".git").exists():
        die(f"{dest} is a git working tree -- remove {dest}/.git first. The card "
            "is written only by this command, never by git.")

    # ---- check everything before writing anything ----

    rom: Path | None = None
    built_here = False
    if card.rom:
        if rom_file:
            rom = Path(rom_file)
            if not rom.is_file():
                die(f"no ROM at {rom}")
        else:
            repo = card.repo
            if repo is None and (want_wifi or want_pair or sounds_dir or lang_files):
                # Settings or sounds alone: the ROM already on the card stays.
                note("no --rom given: the app already on the card is left as it is")
                card.rom = False
                repo = None
            elif repo is None:
                die("no ROM to write: pass --rom dsterm.nds (from a release), "
                    "or run this from a checkout to build one")
            if repo is not None:
                # Build if the ROM is missing or older than any source: make
                # knows, and a flash of yesterday's build nobody notices is
                # the expensive outcome.
                if run(["make", "-C", str(repo / "dsclient")], env=toolchain_env(),
                       stdout=subprocess.DEVNULL).returncode != 0:
                    die("the build failed -- nothing was written to the card")
                rom = repo / "dsclient" / "dsterm.nds"
                if not rom.is_file():
                    die(f"no ROM at {rom} after a successful build")
                built_here = True

    # The dirty gate: the flash log stamps HEAD, and a dirty tree makes that
    # attribution false -- it names a commit that cannot have produced these
    # bytes. It applies exactly when a log line is written.
    # A flash log is kept only where one exists already: it is a record for
    # whoever keeps it, and its rule -- commit first, so the line can name
    # the commit -- is no rule for someone trying out a tile they just edited.
    flash_log = card.repo / "docs" / "flash-log.md" if card.repo else None
    keeps_log = flash_log is not None and flash_log.is_file()
    dirty_note = ""
    if built_here and keeps_log:
        st = run(["git", "-C", str(card.repo), "status", "--porcelain", "--", "dsclient"],
                 capture_output=True, text=True).stdout.strip()
        if st:
            if not card.dirty:
                die("dsclient/ has uncommitted changes. Commit first, so the "
                    "flash log can name the commit that actually produced these "
                    "bytes. To flash anyway -- which is reasonable while "
                    "iterating -- pass --dirty and the log will say the "
                    "attribution is not to be trusted.")
            dirty_note = " [DIRTY TREE — the commit named did not produce these bytes]"

    for e in exts:
        e.card_check(card)

    root = card.dest / "dsterm"
    legacy = card.dest / "dsterm.cfg"
    wifi_file = root / "wifi.cfg"

    # Your own sounds: checked by the card's own rules before anything is
    # written, so a file it would skip is refused here, with the reason.
    # The card's own files count too: the budget is spent in card order across
    # what is there and what is coming, and a cue being replaced must not keep
    # an old take the new folder does not have (sounds.plan_card).
    sound_plan: list[Path] = []
    sound_aside: list[Path] = []
    sounds_target = root / "sounds"
    lang_plan: list[Path] = []
    for lf in lang_files:
        lp = Path(lf).expanduser()
        if not lp.is_file():
            die(f"no language file at {lf}")
        size = lp.stat().st_size
        if size > LANG_FILE_MAX:
            die(f"{lf} is {size} bytes, and the DS reads at most 8 KB of a "
                f".lang file ({LANG_FILE_MAX} bytes): it would refuse this one")
        lang_plan.append(lp)
    if sounds_dir is not None:
        from . import sounds

        folder = Path(sounds_dir).expanduser()
        if not folder.is_dir():
            die(f"{sounds_dir} is not a folder")
        on_card = (sorted(p for p in sounds_target.iterdir() if p.is_file())
                   if sounds_target.is_dir() else [])
        sound_plan, sound_aside, problems = sounds.plan_card(folder, on_card)
        for line in problems:
            note(f"sound skipped: {line}")
        if not sound_plan:
            die(f"no usable sounds in {sounds_dir} -- `dsterm sounds` lists the "
                "names and the format")

    wifi_plan: str | None = None
    if want_wifi:
        ssid, passphrase = ask_wifi()
        wifi_plan = wifi_text(ssid, passphrase)

    pair_plan: tuple[str, str, bytes, Path, str] | None = None
    if want_pair:
        addrs = lan_addresses()
        host = host_override or (addrs[0] if addrs else None)
        if not host:
            die("cannot tell this machine's LAN address; pass --host ADDRESS")
        if is_wsl() and host.startswith("172.") and not host_override:
            note(f"{host} looks like WSL's NAT address, which the console cannot "
                 "reach. Pass --host with the Windows machine's LAN address and "
                 "run: dsterm wsl-setup")
        id_file = root / "id"
        device_id = ""
        if id_file.exists():
            device_id = id_file.read_text().strip()
        if not pairing.is_device_id(device_id):
            device_id = pairing.new_device_id()
        name = pairing.server_name()
        key = os.urandom(16)
        pair_plan = (device_id, host, key, root / "servers" / f"{name}.cfg", name)

    cfg_before = legacy.read_bytes() if legacy.exists() else None
    if cfg_before is None and not wifi_file.exists() and wifi_plan is None:
        note(f"no Wi-Fi settings on {dest}: the console will open its setup "
             "menu at boot, which is fine. Pass --wifi to set them now instead.")

    # ---- write ----

    if card.rom and rom is not None:
        running = card.dest / "dsterm.nds"
        # Back up whatever is running before replacing it.
        if running.exists() and not filecmp.cmp(rom, running, shallow=False):
            backups = card.dest / "dsterm" / "backups"
            backups.mkdir(parents=True, exist_ok=True)
            stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
            backup = backups / f"dsterm.nds.{stamp}"
            copy_verified(running, backup, "the backup of the running ROM")
            print(f"backed up the running ROM to dsterm/backups/{backup.name}")
        copy_verified(rom, running, "the ROM")
        print(f"flashed {rom.stat().st_size} bytes to {running}, verified")

    for e in exts:
        e.card_write(card)

    if sound_plan:
        target = sounds_target
        target.mkdir(parents=True, exist_ok=True)
        if sound_aside:
            # Moved, not deleted: this command never deletes anything on the
            # card, and a sound someone made is theirs to throw away. The card
            # reads only /dsterm/sounds/<cue>*.wav, so old/ is silent.
            aside = target / "old"
            aside.mkdir(exist_ok=True)
            for src in sound_aside:
                dst = aside / src.name
                if dst.exists():
                    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
                    dst = aside / f"{src.stem}.{stamp}{src.suffix}"
                    n = 1
                    while dst.exists():
                        dst = aside / f"{src.stem}.{stamp}-{n}{src.suffix}"
                        n += 1
                src.rename(dst)
                if src.exists() or not dst.is_file():
                    die(f"moving {src.name} into dsterm/sounds/old/ did not take "
                        "-- do not boot until it is out of dsterm/sounds/")
            os.sync()
            print(f"sounds: moved {', '.join(p.name for p in sound_aside)} to "
                  "dsterm/sounds/old/ -- the new folder replaces those cues, "
                  "and nothing was deleted")
        for src in sound_plan:
            copy_verified(src, target / src.name.lower(), src.name)
        print(f"sounds: {len(sound_plan)} copied to dsterm/sounds/, verified; "
              "they play from the next boot, with SOUND set to SOFT on the DS")

    if wifi_plan is not None:
        root.mkdir(exist_ok=True)
        if wifi_file.exists():
            shutil.copyfile(wifi_file, wifi_file.with_name("wifi.cfg.bak"))
        write_verified(wifi_file, wifi_plan, "wifi.cfg")
        print("Wi-Fi settings written to dsterm/wifi.cfg (DHCP)")

    if lang_plan:
        lang_dir = root / "lang"
        lang_dir.mkdir(parents=True, exist_ok=True)
        for src in lang_plan:
            name = src.name if src.name.endswith(".lang") else f"{src.stem}.lang"
            copy_verified(src, lang_dir / name.lower(), name)
        print(f"lang: {len(lang_plan)} copied to dsterm/lang/, verified; "
              "pick it under SETTINGS -> SCREEN -> LANGUAGE on the DS")
        # Counted on the card, after the copy: files already there take
        # slots too, and the DS loads the first LANG_CUSTOM_MAX it finds.
        on_card = sorted(p.name for p in lang_dir.iterdir()
                         if p.is_file() and p.name.lower().endswith(".lang"))
        if len(on_card) > LANG_CUSTOM_MAX:
            note(f"dsterm/lang/ holds {len(on_card)} .lang files, and the DS "
                 f"loads at most {LANG_CUSTOM_MAX} (the first it finds); "
                 "remove the ones you do not use: " + ", ".join(on_card))

    if pair_plan is not None:
        device_id, host, key, path, name = pair_plan
        path.parent.mkdir(parents=True, exist_ok=True)
        write_verified(path, server_text(name, host, PORT, key), path.name)
        write_verified(root / "id", device_id + "\n", "the device id")
        write_verified(root / "last", name + "\n", "the last-server file")
        # The server side only after the card side verified: a key the card
        # does not hold is a device that can never authenticate.
        pairing.save_device(device_id, key, name="paired by dsterm card",
                            address="")
        print(f"paired: device {device_id} -> {name} at {host}:{PORT}")

    # ---- the legacy config must not have moved ----

    if cfg_before is not None:
        if not legacy.exists():
            die("dsterm.cfg vanished during the write -- restore it from your "
                "notes before booting")
        if legacy.read_bytes() != cfg_before:
            die("dsterm.cfg changed during the write. Nothing here should have "
                "touched it; do not boot until you know why.")
        print("dsterm.cfg untouched")

    # A flash is an experiment: an append-only attribution record ties a
    # photograph taken weeks later to an exact verified ROM.
    if built_here and keeps_log and rom is not None:
        head = run(["git", "-C", str(card.repo), "rev-parse", "HEAD"],
                   capture_output=True, text=True).stdout.strip()
        subject = run(["git", "-C", str(card.repo), "log", "-1", "--format=%s"],
                      capture_output=True, text=True).stdout.strip()
        stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        with open(flash_log, "a") as fh:
            fh.write(f"{stamp} {head} {rom.stat().st_size} bytes verified — "
                     f"{subject}{dirty_note}\n")

    if card.eject:
        eject(card.dest)
    return 0


def cmd_sounds(argv: list[str]) -> int:
    from . import sounds

    print(sounds.usage())
    return 0


# The ROM's limits on a custom language (LANG_FILE_MAX and LANG_CUSTOM_MAX in
# dsclient/source/lang.h): a .lang file over 8 KB is refused whole, and only
# the first two .lang files in dsterm/lang/ are loaded.
LANG_FILE_MAX = 8192
LANG_CUSTOM_MAX = 2

LANG_TEMPLATE = """\
# DS Claude Terminal custom UI language pack
# Copy to /dsterm/lang/<code>.lang on the SD card (or run: dsterm card PATH --lang FILE.lang)
# Any key left out, or misspelled, shows in English: translate as much or as
# little as you like. The guide's longer explanations (guide.do.*) are left
# to English here. At most 8 KB; %s is filled in by the DS, keep it.
code = fr
label = FR
name = Français

control.keys = CLAVIER
control.keys.sub = en direct
control.quick = RAPIDE
control.quick.sub = mode, annuler
control.photo = PHOTO
control.photo.sub = montrer
control.again = REFAIRE
control.again.sub = récents
control.compose = RÉDIGER
control.compose.sub = écrire d'abord
control.remote = DISTANT
control.remote.sub = sessions
control.settings = RÉGLAGES
control.settings.sub = son, modèle
control.relink = RELIER
control.hint.scroll = haut/bas défile  maintenir bas: direct
control.hint.focus = croix déplace  A ouvre  B retour
compose.head = RÉDIGER: rien n'est envoyé avant d'appuyer sur A
compose.keys = A envoie   B efface   X annule
compose.focus.head = FOCUS: la croix choisit, A écrit
compose.focus.keys = START envoie  B efface  X sort du focus
compose.mic.idle = R / TOUCHER: maintenir pour dicter (voix)
compose.mic.rec = ÉCOUTE... relâcher R pour transcrire
compose.mic.latch = ENREGISTREMENT -- R ou L arrête
compose.mic.wait = Transcription vocale sur le serveur...
keys.floor.0 = Chaque touche va directement à Claude.
keys.floor.1 = SHFT: abc ABC !@#   X: retour
quick.head = RAPIDE   HAUT/BAS choisir   A faire   X retour
quick.sub = COMMANDS et AGAIN ouvrent l'éditeur
remote.head = DISTANT   HAUT/BAS choisir   A ouvrir   X retour
project.hint = toucher un onglet   X RETOUR
project.do.safe = toucher charge; A envoie
project.do.god = MODE DIEU: toucher exécute immédiatement
status.paste = CHARGER DANS L'ÉDITEUR
status.nopaste = RIEN À COLLER
chime.idle = Claude est prêt
chime.working = Claude travaille...
chime.approval = Claude attend votre accord
chime.question = Claude pose une question

guide.title = COMMANDES
guide.intro = touchez un bouton ou un combo; rien n'est envoyé
guide.press = appuyez sur un bouton
guide.tabs.hint = GAUCHE/DROITE onglets
guide.foot = sortir: B maintenu, FINI ou SELECT+START
guide.done = FINI
guide.tab.control = CONTRÔLE
guide.tab.keys = TOUCHES
guide.tab.compose = RÉDIGER
guide.tab.focus = FOCUS
guide.tab.camera = CAMÉRA
guide.tab.quick = RAPIDE
guide.combo.lr = L puis R
guide.combo.rl = R puis L
guide.combo.holdr = maintenir R
guide.combo.rthen = R, puis %s
guide.combo.dpad = croix
guide.name.screens = ÉCRANS
guide.name.scroll = DÉFILER
guide.name.arrows = FLÈCHES
guide.name.ctrl = CTRL
guide.name.shift = MAJ
guide.name.mode = MODE
guide.name.focus = FOCUS
guide.name.back = RETOUR
guide.name.send = ENVOYER
guide.name.delete = EFFACER
guide.name.cancel = ANNULER
guide.name.snippets = EXTRAITS
guide.name.nothing = INUTILISÉ
guide.name.talk = PARLER
guide.name.latch = VERROU
guide.name.order = PAS FOCUS
guide.name.type = ÉCRIRE
guide.name.move = BOUGER
guide.name.shutter = PHOTO
guide.name.flip = INVERSER
guide.name.open = OUVRIR
guide.name.forget = OUBLIER
guide.name.close = FERMER
"""


def cmd_lang(argv: list[str]) -> int:
    for a in argv:
        if a != "--template":
            die(f"unknown argument '{a}' -- usage: dsterm lang [--template]")
    if argv:
        print(LANG_TEMPLATE, end="")
        return 0
    # The ROM's list (dsclient/source/lang.h).
    print("Built-in UI languages on the DS (SETTINGS -> SCREEN -> LANGUAGE):")
    print("  en   EN   English (default)")
    print("  es   ES   Español")
    print("  zh   ZH   中文 (Simplified Chinese)")
    print("  ja   JA   日本語")
    print()
    print("Import any custom language:")
    print("  dsterm lang --template > fr.lang")
    print("  dsterm card /Volumes/SDCARD --lang fr.lang")
    return 0


# Written beside timer.json the first time `dsterm timer` creates it, so the
# file the timer leaves in somebody's repository is ignored by git rather than
# showing up as untracked. It names only itself and the timer: a watch.json
# the person wrote, or anything else in .dsterm/, is theirs to commit or not.
TIMER_GITIGNORE = "# written by `dsterm timer`\n/.gitignore\n/timer.json\n"


def cmd_timer(argv: list[str]) -> int:
    """A focus timer on PROJECT -> WATCH: .dsterm/timer.json at the top of
    the repository (where the server's PROJECT page reads it), or in the
    current directory outside one."""
    try:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                             capture_output=True, text=True, timeout=3,
                             check=False).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        top = ""
    timer_file = Path(top or Path.cwd()) / ".dsterm" / "timer.json"
    if not argv or argv[0] == "status":
        if not timer_file.is_file():
            print("timer: off (start with: dsterm timer 25)")
            return 0
        try:
            data = json.loads(timer_file.read_text(encoding="utf-8"))
            # Valid JSON of another shape ([], null) is as unreadable as a
            # torn file; so is an end_ts of NaN, or of 1e400 (an infinity,
            # which int() refuses with OverflowError).
            if not isinstance(data, dict):
                raise ValueError("not an object")
            label = str(data.get("label", "focus")) or "focus"
            left = float(data.get("end_ts", 0)) - time.time()
            if left != left:
                raise ValueError("end_ts is not a number")
            if left <= 0:
                print(f"timer: {label} -- done")
            else:
                # Rounded up: "0m 00s remaining" is a timer that is done.
                rem = math.ceil(left)
                print(f"timer: {label} -- {rem // 60}m {rem % 60:02d}s remaining")
        except (OSError, ValueError, TypeError, OverflowError):
            print("timer: off")
        return 0
    if argv[0] in ("stop", "clear", "off"):
        try:
            timer_file.unlink()
        except FileNotFoundError:
            pass
        print("timer: stopped")
        return 0
    try:
        minutes = float(argv[0])
    except ValueError:
        die("usage: dsterm timer [MINUTES [LABEL] | stop | status]")
    # Written as the message says it: 1 to 240, and a NaN (which compares
    # false to everything) is refused by the same test.
    if not 1 <= minutes <= 240:
        die("timer minutes must be between 1 and 240")
    # The rest of the line is the label: `dsterm timer 25 deep work` is
    # "deep work", not "deep".
    label = " ".join(argv[1:]).strip() or "focus"
    timer_file.parent.mkdir(parents=True, exist_ok=True)
    ignore = timer_file.parent / ".gitignore"
    if not ignore.exists():
        ignore.write_text(TIMER_GITIGNORE, encoding="utf-8")
    duration_s = minutes * 60.0
    payload = {
        "label": label[:12],
        "duration_s": duration_s,
        "end_ts": time.time() + duration_s,
    }
    timer_file.write_text(json.dumps(payload), encoding="utf-8")
    print(f"timer: started {minutes:g}m ({label[:12]}) -- shown on PROJECT -> WATCH")
    print(f"  (kept in {timer_file}; git ignores it through .dsterm/.gitignore "
          "unless that file was already yours)")
    return 0


def cmd_install_card(argv: list[str]) -> int:
    """The older, narrower name: the ROM only, through the same verified path."""
    if not argv:
        die("usage: dsterm install-card /Volumes/SDCARD [--dirty]")
    return cmd_card([argv[0], "--no-eject", *argv[1:]])


# -- setup, pairing, devices ------------------------------------------------------


def cmd_setup(argv: list[str]) -> int:
    repo = checkout()
    in_venv = repo is not None and \
        Path(sys.prefix).resolve() == (repo / ".venv").resolve()
    if repo is not None and "--continue" not in argv:
        # A checkout: (re)install into its venv every time, editable, so a
        # pull that adds a dependency or an entry point takes effect.
        venv = repo / ".venv"
        if not in_venv and sys.version_info < (3, 11):
            die(f"this is Python {_platform.python_version()}; 3.11 or newer is "
                "needed -- run with a newer python3 on PATH")
        if not venv.exists():
            run([sys.executable, "-m", "venv", str(venv)], check=True)
            print(f"venv: created {venv}")
        py = venv / "bin" / "python"
        run([str(py), "-m", "pip", "install", "--quiet", "--upgrade", "pip"], check=True)
        if run([str(py), "-m", "pip", "install", "--quiet", "-e", f"{repo}[dev]"]).returncode:
            die("could not install dsterm and its dependencies")
        print("deps: installed from pyproject.toml")
        # Continue inside the venv, so the service is pointed at it.
        os.execv(str(py), [str(py), "-m", "dsterm.cli", "setup", "--continue", *argv])
    argv = [a for a in argv if a != "--continue"]

    # The pairing key, through the project's own generator, which refuses to
    # overwrite: regenerating one silently stops every card authenticating.
    # Only now: dsterm.auth imports the grid, and the grid needs pyte, which
    # a fresh clone does not have until the venv above installed it.
    from . import auth

    if auth.DEFAULT_KEY_PATH.exists():
        print(f"key: {auth.DEFAULT_KEY_PATH} already exists, left alone")
    else:
        from .keygen import main as keygen

        keygen(quiet=True)
    os.chmod(CONFIG_DIR, 0o700)

    started = False
    if service_manager():
        cmd_install_service(argv, from_setup=True)
        # Started (or restarted, so a re-run picks up a new version) here
        # rather than left as a next step: "installed but not running" is the
        # one state that looks finished and makes the console find nothing.
        print()
        cmd_restart([])
        started = service_state() in ("active", "running")
    else:
        print("service: no systemd here (WSL without systemd?); run `dsterm "
              "serve` in a terminal instead")
        link_cli()

    print("\nnext:")
    if not started:
        print("  dsterm start          start the server")
    if is_wsl():
        print("  dsterm wsl-setup      let the handheld reach WSL (Windows firewall)")
    print("  dsterm pair           then pick this computer in the console's menu;")
    print("                        both screens show a code to compare")
    addrs = lan_addresses()
    if addrs:
        print(f"\nthis machine is {', '.join(addrs)} on the network")
    return 0


def ask_code_matches(code_text: str) -> bool:
    """Ask the person at the computer whether the DS shows the same code.
    Only an explicit yes counts."""
    print()
    print("  The DS shows a code. This computer's is:")
    print()
    print(f"        {code_text[:3]} {code_text[3:]}")
    print()
    try:
        answer = input("  Does the DS show the same code? [y/N] ")
    except EOFError:
        return False
    return answer.strip().lower() in ("y", "yes")


def cmd_pair(argv: list[str]) -> int:
    from . import pairing

    seconds = pairing.WINDOW_SECONDS
    if argv[:1] == ["--seconds"] and len(argv) > 1:
        seconds = float(argv[1])
    if not sys.stdin.isatty():
        # The comparison is the security: a code nobody looks at proves
        # nothing.  A card can still be set up with no network at all.
        die("dsterm pair asks you to compare a code, so it needs a terminal. "
            "To pair without one: dsterm card PATH --pair")
    before = {d.device_id for d in pairing.list_devices()}
    pairing.open_window(seconds=seconds)
    print()
    print("  Pairing is open. On the DS: Find servers (or Enter an address),")
    print("  pick this computer, and choose Pair. Both screens will then show")
    print("  a six-digit code to compare.")
    print()
    if probe_tcp() is False:
        note("no server is answering on this machine -- start it: dsterm start")
    addrs = lan_addresses()
    if addrs:
        print(f"  This computer is {', '.join(addrs)}")
    print(f"  Open for {int(seconds)} s. Ctrl-C to close.")
    asked = ""
    try:
        while True:
            window = pairing.read_window()
            if window is None:
                break
            if window.ended or window.remaining() <= 0:
                break
            if window.code and window.code != asked:
                asked = window.code
                matched = ask_code_matches(window.code)
                pairing.write_answer(window.code, matched)
                if not matched:
                    print("\n  Not paired: the codes did not match. If you did not start "
                          "this,\n  someone else on the network may have.")
                    break
                print("  Waiting for the DS (press A there if you have not)...")
            time.sleep(0.25)
    except KeyboardInterrupt:
        pairing.close_window()
        print("\nclosed")
        return 1
    window = pairing.read_window()
    why = window.ended if window else ""
    pairing.close_window()
    after = {d.device_id for d in pairing.list_devices()}
    new = sorted(after - before)
    if new:
        print(f"\npaired: device {', '.join(new)}")
        return 0
    if why and why != "paired":
        print(f"\nnot paired: {why}")
    elif not asked:
        print("\nthe window closed without a console pairing")
    return 1


def cmd_devices(argv: list[str]) -> int:
    from . import pairing

    if argv[:1] == ["forget"]:
        if len(argv) < 2:
            die("usage: dsterm devices forget DEVICE_ID")
        if pairing.forget_device(argv[1]):
            print(f"forgot {argv[1]}; that console must pair again")
            return 0
        die(f"no paired device {argv[1]}")
    devices = pairing.list_devices()
    if not devices:
        print("no paired devices yet -- run: dsterm pair")
        return 0
    for d in devices:
        when = time.strftime("%Y-%m-%d %H:%M", time.localtime(d.paired_at)) if d.paired_at else "?"
        print(f"{d.device_id}  paired {when}  {d.address or ''}  {d.name or ''}".rstrip())
    return 0


# -- doctor -----------------------------------------------------------------------


def cmd_doctor(argv: list[str]) -> int:
    from . import doctor

    return doctor.main(argv)


def cmd_wsl_setup(argv: list[str]) -> int:
    from . import wsl

    return wsl.main(argv)


# -- dispatch ---------------------------------------------------------------------

USAGE = """\
dsterm -- the DS Claude Terminal server

  setup                      pairing key, service, and the next step
  pair                       pair a DS: both screens show a code to compare
  devices [forget ID]        paired handhelds
  doctor                     check everything; paste the output when asking for help
  start | stop | restart     the service (systemd, or launchd on macOS)
  status                     service, socket, handheld, tmux session
  logs [-f]                  the service's log
  serve [ARGS]               run the server in this terminal
  screen [host] [port]       print what the handheld is showing (it stays connected)
  view [--window]            the DS on this computer: both screens, typing
                             and clicks, and the camera when it is on;
                             --window: an emulator window, real pixels
  camera [--window]          watch the DSi's viewfinder (live, not recorded)
  attach                     join the tmux session from this PC (C-b d to leave)
  wsl-setup                  Windows firewall / port proxy for WSL2
  card PATH [opts]           write the ROM (and optionally settings) to an SD card
  sounds                     the cue names for your own sounds, and the format
  lang [--template]          built-in UI languages, and a template to import your own
  timer [MIN [LABEL] | stop | status]
                             a Pomodoro focus timer on PROJECT -> WATCH
  install-service [--cwd P]  the service only
  build [make args]          build the ROM (checkout only)
  test                       the Python and C suites (checkout only)
  install-card PATH          the ROM only (the older, narrower name)

card options:
  --wifi         ask for the Wi-Fi name and password and write them to the card
  --pair         pair the card with this computer offline: nothing over the air
  --host ADDR    the address the card should use for this PC (with --pair)
  --rom FILE     write this ROM instead of building one
  --sounds DIR   check and copy your own sounds (`dsterm sounds` lists the names)
  --lang FILE    copy a custom .lang UI translation file to dsterm/lang/
  --no-eject     leave the card mounted
"""

COMMANDS: dict[str, Callable[[list[str]], int]] = {
    "setup": cmd_setup,
    "pair": cmd_pair,
    "devices": cmd_devices,
    "doctor": cmd_doctor,
    "start": cmd_start,
    "stop": cmd_stop,
    "restart": cmd_restart,
    "status": cmd_status,
    "logs": cmd_logs,
    "serve": cmd_serve,
    "screen": cmd_screen,
    "camera": cmd_camera,
    "view": cmd_view,
    "attach": cmd_attach,
    "wsl-setup": cmd_wsl_setup,
    "card": cmd_card,
    "sounds": cmd_sounds,
    "lang": cmd_lang,
    "timer": cmd_timer,
    "install-service": cmd_install_service,
    "build": cmd_build,
    "test": cmd_test,
    "install-card": cmd_install_card,
}


def usage() -> str:
    text = USAGE
    for e in extensions():
        lines = [f"  {name:<26} {help_}" for name, (help_, _) in e.commands.items()]
        lines += [f"  card ... {flag:<17} {help_}" for flag, help_ in e.card_flags.items()]
        if lines:
            text += f"\n{e.name}:\n" + "\n".join(lines) + "\n"
    return text


def command_help(name: str) -> str:
    """The usage lines for one command, and the card's options for `card`."""
    text = usage()
    # The left column names the command: "start | stop | restart", "card PATH".
    # A description may run on, on lines indented to its column.
    entries: list[tuple[str, list[str]]] = []
    current: list[str] | None = None
    for line in text.splitlines():
        if line.startswith("  ") and not line.startswith("   "):
            current = None
            if name in (part.split()[0] for part in line[2:28].split("|")
                        if part.strip()):
                current = [line[29:].strip()]
                entries.append((line[2:28].strip(), current))
        elif current is not None and line.startswith(" " * 29) and line.strip():
            current.append(line.strip())
        else:
            current = None
    if not entries:
        return text
    # "  setup                      pairing key, ..." -> "dsterm setup" over
    # its description, indented.
    out = "".join(f"dsterm {cmd}\n" + "".join(f"    {d}\n" for d in desc)
                  for cmd, desc in entries)
    if name == "card":
        out += "\n" + text[text.index("card options:"):].split("\n\n")[0] + "\n"
    return out


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(usage(), end="")
        return 0
    # `dsterm setup --help` must explain setup, not run it. Every command but
    # serve (argparse, with its own full help) is answered here, before it
    # can start a service, open a pairing window or write a unit file.
    if argv[0] != "serve" and any(a in ("-h", "--help") for a in argv[1:]):
        print(command_help(argv[0]), end="")
        return 0
    if argv[0] in ("-V", "--version", "version"):
        from importlib.metadata import PackageNotFoundError, version
        try:
            print(f"dsterm {version('ds-claude-terminal')}")
        except PackageNotFoundError:
            print("dsterm (not installed: a checkout without its venv)")
        return 0
    name, rest = argv[0], argv[1:]
    try:
        if name in COMMANDS:
            return COMMANDS[name](rest) or 0
        for e in extensions():
            if name in e.commands:
                return e.commands[name][1](rest) or 0
        die(f"unknown command '{name}' -- try: dsterm --help")
    except Die as exc:
        return int(exc.code or 1)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
