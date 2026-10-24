// What the face buttons type into Claude: the remappable half of the controls.
//
// On the Control and KEYS screens, A, B, X, Y and START each send one key a
// keyboard would: Enter, Space, Delete, Esc, Tab, or Stop (Ctrl-C).  Settings'
// BUTTONS tab chooses which.  Everything else is fixed, on purpose:
//
//   SELECT     cycles the bottom screens -- the way to every other control
//   d-pad      arrows (and scrolling back on the Control screen)
//   L, R       one-shot Ctrl and Shift; together, focus mode
//   menus      every picker, the composer and focus mode keep their own
//              buttons, and X always leads back to the Control screen
//
// A control that can move is one you can lose.  The fixed ones are the ones
// that get you out of anything, so none of them can be mapped away; and Enter
// has to stay on some button, or there would be no way to accept a prompt.
//
// Pure C with no libnds dependency; host-tested.

#ifndef DSTERM_BUTTONS_H
#define DSTERM_BUTTONS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_X,
    BUTTON_Y,
    BUTTON_START,
    BUTTON_COUNT,
} button_t;

typedef enum {
    BUTTON_ENTER = 0,
    BUTTON_SPACE,
    BUTTON_DELETE,
    BUTTON_ESC,
    BUTTON_TAB,
    BUTTON_STOP,
    BUTTON_ACTION_COUNT,
} button_action_t;

typedef struct {
    uint8_t action[BUTTON_COUNT];   // button_action_t, one per button
} button_map_t;

// A Enter, B Delete, X Esc, Y Tab, START Stop.
void button_map_defaults(button_map_t *map);
int button_map_valid(const button_map_t *map);

// Give `button` the action `action`.  Refused (returns 0, map unchanged) if
// that would leave no button on Enter.
int button_map_set(button_map_t *map, button_t button, button_action_t action);

// The first button, in A B X Y START order, that sends `action`, or
// BUTTON_COUNT if none does.
button_t button_for(const button_map_t *map, button_action_t action);

const char *button_name(button_t button);               // "A", "START"
const char *button_action_label(button_action_t action); // "ENT", "STOP" (chips)
const char *button_action_word(button_action_t action);  // "enter", "stop" (file)
// The same word in the language on screen (lang.h), for hints and legends.
const char *button_action_hint(button_action_t action);
// The bytes the action sends, as a keyboard would.  Never NULL.
const char *button_action_bytes(button_action_t action, size_t *len);

// The Control screen's legend line for the face buttons, e.g.
// "A enter  B delete  X esc  Y tab", into `out` (at most `cap` - 1 chars).
void button_map_legend(const button_map_t *map, char *out, size_t cap);

// --------------------------------------------------------- the shutter ---
//
// The camera screen's shutter: one of R, L or A, chosen on Settings'
// BUTTONS tab.  R and L arm on the press edge and fire on release, and
// spoil -- silently -- if the other shoulder is ever held while armed, so
// the L+R chord can never take a picture.  A spoils on nothing.  This is
// remappable where everything else about the shoulders is not: the chord
// still means "leave", whatever takes the picture.
typedef enum {
    SHUTTER_R = 0,
    SHUTTER_L,
    SHUTTER_A,
    SHUTTER_COUNT,
} camera_shutter_t;

const char *camera_shutter_name(camera_shutter_t s);   // "R" (chips, hints)
const char *camera_shutter_word(camera_shutter_t s);   // "r" (settings file)
// The shutter's bit in the kbd key word, and the shoulder it spoils with
// (0 for A).  Out-of-range values answer like R.
unsigned camera_shutter_key_mask(camera_shutter_t s);
unsigned camera_shutter_other_mask(camera_shutter_t s);

#endif  // DSTERM_BUTTONS_H
