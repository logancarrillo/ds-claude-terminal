# Your console

DS Claude Terminal is one homebrew file, `dsterm.nds`, that runs on every
console in the DS family. A stock console cannot run homebrew, so it needs a
one-time softmod (DSi, 3DS) or a flashcart (DS, DS Lite). This page covers
each model, the network, and what lives on the SD card.

Nothing here is specific to your console's firmware, and the app writes
nothing on the card but its own `dsterm/` folder and the files listed under
[The SD card](#the-sd-card).

## Which console

| Console | How it runs homebrew | Mode | Wi-Fi it can join | Notes |
|---|---|---|---|---|
| **DSi, DSi XL** | Unlaunch, optionally with TWiLight Menu++ | DSi | WPA2 (AES) or open | The reference console: everything is tested on it, camera included |
| **3DS, 3DS XL, 2DS, New 3DS, New 3DS XL, New 2DS XL** | Luma3DS custom firmware, then TWiLight Menu++ | DSi | WPA2 (AES) or open | All six behave the same. Expected to work like a DSi; not yet confirmed on hardware |
| **DS, DS Lite** | A Slot-1 flashcart | DS | **Open networks only** | A hardware limit of the DS radio |
| DSi or 3DS with a flashcart | The flashcart | DS | Open networks only | A flashcart always starts games in DS mode |

**Why the mode matters.** The DS radio, and a DSi or 3DS running in *DS mode*,
can only join open (and WEP) networks. *DSi mode* adds WPA2. The app asks for
DSi mode whenever the console has it, and when it cannot join a network it
says which of these is the reason and what to do about it, for the console
in your hands.

**The camera needs DSi mode too.** QUICK's PHOTO row exists only in DSi
mode — on a DSi or DSi XL, and on the 3DS family in DSi mode (its cameras
work there; their colour and orientation have not been confirmed on hardware
yet). There is no camera on a DS or DS Lite, or on anything running in DS
mode. On a DSi the LED beside the outer camera lights while the screen is
open; a 3DS has no camera light, so the red CAMERA ON strip is the signal
there.

The app knows which console it is on, and in which mode. It shows both in
the log at startup and under **ABOUT** on the Settings page: `DSi (DSi mode)`,
`3DS (DSi mode)`, `DS Lite`, and so on. A DSi or 3DS that says `(DS mode)`
there was started in DS mode and can only join open networks; the 3DS section
below says how to change that.

---

## DSi and DSi XL

Follow **<https://dsi.cfw.guide>**. It is well maintained and step by step;
use it rather than any summary, including this one. In outline:

1. Get an SD card (FAT32; 32 GB or smaller is the least trouble).
2. Install **Unlaunch** using the guide's exploit. From then on the console
   can start homebrew `.nds` files from the SD card.
3. Make the **NAND backup** when the guide says to, and keep it somewhere that
   is not the SD card. It is the only way back from a bricked console.
4. Optionally install **TWiLight Menu++** as a nicer launcher.

Copy `dsterm.nds` to the card and start it from Unlaunch or TWiLight Menu++.
Both start it in DSi mode, which is what Wi-Fi needs.

Avoid starting it through a DSiWare exploit (Memory Pit and similar) once
Unlaunch is installed: in that state the SD card may not be readable, and the
app stops with "cannot read the SD card".

## 3DS and 2DS family

All six models (3DS, 3DS XL, 2DS, New 3DS, New 3DS XL, New 2DS XL) run DS and
DSi software natively, in the same compatibility mode, so they are set up the
same way.

1. Install **Luma3DS** custom firmware: **<https://3ds.hacks.guide>**.
2. Install **TWiLight Menu++** (the CIA build) with Universal-Updater or FBI,
   as described at <https://wiki.ds-homebrew.com/twilightmenu/installing-3ds>.
3. Copy `dsterm.nds` to the SD card, open TWiLight Menu++ from the HOME Menu,
   and start it. TWiLight Menu++ runs homebrew like this in **DSi mode** by
   default; if Wi-Fi only offers open networks, open the game's settings
   (press **Y** on `dsterm.nds`) and set **Run in: DSi mode**.
4. Check: once it is running, **SETTINGS** shows `3DS (DSi mode)` under
   **ABOUT**. If it says `3DS (DS mode)`, go back to step 3.

Tips:

- **Sharper text.** The 3DS stretches DS screens to fill its own, which blurs
  small text. Hold **START** while opening TWiLight Menu++ from the HOME Menu
  to keep the original pixel size (with black borders). Don't hold SELECT:
  that opens TWiLight's own settings.
- **Wireless** must be on: the switch on older models, or the HOME Menu
  setting. If the app hears no networks at all, check this first.
- **Forwarders** (a HOME Menu icon that starts `dsterm.nds` directly) go
  through nds-bootstrap and have not been tested; start it from TWiLight
  Menu++ instead.

> The 3DS family is expected to work exactly like a DSi, Wi-Fi included: its
> radio supports WPA2 in this mode and the Wi-Fi library has the code for it.
> It has not yet been confirmed on hardware by the maintainer, so a report,
> either way, is very welcome.

## DS and DS Lite

The original DS and the DS Lite need a **Slot-1 flashcart** (R4 and its many
clones, Acekard 2i, DSTT, and similar). Use one whose menu runs homebrew and
patches it automatically (the "DLDI" step): Wood R4, AKAIO, YSMenu, or
TWiLight Menu++'s flashcart edition all do. Copy `dsterm.nds` to the
flashcart's microSD card and start it from the menu.

**Wi-Fi: open networks only.** The DS radio predates WPA2 and cannot join a
WPA or WPA2 network at all. Your options:

- A **hotspot from your computer** or phone, set to open (no password). Many
  Android phones offer "None" as the security type.
- A **spare router** set to an open 2.4 GHz network, with 802.11b allowed,
  plugged into your home network.

On an open network anyone in range can see that the DS is connected and
when you type, but every frame is encrypted and signed, so they can neither
read your session nor type into it. Still, use a network that only you use,
keep it switched off when you are not, and do not use a public hotspot. (WEP is refused: it adds a password without adding
protection.)

The app needs nothing else from a DS: it fits comfortably in the DS's 4 MB
and runs at full speed on its 67 MHz processor.

Older loaders (MoonShell, Slot-2 devices, PassMe) cannot start modern
homebrew directly. Start `dsterm.nds` through **nds-hb-menu** instead.

---

## The network

- **2.4 GHz.** No DS can see a 5 GHz network. A router that uses one name for
  both bands usually works; a 5 GHz-only network does not.
- **WPA2-AES (also called WPA2-Personal, CCMP)** for a DSi or 3DS. Not WPA3,
  and not "WPA2/WPA3 mixed" or "transition" mode, which many routers now use
  by default. If the DS sees your network but cannot join, mixed mode is the
  first thing to check. WPA (version 1) and Enterprise networks cannot be
  joined. The DS says which one it found.
- **802.11b/g allowed.** A router set to "n only" or "ax only" cannot be
  joined; the DS marks it "11g only".
- **Visible network name.** In DSi mode the app cannot find a hidden network.
- **An address from the router (DHCP), or a fixed one.** The DS asks the
  router first and gives up after eight seconds. If it does, the menu offers
  a fixed address: pick one **outside** the router's DHCP range, or the router
  may later give the same address to another device.
- **The same network as your computer.** The DS connects to the computer's
  address on your local network. VPN addresses (Tailscale and similar) are not
  reachable from the DS.

You do not need to save the network in the console's own settings. The app
has its own Wi-Fi menu.

## The SD card

All a card needs is the app:

```
/dsterm.nds
```

The DS keeps what it learns in its own folder, as plain text files you can
read and edit:

```
/dsterm/wifi.cfg             the network: ssid, wifi_key, ip = dhcp | a.b.c.d ...
/dsterm/servers/<name>.cfg   one file per paired computer: name, host, port, psk
/dsterm/last                 the computer it connects to at startup
/dsterm/id                   this console's device id, made once
/dsterm/sounds/              your own sounds, if any (CUSTOMIZING.md)
/dsterm/lang/                your own interface languages, *.lang, up to two (CUSTOMIZING.md)
/dsterm/quick.txt            the quick commands you saved with QUICK → COMMANDS → + ADD
/dsterm/recent.txt           your last ten prompts, for QUICK's AGAIN
/dsterm/backups/             earlier versions of the app, kept by `dsterm card`
/dsterm-settings.cfg         the Settings page's choices
/dsterm.snippets             optional canned prompts (CUSTOMIZING.md)
```

A `psk` in those files is that console's key for that computer. **Treat the
card like a password.** If one is lost, revoke its key on the computer:

```sh
dsterm devices                 # list the paired consoles
dsterm devices forget <id>
```

**Setting the card up from the computer.** With the card mounted:

```sh
dsterm card /Volumes/SDCARD --rom dsterm.nds --wifi --pair
```

writes the app, asks for the Wi-Fi name and password, and pairs the card with
this computer, so the DS connects on its first start with no typing. It backs
up the app already on the card, verifies everything it writes, never deletes
anything, and ejects the card.

**Cards from older versions** with a single `/dsterm.cfg` keep working: on the
first start, the DS copies its settings into `/dsterm/` and leaves the
original where it was.

## When something goes wrong

The menu says what failed in one line; the log under it, on the bottom
screen, has the detail.

| The DS says | What to do |
|---|---|
| `... is not in range. Is it 2.4 GHz ...` | A typo in the name, a 5 GHz-only network, 802.11b/g switched off, or (DSi mode) a hidden network |
| `could not join ... Wrong password, or WPA3 / mixed mode ...` | Check the password, then set the router to WPA2-AES only |
| `needs DSi mode` (in the network list) | A WPA2 network on a console in DS mode: the message says how to start in DSi mode, or that this DS cannot |
| `11g only` / `Enterprise` (in the list) | The router's settings: allow 802.11b; or use a WPA2-Personal network |
| `No networks heard` | Wireless is off (3DS: the switch or HOME Menu), or nothing on 2.4 GHz is in range |
| `joined ... but the router gave no address (DHCP)` | Choose **Use a static address** in the menu that follows |
| `No servers answered` (Find servers) | The server is not running (`dsterm doctor`), or a firewall blocks UDP 8346. On WSL in NAT mode, use **Scan this network** after `dsterm wsl-setup` |
| `pairing is not open: run dsterm pair` | Run `dsterm pair` on the computer, then pick it on the DS within two minutes |
| `The codes did not match` | Nothing was saved. Run `dsterm pair` and try again; if the two screens show different codes again, something else on the network is answering |
| `the computer said the codes did not match` | Someone typed `n` at `dsterm pair`. Run it again |
| `That server pairs the old way` | Update dsterm on the computer, `dsterm restart`, then pair |
| `the computer runs an older dsterm: update it there` | Update dsterm on the computer: an unencrypted connection is refused. The DS keeps retrying and connects once it is updated |
| `this card's dsterm.nds is older than the computer's` | Copy the current `dsterm.nds` to the card. No re-pairing |
| `No answer from a.b.c.d:8347` | The computer went away, or its firewall blocks TCP 8347: `dsterm doctor`. (If it only changed address, the DS has already looked for it by name; one that does not answer discovery either is off, asleep, or behind a firewall) |
| `... refused this card` | The keys no longer match: in **Saved servers**, press X on that computer to forget it, and pair again |
| `HALTED: cannot read the SD card ...` | DSi/3DS: start the app from Unlaunch or TWiLight Menu++ |
| `HALTED: no SD filesystem ...` | DS: your flashcart menu did not patch the app; try another flashcart kernel |
| `DS CLAUDE TERMINAL STOPPED` | A bug stopped the app: photograph the screen for a bug report, then switch the console off and on ([TROUBLESHOOTING.md](../TROUBLESHOOTING.md#things-doctor-cannot-see)) |

On the computer, `dsterm doctor` checks the server's side of every one of
these. [TROUBLESHOOTING.md](../TROUBLESHOOTING.md) has the rest.
