#include "buttons.h"

#include <stdio.h>
#include <string.h>

#include "camera.h"
#include "lang.h"

static const char *const NAMES[BUTTON_COUNT] = { "A", "B", "X", "Y", "START" };

static const struct {
    const char *label;   // on a chip: three or four letters
    const char *word;    // in a hint and in the settings file
    const char *bytes;
    size_t len;
} ACTIONS[BUTTON_ACTION_COUNT] = {
    [BUTTON_ENTER]  = { "ENT",  "enter",  "\r",   1 },
    [BUTTON_SPACE]  = { "SPC",  "space",  " ",    1 },
    // DEL, as a keyboard's Backspace sends it.
    [BUTTON_DELETE] = { "DEL",  "delete", "\x7f", 1 },
    [BUTTON_ESC]    = { "ESC",  "esc",    "\x1b", 1 },
    [BUTTON_TAB]    = { "TAB",  "tab",    "\t",   1 },
    [BUTTON_STOP]   = { "STOP", "stop",   "\x03", 1 },
};

void button_map_defaults(button_map_t *map)
{
    if (!map) return;
    map->action[BUTTON_A] = BUTTON_ENTER;
    map->action[BUTTON_B] = BUTTON_DELETE;
    map->action[BUTTON_X] = BUTTON_ESC;
    map->action[BUTTON_Y] = BUTTON_TAB;
    map->action[BUTTON_START] = BUTTON_STOP;
}

int button_map_valid(const button_map_t *map)
{
    if (!map) return 0;
    for (unsigned b = 0; b < BUTTON_COUNT; b++) {
        if (map->action[b] >= BUTTON_ACTION_COUNT) return 0;
    }
    return button_for(map, BUTTON_ENTER) != BUTTON_COUNT;
}

int button_map_set(button_map_t *map, button_t button, button_action_t action)
{
    if (!map || button >= BUTTON_COUNT || action >= BUTTON_ACTION_COUNT) return 0;
    button_map_t next = *map;
    next.action[button] = (uint8_t)action;
    if (button_for(&next, BUTTON_ENTER) == BUTTON_COUNT) return 0;
    *map = next;
    return 1;
}

button_t button_for(const button_map_t *map, button_action_t action)
{
    if (!map) return BUTTON_COUNT;
    for (unsigned b = 0; b < BUTTON_COUNT; b++) {
        if (map->action[b] == (uint8_t)action) return (button_t)b;
    }
    return BUTTON_COUNT;
}

const char *button_name(button_t button)
{
    return button < BUTTON_COUNT ? NAMES[button] : "";
}

const char *button_action_label(button_action_t action)
{
    return action < BUTTON_ACTION_COUNT ? ACTIONS[action].label : "";
}

const char *button_action_word(button_action_t action)
{
    return action < BUTTON_ACTION_COUNT ? ACTIONS[action].word : "";
}

// The word in a hint, in the language on screen.  button_action_word() stays
// English: it is also what the settings file stores.
const char *button_action_hint(button_action_t action)
{
    static const tr_id_t HINT[BUTTON_ACTION_COUNT] = {
        [BUTTON_ENTER]  = TR_BUTTON_ENTER,
        [BUTTON_SPACE]  = TR_BUTTON_SPACE,
        [BUTTON_DELETE] = TR_BUTTON_DELETE,
        [BUTTON_ESC]    = TR_BUTTON_ESC,
        [BUTTON_TAB]    = TR_BUTTON_TAB,
        [BUTTON_STOP]   = TR_BUTTON_STOP,
    };
    return action < BUTTON_ACTION_COUNT ? tr(HINT[action]) : "";
}

const char *button_action_bytes(button_action_t action, size_t *len)
{
    if (action >= BUTTON_ACTION_COUNT) action = BUTTON_ENTER;
    if (len) *len = ACTIONS[action].len;
    return ACTIONS[action].bytes;
}

void button_map_legend(const button_map_t *map, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    if (!map) return;
    size_t at = 0;
    // The four face buttons; START has its own line beside SELECT.
    for (unsigned b = BUTTON_A; b <= BUTTON_Y && at < cap; b++) {
        const int n = snprintf(out + at, cap - at, "%s%s %s", b ? "  " : "",
                               NAMES[b], button_action_hint(map->action[b]));
        if (n < 0) break;
        at += (size_t)n;
    }
}

// --------------------------------------------------------- the shutter ---

// CAMERA_KEY_* are the kbd key word's register bits, defined once in
// camera.h: A is bit 0, R bit 8, L bit 9.
static const struct {
    const char *name;
    const char *word;
    unsigned key;
    unsigned other;
} SHUTTERS[SHUTTER_COUNT] = {
    [SHUTTER_R] = { "R", "r", CAMERA_KEY_R, CAMERA_KEY_L },
    [SHUTTER_L] = { "L", "l", CAMERA_KEY_L, CAMERA_KEY_R },
    [SHUTTER_A] = { "A", "a", (1u << 0),    0 },
};

static camera_shutter_t sane(camera_shutter_t s)
{
    return s < SHUTTER_COUNT ? s : SHUTTER_R;
}

const char *camera_shutter_name(camera_shutter_t s)
{
    return SHUTTERS[sane(s)].name;
}

const char *camera_shutter_word(camera_shutter_t s)
{
    return SHUTTERS[sane(s)].word;
}

unsigned camera_shutter_key_mask(camera_shutter_t s)
{
    return SHUTTERS[sane(s)].key;
}

unsigned camera_shutter_other_mask(camera_shutter_t s)
{
    return SHUTTERS[sane(s)].other;
}
