# Architecture

This page is for a developer who has just cloned the repository. It describes
every component, how the two halves talk, the flows that tie them together,
and where to make a given change. The wire format itself is specified in
[PROTOCOL.md](PROTOCOL.md). Setup and day-to-day use are in the
[README](../README.md) and [GETTING_STARTED.md](GETTING_STARTED.md).

## The big picture

The project has two halves, and the split between them is the design.

- **The server** (`dsterm/`, Python 3.11+) runs on the computer. It owns the
  Claude Code process, the terminal emulation, the scrollback history and the
  repository. It decides what the screen looks like.
- **The client** (`dsclient/`, C, built with BlocksDS) runs on a Nintendo DS
  family console. It draws the cells it is sent and sends key bytes back. It
  holds no files, no history and no shell.

The smallest target, an original DS, has a 67 MHz ARM9, 4 MB of RAM and no
MMU. Keeping the emulator, the history and every decision about meaning on the
computer is what makes the handheld side small enough to be correct. It is
also the security model: the protocol gives the handheld the PTY and nothing
else, so a client under an attacker's control can type only what a person at
the keyboard could type.

```
                 computer                                         handheld
+------------------------------------------------+      +-------------------------------+
| tmux -L dsterm  (pinned to COLS x ROWS)        |      | dsterm.nds  (C, BlocksDS)     |
|   window: claude     window: ...               |      |                               |
|        ^ key bytes          | terminal output  |      |  boot.c     Wi-Fi, discovery, |
|        |                    v                  |      |             pairing           |
| dsterm serve  (one select loop, 15 Hz tick)    |      |  main.c     main loop, input, |
|   session.py   PTY + pyte, 5000-line history   |      |             message dispatch  |
|   grid.py      cells, diff, scroll detection   |      |  proto.c    framing, codecs   |
|   server.py    auth, capabilities, pacing,     | TCP  |  seal.c     encrypt + tag     |
|                SEAL                            |<---->|  render.c   top screen        |
|   protocol.py  framing and codecs              | 8347 |  subscreen.c bottom screen    |
|   discovery.py UDP responder                   |<---->|  chime.c -> sound.c -> PSG /  |
|   dashboard.py 127.0.0.1:8348, read-only       | UDP  |             PCM channels      |
|   worker threads: git facts, clipboard,        | 8346 |                               |
|     claude agents --json, scrollback captures, |      |                               |
|     saving pictures                            |      |                               |
+------------------------------------------------+      +-------------------------------+
```

### The computer side

- `dsterm serve` starts the child (by default `claude`) on a PTY. The installed
  service runs it inside a dedicated tmux server (`tmux -L dsterm`), pinned to
  the grid size with `window-size manual` and no status line. The session
  therefore outlives the server process, and `dsterm attach` joins the same
  session from the desktop.
- Everything the PTY prints is fed to a pyte `HistoryScreen` that keeps 5000
  lines of history (`session.py`).
- Fifteen times a second the server snapshots the emulator into a `Grid` and
  compares it with the per-client mirror of what the handheld last received.
  Changes go out as `FRAME_DIFF` messages of at most 90 cells. A scroll is
  recognised and sent as a three-byte `SCROLL` plus a residual diff whenever
  that is cheaper than the plain diff.
- One `selectors` loop serves the listening socket, the client sockets, the PTY,
  the UDP discovery socket and the camera and view sockets. Anything slow or
  unbounded runs on a worker thread and is collected by the tick: git queries
  for the PROJECT page, the Windows clipboard read, `claude agents --json` for
  REMOTE and STATE, scrollback captures, and saving a picture. The loop's own
  tmux questions are short and bounded instead: the window list and the
  pane's mode, each at most once a second with a 2-second timeout, and a
  REMOTE pick's switch. The slash-command list is re-read every 15 seconds, a
  walk capped at 2,000 entries.
- Ports: TCP 8347 for the protocol, UDP 8346 for discovery, and a read-only
  dashboard on `127.0.0.1:8348` (the service passes `--dashboard-port 8348`)
  that refuses to bind anything but loopback.

### The handheld side

- The same ROM, `dsterm.nds`, runs on every DS family console. It boots into a
  menu that joins Wi-Fi, finds the server, pairs with it and connects.
- The top screen is the terminal: a 16-bit bitmap background painted with a
  6x8 font at 42x24 (4x6 at 64x32, 5x7 at 51x27).
- The bottom screen is a second bitmap. `SELECT` cycles its modes (CONTROL,
  KEYS, STATUS, PROJECT); SETTINGS, the composer and the pickers are reached
  from Control screen tiles. `X` returns to CONTROL from any other mode.
- A single loop runs once per VBlank: wait for the frame, read input, drain the
  socket, apply every complete message, draw what changed.

## Repository map

### Top level

| Path | Role |
|---|---|
| `dsclient/` | The handheld client: C sources, Makefile, host test suites, sounds |
| `dsterm/` | The server package, installed as the `dsterm` command |
| `tests/` | The server's pytest suite |
| `tools/` | Code generators, the C/Python cross-check, and diagnostic scripts |
| `docs/` | The guides. `docs/design/` holds the design notes written while each part was built ([index](design/README.md)); `docs/screens/` is generated by `tools/screenshots.sh` |
| `bin/dsterm` | Runs `dsterm.cli` from a checkout, with the checkout's `.venv` if present |
| `.github/workflows/` | CI (`ci.yml`) and releases (`release.yml`) |
| `pyproject.toml` | Package metadata. `pyte` and `wcwidth` are the only runtime dependencies, pinned exactly |
| `dsterm.snippets.example` | An example snippets file |

### Client build: `dsclient/`

| Path | Role |
|---|---|
| `Makefile` | BlocksDS build. `GRID_COLS`/`GRID_ROWS` (default 42x24) become `-DPROTO_COLS`/`-DPROTO_ROWS`. Output is `dsterm-COLSxROWS.nds`, copied to `dsterm.nds`. `EXT=name` compiles `source/ext_name.c` in place of `ext_none.c` |
| `arm7/source/main.c`, `Makefile.arm7` | The ARM7 core (the DS's second CPU, which runs the Wi-Fi driver and sound): BlocksDS's default core with Wi-Fi updates guarded against nesting, which otherwise corrupts memory at random on a DSi, plus the camera-LED handler on `FIFO_USER_01` (the LED is the power manager's I2C, which is the ARM7's). Built by `Makefile` |
| `run_tests.sh` | The host test runner (see [Tests and CI](#tests-and-ci)) |
| `sounds/` | Mastered WAVs and `sounds.txt` (one line per take: bank, file, gain) |
| `tests/` | Host suites. `tests/shot/nds.h` is a stand-in libnds that draws into arrays, used for screenshots |

### Client sources: `dsclient/source/`

Five files include libnds: `main.c`, `boot.c`, `photo.c`, `render.c` and
`subscreen.c`. Everything else is plain C with no platform dependency and is
compiled into the host test suite (`tests/sources.txt`, and `ext_none.c` by
`run_tests.sh`). `render.c` and `subscreen.c` are compiled on the host too,
against a stand-in libnds, by the screenshot harness; `main.c`, `boot.c` and
`photo.c` run only on the DS.

**Platform glue**

| File | Role |
|---|---|
| `main.c` | Entry point and main loop. Owns the socket, `handshake()` and `reconnect()`, input routing (`poll_input`, `poll_touch`, `focus_input`), Control tile actions (`control_press`), message dispatch (`handle_message`), the mapping from the sound conductor to hardware channels (`sound_apply`), loading custom sounds, and where the screen goes when the camera closes. `frame_wait()` lives here |
| `photo.c` | The PHOTO screen: the DSi sensor's TWL buffers and NDMA ping-pong, every state transition, the preview's pacing and a picture's upload. It reaches `main.c` only through `host.h`, and the wire only through `host_send_camera()`, which takes the camera's message types and nothing else |
| `boot.c` | Boot menu, Wi-Fi wizard (scan, passphrase, DHCP with a static fallback), discovery (broadcast and subnet sweep), the pairing exchange, migration of a legacy `/dsterm.cfg`, and the "other computers" half of REMOTE |
| `render.c` | Top screen: cell, cursor and scroll drawing, and overlays (boot menu, composer, pickers, pages). Picks the font by cell size |
| `subscreen.c` | Bottom screen: the layout of every mode, drawn from the geometry tables in `kbd.c`, `settings.c` and `status.c` |

**Connection and security**

| File | Role |
|---|---|
| `proto.c`, `proto.h` | Frame reader, every payload codec, the HELLO payload builder, the message type enum and the grid geometry defaults. Must stay byte-identical to `dsterm/protocol.py` |
| `seal.c` | SEAL: every frame after `AUTH_OK` encrypted and given an 8-byte HMAC tag over the ciphertext ([SEAL.md](SEAL.md)) |
| `chacha20.c` | ChaCha20 (RFC 8439), the cipher SEAL encrypts with |
| `sha256.c` | SHA-256, HMAC-SHA256, HKDF-SHA256 and a constant-time compare |
| `x25519.c` | X25519, derived from TweetNaCl |
| `pair.c` | Pairing by numeric comparison: the commitment, the six-digit code, the HKDF derivation and both acceptance MACs |
| `entropy.c` | A SHA-256 entropy pool. The DS has no RNG, so the pool is stirred by touchscreen ADC noise, button timing, the RTC, the Wi-Fi MAC and scan results. It makes the pairing key and, at every connection, the card nonce SEAL needs (`boot_connect_nonce()` in `boot.c`) |
| `profiles.c` | The card's store under `/dsterm/`: `wifi.cfg`, `servers/<name>.cfg`, `last`, `id`. Writes go to a temporary file first |
| `config.c` | The `key = value` parser, the resolved connection settings (`dsterm_config_t`), and key parsing |
| `discover.c` | Discovery datagrams, the found-server list, and the sweep's next address |
| `bootui.c` | The boot menu's pure half: text fields, lists, the countdown, every label |
| `console.c` | Console family detection from firmware bytes, and Wi-Fi advice that fits the console in hand |
| `backoff.c` | Reconnect delay: half a second, doubling to fifteen |

**Screens and input**

| File | Role |
|---|---|
| `kbd.c` | Touch keyboard (10x5, three layers, one-shot Ctrl and Shift, key repeat), the Control screen's tile rectangles, and the key word built from `REG_KEYINPUT` plus libnds's X and Y |
| `focus.c` | L+R focus mode: the d-pad walks a highlight over tiles or keys. Traversal is derived from `kbd.c`'s rectangles |
| `status.c` | The bottom-screen mode machine, STATUS tabs, and the link, counter and view-position lines |
| `settings.c` | SETTINGS: its five tabs (SESSION, SOUND, SCREEN, LINK, BUTTONS), rows and option chips, scrolling for a tab longer than the screen, the theme colours, the page render, and `/dsterm-settings.cfg` |
| `buttons.c` | What A, B, X, Y and START send (Enter, Space, Delete, Esc, Tab, Stop), the rule that Enter stays on a button, and the legend the Control screen shows |
| `log.c` | The 24-line log ring the STATUS screen shows |
| `keys.h` | Key sequences the card sends (Page Up, Page Down) |
| `font.h`, `font5x7.h`, `font4x6.h` | Generated by `tools/genfont.py` |

**Sound**

| File | Role |
|---|---|
| `sound.c` | The conductor. Two styles: SOFT (`SOUND_STYLE_SNES` in the code: samples on two PCM lanes, one for cues and one for ticks) and CHIP (one square-wave voice, the default). The TYPING setting gates the user's own key and cursor clicks. Three profiles (Studio, Companion, Field), priorities, cooldowns, no queueing, take rotation and pitch variation |
| `sfx_assets.c`, `.h` | Generated by `tools/sfx_build.py`: the built-in samples and bank table. Do not edit |
| `wav.c` | Reads custom WAVs from `/dsterm/sounds/` into 16-bit mono PCM |
| `chime.c` | Turns observed facts (a CR sent, cells changed, the link up or down, STATE edges) into cues and the Control screen's headline |

**Features**

| File | Role |
|---|---|
| `sessions.c` | SESSIONS staging: a list replaces the visible one only when complete (the ABOUT block's session name and the new-output cue use it) |
| `remote.c` | REMOTE staging, the other-computers list, row text, and what `A` does on a row |
| `quick.c` | QUICK: Claude's mode read off its footer, the slash command list, recent prompts (and their file, `/dsterm/recent.txt`), and lines typed on KEYS |
| `camera.c` | The camera's pure half: the preview's 2x2 box and sixteen greys, the YUYV maths, both 4:2:0 packers, the CRC-32, the pacer and the upload window, the shutter and the strip text. `dsterm/camera.py` is its mirror |
| `privacy.c` | HIDE ADDRESSES: stars out IP and MAC addresses wherever the card draws text |
| `mirror.c` | The bottom screen as `dsterm view` receives it: `subscreen.c` records each glyph and fill into a 42x24 grid of cells as it draws, and while a view is watching the card sends that grid when it changes |
| `composer.c` | The 1024-byte compose buffer and its send plan: `KEY` frames of up to 64 bytes, then a CR in its own frame |
| `snippets.c` | The `/dsterm.snippets` parser, pushed sets, and the only API that takes a snippet body toward the PTY (load into the composer) |
| `clip.c` | Desktop clipboard chunks, reassembled and summarised |
| `project.c` | The PROJECT page: tabs NOW, DIFF, DO, WATCH, HEALTH, LOG, its rows and its DO actions |

**The seam**

| File | Role |
|---|---|
| `ext.h`, `ext_none.c` | Every hook an extension can provide, and the empty implementation that ships |
| `host.h` | The read-mostly services `main.c` lends to other files. No socket, no key, no writable grid; the one send is the camera's, of camera types only |

### Server: `dsterm/`

| File | Role |
|---|---|
| `cli.py` | The `dsterm` command: `setup`, `pair`, `devices`, `doctor`, `start`/`stop`/`restart`/`status`/`logs`, `serve`, `screen`, `camera`, `view`, `attach`, `wsl-setup`, `card`, `sounds`, `lang`, `timer`, `install-service`, `install-card`, `build`, `test`. Defines `CliExtension` |
| `serve.py` | `dsterm serve`: arguments, tmux wrapping (`tmux_command`), the child's API-key and env files, extension loading, the dashboard |
| `awake.py` | Keeps the computer from idling to sleep while a handheld is connected (`caffeinate` on macOS, `systemd-inhibit` on Linux); `--no-keep-awake` turns it off |
| `server.py` | The core of the `Server`: the select loop, accept (at most two connections), handshake and takeover, SEAL, dispatching each message, the paced outbox, and the tick that turns the session into frames |
| `connection.py` | `ClientState`, one card's connection, and the records the outbox carries. The connection's own fields, then one part per feature, which only that feature and the core touch |
| `server_camera.py` | Photos, the live view, the feed socket and the tmux popups |
| `server_voice.py` | Push-to-talk: assembling a card's recording, one transcription per card on a worker thread, the watchdog that answers a card whose transcript is late |
| `server_pairing.py` | The pairing exchange and the per-device key a connection's AUTH is checked against |
| `server_scrollback.py` | Scrolling back: the view offset, the tmux capture worker, `VIEW_POS` |
| `server_mirror.py` | `dsterm view`'s glue: `MIRROR`, `TOUCH`, and what the view hub's keys and screens do |
| `server_sessions.py` | The SESSIONS list from tmux's windows, and switching between them |
| `server_agents.py` | REMOTE and STATE from `claude agents`, and the picks that act on them |
| `server_feeds.py` | MODE, snippets, slash commands, the PROJECT page and the clipboard |
| `server_ext.py` | Registering an extension and queuing its frames |
| `protocol.py` | Framing, every codec and constant, and the SEAL primitives. The reference for the wire |
| `session.py` | The child on a PTY, the pyte screen and history, `view(offset)`, the cursor, and `app_scrolls` (asks tmux whether the inner program is on the alternate screen). Multiplexed history is tmux's: the same poll reads `history_size`, and `capture_view` replays a `capture-pane` window through a fresh pyte screen |
| `sessions.py` | tmux window listing and selection, for the SESSIONS list and REMOTE's switches. Rows resolve through tmux window ids, never through an index alone |
| `grid.py` | `Cell` and `Grid`, the geometry (`DSTERM_COLS`/`DSTERM_ROWS`, default 42x24), colour quantisation to the 10-colour palette, glyph folding, diff, scroll detection |
| `render.py` | Draws a `Grid` in a desktop terminal using only what the DS can draw |
| `remote.py` | `claude agents --json`: parsing, row building, pick actions, the poller thread, and the STATE mapping |
| `commands.py` | The computer's own slash commands for QUICK: names from `.claude/commands` and `.claude/skills`, in the project and the home folder |
| `auth.py` | The legacy key file, nonces, and constant-time MAC verification |
| `pairing.py` | The pairing window (a mode-600 file written by `dsterm pair`, which also carries the code and the person's answer), the commitment, the code, the HKDF derivation, per-device keys |
| `x25519.py` | X25519 in pure Python |
| `chacha20.py` | ChaCha20 in pure Python, the card's `chacha20.c` on the server |
| `discovery.py` | The UDP responder: strict parsing, at most 20 answers a second |
| `project.py` | Read-only git facts for the PROJECT page, plus WATCH (`dsterm timer`, `.dsterm/watch.json`, GitHub Actions under `--project-ci`), HEALTH and LOG, collected on a background thread; 48 ASCII rows for a card without `project64` |
| `actions.py` | Discovered DO-tab buttons: your own (`.dsterm-buttons`, `~/.config/ds-claude-terminal/buttons`), package scripts, Makefile targets, Python tests. Names are validated, not quoted |
| `snippets.py` | Server-pushed snippets, under the same limits the card enforces |
| `clip.py` | The Windows clipboard via PowerShell under WSL, folded to ASCII. Content a password manager marks as concealed never leaves the process, and the text is never logged. Off unless `--clip` |
| `dashboard.py`, `dashboard.html` | The loopback-only status page |
| `doctor.py` | `dsterm doctor`: every precondition for a connection, checked in order |
| `wsl.py` | `dsterm wsl-setup`: Hyper-V firewall rules (mirrored mode) or a port proxy (NAT mode) |
| `sounds.py` | Cue names and the card's WAV rules, for `dsterm sounds` and `dsterm card --sounds` |
| `extmap.py` | Generated by `tools/genfont.py`: Unicode code point to wire byte for the extended glyphs |
| `client.py` | The reference client. It does what the DS does, no more; used by the tests and `dsterm screen` |
| `peek.py` | `dsterm screen`: the screen from the local dashboard, which leaves the DS connected; for another computer, it connects as the DS would |
| `camera.py` | The camera's computer half: the preview decoder, the planar-420 to RGB and PNG converters, the photo store (`<project>/.dsterm/photos`, symlink-safe), the mode-600 feed socket, the half-block renderer and the tmux popup controller |
| `voice.py` | The IMA ADPCM codec, the utterance assembler, and speech-to-text: choosing the one backend at startup (local first; cloud only on `--stt-cloud`), running it under a deadline, folding the transcript to ASCII |
| `stt_worker.py` | A Python Whisper library in a process of its own, so its model stays loaded between recordings and a hung call can be killed |
| `view.py` | The server's end of `dsterm view`: the mode-600 view socket, both screens out as JSON lines, keys and taps in, every line bounded |
| `viewer.py` | `dsterm view`: both screens framed like the device, top over bottom, as pictures through the terminal's image protocol where it has one and as text where it does not; the camera's section, clicks on the bottom screen as taps, Ctrl-] for its own keys |
| `dsscreen.py` | The DS's screens as 256x192 pictures, drawn with the card's own fonts, palette and caret rules; PNG, and the kitty and iTerm2 image protocols |
| `dsfont.py` | The card's bitmap fonts, copied from `dsclient/source/font*.h` by `tools/genviewfont.py` (`tests/test_dsscreen.py` checks the copy) |
| `viewwindow.py` | `dsterm view --window`: both screens at real pixels in a tkinter window, scaled whole-number times |
| `camwindow.py` | `dsterm camera --window`: the live view in a tkinter window, whole-number scaled so the pixels stay sharp |
| `keygen.py` | The server's shared key, made by `dsterm setup`, for a card set up by typing it rather than pairing |
| `data/` | The systemd unit, the launchd plist, and the tmux configuration |

How the features share the server: each `server_*.py` is a mixin with its
own state, set in its `_init_<feature>()`, and the core calls its hooks --
start, stop, a tick, a card's connection ending -- rather than touching that
state. Each card's state is in parts too (`connection.py`), one a feature,
and no feature touches another's part. Only `camera_view` is read outside its
owner (the dashboard's badge and the view's status line); everything else one
feature needs from another is a method call, and `tests/test_layout.py` lists
every such call with its reason and fails on one that is not listed. The
heaviest crossings are between SESSIONS and REMOTE/STATE, which share tmux's
window catalogue, and a window switch, which every other feature waits for.

### Tools: `tools/`

| File | Role |
|---|---|
| `crosscheck.py` | Reads the `INTEROP` lines the C suite prints and recomputes each value with the server's own code |
| `fuzz.py` | Runs `tests/test_fuzz.py` with fresh seeds for as long as asked, and prints any seed that fails |
| `measure.py` | Latency, CPU, memory, bandwidth, recovery and PINGs under output floods, under a combined workload ([PERFORMANCE.md](PERFORMANCE.md)) |
| `soak.py` | Watches a live server through a long session with a real DS: logins, why each ended, time back, the camera, memory |
| `sfx_build.py` | Encodes `dsclient/sounds/` into `sfx_assets.c`/`.h`. `--check` fails if they are stale |
| `genfont.py` | Builds the C fonts and `dsterm/extmap.py` from a BDF, and prints glyphs as ASCII art for review |
| `genviewfont.py` | Copies the C fonts into `dsterm/dsfont.py` for `dsterm view`'s pixel view; run it after `genfont.py` |
| `extglyphs.py` | The extended glyph table (box drawing, blocks, marks): the single source both ends are generated from |
| `checkglyphs.py`, `unknown.py` | Which characters the font has, and which ones a real session draws that the wire cannot carry |
| `geomcheck.py` | Runs a scratch server at a candidate geometry and checks every frame against the 536-byte ceiling |
| `screenshots.sh` | Compiles `render.c` and `subscreen.c` against `dsclient/tests/shot/nds.h`, runs `dsclient/tests/screens.c`, and writes `docs/screens/*.png` |
| `ppm2png.py` | Converts the screenshot PPMs to PNG with the standard library and no metadata |
| `release_zip.sh` | Builds both ROMs and the SD-card zip with `SHA256SUMS` |
| `measure_cells.py` | Runs a command at the grid size and reports the cells changed per frame: the measurement behind the cell-diff protocol |
| `spleen-5x8.bdf` | The font source (Spleen, BSD 2-Clause) |

### Tests and CI

| Suite | What it covers |
|---|---|
| `dsclient/run_tests.sh` | Builds the pure client sources (`dsclient/tests/sources.txt`) with `ext_none.c` and the suites in `dsclient/tests/` (`test_core.c`, `test_connect.c`, `test_remote.c`, `test_seal.c`, `test_camera.c`, `test_sound_snes.c`, and `test_fuzz.c`, seeded fuzzing of every parser that reads outside bytes, each input in an exact-size heap block so a one-byte over-read is caught). Compiled with `-Wall -Wextra -Werror` under ASan and UBSan with recovery disabled (UBSan alone on a host whose ASan is broken), at both 42x24 and 64x32. Then pipes the output to `tools/crosscheck.py`. If `dsclient/tests/sources.ext.txt` exists, an extension's suite is built as well |
| `tests/` | pytest. The end-to-end tests use a real PTY, real sockets and a real tmux server, including pairing, discovery, SEAL, scrollback and the camera feed; `claude agents`, PowerShell's clipboard and some tmux answers are faked where a test needs a particular answer. `test_fuzz.py` throws seeded noise at the decoders, SEAL, the server's own terminal emulator, a live server's card, pre-authentication and view inputs, and a program printing noise through a real PTY (the `claude agents` answer, the snippet file and the discovery probe have tests of their own, not fuzzing). `conftest.py` keeps tests off the production tmux socket |
| `dsterm test` | Runs both |

`.github/workflows/ci.yml` runs on every push and pull request with three
jobs: the pytest suite on Python 3.12 with tmux installed; the host C suite and
cross-check under a real gcc; and the ROM, built in the BlocksDS container at
64x32 and then 42x24. `release.yml` runs on a `v*` tag: it builds the card zip
in the container, builds the wheel, and publishes both with their SHA-256 sums.

## Key flows

### Boot to a live screen

1. `main()` installs the exception handler, clears both grids, brings up both
   screens, and mounts the SD card. A card that cannot be read halts with
   advice that depends on how the console started the ROM.
2. Settings (`/dsterm-settings.cfg`), custom sounds (`/dsterm/sounds/`) and
   snippets (`/dsterm.snippets`) are loaded. None of them is required.
3. `boot_run()` loads the profile store, migrating a legacy `/dsterm.cfg` if
   that is all there is. A card that has connected before counts down three
   seconds on its last server; any button returns to the menu.
4. **Wi-Fi.** The wizard scans, the person picks a network and types its
   passphrase on the touch keyboard, and the console asks for DHCP, offering a
   static address if that fails. `console.c` explains what to do when a console
   in DS mode meets a WPA2 network.
5. **Discovery.** The card broadcasts `DSTM probe 1 id=<hex>` to UDP 8346, and
   every server answers with its name, TCP port and whether pairing is open.
   Broadcast does not cross WSL2's NAT, so the card can also sweep its /24 with
   TCP connects to port 8347; an address that sends a `NONCE` is a server.
6. **Pairing.** On the computer, `dsterm pair` opens a two-minute window. The
   card creates its device id once, draws an X25519 private key and a nonce
   from the entropy pool, and runs `PAIR_HELLO` / `PAIR_REPLY` (the server's
   key and a commitment to its nonce) / `PAIR_CONFIRM` (the card's nonce) /
   `PAIR_OK` (the server's nonce, checked against the commitment). Both ends
   now show the same six-digit code, and a person confirms it on each: A on
   the DS sends `PAIR_ACCEPT`, `y` at `dsterm pair` lets the server answer
   `PAIR_DONE`. Both ends derive the same 16-byte device key and neither sends
   it. Only then does the card save `/dsterm/servers/<name>.cfg` and the server
   `devices/<id>.psk`.
7. **Connect.** `net_connect()` opens a non-blocking socket and `handshake()`
   sends `HELLO` (capability tokens, `id=<hex>` and `cn=<hex>`, a fresh nonce
   from `boot_connect_nonce()`), reads the `NONCE`, answers with
   `AUTH = HMAC-SHA256(key, nonce)` (the device key, or the legacy shared key
   on a card that was never paired), and reads `AUTH_OK`, which must carry
   `seal3`: `seal_start()` turns on encryption and per-frame tags under keys
   that also cover the `HELLO` it sent, card nonce and all.
8. **Server, after AUTH_OK.** Any older authenticated connection is dropped
   (the reconnecting device takes over). The server starts `MODE`, the
   snippets, a repaint as a paced batch of diffs plus `CURSOR`, the session
   list and, for cards that asked, the clipboard, an empty REMOTE set ("not
   asked yet"), the current STATE, the slash commands, MIRROR and
   CAMERA_READY. The small ones go at once and the repaint and lists through
   the paced outbox, so the card handles them in whatever order they land.
9. A connection that fails returns to the boot menu with the reason. Once
   connected, a lost link calls `reconnect()`, which clears every
   connection-scoped capability and retries with backoff while the last screen
   stays visible.

### A keypress, round trip

1. `poll_input()` builds the key word from `REG_KEYINPUT` and libnds, computes
   edges, and routes them: an open overlay (composer, session picker, REMOTE,
   an extension's menu) takes everything; then the L+R focus chord; then local
   navigation (`X` back, `SELECT` cycle, the pages); then the button map
   (`buttons.c`, set in SETTINGS -> BUTTONS: by default `A` Enter, `B` Delete,
   `X` Esc, `Y` Tab, `START` stop) and the d-pad with repeat. Touch goes through `poll_touch()`, which ignores the first frame of
   every contact because the touchscreen reports a bad coordinate on pen-down.
2. A touch on the keyboard becomes bytes in `kbd_press()`; `send_key_seq()`
   frames them as `KEY` (at most 64 bytes), and `send_all()` seals the frame
   (encrypts it, then tags it) if SEAL is on. A lone CR also tells the chime a line was sent.
3. On a sealed connection the server's `FrameReader` checks the tag before
   anything else, then decrypts. `_handle()` routes the `KEY` to `_on_key()`, which decodes
   it, returns a scrolled-back view to live, and `Session.write()` puts the
   bytes on the PTY. tmux passes them
   to the child.
4. The child's output wakes the select loop; `Session.pump()` feeds it to pyte.
5. On the next tick, `_tick()` snapshots the grid, detects a scroll or computes
   a diff against the client's mirror, queues `SCROLL` and `FRAME_DIFF`
   messages, queues `CURSOR` if the caret moved, and `_flush_outbox()` releases
   at most four messages per tick. `_send_to()` seals each one.
6. The card's main loop drains the socket into its frame reader, verifies each
   tag and decrypts with `seal_open()`, and `handle_message()` applies the diff (every
   position is validated before any cell is written) and redraws only the
   cells that changed.

### A sound cue

1. A fact arrives: for example `STATE` "needs approval", decoded in
   `handle_message()` and passed to `chime_claude_state()`.
2. `chime.c` turns the edge into a cue (`SOUND_APPROVE`) and calls
   `sound_cue()`. SENT always follows a submitted CR. Without STATE (an older
   server), the chime infers the rest from traffic: THINK after 0.75 s of quiet
   following a send, OUTPUT when sustained output starts (voiced by the CHIP
   style, or by a custom `output.wav`), DONE after 2.5 s of quiet following
   output.
3. `sound.c` applies the rules: the profile may not voice that cue, a cue still
   in its cooldown is dropped, and a lower-priority cue never interrupts a
   higher one. Nothing is queued. In the SOFT style the cue picks a take from
   its bank (never the one just played) and one of three pitch variations, and
   is staged on the cue lane; key ticks and blips use the tick lane, so typing
   never cuts off a cue. A bank with no takes falls back to the CHIP voice.
4. The next `frame_wait()` calls `sound_apply()`, which reads what changed with
   `sound_tick_voice()` and `sound_take_pcm()` and drives the hardware: PSG
   channel 8 for the square-wave voice, channels 0 and 1 for the two PCM lanes.

### A REMOTE pick

1. The REMOTE tile opens the picker. The card sends `REMOTE_OPEN 1` only if a
   `REMOTE` frame has already arrived on this connection.
2. While any card has the screen open, the server asks `claude agents --json`
   on a worker thread every three seconds and sends a new set when the answer
   changes: one row per frame, committed by the card when the last row lands.
3. `A` on a row sends `REMOTE_PICK` (row index, action). The server maps the
   index back to the row it advertised, re-checks the action against that row's
   flags, and allows one pick per second. Open runs `claude attach <id>` in a
   new window of the tmux session, or selects the window it is already in; copy
   (confirmed by a second `A`) runs `claude --resume <id> --fork-session`; new
   runs the server's own command. Every argument comes from the server's own
   reading, never from the wire.
4. The server switches windows behind a switch barrier, so the card receives
   the new screen, session list and mode in order, and queues its
   `REMOTE_NOTE` behind the barrier: the card hears "opening x" after it can
   see x.
5. The other half of the picker, other computers, is the card's own: discovery
   on the LAN, and a switch is an ordinary reconnect (pairing first if needed).

Details: [REMOTE.md](REMOTE.md).

### STATE

The same `claude agents` reading answers what the session in the selected tmux
window is doing: idle, working, waiting for an approval, or waiting for an
answer. The server polls every 1.5 s while the session is busy or waiting or a
card has typed in the last 30 s, and every 5 s otherwise, and only while a card
that announced `state` is connected. It sends `STATE` once after
authentication and then on change. On the card, STATE edges drive THINK,
APPROVE, QUESTION and DONE, the Control screen headline says which button
answers, and while Claude is waiting the Control screen's up and down go to
the prompt's choices instead of scrolling. One reminder plays after 90 seconds
of waiting.

### A picture

1. QUICK's **PHOTO** row opens the camera screen — only on a DSi-mode card,
   and only where `CAMERA_READY` has already landed on this connection (the
   capability proof; its bit 0 is whether this computer can save pictures).
   The overlay sits right after `quick_menu` in `poll_input()` and owns every
   control while it is up.
2. The ARM9 starts a PREVIEW transfer over NDMA channel 1 into one of two
   `TWL_BSS` ping-pong buffers, sends `CAMERA 2`, and each loop turns a
   finished transfer into the shown frame (`DC_InvalidateRange` first — the
   cache holds stale lines) while the next fills the other. The pacer sends
   one 128x96 sixteen-grey `CAMERA_FRAME` at a time, at most every twenty
   frames (3 fps).
3. The shutter fires: the running transfer drains, a CAPTURE fills
   `cam_yuyv` (614,400 bytes), `camera_photo_thumbnail` makes the review
   still, and REVIEW shows it with one STILL preview owed to the wire.
4. **A** packs the capture into 4:2:0 planar — FULL (460,800 bytes) or SMALL
   (115,200), the PICTURE setting — checksums it once, and the upload window
   (8 KB, under lwIP's 11,680-byte send buffer) feeds `PHOTO_CHUNK`s as the
   server's `CAMERA_ACK` credits arrive. When all bytes are out, `PHOTO_END`
   carries the format, the camera and the CRC; SAVING waits up to thirty
   seconds for the answer.
5. The server's worker thread writes the PNG — it alone picks the folder and
   the name — and answers `PHOTO_SAVED` with the path, which opens the
   composer as `Picture from the DS camera: <path> `. **A** sends the prompt;
   **X** sends `PHOTO_DISCARD` and the file goes away.
6. Every way out — B or the d-pad, X, SELECT, L+R, the lid, a dropped link —
   is one close path: drain the transfer, `cameraDeinit()`, the LED off,
   `CAMERA 0` if the link is live and proved, the buffers wiped.

## Design rules worth knowing before changing anything

- **Never `swiWaitForVBlank()` once Wi-Fi is up.** dswifi's network stack runs
  in a cooperative thread. A BIOS wait halts the CPU without yielding, so not
  one packet is processed; that single mistake once appeared as four different
  bugs. Every wait goes through `frame_wait()` (or `host_frame_wait()` outside
  `main.c`), which calls `cothread_yield_irq(IRQ_VBLANK)`. It is also the one
  place the sound hardware, sprites and the bottom screen are written.
- **Bounded buffers, no allocation in the hot paths.** Every buffer on the card
  is fixed-size and every length from the wire is checked before use. The frame
  reader refuses a declared length over 8192 from the header alone; a diff
  validates every position before writing any. The one use of the heap is
  loading custom WAVs at boot, under a fixed budget.
- **The geometry is a contract.** `GRID_COLS`/`GRID_ROWS` in the Makefile
  (`PROTO_COLS`/`PROTO_ROWS` in C) must equal `DSTERM_COLS`/`DSTERM_ROWS` on the
  server. A mismatch is an out-of-bounds write on a device with no MMU. The ROM
  is named for its geometry, the C suite runs at both shipped sizes, and the
  card announces its geometry in HELLO.
- **Server-supplied text never executes without a second press.** Pushed
  snippets, the clipboard and discovered DO actions are loaded into the
  composer, shown in full, and sent only by a deliberate `A`. `snippets.h`
  exposes no direct-send API, and `tests/test_repo.py` fails if one appears.
  God mode, a card-only setting that is off by default and cannot be set by the
  server, lets DO actions skip the composer; it never applies to the clipboard.
  SETTINGS rows that type (`/model`, `/compact` and so on) send build-time
  literals shown beside the button, which is the documented exception.
- **Draw and hit-test from one table.** Key and tile rectangles live in `kbd.c`,
  chips in `settings.c`, tabs in `status.c`. `subscreen.c` draws from them,
  `main.c` hit-tests against them, and `focus.c` derives its traversal from
  them, so a target cannot move on screen without its touch area moving too.
- **Pure where possible, and host-tested.** Only `boot.c`, `main.c`, `photo.c`,
  `render.c` and `subscreen.c` touch libnds. Logic goes in a pure module with a host test.
- **Unknown types are fatal; unknown tokens are inert.** Both frame readers drop
  the connection on a type they do not know, so new behaviour is negotiated
  with HELLO tokens, and a client never sends a new type until the server has
  proved it understands it ([PROTOCOL.md](PROTOCOL.md#the-capability-proof-rule)).
- **Nothing unbounded runs on the server's loop.** Git, the clipboard,
  `claude agents`, scrollback captures and saving pictures run on worker
  threads; the loop's own tmux questions are capped at 2 seconds each and
  asked at most once a second. Emulating the session's output, whose cost
  per byte the program chooses, works to an 8 ms budget a pass in 64-byte
  slices and keeps the rest for the next (`Session.pump`), so a PING is
  answered while a program floods the terminal (`tests/test_responsiveness.py`). Every server-to-client message fits one
  536-byte segment sealed (the MSS the DS reports; `tests/test_protocol.py`
  checks each type at its largest), releases are paced at four per tick, and
  a backlog larger than a full repaint is replaced by a fresh resync.

## Where to change things

| To... | Change | And also |
|---|---|---|
| Add a Control tile | `kbd.h` (`kbd_control_t`, `KBD_CONTROL_COUNT`), the rect table in `kbd.c`, the label in `subscreen.c`, `control_press()` in `main.c` | Tests in `dsclient/tests/test_core.c`; regenerate `docs/screens/` with `tools/screenshots.sh` |
| Add a sound cue | `sound_cue_t` in `sound.h`; a `cue_def_t` and `cue_bank()` in `sound.c`; `BANKS` in `tools/sfx_build.py` (in `sfx_bank_t` order); takes in `dsclient/sounds/sounds.txt` | Run `tools/sfx_build.py`; add the name to `CUES` in `dsterm/sounds.py` (`tests/test_sounds.py` checks they agree); trigger it from `chime.c` |
| Add a SETTINGS row | `settings_row_t` and `settings_t` in `settings.h`; the `ROWS` entry (label, note, options, tab) and parse/save in `settings.c` (bump `SETTINGS_VERSION`) | Apply it in `settings_apply()` / `settings_chip_press()` in `main.c`; eight rows are visible at once, ending above y=160, and a longer tab scrolls (`SETTINGS_VISIBLE_ROWS`) |
| Add a protocol message | `msg_type_t` in `proto.h`; `type_is_known()` and a codec in `proto.c`; `handle_message()` in `main.c`; `MsgType` and codec in `dsterm/protocol.py`; a route in `_handle()` (`server.py`) to an `_on_<message>()` in the feature's `server_*.py` | The type list and its justification in `tests/test_protocol.py`; an `INTEROP` vector and `tools/crosscheck.py` entry, or a pinned server vector in a C test; [PROTOCOL.md](PROTOCOL.md) |
| Add a HELLO capability | `PROTO_HELLO_CAPS` in `proto.h` (the handshake buffer is sized there); `FEATURE_*` in `protocol.py`; a field in `Caps` (`connection.py`) and the line in `_on_hello()` (`server.py`) that sets it | `hello_vector()` in `tools/crosscheck.py` |
| Change the grid size | `GRID_COLS`/`GRID_ROWS` and `DSTERM_COLS`/`DSTERM_ROWS` together | `tools/geomcheck.py COLSxROWS`; a font for the cell size from `tools/genfont.py` |
| Change the camera preview size | `CAMERA_PREVIEW_W`/`CAMERA_PREVIEW_H` in `camera.h` and `PREVIEW_W`/`PREVIEW_H` in `dsterm/protocol.py` together | `tools/camera_budget.py` says what the frames cost to sign and send |
| Add a glyph above ASCII | `tools/extglyphs.py` | Regenerate the fonts and `dsterm/extmap.py` with `tools/genfont.py` |
| Change the PROJECT page | Facts in `dsterm/project.py`, buttons in `dsterm/actions.py`, layout in `dsclient/source/project.c` | `tests/test_project.py`, `tests/test_actions.py` |
| Change snippet limits | `snippets.h` and `dsterm/protocol.py` | `tests/test_snippets.py` reads the C header |
| Add a CLI command | `COMMANDS` in `dsterm/cli.py` | `tests/test_cli.py` |
| Attach an extension | Implement `ext.h` in `source/ext_<name>.c` and build with `EXT=<name>`; server side, an object with a `token` registered under the `dsterm.extensions` entry point | It may send `EXTENSION` (0x99) to cards that announced its token, and add CLI commands through `dsterm.cli` |

An extension (a companion or a status display, say) can observe the session
and draw on the console, but the seam passes it no socket, no key and no
writable grid: it cannot type and it cannot reach the server.
