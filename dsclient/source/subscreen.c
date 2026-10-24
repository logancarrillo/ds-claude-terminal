#include "subscreen.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "chime.h"
#include "ext.h"
#include "focus.h"
#include "guide.h"
#include "kbd.h"
#include "lang.h"
#include "project.h"
#include "mirror.h"
#include "privacy.h"
#include "log.h"
#include "render.h"
#include "sessions.h"
#include "sound.h"
#include "status.h"

// The sub engine has no LCD framebuffer mode -- MODE_FB0 is main-engine only --
// so this is an extended-rotoscale 16-bit bitmap background, 256x256. Written
// exactly like the top screen's framebuffer, so the 6x8 grid and the ORIGIN_X
// offset carry over: 42 cols x 24 rows at 6x8 fills 256x192 of it.
#define SUB_W 256
#define SUB_H 256

// Tile shades, shared by the keyboard keys and the larger Control tiles. Keys
// read as light-on-dark with the 1px gaps as separators; a held key inverts to
// a bright tile with dark text so a press is visible in a photograph.
//
// Restrained on purpose: the highlight is a warm amber rather than white, one
// accent against a near-black screen. The DS Phat/Lite panels are dim and a
// saturated palette blooms, and a status screen that has to be read at a glance
// while something is running is worse for every colour added to it.
#define KBD_TILE    ARGB16(1, 5, 6, 8)
#define KBD_TILE_HI ARGB16(1, 27, 22, 12)
#define KBD_TEXT    ARGB16(1, 24, 25, 27)
#define KBD_TEXT_HI ARGB16(1, 2, 2, 2)
#define SUB_BG      ARGB16(1, 0, 0, 0)
// The Control tiles' small second line, and the bevel that makes a tile read
// as something to press rather than a box drawn round a word: one row of
// light along the top, one of shadow along the bottom.
#define TILE_SUB    ARGB16(1, 14, 15, 17)
#define TILE_LIGHT  ARGB16(1, 9, 10, 13)
#define TILE_SHADE  ARGB16(1, 2, 2, 3)
// Text that explains rather than reports: the button legend.
#define HINT_DIM    ARGB16(1, 13, 13, 14)

// What the face buttons do now (subscreen_set_button_legend), for the
// Control screen's legend and the approval hint.
static char legend_buttons[128] = "A enter  B delete  X esc  Y tab";
static char legend_enter[8] = "A";
static char legend_stop[8] = "START";
static char legend_tab[8] = "Y";


static uint16_t *sub;
// The lower background and where the shake has it, mirroring the top
// screen's pair for the same reason: a landing down here should move this
// panel, not the other one.
static int sub_bg_id;
static int sub_shake_y;
static int composer_active;
static int composer_add_quick;
static int voice_ready;
static int voice_recording;
static int voice_latched;
static int voice_waiting;
static unsigned voice_bars;
static int quick_active;
static int quick_level;
static int quick_selected;
static int quick_top;
static const quick_commands_t *quick_cmds;
static const quick_saved_t *quick_saved_cmds;
static int quick_remove_armed;
static const quick_recent_t *quick_recent;
// Whether this card has a camera (a DSi): QUICK's PHOTO row exists only
// where there is a sensor behind it.
static int quick_camera;
// The camera screen: the two strips' text, pushed in composed.  The rows
// between them are the viewfinder's, written by subscreen_camera_blit() and
// never touched by the layout draw.
static int camera_active;
static int camera_error;
static char camera_top[LOG_COLS + 1];
static char camera_hint[LOG_COLS + 1];
static char claude_mode[QUICK_MODE_MAX + 1];
static int rewind_on;
static int remote_active;
static const remote_list_t *remote_list;
static const remote_computers_t *remote_comps;
static const remote_ui_t *remote_ui_shown;
static const char *remote_here;
// The clipboard, as the STATUS screen's CLIP tab should show it. Pushed in
// like everything else here: main composes the summary and the wrapped lines,
// this module only decides where they go.
static char clip_summary_line[CLIP_LINE_COLS + 1];
static char clip_lines[SUBSCREEN_CLIP_LINES_MAX][CLIP_LINE_COLS + 1];
static unsigned clip_line_count;
static int clip_can_paste;

static settings_t settings_shown;
static settings_ui_t settings_ui_shown;
static project_action_t project_action_list[PROJECT_ACTIONS_MAX];
static unsigned project_action_count;
static unsigned project_action_highlight;
static unsigned project_action_dropped;
static int project_god_mode;
static char project_lines[SUBSCREEN_PROJECT_LINES_MAX][44];
static unsigned project_line_count;
static unsigned project_tab_sel;
void subscreen_set_shake(int offset_y)
{
    int dy = offset_y;
    if (dy > (int)RENDER_SHAKE_MAX) dy = (int)RENDER_SHAKE_MAX;
    if (dy < -(int)RENDER_SHAKE_MAX) dy = -(int)RENDER_SHAKE_MAX;
    if (dy == sub_shake_y) return;
    sub_shake_y = dy;
    bgSetScroll(sub_bg_id, 0, dy);
    bgUpdate();
}

static cell_t text_cell(uint8_t glyph, uint8_t fg)
{
    cell_t c = {glyph, (uint8_t)((fg << 4) | PAL_BLACK), 0};
    return c;
}

// The bottom screen as text, for the computer's view (mirror.h): every
// glyph and fill below is recorded here as it is drawn.
static mirror_t sub_mirror;

const mirror_t *subscreen_mirror(void) { return &sub_mirror; }

// A glyph at a pixel position: drawn, and recorded in the mirror.
static void glyph_px(int x, int y, uint8_t glyph, uint16_t fg, uint16_t bg)
{
    render_glyph_into(sub, SUB_W, x, y, glyph, fg, bg);
    mirror_glyph_px(&sub_mirror, x, y, glyph, fg, bg);
}

// One character (ASCII, raw extended wire glyph 0x80..0xBF, or UTF-8
// codepoint >= 0x80 from the UI glyph atlas) at pixel (x, y).  Returns the
// pixel width drawn (6 for narrow, 12 for wide CJK/Kana).
static int next_char_px(const char **p, int x, int y, uint16_t fg, uint16_t bg)
{
    const uint8_t b0 = (uint8_t)**p;
    if (b0 >= 0x80 && b0 < 0xC0) {
        (*p)++;
        glyph_px(x, y, b0, fg, bg);
        return 6;
    }
    const uint32_t cp = lang_utf8_next(p);
    if (cp <= 0x7E) {
        glyph_px(x, y, (uint8_t)cp, fg, bg);
        return 6;
    }
    const int w = render_unicode_into(sub, SUB_W, x, y, cp, fg, bg);
    mirror_glyph_px(&sub_mirror, x, y, '?', fg, bg);
    if (w > 6) mirror_glyph_px(&sub_mirror, x + 6, y, '?', fg, bg);
    return w;
}

// A filled rectangle, likewise.
static void fill_px(int x, int y, int w, int h, uint16_t colour)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) sub[yy * SUB_W + xx] = colour;
    mirror_fill_px(&sub_mirror, x, y, w, h, colour);
}

// One 6x8 cell. apply_caret is 0: the sub screen must never take the caret
// inversion, which belongs to the main terminal.
static void paint_char(int row, int col, uint8_t glyph, uint8_t fg)
{
    render_cell_into(sub, SUB_W, 0, row, col, text_cell(glyph, fg));
    mirror_glyph_px(&sub_mirror, col * 6, row * 8, glyph,
                    render_palette_rgb(fg), render_palette_rgb(PAL_BLACK));
}

// A whole line, space-padded to the full width so a shorter line never leaves
// stale glyphs from the previous frame on the row.
static void draw_line(int row, const char *s, uint8_t fg)
{
    const char *p = privacy_text(s);
    const uint16_t fgc = render_palette_rgb(fg);
    const uint16_t bgc = render_palette_rgb(PAL_BLACK);
    int col = 0;
    while (col < LOG_COLS && *p) {
        if ((uint8_t)*p < 0x80) {
            paint_char(row, col++, (uint8_t)*p++, fg);
            continue;
        }
        const int w = next_char_px(&p, col * 6, row * 8, fgc, bgc);
        col += w > 6 ? 2 : 1;
    }
    for (; col < LOG_COLS; col++) paint_char(row, col, ' ', fg);
}

// The same row, in two colours: everything before `split` in `fg`, everything
// from it on in `fg2`.  One line, one row, no layout of its own -- the split
// exists only so the quip can be told apart from the state in front of it.
static void draw_line_split(int row, const char *s, uint8_t fg,
                            unsigned split, uint8_t fg2)
{
    const char *p = privacy_text(s);
    const uint16_t bgc = render_palette_rgb(PAL_BLACK);
    int col = 0;
    while (col < LOG_COLS && *p) {
        const uint8_t cur_fg = (unsigned)col >= split ? fg2 : fg;
        if ((uint8_t)*p < 0x80) {
            paint_char(row, col++, (uint8_t)*p++, cur_fg);
            continue;
        }
        const int w = next_char_px(&p, col * 6, row * 8,
                                   render_palette_rgb(cur_fg), bgc);
        col += w > 6 ? 2 : 1;
    }
    for (; col < LOG_COLS; col++) paint_char(row, col, ' ', fg);
}

// Whether `s` has a glyph taller than the 8-row text line: the CJK atlas's
// are ten rows (ink in the lower nine), so a line holding one needs one more
// pixel row below it than the 8-pixel grid gives, and two such lines stacked
// on adjacent rows run into each other.  Latin, accented Latin and the wire
// glyphs are all eight rows.
static int text_tall(const char *s)
{
    const char *p = s ? s : "";
    while (*p) {
        const uint8_t b0 = (uint8_t)*p;
        if (b0 < 0xC0) {            // ASCII, or a raw wire glyph
            p++;
            continue;
        }
        if (lang_cp_is_wide(lang_utf8_next(&p))) return 1;
    }
    return 0;
}

// The pitch for lines stacked one under another: the 8-pixel grid, or ten
// pixels -- nine of glyph and one clear -- when any of them is tall.  English
// never takes the second branch, so its pixels stay on the grid.
#define TALL_PITCH 10

// A full-width line at pixel row `y` rather than on the 8-pixel grid, in an
// arbitrary colour: for the lines that move off the grid when they are tall.
static void draw_line_at(int y, const char *s, uint16_t fg)
{
    const char *p = privacy_text(s);
    int col = 0;
    while (col < LOG_COLS && *p) {
        const int w = next_char_px(&p, col * 6, y, fg, SUB_BG);
        col += w > 6 ? 2 : 1;
    }
    for (; col < LOG_COLS; col++) glyph_px(col * 6, y, ' ', fg, SUB_BG);
}

// A selected row needs a fill, not merely orange letters: on a dim panel an
// outline disappears among terminal text.  This stays cell-aligned so the
// same 42-column clipping rule as draw_line() applies to every session name.
static void draw_selected_px(int y, const char *s, int selected)
{
    const uint16_t bg = selected ? render_palette_rgb(PAL_ORANGE) : SUB_BG;
    const uint8_t fg = selected ? PAL_BLACK : PAL_DEFAULT;
    const uint16_t fgc = render_palette_rgb(fg);
    const char *p = s ? privacy_text(s) : "";
    // A CJK glyph's cell is ten rows, one above and one below the line's
    // eight (render_unicode_into), and its lowest row is ink: a bar of eight
    // would leave that row black on black.  The bar takes the glyph's cell.
    if (selected && text_tall(p)) {
        if (y > 0) fill_px(0, y - 1, LOG_COLS * 6, 1, bg);
        if (y + 8 < LOG_ROWS * 8) fill_px(0, y + 8, LOG_COLS * 6, 1, bg);
    }
    int col = 0;
    while (col < LOG_COLS && *p) {
        const int w = next_char_px(&p, col * 6, y, fgc, bg);
        col += w > 6 ? 2 : 1;
    }
    for (; col < LOG_COLS; col++) {
        glyph_px(col * 6, y, ' ', fgc, bg);
    }
}

static void draw_selected_line(int row, const char *s, int selected)
{
    draw_selected_px(row * 8, s, selected);
}

void subscreen_draw_line(int row, const char *s, uint8_t fg)
{
    draw_line(row, s, fg);
}

void subscreen_draw_line_split(int row, const char *s, uint8_t fg,
                               unsigned split, uint8_t fg2)
{
    draw_line_split(row, s, fg, split, fg2);
}

void subscreen_draw_selected_line(int row, const char *s, int selected)
{
    draw_selected_line(row, s, selected);
}

// The newest (bottom - top + 1) lines of the log, newest at `bottom`, blank
// above once the log has fewer lines than the region.
static void draw_log_region(int top, int bottom)
{
    const int n = log_count();
    for (int r = top; r <= bottom; r++) {
        const int idx = n - (bottom - r) - 1;   // newest line lands on `bottom`
        draw_line(r, idx < 0 ? "" : log_get(idx), PAL_DEFAULT);
    }
}

// Raw-pixel tile shared by keyboard keys, the scroll controls, and the Control
// screen's buttons. Raw pixels rather than cells because none of them are 6x8
// multiples: each is a filled rectangle with its label centred.
static void draw_tile(int x, int y, int w, int h, const char *label,
                     int held, int focused)
{
    // Focus is painted in Claude's own orange -- palette index 9, the colour
    // of his mark and box rules -- so the lit item reads as part of the thing
    // this device is driving. A fill, not an outline: it has to read at a
    // glance from arm's length on a dim panel, and nothing subtle does.
    const uint16_t tile = focused ? render_palette_rgb(PAL_ORANGE)
                                  : held ? KBD_TILE_HI : KBD_TILE;
    const uint16_t text = (focused || held) ? KBD_TEXT_HI : KBD_TEXT;
    fill_px(x, y, w, h, tile);
    // Never wider than the tile: a label that does not fit is cut on a whole
    // character rather than painted over the neighbour.  (Translations are
    // written to fit -- tests/test_core.c measures them -- so this is the
    // backstop for a custom .lang pack.)
    char fit[96];
    const int cols = (int)lang_pad_cols(label, 0u, (unsigned)(w >= 6 ? w / 6 : 1),
                                        fit, sizeof(fit));
    int tx = x + (w - cols * 6) / 2;
    const int ty = y + (h - 8) / 2;
    const char *p = fit;
    while (*p) {
        tx += next_char_px(&p, tx, ty, text, tile);
    }
}

// A Control tile: the destination in capitals and, under it, what it is for
// in a quieter grey -- so a person who has never held the card can tell
// SESSIONS from REMOTE without opening both.  Focus and held states are the
// plain tile's, so the d-pad highlight looks the same everywhere.
static void draw_text_px(int x, int y, const char *text, uint16_t fg,
                         uint16_t bg)
{
    const char *p = text ? text : "";
    while (*p) {
        x += next_char_px(&p, x, y, fg, bg);
    }
}

static void draw_tile2(int x, int y, int w, int h, const char *label,
                       const char *sub_label, int held, int focused)
{
    const int lit = focused || held;
    const uint16_t tile = focused ? render_palette_rgb(PAL_ORANGE)
                                  : held ? KBD_TILE_HI : KBD_TILE;
    for (int yy = y; yy < y + h; yy++) {
        const uint16_t c = lit ? tile : yy == y ? TILE_LIGHT
                                      : yy == y + h - 1 ? TILE_SHADE : tile;
        for (int xx = x; xx < x + w; xx++) sub[yy * SUB_W + xx] = c;
    }
    mirror_fill_px(&sub_mirror, x, y, w, h, tile);
    const int has_sub = sub_label && sub_label[0];
    const int ly = y + (h - 8) / 2 - (has_sub ? 5 : 0);
    // Both lines kept two pixels in from each side (draw_tile's backstop).
    const unsigned max_cols = (unsigned)(w > 10 ? (w - 4) / 6 : 1);
    char fit[96];
    const int lcols = (int)lang_pad_cols(label, 0u, max_cols, fit, sizeof(fit));
    draw_text_px(x + (w - lcols * 6) / 2, ly, fit,
                 lit ? KBD_TEXT_HI : KBD_TEXT, tile);
    if (has_sub) {
        const int scols = (int)lang_pad_cols(sub_label, 0u, max_cols, fit,
                                             sizeof(fit));
        draw_text_px(x + (w - scols * 6) / 2, ly + 11, fit,
                     lit ? KBD_TEXT_HI : TILE_SUB, tile);
    }
}

// One full-width row in an arbitrary colour, for the quieter text the palette
// has no entry for.
static void draw_line_rgb(int row, const char *s, uint16_t fg)
{
    const char *p = privacy_text(s);
    int col = 0;
    while (col < LOG_COLS && *p) {
        const int w = next_char_px(&p, col * 6, row * 8, fg, SUB_BG);
        col += w > 6 ? 2 : 1;
    }
    for (; col < LOG_COLS; col++) {
        glyph_px(col * 6, row * 8, ' ', fg, SUB_BG);
    }
}

// PROJECT: the tab strip and the selected tab's lines.
//
// The content proper is on the top screen, where there is room to read it.
// This half exists because the top screen cannot be touched, so the tabs and
// the buttons have to live down here -- which is the natural shape for the
// device rather than a compromise.
static void draw_project_screen(void)
{
    char line[LOG_COLS + 1];
    status_format_link(line);
    draw_line(0, line, PAL_WHITE);
    // Row 1 is the extension's when it has a line for it, same as on Control:
    // this is a screen you sit on for a while.
    if (!ext_subscreen_draw(EXT_DRAW_PROJECT_STATUS)) {
        status_format_view_pos(line);
        draw_line(1, line, PAL_DEFAULT);
    }

    // Rows 2--4 are the tab strip's pixels.  Blank them as rows first so the
    // two pixel rows above the tiles are clean, then draw the tiles over the
    // top; the strip is inset by the same two pixels the Control tiles use.
    for (int row = 2; row <= 4; row++) draw_line(row, "", PAL_DEFAULT);
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        int x, y, w, h;
        if (!project_tab_rect((project_tab_t)i, &x, &y, &w, &h)) continue;
        draw_tile(x, y, w, h, project_tab_name((project_tab_t)i), 0,
                  (unsigned)i == project_tab_sel);
    }

    if (project_tab_sel == PROJECT_TAB_DO) {
        // The DO tab spends this half on buttons instead of text.  The command
        // each one types is on the top screen next to its label, so nothing is
        // pressed unread unless somebody deliberately armed that.
        for (int row = 5; row <= 19; row++) draw_line(row, "", PAL_DEFAULT);
        for (unsigned i = 0; i < project_action_count; i++) {
            int x, y, w, h;
            if (!project_action_rect(i, &x, &y, &w, &h)) continue;
            draw_tile(x, y, w, h, project_action_list[i].label, 0,
                      i == project_action_highlight);
        }
        if (project_god_mode) {
            draw_line(18, tr(TR_PROJECT_DO_GOD), PAL_RED);
        } else {
            draw_line(18, tr(TR_PROJECT_DO_SAFE), PAL_DEFAULT);
        }
        // Never a silent cap: a truncated list that does not say so reads as
        // the whole list.
        if (project_action_dropped) {
            char note[LOG_COLS + 1];
            (void)snprintf(note, sizeof(note), "%u more not shown",
                           project_action_dropped);
            draw_line(19, note, PAL_ORANGE);
        }
    } else {
        for (int row = 5; row <= 19; row++) {
            const unsigned i = (unsigned)(row - 5);
            draw_line(row, i < project_line_count ? project_lines[i] : "",
                      PAL_DEFAULT);
        }
    }
    // Rows 20--23 stay genuinely empty, exactly as on Control: that strip is
    // left clear for whatever an extension puts there, and a sentence in it
    // would be a sentence something stands on.
    for (int row = 20; row <= 23; row++) draw_line(row, "", PAL_DEFAULT);
}

// The settings rows, as a label and a strip of pickable chips.
//
// The version this replaced drew one fat tile per row whose value advanced by
// one step every time you pressed it, so choosing the third of four options
// meant pressing a button three times and watching a word change. You could
// not see the choices, only the one you were on. Chips fix that: every option
// is on the screen, and the one that is set is lit.
//
// Nothing here decides where a chip goes -- settings.c computes each rect from
// its own label, and this draws what that says. The alternative is two lists
// of coordinates, which is how a button comes to be somewhere other than where
// it looks.
static void draw_settings_screen(void)
{
    for (int row = 0; row <= 23; row++) draw_line(row, "", PAL_DEFAULT);

    // The five tabs, on the line the page title used to have to itself, and
    // how to move between them.  The tab on screen is the highlighted row's.
    const unsigned tab = settings_row_tab((settings_row_t)settings_ui_shown.row);
    int tabs_right = SETTINGS_TAB_X;
    for (unsigned t = 0; t < SETTINGS_TAB_COUNT; t++) {
        int tx, ty, tw, th;
        if (!settings_tab_rect(t, &tx, &ty, &tw, &th)) continue;
        draw_tile(tx, ty, tw, th, settings_tab_label(t), t == tab, 0);
        tabs_right = tx + tw;
    }
    static const char hint[] = "L/R";
    const int hint_x = SUB_W - 4 - (int)(sizeof(hint) - 1) * 6;
    if (hint_x > tabs_right + 2) {
        for (int c = 0; hint[c]; c++) {
            glyph_px(hint_x + c * 6, 4, (uint8_t)hint[c],
                              HINT_DIM, SUB_BG);
        }
    }

    const unsigned scroll = settings_ui_shown.scroll;
    for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
        const settings_row_t row = (settings_row_t)i;
        if (settings_row_tab(row) != tab) continue;
        int x, y, w, h;
        if (!settings_row_rect(row, scroll, &x, &y, &w, &h)) continue;
        const int row_selected = (unsigned)i == settings_ui_shown.row;

        // The label is text on the background rather than a tile of its own:
        // it is not pressable, and drawing it as a button would say it was.
        // Cut to the option column, so a long label from a custom pack ends
        // before the chips instead of running under them.
        char label[96];
        const int label_cols = (settings_row_option_x(row) - x - 6) / 6;
        (void)lang_pad_cols(settings_row_label(row), 0u,
                            (unsigned)(label_cols > 1 ? label_cols : 1),
                            label, sizeof(label));
        draw_text_px(x, y + (h - 8) / 2, label,
                     render_palette_rgb(row_selected ? PAL_ORANGE : PAL_DEFAULT),
                     SUB_BG);

        const unsigned count = settings_option_count(row);
        const unsigned current = settings_option_current(&settings_shown, row);
        for (unsigned o = 0; o < count; o++) {
            int cx, cy, cw, ch;
            if (!settings_option_rect(row, o, scroll, &cx, &cy, &cw, &ch)) continue;
            // The DEFAULTS chip is the only one that renames itself, and only
            // while its confirm is pending: a destructive button that looks
            // identical armed and unarmed is a button you press twice by
            // accident.
            const char *text = settings_option_label(row, o);
            if (row == SETTINGS_ROW_DEFAULTS &&
                settings_ui_shown.reset_armed &&
                settings_ui_shown.row == (unsigned)row) {
                text = tr(TR_SETTINGS_RESET_SURE);
            }
            // Two different kinds of highlight, because they mean two
            // different things: `held` marks the option that is *set*, and the
            // orange focus fill marks the one the d-pad is *on*. On a value
            // row they are usually the same chip; on an action row nothing is
            // ever set, so only the focus fill appears.
            const int is_current = o == current;
            const int is_focused = row_selected && o == settings_ui_shown.option;
            draw_tile(cx, cy, cw, ch, text, is_current, is_focused);
        }
    }

    // A tab longer than the screen: arrows at the right edge say there is
    // more, and touching one scrolls.
    if (settings_more_above(scroll)) {
        glyph_px(SETTINGS_ARROW_X + 2,
                          SETTINGS_ROW_Y + (SETTINGS_ROW_H - 8) / 2, (uint8_t)'^',
                          render_palette_rgb(PAL_ORANGE), SUB_BG);
    }
    if (settings_more_below(tab, scroll)) {
        glyph_px(SETTINGS_ARROW_X + 2,
                          SETTINGS_ROW_Y + (SETTINGS_VISIBLE_ROWS - 1) * SETTINGS_ROW_PITCH +
                              (SETTINGS_ROW_H - 8) / 2,
                          (uint8_t)'v', render_palette_rgb(PAL_ORANGE), SUB_BG);
    }
}

// The STATUS screen: a tab strip, then either the log or the clipboard.
//
// The two counter rows at the bottom are drawn by the caller and stay put on
// both tabs -- the L<number> line a photograph checks to confirm which build
// is running has to be there whatever you are looking at.
static void draw_status_screen(void)
{
    const status_tab_t tab = status_tab_get();

    // Rows 0 and 1 are the strip. Clear them as text first so a shorter tab
    // label cannot leave the previous one's glyphs showing beside it.
    draw_line(0, "", PAL_DEFAULT);
    draw_line(1, "", PAL_DEFAULT);
    for (int i = 0; i < STATUS_TAB_COUNT; i++) {
        int x, y, w, h;
        if (!status_tab_rect((status_tab_t)i, &x, &y, &w, &h)) continue;
        draw_tile(x, y, w, h, status_tab_name((status_tab_t)i),
                  0, (status_tab_t)i == tab);
    }

    if (tab == STATUS_TAB_LOG) {
        draw_log_region(2, LOG_ROWS - 3);
        return;
    }

    for (int row = 2; row <= LOG_ROWS - 3; row++) draw_line(row, "", PAL_DEFAULT);
    // Row 2 is what the clipboard is and what was done to it, in orange
    // because it is the line that says whether what follows is the whole
    // thing. Rows 4.. are the text itself.
    draw_line(2, clip_summary_line, PAL_ORANGE);
    for (unsigned i = 0; i < clip_line_count; i++) {
        const int row = 4 + (int)i;
        if (row > LOG_ROWS - 5) break;
        draw_line(row, clip_lines[i], PAL_DEFAULT);
    }

    int ax, ay, aw, ah;
    status_clip_action_rect(&ax, &ay, &aw, &ah);
    // Drawn even when there is nothing to paste, and drawn plainly then. A
    // button that vanishes is a button somebody looks for; one that is visibly
    // inert says the clipboard is empty, which is the fact they wanted.
    draw_tile(ax, ay, aw, ah,
              clip_can_paste ? tr(TR_STATUS_PASTE) : tr(TR_STATUS_NO_PASTE),
              0, 0);
}

static void draw_keyboard(void)
{
    const kbd_layer_t layer = kbd_layer();
    const int focused = focus_active() && focus_place() == FOCUS_KEYS;
    for (int id = 0; id < kbd_count(); id++) {
        int x, y, w, h;
        kbd_key_rect(id, layer, &x, &y, &w, &h);
        draw_tile(x, y, w, h, kbd_label(id, layer),
                  id == kbd_held(), focused && id == focus_key_id());
    }
}

// The keyboard screen's own state line, drawn above the keys rather than
// below them.
//
// Three rows used to sit under the bottom key row: the view position, the
// modifier state, and a line of button guides.  They are gone, because that
// strip is the only floor an extension has while the keyboard is up, and a
// companion who can only exist on one screen is not much of one.  The guides
// went entirely -- they taught two chords, and a chord you have learned is a
// line you are only reading past.  The state did not go: which layer you are
// on and whether Ctrl is armed both change what the next key does, so they
// move up here onto a row that was already status.
static void draw_keyboard_state(int row)
{
    const char *layer = kbd_layer() == KBD_LAYER_SHIFT ? "SHIFT" :
                        kbd_layer() == KBD_LAYER_SYM ? "SYM" : "BASE";
    char view[LOG_COLS + 1], line[LOG_COLS + 1];
    status_format_view_pos(view);
    // The modifier prefix is at most 18 columns ("SHIFT CTRL SHIFT" plus the
    // gap), so bounding the view field at 24 proves the whole line fits the
    // 42-column row rather than relying on the view text staying short.
    snprintf(line, sizeof(line), "%s%s%s  %.24s", layer,
             kbd_ctrl_armed() ? " CTRL" : "", kbd_shift_armed() ? " SHIFT" : "",
             view);
    // Focus mode keeps its colour: while it is on a physical direction moves
    // the highlight instead of the cursor, and that has to read at a glance
    // rather than be inferred from a lit key.
    draw_line(row, line, focus_active() ? PAL_ORANGE : PAL_DEFAULT);
}

// Everything below the bottom key row is left clear, for an extension to
// stand on.  Keeping the clear explicit means a stale glyph from another
// screen can never end up underfoot looking like terrain.
static void clear_keyboard_floor(void)
{
    for (int row = KBD_FLOOR_ROW; row < LOG_ROWS; row++) {
        draw_line(row, "", PAL_DEFAULT);
    }
}

// QUICK's main list: two text rows per entry, so each is a thumb's height.
#define QUICK_FIRST_LINE 3
#define QUICK_LIST_FIRST_LINE 2

// A title and a hint under it, on rows 0 and 1 -- or, when either is tall,
// at y = 0 and y = TALL_PITCH, which still ends (y 18) above row 3.
static void draw_head2(const char *title, const char *hint)
{
    if (!text_tall(title) && !text_tall(hint)) {
        draw_line(0, title, PAL_ORANGE);
        draw_line_rgb(1, hint, HINT_DIM);
        return;
    }
    for (int line = 0; line <= 2; line++) draw_line(line, "", PAL_DEFAULT);
    draw_line_at(0, title, render_palette_rgb(PAL_ORANGE));
    draw_line_at(TALL_PITCH, hint, HINT_DIM);
}

static void draw_quick_main(void)
{
    for (int line = 2; line < LOG_ROWS; line++) draw_line(line, "", PAL_DEFAULT);
    draw_head2(tr(TR_QUICK_HEAD), tr(TR_QUICK_SUB));
    const int n = (int)quick_row_count(quick_camera);
    unsigned label_cols = 9u;
    for (int i = 0; i < n; i++) {
        const unsigned c = lang_display_cols(quick_row_label((quick_row_t)i));
        if (c > label_cols) label_cols = c;
    }
    if (label_cols > 12u) label_cols = 12u;

    for (int i = 0; i < n; i++) {
        char lbl[48], note[96], row[176];
        const quick_row_t r = (quick_row_t)i;
        (void)lang_pad_cols(quick_row_label(r), label_cols, label_cols,
                            lbl, sizeof(lbl));
        if (r == QUICK_ROW_MODE) {
            char mode_col[16];
            (void)lang_pad_cols(claude_mode[0] ? claude_mode : "--", 8u, 8u,
                                mode_col, sizeof(mode_col));
            const unsigned rem = 42u - 1u - label_cols - 1u - 8u - 1u;
            (void)lang_pad_cols(quick_row_note(r), 0u, rem, note, sizeof(note));
            snprintf(row, sizeof(row), " %s %s %s", lbl, mode_col, note);
        } else if ((r == QUICK_ROW_COMMANDS && quick_cmds) ||
                   (r == QUICK_ROW_AGAIN && quick_recent)) {
            const unsigned cnt = (r == QUICK_ROW_COMMANDS)
                                     ? (quick_cmds->count +
                                        (quick_saved_cmds ? quick_saved_cmds->count : 0u))
                                     : quick_recent->count;
            const unsigned note_w = 42u - 1u - label_cols - 1u - 1u - 2u;
            (void)lang_pad_cols(quick_row_note(r), note_w, note_w,
                                note, sizeof(note));
            snprintf(row, sizeof(row), " %s %s %2u", lbl, note, cnt);
        } else {
            const unsigned rem = 42u - 1u - label_cols - 1u;
            (void)lang_pad_cols(quick_row_note(r), 0u, rem, note, sizeof(note));
            snprintf(row, sizeof(row), " %s %s", lbl, note);
        }
        draw_selected_line(QUICK_FIRST_LINE + 2 * i, row, i == quick_selected);
    }
    // The last row's ninth pixel row would be y 192, off the panel: a tall
    // footer goes up one.
    const char *foot = tr(TR_QUICK_FOOT);
    if (!text_tall(foot)) {
        draw_line_rgb(LOG_ROWS - 1, foot, HINT_DIM);
    } else {
        draw_line_at(LOG_ROWS * 8 - 9, foot, HINT_DIM);
    }
}

// Where COMMANDS' and AGAIN's list starts: row 2, or -- when the two header
// lines are tall and need y 0..18 between them (draw_head2) -- four pixels
// lower.  The list still ends (y 180) above the bottom of the panel.
static int quick_list_y0(int level)
{
    const int commands = level == SUBSCREEN_QUICK_COMMANDS;
    const int tall = commands
        ? text_tall(tr(TR_QUICK_CMD_HEAD)) || text_tall(tr(TR_QUICK_CMD_SUB))
        : text_tall(tr(TR_QUICK_AGAIN_HEAD)) || text_tall(tr(TR_QUICK_AGAIN_SUB));
    return QUICK_LIST_FIRST_LINE * 8 + (tall ? 4 : 0);
}

static void draw_quick_list(void)
{
    const int commands = quick_level == SUBSCREEN_QUICK_COMMANDS;
    const unsigned count = commands ? quick_commands_total(quick_cmds, quick_saved_cmds)
                                    : (quick_recent ? quick_recent->count : 0);
    quick_cmd_kind_t sel_kind = QUICK_CMD_NONE;
    if (commands && quick_selected >= 0) {
        sel_kind = quick_commands_entry(quick_cmds, quick_saved_cmds,
                                        (unsigned)quick_selected, NULL, NULL);
    }
    for (int line = QUICK_LIST_FIRST_LINE; line < LOG_ROWS; line++) {
        draw_line(line, "", PAL_DEFAULT);
    }
    if (commands) {
        draw_head2(sel_kind == QUICK_CMD_SAVED
                       ? (quick_remove_armed ? tr(TR_QUICK_CMD_HEAD_SURE)
                                             : tr(TR_QUICK_CMD_HEAD_SAVED))
                       : sel_kind == QUICK_CMD_ADD ? tr(TR_QUICK_CMD_HEAD_ADD)
                                                   : tr(TR_QUICK_CMD_HEAD),
                   tr(TR_QUICK_CMD_SUB));
    } else {
        draw_head2(tr(TR_QUICK_AGAIN_HEAD), tr(TR_QUICK_AGAIN_SUB));
    }
    const int y0 = quick_list_y0(quick_level);
    // Bottom to top: a tall glyph's lowest row is the next line's first, so
    // the line under it has to be painted before it, not after.
    for (int i = SUBSCREEN_QUICK_LIST_ROWS - 1; i >= 0; i--) {
        const int index = quick_top + i;
        if (index < 0 || (unsigned)index >= count) continue;
        char row[LOG_COLS + 1];
        if (commands) {
            const char *entry = "";
            (void)quick_commands_entry(quick_cmds, quick_saved_cmds,
                                       (unsigned)index, &entry, NULL);
            char text[LOG_COLS];
            quick_recent_row(entry, text, LOG_COLS - 2);
            snprintf(row, sizeof(row), " %s", text);
        } else {
            char text[LOG_COLS];
            quick_recent_row(quick_recent->items[index], text, LOG_COLS - 2);
            snprintf(row, sizeof(row), " %s", text);
        }
        draw_selected_px(y0 + 8 * i, row, index == quick_selected);
    }
    if (!commands && count == 0) {
        const char *l0 = tr(TR_QUICK_AGAIN_EMPTY_0);
        const char *l1 = tr(TR_QUICK_AGAIN_EMPTY_1);
        const int pitch = text_tall(l0) || text_tall(l1) ? TALL_PITCH : 8;
        char line[2][LOG_COLS * 3 + 2];
        snprintf(line[0], sizeof(line[0]), " %s", l0);
        snprintf(line[1], sizeof(line[1]), " %s", l1);
        if (pitch == 8) {
            draw_line(QUICK_LIST_FIRST_LINE + 1, line[0], PAL_DEFAULT);
            draw_line(QUICK_LIST_FIRST_LINE + 2, line[1], PAL_DEFAULT);
        } else {
            draw_line_at(y0 + 8, line[0], render_palette_rgb(PAL_DEFAULT));
            draw_line_at(y0 + 8 + pitch, line[1], render_palette_rgb(PAL_DEFAULT));
        }
    }
}

int subscreen_quick_row_at(int level, int top, int py)
{
    const int line = py / 8;
    if (level == SUBSCREEN_QUICK_MAIN) {
        const int i = (line - QUICK_FIRST_LINE + 1) / 2;
        if (line < QUICK_FIRST_LINE - 1 || i < 0 ||
            i >= (int)quick_row_count(quick_camera)) return -1;
        return i;
    }
    const int y0 = quick_list_y0(level);
    if (py < y0) return -1;
    const int row = (py - y0) / 8;
    if (row >= SUBSCREEN_QUICK_LIST_ROWS) return -1;
    return top + row;
}

// REMOTE: a title, the server's last word on a pick, then the rows.  The
// explanation of the selected row is on the top screen (main.c), where there
// is room to say what A will do before it is pressed.
#define REMOTE_FIRST_ROW 2

int subscreen_remote_row_at(int py)
{
    const int row = py / 8 - REMOTE_FIRST_ROW;
    return row >= 0 && row < LOG_ROWS - REMOTE_FIRST_ROW ? row : -1;
}

static void draw_remote_menu(void)
{
    draw_line(0, tr(TR_REMOTE_HEAD), PAL_ORANGE);
    draw_line(1, remote_list && remote_list->note[0] ? remote_list->note : "",
              PAL_YELLOW);
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(remote_list, remote_comps, rows,
                                   REMOTE_ROWS_MAX);
    const int selected = remote_ui_shown ? remote_ui_shown->selection : -1;
    int line = REMOTE_FIRST_ROW;
    for (unsigned i = 0; i < n && line < LOG_ROWS; i++, line++) {
        char text[LOG_COLS + 1];
        remote_row_text(remote_list, remote_comps, &rows[i], remote_here, text,
                        LOG_COLS);
        if ((int)i == selected) {
            draw_selected_line(line, text, 1);
        } else if (rows[i].kind == REMOTE_ROW_HEADER) {
            draw_line(line, text, PAL_ORANGE);
        } else if (rows[i].kind == REMOTE_ROW_SESSION && remote_list &&
                   remote_list->items[rows[i].index].state ==
                       REMOTE_STATE_NEEDS_YOU) {
            draw_line(line, text, PAL_YELLOW);
        } else {
            draw_line(line, text, PAL_DEFAULT);
        }
    }
    for (; line < LOG_ROWS; line++) draw_line(line, "", PAL_DEFAULT);
}

static void draw_control_screen(void)
{
    char link[LOG_COLS + 1], view[LOG_COLS + 1];
    status_format_link(link);
    status_format_view_pos(view);
    // Row 0: where the card is and how the link is.  Row 1: the reading
    // position, which is blank at the live edge and only speaks when you
    // have scrolled away.  Row 2: what Claude is doing, in plain words --
    // yellow when it is waiting on you.  Row 3: what the buttons do right
    // now.  RELINK sits at the right of rows 3 and 4, clear of all of it.
    draw_line(0, link, PAL_WHITE);
    draw_line(1, view, PAL_DEFAULT);
    const char *headline = chime_headline();
    const uint8_t head_fg = chime_needs_you() ? PAL_YELLOW : PAL_ORANGE;
    // At most 30 columns: RELINK starts at column 31.  Cut to that, on a
    // whole character, so a long translation stops short of the tile
    // instead of running underneath it.
    char confirm_hint[96], rewind_hint[96], hint_fit[96];
    (void)lang_fill(confirm_hint, sizeof(confirm_hint), tr(TR_CONTROL_HINT_CONFIRM),
                    legend_enter[0] ? legend_enter : "A");
    (void)lang_fill(rewind_hint, sizeof(rewind_hint), tr(TR_CONTROL_HINT_UNDO),
                    legend_enter[0] ? legend_enter : "A");
    const char *hint =
        focus_active() ? tr(TR_CONTROL_HINT_FOCUS) :
        rewind_on ? rewind_hint :
        chime_needs_you() ? confirm_hint :
        tr(TR_CONTROL_HINT_SCROLL);
    (void)lang_pad_cols(hint, 0u, 30u, hint_fit, sizeof(hint_fit));
    if (!text_tall(headline) && !text_tall(hint_fit)) {
        draw_line(2, headline, head_fg);
        draw_line_rgb(3, hint_fit, HINT_DIM);
        draw_line(4, "", PAL_DEFAULT);
    } else {
        // Tall glyphs: the hint drops two pixels so the headline's lowest
        // row (y 24) keeps a clear row under it.  It still ends (y 34)
        // above the tiles at y 40.
        draw_line(3, "", PAL_DEFAULT);
        draw_line(4, "", PAL_DEFAULT);
        draw_line(2, headline, head_fg);
        draw_line_at(3 * 8 + 2, hint_fit, HINT_DIM);
    }
    (void)ext_subscreen_draw(EXT_DRAW_CONTROL_STRIP);

    // Drawn from kbd.c's table, never from coordinates written here: the touch
    // targets come from the same table, so the two cannot drift apart. Only
    // the labels live here, and a wrong label is a cosmetic bug rather than a
    // button that does something other than what it says.
    for (int id = KBD_CONTROL_KEYS; id <= KBD_CONTROL_COUNT; id++) {
        int x, y, w, h;
        kbd_control_rect((kbd_control_t)id, &x, &y, &w, &h);
        const char *label = "", *what = "";
        switch ((kbd_control_t)id) {
        case KBD_CONTROL_KEYS:
            label = tr(TR_CONTROL_KEYS);
            what  = tr(TR_CONTROL_KEYS_SUB);
            break;
        case KBD_CONTROL_QUICK: {
            static char mode_what[64];
            label = tr(TR_CONTROL_QUICK);
            if (claude_mode[0]) {
                (void)lang_fill(mode_what, sizeof(mode_what),
                                tr(TR_CONTROL_QUICK_MODE), claude_mode);
                what = mode_what;
            } else {
                what = tr(TR_CONTROL_QUICK_SUB);
            }
            break;
        }
        case KBD_CONTROL_PHOTO:
            // The camera in one tap; a card without one gets QUICK's AGAIN.
            label = quick_camera ? tr(TR_CONTROL_PHOTO) : tr(TR_CONTROL_AGAIN);
            what  = quick_camera ? tr(TR_CONTROL_PHOTO_SUB) : tr(TR_CONTROL_AGAIN_SUB);
            break;
        case KBD_CONTROL_COMPOSE:
            label = tr(TR_CONTROL_COMPOSE);
            what  = tr(TR_CONTROL_COMPOSE_SUB);
            break;
        case KBD_CONTROL_REMOTE:
            label = tr(TR_CONTROL_REMOTE);
            what  = tr(TR_CONTROL_REMOTE_SUB);
            break;
        case KBD_CONTROL_SETTINGS:
            label = tr(TR_CONTROL_SETTINGS);
            what  = tr(TR_CONTROL_SETTINGS_SUB);
            break;
        default: break;
        }
        if (w <= 0) continue;
        draw_tile2(x, y, w, h, label, what, 0,
                   focus_active() && focus_place() == FOCUS_CONTROL &&
                       id == focus_control_id());
    }

    // Recovery is intentionally smaller than the destinations above.  It is
    // touch-only so the six primary tiles remain a clean focus rectangle, but
    // an explicit relink is always one deliberate tap away when Wi-Fi stalls.
    int rx, ry, rw, rh;
    kbd_control_rect(KBD_CONTROL_RECONNECT, &rx, &ry, &rw, &rh);
    draw_tile(rx, ry, rw, rh, tr(TR_CONTROL_RELINK), 0, 0);
    // An extension's own small tile beside it, when there is one.
    kbd_control_rect(KBD_CONTROL_EXTENSION, &rx, &ry, &rw, &rh);
    if (rw > 0 && ext_control_label()) {
        draw_tile(rx, ry, rw, rh, ext_control_label(), 0, 0);
    }

    // The strip under the tiles.  An extension stands there when one is
    // attached; without one it is the place to learn the buttons, which is
    // the one thing everybody needs and nobody should have to look up.
    if (ext_control_label()) {
        for (int row = 20; row <= 23; row++) draw_line(row, "", PAL_DEFAULT);
    } else {
        // From the BUTTONS tab, so the legend never names a button that
        // does something else now.
        char second[160];
        int at = 0;
        second[0] = '\0';
        if (legend_stop[0]) {
            at += snprintf(second + at, sizeof(second) - (size_t)at, "%s %s  ",
                           legend_stop, tr(TR_BUTTON_STOP));
        }
        if (legend_tab[0] && at < (int)sizeof(second)) {
            at += snprintf(second + at, sizeof(second) - (size_t)at, "R+%s %s  ",
                           legend_tab, tr(TR_LEGEND_MODE));
        }
        if (at < (int)sizeof(second)) {
            snprintf(second + at, sizeof(second) - (size_t)at, "SELECT %s",
                     tr(TR_LEGEND_SCREENS));
        }
        const char *third = tr(TR_LEGEND_SHOULDERS);
        // And where all of it, and every combo, is explained.
        const char *fourth = tr(TR_LEGEND_GUIDE);
        if (!text_tall(legend_buttons) && !text_tall(second) &&
            !text_tall(third) && !text_tall(fourth)) {
            draw_line_rgb(20, legend_buttons, HINT_DIM);
            draw_line_rgb(21, second, HINT_DIM);
            draw_line_rgb(22, third, HINT_DIM);
            draw_line_rgb(23, fourth, HINT_DIM);
        } else {
            // Ten-pixel pitch from y 153: the tiles end at y 151 and the
            // last line's ink ends at y 191, the panel's last row.
            for (int row = 20; row <= 23; row++) draw_line(row, "", PAL_DEFAULT);
            draw_line_at(153, legend_buttons, HINT_DIM);
            draw_line_at(153 + TALL_PITCH, second, HINT_DIM);
            draw_line_at(153 + 2 * TALL_PITCH, third, HINT_DIM);
            draw_line_at(153 + 3 * TALL_PITCH, fourth, HINT_DIM);
        }
    }
}

// ------------------------------------------------------- CONTROLS guide ---
//
// The console in the person's hands, drawn in plain pixels: the lower half,
// with the d-pad, A B X Y, START and SELECT where that model has them, and L
// and R on its top edge.  Every button lights in the accent colour while it
// is held.  The tabs, DONE and every label come from guide.c, which also
// decides what each press means; this only draws.
//
// Drawn into a back buffer and copied out, so the console never flickers
// through its own layers as a button lights and goes dark.

static int guide_on;
static guide_t guide_state_shown;
static guide_config_t guide_cfg_shown;
static button_map_t guide_map_shown;
static console_family_t guide_family = CONSOLE_DSI;
static unsigned guide_held;

void subscreen_set_guide(int active, const guide_t *g, const guide_config_t *cfg,
                         console_family_t family, unsigned held)
{
    guide_on = active && g && cfg;
    if (!guide_on) return;
    // Copies, not pointers: the caller's state can move on between this and
    // the next paint without the drawing reading it halfway through.
    guide_state_shown = *g;
    guide_cfg_shown = *cfg;
    if (cfg->map) guide_map_shown = *cfg->map;
    else button_map_defaults(&guide_map_shown);
    guide_cfg_shown.map = &guide_map_shown;
    guide_family = guide_console(family);
    guide_held = held;
}

static int isqrt_int(int v)
{
    if (v <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

// A rectangle with rounded corners of radius `r`.
static void fill_round(int x, int y, int w, int h, int r, uint16_t colour)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        const int d = yy < r ? r - yy : yy >= h - r ? yy - (h - r) + 1 : 0;
        if (d > 0) inset = r - isqrt_int(r * r - d * d);
        fill_px(x + inset, y + yy, w - 2 * inset, 1, colour);
    }
}

static void fill_circle(int cx, int cy, int r, uint16_t colour)
{
    for (int dy = -r; dy <= r; dy++) {
        const int half = isqrt_int(r * r - dy * dy);
        fill_px(cx - half, cy + dy, 2 * half + 1, 1, colour);
    }
}

// Text cut to `max_cols` and centred on `cx`.
static void text_centre(int cx, int y, const char *s, unsigned max_cols,
                        uint16_t fg, uint16_t bg)
{
    char fit[96];
    const int cols = (int)lang_pad_cols(s ? s : "", 0u, max_cols, fit, sizeof(fit));
    draw_text_px(cx - cols * 3, y, fit, fg, bg);
}

static void text_left(int x, int y, const char *s, unsigned max_cols,
                      uint16_t fg, uint16_t bg)
{
    char fit[96];
    (void)lang_pad_cols(s ? s : "", 0u, max_cols, fit, sizeof(fit));
    draw_text_px(x, y, fit, fg, bg);
}

typedef struct {
    uint16_t body, edge, shade, text, dim, shoulder;
    int radius;
    const char *name;
} console_look_t;

static console_look_t console_look(console_family_t f)
{
    console_look_t k;
    k.shoulder = 0;
    switch (f) {
    case CONSOLE_DS:        // the original: silver, square, a hinge to match
        k.body = ARGB16(1, 14, 14, 16); k.edge = ARGB16(1, 20, 20, 22);
        k.shade = ARGB16(1, 9, 9, 11); k.text = ARGB16(1, 30, 30, 31);
        k.dim = ARGB16(1, 22, 22, 24); k.radius = 5; k.name = "DS";
        break;
    case CONSOLE_DS_LITE:   // white and rounded
        k.body = ARGB16(1, 25, 25, 26); k.edge = ARGB16(1, 30, 30, 31);
        k.shade = ARGB16(1, 18, 18, 20); k.text = ARGB16(1, 3, 3, 4);
        k.dim = ARGB16(1, 9, 9, 11); k.radius = 14; k.name = "DS Lite";
        break;
    case CONSOLE_3DS:       // dark blue-black, the circle pad, HOME
        k.body = ARGB16(1, 5, 7, 12); k.edge = ARGB16(1, 9, 12, 19);
        k.shade = ARGB16(1, 2, 3, 6); k.text = ARGB16(1, 26, 27, 29);
        k.dim = ARGB16(1, 14, 15, 18); k.radius = 10; k.name = "3DS";
        break;
    case CONSOLE_DSI:
    default:                // matte black, the power button at the front
        k.body = ARGB16(1, 6, 6, 7); k.edge = ARGB16(1, 11, 11, 12);
        k.shade = ARGB16(1, 3, 3, 3); k.text = ARGB16(1, 26, 27, 29);
        k.dim = ARGB16(1, 14, 14, 15); k.radius = 8; k.name = "DSi";
        break;
    }
    // Shoulders stand out from the body: lighter on a dark console, darker
    // on the white one.
    k.shoulder = f == CONSOLE_DS_LITE ? k.shade : k.edge;
    return k;
}

#define GUIDE_BTN       ARGB16(1, 3, 3, 4)
#define GUIDE_BTN_TEXT  ARGB16(1, 26, 26, 28)
#define GUIDE_SCREEN_BG ARGB16(1, 1, 2, 4)

// The body's box, and the drawn touch screen DONE sits in.
#define GBODY_X 4
#define GBODY_Y 24
#define GBODY_W 248
#define GBODY_H 144
#define GSHOULDER_W 92
#define GSCREEN_X 80
#define GSCREEN_Y 36
#define GSCREEN_W 96
#define GSCREEN_H 72

static void draw_face_button(int cx, int cy, char letter, int lit)
{
    const uint16_t accent = render_palette_rgb(PAL_ORANGE);
    fill_circle(cx, cy, 6, lit ? accent : GUIDE_BTN);
    glyph_px(cx - 2, cy - 3, (uint8_t)letter, lit ? KBD_TEXT_HI : GUIDE_BTN_TEXT,
             lit ? accent : GUIDE_BTN);
}

static void draw_small_oval(int x, int y, int lit)
{
    fill_round(x, y, 14, 6, 3, lit ? render_palette_rgb(PAL_ORANGE) : GUIDE_BTN);
}

static void draw_dpad(int cx, int cy, int arm, int width, int lit_mask,
                      uint16_t body)
{
    const uint16_t accent = render_palette_rgb(PAL_ORANGE);
    const int hw = width / 2;
    (void)body;
    // The centre, then each arm on its own so a held direction lights alone.
    fill_px(cx - hw, cy - hw, width, width, GUIDE_BTN);
    fill_px(cx - hw, cy - hw - arm, width, arm,
            (lit_mask & GUIDE_KEY_UP) ? accent : GUIDE_BTN);
    fill_px(cx - hw, cy + hw + (width & 1), width, arm,
            (lit_mask & GUIDE_KEY_DOWN) ? accent : GUIDE_BTN);
    fill_px(cx - hw - arm, cy - hw, arm, width,
            (lit_mask & GUIDE_KEY_LEFT) ? accent : GUIDE_BTN);
    fill_px(cx + hw + (width & 1), cy - hw, arm, width,
            (lit_mask & GUIDE_KEY_RIGHT) ? accent : GUIDE_BTN);
}

static void draw_shoulder(int x, int left, const char *letter, const char *label,
                          int lit, const console_look_t *k)
{
    const uint16_t accent = render_palette_rgb(PAL_ORANGE);
    const uint16_t fill = lit ? accent : k->shoulder;
    const uint16_t fg = lit ? KBD_TEXT_HI : k->text;
    fill_round(x, 16, GSHOULDER_W, 12, 5, fill);
    char text[96];
    snprintf(text, sizeof(text), "%s %s", letter, label);
    (void)left;
    text_centre(x + GSHOULDER_W / 2, 18, text, (GSHOULDER_W - 8) / 6, fg, fill);
}

static void draw_guide_screen(void)
{
    const guide_ctx_t ctx = guide_state_shown.ctx;
    const guide_config_t *cfg = &guide_cfg_shown;
    const unsigned held = guide_held;
    const console_look_t k = console_look(guide_family);
    const uint16_t accent = render_palette_rgb(PAL_ORANGE);

    fill_px(0, 0, SUB_W, 192, SUB_BG);

    // The contexts.  The one shown is lit like Settings' tab on screen.
    for (int c = 0; c < GUIDE_CTX_COUNT; c++) {
        int tx, ty, tw, th;
        if (!guide_tab_rect((guide_ctx_t)c, cfg, &tx, &ty, &tw, &th)) continue;
        draw_tile(tx, ty, tw, th, tr(guide_ctx_label((guide_ctx_t)c)),
                  c == (int)ctx, 0);
    }

    // The body, its hinge edge, and the shoulders standing on it.
    fill_round(GBODY_X, GBODY_Y, GBODY_W, GBODY_H, k.radius, k.edge);
    fill_round(GBODY_X + 1, GBODY_Y + 1, GBODY_W - 2, GBODY_H - 2,
               k.radius > 1 ? k.radius - 1 : 0, k.body);
    if (guide_family == CONSOLE_DS) {
        // The original's hinge: a darker bar across the top.
        fill_px(GBODY_X + 6, GBODY_Y + 2, GBODY_W - 12, 5, k.shade);
    }
    draw_shoulder(GBODY_X + 4, 1, "L",
                  guide_button_label(ctx, cfg, GUIDE_COMBO_L),
                  (held & GUIDE_KEY_L) != 0, &k);
    draw_shoulder(GBODY_X + GBODY_W - 4 - GSHOULDER_W, 0, "R",
                  guide_button_label(ctx, cfg, GUIDE_COMBO_R),
                  (held & GUIDE_KEY_R) != 0, &k);

    // The touch screen, in its bezel: what to do, and DONE.
    fill_px(GSCREEN_X - 3, GSCREEN_Y - 3, GSCREEN_W + 6, GSCREEN_H + 6, k.shade);
    fill_px(GSCREEN_X, GSCREEN_Y, GSCREEN_W, GSCREEN_H, GUIDE_SCREEN_BG);
    text_centre(GSCREEN_X + GSCREEN_W / 2, GSCREEN_Y + 8, tr(TR_GUIDE_PRESS),
                16u, accent, GUIDE_SCREEN_BG);
    text_centre(GSCREEN_X + GSCREEN_W / 2, GSCREEN_Y + 24, tr(TR_GUIDE_TABS_HINT),
                16u, HINT_DIM, GUIDE_SCREEN_BG);
    draw_tile(GUIDE_DONE_X, GUIDE_DONE_Y, GUIDE_DONE_W, GUIDE_DONE_H,
              tr(TR_GUIDE_DONE), 0, 1);

    // Left: the d-pad (and on a 3DS the circle pad above it, which is the
    // same input in DSi mode, so it lights with it), and what it does here.
    const unsigned dpad = held & GUIDE_KEY_DPAD;
    int dpad_label_y;
    if (guide_family == CONSOLE_3DS) {
        fill_circle(40, 50, 13, k.shade);
        fill_circle(40, 50, 10, dpad ? accent : GUIDE_BTN);
        draw_dpad(40, 92, 9, 9, (int)dpad, k.body);
        dpad_label_y = 110;
    } else {
        draw_dpad(40, 62, 11, 11, (int)dpad, k.body);
        dpad_label_y = 84;
    }
    text_centre(40, dpad_label_y,
                guide_button_label(ctx, cfg, GUIDE_COMBO_DPAD), 11u,
                dpad ? accent : k.text, k.body);

    // Right: A B X Y, and under them what each does here -- under the
    // BUTTONS mapping on CONTROL and KEYS, read fresh on every paint.
    const int fx = 214, fy = 54, off = 13;
    draw_face_button(fx, fy - off, 'X', (held & GUIDE_KEY_X) != 0);
    draw_face_button(fx - off, fy, 'Y', (held & GUIDE_KEY_Y) != 0);
    draw_face_button(fx + off, fy, 'A', (held & GUIDE_KEY_A) != 0);
    draw_face_button(fx, fy + off, 'B', (held & GUIDE_KEY_B) != 0);
    static const struct { guide_combo_t combo; char letter; unsigned key; } FACES[] = {
        { GUIDE_COMBO_X, 'X', GUIDE_KEY_X }, { GUIDE_COMBO_Y, 'Y', GUIDE_KEY_Y },
        { GUIDE_COMBO_A, 'A', GUIDE_KEY_A }, { GUIDE_COMBO_B, 'B', GUIDE_KEY_B },
    };
    int ly = 78;
    for (unsigned i = 0; i < sizeof(FACES) / sizeof(FACES[0]); i++) {
        char line[96];
        snprintf(line, sizeof(line), "%c %s", FACES[i].letter,
                 guide_button_label(ctx, cfg, FACES[i].combo));
        const char *label = guide_button_label(ctx, cfg, FACES[i].combo);
        ly += text_tall(label) ? 11 : 9;
        text_left(184, ly - 9, line, 11u,
                  (held & FACES[i].key) ? accent : k.text, k.body);
    }

    // START and SELECT: beside the face buttons on a DS, DS Lite or DSi;
    // under the screen, either side of HOME, on a 3DS.  Each is named as the
    // console prints it, with what it does here on the line under.
    const int start_lit = (held & GUIDE_KEY_START) != 0;
    const int select_lit = (held & GUIDE_KEY_SELECT) != 0;
    const int sy = ly + 3;
    static const struct { const char *name; guide_combo_t combo; unsigned key; } SS[] = {
        { "START", GUIDE_COMBO_START, GUIDE_KEY_START },
        { "SELECT", GUIDE_COMBO_SELECT, GUIDE_KEY_SELECT },
    };
    for (unsigned i = 0; i < 2; i++) {
        const int lit = (held & SS[i].key) != 0;
        const int y = sy + (int)i * 22;
        text_left(184, y, SS[i].name, 6u, lit ? accent : k.dim, k.body);
        const char *label = guide_button_label(ctx, cfg, SS[i].combo);
        text_left(190, y + (text_tall(label) ? 10 : 9), label, 10u,
                  lit ? accent : k.text, k.body);
        if (guide_family != CONSOLE_3DS) draw_small_oval(232, y + 1, lit);
    }
    if (guide_family == CONSOLE_3DS) {
        draw_small_oval(100, 118, select_lit);
        fill_circle(128, 121, 4, k.shade);
        draw_small_oval(142, 118, start_lit);
        text_centre(107, 128, "SELECT", 6u, select_lit ? accent : k.dim, k.body);
        text_centre(149, 128, "START", 5u, start_lit ? accent : k.dim, k.body);
    }
    if (guide_family == CONSOLE_DSI) {
        // The DSi's power button, bottom left on the front.
        fill_circle(GBODY_X + 14, GBODY_Y + GBODY_H - 14, 4, k.shade);
    }
    // Which console this is drawn as.
    text_left(GBODY_X + 10, GBODY_Y + GBODY_H - 30, k.name, 8u, k.dim, k.body);

    // How to leave, under the console.
    text_centre(SUB_W / 2, GBODY_Y + GBODY_H + 6, tr(TR_GUIDE_FOOT), 42u,
                HINT_DIM, SUB_BG);
}

// Paints the guide into a back buffer and copies it out in one pass.
static void draw_guide(void)
{
    static uint16_t back[SUB_W * 192];
    uint16_t *const front = sub;
    sub = back;
    draw_guide_screen();
    sub = front;
    for (int i = 0; i < SUB_W * 192; i++) sub[i] = back[i];
}

void subscreen_set_composer(int active)
{
    composer_active = active ? 1 : 0;
    if (!composer_active) composer_add_quick = 0;
}

void subscreen_set_composer_add_quick(int add_quick)
{
    composer_add_quick = add_quick ? 1 : 0;
}

void subscreen_set_voice(int ready, int recording, int waiting, unsigned bars)
{
    voice_ready = ready ? 1 : 0;
    voice_recording = recording ? 1 : 0;
    if (!voice_recording) voice_latched = 0;
    voice_waiting = waiting ? 1 : 0;
    voice_bars = bars > 8u ? 8u : bars;
}

void subscreen_set_voice_latched(int latched)
{
    voice_latched = (latched && voice_recording) ? 1 : 0;
}

static int boot_active;
static int boot_keyboard;
static const char *boot_body;

void subscreen_set_boot_body(const char *body)
{
    boot_body = body;
}

// Lines of `text` from `top`, each its own row, the rest of the screen blank.
static void draw_text_block(int top, const char *text, uint8_t fg)
{
    int row = top;
    const char *line = text;
    while (row < LOG_ROWS && line && *line) {
        const char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[LOG_COLS + 1];
        snprintf(buf, sizeof(buf), "%.*s", (int)(len > LOG_COLS ? LOG_COLS : len),
                 line);
        draw_line(row++, buf, fg);
        line = end ? end + 1 : NULL;
    }
    for (; row < LOG_ROWS; row++) draw_line(row, "", fg);
}
static char boot_head[2][LOG_COLS + 1];

void subscreen_set_boot(int active, int keyboard, const char *line0,
                        const char *line1)
{
    boot_active = active ? 1 : 0;
    boot_keyboard = keyboard ? 1 : 0;
    snprintf(boot_head[0], sizeof(boot_head[0]), "%s", line0 ? line0 : "");
    snprintf(boot_head[1], sizeof(boot_head[1]), "%s", line1 ? line1 : "");
}
void subscreen_set_remote(int active, const remote_list_t *list,
                          const remote_computers_t *computers,
                          const remote_ui_t *ui, const char *here)
{
    remote_active = active ? 1 : 0;
    remote_list = list;
    remote_comps = computers;
    remote_ui_shown = ui;
    remote_here = here;
}

void subscreen_set_quick(int active, int level, int selected, int top,
                         const quick_commands_t *commands,
                         const quick_recent_t *recent)
{
    quick_active = active ? 1 : 0;
    quick_level = level;
    quick_selected = selected;
    quick_top = top;
    quick_cmds = commands;
    quick_recent = recent;
}

void subscreen_set_quick_saved(const quick_saved_t *saved, int remove_armed)
{
    quick_saved_cmds = saved;
    quick_remove_armed = remove_armed ? 1 : 0;
}

void subscreen_set_rewind(int picking) { rewind_on = picking ? 1 : 0; }

void subscreen_set_camera_available(int available) { quick_camera = available != 0; }

void subscreen_set_camera(int active, const char *top, const char *hint,
                          int error)
{
    camera_active = active != 0;
    camera_error = error != 0;
    snprintf(camera_top, sizeof(camera_top), "%s", top ? top : "");
    snprintf(camera_hint, sizeof(camera_hint), "%s", hint ? hint : "");
}

// Rows CAMERA_VIEW_Y0..CAMERA_VIEW_Y1 only: the strips above and below are
// the layout's own.  A frame is one NDMA-shaped block, so one dmaCopy.
void subscreen_camera_blit(const uint16_t *frame)
{
    if (!frame) return;
    mirror_flag_rows(&sub_mirror, CAMERA_VIEW_Y0, CAMERA_VIEW_Y1,
                     MIRROR_FLAG_CAMERA);
    dmaCopy(frame + CAMERA_VIEW_Y0 * SUB_W, sub + CAMERA_VIEW_Y0 * SUB_W,
            (CAMERA_VIEW_Y1 - CAMERA_VIEW_Y0 + 1) * SUB_W * sizeof(*frame));
}

void subscreen_set_claude_mode(const char *mode)
{
    snprintf(claude_mode, sizeof(claude_mode), "%s", mode ? mode : "");
}
void subscreen_set_project_actions(const project_action_t *actions,
                                   unsigned count, unsigned selected,
                                   unsigned dropped, int god_mode)
{
    if (count > PROJECT_ACTIONS_MAX) count = PROJECT_ACTIONS_MAX;
    for (unsigned i = 0; i < count; i++) project_action_list[i] = actions[i];
    project_action_count = count;
    project_action_highlight = selected < count ? selected : 0u;
    project_action_dropped = dropped;
    project_god_mode = god_mode ? 1 : 0;
}

void subscreen_set_clip(const char *summary, const char lines[][CLIP_LINE_COLS + 1],
                        unsigned count, int can_paste)
{
    (void)snprintf(clip_summary_line, sizeof(clip_summary_line), "%s",
                   summary ? summary : "");
    clip_line_count = count > SUBSCREEN_CLIP_LINES_MAX
                          ? SUBSCREEN_CLIP_LINES_MAX : count;
    for (unsigned i = 0; i < clip_line_count; i++) {
        (void)snprintf(clip_lines[i], sizeof(clip_lines[i]), "%s", lines[i]);
    }
    clip_can_paste = can_paste ? 1 : 0;
}

void subscreen_set_button_legend(const char *legend, const char *enter,
                                 const char *stop, const char *tab)
{
    snprintf(legend_buttons, sizeof(legend_buttons), "%s", legend ? legend : "");
    snprintf(legend_enter, sizeof(legend_enter), "%s", enter ? enter : "");
    snprintf(legend_stop, sizeof(legend_stop), "%s", stop ? stop : "");
    snprintf(legend_tab, sizeof(legend_tab), "%s", tab ? tab : "");
}

void subscreen_set_settings(const settings_t *settings,
                            const settings_ui_t *ui)
{
    if (settings) settings_shown = *settings;
    if (ui) settings_ui_shown = *ui;
    if (settings_ui_shown.row >= SETTINGS_ROW_COUNT) settings_ui_shown.row = 0u;
}

void subscreen_set_project(unsigned tab, const char lines[][44], unsigned count)
{
    if (count > SUBSCREEN_PROJECT_LINES_MAX) count = SUBSCREEN_PROJECT_LINES_MAX;
    for (unsigned i = 0; i < count; i++) {
        (void)snprintf(project_lines[i], sizeof(project_lines[i]), "%s",
                       lines[i]);
    }
    project_line_count = count;
    project_tab_sel = tab < PROJECT_TAB_COUNT ? tab : 0u;
}

void subscreen_init(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    sub_bg_id = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    sub = (uint16_t *)bgGetGfxPtr(sub_bg_id);
    // All 256 rows, not just the visible 192: the impact shake scrolls this
    // background, and the rows past the bottom are uninitialised VRAM until
    // something clears them.  Doing it once here is what lets the shake stay
    // a two-line function.
    for (int i = 0; i < SUB_W * 256; i++) sub[i] = SUB_BG;
    mirror_clear(&sub_mirror, SUB_BG);

}

void subscreen_update(unsigned long rx, unsigned long msgs, unsigned long tx,
                      unsigned long loops, unsigned raw, unsigned lib)
{
    // Clear only when the layout changes, never on every repaint.
    //
    // Blanking 256x192 and redrawing into the bitmap the LCD is currently
    // showing is visible as a flash. Every draw path below already covers its
    // own area -- draw_line() pads to the full width, draw_tile() fills its
    // rect -- so the clear was only ever covering debris left when one layout
    // gives way to another: keyboard tiles still on screen under the log, and
    // so on. That is the only moment it is needed.
    //
    // If a layout leaves debris after this, that layout's draw path is
    // incomplete. Fix the path rather than going back to clearing every frame.
    const int layout = guide_on ? -8 :
                       boot_active ? (boot_keyboard ? -5 : -4) :
                       composer_active ? -1 : camera_active ? -7 :
                       quick_active ? -2 :
                       remote_active ? -6 :
                       ext_menu_is_open() ? -3 : (int)status_mode_get();
    static int last_layout = -2;
    if (layout != last_layout) {
        for (int i = 0; i < SUB_W * 192; i++) sub[i] = SUB_BG;
        mirror_clear(&sub_mirror, SUB_BG);
        last_layout = layout;
    }

    char line1[LOG_COLS + 1], line2[LOG_COLS + 1];
    status_format_counters(rx, msgs, tx, loops, raw, lib, line1, line2);

    if (guide_on) {
        // Above everything it can be opened over: QUICK, Settings, a menu.
        draw_guide();
    } else if (boot_active) {
        draw_line(0, boot_head[0], PAL_ORANGE);
        draw_line(1, boot_head[1], PAL_DEFAULT);
        if (boot_keyboard) {
            draw_keyboard();
            draw_keyboard_state(20);
            draw_line(21, "", PAL_DEFAULT);
        } else if (boot_body) {
            draw_text_block(2, boot_body, PAL_DEFAULT);
        } else {
            draw_log_region(2, LOG_ROWS - 1);
        }
    } else if (composer_active) {
        const char *head;
        const char *keys;
        uint8_t head_fg;
        // The level, as eight columns of '#' padded with spaces, goes into
        // the language's line; lang_fill() reads only its one %s, so a
        // custom pack's text is never a printf format.
        char meter[LOG_COLS * 3 + 1];
        if (voice_recording) {
            head = voice_latched ? tr(TR_COMPOSE_MIC_LATCH) : tr(TR_COMPOSE_MIC_REC);
            head_fg = PAL_RED;
            char bars[9];
            snprintf(bars, sizeof(bars), "%-8.*s", (int)voice_bars, "########");
            lang_fill(meter, sizeof(meter),
                      tr(voice_latched ? TR_COMPOSE_METER_LATCHED
                         : settings_shown.mic_mode == SETTINGS_MIC_LATCH
                             ? TR_COMPOSE_METER_LATCH
                             : TR_COMPOSE_METER_HOLD),
                      bars);
            keys = meter;
        } else if (voice_waiting) {
            head = tr(TR_COMPOSE_MIC_WAIT);
            head_fg = PAL_ORANGE;
            keys = tr(TR_COMPOSE_KEYS);
        } else if (focus_active() && focus_place() == FOCUS_KEYS) {
            // Focus moves A to typing the lit key, so the lines that say
            // what A does change with it: START sends (or saves).
            head = tr(TR_COMPOSE_FOCUS_HEAD);
            head_fg = PAL_ORANGE;
            keys = tr(composer_add_quick ? TR_COMPOSE_ADD_FOCUS_KEYS
                                         : TR_COMPOSE_FOCUS_KEYS);
        } else if (composer_add_quick) {
            head = tr(TR_COMPOSE_ADD_HEAD);
            head_fg = PAL_YELLOW;
            keys = tr(TR_COMPOSE_ADD_KEYS);
        } else {
            head = tr(TR_COMPOSE_HEAD);
            head_fg = PAL_YELLOW;
            keys = tr(TR_COMPOSE_KEYS);
        }
        // The two lines above the keyboard have its 20 pixels.  On the grid,
        // a tall (CJK) glyph's cell -- a row above and a row below the eight
        // -- ran into its neighbour: the second line painted over the first
        // one's lowest row, and the first's leftovers showed past the end of
        // a short second line.  Tall, they move off the grid to y 1 and 11,
        // each glyph's ten rows clear of the other's and of the keys at 20.
        if (!text_tall(head) && !text_tall(keys)) {
            draw_line(0, head, head_fg);
            draw_line(1, keys, PAL_DEFAULT);
        } else {
            fill_px(0, 0, LOG_COLS * 6, KBD_ORIGIN_Y, SUB_BG);
            draw_line_at(1, head, render_palette_rgb(head_fg));
            draw_line_at(1 + TALL_PITCH, keys, render_palette_rgb(PAL_DEFAULT));
        }
        draw_keyboard();
        // The two rows under the keyboard teach what the keyboard cannot
        // show: ENT and Y, then the shoulders -- L then R puts the d-pad on
        // the keys, and holding R talks, offered only when the computer
        // said it can transcribe.  (SELECT+START is not here: the guide
        // does not open over the composer, whose keys it would shadow.)
        {
            char shoulders[160];
            if (voice_ready) {
                snprintf(shoulders, sizeof(shoulders), "%s   %s",
                         tr(TR_COMPOSE_HINT_FOCUS), tr(TR_COMPOSE_HINT_TALK));
            } else {
                snprintf(shoulders, sizeof(shoulders), "%s",
                         tr(TR_COMPOSE_HINT_FOCUS));
            }
            const char *edit = tr(TR_COMPOSE_HINT_EDIT);
            if (!text_tall(edit) && !text_tall(shoulders)) {
                draw_line(20, edit, PAL_DEFAULT);
                draw_line(21, shoulders, PAL_DEFAULT);
            } else {
                // Tall glyphs need ten pixels a line: rows 20 and 21 are
                // y 160..175, and the second line still ends above the MIC
                // bar at y 176.
                draw_line(20, "", PAL_DEFAULT);
                draw_line(21, "", PAL_DEFAULT);
                draw_line_at(20 * 8 - 3, edit, render_palette_rgb(PAL_DEFAULT));
                draw_line_at(20 * 8 + 7, shoulders, render_palette_rgb(PAL_DEFAULT));
            }
        }
        // Rows 22-23 are the MIC bar's, or nothing: blank them either way.
        draw_line(22, "", PAL_DEFAULT);
        draw_line(23, "", PAL_DEFAULT);
        if (voice_ready || voice_recording || voice_waiting) {
            draw_tile(VOICE_BAR_X, VOICE_BAR_Y, VOICE_BAR_W, VOICE_BAR_H,
                      voice_recording ? (voice_latched ? tr(TR_COMPOSE_MIC_LATCH)
                                                       : tr(TR_COMPOSE_MIC_REC)) :
                      voice_waiting ? tr(TR_COMPOSE_MIC_WAIT) :
                      tr(TR_COMPOSE_MIC_IDLE),
                      voice_recording, voice_recording);
        }
    } else if (camera_active) {
        // The strips only -- the rows between them are the viewfinder's and
        // belong to subscreen_camera_blit(), which is why a repaint here
        // never smears a frame.
        draw_line(0, camera_top, PAL_RED);
        draw_tile(CAMERA_FLIP_X, CAMERA_FLIP_Y, CAMERA_FLIP_W, CAMERA_FLIP_H,
                  "Y flip", 0, 0);
        draw_line(23, camera_hint, camera_error ? PAL_YELLOW : PAL_DEFAULT);
    } else if (quick_active) {
        if (quick_level == SUBSCREEN_QUICK_MAIN) draw_quick_main();
        else draw_quick_list();
    } else if (remote_active) {
        draw_remote_menu();
    } else if (ext_menu_is_open()) {
        (void)ext_subscreen_draw(EXT_DRAW_OVERLAY);
    } else if (status_mode_get() == SCREEN_MODE_SETTINGS) {
        draw_settings_screen();
    } else if (status_mode_get() == SCREEN_MODE_PROJECT) {
        draw_project_screen();
    } else if (status_mode_get() == SCREEN_MODE_STATUS) {
        draw_status_screen();
        draw_line(LOG_ROWS - 2, line1, PAL_WHITE);
        draw_line(LOG_ROWS - 1, line2, PAL_WHITE);
    } else if (status_mode_get() == SCREEN_MODE_KEYS) {
        // Row 0 is the counters, unless the extension has a line for it: this
        // is the screen you are looking at while you type, so it is the screen
        // most worth a word on, and the counters are the most expendable thing
        // up here.  Row 1 stays the keyboard's own state whatever happens --
        // which layer you are on and whether Ctrl is armed both change what
        // the next key does, and that is not negotiable for a readout.
        if (!ext_subscreen_draw(EXT_DRAW_KEYS_STATUS)) {
            // While Claude waits on you, that is the line worth having here
            // too: you may be typing when the approval appears.
            if (chime_needs_you()) {
                draw_line(0, chime_headline(), PAL_YELLOW);
            } else {
                char link[LOG_COLS + 1];
                status_format_link(link);
                draw_line(0, link, PAL_WHITE);
            }
        }
        draw_keyboard_state(1);
        draw_keyboard();
        clear_keyboard_floor();
        // Without an extension standing there, the floor says what the
        // keyboard's own special keys do -- the two that are not obvious.
        if (!ext_control_label()) {
            draw_line_rgb(KBD_FLOOR_ROW, tr(TR_KEYS_FLOOR_0),
                          HINT_DIM);
            draw_line_rgb(KBD_FLOOR_ROW + 1, tr(TR_KEYS_FLOOR_1),
                          HINT_DIM);
        }
    } else {
        draw_control_screen();
    }
}
