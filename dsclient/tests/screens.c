// Photographs of every screen of the card, drawn by the card's own code.
//
//   tools/screenshots.sh          -> docs/screens/*.png
//
// render.c and subscreen.c are compiled against tests/shot/nds.h, whose two
// "VRAM" backgrounds are arrays; each scene below puts the card in a state
// with the same calls main.c makes, draws, and writes both screens stacked
// the way the console holds them.  It is how the UI is reviewed without a
// console in hand, and where the README's pictures come from -- so a picture
// in the docs can never show a screen the code no longer draws.
#include <stdio.h>
#include <string.h>

#include "nds.h"

#include "../source/bootui.h"
#include "../source/buttons.h"
#include "../source/camera.h"
#include "../source/chime.h"
#include "../source/composer.h"
#include "../source/profiles.h"
#include "../source/project.h"
#include "../source/focus.h"
#include "../source/guide.h"
#include "../source/kbd.h"
#include "../source/lang.h"
#include "../source/log.h"
#include "../source/mirror.h"
#include "../source/proto.h"
#include "../source/quick.h"
#include "../source/remote.h"
#include "../source/privacy.h"
#include "../source/render.h"
#include "../source/sessions.h"
#include "../source/settings.h"
#include "../source/sound.h"
#include "../source/status.h"
#include "../source/subscreen.h"

u16 shot_main[256 * 256];
u16 shot_sub[256 * 256];

static grid_t term;

static void put(grid_t *g, int row, int col, const char *text, uint8_t fg)
{
    for (; *text && col < PROTO_COLS; text++, col++) {
        cell_t *c = &g->cells[row * PROTO_COLS + col];
        c->glyph = (uint8_t)*text;
        c->attr = (uint8_t)(fg << 4);
        c->flags = 0;
    }
}

// A Claude Code session mid-task: the kind of screen the card spends its
// life showing.  Plain ASCII, so it reads the same in either font.
static void fill_terminal(int asking)
{
    grid_clear(&term);
    put(&term, 0, 0, " Claude Code", PAL_ORANGE);
    put(&term, 1, 0, " ~/projects/parser", PAL_DEFAULT);
    put(&term, 3, 0, "> fix the failing test in parser.py", PAL_WHITE);
    put(&term, 5, 0, "* Read parser.py", PAL_DEFAULT);
    put(&term, 6, 0, "* The test expects [] for an empty", PAL_DEFAULT);
    put(&term, 7, 0, "  string, but parse() returns None.", PAL_DEFAULT);
    put(&term, 9, 0, "* Update(parser.py)", PAL_GREEN);
    put(&term, 10, 0, "   12 -     return None", PAL_RED);
    put(&term, 11, 0, "   12 +     return []", PAL_GREEN);
    if (asking) {
        put(&term, 13, 0, " Do you want to make this edit to", PAL_WHITE);
        put(&term, 14, 0, " parser.py?", PAL_WHITE);
        put(&term, 15, 0, " > 1. Yes", PAL_ORANGE);
        put(&term, 16, 0, "   2. Yes, and don't ask again", PAL_DEFAULT);
        put(&term, 17, 0, "   3. No, and tell Claude what to do", PAL_DEFAULT);
        put(&term, 18, 0, "      differently (esc)", PAL_DEFAULT);
    } else {
        put(&term, 13, 0, "* Ran pytest -q", PAL_DEFAULT);
        put(&term, 14, 0, "  48 passed in 0.31s", PAL_GREEN);
        put(&term, 16, 0, "Fixed: parse(\"\") now returns []", PAL_DEFAULT);
        put(&term, 17, 0, "and the test passes.", PAL_DEFAULT);
        put(&term, 20, 0, "> ", PAL_WHITE);
    }
}

// Language shots are for looking at, not for the docs: written next to the
// rest but listed on stderr, so tools/screenshots.sh does not publish them.
static int shots_quiet;

static void write_ppm(const char *name)
{
    // Top screen, a 16-pixel hinge, bottom screen; every pixel doubled.
    const int W = 256, H = 192, GAP = 16, S = 2;
    char path[256];
    snprintf(path, sizeof(path), "build/screens/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W * S, (H * 2 + GAP) * S);
    for (int y = 0; y < H * 2 + GAP; y++) {
        for (int sy = 0; sy < S; sy++) {
            for (int x = 0; x < W; x++) {
                uint16_t c;
                if (y < H) c = shot_main[y * 256 + x];
                else if (y < H + GAP) c = ARGB16(1, 6, 6, 7);
                else c = shot_sub[(y - H - GAP) * 256 + x];
                const uint8_t rgb[3] = {
                    (uint8_t)(((c >> 0) & 31) * 255 / 31),
                    (uint8_t)(((c >> 5) & 31) * 255 / 31),
                    (uint8_t)(((c >> 10) & 31) * 255 / 31),
                };
                for (int sx = 0; sx < S; sx++) fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
    fprintf(shots_quiet ? stderr : stdout, "%s\n", path);

    // And the bottom screen as `dsterm view` receives it (mirror.c), so the
    // text the computer shows can be held up against the pixels.
    static uint8_t sub[MIRROR_PAYLOAD_SIZE];
    const size_t n = mirror_encode(subscreen_mirror(), 0, sub, sizeof(sub));
    snprintf(path, sizeof(path), "build/screens/%s.sub", name);
    f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fwrite(sub, 1, n, f);
    fclose(f);
}

static void bottom(void)
{
    subscreen_update(48213, 1290, 371, 18000, 0, 0);
}

static void connected_status(void)
{
    status_link_host("MacBook");
    status_link_set(14, 1, 13, 1, 0, 1, 201, 1, 0);
}

// The controls guide, as main.c paints it: the still session with the
// pop-up over it on top, the drawn console below.  `keys` is a sequence of
// held key words fed to guide_step() one frame each -- the real decision,
// order rules and all -- and the last word is what is drawn lit.
static void guide_shot(const char *name, console_family_t family,
                       guide_ctx_t ctx, const guide_config_t *cfg,
                       const unsigned *keys, unsigned nkeys)
{
    static guide_t g;
    static grid_t session;
    guide_init(&g, ctx, cfg);
    unsigned prev = 0, held = 0;
    for (unsigned i = 0; i < nkeys; i++) {
        held = keys[i];
        (void)guide_step(&g, cfg, held, held & ~prev);
        prev = held;
    }
    guide_session_fill(&session);
    render_overlay_grid(&session);
    if (g.shown) {
        static char lines[GUIDE_POPUP_LINES][GUIDE_LINE_MAX];
        const unsigned n = guide_popup_lines(&g.entry, lines);
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
    subscreen_set_guide(1, &g, cfg, family, held);
    bottom();
    write_ppm(name);
    subscreen_set_guide(0, NULL, NULL, CONSOLE_UNKNOWN, 0);
    bottom();
}

// One shot per context in the language on screen, each with a combo of that
// context pressed and lit.
static void guide_context_shots(const char *lang, const guide_config_t *cfg)
{
    static const struct { guide_ctx_t ctx; const char *tag; unsigned keys[24]; unsigned n; } SHOTS[] = {
        // CONTROL: L, then R joins -- the focus chord.
        { GUIDE_CTX_CONTROL, "control", { 0x200, 0x300 }, 2 },
        // KEYS: R (Shift), then Y: Shift-Tab.
        { GUIDE_CTX_KEYS, "keys", { 0x100, 0, 0x800 }, 3 },
        // COMPOSE: R first, then L, nothing recording: the order notice.
        { GUIDE_CTX_COMPOSE, "compose", { 0x100, 0x300 }, 2 },
        // FOCUS: START sends.
        { GUIDE_CTX_FOCUS, "focus", { 0x008 }, 1 },
        // CAMERA: the shutter (R) pressed.
        { GUIDE_CTX_CAMERA, "camera", { 0x100 }, 1 },
        // QUICK: Y, forget.
        { GUIDE_CTX_QUICK, "quick", { 0x800 }, 1 },
    };
    for (unsigned i = 0; i < sizeof(SHOTS) / sizeof(SHOTS[0]); i++) {
        char name[64];
        snprintf(name, sizeof(name), "guide-%s-%s", lang, SHOTS[i].tag);
        guide_shot(name, CONSOLE_DSI, SHOTS[i].ctx, cfg, SHOTS[i].keys, SHOTS[i].n);
    }
}

int main(void)
{
    // Every picture for the docs is taken with ADDRESSES set to HIDE, so no
    // address -- even an example one -- is in a picture that gets shared.
    privacy_set_hide(1);
    subscreen_set_claude_mode("auto");
    // Shot on a DSi: QUICK's PHOTO row and the settings' SHUTTER and
    // PICTURE rows exist only where the card says a camera does.
    settings_set_camera_available(1);
    subscreen_set_camera_available(1);
    render_init();
    subscreen_init();
    log_init();
    kbd_reset();
    focus_init();
    chime_init();
    sound_set_style(SOUND_STYLE_SNES);
    connected_status();
    status_view_pos_clear();

    // 1. Control, Claude working.
    fill_terminal(0);
    render_grid(&term);
    chime_claude_state(2);
    chime_claude_state(1);
    status_mode_set(SCREEN_MODE_CONTROL);
    bottom();
    write_ppm("control");

    // 2. Control, Claude asking.
    fill_terminal(1);
    render_grid(&term);
    chime_claude_state(2);
    chime_claude_state(3);
    bottom();
    write_ppm("control-approval");
    chime_claude_state(1);

    // 3. Keys.
    fill_terminal(0);
    render_grid(&term);
    status_mode_set(SCREEN_MODE_KEYS);
    bottom();
    write_ppm("keys");

    // 4. Settings.
    settings_t st;
    settings_ui_t ui;
    settings_about_t about;
    settings_defaults(&st);
    settings_ui_init(&ui);
    ui.row = SETTINGS_ROW_SOUND;
    // As the card has it after L/R to this tab: on the value in use.
    ui.option = settings_option_current(&st, SETTINGS_ROW_SOUND);
    settings_about_init(&about);
    // As main.c's settings_about_compose() writes it on a DSi.
    snprintf(about.build, sizeof(about.build), "DSi (DSi mode)  42x24  v1");
    snprintf(about.link, sizeof(about.link), "MacBook  14ms");
    snprintf(about.session, sizeof(about.session), "claude");
    snprintf(about.card, sizeof(about.card), "/dsterm-settings.cfg (saved)");
    static grid_t page;
    settings_render(&st, &ui, &about, &page);
    render_overlay_grid(&page);
    subscreen_set_settings(&st, &ui);
    status_mode_set(SCREEN_MODE_SETTINGS);
    bottom();
    write_ppm("settings");

    // 4b. Settings, BUTTONS: Y remapped to Space, SHUTTER and DEFAULTS on the
    // same screen without scrolling.
    st.buttons.action[BUTTON_Y] = BUTTON_SPACE;
    settings_ui_tab(&st, &ui, SETTINGS_TAB_BUTTONS);
    ui.row = SETTINGS_ROW_SHUTTER;
    settings_render(&st, &ui, &about, &page);
    render_overlay_grid(&page);
    subscreen_set_settings(&st, &ui);
    bottom();
    write_ppm("settings-buttons");

    // 5. REMOTE.
    static remote_list_t rl;
    remote_init(&rl);
    {
        const char *names[] = { "fix-tests", "docs-pass", "untitled" };
        const uint8_t states[] = { REMOTE_STATE_NEEDS_YOU, REMOTE_STATE_WORKING,
                                   REMOTE_STATE_IDLE };
        const uint8_t flags[] = { REMOTE_FLAG_LIVE, REMOTE_FLAG_LIVE,
                                  REMOTE_FLAG_COPY };
        for (unsigned i = 0; i < 3; i++) {
            remote_chunk_t c;
            memset(&c, 0, sizeof(c));
            c.count = 3;
            c.entry.index = (uint8_t)i;
            c.entry.state = states[i];
            c.entry.flags = flags[i];
            c.entry.age_min = (uint16_t)(4 + i * 30);
            snprintf(c.entry.name, sizeof(c.entry.name), "%s", names[i]);
            snprintf(c.entry.detail, sizeof(c.entry.detail), "%s",
                     i == 0 ? "parser - permission prompt" : "parser");
            (void)remote_apply_chunk(&rl, &c);
        }
    }
    static remote_computers_t comps;
    memset(&comps, 0, sizeof(comps));
    comps.count = 2;
    snprintf(comps.items[0].name, sizeof(comps.items[0].name), "MacBook");
    snprintf(comps.items[0].host, sizeof(comps.items[0].host), "192.0.2.10");
    comps.items[0].current = comps.items[0].saved = comps.items[0].online = 1;
    snprintf(comps.items[1].name, sizeof(comps.items[1].name), "DESKTOP-PC");
    snprintf(comps.items[1].host, sizeof(comps.items[1].host), "192.0.2.20");
    comps.items[1].saved = comps.items[1].online = 1;
    static remote_ui_t rui;
    remote_ui_init(&rui);
    remote_row_t rows[REMOTE_ROWS_MAX];
    const unsigned n = remote_rows(&rl, &comps, rows, REMOTE_ROWS_MAX);
    remote_ui_settle(&rui, rows, n);
    {
        char title[64], body[640], footer[64];
        remote_explain(&rui, &rl, &comps, rows, n, "MacBook", title,
                       sizeof(title), body, sizeof(body), footer, sizeof(footer));
        render_overlay_text(title, body, footer);
    }
    status_mode_set(SCREEN_MODE_CONTROL);
    subscreen_set_remote(1, &rl, &comps, &rui, "MacBook");
    bottom();
    write_ppm("remote");
    subscreen_set_remote(0, NULL, NULL, NULL, NULL);

    // 6. Status / log.
    render_overlay_close(&term);
    fill_terminal(0);
    render_grid(&term);
    // Lines the ROM really prints on the way in (boot.c, main.c).
    log_printf("DS Claude Terminal");
    log_printf("wifi: init (DSi mode)");
    log_printf("wifi: joining HomeNetwork (dhcp)");
    log_printf("wifi: ok, ip 192.0.2.42");
    log_printf("tcp: 192.0.2.10:8347");
    log_printf("tcp: connected");
    log_printf("authenticated; frames signed");
    status_mode_set(SCREEN_MODE_STATUS);
    bottom();
    write_ppm("status");

    // 7. QUICK.
    {
        static quick_commands_t qc;
        static quick_recent_t qr;
        quick_commands_init(&qc);
        const char theirs[] = "/deploy\n/release-notes";
        (void)quick_commands_apply(&qc, (const uint8_t *)theirs, sizeof(theirs) - 1);
        quick_recent_init(&qr);
        (void)quick_recent_add(&qr, "commit this with a clear message");
        (void)quick_recent_add(&qr, "run the tests and fix what fails");
        status_mode_set(SCREEN_MODE_CONTROL);
        subscreen_set_quick(1, SUBSCREEN_QUICK_MAIN, 0, 0, &qc, &qr);
        bottom();
        write_ppm("quick");
        subscreen_set_quick(0, SUBSCREEN_QUICK_MAIN, 0, 0, NULL, NULL);
    }

    // 7b. The camera, LIVE: the viewfinder rows are the synthetic chart the
    // test vectors agree on (test_camera.c's synth_frame), and the strips
    // and FLIP tile are camera.c's own words.
    {
        static uint16_t frame[CAMERA_SRC_W * CAMERA_SRC_H];
        for (int y = 0; y < CAMERA_SRC_H; y++) {
            for (int x = 0; x < CAMERA_SRC_W; x++) {
                frame[y * CAMERA_SRC_W + x] =
                    (uint16_t)(0x8000 | (x * 31 / 255) |
                               ((y * 31 / 191) << 5) |
                               (((x + y) & 31) << 10));
            }
        }
        char top[LOG_COLS + 1], hint[LOG_COLS + 1];
        camera_ui_top(CAMERA_UI_LIVE, PROTO_CAMERA_OUTER, top, sizeof(top));
        camera_ui_hint(CAMERA_UI_LIVE, camera_shutter_name(SHUTTER_R),
                       0, hint, sizeof(hint));
        subscreen_set_camera(1, top, hint, 0);
        bottom();
        subscreen_camera_blit(frame);
        write_ppm("camera");

        // REVIEW: the same frame held while the strip offers send or retake.
        camera_ui_top(CAMERA_UI_REVIEW, PROTO_CAMERA_OUTER, top, sizeof(top));
        camera_ui_hint(CAMERA_UI_REVIEW, camera_shutter_name(SHUTTER_R),
                       0, hint, sizeof(hint));
        subscreen_set_camera(1, top, hint, 0);
        bottom();
        subscreen_camera_blit(frame);
        write_ppm("camera-review");

        // SENDING, mid-upload.
        camera_ui_top(CAMERA_UI_SENDING, PROTO_CAMERA_OUTER, top, sizeof(top));
        camera_ui_hint(CAMERA_UI_SENDING, camera_shutter_name(SHUTTER_R),
                       42, hint, sizeof(hint));
        subscreen_set_camera(1, top, hint, 0);
        bottom();
        subscreen_camera_blit(frame);
        write_ppm("camera-sending");
        subscreen_set_camera(0, NULL, NULL, 0);
    }

    // 8. Composer.
    render_overlay_text("COMPOSE -- LOCAL, NOTHING SENT YET",
                        "Add a test for the empty string case\n"
                        "and run the suite.",
                        "A SEND  B DEL  Y SNIPPETS  X CANCEL");
    subscreen_set_composer(1);
    bottom();
    write_ppm("compose");

    // 8b. Composer with focus on its keyboard (L first, then R): a key lit,
    // and the top screen saying what A, B, START and X do now. A look, not a
    // doc picture, so it is listed on stderr.
    render_overlay_text("COMPOSE -- LOCAL, NOTHING SENT YET",
                        "Add a test for the empty string case\n"
                        "and run the suite.",
                        "FOCUS  A TYPE  B DEL  START SEND  X OUT");
    focus_on(FOCUS_KEYS);
    focus_move(3, 1);
    bottom();
    write_ppm("compose-focus");
    focus_off();
    subscreen_set_composer(0);

    // 9. PROJECT: the DO tab, with the built-in buttons and one of yours.
    {
        static project_t proj;
        project_init(&proj);
        project_add(&proj, PROJECT_TAB_NOW, "link", "connected", 3);
        project_add(&proj, PROJECT_TAB_NOW, "session", "claude", 9);
        project_add(&proj, PROJECT_TAB_NOW, "doing", "Claude is working", 0);
        // A server's own button arrives over the wire, as from a project
        // with `npm test`; the card's four built-ins come with it.
        (void)project_wire_row(&proj, 1, 0, PROJECT_TAB_DO, 0, "test",
                               "!npm run test");
        project_tab_set(&proj, PROJECT_TAB_DO);
        static grid_t ppage;
        project_render(&proj, &ppage);
        render_overlay_grid(&ppage);
        project_action_t acts[PROJECT_ACTIONS_MAX];
        const unsigned na = project_actions(&proj, acts, PROJECT_ACTIONS_MAX);
        char plines[2][44];
        snprintf(plines[0], sizeof(plines[0]), "DO");
        snprintf(plines[1], sizeof(plines[1]), "tap a tab   X BACK");
        subscreen_set_project((unsigned)PROJECT_TAB_DO, plines, 2);
        subscreen_set_project_actions(acts, na, 0, 0, 0);
        status_mode_set(SCREEN_MODE_PROJECT);
        bottom();
        write_ppm("project");
        status_mode_set(SCREEN_MODE_CONTROL);
    }

    // 10. First boot: the menu, and the guide on the lower screen.
    {
        static profiles_t prof;
        memset(&prof, 0, sizeof(prof));
        boot_row_t brows[BOOT_MAIN_ROWS];
        static char labels[BOOT_MAIN_ROWS][BOOTUI_COLS + 1];
        const unsigned nb = bootui_main_rows(&prof, 0, brows, labels);
        const char *rows2[BOOT_MAIN_ROWS];
        for (unsigned i = 0; i < nb; i++) rows2[i] = labels[i];
        render_overlay_list("DS CLAUDE TERMINAL", rows2, (int)nb, 0, 1, "A select");
        subscreen_set_boot_body(BOOTUI_FIRST_BOOT_GUIDE);
        subscreen_set_boot(1, 0, "DS Claude Terminal",
                           "Welcome! Start with step 1 below.");
        bottom();
        write_ppm("first-boot");
        subscreen_set_boot_body(NULL);
        subscreen_set_boot(0, 0, NULL, NULL);
    }

    // 11b. For the docs: the controls guide on a DSi, in COMPOSE, with L
    // held and then R -- the focus chord the composer otherwise hides -- and
    // QUICK's COMMANDS with a saved command above + ADD.
    lang_set_current(LANG_EN);
    {
        static button_map_t dmap;
        button_map_defaults(&dmap);
        guide_config_t dcfg = { &dmap, SHUTTER_R, 0, 1, 1 };
        static const unsigned l_then_r[] = { 0x200, 0x300 };
        guide_shot("controls-guide", CONSOLE_DSI, GUIDE_CTX_COMPOSE, &dcfg,
                   l_then_r, 2);
        render_overlay_close(&term);

        static quick_commands_t dqc;
        static quick_recent_t dqr;
        static quick_saved_t dqs;
        quick_commands_init(&dqc);
        const char theirs[] = "/deploy\n/release-notes";
        (void)quick_commands_apply(&dqc, (const uint8_t *)theirs, sizeof(theirs) - 1);
        quick_recent_init(&dqr);
        quick_saved_init(&dqs);
        (void)quick_saved_add(&dqs, "run the tests and fix what fails");
        subscreen_set_quick_saved(&dqs, 0);
        fill_terminal(0);
        render_grid(&term);
        status_mode_set(SCREEN_MODE_CONTROL);
        subscreen_set_quick(1, SUBSCREEN_QUICK_COMMANDS,
                            (int)quick_commands_total(&dqc, &dqs) - 1, 0, &dqc, &dqr);
        bottom();
        write_ppm("quick-commands");
        subscreen_set_quick(0, SUBSCREEN_QUICK_MAIN, 0, 0, NULL, NULL);
        subscreen_set_quick_saved(NULL, 0);
        bottom();
    }

    // 12. The controls guide: every console, every context, a pop-up, a
    // remapped A, and (in the loop below) every language.  Quiet: these are
    // to look at, not doc pictures.
    shots_quiet = 1;
    lang_set_current(LANG_EN);
    {
        static button_map_t gmap;
        button_map_defaults(&gmap);
        guide_config_t gcfg = { &gmap, SHUTTER_R, 0, 1, 1 };
        static const unsigned none[] = { 0 };
        guide_shot("guide-intro", CONSOLE_DSI, GUIDE_CTX_CONTROL, &gcfg, none, 0);
        static const unsigned hold_a[] = { 0x001 };
        guide_shot("guide-ds", CONSOLE_DS, GUIDE_CTX_CONTROL, &gcfg, hold_a, 1);
        guide_shot("guide-dslite", CONSOLE_DS_LITE, GUIDE_CTX_CONTROL, &gcfg, hold_a, 1);
        guide_shot("guide-dsi", CONSOLE_DSI, GUIDE_CTX_CONTROL, &gcfg, hold_a, 1);
        static const unsigned hold_down[] = { 0x080 };
        guide_shot("guide-3ds", CONSOLE_3DS, GUIDE_CTX_CONTROL, &gcfg, hold_down, 1);
        guide_shot("guide-unknown", CONSOLE_UNKNOWN, GUIDE_CTX_KEYS, &gcfg, hold_a, 1);
        // A remapped to Space: the label and the pop-up follow the map.
        gmap.action[BUTTON_A] = BUTTON_SPACE;
        gmap.action[BUTTON_B] = BUTTON_ENTER;
        guide_shot("guide-remap", CONSOLE_DS_LITE, GUIDE_CTX_CONTROL, &gcfg, hold_a, 1);
        button_map_defaults(&gmap);
        // Holding R in COMPOSE: talk; then L in LATCH mode.
        static unsigned hold_r[12];
        for (unsigned i = 0; i < 8; i++) hold_r[i] = 0x100;
        hold_r[8] = 0x300;
        gcfg.latch = 1;
        guide_shot("guide-compose-latch", CONSOLE_3DS, GUIDE_CTX_COMPOSE, &gcfg, hold_r, 9);
        gcfg.latch = 0;
        guide_shot("guide-compose-talk", CONSOLE_DS, GUIDE_CTX_COMPOSE, &gcfg, hold_r, 8);
        // A card without a camera: no CAMERA tab.
        gcfg.camera = 0;
        static const unsigned sel[] = { 0x004 };
        guide_shot("guide-nocamera", CONSOLE_DS_LITE, GUIDE_CTX_CONTROL, &gcfg, sel, 1);
        gcfg.camera = 1;
        guide_context_shots("en", &gcfg);
    }
    render_overlay_close(&term);

    // The composer's new hint rows, in English, with speech-to-text offered.
    render_overlay_text("COMPOSE -- LOCAL, NOTHING SENT YET",
                        "Add a test for the empty string case",
                        "A SEND  B DEL  Y SNIPPETS  X CANCEL");
    subscreen_set_composer(1);
    subscreen_set_voice(1, 0, 0, 0);
    bottom();
    write_ppm("lang-en-compose");
    subscreen_set_voice(0, 0, 0, 0);
    subscreen_set_composer(0);
    render_overlay_close(&term);
    shots_quiet = 0;

    // 11. Every built-in language: the screens with the most words on them,
    // to look at before a translation or a layout change ships.
    shots_quiet = 1;
    for (unsigned l = 1; l < LANG_BUILTIN_COUNT; l++) {
        char name[64];
        lang_set_current(l);
        st.language = (int)l;
        chime_set_answer_buttons("A", "X");
        {
            button_map_t bm;
            char legend[128];
            button_map_defaults(&bm);
            button_map_legend(&bm, legend, sizeof(legend));
            subscreen_set_button_legend(legend, "A", "START", "Y");
        }

        fill_terminal(1);
        render_grid(&term);
        chime_claude_state(2);
        chime_claude_state(3);
        status_mode_set(SCREEN_MODE_CONTROL);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-control", lang_code(l));
        write_ppm(name);
        // One language for the docs: Japanese, the one furthest from the
        // English pictures, so the README can show the DS speaking it.
        if (l == LANG_JA) {
            shots_quiet = 0;
            write_ppm("control-ja");
            shots_quiet = 1;
        }
        chime_claude_state(1);

        for (unsigned t = 0; t < SETTINGS_TAB_COUNT; t++) {
            settings_ui_tab(&st, &ui, t);
            settings_render(&st, &ui, &about, &page);
            render_overlay_grid(&page);
            subscreen_set_settings(&st, &ui);
            status_mode_set(SCREEN_MODE_SETTINGS);
            bottom();
            snprintf(name, sizeof(name), "lang-%s-settings-%u", lang_code(l), t);
            write_ppm(name);
        }

        static quick_commands_t qc;
        static quick_recent_t qr;
        static quick_saved_t qs;
        quick_commands_init(&qc);
        quick_recent_init(&qr);
        quick_saved_init(&qs);
        (void)quick_saved_add(&qs, "run the tests and fix what fails");
        subscreen_set_quick_saved(&qs, 0);
        status_mode_set(SCREEN_MODE_CONTROL);
        subscreen_set_quick(1, SUBSCREEN_QUICK_MAIN, 0, 0, &qc, &qr);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-quick", lang_code(l));
        write_ppm(name);
        subscreen_set_quick(1, SUBSCREEN_QUICK_COMMANDS, 0, 0, &qc, &qr);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-commands", lang_code(l));
        write_ppm(name);
        // The last row, + ADD, highlighted: a tall label on the bar.
        subscreen_set_quick(1, SUBSCREEN_QUICK_COMMANDS,
                            (int)quick_commands_total(&qc, &qs) - 1, 0, &qc, &qr);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-commands-add", lang_code(l));
        write_ppm(name);
        subscreen_set_quick(1, SUBSCREEN_QUICK_AGAIN, 0, 0, &qc, &qr);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-again", lang_code(l));
        write_ppm(name);
        subscreen_set_quick(0, SUBSCREEN_QUICK_MAIN, 0, 0, NULL, NULL);
        subscreen_set_quick_saved(NULL, 0);

        {
            static button_map_t gmap;
            button_map_defaults(&gmap);
            guide_config_t gcfg = { &gmap, SHUTTER_R, 0, 1, 1 };
            guide_context_shots(lang_code(l), &gcfg);
            render_overlay_close(&term);
        }
        render_overlay_text("COMPOSE", "...", "A SEND");
        subscreen_set_composer(1);
        subscreen_set_voice(1, 0, 0, 0);
        bottom();
        snprintf(name, sizeof(name), "lang-%s-compose", lang_code(l));
        write_ppm(name);
        subscreen_set_voice(0, 0, 0, 0);
        subscreen_set_composer(0);
        render_overlay_close(&term);
    }
    lang_set_current(LANG_EN);
    return 0;
}
