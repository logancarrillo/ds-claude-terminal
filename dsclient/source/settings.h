// Settings: the things you pick this device up to change.
//
// The first version of this page had four rows -- model, brightness, key
// repeat, god mode -- and three of them were furniture. Brightness duplicated
// a button on the side of the console. Key repeat tuned a behaviour nobody
// reaches on a touchscreen. And the model row did not change the model: it
// wrote `/model opus` into the composer and left. A settings page whose rows
// mostly do not matter teaches you not to open it, which is worse than not
// having one.
//
// So the rule for what goes here is narrow: a row earns its place by being a
// thing somebody actually wants mid-session, that this device can genuinely
// do, and that has nowhere better to live.
//
// Five tabs: SESSION is what reaches Claude; SOUND, SCREEN and LINK are this
// device, and the place to make it yours; BUTTONS is what the keys do.  L and
// R move between them.
//
// SESSION
//   MODEL     switch the running session's model.  The reason the page exists.
//   SESSION   /compact, /cost, /status -- the three session commands worth a
//             button, and the reason you would rather not type them one glass
//             key at a time.
//   GOD MODE  the arming switch for the PROJECT page's one-tap actions.
//
// SOUND
//   SOUND     the instrument: the sampled SOFT set (desk sounds), the
//             square-wave CHIP voice, or OFF.
//   VOLUME    how loud, for the room you are in.
//   TYPING    the clicks of your own keys and the cursor, on or off -- the
//             sounds that tell Claude's state stay either way.
//   WRITING   soft typing sounds while Claude writes: OFF, LOW, MID (the
//             default) or HIGH.  Sparse at first and thinner the longer an
//             answer runs, so a long one never becomes a drone.
//
// SCREEN
//   THEME     the accent colour: Claude's clay, sea, moss, or mono.
//   LANGUAGE  EN, ES, ZH, JA, and up to two packs from /dsterm/lang (lang.h).
//   ON BOOT   which lower screen the card comes up on.
//   ADDRESSES SHOW or HIDE: hidden, every IP address the card draws is
//             starred out (privacy.h), for a stream, a video or a photo.
//
// LINK
//   AUTOCONNECT  whether the card connects to the last computer by itself
//             at startup, after a three-second countdown, or waits in the
//             menu for you to pick.
//   VIEW TAPS OFF or ON: whether a click on this DS's bottom screen in
//             `dsterm view` on the computer taps it.  Off by default: it
//             lets the computer press anything here, so it is the person
//             holding the DS who turns it on -- and GOD MODE can still only
//             be armed from the DS itself.
//   PICTURE   FULL (640x480) or SMALL (320x240): the still the camera sends.
//             Only on a card with a camera -- DSi -- so the row is absent
//             elsewhere.
//   LIVE VIEW GREY (128x96, sixteen greys) or COLOUR (96x72, 256 colours):
//             the camera's view on the computer.  Camera cards only, too.
//   MIC       HOLD (let go of R to transcribe) or LATCH (press L with R held
//             to keep recording hands-free).
//
// BUTTONS
//   A, B, X, Y, START   what each sends on the Control and KEYS screens:
//             Enter, Space, Delete, Esc, Tab or Stop (buttons.h).  Enter
//             has to stay on one of them.
//   SHUTTER   the camera screen's shutter: R, L or A (buttons.h).  Only on
//             a card with a camera -- DSi -- so the row is absent elsewhere.
//   CONTROLS  opens the controls guide (guide.h): every button and combo,
//             drawn on this console and tried without sending anything.
//   DEFAULTS  put every tab back, behind a confirm (the chip reads SURE?).
//
// Everything about an extension stays behind its own tile. That was the ask
// and it is also the right line: this page is about the device and the
// session, not about who else is on it.
//
// -- picking types, and picking sends -------------------------------------
//
// Two rows here reach the PTY, and they do it with no composer step. That is
// a deliberate exception to a rule this program otherwise keeps strictly, so
// the argument is written down rather than assumed:
//
// The rule is that *server-supplied* text never executes on its own
// (snippets.h:37-42, enforced by tests/test_repo.py). It exists because a
// pushed snippet or a discovered action is a string somebody else chose, and
// the composer is where you get to read it first.
//
// Nothing on this page is that. Every string a settings row can type is a
// literal in this file, fixed at build time, unreachable from the wire, and
// printed on the top screen beside the button that types it. There is nobody
// to be protected from. Meanwhile the cost of the gate here is exactly the
// defect being fixed: a switch that needs a second screen to take effect is
// the switch that got called fake.
//
// The one destructive row -- DEFAULTS -- gets a confirm instead, which is the
// protection that actually fits it.
//
// Pure C with no libnds dependency: the row machine, the option tables, the
// geometry and every string are host-tested. Applying a value is somebody
// else's job. That separation is deliberate -- a setting that cannot be tested
// because reading it requires hardware is a setting that drifts.

#ifndef DSTERM_SETTINGS_H
#define DSTERM_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#include "buttons.h"
#include "lang.h"
#include "proto.h"

// Which model the session should be asked to run.
//
// Aliases, not identifiers. Claude Code resolves `opus`/`sonnet`/`haiku`
// itself and has done across several model generations; a pinned identifier
// spelled out here would be a number that goes stale on a device you have to
// reflash to correct.
//
// UNSET is not a fifth model, it is the honest state before you have asked for
// one: this card cannot see what the session is running, only what it last
// asked for, and a page that showed "opus" on boot would be inventing that.
typedef enum {
    SETTINGS_MODEL_UNSET = 0,
    SETTINGS_MODEL_OPUS,
    SETTINGS_MODEL_SONNET,
    SETTINGS_MODEL_HAIKU,
    SETTINGS_MODEL_COUNT,
} settings_model_t;

// Which lower screen the card comes up on. Only the three that are somewhere
// to *be*: STATUS is a log you glance at and SETTINGS is a page you leave.
typedef enum {
    SETTINGS_BOOT_CONTROL = 0,
    SETTINGS_BOOT_KEYS,
    SETTINGS_BOOT_PROJECT,
    SETTINGS_BOOT_COUNT,
} settings_boot_t;

// The SOUND row.  OFF is a value of the instrument rather than a separate
// switch, because "which sound" and "whether sound" are one decision to the
// person making it.
typedef enum {
    SETTINGS_SOUND_SNES = 0,
    SETTINGS_SOUND_CHIP,
    SETTINGS_SOUND_OFF,
    SETTINGS_SOUND_COUNT,
} settings_sound_t;

// The accent colour: headings, the highlight, the selected tile -- and
// Claude Code's own orange in the terminal, which is the same colour.
typedef enum {
    SETTINGS_THEME_CLAY = 0,
    SETTINGS_THEME_SEA,
    SETTINGS_THEME_MOSS,
    SETTINGS_THEME_MONO,
    SETTINGS_THEME_COUNT,
} settings_theme_t;

typedef enum {
    SETTINGS_VOLUME_LOW = 0,
    SETTINGS_VOLUME_MID,
    SETTINGS_VOLUME_HIGH,
    SETTINGS_VOLUME_COUNT,
} settings_volume_t;

// The still the camera sends: the whole sensor, or a quarter of it to get
// there four times faster.  Values are camera.h's wire formats minus one.
typedef enum {
    SETTINGS_PICTURE_FULL = 0,
    SETTINGS_PICTURE_SMALL,
    SETTINGS_PICTURE_COUNT,
} settings_picture_t;

// The camera's view on the computer: sharper in greys, or in colour at a
// lower size -- the same bytes a frame either way (camera.h).
typedef enum {
    SETTINGS_LIVEVIEW_GREY = 0,
    SETTINGS_LIVEVIEW_COLOUR,
    SETTINGS_LIVEVIEW_COUNT,
} settings_liveview_t;

// Push-to-talk in the composer: hold R the whole time (HOLD), or allow L
// while holding R to latch the microphone until R or L is tapped (LATCH).
typedef enum {
    SETTINGS_MIC_HOLD = 0,
    SETTINGS_MIC_LATCH,
    SETTINGS_MIC_COUNT,
} settings_mic_t;

typedef struct {
    // What was last asked for, not what is running. See SETTINGS_MODEL_UNSET.
    settings_model_t model;
    settings_boot_t boot;
    settings_sound_t sound;
    settings_volume_t volume;
    // WRITING: 0 off, 1 low, 2 mid, 3 high.  (Named for the file's key,
    // which predates the levels: version 3 wrote on/off.)
    int blips;
    // The clicks of your own keys and the cursor.  On by default.
    int typing;
    settings_theme_t theme;
    // What A, B, X, Y and START send (button_action_t, buttons.h).
    button_map_t buttons;
    // The camera screen's shutter (camera_shutter_t, buttons.h).  Default R.
    camera_shutter_t camera_shutter;
    // The still's size (settings_picture_t).  Default FULL.
    settings_picture_t picture;
    // The computer's live view (settings_liveview_t).  Default GREY.
    settings_liveview_t liveview;
    // Push-to-talk latch mode (settings_mic_t).  Default HOLD.
    settings_mic_t mic_mode;
    // Connect to the last computer by itself at startup.  On by default:
    // that is what a card that has been paired is for.
    int autoconnect;
    // Draw IP addresses as stars (privacy.h).  Off by default: the address is
    // what you need to see when something will not connect.
    int hide_addresses;
    // `dsterm view`'s clicks tap the bottom screen (main.c).  Off by default.
    int view_taps;
    // The UI language index (0 = LANG_EN, 1 = LANG_ES, 2 = LANG_ZH, 3 = LANG_JA,
    // or a loaded custom language from /dsterm/lang/*.lang).
    unsigned language;
    // Whether a one-tap action on the PROJECT page may reach the PTY without
    // passing through the composer first.
    //
    // Everything else on this page is a preference. This one is a safety rule
    // being switched off, and it is worth being blunt about why it is allowed
    // to exist at all: the composer step is what stands between a *planted*
    // action and a keystroke, on a link with no per-message integrity. Off by
    // default, never assertable by the server, saved so it survives a reboot
    // only because a switch that silently re-arms itself is worse than one
    // that stays where you left it -- and the page says so while it is on.
    int god_mode;
} settings_t;

// Rows on the page, in display order, tab by tab.
typedef enum {
    // SESSION
    SETTINGS_ROW_MODEL = 0,
    SETTINGS_ROW_SESSION,
    SETTINGS_ROW_GOD_MODE,
    // SOUND
    SETTINGS_ROW_SOUND,
    SETTINGS_ROW_VOLUME,
    SETTINGS_ROW_TYPING,
    SETTINGS_ROW_BLIPS,
    // SCREEN
    SETTINGS_ROW_THEME,
    SETTINGS_ROW_LANGUAGE,
    SETTINGS_ROW_BOOT,
    SETTINGS_ROW_ADDRESSES,
    // LINK
    SETTINGS_ROW_AUTOCONNECT,
    SETTINGS_ROW_VIEW_TAPS,
    // Present only when the card says it has a camera, like SHUTTER: without
    // the sensor the setting would size a picture that cannot be taken.
    SETTINGS_ROW_PICTURE,
    SETTINGS_ROW_LIVEVIEW,
    SETTINGS_ROW_MIC,
    // BUTTONS
    SETTINGS_ROW_BUTTON_A,
    SETTINGS_ROW_BUTTON_B,
    SETTINGS_ROW_BUTTON_X,
    SETTINGS_ROW_BUTTON_Y,
    SETTINGS_ROW_BUTTON_START,
    // Present only when the card says it has a camera: without the sensor
    // the setting would pick the shutter of a screen that cannot open.
    SETTINGS_ROW_SHUTTER,
    // Opens the controls guide (guide.h): not a setting, a door.
    SETTINGS_ROW_CONTROLS,
    SETTINGS_ROW_DEFAULTS,
    SETTINGS_ROW_COUNT,
} settings_row_t;

#define SETTINGS_TAB_SESSION 0u
#define SETTINGS_TAB_SOUND   1u
#define SETTINGS_TAB_SCREEN  2u
#define SETTINGS_TAB_LINK    3u
#define SETTINGS_TAB_BUTTONS 4u
#define SETTINGS_TAB_COUNT   5u

// WRITING's levels.
#define SETTINGS_WRITING_OFF 0
#define SETTINGS_WRITING_LOW 1
#define SETTINGS_WRITING_MID 2
#define SETTINGS_WRITING_HIGH 3

// The most options any one row offers. Six today (a button's actions); the geometry below proves
// every row's options fit the screen, so this is a bound on the tables rather
// than a layout constant.
#define SETTINGS_OPTIONS_MAX 6

// Transient page state: where the highlight is, and the DEFAULTS confirm.
// The tab on screen is the highlighted row's: there is no separate tab to
// fall out of step with it.
//
// Separate from settings_t on purpose. settings_t is what gets written to the
// card; none of this belongs in a file, and keeping them apart means a page
// redraw can never be mistaken for something worth saving.
typedef struct {
    unsigned row;
    unsigned option;
    // The DEFAULTS row's second press. Armed by the first, cleared by any
    // other press and by leaving the page, so it can never be sitting armed
    // when you come back and reach for something else.
    int reset_armed;
    // The first of the tab's rows on screen: a tab longer than the screen
    // scrolls, and the highlight is always kept in view.
    unsigned scroll;
    // The last press was refused (Enter would have left every button).
    // Cleared by the next press or move.
    int refused;
} settings_ui_t;

void settings_defaults(settings_t *settings);
void settings_ui_init(settings_ui_t *ui);
int settings_valid(const settings_t *settings);

// Runtime, not stored: whether this card has a camera (a DSi).  When it does
// not, the SHUTTER row does not exist -- row counts, rects, hit tests and
// navigation all skip it, as if it were never in the table.
void settings_set_camera_available(int on);
int settings_camera_available(void);

// ------------------------------------------------------------- the rows ---

const char *settings_row_label(settings_row_t row);
// Which tab a row is on, and a tab's name.
unsigned settings_row_tab(settings_row_t row);
// How many rows a tab has (some scroll: see SETTINGS_VISIBLE_ROWS).
unsigned settings_tab_rows(unsigned tab);
const char *settings_tab_label(unsigned tab);
// The accent colour a theme stands for, as the DS's RGB15.
uint16_t settings_theme_rgb(settings_theme_t theme);
// One line of explanation for the selected row, shown on the top screen. A
// switch whose effect you might not see for a minute is a switch you cannot
// tell you have pressed.
const char *settings_row_note(settings_row_t row);

// How many options this row offers, and the label of one of them.
unsigned settings_option_count(settings_row_t row);
const char *settings_option_label(settings_row_t row, unsigned option);

// The exact text picking this option will type into the session, or NULL if it
// only changes something on the card. Shown on the top screen beside the row,
// which is the whole reason this is a separate query rather than a side effect
// of pressing: you can read what a button types before you press it.
const char *settings_option_sends(settings_row_t row, unsigned option);

// Which option is currently the row's value, or SETTINGS_OPTIONS_MAX for a row
// that has no current value -- SESSION and DEFAULTS are things you do, not
// things you are.
unsigned settings_option_current(const settings_t *settings,
                                 settings_row_t row);

// ----------------------------------------------------------- the presses ---

// Move the highlight. `rows` and `cols` are deltas; the row's option index is
// clamped into the new row rather than wrapping, because a d-pad walking a
// grid should stop at its edges the way the Control screen's does.
void settings_ui_move(const settings_t *settings, settings_ui_t *ui,
                      int cols, int rows);
// Show `tab`: the highlight goes to its first row, on that row's current
// value where it has one.
void settings_ui_tab(const settings_t *settings, settings_ui_t *ui,
                     unsigned tab);

// What pressing an option did.
typedef enum {
    SETTINGS_PRESS_NOTHING = 0,  // out of range, or a confirm still pending
    SETTINGS_PRESS_CHANGED,      // a stored value moved: apply it and save
    SETTINGS_PRESS_SENDS,        // type `*send` into the session
    SETTINGS_PRESS_RESET,        // defaults restored: apply it and save
    SETTINGS_PRESS_GUIDE,        // open the controls guide; nothing changed
} settings_press_t;

// Press one option on one row. Updates `settings` and `ui` in place; `*send`
// is set to the text to type when the result is SETTINGS_PRESS_SENDS and to
// NULL otherwise, so a caller that forgets to check the result still cannot
// type a stale string.
settings_press_t settings_press(settings_t *settings, settings_ui_t *ui,
                                settings_row_t row, unsigned option,
                                const char **send);

// ---------------------------------------------------------- the geometry ---
//
// Drawing and hit-testing both come from these, so a chip cannot be shown at
// one place and pressed at another -- the rule every picker here opens by
// insisting on. A tab's eight rows of 18 from y=17 finish at y=161 -- the
// last one's chips at y=160, where the strip kept clear for an extension
// begins; the chips are 15 pixels tall, a comfortable stylus target and still
// a thumb's.  The two tab chips share the top line with the page's hint.
#define SETTINGS_TAB_X 4
#define SETTINGS_TAB_Y 1
#define SETTINGS_TAB_H 14
#define SETTINGS_ROW_Y 17
#define SETTINGS_ROW_H 17
#define SETTINGS_ROW_PITCH 18
#define SETTINGS_VISIBLE_ROWS 8   // a longer tab scrolls
#define SETTINGS_LABEL_X 4
#define SETTINGS_OPTION_X 82      // where the chips start, clear of the labels
#define SETTINGS_OPTION_H 15

// Where a row's chips start in the active language: computed from the widest
// row label on `row`'s tab so no translated label ever runs under its chips.
int settings_row_option_x(settings_row_t row);

// `scroll` is settings_ui_t.scroll: rows scrolled out of view have no rect
// (returns 0), so nothing is drawn or pressed where it is not.
int settings_row_rect(settings_row_t row, unsigned scroll,
                      int *x, int *y, int *w, int *h);
int settings_tab_rect(unsigned tab, int *x, int *y, int *w, int *h);
// Which tab chip a touch landed on, or SETTINGS_TAB_COUNT for none.
unsigned settings_tab_hit(int x, int y);
// A chip's rect. Width comes from its own label, so the tables are the only
// place a chip's size is decided.
int settings_option_rect(settings_row_t row, unsigned option, unsigned scroll,
                         int *x, int *y, int *w, int *h);

// Which row of `tab` a touch landed on, or SETTINGS_ROW_COUNT for none.
settings_row_t settings_row_hit(unsigned tab, unsigned scroll, int x, int y);
// The scroll arrows at the right edge of a tab that is longer than the
// screen: -1 for a touch on the "more above" arrow, 1 for "more below", 0
// for neither (or no arrow there to touch).
#define SETTINGS_ARROW_X 244
int settings_scroll_hit(unsigned tab, unsigned scroll, int x, int y);
// Whether a tab has rows above or below what is on screen.
int settings_more_above(unsigned scroll);
int settings_more_below(unsigned tab, unsigned scroll);

// Which chip a touch landed on within a row, or SETTINGS_OPTIONS_MAX for none.
// A touch on the row but not on a chip selects the row without pressing
// anything, which is what makes the labels safe to rest a thumb on.
unsigned settings_option_hit(settings_row_t row, unsigned scroll, int x, int y);

// ------------------------------------------------------------- the page ---
//
// The top screen, rendered into an ordinary grid_t and drawn with the ordinary
// cell renderer -- the same trick project.h explains at length. It means the
// page is made of the same glyphs as the terminal, and it means an extension's
// safe-rect search and roaming machinery work on it unchanged, so it gets to
// live here too.

// Facts about this build and this link, composed by whoever knows them and
// only placed by this module. Finished strings, for the same reason project.c
// takes finished rows: two places formatting one value is how they drift.
#define SETTINGS_ABOUT_MAX 40
typedef struct {
    char build[SETTINGS_ABOUT_MAX + 1];   // rom name and geometry
    char link[SETTINGS_ABOUT_MAX + 1];    // where it is connected, and how well
    char session[SETTINGS_ABOUT_MAX + 1]; // the selected session, if any
    // The extension's row, if it has one: an empty label skips the row.
    char ext_label[SETTINGS_ABOUT_MAX + 1];
    char ext_value[SETTINGS_ABOUT_MAX + 1];
    char card[SETTINGS_ABOUT_MAX + 1];    // where the settings file went
} settings_about_t;

void settings_about_init(settings_about_t *about);

// Every cell is written, including the blank ones: a page that only paints its
// own text leaves the previous page's showing through.
void settings_render(const settings_t *settings, const settings_ui_t *ui,
                     const settings_about_t *about, grid_t *grid);

// ------------------------------------------------------------- the file ---
//
// A small versioned key=value file, and deliberately a *separate* one from
// anything an extension keeps: a corrupt UI setting must not be able to cost
// somebody their extension's preferences, or the other way round.
//
// Version 3 adds sound, volume and blips.  Version 2 files load with those at
// their defaults.  Version 1 carried `dim` and `repeat`, which no longer
// exist, and spelled the unset model `default`. Both are read and discarded
// rather than rejected, so upgrading the card does not throw away the one
// setting that survived -- see settings_parse().
// Version 4 adds typing, theme, autoconnect, the button map (button_a ...
// button_start) and WRITING's levels (blips=off|low|mid|high; version 3's
// on reads as mid).  Keys added later join it without a bump -- shutter and
// picture, the camera's -- because an older card ignores a key it does not
// know and keeps the rest, where an unknown version would have it reset
// everything.
#define SETTINGS_VERSION 4
#define SETTINGS_FILE_MAX 512
#define SETTINGS_DEFAULT_FILENAME "dsterm-settings.cfg"

int settings_parse(const char *text, settings_t *settings,
                   char *err, size_t err_len);
int settings_load(const char *path, settings_t *settings,
                  char *err, size_t err_len);
int settings_save(const char *path, const settings_t *settings,
                  char *err, size_t err_len);

#endif  // DSTERM_SETTINGS_H
