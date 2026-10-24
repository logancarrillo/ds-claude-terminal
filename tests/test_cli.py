"""`dsterm`, the command: run as a subprocess against a temporary HOME and a
temporary card, so nothing here touches the real key, service or SD card.

What is tested is mostly the card writer's judgement -- the refusals and the
guarantees -- because a command that copies files is not worth a test, and a
command that declines to copy them onto a git tree, or over a config nobody
asked it to replace, is.
"""

from __future__ import annotations

import os
import plistlib
import subprocess
import sys
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent


def dsterm(*args: str, home: Path, stdin: str | None = None,
           expect: int | None = 0) -> subprocess.CompletedProcess:
    env = dict(os.environ, HOME=str(home), PYTHONPATH=str(REPO))
    # A new session has no controlling terminal, so nothing the command does
    # can reach past the pipe to /dev/tty: getpass there waited forever for
    # a password the test had already written to stdin.
    result = subprocess.run([sys.executable, "-m", "dsterm.cli", *args],
                            capture_output=True, text=True, env=env,
                            input=stdin, cwd=str(home), timeout=60,
                            start_new_session=True)
    if expect is not None and result.returncode != expect:
        pytest.fail(f"dsterm {' '.join(args)} exited {result.returncode}, expected "
                    f"{expect}\nstdout: {result.stdout}\nstderr: {result.stderr}")
    return result


@pytest.fixture
def home(tmp_path) -> Path:
    h = tmp_path / "home"
    h.mkdir()
    return h


@pytest.fixture
def card(tmp_path) -> Path:
    c = tmp_path / "card"
    c.mkdir()
    return c


@pytest.fixture
def rom(tmp_path) -> Path:
    r = tmp_path / "dsterm.nds"
    r.write_bytes(b"NDS" + os.urandom(4096))
    return r


class TestTheCommand:
    def test_help_names_the_commands(self, home):
        out = dsterm("--help", home=home).stdout
        for name in ("setup", "pair", "devices", "doctor", "card", "wsl-setup"):
            assert name in out

    def test_an_unknown_command_is_refused(self, home):
        r = dsterm("frobnicate", home=home, expect=1)
        assert "unknown command" in r.stderr

    @pytest.mark.parametrize("name", ["setup", "start", "stop", "restart", "pair",
                                      "install-service", "card", "screen", "test"])
    def test_help_after_a_command_explains_it_and_does_nothing(self, name,
                                                               monkeypatch, capsys):
        # `dsterm setup --help` used to run setup -- service and all.
        from dsterm import cli

        ran = []
        monkeypatch.setitem(cli.COMMANDS, name, lambda argv: ran.append(argv) or 0)
        assert cli.main([name, "--help"]) == 0
        assert ran == []
        out = capsys.readouterr().out
        assert out.startswith("dsterm ") and name in out.split("\n")[0]

    def test_version(self, home):
        assert dsterm("--version", home=home).stdout.startswith("dsterm ")

    def test_the_link_setup_makes_runs_the_checkout_from_anywhere(self, home):
        # Setup from a clone links ~/.local/bin/dsterm to bin/dsterm. The
        # launcher found the checkout from the link's own directory and ran
        # "No module named 'dsterm'" everywhere but inside the clone.
        link = home / ".local" / "bin" / "dsterm"
        link.parent.mkdir(parents=True)
        link.symlink_to(REPO / "bin" / "dsterm")
        relative = home / "dsterm"
        relative.symlink_to(Path(".local") / "bin" / "dsterm")
        env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
        env["HOME"] = str(home)
        for path in (link, relative):
            r = subprocess.run([str(path), "--version"], capture_output=True,
                               text=True, cwd=str(home), env=env, timeout=60)
            assert r.returncode == 0 and r.stdout.startswith("dsterm "), r.stderr


class TestCardRefusals:
    def test_not_mounted(self, home, tmp_path, rom):
        r = dsterm("card", str(tmp_path / "nope"), "--rom", str(rom), home=home, expect=1)
        assert "not mounted" in r.stderr

    def test_a_git_working_tree(self, home, card, rom):
        (card / ".git").mkdir()
        r = dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home, expect=1)
        assert "git working tree" in r.stderr
        assert not (card / "dsterm.nds").exists()

    def test_two_paths(self, home, card, tmp_path, rom):
        other = tmp_path / "other"
        other.mkdir()
        r = dsterm("card", str(card), str(other), "--rom", str(rom), home=home, expect=1)
        assert "one card path" in r.stderr

    def test_an_unknown_option(self, home, card, rom):
        r = dsterm("card", str(card), "--yolo", "--rom", str(rom), home=home, expect=1)
        assert "unknown option" in r.stderr

    def test_a_missing_rom_file(self, home, card, tmp_path):
        r = dsterm("card", str(card), "--rom", str(tmp_path / "x.nds"), home=home, expect=1)
        assert "no ROM" in r.stderr


class TestCardWrites:
    def test_the_rom_arrives_and_the_old_one_is_kept(self, home, card, rom, tmp_path):
        old = card / "dsterm.nds"
        old.write_bytes(b"OLD ROM")
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        assert old.read_bytes() == rom.read_bytes()
        [backup] = (card / "dsterm" / "backups").iterdir()
        assert backup.read_bytes() == b"OLD ROM"

    def test_the_same_rom_is_not_backed_up_again(self, home, card, rom):
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        assert not (card / "dsterm" / "backups").exists()

    def test_the_legacy_config_is_never_touched(self, home, card, rom):
        cfg = card / "dsterm.cfg"
        cfg.write_text("ssid = Example\n")
        r = dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        assert cfg.read_text() == "ssid = Example\n"
        assert "dsterm.cfg untouched" in r.stdout

    def test_a_card_with_no_settings_is_fine_and_said_so(self, home, card, rom):
        r = dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        assert "setup" in r.stderr and "menu" in r.stderr
        # It must not invent a config: a scaffold with placeholder
        # credentials would boot and fail to associate.
        assert not (card / "dsterm.cfg").exists()
        assert not (card / "dsterm" / "wifi.cfg").exists()

    def test_nothing_on_the_card_is_deleted(self, home, card, rom):
        (card / "_nds").mkdir()
        (card / "_nds" / "TWiLightMenu").write_text("launcher")
        (card / "boot.nds").write_text("unlaunch payload")
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", home=home)
        assert (card / "_nds" / "TWiLightMenu").read_text() == "launcher"
        assert (card / "boot.nds").read_text() == "unlaunch payload"

    def test_wifi_is_written_when_asked(self, home, card, rom):
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", "--wifi",
               home=home, stdin="Example Network\npassphrase1\n")
        text = (card / "dsterm" / "wifi.cfg").read_text()
        assert "ssid = Example Network" in text and "wifi_key = passphrase1" in text
        assert "ip = dhcp" in text

    def test_a_short_wifi_password_is_refused_before_anything_is_written(
            self, home, card, rom):
        r = dsterm("card", str(card), "--rom", str(rom), "--no-eject", "--wifi",
                   home=home, stdin="Example\nshort\n", expect=1)
        assert "8 to 63" in r.stderr
        assert not (card / "dsterm.nds").exists()

    def test_offline_pairing_writes_both_halves(self, home, card, rom):
        r = dsterm("card", str(card), "--rom", str(rom), "--no-eject", "--pair",
                   "--host", "192.0.2.10", home=home)
        assert "paired: device" in r.stdout
        device_id = (card / "dsterm" / "id").read_text().strip()
        [profile] = (card / "dsterm" / "servers").iterdir()
        text = profile.read_text()
        assert "host = 192.0.2.10" in text and "port = 8347" in text
        psk = next(l.split("=", 1)[1].strip() for l in text.splitlines()
                   if l.startswith("psk"))
        server_key = (home / ".config" / "ds-claude-terminal" / "devices" /
                      f"{device_id}.psk").read_text().strip()
        assert psk == server_key, "the card and the server hold different keys"
        assert (card / "dsterm" / "last").read_text().strip() == profile.stem

    def test_offline_pairing_keeps_an_existing_device_id(self, home, card, rom):
        (card / "dsterm").mkdir()
        (card / "dsterm" / "id").write_text("0a1b2c3d\n")
        dsterm("card", str(card), "--rom", str(rom), "--no-eject", "--pair",
               "--host", "192.0.2.10", home=home)
        assert (card / "dsterm" / "id").read_text().strip() == "0a1b2c3d"
        assert (home / ".config" / "ds-claude-terminal" / "devices" /
                "0a1b2c3d.psk").exists()


class TestDevicesAndPairing:
    def test_devices_list_and_forget(self, home):
        from dsterm import pairing

        cfg = home / ".config" / "ds-claude-terminal"
        pairing.save_device("0a1b2c3d", os.urandom(16), address="192.0.2.5",
                            config_dir=cfg)
        assert "0a1b2c3d" in dsterm("devices", home=home).stdout
        dsterm("devices", "forget", "0a1b2c3d", home=home)
        assert "no paired devices" in dsterm("devices", home=home).stdout
        dsterm("devices", "forget", "0a1b2c3d", home=home, expect=1)

    def test_pair_needs_a_terminal_to_compare_the_code_on(self, home):
        # The comparison is the security, so a pair run where nobody can see
        # the code is refused, pointing at the way that needs no network.
        r = dsterm("pair", "--seconds", "1", home=home, expect=1)
        assert "needs a terminal" in r.stderr and "dsterm card PATH --pair" in r.stderr
        assert not (home / ".config" / "ds-claude-terminal" / "pairing").exists()

    def test_pair_opens_and_closes_when_it_expires(self, home):
        import pty

        master, slave = pty.openpty()
        env = dict(os.environ, HOME=str(home), PYTHONPATH=str(REPO))
        started = time.monotonic()
        proc = subprocess.Popen([sys.executable, "-m", "dsterm.cli", "pair", "--seconds", "1"],
                                stdin=slave, stdout=slave, stderr=slave, env=env,
                                cwd=str(home), start_new_session=True)
        os.close(slave)
        out = bytearray()
        try:
            while True:
                try:
                    chunk = os.read(master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                out += chunk
            assert proc.wait(timeout=20) == 1
        finally:
            os.close(master)
            if proc.poll() is None:
                proc.kill()
        text = out.decode(errors="replace")
        assert time.monotonic() - started < 20
        assert "Pairing is open" in text and "six-digit code" in text
        assert "closed without a console pairing" in text
        assert not (home / ".config" / "ds-claude-terminal" / "pairing").exists()


class TestServiceTemplates:
    def test_the_unit_is_fully_substituted(self):
        from dsterm import cli

        text = cli.unit_text(Path("/srv/work"))
        assert "@" not in text.replace("%h", "")
        assert f"ExecStart={sys.executable} -m dsterm.cli serve" in text
        assert "WorkingDirectory=/srv/work" in text
        assert "KillMode=process" in text, "stopping must not kill the tmux session"

    def test_the_plist_is_valid_and_substituted(self):
        from dsterm import cli

        text = cli.plist_text(Path("/srv/work"))
        assert "@" not in text
        plist = plistlib.loads(text.encode())
        assert plist["Label"] == cli.LAUNCHD_LABEL
        assert plist["ProgramArguments"][:4] == [sys.executable, "-m", "dsterm.cli", "serve"]
        assert plist["WorkingDirectory"] == "/srv/work"
        assert ".local/bin" in plist["EnvironmentVariables"]["PATH"]


class TestWslScript:
    def test_mirrored_opens_both_ports_on_the_hyperv_firewall(self):
        from dsterm import wsl

        text = wsl.script("mirrored", None)
        assert "New-NetFirewallHyperVRule -Name 'DSTerm-In'" in text
        assert "-Protocol TCP -LocalPorts $tcp" in text
        assert "-Protocol UDP -LocalPorts $udp" in text
        assert "Remove-NetFirewallHyperVRule" in text, "re-running must not duplicate rules"
        assert "portproxy" not in text

    def test_nat_proxies_to_wsl(self):
        from dsterm import wsl

        text = wsl.script("nat", "172.20.1.2")
        assert "connectaddress=172.20.1.2" in text
        assert "-Profile Private,Domain" in text, "never open the port on public networks"
        assert "portproxy delete" in text
        with pytest.raises(ValueError):
            wsl.script("nat", None)

    def test_it_never_changes_the_networking_mode(self):
        from dsterm import wsl

        for mode in ("nat", "mirrored"):
            assert ".wslconfig" not in wsl.script(mode, "172.20.1.2").split("Recommended")[0]


class TestDoctor:
    def test_it_runs_and_reports_a_missing_key_without_printing_secrets(self, home):
        r = dsterm("doctor", home=home, expect=None)
        assert "[FAIL] key" in r.stdout
        assert r.returncode == 1
        for line in r.stdout.splitlines():
            assert line[:6] in ("[ ok ]", "[warn]", "[FAIL]"), line

    def test_a_claude_only_this_shell_can_find_is_a_warning(self, monkeypatch):
        # nvm and npm-global installs: on the login PATH, not on the fixed
        # PATH the service runs with, so the session dies the moment it starts.
        from dsterm import cli, doctor

        monkeypatch.setattr(cli, "service_manager", lambda: "launchd")
        nvm = "/opt/nvm/versions/node/v22/bin/claude"
        monkeypatch.setattr(doctor.shutil, "which",
                            lambda name, path=None: None if path else nvm)
        # Only as far as the claude line: later checks talk to tmux.
        level, _, detail = next(c for c in doctor.checks() if c[1] == "claude")
        assert level == doctor.WARN
        assert "not on the service's PATH" in detail and nvm in detail

    def test_the_service_path_is_the_templates(self, monkeypatch):
        from dsterm import cli, doctor

        for manager in ("launchd", "systemd"):
            monkeypatch.setattr(cli, "service_manager", lambda m=manager: m)
            path = doctor.service_path()
            assert path and str(Path.home() / ".local" / "bin") in path.split(":")
            assert "@HOME@" not in path and "%h" not in path


def test_serve_conf_overrides_the_services_options_but_not_its_command(tmp_path):
    """The service's command line is defaults; serve.conf wins for an option
    both set (argparse keeps the last, so the file goes last); an explicit
    `-- command` on the real command line still beats the file's."""
    from dsterm import cli

    conf = tmp_path / "serve.conf"
    conf.write_text("# mine\n--clip   # the clipboard\n--dashboard-port 9000\n"
                    "-- claude --model sonnet\n")
    from_file = cli.serve_options(conf)
    assert from_file == ["--clip", "--dashboard-port", "9000",
                         "--", "claude", "--model", "sonnet"]
    assert cli.merge_serve_args(from_file, list(cli.SERVE_ARGS)) == [
        *cli.SERVE_ARGS, "--clip", "--dashboard-port", "9000",
        "--", "claude", "--model", "sonnet"]
    assert cli.merge_serve_args(from_file, ["--", "bash"]) == [
        "--clip", "--dashboard-port", "9000", "--", "bash"]
    assert cli.serve_options(tmp_path / "absent") == []

    from dsterm.serve import build_parser
    args = build_parser().parse_args(cli.merge_serve_args(from_file, list(cli.SERVE_ARGS)))
    assert args.dashboard_port == 9000 and args.tmux == "dsterm"


def _serve_conf(tmp_path, text):
    from dsterm import cli
    from dsterm.serve import build_parser

    conf = tmp_path / "serve.conf"
    conf.write_text(text)
    cli.check_serve_conf(conf, cli.serve_conf_words(conf), build_parser())


@pytest.mark.parametrize("text", [
    "--clip\n--grid 64x32\n--port=9000\n--dash 9001  # a prefix argparse takes\n",
    "--cwd ~/code --no-remote\n-- FOO=bar claude\n",   # anything after --
    "",
])
def test_a_serve_conf_the_server_reads_is_accepted(tmp_path, text):
    _serve_conf(tmp_path, text)


@pytest.mark.parametrize("text,line,why", [
    # The env file's syntax in the wrong file: it used to become the child
    # command and swallow the service's own --tmux after it.
    ("--clip\nANTHROPIC_MODEL=sonnet\n", 2, "is not an option"),
    ("claude --model sonnet\n", 1, "is not an option"),
    ("--clip yes\n", 1, "is not an option"),
    ("--bogus\n", 1, "unknown option"),
    ("--clip=1\n", 1, "takes no value"),
])
def test_a_stray_word_in_serve_conf_is_refused_with_its_line(tmp_path, capsys, text,
                                                              line, why):
    from dsterm import cli

    with pytest.raises(cli.Die):
        _serve_conf(tmp_path, text)
    err = capsys.readouterr().err
    assert f"serve.conf:{line}:" in err and why in err


def test_cmd_serve_refuses_a_bad_serve_conf_before_serving(tmp_path, monkeypatch, capsys):
    from dsterm import cli, serve

    conf = tmp_path / "serve.conf"
    conf.write_text("--clip\nKEY=VALUE\n")
    monkeypatch.setattr(cli, "SERVE_CONF", conf)
    monkeypatch.setattr(serve, "main", lambda args: pytest.fail("served anyway"))
    with pytest.raises(cli.Die):
        cli.cmd_serve(list(cli.SERVE_ARGS))
    assert f"{conf}:2:" in capsys.readouterr().err

    conf.write_text("--dashboard-port 9000\n")
    seen = []
    monkeypatch.setattr(serve, "main", lambda args: seen.append(args) or 0)
    assert cli.cmd_serve(list(cli.SERVE_ARGS)) == 0
    assert serve.build_parser().parse_args(seen[0]).dashboard_port == 9000


@pytest.mark.parametrize("flag,dest", [("--cwd", "cwd"), ("--key-file", "key_file"),
                                       ("--api-key-file", "api_key_file"),
                                       ("--env-file", "env_file"),
                                       ("--snippets-file", "snippets_file")])
def test_path_options_expand_the_home_directory(flag, dest):
    """serve.conf is split by shlex, which leaves `~` alone; a shell would not."""
    from dsterm.serve import build_parser

    args = build_parser().parse_args([flag, "~/somewhere"])
    assert getattr(args, dest) == os.path.expanduser("~/somewhere")
    assert "~" not in getattr(args, dest)


def test_serve_help_lists_grid():
    from dsterm.serve import build_parser

    assert "--grid COLSxROWS" in build_parser().format_help()


def test_grid_option_sets_the_geometry(monkeypatch):
    from dsterm import cli

    # Registered with monkeypatch first, so whatever apply_grid writes is
    # undone after the test and cannot leak a 64x32 server into later ones.
    monkeypatch.setenv("DSTERM_COLS", "42")
    monkeypatch.setenv("DSTERM_ROWS", "24")
    assert cli.apply_grid(["--grid", "64x32", "--clip", "--", "x", "--grid"]) == [
        "--clip", "--", "x", "--grid"]
    import os
    assert os.environ["DSTERM_COLS"] == "64" and os.environ["DSTERM_ROWS"] == "32"


def test_grid_reaches_the_server_it_starts(tmp_path):
    """`--grid` works only if nothing imported dsterm.grid before it set the
    environment.  Checked in a fresh interpreter, the way `dsterm serve`
    runs: after cli.apply_grid, the grid module must read 64x32."""
    import subprocess
    import sys
    code = ("import os; from dsterm import cli; cli.apply_grid(['--grid', '64x32']); "
            "from dsterm import grid; print(grid.COLS, grid.ROWS)")
    env = {k: v for k, v in __import__("os").environ.items()
           if k not in ("DSTERM_COLS", "DSTERM_ROWS")}
    out = subprocess.run([sys.executable, "-c", code], capture_output=True,
                         text=True, env=env, check=True).stdout.split()
    assert out == ["64", "32"]


def test_the_package_imports_nothing_heavy():
    """`bin/dsterm setup` imports dsterm.cli before the dependencies are
    installed; the package itself must not pull in pyte through the grid."""
    import subprocess
    import sys
    code = ("import sys, dsterm, dsterm.cli; "
            "print('dsterm.grid' in sys.modules, 'pyte' in sys.modules)")
    out = subprocess.run([sys.executable, "-c", code], capture_output=True,
                         text=True, check=True).stdout.split()
    assert out == ["False", "False"]


def test_card_sounds_without_a_rom_for_a_release_install(tmp_path, monkeypatch):
    """A release install has no checkout to build a ROM from; `dsterm card
    PATH --sounds DIR` must still copy the sounds and leave the app alone."""
    import wave
    from dsterm import cli

    monkeypatch.setattr(cli, "checkout", lambda: None)
    monkeypatch.setattr(cli, "extensions", lambda: [])
    card = tmp_path / "card"
    card.mkdir()
    (card / "dsterm.nds").write_bytes(b"the app already there")
    sounds = tmp_path / "mine"
    sounds.mkdir()
    with wave.open(str(sounds / "done.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes(b"\x00\x00" * 2000)
    assert cli.cmd_card([str(card), "--sounds", str(sounds), "--no-eject"]) == 0
    assert (card / "dsterm" / "sounds" / "done.wav").is_file()
    assert (card / "dsterm.nds").read_bytes() == b"the app already there"


def test_card_sounds_set_a_replaced_cues_old_takes_aside(tmp_path, monkeypatch):
    """A new done.wav replaces the cue: an old done-2.wav left beside it kept
    playing every other time. It goes into dsterm/sounds/old/ -- moved, never
    deleted, and never over something already there."""
    import wave
    from dsterm import cli

    def wav(path, frames):
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(22050)
            w.writeframes(b"\x01\x00" * frames)

    monkeypatch.setattr(cli, "checkout", lambda: None)
    monkeypatch.setattr(cli, "extensions", lambda: [])
    card = tmp_path / "card"
    sounds_dir = card / "dsterm" / "sounds"
    (sounds_dir / "old").mkdir(parents=True)
    wav(sounds_dir / "done.wav", 100)
    wav(sounds_dir / "done-2.wav", 200)
    wav(sounds_dir / "sent.wav", 300)
    wav(sounds_dir / "old" / "done-2.wav", 400)        # an earlier one, kept
    before = (sounds_dir / "done-2.wav").read_bytes()
    mine = tmp_path / "mine"
    mine.mkdir()
    wav(mine / "done.wav", 500)
    assert cli.cmd_card([str(card), "--sounds", str(mine), "--no-eject"]) == 0
    assert (sounds_dir / "done.wav").read_bytes() == (mine / "done.wav").read_bytes()
    assert not (sounds_dir / "done-2.wav").exists()
    assert (sounds_dir / "sent.wav").is_file()                   # another cue: kept
    old = sorted(p.name for p in (sounds_dir / "old").iterdir())
    assert len(old) == 2 and "done-2.wav" in old
    moved = next(p for p in (sounds_dir / "old").iterdir() if p.name != "done-2.wav")
    assert moved.read_bytes() == before


# -- dsterm timer --------------------------------------------------------------

@pytest.fixture
def timer_dir(tmp_path, monkeypatch):
    """A working directory outside any repository: the timer lives there."""
    d = tmp_path / "work"
    d.mkdir()
    monkeypatch.chdir(d)
    monkeypatch.setenv("GIT_CEILING_DIRECTORIES", str(tmp_path))
    return d


class TestTimer:
    def test_start_status_and_stop(self, timer_dir, capsys):
        import json
        from dsterm import cli

        assert cli.cmd_timer(["25", "deep", "work"]) == 0
        data = json.loads((timer_dir / ".dsterm" / "timer.json").read_text())
        assert data["label"] == "deep work"
        assert (timer_dir / ".dsterm" / ".gitignore").is_file()
        capsys.readouterr()
        assert cli.cmd_timer(["status"]) == 0
        out = capsys.readouterr().out
        assert out.startswith("timer: deep work -- ")
        assert "remaining" in out and ("24m" in out or "25m 00s" in out)
        assert cli.cmd_timer(["stop"]) == 0
        assert not (timer_dir / ".dsterm" / "timer.json").exists()
        cli.cmd_timer([])
        assert "timer: off" in capsys.readouterr().out

    def test_a_finished_timer_says_done(self, timer_dir, capsys):
        import json
        from dsterm import cli

        (timer_dir / ".dsterm").mkdir()
        (timer_dir / ".dsterm" / "timer.json").write_text(json.dumps(
            {"label": "tea", "end_ts": time.time() - 0.2}))
        assert cli.cmd_timer(["status"]) == 0
        assert capsys.readouterr().out == "timer: tea -- done\n"

    @pytest.mark.parametrize("body", [
        "[]", "null", '"x"', '{"end_ts": 1e400}', '{"end_ts": NaN}',
        '{"end_ts": "soon"}', '{"end_ts": [1]}', "{torn",
    ])
    def test_status_of_a_bad_file_is_off_not_a_traceback(
            self, timer_dir, capsys, body):
        from dsterm import cli

        (timer_dir / ".dsterm").mkdir()
        (timer_dir / ".dsterm" / "timer.json").write_text(body)
        assert cli.cmd_timer(["status"]) == 0
        assert capsys.readouterr().out == "timer: off\n"

    @pytest.mark.parametrize("argv", [["soon"], ["0"], ["241"], ["nan"]])
    def test_a_bad_duration_is_refused(self, timer_dir, argv):
        from dsterm import cli

        with pytest.raises(SystemExit) as exc:
            cli.cmd_timer(argv)
        assert exc.value.code == 1
        assert not (timer_dir / ".dsterm" / "timer.json").exists()


# -- dsterm lang and card --lang ------------------------------------------------

class TestLang:
    def test_an_unknown_argument_is_refused(self, capsys):
        from dsterm import cli

        with pytest.raises(SystemExit):
            cli.cmd_lang(["--tempate"])
        assert "unknown argument '--tempate'" in capsys.readouterr().err

    def test_the_template_fits_and_names_only_real_keys(self, capsys):
        import re
        from dsterm import cli

        assert cli.cmd_lang(["--template"]) == 0
        text = capsys.readouterr().out
        assert len(text.encode("utf-8")) <= cli.LANG_FILE_MAX
        assert "son, modèle" in text and "English" in text
        keys = [line.split("=", 1)[0].strip() for line in text.splitlines()
                if "=" in line and not line.startswith("#")]
        assert len(keys) == len(set(keys))
        lang_c = REPO / "dsclient" / "source" / "lang.c"
        if not lang_c.is_file():
            pytest.skip("no ROM source in this tree")
        table = lang_c.read_text(encoding="utf-8").split("KEY_NAMES", 1)[1]
        names = set(re.findall(r'\]\s*=\s*"([a-z0-9_.]+)"', table.split("};", 1)[0]))
        assert set(keys) - {"code", "label", "name"} <= names
        for key in ("compose.focus.head", "compose.focus.keys",
                    "compose.mic.latch", "guide.title"):
            assert key in keys

    def _card(self, tmp_path, monkeypatch):
        from dsterm import cli

        monkeypatch.setattr(cli, "checkout", lambda: None)
        monkeypatch.setattr(cli, "extensions", lambda: [])
        card = tmp_path / "card"
        card.mkdir()
        (card / "dsterm.nds").write_bytes(b"the app already there")
        return card

    def test_a_file_over_8_kb_is_refused_with_the_reason(
            self, tmp_path, monkeypatch, capsys):
        from dsterm import cli

        card = self._card(tmp_path, monkeypatch)
        big = tmp_path / "fr.lang"
        big.write_bytes(b"#" * (cli.LANG_FILE_MAX + 1))
        with pytest.raises(SystemExit):
            cli.cmd_card([str(card), "--lang", str(big), "--no-eject"])
        assert "reads at most 8 KB" in capsys.readouterr().err
        assert not (card / "dsterm" / "lang").exists()

    def test_more_languages_than_the_ds_loads_is_warned(
            self, tmp_path, monkeypatch, capsys):
        from dsterm import cli

        card = self._card(tmp_path, monkeypatch)
        files = []
        for code in ("fr", "de", "it"):
            f = tmp_path / f"{code}.lang"
            f.write_text(f"code = {code}\n")
            files += ["--lang", str(f)]
        assert cli.cmd_card([str(card), *files, "--no-eject"]) == 0
        err = capsys.readouterr().err
        assert f"loads at most {cli.LANG_CUSTOM_MAX}" in err
        capsys.readouterr()
        card2 = tmp_path / "card2"
        card2.mkdir()
        (card2 / "dsterm.nds").write_bytes(b"x")
        assert cli.cmd_card([str(card2), *files[:2], "--no-eject"]) == 0
        assert "loads at most" not in capsys.readouterr().err
