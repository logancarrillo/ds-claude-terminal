<div align="center">

<img src="docs/assets/banner.png" alt="DS Claude Terminal: Claude Code on a Nintendo DS. The title in the DS's own pixel font beside a DS showing Claude asking to approve an edit" width="100%">

<br><br>

Your DS becomes a wireless screen and keyboard for the Claude Code session on
your computer. Read what Claude is doing, approve its edits with a button, and
type prompts on the touch screen, from the couch.

<br>

![Consoles](https://img.shields.io/badge/console-DS%20%C2%B7%20DSi%20%C2%B7%203DS-d97757?style=flat-square)
![Computers](https://img.shields.io/badge/computer-macOS%20%C2%B7%20Linux%20%C2%B7%20Windows%20(WSL2)-3d3d3a?style=flat-square)
![Python](https://img.shields.io/badge/python-3.11%2B-3d3d3a?style=flat-square)
![C](https://img.shields.io/badge/DS%20client-C%20%C2%B7%20BlocksDS-3d3d3a?style=flat-square)
![Encrypted](https://img.shields.io/badge/link-encrypted%20%2B%20signed-d97757?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-3d3d3a?style=flat-square)

[**Features**](#features) · [**Controls guide**](#learn-the-buttons-the-controls-guide) · [**Setup**](#setup) · [**Pairing**](#pairing) · [**Commands**](#commands) · [Using it](docs/USING.md) · [Under the hood](#under-the-hood) · [Troubleshooting](TROUBLESHOOTING.md)

</div>

<br>

Your code and Claude Code itself stay on your computer. The DS only shows the
screen and sends keys, everything between them is encrypted, and nothing the
DS sends can run a command or read a file.

## Features

### On the DS

- **Your live session.** The top screen is the Claude Code terminal, 42×24,
  updated as it changes. A second build fits 64×32 in a smaller font.
- **Approvals on a button.** When Claude wants to edit a file or run a
  command, the DS turns yellow and beeps: **A** says yes, **X** says no. When
  it asks a question, the d-pad picks the answer.
- **Always knows what Claude is doing.** Working, done, needs your OK, asking
  you something: in plain words on the bottom screen, and with a sound.
- **Typing.** A touch keyboard that types live, a composer for prompts worth
  editing before they're sent, and snippets for the things you type often.
  In the composer, hold **L**, then press **R**, and the d-pad types.
- **Voice.** Hold **R** in the composer and speak; the words land in the
  composer, to read before you send them. Transcribed by Whisper on your
  computer; a cloud service only if you turn it on.
- **QUICK.** Claude's mode on one tile (switch it with a tap), plus rewind,
  the diff, every slash command (yours included), your own saved commands
  (**+ ADD**) and your recent prompts.
- **REMOTE.** Every Claude Code session on the computer, which are working
  and which need you; switch to one, start a new one, or move the DS to
  another computer on your network.
- **PHOTO** (DSi and 3DS). A live viewfinder on either camera; take the
  picture and it lands on the computer with its path in your prompt.
- **PROJECT.** The project Claude is working in, on both screens: what's
  happening now, what changed in the repository, and one-tap buttons for
  your `npm` scripts, `make` targets, tests and your own commands.
- **Read back.** Scroll up through everything the session printed; hold
  down to jump back to now.
- **Sounds.** The classic chip voice, a soft desk set with three variations
  of each sound, or your own WAV files. Or silence.
- **A controls guide.** Hold **SELECT**, then press **START**: a drawing of
  your console names every button and combo as you press it, and nothing
  you press is sent.
- **Four interface languages.** English, Spanish, Simplified Chinese or
  Japanese, or one of your own. Claude's screen is not translated.
- **Yours to set up.** Volume, typing clicks, four accent colours, the start
  screen, autoconnect, and A/B/X/Y/START mapped to the keys you want.

### On the computer

- **One command to install,** on macOS, Linux, or Windows with WSL2. It runs
  in the background and starts by itself; `dsterm doctor` checks everything
  and says how to fix what isn't right.
- **`dsterm view`.** Both DS screens on your computer, pixel for pixel like an
  emulator, in the terminal or a window of its own. Type to Claude from there
  and click the bottom screen to tap the DS.
- **The camera, live,** in a terminal or a window while it's open on the DS.
- **`dsterm card`.** Set up an SD card from the computer: the app, your Wi-Fi,
  a pairing and your own sounds, with nothing to type on the DS.
- **Your session survives.** Claude Code runs in tmux, so restarting the
  server or losing the DS never loses your work. A Mac or Linux computer
  stays awake while the DS is connected.
- **More than one computer.** Pair the DS with each; switch between them from
  the DS.

### Connection and security

- **Finds your computer by itself** on the network, and sets up Wi-Fi with an
  on-screen wizard (WPA2 on a DSi or 3DS).
- **Pairs by comparing a six-digit code,** like Bluetooth. No passwords to
  type, a key of its own for each DS, and `dsterm devices forget` revokes one.
- **Every frame encrypted and signed** (ChaCha20 and HMAC-SHA256), with no
  way to switch it off.
- **Reconnects by itself** after sleep, Wi-Fi drops or a server restart.
- **Can only do what a keyboard can.** Text pushed to the DS needs a second
  press before it's typed, and nothing the DS sends runs a command or reads a
  file.
- **Demo mode.** One setting stars out every IP address on screen, for streams
  and photos.

<p align="center">
  <img src="docs/screens/control-approval.png" width="210" alt="Claude asking for approval on the DS">
  &nbsp;
  <img src="docs/screens/remote.png" width="210" alt="REMOTE: Claude sessions on this computer, and other computers">
  &nbsp;
  <img src="docs/screens/camera.png" width="210" alt="The camera screen: aim, take a picture, send it to Claude">
  &nbsp;
  <img src="docs/screens/settings.png" width="210" alt="Settings: sound, volume, theme, start screen, autoconnect">
</p>

## Learn the buttons: the controls guide

<p align="center">
  <img src="docs/screens/controls-guide.png" width="260" alt="The controls guide: a drawing of the DSi below, with L and R lit, and a box over a Claude Code session saying L then R is FOCUS">
</p>

The DS can teach you its own buttons. Nothing you press in the guide is sent
anywhere, so try anything.

1. **Open it:** hold **SELECT**, then press **START**. (Or **QUICK →
   CONTROLS**.)
2. **Press any button or combo.** It lights up on a drawing of your console,
   and a box on the top screen says what it does. The labels follow your own
   button settings.
3. **Pick a screen** with the tabs along the top (Control, KEYS, COMPOSE,
   FOCUS, CAMERA, QUICK). **Leave** with **DONE**, or by holding **B**.

New in 1.2, from left to right: typing in the composer without the stylus
(hold **L**, then press **R**), your own saved commands in QUICK, and the DS
in Japanese.

<p align="center">
  <img src="docs/screens/compose-focus.png" width="210" alt="The composer with focus on: a key lit on the keyboard, A types it, START sends">
  &nbsp;
  <img src="docs/screens/quick-commands.png" width="210" alt="QUICK, COMMANDS: a saved command above the computer's own, and + ADD">
  &nbsp;
  <img src="docs/screens/control-ja.png" width="210" alt="The Control screen in Japanese, asking for approval">
</p>

## Supported consoles

| Console | Runs the app with | Wi-Fi |
|---|---|---|
| **DSi, DSi XL** | Unlaunch or TWiLight Menu++ | WPA2 or open, 2.4 GHz |
| **3DS, 3DS XL, 2DS, New 3DS, New 3DS XL, New 2DS XL** | TWiLight Menu++ | WPA2 or open, 2.4 GHz |
| **DS, DS Lite** | A flashcart (R4, Acekard, DSTT, ...) | Open networks only (a hardware limit) |

One file runs on all of them. It's tested on a DSi; reports from the others
are very welcome ([status](#status)).

---

## Setup

Four steps, once. After that you just switch the DS on.

| | Step | Time |
|:-:|---|---|
| **1** | [Get the console ready](#1-get-the-console-ready) | 0 min if it already runs homebrew, an evening if not |
| **2** | [Put the app on the SD card](#2-put-the-app-on-the-sd-card) | 2 min |
| **3** | [Install the server on your computer](#3-install-the-server-on-your-computer) | 5 min |
| **4** | [Pair the DS with your computer](#pairing) | 1 min |

**You need:** a DS-family console, its SD card, a Mac / Linux PC / Windows
10-11 PC, [Claude Code](https://claude.com/claude-code) installed and logged
in on it, and 2.4 GHz Wi-Fi that both can join.

### 1. Get the console ready

A console from the shop only runs cartridges. To run this app it needs a
one-time softmod, or a flashcart for a DS or DS Lite.

| Console | Do this |
|---|---|
| DSi, DSi XL | Install Unlaunch: <https://dsi.cfw.guide> |
| 3DS, 2DS (any) | Custom firmware, then TWiLight Menu++: <https://3ds.hacks.guide> |
| DS, DS Lite | A Slot-1 flashcart that runs homebrew |

Already done this for other homebrew? Skip ahead. More on each model:
[docs/HARDWARE.md](docs/HARDWARE.md).

> [!TIP]
> **You should see:** a menu (TWiLight Menu++, Unlaunch, or your flashcart's)
> listing the files on the SD card.

### 2. Put the app on the SD card

1. Download **`dsterm-<version>.zip`** and
   **`ds_claude_terminal-<version>-py3-none-any.whl`** from this repository's
   **Releases** page.
2. Unzip the zip. Inside are **`dsterm.nds`** and a folder called **`dsterm`**.
3. Copy **both** to the top level of the SD card, not inside another folder.
4. Put the card back in the console.

> [!TIP]
> **You should see:** **DS Claude Terminal** in the console's menu.

### 3. Install the server on your computer

The server runs Claude Code on your computer and sends it to the DS. You
install it once; it keeps running in the background, and starts again after
a restart.

<details open>
<summary><b>Mac</b></summary>

<br>

Install [Homebrew](https://brew.sh) if you don't have it, then in Terminal:

```sh
brew install tmux pipx
pipx ensurepath
```

**Close Terminal and open a new one**, then, in the folder with the `.whl`:

```sh
pipx install ./ds_claude_terminal-*.whl
dsterm setup
```

The first time the server starts, macOS may ask whether Python may accept
incoming connections. Click **Allow**, or the DS can't reach it.

</details>

<details>
<summary><b>Linux</b></summary>

<br>

Debian and Ubuntu shown; use your distribution's packages elsewhere.

```sh
sudo apt install tmux pipx
pipx ensurepath
```

**Close the terminal and open a new one**, then, in the folder with the
`.whl`:

```sh
pipx install ./ds_claude_terminal-*.whl
dsterm setup
```

It needs Python 3.11 or newer. Ubuntu 24.04 has it. On 22.04, run
`sudo apt install python3.11` first and add `--python python3.11` to the
`pipx install` line.

</details>

<details>
<summary><b>Windows</b> (WSL2)</summary>

<br>

1. Install [WSL2 with Ubuntu](https://learn.microsoft.com/windows/wsl/install)
   if you haven't.
2. In the **Ubuntu** window, follow the **Linux** steps above.
3. Still in Ubuntu, let the DS through Windows' firewall (it asks first):

   ```sh
   dsterm wsl-setup
   ```

4. **Keep WSL running.** Windows shuts WSL down a few seconds after the last
   Ubuntu window closes, and the server would go with it. In **PowerShell**,
   run `notepad $env:USERPROFILE\.wslconfig`, put this in the file and save:

   ```ini
   [general]
   instanceIdleTimeout=-1

   [wsl2]
   vmIdleTimeout=-1
   ```

   Then run `wsl --shutdown` in PowerShell and open Ubuntu again.
5. *Optional:* to start WSL by itself when you sign in to Windows, see
   [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md#start-wsl-when-you-sign-in-windows).

</details>

**Check it** on any computer:

```sh
dsterm doctor
```

> [!TIP]
> **You should see:** a list of `[ ok ]` lines. Any `[warn]` or `[FAIL]` line
> says what to do; [TROUBLESHOOTING.md](TROUBLESHOOTING.md) explains each one.

Claude Code starts in your home folder. To start it in a project instead:
`dsterm install-service --cwd ~/projects/my-app`.

### Pairing

Pairing is a one-time introduction, like pairing headphones over Bluetooth.
The DS and your computer check that they're really talking to each other and
agree on a key. **You don't type a password.** You just check that both
screens show the same six-digit code. From then on the connection is
encrypted, and nobody else on your network can read it or type into it.

<table>
<tr>
<td width="62%" valign="top">

#### A. Over Wi-Fi (the usual way)

1. **On the computer**, run `dsterm pair`. Pairing stays open for two
   minutes.
2. **On the DS**, start **DS Claude Terminal** and choose **Find servers on
   this network**.
3. **First time only:** pick your Wi-Fi network and type its password on the
   touch keyboard.
4. **Pick your computer** from the list (it's marked as open for pairing),
   then **Pair**.
5. **Compare the codes.** Both screens show six digits, like `482 913`.
   - **The same?** Press **A** on the DS, and type `y` on the computer.
   - **Different?** Press **B**. Something else answered; nothing is saved.
     Run `dsterm pair` and try again.

</td>
<td width="38%" valign="top" align="center">

<img src="docs/screens/first-boot.png" width="230" alt="The first-boot menu on the top screen, and the three-step guide on the bottom screen">

<sub>The first start: the menu on top, a short guide below.</sub>

</td>
</tr>
</table>

> [!TIP]
> **You should see:** "Paired" on the DS, then Claude Code on the top screen
> and the Control screen below, with your computer's name at the top and a
> short chime.

#### B. From the computer, with the SD card

Rather not type a password on the DS? With the card in your computer, one
command copies the app, writes your Wi-Fi details and pairs it. Put the card
back in the DS and it connects on its own.

```sh
dsterm card /Volumes/SDCARD --rom dsterm.nds --wifi --pair
```

Use your card's path: `/Volumes/NAME` on a Mac, `/media/you/NAME` on Linux,
`/mnt/d` for drive D: in WSL. It checks everything it writes, never deletes
anything already on the card, and ejects it when done.

#### From now on

- **Switch the DS on** and it counts down from three and connects by itself.
  Press any button during the countdown for the menu instead.
- **If the connection drops** (the DS slept, the Wi-Fi blinked, the server
  restarted), it reconnects by itself. You never pair again.
- **Another computer?** Run `dsterm pair` there, press a button during the
  DS's countdown, and pair the same way. Switch between them in **REMOTE**.

### Try it

1. Press **KEYS**, type `hello` on the touch keyboard, and press **A**
   (Enter). Claude answers on the top screen.
2. Ask Claude to change a small file. When it asks to edit, the DS turns
   yellow: **"Claude needs your OK: A yes X no"**. Press **A**.
3. Press **X** to get back to the Control screen from anywhere.
4. Hold **SELECT**, then press **START**: the controls guide shows what every
   button does.

[docs/USING.md](docs/USING.md) is the tour of everything else. To update,
install from source, or uninstall, see
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md).

---

## Commands

`dsterm --help` lists them all. The ones you'll use:

### Every day

| Command | What it does |
|---|---|
| `dsterm status` | Is the server running, which DS is connected, the session |
| `dsterm doctor` | Checks everything, and says how to fix whatever isn't right |
| `dsterm pair` | Pair another DS (or the same one again) |
| `dsterm devices` | The paired DS consoles; `dsterm devices forget ID` revokes one |
| `dsterm restart` | Restart the server (also `start` and `stop`). Your Claude session keeps running |
| `dsterm logs -f` | The server's log, live |

### Watching or joining the session

Pick the one that fits what you're doing:

| Command | Best for |
|---|---|
| `tmux -L dsterm attach -t dsterm` | **Working at the computer.** The same Claude session in your own terminal, fast and full-colour. `dsterm attach` does the same. Leave with **Ctrl-B** then **D**; the session keeps running |
| `dsterm view --window` | **Showing it off.** The DS itself, both screens pixel for pixel, in a window: for demos, streams and recordings. Type and click from there |
| `dsterm view` | Both DS screens in the terminal: pictures in iTerm2, Ghostty, kitty and WezTerm, text anywhere else. **Ctrl-]** then **e** opens the window, **q** leaves |
| `dsterm camera --window` | The DSi camera's live view, while it's open on the DS |
| `dsterm screen` | Print what the DS shows right now, once |
| <http://127.0.0.1:8348> | A read-only dashboard in your browser, on the computer only |

The session is sized for the DS (42×24), so it stays that size wherever you
join it from.

### On Windows

The server lives in WSL. From **Ubuntu**, every command above works as
written. From **PowerShell** or **Command Prompt**, put `wsl` in front:

```powershell
wsl -d Ubuntu -- bash -lc "dsterm view --window"
wsl -d Ubuntu -- tmux -L dsterm attach -t dsterm
wsl -d Ubuntu -- bash -lc "dsterm doctor"
```

- `Ubuntu` is your distribution's name (`wsl -l` lists them).
- `bash -lc` runs a login shell, which is what finds `dsterm` in
  `~/.local/bin`. Or give the full path, and the Linux user if the server runs
  as someone other than your default:
  `wsl -d Ubuntu -u <you> -- /home/<you>/.local/bin/dsterm view --window`.
- The windows need Python's tkinter: `sudo apt install python3-tk` once in
  Ubuntu (the same on Linux). Windows 11 then shows them by itself (WSLg).

### Setting up and changing things

| Command | What it does |
|---|---|
| `dsterm setup` | Install the server and start it (run once) |
| `dsterm install-service --cwd ~/projects/app` | Start Claude Code in a project instead of your home folder |
| `dsterm card PATH --rom dsterm.nds --wifi --pair` | Set up an SD card from the computer: the app, Wi-Fi and a pairing |
| `dsterm card PATH --sounds FOLDER` | Copy your own sounds to the card; `dsterm sounds` lists the names |
| `dsterm lang --template > fr.lang` | Start an interface language of your own; `dsterm card PATH --lang fr.lang` copies it to the card |
| `dsterm wsl-setup` | Let the DS through Windows' firewall (Windows only) |

## Under the hood

Two programs and the protocol between them, built for a 2004 handheld with a
67 MHz ARM9 and 4 MB of RAM.

- **A homebrew DS client in C** ([BlocksDS](https://blocksds.skylyrac.net),
  no OS): its own Wi-Fi setup, discovery and pairing menus, a bitmap
  terminal renderer, a touch keyboard, a sound mixer, and a camera pipeline
  that sends pictures over Wi-Fi.
- **A Python server** that runs Claude Code in tmux, emulates the terminal
  with [pyte](https://github.com/selectel/pyte), and streams only the cells
  that changed. Every message fits a single 536-byte TCP segment, the DS's
  limit, and the terminal emulator works to a time budget so no program's
  output can stall the link.
- **Cryptography written twice and checked against itself.** X25519 pairing
  with numeric comparison, then ChaCha20 and HMAC-SHA256 on every frame,
  implemented in both C and Python, pinned to the RFC test vectors and
  cross-checked byte for byte ([how it works](docs/SEAL.md)).
- **Tested like it matters.** About 1,200 Python tests, tens of thousands of
  C checks under AddressSanitizer and UBSan at two screen sizes, seeded
  fuzzing of every parser that reads outside bytes, CI on every push, and
  latency measured under load ([numbers](docs/PERFORMANCE.md)).

[ARCHITECTURE.md](docs/ARCHITECTURE.md) walks through every part.

## Guides

| Guide | For |
|---|---|
| [Using it](docs/USING.md) | Every screen and button, approvals, QUICK, voice, REMOTE, the camera, `dsterm view` |
| [Your console](docs/HARDWARE.md) | Each DS model, the SD card, and the network |
| [Customizing](docs/CUSTOMIZING.md) | Your own sounds, buttons, snippets and settings, and changing the code |
| [Updating, from source, uninstalling](docs/GETTING_STARTED.md) | Everything after the first setup |
| [Troubleshooting](TROUBLESHOOTING.md) | `dsterm doctor`, and what each message on the DS means |
| [Performance](docs/PERFORMANCE.md) | What the server costs and how fast it answers, measured |
| [How it works](docs/ARCHITECTURE.md) | Every part of the repository and how they fit together |
| [Protocol](docs/PROTOCOL.md) and [encryption](docs/SEAL.md) | The wire format, for anyone writing another client or server |
| [Security](SECURITY.md) | The threat model, and how to report a problem |
| [Contributing](CONTRIBUTING.md) | Building, testing, and the rules the code keeps |

## Security, briefly

- **Pairing never sends a key.** The DS and the computer run an X25519
  exchange and you confirm the same code on both screens, which shuts out
  anyone in the middle. Each DS gets its own key; `dsterm devices forget`
  revokes one.
- **Every frame is encrypted and signed.** Nobody on your network can read
  your session or what you type, and a message that was forged, replayed or
  altered ends the connection instead of reaching it.
- **The DS can only do what a keyboard can.** Text pushed to it (snippets,
  the clipboard, project buttons) is shown in full and needs a second press
  before it's typed. GOD MODE, off until you turn it on and shown in red,
  lets project buttons skip that press.
- **What still shows on the network** is that a DS is talking to your
  computer, and when you type, but not what.

> [!WARNING]
> **Never expose the server's port to the internet.** To use it away from
> home, use a VPN that ends on your home network.

[SECURITY.md](SECURITY.md) has the full model.

## Status

Tested end to end on a DSi: Wi-Fi, discovery, pairing, the encrypted link, a
live Claude Code session, the buttons and touch keyboard, the camera, and
reconnecting after the console sleeps. The DS, DS Lite and 3DS family run the
same file, and their Wi-Fi follows the BlocksDS and TWiLight Menu++
documentation, but they haven't been tried on hardware yet. If you have one,
an **Install report** issue helps the next person most.

## Credits

Made by LC. MIT license; see [LICENSE](LICENSE). Built with
[BlocksDS](https://blocksds.skylyrac.net) and [pyte](https://github.com/selectel/pyte);
the sound set was generated with ElevenLabs and arranged for the DS.
Third-party notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

<sub>Not affiliated with Nintendo or Anthropic.</sub>
