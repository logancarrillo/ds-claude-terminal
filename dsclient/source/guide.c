#include "guide.h"

#include <stdio.h>
#include <string.h>

#include "render.h"

// ------------------------------------------------------------- contexts ---

static const tr_id_t CTX_LABEL[GUIDE_CTX_COUNT] = {
    [GUIDE_CTX_CONTROL] = TR_GUIDE_TAB_CONTROL,
    [GUIDE_CTX_KEYS]    = TR_GUIDE_TAB_KEYS,
    [GUIDE_CTX_COMPOSE] = TR_GUIDE_TAB_COMPOSE,
    [GUIDE_CTX_FOCUS]   = TR_GUIDE_TAB_FOCUS,
    [GUIDE_CTX_CAMERA]  = TR_GUIDE_TAB_CAMERA,
    [GUIDE_CTX_QUICK]   = TR_GUIDE_TAB_QUICK,
};

tr_id_t guide_ctx_label(guide_ctx_t ctx)
{
    return (unsigned)ctx < GUIDE_CTX_COUNT ? CTX_LABEL[ctx] : TR_GUIDE_TAB_CONTROL;
}

int guide_ctx_present(guide_ctx_t ctx, const guide_config_t *cfg)
{
    if ((unsigned)ctx >= GUIDE_CTX_COUNT) return 0;
    // The camera's combos on a card with no camera would teach a screen
    // that cannot open.
    if (ctx == GUIDE_CTX_CAMERA) return cfg && cfg->camera;
    return 1;
}

guide_ctx_t guide_ctx_step(guide_ctx_t ctx, int dir, const guide_config_t *cfg)
{
    int next = (unsigned)ctx < GUIDE_CTX_COUNT ? (int)ctx : 0;
    const int step = dir < 0 ? -1 : 1;
    for (int i = 0; i < GUIDE_CTX_COUNT; i++) {
        next = (next + step + GUIDE_CTX_COUNT) % GUIDE_CTX_COUNT;
        if (guide_ctx_present((guide_ctx_t)next, cfg)) break;
    }
    return (guide_ctx_t)next;
}

guide_ctx_t guide_ctx_for(int screen_keys, int focus_on, int quick_open)
{
    if (quick_open) return GUIDE_CTX_QUICK;
    // Focus on the Control or KEYS screen is that screen's own L+R, which
    // its tab explains; FOCUS is the composer's, which cannot be open here.
    (void)focus_on;
    return screen_keys ? GUIDE_CTX_KEYS : GUIDE_CTX_CONTROL;
}

// --------------------------------------------------------------- entries ---

static const char *const BUTTON_TEXT[GUIDE_COMBO_COUNT] = {
    [GUIDE_COMBO_A] = "A", [GUIDE_COMBO_B] = "B", [GUIDE_COMBO_X] = "X",
    [GUIDE_COMBO_Y] = "Y", [GUIDE_COMBO_START] = "START",
    [GUIDE_COMBO_SELECT] = "SELECT", [GUIDE_COMBO_L] = "L",
    [GUIDE_COMBO_R] = "R", [GUIDE_COMBO_LR] = "L+R",
    [GUIDE_COMBO_INTRO] = "SELECT+START",
    [GUIDE_COMBO_SHUTTER] = "SHUTTER",   // camera_entry names the real key
};

static guide_entry_t entry_of(guide_combo_t combo, tr_id_t name, tr_id_t what)
{
    guide_entry_t e;
    e.combo = combo;
    e.combo_text = TR_COUNT;
    e.button = (unsigned)combo < GUIDE_COMBO_COUNT && BUTTON_TEXT[combo]
                   ? BUTTON_TEXT[combo] : "";
    e.name = name;
    e.action = BUTTON_ACTION_COUNT;
    e.what = what;
    switch (combo) {
    case GUIDE_COMBO_DPAD:      e.combo_text = TR_GUIDE_COMBO_DPAD; break;
    case GUIDE_COMBO_HOLD_R:    e.combo_text = TR_GUIDE_COMBO_HOLD_R; break;
    case GUIDE_COMBO_R_THEN_L:  e.combo_text = TR_GUIDE_COMBO_R_THEN_L; break;
    case GUIDE_COMBO_L_THEN_R:  e.combo_text = TR_GUIDE_COMBO_L_THEN_R; break;
    case GUIDE_COMBO_SHIFT_TAB: e.combo_text = TR_GUIDE_COMBO_R_THEN; break;
    default: break;
    }
    return e;
}

static guide_entry_t nothing(guide_combo_t combo)
{
    return entry_of(combo, TR_GUIDE_NAME_NOTHING, TR_GUIDE_DO_NOTHING);
}

static int is_face(guide_combo_t c)
{
    return c == GUIDE_COMBO_A || c == GUIDE_COMBO_B || c == GUIDE_COMBO_X ||
           c == GUIDE_COMBO_Y || c == GUIDE_COMBO_START;
}

static button_t face_button(guide_combo_t c)
{
    switch (c) {
    case GUIDE_COMBO_A: return BUTTON_A;
    case GUIDE_COMBO_B: return BUTTON_B;
    case GUIDE_COMBO_X: return BUTTON_X;
    case GUIDE_COMBO_Y: return BUTTON_Y;
    default:            return BUTTON_START;
    }
}

static const tr_id_t ACTION_WHAT[BUTTON_ACTION_COUNT] = {
    [BUTTON_ENTER]  = TR_GUIDE_DO_ENTER,
    [BUTTON_SPACE]  = TR_GUIDE_DO_SPACE,
    [BUTTON_DELETE] = TR_GUIDE_DO_DELETE,
    [BUTTON_ESC]    = TR_GUIDE_DO_ESC,
    [BUTTON_TAB]    = TR_GUIDE_DO_TAB,
    [BUTTON_STOP]   = TR_GUIDE_DO_STOP,
};

// CONTROL and KEYS: the face buttons send what Settings' BUTTONS tab says
// (main.c's FACE table reads the same map), so the entry is the action's.
static guide_entry_t control_entry(guide_ctx_t ctx, const guide_config_t *cfg,
                                   guide_combo_t combo)
{
    const int keys = ctx == GUIDE_CTX_KEYS;
    if (is_face(combo)) {
        // Off the Control screen X is "back" before it is anything else:
        // status_mode_back() swallows it, so nothing is sent.
        if (keys && combo == GUIDE_COMBO_X) {
            return entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_X_BACK);
        }
        button_map_t defaults;
        const button_map_t *map = cfg ? cfg->map : NULL;
        if (!map) {
            button_map_defaults(&defaults);
            map = &defaults;
        }
        const unsigned a = map->action[face_button(combo)];
        guide_entry_t e = entry_of(combo, TR_COUNT, TR_GUIDE_DO_NOTHING);
        if (a < BUTTON_ACTION_COUNT) {
            e.action = (button_action_t)a;
            e.what = ACTION_WHAT[a];
        }
        return e;
    }
    switch (combo) {
    case GUIDE_COMBO_SELECT:
        return entry_of(combo, TR_GUIDE_NAME_SCREENS, TR_GUIDE_DO_SELECT);
    case GUIDE_COMBO_DPAD:
        return keys ? entry_of(combo, TR_GUIDE_NAME_ARROWS, TR_GUIDE_DO_ARROWS)
                    : entry_of(combo, TR_GUIDE_NAME_SCROLL, TR_GUIDE_DO_SCROLL);
    case GUIDE_COMBO_L:
        return entry_of(combo, TR_GUIDE_NAME_CTRL, TR_GUIDE_DO_CTRL);
    case GUIDE_COMBO_R:
        return entry_of(combo, TR_GUIDE_NAME_SHIFT, TR_GUIDE_DO_SHIFT);
    case GUIDE_COMBO_SHIFT_TAB: {
        guide_entry_t e = entry_of(combo, TR_GUIDE_NAME_MODE, TR_GUIDE_DO_SHIFT_TAB);
        button_map_t defaults;
        const button_map_t *map = cfg ? cfg->map : NULL;
        if (!map) {
            button_map_defaults(&defaults);
            map = &defaults;
        }
        const button_t b = button_for(map, BUTTON_TAB);
        e.button = b < BUTTON_COUNT ? button_name(b) : "Y";
        return e;
    }
    case GUIDE_COMBO_LR:
        return keys ? entry_of(combo, TR_GUIDE_NAME_FOCUS, TR_GUIDE_DO_FOCUS_KEYS)
                    : entry_of(combo, TR_GUIDE_NAME_FOCUS, TR_GUIDE_DO_FOCUS_TILES);
    default:
        return nothing(combo);
    }
}

// COMPOSE and FOCUS: the composer's own keys (main.c's composer_input and
// composer_focus_input).  The mapping does not reach in here: the composer
// swallows every key, and A is always the send.
static guide_entry_t compose_entry(guide_ctx_t ctx, const guide_config_t *cfg,
                                   guide_combo_t combo, int recording)
{
    const int focus = ctx == GUIDE_CTX_FOCUS;
    switch (combo) {
    case GUIDE_COMBO_A:
        return focus ? entry_of(combo, TR_GUIDE_NAME_TYPE, TR_GUIDE_DO_TYPE)
                     : entry_of(combo, TR_GUIDE_NAME_SEND, TR_GUIDE_DO_SEND);
    case GUIDE_COMBO_B:
        return entry_of(combo, TR_GUIDE_NAME_DELETE, TR_GUIDE_DO_DEL);
    case GUIDE_COMBO_X:
        // A recording takes X first (voice_ptt_step's cancel); only then is
        // X the composer's own.
        if (recording) return entry_of(combo, TR_GUIDE_NAME_CANCEL, TR_GUIDE_DO_MIC_CANCEL);
        return focus ? entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_FOCUS_OUT)
                     : entry_of(combo, TR_GUIDE_NAME_CANCEL, TR_GUIDE_DO_CLOSE);
    case GUIDE_COMBO_Y:
        return entry_of(combo, TR_GUIDE_NAME_SNIPPETS, TR_GUIDE_DO_SNIPPETS);
    case GUIDE_COMBO_START:
        return focus ? entry_of(combo, TR_GUIDE_NAME_SEND, TR_GUIDE_DO_FOCUS_SEND)
                     : nothing(combo);
    case GUIDE_COMBO_DPAD:
        return focus ? entry_of(combo, TR_GUIDE_NAME_MOVE, TR_GUIDE_DO_MOVE_KEYS)
                     : nothing(combo);
    case GUIDE_COMBO_R:
    case GUIDE_COMBO_HOLD_R: {
        guide_entry_t e = entry_of(GUIDE_COMBO_HOLD_R, TR_GUIDE_NAME_TALK,
                                   cfg && cfg->talk ? TR_GUIDE_DO_TALK
                                                    : TR_GUIDE_DO_TALK_OFF);
        return e;
    }
    case GUIDE_COMBO_R_THEN_L:
        if (!recording) {
            return entry_of(combo, TR_GUIDE_NAME_ORDER, TR_GUIDE_DO_ORDER);
        }
        return cfg && cfg->latch
                   ? entry_of(combo, TR_GUIDE_NAME_LATCH, TR_GUIDE_DO_LATCH)
                   : entry_of(combo, TR_GUIDE_NAME_CANCEL, TR_GUIDE_DO_HOLD_CANCEL);
    case GUIDE_COMBO_L:
    case GUIDE_COMBO_LR:
    case GUIDE_COMBO_L_THEN_R:
        // L alone is half of the focus chord, and both at once are the
        // chord too (voice_ptt_step: "or both landed together").
        return focus ? entry_of(GUIDE_COMBO_L_THEN_R, TR_GUIDE_NAME_FOCUS,
                                TR_GUIDE_DO_FOCUS_OUT)
                     : entry_of(GUIDE_COMBO_L_THEN_R, TR_GUIDE_NAME_FOCUS,
                                TR_GUIDE_DO_FOCUS);
    default:
        return nothing(combo);
    }
}

// CAMERA: photo.c's photo_input, in its LIVE state.
static guide_entry_t camera_entry(const guide_config_t *cfg, guide_combo_t combo)
{
    const camera_shutter_t s = cfg ? cfg->shutter : SHUTTER_R;
    const guide_combo_t shutter_combo = s == SHUTTER_L ? GUIDE_COMBO_L
                                      : s == SHUTTER_A ? GUIDE_COMBO_A
                                                       : GUIDE_COMBO_R;
    if (combo == shutter_combo || combo == GUIDE_COMBO_SHUTTER) {
        guide_entry_t e = entry_of(GUIDE_COMBO_SHUTTER, TR_GUIDE_NAME_SHUTTER,
                                   TR_GUIDE_DO_SHUTTER);
        e.button = camera_shutter_name(s);
        return e;
    }
    switch (combo) {
    case GUIDE_COMBO_A:
        return entry_of(combo, TR_GUIDE_NAME_SEND, TR_GUIDE_DO_CAM_SEND);
    case GUIDE_COMBO_B:
        return entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_CAM_B);
    case GUIDE_COMBO_X:
        return entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_CAM_X);
    case GUIDE_COMBO_Y:
        return entry_of(combo, TR_GUIDE_NAME_FLIP, TR_GUIDE_DO_FLIP);
    case GUIDE_COMBO_SELECT:
        return entry_of(combo, TR_GUIDE_NAME_SCREENS, TR_GUIDE_DO_CAM_SELECT);
    case GUIDE_COMBO_DPAD:
    case GUIDE_COMBO_LR:
        return entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_CAM_LEAVE);
    default:
        return nothing(combo);
    }
}

// QUICK and the menus like it: main.c's quick_input.
static guide_entry_t quick_entry(guide_combo_t combo)
{
    switch (combo) {
    case GUIDE_COMBO_DPAD:
        return entry_of(combo, TR_GUIDE_NAME_MOVE, TR_GUIDE_DO_MOVE);
    case GUIDE_COMBO_A:
        return entry_of(combo, TR_GUIDE_NAME_OPEN, TR_GUIDE_DO_OPEN);
    case GUIDE_COMBO_B:
    case GUIDE_COMBO_X:
        return entry_of(combo, TR_GUIDE_NAME_BACK, TR_GUIDE_DO_BACK);
    case GUIDE_COMBO_Y:
        return entry_of(combo, TR_GUIDE_NAME_FORGET, TR_GUIDE_DO_FORGET);
    case GUIDE_COMBO_LR:
        return entry_of(combo, TR_GUIDE_NAME_CLOSE, TR_GUIDE_DO_CLOSE_MENU);
    default:
        return nothing(combo);
    }
}

guide_entry_t guide_entry(guide_ctx_t ctx, const guide_config_t *cfg,
                          guide_combo_t combo, int recording)
{
    if (combo == GUIDE_COMBO_INTRO) {
        return entry_of(combo, TR_GUIDE_TITLE, TR_GUIDE_INTRO);
    }
    switch (ctx) {
    case GUIDE_CTX_CONTROL:
    case GUIDE_CTX_KEYS:
        return control_entry(ctx, cfg, combo);
    case GUIDE_CTX_COMPOSE:
    case GUIDE_CTX_FOCUS:
        return compose_entry(ctx, cfg, combo, recording);
    case GUIDE_CTX_CAMERA:
        if (!guide_ctx_present(ctx, cfg)) return nothing(combo);
        return camera_entry(cfg, combo);
    case GUIDE_CTX_QUICK:
        return quick_entry(combo);
    default:
        return nothing(combo);
    }
}

// A mapped button's action as a word in the language on screen -- "ENTER",
// "INTRO", "确认" -- upper-cased like the guide's other names (ASCII only;
// nothing else has a case worth changing here).  One slot per action, so two
// calls in one argument list never share a buffer.
static const char *action_word(button_action_t action)
{
    static char words[BUTTON_ACTION_COUNT][LANG_STR_MAX + 1];
    if (action >= BUTTON_ACTION_COUNT) return "";
    const char *hint = button_action_hint(action);
    size_t k = 0;
    for (; hint[k] && k < LANG_STR_MAX; k++) {
        const char c = hint[k];
        words[action][k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    words[action][k] = '\0';
    return words[action];
}

// The narrowest place the drawing puts a button's label: a face button's
// legend, "X label" in 11 columns (test_guide_text_and_geometry_fit).
#define GUIDE_LABEL_COLS 9u

const char *guide_button_label(guide_ctx_t ctx, const guide_config_t *cfg,
                               guide_combo_t combo)
{
    const guide_entry_t e = guide_entry(ctx, cfg, combo, 0);
    if (e.name == TR_COUNT) {
        // English keeps the chip's own abbreviation, "ENT" -- the same
        // letters SETTINGS -> BUTTONS shows.  A translation shows its word
        // when it fits, rather than an English abbreviation among its own.
        const char *word = action_word(e.action);
        if (lang_current() != LANG_EN &&
            lang_display_cols(word) <= GUIDE_LABEL_COLS) {
            return word;
        }
        return button_action_label(e.action);
    }
    if (e.name == TR_GUIDE_NAME_NOTHING) return "-";
    return tr(e.name);
}

void guide_entry_texts(const guide_entry_t *e, char *combo, size_t combo_cap,
                       char *name, size_t name_cap, char *what, size_t what_cap)
{
    if (!e) return;
    if (combo && combo_cap) {
        if (e->combo_text == TR_COUNT) {
            snprintf(combo, combo_cap, "%s", e->button ? e->button : "");
        } else {
            (void)lang_fill(combo, combo_cap, tr(e->combo_text),
                            e->button ? e->button : "");
        }
    }
    if (name && name_cap) {
        // The pop-up has a line to itself for the name, so the whole word.
        snprintf(name, name_cap, "%s",
                 e->name == TR_COUNT ? action_word(e->action) : tr(e->name));
    }
    if (what && what_cap) snprintf(what, what_cap, "%s", tr(e->what));
}

// ----------------------------------------------------------------- input ---

static void context_reset(guide_t *g)
{
    voice_ptt_init(&g->ptt);
    g->recording = 0;
    g->shift_armed = 0;
    camera_shutter_reset(&g->shutter);
}

void guide_init(guide_t *g, guide_ctx_t ctx, const guide_config_t *cfg)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->ctx = guide_ctx_present(ctx, cfg) ? ctx : GUIDE_CTX_CONTROL;
    context_reset(g);
    // The pop-up the guide opens with says how to use it, and stays until
    // the first press replaces it.
    g->entry = guide_entry(g->ctx, cfg, GUIDE_COMBO_INTRO, 0);
    g->shown = 1;
    g->shown_frames = 0;
}

void guide_set_ctx(guide_t *g, guide_ctx_t ctx, const guide_config_t *cfg)
{
    if (!g || !guide_ctx_present(ctx, cfg)) return;
    g->ctx = ctx;
    context_reset(g);
}

static guide_step_t show(guide_t *g, guide_entry_t e)
{
    g->entry = e;
    g->shown = 1;
    g->shown_frames = 0;
    return GUIDE_STEP_SHOW;
}

// The first single press in `edge`, in the order main.c's FACE table and
// the menus test them.
static guide_combo_t single_press(unsigned edge)
{
    if (edge & GUIDE_KEY_A) return GUIDE_COMBO_A;
    if (edge & GUIDE_KEY_B) return GUIDE_COMBO_B;
    if (edge & GUIDE_KEY_X) return GUIDE_COMBO_X;
    if (edge & GUIDE_KEY_Y) return GUIDE_COMBO_Y;
    if (edge & GUIDE_KEY_START) return GUIDE_COMBO_START;
    if (edge & GUIDE_KEY_SELECT) return GUIDE_COMBO_SELECT;
    if (edge & (GUIDE_KEY_UP | GUIDE_KEY_DOWN)) return GUIDE_COMBO_DPAD;
    return GUIDE_COMBO_NONE;
}

static int lr_chord(unsigned held, unsigned edge)
{
    return (held & (GUIDE_KEY_L | GUIDE_KEY_R)) == (GUIDE_KEY_L | GUIDE_KEY_R) &&
           (edge & (GUIDE_KEY_L | GUIDE_KEY_R)) != 0;
}

static guide_step_t step_control(guide_t *g, const guide_config_t *cfg,
                                 unsigned held, unsigned edge)
{
    // poll_input's order: the chord first, then the shoulders' one-shot
    // modifiers, then the face table.
    if (lr_chord(held, edge)) {
        g->shift_armed = 0;
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_LR, 0));
    }
    if (edge & GUIDE_KEY_L) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_L, 0));
    }
    if (edge & GUIDE_KEY_R) {
        // One-shot: press twice to disarm (docs/design/INPUT_AND_KEYBOARD.md).
        g->shift_armed = !g->shift_armed;
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_R, 0));
    }
    const guide_combo_t c = single_press(edge);
    if (c == GUIDE_COMBO_NONE) return GUIDE_STEP_NONE;
    if (is_face(c) && !(g->ctx == GUIDE_CTX_KEYS && c == GUIDE_COMBO_X)) {
        button_map_t defaults;
        const button_map_t *map = cfg && cfg->map ? cfg->map : NULL;
        if (!map) {
            button_map_defaults(&defaults);
            map = &defaults;
        }
        const int armed = g->shift_armed;
        g->shift_armed = 0;
        if (armed && map->action[face_button(c)] == BUTTON_TAB) {
            return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_SHIFT_TAB, 0));
        }
    }
    if (c == GUIDE_COMBO_DPAD) g->shift_armed = 0;
    return show(g, guide_entry(g->ctx, cfg, c, 0));
}

static guide_step_t step_compose(guide_t *g, const guide_config_t *cfg,
                                 unsigned held, unsigned edge)
{
    // The composer's own shoulder machine, fed exactly as composer_input
    // feeds it: which came first, L or R, is its decision, not this one's.
    const voice_ptt_action_t ptt = voice_ptt_step(
        &g->ptt, cfg && cfg->latch,
        (held & GUIDE_KEY_R) != 0, (held & GUIDE_KEY_L) != 0,
        (edge & GUIDE_KEY_R) != 0, (edge & GUIDE_KEY_L) != 0,
        (edge & GUIDE_KEY_X) != 0, g->recording, 0, 0);
    switch (ptt) {
    case VOICE_PTT_FOCUS:
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_L_THEN_R, 0));
    case VOICE_PTT_START:
        // A computer without speech-to-text never starts one (main.c asks
        // mic_can_transcribe() first), so neither does the pretend one.
        if (cfg && cfg->talk) g->recording = 1;
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_HOLD_R, 0));
    case VOICE_PTT_LATCH:
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_R_THEN_L, 1));
    case VOICE_PTT_STOP:
    case VOICE_PTT_AUTO_STOP:
        // Let go: the words would go into the draft.  The TALK pop-up
        // already says so; keep it.
        g->recording = 0;
        return GUIDE_STEP_NONE;
    case VOICE_PTT_CANCEL:
        g->recording = 0;
        if (edge & GUIDE_KEY_X) {
            return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_X, 1));
        } else {
            // R then L while recording, not latching: the cancel.
            guide_entry_t e = guide_entry(g->ctx, cfg, GUIDE_COMBO_R_THEN_L, 1);
            e.name = TR_GUIDE_NAME_CANCEL;
            e.what = TR_GUIDE_DO_HOLD_CANCEL;
            return show(g, e);
        }
    case VOICE_PTT_NONE:
    default:
        break;
    }
    if (g->ptt.chord_notice) {
        // R first, then L, with nothing recording: the order notice.
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_R_THEN_L, 0));
    }
    // A shoulder on its own explains the combo it starts.
    if ((edge & GUIDE_KEY_R) && !(held & GUIDE_KEY_L)) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_HOLD_R, 0));
    }
    if ((edge & GUIDE_KEY_L) && !(held & GUIDE_KEY_R)) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_L, 0));
    }
    const guide_combo_t c = single_press(edge);
    if (c == GUIDE_COMBO_NONE) return GUIDE_STEP_NONE;
    return show(g, guide_entry(g->ctx, cfg, c, g->recording));
}

static guide_step_t step_camera(guide_t *g, const guide_config_t *cfg,
                                unsigned held, unsigned edge)
{
    const camera_shutter_t s = cfg ? cfg->shutter : SHUTTER_R;
    // photo_input's order: the leave chord, X, SELECT, then the state's keys.
    if (camera_lr_chord(held, edge)) {
        camera_shutter_reset(&g->shutter);
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_LR, 0));
    }
    if (edge & GUIDE_KEY_X) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_X, 0));
    if (edge & GUIDE_KEY_SELECT) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_SELECT, 0));
    }
    if (edge & GUIDE_KEY_B) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_B, 0));
    if (edge & (GUIDE_KEY_UP | GUIDE_KEY_DOWN)) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_DPAD, 0));
    }
    if (edge & GUIDE_KEY_Y) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_Y, 0));
    // The shutter: armed on the press, fired on the release, spoiled by the
    // other shoulder (camera_shutter_input, as photo.c runs it).
    const unsigned mask = camera_shutter_key_mask(s);
    const int pressed = (edge & mask) != 0;
    const int fired = camera_shutter_input(&g->shutter, mask,
                                           camera_shutter_other_mask(s),
                                           held, edge);
    if (pressed || fired) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_SHUTTER, 0));
    }
    if (edge & GUIDE_KEY_A) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_A, 0));
    if (edge & GUIDE_KEY_START) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_START, 0));
    }
    if (edge & GUIDE_KEY_L) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_L, 0));
    if (edge & GUIDE_KEY_R) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_R, 0));
    return GUIDE_STEP_NONE;
}

static guide_step_t step_quick(guide_t *g, const guide_config_t *cfg,
                               unsigned held, unsigned edge)
{
    if (lr_chord(held, edge)) {
        return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_LR, 0));
    }
    const guide_combo_t c = single_press(edge);
    if (c != GUIDE_COMBO_NONE) return show(g, guide_entry(g->ctx, cfg, c, 0));
    if (edge & GUIDE_KEY_L) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_L, 0));
    if (edge & GUIDE_KEY_R) return show(g, guide_entry(g->ctx, cfg, GUIDE_COMBO_R, 0));
    return GUIDE_STEP_NONE;
}

guide_step_t guide_step(guide_t *g, const guide_config_t *cfg,
                        unsigned held, unsigned edge)
{
    if (!g) return GUIDE_STEP_NONE;

    // The ways out come first, in every context: SELECT + START (the chord
    // that opened it), or B held for a second.
    const unsigned both = GUIDE_KEY_SELECT | GUIDE_KEY_START;
    if ((held & both) == both && (edge & both)) return GUIDE_STEP_LEAVE;
    if (held & GUIDE_KEY_B) {
        if (++g->b_frames >= GUIDE_B_HOLD_FRAMES) {
            g->b_frames = 0;
            return GUIDE_STEP_LEAVE;
        }
    } else {
        g->b_frames = 0;
    }

    // Left and right pick the context; up and down are still demonstrated.
    if (edge & (GUIDE_KEY_LEFT | GUIDE_KEY_RIGHT)) {
        g->ctx = guide_ctx_step(g->ctx, (edge & GUIDE_KEY_RIGHT) ? 1 : -1, cfg);
        context_reset(g);
        return GUIDE_STEP_CONTEXT;
    }
    if (!guide_ctx_present(g->ctx, cfg)) {
        g->ctx = GUIDE_CTX_CONTROL;
        context_reset(g);
    }

    switch (g->ctx) {
    case GUIDE_CTX_CONTROL:
    case GUIDE_CTX_KEYS:
        return step_control(g, cfg, held, edge);
    case GUIDE_CTX_COMPOSE:
    case GUIDE_CTX_FOCUS:
        return step_compose(g, cfg, held, edge);
    case GUIDE_CTX_CAMERA:
        return step_camera(g, cfg, held, edge);
    case GUIDE_CTX_QUICK:
    default:
        return step_quick(g, cfg, held, edge);
    }
}

int guide_tick(guide_t *g)
{
    // The opening pop-up waits for the first press; every other one goes
    // after about three seconds, or sooner when the next press replaces it.
    if (!g || !g->shown || g->entry.combo == GUIDE_COMBO_INTRO) return 0;
    if (++g->shown_frames < GUIDE_POPUP_FRAMES) return 0;
    g->shown = 0;
    g->shown_frames = 0;
    return 1;
}

// -------------------------------------------------------------- wrapping ---

unsigned guide_wrap(const char *s, unsigned cols, char lines[2][GUIDE_LINE_MAX])
{
    lines[0][0] = lines[1][0] = '\0';
    if (!s) return 1;
    const char *p = s;
    for (unsigned line = 0; line < 2; line++) {
        while (*p == ' ') p++;
        if (!*p) return line ? line : 1;
        const char *start = p, *q = p;
        const char *brk_end = NULL, *brk_next = NULL;
        unsigned used = 0;
        int overflow = 0;
        while (*q) {
            const char *at = q;
            const uint32_t cp = lang_utf8_next(&q);
            const unsigned w = lang_cp_is_wide(cp) ? 2u : 1u;
            if (used + w > cols) { q = at; overflow = 1; break; }
            used += w;
            // A line may end at a space (which is dropped) or after a wide
            // character: CJK text breaks between any two of them.
            if (cp == ' ') { brk_end = at; brk_next = q; }
            else if (w == 2) { brk_end = q; brk_next = q; }
        }
        const char *end = q, *next = q;
        if (overflow && brk_end && brk_end > start) {
            end = brk_end;
            next = brk_next;
        }
        size_t n = (size_t)(end - start);
        if (n >= GUIDE_LINE_MAX) n = GUIDE_LINE_MAX - 1;
        memcpy(lines[line], start, n);
        lines[line][n] = '\0';
        while (n > 0 && lines[line][n - 1] == ' ') lines[line][--n] = '\0';
        p = next;
        while (*p == ' ') p++;
        if (!*p) return line + 1;
    }
    return 0;
}

// --------------------------------------------------------- the geometry ---

int guide_tab_rect(guide_ctx_t ctx, const guide_config_t *cfg,
                   int *x, int *y, int *w, int *h)
{
    if (!guide_ctx_present(ctx, cfg)) return 0;
    // Each tab is its label and two pixels a side, then the row's spare
    // width is shared out so the strip spans the screen.
    int widths[GUIDE_CTX_COUNT], n = 0, total = 0;
    for (int i = 0; i < GUIDE_CTX_COUNT; i++) {
        widths[i] = 0;
        if (!guide_ctx_present((guide_ctx_t)i, cfg)) continue;
        widths[i] = (int)lang_display_cols(tr(CTX_LABEL[i])) * 6 + 4;
        total += widths[i];
        n++;
    }
    const int gap = 2;
    int spare = 256 - total - gap * (n - 1);
    if (spare < 0) spare = 0;
    const int each = spare / n;
    int left = spare - each * n;
    int at = 0;
    for (int i = 0; i < GUIDE_CTX_COUNT; i++) {
        if (!widths[i]) continue;
        int tw = widths[i] + each;
        if (left > 0) { tw++; left--; }
        if (i == (int)ctx) {
            if (x) *x = at;
            if (y) *y = GUIDE_TAB_Y;
            if (w) *w = tw;
            if (h) *h = GUIDE_TAB_H;
            return 1;
        }
        at += tw + gap;
    }
    return 0;
}

guide_ctx_t guide_tab_hit(const guide_config_t *cfg, int x, int y)
{
    for (int i = 0; i < GUIDE_CTX_COUNT; i++) {
        int tx, ty, tw, th;
        if (!guide_tab_rect((guide_ctx_t)i, cfg, &tx, &ty, &tw, &th)) continue;
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th) return (guide_ctx_t)i;
    }
    return GUIDE_CTX_COUNT;
}

int guide_done_hit(int x, int y)
{
    return x >= GUIDE_DONE_X && x < GUIDE_DONE_X + GUIDE_DONE_W &&
           y >= GUIDE_DONE_Y && y < GUIDE_DONE_Y + GUIDE_DONE_H;
}

console_family_t guide_console(console_family_t family)
{
    switch (family) {
    case CONSOLE_DS:
    case CONSOLE_DS_LITE:
    case CONSOLE_DSI:
    case CONSOLE_3DS:
        return family;
    case CONSOLE_UNKNOWN:
    default:
        return CONSOLE_DSI;
    }
}

// ------------------------------------------------------------ the session ---

// Wire bytes for the box and bullet glyphs (dsterm/extmap.py).
#define G_H   "\x80"   // ─
#define G_V   "\x81"   // │
#define G_TL  "\x8b"   // ╭
#define G_TR  "\x8c"   // ╮
#define G_BR  "\x8d"   // ╯
#define G_BL  "\x8e"   // ╰
#define G_ELB "\x84"   // └
#define G_DOT "\xa6"   // ●
#define G_STAR "\xb0"  // ✻

static void put(grid_t *g, int row, int col, const char *text, uint8_t fg,
                uint8_t flags)
{
    if (row < 0 || row >= PROTO_ROWS) return;
    for (; *text && col < PROTO_COLS; text++, col++) {
        cell_t *c = &g->cells[row * PROTO_COLS + col];
        c->glyph = (uint8_t)*text;
        c->attr = (uint8_t)(fg << 4);
        c->flags = flags;
    }
}

int guide_input_row(void)
{
    return PROTO_ROWS - 4;
}

int guide_input_px(void)
{
    // The terminal's cell height and top margin at this build's grid
    // (render.c's TERM_CELL_H and ORIGIN_Y).
    const int cell_h = 192 / PROTO_ROWS;
    const int origin_y = (192 - PROTO_ROWS * cell_h) / 2;
    return origin_y + guide_input_row() * cell_h;
}

void guide_session_fill(grid_t *g)
{
    if (!g) return;
    grid_clear(g);
    put(g, 0, 0, G_STAR " Welcome to Claude Code", PAL_ORANGE, CELL_FLAG_BOLD);
    put(g, 1, 0, "  cwd: ~/projects/parser", PAL_DEFAULT, 0);
    put(g, 3, 0, "> the empty-string test fails, fix it", PAL_WHITE, 0);
    put(g, 5, 0, G_DOT, PAL_GREEN, 0);
    put(g, 5, 2, "Read(parser.py)", PAL_WHITE, CELL_FLAG_BOLD);
    put(g, 6, 0, "  " G_ELB " Read 48 lines", PAL_DEFAULT, 0);
    put(g, 8, 0, G_DOT, PAL_WHITE, 0);
    put(g, 8, 2, "parse(\"\") returns None; the test", PAL_DEFAULT, 0);
    put(g, 9, 2, "expects []. One line fixes it.", PAL_DEFAULT, 0);
    put(g, 11, 0, G_DOT, PAL_GREEN, 0);
    put(g, 11, 2, "Update(parser.py)", PAL_WHITE, CELL_FLAG_BOLD);
    put(g, 12, 0, "  " G_ELB " 12 -     return None", PAL_RED, 0);
    put(g, 13, 0, "    12 +     return []", PAL_GREEN, 0);
    put(g, 15, 0, G_DOT, PAL_WHITE, 0);
    put(g, 15, 2, "Fixed: 48 tests pass.", PAL_DEFAULT, 0);

    // The input box, the full width of the grid, and the hint under it.
    const int top = guide_input_row();
    static char rule[PROTO_COLS * 3 + 1];
    size_t at = 0;
    at += (size_t)snprintf(rule + at, sizeof(rule) - at, "%s", G_TL);
    for (int c = 1; c < PROTO_COLS - 1; c++) {
        at += (size_t)snprintf(rule + at, sizeof(rule) - at, "%s", G_H);
    }
    snprintf(rule + at, sizeof(rule) - at, "%s", G_TR);
    put(g, top, 0, rule, PAL_DEFAULT, 0);
    rule[0] = G_BL[0];
    rule[PROTO_COLS - 1] = G_BR[0];
    put(g, top + 2, 0, rule, PAL_DEFAULT, 0);
    put(g, top + 1, 0, G_V, PAL_DEFAULT, 0);
    put(g, top + 1, 2, ">", PAL_WHITE, 0);
    put(g, top + 1, PROTO_COLS - 1, G_V, PAL_DEFAULT, 0);
    put(g, top + 3, 2, "? for shortcuts", PAL_DEFAULT, 0);
}

void guide_popup_rect(unsigned lines, int *x, int *y, int *w, int *h)
{
    if (lines < 1) lines = 1;
    if (lines > 4) lines = 4;
    const int bh = (int)lines * GUIDE_POPUP_PITCH + 2 * GUIDE_POPUP_PAD;
    // At least six pixels clear of the input box's top rule: the box covers
    // the transcript, which is a backdrop, and never the line you would type
    // on.  Its top is on a text row's edge, so no row shows a sliver above.
    const int cell_h = 192 / PROTO_ROWS;
    const int origin_y = (192 - PROTO_ROWS * cell_h) / 2;
    int by = guide_input_px() - 6 - bh;
    by = origin_y + ((by - origin_y) / cell_h) * cell_h;
    if (x) *x = GUIDE_POPUP_X;
    if (y) *y = by;
    if (w) *w = GUIDE_POPUP_W;
    if (h) *h = bh;
}

unsigned guide_popup_lines(const guide_entry_t *e,
                           char lines[GUIDE_POPUP_LINES][GUIDE_LINE_MAX])
{
    for (unsigned i = 0; i < GUIDE_POPUP_LINES; i++) lines[i][0] = '\0';
    if (!e) return 0;
    static char what[GUIDE_LINE_MAX * 2];
    guide_entry_texts(e, lines[0], GUIDE_LINE_MAX, lines[1], GUIDE_LINE_MAX,
                      what, sizeof(what));
    char wrapped[2][GUIDE_LINE_MAX];
    unsigned n = guide_wrap(what, GUIDE_POPUP_COLS, wrapped);
    // A custom pack's text that does not fit two lines is cut, not spilled.
    if (n == 0) n = 2;
    memcpy(lines[2], wrapped[0], GUIDE_LINE_MAX);
    if (n > 1) memcpy(lines[3], wrapped[1], GUIDE_LINE_MAX);
    return 2 + n;
}
