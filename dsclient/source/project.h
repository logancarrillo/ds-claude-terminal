// The PROJECT page: what is going on, on the other end.
//
// A terminal on a handheld is a novelty until it can answer the questions you
// actually pick the device up to ask -- which branch am I on, what has the
// agent done to my repo, is it stuck, is it waiting for me.  This is that
// page.  It is a fifth lower-screen mode reached with SELECT, and it uses both
// screens: the content goes on the top one because that is where there is room
// to read, and the tabs and buttons go on the bottom one because that is the
// only screen a Nintendo DS can be touched on.
//
// The page is rendered into an ordinary `grid_t` and drawn with the ordinary
// cell renderer.  That is not an implementation detail, it is the design: it
// means the page is made of the same glyphs as everything else so it looks
// like the rest of the device, and it means an extension's safe-rect search, the
// never-cover-text rule, and the whole roaming machinery work on it unchanged.
// To every other subsystem the page is just another terminal.
//
// Pure C with no libnds dependency, so the tab machine, every line's
// formatting, and the layout are host-tested.  The same rule status.c is
// written under applies here and for the same reason: a line that overruns its
// columns corrupts the screen it was added to explain.

#ifndef DSTERM_PROJECT_H
#define DSTERM_PROJECT_H

#include <stdint.h>

#include "proto.h"

typedef enum {
    // The one you glance at.  Where you are, what it is doing, and whether it
    // is waiting for you -- which is the single most useful thing a second
    // screen can tell someone running an agent.
    PROJECT_TAB_NOW = 0,
    // What it has done to the repo since you last looked.
    PROJECT_TAB_DIFF,
    // The one-tap actions.
    PROJECT_TAB_DO,
    // A live tail of one pinned thing, so the terminal can be on something
    // else.
    PROJECT_TAB_WATCH,
    // Is the machine on the other end alright.
    PROJECT_TAB_HEALTH,
    // The session so far: what happened, and how long it has taken.
    PROJECT_TAB_LOG,
    PROJECT_TAB_COUNT,
} project_tab_t;

// A page is a list of finished lines.  Whoever fills them in owns every
// decision about wording and units; this module owns only where they go, so
// that the display half cannot drift from the data half.
#define PROJECT_ROWS_MAX 18
#define PROJECT_LABEL_MAX 12
#define PROJECT_VALUE_MAX 40

typedef struct {
    char label[PROJECT_LABEL_MAX + 1];
    char value[PROJECT_VALUE_MAX + 1];
    // Palette index for the value, so a branch can be orange and a warning
    // red without this module knowing what either means.
    uint8_t fg;
} project_row_t;

typedef struct {
    project_row_t row[PROJECT_ROWS_MAX];
    uint8_t count;
} project_page_t;

typedef struct {
    project_page_t page[PROJECT_TAB_COUNT];
    uint8_t tab;

    // A snapshot in flight, and which tabs the server has claimed.
    //
    // Rows arrive one per frame because a whole page does not fit in one
    // message, so the same rule MSG_SESSIONS follows applies here: stage the
    // lot, commit only when it is complete, and never render half an update.
    // A tab the server has sent rows for belongs to the server from then on;
    // the ones it has not are filled from what this client already knows.
    // Both halves are honest and the reader can tell which is which, because
    // an unclaimed tab says which side is not reporting.
    project_page_t stage[PROJECT_TAB_COUNT];
    uint8_t stage_expect;
    uint8_t stage_seen;
    uint8_t stage_tabs;          // bitmask of tabs in the snapshot in flight
    uint8_t server_tabs;         // bitmask of tabs the server owns
    uint8_t action_sel;          // highlighted DO button, for the d-pad
} project_t;

void project_init(project_t *project);

// Short enough for a tab tile at any geometry: six columns of six pixels.
const char *project_tab_name(project_tab_t tab);

// Step the selected tab, wrapping.  `delta` is any integer; only its sign
// matters, and zero is a no-op.
void project_tab_step(project_t *project, int delta);
void project_tab_set(project_t *project, project_tab_t tab);
static inline project_tab_t project_tab(const project_t *project)
{
    return (project_tab_t)project->tab;
}

// Replace a page's contents.  Clear, then append: a page that is rebuilt every
// second must not be able to leave a stale line behind, and appending to a
// cleared page is the only shape where that cannot happen.
void project_clear(project_t *project, project_tab_t tab);
void project_add(project_t *project, project_tab_t tab, const char *label,
                 const char *value, uint8_t fg);

// Render the selected tab into `grid`, which the caller then draws with the
// ordinary cell renderer.  Every cell is written -- including the blank ones --
// because a page that only paints its own text leaves the previous page's
// showing through, which is the debris bug every picker here guards against.
void project_render(const project_t *project, grid_t *grid);

// Feed one decoded MSG_PROJECT row.  Returns 1 on the row that completes a
// snapshot, at which point the staged pages have been committed and the page
// is worth redrawing.  An out-of-order or over-long snapshot is discarded
// rather than partly applied.
//
// `count` of zero is the server saying it has nothing to report, which is a
// complete answer and commits immediately.
int project_wire_row(project_t *project, unsigned count, unsigned index,
                     unsigned tab, unsigned fg, const char *label,
                     const char *value);

// The link went away.  Give every tab back to the local facts: a branch name
// from a server that is no longer talking is the kind of stale truth that is
// worse than an admission of ignorance.
void project_wire_reset(project_t *project);

// Does this tab's content come from the server?  Callers that compose local
// content use this to leave the server's tabs alone.
int project_tab_from_server(const project_t *project, project_tab_t tab);

// ------------------------------------------------------------- the DO tab ---
//
// An action is a label and the text it types.  Nothing more: there is no
// message in this protocol that names a command, and there is not one here
// either -- pressing a button types into the PTY exactly the way a person
// would, which is why the value the server sent is shown on the page beside
// the label.  You can read what a button will type before you press it, and
// that is deliberate rather than incidental.
//
// Rows the server sends for the DO tab *are* the actions: their value is the
// command.  The client's own four are compiled in and always present.
#define PROJECT_ACTIONS_MAX 8

typedef struct {
    char label[PROJECT_LABEL_MAX + 1];
    char body[PROJECT_VALUE_MAX + 1];
    uint8_t from_server;
} project_action_t;

// The merged list: the server's contextual ones first, because those are what
// you came to the page for, then the client's generics filling what is left.
// Returns how many were written.
unsigned project_actions(const project_t *project, project_action_t *out,
                         unsigned max);

// How many the merge had to leave out.  Said on the page rather than dropped
// quietly: a list that silently truncates reads as a complete list.
unsigned project_actions_dropped(const project_t *project);

// Whether an action's body may become keystrokes: non-empty and nothing but
// printable ASCII (0x20..0x7E).  The PROJECT decoder keeps 0x80..0xBF for the
// page's glyphs -- blocks, sparklines -- because a row is read before it is
// pressed; but those same bytes on the PTY are C1 controls (0x9B is a
// one-byte CSI), so anything turning a row into keys checks this first and
// refuses the whole action rather than sending or quietly dropping a byte.
int project_action_keyable(const char *body);

// The selected action, for the d-pad; touch does not need it but a stylus is
// not always to hand.
void project_action_select(project_t *project, int delta);
static inline unsigned project_action_selected(const project_t *project)
{
    return project->action_sel;
}

// Two columns of four, ending above y=160 -- rows 20 to 23 of the lower screen
// are kept clear for an extension and a button there is a button it stands on.
#define PROJECT_ACTION_Y 44
#define PROJECT_ACTION_H 26
#define PROJECT_ACTION_PITCH 28
int project_action_rect(unsigned index, int *x, int *y, int *w, int *h);
unsigned project_action_hit(int x, int y);

// The lower screen's tab strip, in sub-screen pixels.  The drawing code and
// the touch hit-test both come from here so the two cannot drift: a button
// that is not where it looks is worse than no button.
#define PROJECT_TAB_STRIP_Y 18
#define PROJECT_TAB_STRIP_H 22
int project_tab_rect(project_tab_t tab, int *x, int *y, int *w, int *h);

// Which tab a touch landed on, or PROJECT_TAB_COUNT for none.
project_tab_t project_tab_hit(int x, int y);

#endif  // DSTERM_PROJECT_H
