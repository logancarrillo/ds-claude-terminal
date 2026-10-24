# Changelog

## 1.2 (unreleased)

### Talk to Claude with your voice
- Hold **R** in the composer, speak, and let go: your words appear as text.
- Nothing is sent until you read it and press **A**.
- **SETTINGS → LINK → MIC → LATCH**: press **L** while holding **R** to keep
  recording hands-free; tap **R** or **L** to stop.
- Transcribed on your own computer by a local Whisper. Cloud is used only if
  you turn it on.

### A controls guide on the DS
- Hold **SELECT**, then press **START** (or **QUICK → CONTROLS**).
- A drawing of your console lights each button as you press it.
- A box on the top screen says what the button or combo does.
- Labels follow your own button settings. Nothing you press is sent.

### Type in the composer without the stylus
- Hold **L**, then press **R**: the d-pad walks the keyboard.
- **A** types the lit key, **START** sends, **X** leaves focus.

### Four languages
- The DS's menus in English, Spanish, Chinese or Japanese:
  **SETTINGS → SCREEN → LANGUAGE**.
- Add your own with `dsterm lang --template`.

### Your own quick commands
- **QUICK → COMMANDS → + ADD** saves a command on the card.
- Pick it later to load it into the composer; **A** sends it.

### Simpler settings
- Five tabs instead of three: SESSION, SOUND, SCREEN, LINK and BUTTONS.
- Your saved settings carry over.

### A fuller PROJECT page
- **WATCH**: a focus timer (`dsterm timer 25`) and your own rows.
- **HEALTH**: the computer's load and free disk. **LOG**: recent commits.

### Fixes
- Pressing **A** with nothing typed no longer sounds like a prompt was sent.
- A settings file with an unknown value now keeps every other setting.

### Details

**Voice.**
- Local backends, in order: `DSTERM_STT_CMD`, mlx-whisper, faster-whisper,
  openai-whisper, whisper.cpp's `whisper-cli`. mlx-whisper and openai-whisper
  also need ffmpeg. `dsterm serve` says at every start where recordings go.
- Cloud (OpenAI, Groq, or your own `DSTERM_STT_URL`) is used only with
  `dsterm serve --stt-cloud`, only over https, and only when nothing local is
  installed. An API key set for other programs sends nothing, and a local
  failure is never retried in the cloud. `--no-mic` turns the microphone
  off ([SECURITY.md](SECURITY.md)).
- Up to 15 seconds a recording. The computer gives up at 15 seconds
  (`DSTERM_STT_TIMEOUT`, at most 17), the DS at 20. The model loads as
  `dsterm serve` starts (on a first run ever, a download); recordings
  meanwhile are answered "busy".
- Transcripts are plain ASCII, so "café" arrives as "cafe". With the DS in
  Chinese or Japanese, dictation listens for English.
- Needs this release on both sides. An older server is never sent a voice
  message, and an older DS never asks.

**Languages.** Chinese and Japanese use Fusion Pixel Font (SIL OFL 1.1),
drawn for 10-pixel text and cut to the characters the DS uses. Menus are
laid out from each language's own word widths. What Claude prints is not
translated.

**PROJECT.** Up to 64 rows with block and sparkline glyphs. A DS still on
1.0.x gets the 48 rows it can show (NOW, DIFF and DO first) in plain ASCII.
GitHub Actions runs show only with `dsterm serve --project-ci`, which asks
GitHub every 30 seconds.

**Settings.** CUSTOMIZE's rows moved to SOUND, SCREEN and LINK. The file
stores values, not tabs, so nothing needs resetting.

## 1.0.1 (2026-10-17)

### Approvals are tied to the prompt they answer

Before, pressing **A** sent an Enter that wasn't tied to any particular
permission prompt. A double press, a frame delayed on the way, or a prompt
answered at the computer while the press was in flight could land that Enter
on the *next* prompt. Now:

- **Every permission prompt has an id.** The computer sends it to the DS
  with the "needs your OK" state. An Enter pressed while a prompt is open
  carries that id, and the computer delivers it only if the same prompt is
  still waiting.
- **Late, crossed and double presses are held back.** This covers an Enter
  sent before the DS had heard of a prompt, and an Enter pressed within a
  moment of the last one, while the computer checks whether a new prompt is
  waiting behind it. The DS says *"not sent: the screen changed -- look, then
  press again"*.
- **X (decline), the d-pad and typing are never held back.** Declining is
  safe wherever it lands, and an arrow only moves the highlight.
- **Nothing times out.** A prompt waits in Claude Code until a key reaches
  it, so a lost connection never approves or declines anything.
- **Old DS apps and old computers work as before.** The feature switches on
  only when both sides have it.

It is pinned C against Python, tested against random interleavings of
prompts, presses, late readings and answers at the computer
(`tests/test_answers_random.py`), and run on a DSi. The one case it can't
cover is in [PROTOCOL.md](docs/PROTOCOL.md#answers): a prompt answered at the
computer, with the next one shown within half a second of an arrow pressed on
the DS.

## 1.0.0

The first release: your live Claude Code session on a DS, approvals on a
button, QUICK, REMOTE, the DSi camera, `dsterm view`, pairing by a six-digit
code, and an encrypted, signed link. The downloads are on the repository's
Releases page.
