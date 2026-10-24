// QUICK: the Control screen's tile for the things you reach for mid-session,
// in one short list instead of five tiles.
//
//   MODE      Claude Code's mode -- the tile itself shows which one, read off
//             Claude's own footer -- and A moves to the next (Shift-Tab).
//   UNDO      Claude Code's rewind (/rewind): pick the point to go back to
//             with the d-pad, A to restore, X to leave it.
//   CHANGES   the PROJECT page's DIFF tab: what is different in the repo.
//   COMMANDS  slash commands, built in and the computer's own (the project's
//             and your .claude/commands and skills), into the composer.
//   AGAIN     the prompts you sent lately, into the composer to send again.
//   VOICE     the composer, with the microphone ready.
//   CONTROLS  the controls guide (guide.h): every button and combo, tried
//             on a drawing of this console without sending anything.
//   PHOTO     the camera screen -- a DSi-only row.  It is the last row, and
//             a card without a camera does not have it (quick_row_count).
//
// Everything that lands in the composer still needs A to send: COMMANDS can
// carry names the computer chose, and the composer is where they are read
// first (snippets.h has the rule).  MODE and UNDO type fixed keys the person
// just asked for, like the settings rows do.
//
// Pure C with no libnds dependency; host-tested.

#ifndef DSTERM_QUICK_H
#define DSTERM_QUICK_H

#include <stddef.h>
#include <stdint.h>

#include "lang.h"

typedef enum {
    QUICK_ROW_MODE = 0,
    QUICK_ROW_UNDO,
    QUICK_ROW_CHANGES,
    QUICK_ROW_COMMANDS,
    QUICK_ROW_AGAIN,
    QUICK_ROW_VOICE,
    QUICK_ROW_CONTROLS,
    QUICK_ROW_PHOTO,
    QUICK_ROW_COUNT,
} quick_row_t;

const char *quick_row_label(quick_row_t row);
// What the row does, in a few words, for the right of its line.
const char *quick_row_note(quick_row_t row);
// PHOTO's note is a fact of the connection, not the card: main sets it when
// CAMERA_READY lands (TR_COUNT restores "see it, send it").  A string id, so
// it is drawn in whatever language is on screen when the menu is.
void quick_set_photo_note(tr_id_t note);
// VOICE's note follows MIC_READY on the connection (TR_COUNT restores
// "speak into the composer").
void quick_set_voice_note(tr_id_t note);

// How many rows the menu has on this card: PHOTO is last, so it is simply
// one fewer on a card without a camera.
unsigned quick_row_count(int camera);

// ------------------------------------------------------------------ MODE ---

#define QUICK_MODE_MAX 10

// Claude Code's mode from one line of its screen, as one short word: "auto",
// "manual", "plan", "edits" (accept edits), "bypass", or "normal" where only
// the "? for shortcuts" hint stands in its place.  Returns 1 and fills
// `out` when the line is the footer that names it, 0 otherwise.  The words
// come from Claude Code's own text ("plan mode on", "accept edits on"), so a
// mode it adds later still shows, by its own name.
int quick_mode_from_line(const char *line, char out[QUICK_MODE_MAX + 1]);

// ------------------------------------------------------------- COMMANDS ---

#define QUICK_COMMANDS_MAX 24
#define QUICK_COMMAND_MAX 31          // "/" and up to 30 characters

typedef struct {
    unsigned count;
    char names[QUICK_COMMANDS_MAX][QUICK_COMMAND_MAX + 1];
} quick_commands_t;

// The built-in ones, which are always there, then the computer's.
void quick_commands_init(quick_commands_t *c);
// Replace the computer's part with a COMMANDS frame's payload: names
// separated by newlines, each a slash and then letters, digits and _ - : .
// A name that is not one is skipped; a duplicate of a built-in is dropped.
// Returns how many of the computer's were taken.
unsigned quick_commands_apply(quick_commands_t *c, const uint8_t *payload,
                              size_t len);
// How many of the list are built in (they come first).
unsigned quick_commands_builtin(void);

// ------------------------------------------------------- SAVED COMMANDS ---
//
// Custom quick commands saved on the card (/dsterm/quick.txt).  Shown in
// COMMANDS after the built-ins and above the computer's own, with a "+ ADD"
// row at the end of the list.  May hold any printable prompt text up to the
// composer's limit (not only slash commands), and choosing one loads it into
// the composer so it still takes A to send.
#define QUICK_SAVED_MAX 16
#define QUICK_SAVED_TEXT_MAX 1024

typedef struct {
    unsigned count;
    char items[QUICK_SAVED_MAX][QUICK_SAVED_TEXT_MAX + 1];
} quick_saved_t;

void quick_saved_init(quick_saved_t *s);
int  quick_saved_add(quick_saved_t *s, const char *text);
// The same, saying why not, and where the command is (`*index`, set on OK,
// including when it was already in the list).  A tab becomes a space -- the
// composer takes tabs from a snippet or the clipboard, and a saved command
// is shown on one line -- and nothing else outside printable ASCII is kept.
typedef enum {
    QUICK_ADD_OK = 0,
    QUICK_ADD_EMPTY,        // nothing but spaces
    QUICK_ADD_TOO_LONG,     // over QUICK_SAVED_TEXT_MAX
    QUICK_ADD_INVALID,      // a byte that is not printable ASCII, tab or newline
    QUICK_ADD_FULL,         // QUICK_SAVED_MAX already
} quick_add_result_t;
quick_add_result_t quick_saved_add_at(quick_saved_t *s, const char *text,
                                      unsigned *index);
int  quick_saved_remove(quick_saved_t *s, unsigned index);
size_t quick_saved_format(const quick_saved_t *s, char *out, size_t cap);
void quick_saved_parse(quick_saved_t *s, const char *text);

typedef enum {
    QUICK_CMD_NONE = 0,
    QUICK_CMD_BUILTIN,
    QUICK_CMD_SAVED,
    QUICK_CMD_SERVER,
    QUICK_CMD_ADD,
} quick_cmd_kind_t;

// Total rows in the COMMANDS menu: built-ins + saved + server's + "+ ADD".
unsigned quick_commands_total(const quick_commands_t *c,
                              const quick_saved_t *s);
// Classify row `index` in the COMMANDS menu and return its text (or "+ ADD").
// When the row is QUICK_CMD_SAVED, `*saved_idx` receives its index in `s`.
quick_cmd_kind_t quick_commands_entry(const quick_commands_t *c,
                                      const quick_saved_t *s,
                                      unsigned index,
                                      const char **text_out,
                                      unsigned *saved_idx);

// ---------------------------------------------------------------- AGAIN ---

#define QUICK_RECENT_MAX 10
#define QUICK_RECENT_TEXT_MAX 400     // a longer prompt is not kept

typedef struct {
    unsigned count;                   // newest first
    char items[QUICK_RECENT_MAX][QUICK_RECENT_TEXT_MAX + 1];
} quick_recent_t;

void quick_recent_init(quick_recent_t *r);
// Remember a prompt that was just sent.  Trailing whitespace is trimmed, an
// empty or overlong one is not kept, and one already in the list moves to the
// front rather than appearing twice.  Returns 1 if the list changed.
int quick_recent_add(quick_recent_t *r, const char *text);
// Forget one.  Returns 1 if there was one to forget.
int quick_recent_remove(quick_recent_t *r, unsigned index);

// The list as a file: one prompt per line, newest first, with a backslash
// escaping newlines (\n) and itself (\\).  Returns the length written, or 0
// if it does not fit `cap`.
size_t quick_recent_format(const quick_recent_t *r, char *out, size_t cap);
// Read it back.  Lines that do not decode are skipped.
void quick_recent_parse(quick_recent_t *r, const char *text);

// One line of the list, for a row: newlines shown as " / ", cut to `cols`.
void quick_recent_row(const char *text, char *out, size_t cols);

// Lines typed on the KEYS screen, so a prompt typed there can be sent again
// too.  Fed every byte that is sent; a carriage return completes the line.
// Anything that moves the cursor or edits in ways this cannot follow (arrows,
// Tab, Esc) gives the line up until the next return: AGAIN keeps what was
// typed, never a guess at it.
typedef struct {
    char buf[QUICK_RECENT_TEXT_MAX + 1];
    size_t len;
    int lost;
} quick_line_t;

void quick_line_reset(quick_line_t *l);
// Returns 1, with the line in `out`, when `bytes` completed one worth keeping.
int quick_line_feed(quick_line_t *l, const char *bytes, size_t n,
                    char *out, size_t cap);

#endif  // DSTERM_QUICK_H
