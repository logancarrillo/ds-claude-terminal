// The boot menu.  See boot.h for what it is for; this file is the libnds and
// dswifi half, and is deliberately thin over the pure modules it drives.
//
// Three rules it keeps, all inherited from the rest of the client:
//
//  * Every wait is host_frame_wait().  The network stack is a cooperative
//    thread; a BIOS wait starves it and every socket below goes silent.
//  * Nothing is drawn every frame.  render_overlay_* clears and repaints the
//    top screen, which is visible as a flash, so screens redraw on change.
//  * Buttons are read from REG_KEYINPUT and libnds together, through
//    kbd_held_keyword(), because libnds alone reported nothing on hardware.

#include "boot.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <dswifi9.h>
#include <nds.h>

#include "bootui.h"
#include "console.h"
#include "discover.h"
#include "entropy.h"
#include "host.h"
#include "kbd.h"
#include "pair.h"
#include "profiles.h"
#include "proto.h"
#include "render.h"
#include "subscreen.h"
#include "x25519.h"

#define FRAMES_PER_SECOND 60
#define COUNTDOWN_FRAMES (3 * FRAMES_PER_SECOND)
#define LIST_VISIBLE (PROTO_ROWS - 4)
#define SERVER_PORT_DEFAULT 8347

// The legacy flat file, tried on the same three drive prefixes as ever: libfat
// mounts a DSi's SD as "sd:" and a flashcart as "fat:", and a bare path uses
// whichever fatInitDefault() made the default.
static const char *LEGACY_PATHS[] = {
    "/dsterm.cfg",
    "sd:/dsterm.cfg",
    "fat:/dsterm.cfg",
};
#define LEGACY_COUNT (int)(sizeof(LEGACY_PATHS) / sizeof(LEGACY_PATHS[0]))

static profiles_t prof;
static int prof_loaded;
static entropy_t pool;

// Wi-Fi: initialised once; associated with `joined` when wifi_joined.
static int wifi_ready;
static int wifi_joined;
static wifi_profile_t joined;
static uint32_t my_ip, my_mask;        // host byte order, valid when joined
static console_t console;              // which DS this is (console.h)

static void console_detect(void)
{
    uint8_t type = 0, board = 0;
    (void)readFirmware(0x1D, &type, 1);
    (void)readFirmware(0x1FD, &board, 1);
    console.dsi_mode = isDSiMode() ? 1 : 0;
    console.family = console_classify(type, board, console.dsi_mode,
                                      isDSiMode() && isHwDebugger());
}

const char *boot_console_name(void)
{
    return console_name(&console);
}

console_family_t boot_console_family(void)
{
    return console.family;
}

static framereader_t boot_reader;

// ---------------------------------------------------------------- input ---

static unsigned prev_held;
static unsigned dpad_held_frames;
static int touch_was_down;
static unsigned touch_frames;
static int touch_key = -1;

// Stir the pool with whatever the person just did.  Cheap: one SHA-256 of a
// few words, and only on frames where something happened.
static void stir(unsigned held, unsigned edge)
{
    // Touch samples are stirred in ui_touch_key(), where they are read.
    if (!edge) return;
    const uint32_t words[4] = {
        (uint32_t)host_frame_count(), (uint32_t)REG_VCOUNT, held, edge,
    };
    entropy_add(&pool, words, sizeof(words));
}

// One frame.  Returns the newly-pressed key word, with a held D-pad
// direction repeating after a short delay so long lists can be scrolled.
static unsigned ui_poll(void)
{
    host_frame_wait();
    scanKeys();
    const unsigned held = kbd_held_keyword((unsigned)(~REG_KEYINPUT), keysHeld());
    unsigned edge = kbd_edge_keyword(held, prev_held);
    prev_held = held;
    stir(held, edge);

    const unsigned dirs = KEY_UP | KEY_DOWN;
    if (held & dirs) {
        dpad_held_frames++;
        if (dpad_held_frames > 20 && (dpad_held_frames - 20) % 5 == 0) {
            edge |= held & dirs;
        }
    } else {
        dpad_held_frames = 0;
    }
    return edge;
}

// A touch on the bottom-screen keyboard: the key id pressed this frame (with
// repeat for keys that repeat), or -1.  The first frame of a contact reads a
// garbage coordinate on this hardware, so the press lands on the second --
// the same rule main.c's poll_touch() keeps.
static int ui_touch_key(void)
{
    if (keysHeld() & KEY_TOUCH) {
        touchPosition pos;
        touchRead(&pos);
        // The raw ADC readings are the best randomness this console has:
        // their low bits are analogue noise under a finger.
        entropy_add(&pool, &pos, sizeof(pos));
        if (!touch_was_down) {
            touch_was_down = 1;
            touch_frames = 0;
            touch_key = -1;
            return -1;
        }
        touch_frames++;
        if (touch_frames == 1) {
            touch_key = kbd_hit(pos.px, pos.py);
            kbd_set_held(touch_key);
            host_refresh_sub();
            return touch_key;
        }
        if (touch_key >= 0 && kbd_repeatable(touch_key) &&
            kbd_repeat_should_fire(touch_frames)) {
            return touch_key;
        }
        return -1;
    }
    if (touch_was_down) {
        touch_was_down = 0;
        kbd_set_held(-1);
        host_refresh_sub();
    }
    touch_key = -1;
    return -1;
}

// ---------------------------------------------------------------- screens ---

static void lower(int keyboard, const char *line0, const char *line1)
{
    subscreen_set_boot(1, keyboard, line0, line1);
    host_refresh_sub();
}

// Draw a list with its own window: render_overlay_list has no scrolling.
static void draw_list(const char *title, char (*labels)[BOOTUI_COLS + 1],
                      const menu_list_t *l, const char *hint)
{
    const char *rows[LIST_VISIBLE];
    unsigned n = 0;
    for (unsigned i = l->top; i < l->count && n < LIST_VISIBLE; i++) {
        rows[n++] = labels[i];
    }
    render_overlay_list(title, rows, (int)n, (int)(l->selected - l->top), 1, hint);
}

static void message(const char *title, const char *body)
{
    render_overlay_text(title, body, "A or B: continue");
    lower(0, title, "A or B to continue");
    for (;;) {
        const unsigned edge = ui_poll();
        if (edge & (KEY_A | KEY_B | KEY_START)) return;
    }
}

static int confirm(const char *title, const char *body)
{
    render_overlay_text(title, body, "A: yes   B: no");
    lower(0, title, "A yes   B no");
    for (;;) {
        const unsigned edge = ui_poll();
        if (edge & KEY_A) return 1;
        if (edge & KEY_B) return 0;
    }
}

// A screen that is working on something: drawn once, then left alone while
// the caller polls.  B is checked by the caller.
static void working(const char *title, const char *body)
{
    render_overlay_text(title, body, "B: stop");
    lower(0, title, "working -- B stops");
}

// Pick from a list.  Returns the index, or -1 for B.  `extra` reports which
// of X/Y ended the pick (with the index), for lists that offer an action.
static int pick(const char *title, char (*labels)[BOOTUI_COLS + 1],
                unsigned count, const char *hint, unsigned *extra)
{
    menu_list_t l = {0};
    menu_list_set(&l, count);
    lower(0, title, hint);
    int dirty = 1;
    if (extra) *extra = 0;
    for (;;) {
        if (dirty) {
            draw_list(title, labels, &l, hint);
            dirty = 0;
        }
        const unsigned edge = ui_poll();
        if (edge & KEY_UP)   { menu_list_move(&l, -1, LIST_VISIBLE); dirty = 1; }
        if (edge & KEY_DOWN) { menu_list_move(&l, 1, LIST_VISIBLE);  dirty = 1; }
        if (edge & KEY_B) return -1;
        if (count == 0) continue;
        if (edge & KEY_A) return (int)l.selected;
        if (extra && (edge & (KBD_KEY_X | KBD_KEY_Y))) {
            *extra = edge & (KBD_KEY_X | KBD_KEY_Y);
            return (int)l.selected;
        }
    }
}

// Type into a field.  Returns 1 when it is complete and accepted, 0 on cancel.
static int edit(const char *title, field_t *f, const char *help)
{
    char shown[BOOTUI_COLS + 1], body[320];
    int dirty = 1;
    // Digits and '.' are on the base layer; ':' for a port is on SHIFT.
    kbd_layer_set(KBD_LAYER_BASE);
    lower(1, field_prompt(f->kind), "A done  B delete  X cancel  touch types");
    const char *problem = "";
    for (;;) {
        if (dirty) {
            field_display(f, shown);
            snprintf(body, sizeof(body), "%s\n\n  %s\n\n%s\n\n%s",
                     field_prompt(f->kind), shown, help ? help : "", problem);
            render_overlay_text(title, body, "A done  B delete  X cancel");
            dirty = 0;
        }
        const unsigned edge = ui_poll();
        int result = FIELD_IGNORED;
        if (edge & KEY_A) result = field_input(f, '\r');
        else if (edge & KEY_B) result = field_input(f, 0x7f);
        else if (edge & KBD_KEY_X) result = FIELD_CANCEL;

        const int id = ui_touch_key();
        if (id >= 0 && result == FIELD_IGNORED) {
            uint8_t bytes[PROTO_MAX_KEY_PAYLOAD];
            const int n = kbd_press(id, bytes, sizeof(bytes));
            // A layer key presses nothing but changes the keyboard: repaint.
            if (n <= 0) host_refresh_sub();
            // Escape sequences (arrows) are not field text.
            if (n > 0 && !(n > 1 && bytes[0] == 0x1b)) {
                for (int i = 0; i < n; i++) {
                    const field_result_t r = field_input(f, bytes[i]);
                    if (r != FIELD_IGNORED) result = r;
                    if (r == FIELD_DONE || r == FIELD_CANCEL) break;
                }
            }
        }

        switch (result) {
        case FIELD_DONE:
            lower(0, "", "");
            return 1;
        case FIELD_CANCEL:
            lower(0, "", "");
            return 0;
        case FIELD_INCOMPLETE:
            problem = "Not complete yet.";
            dirty = 1;
            break;
        case FIELD_CHANGED:
            problem = "";
            dirty = 1;
            break;
        default:
            break;
        }
    }
}

// ----------------------------------------------------------------- Wi-Fi ---

static int wifi_init_once(void)
{
    if (wifi_ready) return 1;
    host_status("wifi: init (DSi mode)");
    if (!Wifi_InitDefault(WIFI_ATTEMPT_DSI_MODE | INIT_ONLY)) {
        host_status("wifi: init failed");
        return 0;
    }
    wifi_ready = 1;
    uint8_t mac[6] = {0};
    Wifi_GetData(WIFIGETDATA_MACADDRESS, sizeof(mac), mac);
    entropy_add(&pool, mac, sizeof(mac));
    host_status("console: %s", console_name(&console));
    if (!isDSiMode()) {
        host_status("wifi: DS mode -- open networks only");
    }
    return 1;
}

static ap_security_t map_security(int s)
{
    switch (s) {
    case AP_SECURITY_OPEN: return AP_OPEN;
    case AP_SECURITY_WEP:  return AP_WEP;
    case AP_SECURITY_WPA:  return AP_WPA;
    default:               return AP_WPA2;
    }
}

// Listen for beacons for a few seconds and return what was heard, tidied.
static unsigned wifi_scan(bootui_ap_t *aps, unsigned max)
{
    if (!wifi_init_once()) return 0;
    if (wifi_joined) {
        // Scanning drops the association; say so rather than surprise.
        Wifi_DisconnectAP();
        wifi_joined = 0;
    }
    Wifi_ScanMode();
    working("Wi-Fi", "Listening for networks...\n\n"
                     "Only 2.4 GHz networks can be seen by this console.");
    for (int f = 0; f < 3 * FRAMES_PER_SECOND; f++) {
        if (ui_poll() & KEY_B) break;
    }
    unsigned n = 0;
    const int found = Wifi_GetNumAP();
    for (int i = 0; i < found && n < max; i++) {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK) continue;
        entropy_add(&pool, &ap, sizeof(ap));
        bootui_ap_t *out = &aps[n++];
        memset(out, 0, sizeof(*out));
        memcpy(out->ssid, ap.ssid, ap.ssid_len < 32 ? ap.ssid_len : 32);
        out->ssid[32] = '\0';
        out->security = map_security(ap.security_type);
        out->compatible = (ap.flags & WFLAG_APDATA_COMPATIBLE) ? 1 : 0;
        out->wpa = (ap.flags & WFLAG_APDATA_WPA) ? 1 : 0;
        out->auth = (uint8_t)ap.auth_type;
        out->dsi_mode = (uint8_t)console.dsi_mode;
        // DSi mode reports dBm through an unsigned byte; DS mode a raw
        // strength.  Either way larger is stronger once read correctly.
        out->rssi = console.dsi_mode ? (int)(int8_t)(uint8_t)ap.rssi : ap.rssi;
    }
    return bootui_aps_tidy(aps, n);
}

static int same_wifi(const wifi_profile_t *a, const wifi_profile_t *b)
{
    return strcmp(a->ssid, b->ssid) == 0 && strcmp(a->wifi_key, b->wifi_key) == 0 &&
           a->dhcp == b->dhcp && (a->dhcp || (strcmp(a->ip, b->ip) == 0 &&
                                              strcmp(a->gateway, b->gateway) == 0 &&
                                              strcmp(a->netmask, b->netmask) == 0));
}

// Join a network.  The sequence follows the hardware-tested probe this client
// was built from:
//
//   - WIFI_ATTEMPT_DSI_MODE (wifi_init_once).  Without it WPA2 silently falls
//     back to the DS-mode stack, which does WEP and open only, and the
//     failure looks exactly like a wrong passphrase.
//   - Never WFC_CONNECT: it reads only the DS legacy slots 1-3, which cannot
//     hold WPA2 credentials, and blocks forever producing no output at all.
//   - Wifi_ConnectSecureAP() takes an access point record, not an SSID
//     string, so the AP has to be located by scanning first.
//   - DHCP is bounded.  dswifi's DHCP was seen to time out against one
//     gateway; eight seconds of trying costs little and says so when it fails.
static int wifi_associate(const wifi_profile_t *w, char *why, size_t why_len)
{
    if (!wifi_init_once()) {
        snprintf(why, why_len, "the Wi-Fi hardware did not start");
        return 0;
    }
    if (wifi_joined && same_wifi(&joined, w)) return 1;
    if (wifi_joined) {
        Wifi_DisconnectAP();
        wifi_joined = 0;
        for (int i = 0; i < 10; i++) host_frame_wait();
    }

    host_status("wifi: scanning for '%s'", w->ssid);
    Wifi_ScanMode();
    Wifi_AccessPoint want, found;
    memset(&want, 0, sizeof(want));
    const size_t ssid_len = strlen(w->ssid);
    memcpy(want.ssid, w->ssid, ssid_len);
    want.ssid_len = (u8)ssid_len;
    int idx = -1;
    for (int f = 0; f < 10 * FRAMES_PER_SECOND && idx < 0; f++) {
        idx = Wifi_FindMatchingAP(1, &want, &found);
        if (idx < 0) host_frame_wait();
    }
    if (idx < 0) {
        // In DSi mode the radio never asks for hidden networks by name, so a
        // hidden one is "not in range" however close it is.
        snprintf(why, why_len, "'%.32s' is not in range.\nIs it 2.4 GHz, with "
                 "802.11b/g allowed?%s", w->ssid,
                 console.dsi_mode ? "\nA hidden network has to be made\n"
                                    "visible for DSi mode." : "");
        return 0;
    }
    // The same verdict the network list shows.  A card moved from a DSi to
    // a DS Lite arrives here with a WPA2 profile, and has to be told what
    // this console can and cannot do rather than shown a timeout.
    {
        bootui_ap_t probe;
        memset(&probe, 0, sizeof(probe));
        probe.security = map_security(found.security_type);
        probe.compatible = (found.flags & WFLAG_APDATA_COMPATIBLE) ? 1 : 0;
        probe.wpa = (found.flags & WFLAG_APDATA_WPA) ? 1 : 0;
        probe.auth = (uint8_t)found.auth_type;
        probe.dsi_mode = (uint8_t)console.dsi_mode;
        const ap_verdict_t v = bootui_ap_verdict(&probe);
        if (v != AP_VERDICT_OK) {
            snprintf(why, why_len, "%s",
                     bootui_verdict_text(v, console_wpa_advice(&console)));
            return 0;
        }
    }
    const int secure = found.security_type == AP_SECURITY_WPA2;
    if (secure && w->wifi_key[0] == '\0') {
        snprintf(why, why_len, "'%.32s' needs a password.", w->ssid);
        return 0;
    }

    if (w->dhcp) {
        Wifi_SetIP(0, 0, 0, 0, 0);
    } else {
        struct in_addr ip, gw, mask;
        inet_aton(w->ip, &ip);
        inet_aton(w->gateway, &gw);
        inet_aton(w->netmask, &mask);
        // DNS is the gateway, unused: everything here connects by address.
        Wifi_SetIP(ip.s_addr, gw.s_addr, mask.s_addr, gw.s_addr, 0);
    }
    Wifi_InternetMode();
    for (int i = 0; i < 10; i++) host_frame_wait();

    host_status("wifi: joining %s (%s)", w->ssid, w->dhcp ? "dhcp" : "static");
    const int rc = secure ? Wifi_ConnectSecureAP(&found, w->wifi_key, strlen(w->wifi_key))
                          : Wifi_ConnectOpenAP(&found);
    if (rc != 0) {
        snprintf(why, why_len, "the radio refused to start joining");
        return 0;
    }

    int last = -1, dhcp_frames = 0;
    for (int f = 0; f < 30 * FRAMES_PER_SECOND; f++) {
        const int st = Wifi_AssocStatus();
        if (st != last) {
            host_status("  %s", ASSOCSTATUS_STRINGS[st]);
            last = st;
        }
        if (st == ASSOCSTATUS_ASSOCIATED) {
            struct in_addr gw, mask, dns1, dns2;
            const struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
            my_ip = ntohl(ip.s_addr);
            my_mask = ntohl(mask.s_addr);
            char text[CFG_ADDR_MAX];
            ipv4_format(my_ip, text);
            host_status("wifi: ok, ip %s", text);
            entropy_add_u32(&pool, my_ip);
            joined = *w;
            wifi_joined = 1;
            return 1;
        }
        if (st == ASSOCSTATUS_CANNOTCONNECT) {
            snprintf(why, why_len, "could not join '%.32s'. Wrong password, or "
                     "WPA3 / mixed mode on the router?", w->ssid);
            Wifi_DisconnectAP();
            return 0;
        }
        if (st == ASSOCSTATUS_ACQUIRINGDHCP && ++dhcp_frames > 8 * FRAMES_PER_SECOND) {
            snprintf(why, why_len, "joined '%.32s' but the router gave no "
                     "address (DHCP). Try a static address.", w->ssid);
            Wifi_DisconnectAP();
            return 0;
        }
        host_frame_wait();
    }
    snprintf(why, why_len, "timed out joining '%.32s'", w->ssid);
    Wifi_DisconnectAP();
    return 0;
}

// The router's likely address from ours: a.b.c.1.
static void guess_gateway(const char *ip, char out[CFG_ADDR_MAX])
{
    uint32_t a;
    if (ipv4_parse(ip, &a)) ipv4_format((a & 0xFFFFFF00u) | 1u, out);
    else snprintf(out, CFG_ADDR_MAX, "192.168.1.1");
}

static int wifi_static(wifi_profile_t *w)
{
    field_t f;
    field_init(&f, FIELD_ADDRESS, w->ip[0] ? w->ip : "192.168.1.60");
    if (!edit("Static address", &f,
              "Pick one OUTSIDE your router's DHCP range,\n"
              "or it may later be given to another device.")) {
        return 0;
    }
    char host[CFG_ADDR_MAX];
    uint16_t unused;
    if (!host_port_parse(f.buf, host, &unused, 0) || strchr(f.buf, ':')) {
        message("Static address", "That is not an address.");
        return 0;
    }
    char gw[CFG_ADDR_MAX];
    guess_gateway(host, gw);
    field_init(&f, FIELD_ADDRESS, w->gateway[0] ? w->gateway : gw);
    if (!edit("Router address", &f, "Usually ends in .1 or .254.")) return 0;
    if (!addr_valid(f.buf)) {
        message("Router address", "That is not an address.");
        return 0;
    }
    w->dhcp = 0;
    strcpy(w->ip, host);
    strcpy(w->gateway, f.buf);
    if (!w->netmask[0]) strcpy(w->netmask, "255.255.255.0");
    return 1;
}

static void save_wifi(const wifi_profile_t *w)
{
    char err[64];
    if (profiles_save_wifi(PROFILES_ROOT, w, err, sizeof(err))) {
        prof.wifi = *w;
        prof.have_wifi = 1;
    } else {
        host_status("wifi: %s", err);
        message("Wi-Fi", "Connected, but the settings could not be saved "
                         "to the SD card. Is it locked or full?");
    }
}

// The wizard.  Returns 1 with Wi-Fi joined and saved, 0 if the person backed
// out.
static int wifi_wizard(void)
{
    static bootui_ap_t aps[BOOTUI_APS_MAX];
    static char labels[BOOTUI_APS_MAX + 2][BOOTUI_COLS + 1];
    for (;;) {
        const unsigned n = wifi_scan(aps, BOOTUI_APS_MAX);
        for (unsigned i = 0; i < n; i++) bootui_ap_row(&aps[i], labels[i]);
        snprintf(labels[n], BOOTUI_COLS + 1, "Scan again");
        snprintf(labels[n + 1], BOOTUI_COLS + 1, "Type a network name");
        if (n == 0) {
            message("Wi-Fi", console_silent_advice(&console));
        }
        const int choice = pick(isDSiMode() ? "Wi-Fi networks"
                                            : "Wi-Fi networks (DS mode: open only)",
                                labels, n + 2, "A pick   B back", NULL);
        if (choice < 0) return 0;
        if ((unsigned)choice == n) continue;

        wifi_profile_t w;
        memset(&w, 0, sizeof(w));
        w.dhcp = 1;
        strcpy(w.netmask, "255.255.255.0");
        // A typed network is assumed to be the kind this console can join:
        // WPA2 in DSi mode, open in DS mode (where WPA2 cannot work anyway).
        ap_security_t sec = console.dsi_mode ? AP_WPA2 : AP_OPEN;
        if ((unsigned)choice == n + 1) {
            field_t f;
            field_init(&f, FIELD_TEXT, NULL);
            if (!edit("Network name", &f, console.dsi_mode
                      ? "Its exact name. In DSi mode the network\n"
                        "must not be hidden."
                      : "For a network that hides its name.")) continue;
            strcpy(w.ssid, f.buf);
        } else {
            strcpy(w.ssid, aps[choice].ssid);
            sec = aps[choice].security;
            const ap_verdict_t v = bootui_ap_verdict(&aps[choice]);
            if (v != AP_VERDICT_OK) {
                message("Wi-Fi", bootui_verdict_text(v, console_wpa_advice(&console)));
                continue;
            }
        }
        // Keep what was saved for this network as the starting point.
        if (prof.have_wifi && strcmp(prof.wifi.ssid, w.ssid) == 0) {
            strcpy(w.wifi_key, prof.wifi.wifi_key);
            w.dhcp = prof.wifi.dhcp;
            strcpy(w.ip, prof.wifi.ip);
            strcpy(w.gateway, prof.wifi.gateway);
            strcpy(w.netmask, prof.wifi.netmask[0] ? prof.wifi.netmask : "255.255.255.0");
        }
        if (sec != AP_OPEN) {
            field_t f;
            field_init(&f, FIELD_PASSPHRASE, w.wifi_key);
            if (!edit(w.ssid, &f, "Shown as you type: retyping it blind\n"
                                  "on this keyboard would be worse.")) {
                continue;
            }
            strcpy(w.wifi_key, f.buf);
        }

        char why[160];
        for (;;) {
            working("Wi-Fi", w.dhcp ? "Joining, and asking the router for an "
                                      "address..." : "Joining...");
            if (wifi_associate(&w, why, sizeof(why))) {
                save_wifi(&w);
                char body[96], ip[CFG_ADDR_MAX];
                ipv4_format(my_ip, ip);
                snprintf(body, sizeof(body), "Connected to %.32s.\n\nThis console "
                         "is %s.", w.ssid, ip);
                message("Wi-Fi", body);
                return 1;
            }
            message("Could not join", why);
            static char opts[3][BOOTUI_COLS + 1];
            snprintf(opts[0], sizeof(opts[0]), "Try again");
            snprintf(opts[1], sizeof(opts[1]), w.dhcp ? "Use a static address"
                                                      : "Change the static address");
            snprintf(opts[2], sizeof(opts[2]), "Back to the network list");
            const int next = pick("Wi-Fi: what next?", opts, 3, "A pick   B back", NULL);
            if (next == 0) continue;
            if (next == 1) {
                (void)wifi_static(&w);   // cancelled or not, try what we have
                continue;
            }
            break;
        }
    }
}

static int ensure_wifi(void)
{
    char why[160];
    if (prof.have_wifi) {
        working("Wi-Fi", "Joining the saved network...");
        if (wifi_associate(&prof.wifi, why, sizeof(why))) return 1;
        message("Wi-Fi", why);
    }
    return wifi_wizard();
}

// ------------------------------------------------------------- the server ---

static void set_nonblocking(int s)
{
    unsigned long on = 1;
    (void)ioctl(s, FIONBIO, &on);
}

// Open a TCP connection and wait for the server's NONCE, which it sends the
// moment it accepts: receiving one is what proves this is a dsterm server.
// Non-blocking throughout, so a host that is not there costs `frames`, not
// lwIP's SYN retries.  Returns the socket with the NONCE consumed, or -1.
static int open_server(uint32_t addr, uint16_t port, int frames, int *cancelled)
{
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    set_nonblocking(s);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(addr);
    (void)connect(s, (struct sockaddr *)&sa, sizeof(sa));

    framereader_init(&boot_reader);
    for (int f = 0; f < frames; f++) {
        uint8_t chunk[256];
        errno = 0;
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n > 0) {
            if ((size_t)n > framereader_space(&boot_reader)) break;
            framereader_feed(&boot_reader, chunk, (size_t)n);
            msg_type_t type;
            const uint8_t *payload;
            uint16_t len;
            const proto_result_t r = framereader_next(&boot_reader, &type, &payload, &len);
            if (r == PROTO_OK && type == MSG_NONCE) return s;
            if (r != PROTO_NEED_MORE) break;
            continue;
        }
        if (n == 0) break;      // closed
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS &&
            errno != ENOTCONN && errno != EALREADY) {
            break;              // refused, unreachable: not a server
        }
        if (ui_poll() & KEY_B) {
            if (cancelled) *cancelled = 1;
            break;
        }
    }
    closesocket(s);
    return -1;
}

static int send_frame(int s, msg_type_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t out[PROTO_HEADER_SIZE + 64];
    if (len > sizeof(out) - PROTO_HEADER_SIZE) return 0;
    const size_t n = proto_encode(out, type, payload, len);
    size_t sent = 0;
    for (int stalls = 0; sent < n && stalls < FRAMES_PER_SECOND; ) {
        const int w = send(s, out + sent, n - sent, 0);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            stalls++;
            host_frame_wait();
            continue;
        }
        return 0;
    }
    return sent == n;
}

// Wait for one of the pairing replies, skipping anything else.  With
// `cancellable`, B gives up (returns 0) -- for the wait on a person.
static int await_pair(int s, msg_type_t *type, const uint8_t **payload,
                      uint16_t *len, int frames, int cancellable)
{
    for (int f = 0; f < frames; f++) {
        if (cancellable && (ui_poll() & KEY_B)) return 0;
        const proto_result_t r = framereader_next(&boot_reader, type, payload, len);
        if (r == PROTO_OK) {
            if (*type == MSG_PAIR_REPLY || *type == MSG_PAIR_OK ||
                *type == MSG_PAIR_DONE || *type == MSG_PAIR_FAIL ||
                *type == MSG_ERROR) {
                return 1;
            }
            continue;
        }
        if (r != PROTO_NEED_MORE) return 0;
        uint8_t chunk[256];
        errno = 0;
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n > 0) {
            if ((size_t)n > framereader_space(&boot_reader)) return 0;
            framereader_feed(&boot_reader, chunk, (size_t)n);
            continue;
        }
        if (n == 0) return 0;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return 0;
        host_frame_wait();
    }
    return 0;
}

static void fail_reason(const uint8_t *payload, uint16_t len, char *why, size_t n)
{
    char text[65];
    size_t k = 0;
    for (uint16_t i = 0; i < len && k < sizeof(text) - 1; i++) {
        const uint8_t c = payload[i];
        text[k++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    text[k] = '\0';
    snprintf(why, n, "The server said: %s", text);
}

static void ensure_device_id(void)
{
    if (device_id_valid(prof.device_id)) return;
    uint8_t r[ENTROPY_SIZE];
    entropy_add_u32(&pool, (uint32_t)time(NULL));
    entropy_take(&pool, r);
    device_id_from_bytes(r, prof.device_id);
    if (!profiles_save_device_id(PROFILES_ROOT, prof.device_id)) {
        host_status("id: could not save %s", prof.device_id);
    }
}

void boot_connect_nonce(uint8_t *out)
{
    // Autoconnect reaches this with no button pressed, so the pool may hold
    // only what the boot itself put there: the clock, the MAC, the Wi-Fi
    // scan and the timing of the join.  That is enough, because the nonce
    // only has to be new.  The count separates connections within a boot;
    // between boots the clock moves on, and even on a console whose clock
    // battery has died (it starts at the same time every power-on) the frame
    // and scanline at which the join completed, and the signal strengths in
    // the scan, do not repeat.
    static uint32_t connects;
    entropy_add_u32(&pool, ++connects);
    entropy_add_u32(&pool, (uint32_t)time(NULL));
    entropy_add_u32(&pool, (uint32_t)host_frame_count());
    entropy_add_u32(&pool, (uint32_t)REG_VCOUNT);
    uint8_t r[ENTROPY_SIZE];
    entropy_take(&pool, r);
    memcpy(out, r, PROTO_CARD_NONCE_BYTES);
    memset(r, 0, sizeof(r));
}

// The whole card side of pairing.  Returns 1 with the device key.
static int pair_with(uint32_t addr, uint16_t port, const char *name,
                     uint8_t key[PAIR_KEY_SIZE], char *why, size_t why_len)
{
    // Every failure below says why; this is for one that does not.
    snprintf(why, why_len, "The connection to the computer was lost. "
             "Nothing was saved.");
    ensure_device_id();
    uint8_t id[PROTO_DEVICE_ID_BYTES];
    device_id_to_bytes(prof.device_id, id);

    working("Pairing", "Making a key...");
    // Everything the person has done since power-on is in the pool by now:
    // every button and every touch.
    entropy_add_u32(&pool, (uint32_t)host_frame_count());
    entropy_add_u32(&pool, (uint32_t)time(NULL));
    uint8_t priv[X25519_SIZE], pub[X25519_SIZE], fresh[ENTROPY_SIZE];
    entropy_take(&pool, priv);
    x25519_public(pub, priv);
    uint8_t card_nonce[PAIR_NONCE_SIZE];
    entropy_take(&pool, fresh);
    memcpy(card_nonce, fresh, PAIR_NONCE_SIZE);
    memset(fresh, 0, sizeof(fresh));

    working("Pairing", "Connecting...");
    int cancelled = 0;
    const int s = open_server(addr, port, 5 * FRAMES_PER_SECOND, &cancelled);
    if (s < 0) {
        memset(priv, 0, sizeof(priv));
        snprintf(why, why_len, cancelled ? "Stopped." :
                 "No dsterm server answered at that address.");
        return 0;
    }

    uint8_t hello[PAIR_HELLO_SIZE];
    memcpy(hello, id, PROTO_DEVICE_ID_BYTES);
    memcpy(hello + PROTO_DEVICE_ID_BYTES, pub, X25519_SIZE);
    int ok = send_frame(s, MSG_PAIR_HELLO, hello, sizeof(hello));

    msg_type_t type;
    const uint8_t *payload;
    uint16_t len;
    uint8_t server_pub[X25519_SIZE], commit[PAIR_COMMIT_SIZE];
    uint8_t confirm_key[PAIR_CONFIRM_KEY_SIZE];
    if (ok) ok = await_pair(s, &type, &payload, &len, 10 * FRAMES_PER_SECOND, 0);
    if (!ok) {
        snprintf(why, why_len, "The server closed the connection. It may be "
                 "too old to pair: update dsterm there, or type the key instead.");
    } else if (type != MSG_PAIR_REPLY) {
        fail_reason(payload, len, why, why_len);
        ok = 0;
    } else if (len != PAIR_REPLY_SIZE) {
        snprintf(why, why_len, "That server pairs the old way: update dsterm "
                 "there, then try again.");
        ok = 0;
    } else {
        memcpy(server_pub, payload, X25519_SIZE);
        memcpy(commit, payload + X25519_SIZE, PAIR_COMMIT_SIZE);
        ok = send_frame(s, MSG_PAIR_CONFIRM, card_nonce, sizeof(card_nonce));
    }

    // The server's nonce, which must open what it committed to before it
    // saw ours: that order is what keeps anyone in the middle from steering
    // the code.
    uint8_t server_nonce[PAIR_NONCE_SIZE];
    if (ok && !await_pair(s, &type, &payload, &len, 10 * FRAMES_PER_SECOND, 0)) {
        snprintf(why, why_len, "The server stopped answering.");
        ok = 0;
    } else if (ok && type != MSG_PAIR_OK) {
        fail_reason(payload, len, why, why_len);
        ok = 0;
    } else if (ok && !pair_commitment_kept(pub, server_pub, payload, len, commit)) {
        snprintf(why, why_len, "The computer did not keep its word in the "
                 "exchange, which is what an attacker would do. Nothing was saved.");
        ok = 0;
    } else if (ok) {
        memcpy(server_nonce, payload, PAIR_NONCE_SIZE);
        uint8_t shared[X25519_SIZE];
        x25519(shared, priv, server_pub);
        ok = pair_derive(shared, card_nonce, server_nonce, id, pub, server_pub,
                         confirm_key, key);
        memset(shared, 0, sizeof(shared));
        if (!ok) snprintf(why, why_len, "The server's key was unusable.");
    }
    memset(priv, 0, sizeof(priv));

    if (ok) {
        // The comparison: the whole point.  Both screens show a code, and
        // only the same code on both means nobody is in between.
        const uint32_t code = pair_code(pub, server_pub, card_nonce, server_nonce);
        char body[200];
        snprintf(body, sizeof(body),
                 "This DS shows:   %03u %03u\n\n"
                 "Does the computer show the same\n"
                 "code? (%.24s)\n\n"
                 "If its code is different, press B:\n"
                 "someone else may be trying to pair.",
                 (unsigned)(code / 1000u), (unsigned)(code % 1000u), name);
        if (!confirm("Compare the codes", body)) {
            snprintf(why, why_len, "The codes did not match, so nothing was "
                     "saved. Run dsterm pair again to retry.");
            ok = 0;
        }
    }
    if (ok) {
        uint8_t mac[PAIR_MAC_SIZE];
        pair_accept_mac(confirm_key, PAIR_ROLE_CARD, pub, server_pub, mac);
        ok = send_frame(s, MSG_PAIR_ACCEPT, mac, sizeof(mac));
    }
    if (ok) {
        working("Pairing", "Now answer y on the computer.\n\n"
                           "(B stops.)");
        // A person at the computer: as long as its window lasts.
        if (!await_pair(s, &type, &payload, &len, 120 * FRAMES_PER_SECOND, 1)) {
            snprintf(why, why_len, "The computer did not confirm (no answer, "
                     "or it said no). Nothing was saved.");
            ok = 0;
        } else if (type != MSG_PAIR_DONE) {
            fail_reason(payload, len, why, why_len);
            ok = 0;
        } else if (!pair_server_done(confirm_key, pub, server_pub, payload, len)) {
            snprintf(why, why_len, "The computer's confirmation did not check "
                     "out. Nothing was saved.");
            ok = 0;
        }
    }
    memset(confirm_key, 0, sizeof(confirm_key));
    closesocket(s);
    return ok;
}

// A name for a new profile that does not collide with a different saved
// server: "desk", then "desk-2", ...
static void unique_name(const char *want, const char *host, uint16_t port,
                        char out[PROFILE_NAME_MAX])
{
    char base[PROFILE_NAME_MAX];
    profile_name_from(want, base);
    snprintf(out, PROFILE_NAME_MAX, "%s", base);
    for (int n = 2; n < 100; n++) {
        const int i = profiles_find(&prof, out);
        if (i < 0) return;
        const server_profile_t *s = &prof.servers[i];
        if (strcmp(s->host, host) == 0 && s->port == port) return;  // same one
        char suffix[8];
        snprintf(suffix, sizeof(suffix), "-%d", n);
        const size_t room = PROFILE_NAME_MAX - 1 - strlen(suffix);
        snprintf(out, PROFILE_NAME_MAX, "%.*s%s", (int)room, base, suffix);
    }
}

static int save_server(server_profile_t *s)
{
    char err[64];
    if (!profiles_save_server(PROFILES_ROOT, s, err, sizeof(err))) {
        host_status("profile: %s", err);
        message("Saving", "Paired, but the SD card would not take the "
                          "profile. Is it locked or full?");
        return 0;
    }
    profiles_load(PROFILES_ROOT, &prof);
    return 1;
}

// Pair with a server by comparing codes, or take its key typed by hand.  On success the
// profile is saved and its index returned; -1 otherwise.
static int add_server(const char *name, const char *host, uint16_t port,
                      int pairing_open)
{
    static char opts[2][BOOTUI_COLS + 1];
    snprintf(opts[0], sizeof(opts[0]), "Pair (run: dsterm pair on it)");
    snprintf(opts[1], sizeof(opts[1]), "Type the 32-character key instead");
    char title[BOOTUI_COLS + 1];
    snprintf(title, sizeof(title), "New server: %.20s", name);
    const int how = pick(title, opts, 2, "A pick   B back", NULL);
    if (how < 0) return -1;

    uint32_t addr;
    if (!ipv4_parse(host, &addr)) return -1;
    server_profile_t s;
    memset(&s, 0, sizeof(s));
    unique_name(name, host, port, s.name);
    strcpy(s.host, host);
    s.port = port;

    if (how == 0) {
        if (!pairing_open &&
            !confirm(title, "On that computer, run:\n\n    dsterm pair\n\n"
                            "then press A here. Both screens will\n"
                            "show a code to compare.")) {
            return -1;
        }
        char why[200];
        if (pair_with(addr, port, s.name, s.psk, why, sizeof(why))) {
            if (!save_server(&s)) return -1;
            message("Paired", "This console now has its own key for "
                              "that computer. Nothing secret was sent.");
            return profiles_find(&prof, s.name);
        }
        message("Not paired", why);
        return -1;
    }

    field_t f;
    field_init(&f, FIELD_KEY, NULL);
    if (!edit(title, &f, "On the computer: the 32 hex digits in\n"
                         "~/.config/ds-claude-terminal/psk")) {
        return -1;
    }
    char err[64];
    if (!config_parse_psk(f.buf, s.psk, err, sizeof(err))) {
        message("Key", err);
        return -1;
    }
    if (!save_server(&s)) return -1;
    return profiles_find(&prof, s.name);
}

// ------------------------------------------------------------- discovery ---

static void find_by_broadcast(discover_list_t *list)
{
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        host_status("find: no UDP socket (errno %d)", errno);
        return;
    }
    const int yes = 1;
    (void)setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    set_nonblocking(s);

    char probe[DISCOVER_DATAGRAM_MAX];
    const int plen = discover_probe(probe, sizeof(probe), prof.device_id);
    struct sockaddr_in to[2];
    memset(to, 0, sizeof(to));
    to[0].sin_family = to[1].sin_family = AF_INET;
    to[0].sin_port = to[1].sin_port = htons(DISCOVER_PORT);
    to[0].sin_addr.s_addr = htonl(0xFFFFFFFFu);
    // The subnet's own broadcast too: some stacks and routers treat the
    // limited broadcast differently.
    to[1].sin_addr.s_addr = htonl(my_ip | ~my_mask);

    char body[96];
    for (int f = 0; f < 3 * FRAMES_PER_SECOND; f++) {
        if (f % 20 == 0 && plen > 0) {
            for (int i = 0; i < 2; i++) {
                (void)sendto(s, probe, (size_t)plen, 0,
                             (struct sockaddr *)&to[i], sizeof(to[i]));
            }
        }
        for (;;) {
            uint8_t buf[DISCOVER_DATAGRAM_MAX + 1];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            const int n = recvfrom(s, buf, sizeof(buf), 0,
                                   (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            discover_entry_t e;
            if (discover_parse_answer(buf, (size_t)n, &e)) {
                ipv4_format(ntohl(from.sin_addr.s_addr), e.host);
                discover_list_add(list, &e);
            }
        }
        if (f % 30 == 0) {
            snprintf(body, sizeof(body), "Asking the network...\n\n%u found",
                     list->count);
            working("Find servers", body);
        }
        if (ui_poll() & KEY_B) break;
    }
    closesocket(s);
}

#define SWEEP_SLOTS 6
#define SWEEP_FRAMES 30         // half a second per address

static void find_by_sweep(discover_list_t *list, uint16_t port)
{
    discover_sweep_t sweep;
    if (!discover_sweep_init(&sweep, my_ip, my_mask)) return;
    struct { int sock; uint32_t addr; int age; } slot[SWEEP_SLOTS];
    for (int i = 0; i < SWEEP_SLOTS; i++) slot[i].sock = -1;

    char body[160], net[CFG_ADDR_MAX];
    ipv4_format(my_ip & 0xFFFFFF00u, net);
    int finished = 0, stopped = 0;
    while (!finished && !stopped) {
        finished = 1;
        for (int i = 0; i < SWEEP_SLOTS; i++) {
            if (slot[i].sock < 0) {
                if (sweep.next == 0 || sweep.next > sweep.last) continue;
                // The socket first: if lwIP is out of them this frame, the
                // address is simply tried on a later one.
                const int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s < 0) { finished = 0; continue; }
                uint32_t a;
                if (!discover_sweep_next(&sweep, &a)) { closesocket(s); continue; }
                set_nonblocking(s);
                struct sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port = htons(port);
                sa.sin_addr.s_addr = htonl(a);
                (void)connect(s, (struct sockaddr *)&sa, sizeof(sa));
                slot[i].sock = s;
                slot[i].addr = a;
                slot[i].age = 0;
            }
            finished = 0;
            uint8_t chunk[64];
            errno = 0;
            const int n = recv(slot[i].sock, chunk, sizeof(chunk), 0);
            int done = 0;
            if (n >= PROTO_HEADER_SIZE && chunk[0] == PROTO_MAGIC0 &&
                chunk[1] == PROTO_MAGIC1 && chunk[3] == MSG_NONCE) {
                discover_entry_t e;
                memset(&e, 0, sizeof(e));
                ipv4_format(slot[i].addr, e.host);
                snprintf(e.name, sizeof(e.name), "%s", e.host);
                e.port = port;
                e.swept = 1;
                discover_list_add(list, &e);
                done = 1;
            } else if (n >= 0) {
                done = n > 0 || errno == 0;       // data that is not ours, or closed
            } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINPROGRESS && errno != ENOTCONN &&
                       errno != EALREADY) {
                done = 1;                          // refused or unreachable
            }
            if (done || ++slot[i].age > SWEEP_FRAMES) {
                closesocket(slot[i].sock);
                slot[i].sock = -1;
            }
        }
        if ((host_frame_count() % 15) == 0) {
            snprintf(body, sizeof(body), "Trying every address on %s/24\n"
                     "for a server on port %u.\n\n%u of %u   found %u",
                     net, (unsigned)port, sweep.done, sweep.total, list->count);
            working("Scan this network", body);
        }
        if (ui_poll() & KEY_B) stopped = 1;
    }
    for (int i = 0; i < SWEEP_SLOTS; i++) {
        if (slot[i].sock >= 0) closesocket(slot[i].sock);
    }
}

// --------------------------------------------------------------- the menu ---

static void remember_last(const char *name)
{
    if (strcmp(prof.last, name) != 0) {
        snprintf(prof.last, sizeof(prof.last), "%s", name);
        (void)profiles_save_last(PROFILES_ROOT, name);
    }
}

// Ready `cfg` for profile `index`.  `remember` makes it the server the card
// counts down to at the next power-on; a REMOTE switch passes 0 and commits
// it only once the new server has actually answered (boot_switch_done()), so
// a computer that could not be reached does not become the boot target.
static int resolve_profile(int index, dsterm_config_t *cfg, int remember)
{
    if (index < 0 || (unsigned)index >= prof.count) return 0;
    if (!ensure_wifi()) return 0;
    const server_profile_t *s = &prof.servers[index];
    ensure_device_id();
    // From what is actually joined, which the wizard may have just changed.
    profiles_resolve(&joined, s, prof.device_id, cfg);
    if (remember) remember_last(s->name);
    return 1;
}

static int connect_profile(int index, dsterm_config_t *cfg)
{
    return resolve_profile(index, cfg, 1);
}

// A saved computer being reached at a new address, saved only once it has
// answered: the name that led here is a hint, the card's key is the proof.
static int moved_index = -1;
static char moved_host[CFG_ADDR_MAX];

static void moved_forget(void)
{
    moved_index = -1;
    moved_host[0] = '\0';
}

// Ready `cfg` for saved profile `index` at `host` instead of where it is saved.
static int resolve_moved(int index, const char *host, dsterm_config_t *cfg,
                         int remember)
{
    if (!resolve_profile(index, cfg, remember)) return 0;
    snprintf(cfg->server, sizeof(cfg->server), "%s", host);
    moved_index = index;
    snprintf(moved_host, sizeof(moved_host), "%s", host);
    return 1;
}

static int menu_find(dsterm_config_t *cfg)
{
    if (!ensure_wifi()) return 0;
    static discover_list_t list;
    discover_list_init(&list);
    find_by_broadcast(&list);
    static char labels[DISCOVER_MAX + 1][BOOTUI_COLS + 1];
    for (;;) {
        for (unsigned i = 0; i < list.count; i++) {
            const discover_entry_t *e = &list.entries[i];
            const char *note =
                profiles_find_host(&prof, e->host, e->port) >= 0 ? "saved" :
                profiles_find_moved(&prof, e->name, e->host, e->port) >= 0 ? "moved" :
                e->pairing ? "pair" : "";
            bootui_server_row(e->name, e->host, e->port, note, labels[i]);
        }
        snprintf(labels[list.count], BOOTUI_COLS + 1, "Scan this network (slow)");
        const int choice = pick(list.count ? "Servers found" : "No servers answered",
                                labels, list.count + 1, "A pick   B back", NULL);
        if (choice < 0) return 0;
        if ((unsigned)choice == list.count) {
            find_by_sweep(&list, SERVER_PORT_DEFAULT);
            continue;
        }
        const discover_entry_t *e = &list.entries[choice];
        int index = profiles_find_host(&prof, e->host, e->port);
        // Saved under its old address: the same key, the new address, saved
        // once it has answered (boot_switch_done() after the handshake).
        const int moved = index < 0 ? profiles_find_moved(&prof, e->name, e->host,
                                                          e->port) : -1;
        if (moved >= 0 && resolve_moved(moved, e->host, cfg, 1)) return 1;
        if (index < 0) index = add_server(e->name, e->host, e->port, e->pairing);
        if (index >= 0 && connect_profile(index, cfg)) return 1;
    }
}

static int menu_address(dsterm_config_t *cfg)
{
    field_t f;
    field_init(&f, FIELD_ADDRESS, NULL);
    if (!edit("Enter an address", &f, "The computer running the server.\n"
                                      "Add :port if it is not 8347.")) {
        return 0;
    }
    char host[CFG_ADDR_MAX];
    uint16_t port;
    if (!host_port_parse(f.buf, host, &port, SERVER_PORT_DEFAULT)) return 0;
    if (!ensure_wifi()) return 0;
    int index = profiles_find_host(&prof, host, port);
    if (index < 0) index = add_server(host, host, port, 0);
    return index >= 0 && connect_profile(index, cfg);
}

static int menu_saved(dsterm_config_t *cfg)
{
    static char labels[PROFILES_MAX][BOOTUI_COLS + 1];
    for (;;) {
        for (unsigned i = 0; i < prof.count; i++) {
            const server_profile_t *s = &prof.servers[i];
            bootui_server_row(s->name, s->host, s->port,
                              strcmp(s->name, prof.last) == 0 ? "last" : "", labels[i]);
        }
        unsigned extra = 0;
        const int choice = pick(prof.count ? "Saved servers" : "No saved servers",
                                labels, prof.count, "A connect  X forget  B back", &extra);
        if (choice < 0) return 0;
        if (extra & KBD_KEY_X) {
            char body[96];
            snprintf(body, sizeof(body), "Forget %s?\n\nIts key is deleted from "
                     "this card. Pair again to reconnect.", prof.servers[choice].name);
            if (confirm("Forget", body)) {
                (void)profiles_forget_server(PROFILES_ROOT, prof.servers[choice].name);
                profiles_load(PROFILES_ROOT, &prof);
            }
            continue;
        }
        return connect_profile(choice, cfg);
    }
}

// Migrate /dsterm.cfg into /dsterm/ on a card that has never had the new
// layout.  The old file is left exactly where it was.
static void migrate_legacy(char *notice, size_t n)
{
    if (prof.have_wifi || prof.count) return;
    for (int i = 0; i < LEGACY_COUNT; i++) {
        dsterm_config_t legacy;
        char err[128] = "";
        if (config_load(LEGACY_PATHS[i], &legacy, err, sizeof(err))) {
            wifi_profile_t w;
            server_profile_t s;
            profiles_from_legacy(&legacy, &w, &s);
            char e1[64] = "", e2[64] = "";
            if (profiles_save_wifi(PROFILES_ROOT, &w, e1, sizeof(e1)) &&
                profiles_save_server(PROFILES_ROOT, &s, e2, sizeof(e2)) &&
                profiles_save_last(PROFILES_ROOT, s.name)) {
                host_status("config: moved %s into %s/", LEGACY_PATHS[i], PROFILES_ROOT);
            } else {
                host_status("config: could not write %s/ (%s%s)", PROFILES_ROOT, e1, e2);
                // Still usable this boot, from memory.
                prof.wifi = w;
                prof.have_wifi = 1;
                prof.servers[0] = s;
                prof.count = 1;
                snprintf(prof.last, sizeof(prof.last), "%s", s.name);
                return;
            }
            profiles_load(PROFILES_ROOT, &prof);
            return;
        }
        if (strncmp(err, "cannot open", 11) != 0) {
            snprintf(notice, n, "dsterm.cfg: %.60s (ignored)", err);
            return;
        }
    }
}

static int autoconnect = 1;

void boot_set_autoconnect(int on)
{
    autoconnect = on ? 1 : 0;
}

int boot_run(dsterm_config_t *cfg, const char *notice)
{
    // A new address waiting on an answer belongs to the attempt that failed.
    moved_forget();
    static char notice_buf[96];
    if (!prof_loaded) {
        console_detect();
        entropy_init(&pool);
        entropy_add_u32(&pool, (uint32_t)time(NULL));
        profiles_load(PROFILES_ROOT, &prof);
        notice_buf[0] = '\0';
        migrate_legacy(notice_buf, sizeof(notice_buf));
        if (prof.skipped) {
            host_status("profiles: %u file(s) in %s/servers could not be read",
                        prof.skipped, PROFILES_ROOT);
        }
        prof_loaded = 1;
        wifi_init_once();
        if (!notice && notice_buf[0]) notice = notice_buf;
    }

    countdown_t cd;
    countdown_start(&cd, notice || !autoconnect ? 0 : COUNTDOWN_FRAMES);
    menu_list_t l = {0};
    boot_row_t rows[BOOT_MAIN_ROWS];
    char labels[BOOT_MAIN_ROWS][BOOTUI_COLS + 1];
    unsigned shown_seconds = 99;
    int dirty = 1;
    char title[BOOTUI_COLS + 1];

    for (;;) {
        const unsigned secs = countdown_seconds(&cd);
        const unsigned count = bootui_main_rows(&prof, secs, rows, labels);
        if (count == 0 || rows[0] != BOOT_ROW_CONNECT_LAST) countdown_cancel(&cd);
        menu_list_set(&l, count);
        if (dirty || secs != shown_seconds) {
            snprintf(title, sizeof(title), "%s",
                     notice ? notice : "DS CLAUDE TERMINAL");
            draw_list(title, labels, &l, prof.count
                      ? "A select   START connect"
                      : "A select");
            // A card that has never met a server gets the three steps that
            // get it there, where the log would otherwise be.  Everybody
            // else gets the log: it is what explains a failed connection.
            subscreen_set_boot_body(prof.count || notice ? NULL
                                                     : BOOTUI_FIRST_BOOT_GUIDE);
            lower(0, notice ? "Could not connect -- see the log below"
                            : "DS Claude Terminal",
                  prof.count ? "up/down choose   A select"
                             : "Welcome! Start with step 1 below.");
            shown_seconds = secs;
            dirty = 0;
        }

        const unsigned edge = ui_poll();
        const int touched = (keysHeld() & KEY_TOUCH) != 0;
        if (edge || touched) {
            if (cd.active) {
                countdown_cancel(&cd);
                dirty = 1;
            }
        }
        int act = -1;
        if (countdown_tick(&cd)) act = 0;
        if (edge & KEY_UP)   { menu_list_move(&l, -1, LIST_VISIBLE); dirty = 1; }
        if (edge & KEY_DOWN) { menu_list_move(&l, 1, LIST_VISIBLE);  dirty = 1; }
        if (edge & KEY_A) act = (int)l.selected;
        if ((edge & KEY_START) && count && rows[0] == BOOT_ROW_CONNECT_LAST) act = 0;
        if (act < 0) continue;

        int done = 0;
        // The guide belongs to this menu; the screens below show progress.
        subscreen_set_boot_body(NULL);
        switch (rows[act]) {
        case BOOT_ROW_CONNECT_LAST:
            done = connect_profile(profiles_find(&prof, prof.last), cfg);
            break;
        case BOOT_ROW_FIND:
            done = menu_find(cfg);
            break;
        case BOOT_ROW_ADDRESS:
            done = menu_address(cfg);
            break;
        case BOOT_ROW_SAVED:
            done = menu_saved(cfg);
            break;
        case BOOT_ROW_WIFI:
            (void)wifi_wizard();
            break;
        }
        if (done) {
            subscreen_set_boot_body(NULL);
            subscreen_set_boot(0, 0, NULL, NULL);
            host_refresh_sub();
            return 1;
        }
        notice = NULL;
        dirty = 1;
    }
}

// ----------------------------------------------------- REMOTE: computers ---
//
// While the REMOTE picker is open the card listens for other servers the way
// "Find servers" does, but without taking the screen: one UDP socket, a probe
// a second, answers drained each frame.  The list it feeds is the saved
// servers (paired, so one press away) merged with whatever answered.

static int peer_sock = -1;
static discover_list_t peers;
static unsigned peer_frames;

static void peers_probe(void)
{
    if (peer_sock < 0) return;
    char probe[DISCOVER_DATAGRAM_MAX];
    const int plen = discover_probe(probe, sizeof(probe), prof.device_id);
    if (plen <= 0) return;
    struct sockaddr_in to[2];
    memset(to, 0, sizeof(to));
    to[0].sin_family = to[1].sin_family = AF_INET;
    to[0].sin_port = to[1].sin_port = htons(DISCOVER_PORT);
    to[0].sin_addr.s_addr = htonl(0xFFFFFFFFu);
    to[1].sin_addr.s_addr = htonl(my_ip | ~my_mask);
    for (int i = 0; i < 2; i++) {
        (void)sendto(peer_sock, probe, (size_t)plen, 0,
                     (struct sockaddr *)&to[i], sizeof(to[i]));
    }
}

void boot_peers_open(void)
{
    discover_list_init(&peers);
    peer_frames = 0;
    if (peer_sock >= 0 || !wifi_joined) return;
    peer_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (peer_sock < 0) return;
    const int yes = 1;
    (void)setsockopt(peer_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    set_nonblocking(peer_sock);
    peers_probe();
}

void boot_peers_poll(void)
{
    if (peer_sock < 0) return;
    // A probe on opening (boot_peers_open), one a second later for any
    // answer the first missed, then one every five seconds: a computer that
    // starts while the picker is up still appears, without a broadcast every
    // second for as long as the picker stays open.
    ++peer_frames;
    if (peer_frames == FRAMES_PER_SECOND ||
        peer_frames % (5 * FRAMES_PER_SECOND) == 0) {
        peers_probe();
    }
    for (int budget = 0; budget < 8; budget++) {
        uint8_t buf[DISCOVER_DATAGRAM_MAX + 1];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(peer_sock, buf, sizeof(buf), 0,
                               (struct sockaddr *)&from, &fl);
        if (n <= 0) break;
        discover_entry_t e;
        if (discover_parse_answer(buf, (size_t)n, &e)) {
            ipv4_format(ntohl(from.sin_addr.s_addr), e.host);
            discover_list_add(&peers, &e);
        }
    }
}

void boot_peers_close(void)
{
    if (peer_sock >= 0) closesocket(peer_sock);
    peer_sock = -1;
}

// The name a computer announces for itself, if one at `host`:`port` has
// answered discovery since the picker opened; NULL otherwise.
static const char *announced_name(const char *host, uint16_t port)
{
    for (unsigned k = 0; k < peers.count; k++) {
        if (strcmp(peers.entries[k].host, host) == 0 &&
            peers.entries[k].port == port && peers.entries[k].name[0]) {
            return peers.entries[k].name;
        }
    }
    return NULL;
}

const char *boot_server_name(void)
{
    // During a switch that has not answered yet, the name is still the one
    // being left -- the header must not claim the card is somewhere it is not.
    // A card that knows its computer only by address (typed in, or carried
    // over from the old single file) shows the name the computer announces.
    if (profile_name_is_address(prof.last)) {
        const int i = profiles_find(&prof, prof.last);
        if (i >= 0) {
            const char *n = announced_name(prof.servers[i].host, prof.servers[i].port);
            if (n) return n;
        }
    }
    return prof.last;
}

static void computer_from_profile(const server_profile_t *s,
                                  remote_computer_t *m)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", s->name);
    snprintf(m->host, sizeof(m->host), "%s", s->host);
    m->port = s->port;
    m->saved = 1;
}

void boot_peers_list(remote_computers_t *out, const char *host, uint16_t port)
{
    memset(out, 0, sizeof(*out));
    // The one you are on first, then the paired ones that answered, then the
    // paired ones that did not, then strangers -- the order of how likely a
    // press is to work.
    for (int pass = 0; pass < 4 && out->count < REMOTE_COMPUTERS_MAX; pass++) {
        for (unsigned i = 0; i < prof.count &&
                        out->count < REMOTE_COMPUTERS_MAX; i++) {
            const server_profile_t *s = &prof.servers[i];
            const int current = strcmp(s->host, host) == 0 && s->port == port;
            int online = 0, pairing = 0;
            for (unsigned k = 0; k < peers.count; k++) {
                if (strcmp(peers.entries[k].host, s->host) == 0 &&
                    peers.entries[k].port == s->port) {
                    online = 1;
                    pairing = peers.entries[k].pairing;
                }
            }
            const int want = pass == 0 ? current : pass == 1 ? (!current && online)
                           : pass == 2 ? (!current && !online) : 0;
            if (!want) continue;
            remote_computer_t *m = &out->items[out->count++];
            computer_from_profile(s, m);
            m->current = (uint8_t)current;
            m->online = (uint8_t)(online || current);
            m->pairing = (uint8_t)pairing;
            const char *announced = announced_name(s->host, s->port);
            if (announced && profile_name_is_address(s->name)) {
                snprintf(m->name, sizeof(m->name), "%s", announced);
            }
            // Not answering where it is saved, but a computer of the same
            // name answers somewhere else: the router moved it.  One row,
            // at the new address -- not a silent old one and a stranger.
            if (!online && !current) {
                for (unsigned k = 0; k < peers.count; k++) {
                    const discover_entry_t *e = &peers.entries[k];
                    if (profiles_find_moved(&prof, e->name, e->host, e->port) ==
                        (int)i) {
                        snprintf(m->old_host, sizeof(m->old_host), "%s", s->host);
                        snprintf(m->host, sizeof(m->host), "%s", e->host);
                        m->moved = 1;
                        m->online = 1;
                        break;
                    }
                }
            }
        }
        if (pass != 3) continue;
        for (unsigned k = 0; k < peers.count &&
                        out->count < REMOTE_COMPUTERS_MAX; k++) {
            const discover_entry_t *e = &peers.entries[k];
            if (profiles_find_host(&prof, e->host, e->port) >= 0) continue;
            // Already shown as a saved computer that moved here.
            const int moved = profiles_find_moved(&prof, e->name, e->host, e->port);
            if (moved >= 0 && !announced_name(prof.servers[moved].host,
                                              prof.servers[moved].port)) {
                continue;
            }
            remote_computer_t *m = &out->items[out->count++];
            memset(m, 0, sizeof(*m));
            snprintf(m->name, sizeof(m->name), "%s", e->name);
            snprintf(m->host, sizeof(m->host), "%s", e->host);
            m->port = e->port;
            m->online = 1;
            m->pairing = (uint8_t)e->pairing;
            m->current = strcmp(e->host, host) == 0 && e->port == port;
        }
    }
}

static char switch_name[PROFILE_NAME_MAX];

void boot_switch_done(void)
{
    if (switch_name[0]) remember_last(switch_name);
    switch_name[0] = '\0';
    if (moved_index >= 0 && (unsigned)moved_index < prof.count) {
        server_profile_t s = prof.servers[moved_index];
        snprintf(s.host, sizeof(s.host), "%s", moved_host);
        char err[64];
        if (profiles_save_server(PROFILES_ROOT, &s, err, sizeof(err))) {
            host_status("profile: %s is at %s now", s.name, s.host);
            profiles_load(PROFILES_ROOT, &prof);
        } else {
            host_status("profile: %s", err);
        }
    }
    moved_forget();
}

int boot_find_moved_last(dsterm_config_t *cfg)
{
    if (!cfg || !prof.last[0]) return 0;
    const int index = profiles_find(&prof, prof.last);
    if (index < 0) return 0;
    static discover_list_t list;
    discover_list_init(&list);
    find_by_broadcast(&list);
    for (unsigned k = 0; k < list.count; k++) {
        const discover_entry_t *e = &list.entries[k];
        if (profiles_find_moved(&prof, e->name, e->host, e->port) == index) {
            host_status("%s answers at %s now", prof.servers[index].name, e->host);
            return resolve_moved(index, e->host, cfg, 1);
        }
    }
    return 0;
}

int boot_switch_to(const remote_computer_t *m, dsterm_config_t *cfg)
{
    if (!m || !cfg) return 0;
    moved_forget();
    if (m->moved) {
        const int saved = profiles_find_host(&prof, m->old_host, m->port);
        const int ok = saved >= 0 && resolve_moved(saved, m->host, cfg, 0);
        switch_name[0] = '\0';
        if (ok) snprintf(switch_name, sizeof(switch_name), "%s",
                         prof.servers[saved].name);
        subscreen_set_boot(0, 0, NULL, NULL);
        host_refresh_sub();
        return ok;
    }
    int index = profiles_find_host(&prof, m->host, m->port);
    if (index < 0) {
        // Pairing borrows the boot menu's screens for the code; they are
        // handed back below whatever the outcome.
        index = add_server(m->name, m->host, m->port, m->pairing);
    }
    const int ok = index >= 0 && resolve_profile(index, cfg, 0);
    switch_name[0] = '\0';
    if (ok) snprintf(switch_name, sizeof(switch_name), "%s",
                     prof.servers[index].name);
    subscreen_set_boot(0, 0, NULL, NULL);
    host_refresh_sub();
    return ok;
}
