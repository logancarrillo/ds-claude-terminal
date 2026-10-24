// DS Claude Terminal -- handheld client.
//
// Display and input only. It receives cells and sends key bytes; there is no
// message type that names a file, a path, or a command, so this program cannot
// ask the PC to do anything a person at the keyboard could not.
//
// Main screen: the 42x24 terminal. Sub screen: a status console, so connection
// problems are legible without disturbing the terminal.

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dswifi9.h>
#include <fat.h>
#include <nds.h>

#include "backoff.h"
#include "boot.h"
#include "buttons.h"
#include "camera.h"
#include "chime.h"
#include "composer.h"
#include "config.h"
#include "ext.h"
#include "focus.h"
#include "guide.h"
#include "host.h"
#include "kbd.h"
#include "keys.h"
#include "lang.h"
#include "log.h"
#include "mic.h"
#include "mirror.h"
#include "photo.h"
#include "privacy.h"
#include "quick.h"
#include "profiles.h"
#include "project.h"
#include "proto.h"
#include "remote.h"
#include "render.h"
#include "seal.h"
#include "sha256.h"
#include "snippets.h"
#include "sessions.h"
#include "settings.h"
#include "sound.h"
#include "status.h"
#include "subscreen.h"
#include "voice.h"
#include "wav.h"

// Snippets are optional.  The drive prefix is not knowable in advance: libfat
// mounts a DSi's internal SD as "sd:" and a flashcart as "fat:", and a bare
// path uses whichever fatInitDefault() made the default, so all three are
// tried (the legacy config is searched the same way, in boot.c). A console with no snippet file is not misconfigured, so a
// missing file is silent -- only a malformed one is reported.
static const char *SNIPPET_PATHS[] = {
    "/dsterm.snippets",
    "sd:/dsterm.snippets",
    "fat:/dsterm.snippets",
};
#define SNIPPET_PATH_COUNT (int)(sizeof(SNIPPET_PATHS) / sizeof(SNIPPET_PATHS[0]))


static dsterm_config_t cfg;
static framereader_t reader;
static grid_t grid;
// The PROJECT page, and the grid it is composed into.
//
// A second whole grid is 3 bytes x 1008 cells -- three kilobytes to make the
// page indistinguishable from the terminal to every other subsystem: the
// renderer draws it with the same painter, and an extension's safe-rect search
// finds places to stand on it without being told it is not the terminal.  That
// is a good trade on a machine with four megabytes and no allocator.
static project_t project;
static grid_t page_grid;
// Device facts worth showing, sampled where they are already read rather than
// read a second time: see link_refresh().
static unsigned health_battery;
static int health_rssi, health_rssi_known, health_external;
static int sock = -1;
// Per-frame integrity after the handshake (seal.h, docs/SEAL.md).  Reset with
// every connection; on only when the server's AUTH_OK said so.
static seal_t seal;

// The local composer. Client-only: while it is open no key reaches the PTY,
// and the only thing that ever sends is an explicit Send. That is the whole
// safety property of the composer -- a prompt being typed on a handheld must
// not be half-submitted by a stray press.
//
// The snippet list is a pane of it rather than a screen of its own. A snippet
// has never been anything but a body loaded into this buffer for a second
// deliberate press (snippets.h), so a separate tile and a separate overlay
// were two doors into the same room -- and they cost a tile on a screen that
// only has six.
typedef enum {
    COMPOSER_PANE_WRITE = 0,    // the buffer, being typed into
    COMPOSER_PANE_SNIPPETS,     // the list, to load one from
    // The desktop's clipboard. A pane rather than a screen for the same reason
    // snippets are one: it is text that arrives from elsewhere and needs the
    // deliberate second press before it can reach the PTY, which is exactly
    // what this buffer is for.
    COMPOSER_PANE_CLIP,
    COMPOSER_PANE_COUNT,
} composer_pane_t;

static composer_t composer;
static snippets_t snippets;
static composer_pane_t composer_pane = COMPOSER_PANE_WRITE;
// The desktop's clipboard, as far as this card knows about it. Reset with the
// link, because the most perishable thing this device can show is what
// somebody last copied on a machine it can no longer hear from.
static clip_t clip;
// Declared here because losing the link has to repaint the pane, and that
// happens a long way above where the rebuild is defined.
static void clip_resync(void);
static int snippet_pick = 0;     // highlighted row in the snippets pane
static int snippet_focus = 0;    // visible L+R focus state for that list
static int composer_focus_notice = 0;

// True only while the buffer itself is showing: the bottom-screen keyboard
// types into the composer in the WRITE pane and drives the list in the other,
// so one predicate owns that distinction rather than four call sites.
static inline int composer_writing(void)
{
    return composer.mode != COMPOSER_CLOSED &&
           composer_pane == COMPOSER_PANE_WRITE;
}

// The two lower-screen modes that also take the top screen.
//
// Both compose a whole page into `page_grid` and hand it to the ordinary cell
// renderer, so to everything downstream -- the painter, the never-cover-text
// rule, an extension's safe-rect search and its whole roaming machinery -- they
// are just another terminal.  Asked in one place because three call sites
// testing the same pair of enum values is how the third one gets forgotten
// and a page ends up drawn on a screen nobody handed it.
static inline int page_mode(screen_mode_t mode)
{
    return mode == SCREEN_MODE_PROJECT || mode == SCREEN_MODE_SETTINGS;
}

// The camera's: the saved picture a composer was opened on is owed a
// PHOTO_DISCARD when the composer leaves unsent.  Defined with the rest of
// the camera section; the forward declaration is all composer_close needs.
static void photo_discard_pending(void);

static voice_ptt_t mic_ptt;
static int mic_touch_held = 0;
static int composer_adding_quick = 0;

// Shut the whole overlay, both panes.  One function so that closing can never
// leave the pane state behind for the next open to inherit -- which is how a
// COMPOSE button comes to open a list of snippets.
static void composer_close(void)
{
    // Focus taken inside the composer ends with it: left on, the next A on
    // the Control screen would type the lit key into the session itself.
    if (focus_active()) focus_off();
    mic_cancel();
    voice_ptt_init(&mic_ptt);
    mic_touch_held = 0;
    composer_adding_quick = 0;
    photo_discard_pending();
    composer_cancel(&composer);
    composer_pane = COMPOSER_PANE_WRITE;
    snippet_focus = 0;
}

// Open the composer onto an empty buffer, the way every opener does: the
// write pane first, the snippet highlight home.  One function so an opener
// cannot inherit a pane the last session left behind.
static void composer_open_write(void)
{
    composer_open(&composer);
    composer_adding_quick = 0;
    composer_pane = COMPOSER_PANE_WRITE;
    snippet_pick = 0;
    snippet_focus = 0;
}

// Pickers live on the bottom screen, so the terminal stays
// visible while deciding where to work next.  Neither picker is allowed to
// leak a d-pad press or face key through to the PTY.
static sessions_t sessions;

// `dsterm view` (mirror.h): whether a view on the computer is watching the
// bottom screen -- the server's MSG_MIRROR, which is also the proof it knows
// MSG_SUBSCREEN -- and a tap it asked for, held down for a few frames so the
// touch code below sees it exactly as it would a finger.
static int mirror_watching = 0;
static uint32_t mirror_sent_hash = 0;
static unsigned long mirror_sent_at = 0;
static int inject_frames = 0;
static uint8_t inject_x, inject_y;
#define MIRROR_MIN_FRAMES 15        // at most four screens a second
#define MIRROR_REFRESH_FRAMES 120   // and one every two seconds regardless
#define MIRROR_UPLOAD_FRAMES 60     // once a second while a picture uploads
#define INJECT_TAP_FRAMES 4

// QUICK (quick.h): the list, which of its three levels is showing, the
// highlight, and the first row of a long sub-list on screen.
static int quick_menu = 0;
static int quick_level = SUBSCREEN_QUICK_MAIN;
static int quick_sel = 0;
static int quick_top = 0;
static int composer_photo_id = 0; // the saved picture's id while it is composed

static unsigned quick_held, quick_frames;
static quick_commands_t commands;
static quick_saved_t saved_commands;
static int quick_remove_armed = 0;
static quick_recent_t recent;
static quick_line_t typed_line;
// Claude's mode, as last read off its footer ("" until seen).
static char claude_mode[QUICK_MODE_MAX + 1];
// UNDO's rewind list is on screen: on Control the d-pad moves through it, as
// it does when Claude asks something.  Ends on Esc, after the two Enters a
// rewind takes, or after a minute and a half.
static int rewind_picking;
static int rewind_enters;
static unsigned rewind_frames;
#define REWIND_PICK_FRAMES (90 * 60)
#define RECENT_PATH PROFILES_ROOT "/recent.txt"
#define QUICK_PATH  PROFILES_ROOT "/quick.txt"
#define QUICK_TMP   PROFILES_ROOT "/quick.new"

// REMOTE: the server's list of Claude sessions, the computers this card can
// move to, and the picker over both (remote.h).  `remote_menu` is the picker
// being on screen.
static remote_list_t remote;
static remote_computers_t computers;
static remote_ui_t remote_ui;
static int remote_menu = 0;
static unsigned remote_held, remote_frames;
// The address this card is connected to, so the computers list can mark it.
static void remote_close(void);

// UI settings: a separate small file from any extension's, deliberately, so a
// corrupt one cannot cost somebody the other.
static settings_t ui_settings;
static settings_ui_t ui_settings_ui;
static settings_about_t ui_settings_about;
static const char *settings_path = "/dsterm-settings.cfg";
static int settings_written = 1;


// Frame counter, shared so the round-trip clock can be read from the message
// handler as well as the main loop.
static unsigned long frame_count = 0;

// ----------------------------------------------------------------- sched ---

// Wait for the next frame AND let the network stack run.
//
// Never swiWaitForVBlank() once Wi-Fi is up. BlocksDS schedules threads
// cooperatively and dswifi's lwIP port runs in one of them -- the library pulls
// in sys_thread_new and cothread_yield_irq. A BIOS halt stops this thread
// without ever handing the scheduler control, so lwIP gets no CPU and not one
// packet is processed.
//
// That single mistake produced every network failure this program has had, each
// of which looked like something else:
//
//   - ioctl(FIONBIO) "not implemented": it worked. recv() returned EAGAIN
//     forever because nothing was ever received, not because the flag was
//     ignored.
//   - select() with a zero timeout never reporting readable: it returns without
//     blocking, so it never yields, so no data can arrive.
//   - select() with a timeout blocking straight past it: sys_arch_sem_wait
//     waits on a signal, not a clock, so data arriving is the only thing that
//     wakes it. The main loop parked there and stopped polling the buttons.
static void sub_paint(void);
static void sound_apply(void);

static void frame_wait(void)
{
    cothread_yield_irq(IRQ_VBLANK);
    // The terminal remains a bitmap background; anything drawn over it as a
    // hardware OBJ stages its changes in RAM and commits them only here, just
    // after the VBlank yield.  Writing OAM or OBJ VRAM from a random input or
    // network path would visibly tear and risks competing with the hot
    // terminal renderer.  The same goes for the sound hardware.
    sound_apply();
    ext_vblank();
    // Immediately after the yield is the one moment the sub screen can be
    // rewritten without tearing, and it bounds the repaint rate to once a
    // frame no matter how many things asked for one. Every wait in this program
    // goes through here, including the boot loops, so a status() line still
    // appears on the next frame rather than waiting for the main loop to start.
    sub_paint();
    // The viewfinder rides the same moment: one fresh frame per repaint, into
    // the rows the strips do not own.
    photo_blit();
}

// ---------------------------------------------------------------- status ---

// The sub screen's text console is gone (replaced by the bitmap, subscreen.c),
// so status() appends to the log ring and repaints the sub screen instead of
// printing to a console.
//
// The last-rendered counters are kept here so a mid-loop status() (e.g. a
// protocol error) repaints with the real numbers rather than a flash of zeros.
static unsigned long last_rx = 0, last_msg = 0, last_tx = 0, last_loops = 0;
static unsigned last_raw = 0, last_lib = 0;

// Marking dirty is not painting.
//
// The sub screen has no back buffer: subscreen_update() writes straight into
// the bitmap the LCD is showing, so every repaint is visible. This used to be
// called synchronously from status() (once per log line), from every VIEW_POS,
// from every touch, and once a second -- and one scroll press produces a
// VIEW_POS plus a ~12-message resync, so a single thumb press could repaint the
// screen a dozen times in one frame. That is the flashing.
//
// Now every one of those callers marks the screen dirty and returns, and
// frame_wait() paints at most once per frame, just after the VBlank yield.
// Coalescing is the fix; painting less often would only have made it rarer.
static int sub_dirty = 1;

// The top-screen overlay has the same problem and the same fix. Its repaint is
// a render_clear() plus a full redraw, and it was being called on every
// composer keystroke -- so typing flashed the terminal screen exactly the way
// the bottom one flashed. Marked dirty, painted once a frame, and not painted
// at all while nothing changes.
static int overlay_dirty = 0;
static void overlay_repaint(void);
// The controls guide (guide.h): open or not, and the calls the menus that
// open it need before its section below.
static int guide_open = 0;
// The screen SELECT cycled away from on the press still held, or -1: when
// that SELECT turns out to be the first half of SELECT + START, the guide
// opens over the screen it was pressed on rather than the next one.
static int select_cycled_from = -1;
static void guide_show(guide_ctx_t ctx);
static void guide_paint_top(void);
static void overlay_sync(void);
static void page_repaint(void);
static void project_resync(void);
static void recent_remember(const char *text);
static void quick_saved_commit_from_composer(void);
static void focus_step(unsigned dp, unsigned long *tx);
static void typed_bytes(const char *bytes, size_t n);
static void rewind_done(void);

static void refresh_sub(void)
{
    sub_dirty = 1;
}

static void sub_paint(void)
{
    if (overlay_dirty) {
        overlay_dirty = 0;
        overlay_repaint();
    }
    if (!sub_dirty) return;
    sub_dirty = 0;
    subscreen_update(last_rx, last_msg, last_tx, last_loops, last_raw, last_lib);
}

static void status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
    refresh_sub();
}

// The services host.h promises, in terms of the statics above.  Thin on
// purpose: each one is the same call main.c makes for itself.
void host_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
    refresh_sub();
}

void host_refresh_sub(void)
{
    refresh_sub();
}

void host_frame_wait(void)
{
    frame_wait();
    frame_count++;
}

unsigned long host_frame_count(void)
{
    return frame_count;
}

void host_overlay_sync(void)
{
    overlay_sync();
}

void host_frame_yield(void)
{
    frame_wait();
}

const settings_t *host_settings(void)
{
    return &ui_settings;
}

int host_link_up(void)
{
    return sock >= 0;
}

// A blank separator line. log_blank() rather than log_printf(""): an empty
// format string is a warning, and warnings in this file are worth keeping at
// zero so real ones stand out.
static void blank_line(void)
{
    log_blank();
    refresh_sub();
}

// A reason longer than a row is wrapped at spaces rather than cut off: the
// part of a message that says how to fix something is usually its end.
static void status_wrapped(const char *prefix, const char *text)
{
    char line[LOG_COLS + 1];
    size_t used = (size_t)snprintf(line, sizeof(line), "%s", prefix);
    const char *word = text;
    while (*word) {
        while (*word == ' ') word++;
        const char *end = word;
        while (*end && *end != ' ') end++;
        const size_t wlen = (size_t)(end - word);
        if (used && used + 1 + wlen > LOG_COLS) {
            status("%s", line);
            used = 0;
            line[0] = '\0';
        }
        if (wlen) {
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%.*s",
                                     used ? " " : "", (int)wlen, word);
        }
        word = end;
    }
    if (used) status("%s", line);
}

static void halt(const char *why)
{
    blank_line();
    status_wrapped("HALTED:", why);
    blank_line();
    status("Fix it, then start dsterm again from your launcher.");
    sub_paint();   // nothing will call frame_wait() again; paint the reason
    while (1) cothread_yield_irq(IRQ_VBLANK);
}

// ------------------------------------------------------------------ time ---

// A microsecond clock that allocates no hardware timer.
//
// The main loop is VBlank-synced, so frame_count is a frame counter: 16715 us
// each at the DS's 59.8262 Hz. REG_VCOUNT refines that to the current scanline,
// 263 per frame, which is finer than the millisecond the status line reports.
// Scaling by 16715/263 rather than a rounded 63.6 us keeps it monotonic across
// a frame boundary; a clock that can go backwards produces a negative round
// trip that then prints as an enormous one.
//
// dswifi's half of the machine already owns hardware timers. Racing it for one
// to drive a cosmetic readout is exactly the kind of fault that would be
// attributed to the network for a week.
static unsigned long now_us(void)
{
    return frame_count * 16715UL + ((unsigned long)REG_VCOUNT * 16715UL) / 263UL;
}

// ----------------------------------------------------------------- sound ---

// sound.c owns cue/sample priorities, mute and volume policy; this owns the
// libnds calls. Terminal state stays on one PSG channel; the other hardware
// channel is reserved beside it for an extension's own lane, which can never
// replace or alter a terminal cue.
//
// Driven from frame_wait(), not the main loop, because reconnect() spends its
// backoff inside frame_wait(): a cue started as the link dropped would never
// advance a note otherwise, and the one sound you most need to hear would be
// the one that never plays.
// Fixed hardware channels rather than libnds's allocator, so a dynamic pick can
// never hand one user's channel to another and let its soundKill() stop the
// wrong sound.
//
// The square-wave voice must be on 8..13: those are the only channels with a
// square-wave generator.  14 and 15 are the noise channels, and a "PSG" note
// started on one plays white noise at that rate -- which is what every chip
// cue on this card was, from the day it was moved to 14 until this comment.
// PCM plays on any channel; the terminal's two lanes take 0 and 1, and 15 is
// left to an extension.
enum {
    TERMINAL_PSG_CHANNEL = 8,
    TERMINAL_PCM_CUE_CHANNEL = 0,
    TERMINAL_PCM_TICK_CHANNEL = 1,
};
static int tone_channel = -1;
static int pcm_channel[SOUND_LANES] = { -1, -1 };

static void sound_apply(void)
{
    sound_voice_t voice;
    if (sound_tick_voice(&voice)) {
        if (tone_channel >= 0) {
            soundKill(tone_channel);
            tone_channel = -1;
        }
        if (voice.hz) {
            tone_channel = soundPlayPSGChannel(
                TERMINAL_PSG_CHANNEL, (DutyCycle)voice.duty, (u16)voice.hz,
                voice.volume, 64);
        }
    }

    static const int lane_hw[SOUND_LANES] = {
        TERMINAL_PCM_CUE_CHANNEL, TERMINAL_PCM_TICK_CHANNEL,
    };
    for (unsigned lane = 0; lane < SOUND_LANES; lane++) {
        sound_pcm_t pcm;
        if (!sound_take_pcm(lane, &pcm)) continue;
        if (pcm_channel[lane] >= 0) {
            soundKill(pcm_channel[lane]);
            pcm_channel[lane] = -1;
        }
        if (!pcm.sample) continue;
        // The samples are const data in main RAM that the ARM7 reads
        // directly; nothing on this side ever writes them, so there is no
        // dirty cache line to flush first.
        pcm_channel[lane] = soundPlaySampleChannel(
            lane_hw[lane], pcm.sample->data,
            pcm.sample->format == SFX_FORMAT_PCM16 ? SoundFormat_16Bit
                                                   : SoundFormat_ADPCM,
            pcm.sample->bytes, pcm.hz, pcm.volume, 64, false, 0);
    }
}

// --------------------------------------------------------------- chime ---
//
// The sound cues are driven by chime.c, a pure traffic-state machine fed one
// fact at a time: a line was sent, cells changed, a resync is ours, the view
// is scrolled back. The settled logic that used to live here moved there,
// guards and all -- including the honesty rule that "done" means "the screen
// stopped changing", not "Claude is waiting for you".

// ------------------------------------------------------------------ link ---

// Once a second: probe the round trip, read the battery and the signal, and
// hand all of it to status.c for the Control screen's top line.
//
// The probe is a PING with no payload and the server answers with an equally
// empty PONG. The protocol has no correlation id and none is added here: at
// most one probe is outstanding at a time, so the next PONG is unambiguously
// the answer to the last PING. A probe that goes unanswered clears the reading
// rather than freezing it -- "--ms" is information, a stale number is not.
#define PROBE_TIMEOUT_US 3000000UL

static unsigned long probe_sent_us = 0;
static int probe_outstanding = 0;
static unsigned rtt_ms = 0;
static int rtt_known = 0;
// Whether the socket is up, as far as this client knows.  Set in the same
// three places the chime is told, and read by the PROJECT page.
static int link_connected = 0;

// The scrolling regime the server last announced (MSG_MODE). While the
// application owns its scrollback there is no server history to move through,
// so scroll gestures become PageUp/PageDown. Unknown until the first MODE,
// which arrives right after AUTH_OK -- the server always tells, even when the
// answer is "server scrolls", so 0 here before then means "not told yet".
static int app_scrolls = 0;
static int app_scrolls_known = 0;
// The permission prompt open on the computer, as the last MSG_STATE said:
// whether this connection has been sent an ask id at all (the proof that the
// server knows MSG_ANSWER), the id, and whether the state is an approval.
// An Enter pressed while it is goes as an ANSWER naming that prompt.
static int ask_known = 0;
static uint32_t ask_id = 0;
static int ask_open = 0;

// Battery, charger, and signal, read fresh each second.
//
// getBatteryLevel() already normalises the two consoles: on a DSi it is the
// hardware's own 0-15 reading, and on a DS/DS Lite -- which only distinguishes
// high from low -- libnds returns 15 or 3. So this is not DSi-only, but the DS
// Lite number is a two-state reading wearing a scale, which is why status.c
// prints "bat ok"/"bat LOW" there and bars ("n/4") only in DSi mode.
static void link_refresh(void)
{
    if (probe_outstanding && now_us() - probe_sent_us > PROBE_TIMEOUT_US) {
        probe_outstanding = 0;
        rtt_known = 0;
    }

    const u32 battery = getBatteryLevel();
    const unsigned battery_level = (unsigned)(battery & BATTERY_LEVEL_MASK);
    // A DS/DS Lite reports only high (15) or low (3); on a DSi the same
    // threshold is deliberately conservative.  This is a device fact, not a
    // guess from terminal shape, and the actor only reacts on a real edge.
    ext_battery(battery_level <= BATTERY_LEVEL_DS_LOW);
    // Documented as 0-255 and not using the buffer.
    const int rssi = Wifi_GetData(WIFIGETDATA_RSSI, 0, NULL);

    health_battery = battery_level;
    health_external = (battery & BATTERY_CHARGER_CONNECTED) != 0;
    health_rssi = rssi;
    health_rssi_known = rssi >= 0;

    status_link_set(rtt_ms, rtt_known,
                    battery_level, 1,
                    (battery & BATTERY_CHARGER_CONNECTED) != 0,
                    isDSiMode() ? 1 : 0,
                    rssi, rssi >= 0, sound_muted());
}

// ------------------------------------------------------------------- net ---

static int net_connect(void)
{
    status("tcp: %s:%u", cfg.server, cfg.port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        status("socket() failed: %d", errno);
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    if (!inet_aton(cfg.server, &addr.sin_addr)) {
        status("bad server address '%s'", cfg.server);
        return 0;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        // errno 113 EHOSTUNREACH here means no route -- e.g. a Tailscale
        // address, which the DS cannot reach. Use the LAN address.
        status("connect() failed: errno %d", errno);
        closesocket(sock);
        sock = -1;
        return 0;
    }

    // Non-blocking, so the main loop can poll the socket and the buttons in the
    // same pass without ever parking inside lwIP. FIONBIO is declared by
    // libnds's own sys/ioctl.h and implemented by lwip_ioctl; an earlier note
    // here claimed otherwise and was wrong -- what was broken was the thread
    // starvation described at frame_wait(), which made every read return EAGAIN
    // whatever the flag said.
    unsigned long nonblocking = 1;
    if (ioctl(sock, FIONBIO, &nonblocking) < 0) {
        status("FIONBIO failed: errno %d", errno);
        closesocket(sock);
        sock = -1;
        return 0;
    }

    status("tcp: connected");
    return 1;
}

// Read whatever is waiting without blocking.
// Returns bytes read, 0 if nothing is ready, -1 on close or a real error.
static int try_recv(uint8_t *buf, size_t len)
{
    errno = 0;
    const int n = recv(sock, buf, len, 0);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;   // n == 0 is the peer closing
}

// A non-blocking socket can refuse a write when the send buffer is full, so a
// short send is retried across frames rather than dropped. Bounded: a send that
// cannot drain in a second means the link is gone, and reporting that beats
// looping forever with a screen that no longer updates.
#define SEND_ATTEMPT_FRAMES 60

static int send_all(const uint8_t *buf_in, size_t len)
{
    // Every caller hands over exactly one frame, which is what lets the seal
    // be applied here, once, for all of them.  The card's frames are a few
    // bytes; one that is not is refused rather than sent unsealed.
    // Static, not stack: a camera frame is 3 KB, and there is no need to pay
    // a 3 KB stack frame for every KEY byte sent.
    static uint8_t sealed[PROTO_HEADER_SIZE + PROTO_MAX_CARD_PAYLOAD + SEAL_TAG];
    const uint8_t *buf = buf_in;
    if (seal.on) {
        if (len + SEAL_TAG > sizeof(sealed)) {
            status("refused to send an oversized frame");
            return 0;
        }
        memcpy(sealed, buf_in, len);
        len = seal_frame(&seal, sealed, len, sizeof(sealed));
        if (!len) {
            status("could not seal a frame");
            return 0;
        }
        buf = sealed;
    }
    size_t sent = 0;
    int stalls = 0;

    while (sent < len) {
        errno = 0;
        const int n = send(sock, buf + sent, len - sent, 0);
        if (n > 0) {
            sent += (size_t)n;
            stalls = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > SEND_ATTEMPT_FRAMES) return 0;
            frame_wait();
            continue;
        }
        return 0;
    }
    return 1;
}

// Block until one message arrives, or `frames` pass. Returns 1 on success.
// `what` names the thing being awaited, so a timeout says which step stalled.
static int recv_message(msg_type_t *type, const uint8_t **payload,
                        uint16_t *len, int frames, const char *what)
{
    uint8_t chunk[1024];

    for (int frame = 0; frame < frames; frame++) {
        proto_result_t r = framereader_next(&reader, type, payload, len);
        if (r == PROTO_OK) return 1;
        if (r != PROTO_NEED_MORE) {
            status("protocol error: %s", proto_strerror(r));
            return 0;
        }

        const int n = try_recv(chunk, sizeof(chunk));
        if (n < 0) {
            status("server closed (errno=%d)", errno);
            return 0;
        }
        if (n > 0) {
            const size_t room = framereader_space(&reader);
            if ((size_t)n > room) {
                // Cannot happen with a well-behaved server: the largest message
                // is a full frame at 3030 bytes and the buffer holds 8198.
                status("receive buffer overflow");
                return 0;
            }
            framereader_feed(&reader, chunk, (size_t)n);
            continue;   // drain what is queued before waiting again
        }
        frame_wait();
    }
    status("timed out awaiting %s", what);
    return 0;
}

// --------------------------------------------------------------- handshake -

// Why the last handshake was refused, in the server's own words, for the boot
// menu to show ("grid: this server is 64x32, this card is 42x24").  Empty
// when it was refused without one.
static char refusal[64];

static int handshake(void)
{
    refusal[0] = '\0';
    // Sized in proto.h for the larger of the HELLO and AUTH frames: the
    // capability list once outgrew the MAC, and this exact buffer put six
    // bytes of a token through the saved registers.
    uint8_t out[PROTO_HANDSHAKE_BUF_SIZE];
    size_t n;

    // The payload is a whitespace-separated capability list: a version token,
    // then feature tokens. Unknown tokens are ignored by both sides, so
    // announcing "mode" costs nothing against an older server -- and it is the
    // gate for MODE, which the server sends only to a card that asked for it.
    //
    // The extension may append tokens of its own.  One that does not fit
    // the bound proto.h sets announces nothing: the terminal's tokens are
    // the ones the link needs, and a truncated token is a lie.
    //
    // A paired card adds its device id, which tells the server which key to
    // check the AUTH below against.  proto_hello_payload() owns the bounds
    // (and is host-tested at its worst case): an id that is not exactly eight
    // hex digits, or extension tokens that do not fit, are left out whole.
    // A new connection starts unsealed; AUTH_OK decides (docs/SEAL.md).
    seal_reset(&seal);

    // This connection's own nonce, in the HELLO and so in the seal's keys:
    // a server replayed at the card can never steer it back into an old
    // connection's keystream (docs/SEAL.md).
    uint8_t card_nonce[PROTO_CARD_NONCE_BYTES];
    boot_connect_nonce(card_nonce);
    char caps[PROTO_HELLO_MAX + 1];
    const char *ext = ext_hello_tokens();
    int caps_len = proto_hello_payload(caps, sizeof(caps), cfg.device_id,
                                       card_nonce, ext, photo_available());
    if (ext && ext[0] && !strstr(caps, ext)) {
        status("extension tokens too long; announcing core only");
    }
    n = proto_encode(out, MSG_HELLO, (const uint8_t *)caps,
                     (uint16_t)caps_len);
    if (!send_all(out, n)) { status("failed to send HELLO"); return 0; }
    status("sent HELLO, awaiting nonce");

    msg_type_t type;
    const uint8_t *payload;
    uint16_t len;
    if (!recv_message(&type, &payload, &len, 60 * 10, "NONCE")) return 0;

    if (type == MSG_ERROR) {
        status("server refused: %.*s", (int)len, (const char *)payload);
        return 0;
    }
    if (type != MSG_NONCE || len != PROTO_NONCE_SIZE) {
        status("expected NONCE, got 0x%02X len %u", type, len);
        return 0;
    }

    // The pairing key never crosses the link -- only proof that we hold it.
    // On an open or WEP network anyone can read this exchange and learn nothing
    // reusable, because the nonce is fresh per connection.
    // Kept: the seal's key is derived from it after AUTH_OK, and `payload`
    // points into the frame reader, which the next read overwrites.
    uint8_t nonce[PROTO_NONCE_SIZE];
    memcpy(nonce, payload, PROTO_NONCE_SIZE);
    uint8_t mac[PROTO_MAC_SIZE];
    hmac_sha256(cfg.psk, CFG_PSK_BYTES, payload, len, mac);

    n = proto_encode(out, MSG_AUTH, mac, PROTO_MAC_SIZE);
    if (!send_all(out, n)) { status("failed to send AUTH"); return 0; }
    status("sent AUTH, awaiting result");

    if (!recv_message(&type, &payload, &len, 60 * 10, "AUTH_OK")) return 0;
    if (type == MSG_ERROR) {
        // A refusal with a reason (the server answers HELLO before AUTH).
        snprintf(refusal, sizeof(refusal), "%.*s",
                 (int)(len < sizeof(refusal) ? len : sizeof(refusal) - 1),
                 (const char *)payload);
        for (char *c = refusal; *c; c++) {
            if (*c < 0x20 || *c > 0x7e) *c = '?';
        }
        status_wrapped("server refused:", refusal);
        return 0;
    }
    if (type == MSG_AUTH_FAIL) {
        status("auth rejected: psk does not match the server");
        return 0;
    }
    if (type != MSG_AUTH_OK) {
        status("expected AUTH_OK, got 0x%02X", type);
        return 0;
    }

    // "seal3" in AUTH_OK: from the next frame on, both directions are
    // encrypted and signed.  There is no fallback.  "seal" is a server from
    // before encryption, which this card names so the person knows which
    // side to update; anything else is too old to sign or someone in the
    // middle stripping the word, and on an open network those look the same.
    // Every refusal returns 0 and the reconnect loop tries again: the
    // pairing is never forgotten over a version.
    if (len != 5 || memcmp(payload, "seal3", 5) != 0) {
        if (len == 4 && memcmp(payload, "seal", 4) == 0) {
            snprintf(refusal, sizeof(refusal), "the computer runs an older "
                     "dsterm: update it there");
        } else {
            snprintf(refusal, sizeof(refusal), "the computer does not "
                     "encrypt its frames: update dsterm there");
        }
        status_wrapped("refused:", refusal);
        return 0;
    }
    seal_start(&seal, cfg.psk, CFG_PSK_BYTES, nonce,
               (const uint8_t *)caps, (size_t)caps_len);
    status("authenticated; frames encrypted");
    return 1;
}

// ------------------------------------------------------------- reconnect ---

// Reconnect for as long as the console is on.
//
// Everything on the server exists to make this work: the session lives in tmux
// and outlives the server process, the child is respawned when it exits, and a
// client that proves it holds the key displaces its own stale connection. The
// handheld halting on the first blip wasted all of that and meant walking to
// the console to relaunch it from Unlaunch.
//
// The terminal is deliberately left on screen. The last thing it displayed is
// the most useful thing to look at while this is happening, and the server
// resends every cell on authentication anyway.
// Everything that belongs to one connection, forgotten: the next server
// proves its capabilities again or they stay off.  One function, so that the
// two ways a connection ends -- a reconnect and a REMOTE switch -- cannot
// forget different things (they once did: a switch kept the old computer's
// PROJECT buttons, whose text would have typed into the new session).
// `tidy_pages` hands the server-owned PROJECT tabs and the clipboard back to
// local facts; a reconnect does it once, not on every retry.
static void connection_forget(int tidy_pages)
{
    if (sock >= 0) {
        closesocket(sock);
        sock = -1;
    }
    // Any bytes left from the dropped connection would be read as the start
    // of the new stream and desynchronise the framing immediately.
    framereader_init(&reader);
    seal_reset(&seal);
    // A view watches one connection; the next server says again.
    mirror_watching = 0;
    inject_frames = 0;
    // A session list is a capability proof for this particular connection.
    // Do not retain an old 0x97 list across a reconnect: an older server
    // must never receive 0x15 merely because the previous one supported it.
    // Closing the picker also keeps a stale selection off screen.
    sessions_init(&sessions);
    // The camera's proof belonged to the dead link: the hardware closes
    // without a goodbye byte (a dead socket is told nothing), and the saved
    // picture's id -- per connection on the server -- is forgotten.
    photo_forget();
    mic_forget();
    composer_photo_id = 0;
    quick_set_photo_note(TR_QUICK_NOTE_NOT_HERE);
    quick_set_voice_note(TR_QUICK_NOTE_NOT_HERE);
    // The computer's commands belong to this connection's computer.
    quick_commands_init(&commands);
    rewind_done();
    // REMOTE and STATE likewise.  The picker closes without remote_close():
    // telling a dead socket it closed would be a failed send, and a failed
    // send reconnects -- from inside the reconnect.
    remote_init(&remote);
    if (remote_menu) {
        remote_menu = 0;
        boot_peers_close();
    }
    chime_claude_reset();
    ext_menu_reset();
    status_view_pos_clear();
    // The regime belongs to the connection that just died: the server
    // re-announces it after every AUTH_OK. Carrying a stale regime across
    // would make a physical button do the wrong thing on the new link.
    status_scroll_regime_clear();
    app_scrolls = 0;
    app_scrolls_known = 0;
    // The prompt was the old connection's; the new one says again.
    ask_known = 0;
    ask_open = 0;
    // The probe belonged to the connection that just died. Carrying its
    // round trip across would print a number measured against a socket that
    // no longer exists.
    probe_outstanding = 0;
    rtt_known = 0;
    // The link line owns a separate cached copy of RTT. Refresh it now so a
    // manual RELINK does not leave a number measured on the dead socket
    // visible until the next once-a-second status pass.
    link_refresh();
    if (tidy_pages) {
        // A branch name from a server that stopped talking is a stale truth,
        // and a stale truth on a status page is worse than an admission that
        // nobody is reporting.
        project_wire_reset(&project);
        clip_wire_reset(&clip);
        clip_resync();
    }
    overlay_sync();
}

static void reconnect(void)
{
    for (unsigned attempt = 1; ; attempt++) {
        connection_forget(attempt == 1);
        if (attempt == 1) {
            link_connected = 0;
            ext_link(0);
            chime_link_changed(0);
        }

        status("reconnecting (attempt %u)", attempt);

        if (net_connect() && handshake()) {
            status("reconnected");
            link_connected = 1;
            ext_link(1);
            chime_link_changed(1);
            // The reconnect resend is this client's own doing; it must not be
            // read as the session producing output.
            chime_view_sent();
            return;
        }

        const unsigned delay = backoff_frames(attempt);
        for (unsigned f = 0; f < delay; f++) {
            frame_wait();
            // Keep the counters and the log moving, so a console stuck here
            // still shows something changing rather than looking hung.
            scanKeys();
            if (f % 60 == 0) refresh_sub();
        }
    }
}

// The PHOTO screen's one way onto the wire (photo.c): whole frames of the
// camera's own types, counted like a keystroke, and a failed send the same
// dead-link evidence send_key_seq treats it as.  Any other type is refused,
// so the camera can send pictures and never type.
int host_send_camera(const uint8_t *frame, size_t n, unsigned long *tx)
{
    if (n < PROTO_HEADER_SIZE) return 0;
    if (!proto_camera_type(frame[3])) {
        status("camera: refused a frame that is not the camera's");
        return 0;
    }
    if (send_all(frame, n)) {
        if (tx) (*tx)++;
        return 1;
    }
    status("send failed; connection lost");
    reconnect();
    return 0;
}

int host_send_voice(const uint8_t *frame, size_t n, unsigned long *tx)
{
    if (n < PROTO_HEADER_SIZE) return 0;
    if (!proto_voice_type(frame[3])) {
        status("voice: refused a frame that is not the microphone's");
        return 0;
    }
    if (send_all(frame, n)) {
        if (tx) (*tx)++;
        return 1;
    }
    status("send failed; connection lost");
    reconnect();
    return 0;
}

// ----------------------------------------------------------------- input ---

// Edges are computed here rather than taken from keysDown().
//
// On hardware libnds reported 0000 for every button, including A and the d-pad,
// which the ARM9 reads straight out of REG_KEYINPUT -- those cannot depend on
// the ARM7, on Wi-Fi, or on anything else this program does. Reading the
// register directly removes the whole question.
//
// REG_KEYINPUT is active-low and its low ten bits sit at exactly the same
// positions as libnds's KEY_* constants, so the mapping below is unchanged. X
// and Y are the exception: they live in REG_KEYXY, which only the ARM7 can
// read, so they arrive through libnds. Folding them in is the pure key-word
// assembly in kbd.c (kbd_held_keyword), so one complete word reaches every
// consumer instead of each one patching the missing bits for itself -- the
// duplication that made Y-space dead in focus while the face-key table worked.

static unsigned prev_raw = 0;

// d-pad key repeat: the direction currently held and how many frames it has
// been held. The arrow fires on the edge, then at the shared repeat cadence
// (400 ms then every 60 ms, from kbd.c) while the same direction stays held.
static unsigned dpad_held = 0;
static unsigned dpad_frames = 0;
// A view chord owns its d-pad direction until that direction is released. If
// L is released first, the still-held direction must not become a normal arrow
// or enter the repeat clock halfway through a press.
static int dpad_suppressed = 0;
// Frames DOWN has been held after a scroll on the Control screen; at
// LIVE_HOLD_FRAMES the view jumps straight back to the newest output.
static unsigned live_hold_frames = 0;
#define LIVE_HOLD_FRAMES 30

// Touch state. The first frame a pen lands reports a garbage coordinate, so it
// is swallowed; the press registers on the next frame, and the touched key
// stays held -- inverted and repeating -- until the pen lifts.
static int touch_was_down = 0;


static int touch_held(void)
{
    return (keysHeld() & KEY_TOUCH) || inject_frames > 0;
}

static void touch_read(touchPosition *pos)
{
    if (inject_frames > 0) {
        memset(pos, 0, sizeof(*pos));
        pos->px = inject_x;
        pos->py = inject_y;
        return;
    }
    touchRead(pos);
}
int host_touch_held(void)
{
    return touch_held();
}

void host_touch_point(int *x, int *y)
{
    touchPosition pos;
    touch_read(&pos);
    *x = pos.px;
    *y = pos.py;
}

static unsigned touch_frames = 0;
static int touch_key = -1;
// The user's own keypress, voiced.  Any texture stops for it; then the
// extension may take it, and the conductor's PSG tick answers if not.
static void key_tick(int backspace)
{
    sound_phrase_stop();
    if (!ext_key_tick(backspace)) sound_key_typed(backspace);
}

void host_key_tick(int backspace)
{
    key_tick(backspace);
}

// Returns 1 if the bytes left the console. Most callers are a single keystroke
// and ignore it, but a multi-frame burst has to know: see composer_send().
static int send_key_seq(const char *seq, size_t seq_len, unsigned long *tx)
{
    uint8_t out[PROTO_HEADER_SIZE + PROTO_MAX_KEY_PAYLOAD];
    // Refuse rather than overflow. Every caller until now passed one to three
    // bytes, so the fixed buffer was sized for a keystroke and the bound was
    // an unwritten assumption -- and the first caller with a kilobyte of
    // clipboard in hand would have written 954 bytes past the end of a stack
    // array on a device with no MMU. Chunking is send_key_text()'s job; this
    // one refuses anything that does not fit a single KEY frame.
    if (seq_len > PROTO_MAX_KEY_PAYLOAD) {
        status("refused an oversized key frame (%u bytes)", (unsigned)seq_len);
        return 0;
    }
    // Enter while Claude is asking: tie it to the prompt on screen, so it
    // reaches that prompt or nothing (docs/PROTOCOL.md, "Answers").
    static uint8_t answer[PROTO_HEADER_SIZE + PROTO_ANSWER_MAX];
    const uint8_t *frame = out;
    size_t n;
    if (ask_known && ask_open && memchr(seq, '\r', seq_len)) {
        n = proto_encode_answer(answer, ask_id, (const uint8_t *)seq, seq_len);
        frame = answer;
    } else {
        n = proto_encode(out, MSG_KEY, (const uint8_t *)seq, (uint16_t)seq_len);
    }
    if (n && send_all(frame, n)) {
        (*tx)++;
        // A lone CR is a submission -- the composer's final frame, the ENT
        // key, or the physical A button all arrive here as one byte. That is
        // the one transmission the user means as "send", and the only one
        // that cues. This single site is why the send sound works for live
        // typing and the composer alike: the sound follows the wire, not the
        // UI path that produced the bytes.
        // ...but only when it submits something: chime_typed() says whether
        // anything was typed on the line first (chime.h).
        if (chime_typed(seq, seq_len)) {
            ext_event(EXT_EVENT_LOCAL_SEND);
            chime_sent();
        }
        return 1;
    }
    // A refused write means the connection is gone as surely as a refused read
    // does, and it is often noticed here first: the handheld sends only when a
    // key is pressed, so an idle console can sit on a dead socket for minutes
    // before a read would reveal it. The keystroke itself is lost -- resending
    // it after the handshake would deliver it to whatever the screen became.
    status("send failed; connection lost");
    reconnect();
    return 0;
}

// Type a run of text at the PTY, in KEY-frame-sized pieces.
//
// The same plan the composer uses for its body -- <=64-byte chunks -- but
// without the trailing CR, because this is a paste rather than a submission.
// That distinction is the whole safety argument for pasting at all: text with
// no newline in it lands on the prompt line and waits for the person to press
// the submit key, so it has not executed on its own.
//
// Stops at the first refused frame. A half-delivered paste is bad, but the
// alternative is carrying on writing into a socket that has already been
// reported dead.
static int send_key_text(const char *text, size_t len, unsigned long *tx)
{
    size_t at = 0;
    while (at < len) {
        size_t take = len - at;
        if (take > PROTO_MAX_KEY_PAYLOAD) take = PROTO_MAX_KEY_PAYLOAD;
        if (!send_key_seq(text + at, take, tx)) return 0;
        at += take;
    }
    return 1;
}

// Send a signed 16-bit view delta in network byte order. Converting through
// uint16_t makes the two's-complement wire bytes explicit rather than relying
// on implementation-defined right shift of a negative int.
static void send_view_delta(int16_t lines, unsigned long *tx)
{
    // The resync this triggers is this client's own doing; it must not be
    // mistaken for a session that produced output and then settled.
    chime_view_sent();

    const uint16_t wire = (uint16_t)lines;
    const uint8_t payload[2] = {
        (uint8_t)(wire >> 8), (uint8_t)(wire & 0xFF)
    };
    uint8_t out[PROTO_HEADER_SIZE + sizeof(payload)];
    const size_t n = proto_encode(out, MSG_VIEW, payload, sizeof(payload));
    if (send_all(out, n)) {
        (*tx)++;
        return;
    }
    status("send failed; connection lost");
    reconnect();
}

void host_link_refresh(void)
{
    link_refresh();
}

const sessions_t *host_sessions(void)
{
    return &sessions;
}

void host_view(host_view_t *out)
{
    out->terminal = &grid;
    out->page = &page_grid;
    out->page_mode = page_mode(status_mode_get());
    out->composer_open = composer.mode != COMPOSER_CLOSED;
    out->session_menu_open = quick_menu || remote_menu ||
                           photo_is_open();
}

// The scroll gesture, routed by regime. When the application owns its
// scrollback the server has no history to move through, and the app's own
// transcript scrolls with PageUp/PageDown -- confirmed working against Claude
// Code on a live session (docs/design/SCROLLBACK_MEASUREMENT.md). Otherwise the
// server's history is the thing to move through and the same gesture sends
// VIEW. The bytes sent in either regime are exactly what a keyboard would
// send: PageUp/PageDown, or an intent to move the view.
static void scroll_gesture(int back, unsigned long *tx)
{
    if (!app_scrolls_known) {
        status("waiting for selected session mode");
        return;
    }
    if (app_scrolls) {
        send_key_seq(back ? SEQ_PAGE_UP : SEQ_PAGE_DOWN, SEQ_PAGE_LEN, tx);
        return;
    }
    send_view_delta(back ? -KBD_VIEW_STEP : KBD_VIEW_STEP, tx);
}

// One probe per second. A failed send is the same evidence of a dead link that
// a failed keystroke is, and is handled the same way.
static void send_ping(unsigned long *tx)
{
    uint8_t out[PROTO_HEADER_SIZE];
    const size_t n = proto_encode_empty(out, MSG_PING);
    if (!send_all(out, n)) {
        status("send failed; connection lost");
        reconnect();
        return;
    }
    (*tx)++;
    // The PING still goes every second -- it is also how a dead link is
    // noticed -- but the round trip is timed from the probe still awaiting
    // its PONG, so a slow answer is not measured against a later ping.
    if (!probe_outstanding) {
        probe_sent_us = now_us();
        probe_outstanding = 1;
    }
}

// ------------------------------------------------------------- composer ---

// Repaint whichever local overlay owns the top screen, or hand the screen back
// to the terminal. render_overlay_close() repaints from the grid this client
// already holds, so dismissing the composer costs no round trip and needs no
// resync -- the cells were never overwritten in the model, only on the LCD.
static void overlay_repaint(void)
{
    // The guide stands over everything it can be opened above.
    if (guide_open) {
        guide_paint_top();
        return;
    }
    if (remote_menu) {
        remote_row_t rows[REMOTE_ROWS_MAX];
        const unsigned n = remote_rows(&remote, &computers, rows, REMOTE_ROWS_MAX);
        static char title[64], body[640], footer[64];
        remote_explain(&remote_ui, &remote, &computers, rows, n,
                       boot_server_name(), title, sizeof(title),
                       body, sizeof(body), footer, sizeof(footer));
        render_overlay_text(title, body, footer);
        return;
    }

    if (composer.mode != COMPOSER_CLOSED &&
        composer_pane == COMPOSER_PANE_CLIP) {
        char summary[CLIP_LINE_COLS + 1];
        static char lines[12][CLIP_LINE_COLS + 1];
        clip_summary(&clip, summary, sizeof(summary));
        const unsigned n = clip_wrap(&clip, lines, 12);

        // One buffer, the summary then the text, because render_overlay_text
        // takes a body rather than a list. Bounded by construction: twelve
        // lines of forty-two plus a forty-two column summary.
        static char body[CLIP_LINE_COLS + 2 + 12 * (CLIP_LINE_COLS + 1) + 1];
        size_t at = 0;
        at += (size_t)snprintf(body + at, sizeof(body) - at, "%s\n\n", summary);
        for (unsigned i = 0; i < n && at < sizeof(body) - 1; i++) {
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%s\n",
                                   lines[i]);
        }
        render_overlay_text("COMPOSE / CLIPBOARD -- FROM THE DESKTOP", body,
                            clip_pastable(&clip)
                                ? "A LOAD IT   Y WRITE   X CANCEL"
                                : "Y WRITE   X CANCEL");
        return;
    }

    if (composer.mode != COMPOSER_CLOSED &&
        composer_pane == COMPOSER_PANE_SNIPPETS) {
        static char labels[SNIPPETS_MAX][SNIPPET_LABEL_MAX + 3];
        static const char *rows[SNIPPETS_MAX];
        for (int i = 0; i < snippets.count && i < SNIPPETS_MAX; i++) {
            snprintf(labels[i], sizeof(labels[i]), "%c %s",
                     i == snippet_pick ? '>' : ' ', snippets.items[i].label);
            rows[i] = labels[i];
        }
        if (snippets.count == 0) {
            render_overlay_text("COMPOSE / SNIPPETS -- NONE",
                                "No dsterm.snippets on the card.\n\n"
                                "One label=body per line; \\n for a\n"
                                "newline. See dsterm.snippets.example.",
                                "Y WRITE   X CANCEL");
            return;
        }
        // The title names both panes and which one is showing, because the
        // pane key is the only way back and a screen that does not say it has
        // panes has one door that does nothing.
        render_overlay_list(snippet_focus ? "COMPOSE / SNIPPETS -- FOCUS"
                                          : "COMPOSE / SNIPPETS",
                            rows, snippets.count, snippet_pick, snippet_focus,
                            snippet_focus ? "UP/DOWN MOVE  A PICK  B/L+R OUT" :
                                            "UP/DOWN  A PICK  Y WRITE  X OUT");
        return;
    }

    // AGAIN: the highlighted prompt in full, since a row shows only its start.
    if (composer.mode == COMPOSER_CLOSED && quick_menu &&
        quick_level == SUBSCREEN_QUICK_AGAIN &&
        (unsigned)quick_sel < recent.count) {
        render_overlay_text("AGAIN -- A LOADS IT INTO THE COMPOSER",
                            recent.items[quick_sel], "A COMPOSE   Y FORGET   X BACK");
        return;
    }

    // COMMANDS: a saved command in full, since it may be a multi-line prompt.
    if (composer.mode == COMPOSER_CLOSED && quick_menu &&
        quick_level == SUBSCREEN_QUICK_COMMANDS && quick_sel >= 0) {
        const char *entry = "";
        if (quick_commands_entry(&commands, &saved_commands,
                                 (unsigned)quick_sel, &entry, NULL) ==
            QUICK_CMD_SAVED) {
            render_overlay_text(
                "QUICK COMMAND -- A LOADS IT INTO THE COMPOSER",
                entry,
                quick_remove_armed ? "A COMPOSE   Y SURE?   X BACK"
                                   : "A COMPOSE   Y FORGET   X BACK");
            return;
        }
    }

    // The two full pages own the top screen while they are selected.  Both are
    // drawn from a grid this client composed, with the terminal's own painter,
    // so they are made of the same glyphs as everything else and an extension
    // can stand on them -- see project.h.
    if (composer.mode == COMPOSER_CLOSED && page_mode(status_mode_get())) {
        render_overlay_grid(&page_grid);
        return;
    }

    if (composer.mode == COMPOSER_CLOSED) {
        render_overlay_close(&grid);
        return;
    }

    render_overlay_text(composer_adding_quick
                            ? "ADD QUICK COMMAND -- SAVED ON CARD, NOT SENT"
                            : "COMPOSE -- LOCAL, NOTHING SENT YET",
                        composer.body,
                        composer_focus_notice ?
                            "FOCUS: HOLD L FIRST, THEN PRESS R" :
                        focus_active() ?
                            (composer_adding_quick
                                 ? "FOCUS  A TYPE  B DEL  START SAVE  X OUT"
                                 : "FOCUS  A TYPE  B DEL  START SEND  X OUT") :
                        composer_adding_quick ?
                            "A SAVE COMMAND  B DEL  Y SNIPPETS  X CANCEL" :
                            "A SEND  B DEL  Y SNIPPETS  X CANCEL");
}

// Open or close the local overlays. The sub screen becomes the composer's
// keyboard for both EDIT and REVIEW: leaving it as the ordinary keyboard in
// REVIEW would put live PTY keys one touch away from a prompt the user is
// still reading.
static void overlay_sync(void)
{
    subscreen_set_composer(composer.mode != COMPOSER_CLOSED);
    subscreen_set_composer_add_quick(composer_adding_quick);
    subscreen_set_quick(quick_menu, quick_level, quick_sel, quick_top, &commands,
                        &recent);
    subscreen_set_quick_saved(&saved_commands, quick_remove_armed);
    subscreen_set_remote(remote_menu, &remote, &computers, &remote_ui,
                         boot_server_name());
    ext_overlay_sync();
    overlay_dirty = 1;
    refresh_sub();
}

// Send the composed body as consecutive KEY frames of at most 64 bytes with the
// carriage return as its own final frame (composer.c owns that plan).
//
// The CR being last and alone is the point: if the burst is interrupted, the
// far end is left holding an unsubmitted line the user can see and clear,
// rather than a truncated prompt that has already been submitted.
static void composer_send(unsigned long *tx)
{
    const size_t frames = composer_frame_count(&composer);
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *data = NULL;
        size_t n = 0;
        if (!composer_frame(&composer, i, &data, &n)) break;
        if (send_key_seq((const char *)data, n, tx)) continue;

        // The link died mid-burst and send_key_seq() has already reconnected.
        // Stop rather than delivering the rest of the body -- and the CR --
        // into whatever session came back: that would submit a truncated
        // prompt, which is the one outcome the trailing-CR plan exists to
        // prevent. The body is kept so it can be sent again deliberately.
        status("send interrupted; nothing submitted");
        overlay_sync();
        return;
    }
    // The SEND cue fired from the final CR frame inside send_key_seq().
    recent_remember(composer.body);
    composer_photo_id = 0;   // sent, not cancelled: the picture stays
    composer_close();
    overlay_sync();
}

static void dpad_send(unsigned dpad, unsigned long *tx)
{
    const char *seq = NULL;
    size_t len = 0;
    if (dpad & KEY_UP)       { seq = "\x1b[A"; len = 3; }
    else if (dpad & KEY_DOWN) { seq = "\x1b[B"; len = 3; }
    else if (dpad & KEY_RIGHT){ seq = "\x1b[C"; len = 3; }
    else if (dpad & KEY_LEFT) { seq = "\x1b[D"; len = 3; }
    if (seq && send_key_seq(seq, len, tx)) typed_bytes(seq, len);
}

// Press the touched key: kbd_press() runs local actions (SHFT cycles the layer,
// CTRL arms) or produces a key sequence to send. The caller repaints after, so
// a layer or Ctrl-state change shows in the footer.
static void kbd_press_key(int id, unsigned long *tx)
{
    if (id < 0) return;
    uint8_t bytes[PROTO_MAX_KEY_PAYLOAD];
    const int n = kbd_press(id, bytes, sizeof(bytes));
    if (n <= 0) return;
    if (send_key_seq((const char *)bytes, (size_t)n, tx)) {
        typed_bytes((const char *)bytes, (size_t)n);
    }
    // The key sounds when a byte was produced, not when it was sent: it is
    // feedback on the press, and a dead link already announces itself loudly.
    // Enter is skipped -- a submitted line owns SENT -- and ESC is a mode
    // action, not typing.
    const uint8_t b = bytes[0];
    if (b == '\r') return;
    if (b == 0x1b) {
        // ESC is a mode action, not typing: no key tick.
        ext_event(EXT_EVENT_ESCAPE);
        return;
    }
    if (b == ' ') ext_event(EXT_EVENT_SPACE);
    key_tick(b == 0x7f);
    ext_event(EXT_EVENT_KEY);
}

// Feed a keyboard press into the local buffer instead of into the socket.
static void composer_key(int id)
{
    uint8_t bytes[PROTO_MAX_KEY_PAYLOAD];
    const int n = kbd_press(id, bytes, sizeof(bytes));
    if (n <= 0) return;                       // SHFT/CTRL are local; they are done
    if (n > 1 && bytes[0] == 0x1b) return;    // escape sequences are not body text

    int typed = 0;
    int deleted = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t b = bytes[i];
        if (b == 0x7f) {
            composer_backspace(&composer);
            typed = 1;
            deleted = 1;                      // delete is a typing action too
        } else if (b == '\r') {
            composer_append(&composer, '\n');
            typed = 1;
        } else if (b == 0x1b) {
            continue;                         // ESC is not a body byte; X cancels
        } else {
            composer_append(&composer, b);
            typed = 1;
        }
    }
    // The composer's keys never reach the PTY, so send_key_seq() cannot voice
    // them; the cue belongs to the local editor instead.
    if (typed) {
        key_tick(deleted);
        ext_event(EXT_EVENT_KEY);
    }
}

// While the composer is open the only thing that sends is Send.
static int shoulder_chord(unsigned raw, unsigned edge)
{
    return ((raw & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) &&
           (edge & (KEY_L | KEY_R));
}

int host_shoulder_chord(unsigned raw, unsigned edge)
{
    return shoulder_chord(raw, edge);
}

// The snippets pane: the list, and the one way a body reaches the buffer.
//
// Y goes back to the buffer, which is also where a pick lands, so the pane is
// never somewhere you can be stuck. X still closes the whole overlay from
// either pane -- one cancel key, one meaning, wherever you are in it.
static void composer_snippets_input(unsigned raw, unsigned edge)
{
    if (shoulder_chord(raw, edge) && snippets.count > 0) {
        snippet_focus = !snippet_focus;
        if (snippet_focus) {
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        }
        overlay_sync();
        return;
    }
    if (snippet_focus && (edge & KEY_B)) {
        snippet_focus = 0;
        overlay_sync();
        return;
    }
    if (edge & KEY_Y) {
        composer_pane = COMPOSER_PANE_CLIP;
        snippet_focus = 0;
        overlay_sync();
        return;
    }
    if (edge & KEY_X) {
        composer_close();
        overlay_sync();
        return;
    }
    if (snippets.count == 0) return;

    if (edge & KEY_UP) {
        snippet_pick = (snippet_pick + snippets.count - 1) % snippets.count;
        if (snippet_focus) {
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        }
        overlay_sync();
    } else if (edge & KEY_DOWN) {
        snippet_pick = (snippet_pick + 1) % snippets.count;
        if (snippet_focus) {
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        }
        overlay_sync();
    } else if (edge & KEY_A) {
        // Straight into the buffer, already written, so the next press is
        // Send rather than an editor the user has to leave again. Picking is
        // not sending: the body is on the top screen first, and A commits it.
        // snippet_load_picked is the one choke point (docs/design/SNIPPETS_PUSH.md),
        // and the merge did not add a second one -- the pane switch moved,
        // the rule did not.
        if (snippet_load_picked(&snippets, snippet_pick, &composer)) {
            composer_pane = COMPOSER_PANE_WRITE;
            snippet_focus = 0;
            overlay_sync();
        } else {
            status("snippet does not fit the composer");
        }
    }
}

// The clipboard pane: read it, and load it if you want it.
//
// A is the only thing here that touches the buffer, and it loads rather than
// sends. Text that arrived from any machine syncing the desktop's clipboard is
// exactly what the deliberate-second-press rule exists for, and this pane does
// not get an exception to it.
static void composer_clip_input(unsigned edge)
{
    if (edge & KEY_Y) {
        composer_pane = COMPOSER_PANE_WRITE;
        overlay_sync();
        return;
    }
    if (edge & KEY_X) {
        composer_close();
        overlay_sync();
        return;
    }
    if (edge & KEY_A) {
        if (!clip_pastable(&clip)) {
            status("nothing on the clipboard to load");
            return;
        }
        if (composer_load(&composer, clip.text)) {
            composer_pane = COMPOSER_PANE_WRITE;
            overlay_sync();
            status("clipboard loaded -- A sends it");
        } else {
            // composer_load refuses rather than truncates, and so does this.
            // A paste cut short is a prompt that says something else.
            status("the clipboard does not fit the composer");
        }
    }
}

// Focus inside the composer: the d-pad walks the composer's keyboard and A
// types the lit key -- into the buffer, through composer_key(), the same path
// a tap takes, so nothing here can reach the PTY. A is taken, so START sends
// (or saves, adding a quick command); B deletes; X leaves focus, not the
// composer, so a half-written prompt is never lost to it. Returns 1 when it
// used the frame.
static unsigned composer_focus_held, composer_focus_frames;

static int composer_focus_input(unsigned raw, unsigned edge, unsigned long *tx)
{
    if (edge & KEY_X) { focus_off(); overlay_sync(); return 1; }
    if (edge & KEY_A) {
        composer_key(focus_key_id());
        overlay_sync();
        return 1;
    }
    if (edge & KEY_B) { composer_backspace(&composer); overlay_sync(); return 1; }
    if (edge & KEY_START) {
        if (composer.len == 0) return 1;
        if (composer_adding_quick) quick_saved_commit_from_composer();
        else                       composer_send(tx);
        overlay_sync();
        return 1;
    }
    const unsigned dp = raw & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (dp != composer_focus_held) {
        composer_focus_held = dp;
        composer_focus_frames = 0;
        if (dp) focus_step(dp, tx);
    } else if (dp) {
        composer_focus_frames++;
        if (kbd_repeat_should_fire(composer_focus_frames)) focus_step(dp, tx);
    }
    if (dp) { overlay_sync(); return 1; }
    // Y (snippets) and everything else fall through to the editor.
    return 0;
}

static void composer_input(unsigned raw, unsigned edge, unsigned long *tx)
{
    if (composer_pane == COMPOSER_PANE_SNIPPETS) {
        composer_snippets_input(raw, edge);
        return;
    }
    if (composer_pane == COMPOSER_PANE_CLIP) {
        composer_clip_input(edge);
        return;
    }

    const voice_ptt_action_t ptt = voice_ptt_step(
        &mic_ptt, ui_settings.mic_mode == SETTINGS_MIC_LATCH,
        (raw & KEY_R) != 0, (raw & KEY_L) != 0,
        (edge & KEY_R) != 0, (edge & KEY_L) != 0, (edge & KEY_X) != 0,
        mic_recording(), mic_waiting(), mic_full());
    if (ptt == VOICE_PTT_FOCUS) {
        // L first, then R: focus on the composer's own keyboard. It stays a
        // text editor with the same guarantee -- every key it types lands in
        // the buffer (composer_focus_input), nothing reaches the PTY until an
        // explicit send -- and the chord again leaves it.
        if (focus_active()) {
            focus_off();
        } else {
            focus_on(FOCUS_KEYS);
            composer_focus_held = 0;
            composer_focus_frames = 0;
        }
        composer_focus_notice = 0;
        overlay_sync();
        return;
    }
    if (mic_ptt.chord_notice) {
        // R first, then L, with nothing recording: not the focus chord. Say
        // which order is, rather than a chord that dies.
        if (ptt == VOICE_PTT_CANCEL) mic_cancel();
        composer_focus_notice = 1;
        overlay_sync();
        return;
    }
    switch (ptt) {
    case VOICE_PTT_START:
        // Capability first: a server that never proved speech-to-text
        // gets no VOICE_START, and the hold gets at most one line per
        // connection rather than one per press.
        if (mic_can_transcribe()) {
            (void)mic_start(lang_voice_code(ui_settings.language), tx);
        } else {
            mic_explain_unavailable();
        }
        break;
    case VOICE_PTT_LATCH:
        mic_set_latched(1);
        status("voice: recording -- tap R or L to stop");
        overlay_sync();
        return;
    case VOICE_PTT_STOP:
        // A shoulder let go: a recording the MIC bar is still holding is the
        // touch's to stop, not the shoulder's.
        if (!mic_touch_held) mic_stop(tx);
        overlay_sync();
        return;
    case VOICE_PTT_AUTO_STOP:
        // The 60 KB cap holds whoever started the recording, the bar
        // included.  mic_tick() normally stopped it already (mic.h, mic_full).
        mic_stop(tx);
        mic_touch_held = 0;
        overlay_sync();
        return;
    case VOICE_PTT_CANCEL:
        mic_cancel();
        mic_touch_held = 0;
        overlay_sync();
        return;
    case VOICE_PTT_NONE:
    default:
        break;
    }

    // Leave the explanation up until an actual editor action supersedes it;
    // clearing it on the next held-shoulder frame would make a one-frame
    // message the user can never read.
    if (edge & ~(KEY_L | KEY_R)) composer_focus_notice = 0;
    if (focus_active() && composer_focus_input(raw, edge, tx)) return;
    if (edge & KEY_X) {
        composer_close();
        overlay_sync();
        return;
    }
    // Y is the pane key.  It is free here: the composer swallows every key,
    // so Y's usual Tab never had a meaning inside this overlay.  It does not
    // discard the buffer -- you can go and fetch a snippet with a half-typed
    // prompt still in hand, and picking one replaces it only because that is
    // what loading a snippet has always done.
    if (edge & KEY_Y) {
        // The snippets pane never steps the push-to-talk helper, so a
        // recording left running behind it -- latched, or R still held --
        // could be stopped only by X, which closes the composer and the
        // draft with it. Finish it here: what was said is transcribed into
        // the composer as if R had been let go.
        if (mic_recording()) mic_stop(tx);
        voice_ptt_init(&mic_ptt);
        // The snippets pane has its own focus (L+R there); the keyboard's
        // would light a key nobody can press from that pane.
        if (focus_active()) focus_off();
        composer_pane = COMPOSER_PANE_SNIPPETS;
        snippet_pick = 0;
        snippet_focus = 0;
        overlay_sync();
        return;
    }

    // A commits, here as everywhere else. A newline inside the body comes from
    // the on-screen ENT key, so A never has to mean two things.
    //
    // An empty body closes instead of sending. The send plan is "the body, then
    // a lone CR", so an empty composer would send just the CR -- and a stray CR
    // is precisely what accepts a permission prompt. Opening this screen by
    // accident and pressing the commit key must not approve anything.
    if (edge & KEY_A) {
        if (composer.len == 0)      { composer_close(); overlay_sync(); }
        else if (composer_adding_quick) quick_saved_commit_from_composer();
        else                        composer_send(tx);
    }
    else if (edge & KEY_B) { composer_backspace(&composer); overlay_sync(); }
}

// ------------------------------------------------------------- REMOTE ---

// Both REMOTE messages ride on the proof rule SESSION_PICK keeps: nothing is
// sent until a MSG_REMOTE has arrived on this connection.
static int send_remote_frame(size_t n, const uint8_t *out, unsigned long *tx)
{
    if (send_all(out, n)) {
        if (tx) (*tx)++;
        return 1;
    }
    status("send failed; connection lost");
    reconnect();
    return 0;
}

static void send_remote_open(int open)
{
    if (!remote.ready) return;
    uint8_t out[PROTO_HEADER_SIZE + 1];
    (void)send_remote_frame(proto_encode_remote_open(out, open), out, NULL);
}

static void remote_rebuild_computers(void)
{
    boot_peers_list(&computers, cfg.server, cfg.port);
}

static void remote_settle(void)
{
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&remote, &computers, rows, REMOTE_ROWS_MAX);
    remote_ui_settle(&remote_ui, rows, n);
}

static void remote_open(void)
{
    if (focus_active()) focus_off();
    remote_menu = 1;
    remote_held = remote_frames = 0;
    remote_ui_init(&remote_ui);
    remote.note[0] = '\0';
    boot_peers_open();
    remote_rebuild_computers();
    remote_settle();
    send_remote_open(1);
    overlay_sync();
}

static void remote_close(void)
{
    remote_menu = 0;
    boot_peers_close();
    send_remote_open(0);
    overlay_sync();
}

// Move this card to another computer.  The old link is closed first -- one
// server at a time -- and if the new one cannot be reached the card goes
// straight back to the one it left, saying so, rather than stranding the
// person on a boot menu mid-session.
static void switch_server(const remote_computer_t *m)
{
    dsterm_config_t next = cfg;
    remote_close();
    if (!boot_switch_to(m, &next)) {
        status("stayed on %s", boot_server_name());
        page_repaint();
        overlay_sync();
        return;
    }
    const dsterm_config_t prev = cfg;
    connection_forget(1);
    ext_link(0);
    cfg = next;
    status("switching to %s (%s)", m->name, cfg.server);
    grid_clear(&grid);
    page_repaint();
    overlay_sync();
    if (net_connect() && handshake()) {
        boot_switch_done();
        ext_link(1);
        status("now on %s", m->name);
        status_link_host(m->name);
        chime_session_switch();
        chime_view_sent();
        link_refresh();
        refresh_sub();
        return;
    }
    status("could not reach %s; going back", m->name);
    sound_cue(SOUND_DENY);
    cfg = prev;
    reconnect();
}

static void remote_activate(unsigned long *tx)
{
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&remote, &computers, rows, REMOTE_ROWS_MAX);
    uint8_t index = 0;
    const remote_do_t what = remote_ui_activate(&remote_ui, &remote, &computers,
                                                rows, n, &index);
    uint8_t out[PROTO_HEADER_SIZE + 4];
    const uint16_t check = index < remote.count
                               ? remote_row_check(&remote.items[index]) : 0;
    switch (what) {
    case REMOTE_DO_OPEN:
        if (send_remote_frame(proto_encode_remote_pick(out, index,
                                                       REMOTE_PICK_OPEN, check),
                              out, tx)) {
            remote_set_note(&remote, "opening...", 10);
            chime_session_switch();
            remote_close();
        }
        break;
    case REMOTE_DO_COPY:
        if (send_remote_frame(proto_encode_remote_pick(out, index,
                                                       REMOTE_PICK_COPY, check),
                              out, tx)) {
            chime_session_switch();
            remote_close();
        }
        break;
    case REMOTE_DO_NEW:
        if (send_remote_frame(proto_encode_remote_pick(out, REMOTE_PICK_NEW_INDEX,
                                                       REMOTE_PICK_NEW, 0),
                              out, tx)) {
            chime_session_switch();
            remote_close();
        }
        break;
    case REMOTE_DO_ARM_COPY:
        // The explanation on the top screen changed to say what a second A
        // does; nothing else happens yet.
        overlay_sync();
        break;
    case REMOTE_DO_SWITCH:
        if (index < computers.count) {
            const remote_computer_t m = computers.items[index];
            switch_server(&m);
        }
        break;
    case REMOTE_DO_ALREADY:
        remote_set_note(&remote, "you are on this one", 19);
        overlay_sync();
        break;
    case REMOTE_DO_REFUSE:
        sound_cue(SOUND_DENY);
        remote_set_note(&remote, "that one can't be opened from here", 34);
        overlay_sync();
        break;
    case REMOTE_DO_NOTHING:
    default:
        break;
    }
}

static void remote_move(int delta)
{
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&remote, &computers, rows, REMOTE_ROWS_MAX);
    if (remote_ui_move(&remote_ui, rows, n, delta)) {
        ext_event(EXT_EVENT_FOCUS);
        chime_focus();
        overlay_sync();
    }
}

static void remote_input(unsigned raw, unsigned edge, unsigned long *tx)
{
    if (shoulder_chord(raw, edge) || (edge & (KEY_B | KEY_X))) {
        if (remote_ui.copy_armed && (edge & KEY_B)) {
            remote_ui.copy_armed = 0;      // B first cancels the armed copy
            overlay_sync();
            return;
        }
        remote_close();
        return;
    }
    if (edge & KEY_A) {
        remote_activate(tx);
        return;
    }
    const unsigned dpad = raw & (KEY_UP | KEY_DOWN);
    if (dpad && dpad != remote_held) {
        remote_held = dpad;
        remote_frames = 0;
        remote_move((dpad & KEY_UP) ? -1 : 1);
    } else if (dpad) {
        if (kbd_repeat_should_fire(++remote_frames)) {
            remote_move((dpad & KEY_UP) ? -1 : 1);
        }
    } else {
        remote_held = remote_frames = 0;
    }
}

// Once a frame while the picker is up: collect discovery answers, and
// redraw the computers half when what it would show has changed.
static void remote_tick(void)
{
    if (!remote_menu) return;
    boot_peers_poll();
    if (frame_count % 30 == 0) {
        remote_computers_t before = computers;
        remote_rebuild_computers();
        if (memcmp(&before, &computers, sizeof(computers)) != 0) {
            remote_settle();
            overlay_sync();
        }
    }
}

// ---------------------------------------------------------------- QUICK ---

static void recent_save(void)
{
    static char text[QUICK_RECENT_MAX * (2 * QUICK_RECENT_TEXT_MAX + 1) + 1];
    const size_t n = quick_recent_format(&recent, text, sizeof(text));
    FILE *fh = fopen(RECENT_PATH, "wb");
    if (!fh) return;
    if (n) (void)fwrite(text, 1, n, fh);
    fclose(fh);
}

static void recent_load(void)
{
    static char text[QUICK_RECENT_MAX * (2 * QUICK_RECENT_TEXT_MAX + 1) + 1];
    quick_recent_init(&recent);
    FILE *fh = fopen(RECENT_PATH, "rb");
    if (!fh) return;
    const size_t got = fread(text, 1, sizeof(text) - 1, fh);
    fclose(fh);
    text[got] = '\0';
    quick_recent_parse(&recent, text);
}

static void recent_remember(const char *text)
{
    if (quick_recent_add(&recent, text)) recent_save();
}

// One buffer for both directions: the formatted list is at most this, and
// the two never run at once.
static char quick_file[QUICK_SAVED_MAX * (2 * QUICK_SAVED_TEXT_MAX + 1) + 1];

// The new list goes to a file of its own first, and only once that is whole
// on the card does it replace the old one. Removing the old file first meant
// a full, read-only or pulled card lost every saved command at once. If the
// rename itself fails, the complete list is still in QUICK_TMP, and loading
// falls back to it.
static int quick_saved_save(void)
{
    const size_t n = quick_saved_format(&saved_commands, quick_file, sizeof(quick_file));
    FILE *fh = fopen(QUICK_TMP, "wb");
    if (!fh) return 0;
    const int wrote = (n == 0 || fwrite(quick_file, 1, n, fh) == n);
    if (fclose(fh) != 0 || !wrote) {
        (void)remove(QUICK_TMP);
        return 0;
    }
    (void)remove(QUICK_PATH);
    return rename(QUICK_TMP, QUICK_PATH) == 0;
}

static void quick_saved_load(void)
{
    quick_saved_init(&saved_commands);
    FILE *fh = fopen(QUICK_PATH, "rb");
    if (!fh) fh = fopen(QUICK_TMP, "rb");
    if (!fh) return;
    const size_t got = fread(quick_file, 1, sizeof(quick_file) - 1, fh);
    fclose(fh);
    quick_file[got] = '\0';
    quick_saved_parse(&saved_commands, quick_file);
}

static void quick_saved_commit_from_composer(void)
{
    // A save that fails keeps the composer and its text, and says why: the
    // words are the person's, and closing on a failure threw them away.
    unsigned at = 0;
    switch (quick_saved_add_at(&saved_commands, composer.body, &at)) {
    case QUICK_ADD_OK:
        break;
    case QUICK_ADD_EMPTY:
        status("quick: nothing to save -- type a command first");
        overlay_sync();
        return;
    case QUICK_ADD_TOO_LONG:
        status("quick: too long to save (%u characters max)", QUICK_SAVED_TEXT_MAX);
        overlay_sync();
        return;
    case QUICK_ADD_INVALID:
        status("quick: only plain characters can be saved");
        overlay_sync();
        return;
    case QUICK_ADD_FULL:
    default:
        status("quick: the list is full (%u max) -- Y on one forgets it",
               QUICK_SAVED_MAX);
        overlay_sync();
        return;
    }
    if (quick_saved_save()) status("quick: saved command");
    else status("quick: could not write the card -- saved until restart");
    composer_close();
    quick_menu = 1;
    quick_level = SUBSCREEN_QUICK_COMMANDS;
    quick_remove_armed = 0;
    quick_sel = (int)(quick_commands_builtin() + at);
    quick_top = 0;
    if (quick_sel >= SUBSCREEN_QUICK_LIST_ROWS) {
        quick_top = quick_sel - SUBSCREEN_QUICK_LIST_ROWS + 1;
    }
    overlay_sync();
}

// A byte stream typed live (KEYS, the face buttons, the d-pad): a line ended
// with Enter is kept for AGAIN.  Short ones -- a "y", a "2" -- are answers,
// not prompts worth sending again.
static void typed_bytes(const char *bytes, size_t n)
{
    char line[QUICK_RECENT_TEXT_MAX + 1];
    if (quick_line_feed(&typed_line, bytes, n, line, sizeof(line)) &&
        strlen(line) >= 4) {
        recent_remember(line);
    }
}

// Claude's mode, read off the bottom of its screen.  Only at the live edge:
// scrolled back, the bottom rows are history.
static void claude_mode_scan(void)
{
    unsigned back = 0;
    if (status_view_pos_get(&back, NULL) && back > 0) return;
    char found[QUICK_MODE_MAX + 1] = "";
    for (int row = PROTO_ROWS - 1; row >= PROTO_ROWS - 6 && !found[0]; row--) {
        char text[PROTO_COLS + 1];
        for (int col = 0; col < PROTO_COLS; col++) {
            const uint8_t g = grid.cells[row * PROTO_COLS + col].glyph;
            text[col] = (g >= 0x20 && g <= 0x7e) ? (char)g : ' ';
        }
        text[PROTO_COLS] = '\0';
        (void)quick_mode_from_line(text, found);
    }
    if (found[0] && strcmp(found, claude_mode) != 0) {
        snprintf(claude_mode, sizeof(claude_mode), "%s", found);
        subscreen_set_claude_mode(claude_mode);
        refresh_sub();
    }
}

static unsigned quick_count(void)
{
    if (quick_level == SUBSCREEN_QUICK_COMMANDS) {
        return quick_commands_total(&commands, &saved_commands);
    }
    if (quick_level == SUBSCREEN_QUICK_AGAIN) return recent.count;
    return quick_row_count(photo_available());
}

static void quick_show(int level)
{
    quick_level = level;
    quick_sel = 0;
    quick_top = 0;
    quick_remove_armed = 0;
    overlay_sync();
}

static void quick_close(void)
{
    quick_menu = 0;
    quick_held = 0;
    quick_frames = 0;
    quick_remove_armed = 0;
    quick_level = SUBSCREEN_QUICK_MAIN;
    overlay_sync();
}

static void quick_move(int delta)
{
    const int count = (int)quick_count();
    if (count <= 0) return;
    int next = quick_sel + delta;
    if (next < 0) next = 0;
    if (next >= count) next = count - 1;
    if (next == quick_sel) return;
    quick_sel = next;
    quick_remove_armed = 0;
    if (quick_sel < quick_top) quick_top = quick_sel;
    if (quick_sel >= quick_top + SUBSCREEN_QUICK_LIST_ROWS) {
        quick_top = quick_sel - SUBSCREEN_QUICK_LIST_ROWS + 1;
    }
    ext_event(EXT_EVENT_FOCUS);
    chime_focus();
    overlay_sync();
}

// Into the composer, read before it is sent: a command may be one this
// computer named, and a recent prompt is better looked at than fired.
static void quick_compose(const char *text)
{
    quick_close();
    composer_open_write();
    if (!composer_load(&composer, text)) status("quick: too long for the composer");
    overlay_sync();
}

static void quick_activate(unsigned long *tx)
{
    if (quick_level == SUBSCREEN_QUICK_COMMANDS) {
        quick_remove_armed = 0;
        const char *text = "";
        const quick_cmd_kind_t kind = quick_commands_entry(
            &commands, &saved_commands, (unsigned)quick_sel, &text, NULL);
        if (kind == QUICK_CMD_ADD) {
            quick_close();
            composer_open_write();
            composer_adding_quick = 1;
            overlay_sync();
        } else if (kind != QUICK_CMD_NONE) {
            quick_compose(text);
        }
        return;
    }
    if (quick_level == SUBSCREEN_QUICK_AGAIN) {
        if ((unsigned)quick_sel < recent.count) quick_compose(recent.items[quick_sel]);
        return;
    }
    switch ((quick_row_t)quick_sel) {
    case QUICK_ROW_MODE:
        // Shift-Tab, which is how Claude Code moves to its next mode.  The
        // list stays up so the next press is one A away; the tile and this
        // row follow the footer as it changes.
        if (send_key_seq("\x1b[Z", 3, tx)) chime_focus();
        break;
    case QUICK_ROW_UNDO:
        quick_close();
        if (send_key_seq("/rewind", 7, tx) && send_key_seq("\r", 1, tx)) {
            rewind_picking = 1;
            rewind_enters = 0;
            rewind_frames = 0;
            subscreen_set_rewind(1);
            if (status_mode_get() != SCREEN_MODE_CONTROL) {
                status_mode_set(SCREEN_MODE_CONTROL);
                page_repaint();
            }
            overlay_sync();
        }
        break;
    case QUICK_ROW_CHANGES:
        quick_close();
        project_tab_set(&project, PROJECT_TAB_DIFF);
        status_mode_set(SCREEN_MODE_PROJECT);
        project_resync();
        page_repaint();
        overlay_sync();
        break;
    case QUICK_ROW_COMMANDS:
        quick_show(SUBSCREEN_QUICK_COMMANDS);
        break;
    case QUICK_ROW_AGAIN:
        quick_show(SUBSCREEN_QUICK_AGAIN);
        break;
    case QUICK_ROW_VOICE:
        if (!mic_can_transcribe()) {
            status("voice: not available on this computer");
            break;
        }
        quick_close();
        composer_open_write();
        overlay_sync();
        status("voice: hold R (or tap the MIC bar) to speak");
        break;
    case QUICK_ROW_CONTROLS:
        // QUICK stays open underneath: leaving the guide comes back here.
        guide_show(GUIDE_CTX_CONTROL);
        break;
    case QUICK_ROW_PHOTO:
        // A camera on a computer that never sent READY is not a camera at
        // all; the row's note says why.
        if (!photo_proved()) {
            status("camera: not on this computer");
            break;
        }
        quick_close();
        photo_open();
        break;
    case QUICK_ROW_COUNT:
    default:
        break;
    }
}

static void quick_input(unsigned raw, unsigned edge, unsigned long *tx)
{
    // B or X: out one level, then out of QUICK.
    if (shoulder_chord(raw, edge) || (edge & (KEY_B | KEY_X))) {
        if (quick_remove_armed && (edge & KEY_B)) {
            quick_remove_armed = 0;
            overlay_sync();
            return;
        }
        if (quick_level != SUBSCREEN_QUICK_MAIN && !shoulder_chord(raw, edge)) {
            const int back_to = quick_level == SUBSCREEN_QUICK_COMMANDS
                                    ? QUICK_ROW_COMMANDS : QUICK_ROW_AGAIN;
            quick_show(SUBSCREEN_QUICK_MAIN);
            quick_sel = back_to;
            overlay_sync();
        } else {
            quick_close();
        }
        return;
    }
    if (edge & KEY_A) {
        quick_activate(tx);
        return;
    }
    if ((edge & KEY_Y) && quick_level == SUBSCREEN_QUICK_COMMANDS) {
        unsigned saved_idx = 0;
        if (quick_commands_entry(&commands, &saved_commands,
                                 (unsigned)quick_sel, NULL, &saved_idx) ==
            QUICK_CMD_SAVED) {
            if (!quick_remove_armed) {
                quick_remove_armed = 1;
                overlay_sync();
            } else if (quick_saved_remove(&saved_commands, saved_idx)) {
                if (!quick_saved_save()) {
                    status("quick: could not write the card -- gone until restart");
                }
                quick_remove_armed = 0;
                const int total = (int)quick_count();
                if (quick_sel >= total && quick_sel > 0) quick_sel--;
                if (quick_top > quick_sel) quick_top = quick_sel;
                overlay_sync();
            }
        }
        return;
    }
    if ((edge & KEY_Y) && quick_level == SUBSCREEN_QUICK_AGAIN) {
        if (quick_recent_remove(&recent, (unsigned)quick_sel)) {
            recent_save();
            if (quick_sel >= (int)recent.count && quick_sel > 0) quick_sel--;
            if (quick_top > quick_sel) quick_top = quick_sel;
            overlay_sync();
        }
        return;
    }

    const unsigned dpad = raw & (KEY_UP | KEY_DOWN);
    if (dpad && dpad != quick_held) {
        quick_held = dpad;
        quick_frames = 0;
        quick_move((dpad & KEY_UP) ? -1 : 1);
    } else if (dpad && dpad == quick_held) {
        quick_frames++;
        if (kbd_repeat_should_fire(quick_frames)) quick_move((dpad & KEY_UP) ? -1 : 1);
    } else {
        quick_held = 0;
        quick_frames = 0;
    }
}

// `dsterm view`: the bottom screen, as text, while a view is watching --
// sent when it changed, no more than four times a second, and every two
// seconds regardless so a view that joined late is never left blank.
static void mirror_tick(unsigned long *tx)
{
    if (!mirror_watching || sock < 0) return;
    // A picture's SENDING strip changes every few frames; the link is the
    // picture's then, and a view keeps up at once a second.
    const unsigned long gap = photo_uploading() ? MIRROR_UPLOAD_FRAMES
                                                : MIRROR_MIN_FRAMES;
    if (frame_count - mirror_sent_at < gap) return;
    const mirror_t *m = subscreen_mirror();
    const uint32_t h = mirror_hash(m);
    if (h == mirror_sent_hash &&
        frame_count - mirror_sent_at < MIRROR_REFRESH_FRAMES) {
        return;
    }
    static uint8_t body[MIRROR_PAYLOAD_SIZE];
    static uint8_t out[PROTO_HEADER_SIZE + MIRROR_PAYLOAD_SIZE];
    const size_t len = mirror_encode(m, ui_settings.view_taps ? MIRROR_FRAME_TAPS : 0,
                                     body, sizeof(body));
    if (!len) return;
    const size_t n = proto_encode(out, MSG_SUBSCREEN, body, (uint16_t)len);
    mirror_sent_hash = h;
    mirror_sent_at = frame_count;
    if (!send_all(out, n)) {
        status("send failed; connection lost");
        reconnect();
        return;
    }
    (*tx)++;
}

// Where the screen goes after closing.
static void photo_leave(photo_exit_t where)
{
    photo_close(1);
    if (where == PHOTO_EXIT_CONTROL) {
        status_mode_set(SCREEN_MODE_CONTROL);
        page_repaint();
        overlay_sync();
        return;
    }
    if (where == PHOTO_EXIT_NEXT) {
        status_mode_next();
        page_repaint();
        overlay_sync();
        return;
    }
    // Back to the row it was opened from.
    quick_menu = 1;
    quick_level = SUBSCREEN_QUICK_MAIN;
    quick_sel = QUICK_ROW_PHOTO;
    quick_top = 0;
    overlay_sync();
}

// The saved picture's composer owes its id an answer.  Cleared before the
// send: connection_forget must never find a live id twice, and the send's
// own reconnect path passes through it.
static void photo_discard_pending(void)
{
    const int pid = composer_photo_id;
    composer_photo_id = 0;
    if (pid && photo_proved() && sock >= 0) {
        uint8_t out[PROTO_HEADER_SIZE + 2];
        const size_t n = proto_encode_photo_discard(out, (uint16_t)pid);
        if (!send_all(out, n)) {
            status("send failed; connection lost");
            reconnect();
        }
    }
}

// A PHOTO_SAVED the SAVING state was waiting for: the only caller that opens
// the composer on a camera path.  OK closes the camera and composes the
// caption; anything else is FAILED with the computer's reason.
static void camera_saved(const photo_saved_t *saved)
{
    if (!photo_saved_ok(saved)) return;
    composer_photo_id = saved->id;
    photo_close(1);
    composer_open_write();
    char body[COMPOSER_MAX + 1];
    snprintf(body, sizeof(body), "Picture from the DS camera: %s ",
             saved->path);
    if (!composer_load(&composer, body)) {
        // A path at the wire's limit can exceed the composer: the prompt the
        // person sees must still open rather than swallow the answer.
        composer_load(&composer, "Picture from the DS camera. ");
    }
    overlay_sync();
}

static void rewind_done(void)
{
    if (!rewind_picking) return;
    rewind_picking = 0;
    subscreen_set_rewind(0);
    refresh_sub();
}

// ------------------------------------------------------ your own sounds ---

// Custom sounds, from the card: /dsterm/sounds/<cue>.wav, and <cue>-2.wav and
// <cue>-3.wav for variations (docs/CUSTOMIZING.md lists the cue names).  A
// file that is there replaces the built-in sound for that cue; a file that
// is not costs nothing.  Loaded once, at boot, into memory that is never
// freed -- the conductor borrows it for as long as the card is on.
//
// The budget is generous for a few sounds and small for the console: a
// megabyte is a quarter of a DS Lite's RAM, and nothing else here comes
// close to using the rest.
#define CUSTOM_SOUND_BUDGET (1024u * 1024u)
#define CUSTOM_SOUND_FILE_MAX (600u * 1024u)
static sfx_sample_t custom_takes[SFX_BANK_COUNT][SOUND_CUSTOM_TAKES];
static unsigned custom_bytes;
static unsigned custom_files;

static int read_whole_file(const char *path, uint8_t **out, size_t *len)
{
    FILE *fh = fopen(path, "rb");
    if (!fh) return 0;
    if (fseek(fh, 0, SEEK_END) != 0) { fclose(fh); return 0; }
    const long size = ftell(fh);
    if (size <= 0 || (unsigned long)size > CUSTOM_SOUND_FILE_MAX) {
        fclose(fh);
        status("sounds: %s is too big (max 600 KB)", path);
        return 0;
    }
    rewind(fh);
    uint8_t *buf = malloc((size_t)size);
    if (!buf) { fclose(fh); return 0; }
    const size_t got = fread(buf, 1, (size_t)size, fh);
    fclose(fh);
    if (got != (size_t)size) { free(buf); return 0; }
    *out = buf;
    *len = got;
    return 1;
}

static void load_custom_sounds(void)
{
    for (unsigned b = 0; b < SFX_BANK_COUNT; b++) {
        unsigned takes = 0;
        for (unsigned k = 1; k <= SOUND_CUSTOM_TAKES; k++) {
            char path[64];
            if (k == 1) snprintf(path, sizeof(path), "%s/sounds/%s.wav",
                                 PROFILES_ROOT, SFX_BANK_NAMES[b]);
            else        snprintf(path, sizeof(path), "%s/sounds/%s-%u.wav",
                                 PROFILES_ROOT, SFX_BANK_NAMES[b], k);
            uint8_t *file = NULL;
            size_t len = 0;
            if (!read_whole_file(path, &file, &len)) continue;
            char why[48] = "";
            uint32_t hz = 0;
            const size_t samples = wav_measure(file, len, &hz, why, sizeof(why));
            const size_t bytes = samples * sizeof(int16_t);
            int16_t *pcm = NULL;
            if (samples && custom_bytes + bytes <= CUSTOM_SOUND_BUDGET) {
                pcm = malloc(bytes);
            } else if (samples) {
                snprintf(why, sizeof(why), "over the 1 MB sound budget");
            }
            wav_t wav = { pcm, 0, 0 };
            if (pcm && wav_decode(file, len, &wav, samples, why, sizeof(why))) {
                // The ARM7 reads samples straight out of main RAM, past the
                // ARM9's data cache: what was just written must be flushed
                // there before the hardware is pointed at it.
                DC_FlushRange(pcm, bytes);
                sfx_sample_t *t = &custom_takes[b][takes++];
                t->data = (const uint8_t *)pcm;
                t->bytes = (uint32_t)bytes;
                t->hz = (uint16_t)wav.hz;
                t->frames = (uint16_t)((wav.samples * 60u + wav.hz - 1u) / wav.hz);
                t->gain = sound_bank_gain(b);
                t->format = SFX_FORMAT_PCM16;
                custom_bytes += (unsigned)bytes;
                custom_files++;
            } else {
                free(pcm);
                status("sounds: %s skipped: %s", path, why[0] ? why : "unreadable");
            }
            free(file);
        }
        if (takes) (void)sound_set_custom(b, custom_takes[b], takes);
    }
    if (custom_files) {
        status("sounds: %u of your own, for %u cues", custom_files,
               sound_custom_banks());
    }
}

static void load_custom_langs(void)
{
    static const char *const ROOTS[] = {
        "/dsterm/lang",
        "sd:/dsterm/lang",
        "fat:/dsterm/lang",
    };
    unsigned loaded = 0;
    for (unsigned r = 0; r < sizeof(ROOTS) / sizeof(ROOTS[0]); r++) {
        DIR *dir = opendir(ROOTS[r]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && lang_count() < LANG_MAX) {
            const char *name = ent->d_name;
            const size_t n = strlen(name);
            if (n <= 5 || strcmp(name + n - 5, ".lang") != 0) continue;
            char code[LANG_CODE_MAX + 1];
            const size_t clen = n - 5 < LANG_CODE_MAX ? n - 5 : LANG_CODE_MAX;
            memcpy(code, name, clen);
            code[clen] = '\0';
            char path[128], err[64] = "";
            snprintf(path, sizeof(path), "%s/%s", ROOTS[r], name);
            if (lang_load_file(path, code, err, sizeof(err)) >= 0) {
                loaded++;
            } else {
                status("lang: %s skipped (%s)", name, err[0] ? err : "invalid");
            }
        }
        closedir(dir);
        if (loaded) break;
    }
    if (loaded) {
        status("lang: %u custom language pack(s) loaded", loaded);
    }
}

// The facts the ABOUT block reports.  Composed here because this is where they
// are known, and only placed by settings.c -- the same split project.c uses,
// and for the same reason: two places formatting one value is how they drift.
static void settings_about_compose(settings_about_t *about)
{
    settings_about_init(about);
    // What this ROM *is*, not which commit built it.  A git id compiled in
    // here would go stale the moment an incremental build reused an old
    // main.o, and a build id that is quietly wrong is worse than none -- which
    // commit is on the card belongs in a flash log, against a
    // checksum, where it can be checked rather than believed.
    (void)snprintf(about->build, sizeof(about->build),
                   "%.20s  %dx%d  v%d", boot_console_name(), PROTO_COLS,
                   PROTO_ROWS, PROTO_VERSION);
    // Whatever the link line already says, said again here rather than
    // recomputed: one formatter for the connection, used twice.  It comes back
    // padded to a full row, so the padding is trimmed rather than clipped --
    // this field is narrower than a row and a value ending in eight spaces
    // would look like a value that had been cut off.
    char link[LOG_COLS + 1];
    status_format_link(link);
    size_t link_len = strlen(link);
    while (link_len && link[link_len - 1] == ' ') link[--link_len] = '\0';
    (void)snprintf(about->link, sizeof(about->link), "%.*s",
                   (int)SETTINGS_ABOUT_MAX, link);

    const session_t *selected = sessions_selected(&sessions);
    if (selected && selected->name[0]) {
        (void)snprintf(about->session, sizeof(about->session), "%s",
                       selected->name);
    }
    // The extension's line, if it has one; the page skips the row otherwise.
    (void)snprintf(about->ext_label, sizeof(about->ext_label), "%s",
                   ext_about_label() ? ext_about_label() : "");
    (void)snprintf(about->ext_value, sizeof(about->ext_value), "%s",
                   ext_about_value() ? ext_about_value() : "");
    // Whether the file was actually written, not whether we asked it to be.
    // That distinction cost four builds of silently unsaved preferences once
    // already; saying it on the page is cheap insurance against the next one.
    (void)snprintf(about->card, sizeof(about->card), "%s (%s)",
                   settings_path,
                   settings_written ? "saved" : "unsaved");
}

// The sound rows, carried to the conductor.  OFF is a mute rather than a
// third instrument, so the chosen instrument survives being turned back on.
static void sound_settings_apply(void)
{
    static const sound_volume_t LEVELS[SETTINGS_VOLUME_COUNT] = {
        SOUND_VOLUME_LOW, SOUND_VOLUME_NEAR, SOUND_VOLUME_ROOM,
    };
    if (ui_settings.sound == SETTINGS_SOUND_OFF) {
        sound_set_muted(1);
    } else {
        (void)sound_set_style(ui_settings.sound == SETTINGS_SOUND_CHIP
                                  ? SOUND_STYLE_CHIP : SOUND_STYLE_SNES);
        sound_set_muted(0);
    }
    (void)sound_set_volume(LEVELS[ui_settings.volume]);
    sound_set_blips(ui_settings.blips);
}

// Push the settings at the things that own them, and repaint both screens.
// The module decides what a press means; this decides nothing and only carries
// the answer.
static void settings_apply(void)
{
    boot_set_autoconnect(ui_settings.autoconnect);
    if (lang_current() != ui_settings.language) {
        lang_set_current(ui_settings.language);
        page_repaint();
    }
    // Every line the card draws goes through the mask, so the change shows
    // everywhere at once: repaint both screens.
    if (privacy_hiding() != ui_settings.hide_addresses) {
        privacy_set_hide(ui_settings.hide_addresses);
        page_repaint();
    }
    // Every hint that names a button names the one that does it now.
    {
        const button_t yes = button_for(&ui_settings.buttons, BUTTON_ENTER);
        const button_t no = button_for(&ui_settings.buttons, BUTTON_ESC);
        chime_set_answer_buttons(button_name(yes), button_name(no));
        char legend[128];
        button_map_legend(&ui_settings.buttons, legend, sizeof(legend));
        const button_t stop = button_for(&ui_settings.buttons, BUTTON_STOP);
        const button_t tab = button_for(&ui_settings.buttons, BUTTON_TAB);
        subscreen_set_button_legend(legend, button_name(yes), button_name(stop),
                                    button_name(tab));
    }
    sound_set_typing(ui_settings.typing);
    if (render_set_accent(settings_theme_rgb(ui_settings.theme))) {
        // The accent is in the terminal's palette too (Claude's own orange):
        // repaint it, not just the page.
        page_repaint();
    }
    subscreen_set_settings(&ui_settings, &ui_settings_ui);
    settings_about_compose(&ui_settings_about);
    // Same rule as project_resync(): the two pages share one grid, so only the
    // one currently on screen may paint into it.
    if (status_mode_get() == SCREEN_MODE_SETTINGS) {
        settings_render(&ui_settings, &ui_settings_ui, &ui_settings_about,
                        &page_grid);
    }
}

static void settings_save_current(void)
{
    char err[80] = "";
    if (!settings_save(settings_path, &ui_settings, err, sizeof(err))) {
        settings_written = 0;
        status("settings: temporary (%s)", err[0] ? err : "save failed");
    } else {
        settings_written = 1;
    }
}

// A chip was pressed.
//
// Two rows here type into the live session with no composer step, which is a
// deliberate exception to a rule this program otherwise keeps strictly.  The
// argument is in settings.h and worth repeating in one line: the composer gate
// exists so *server-supplied* text is read before it runs, and every string
// this page can type is a literal compiled into the ROM, unreachable from the
// wire, and printed on the top screen beside the chip that types it.  There is
// nobody here to be protected from, and the gate's only effect would be to
// reproduce the defect being fixed -- a switch that does not switch anything.
//
// The destructive row gets a confirm instead, which is the protection that
// actually fits it.
static void settings_chip_press(settings_row_t row, unsigned option,
                                unsigned long *tx)
{
    // A tap from the computer may change any setting but this one: GOD MODE
    // turns a safety rule off, and that is for the person holding the DS.
    if (inject_frames > 0 && row == SETTINGS_ROW_GOD_MODE && option != 0) {
        status("GOD MODE can only be armed on the DS itself");
        return;
    }
    const char *send = NULL;
    const settings_press_t result =
        settings_press(&ui_settings, &ui_settings_ui, row, option, &send);

    switch (result) {
    case SETTINGS_PRESS_SENDS:
        if (send && send[0]) {
            const size_t len = strlen(send);
            if (send_key_seq(send, len, tx) && send_key_seq("\r", 1, tx)) {
                status("settings: sent %s", send);
            }
        }
        // A model pick is also a stored value: what was last asked for.
        if (row == SETTINGS_ROW_MODEL) settings_save_current();
        break;
    case SETTINGS_PRESS_CHANGED:
        settings_save_current();
        // A sound setting answers with a sound, so the choice can be heard
        // rather than read: the instrument or level plays its "settled"
        // chime, and turning blips on plays one blip.
        if (row == SETTINGS_ROW_SOUND || row == SETTINGS_ROW_VOLUME ||
            row == SETTINGS_ROW_BLIPS) {
            sound_settings_apply();
            if (row == SETTINGS_ROW_BLIPS) {
                if (ui_settings.blips) (void)sound_blip('a');
            } else {
                sound_preview();
            }
        }
        break;
    case SETTINGS_PRESS_RESET:
        settings_save_current();
        sound_settings_apply();
        status("settings: back to defaults");
        break;
    case SETTINGS_PRESS_GUIDE:
        // Settings stays underneath, on this row, for when the guide closes.
        guide_show(GUIDE_CTX_CONTROL);
        break;
    case SETTINGS_PRESS_NOTHING:
    default:
        break;
    }

    settings_apply();
    overlay_sync();
}

// Press one of the DO tab's buttons.
//
// Two paths, and the difference between them is the whole safety argument for
// the page.  Ordinarily the command lands in the composer, on screen, where a
// second deliberate press sends it -- identical to how a pushed snippet
// behaves, and for the identical reason: this link has no per-message
// integrity, so anyone who can inject can plant an action, and the only thing
// between a planted action and a keystroke is somebody reading it.
//
// God mode is that gate switched off, deliberately, by the person holding the
// device, from a page that says in red that it is off.  The server cannot
// assert it and no wire value reaches this decision.
static void project_action_press(unsigned index, unsigned long *tx)
{
    project_action_t actions[PROJECT_ACTIONS_MAX];
    const unsigned n = project_actions(&project, actions, PROJECT_ACTIONS_MAX);
    if (index >= n) return;
    project.action_sel = (uint8_t)index;

    const char *body = actions[index].body;
    if (!body[0]) return;
    // The decoder keeps 0x80..0xBF in a row so the page can draw its glyphs,
    // which makes it display text, not keys: on the PTY those bytes are C1
    // controls.  Re-checked here, on the one path from a row to the wire,
    // and refused whole -- an action with a byte missing is a different
    // command.  The label is not printed: it may carry the same bytes.
    if (!project_action_keyable(body)) {
        status("do: refused an action with characters the DS cannot type");
        return;
    }
    if (!ui_settings.god_mode) {
        if (!composer_load(&composer, body)) {
            status("do: that action does not fit the composer");
            return;
        }
        overlay_sync();
        status("do: %s -- A sends it", actions[index].label);
        return;
    }
    // Checked to printable ASCII just above, and the composer's own body
    // limit is far larger than a row can carry, so the send below cannot be
    // made to overrun or to carry a control byte by anything on the wire.
    const size_t len = strlen(body);
    if (!send_key_seq(body, len, tx)) return;
    if (!send_key_seq("\r", 1, tx)) return;
    status("do: ran %s", actions[index].label);
}

// Rebuild the clipboard's lower-screen presentation.
//
// Same split as everywhere else on this device: clip.c owns the wording and
// the wrapping because both are host-tested, this composes nothing and only
// carries the answer, and subscreen.c decides where it goes.
static void clip_resync(void)
{
    char summary[CLIP_LINE_COLS + 1];
    static char lines[SUBSCREEN_CLIP_LINES_MAX][CLIP_LINE_COLS + 1];
    clip_summary(&clip, summary, sizeof(summary));
    const unsigned n = clip_wrap(&clip, lines, SUBSCREEN_CLIP_LINES_MAX);
    subscreen_set_clip(summary, lines, n, clip_pastable(&clip));
}

// Rebuild the PROJECT page and push its lower half.
//
// Everything here is a fact this client already holds -- the link, the device,
// the counters, the session list the server advertised.  Nothing is inferred
// from the shape of the terminal, and the tabs that need facts only the server
// can state say so plainly rather than guessing or showing an empty page.
// Those become real when the server learns to send them.
static void project_resync(void)
{
    char v[PROJECT_VALUE_MAX + 1];
    const unsigned uptime_s = (unsigned)(frame_count / 60u);

    // Only the tabs the server has not claimed.  Once it starts reporting a
    // branch, overwriting that every second with a link status would be a
    // fight between two truths, and the server's is the one you came here for.
    if (!project_tab_from_server(&project, PROJECT_TAB_NOW)) {
    project_clear(&project, PROJECT_TAB_NOW);
    project_add(&project, PROJECT_TAB_NOW, "link",
                link_connected ? "connected" : "offline",
                link_connected ? 3 /* green */ : 2 /* red */);
    if (rtt_known) {
        (void)snprintf(v, sizeof(v), "%u ms", rtt_ms);
        project_add(&project, PROJECT_TAB_NOW, "round trip", v, 0);
    }
    for (int i = 0; i < sessions.count; i++) {
        if (!(sessions.items[i].flags & SESSION_FLAG_ACTIVE)) continue;
        project_add(&project, PROJECT_TAB_NOW, "session",
                    sessions.items[i].name, 9 /* orange */);
        break;
    }
    project_add(&project, PROJECT_TAB_NOW, "doing",
                chime_posture_text(), 0);
    if (ext_now_label()) {
        project_add(&project, PROJECT_TAB_NOW, ext_now_label(),
                    ext_now_value() ? ext_now_value() : "", 0);
    }
    }

    if (!project_tab_from_server(&project, PROJECT_TAB_HEALTH)) {
        project_clear(&project, PROJECT_TAB_HEALTH);
        (void)snprintf(v, sizeof(v), "%u%s", health_battery,
                       health_external ? " (charging)" : "");
        project_add(&project, PROJECT_TAB_HEALTH, "battery", v, 0);
        if (health_rssi_known) {
            (void)snprintf(v, sizeof(v), "%d", health_rssi);
            project_add(&project, PROJECT_TAB_HEALTH, "wifi", v, 0);
        }
        (void)snprintf(v, sizeof(v), "%u ms%s", rtt_ms, rtt_known ? "" : " (stale)");
        project_add(&project, PROJECT_TAB_HEALTH, "round trip", v, 0);
        (void)snprintf(v, sizeof(v), "%lu", last_loops);
        project_add(&project, PROJECT_TAB_HEALTH, "frames", v, 0);
        (void)snprintf(v, sizeof(v), "%um %02us", uptime_s / 60u, uptime_s % 60u);
        project_add(&project, PROJECT_TAB_HEALTH, "up", v, 0);
    }

    if (!project_tab_from_server(&project, PROJECT_TAB_LOG)) {
        project_clear(&project, PROJECT_TAB_LOG);
        (void)snprintf(v, sizeof(v), "%lu", last_rx);
        project_add(&project, PROJECT_TAB_LOG, "bytes in", v, 0);
        (void)snprintf(v, sizeof(v), "%lu", last_tx);
        project_add(&project, PROJECT_TAB_LOG, "bytes out", v, 0);
        (void)snprintf(v, sizeof(v), "%lu", last_msg);
        project_add(&project, PROJECT_TAB_LOG, "messages", v, 0);
        (void)snprintf(v, sizeof(v), "%um %02us", uptime_s / 60u, uptime_s % 60u);
        project_add(&project, PROJECT_TAB_LOG, "connected", v, 0);
    }

    // The three the server owns.  Saying which side is missing is the useful
    // half of an empty page: "not reported" is a fact about the server, and
    // somebody reading it knows to go and look at the server.
    static const char *const UNREPORTED = "this server does not report it";
    static const project_tab_t SERVER_TABS[] = {
        PROJECT_TAB_DIFF, PROJECT_TAB_DO, PROJECT_TAB_WATCH,
    };
    static const char *const SERVER_LABELS[] = { "repo", "actions", "pinned" };
    for (unsigned i = 0; i < sizeof(SERVER_TABS) / sizeof(SERVER_TABS[0]); i++) {
        if (project_tab_from_server(&project, SERVER_TABS[i])) continue;
        project_clear(&project, SERVER_TABS[i]);
        project_add(&project, SERVER_TABS[i], SERVER_LABELS[i], UNREPORTED, 0);
    }

    // Only when the page is the one on screen.  PROJECT and SETTINGS compose
    // into the same grid -- there is only ever one top-screen page -- so an
    // unconditional render here would paint the project over the settings
    // every second while somebody was reading the settings.
    if (status_mode_get() == SCREEN_MODE_PROJECT) {
        project_render(&project, &page_grid);
    }

    char lines[6][44];
    unsigned line_count = 2;
    (void)snprintf(lines[0], sizeof(lines[0]), "%s", project_tab_name(
        project_tab(&project)));
    (void)snprintf(lines[1], sizeof(lines[1]), "%s", tr(TR_PROJECT_HINT));
    if (project_tab(&project) == PROJECT_TAB_HEALTH &&
        project_tab_from_server(&project, PROJECT_TAB_HEALTH)) {
        lines[2][0] = '\0';
        (void)snprintf(lines[3], sizeof(lines[3]),
                       "DS bat %u%s   wifi %d   rtt %u ms",
                       health_battery, health_external ? "+" : "",
                       health_rssi_known ? health_rssi : 0, rtt_ms);
        (void)snprintf(lines[4], sizeof(lines[4]),
                       "DS uptime %um %02us   frames %lu",
                       uptime_s / 60u, uptime_s % 60u, last_loops);
        line_count = 5;
    } else if (project_tab(&project) == PROJECT_TAB_LOG &&
               project_tab_from_server(&project, PROJECT_TAB_LOG)) {
        lines[2][0] = '\0';
        (void)snprintf(lines[3], sizeof(lines[3]),
                       "DS in %lu B   out %lu B", last_rx, last_tx);
        (void)snprintf(lines[4], sizeof(lines[4]),
                       "DS msgs %lu   up %um %02us",
                       last_msg, uptime_s / 60u, uptime_s % 60u);
        line_count = 5;
    }
    subscreen_set_project((unsigned)project_tab(&project), lines, line_count);
    project_action_t actions[PROJECT_ACTIONS_MAX];
    const unsigned action_count = project_actions(&project, actions,
                                                  PROJECT_ACTIONS_MAX);
    subscreen_set_project_actions(actions, action_count,
                                  project_action_selected(&project),
                                  project_actions_dropped(&project),
                                  ui_settings.god_mode);
    // The page an extension may be drawing on has just been rewritten.  Same
    // epoch bump a terminal frame gets, for the same reason: a line that
    // appeared under its drawing has to be noticed before the next frame.
    ext_terminal_changed();
}

// Repaint whichever page owns the top screen, after a change of mode.
//
// PROJECT and SETTINGS share one `page_grid`, because there is only ever one
// top-screen page.  Each of their own rebuild paths refuses to paint unless it
// is the one showing, which keeps them from fighting -- but it also means that
// arriving at a page leaves the *other* page's glyphs there until the arriving
// one next rebuilds.  For PROJECT that is up to a second of the settings page
// on screen.  So every mode transition comes through here.
static void page_repaint(void)
{
    switch (status_mode_get()) {
    case SCREEN_MODE_PROJECT:
        project_render(&project, &page_grid);
        break;
    case SCREEN_MODE_SETTINGS:
        settings_about_compose(&ui_settings_about);
        settings_render(&ui_settings, &ui_settings_ui, &ui_settings_about,
                        &page_grid);
        break;
    case SCREEN_MODE_CONTROL:
    case SCREEN_MODE_KEYS:
    case SCREEN_MODE_STATUS:
    case SCREEN_MODE_COUNT:
    default:
        // Nothing to paint: the terminal has the screen back, and
        // render_overlay_close() gives it back from the grid we already hold.
        break;
    }
}

// A touch control on the Control screen.
//
// The approval pair is the reason this screen exists, so the bytes are worth
// being explicit about: A sends CR and B sends ESC. Not "y\r" and "n\r" --
// Claude Code's permission prompt is a numbered select list, so the literal n
// is discarded and the CR behind it accepts whatever option is highlighted,
// which makes the reject button approve. The rule that this client sends
// generic key bytes and never reads the terminal's text is right, and is
// exactly why the bytes have to be the ones a keyboard would send.
// Straight back to the newest output, in one request -- what the LIVE tile
// did, now held down on the d-pad (poll_input).
static void jump_live(unsigned long *tx)
{
    // The application owns the transcript, so the server has no history and
    // VIEW can never move. Paging down is how the app scrolls toward the
    // bottom of its own transcript -- the closest thing to "go live" the
    // app defines, sent as the same generic key bytes a keyboard sends.
    if (!app_scrolls_known) {
        status("waiting for selected session mode");
        refresh_sub();
        return;
    }
    if (app_scrolls) {
        send_key_seq(SEQ_PAGE_DOWN, SEQ_PAGE_LEN, tx);
        return;
    }
    // Back to live in one request rather than a dozen steps. The delta
    // ceiling matches the session history, so this reaches live from
    // anywhere inside it; the server clamps anything beyond, and clamping
    // here as well is not decoration -- an unclamped value above 32767
    // would encode as a negative int16 and scroll further back instead.
    unsigned back = 0;
    if (status_view_pos_get(&back, NULL) && back > 0) {
        send_view_delta(back > PROTO_VIEW_MAX_DELTA
                            ? (int16_t)PROTO_VIEW_MAX_DELTA
                            : (int16_t)back,
                        tx);
    }
}

static void control_press(kbd_control_t hit, unsigned long *tx)
{
    chime_user_activity();
    ext_event(EXT_EVENT_LOCAL_INPUT);
    switch (hit) {
    case KBD_CONTROL_KEYS:
        // The keyboard types live: each touch is one KEY message to the PTY,
        // and the top screen stays the terminal -- no overlay, nothing
        // buffered locally. The composer is one tile away for prompts worth
        // editing before they reach the PTY.
        status_mode_set(SCREEN_MODE_KEYS);
        // Focus follows the screen it landed on: the highlight becomes the
        // keyboard's own cursor, home at 'a'.
        if (focus_active()) focus_on(FOCUS_KEYS);
        // The tile is reachable from a page, and leaving a page has to give
        // the top screen back rather than only redrawing the bottom one.
        overlay_sync();
        break;

    case KBD_CONTROL_COMPOSE:
        // Overlays own every input while open, so focus yields to them.
        if (focus_active()) focus_off();
        // Always the buffer first.  The snippets pane is one Y away, and a
        // tile that opened onto a list some of the time would be a tile whose
        // behaviour depends on what you did the last time you used it.
        composer_open_write();
        overlay_sync();
        break;

    case KBD_CONTROL_PHOTO:
        // The camera in one tap, rather than QUICK -> PHOTO. A card without
        // one has QUICK's AGAIN here instead: the recent prompts.
        if (focus_active()) focus_off();
        if (!photo_available()) {
            quick_menu = 1;
            quick_show(SUBSCREEN_QUICK_AGAIN);
            break;
        }
        if (!photo_proved()) {
            status("camera: not on this computer");
            break;
        }
        quick_close();
        photo_open();
        break;

    case KBD_CONTROL_SETTINGS:
        // A lower-screen mode rather than an overlay, but the page owns the
        // d-pad while it is up, so there is nothing left for a highlight to
        // walk: the page's own selection is the highlight.
        if (focus_active()) focus_off();
        status_mode_set(SCREEN_MODE_SETTINGS);
        // Both screens: like PROJECT, the page takes the top one, so entering
        // and leaving have to repaint both.
        page_repaint();
        overlay_sync();
        break;

    case KBD_CONTROL_QUICK:
        if (focus_active()) focus_off();
        quick_menu = 1;
        quick_show(SUBSCREEN_QUICK_MAIN);
        break;

    case KBD_CONTROL_REMOTE:
        remote_open();
        break;

    case KBD_CONTROL_EXTENSION:
        if (focus_active()) focus_off();
        ext_menu_open();
        overlay_sync();
        break;

    case KBD_CONTROL_RECONNECT:
        // A deliberate recovery action.  reconnect() closes the old socket,
        // clears connection-scoped view/session capability state, and waits
        // cooperatively through every retry, so this is safe even while the
        // terminal is otherwise healthy.
        if (focus_active()) focus_off();
        status("manual relink requested");
        reconnect();
        break;

    case KBD_CONTROL_NONE:
    default:
        break;
    }
}

// Touch, dispatched by whatever owns the sub screen. The first frame of a
// contact is always swallowed: the DS touchscreen reports a garbage coordinate
// on touch-down, and that one fact is the most common source of phantom
// presses on this hardware.
static void poll_touch(unsigned long *tx)
{
    const int overlay = composer.mode != COMPOSER_CLOSED ||
                        quick_menu || remote_menu || ext_menu_is_open();
    const int remote_live = remote_menu && composer.mode == COMPOSER_CLOSED;
    const int quick_live = quick_menu && composer.mode == COMPOSER_CLOSED;
    const int composing = composer_writing();
    const int keys_live = !overlay && status_mode_get() == SCREEN_MODE_KEYS;
    const int control   = !overlay && status_mode_get() == SCREEN_MODE_CONTROL;
    const int project_live = !overlay &&
                             status_mode_get() == SCREEN_MODE_PROJECT;
    const int settings_live = !overlay &&
                              status_mode_get() == SCREEN_MODE_SETTINGS;
    // STATUS is touchable now that it has tabs. Touch only, deliberately: it
    // leaves the terminal on the top screen, so binding left/right here would
    // cost cursor-left and cursor-right on a screen somebody may be typing at.
    const int status_live = !overlay &&
                            status_mode_get() == SCREEN_MODE_STATUS;

    const int touch_down = (composing || keys_live || control ||
                            ext_menu_is_open() || remote_live || quick_live ||
                            project_live || settings_live || status_live) &&
                           touch_held();

    if (touch_down && !touch_was_down) {
        touch_frames = 0;          // first frame: garbage coordinate
        touch_key = -1;
        ext_touch_down();
    } else if (touch_down) {
        touch_frames++;
        if (ext_touch_track()) {
            // The picker owns this contact until the pen lifts.
        } else if (touch_frames == 1) {
            touchPosition pos;
            touch_read(&pos);
            chime_user_activity();
            ext_event(EXT_EVENT_LOCAL_INPUT);

            if (quick_live) {
                // As REMOTE: a tap selects, a tap on the selected one does it.
                const int at = subscreen_quick_row_at(quick_level, quick_top, pos.py);
                if (at >= 0 && (unsigned)at < quick_count()) {
                    if (at == quick_sel) {
                        quick_activate(tx);
                    } else {
                        quick_sel = at;
                        quick_remove_armed = 0;
                        chime_focus();
                        overlay_sync();
                    }
                }
            } else if (remote_live) {
                // A tap selects a row; a tap on the row already selected
                // presses it -- the same two steps as UP/DOWN then A, so a
                // stray touch can never open something.
                const int at = subscreen_remote_row_at(pos.py);
                remote_row_t rows[REMOTE_ROWS_MAX];
                const unsigned n = remote_rows(&remote, &computers, rows,
                                               REMOTE_ROWS_MAX);
                if (at >= 0 && (unsigned)at < n &&
                    remote_row_selectable(&rows[at])) {
                    if (remote_ui.selection == at) {
                        remote_activate(tx);
                    } else {
                        remote_ui.selection = at;
                        remote_ui.copy_armed = 0;
                        chime_focus();
                        overlay_sync();
                    }
                }
            } else if (control) {
                // The extension gets the first look at a tap here: something
                // of its own under the pen takes the touch, and the tile
                // behind it is not pressed.  It cannot leak a PTY byte.
                if (!ext_touch_hit(pos.px, pos.py)) {
                    const kbd_control_t hit = kbd_control_hit(pos.px, pos.py);
                    control_press(hit, tx);
                    if (hit == KBD_CONTROL_EXTENSION) ext_touch_control_opened();
                }
            } else if (status_live) {
                const status_tab_t hit = status_tab_hit(pos.px, pos.py);
                if (hit != STATUS_TAB_COUNT) {
                    status_tab_set(hit);
                    refresh_sub();
                } else if (status_tab_get() == STATUS_TAB_CLIP &&
                           status_clip_action_hit(pos.px, pos.py)) {
                    // The bridge between the two halves of this feature: the
                    // tab is where you read the clipboard, the composer is
                    // where a paste has to pass a deliberate press, and
                    // without this the tab is a dead end you have to leave by
                    // hand.
                    if (!clip_pastable(&clip)) {
                        status("nothing on the clipboard to load");
                    } else if (composer_load(&composer, clip.text)) {
                        composer_pane = COMPOSER_PANE_WRITE;
                        snippet_pick = 0;
                        snippet_focus = 0;
                        overlay_sync();
                        status("clipboard loaded -- A sends it");
                    } else {
                        status("the clipboard does not fit the composer");
                    }
                }
            } else if (settings_live) {
                const unsigned tab_hit = settings_tab_hit(pos.px, pos.py);
                const unsigned tab = settings_row_tab(
                    (settings_row_t)ui_settings_ui.row);
                const unsigned scroll = ui_settings_ui.scroll;
                const settings_row_t hit = settings_row_hit(tab, scroll, pos.px, pos.py);
                const int arrow = settings_scroll_hit(tab, scroll, pos.px, pos.py);
                if (arrow) {
                    settings_ui_move(&ui_settings, &ui_settings_ui, 0, arrow);
                    settings_apply();
                    overlay_sync();
                } else if (tab_hit != SETTINGS_TAB_COUNT) {
                    if (tab_hit != tab) {
                        settings_ui_tab(&ui_settings, &ui_settings_ui, tab_hit);
                        settings_apply();
                        overlay_sync();
                    }
                } else if (hit != SETTINGS_ROW_COUNT) {
                    const unsigned chip = settings_option_hit(hit, scroll, pos.px,
                                                              pos.py);
                    if (chip != SETTINGS_OPTIONS_MAX) {
                        settings_chip_press(hit, chip, tx);
                    } else {
                        // On the row but not on a chip: move the highlight and
                        // press nothing.  That is what makes the labels safe
                        // to rest a thumb on, and it is why the hit test is
                        // two questions rather than one.
                        ui_settings_ui.row = (unsigned)hit;
                        ui_settings_ui.reset_armed = 0;
                        const unsigned n = settings_option_count(hit);
                        if (n && ui_settings_ui.option >= n) {
                            ui_settings_ui.option = n - 1;
                        }
                        settings_apply();
                        overlay_sync();
                    }
                }
            } else if (project_live) {
                const project_tab_t hit = project_tab_hit(pos.px, pos.py);
                if (hit != PROJECT_TAB_COUNT) {
                    project_tab_set(&project, hit);
                    project_resync();
                } else if (project_tab(&project) == PROJECT_TAB_DO) {
                    // Below the strip, the DO tab has buttons; every other tab
                    // is read rather than driven and a tap there does nothing.
                    const unsigned act = project_action_hit(pos.px, pos.py);
                    if (act != PROJECT_ACTIONS_MAX) {
                        project_action_press(act, tx);
                    }
                }
            } else if (composing) {
                if (voice_bar_hit(pos.px, pos.py) && mic_can_transcribe()) {
                    touch_key = -1;
                    kbd_set_held(-1);
                    if (!mic_recording() && !mic_waiting()) {
                        mic_touch_held = 1;
                        (void)mic_start(lang_voice_code(ui_settings.language), tx);
                    }
                } else {
                    touch_key = kbd_hit(pos.px, pos.py);
                    kbd_set_held(touch_key);
                    if (touch_key >= 0) composer_key(touch_key);
                }
            } else {
                touch_key = kbd_hit(pos.px, pos.py);
                kbd_set_held(touch_key);
                kbd_press_key(touch_key, tx);
            }
            if (composing) overlay_sync();     // repaints the sub screen too
            else           refresh_sub();      // show the held control inverted
        } else if (touch_key >= 0 &&
                   kbd_repeatable(touch_key) &&
                   kbd_repeat_should_fire(touch_frames)) {
            if (composing)      { composer_key(touch_key); overlay_sync(); }
            else if (keys_live) kbd_press_key(touch_key, tx);
        }
    } else if (touch_was_down) {
        // A contact must have two matching settled readings before it applies;
        // lift only ends the sample window. Headings and empty rows remain
        // inert as well.
        if (mic_touch_held) {
            mic_touch_held = 0;
            if (mic_recording()) mic_stop(tx);
        }
        ext_touch_up();
        kbd_set_held(-1);
        refresh_sub();
        touch_key = -1;
        touch_frames = 0;
    }
    touch_was_down = touch_down;
}

// d-pad repeat state for focus navigation, separate from the arrow repeat
// state: the two never run in the same frame, but keeping them apart means
// neither path can corrupt the other's clock.
static unsigned focus_held = 0;
static unsigned focus_frames = 0;
static unsigned focus_space_frames = 0;
static unsigned focus_delete_frames = 0;

static void focus_step(unsigned dp, unsigned long *tx);

// The face keys, while focus is on. Everything here is local except START --
// the interrupt is the one thing that must work no matter what mode the
// bottom screen is in.
static void focus_input(unsigned raw, unsigned edge, unsigned long *tx)
{
    // R submits only on the focused keyboard.  It is deliberately edge-only:
    // holding a shoulder must never stream CRs, and an R+face-key edge submits
    // once rather than also activating, deleting, or spacing.  L+R is removed
    // from edge before this function runs, so the focus toggle never submits.
    if ((edge & KEY_R) && focus_r_is_enter()) {
        send_key_seq("\r", 1, tx);
        refresh_sub();
        return;
    }

    // L pastes the desktop clipboard, as R's opposite number: R puts the line
    // in, L puts the text in. Edge-only for the same reason R is -- holding a
    // shoulder must never stream a clipboard -- and L+R is already removed
    // from edge above, so the focus toggle never pastes.
    //
    // Straight at the PTY, and that is not a hole in the
    // never-executes-on-its-own rule but an application of it: a paste with no
    // newline in it lands on the prompt line and stops, and the submit is
    // still a separate deliberate press. A clipboard that *does* contain a
    // newline would submit itself unread, so that one goes to the composer to
    // be looked at first. The card decides which, from the bytes it holds.
    if (edge & KEY_L) {
        if (!clip_pastable(&clip)) {
            status(clip_withheld(&clip)
                       ? "clipboard withheld: a concealed copy"
                       : "nothing on the clipboard to paste");
        } else if (clip_paste_is_inert(&clip)) {
            if (send_key_text(clip.text, clip.len, tx)) {
                status("pasted %u bytes -- R sends", (unsigned)clip.len);
            }
        } else if (composer_load(&composer, clip.text)) {
            // Leaves focus, which is visible and is the point: this clipboard
            // would have submitted itself, so it gets read first.
            focus_off();
            composer_pane = COMPOSER_PANE_WRITE;
            snippet_pick = 0;
            snippet_focus = 0;
            overlay_sync();
            status("multi-line clipboard -- read it, then A sends");
        } else {
            status("the clipboard does not fit the composer");
        }
        refresh_sub();
        return;
    }

    // B is context-sensitive, and the context is visible at a glance: while
    // the highlight is on a key, B is space -- the focused keyboard's shortcut
    // layout.  It repeats at the standard typing cadence.  While the highlight
    // is on a tile, B is "back" (leave focus): the tiles are not a typing
    // surface. X stays "cancel" everywhere in focus.
    if (edge & KEY_B) {
        focus_space_frames = 0;
        if (focus_b_is_space()) {
            send_key_seq(" ", 1, tx);
            key_tick(0);
        } else {
            focus_off();
        }
        refresh_sub();
        return;
    }
    if (raw & KEY_B) {
        focus_space_frames++;
        if (focus_b_is_space() && kbd_repeat_should_fire(focus_space_frames)) {
            send_key_seq(" ", 1, tx);
            key_tick(0);
        }
        // Fall through: holding B must not swallow the d-pad, or spacing a
        // word would freeze the highlight.
    } else {
        focus_space_frames = 0;
    }

    if (edge & KEY_X) { focus_off(); refresh_sub(); return; }

    if (edge & KEY_Y) {
        focus_delete_frames = 0;
        if (focus_y_is_backspace()) {
            send_key_seq("\x7f", 1, tx);
            key_tick(1);
            refresh_sub();
        }
        return;
    }
    if (raw & KEY_Y) {
        focus_delete_frames++;
        if (focus_y_is_backspace() &&
            kbd_repeat_should_fire(focus_delete_frames)) {
            send_key_seq("\x7f", 1, tx);
            key_tick(1);
        }
        // Fall through: holding Y must not swallow the d-pad.
    } else {
        focus_delete_frames = 0;
    }

    if (edge & KEY_A) {
        if (focus_place() == FOCUS_CONTROL) {
            // control_press() keeps focus consistent with what it activates:
            // KEYS moves the cursor onto the keyboard, overlays take it over.
            control_press((kbd_control_t)focus_control_id(), tx);
        } else {
            const int id = focus_key_id();
            if (id >= 0) kbd_press_key(id, tx);
        }
        refresh_sub();
        return;
    }

    if (edge & KEY_START) { send_key_seq("\x03", 1, tx); return; }

    // SELECT leaves focus and cycles the screen, so the new screen never
    // inherits a cursor that describes the old one.
    if (edge & KEY_SELECT) {
        select_cycled_from = (int)status_mode_get();
        focus_off();
        status_mode_next();
        refresh_sub();
        return;
    }

    const unsigned dp = raw & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (dp != focus_held) {
        focus_held = dp;
        focus_frames = 0;
        if (dp) focus_step(dp, tx);
    } else if (dp) {
        focus_frames++;
        // The standard repeat cadence: 400 ms, then every 60 ms. Without
        // repeat, crossing a ten-key row is ten presses; with it, one hold.
        if (kbd_repeat_should_fire(focus_frames)) focus_step(dp, tx);
    } else {
        focus_frames = 0;
    }
}

static void focus_step(unsigned dp, unsigned long *tx)
{
    (void)tx;
    const int before = focus_place() == FOCUS_CONTROL ?
                       focus_control_id() : focus_key_id();
    int dx = 0, dy = 0;
    if (dp & KEY_UP) dy = -1;
    else if (dp & KEY_DOWN) dy = 1;
    else if (dp & KEY_LEFT) dx = -1;
    else if (dp & KEY_RIGHT) dx = 1;

    if (dy < 0 && focus_at_top()) {
        // Up past the top row returns to the input box: the symmetric answer
        // to Down having brought the cursor down here.
        focus_off();
    } else {
        focus_move(dx, dy);
    }
    if (focus_active()) {
        const int after = focus_place() == FOCUS_CONTROL ?
                          focus_control_id() : focus_key_id();
        if (after != before) {
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        }
    }
    refresh_sub();
}


// ------------------------------------------------------------ CONTROLS ---

// The guide's state: guide.c decides what every press means; this is the
// thin part that reads the card's settings for it, feeds it the key word,
// and draws what it decided.
static guide_t guide;
static guide_config_t guide_cfg;
static unsigned guide_held;
static unsigned guide_touch_frames;

static void guide_config_fill(void)
{
    // Read every time: a remap on Settings' BUTTONS tab shows at once.
    guide_cfg.map = &ui_settings.buttons;
    guide_cfg.shutter = ui_settings.camera_shutter;
    guide_cfg.latch = ui_settings.mic_mode == SETTINGS_MIC_LATCH;
    guide_cfg.talk = mic_can_transcribe();
    guide_cfg.camera = photo_available();
}

static void guide_sync(void)
{
    subscreen_set_guide(guide_open, &guide, &guide_cfg, boot_console_family(),
                        guide_held);
    refresh_sub();
}

// The top screen while the guide is up: the still session, and the pop-up
// over it when there is one.  Nothing here is read from the PTY.
static void guide_paint_top(void)
{
    static grid_t session;
    static int filled = 0;
    if (!filled) {
        guide_session_fill(&session);
        filled = 1;
    }
    render_overlay_grid(&session);
    if (!guide.shown) return;
    static char lines[GUIDE_POPUP_LINES][GUIDE_LINE_MAX];
    const unsigned n = guide_popup_lines(&guide.entry, lines);
    const char *rows[GUIDE_POPUP_LINES];
    for (unsigned i = 0; i < GUIDE_POPUP_LINES; i++) rows[i] = lines[i];
    const uint16_t colours[GUIDE_POPUP_LINES] = {
        render_palette_rgb(PAL_ORANGE), render_palette_rgb(PAL_WHITE),
        render_palette_rgb(PAL_DEFAULT), render_palette_rgb(PAL_DEFAULT),
    };
    int x, y, w, h;
    guide_popup_rect(n, &x, &y, &w, &h);
    render_overlay_box(x, y, w, h, rows, colours, (int)n, GUIDE_POPUP_PITCH);
}

static void guide_show(guide_ctx_t ctx)
{
    guide_config_fill();
    guide_init(&guide, ctx, &guide_cfg);
    guide_open = 1;
    guide_held = 0;
    // The contact that opened it (a QUICK row, a Settings chip) is still
    // down: it must not also press a tab or DONE where it happens to rest.
    guide_touch_frames = 2;
    kbd_set_held(-1);
    guide_sync();
    overlay_sync();
}

// Buttons held when the guide closed: ignored until released (poll_input).
static unsigned held_past_guide;

static void guide_close(unsigned raw)
{
    guide_open = 0;
    subscreen_set_guide(0, NULL, NULL, CONSOLE_UNKNOWN, 0);
    // Whatever is still held was pressed for the guide.  None of it may
    // land on the screen underneath once that screen has the keys back: a
    // held direction waits for its release, and a pen still down does not
    // become a fresh tap on whatever is under it.
    held_past_guide = raw & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R |
                             KEY_START | KEY_SELECT);
    const unsigned dp = raw & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    dpad_suppressed = dp != 0;
    dpad_held = dp;
    dpad_frames = 0;
    focus_held = dp;
    composer_focus_held = dp;
    quick_held = raw & (KEY_UP | KEY_DOWN);
    if (touch_held()) {
        touch_was_down = 1;
        touch_frames = 2;
        touch_key = -1;
    }
    page_repaint();
    overlay_sync();
}

// SELECT held, then START -- or both in the same frame. Never START first:
// by the time SELECT joins it, START's own action (Ctrl-C by default, Enter
// if remapped) has already gone to the session, so opening the guide then
// would teach the chord by interrupting Claude. START first stays START.
static int guide_chord(unsigned raw, unsigned edge)
{
    return (edge & KEY_START) && (raw & KEY_SELECT);
}

// The guide's only touch targets: the context tabs and DONE.
static void guide_touch(unsigned raw)
{
    const int down = touch_held();
    if (down && !touch_was_down) {
        guide_touch_frames = 0;        // first frame: garbage coordinate
    } else if (down && ++guide_touch_frames == 1) {
        touchPosition pos;
        touch_read(&pos);
        if (guide_done_hit(pos.px, pos.py)) {
            touch_was_down = 1;
            guide_close(raw);
            return;
        }
        const guide_ctx_t hit = guide_tab_hit(&guide_cfg, pos.px, pos.py);
        if (hit != GUIDE_CTX_COUNT && hit != guide.ctx) {
            guide_set_ctx(&guide, hit, &guide_cfg);
            chime_focus();
            guide_sync();
        }
    }
    touch_was_down = down;
}

// Every press while the guide is up comes here and nowhere else.
static void guide_input(unsigned raw, unsigned edge)
{
    guide_config_fill();
    const guide_step_t step = guide_step(&guide, &guide_cfg, raw, edge);
    if (step == GUIDE_STEP_LEAVE) {
        guide_close(raw);
        return;
    }
    guide_touch(raw);
    if (!guide_open) return;
    if (step == GUIDE_STEP_CONTEXT) chime_focus();
    if (step == GUIDE_STEP_SHOW || step == GUIDE_STEP_CONTEXT) overlay_dirty = 1;
    if (guide_tick(&guide)) overlay_dirty = 1;
    if (raw != guide_held || step != GUIDE_STEP_NONE) {
        guide_held = raw;
        guide_sync();
    }
}

// Returns the combined newly-pressed word so the caller can show it.
static int poll_input(unsigned long *tx)
{
    const unsigned raw_all = kbd_held_keyword((unsigned)(~REG_KEYINPUT), keysHeld());
    const unsigned edge_all = kbd_edge_keyword(raw_all, prev_raw);
    prev_raw = raw_all;
    // A button still held when the guide closed was pressed for the guide.
    // It counts as up until it is let go, so a B held to leave cannot go on
    // to repeat spaces into the session from focus, nor a held A or START
    // send Enter or Ctrl-C to the screen underneath.
    held_past_guide &= raw_all;
    const unsigned raw = raw_all & ~held_past_guide;
    const unsigned edge = edge_all & ~held_past_guide;
    // The raw edge is returned to the caller for diagnostics.  Focus may
    // consume shoulders from its private copy when L+R is a mode chord.
    unsigned focus_edge = edge;

    // A deliberate press wakes the extension's quiet clock even when it is only a
    // local modifier or belongs to an overlay. Key repeats are not separate
    // acts of attention; the initial press already registered them.
    if (edge) {
        chime_user_activity();
        ext_event(EXT_EVENT_LOCAL_INPUT);
    }
    if (!(raw & KEY_SELECT)) select_cycled_from = -1;

    // The controls guide owns every control while it is open, ahead of every
    // other dispatch, and returns before any of them.  That is its safety
    // property: a person learning the buttons by pressing them is shown what
    // each would do, and nothing reaches the PTY, the composer or the camera
    // -- no Enter, no Esc, no Ctrl-C, no shutter.  Touch is the guide's too
    // (its tabs and DONE): poll_touch() is not called.
    if (guide_open) {
        guide_input(raw, edge);
        return (int)edge;
    }

    // A local overlay owns every control while it is open, and nothing past
    // this point can reach the PTY. That is the composer's whole safety
    // property: a prompt being typed on a handheld must not be half-submitted
    // by a stray press, so the only thing that sends is an explicit Send.
    if (composer.mode != COMPOSER_CLOSED) {
        composer_input(raw, edge, tx);
        poll_touch(tx);
        return (int)edge;
    }
    // SELECT, then START, opens the guide from anywhere but the composer
    // (above, whose keys it would shadow mid-prompt) and the camera.
    // SELECT's own press cycled the screen, which is put back, so the guide
    // opens over where you were. START first is never the chord (guide_chord).
    if (!photo_is_open() && guide_chord(raw, edge)) {
        if (select_cycled_from >= 0) {
            if (focus_active()) focus_off();
            status_mode_set((screen_mode_t)select_cycled_from);
            select_cycled_from = -1;
            page_repaint();
        }
        guide_show(guide_ctx_for(status_mode_get() == SCREEN_MODE_KEYS,
                                 focus_active(),
                                 quick_menu || remote_menu || ext_menu_is_open()));
        return (int)edge;
    }
    if (quick_menu) {
        quick_input(raw, edge, tx);
        poll_touch(tx);
        return (int)edge;
    }
    // The camera overlay owns every control while it is up: nothing a thumb
    // does to frame a picture may leak into the PTY.  Its shutter is the one
    // deliberate exception, and it sends a camera message, not key bytes.
    if (photo_is_open()) {
        const photo_exit_t where = photo_input(raw, edge);
        if (where != PHOTO_STAY) photo_leave(where);
        return (int)edge;
    }
    if (remote_menu) {
        remote_input(raw, edge, tx);
        poll_touch(tx);
        return (int)edge;
    }
    if (ext_menu_is_open()) {
        ext_menu_input(raw, edge);
        poll_touch(tx);
        return (int)edge;
    }

    // L+R together toggles focus mode -- the d-pad model: the d-pad walks a
    // highlight over the bottom screen's controls and keys, and A activates
    // the lit item. A shoulder chord, because no single button is free: every
    // face key and SELECT carry a meaning, and L or R alone must stay a
    // modifier arm (L then a letter is Ctrl-letter). The frame the chord
    // completes is the frame one shoulder was pressed, so the arming below
    // must not also arm that shoulder -- focus is deliberate and a stray Ctrl
    // is a surprising keystroke. STATUS is a log with no controls, so there
    // the chord explicitly says why focus is unavailable.
    const int lr_both = ((raw & (KEY_L | KEY_R)) == (KEY_L | KEY_R));
    if (lr_both && (edge & (KEY_L | KEY_R))) {
        if (kbd_ctrl_armed()) kbd_ctrl_arm();   // toggles, so this disarms
        kbd_shift_consume();
        if (focus_active()) {
            focus_off();
        } else if (status_mode_get() == SCREEN_MODE_CONTROL) {
            focus_on(FOCUS_CONTROL);
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        } else if (status_mode_get() == SCREEN_MODE_KEYS) {
            focus_on(FOCUS_KEYS);
            ext_event(EXT_EVENT_FOCUS);
            chime_focus();
        } else if (status_mode_get() == SCREEN_MODE_SETTINGS) {
            // SETTINGS walks with the d-pad already; nothing to focus.
            status("SETTINGS: the d-pad already moves the highlight");
        } else {
            // STATUS has no selectable targets, but a learned focus chord
            // must not silently vanish there.
            status("focus unavailable: status is read-only");
        }
        refresh_sub();
        // The chord owns both shoulders.  In particular, entering keyboard
        // focus with L+R must not turn that same R edge into a submission.
        focus_edge &= ~(KEY_L | KEY_R);
    } else if (focus_active()) {
        // The focused keyboard dedicates R to Enter.  Other focused surfaces
        // retain the existing one-shot Shift arm because they are not active
        // text entry.  L alone stays inert -- it is the exit chord's other
        // half.
        if (edge & KEY_R) {
            if (!focus_r_is_enter()) {
                kbd_shift_arm();
                refresh_sub();
            }
        }
    } else if (status_mode_get() != SCREEN_MODE_SETTINGS) {
        // Not on SETTINGS, where L and R switch tabs: arming Ctrl or Shift
        // there would leave it waiting for the next key typed elsewhere.
        if (edge & KEY_L) {
            kbd_ctrl_arm();
            refresh_sub();
        }
        if (edge & KEY_R) {
            kbd_shift_arm();
            refresh_sub();
        }
    }

    // While focus is on, the d-pad and the face keys belong to the highlight,
    // not to the PTY. That is the mode's whole point, and it is visible: the
    // lit tile or key is filled in Claude's orange and the footer says so.
    if (focus_active()) {
        focus_input(raw, focus_edge, tx);
        poll_touch(tx);
        return (int)edge;
    }

    // X backs out to the Control screen from anywhere the bottom screen has
    // gone: KEYS, STATUS, PROJECT, SETTINGS.  One press home, rather than up
    // to four presses of a cycling button to get back to the screen with the
    // buttons on it.
    //
    // It costs nothing, and that is not an accident -- status_mode_back()
    // reports whether it did anything, and only then is the key swallowed.
    // On Control, where X is the Escape half of the approval pair and the one
    // that dismisses a permission prompt, it falls straight through to the key
    // table below and still sends 0x1b.  A navigation key that quietly eats
    // Escape on the screen you approve things from would be a bad trade.
    if ((edge & KEY_X) && status_mode_back()) {
        // Same repaint as SELECT: PROJECT and SETTINGS own the top screen, so
        // leaving one has to give it back as well as redraw the bottom.
        page_repaint();
        overlay_sync();
        poll_touch(tx);
        return (int)edge;
    }

    // SELECT cycles the bottom screen: CONTROL -> KEYS -> STATUS -> PROJECT.
    // Local only -- no bytes are sent -- so it is handled here rather than in
    // the key table below.
    if (edge & KEY_SELECT) {
        select_cycled_from = (int)status_mode_get();
        status_mode_next();
        page_repaint();
        // Not just the sub screen any more: PROJECT owns the top one too, so
        // entering or leaving it has to repaint both.  overlay_sync() does
        // that and bumps the extension's epoch, which matters here because the
        // grid under its drawing has just been swapped for a different one.
        overlay_sync();
    }

    // The two local pages own input while they are up, exactly as focus mode
    // does above and for the same reason: they are read, not typed into, and
    // the top screen is showing the page rather than the terminal, so a key
    // pressed here would be a byte sent blind at something you cannot see.
    // Nothing below this point runs, so no page key can reach the PTY.
    if (status_mode_get() == SCREEN_MODE_SETTINGS) {
        // The d-pad walks the chip grid: up and down between rows, left and
        // right along a row's options.  A presses the highlighted chip, so a
        // stylus is never required for anything.
        if (edge & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            const int rows = (edge & KEY_DOWN) ? 1 : (edge & KEY_UP) ? -1 : 0;
            const int cols = (edge & KEY_RIGHT) ? 1 : (edge & KEY_LEFT) ? -1 : 0;
            settings_ui_move(&ui_settings, &ui_settings_ui, cols, rows);
            settings_apply();
            overlay_sync();
        }
        if (edge & KEY_A) {
            settings_chip_press((settings_row_t)ui_settings_ui.row,
                                ui_settings_ui.option, tx);
        }
        // L and R step between the five tabs.  Both together stay the focus
        // chord they are everywhere else.
        if ((edge & (KEY_L | KEY_R)) && (raw & (KEY_L | KEY_R)) != (KEY_L | KEY_R)) {
            const unsigned tab = settings_row_tab((settings_row_t)ui_settings_ui.row);
            const unsigned next = (edge & KEY_R)
                ? (tab + 1u) % SETTINGS_TAB_COUNT
                : (tab + SETTINGS_TAB_COUNT - 1u) % SETTINGS_TAB_COUNT;
            settings_ui_tab(&ui_settings, &ui_settings_ui, next);
            settings_apply();
            overlay_sync();
        }
        poll_touch(tx);
        return (int)edge;
    }
    if (status_mode_get() == SCREEN_MODE_PROJECT) {
        if (edge & (KEY_LEFT | KEY_RIGHT)) {
            project_tab_step(&project, (edge & KEY_RIGHT) ? 1 : -1);
            project_resync();
            overlay_sync();
            refresh_sub();
        }
        if (project_tab(&project) == PROJECT_TAB_DO) {
            // A stylus is not always to hand.
            if (edge & (KEY_UP | KEY_DOWN)) {
                project_action_select(&project, (edge & KEY_DOWN) ? 1 : -1);
                project_resync();
                refresh_sub();
            }
            if (edge & KEY_A) {
                project_action_press(project_action_selected(&project), tx);
            }
        }
        poll_touch(tx);
        return (int)edge;
    }

    // L and R are the one-shot modifiers (docs/design/INPUT_AND_KEYBOARD.md: sticky and
    // one-shot -- press twice to disarm). Local only; the footer reflects the
    // armed state.
    if (edge & KEY_L) {
        kbd_ctrl_arm();
        refresh_sub();
    }
    if (edge & KEY_R) {
        kbd_shift_arm();
        refresh_sub();
    }

    // Physical A/B/X/Y/START: single-shot, no repeat -- a repeated Enter or
    // interrupt is never what anyone meant (docs/design/INPUT_AND_KEYBOARD.md).
    const int down = (int)edge;
    const char *seq = NULL;
    size_t seq_len = 0;
    int y_was_shift_tab = 0;

    // What each face button sends is Settings' BUTTONS tab (buttons.h): by
    // default A Enter, B Delete, X Esc, Y Tab, START Stop.  The approval
    // pair is whichever buttons hold Enter and Esc, and the headline and
    // the legend name them.  Every byte is what a keyboard would send.
    static const struct { int key; button_t button; } FACE[] = {
        { KEY_A, BUTTON_A }, { KEY_B, BUTTON_B }, { KEY_X, BUTTON_X },
        { KEY_Y, BUTTON_Y }, { KEY_START, BUTTON_START },
    };
    button_action_t pressed = BUTTON_ACTION_COUNT;
    for (unsigned i = 0; i < sizeof(FACE) / sizeof(FACE[0]); i++) {
        if (down & FACE[i].key) {
            pressed = (button_action_t)ui_settings.buttons.action[FACE[i].button];
            break;
        }
    }
    if (pressed == BUTTON_TAB && kbd_shift_armed()) {
        // Shift-Tab ("\x1b[Z"), how Claude Code cycles modes, when the
        // one-shot Shift from R is armed.  Consuming it shows in the footer.
        // Captured before consume() clears it, or the typing cue below would
        // voice a mode cycle as a keypress.
        y_was_shift_tab = 1;
        seq = "\x1b[Z"; seq_len = 3;
        kbd_shift_consume();
        refresh_sub();
    } else if (pressed != BUTTON_ACTION_COUNT) {
        seq = button_action_bytes(pressed, &seq_len);
    }

    if (seq && send_key_seq(seq, seq_len, tx)) typed_bytes(seq, seq_len);
    // UNDO's rewind takes two Enters (the point, then what to restore);
    // Esc or Stop leaves it.  Either way the d-pad goes back to scrolling.
    if (rewind_picking) {
        if (pressed == BUTTON_ENTER && ++rewind_enters >= 2) rewind_done();
        else if (pressed == BUTTON_ESC || pressed == BUTTON_STOP) rewind_done();
    }
    // The typing actions voice themselves: Delete, Space and Tab.  Enter
    // skips because a submitted CR already owns SENT; Esc and Stop are mode
    // actions, not typing; and a Shift-Tab is a mode cycle, not a keypress.
    if (pressed == BUTTON_DELETE) key_tick(1);
    else if ((pressed == BUTTON_SPACE || pressed == BUTTON_TAB) && !y_was_shift_tab) {
        key_tick(0);
    }

    // Control mode spends the d-pad on reading: up and down move the view
    // through history without the L chord, because reading is what that screen
    // is for. Left and right keep a way to move a selection -- a Control mode
    // that can only accept the highlighted option is worse than no Control
    // mode at all -- and fall through to dpad_send() below as ordinary arrows.
    //
    // They send the horizontal pair. If a permission list turns out to honour
    // only the vertical arrows, this comment is the map: dpad_send() already
    // sends "\x1b[A"/"\x1b[B" for up/down, so the change is to route left and
    // right to those two byte strings instead.
    // ...except while Claude is waiting on you: then the thing on screen is
    // its list of choices, and up and down move through it, as they would at
    // a keyboard.  The headline says so ("d-pad picks").
    if (status_mode_get() == SCREEN_MODE_CONTROL &&
        (chime_needs_you() || rewind_picking) &&
        (edge & (KEY_UP | KEY_DOWN))) {
        // Falls through to dpad_send() below with key repeat, untouched.
    } else if (status_mode_get() == SCREEN_MODE_CONTROL &&
               (edge & (KEY_UP | KEY_DOWN))) {
        scroll_gesture(edge & KEY_UP, tx);
        dpad_suppressed = 1;
        live_hold_frames = (edge & KEY_DOWN) ? 1 : 0;
        refresh_sub();
    }

    // Shift armed, then Up or Down, scrolls from the keyboard screen.
    //
    // This used to be L *held* plus a direction, which asked for exactly the
    // grip the one-shot modifiers exist to avoid: docs/design/INPUT_AND_KEYBOARD.md chose
    // arm-then-press because "a modifier and a letter cannot be pressed
    // together" on this hardware -- L is on the shoulder and everything else is
    // under the thumbs. A held chord for scrolling had the same problem and no
    // excuse, since the armed mechanism was already sitting there.
    //
    // R arms Shift, Up or Down scrolls, and the arm is consumed like any other
    // byte-producing key would consume it.
    //
    // `else if`, because in Control mode the block above already scrolled:
    // otherwise one press would send two deltas and move the view twice as far
    // as the button says.
    else if (kbd_shift_armed() && (edge & (KEY_UP | KEY_DOWN))) {
        scroll_gesture(edge & KEY_UP, tx);
        kbd_shift_consume();
        dpad_suppressed = 1;
        refresh_sub();
    }

    // d-pad, with key repeat. A direction consumed by a view chord remains
    // suppressed until release, even if L is released first.
    const unsigned dpad = raw & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    if (dpad_suppressed) {
        if (!dpad) dpad_suppressed = 0;
        // Down, held past half a second: all the way back to now.
        if (live_hold_frames && (raw & KEY_DOWN)) {
            if (++live_hold_frames == LIVE_HOLD_FRAMES) {
                jump_live(tx);
                refresh_sub();
            }
        } else {
            live_hold_frames = 0;
        }
        dpad_held = dpad;
        dpad_frames = 0;
    } else if (dpad && dpad != dpad_held) {
        dpad_held = dpad;
        dpad_frames = 0;
        dpad_send(dpad, tx);
    } else if (dpad && dpad == dpad_held) {
        dpad_frames++;
        if (kbd_repeat_should_fire(dpad_frames)) dpad_send(dpad, tx);
    } else {
        dpad_held = 0;
        dpad_frames = 0;
    }

    // Touch is meaningful on the KEYS and CONTROL screens; in STATUS mode the
    // sub screen is a log and a touch does nothing.
    poll_touch(tx);

    return down;
}

// ------------------------------------------------------------------- diag --

// The counters no longer have their own function: refresh_sub() paints the two
// counter lines via subscreen_update() once a second, from the accumulated
// totals below. The lines exist because a photograph of the handheld cannot
// distinguish failures that all look like a frozen screen, and guessing between
// them has already cost two hardware cycles:
//
//   in 0 B            nothing is reaching this program at all
//   in > 0, 0 msgs    bytes arrive but never form a message -- framing
//   msgs > 0          the wire is fine and the bug is in the renderer
//
//   raw 000, lib 000   no press reached the ARM9 at all
//   raw set, lib 000   the register sees it and libnds does not
//   both set, sent 0   both see it and this program declined to send
//   sent rising        the DS sent, and the loss is past this program
//
// `loop` is the main loop's counter, which also confirms the console is
// running this build rather than a stale card.  TROUBLESHOOTING.md has the
// same table for the person holding the console.

// ------------------------------------------------------------------ main ---

static void handle_message(msg_type_t type, const uint8_t *payload, uint16_t len)
{
    proto_result_t r;
    int lines = 0;

    switch (type) {
    case MSG_FRAME_FULL:
        r = proto_apply_full(&grid, payload, len);
        if (r == PROTO_OK) {
            render_grid(&grid);
            ext_terminal_traffic();
            chime_cells(len > 0 ? payload[0] : ' ');
            claude_mode_scan();
        } else {
            status("bad full frame: %s", proto_strerror(r));
        }
        break;

    case MSG_FRAME_DIFF:
        r = proto_apply_diff(&grid, payload, len);
        if (r != PROTO_OK) {
            status("bad diff: %s", proto_strerror(r));
            break;
        }
        // Redraw only what changed: the payload lists exactly those cells, so
        // a typical frame touches ~117 of 1008.
        for (uint16_t i = 0; i < len / PROTO_DIFF_ENTRY; i++) {
            const uint8_t *p = payload + (size_t)i * PROTO_DIFF_ENTRY;
            render_cell(p[0], p[1], grid.cells[(size_t)p[0] * PROTO_COLS + p[1]]);
        }
        ext_terminal_traffic();
        // The first changed glyph of the message pitches the murmur blip.
        chime_cells(len >= PROTO_DIFF_ENTRY ? payload[2] : ' ');
        claude_mode_scan();
        break;

    case MSG_SCROLL:
        r = proto_apply_scroll(&grid, payload, len, &lines);
        if (r == PROTO_OK) {
            render_scroll(lines);
            ext_terminal_traffic();
            chime_cells(' ');
            claude_mode_scan();
        } else {
            status("bad scroll: %s", proto_strerror(r));
        }
        break;

    case MSG_CURSOR: {
        cursor_t cur;
        r = proto_apply_cursor(&cur, payload, len);
        if (r == PROTO_OK) {
            ext_cursor(&cur);
            render_cursor_set(&grid, cur.row, cur.col, cur.visible);
        } else {
            status("bad cursor: %s", proto_strerror(r));
        }
        break;
    }

    case MSG_PONG:
        // Answers the outstanding probe, if there still is one. A PONG that
        // arrives after the probe timed out is ignored rather than reported as
        // a fast round trip that never happened.
        if (probe_outstanding) {
            probe_outstanding = 0;
            rtt_ms = (unsigned)((now_us() - probe_sent_us) / 1000UL);
            rtt_known = 1;
        }
        break;

    case MSG_VIEW_POS: {
        view_pos_t pos;
        r = proto_apply_view_pos(&pos, payload, len);
        if (r == PROTO_OK) {
            status_view_pos_set(pos.lines_back, pos.available);
            // Reading history is our own traffic: no murmurs, no cues, and
            // the quiet clock restarts when the reader returns to live.
            chime_history(pos.lines_back > 0);
            refresh_sub();
        } else {
            status("bad view pos: %s", proto_strerror(r));
        }
        break;
    }

    case MSG_MODE: {
        uint8_t flags = 0;
        r = proto_apply_mode(&flags, payload, len);
        if (r == PROTO_OK) {
            app_scrolls = flags & PROTO_MODE_APP_SCROLLS;
            app_scrolls_known = 1;
            status_scroll_regime_set(app_scrolls);
            refresh_sub();
        } else {
            status("bad mode: %s", proto_strerror(r));
        }
        break;
    }

    case MSG_SESSIONS: {
        session_chunk_t chunk;
        r = proto_apply_sessions(&chunk, payload, len);
        if (r != PROTO_OK) {
            status("bad sessions chunk: %s", proto_strerror(r));
            break;
        }
        const int complete = sessions_apply_chunk(&sessions, &chunk);
        if (complete < 0) {
            status("bad sessions chunk; list discarded");
            break;
        }
        if (complete > 0) {
            // `sessions` owns the false->true test.  One aggregate cue per
            // completed list is enough even if several background lanterns
            // woke together; the conductor will never queue a noisy chorus.
            if (sessions_take_new_output_mask(&sessions)) {
                ext_event(EXT_EVENT_LANTERN);
                chime_session_lantern();
            }
            // A SESSIONS snapshot updates state, not the ordinary Control
            // screen.  Repainting the full lower bitmap for every background
            // list publication used to make a legal idle-age refresh exercise
            // the display path forever even though that screen renders none of
            // the list's fields.  Keep the snapshot/pick gate current, but
            // redraw only while the picker is actually visible; opening it
            // calls overlay_sync() and draws the latest committed snapshot.
            // Nothing on screen shows the list any more (REMOTE does): it
            // is kept for the ABOUT block and the lantern cue.
        }
        break;
    }

    case MSG_COMMANDS:
        (void)quick_commands_apply(&commands, payload, len);
        if (quick_menu) {
            if ((unsigned)quick_sel >= quick_count()) quick_sel = 0;
            overlay_sync();
        }
        break;

    case MSG_SNIPPETS: {
        // One chunk of a pushed snippet set (docs/design/SNIPPETS_PUSH.md). The
        // list is RAM-only and applied only when the set completes; the card
        // file remains the durable store. Picked snippets still pass through
        // the composer -- picking is not sending.
        char label[SNIPPET_LABEL_MAX + 1];
        char body[COMPOSER_MAX + 1];
        size_t label_len = 0, body_len = 0;
        unsigned index = 0, total = 0;
        r = proto_apply_snippet(&index, &total, label, sizeof(label),
                                &label_len, body, sizeof(body), &body_len,
                                payload, len);
        if (r != PROTO_OK) {
            status("bad snippet chunk: %s", proto_strerror(r));
            break;
        }
        const int done = snippets_apply_pushed(&snippets, index, total,
                                               label, label_len,
                                               body, body_len);
        if (done < 0) {
            status("bad snippet chunk; push discarded");
        } else if (done > 0) {
            status("snippets: %u from server", total);
            refresh_sub();
        }
        break;
    }

    case MSG_CLIP: {
        unsigned count = 0, index = 0, seq = 0, flags = 0, body_len = 0;
        uint32_t total = 0;
        char body[CLIP_CHUNK_MAX + 1];
        r = proto_apply_clip(&count, &index, &seq, &flags, &total,
                             body, sizeof(body), &body_len, payload, len);
        if (r != PROTO_OK) {
            // Opted into by this client, so the type is expected -- but a
            // malformed frame is a clipboard that does not update, not a
            // terminal worth tearing down. Unlike every other decoder here it
            // may also be refusing a body that held a control byte, which is
            // deliberate: a clipboard silently altered on the way in is a
            // paste that does something other than what was copied.
            status("ignored malformed clipboard frame");
            break;
        }
        if (clip_wire_chunk(&clip, count, index, seq, flags, total,
                            body, body_len)) {
            clip_resync();
            if (composer.mode != COMPOSER_CLOSED &&
                composer_pane == COMPOSER_PANE_CLIP) {
                overlay_sync();
            }
            refresh_sub();
        }
        break;
    }

    case MSG_PROJECT: {
        unsigned count = 0, index = 0, tab = 0, fg = 0;
        char label[PROJECT_LABEL_MAX + 1];
        char value[PROJECT_VALUE_MAX + 1];
        r = proto_apply_project(&count, &index, &tab, &fg,
                                label, sizeof(label), value, sizeof(value),
                                payload, len);
        if (r != PROTO_OK) {
            // Opted into by this client, so the type is expected -- but a
            // malformed body is a page that does not update, not a terminal
            // worth tearing down.
            status("ignored malformed project row");
            break;
        }
        if (project_wire_row(&project, count, index, tab, fg, label, value)) {
            project_resync();
            if (status_mode_get() == SCREEN_MODE_PROJECT) overlay_sync();
            refresh_sub();
        }
        break;
    }

    case MSG_REMOTE: {
        remote_chunk_t chunk;
        if (proto_apply_remote(&chunk, payload, len) != PROTO_OK) {
            status("ignored a malformed remote row");
            break;
        }
        const int was_ready = remote.ready;
        // Which session the selection is on, by name, before the set that
        // may re-sort it lands.
        char keep_name[REMOTE_NAME_MAX + 1] = "", keep_detail[REMOTE_DETAIL_MAX + 1] = "";
        int keeping = 0;
        if (remote_menu && remote_ui.selection >= 0) {
            remote_row_t rows[REMOTE_ROWS_MAX];
            const unsigned n = remote_rows(&remote, &computers, rows,
                                           REMOTE_ROWS_MAX);
            if ((unsigned)remote_ui.selection < n &&
                rows[remote_ui.selection].kind == REMOTE_ROW_SESSION) {
                const remote_session_t *sel =
                    &remote.items[rows[remote_ui.selection].index];
                snprintf(keep_name, sizeof(keep_name), "%s", sel->name);
                snprintf(keep_detail, sizeof(keep_detail), "%s", sel->detail);
                keeping = 1;
            }
        }
        const int done = remote_apply_chunk(&remote, &chunk);
        if (done > 0) {
            // The first set proves the server speaks REMOTE.  If the picker
            // was opened before that proof, tell the server now.
            if (!was_ready && remote_menu) send_remote_open(1);
            if (remote_menu) {
                remote_ui_follow(&remote_ui, &remote, &computers,
                                 keeping ? keep_name : NULL, keep_detail);
                overlay_sync();
            }
        }
        break;
    }

    case MSG_REMOTE_NOTE: {
        char note[REMOTE_NOTE_MAX + 1];
        if (proto_apply_remote_note(note, sizeof(note), payload, len) == PROTO_OK) {
            remote_set_note(&remote, note, strlen(note));
            status("remote: %s", note);
            if (remote_menu) overlay_sync();
        }
        break;
    }

    case MSG_STATE: {
        uint8_t st = 0;
        char detail[PROTO_STATE_DETAIL_MAX + 1];
        int has_ask = 0;
        uint32_t id = 0;
        uint8_t flags = 0;
        if (proto_apply_state_ask(&st, detail, sizeof(detail), &has_ask, &id,
                                  &flags, payload, len) == PROTO_OK) {
            if (has_ask) {
                ask_known = 1;
                ask_id = id;
                ask_open = st == PROTO_STATE_APPROVAL;
                // The computer held back an Enter: the prompt it was meant
                // for had gone, or this card had not seen it yet.  Say so,
                // or the press just seems lost.
                if (flags & PROTO_ASK_FLAG_DROPPED) {
                    status("not sent: the screen changed -- look, then press again");
                }
            }
            chime_claude_state(st);
            refresh_sub();
        }
        break;
    }

    case MSG_CAMERA_READY: {
        // The computer's camera proof for this connection: every camera
        // message we may send is gated on it, and PHOTO's note follows it.
        int can_save = 0;
        if (proto_decode_camera_ready(&can_save, payload, len) != PROTO_OK) {
            status("ignored a malformed camera ready");
            break;
        }
        photo_ready(can_save);
        quick_set_photo_note(can_save ? TR_COUNT : TR_QUICK_NOTE_NO_SAVE);
        if (quick_menu) overlay_sync();
        break;
    }

    case MSG_CAMERA_ACK: {
        uint8_t kind = 0;
        uint32_t value = 0;
        if (proto_decode_camera_ack(&kind, &value, payload, len) != PROTO_OK) {
            status("ignored a malformed camera ack");
            break;
        }
        if (!photo_ack(kind, value)) {
            // A credit outside the window means the server lost count of
            // this upload: the wire, not the picture, is what is wrong.
            status("camera ack out of window; reconnecting");
            reconnect();
        }
        break;
    }

    case MSG_MIRROR: {
        int watching = 0;
        if (proto_decode_mirror(&watching, payload, len) != PROTO_OK) {
            status("ignored a malformed view state");
            break;
        }
        if (watching && !mirror_watching) {
            status("view: the computer is watching the bottom screen");
            mirror_sent_hash = 0;              // send the whole screen now
            mirror_sent_at = frame_count - MIRROR_MIN_FRAMES;
        }
        mirror_watching = watching;
        break;
    }

    case MSG_TOUCH: {
        uint8_t x = 0, y = 0;
        if (proto_decode_touch(&x, &y, payload, len) != PROTO_OK) {
            status("ignored a malformed tap");
            break;
        }
        if (!ui_settings.view_taps) {
            static unsigned long said_at;
            if (!said_at || frame_count - said_at > 600) {
                said_at = frame_count ? frame_count : 1;
                status("a tap from the computer was ignored: VIEW TAPS is off");
            }
            break;
        }
        inject_x = x;
        inject_y = y;
        inject_frames = INJECT_TAP_FRAMES;
        break;
    }

    case MSG_PHOTO_SAVED: {
        photo_saved_t saved;
        if (proto_decode_photo_saved(&saved, payload, len) != PROTO_OK) {
            status("ignored a malformed photo answer");
            break;
        }
        if (!photo_saving()) {
            // Out of turn -- a picture already let go (the camera closed, or
            // SAVING timed out).  Nothing will name it, so it is not kept.
            if (saved.status == PROTO_SAVED_OK && saved.id && photo_proved()) {
                uint8_t out[PROTO_HEADER_SIZE + 2];
                const size_t n = proto_encode_photo_discard(out, saved.id);
                if (!send_all(out, n)) {
                    status("send failed; connection lost");
                    reconnect();
                    break;
                }
            }
            status("photo answer out of turn; discarded");
            break;
        }
        camera_saved(&saved);
        break;
    }

    case MSG_MIC_READY: {
        int can_stt = 0;
        if (proto_decode_mic_ready(&can_stt, payload, len) != PROTO_OK) {
            status("ignored a malformed mic ready");
            break;
        }
        mic_ready(can_stt);
        quick_set_voice_note(can_stt ? TR_COUNT : TR_QUICK_NOTE_NO_STT);
        if (quick_menu || composer.mode != COMPOSER_CLOSED) overlay_sync();
        break;
    }

    case MSG_VOICE_ACK: {
        uint32_t acked = 0;
        if (proto_decode_voice_ack(&acked, payload, len) != PROTO_OK) {
            // Still owed and still paid: see mic_ack_malformed().
            status("ignored a malformed voice ack");
            mic_ack_malformed();
            break;
        }
        // Never a reconnect: a late ack -- after X, or after the next
        // utterance began -- is well formed and merely late (mic.c).
        mic_ack(acked);
        break;
    }

    case MSG_VOICE_TEXT: {
        // Static: about 490 bytes, too big for the ARM9 stack.  Messages are
        // handled one at a time from the main loop, so nothing re-enters.
        static voice_text_t vt;
        if (proto_decode_voice_text(&vt, payload, len) != PROTO_OK) {
            // The reply the last VOICE_END was owed, unreadable: pay the
            // ledger so the next recording is not refused (mic.h).
            mic_text_malformed();
            overlay_sync();
            break;
        }
        // Only the utterance this card is waiting on gets through, and its
        // words land in the composer to be read -- never on the PTY.
        if (mic_text_arrived(&vt)) {
            if (composer.mode == COMPOSER_CLOSED) {
                if (focus_active()) focus_off();
                composer_open_write();
            }
            // The composer's limit cuts the tail; say so, or the person sends
            // half a sentence believing it whole.
            const size_t landed = composer_append_text(&composer, vt.text,
                                                       vt.text_len);
            if (landed < vt.text_len) {
                status("voice: composer full -- transcript cut, A sends it");
            } else {
                status("voice: transcribed -- A sends it");
            }
        }
        overlay_sync();
        break;
    }

    case MSG_ERROR:
        status("server error: %.*s", (int)len, (const char *)payload);
        break;

    default:
        // A type the extension asked for is its to handle.  Anything else
        // -- a client-to-server type, say -- means the server is confused.
        if (ext_message(type, payload, len)) break;
        status("unexpected message 0x%02X", type);
        break;
    }
}

// ------------------------------------------------------- the crash screen ---

// What the card shows if it ever stops on a CPU exception, in place of
// libnds's "Guru Meditation" dump: what happened in words, the registers a
// bug report needs (addr2line maps pc and lr to source lines against
// build/dsterm-*.elf), and the log lines that led up to it.
//
// It also sidesteps a bug in that dump: for an abort it decodes the
// instruction at the faulting address after asking mpuRegionIsCode() of an
// address rather than of a region, so an abort at a non-code address faults
// again inside the handler, and the screen shows the handler's registers
// instead of the crash's.
//
// It runs in the exception handler with interrupts off: no printf (it needs
// thread-local storage), no allocation, nothing that waits.
static void crash_hex(char *out, uint32_t v)
{
    for (int i = 7; i >= 0; i--) {
        out[i] = "0123456789ABCDEF"[v & 15u];
        v >>= 4;
    }
    out[8] = '\0';
}

static void crash_regs(int row, const char *const names[3], const int index[3])
{
    char line[LOG_COLS + 1] = "";
    char hex[9];
    for (int i = 0; i < 3 && index[i] >= 0; i++) {
        crash_hex(hex, (uint32_t)exceptionRegisters[index[i]]);
        strcat(line, names[i]);
        strcat(line, hex);
        strcat(line, "  ");
    }
    subscreen_draw_line(row, line, PAL_DEFAULT);
}

static void crash_screen(void)
{
    // The BIOS keeps the interrupted mode's status register (SPSR) at the
    // bottom of its exception stack, where libnds's entry code reads it too.
    const uint32_t spsr = *(volatile uint32_t *)0x02FFFD90;
    const uint32_t mode = getCPSR() & 0x1F;
    const uint32_t r15 = (uint32_t)exceptionRegisters[15];
    const int thumb = (spsr & 0x20) != 0;
    char line[LOG_COLS + 1], hex[9];

    for (int row = 0; row < LOG_ROWS; row++) subscreen_draw_line(row, "", PAL_DEFAULT);
    subscreen_draw_line(0, "DS CLAUDE TERMINAL STOPPED", PAL_ORANGE);
    subscreen_draw_line(2, "The app hit a bug and had to stop. Please", PAL_WHITE);
    subscreen_draw_line(3, "photograph this screen for a bug report,", PAL_WHITE);
    subscreen_draw_line(4, "then switch the console off and on.", PAL_WHITE);

    // An abort leaves r15 eight bytes past the instruction; an undefined
    // instruction, one instruction past it.
    const char *what = mode == 0x17 ? "memory fault at " :
                       mode == 0x1B ? "bad instruction at " : "stopped at ";
    const uint32_t at = mode == 0x17 ? r15 - 8 :
                        mode == 0x1B ? r15 - (thumb ? 2u : 4u) : r15;
    crash_hex(hex, at);
    strcpy(line, what);
    strcat(line, hex);
    strcat(line, thumb ? " (thumb)" : " (arm)");
    subscreen_draw_line(6, line, PAL_YELLOW);

    strcpy(line, "lr ");
    crash_hex(hex, (uint32_t)exceptionRegisters[14]);
    strcat(line, hex);
    strcat(line, "  sp ");
    crash_hex(hex, (uint32_t)exceptionRegisters[13]);
    strcat(line, hex);
    strcat(line, "  psr ");
    crash_hex(hex, spsr);
    strcat(line, hex);
    subscreen_draw_line(7, line, PAL_DEFAULT);

    static const char *const n0[3] = {"r0 ", "r1 ", "r2 "};
    static const char *const n1[3] = {"r3 ", "r4 ", "r5 "};
    static const char *const n2[3] = {"r6 ", "r7 ", "r8 "};
    static const char *const n3[3] = {"r9 ", "r10 ", "r11 "};
    static const char *const n4[3] = {"r12 ", "", ""};
    crash_regs(8, n0, (const int[3]){0, 1, 2});
    crash_regs(9, n1, (const int[3]){3, 4, 5});
    crash_regs(10, n2, (const int[3]){6, 7, 8});
    crash_regs(11, n3, (const int[3]){9, 10, 11});
    crash_regs(12, n4, (const int[3]){12, -1, -1});

    subscreen_draw_line(14, "WHAT IT WAS DOING", PAL_ORANGE);
    const int count = log_count();
    const int shown = count < LOG_ROWS - 15 ? count : LOG_ROWS - 15;
    for (int i = 0; i < shown; i++) {
        subscreen_draw_line(15 + i, log_get(count - shown + i), PAL_DEFAULT);
    }

    while (1) {
    }
}

int main(void)
{
    // Before anything else can fault: see crash_screen().
    setExceptionHandler(crash_screen);

    // Before anything can draw it.  A zero-initialised grid is not an empty
    // one: glyph 0 is outside both font ranges, and render falls that back to
    // '?' on purpose so a protocol bug is visible.  Rendering the grid before
    // its first clear therefore paints 1008 question marks over the terminal,
    // which is exactly what the boot screen did -- the extension's boot step,
    // which repaints the grid, runs while loading preferences, and that is
    // long before the session's own clear down at connect time.
    grid_clear(&grid);
    grid_clear(&page_grid);
    project_init(&project);
    clip_init(&clip);
    clip_resync();

    // Power both engines and bring up both screens before the first status()
    // line, because status() now paints the sub screen from the log ring.
    render_init();
    subscreen_init();
    log_init();
    kbd_reset();
    focus_init();
    composer_init(&composer);
    snippets_init(&snippets);
    sessions_init(&sessions);
    remote_init(&remote);
    remote_ui_init(&remote_ui);
    ext_init();
    status_view_pos_clear();
    status_scroll_regime_clear();

    // Hardware channels only, started from sound.c's cues: no mixer, no
    // streaming, and nothing that can block. Backlight is deliberately left
    // alone -- there is no automatic dimming anywhere in this program, because
    // a screen that darkens on its own while someone is reading a long answer
    // is a bug that looks like a hardware fault.
    soundEnable();
    chime_init();

    status("DS Claude Terminal");
    blank_line();

    // No filesystem is the one failure this program cannot work around: the
    // pairing keys live on the card.  Say what usually causes it, which
    // differs by how the console started dsterm.
    if (!fatInitDefault()) {
        halt(isDSiMode()
             ? "cannot read the SD card. Start dsterm from Unlaunch or "
               "TWiLight Menu++ (not a DSiWare exploit)."
             : "no SD filesystem. The flashcart must DLDI-patch dsterm.nds "
               "(most menus do it automatically).");
    }

    ext_boot();

    // UI settings, on the same footing as an extension's: a convenience, never a
    // boot dependency.  A missing file is first boot and a malformed one is a
    // line in the log, not a halt.
    {
        char serr[96] = "";
        // The camera is a DSi thing: QUICK's PHOTO row, the SHUTTER setting
        // and the feature this card's HELLO advertises all hang off this.
        photo_boot(isDSiMode() ? 1 : 0);
        settings_set_camera_available(photo_available());
        subscreen_set_camera_available(photo_available());
        // Until a CAMERA_READY proves otherwise, PHOTO's note is the truth.
        quick_set_photo_note(TR_QUICK_NOTE_NOT_HERE);
        quick_set_voice_note(TR_QUICK_NOTE_NOT_HERE);
        lang_init();
        load_custom_langs();
        settings_ui_init(&ui_settings_ui);
        if (!settings_load(settings_path, &ui_settings, serr, sizeof(serr))) {
            settings_defaults(&ui_settings);
            if (strncmp(serr, "cannot open", 11) != 0) {
                status("settings: %s (using defaults)", serr);
            }
        }
        // Where the card comes up.  Applied before the first paint so the
        // chosen screen is the first one seen, rather than CONTROL flashing
        // past on the way to it.
        switch (ui_settings.boot) {
        case SETTINGS_BOOT_KEYS:
            status_mode_set(SCREEN_MODE_KEYS);
            break;
        case SETTINGS_BOOT_PROJECT:
            status_mode_set(SCREEN_MODE_PROJECT);
            break;
        case SETTINGS_BOOT_CONTROL:
        case SETTINGS_BOOT_COUNT:
        default:
            break;
        }
        settings_apply();
        sound_settings_apply();
        page_repaint();
        // Your own sounds before the first one plays, so a custom boot
        // jingle is the one you hear.
        load_custom_sounds();
        // Hello.  Once per power-on, in whichever instrument was chosen, and
        // not at all with SOUND set to OFF.
        sound_cue(SOUND_BOOT);
    }

    ext_boot_late();

    // QUICK's lists: the built-in commands until a computer adds its own, and
    // the prompts you sent last time.
    quick_commands_init(&commands);
    quick_saved_load();
    recent_load();

    // Snippets are optional. A console without them is not misconfigured, so
    // only a file that exists and does not parse is worth a line -- and that
    // line is a warning, not a halt: a typo in a convenience feature must not
    // stand between the handheld and the terminal it exists to show.
    for (int i = 0; i < SNIPPET_PATH_COUNT; i++) {
        char serr[96] = "";
        if (snippets_load(SNIPPET_PATHS[i], &snippets, serr, sizeof(serr))) {
            status("snippets: %d from %s", snippets.count, SNIPPET_PATHS[i]);
            break;
        }
        if (strncmp(serr, "cannot open", 11) != 0) {
            status("snippets: %s", serr);
            status("  in %s", SNIPPET_PATHS[i]);
            snippets_init(&snippets);
            break;
        }
    }

    // The boot menu owns the screens until a server is chosen: Wi-Fi, finding
    // the server, pairing.  A connection that then fails goes back to it with
    // the reason, rather than halting -- a card that cannot reach its server
    // must still be able to pick another, or a network, without a PC.
    const screen_mode_t boot_mode = status_mode_get();
    static char notice[64];
    const char *why = NULL;
    for (;;) {
        boot_run(&cfg, why);
        status_mode_set(boot_mode);
        status("  %s -> %s:%u", cfg.ssid, cfg.server, cfg.port);
        grid_clear(&grid);
        framereader_init(&reader);
        int up = net_connect();
        if (!up) {
            // The router may have given the computer a new address: ask the
            // network for it by name before sending the person to the menu.
            snprintf(notice, sizeof(notice), "No answer from %s:%u",
                     cfg.server, cfg.port);
            if (boot_find_moved_last(&cfg)) {
                status("trying %s at its new address %s", boot_server_name(),
                       cfg.server);
                up = net_connect();
            }
        }
        if (!up) {
            // notice already says where nothing answered
        } else if (!handshake()) {
            if (sock >= 0) { closesocket(sock); sock = -1; }
            if (refusal[0]) {
                snprintf(notice, sizeof(notice), "%.63s", refusal);
            } else {
                snprintf(notice, sizeof(notice), "%s:%u refused this card",
                         cfg.server, cfg.port);
            }
        } else {
            break;
        }
        why = notice;
    }
    // Hand the top screen back from the menu to the terminal (or the page
    // the card boots to).
    page_repaint();
    overlay_sync();

    link_connected = 1;
    ext_link(1);
    chime_link_changed(1);
    // The full-screen resend that follows is this client's own doing; it must
    // not be read as the session producing output.
    chime_view_sent();

    // Commits a computer's new address, if that is how it was reached: it
    // has just authenticated with this card's key.
    boot_switch_done();
    status("connected to %s", boot_server_name()[0] ? boot_server_name()
                                                     : cfg.server);
    status_link_host(boot_server_name());

    // The Control screen is the default, so paint it before the first frame
    // rather than leaving a second of stale boot log under a live terminal.
    link_refresh();
    refresh_sub();

    static uint8_t chunk[1024];
    unsigned long n_rx = 0, n_msg = 0, n_tx = 0;
    unsigned acc_raw = 0, acc_lib = 0;

    while (1) {
        frame_wait();
        scanKeys();
        frame_count++;
        chime_tick();
        if (rewind_picking && ++rewind_frames >= REWIND_PICK_FRAMES) rewind_done();

        // Both sources, accumulated, so one press is still visible a second
        // later when the line is next drawn.
        acc_raw |= (unsigned)(~REG_KEYINPUT) & KBD_REG_MASK;
        acc_lib |= (unsigned)keysHeld();

        poll_input(&n_tx);
        if (inject_frames > 0) inject_frames--;
        remote_tick();
        if (photo_is_open()) photo_tick(&n_tx);
        mic_tick(&n_tx);
        mirror_tick(&n_tx);

        // Once a second: probe the round trip, re-read battery and signal, and
        // repaint the counters. The probe is sent before link_refresh() reads
        // the result, so what the screen shows is always the previous second's
        // answer -- a reading that is one second old and true, rather than one
        // that is current and invented.
        if (frame_count % 60 == 0) {
            send_ping(&n_tx);
            link_refresh();
            last_rx = n_rx; last_msg = n_msg; last_tx = n_tx;
            last_loops = frame_count; last_raw = acc_raw; last_lib = acc_lib;
            // The page is rebuilt on the same beat as the counters it shows.
            // Per frame it would be a blur and would cost a grid render sixty
            // times a second for facts that change once.
            project_resync();
            if (status_mode_get() == SCREEN_MODE_PROJECT) overlay_sync();
            refresh_sub();
        }

        // Drain the socket rather than taking one read per frame: a screen
        // arrives as a dozen small messages and holding them back a frame each
        // would make a repaint visibly crawl.
        while (1) {
            const int n = try_recv(chunk, sizeof(chunk));
            if (n == 0) break;
            if (n < 0) {
                // Not fatal any more. A restarted server, a Wi-Fi blip or a
                // closed lid all land here, and all of them are recoverable.
                status("server closed (errno=%d)", errno);
                reconnect();
                break;
            }
            n_rx += (unsigned long)n;
            if ((size_t)n > framereader_space(&reader)) {
                // A server that overruns the frame reader is confused, not
                // fatal: start the link again rather than stop the card.
                status("receive buffer overflow; reconnecting");
                reconnect();
                break;
            }
            framereader_feed(&reader, chunk, (size_t)n);
        }

        // Drain every complete message before drawing again, so a burst does
        // not spread across frames and tear.
        while (1) {
            msg_type_t type;
            const uint8_t *payload;
            uint16_t len;
            const proto_result_t r = framereader_next(&reader, &type, &payload, &len);
            if (r == PROTO_NEED_MORE) break;
            if (r != PROTO_OK) {
                // Framing lost: nothing after this point can be trusted to
                // start where a message starts.  A fresh connection is the
                // only resynchronisation there is -- and it costs a second,
                // where halting cost a walk to the console.
                status("protocol error: %s; reconnecting", proto_strerror(r));
                reconnect();
                break;
            }
            if (seal.on) {
                // Before the type or the payload is looked at: a frame that
                // is not the server's own ends the connection, and nothing
                // in it is acted on.
                // The tag is checked before a byte is decrypted, into a
                // buffer of the card's own: the reader's stays as received.
                static uint8_t plain[PROTO_MAX_PAYLOAD];
                const int body = seal_open(&seal, type, payload, len, plain);
                if (body < 0) {
                    status("integrity check failed; reconnecting");
                    reconnect();
                    break;
                }
                payload = plain;
                len = (uint16_t)body;
            }
            n_msg++;
            handle_message(type, payload, len);
        }

        ext_frame();
    }
    return 0;
}
