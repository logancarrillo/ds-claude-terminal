#include "lang.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int used;
    char code[LANG_CODE_MAX + 1];
    char label[LANG_LABEL_MAX + 1];
    char name[LANG_NAME_MAX + 1];
    char strings[TR_COUNT][LANG_STR_MAX + 1];
} custom_lang_t;

static unsigned active_lang = LANG_EN;
static custom_lang_t custom_langs[LANG_CUSTOM_MAX];
static unsigned custom_count = 0;

static void set_err(char *err, size_t err_len, const char *msg)
{
    if (err && err_len) (void)snprintf(err, err_len, "%s", msg);
}

static int ascii_casecmp(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static const char *const KEY_NAMES[TR_COUNT] = {
    [TR_CONTROL_KEYS]         = "control.keys",
    [TR_CONTROL_KEYS_SUB]     = "control.keys.sub",
    [TR_CONTROL_QUICK]        = "control.quick",
    [TR_CONTROL_QUICK_SUB]    = "control.quick.sub",
    [TR_CONTROL_PHOTO]        = "control.photo",
    [TR_CONTROL_PHOTO_SUB]    = "control.photo.sub",
    [TR_CONTROL_AGAIN]        = "control.again",
    [TR_CONTROL_AGAIN_SUB]    = "control.again.sub",
    [TR_CONTROL_COMPOSE]      = "control.compose",
    [TR_CONTROL_COMPOSE_SUB]  = "control.compose.sub",
    [TR_CONTROL_REMOTE]       = "control.remote",
    [TR_CONTROL_REMOTE_SUB]   = "control.remote.sub",
    [TR_CONTROL_SETTINGS]     = "control.settings",
    [TR_CONTROL_SETTINGS_SUB] = "control.settings.sub",
    [TR_CONTROL_RELINK]       = "control.relink",
    [TR_CONTROL_HINT_SCROLL]  = "control.hint.scroll",
    [TR_CONTROL_HINT_FOCUS]   = "control.hint.focus",
    [TR_COMPOSE_HEAD]         = "compose.head",
    [TR_COMPOSE_KEYS]         = "compose.keys",
    [TR_COMPOSE_FOCUS_HEAD]   = "compose.focus.head",
    [TR_COMPOSE_FOCUS_KEYS]   = "compose.focus.keys",
    [TR_COMPOSE_MIC_IDLE]     = "compose.mic.idle",
    [TR_COMPOSE_MIC_REC]      = "compose.mic.rec",
    [TR_COMPOSE_MIC_LATCH]    = "compose.mic.latch",
    [TR_COMPOSE_MIC_WAIT]     = "compose.mic.wait",
    [TR_KEYS_FLOOR_0]         = "keys.floor.0",
    [TR_KEYS_FLOOR_1]         = "keys.floor.1",
    [TR_QUICK_HEAD]           = "quick.head",
    [TR_QUICK_SUB]            = "quick.sub",
    [TR_REMOTE_HEAD]          = "remote.head",
    [TR_PROJECT_HINT]         = "project.hint",
    [TR_PROJECT_DO_SAFE]      = "project.do.safe",
    [TR_PROJECT_DO_GOD]       = "project.do.god",
    [TR_STATUS_PASTE]         = "status.paste",
    [TR_STATUS_NO_PASTE]      = "status.nopaste",
    [TR_CHIME_IDLE]           = "chime.idle",
    [TR_CHIME_WORKING]        = "chime.working",
    [TR_CHIME_APPROVAL]       = "chime.approval",
    [TR_CHIME_QUESTION]       = "chime.question",
    [TR_CHIME_LANTERN]        = "chime.lantern",
    [TR_CHIME_SENT]           = "chime.sent",
    [TR_CHIME_ANSWERING]      = "chime.answering",
    [TR_CHIME_DONE]           = "chime.done",
    [TR_CHIME_ANSWERED]       = "chime.answered",
    [TR_SETTINGS_TAB_SESSION]   = "settings.tab.session",
    [TR_SETTINGS_TAB_SOUND]     = "settings.tab.sound",
    [TR_SETTINGS_TAB_SCREEN]    = "settings.tab.screen",
    [TR_SETTINGS_TAB_LINK]      = "settings.tab.link",
    [TR_SETTINGS_TAB_BUTTONS]   = "settings.tab.buttons",
    [TR_SETTINGS_ROW_MODEL]     = "settings.row.model",
    [TR_SETTINGS_ROW_SESSION]   = "settings.row.session",
    [TR_SETTINGS_ROW_GOD_MODE]  = "settings.row.god_mode",
    [TR_SETTINGS_ROW_SOUND]     = "settings.row.sound",
    [TR_SETTINGS_ROW_VOLUME]    = "settings.row.volume",
    [TR_SETTINGS_ROW_TYPING]    = "settings.row.typing",
    [TR_SETTINGS_ROW_BLIPS]     = "settings.row.blips",
    [TR_SETTINGS_ROW_THEME]     = "settings.row.theme",
    [TR_SETTINGS_ROW_LANGUAGE]  = "settings.row.language",
    [TR_SETTINGS_ROW_BOOT]      = "settings.row.boot",
    [TR_SETTINGS_ROW_ADDRESSES] = "settings.row.addresses",
    [TR_SETTINGS_ROW_AUTOCONNECT] = "settings.row.autoconnect",
    [TR_SETTINGS_ROW_VIEW_TAPS] = "settings.row.view_taps",
    [TR_SETTINGS_ROW_PICTURE]   = "settings.row.picture",
    [TR_SETTINGS_ROW_LIVEVIEW]  = "settings.row.liveview",
    [TR_SETTINGS_ROW_MIC]       = "settings.row.mic",
    [TR_SETTINGS_ROW_SHUTTER]   = "settings.row.shutter",
    [TR_SETTINGS_ROW_DEFAULTS]  = "settings.row.defaults",
    [TR_QUICK_ROW_MODE]         = "quick.row.mode",
    [TR_QUICK_ROW_MODE_NOTE]    = "quick.row.mode.note",
    [TR_QUICK_ROW_UNDO]         = "quick.row.undo",
    [TR_QUICK_ROW_UNDO_NOTE]    = "quick.row.undo.note",
    [TR_QUICK_ROW_CHANGES]      = "quick.row.changes",
    [TR_QUICK_ROW_CHANGES_NOTE] = "quick.row.changes.note",
    [TR_QUICK_ROW_COMMANDS]     = "quick.row.commands",
    [TR_QUICK_ROW_COMMANDS_NOTE]= "quick.row.commands.note",
    [TR_QUICK_ROW_AGAIN]        = "quick.row.again",
    [TR_QUICK_ROW_AGAIN_NOTE]   = "quick.row.again.note",
    [TR_QUICK_ROW_VOICE]        = "quick.row.voice",
    [TR_QUICK_ROW_VOICE_NOTE]   = "quick.row.voice.note",
    [TR_QUICK_ROW_PHOTO]        = "quick.row.photo",
    [TR_QUICK_ROW_PHOTO_NOTE]   = "quick.row.photo.note",
    [TR_QUICK_FOOT] = "quick.foot",
    [TR_QUICK_ADD] = "quick.add",
    [TR_QUICK_CMD_HEAD] = "quick.commands.head",
    [TR_QUICK_CMD_HEAD_SAVED] = "quick.commands.head.saved",
    [TR_QUICK_CMD_HEAD_SURE] = "quick.commands.head.sure",
    [TR_QUICK_CMD_HEAD_ADD] = "quick.commands.head.add",
    [TR_QUICK_CMD_SUB] = "quick.commands.sub",
    [TR_QUICK_AGAIN_HEAD] = "quick.again.head",
    [TR_QUICK_AGAIN_SUB] = "quick.again.sub",
    [TR_QUICK_AGAIN_EMPTY_0] = "quick.again.empty.0",
    [TR_QUICK_AGAIN_EMPTY_1] = "quick.again.empty.1",
    [TR_CHIME_YES] = "chime.yes",
    [TR_CHIME_NO] = "chime.no",
    [TR_CONTROL_HINT_CONFIRM] = "control.hint.confirm",
    [TR_CONTROL_HINT_UNDO] = "control.hint.undo",
    [TR_CONTROL_QUICK_MODE] = "control.quick.mode",
    [TR_BUTTON_ENTER] = "button.enter",
    [TR_BUTTON_SPACE] = "button.space",
    [TR_BUTTON_DELETE] = "button.delete",
    [TR_BUTTON_ESC] = "button.esc",
    [TR_BUTTON_TAB] = "button.tab",
    [TR_BUTTON_STOP] = "button.stop",
    [TR_LEGEND_MODE] = "legend.mode",
    [TR_LEGEND_SCREENS] = "legend.screens",
    [TR_LEGEND_SHOULDERS] = "legend.shoulders",
    [TR_GUIDE_TAB_CONTROL] = "guide.tab.control",
    [TR_GUIDE_TAB_KEYS] = "guide.tab.keys",
    [TR_GUIDE_TAB_COMPOSE] = "guide.tab.compose",
    [TR_GUIDE_TAB_FOCUS] = "guide.tab.focus",
    [TR_GUIDE_TAB_CAMERA] = "guide.tab.camera",
    [TR_GUIDE_TAB_QUICK] = "guide.tab.quick",
    [TR_GUIDE_DONE] = "guide.done",
    [TR_GUIDE_PRESS] = "guide.press",
    [TR_GUIDE_TABS_HINT] = "guide.tabs.hint",
    [TR_GUIDE_FOOT] = "guide.foot",
    [TR_GUIDE_TITLE] = "guide.title",
    [TR_GUIDE_INTRO] = "guide.intro",
    [TR_GUIDE_COMBO_L_THEN_R] = "guide.combo.lr",
    [TR_GUIDE_COMBO_R_THEN_L] = "guide.combo.rl",
    [TR_GUIDE_COMBO_HOLD_R] = "guide.combo.holdr",
    [TR_GUIDE_COMBO_R_THEN] = "guide.combo.rthen",
    [TR_GUIDE_COMBO_DPAD] = "guide.combo.dpad",
    [TR_GUIDE_NAME_SCREENS] = "guide.name.screens",
    [TR_GUIDE_NAME_SCROLL] = "guide.name.scroll",
    [TR_GUIDE_NAME_ARROWS] = "guide.name.arrows",
    [TR_GUIDE_NAME_CTRL] = "guide.name.ctrl",
    [TR_GUIDE_NAME_SHIFT] = "guide.name.shift",
    [TR_GUIDE_NAME_MODE] = "guide.name.mode",
    [TR_GUIDE_NAME_FOCUS] = "guide.name.focus",
    [TR_GUIDE_NAME_BACK] = "guide.name.back",
    [TR_GUIDE_NAME_SEND] = "guide.name.send",
    [TR_GUIDE_NAME_DELETE] = "guide.name.delete",
    [TR_GUIDE_NAME_CANCEL] = "guide.name.cancel",
    [TR_GUIDE_NAME_SNIPPETS] = "guide.name.snippets",
    [TR_GUIDE_NAME_NOTHING] = "guide.name.nothing",
    [TR_GUIDE_NAME_TALK] = "guide.name.talk",
    [TR_GUIDE_NAME_LATCH] = "guide.name.latch",
    [TR_GUIDE_NAME_ORDER] = "guide.name.order",
    [TR_GUIDE_NAME_TYPE] = "guide.name.type",
    [TR_GUIDE_NAME_MOVE] = "guide.name.move",
    [TR_GUIDE_NAME_SHUTTER] = "guide.name.shutter",
    [TR_GUIDE_NAME_FLIP] = "guide.name.flip",
    [TR_GUIDE_NAME_OPEN] = "guide.name.open",
    [TR_GUIDE_NAME_FORGET] = "guide.name.forget",
    [TR_GUIDE_NAME_CLOSE] = "guide.name.close",
    [TR_GUIDE_DO_ENTER] = "guide.do.enter",
    [TR_GUIDE_DO_SPACE] = "guide.do.space",
    [TR_GUIDE_DO_DELETE] = "guide.do.delete",
    [TR_GUIDE_DO_ESC] = "guide.do.esc",
    [TR_GUIDE_DO_TAB] = "guide.do.tab",
    [TR_GUIDE_DO_STOP] = "guide.do.stop",
    [TR_GUIDE_DO_X_BACK] = "guide.do.xback",
    [TR_GUIDE_DO_SELECT] = "guide.do.select",
    [TR_GUIDE_DO_SCROLL] = "guide.do.scroll",
    [TR_GUIDE_DO_ARROWS] = "guide.do.arrows",
    [TR_GUIDE_DO_CTRL] = "guide.do.ctrl",
    [TR_GUIDE_DO_SHIFT] = "guide.do.shift",
    [TR_GUIDE_DO_SHIFT_TAB] = "guide.do.shifttab",
    [TR_GUIDE_DO_FOCUS_TILES] = "guide.do.focustiles",
    [TR_GUIDE_DO_FOCUS_KEYS] = "guide.do.focuskeys",
    [TR_GUIDE_DO_SEND] = "guide.do.send",
    [TR_GUIDE_DO_DEL] = "guide.do.del",
    [TR_GUIDE_DO_CLOSE] = "guide.do.close",
    [TR_GUIDE_DO_MIC_CANCEL] = "guide.do.miccancel",
    [TR_GUIDE_DO_SNIPPETS] = "guide.do.snippets",
    [TR_GUIDE_DO_NOTHING] = "guide.do.nothing",
    [TR_GUIDE_DO_TALK] = "guide.do.talk",
    [TR_GUIDE_DO_TALK_OFF] = "guide.do.talkoff",
    [TR_GUIDE_DO_LATCH] = "guide.do.latch",
    [TR_GUIDE_DO_HOLD_CANCEL] = "guide.do.holdcancel",
    [TR_GUIDE_DO_ORDER] = "guide.do.order",
    [TR_GUIDE_DO_FOCUS] = "guide.do.focus",
    [TR_GUIDE_DO_TYPE] = "guide.do.type",
    [TR_GUIDE_DO_FOCUS_SEND] = "guide.do.focussend",
    [TR_GUIDE_DO_FOCUS_OUT] = "guide.do.focusout",
    [TR_GUIDE_DO_MOVE_KEYS] = "guide.do.movekeys",
    [TR_GUIDE_DO_SHUTTER] = "guide.do.shutter",
    [TR_GUIDE_DO_CAM_SEND] = "guide.do.camsend",
    [TR_GUIDE_DO_CAM_LEAVE] = "guide.do.camleave",
    [TR_GUIDE_DO_FLIP] = "guide.do.flip",
    [TR_GUIDE_DO_CAM_B] = "guide.do.camb",
    [TR_GUIDE_DO_CAM_X] = "guide.do.camx",
    [TR_GUIDE_DO_CAM_SELECT] = "guide.do.camselect",
    [TR_GUIDE_DO_MOVE] = "guide.do.move",
    [TR_GUIDE_DO_OPEN] = "guide.do.open",
    [TR_GUIDE_DO_BACK] = "guide.do.back",
    [TR_GUIDE_DO_FORGET] = "guide.do.forget",
    [TR_GUIDE_DO_CLOSE_MENU] = "guide.do.closemenu",
    [TR_COMPOSE_HINT_EDIT] = "compose.hint.edit",
    [TR_COMPOSE_HINT_FOCUS] = "compose.hint.focus",
    [TR_COMPOSE_HINT_TALK] = "compose.hint.talk",
    [TR_LEGEND_GUIDE] = "legend.guide",
    [TR_QUICK_ROW_CONTROLS] = "quick.row.controls",
    [TR_QUICK_ROW_CONTROLS_NOTE] = "quick.row.controls.note",
    [TR_SETTINGS_ROW_CONTROLS] = "settings.row.controls",
    [TR_SETTINGS_RESET_SURE] = "settings.defaults.sure",
    [TR_COMPOSE_METER_LATCHED] = "compose.meter.latched",
    [TR_COMPOSE_METER_LATCH] = "compose.meter.latch",
    [TR_COMPOSE_METER_HOLD] = "compose.meter.hold",
    [TR_COMPOSE_ADD_HEAD] = "compose.add.head",
    [TR_COMPOSE_ADD_KEYS] = "compose.add.keys",
    [TR_COMPOSE_ADD_FOCUS_KEYS] = "compose.add.focus.keys",
    [TR_QUICK_NOTE_NOT_HERE] = "quick.note.not_here",
    [TR_QUICK_NOTE_NO_STT] = "quick.note.no_stt",
    [TR_QUICK_NOTE_NO_SAVE] = "quick.note.no_save",
};

static const struct {
    const char *code;
    const char *label;
    const char *name;
    const char *strings[TR_COUNT];
} BUILTIN[LANG_BUILTIN_COUNT] = {
    [LANG_EN] = {
        "en", "EN", "English",
        {
            [TR_CONTROL_KEYS]         = "KEYS",
            [TR_CONTROL_KEYS_SUB]     = "type live",
            [TR_CONTROL_QUICK]        = "QUICK",
            [TR_CONTROL_QUICK_SUB]    = "mode, undo",
            [TR_CONTROL_PHOTO]        = "PHOTO",
            [TR_CONTROL_PHOTO_SUB]    = "show Claude",
            [TR_CONTROL_AGAIN]        = "AGAIN",
            [TR_CONTROL_AGAIN_SUB]    = "your prompts",
            [TR_CONTROL_COMPOSE]      = "COMPOSE",
            [TR_CONTROL_COMPOSE_SUB]  = "write first",
            [TR_CONTROL_REMOTE]       = "REMOTE",
            [TR_CONTROL_REMOTE_SUB]   = "elsewhere",
            [TR_CONTROL_SETTINGS]     = "SETTINGS",
            [TR_CONTROL_SETTINGS_SUB] = "sound, model",
            [TR_CONTROL_RELINK]       = "RELINK",
            [TR_CONTROL_HINT_SCROLL]  = "up/down scroll  hold down: now",
            [TR_CONTROL_HINT_FOCUS]   = "d-pad moves  A opens  B out",
            [TR_COMPOSE_HEAD]         = "COMPOSE: nothing is sent until you press A",
            [TR_COMPOSE_KEYS]         = "A sends   B deletes   X cancels",
            [TR_COMPOSE_FOCUS_HEAD]   = "FOCUS: d-pad picks a key, A types it",
            [TR_COMPOSE_FOCUS_KEYS]   = "START sends   B deletes   X leaves focus",
            [TR_COMPOSE_MIC_IDLE]     = "R / TAP: hold to speak (voice to text)",
            [TR_COMPOSE_MIC_REC]      = "LISTENING... release R to transcribe",
            [TR_COMPOSE_MIC_LATCH]    = "RECORDING -- tap R or L to stop",
            [TR_COMPOSE_MIC_WAIT]     = "Transcribing voice on server...",
            [TR_KEYS_FLOOR_0]         = "Every key goes straight to Claude.",
            [TR_KEYS_FLOOR_1]         = "SHFT: abc ABC !@#   X: back",
            [TR_QUICK_HEAD]           = "QUICK   UP/DOWN choose   A do   X back",
            [TR_QUICK_SUB]            = "COMMANDS and AGAIN open the composer",
            [TR_REMOTE_HEAD]          = "REMOTE   UP/DOWN choose   A open   X back",
            [TR_PROJECT_HINT]         = "tap a tab   X BACK",
            [TR_PROJECT_DO_SAFE]      = "a tap loads it; A sends",
            [TR_PROJECT_DO_GOD]       = "GOD MODE: a tap runs it immediately",
            [TR_STATUS_PASTE]         = "LOAD INTO COMPOSER",
            [TR_STATUS_NO_PASTE]      = "NOTHING TO PASTE",
            // The chime headline's lines, byte for byte what it showed
            // before it could be translated (chime.c, chime_headline).
            [TR_CHIME_IDLE]           = "Ready",
            [TR_CHIME_WORKING]        = "Claude is working",
            [TR_CHIME_APPROVAL]       = "Claude needs your OK",
            [TR_CHIME_QUESTION]       = "Claude asked you something",
            [TR_CHIME_LANTERN]        = "Another session has new output",
            [TR_CHIME_SENT]           = "Sent -- waiting for Claude",
            [TR_CHIME_ANSWERING]      = "Claude is answering",
            [TR_CHIME_DONE]           = "Done -- your turn",
            [TR_CHIME_ANSWERED]       = "Answer ready",
            [TR_SETTINGS_TAB_SESSION]   = "SESSION",
            [TR_SETTINGS_TAB_SOUND]     = "SOUND",
            [TR_SETTINGS_TAB_SCREEN]    = "SCREEN",
            [TR_SETTINGS_TAB_LINK]      = "LINK",
            [TR_SETTINGS_TAB_BUTTONS]   = "BUTTONS",
            [TR_SETTINGS_ROW_MODEL]     = "MODEL",
            [TR_SETTINGS_ROW_SESSION]   = "SESSION",
            [TR_SETTINGS_ROW_GOD_MODE]  = "GOD MODE",
            [TR_SETTINGS_ROW_SOUND]     = "SOUND",
            [TR_SETTINGS_ROW_VOLUME]    = "VOLUME",
            [TR_SETTINGS_ROW_TYPING]    = "TYPING",
            [TR_SETTINGS_ROW_BLIPS]     = "WRITING",
            [TR_SETTINGS_ROW_THEME]     = "THEME",
            [TR_SETTINGS_ROW_LANGUAGE]  = "LANGUAGE",
            [TR_SETTINGS_ROW_BOOT]      = "ON BOOT",
            [TR_SETTINGS_ROW_ADDRESSES] = "ADDRESSES",
            [TR_SETTINGS_ROW_AUTOCONNECT] = "AUTOCONNECT",
            [TR_SETTINGS_ROW_VIEW_TAPS] = "VIEW TAPS",
            [TR_SETTINGS_ROW_PICTURE]   = "PICTURE",
            [TR_SETTINGS_ROW_LIVEVIEW]  = "LIVE VIEW",
            [TR_SETTINGS_ROW_MIC]       = "MIC",
            [TR_SETTINGS_ROW_SHUTTER]   = "SHUTTER",
            [TR_SETTINGS_ROW_DEFAULTS]  = "DEFAULTS",
            [TR_QUICK_ROW_MODE]         = "MODE",
            [TR_QUICK_ROW_MODE_NOTE]    = "A: Claude's next mode",
            [TR_QUICK_ROW_UNDO]         = "UNDO",
            [TR_QUICK_ROW_UNDO_NOTE]    = "rewind to an earlier point",
            [TR_QUICK_ROW_CHANGES]      = "CHANGES",
            [TR_QUICK_ROW_CHANGES_NOTE] = "what Claude changed",
            [TR_QUICK_ROW_COMMANDS]     = "COMMANDS",
            [TR_QUICK_ROW_COMMANDS_NOTE]= "slash commands",
            [TR_QUICK_ROW_AGAIN]        = "AGAIN",
            [TR_QUICK_ROW_AGAIN_NOTE]   = "your recent prompts",
            [TR_QUICK_ROW_VOICE]        = "VOICE",
            [TR_QUICK_ROW_VOICE_NOTE]   = "speak into the composer",
            [TR_QUICK_ROW_PHOTO]        = "PHOTO",
            [TR_QUICK_ROW_PHOTO_NOTE]   = "see it, send it",
            [TR_QUICK_FOOT] = "the tile shows Claude's mode",
            [TR_QUICK_ADD] = "+ ADD",
            [TR_QUICK_CMD_HEAD] = "COMMANDS   A compose   X back",
            [TR_QUICK_CMD_HEAD_SAVED] = "COMMANDS   A compose   Y forget   X back",
            [TR_QUICK_CMD_HEAD_SURE] = "COMMANDS   A compose   Y SURE?   X back",
            [TR_QUICK_CMD_HEAD_ADD] = "COMMANDS   A add command   X back",
            [TR_QUICK_CMD_SUB] = "built in, saved (+ ADD), then computer's",
            [TR_QUICK_AGAIN_HEAD] = "AGAIN   A compose   Y forget   X back",
            [TR_QUICK_AGAIN_SUB] = "what you sent lately, newest first",
            [TR_QUICK_AGAIN_EMPTY_0] = "Nothing yet: prompts you send",
            [TR_QUICK_AGAIN_EMPTY_1] = "from here land in this list.",
            [TR_CHIME_YES] = "yes",
            [TR_CHIME_NO] = "no",
            [TR_CONTROL_HINT_CONFIRM] = "d-pad picks  %s confirms",
            [TR_CONTROL_HINT_UNDO] = "UNDO: d-pad picks  %s chooses",
            [TR_CONTROL_QUICK_MODE] = "mode: %s",
            [TR_BUTTON_ENTER] = "enter",
            [TR_BUTTON_SPACE] = "space",
            [TR_BUTTON_DELETE] = "delete",
            [TR_BUTTON_ESC] = "esc",
            [TR_BUTTON_TAB] = "tab",
            [TR_BUTTON_STOP] = "stop",
            [TR_LEGEND_MODE] = "mode",
            [TR_LEGEND_SCREENS] = "screens",
            [TR_LEGEND_SHOULDERS] = "L ctrl  R shift  L+R pick a tile",
            [TR_GUIDE_TAB_CONTROL] = "CONTROL",
            [TR_GUIDE_TAB_KEYS] = "KEYS",
            [TR_GUIDE_TAB_COMPOSE] = "COMPOSE",
            [TR_GUIDE_TAB_FOCUS] = "FOCUS",
            [TR_GUIDE_TAB_CAMERA] = "CAMERA",
            [TR_GUIDE_TAB_QUICK] = "QUICK",
            [TR_GUIDE_DONE] = "DONE",
            [TR_GUIDE_PRESS] = "press a button",
            [TR_GUIDE_TABS_HINT] = "LEFT/RIGHT tabs",
            [TR_GUIDE_FOOT] = "leave: hold B, DONE, or SELECT+START",
            [TR_GUIDE_TITLE] = "CONTROLS",
            [TR_GUIDE_INTRO] = "press any button or combo; nothing is sent from here",
            [TR_GUIDE_COMBO_L_THEN_R] = "L then R",
            [TR_GUIDE_COMBO_R_THEN_L] = "R then L",
            [TR_GUIDE_COMBO_HOLD_R] = "hold R",
            [TR_GUIDE_COMBO_R_THEN] = "R, then %s",
            [TR_GUIDE_COMBO_DPAD] = "d-pad",
            [TR_GUIDE_NAME_SCREENS] = "SCREENS",
            [TR_GUIDE_NAME_SCROLL] = "SCROLL",
            [TR_GUIDE_NAME_ARROWS] = "ARROWS",
            [TR_GUIDE_NAME_CTRL] = "CTRL",
            [TR_GUIDE_NAME_SHIFT] = "SHIFT",
            [TR_GUIDE_NAME_MODE] = "MODE",
            [TR_GUIDE_NAME_FOCUS] = "FOCUS",
            [TR_GUIDE_NAME_BACK] = "BACK",
            [TR_GUIDE_NAME_SEND] = "SEND",
            [TR_GUIDE_NAME_DELETE] = "DELETE",
            [TR_GUIDE_NAME_CANCEL] = "CANCEL",
            [TR_GUIDE_NAME_SNIPPETS] = "SNIPPETS",
            [TR_GUIDE_NAME_NOTHING] = "NOT USED",
            [TR_GUIDE_NAME_TALK] = "TALK",
            [TR_GUIDE_NAME_LATCH] = "LATCH",
            [TR_GUIDE_NAME_ORDER] = "NOT FOCUS",
            [TR_GUIDE_NAME_TYPE] = "TYPE",
            [TR_GUIDE_NAME_MOVE] = "MOVE",
            [TR_GUIDE_NAME_SHUTTER] = "SHUTTER",
            [TR_GUIDE_NAME_FLIP] = "FLIP",
            [TR_GUIDE_NAME_OPEN] = "OPEN",
            [TR_GUIDE_NAME_FORGET] = "FORGET",
            [TR_GUIDE_NAME_CLOSE] = "CLOSE",
            [TR_GUIDE_DO_ENTER] = "sends Enter: submits, or accepts the lit choice",
            [TR_GUIDE_DO_SPACE] = "types a space",
            [TR_GUIDE_DO_DELETE] = "sends Backspace: deletes before the cursor",
            [TR_GUIDE_DO_ESC] = "sends Esc: says no to a prompt, or interrupts",
            [TR_GUIDE_DO_TAB] = "sends Tab; after R it is Shift-Tab (next mode)",
            [TR_GUIDE_DO_STOP] = "sends Ctrl-C: stops what Claude is doing",
            [TR_GUIDE_DO_X_BACK] = "back to the Control screen; nothing is sent",
            [TR_GUIDE_DO_SELECT] = "next screen: CONTROL, KEYS, STATUS, PROJECT",
            [TR_GUIDE_DO_SCROLL] = "scrolls back (hold DOWN: live); picks when asked",
            [TR_GUIDE_DO_ARROWS] = "sends the arrow keys; they repeat while held",
            [TR_GUIDE_DO_CTRL] = "arms Ctrl: the next key is sent as Ctrl+key",
            [TR_GUIDE_DO_SHIFT] = "arms Shift once: Tab is Shift-Tab, UP/DOWN scroll",
            [TR_GUIDE_DO_SHIFT_TAB] = "Shift-Tab: Claude Code moves to its next mode",
            [TR_GUIDE_DO_FOCUS_TILES] = "focus: the d-pad walks the tiles, A opens one",
            [TR_GUIDE_DO_FOCUS_KEYS] = "focus: the d-pad walks the keys, A types, R enters",
            [TR_GUIDE_DO_SEND] = "sends what you wrote (empty: closes)",
            [TR_GUIDE_DO_DEL] = "deletes the last character of the draft",
            [TR_GUIDE_DO_CLOSE] = "closes the composer; the draft is dropped",
            [TR_GUIDE_DO_MIC_CANCEL] = "cancels the recording; the draft stays",
            [TR_GUIDE_DO_SNIPPETS] = "snippets, and the clipboard if it is on",
            [TR_GUIDE_DO_NOTHING] = "does nothing here",
            [TR_GUIDE_DO_TALK] = "hold to record; let go to write it into the draft",
            [TR_GUIDE_DO_TALK_OFF] = "talking needs speech-to-text on the computer",
            [TR_GUIDE_DO_LATCH] = "the mic stays on; tap R or L to stop",
            [TR_GUIDE_DO_HOLD_CANCEL] = "cancels the recording (MIC is HOLD)",
            [TR_GUIDE_DO_ORDER] = "not focus: hold L first, then press R",
            [TR_GUIDE_DO_FOCUS] = "the d-pad picks a key; A types it; START sends",
            [TR_GUIDE_DO_TYPE] = "types the lit key into the draft",
            [TR_GUIDE_DO_FOCUS_SEND] = "sends what you wrote",
            [TR_GUIDE_DO_FOCUS_OUT] = "leaves focus; the draft stays",
            [TR_GUIDE_DO_MOVE_KEYS] = "moves over the keys; repeats while held",
            [TR_GUIDE_DO_SHUTTER] = "takes the picture as you let go",
            [TR_GUIDE_DO_CAM_SEND] = "in review: sends the picture to the computer",
            [TR_GUIDE_DO_CAM_LEAVE] = "leaves the camera, back to QUICK",
            [TR_GUIDE_DO_FLIP] = "switches between the two cameras",
            [TR_GUIDE_DO_CAM_B] = "back to QUICK; in review, takes it again",
            [TR_GUIDE_DO_CAM_X] = "leaves the camera for the Control screen",
            [TR_GUIDE_DO_CAM_SELECT] = "leaves the camera for the next screen",
            [TR_GUIDE_DO_MOVE] = "moves the highlight",
            [TR_GUIDE_DO_OPEN] = "opens the row, or loads it into the composer",
            [TR_GUIDE_DO_BACK] = "back one level, then out",
            [TR_GUIDE_DO_FORGET] = "forgets a saved command or prompt (press twice)",
            [TR_GUIDE_DO_CLOSE_MENU] = "closes the menu",
            [TR_COMPOSE_HINT_EDIT] = "ENT: new line   Y: snippets, clipboard",
            [TR_COMPOSE_HINT_FOCUS] = "L then R: focus",
            [TR_COMPOSE_HINT_TALK] = "hold R: talk",
            [TR_LEGEND_GUIDE] = "hold SELECT, then START: controls",
            [TR_QUICK_ROW_CONTROLS] = "CONTROLS",
            [TR_QUICK_ROW_CONTROLS_NOTE] = "every button and combo",
            [TR_SETTINGS_ROW_CONTROLS] = "CONTROLS",
            [TR_SETTINGS_RESET_SURE] = "SURE?",
            [TR_COMPOSE_METER_LATCHED] = "MIC [%s]  R/L stop  X cancel",
            [TR_COMPOSE_METER_LATCH] = "MIC [%s]  L latch  X cancel",
            [TR_COMPOSE_METER_HOLD] = "MIC [%s]  X cancel",
            [TR_COMPOSE_ADD_HEAD] = "ADD QUICK COMMAND: A saves on card",
            [TR_COMPOSE_ADD_KEYS] = "A saves   B deletes   X cancels",
            [TR_COMPOSE_ADD_FOCUS_KEYS] = "START saves   B deletes   X leaves focus",
            [TR_QUICK_NOTE_NOT_HERE] = "not on this computer",
            [TR_QUICK_NOTE_NO_STT] = "no speech-to-text on server",
            [TR_QUICK_NOTE_NO_SAVE] = "cannot save pictures here",
        },
    },
    [LANG_ES] = {
        "es", "ES", "Español",
        {
            [TR_CONTROL_KEYS]         = "TECLAS",
            [TR_CONTROL_KEYS_SUB]     = "en vivo",
            [TR_CONTROL_QUICK]        = "RÁPIDO",
            [TR_CONTROL_QUICK_SUB]    = "modo, atrás",
            [TR_CONTROL_PHOTO]        = "FOTO",
            [TR_CONTROL_PHOTO_SUB]    = "mostrar",
            [TR_CONTROL_AGAIN]        = "REPETIR",
            [TR_CONTROL_AGAIN_SUB]    = "recientes",
            [TR_CONTROL_COMPOSE]      = "REDACTAR",
            [TR_CONTROL_COMPOSE_SUB]  = "editar antes",
            [TR_CONTROL_REMOTE]       = "REMOTO",
            [TR_CONTROL_REMOTE_SUB]   = "sesiones",
            [TR_CONTROL_SETTINGS]     = "AJUSTES",
            [TR_CONTROL_SETTINGS_SUB] = "sonido, tema",
            [TR_CONTROL_RELINK]       = "CONECTAR",
            [TR_CONTROL_HINT_SCROLL]  = "arr/abj mueve  mantén: ahora",
            [TR_CONTROL_HINT_FOCUS]   = "cruceta mueve  A abre  B salir",
            [TR_COMPOSE_HEAD]         = "REDACTAR: nada se envía hasta pulsar A",
            [TR_COMPOSE_KEYS]         = "A envía   B borra   X cancela",
            [TR_COMPOSE_FOCUS_HEAD]   = "FOCO: la cruceta elige, A escribe",
            [TR_COMPOSE_FOCUS_KEYS]   = "START envía   B borra   X sale del foco",
            [TR_COMPOSE_MIC_IDLE]     = "R / TOCA: mantén para hablar (voz)",
            [TR_COMPOSE_MIC_REC]      = "ESCUCHANDO... suelta R para transcribir",
            [TR_COMPOSE_MIC_LATCH]    = "GRABANDO -- toca R o L para parar",
            [TR_COMPOSE_MIC_WAIT]     = "Transcribiendo voz en el servidor...",
            [TR_KEYS_FLOOR_0]         = "Cada tecla va directo a Claude.",
            [TR_KEYS_FLOOR_1]         = "SHFT: abc ABC !@#   X: volver",
            [TR_QUICK_HEAD]           = "RÁPIDO  ARR/ABJ elegir  A hacer  X salir",
            [TR_QUICK_SUB]            = "COMANDOS y REPETIR abren el editor",
            [TR_REMOTE_HEAD]          = "REMOTO  ARR/ABJ elegir  A abrir  X salir",
            [TR_PROJECT_HINT]         = "toca pestaña   X VOLVER",
            [TR_PROJECT_DO_SAFE]      = "tocar lo carga; A lo envía",
            [TR_PROJECT_DO_GOD]       = "MODO DIOS: tocar ejecuta al instante",
            [TR_STATUS_PASTE]         = "CARGAR EN EDITOR",
            [TR_STATUS_NO_PASTE]      = "NADA QUE PEGAR",
            [TR_CHIME_IDLE]           = "Listo",
            [TR_CHIME_WORKING]        = "Claude está trabajando",
            [TR_CHIME_APPROVAL]       = "Claude necesita tu OK",
            [TR_CHIME_QUESTION]       = "Claude te hizo una pregunta",
            [TR_CHIME_LANTERN]        = "Otra sesión tiene salida nueva",
            [TR_CHIME_SENT]           = "Enviado -- esperando a Claude",
            [TR_CHIME_ANSWERING]      = "Claude está respondiendo",
            [TR_CHIME_DONE]           = "Hecho -- tu turno",
            [TR_CHIME_ANSWERED]       = "Respuesta lista",
            [TR_SETTINGS_TAB_SESSION]   = "SESIÓN",
            [TR_SETTINGS_TAB_SOUND]     = "SONIDO",
            [TR_SETTINGS_TAB_SCREEN]    = "PANT.",
            [TR_SETTINGS_TAB_LINK]      = "RED",
            [TR_SETTINGS_TAB_BUTTONS]   = "BOTONES",
            [TR_SETTINGS_ROW_MODEL]     = "MODELO",
            [TR_SETTINGS_ROW_SESSION]   = "SESIÓN",
            [TR_SETTINGS_ROW_GOD_MODE]  = "MODO DIOS",
            [TR_SETTINGS_ROW_SOUND]     = "SONIDO",
            [TR_SETTINGS_ROW_VOLUME]    = "VOLUMEN",
            [TR_SETTINGS_ROW_TYPING]    = "TECLEO",
            [TR_SETTINGS_ROW_BLIPS]     = "ESCRITURA",
            [TR_SETTINGS_ROW_THEME]     = "TEMA",
            [TR_SETTINGS_ROW_LANGUAGE]  = "IDIOMA",
            [TR_SETTINGS_ROW_BOOT]      = "INICIO",
            [TR_SETTINGS_ROW_ADDRESSES] = "IP OCULTA",
            [TR_SETTINGS_ROW_AUTOCONNECT] = "AUTOCONEX.",
            [TR_SETTINGS_ROW_VIEW_TAPS] = "TOQUES PC",
            [TR_SETTINGS_ROW_PICTURE]   = "FOTO",
            [TR_SETTINGS_ROW_LIVEVIEW]  = "VISTA",
            [TR_SETTINGS_ROW_MIC]       = "MIC",
            [TR_SETTINGS_ROW_SHUTTER]   = "DISPARO",
            [TR_SETTINGS_ROW_DEFAULTS]  = "REINICIAR",
            [TR_QUICK_ROW_MODE]         = "MODO",
            [TR_QUICK_ROW_MODE_NOTE]    = "A: siguiente modo",
            [TR_QUICK_ROW_UNDO]         = "DESHACER",
            [TR_QUICK_ROW_UNDO_NOTE]    = "volver a un punto anterior",
            [TR_QUICK_ROW_CHANGES]      = "CAMBIOS",
            [TR_QUICK_ROW_CHANGES_NOTE] = "lo que Claude cambió",
            [TR_QUICK_ROW_COMMANDS]     = "COMANDOS",
            [TR_QUICK_ROW_COMMANDS_NOTE]= "comandos con barra",
            [TR_QUICK_ROW_AGAIN]        = "REPETIR",
            [TR_QUICK_ROW_AGAIN_NOTE]   = "tus envíos recientes",
            [TR_QUICK_ROW_VOICE]        = "VOZ",
            [TR_QUICK_ROW_VOICE_NOTE]   = "dictar en el editor",
            [TR_QUICK_ROW_PHOTO]        = "FOTO",
            [TR_QUICK_ROW_PHOTO_NOTE]   = "ver y enviar foto",
            [TR_QUICK_FOOT] = "el botón RÁPIDO muestra el modo de Claude",
            [TR_QUICK_ADD] = "+ AÑADIR",
            [TR_QUICK_CMD_HEAD] = "COMANDOS   A editar   X volver",
            [TR_QUICK_CMD_HEAD_SAVED] = "COMANDOS  A editar  Y olvidar  X volver",
            [TR_QUICK_CMD_HEAD_SURE] = "COMANDOS  A editar  Y ¿SEGURO?  X volver",
            [TR_QUICK_CMD_HEAD_ADD] = "COMANDOS   A añadir comando   X volver",
            [TR_QUICK_CMD_SUB] = "fijos, guardados (+ AÑADIR), luego del PC",
            [TR_QUICK_AGAIN_HEAD] = "REPETIR  A editar  Y olvidar  X volver",
            [TR_QUICK_AGAIN_SUB] = "lo que enviaste, lo más nuevo arriba",
            [TR_QUICK_AGAIN_EMPTY_0] = "Nada aún: lo que envíes desde",
            [TR_QUICK_AGAIN_EMPTY_1] = "aquí aparecerá en esta lista.",
            [TR_CHIME_YES] = "sí",
            [TR_CHIME_NO] = "no",
            [TR_CONTROL_HINT_CONFIRM] = "cruceta elige  %s confirma",
            [TR_CONTROL_HINT_UNDO] = "DESHACER: cruceta  %s elige",
            [TR_CONTROL_QUICK_MODE] = "modo: %s",
            [TR_BUTTON_ENTER] = "intro",
            [TR_BUTTON_SPACE] = "espacio",
            [TR_BUTTON_DELETE] = "borrar",
            [TR_BUTTON_ESC] = "esc",
            [TR_BUTTON_TAB] = "tab",
            [TR_BUTTON_STOP] = "parar",
            [TR_LEGEND_MODE] = "modo",
            [TR_LEGEND_SCREENS] = "pantallas",
            [TR_LEGEND_SHOULDERS] = "L ctrl  R mayús  L+R elegir botón",
            [TR_GUIDE_TAB_CONTROL] = "CONTROL",
            [TR_GUIDE_TAB_KEYS] = "TECLAS",
            [TR_GUIDE_TAB_COMPOSE] = "REDACTAR",
            [TR_GUIDE_TAB_FOCUS] = "FOCO",
            [TR_GUIDE_TAB_CAMERA] = "CÁMARA",
            [TR_GUIDE_TAB_QUICK] = "RÁPIDO",
            [TR_GUIDE_DONE] = "LISTO",
            [TR_GUIDE_PRESS] = "pulsa un botón",
            [TR_GUIDE_TABS_HINT] = "IZQ/DER pestaña",
            [TR_GUIDE_FOOT] = "salir: B largo, LISTO o SELECT+START",
            [TR_GUIDE_TITLE] = "CONTROLES",
            [TR_GUIDE_INTRO] = "pulsa un botón o combinación; aquí no se envía nada",
            [TR_GUIDE_COMBO_L_THEN_R] = "L y luego R",
            [TR_GUIDE_COMBO_R_THEN_L] = "R y luego L",
            [TR_GUIDE_COMBO_HOLD_R] = "mantén R",
            [TR_GUIDE_COMBO_R_THEN] = "R, luego %s",
            [TR_GUIDE_COMBO_DPAD] = "cruceta",
            [TR_GUIDE_NAME_SCREENS] = "PANTALLAS",
            [TR_GUIDE_NAME_SCROLL] = "DESPLAZAR",
            [TR_GUIDE_NAME_ARROWS] = "FLECHAS",
            [TR_GUIDE_NAME_CTRL] = "CTRL",
            [TR_GUIDE_NAME_SHIFT] = "MAYÚS",
            [TR_GUIDE_NAME_MODE] = "MODO",
            [TR_GUIDE_NAME_FOCUS] = "FOCO",
            [TR_GUIDE_NAME_BACK] = "VOLVER",
            [TR_GUIDE_NAME_SEND] = "ENVIAR",
            [TR_GUIDE_NAME_DELETE] = "BORRAR",
            [TR_GUIDE_NAME_CANCEL] = "CANCELAR",
            [TR_GUIDE_NAME_SNIPPETS] = "TEXTOS",
            [TR_GUIDE_NAME_NOTHING] = "SIN USO",
            [TR_GUIDE_NAME_TALK] = "HABLAR",
            [TR_GUIDE_NAME_LATCH] = "FIJAR",
            [TR_GUIDE_NAME_ORDER] = "NO ES FOCO",
            [TR_GUIDE_NAME_TYPE] = "ESCRIBIR",
            [TR_GUIDE_NAME_MOVE] = "MOVER",
            [TR_GUIDE_NAME_SHUTTER] = "DISPARO",
            [TR_GUIDE_NAME_FLIP] = "GIRAR",
            [TR_GUIDE_NAME_OPEN] = "ABRIR",
            [TR_GUIDE_NAME_FORGET] = "OLVIDAR",
            [TR_GUIDE_NAME_CLOSE] = "CERRAR",
            [TR_GUIDE_DO_ENTER] = "envía Intro: confirma o acepta la opción",
            [TR_GUIDE_DO_SPACE] = "escribe un espacio",
            [TR_GUIDE_DO_DELETE] = "envía Retroceso: borra antes del cursor",
            [TR_GUIDE_DO_ESC] = "envía Esc: rechaza o interrumpe",
            [TR_GUIDE_DO_TAB] = "envía Tab; tras R, Mayús-Tab (siguiente modo)",
            [TR_GUIDE_DO_STOP] = "envía Ctrl-C: detiene a Claude",
            [TR_GUIDE_DO_X_BACK] = "vuelve a la pantalla Control; no envía nada",
            [TR_GUIDE_DO_SELECT] = "siguiente: CONTROL, TECLAS, ESTADO, PROYECTO",
            [TR_GUIDE_DO_SCROLL] = "desplaza (mantén ABAJO: ahora); elige si pregunta",
            [TR_GUIDE_DO_ARROWS] = "envía las flechas; se repiten al mantener",
            [TR_GUIDE_DO_CTRL] = "arma Ctrl: la siguiente tecla va con Ctrl",
            [TR_GUIDE_DO_SHIFT] = "arma Mayús: Tab es Mayús-Tab, ARR/ABJ desplaza",
            [TR_GUIDE_DO_SHIFT_TAB] = "Mayús-Tab: Claude Code pasa al siguiente modo",
            [TR_GUIDE_DO_FOCUS_TILES] = "foco: la cruceta recorre botones, A abre",
            [TR_GUIDE_DO_FOCUS_KEYS] = "foco: la cruceta elige teclas, A escribe, R intro",
            [TR_GUIDE_DO_SEND] = "envía lo escrito (vacío: cierra)",
            [TR_GUIDE_DO_DEL] = "borra el último carácter del borrador",
            [TR_GUIDE_DO_CLOSE] = "cierra el editor y descarta el borrador",
            [TR_GUIDE_DO_MIC_CANCEL] = "cancela la grabación; el borrador queda",
            [TR_GUIDE_DO_SNIPPETS] = "plantillas, y el portapapeles si está activo",
            [TR_GUIDE_DO_NOTHING] = "aquí no hace nada",
            [TR_GUIDE_DO_TALK] = "mantén para grabar; suelta para escribirlo",
            [TR_GUIDE_DO_TALK_OFF] = "hablar requiere voz a texto en el PC",
            [TR_GUIDE_DO_LATCH] = "el micro sigue; toca R o L para parar",
            [TR_GUIDE_DO_HOLD_CANCEL] = "cancela la grabación (MIC en HOLD)",
            [TR_GUIDE_DO_ORDER] = "no es foco: mantén L primero, luego R",
            [TR_GUIDE_DO_FOCUS] = "la cruceta elige; A escribe; START envía",
            [TR_GUIDE_DO_TYPE] = "escribe la tecla marcada en el borrador",
            [TR_GUIDE_DO_FOCUS_SEND] = "envía lo escrito",
            [TR_GUIDE_DO_FOCUS_OUT] = "sale del foco; el borrador queda",
            [TR_GUIDE_DO_MOVE_KEYS] = "recorre las teclas; se repite al mantener",
            [TR_GUIDE_DO_SHUTTER] = "toma la foto al soltar",
            [TR_GUIDE_DO_CAM_SEND] = "en revisión: envía la foto al PC",
            [TR_GUIDE_DO_CAM_LEAVE] = "sale de la cámara, vuelve a RÁPIDO",
            [TR_GUIDE_DO_FLIP] = "cambia entre las dos cámaras",
            [TR_GUIDE_DO_CAM_B] = "vuelve a RÁPIDO; en revisión, repite la foto",
            [TR_GUIDE_DO_CAM_X] = "sale de la cámara a la pantalla Control",
            [TR_GUIDE_DO_CAM_SELECT] = "sale de la cámara a la siguiente pantalla",
            [TR_GUIDE_DO_MOVE] = "mueve la selección",
            [TR_GUIDE_DO_OPEN] = "abre la fila o la carga en el editor",
            [TR_GUIDE_DO_BACK] = "vuelve un nivel, luego sale",
            [TR_GUIDE_DO_FORGET] = "olvida un comando o envío guardado (2x)",
            [TR_GUIDE_DO_CLOSE_MENU] = "cierra el menú",
            [TR_COMPOSE_HINT_EDIT] = "ENT: nueva línea   Y: plantillas",
            [TR_COMPOSE_HINT_FOCUS] = "L y luego R: foco",
            [TR_COMPOSE_HINT_TALK] = "mantén R: hablar",
            [TR_LEGEND_GUIDE] = "SELECT y luego START: controles",
            [TR_QUICK_ROW_CONTROLS] = "CONTROLES",
            [TR_QUICK_ROW_CONTROLS_NOTE] = "cada botón y combinación",
            [TR_SETTINGS_ROW_CONTROLS] = "CONTROLES",
            [TR_SETTINGS_RESET_SURE] = "¿SEGURO?",
            [TR_COMPOSE_METER_LATCHED] = "MIC [%s]  R/L parar  X cancelar",
            [TR_COMPOSE_METER_LATCH] = "MIC [%s]  L fijar  X cancelar",
            [TR_COMPOSE_METER_HOLD] = "MIC [%s]  X cancelar",
            [TR_COMPOSE_ADD_HEAD] = "AÑADIR COMANDO: A lo guarda en la tarjeta",
            [TR_COMPOSE_ADD_KEYS] = "A guarda   B borra   X cancela",
            [TR_COMPOSE_ADD_FOCUS_KEYS] = "START guarda   B borra   X sale del foco",
            [TR_QUICK_NOTE_NOT_HERE] = "no está en este PC",
            [TR_QUICK_NOTE_NO_STT] = "servidor sin voz a texto",
            [TR_QUICK_NOTE_NO_SAVE] = "el PC no guarda fotos",
        },
    },
    [LANG_ZH] = {
        "zh", "ZH", "中文",
        {
            [TR_CONTROL_KEYS]         = "键盘",
            [TR_CONTROL_KEYS_SUB]     = "实时输入",
            [TR_CONTROL_QUICK]        = "快捷",
            [TR_CONTROL_QUICK_SUB]    = "模式与撤回",
            [TR_CONTROL_PHOTO]        = "拍照",
            [TR_CONTROL_PHOTO_SUB]    = "发送照片",
            [TR_CONTROL_AGAIN]        = "重发",
            [TR_CONTROL_AGAIN_SUB]    = "历史输入",
            [TR_CONTROL_COMPOSE]      = "编辑",
            [TR_CONTROL_COMPOSE_SUB]  = "先写后发",
            [TR_CONTROL_REMOTE]       = "远程",
            [TR_CONTROL_REMOTE_SUB]   = "切换会话",
            [TR_CONTROL_SETTINGS]     = "设置",
            [TR_CONTROL_SETTINGS_SUB] = "声音与语言",
            [TR_CONTROL_RELINK]       = "重连",
            [TR_CONTROL_HINT_SCROLL]  = "上下滚动  长按下键回到最新",
            [TR_CONTROL_HINT_FOCUS]   = "方向键移动  A打开  B退出",
            [TR_COMPOSE_HEAD]         = "编辑: 按下 A 键前不会发送",
            [TR_COMPOSE_KEYS]         = "A 发送   B 删除   X 取消",
            [TR_COMPOSE_FOCUS_HEAD]   = "方向键选择按键, A 输入",
            [TR_COMPOSE_FOCUS_KEYS]   = "START 发送   B 删除   X 退出",
            [TR_COMPOSE_MIC_IDLE]     = "按住 R 或点击:语音输入 (English)",
            [TR_COMPOSE_MIC_REC]      = "正在聆听 English... 松开 R 转为文字",
            [TR_COMPOSE_MIC_LATCH]    = "录音已锁定 -- 按 R 或 L 结束",
            [TR_COMPOSE_MIC_WAIT]     = "服务器正在识别语音...",
            [TR_KEYS_FLOOR_0]         = "每个按键都会直接发送给 Claude",
            [TR_KEYS_FLOOR_1]         = "SHFT: 切换层   X: 返回",
            [TR_QUICK_HEAD]           = "快捷   上下选择   A 确认   X 返回",
            [TR_QUICK_SUB]            = "命令与重发会先载入编辑器",
            [TR_REMOTE_HEAD]          = "远程   上下选择   A 打开   X 返回",
            [TR_PROJECT_HINT]         = "点击标签页   X 返回",
            [TR_PROJECT_DO_SAFE]      = "点击载入编辑器; 按 A 发送",
            [TR_PROJECT_DO_GOD]       = "上帝模式: 点击立即执行",
            [TR_STATUS_PASTE]         = "载入到编辑器",
            [TR_STATUS_NO_PASTE]      = "剪贴板为空",
            [TR_CHIME_IDLE]           = "就绪",
            [TR_CHIME_WORKING]        = "Claude 正在工作",
            [TR_CHIME_APPROVAL]       = "Claude 需要你的确认",
            [TR_CHIME_QUESTION]       = "Claude 向你提问了",
            [TR_CHIME_LANTERN]        = "其他会话有新输出",
            [TR_CHIME_SENT]           = "已发送 -- 等待 Claude",
            [TR_CHIME_ANSWERING]      = "Claude 正在回答",
            [TR_CHIME_DONE]           = "完成 -- 轮到你了",
            [TR_CHIME_ANSWERED]       = "回答已就绪",
            [TR_SETTINGS_TAB_SESSION]   = "会话",
            [TR_SETTINGS_TAB_SOUND]     = "声音",
            [TR_SETTINGS_TAB_SCREEN]    = "显示",
            [TR_SETTINGS_TAB_LINK]      = "连接",
            [TR_SETTINGS_TAB_BUTTONS]   = "按键",
            [TR_SETTINGS_ROW_MODEL]     = "模型",
            [TR_SETTINGS_ROW_SESSION]   = "会话",
            [TR_SETTINGS_ROW_GOD_MODE]  = "上帝模式",
            [TR_SETTINGS_ROW_SOUND]     = "音效",
            [TR_SETTINGS_ROW_VOLUME]    = "音量",
            [TR_SETTINGS_ROW_TYPING]    = "按键音",
            [TR_SETTINGS_ROW_BLIPS]     = "打字音",
            [TR_SETTINGS_ROW_THEME]     = "主题",
            [TR_SETTINGS_ROW_LANGUAGE]  = "语言",
            [TR_SETTINGS_ROW_BOOT]      = "启动页",
            [TR_SETTINGS_ROW_ADDRESSES] = "地址显示",
            [TR_SETTINGS_ROW_AUTOCONNECT] = "自动连接",
            [TR_SETTINGS_ROW_VIEW_TAPS] = "电脑点击",
            [TR_SETTINGS_ROW_PICTURE]   = "照片尺寸",
            [TR_SETTINGS_ROW_LIVEVIEW]  = "取景画面",
            [TR_SETTINGS_ROW_MIC]       = "麦克风",
            [TR_SETTINGS_ROW_SHUTTER]   = "快门键",
            [TR_SETTINGS_ROW_DEFAULTS]  = "恢复默认",
            [TR_QUICK_ROW_MODE]         = "模式",
            [TR_QUICK_ROW_MODE_NOTE]    = "A: 切换下一模式",
            [TR_QUICK_ROW_UNDO]         = "撤回",
            [TR_QUICK_ROW_UNDO_NOTE]    = "回到之前的节点",
            [TR_QUICK_ROW_CHANGES]      = "变更",
            [TR_QUICK_ROW_CHANGES_NOTE] = "查看修改内容",
            [TR_QUICK_ROW_COMMANDS]     = "命令",
            [TR_QUICK_ROW_COMMANDS_NOTE]= "常用斜杠命令",
            [TR_QUICK_ROW_AGAIN]        = "重发",
            [TR_QUICK_ROW_AGAIN_NOTE]   = "最近发送的内容",
            [TR_QUICK_ROW_VOICE]        = "语音",
            [TR_QUICK_ROW_VOICE_NOTE]   = "语音输入到编辑器",
            [TR_QUICK_ROW_PHOTO]        = "拍照",
            [TR_QUICK_ROW_PHOTO_NOTE]   = "拍摄并发送照片",
            [TR_QUICK_FOOT] = "快捷按键上显示 Claude 的模式",
            [TR_QUICK_ADD] = "+ 新命令",
            [TR_QUICK_CMD_HEAD] = "命令   A 编辑   X 返回",
            [TR_QUICK_CMD_HEAD_SAVED] = "命令   A 编辑   Y 删除   X 返回",
            [TR_QUICK_CMD_HEAD_SURE] = "命令   A 编辑   Y 确定删除?   X 返回",
            [TR_QUICK_CMD_HEAD_ADD] = "命令   A 写入新命令   X 返回",
            [TR_QUICK_CMD_SUB] = "内置, 自定 (+ 新命令), 电脑命令",
            [TR_QUICK_AGAIN_HEAD] = "重发   A 编辑   Y 删除   X 返回",
            [TR_QUICK_AGAIN_SUB] = "最近发送的内容, 最新在前",
            [TR_QUICK_AGAIN_EMPTY_0] = "空: 在此处发送的内容",
            [TR_QUICK_AGAIN_EMPTY_1] = "都会在此显示",
            [TR_CHIME_YES] = "执行",
            [TR_CHIME_NO] = "取消",
            [TR_CONTROL_HINT_CONFIRM] = "方向键选择  %s 确认",
            [TR_CONTROL_HINT_UNDO] = "撤回: 方向键选择  %s 确定",
            [TR_CONTROL_QUICK_MODE] = "模式: %s",
            [TR_BUTTON_ENTER] = "确认",
            [TR_BUTTON_SPACE] = "空键",
            [TR_BUTTON_DELETE] = "删除",
            [TR_BUTTON_ESC] = "esc",
            [TR_BUTTON_TAB] = "tab",
            [TR_BUTTON_STOP] = "停止",
            [TR_LEGEND_MODE] = "模式",
            [TR_LEGEND_SCREENS] = "切换画面",
            [TR_LEGEND_SHOULDERS] = "L ctrl  R shift  L+R 选择按键",
            [TR_GUIDE_TAB_CONTROL] = "主画面",
            [TR_GUIDE_TAB_KEYS] = "键盘",
            [TR_GUIDE_TAB_COMPOSE] = "编辑",
            [TR_GUIDE_TAB_FOCUS] = "选键",
            [TR_GUIDE_TAB_CAMERA] = "拍照",
            [TR_GUIDE_TAB_QUICK] = "快捷",
            [TR_GUIDE_DONE] = "完成",
            [TR_GUIDE_PRESS] = "按下按键查看",
            [TR_GUIDE_TABS_HINT] = "LEFT/RIGHT 切换",
            [TR_GUIDE_FOOT] = "退出: 按住 B, 完成, 或 SELECT+START",
            [TR_GUIDE_TITLE] = "按键作用",
            [TR_GUIDE_INTRO] = "按下按键查看作用; 不会发送",
            [TR_GUIDE_COMBO_L_THEN_R] = "先 L 后 R",
            [TR_GUIDE_COMBO_R_THEN_L] = "先 R 后 L",
            [TR_GUIDE_COMBO_HOLD_R] = "按住 R",
            [TR_GUIDE_COMBO_R_THEN] = "先 R 后 %s",
            [TR_GUIDE_COMBO_DPAD] = "方向键",
            [TR_GUIDE_NAME_SCREENS] = "切换画面",
            [TR_GUIDE_NAME_SCROLL] = "滚动",
            [TR_GUIDE_NAME_ARROWS] = "方向",
            [TR_GUIDE_NAME_CTRL] = "Ctrl",
            [TR_GUIDE_NAME_SHIFT] = "Shift",
            [TR_GUIDE_NAME_MODE] = "模式",
            [TR_GUIDE_NAME_FOCUS] = "选键",
            [TR_GUIDE_NAME_BACK] = "返回",
            [TR_GUIDE_NAME_SEND] = "发送",
            [TR_GUIDE_NAME_DELETE] = "删除",
            [TR_GUIDE_NAME_CANCEL] = "取消",
            [TR_GUIDE_NAME_SNIPPETS] = "常用文字",
            [TR_GUIDE_NAME_NOTHING] = "不起作用",
            [TR_GUIDE_NAME_TALK] = "语音",
            [TR_GUIDE_NAME_LATCH] = "锁定",
            [TR_GUIDE_NAME_ORDER] = "先按 L",
            [TR_GUIDE_NAME_TYPE] = "输入",
            [TR_GUIDE_NAME_MOVE] = "移动",
            [TR_GUIDE_NAME_SHUTTER] = "快门",
            [TR_GUIDE_NAME_FLIP] = "切换",
            [TR_GUIDE_NAME_OPEN] = "打开",
            [TR_GUIDE_NAME_FORGET] = "删除",
            [TR_GUIDE_NAME_CLOSE] = "退出",
            [TR_GUIDE_DO_ENTER] = "发送 Enter: 确认选择",
            [TR_GUIDE_DO_SPACE] = "发送空键 (Space)",
            [TR_GUIDE_DO_DELETE] = "发送 Backspace: 删除前一个字",
            [TR_GUIDE_DO_ESC] = "发送 Esc: 取消或停止",
            [TR_GUIDE_DO_TAB] = "发送 Tab; 先按 R 为 Shift-Tab",
            [TR_GUIDE_DO_STOP] = "发送 Ctrl-C: Claude 停止工作",
            [TR_GUIDE_DO_X_BACK] = "返回主画面, 不发送",
            [TR_GUIDE_DO_SELECT] = "切换下一个画面",
            [TR_GUIDE_DO_SCROLL] = "滚动 (长按下: 最新); 提问时选择",
            [TR_GUIDE_DO_ARROWS] = "发送方向键, 按住会重复",
            [TR_GUIDE_DO_CTRL] = "下一个键为 Ctrl+键",
            [TR_GUIDE_DO_SHIFT] = "下一个键为 Shift; Tab 为 Shift-Tab",
            [TR_GUIDE_DO_SHIFT_TAB] = "Shift-Tab: Claude Code 切换下一模式",
            [TR_GUIDE_DO_FOCUS_TILES] = "方向键选择按键, A 打开",
            [TR_GUIDE_DO_FOCUS_KEYS] = "方向键选择键盘, A 输入, R 为 Enter",
            [TR_GUIDE_DO_SEND] = "发送编辑内容 (空: 退出)",
            [TR_GUIDE_DO_DEL] = "删除最后一个字",
            [TR_GUIDE_DO_CLOSE] = "退出编辑, 删除内容",
            [TR_GUIDE_DO_MIC_CANCEL] = "取消录音, 内容不变",
            [TR_GUIDE_DO_SNIPPETS] = "常用文字与剪贴板",
            [TR_GUIDE_DO_NOTHING] = "此处不起作用",
            [TR_GUIDE_DO_TALK] = "按住录音, 松开转为文字",
            [TR_GUIDE_DO_TALK_OFF] = "电脑需要语音识别",
            [TR_GUIDE_DO_LATCH] = "录音锁定; 按 R 或 L 结束",
            [TR_GUIDE_DO_HOLD_CANCEL] = "取消录音 (麦克风: HOLD)",
            [TR_GUIDE_DO_ORDER] = "选键: 先按住 L, 再按 R",
            [TR_GUIDE_DO_FOCUS] = "方向键选择按键, A 输入, START 发送",
            [TR_GUIDE_DO_TYPE] = "输入选中的键",
            [TR_GUIDE_DO_FOCUS_SEND] = "发送编辑内容",
            [TR_GUIDE_DO_FOCUS_OUT] = "退出选键, 内容不变",
            [TR_GUIDE_DO_MOVE_KEYS] = "在键盘上移动, 按住会重复",
            [TR_GUIDE_DO_SHUTTER] = "松开时拍照",
            [TR_GUIDE_DO_CAM_SEND] = "查看时: 发送照片到电脑",
            [TR_GUIDE_DO_CAM_LEAVE] = "退出拍照, 回到快捷",
            [TR_GUIDE_DO_FLIP] = "前后切换",
            [TR_GUIDE_DO_CAM_B] = "回到快捷; 查看时重拍",
            [TR_GUIDE_DO_CAM_X] = "退出拍照, 回到主画面",
            [TR_GUIDE_DO_CAM_SELECT] = "退出拍照, 切换下一个画面",
            [TR_GUIDE_DO_MOVE] = "移动选择",
            [TR_GUIDE_DO_OPEN] = "打开, 或载入编辑",
            [TR_GUIDE_DO_BACK] = "返回上一层, 再退出",
            [TR_GUIDE_DO_FORGET] = "删除自定命令或历史 (按 2 次)",
            [TR_GUIDE_DO_CLOSE_MENU] = "退出此画面",
            [TR_COMPOSE_HINT_EDIT] = "ENT: 换行   Y: 常用文字与剪贴板",
            [TR_COMPOSE_HINT_FOCUS] = "先 L 后 R: 选键",
            [TR_COMPOSE_HINT_TALK] = "按住 R: 语音",
            [TR_LEGEND_GUIDE] = "按住 SELECT 再按 START: 按键作用",
            [TR_QUICK_ROW_CONTROLS] = "按键作用",
            [TR_QUICK_ROW_CONTROLS_NOTE] = "每个按键的作用",
            [TR_SETTINGS_ROW_CONTROLS] = "按键作用",
            [TR_SETTINGS_RESET_SURE] = "确定?",
            [TR_COMPOSE_METER_LATCHED] = "MIC [%s]  R/L 结束  X 取消",
            [TR_COMPOSE_METER_LATCH] = "MIC [%s]  L 锁定  X 取消",
            [TR_COMPOSE_METER_HOLD] = "MIC [%s]  X 取消",
            [TR_COMPOSE_ADD_HEAD] = "新命令: 按 A 写入此 DS",
            [TR_COMPOSE_ADD_KEYS] = "A 写入   B 删除   X 取消",
            [TR_COMPOSE_ADD_FOCUS_KEYS] = "START 写入   B 删除   X 退出",
            [TR_QUICK_NOTE_NOT_HERE] = "此电脑上不起作用",
            [TR_QUICK_NOTE_NO_STT] = "服务器不开启语音识别",
            [TR_QUICK_NOTE_NO_SAVE] = "电脑上不写入照片",
        },
    },
    [LANG_JA] = {
        "ja", "JA", "日本語",
        {
            [TR_CONTROL_KEYS]         = "キー",
            [TR_CONTROL_KEYS_SUB]     = "直接入力",
            [TR_CONTROL_QUICK]        = "クイック",
            [TR_CONTROL_QUICK_SUB]    = "モード・戻す",
            [TR_CONTROL_PHOTO]        = "カメラ",
            [TR_CONTROL_PHOTO_SUB]    = "写真を撮る",
            [TR_CONTROL_AGAIN]        = "再送",
            [TR_CONTROL_AGAIN_SUB]    = "最近の入力",
            [TR_CONTROL_COMPOSE]      = "作成",
            [TR_CONTROL_COMPOSE_SUB]  = "下書き作成",
            [TR_CONTROL_REMOTE]       = "リモート",
            [TR_CONTROL_REMOTE_SUB]   = "別セッション",
            [TR_CONTROL_SETTINGS]     = "設定",
            [TR_CONTROL_SETTINGS_SUB] = "音・言語",
            [TR_CONTROL_RELINK]       = "再接続",
            [TR_CONTROL_HINT_SCROLL]  = "上下:スクロール  下長押し:最新",
            [TR_CONTROL_HINT_FOCUS]   = "十字:移動  A:開く  B:戻る",
            [TR_COMPOSE_HEAD]         = "作成: Aを押すまで送信されません",
            [TR_COMPOSE_KEYS]         = "A 送信   B 削除   X キャンセル",
            [TR_COMPOSE_FOCUS_HEAD]   = "十字で選択 A で入力",
            [TR_COMPOSE_FOCUS_KEYS]   = "START 送信   B 削除   X 戻る",
            [TR_COMPOSE_MIC_IDLE]     = "R長押し/タップで音声入力 (English)",
            [TR_COMPOSE_MIC_REC]      = "録音中 (English)... Rを離すと文字起こし",
            [TR_COMPOSE_MIC_LATCH]    = "録音ロック中 -- R / L で停止",
            [TR_COMPOSE_MIC_WAIT]     = "サーバーで音声認識中...",
            [TR_KEYS_FLOOR_0]         = "キーは直接Claudeへ送られます",
            [TR_KEYS_FLOOR_1]         = "SHFT: 切替   X: 戻る",
            [TR_QUICK_HEAD]           = "クイック   上下:選択   A:決定   X:戻る",
            [TR_QUICK_SUB]            = "コマンドと再送は作成画面へ",
            [TR_REMOTE_HEAD]          = "リモート   上下:選択   A:開く   X:戻る",
            [TR_PROJECT_HINT]         = "タブ選択   X:戻る",
            [TR_PROJECT_DO_SAFE]      = "タップで読込・Aで送信",
            [TR_PROJECT_DO_GOD]       = "GOD MODE: タップで即時実行",
            [TR_STATUS_PASTE]         = "作成画面に読み込む",
            [TR_STATUS_NO_PASTE]      = "貼り付けなし",
            [TR_CHIME_IDLE]           = "準備完了",
            [TR_CHIME_WORKING]        = "Claude 作業中",
            [TR_CHIME_APPROVAL]       = "Claude の確認待ち",
            [TR_CHIME_QUESTION]       = "Claude が質問しています",
            [TR_CHIME_LANTERN]        = "別セッションに新しい出力",
            [TR_CHIME_SENT]           = "送信済み -- 応答待ち",
            [TR_CHIME_ANSWERING]      = "Claude 回答中",
            [TR_CHIME_DONE]           = "完了 -- あなたの番です",
            [TR_CHIME_ANSWERED]       = "回答完了",
            [TR_SETTINGS_TAB_SESSION]   = "セッション",
            [TR_SETTINGS_TAB_SOUND]     = "音",
            [TR_SETTINGS_TAB_SCREEN]    = "画面",
            [TR_SETTINGS_TAB_LINK]      = "接続",
            [TR_SETTINGS_TAB_BUTTONS]   = "ボタン",
            [TR_SETTINGS_ROW_MODEL]     = "モデル",
            [TR_SETTINGS_ROW_SESSION]   = "コマンド",
            [TR_SETTINGS_ROW_GOD_MODE]  = "GOD MODE",
            [TR_SETTINGS_ROW_SOUND]     = "サウンド",
            [TR_SETTINGS_ROW_VOLUME]    = "音量",
            [TR_SETTINGS_ROW_TYPING]    = "打鍵音",
            [TR_SETTINGS_ROW_BLIPS]     = "書込音",
            [TR_SETTINGS_ROW_THEME]     = "テーマ",
            [TR_SETTINGS_ROW_LANGUAGE]  = "言語",
            [TR_SETTINGS_ROW_BOOT]      = "起動画面",
            [TR_SETTINGS_ROW_ADDRESSES] = "IP表示",
            [TR_SETTINGS_ROW_AUTOCONNECT] = "自動接続",
            [TR_SETTINGS_ROW_VIEW_TAPS] = "PCタップ",
            [TR_SETTINGS_ROW_PICTURE]   = "写真サイズ",
            [TR_SETTINGS_ROW_LIVEVIEW]  = "プレビュー",
            [TR_SETTINGS_ROW_MIC]       = "マイク",
            [TR_SETTINGS_ROW_SHUTTER]   = "シャッター",
            [TR_SETTINGS_ROW_DEFAULTS]  = "初期化",
            [TR_QUICK_ROW_MODE]         = "モード",
            [TR_QUICK_ROW_MODE_NOTE]    = "A: 次のモードへ",
            [TR_QUICK_ROW_UNDO]         = "戻す",
            [TR_QUICK_ROW_UNDO_NOTE]    = "前の時点に巻き戻す",
            [TR_QUICK_ROW_CHANGES]      = "変更点",
            [TR_QUICK_ROW_CHANGES_NOTE] = "変更された差分を見る",
            [TR_QUICK_ROW_COMMANDS]     = "コマンド",
            [TR_QUICK_ROW_COMMANDS_NOTE]= "スラッシュコマンド",
            [TR_QUICK_ROW_AGAIN]        = "再送",
            [TR_QUICK_ROW_AGAIN_NOTE]   = "最近送った入力",
            [TR_QUICK_ROW_VOICE]        = "音声",
            [TR_QUICK_ROW_VOICE_NOTE]   = "音声で下書き入力",
            [TR_QUICK_ROW_PHOTO]        = "カメラ",
            [TR_QUICK_ROW_PHOTO_NOTE]   = "撮影して送信",
            [TR_QUICK_FOOT] = "クイックのタイルに Claude のモードを表示",
            [TR_QUICK_ADD] = "+ 作成",
            [TR_QUICK_CMD_HEAD] = "コマンド   A:作成   X:戻る",
            [TR_QUICK_CMD_HEAD_SAVED] = "コマンド   A:作成   Y:削除   X:戻る",
            [TR_QUICK_CMD_HEAD_SURE] = "コマンド   A:作成   Y:削除する?   X:戻る",
            [TR_QUICK_CMD_HEAD_ADD] = "コマンド   A:新しいコマンド   X:戻る",
            [TR_QUICK_CMD_SUB] = "初期・自作 (+ 作成)・PCのコマンド",
            [TR_QUICK_AGAIN_HEAD] = "再送   A:作成   Y:削除   X:戻る",
            [TR_QUICK_AGAIN_SUB] = "最近送った入力 (上が新しい)",
            [TR_QUICK_AGAIN_EMPTY_0] = "空: ここで送った入力は",
            [TR_QUICK_AGAIN_EMPTY_1] = "このリストに表示されます",
            [TR_CHIME_YES] = "実行",
            [TR_CHIME_NO] = "中止",
            [TR_CONTROL_HINT_CONFIRM] = "十字:選択  %s:決定",
            [TR_CONTROL_HINT_UNDO] = "戻す: 十字:選択  %s:決定",
            [TR_CONTROL_QUICK_MODE] = "動作: %s",
            [TR_BUTTON_ENTER] = "決定",
            [TR_BUTTON_SPACE] = "空き",
            [TR_BUTTON_DELETE] = "削除",
            [TR_BUTTON_ESC] = "esc",
            [TR_BUTTON_TAB] = "tab",
            [TR_BUTTON_STOP] = "停止",
            [TR_LEGEND_MODE] = "モード",
            [TR_LEGEND_SCREENS] = "画面切替",
            [TR_LEGEND_SHOULDERS] = "L ctrl  R shift  L+R タイル選択",
            [TR_GUIDE_TAB_CONTROL] = "メイン",
            [TR_GUIDE_TAB_KEYS] = "キー",
            [TR_GUIDE_TAB_COMPOSE] = "作成",
            [TR_GUIDE_TAB_FOCUS] = "選択",
            [TR_GUIDE_TAB_CAMERA] = "カメラ",
            [TR_GUIDE_TAB_QUICK] = "クイック",
            [TR_GUIDE_DONE] = "完了",
            [TR_GUIDE_PRESS] = "ボタンを押す",
            [TR_GUIDE_TABS_HINT] = "LEFT/RIGHT 切替",
            [TR_GUIDE_FOOT] = "戻る: B長押し・完了・SELECT+START",
            [TR_GUIDE_TITLE] = "コントロール",
            [TR_GUIDE_INTRO] = "ボタンを押すと動作を表示 (送信なし)",
            [TR_GUIDE_COMBO_L_THEN_R] = "Lの次にR",
            [TR_GUIDE_COMBO_R_THEN_L] = "Rの次にL",
            [TR_GUIDE_COMBO_HOLD_R] = "R長押し",
            [TR_GUIDE_COMBO_R_THEN] = "Rの次に%s",
            [TR_GUIDE_COMBO_DPAD] = "十字キー",
            [TR_GUIDE_NAME_SCREENS] = "画面切替",
            [TR_GUIDE_NAME_SCROLL] = "スクロール",
            [TR_GUIDE_NAME_ARROWS] = "十字キー",
            [TR_GUIDE_NAME_CTRL] = "Ctrl",
            [TR_GUIDE_NAME_SHIFT] = "Shift",
            [TR_GUIDE_NAME_MODE] = "モード",
            [TR_GUIDE_NAME_FOCUS] = "キー選択",
            [TR_GUIDE_NAME_BACK] = "戻る",
            [TR_GUIDE_NAME_SEND] = "送信",
            [TR_GUIDE_NAME_DELETE] = "削除",
            [TR_GUIDE_NAME_CANCEL] = "取消",
            [TR_GUIDE_NAME_SNIPPETS] = "定型文",
            [TR_GUIDE_NAME_NOTHING] = "なし",
            [TR_GUIDE_NAME_TALK] = "音声",
            [TR_GUIDE_NAME_LATCH] = "ロック",
            [TR_GUIDE_NAME_ORDER] = "Lが先",
            [TR_GUIDE_NAME_TYPE] = "入力",
            [TR_GUIDE_NAME_MOVE] = "移動",
            [TR_GUIDE_NAME_SHUTTER] = "シャッター",
            [TR_GUIDE_NAME_FLIP] = "切替",
            [TR_GUIDE_NAME_OPEN] = "開く",
            [TR_GUIDE_NAME_FORGET] = "削除",
            [TR_GUIDE_NAME_CLOSE] = "出る",
            [TR_GUIDE_DO_ENTER] = "Enter を送信: 決定・選択",
            [TR_GUIDE_DO_SPACE] = "Space を送信",
            [TR_GUIDE_DO_DELETE] = "Backspace を送信: 前の字を削除",
            [TR_GUIDE_DO_ESC] = "Esc を送信: キャンセル",
            [TR_GUIDE_DO_TAB] = "Tab を送信; R が先なら Shift-Tab",
            [TR_GUIDE_DO_STOP] = "Ctrl-C を送信: Claude を止める",
            [TR_GUIDE_DO_X_BACK] = "メイン画面へ戻る (送信なし)",
            [TR_GUIDE_DO_SELECT] = "次の画面へ",
            [TR_GUIDE_DO_SCROLL] = "スクロール (下長押し: 最新); 質問中は選択",
            [TR_GUIDE_DO_ARROWS] = "十字キーを送信; 長押しで続けて送信",
            [TR_GUIDE_DO_CTRL] = "次のキーを Ctrl+キー にする",
            [TR_GUIDE_DO_SHIFT] = "次のキーを Shift に; Tab は Shift-Tab",
            [TR_GUIDE_DO_SHIFT_TAB] = "Shift-Tab: Claude Code の次のモード",
            [TR_GUIDE_DO_FOCUS_TILES] = "十字でタイル選択, A で開く",
            [TR_GUIDE_DO_FOCUS_KEYS] = "十字でキー選択, A で入力, R で Enter",
            [TR_GUIDE_DO_SEND] = "下書きを送信 (空なら戻る)",
            [TR_GUIDE_DO_DEL] = "下書きの字を削除",
            [TR_GUIDE_DO_CLOSE] = "作成画面を出る; 下書きは削除",
            [TR_GUIDE_DO_MIC_CANCEL] = "録音をキャンセル; 下書きは変更なし",
            [TR_GUIDE_DO_SNIPPETS] = "定型文とクリップボード",
            [TR_GUIDE_DO_NOTHING] = "ここでは動作なし",
            [TR_GUIDE_DO_TALK] = "長押しで録音; 離すと文字に",
            [TR_GUIDE_DO_TALK_OFF] = "音声認識のある PC が要る",
            [TR_GUIDE_DO_LATCH] = "録音ロック中; R / L で停止",
            [TR_GUIDE_DO_HOLD_CANCEL] = "録音をキャンセル (マイク: HOLD)",
            [TR_GUIDE_DO_ORDER] = "キー選択: L を押したまま R",
            [TR_GUIDE_DO_FOCUS] = "十字でキー選択, A で入力, START で送信",
            [TR_GUIDE_DO_TYPE] = "選択中のキーを入力",
            [TR_GUIDE_DO_FOCUS_SEND] = "下書きを送信",
            [TR_GUIDE_DO_FOCUS_OUT] = "キー選択を出る; 下書きは変更なし",
            [TR_GUIDE_DO_MOVE_KEYS] = "キーの上を移動; 長押しで続く",
            [TR_GUIDE_DO_SHUTTER] = "離すと撮影",
            [TR_GUIDE_DO_CAM_SEND] = "確認中: 写真を PC へ送信",
            [TR_GUIDE_DO_CAM_LEAVE] = "カメラを出て, クイックへ",
            [TR_GUIDE_DO_FLIP] = "カメラの切替",
            [TR_GUIDE_DO_CAM_B] = "クイックへ; 確認中は撮り直し",
            [TR_GUIDE_DO_CAM_X] = "カメラを出て, メイン画面へ",
            [TR_GUIDE_DO_CAM_SELECT] = "カメラを出て, 次の画面へ",
            [TR_GUIDE_DO_MOVE] = "選択を移動",
            [TR_GUIDE_DO_OPEN] = "開く, または作成画面に読込",
            [TR_GUIDE_DO_BACK] = "前へ戻る, 次に出る",
            [TR_GUIDE_DO_FORGET] = "自作コマンド・最近の入力を削除 (2回)",
            [TR_GUIDE_DO_CLOSE_MENU] = "この画面を出る",
            [TR_COMPOSE_HINT_EDIT] = "ENT: 改行   Y: 定型文・クリップボード",
            [TR_COMPOSE_HINT_FOCUS] = "Lの次にR: キー選択",
            [TR_COMPOSE_HINT_TALK] = "R長押し: 音声",
            [TR_LEGEND_GUIDE] = "SELECTを押してSTART: コントロール",
            [TR_QUICK_ROW_CONTROLS] = "コントロール",
            [TR_QUICK_ROW_CONTROLS_NOTE] = "ボタンとコンボの動作",
            [TR_SETTINGS_ROW_CONTROLS] = "コントロール",
            [TR_SETTINGS_RESET_SURE] = "確定?",
            [TR_COMPOSE_METER_LATCHED] = "MIC [%s]  R/L 停止  X 取消",
            [TR_COMPOSE_METER_LATCH] = "MIC [%s]  L ロック  X 取消",
            [TR_COMPOSE_METER_HOLD] = "MIC [%s]  X 取消",
            [TR_COMPOSE_ADD_HEAD] = "新しいコマンド: A で DS に書き込み",
            [TR_COMPOSE_ADD_KEYS] = "A 書込   B 削除   X キャンセル",
            [TR_COMPOSE_ADD_FOCUS_KEYS] = "START 書込   B 削除   X 戻る",
            [TR_QUICK_NOTE_NOT_HERE] = "この PC にはありません",
            [TR_QUICK_NOTE_NO_STT] = "サーバーに音声認識なし",
            [TR_QUICK_NOTE_NO_SAVE] = "PC に写真を書き込めません",
        },
    },
};

void lang_init(void)
{
    active_lang = LANG_EN;
    memset(custom_langs, 0, sizeof(custom_langs));
    custom_count = 0;
}

unsigned lang_count(void)
{
    return (unsigned)LANG_BUILTIN_COUNT + custom_count;
}

unsigned lang_current(void)
{
    return active_lang < lang_count() ? active_lang : (unsigned)LANG_EN;
}

void lang_set_current(unsigned index)
{
    if (index < lang_count()) active_lang = index;
}

int lang_find_code(const char *code)
{
    if (!code || !code[0]) return -1;
    for (unsigned i = 0; i < LANG_BUILTIN_COUNT; i++) {
        if (ascii_casecmp(code, BUILTIN[i].code) ||
            ascii_casecmp(code, BUILTIN[i].label)) {
            return (int)i;
        }
    }
    for (unsigned i = 0; i < custom_count; i++) {
        if (ascii_casecmp(code, custom_langs[i].code) ||
            ascii_casecmp(code, custom_langs[i].label)) {
            return (int)(LANG_BUILTIN_COUNT + i);
        }
    }
    return -1;
}

const char *lang_code(unsigned index)
{
    if (index < LANG_BUILTIN_COUNT) return BUILTIN[index].code;
    const unsigned custom = index - LANG_BUILTIN_COUNT;
    if (custom < custom_count) return custom_langs[custom].code;
    return BUILTIN[LANG_EN].code;
}

const char *lang_voice_code(unsigned index)
{
    if (index == LANG_ZH || index == LANG_JA) return BUILTIN[LANG_EN].code;
    const char *code = lang_code(index);
    static const char *const NON_LATIN[] = { "zh", "ja", "ko" };
    for (size_t i = 0; i < sizeof(NON_LATIN) / sizeof(NON_LATIN[0]); i++) {
        // A prefix, so "zh-tw" and "ja_jp" count; codes are stored lowercase
        // (clean_code), and the next byte must end the language part.
        if (code[0] == NON_LATIN[i][0] && code[1] == NON_LATIN[i][1] &&
            (code[2] == '\0' || code[2] == '-' || code[2] == '_')) {
            return BUILTIN[LANG_EN].code;
        }
    }
    return code;
}

const char *lang_label(unsigned index)
{
    if (index < LANG_BUILTIN_COUNT) return BUILTIN[index].label;
    const unsigned custom = index - LANG_BUILTIN_COUNT;
    if (custom < custom_count) return custom_langs[custom].label;
    return "";
}

const char *lang_name(unsigned index)
{
    if (index < LANG_BUILTIN_COUNT) return BUILTIN[index].name;
    const unsigned custom = index - LANG_BUILTIN_COUNT;
    if (custom < custom_count) return custom_langs[custom].name;
    return BUILTIN[LANG_EN].name;
}

const char *lang_tr_at(unsigned index, tr_id_t id)
{
    if ((unsigned)id >= TR_COUNT) return "";
    if (index < LANG_BUILTIN_COUNT) {
        const char *s = BUILTIN[index].strings[id];
        return (s && s[0]) ? s : BUILTIN[LANG_EN].strings[id];
    }
    const unsigned custom = index - LANG_BUILTIN_COUNT;
    if (custom < custom_count && custom_langs[custom].strings[id][0]) {
        return custom_langs[custom].strings[id];
    }
    return BUILTIN[LANG_EN].strings[id];
}

const char *lang_builtin_string(unsigned index, tr_id_t id)
{
    if (index >= LANG_BUILTIN_COUNT || (unsigned)id >= TR_COUNT) return NULL;
    const char *s = BUILTIN[index].strings[id];
    return (s && s[0]) ? s : NULL;
}

const char *tr(tr_id_t id)
{
    return lang_tr_at(active_lang, id);
}

const char *lang_key_name(tr_id_t id)
{
    if ((unsigned)id >= TR_COUNT) return NULL;
    return KEY_NAMES[id];
}

tr_id_t lang_key_find(const char *key)
{
    if (!key || !key[0]) return TR_COUNT;
    for (unsigned i = 0; i < TR_COUNT; i++) {
        if (ascii_casecmp(key, KEY_NAMES[i])) return (tr_id_t)i;
    }
    return TR_COUNT;
}

static int valid_code_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static void clean_code(const char *raw, char out[LANG_CODE_MAX + 1])
{
    size_t k = 0;
    if (raw) {
        for (size_t i = 0; raw[i] && k < LANG_CODE_MAX; i++) {
            char c = raw[i];
            if (!valid_code_char(c)) continue;
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[k++] = c;
        }
    }
    out[k] = '\0';
}

// A value longer than LANG_STR_MAX is cut there -- on a whole UTF-8
// character, as lang_fill() cuts, since half a character draws as '?'.
// Returns how many of the first `n` bytes of `s` end on a character
// boundary: `n` itself unless its last sequence is incomplete.
static size_t utf8_whole_prefix(const char *s, size_t n)
{
    size_t lead = n;
    unsigned back = 0;
    while (lead > 0 && back < 4 && ((unsigned char)s[lead - 1] & 0xC0) == 0x80) {
        lead--;
        back++;
    }
    if (lead == 0) return n;            // nothing but continuation bytes
    const unsigned char c = (unsigned char)s[lead - 1];
    const size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    const size_t have = n - (lead - 1);
    return have < want ? lead - 1 : n;
}

static void default_label_from_code(const char *code, char out[LANG_LABEL_MAX + 1])
{
    size_t k = 0;
    for (size_t i = 0; code[i] && k < LANG_LABEL_MAX; i++) {
        char c = code[i];
        if (c == '-' || c == '_') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[k++] = c;
    }
    out[k] = '\0';
}

int lang_load_text(const char *text, const char *fallback_code,
                   char *err, size_t err_len)
{
    if (!text) {
        set_err(err, err_len, "no language text");
        return -1;
    }

    // Static, as are the line below and lang_load_file()'s buffer: together
    // they are about 11 KB, which is most of the ARM9's ~16 KB DTCM stack, on
    // a device with no MMU to say when a stack runs into the data under it.
    // Nothing here is reentrant and nothing needs it to be.
    static custom_lang_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (fallback_code) clean_code(fallback_code, parsed.code);

    const char *at = text;
    // Skip UTF-8 BOM if present.
    if ((unsigned char)at[0] == 0xEF &&
        (unsigned char)at[1] == 0xBB &&
        (unsigned char)at[2] == 0xBF) {
        at += 3;
    }

    unsigned entries = 0;
    while (*at) {
        static char line[256];
        size_t n = 0;
        while (*at && *at != '\n' && n + 1 < sizeof(line)) line[n++] = *at++;
        while (*at && *at != '\n') at++;
        if (*at == '\n') at++;
        line[n] = '\0';

        char *cursor = line;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '\0' || *cursor == '#') continue;

        char *eq = strchr(cursor, '=');
        if (!eq) {
            set_err(err, err_len, "language line without key=value");
            return -1;
        }
        *eq = '\0';
        char *key = cursor;
        char *val = eq + 1;

        char *kend = key + strlen(key);
        while (kend > key && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char *vend = val + strlen(val);
        while (vend > val && (vend[-1] == ' ' || vend[-1] == '\t' || vend[-1] == '\r')) {
            *--vend = '\0';
        }

        if (ascii_casecmp(key, "code")) {
            clean_code(val, parsed.code);
            continue;
        }
        if (ascii_casecmp(key, "label")) {
            size_t k = 0;
            for (size_t i = 0; val[i] && k < LANG_LABEL_MAX; i++) {
                unsigned char c = (unsigned char)val[i];
                if (c < 0x21 || c > 0x7E) continue;
                if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
                parsed.label[k++] = (char)c;
            }
            parsed.label[k] = '\0';
            continue;
        }
        if (ascii_casecmp(key, "name")) {
            (void)snprintf(parsed.name, sizeof(parsed.name), "%s", val);
            continue;
        }

        const tr_id_t id = lang_key_find(key);
        if (id < TR_COUNT && val[0]) {
            // Strip control bytes < 0x20 so a malformed file cannot inject newlines.
            size_t k = 0, i = 0;
            for (; val[i] && k < LANG_STR_MAX; i++) {
                if ((unsigned char)val[i] >= 0x20) {
                    parsed.strings[id][k++] = val[i];
                }
            }
            while (val[i] && (unsigned char)val[i] < 0x20) i++;
            if (val[i]) k = utf8_whole_prefix(parsed.strings[id], k);
            parsed.strings[id][k] = '\0';
            entries++;
        }
    }

    if (!parsed.code[0]) {
        set_err(err, err_len, "language file has no code=");
        return -1;
    }
    if (!parsed.label[0]) default_label_from_code(parsed.code, parsed.label);
    if (!parsed.name[0]) (void)snprintf(parsed.name, sizeof(parsed.name), "%s", parsed.label);
    if (entries == 0) {
        set_err(err, err_len, "language file has no known string keys");
        return -1;
    }

    // Refuse colliding with a built-in language code.
    for (unsigned i = 0; i < LANG_BUILTIN_COUNT; i++) {
        if (ascii_casecmp(parsed.code, BUILTIN[i].code)) {
            set_err(err, err_len, "cannot override built-in language code");
            return -1;
        }
    }

    // Update existing custom pack if already loaded.
    for (unsigned i = 0; i < custom_count; i++) {
        if (ascii_casecmp(parsed.code, custom_langs[i].code)) {
            parsed.used = 1;
            custom_langs[i] = parsed;
            return (int)(LANG_BUILTIN_COUNT + i);
        }
    }

    if (custom_count >= LANG_CUSTOM_MAX) {
        set_err(err, err_len, "custom language slots full");
        return -1;
    }
    parsed.used = 1;
    custom_langs[custom_count] = parsed;
    return (int)(LANG_BUILTIN_COUNT + custom_count++);
}

int lang_load_file(const char *path, const char *fallback_code,
                   char *err, size_t err_len)
{
    if (!path || !path[0]) {
        set_err(err, err_len, "no language path");
        return -1;
    }
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        set_err(err, err_len, "cannot open language file");
        return -1;
    }
    static char buf[LANG_FILE_MAX + 1];   // static: see lang_load_text()
    const size_t got = fread(buf, 1, LANG_FILE_MAX + 1, fh);
    fclose(fh);
    if (got > LANG_FILE_MAX) {
        set_err(err, err_len, "language file exceeds 8 KB");
        return -1;
    }
    buf[got] = '\0';
    return lang_load_text(buf, fallback_code, err, err_len);
}

uint32_t lang_utf8_next(const char **s)
{
    if (!s || !*s || !**s) return 0;
    const unsigned char *p = (const unsigned char *)*s;
    const unsigned char b0 = p[0];
    if (b0 < 0x80) {
        *s = (const char *)(p + 1);
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if ((p[1] & 0xC0) != 0x80) {
            *s = (const char *)(p + 1);
            return '?';
        }
        const uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
        *s = (const char *)(p + 2);
        return cp >= 0x80 ? cp : '?';
    }
    if ((b0 & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) {
            *s = (const char *)(p + 1);
            return '?';
        }
        const uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) |
                            ((uint32_t)(p[1] & 0x3F) << 6) |
                            (uint32_t)(p[2] & 0x3F);
        *s = (const char *)(p + 3);
        return cp >= 0x800 ? cp : '?';
    }
    if ((b0 & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 ||
            (p[3] & 0xC0) != 0x80) {
            *s = (const char *)(p + 1);
            return '?';
        }
        const uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) |
                            ((uint32_t)(p[1] & 0x3F) << 12) |
                            ((uint32_t)(p[2] & 0x3F) << 6) |
                            (uint32_t)(p[3] & 0x3F);
        *s = (const char *)(p + 4);
        return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : '?';
    }
    *s = (const char *)(p + 1);
    return '?';
}

int lang_cp_is_wide(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF01 && cp <= 0xFF60);
}

unsigned lang_display_cols(const char *s)
{
    if (!s) return 0;
    unsigned cols = 0;
    const char *p = s;
    uint32_t cp;
    while ((cp = lang_utf8_next(&p)) != 0) {
        cols += lang_cp_is_wide(cp) ? 2u : 1u;
    }
    return cols;
}

unsigned lang_pad_cols(const char *s, unsigned min_cols, unsigned max_cols,
                       char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    size_t at = 0;
    unsigned cols = 0;
    const char *p = s ? s : "";
    while (*p) {
        const char *prev = p;
        const uint32_t cp = lang_utf8_next(&p);
        if (!cp) break;
        const unsigned w = lang_cp_is_wide(cp) ? 2u : 1u;
        const size_t bytes = (size_t)(p - prev);
        if (max_cols && cols + w > max_cols) break;
        if (at + bytes >= cap) break;
        memcpy(out + at, prev, bytes);
        at += bytes;
        cols += w;
    }
    while (cols < min_cols && (!max_cols || cols < max_cols) && at + 1 < cap) {
        out[at++] = ' ';
        cols++;
    }
    out[at] = '\0';
    return cols;
}

char *lang_fill(char *out, size_t cap, const char *tmpl, const char *arg)
{
    if (!out || cap == 0) return out;
    size_t at = 0;
    int filled = 0;
    const char *p = tmpl ? tmpl : "";
    while (*p) {
        const char *src = p;
        size_t len;
        if (!filled && p[0] == '%' && p[1] == 's') {
            filled = 1;
            p += 2;
            src = arg ? arg : "";
            len = strlen(src);
        } else {
            const char *prev = p;
            (void)lang_utf8_next(&p);
            len = (size_t)(p - prev);
        }
        // Whole characters only: a cut UTF-8 sequence draws as '?'.
        const char *q = src;
        while ((size_t)(q - src) < len) {
            const char *prev = q;
            (void)lang_utf8_next(&q);
            const size_t n = (size_t)(q - prev);
            if (at + n >= cap) { out[at] = '\0'; return out; }
            memcpy(out + at, prev, n);
            at += n;
        }
    }
    out[at] = '\0';
    return out;
}

// -------------------------------------------------- accented Latin glyphs ---

typedef enum {
    MARK_GRAVE = 0,
    MARK_ACUTE,
    MARK_CIRCUMFLEX,
    MARK_TILDE,
    MARK_DIAERESIS,
    MARK_CEDILLA,
    MARK_COUNT,
} latin_mark_t;

// Rows for each mark.  The letters sit in columns 0..3 (bits 0x20..0x04), so
// marks centre on the gap between columns 1 and 2.  Lower case gets two rows,
// upper case one.
static const uint8_t MARK_LOWER[MARK_COUNT][2] = {
    [MARK_GRAVE]      = { 0x10, 0x08 },
    [MARK_ACUTE]      = { 0x04, 0x08 },
    [MARK_CIRCUMFLEX] = { 0x18, 0x24 },
    [MARK_TILDE]      = { 0x14, 0x28 },
    [MARK_DIAERESIS]  = { 0x00, 0x24 },
    [MARK_CEDILLA]    = { 0x00, 0x00 },
};
static const uint8_t MARK_UPPER[MARK_COUNT] = {
    [MARK_GRAVE]      = 0x30,
    [MARK_ACUTE]      = 0x0C,
    [MARK_CIRCUMFLEX] = 0x18,
    [MARK_TILDE]      = 0x14,
    [MARK_DIAERESIS]  = 0x24,
    [MARK_CEDILLA]    = 0x00,
};

static int latin_split(uint32_t cp, char *base, latin_mark_t *mark)
{
    // Upper and lower case share a layout 0x20 apart, except where noted.
    static const struct { char base; uint8_t mark; } TABLE[0x20] = {
        [0x00] = { 'A', MARK_GRAVE },  [0x01] = { 'A', MARK_ACUTE },
        [0x02] = { 'A', MARK_CIRCUMFLEX }, [0x03] = { 'A', MARK_TILDE },
        [0x04] = { 'A', MARK_DIAERESIS },
        [0x07] = { 'C', MARK_CEDILLA },
        [0x08] = { 'E', MARK_GRAVE },  [0x09] = { 'E', MARK_ACUTE },
        [0x0A] = { 'E', MARK_CIRCUMFLEX }, [0x0B] = { 'E', MARK_DIAERESIS },
        [0x0C] = { 'I', MARK_GRAVE },  [0x0D] = { 'I', MARK_ACUTE },
        [0x0E] = { 'I', MARK_CIRCUMFLEX }, [0x0F] = { 'I', MARK_DIAERESIS },
        [0x11] = { 'N', MARK_TILDE },
        [0x12] = { 'O', MARK_GRAVE },  [0x13] = { 'O', MARK_ACUTE },
        [0x14] = { 'O', MARK_CIRCUMFLEX }, [0x15] = { 'O', MARK_TILDE },
        [0x16] = { 'O', MARK_DIAERESIS },
        [0x19] = { 'U', MARK_GRAVE },  [0x1A] = { 'U', MARK_ACUTE },
        [0x1B] = { 'U', MARK_CIRCUMFLEX }, [0x1C] = { 'U', MARK_DIAERESIS },
        [0x1D] = { 'Y', MARK_ACUTE },
    };
    if (cp == 0xFF) {                       // y-diaeresis: its capital is not Latin-1
        *base = 'y';
        *mark = MARK_DIAERESIS;
        return 1;
    }
    if (cp < 0xC0 || cp > 0xFD) return 0;
    const int lower = cp >= 0xE0;
    const unsigned slot = (unsigned)(cp - (lower ? 0xE0u : 0xC0u));
    if (!TABLE[slot].base) return 0;
    *base = lower ? (char)(TABLE[slot].base + 32) : TABLE[slot].base;
    *mark = (latin_mark_t)TABLE[slot].mark;
    return 1;
}

int lang_latin_glyph(uint32_t cp, const uint8_t ascii[][8], uint8_t out[8])
{
    char base;
    latin_mark_t mark;
    if (!ascii || !out || !latin_split(cp, &base, &mark)) return 0;
    const uint8_t *src = ascii[(unsigned char)base - 0x20u];
    for (unsigned r = 0; r < 8; r++) out[r] = src[r];

    if (mark == MARK_CEDILLA) {
        out[7] |= 0x08;                     // the empty row under the bowl
        return 1;
    }
    if (base >= 'a' && base <= 'z') {
        if (base == 'i') out[1] = 0;        // the dot gives way to the mark
        out[0] |= MARK_LOWER[mark][0];
        out[1] |= MARK_LOWER[mark][1];
        return 1;
    }
    // Upper case fills rows 1..6.  Drop the first row (from 2 on) that
    // repeats the one under it -- a vertical stroke loses one pixel of
    // height, which reads; a crossbar or bowl does not -- or row 4 if none
    // does, and move what is above it down a row.  Row 0 takes the mark and
    // row 1 stays clear, so the mark never touches the letter.
    unsigned drop = 4;
    for (unsigned r = 2; r <= 5; r++) {
        if (src[r] == src[r + 1]) { drop = r; break; }
    }
    for (unsigned r = drop; r >= 2; r--) out[r] = src[r - 1];
    out[1] = 0;
    out[0] = MARK_UPPER[mark];
    return 1;
}
