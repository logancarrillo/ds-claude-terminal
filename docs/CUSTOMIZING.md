# Customizing

Most of what you might want to change needs no compiler. The quickest is on
the DS itself: **SETTINGS**, then the **SOUND**, **SCREEN** or **LINK** tab
(tap one, or press L or R). Sounds, buttons, snippets, interface languages and
server options are files. The rest is a small,
well-tested C codebase, and the second half of this page walks through the
common changes.

| You want to | Do this | Rebuild? |
|---|---|---|
| Change sound, volume, typing clicks, writing sounds | The DS's **SETTINGS → SOUND** ([USING.md](USING.md#settings)) | No |
| Change the accent colour, interface language, start screen, or hide addresses | The DS's **SETTINGS → SCREEN** | No |
| Change autoconnect, view taps, the picture size, the live view, the microphone mode | The DS's **SETTINGS → LINK** | No |
| Remap A, B, X, Y and START, or pick the camera's shutter | The DS's **SETTINGS → BUTTONS** | No |
| Use your own sounds | Put WAV files in `/dsterm/sounds/` on the SD card | No |
| Add an interface language | `dsterm lang --template > fr.lang`, translate the right-hand sides, then `dsterm card PATH --lang fr.lang` | No |
| Save your own quick commands | On the DS: **QUICK → COMMANDS → + ADD** ([USING.md](USING.md#quick)) | No |
| Add buttons for your commands | Write a `buttons` file on the computer | No |
| Keep canned prompts | Write a `snippets` file | No |
| Change server options | Write `~/.config/ds-claude-terminal/serve.conf` | No |
| Set up speech-to-text | Install a local Whisper, or set `DSTERM_STT_...` ([below](#speech-to-text)) | No |
| Change what a Control tile does | Edit two `switch` statements | Yes (the ROM) |
| Change the built-in sound set | Replace WAVs and run one script | Yes (the ROM) |

---

## Your own sounds

Name a WAV file after a cue, put it in `/dsterm/sounds/` on the SD card, and
it replaces the built-in sound for that cue the next time the DS starts.
`done.wav` replaces DONE; add `done-2.wav` and `done-3.wav` and the DS rotates
through the three, never playing the same one twice in a row.

| File | Plays when |
|---|---|
| `sent.wav` | you sent a prompt |
| `think.wav` | Claude started working |
| `output.wav` | the answer started arriving (silent unless you add this file) |
| `done.wav` | Claude finished: your turn |
| `approve.wav` | Claude needs your OK (a permission prompt) |
| `question.wav` | Claude asked you something |
| `link.wav` | connected to the computer |
| `drop.wav` | the connection dropped |
| `lantern.wav` | another session has new output, or the one reminder |
| `switch.wav` | you switched session or computer |
| `key.wav` | a key on the touch keyboard |
| `delete.wav` | backspace |
| `cursor.wav` | menu ticks; silent in the standard sound profile, so most people never hear it |
| `deny.wav` | a press that could not do anything |
| `blip.wav` | soft typing while Claude writes (the WRITING setting) |
| `boot.wav` | the DS started |

`dsterm sounds` prints the same list.

**The format:** PCM WAV, 8- or 16-bit, mono or stereo, 4 to 48 kHz, at most
three seconds. Anything an audio editor exports as "WAV (PCM)" works.
Compressed or 24/32-bit files are skipped, and the DS's log (SELECT to
STATUS) says which file and why. The DS keeps them uncompressed, and all of
them together may use a megabyte: about 30 seconds of sound at 16 kHz, or
12 at 44.1 kHz. 16 or 22 kHz is plenty for a DS speaker.

Your sounds play when **SOUND** is set to **SOFT** (SETTINGS → SOUND).
CHIP, the default, is the square-wave voice and has no samples. Like the built-in ones, each plays as recorded
or a whole tone higher or lower, so a sound with a strong pitch will be heard
in three keys.

**The easy way to copy them:** put your WAVs in a folder, put the SD card in
your computer, and run

```sh
dsterm card /Volumes/SDCARD --sounds ~/my-ds-sounds
```

It checks every file by the same rules the DS uses and copies only the ones
that will play, saying why it skipped any others.

**Making sounds that work on a DS:**

- **Short.** A cue is a quarter of a second to a second. Keys and blips are a
  few hundredths. A long sound is cut off by the next one.
- **No silence at the start.** Trim it, or every cue feels late.
- **Loud and clean.** Normalize to about -1 dB. The DS plays every cue at the
  same loudness the built-in one had, so a quiet file stays quiet.
- **Mid and high frequencies.** The DS speakers reproduce almost nothing
  below about 400 Hz; a bass-heavy sound is mostly silence on them. Check
  with headphones and with the speakers.
- **Distinct shapes.** The built-in set gives each cue its own object (a key,
  a pencil, a notebook, a knock), which is what lets you tell them apart
  without looking.

To put a sound back, delete its file from the card.

## Your own buttons

The **PROJECT** screen's **DO** tab (SELECT until you reach PROJECT, then left
or right to DO) has a row of buttons. Each types a line into the session. Some
are built in and some are found in your project (its `npm` scripts, `make`
targets and `pytest`); the first ones are yours.

Write them one per line, `label = what it types`, in either of:

- `.dsterm-buttons` in the project directory Claude Code runs in, for that
  project, or
- `~/.config/ds-claude-terminal/buttons`, for every project.

```
# label  = what the button types
tests    = !npm test -- --watch=false
deploy   = !./scripts/deploy.sh staging
review   = Review the last commit for bugs and missing tests.
compact  = /compact
```

What a button types goes into Claude Code's prompt, so:

- start it with **`!`** to run a shell command (Claude Code's bash mode);
- start it with **`/`** for a Claude Code command;
- or write a plain sentence for a prompt.

The DO tab holds six buttons besides the built-in ones; yours come first.
Labels can be up to 12 characters; commands up to 40. A line that doesn't
fit is skipped rather than shortened, because a shortened command is a
different command. Pressing a button loads it into the composer, so you see
the exact text before **A** sends it. (With **GOD MODE** on in Settings, it
sends immediately; the page says so in red.) Changes to the files show up on
the DS within a few seconds.

## Snippets

Snippets are prompts you'd rather not type on a touch keyboard. In the
composer, **Y** opens the list; picking one loads it, ready to read and send.

One `label=body` per line, up to eight, in either place:

- `~/.config/ds-claude-terminal/snippets` on the computer (sent to the DS
  when it connects), or
- `/dsterm.snippets` on the SD card.

```
review=Review the diff for bugs, then list anything that needs a test.
explain=Explain the last error and how to fix it, briefly.
commit=Write a commit message for the staged changes.
```

Labels are up to 15 characters, bodies up to 255. In a body, `\n` is a new
line and `\t` a tab. `dsterm.snippets.example` in this repository describes
the format in full.

## The server

The background service runs `dsterm serve` with a fixed command line. To add
options to it without editing the service, write them in
`~/.config/ds-claude-terminal/serve.conf`, one or more per line, and restart:

```
# ~/.config/ds-claude-terminal/serve.conf
--clip                     # send the computer's clipboard to the DS (WSL only)
--grid 64x32               # match the smaller-font ROM build
-- claude --model sonnet   # the command the session runs
```

```sh
dsterm restart
```

| Option | Does |
|---|---|
| `--cwd PATH` | Where Claude Code runs (or `dsterm install-service --cwd PATH`) |
| `--grid 64x32` | The terminal size, matching the ROM in use (below) |
| `--clip` | Sends the Windows clipboard to the DS's composer (WSL only). Off by default, because it sends every copy you make, on this computer and any synced to it, to the DS |
| `--no-remote` | Hides other sessions from the DS's REMOTE screen |
| `--no-camera` | Answers the DS's PHOTO row with "not on this computer": no READY, so nothing camera-related crosses |
| `--no-camera-popup` | The camera's live view stops popping up in other attached terminals; `dsterm camera` still shows it |
| `--photo-dir PATH` | Where sent pictures are saved, instead of `.dsterm/photos/` in the project on screen |
| `--photo-keep N` | How many pictures the folder keeps (default 20; nothing older than a week is kept either way) |
| `--no-mic` | Turns off push-to-talk: the DS is never told a microphone is available, and nothing voice-related crosses |
| `--stt-cloud` | Lets push-to-talk use a cloud speech-to-text service (OpenAI, Groq or `DSTERM_STT_URL`) when no local one is installed. Off by default: without it a recording never leaves this computer, whatever API keys are set ([USING.md](USING.md#voice-push-to-talk)) |
| `--project-ci` | Shows GitHub Actions runs on PROJECT → WATCH, read with `gh run list` every 30 seconds. Off by default because it is a network call |
| `--no-keep-awake` | Lets the computer idle to sleep even while a DS is connected (by default a Mac or Linux computer stays awake until the last DS leaves; on WSL, Windows decides) |
| `--fps N` | How often the screen is checked for changes (default 15) |
| `--port`, `--discovery-port`, `--dashboard-port` | The TCP, UDP and dashboard ports |
| `-- COMMAND ...` | Run something other than `claude` in the session |

`dsterm serve --help` lists everything.

**Environment for Claude Code.** The session inherits the service's
environment plus two optional files, both readable only by you (mode 600):

```
~/.config/ds-claude-terminal/anthropic-key   an API key on its own line (ANTHROPIC_API_KEY)
~/.config/ds-claude-terminal/env             KEY=VALUE lines, # comments
```

If Claude Code is already logged in, you need neither.

### Speech-to-text

Push-to-talk ([USING.md](USING.md#voice-push-to-talk)) uses one backend,
chosen when the server starts: a local one if any is installed, otherwise a
cloud one only if you opted in. The variables go in the `env` file above and
take effect on `dsterm restart`:

| Variable | Does |
|---|---|
| `DSTERM_STT_CMD` | Your own program, run as `program WAV LANG`; what it prints is the transcript. Wins over everything else |
| `DSTERM_STT_MODEL` | The model: a name for the Python libraries (`base` by default) or the cloud services, or the path of a `ggml-*.bin` file for whisper.cpp |
| `DSTERM_STT_TIMEOUT` | Seconds a recording may take, 5 to 17 (default 15) |
| `DSTERM_STT_URL` | A cloud service of your own; must be `https://` |
| `DSTERM_STT_KEY` | The key sent to `DSTERM_STT_URL`, and only there |
| `OPENAI_API_KEY`, `GROQ_API_KEY` | The cloud services used with `--stt-cloud` when no `DSTERM_STT_URL` is set (in that order) |

And in `serve.conf`:

| Option | Does |
|---|---|
| `--stt-cloud` | Allows the cloud backends at all. Without it a recording never leaves this computer, whatever keys are set. (`DSTERM_STT_CLOUD=1` in the server's own environment does the same; in the `env` file it does nothing) |
| `--no-mic` | Turns push-to-talk off |
| `--project-ci` | Not voice, but also off unless added: GitHub Actions runs on PROJECT → WATCH |

`dsterm serve` prints where recordings go at every start (`dsterm logs`
shows it), for example `voice: transcribed on this computer (faster-whisper)`.

### A bigger grid

The release has two builds: `dsterm.nds` (42x24 with a 6x8 font, the
default) and `dsterm/dsterm-64x32.nds` (64x32 with a 4x6 font: more on screen,
smaller to read). The server must match the ROM, so to use the larger grid,
copy `dsterm-64x32.nds` to the card root as `dsterm.nds`, add `--grid 64x32`
to `serve.conf`, and `dsterm restart`. If the two do not match, the server
refuses the connection and the DS says which size each side is.

---

## Building your own ROM

You need [BlocksDS](https://blocksds.skylyrac.net/docs/setup/) (version 1.23 or
later). Then, in a clone of this repository:

```sh
python3 -m venv .venv && .venv/bin/pip install -e '.[dev]'   # once
./bin/dsterm build                     # writes dsclient/dsterm.nds
./bin/dsterm test                      # both test suites
./bin/dsterm card /Volumes/SDCARD      # rebuilds if a source changed, then writes and verifies the card
```

`./bin/dsterm` is the same `dsterm` command, run from this checkout, so it
uses the code you just changed. `build` is `make -C dsclient` with the
toolchain found for you; add `GRID_COLS=64 GRID_ROWS=32` for the larger grid.
`card` with no `--rom` builds from the checkout; with `--rom FILE` it writes
that file instead. [CONTRIBUTING.md](../CONTRIBUTING.md) has the rest. In `dsclient/Makefile`,
`GAME_TITLE`, `GAME_SUBTITLE`, `GAME_AUTHOR` and `GAME_ICON` (a 32x32,
16-colour GIF) are what launchers show for the app.

## Changing a Control screen tile

The six tiles are drawn, touched and walked with the d-pad from one table, so
changing what one does takes two edits. Say you'd rather have a **COMPACT**
tile that types `/compact` than the **PHOTO** tile.

**1. The label** is in `draw_control_screen()` in
[`dsclient/source/subscreen.c`](../dsclient/source/subscreen.c): the tile's
name and the short line under it.

```c
case KBD_CONTROL_PHOTO:    label = "COMPACT";  what = "free context"; break;
```

**2. What it does** is in `control_press()` in
[`dsclient/source/main.c`](../dsclient/source/main.c). Replace the body of the
`KBD_CONTROL_PHOTO` case:

```c
case KBD_CONTROL_PHOTO:
    // Type /compact and press Enter, as if at the keyboard.
    send_key_text("/compact", 8, tx);
    send_key_seq("\r", 1, tx);
    break;
```

`send_key_text()` types text and `send_key_seq()` sends exact bytes (`"\x1b"` is
Esc, `"\x03"` Ctrl-C, `"\x1b[Z"` Shift-Tab). To open a screen instead, copy the
pattern of a case that already does, such as `KBD_CONTROL_SETTINGS`.

**3. Build and look:**

```sh
./bin/dsterm build && ./bin/dsterm test
tools/screenshots.sh           # redraws docs/screens/*.png from the code
```

`tools/screenshots.sh` draws every screen with the DS's own drawing code on
your computer, so you can see the new tile before it is on a console.

**Moving or adding tiles.** The positions are the `control_rects[]` table in
[`dsclient/source/kbd.c`](../dsclient/source/kbd.c) (pixels, 256x192 screen),
and the ids are `kbd_control_t` in `kbd.h`. Drawing, touch and d-pad focus all
read that table, so a tile moved there moves everywhere. A seventh tile means
a new id before `KBD_CONTROL_RECONNECT`, a rect, a label, a case, and
`KBD_CONTROL_COUNT` raised by one; the tests in `dsclient/tests/test_core.c`
check that tiles do not overlap, fit on screen, and are big enough for a
thumb, and will tell you if they are not.

**The physical buttons** are a map anyone can change on the DS (SETTINGS →
BUTTONS). Its defaults are `button_map_defaults()` in `buttons.c` (`A` sends
Enter, `B` Delete, and so on), and the legend under the tiles is built from
the map by `button_map_legend()`, so it follows any change by itself.

## Changing the built-in sound set

The sounds compiled into the ROM are the WAVs in
[`dsclient/sounds/`](../dsclient/sounds), listed in
[`dsclient/sounds/sounds.txt`](../dsclient/sounds/sounds.txt) with a loudness
for each:

```
# bank     file           gain
done       done-1.wav     80
done       done-2.wav     80
```

Replace or add WAVs (mono, 16-bit, 16384 Hz), edit the list, and regenerate:

```sh
python3 tools/sfx_build.py     # writes dsclient/source/sfx_assets.[ch]
dsterm build
```

Two things matter for how a sound comes across on a DS. Its speaker plays
almost nothing below about 400 Hz, so a sound that is all low thump is nearly
silent there; and a click that rings for long talks over the next keypress,
which is why the suite keeps key, cursor and blip sounds under 5 frames.
Long sounds are stored as the DS's own 4-bit ADPCM, short clicks as 16-bit
PCM. When each cue plays, how important it is, and how often it may repeat
are in [`dsclient/source/sound.c`](../dsclient/source/sound.c) (priorities,
cooldowns, and which sound profile includes it) and
[`dsclient/source/chime.c`](../dsclient/source/chime.c) (which events trigger
which cue).

## Going further

- [ARCHITECTURE.md](ARCHITECTURE.md) maps every file in the repository and
  how a keypress, a frame and a sound travel through them.
- [PROTOCOL.md](PROTOCOL.md) is the wire format, for another client or server.
- `dsclient/source/ext.h` is a seam for something that wants to live beside
  the terminal on the DS (a status display, a companion), with no access to
  the keyboard or the server.
