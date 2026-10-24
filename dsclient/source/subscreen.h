// The bottom screen: a 256x256 16-bit bitmap background painted with the same
// 6x8 glyph routine as the top screen (render_cell_into). Replaces the console
// that used to show the boot log, so it owns that log (log.c) plus the counters
// that used to be drawn with printf cursor escapes.
//
// Modes, cycled by SELECT (status.h owns the state):
//   SCREEN_MODE_CONTROL  link line, scrollback readout, large touch controls
//   SCREEN_MODE_KEYS     two status lines on top, the keyboard, hint at bottom
//   SCREEN_MODE_STATUS   full-screen log, counters pinned on the last two rows
//   SCREEN_MODE_PROJECT  the tab strip and the DO buttons; takes the top screen
//
// And one mode that is not on the cycle, reached from its own Control tile:
//   SCREEN_MODE_SETTINGS the chip rows; also takes the top screen
//
// X returns to CONTROL from any of them, so no screen is more than one press
// from the one with the buttons on it.

#ifndef DSTERM_SUBSCREEN_H
#define DSTERM_SUBSCREEN_H

#include <stddef.h>
#include <stdint.h>

#include "clip.h"
#include "guide.h"
#include "project.h"
#include "settings.h"
#include "mirror.h"
#include "quick.h"
#include "remote.h"
#include "sessions.h"

void subscreen_init(void);

// Repaint the whole sub screen from the current log, mode, and counters. The
// counters are the two lines that used to be the diag() console output, so the
// L<number> line a photograph checks still exists. Called after every log
// append (boot messages appear immediately) and once per second (counters).
void subscreen_update(unsigned long rx, unsigned long msgs, unsigned long tx,
                      unsigned long loops, unsigned raw, unsigned lib);

// While the composer is open the sub screen is its keyboard, whatever mode
// SELECT last left. It stays the composer's keyboard in REVIEW as well as in
// EDIT: leaving the ordinary keyboard there would put live PTY keys one touch
// away from a prompt the user is still reading.
void subscreen_set_composer(int active);
void subscreen_set_composer_add_quick(int add_quick);
void subscreen_set_voice(int ready, int recording, int waiting, unsigned bars);
void subscreen_set_voice_latched(int latched);

// The boot menu's lower screen: two header lines (what this step is, and
// what the buttons do), then either the touch keyboard -- while a field is
// being typed into -- or the tail of the boot log, so what the radio and
// the network are doing stays visible under the menu.  Owned by boot.c;
// inactive once the terminal is running.
// While the boot menu is up without its keyboard, the lower screen shows
// `body` (lines separated by '\n') in place of the log -- the first-run guide
// -- or the log when `body` is NULL.  Borrowed; must outlive the menu.
void subscreen_set_boot_body(const char *body);
void subscreen_set_boot(int active, int keyboard, const char *line0,
                        const char *line1);

// The session manager is a bottom-screen overlay.  Unlike the composer and
// snippet overlays it leaves the terminal visible on the top screen: its
// state lives here so the main loop can own input and this module can own
// the one-frame-at-a-time drawing discipline.  (The extension's picker is
// the same kind of overlay and draws itself, through ext_subscreen_draw().)
// REMOTE: the picker of other sessions and other computers (remote.h).  The
// pointers are borrowed and must stay valid while `active`.
void subscreen_set_remote(int active, const remote_list_t *list,
                          const remote_computers_t *computers,
                          const remote_ui_t *ui, const char *here);
// Which REMOTE row a touch landed on, or -1.  Rows are text rows from 2 down.
int subscreen_remote_row_at(int py);

// QUICK (quick.h): the list on the bottom screen, with the terminal left on
// the top one so a MODE press can be watched taking effect.  `level` is one
// of the three below; `top` is the first row of a long sub-list on screen.
// The pointers are borrowed and must stay valid while `active`.
#define SUBSCREEN_QUICK_MAIN 0
#define SUBSCREEN_QUICK_COMMANDS 1
#define SUBSCREEN_QUICK_AGAIN 2
#define SUBSCREEN_QUICK_LIST_ROWS 20     // sub-list rows on screen at once
void subscreen_set_quick(int active, int level, int selected, int top,
                         const quick_commands_t *commands,
                         const quick_recent_t *recent);
void subscreen_set_quick_saved(const quick_saved_t *saved, int remove_armed);
// Claude's mode as QUICK last read it ("" while unknown): on the tile, and on
// the MODE row.
void subscreen_set_claude_mode(const char *mode);
// The bottom screen as text, as last drawn (mirror.h).
const mirror_t *subscreen_mirror(void);

// UNDO's rewind list is up: the Control screen's hint says the d-pad picks.
void subscreen_set_rewind(int picking);
// Whether this card has a camera, from isDSiMode() at boot: QUICK's PHOTO
// row exists only where there is a sensor behind it.
void subscreen_set_camera_available(int available);
// The camera screen (camera.h): main composes the two strips and pushes them
// in like everything else; this module owns where they go and the FLIP tile.
// `error` draws the hint in the failure colour.
void subscreen_set_camera(int active, const char *top, const char *hint,
                          int error);
// The viewfinder: rows CAMERA_VIEW_Y0..CAMERA_VIEW_Y1 of `frame` (256 wide)
// into the bitmap, leaving the two strips alone.  Called from frame_wait()
// just after the repaint, only when a fresh frame exists.
void subscreen_camera_blit(const uint16_t *frame);
// Which entry of the level on screen a touch at `py` landed on, or -1.
int subscreen_quick_row_at(int level, int top, int py);

// The PROJECT mode's lower half: which tab is selected, and the finished lines
// to show under the tab strip.  Main composes the text; this module only
// decides where it goes.
#define SUBSCREEN_PROJECT_LINES_MAX 15
void subscreen_set_project(unsigned tab, const char lines[][44], unsigned count);

// The DO tab's buttons: the merged action list and which one the d-pad has.
// Passed as the finished list rather than the project state, so this module
// keeps showing what it is told and never re-derives the merge.
void subscreen_set_project_actions(const project_action_t *actions,
                                   unsigned count, unsigned selected,
                                   unsigned dropped, int god_mode);

// The STATUS screen's CLIP tab: the summary line and the wrapped preview.
// Same contract as everything else here -- main composes the text, this module
// only decides where it goes.
#define SUBSCREEN_CLIP_LINES_MAX 14
void subscreen_set_clip(const char *summary,
                        const char lines[][CLIP_LINE_COLS + 1],
                        unsigned count, int can_paste);

// The SETTINGS mode's state: the live values to draw, and where the highlight
// is.  Pushed in like everything else here -- this module shows what it is
// told rather than reading the settings back out of whoever applies them.
// What the face buttons do now (Settings' BUTTONS tab), for the Control
// screen's legend and hints: the legend line (buttons.h), and the names of
// the buttons on Enter, Stop and Tab ("" for none).
void subscreen_set_button_legend(const char *legend, const char *enter,
                                 const char *stop, const char *tab);
void subscreen_set_settings(const settings_t *settings,
                            const settings_ui_t *ui);

// The lower screen's half of render_set_shake(), with the same contract: it
// moves this whole panel by `offset_y` and 0 puts it back.  A landing shakes
// the screen it landed on, so only one of the two is ever moving.
void subscreen_set_shake(int offset_y);

// The controls guide (guide.h) owns the whole bottom screen while `active`:
// the tabs, the drawn console with `held` lit, and DONE.  Copied, so the
// caller's state may move on before the next paint.
void subscreen_set_guide(int active, const guide_t *g, const guide_config_t *cfg,
                         console_family_t family, unsigned held);

// Three ways to put a finished 42-column line on the panel, for anything
// that draws through ext.h: a plain row, a row in two colours split at a
// column, and a row filled as the selected item of a list.  Cell-aligned,
// padded to the full width, and nothing else -- an extension gets rows,
// not pixels.
void subscreen_draw_line(int row, const char *s, uint8_t fg);
void subscreen_draw_line_split(int row, const char *s, uint8_t fg,
                               unsigned split, uint8_t fg2);
void subscreen_draw_selected_line(int row, const char *s, int selected);

// The Control screen's touch targets live in kbd.h (kbd_control_t,
// kbd_control_rect, kbd_control_hit) rather than here. This file draws them
// from that table and main.c hit-tests against the same one, so a tile cannot
// move on screen without its touch target moving with it -- and unlike this
// file, kbd.c has no libnds dependency, so the geometry is host-tested.

#endif  // DSTERM_SUBSCREEN_H
