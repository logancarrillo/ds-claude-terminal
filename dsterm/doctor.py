"""`dsterm doctor`: every thing that has to be true for a handheld to connect,
checked in order, in a form worth pasting into an issue.

Nothing here changes anything. Each line is ok, warn or FAIL, with what to do
about it; TROUBLESHOOTING.md is keyed to these lines. It deliberately prints
no secrets: the key's presence and permissions, never the key.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

OK, WARN, FAIL = "ok", "warn", "FAIL"



def service_path() -> str | None:
    """The PATH the installed service gives the server and Claude Code.

    Fixed in the unit and the plist, not inherited from a login shell, so a
    `claude` this shell finds can still be one the service cannot.
    """
    from . import cli

    home = str(Path.home())
    manager = cli.service_manager()
    if manager == "launchd":
        m = re.search(r"<key>PATH</key>\s*<string>([^<]*)</string>",
                      (cli.DATA / "dsterm.plist").read_text())
        return m.group(1).replace("@HOME@", home) if m else None
    if manager == "systemd":
        m = re.search(r"^Environment=PATH=(.*)$",
                      (cli.DATA / "dsterm.service").read_text(), re.M)
        return m.group(1).replace("%h", home) if m else None
    return None

# What `dsterm doctor` suggests for a local speech-to-text library: the same
# commands docs/USING.md gives, since a pipx install has its own venv and a
# bare `pip install` would put the library somewhere dsterm cannot import it.
STT_INSTALL_HINT = ("pipx inject ds-claude-terminal faster-whisper (or "
                    "mlx-whisper on Apple silicon, with ffmpeg)")


def serve_env(env_path: Path | None = None) -> tuple[dict[str, str], str | None]:
    """(the environment `dsterm serve` would choose a backend from, a problem
    with the env file or None).

    The same merge serve.py makes: this process's environment with the env
    file over it, so a DSTERM_STT_CMD or DSTERM_STT_URL set only in the file
    is seen here as it is there. A file serve would refuse is reported, and
    the inherited environment used alone."""
    from . import serve

    path = env_path if env_path is not None else serve.DEFAULT_ENV_PATH
    env = dict(os.environ)
    try:
        env.update(serve.load_env_file(path))
    except (OSError, ValueError) as exc:
        return env, str(exc)
    return env, None


def voice_check(env: dict[str, str]) -> tuple[str, str]:
    """(level, detail) for the voice line, decided as `dsterm serve` started
    from this shell would decide it. The cloud opt-in is read from this
    process's environment, never the file (voice.cloud_opted_in)."""
    from . import voice as dsvoice

    backend, where = dsvoice.choose_backend(
        env, cloud=dsvoice.cloud_opted_in(False))
    if backend is not None:
        return OK, f"Push-to-Talk: {where}"
    # `where` carries the real reason ("off: --stt-cloud is on, but ..."), so
    # it is printed, followed by what would turn voice on.
    return OK, f"Push-to-Talk {where} -- optional: {STT_INSTALL_HINT}"


def _version() -> str:
    try:
        from importlib.metadata import version

        return version("ds-claude-terminal")
    except Exception:
        return "(checkout)"


def checks():
    from . import auth, cli, pairing, wsl

    yield OK, "dsterm", f"{_version()} on Python {platform.python_version()}"
    if sys.version_info < (3, 11):
        yield FAIL, "python", "3.11 or newer is required"
    osname = "WSL" if cli.is_wsl() else platform.system()
    yield OK, "system", f"{osname} {platform.release()}"

    key = auth.DEFAULT_KEY_PATH
    if not key.exists():
        yield FAIL, "key", f"no {key} -- run: dsterm setup"
    else:
        mode = key.stat().st_mode
        if mode & (stat.S_IRWXG | stat.S_IRWXO):
            yield FAIL, "key", f"{key} is readable by others -- chmod 600 it"
        else:
            try:
                auth.load_key(key)
                yield OK, "key", f"{key} (600)"
            except ValueError as exc:
                yield FAIL, "key", str(exc)

    if cli.CONFIG_DIR.exists() and cli.CONFIG_DIR.stat().st_mode & (stat.S_IRWXG | stat.S_IRWXO):
        yield WARN, "config dir", f"{cli.CONFIG_DIR} is readable by others -- chmod 700 it"

    claude = shutil.which("claude")
    local = Path.home() / ".local" / "bin" / "claude"
    svc_path = service_path()
    in_service = svc_path is None or shutil.which("claude", path=svc_path)
    if claude and not in_service:
        # Found from this shell, not by the service: an nvm or npm-global
        # install. The session would die the moment it started ("[exited]").
        yield WARN, "claude", (f"{claude} is not on the service's PATH -- link it: "
                               f'ln -s "{claude}" ~/.local/bin/claude')
    elif claude:
        yield OK, "claude", claude
    elif local.exists():
        yield WARN, "claude", f"{local} exists but is not on this shell's PATH (the service adds it)"
    else:
        yield FAIL, "claude", "Claude Code is not installed or not on PATH"

    # The DS learns "Claude needs your OK" and the REMOTE list from
    # `claude agents --json`; a Claude Code too old to have it still works,
    # but the DS can then only guess what Claude is doing from the screen.
    path = claude or (str(local) if local.exists() else None)
    if path:
        try:
            r = subprocess.run([path, "agents", "--json"], capture_output=True,
                               text=True, timeout=15)
            json.loads(r.stdout)
            ok = r.returncode == 0
        except (OSError, subprocess.SubprocessError, ValueError):
            ok = False
        if ok:
            yield OK, "agents", "approvals, questions and REMOTE can be reported"
        else:
            yield WARN, "agents", ("not available: the DS will not hear about "
                                          "approvals or list sessions -- claude update")

    tmux = shutil.which("tmux")
    if tmux:
        v = subprocess.run(["tmux", "-V"], capture_output=True, text=True).stdout.strip()
        yield OK, "tmux", v or tmux
    else:
        yield FAIL, "tmux", "not installed (apt install tmux / brew install tmux)"

    manager = cli.service_manager()
    state = cli.service_state()
    if manager is None:
        yield WARN, "service", "no systemd/launchd here: run `dsterm serve` in a terminal"
    elif state in ("active", "running"):
        yield OK, "service", f"{manager}: {state}"
    else:
        yield FAIL, "service", f"{manager}: {state} -- dsterm install-service, then dsterm start"

    if cli.probe_tcp():
        yield OK, "tcp", f"a dsterm server answers on port {cli.PORT}"
    else:
        yield FAIL, "tcp", f"nothing answers on 127.0.0.1:{cli.PORT} -- dsterm start; dsterm logs"

    answer = cli.probe_udp()
    if answer:
        yield OK, "discovery", f"udp {cli.DISCOVERY_PORT} answers as '{answer.name}'"
    else:
        yield WARN, "discovery", (f"no answer on udp {cli.DISCOVERY_PORT}: the console's "
                                  "Find servers will not see this PC (Enter an address still works)")

    addrs = cli.lan_addresses()
    if addrs:
        yield OK, "addresses", ", ".join(addrs)
    else:
        yield FAIL, "addresses", "no LAN address found"

    devices = pairing.list_devices()
    yield OK, "devices", (f"{len(devices)} paired" if devices
                          else "none paired yet -- run: dsterm pair")
    window = pairing.current_window()
    if window:
        yield OK, "pairing", f"window open for {int(window.remaining())} s"

    # Where a recording would go, as `dsterm serve` started from this shell
    # would decide it: from the same environment, the env file merged over
    # this one. A --stt-cloud in serve.conf is not seen from here, so the
    # cloud case says what it depends on rather than guessing.
    env, env_problem = serve_env()
    if env_problem:
        yield WARN, "env file", f"{env_problem} -- dsterm serve will refuse to start"
    level, detail = voice_check(env)
    yield level, "voice", detail

    if cli.is_wsl():
        mode = wsl.networking_mode()
        if mode == "mirrored":
            yield OK, "wsl", "mirrored networking -- run dsterm wsl-setup once for the firewall"
        elif mode == "nat":
            ip = wsl.wsl_address()
            yield WARN, "wsl", (f"NAT networking (WSL is {ip}): the console cannot reach "
                                "it without a port proxy -- run dsterm wsl-setup, and "
                                "use Scan this network on the console")
        else:
            yield WARN, "wsl", "networking mode unknown"
        if addrs and all(a.startswith("172.") for a in addrs):
            yield WARN, "wsl", ("these are WSL's internal addresses; the console must use "
                                "the Windows machine's LAN address")


def main(argv: list[str]) -> int:
    worst = OK
    for level, what, detail in checks():
        mark = {OK: "[ ok ]", WARN: "[warn]", FAIL: "[FAIL]"}[level]
        print(f"{mark} {what:<10} {detail}")
        if level == FAIL:
            worst = FAIL
        elif level == WARN and worst == OK:
            worst = WARN
    return 1 if worst == FAIL else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
