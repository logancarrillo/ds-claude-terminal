# Security

## The threat model, in one page

- **The link is authenticated, encrypted and signed.** A 128-bit key and an
  HMAC-SHA256 challenge-response decide who may connect. After that, every
  frame in both directions is encrypted with ChaCha20 and carries an HMAC tag
  over the ciphertext and a per-direction sequence number, under keys derived
  from the pairing key, that connection's nonce and the console's opening
  HELLO, which carries a fresh nonce of the console's own
  ([SEAL](docs/SEAL.md)). Nobody on your network can read the terminal, what
  you type, the clipboard or the pictures; a frame injected, replayed,
  reordered or altered ends the connection instead of reaching the session;
  and a recorded session played back at the console gets new keys and goes
  nowhere. None of it is optional: each side refuses a peer that does not
  encrypt, by name and never as a wrong key, so it cannot be negotiated away
  and a version mismatch never costs a pairing.
  What crosses in the clear: that a DS is talking to this computer, the
  computer's name in a discovery answer, the handshake (nothing in it is
  reusable), and each frame's type and length, which show when you type but
  not what.
- **The handheld gets the PTY and nothing else.** No message type runs a
  command of the console's choosing or reads a file, and no message from the
  console names a path; one message from the server — `PHOTO_SAVED` —
  carries the path of a picture it just saved, and it lands only in the
  composer. The one message that makes the computer run anything is the end
  of a recording, and what it runs was fixed when the server started (see
  **Voice**). A fully compromised client can type what a person at your
  keyboard could type, and nothing more.
- **An approval reaches only the prompt it was meant for.** The DS answers a
  permission prompt the way a keyboard does (A is Enter, X is Escape), and
  each prompt gets an id. An Enter pressed while a prompt is open names it,
  and the computer writes it only if that prompt is still the one waiting, so
  a double press, a delayed frame or a prompt answered at the computer in the
  meantime never approves the next one. Nothing times out: a prompt waits
  until a key reaches it, so a dropped link approves nothing
  ([Answers](docs/PROTOCOL.md#answers)).
- **Camera.** The card sends pixels and intents; the server alone picks the
  folder (`.dsterm/photos` in the project on screen, or `--photo-dir`) and the
  name. The path a `PHOTO_SAVED` answers with lands only in the composer, so
  it is read before it is sent. A `.dsterm` or `.git/info` planted as a link
  is refused rather than written through; pictures are mode 600 in a mode-700
  folder, only names the server itself made are ever deleted, and the folder
  keeps the newest twenty pictures (`--photo-keep`) and nothing older than a
  week. The live
  view goes to one Unix socket, mode 600 in a mode-700 folder with a uid check
  on Linux — never to the loopback HTTP dashboard, which shows only a red
  CAMERA ON badge. It is held in memory, never logged, and nothing is
  recorded but a picture you press A to send. The sensor is powered only
  while its screen is open, and the LED lights for either camera.
- **Voice stays on this computer unless you say otherwise.** Holding **R**
  in the composer records from the DS's microphone (the strip says
  LISTENING while it does); the recording goes to this computer over the
  encrypted link and is transcribed by a local speech-to-text program --
  `mlx-whisper`, `faster-whisper`, `openai-whisper`, `whisper-cli`, or your
  own `DSTERM_STT_CMD`. A cloud service (OpenAI, Groq, or `DSTERM_STT_URL`)
  is used only if you start the server with `--stt-cloud` (or
  `DSTERM_STT_CLOUD=1` in its environment) and nothing local is installed:
  an `OPENAI_API_KEY` that happens to be set sends nothing, a local backend
  that fails is never followed by an upload, a cloud URL must be https, a
  redirect is refused rather than followed, and a custom URL is sent
  `DSTERM_STT_KEY`, never your OpenAI key. `dsterm serve` prints where voice
  goes every time it starts, and `--no-mic` turns the microphone off
  entirely. The audio is written to a mode-600 temporary file for the
  transcriber and removed whatever happens; it is never logged or kept. The
  console contributes samples and a language hint, and only a two- or
  three-letter code reaches the transcriber's arguments; everything else
  about what runs was fixed at startup, as arguments, never through a shell,
  and under a deadline at which it is killed. The transcript -- third-party
  text, when the cloud was opted in -- is folded to printable ASCII and lands
  in the composer, where nothing is sent until you press **A**. The first
  run of a Python Whisper library may download its model weights; that is
  the library fetching a model, not your audio leaving.
- **`dsterm view` is the same door as `tmux attach`.** Its socket is mode
  600 in a mode-700 folder, with a uid check on Linux, and what is typed
  through it reaches the session `dsterm attach` already reaches. Its one
  new power is a tap on the DS's bottom screen, which the DS ignores unless
  the person holding it turned on VIEW TAPS (off until they do), and a tap
  can never arm GOD MODE. The DS sends its bottom screen, as text, only while
  a view is open; a glyph byte that is not a character arrives as `?`, so a
  card cannot put an escape sequence in the viewer's terminal.
- **Server-pushed text never becomes a keystroke on its own.** Snippets, the
  clipboard and the PROJECT page's buttons land in the composer and need a
  second, deliberate press. The one exception is GOD MODE, which the person
  holding the DS switches on and the page shows in red: it lets PROJECT's
  DO buttons run without that press, and never applies to snippets or the
  clipboard. This gate predates signed frames and stays, as a second line
  behind them.
- **REMOTE takes nothing from the wire.** A pick is a row index and an action
  number; the session id, directory and command come from the computer's own
  `claude agents` answer, are re-validated, and reach tmux as arguments, never
  through a shell.
- **Pairing makes a key without sending one, and a person checks it.**
  `dsterm pair` opens a two-minute window (a mode-600 file, not a socket).
  The console and server run X25519, and both screens show a six-digit code
  derived from the exchange; over the network, the key is saved only if the
  person says the codes match on both the DS and the computer (Bluetooth's
  numeric comparison). `dsterm card PATH --pair` is the offline way: it
  writes a fresh key onto an SD card mounted on this computer, and nothing
  crosses the air. The server commits to its nonce before seeing the console's,
  so someone in the middle cannot steer the two codes together: one chance in
  a million per attempt, nothing to guess offline, and every attempt is a
  window a person opened and is watching. A low-order key is refused. The
  pairing messages are the only ones allowed before authentication besides
  HELLO and AUTH, and the connection that carries them never authenticates:
  the card closes it after PAIR_DONE, and the server refuses anything more
  on it.
  Each console gets its own key, revocable with `dsterm devices forget`.
- **Discovery says nothing secret.** UDP 8346 answers a probe with the host
  name, the TCP port and whether pairing is open; it is parsed strictly and
  rate-limited.
- **The dashboard binds loopback only**, has no route that can type, answers
  only requests that name a loopback host (so a web page cannot reach it by
  rebinding its own domain), and its one action, restarting the child, needs
  a header another site's page cannot send.
- **Secrets stay off the ROM and out of the repo.** On the PC, keys live in
  `~/.config/ds-claude-terminal/` (`psk`, and `devices/` for paired consoles;
  mode 600, enforced). On the console they live in `/dsterm/servers/*.cfg` on
  the SD card -- treat the card like a password. Nothing is compiled into
  `dsterm.nds`. The card also keeps your last ten prompts, for QUICK's AGAIN,
  in `/dsterm/recent.txt`, and the quick commands you saved with + ADD in
  `/dsterm/quick.txt`.
- **Showing it to someone.** SETTINGS → SCREEN → ADDRESSES → HIDE stars out
  every IP and MAC address the DS draws, for a stream, a video or a photo.
  What Claude's own session prints is not changed.

Out of scope, and documented as such: exposing the port to the internet;
someone who can read your traffic *and* later steals a key (the keys come from
the pairing key, so the SD card or the computer's config directory would let
them decrypt what they recorded: there is no forward secrecy yet, see
[SEAL](docs/SEAL.md)); and traffic analysis of frame sizes and timing. Open
Wi-Fi networks can be joined, and a DS or DS Lite can join nothing else; with
every frame encrypted and signed, anyone in range can see that you are
connected but can neither read nor type into the session. WEP and WPA1 are
refused.

## How each claim is held up

Where each part of the model above is enforced, and the tests that would
fail if it stopped being true. CI runs all of them on every push.

| Claim | Enforced in | Tested by |
|---|---|---|
| A man in the middle of pairing gets two different codes | `pairing.py`, `dsclient/source/pair.c` | `tests/test_pairing.py` (`test_a_man_in_the_middle_gets_two_different_codes`, the commitment and low-order-key tests); the card's half pinned against the server's by `tools/crosscheck.py` |
| A pairing connection never authenticates | `server_pairing.py`, `server.py` (`_handle`) | `test_a_pairing_connection_cannot_reach_the_pty`, `test_a_pairing_connection_never_goes_on_to_authenticate` |
| Every frame after the handshake is signed; a forged, replayed, reordered, dropped, reflected or borrowed one ends the connection | `protocol.py` (`Seal`), `dsclient/source/seal.c` | `tests/test_seal.py`, `dsclient/tests/test_seal.c`; and in both fuzz suites a random tag is never accepted |
| Every frame after the handshake is encrypted: nothing typed or shown can be read off the wire | `chacha20.py` and `protocol.py` (`seal_frame`, `unseal`), `dsclient/source/chacha20.c` and `seal.c` | `tests/test_encryption.py` (`test_nothing_typed_or_shown_can_be_read_off_the_wire`: a recording proxy, and the card's own decoders run over what it caught); ChaCha20 against RFC 8439 on both sides and C against Python in `tools/crosscheck.py` |
| A recorded session replayed at the console cannot make it reuse a keystream | the card nonce (`boot_connect_nonce` in `boot.c`, `cn=` in the HELLO), `seal_start` | `tests/test_encryption.py` (`test_a_recorded_session_replayed_at_a_card_gets_nowhere`, `test_every_connection_has_its_own_card_nonce`), `dsclient/tests/test_seal.c` (a new card nonce under a replayed server nonce gives new keys) |
| Encryption cannot be negotiated away, and a version mismatch is never taken for a revoked key | `server.py` (the AUTH branch), `client.py`, `dsclient/source/main.c` (`handshake`) | `tests/test_seal.py` (`test_without_seal3_and_a_card_nonce_the_card_is_refused_by_name`, `test_a_card_names_a_server_from_before_encryption`, `test_a_card_refuses_an_auth_ok_without_seal`) |
| Nothing but HELLO, AUTH and pairing before authentication | `PRE_AUTH_ALLOWED` in `protocol.py`, enforced in `Server._handle` (`server.py`) | `tests/test_server.py` (`TestAuthorization`), `tests/test_fuzz.py` |
| An Enter reaches only the permission prompt it was pressed for | `server_agents.py` (questions), `dsclient/source/main.c` (`send_key_seq`) | `tests/test_answers.py` (a second press, a reading from before the answer, a plain Enter that crossed the prompt, the screen changing under it); `ANSWER` and `STATE`'s ask tail pinned C against Python by `tools/crosscheck.py` and `dsclient/tests/test_remote.c` |
| No message from the card runs a command, reads a file or names a path | `protocol.py`, each handler | `tests/test_protocol.py`: the type list, each with its written argument |
| Malformed input costs the connection, never the server or the card | every decoder and reader | `tests/test_fuzz.py` (decoders, SEAL, the server's own terminal emulator, a live server under card, pre-auth and view noise, and a program printing noise through a real PTY while PINGs are timed), `dsclient/tests/test_fuzz.c` (the frame reader, every server-to-client decoder, SEAL, discovery, the config, profile, snippet, settings and recent-prompt files, short typed texts, the list stagers, WAVs and COMMANDS, each input in an exact-size buffer, under ASan and UBSan) |
| REMOTE runs only `claude`, with arguments, never a shell | `remote.py`, `server_agents.py` | `tests/test_remote.py` (`test_new_window_argv_is_exact_and_shell_free`, the pick and row-check tests) |
| `dsterm view`'s socket is this user's only; a tap needs VIEW TAPS | `view.py`, `dsclient/source/main.c` | `tests/test_view.py`; the TOUCH and VIEW TAPS decoders in `dsclient/tests/test_core.c` |
| Voice is transcribed locally unless `--stt-cloud`; an API key alone uploads nothing; no fallback; one utterance per card under a deadline; a late transcript is dropped | `voice.py` (`choose_backend`, `Transcriber`), `server_voice.py` | `tests/test_voice.py` (`TestBackendChoice`, `test_a_failing_local_backend_never_falls_back_to_the_cloud`, `test_no_backend_and_no_opt_in_says_so_and_uploads_nothing`, the deadline, BUSY and disconnect tests) |
| Pictures land only in the photo folder; links are refused | `camera.py` | `tests/test_camera.py` (the symlink, retention and discard tests) |
| The dashboard is loopback only and cannot type | `dashboard.py` | `tests/test_dashboard.py` |
| Discovery says nothing secret and is rate-limited | `discovery.py` | `tests/test_discovery.py` |
| Pushed text needs a second press (GOD MODE aside) | `dsclient/source/main.c` | by review: `main.c` runs only on the DS, so the gate itself is not host-tested. What it calls is: `snippet_load_picked` fills only the composer, and the clipboard's paste rules are pinned (`dsclient/tests/test_core.c`) |

The pairing construction is Bluetooth's numeric comparison over X25519 and
HKDF-SHA256, implemented twice and cross-checked; it has not had an outside
review. One is welcome.

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** on this repository
(Security → Report a vulnerability) rather than a public issue. Especially
welcome: anything that lets a client reach the PTY before authenticating,
anything that lets pairing succeed without both people accepting matching
codes, or lets someone in the middle make the codes match,
anything that turns a server-pushed message into a keystroke without the
composer gate, any frame accepted without a valid SEAL tag on a sealed
connection, any way a REMOTE pick could run something other than `claude`,
any out-of-bounds read or write in the client's decoder or WAV reader, and
any way the dashboard could be reached from outside loopback. On voice: any
way a recording could leave the computer without `--stt-cloud`, any way a
key could reach a host other than its own service, and any way the console
could influence what the transcriber runs beyond the audio and a two- or
three-letter language code. On the camera:
any way a photo write could land outside the photo folder, and any way a
frame could reach the viewer socket as another user. On `dsterm view`: any
way another user could open its socket, and any way a tap could act on a DS
whose VIEW TAPS is off.
