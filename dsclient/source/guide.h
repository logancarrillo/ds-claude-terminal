// CONTROLS: a guide to every button and combo, on the card itself.
//
// The card has more combos than any screen can name -- L then R, R then L,
// R+Y, L+R on Control, holding R, SELECT, and five remappable face buttons --
// and nothing on the device taught them.  This is the place that does.
//
// Opened from QUICK's CONTROLS row, Settings -> BUTTONS -> CONTROLS, or
// SELECT + START from anywhere except the composer and the camera.  The
// bottom screen draws the console being held, with each button lit while it
// is held; the top screen is a still Claude Code session with a pop-up that
// says what the last button or combo would have done.  Would have: the guide
// owns every press while it is up and acts on none of them (main.c's
// guide_input), so trying a combo here is always safe.
//
// What a press means depends on where you are, so the guide has contexts --
// CONTROL, KEYS, COMPOSE, FOCUS (the composer's keyboard), CAMERA and QUICK --
// picked with tabs or the d-pad's left and right.  The meanings are the
// code's, not a summary of it: COMPOSE runs the same voice_ptt_step() the
// composer does, so "which shoulder came first" is decided by the same rule,
// and CAMERA runs camera_shutter_input().
//
// Pure C with no libnds dependency: which combo a press is, in which context
// and under which mapping, the text keys for it, the tab and DONE geometry,
// the pop-up's place and the still session are all host-tested.  Drawing is
// subscreen.c's and render.c's.

#ifndef DSTERM_GUIDE_H
#define DSTERM_GUIDE_H

#include <stddef.h>
#include <stdint.h>

#include "buttons.h"
#include "camera.h"
#include "console.h"
#include "lang.h"
#include "proto.h"
#include "voice.h"

// The key word's bits (kbd.h: the register's ten, then X and Y), named here
// so this module needs no libnds.  The same positions as libnds's KEY_*.
#define GUIDE_KEY_A      0x001u
#define GUIDE_KEY_B      0x002u
#define GUIDE_KEY_SELECT 0x004u
#define GUIDE_KEY_START  0x008u
#define GUIDE_KEY_RIGHT  0x010u
#define GUIDE_KEY_LEFT   0x020u
#define GUIDE_KEY_UP     0x040u
#define GUIDE_KEY_DOWN   0x080u
#define GUIDE_KEY_R      0x100u
#define GUIDE_KEY_L      0x200u
#define GUIDE_KEY_X      0x400u
#define GUIDE_KEY_Y      0x800u
#define GUIDE_KEY_DPAD   (GUIDE_KEY_UP | GUIDE_KEY_DOWN | GUIDE_KEY_LEFT | GUIDE_KEY_RIGHT)

// B held this long leaves the guide: one second at 60 frames.
#define GUIDE_B_HOLD_FRAMES 60u
// A pop-up stays this long, or until the next press: about three seconds.
#define GUIDE_POPUP_FRAMES 180u

typedef enum {
    GUIDE_CTX_CONTROL = 0,
    GUIDE_CTX_KEYS,
    GUIDE_CTX_COMPOSE,
    GUIDE_CTX_FOCUS,     // the composer's keyboard, after L then R
    GUIDE_CTX_CAMERA,    // only on a card with a camera
    GUIDE_CTX_QUICK,     // QUICK and the other menus
    GUIDE_CTX_COUNT,
} guide_ctx_t;

typedef enum {
    GUIDE_COMBO_NONE = 0,
    GUIDE_COMBO_A,
    GUIDE_COMBO_B,
    GUIDE_COMBO_X,
    GUIDE_COMBO_Y,
    GUIDE_COMBO_START,
    GUIDE_COMBO_SELECT,
    GUIDE_COMBO_DPAD,        // up or down: left and right pick the context
    GUIDE_COMBO_L,
    GUIDE_COMBO_R,
    GUIDE_COMBO_SHIFT_TAB,   // R, then the button that sends Tab
    GUIDE_COMBO_LR,          // L+R together, either order
    GUIDE_COMBO_HOLD_R,      // COMPOSE and FOCUS: talk
    GUIDE_COMBO_R_THEN_L,    // COMPOSE and FOCUS: latch, cancel, or the notice
    GUIDE_COMBO_L_THEN_R,    // COMPOSE and FOCUS: focus on, or off
    GUIDE_COMBO_SHUTTER,     // CAMERA: whichever key SHUTTER names
    GUIDE_COMBO_INTRO,       // the pop-up the guide opens with
    GUIDE_COMBO_COUNT,
} guide_combo_t;

// One pop-up's worth: which combo, and the keys for its three lines.
typedef struct {
    guide_combo_t combo;
    // The combo as written: a TR key ("L then R"), or TR_COUNT for a plain
    // button name in `button` ("A", "L+R", "SELECT").  TR_GUIDE_COMBO_R_THEN
    // takes `button` as its %s.
    tr_id_t combo_text;
    const char *button;
    // Its name: a TR key, or TR_COUNT when it is a mapped face button and
    // the name is that action's chip (button_action_label: "ENT").
    tr_id_t name;
    button_action_t action;  // BUTTON_ACTION_COUNT unless mapped
    // What it does in the context, one key, wrapped to two lines.
    tr_id_t what;
} guide_entry_t;

// What the card is, as the guide needs to know it.  Read each step, so a
// remap on the BUTTONS tab shows at once.
typedef struct {
    const button_map_t *map;   // ui_settings.buttons
    camera_shutter_t shutter;  // ui_settings.camera_shutter
    int latch;                 // MIC is LATCH (ui_settings.mic_mode)
    int talk;                  // the computer offered speech-to-text
    int camera;                // the card has a camera: CAMERA is a context
} guide_config_t;

typedef struct {
    guide_ctx_t ctx;
    // COMPOSE and FOCUS: the composer's own push-to-talk machine, and
    // whether its pretend recording is running.
    voice_ptt_t ptt;
    int recording;
    // CONTROL and KEYS: R's one-shot Shift, armed and not yet used.
    int shift_armed;
    // CAMERA: the shutter's arm and spoil, as camera.c keeps it.
    camera_shutter_state_t shutter;
    // Frames B has been held, for the way out.
    unsigned b_frames;
    // The pop-up on screen, and for how long.  `shown` is 0 with none.
    int shown;
    guide_entry_t entry;
    unsigned shown_frames;
} guide_t;

void guide_init(guide_t *g, guide_ctx_t ctx, const guide_config_t *cfg);
// Show another context (a tab touched): its pretend state starts afresh.
void guide_set_ctx(guide_t *g, guide_ctx_t ctx, const guide_config_t *cfg);

// What one frame of input did.
typedef enum {
    GUIDE_STEP_NONE = 0,  // nothing to redraw but the lit buttons
    GUIDE_STEP_SHOW,      // a new pop-up: g->entry
    GUIDE_STEP_CONTEXT,   // the context changed
    GUIDE_STEP_LEAVE,     // SELECT + START, or B held for a second
} guide_step_t;

// `held` is the key word this frame and `edge` its newly pressed bits.
guide_step_t guide_step(guide_t *g, const guide_config_t *cfg,
                        unsigned held, unsigned edge);

// One frame passing: returns 1 the frame a pop-up times out.
int guide_tick(guide_t *g);

// The pop-up a combo shows in a context, with no history: what the drawing
// labels a button with, and what the tests read.  `recording` is the
// pretend recording R then L acts on (COMPOSE and FOCUS).
guide_entry_t guide_entry(guide_ctx_t ctx, const guide_config_t *cfg,
                          guide_combo_t combo, int recording);

// Whether a context exists on this card (CAMERA needs a camera), and the
// next one along from `ctx` in direction `dir` (+1 or -1), wrapping and
// skipping one that does not.
int guide_ctx_present(guide_ctx_t ctx, const guide_config_t *cfg);
guide_ctx_t guide_ctx_step(guide_ctx_t ctx, int dir, const guide_config_t *cfg);
tr_id_t guide_ctx_label(guide_ctx_t ctx);

// Which context the guide opens on: where the person was.
guide_ctx_t guide_ctx_for(int screen_keys, int focus_on, int quick_open);

// The three lines of a pop-up, in the language on screen: the combo, its
// name, and what it does.  Each written into its own buffer.
void guide_entry_texts(const guide_entry_t *e, char *combo, size_t combo_cap,
                       char *name, size_t name_cap, char *what, size_t what_cap);

// A face button's short label on the drawing: what it sends under the
// mapping on CONTROL and KEYS ("ENT" in English, the language's own word in
// a translation when it fits), what it does elsewhere ("SEND"), and "-" for
// nothing.  The pop-up's name line has room for the whole word in every
// language ("ENTER").
const char *guide_button_label(guide_ctx_t ctx, const guide_config_t *cfg,
                               guide_combo_t combo);

// --------------------------------------------------------- the wrapping ---

#define GUIDE_LINE_MAX 96   // bytes in one wrapped line, NUL included

// Break `s` into at most two lines of at most `cols` display columns (wide
// CJK characters are two): at a space for Latin text, between characters
// for CJK.  Returns the number of lines (1 or 2), or 0 when it does not fit
// in two -- the second line is then cut on a whole character.
unsigned guide_wrap(const char *s, unsigned cols,
                    char lines[2][GUIDE_LINE_MAX]);

// ---------------------------------------------------- the bottom screen ---
//
// The tabs across the top and the DONE tile, one table for drawing and for
// touch.  Tabs are sized to their labels in the language on screen and
// share the row's width; CAMERA has none on a card without a camera.
#define GUIDE_TAB_Y 1
#define GUIDE_TAB_H 13
int guide_tab_rect(guide_ctx_t ctx, const guide_config_t *cfg,
                   int *x, int *y, int *w, int *h);
// The tab under a touch, or GUIDE_CTX_COUNT for none.
guide_ctx_t guide_tab_hit(const guide_config_t *cfg, int x, int y);

// DONE sits in the drawn console's own touch screen.
#define GUIDE_DONE_X 98
#define GUIDE_DONE_Y 84
#define GUIDE_DONE_W 60
#define GUIDE_DONE_H 16
int guide_done_hit(int x, int y);

// Which console to draw: the DSi stands in for one the card cannot name.
console_family_t guide_console(console_family_t family);

// ------------------------------------------------------- the top screen ---
//
// A still Claude Code session -- a prompt, a tool call, a short answer and
// the input box -- for the pop-ups to stand on.  Nothing in it comes from
// the PTY: it is written here, at the grid's own size.
void guide_session_fill(grid_t *g);
// The input box's top row in that session, and its first pixel row on the
// top screen (the terminal's cell height at this build's grid).
int guide_input_row(void);
int guide_input_px(void);

// Where the pop-up goes: above the input box, never over it.  The box is
// `lines` text lines tall at GUIDE_POPUP_PITCH (tall enough for CJK).
#define GUIDE_POPUP_X 6
#define GUIDE_POPUP_W 244
#define GUIDE_POPUP_PAD 6
#define GUIDE_POPUP_PITCH 11
#define GUIDE_POPUP_COLS ((GUIDE_POPUP_W - 2 * GUIDE_POPUP_PAD) / 6)
void guide_popup_rect(unsigned lines, int *x, int *y, int *w, int *h);

// The pop-up's lines for an entry, in the language on screen: the combo,
// its name, then what it does wrapped to GUIDE_POPUP_COLS (one or two
// lines).  Returns how many lines (3 or 4).
#define GUIDE_POPUP_LINES 4
unsigned guide_popup_lines(const guide_entry_t *e,
                           char lines[GUIDE_POPUP_LINES][GUIDE_LINE_MAX]);

#endif  // DSTERM_GUIDE_H
