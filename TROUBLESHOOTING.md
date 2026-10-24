# Troubleshooting

Start with:

```sh
dsterm doctor
```

It checks everything the server side needs, in order, and prints one line
each. Every line below is one it can print. When asking for help, paste the
whole output: it contains no secrets (the key's presence and permissions,
never the key).

For the console side, see the error table in
[docs/HARDWARE.md](docs/HARDWARE.md#when-something-goes-wrong).

| `doctor` says | What to do |
|---|---|
| `[FAIL] python  3.11 or newer is required` | Install a newer Python and reinstall: `pipx reinstall ds-claude-terminal --python python3.12` |
| `[FAIL] key  no .../psk -- run: dsterm setup` | `dsterm setup`. It makes the key and never overwrites one |
| `[FAIL] key  ... is readable by others` | `chmod 600 ~/.config/ds-claude-terminal/psk`. The server refuses a key others can read |
| `[warn] config dir  ... readable by others` | `chmod 700 ~/.config/ds-claude-terminal` |
| `[FAIL] claude  Claude Code is not installed` | Install Claude Code and log in once in a normal terminal (`claude`) |
| `[warn] claude  ... not on this shell's PATH` | Fine for the service, which adds `~/.local/bin`; add it to your shell too |
| `[warn] claude  ... is not on the service's PATH` | An nvm or npm install the service cannot see. Run the `ln -s` it prints, then `dsterm restart` |
| `[warn] agents  not available ...` | Claude Code is too old to report what it is doing: `claude update`. Until then the DS guesses from the screen and REMOTE lists nothing |
| `[FAIL] tmux  not installed` | `sudo apt install tmux` / `brew install tmux` |
| `[warn] service  no systemd/launchd here` | WSL without systemd: run `dsterm serve` in a terminal and leave it open, or enable systemd in `/etc/wsl.conf` (`[boot] systemd=true`) |
| `[FAIL] service  ...: inactive` | `dsterm install-service`, then `dsterm start`; then `dsterm logs` if it stops again |
| `[FAIL] tcp  nothing answers on 127.0.0.1:8347` | The server is not running, or failed to start: `dsterm logs`. A "address already in use" there means another copy is running |
| `[warn] discovery  no answer on udp 8346` | Find servers will not see this PC. Enter an address on the console still works. Check nothing else holds UDP 8346 |
| `[FAIL] addresses  no LAN address found` | This machine is not on a network the console can be on |
| `[warn] wsl  NAT networking ...` | Run `dsterm wsl-setup`, and use **Scan this network** on the console. Better: switch WSL to mirrored mode |
| `[ ok ] wsl  mirrored networking -- run dsterm wsl-setup once` | If the console still cannot reach it, run `dsterm wsl-setup` (it is idempotent) |
| `[warn] wsl  these are WSL's internal addresses` | The console must use the Windows machine's LAN address (the script `wsl-setup` runs prints it) |
| `[ ok ] voice  Push-to-Talk: transcribed on this computer (...)` | Push-to-talk will work, with the backend named. `sent to ... (cloud ...)` instead means recordings go to that service |
| `[ ok ] voice  no local speech-to-text (optional: ...)` | Push-to-talk is off until you install one ([Voice](#voice), below). Not a fault |

## Things `doctor` cannot see

- **The DS shows "DS CLAUDE TERMINAL STOPPED".** A bug stopped the app.
  Photograph the screen (the numbers and the last log lines are what a fix
  needs) and open an issue with it, then switch the console off and on. The
  computer and the session are unaffected.
- **The session was gone after a while away.** The computer slept. The
  server keeps it awake while a DS is connected, but not with the lid closed,
  and not once the DS has gone (switched off or asleep). On WSL, Windows'
  own sleep settings decide.

- **The DS says "the computer runs an older dsterm: update it there"** (or
  "the computer does not encrypt its frames"). The server is from an older
  release.
  Update it (`pipx install --force` the new wheel, or `git pull` in its
  clone, then `dsterm restart`). The DS keeps its pairing and keeps
  retrying, so it connects by itself once the server is updated.
  Unencrypted connections are refused on purpose: on an open network an old
  server and an attacker look the same.
- **The DS says the server refused it: "this card's dsterm.nds is older than
  the computer's".** The other way round: the card has an old `dsterm.nds`.
  Copy the current one to it (`dsterm card PATH --rom dsterm.nds`). No
  re-pairing is needed.
- **"integrity check failed; reconnecting", over and over.** Something
  between the DS and the computer is altering traffic, or one side is a
  development build the other does not match. Update both to the same
  release.
- **The DS says the server refused it because of the grid size.** The ROM and
  the server disagree about 42x24 and 64x32. Either copy the matching
  `dsterm.nds` to the card, or set `--grid` in
  `~/.config/ds-claude-terminal/serve.conf` ([CUSTOMIZING.md](docs/CUSTOMIZING.md#a-bigger-grid)).
- **REMOTE says "Update dsterm on it to list sessions".** That computer's
  server is from before REMOTE existed (or runs with `--no-remote`). Update it
  the way you installed it (`pipx install --force` the new wheel, or
  `git pull` in its clone), then `dsterm restart` there. If its `serve.conf`
  has `--no-remote`, remove that line and `dsterm restart` instead.
- **REMOTE says "Claude Code isn't installed there".** The service cannot find
  `claude` on its PATH: see the last item below.
- **The DS never says "Claude needs your OK".** It learns that from the
  server, which asks `claude agents`; an older server, or a `claude` too old
  to have `claude agents`, leaves the DS guessing from the screen. Update
  Claude Code (`claude update`) and the server.
- **A custom sound does not play.** The DS's log (SELECT to STATUS) says which
  file was skipped and why. `dsterm card PATH --sounds FOLDER` checks them on
  the computer first.
- **A language of your own is missing from LANGUAGE.** The DS's log says
  `lang: NAME skipped (...)` with the reason. At most two of your own load,
  from `/dsterm/lang/`; start again from `dsterm lang --template`.

- **Pairing says "pairing is not open".** Run `dsterm pair` on the computer
  first; it stays open for two minutes. Then pick the computer on the DS again.
- **The two screens showed different codes.** Press B on the DS (or `n` on
  the computer) and nothing is saved. Something other than your computer
  answered the DS, or a second console was pairing at the same moment. Try
  again; if the codes differ twice on your own network, something on it is
  interfering, and it is worth finding out what.
- **Pairing says "the server pairs the old way".** The computer's dsterm is
  older than the card's. Update it, then `dsterm restart` and pair again.
- **It paired, then "refused this card".** The server's device key and the
  card's differ. That happens if `~/.config/ds-claude-terminal/devices` was
  deleted or the card was restored from an old backup. On the console, forget
  the server (Saved servers, X) and pair again.
- **Connected, but the screen is garbled.** The two halves disagree about the
  grid size and one of them is older than the check that refuses that by name.
  Match them: the release's `dsterm.nds` is 42x24, and `--grid 64x32` in
  `serve.conf` goes only with the 64x32 build.
- **macOS: the console cannot connect, but `doctor` is all green.** The
  application firewall may be blocking Python. Allow it in System Settings →
  Network → Firewall → Options. On macOS 15 and later, also check System
  Settings → Privacy & Security → **Local Network**, and allow Python (or the
  terminal app you ran `dsterm serve` from) if it is listed and off.
- **Windows: it works while an Ubuntu window is open, and stops after you
  close it.** WSL shuts down without a window open, service or not. The
  `.wslconfig` settings in the [README's Windows setup](README.md#3-install-the-server-on-your-computer) keep it
  running.
- **The session died the instant it started** (tmux shows `[exited]`). The
  service could not find `claude`. It runs with a fixed PATH that includes
  `~/.local/bin` (and Homebrew's directories on macOS), not your login PATH,
  so a Claude Code installed with nvm or npm elsewhere is invisible to it.
  `dsterm doctor` says so. Link it into `~/.local/bin` and restart:
  `ln -s "$(command -v claude)" ~/.local/bin/claude && dsterm restart`.

### The camera

- **QUICK has no PHOTO row.** The console is in DS mode (or is a DS / DS
  Lite, which have no camera). Start `dsterm.nds` from Unlaunch or TWiLight
  Menu++ — both use DSi mode — and check ABOUT says `(DSi mode)`.
- **PHOTO says "not on this computer".** The server is older than the camera,
  or runs with `--no-camera`. Update it, or remove that line from
  `serve.conf`, and `dsterm restart`.
- **"the camera did not start".** The sensor's init failed — report it with a
  photo of the STATUS log. A DSi mode console that has lost its camera to a
  hardware fault says this.
- **The popup never shows in my other terminals.** tmux older than 3.2 has
  no `display-popup`; or the server runs `--no-camera-popup`; or you closed
  it with q — it comes back the next time the camera opens. `dsterm camera`
  works regardless.
- **`dsterm camera` says "no camera feed".** The server is not running on
  this computer (the feed is a local socket, not the network).
- **Claude asks before reading the picture.** The file lands under
  `.dsterm/photos/` in the project on screen, which Claude can already read,
  so normally it does not ask; a `--photo-dir` outside the project is a folder
  it does not know, and prompts.
- **The upload is slow.** Use **PICTURE → SMALL**: a quarter of the pixels,
  about four times faster.
- **PHOTO_SAVED said no, by its message:** "the computer is busy" — it was
  saving another picture; try again. "it arrived damaged" — the upload's
  checksum failed; retake. "the computer could not save it" — the photo
  folder could not be written; `dsterm logs` has the reason. "wait a moment"
  — pictures are one per three seconds. "the photo folder's name is not plain
  ASCII" — a `--photo-dir` whose path has a non-ASCII character is refused;
  move it or use the project default.

### Voice

`dsterm doctor` reads the same environment `dsterm serve` does, the `env`
file included, but not the flags in `serve.conf`, so it does not know about a
`--stt-cloud` there. The line `dsterm serve` prints at every start (`dsterm
logs`) is the one that counts, for example `voice: transcribed on this
computer (faster-whisper)`. The DS's own messages are in its log (SELECT to STATUS):

- **`voice: this computer has no speech-to-text`**, or QUICK's VOICE row says
  "no speech-to-text on server". Nothing local is installed. Install one
  ([USING.md](docs/USING.md#voice-push-to-talk)), for example `pipx inject
  ds-claude-terminal faster-whisper`, then `dsterm restart`. mlx-whisper and
  openai-whisper also need `ffmpeg` on the service's PATH; without it they
  are passed over.
- **`voice: off: --stt-cloud is on, but none of OPENAI_API_KEY, GROQ_API_KEY
  or DSTERM_STT_URL is set`** (from `dsterm serve`). The cloud is allowed but
  has nowhere to go: put one of those in the `env` file
  ([CUSTOMIZING.md](docs/CUSTOMIZING.md#speech-to-text)).
  **`... but DSTERM_STT_URL is not an https:// URL`**: it must be https.
- **`voice: server busy`.** The computer is still loading the model, which
  on the very first start can mean downloading it (minutes), or is still on
  the previous recording. Wait and try again.
- **`voice: no speech heard`.** Nothing was recognised. The transcript is
  plain ASCII, so speech in Chinese or Japanese always arrives empty.
- **`voice: no speech-to-text on server`** after a recording. The backend
  failed or took longer than `DSTERM_STT_TIMEOUT`; `dsterm logs` says which
  (`voice not transcribed: ...` or `no transcript in N s; abandoned`). A
  local backend that fails is not retried in the cloud.
- **`voice: timed out waiting for transcription`** or **`still finishing
  the last recording`.** The DS waited its 20 seconds and gave up; it takes a
  new recording once the computer's late answer arrives.
- **`voice: audio damaged`** or **`the computer lost track of the
  recording`.** The recording did not arrive whole. Record it again.
- **`voice: the microphone did not start`.** The DS's microphone failed to
  start; report it with a photo of the STATUS log.

## The two counter lines on the DS

The last two lines of the STATUS log are live counters. When the screen looks
frozen, they say where things stopped; please include them in a bug report.

```
in 48213 B  1290 msgs  sent 371
loop 18000  keys raw 000 lib 000
```

| What you see | What it means |
|---|---|
| `in 0 B` | Nothing from the computer reaches the DS: the network or the server |
| `in` rising, `0 msgs` | Bytes arrive but never form a message: the two sides are from different releases |
| `msgs` rising, screen still | The connection is fine and the problem is in drawing: report it |
| `raw 000 lib 000` while holding a button | The press does not reach the program at all |
| `raw` set, `lib 000` | The hardware sees the press and the library does not |
| `sent` stays put while you type | The DS saw the press and did not send it: report it |
| `sent` rising, nothing happens | The DS sent it; look at the computer (`dsterm logs -f`) |
| `loop` not rising | The program itself has stopped |
