// Draws the PROTO_COLS x PROTO_ROWS cell grid onto the DS main screen.
//
// Uses a 16-bit bitmap background (MODE_5_2D, BG3 in VRAM_A) rather than tiled
// backgrounds: sub-8px glyphs do not align to 8x8 tiles, and per-cell
// foreground and background colour would need more palette juggling than it is
// worth. It still gives the renderer a direct 256px-wide pixel surface, while
// leaving the main OBJ engine and VRAM_E free for anything drawn over it.
//
// The cell size follows the grid geometry (42x24 is 6x8, 64x32 is 4x6, 51x27
// is 5x7) and the glyph loop is shared with the sub screen's fixed 6x8 UI
// layout through render_glyph_into() and render_cell_into(), so the terminal,
// the log and the keyboard labels stay one font routine apart from the
// terminal's own zoomed font.

#ifndef DSTERM_RENDER_H
#define DSTERM_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

// Palette indices, matching _PALETTE_INDEX in dsterm/protocol.py.
#define PAL_DEFAULT 0
#define PAL_BLACK   1
#define PAL_RED     2
#define PAL_GREEN   3
#define PAL_YELLOW  4
#define PAL_BLUE    5
#define PAL_MAGENTA 6
#define PAL_CYAN    7
#define PAL_WHITE   8
// Not an ANSI colour. Claude Code paints its mark, its box rules and its
// prompt in 256-colour 174 and its neighbours -- the colour this program
// renders more than any other, and one that eight ANSI slots have nothing
// within reach of. The attribute byte already carried four bits per colour
// with only nine spoken for, so this costs no wire and no protocol version.
#define PAL_ORANGE  9

void render_init(void);

// Move the whole terminal by `offset_y` pixels, and put it back with 0.  A
// panel that can be nudged is how anything landing on it gets to have
// weight.  Vertical only -- this background is exactly screen-width, so a
// horizontal scroll would wrap the text around the edge -- and clamped to
// RENDER_SHAKE_MAX, which is also the bound render_init() clears enough
// background rows to cover.  Cheap and idempotent: an unchanged offset does
// not touch the hardware at all.
#define RENDER_SHAKE_MAX 4
void render_set_shake(int offset_y);

// Redraw one cell. The hot path: a typical frame touches ~117 of 1008 cells.
void render_cell(int row, int col, cell_t c);

void render_grid(const grid_t *g);

// Paint one 6x8 glyph at a pixel origin into an arbitrary 16-bit surface.
// The single glyph loop, shared by the terminal, the sub screen's text and the
// keyboard labels. Any glyph outside the font tables renders as '?', so a
// protocol bug is visible rather than silent.
void render_glyph_into(uint16_t *target, int stride, int px, int py,
                       uint8_t glyph, uint16_t fgc, uint16_t bgc);

// Paint one Unicode codepoint into an arbitrary 16-bit surface using font6x8
// for ASCII and ui_font.h for Latin-1 accents, Kana, and CJK ideographs.
// Returns the drawn width in pixels (6 for narrow, 12 for wide CJK/Kana).
int render_unicode_into(uint16_t *target, int stride, int px, int py,
                        uint32_t cp, uint16_t fgc, uint16_t bgc);

// Paint one cell into an arbitrary 16-bit surface, so the sub screen's bitmap
// background reuses the glyph loop above rather than duplicating it.
//
// The RGB15 value of a palette index, for code painting raw pixels in the
// same colours the terminal uses -- e.g. the focus highlight in Claude's
// orange. Out-of-range indices return the default foreground.
uint16_t render_palette_rgb(uint8_t idx);

// Settings' THEME: the accent (PAL_ORANGE, Claude's clay by default) becomes
// `rgb`.  Returns 1 when that changed the colour, so the caller repaints.
int render_set_accent(uint16_t rgb);

// `apply_caret` is 0 for the sub screen: the caret belongs to the terminal, and
// a second screen inverting the same coordinates would draw a phantom one.
void render_cell_into(uint16_t *target, int stride, int apply_caret,
                      int row, int col, cell_t c);

// The top screen's own cell painter: same contract as render_cell_into, but
// at the terminal's grid cell size and font, centred on the panel.  The sub
// screen keeps the fixed 6x8 layout via render_cell_into.
void render_term_cell_into(uint16_t *target, int stride, int apply_caret,
                           int row, int col, cell_t c);

// Move (or hide) the terminal caret. The vacated cell is repainted from `g`
// and the new one drawn inverted, so a moving caret leaves no trail. A hidden
// caret draws nothing and clears whatever was under it.
void render_cursor_set(const grid_t *g, int row, int col, int visible);

// Shift the screen up by `lines` text rows and blank the vacated ones.
// Moves pixels directly instead of redrawing 1008 cells -- the server sends
// SCROLL precisely because that case is otherwise expensive.
void render_scroll(int lines);

void render_clear(void);

// A local composer replaces the terminal temporarily. It is intentionally a
// client-only overlay and is redrawn from the received grid when dismissed.
void render_overlay_text(const char *title, const char *body, const char *hint);

// Draw a terminal-sized local overlay containing one label per row. `rows` is
// allowed to be NULL when count is zero; NULL individual labels draw as empty.
// Non-NULL labels must be NUL-terminated storage owned by the caller. At most
// the visible list region is read, every label is clipped to the terminal
// width, and a bad selected index simply leaves every row unselected.
// When `focused` is non-zero, the selected row is filled with Claude's orange
// and uses dark text. Without focus, selection remains data for the caller but
// is intentionally not painted as an active target.
void render_overlay_list(const char *title, const char * const *rows,
                         int count, int selected, int focused,
                         const char *hint);

// Paint a grid the client composed, over the terminal, in the terminal's own
// cells.  See project.h for why the PROJECT page is a grid rather than a
// bespoke layout.  Close it with render_overlay_close() like any other.
void render_overlay_grid(const grid_t *g);

// A box with an accent border and `count` lines of UI-font text, `pitch`
// pixels apart, each in its own colour, over the overlay already up.  Drawn
// only if it fits the screen.
void render_overlay_box(int x, int y, int w, int h, const char *const *lines,
                        const uint16_t *colours, int count, int pitch);

void render_overlay_close(const grid_t *g);
int render_overlay_active(void);

#endif  // DSTERM_RENDER_H
