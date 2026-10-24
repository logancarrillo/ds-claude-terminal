// UI localization and custom SD-card language packs.
//
// English (the default), Spanish, Chinese and Japanese are compiled into the
// ROM, so a card with no extra files on it can switch between them at any
// time from SETTINGS -> SCREEN -> LANGUAGE.
//
// Additional languages can be dropped onto the SD card as UTF-8 key=value
// files under /dsterm/lang/<code.lang>.  Every key is optional: a key a
// custom file does not set falls back to English, so a partial file never
// leaves a blank button.
//
// Pure C with no libnds dependency; host-tested and fuzzed.

#ifndef DSTERM_LANG_H
#define DSTERM_LANG_H

#include <stddef.h>
#include <stdint.h>

// Chinese and Japanese draw from Fusion Pixel Font 10px (SIL OFL 1.1, see
// THIRD_PARTY_NOTICES.md): glyphs designed at that size, in a 12x10 cell,
// subset by tools/genuifont.py to the characters the built-in strings use.
// The first atlas squeezed full-size Hanzi into 8 pixels and they came out
// as blobs; a language you pick and then cannot read is worse than none,
// since the way back is through the same menu. Render before changing them.
typedef enum {
    LANG_EN = 0,    // English (default)
    LANG_ES = 1,    // Espanol
    LANG_ZH,        // Chinese (Simplified)
    LANG_JA,        // Japanese
    LANG_BUILTIN_COUNT,
} lang_builtin_t;

// Up to two custom language files from /dsterm/lang/*.lang join the
// built-ins on the SETTINGS -> SCREEN -> LANGUAGE row (never more than
// SETTINGS_OPTIONS_MAX chips).
#define LANG_CUSTOM_MAX 2
#define LANG_MAX (LANG_BUILTIN_COUNT + LANG_CUSTOM_MAX)

#define LANG_CODE_MAX 7     // e.g. "en", "es", "zh", "ja", "fr", "pt-br"
#define LANG_LABEL_MAX 4    // chip text: "EN", "ES", "ZH", "JA", "FR"
#define LANG_NAME_MAX 24    // native name: "English", "Español", "中文", "日本語"
#define LANG_STR_MAX 63     // max UTF-8 bytes per localized UI string
#define LANG_FILE_MAX 8192  // max bytes read from a .lang file

// Every translatable UI string in the handheld's chrome.  Terminal cells from
// the PTY are never passed through this table.
typedef enum {
    TR_CONTROL_KEYS = 0,
    TR_CONTROL_KEYS_SUB,
    TR_CONTROL_QUICK,
    TR_CONTROL_QUICK_SUB,
    TR_CONTROL_PHOTO,
    TR_CONTROL_PHOTO_SUB,
    TR_CONTROL_AGAIN,
    TR_CONTROL_AGAIN_SUB,
    TR_CONTROL_COMPOSE,
    TR_CONTROL_COMPOSE_SUB,
    TR_CONTROL_REMOTE,
    TR_CONTROL_REMOTE_SUB,
    TR_CONTROL_SETTINGS,
    TR_CONTROL_SETTINGS_SUB,
    TR_CONTROL_RELINK,
    TR_CONTROL_HINT_SCROLL,
    TR_CONTROL_HINT_FOCUS,
    TR_COMPOSE_HEAD,
    TR_COMPOSE_KEYS,
    TR_COMPOSE_FOCUS_HEAD,
    TR_COMPOSE_FOCUS_KEYS,
    TR_COMPOSE_MIC_IDLE,
    TR_COMPOSE_MIC_REC,
    TR_COMPOSE_MIC_LATCH,
    TR_COMPOSE_MIC_WAIT,
    TR_KEYS_FLOOR_0,
    TR_KEYS_FLOOR_1,
    TR_QUICK_HEAD,
    TR_QUICK_SUB,
    TR_REMOTE_HEAD,
    TR_PROJECT_HINT,
    TR_PROJECT_DO_SAFE,
    TR_PROJECT_DO_GOD,
    TR_STATUS_PASTE,
    TR_STATUS_NO_PASTE,
    TR_CHIME_IDLE,
    TR_CHIME_WORKING,
    TR_CHIME_APPROVAL,
    TR_CHIME_QUESTION,
    TR_CHIME_LANTERN,
    TR_CHIME_SENT,
    TR_CHIME_ANSWERING,
    TR_CHIME_DONE,
    TR_CHIME_ANSWERED,
    TR_SETTINGS_TAB_SESSION,
    TR_SETTINGS_TAB_SOUND,
    TR_SETTINGS_TAB_SCREEN,
    TR_SETTINGS_TAB_LINK,
    TR_SETTINGS_TAB_BUTTONS,
    TR_SETTINGS_ROW_MODEL,
    TR_SETTINGS_ROW_SESSION,
    TR_SETTINGS_ROW_GOD_MODE,
    TR_SETTINGS_ROW_SOUND,
    TR_SETTINGS_ROW_VOLUME,
    TR_SETTINGS_ROW_TYPING,
    TR_SETTINGS_ROW_BLIPS,
    TR_SETTINGS_ROW_THEME,
    TR_SETTINGS_ROW_LANGUAGE,
    TR_SETTINGS_ROW_BOOT,
    TR_SETTINGS_ROW_ADDRESSES,
    TR_SETTINGS_ROW_AUTOCONNECT,
    TR_SETTINGS_ROW_VIEW_TAPS,
    TR_SETTINGS_ROW_PICTURE,
    TR_SETTINGS_ROW_LIVEVIEW,
    TR_SETTINGS_ROW_MIC,
    TR_SETTINGS_ROW_SHUTTER,
    TR_SETTINGS_ROW_DEFAULTS,
    TR_QUICK_ROW_MODE,
    TR_QUICK_ROW_MODE_NOTE,
    TR_QUICK_ROW_UNDO,
    TR_QUICK_ROW_UNDO_NOTE,
    TR_QUICK_ROW_CHANGES,
    TR_QUICK_ROW_CHANGES_NOTE,
    TR_QUICK_ROW_COMMANDS,
    TR_QUICK_ROW_COMMANDS_NOTE,
    TR_QUICK_ROW_AGAIN,
    TR_QUICK_ROW_AGAIN_NOTE,
    TR_QUICK_ROW_VOICE,
    TR_QUICK_ROW_VOICE_NOTE,
    TR_QUICK_ROW_PHOTO,
    TR_QUICK_ROW_PHOTO_NOTE,
    TR_QUICK_FOOT,
    TR_QUICK_ADD,
    TR_QUICK_CMD_HEAD,
    TR_QUICK_CMD_HEAD_SAVED,
    TR_QUICK_CMD_HEAD_SURE,
    TR_QUICK_CMD_HEAD_ADD,
    TR_QUICK_CMD_SUB,
    TR_QUICK_AGAIN_HEAD,
    TR_QUICK_AGAIN_SUB,
    TR_QUICK_AGAIN_EMPTY_0,
    TR_QUICK_AGAIN_EMPTY_1,
    TR_CHIME_YES,
    TR_CHIME_NO,
    TR_CONTROL_HINT_CONFIRM,    // "%s" is the button that confirms
    TR_CONTROL_HINT_UNDO,       // likewise
    TR_CONTROL_QUICK_MODE,      // "%s" is Claude's mode word
    TR_BUTTON_ENTER,            // the legend's words for what a button sends
    TR_BUTTON_SPACE,
    TR_BUTTON_DELETE,
    TR_BUTTON_ESC,
    TR_BUTTON_TAB,
    TR_BUTTON_STOP,
    TR_LEGEND_MODE,
    TR_LEGEND_SCREENS,
    TR_LEGEND_SHOULDERS,
    // The controls guide (guide.h) and the hints that point at it.
    TR_GUIDE_TAB_CONTROL,
    TR_GUIDE_TAB_KEYS,
    TR_GUIDE_TAB_COMPOSE,
    TR_GUIDE_TAB_FOCUS,
    TR_GUIDE_TAB_CAMERA,
    TR_GUIDE_TAB_QUICK,
    TR_GUIDE_DONE,
    TR_GUIDE_PRESS,
    TR_GUIDE_TABS_HINT,
    TR_GUIDE_FOOT,
    TR_GUIDE_TITLE,
    TR_GUIDE_INTRO,
    TR_GUIDE_COMBO_L_THEN_R,
    TR_GUIDE_COMBO_R_THEN_L,
    TR_GUIDE_COMBO_HOLD_R,
    TR_GUIDE_COMBO_R_THEN,
    TR_GUIDE_COMBO_DPAD,
    TR_GUIDE_NAME_SCREENS,
    TR_GUIDE_NAME_SCROLL,
    TR_GUIDE_NAME_ARROWS,
    TR_GUIDE_NAME_CTRL,
    TR_GUIDE_NAME_SHIFT,
    TR_GUIDE_NAME_MODE,
    TR_GUIDE_NAME_FOCUS,
    TR_GUIDE_NAME_BACK,
    TR_GUIDE_NAME_SEND,
    TR_GUIDE_NAME_DELETE,
    TR_GUIDE_NAME_CANCEL,
    TR_GUIDE_NAME_SNIPPETS,
    TR_GUIDE_NAME_NOTHING,
    TR_GUIDE_NAME_TALK,
    TR_GUIDE_NAME_LATCH,
    TR_GUIDE_NAME_ORDER,
    TR_GUIDE_NAME_TYPE,
    TR_GUIDE_NAME_MOVE,
    TR_GUIDE_NAME_SHUTTER,
    TR_GUIDE_NAME_FLIP,
    TR_GUIDE_NAME_OPEN,
    TR_GUIDE_NAME_FORGET,
    TR_GUIDE_NAME_CLOSE,
    TR_GUIDE_DO_ENTER,
    TR_GUIDE_DO_SPACE,
    TR_GUIDE_DO_DELETE,
    TR_GUIDE_DO_ESC,
    TR_GUIDE_DO_TAB,
    TR_GUIDE_DO_STOP,
    TR_GUIDE_DO_X_BACK,
    TR_GUIDE_DO_SELECT,
    TR_GUIDE_DO_SCROLL,
    TR_GUIDE_DO_ARROWS,
    TR_GUIDE_DO_CTRL,
    TR_GUIDE_DO_SHIFT,
    TR_GUIDE_DO_SHIFT_TAB,
    TR_GUIDE_DO_FOCUS_TILES,
    TR_GUIDE_DO_FOCUS_KEYS,
    TR_GUIDE_DO_SEND,
    TR_GUIDE_DO_DEL,
    TR_GUIDE_DO_CLOSE,
    TR_GUIDE_DO_MIC_CANCEL,
    TR_GUIDE_DO_SNIPPETS,
    TR_GUIDE_DO_NOTHING,
    TR_GUIDE_DO_TALK,
    TR_GUIDE_DO_TALK_OFF,
    TR_GUIDE_DO_LATCH,
    TR_GUIDE_DO_HOLD_CANCEL,
    TR_GUIDE_DO_ORDER,
    TR_GUIDE_DO_FOCUS,
    TR_GUIDE_DO_TYPE,
    TR_GUIDE_DO_FOCUS_SEND,
    TR_GUIDE_DO_FOCUS_OUT,
    TR_GUIDE_DO_MOVE_KEYS,
    TR_GUIDE_DO_SHUTTER,
    TR_GUIDE_DO_CAM_SEND,
    TR_GUIDE_DO_CAM_LEAVE,
    TR_GUIDE_DO_FLIP,
    TR_GUIDE_DO_CAM_B,
    TR_GUIDE_DO_CAM_X,
    TR_GUIDE_DO_CAM_SELECT,
    TR_GUIDE_DO_MOVE,
    TR_GUIDE_DO_OPEN,
    TR_GUIDE_DO_BACK,
    TR_GUIDE_DO_FORGET,
    TR_GUIDE_DO_CLOSE_MENU,
    TR_COMPOSE_HINT_EDIT,
    TR_COMPOSE_HINT_FOCUS,
    TR_COMPOSE_HINT_TALK,
    TR_LEGEND_GUIDE,
    TR_QUICK_ROW_CONTROLS,
    TR_QUICK_ROW_CONTROLS_NOTE,
    TR_SETTINGS_ROW_CONTROLS,
    TR_SETTINGS_RESET_SURE,     // the DEFAULTS chip while its confirm is armed
    TR_COMPOSE_METER_LATCHED,   // "%s" is the eight-column level bar
    TR_COMPOSE_METER_LATCH,     // likewise, LATCH mode before L latches
    TR_COMPOSE_METER_HOLD,      // likewise, HOLD mode
    TR_COMPOSE_ADD_HEAD,        // the composer as QUICK's + ADD opens it
    TR_COMPOSE_ADD_KEYS,
    TR_COMPOSE_ADD_FOCUS_KEYS,
    TR_QUICK_NOTE_NOT_HERE,     // VOICE / PHOTO notes until the server says
    TR_QUICK_NOTE_NO_STT,
    TR_QUICK_NOTE_NO_SAVE,
    TR_COUNT,
} tr_id_t;

// Reset custom languages and set active language to LANG_EN.
void lang_init(void);

// Total languages available (LANG_BUILTIN_COUNT + loaded custom packs).
unsigned lang_count(void);

// Active language index (0 .. lang_count() - 1).
unsigned lang_current(void);
void lang_set_current(unsigned index);

// Look up a language index by its code ("en", "es", "zh", "ja", or custom).
// Case-insensitive; returns -1 if unknown.
int lang_find_code(const char *code);

// Metadata for a language index.
const char *lang_code(unsigned index);
const char *lang_label(unsigned index);
const char *lang_name(unsigned index);

// The language hint VOICE_START carries for UI language `index`.  It is not
// always lang_code(): the server folds every transcript to printable ASCII
// (the composer and the wire carry nothing else), so dictating in Chinese or
// Japanese comes back as nothing at all.  A UI language written in a
// non-Latin script -- the built-in ZH and JA, and a custom pack whose code
// starts zh, ja or ko -- asks for English instead, and its compose bar says
// so.  Every other language passes its own code through.
const char *lang_voice_code(unsigned index);

// Translate a UI string ID in the active language (or in a specific index).
// Never returns NULL; falls back to LANG_EN for any unset key.
const char *tr(tr_id_t id);
const char *lang_tr_at(unsigned index, tr_id_t id);
// A built-in language's own string for `id`, or NULL where it has none (no
// English fallback): for the test that every built-in covers every key.
const char *lang_builtin_string(unsigned index, tr_id_t id);

// Copy `tmpl` into `out` with its first "%s" replaced by `arg`.  Nothing else
// in `tmpl` is interpreted (a custom pack's text is never a printf format),
// and the copy is cut on a whole UTF-8 character.  Returns `out`.
char *lang_fill(char *out, size_t cap, const char *tmpl, const char *arg);

// The dot-separated key name used in .lang files for `id`, or NULL.
const char *lang_key_name(tr_id_t id);
// Reverse lookup of a .lang key name, or TR_COUNT if unknown.
tr_id_t lang_key_find(const char *key);

// Parse and register one custom language pack from UTF-8 text (`key = value`
// lines).  Requires at least `code = ...` (or derives it from `fallback_code`).
// If a custom language with the same code is already registered, updates it in
// place.  Returns the language index (>= 0) on success, or -1 on error.
int lang_load_text(const char *text, const char *fallback_code,
                   char *err, size_t err_len);

// Load a `.lang` file from disk.
int lang_load_file(const char *path, const char *fallback_code,
                   char *err, size_t err_len);

// Decode one UTF-8 codepoint from `*s` and advance `*s`.  Returns 0 at NUL,
// and '?' (0x3F) on any malformed or overlong byte sequence (advancing 1 byte
// so a caller never loops forever).
uint32_t lang_utf8_next(const char **s);

// True if `cp` is a wide (2-cell / 12px) CJK or Kana character in the UI
// glyph atlas.
int lang_cp_is_wide(uint32_t cp);

// Visual width of a UTF-8 string in 6px cells (wide CJK/Kana count as 2).
unsigned lang_display_cols(const char *s);

// Format a UTF-8 string into `out` (capacity `cap`) padded with trailing
// spaces to at least `min_cols` visual columns and truncated on a whole
// UTF-8 character boundary to at most `max_cols` visual columns.  Returns
// the visual column count of `out`.
unsigned lang_pad_cols(const char *s, unsigned min_cols, unsigned max_cols,
                       char *out, size_t cap);

// The accented Latin-1 letters a language needs, drawn as the terminal
// font's own ASCII letter plus a mark, rather than the generated atlas's
// heavy downscales (where a, á, à and â were nearly the same blob).  `ascii`
// is a 6x8 table for 0x20..0x7E, one byte per row with bit 5 leftmost
// (font.h's font6x8).  Lower case keeps its shape and takes a two-row mark in
// the two empty rows above the x-height; upper case gives up one repeated
// row to make room for a one-row mark with a gap under it; cedillas hang in
// the empty bottom row.  Returns 1 and fills `out` for a letter it composes,
// 0 for anything else (the caller falls back to ui_font.h).
int lang_latin_glyph(uint32_t cp, const uint8_t ascii[][8], uint8_t out[8]);

#endif  // DSTERM_LANG_H
