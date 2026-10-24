#include "project.h"

#include <stdio.h>
#include <string.h>

#include "privacy.h"

// The sub screen is 256 wide; six tabs divide it evenly with a two-pixel
// gutter at each end, which is the same inset the control tiles use.
#define TAB_STRIP_X 2
#define TAB_STRIP_W 252
#define TAB_W (TAB_STRIP_W / PROJECT_TAB_COUNT)

static const char *const TAB_NAMES[PROJECT_TAB_COUNT] = {
    "NOW", "DIFF", "DO", "WATCH", "HEALTH", "LOG",
};

void project_init(project_t *project)
{
    if (!project) return;
    memset(project, 0, sizeof(*project));
    project->tab = (uint8_t)PROJECT_TAB_NOW;
}

const char *project_tab_name(project_tab_t tab)
{
    if ((unsigned)tab >= PROJECT_TAB_COUNT) return "";
    return TAB_NAMES[tab];
}

void project_tab_step(project_t *project, int delta)
{
    if (!project || delta == 0) return;
    const int step = delta > 0 ? 1 : -1;
    int next = (int)project->tab + step;
    if (next < 0) next = PROJECT_TAB_COUNT - 1;
    if (next >= PROJECT_TAB_COUNT) next = 0;
    project->tab = (uint8_t)next;
}

void project_tab_set(project_t *project, project_tab_t tab)
{
    if (!project || (unsigned)tab >= PROJECT_TAB_COUNT) return;
    project->tab = (uint8_t)tab;
}

void project_clear(project_t *project, project_tab_t tab)
{
    if (!project || (unsigned)tab >= PROJECT_TAB_COUNT) return;
    project->page[tab].count = 0;
}

void project_add(project_t *project, project_tab_t tab, const char *label,
                 const char *value, uint8_t fg)
{
    if (!project || (unsigned)tab >= PROJECT_TAB_COUNT) return;
    project_page_t *page = &project->page[tab];
    if (page->count >= PROJECT_ROWS_MAX) return;
    project_row_t *row = &page->row[page->count++];
    (void)snprintf(row->label, sizeof(row->label), "%s", label ? label : "");
    (void)snprintf(row->value, sizeof(row->value), "%s", value ? value : "");
    row->fg = fg;
}

// One cell, bounded.  Everything below goes through here so a formatting bug
// can produce a wrong page but never a write outside the grid.
static void put(grid_t *grid, int row, int col, char ch, uint8_t fg)
{
    if (row < 0 || row >= PROTO_ROWS || col < 0 || col >= PROTO_COLS) return;
    cell_t *cell = &grid->cells[row * PROTO_COLS + col];
    cell->glyph = (uint8_t)ch;
    cell->attr = (uint8_t)(fg << 4);
    cell->flags = 0;
}

// Text from `col`, clipped at the right edge.  Returns the column after the
// last glyph written, so callers can lay a line out left to right without
// each of them re-deriving where the previous piece ended.
static int put_text(grid_t *grid, int row, int col, const char *text,
                    uint8_t fg)
{
    if (!text) return col;
    text = privacy_text(text);   // HIDE ADDRESSES (privacy.h)
    for (; *text && col < PROTO_COLS; text++, col++) {
        put(grid, row, col, *text, fg);
    }
    return col;
}

void project_render(const project_t *project, grid_t *grid)
{
    if (!project || !grid) return;
    const project_tab_t tab = project_tab(project);
    const project_page_t *page = &project->page[tab];

    // Blank first, every cell of it.  A page that paints only its own text
    // leaves the last page showing through in the gaps, which is exactly the
    // debris bug every picker on this device has to guard against -- and here
    // it would also leave an extension standing on a line that is no longer there.
    for (int i = 0; i < PROTO_CELLS; i++) {
        grid->cells[i].glyph = ' ';
        grid->cells[i].attr = 0;
        grid->cells[i].flags = 0;
    }

    // Row 0: which tab, and the ones either side of it, so the top screen says
    // where you are without needing the bottom one in view.
    int col = put_text(grid, 0, 0, project_tab_name(tab), 9 /* orange */);
    col = put_text(grid, 0, col + 1, "-- PROJECT", 0);
    (void)col;

    // Rows 2..: the page itself.  Labels are left-aligned in a fixed gutter so
    // the values line up down the screen; a value that would run off the edge
    // is clipped by put_text rather than wrapped, because a wrapped value on a
    // 42-column screen reads as two facts.
    const int value_col = PROJECT_LABEL_MAX + 2;
    if (tab == PROJECT_TAB_DO) {
        // Every button, the card's own and the server's, with the exact text
        // it types -- the promise the DO tab makes is that nothing is pressed
        // unread, and that needs the built-in ones listed too.
        project_action_t actions[PROJECT_ACTIONS_MAX];
        const unsigned n = project_actions(project, actions, PROJECT_ACTIONS_MAX);
        const unsigned sel = project_action_selected(project);
        for (unsigned i = 0; i < n; i++) {
            const int row = 2 + (int)i;
            if (row >= PROTO_ROWS - 2) break;
            put_text(grid, row, 0, i == sel ? ">" : " ", 9);
            put_text(grid, row, 2, actions[i].label, i == sel ? 9 : 0);
            put_text(grid, row, value_col, actions[i].body, 0);
        }
        put_text(grid, PROTO_ROWS - 1, 0, "! runs a shell command in Claude Code", 0);
        return;
    }
    for (unsigned i = 0; i < page->count && i < PROJECT_ROWS_MAX; i++) {
        const int row = 2 + (int)i;
        if (row >= PROTO_ROWS - 1) break;
        put_text(grid, row, 0, page->row[i].label, 0);
        put_text(grid, row, value_col, page->row[i].value, page->row[i].fg);
    }

    if (page->count == 0) {
        put_text(grid, 2, 0, "nothing reported yet", 0);
    }
}

static void stage_add(project_page_t *page, const char *label,
                      const char *value, uint8_t fg)
{
    if (page->count >= PROJECT_ROWS_MAX) return;
    project_row_t *row = &page->row[page->count++];
    (void)snprintf(row->label, sizeof(row->label), "%s", label ? label : "");
    (void)snprintf(row->value, sizeof(row->value), "%s", value ? value : "");
    row->fg = fg;
}

int project_wire_row(project_t *project, unsigned count, unsigned index,
                     unsigned tab, unsigned fg, const char *label,
                     const char *value)
{
    if (!project) return 0;

    // Nothing to report is a complete answer.  It clears whatever the server
    // last said without pretending the client knows better.
    if (count == 0) {
        for (int i = 0; i < PROJECT_TAB_COUNT; i++) project->page[i].count = 0;
        project->server_tabs = 0;
        project->stage_expect = 0;
        project->stage_seen = 0;
        project->stage_tabs = 0;
        return 1;
    }

    if (count > PROTO_PROJECT_ROWS_MAX || tab >= PROJECT_TAB_COUNT) return 0;

    // Rows must arrive in order.  Anything else is a snapshot that overlapped
    // another one or lost a frame, and half a snapshot is worse than none.
    if (index == 0) {
        for (int i = 0; i < PROJECT_TAB_COUNT; i++) project->stage[i].count = 0;
        project->stage_expect = (uint8_t)count;
        project->stage_seen = 0;
        project->stage_tabs = 0;
    } else if (project->stage_expect != count ||
               project->stage_seen != index) {
        project->stage_expect = 0;
        project->stage_seen = 0;
        project->stage_tabs = 0;
        return 0;
    }

    stage_add(&project->stage[tab], label, value, (uint8_t)fg);
    project->stage_tabs = (uint8_t)(project->stage_tabs | (1u << tab));
    project->stage_seen++;
    if (project->stage_seen < project->stage_expect) return 0;

    // Complete.  Only the tabs this snapshot spoke about change hands; a tab
    // the server said nothing about keeps whatever it had, which for an
    // unclaimed one is the local content.
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        if (!(project->stage_tabs & (1u << i))) continue;
        project->page[i] = project->stage[i];
    }
    project->server_tabs = (uint8_t)(project->server_tabs |
                                     project->stage_tabs);
    project->stage_expect = 0;
    project->stage_seen = 0;
    project->stage_tabs = 0;
    return 1;
}

void project_wire_reset(project_t *project)
{
    if (!project) return;
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        if (project->server_tabs & (1u << i)) project->page[i].count = 0;
    }
    project->server_tabs = 0;
    project->stage_expect = 0;
    project->stage_seen = 0;
    project->stage_tabs = 0;
}

int project_tab_from_server(const project_t *project, project_tab_t tab)
{
    if (!project || (unsigned)tab >= PROJECT_TAB_COUNT) return 0;
    return (project->server_tabs & (1u << tab)) != 0;
}

// ------------------------------------------------------------- the DO tab ---

// The client's own, compiled in and always available.  Chosen for what a
// person actually taps on a handheld sitting beside a running agent: what has
// it done, and what is here.  None of them writes anything, none of them
// takes an argument, and none of them can be changed from the wire.
//
// Each starts with '!', which is Claude Code's "run this as a shell command"
// prefix: what is on screen is Claude's prompt, not a shell, and a bare
// `git status` typed there is a message to Claude rather than a command.
// (There was a `clear` button.  Typed at Claude it asked Claude to clear;
// /clear would have wiped the conversation.  Neither is a one-tap action.)
static const project_action_t GENERIC[] = {
    { "status", "!git status --short",      0 },
    { "diff",   "!git diff --stat",         0 },
    { "log",    "!git log --oneline -10",   0 },
    { "ls",     "!ls -la",                  0 },
};
#define GENERIC_COUNT ((unsigned)(sizeof(GENERIC) / sizeof(GENERIC[0])))

static unsigned server_action_count(const project_t *project)
{
    return project->page[PROJECT_TAB_DO].count;
}

unsigned project_actions(const project_t *project, project_action_t *out,
                         unsigned max)
{
    if (!project || !out || max == 0) return 0;
    unsigned n = 0;

    // The server's first: those are the ones that know what you are working
    // on, and they are the reason to open this tab at all.  Only if it has
    // actually claimed the tab -- the placeholder row the client writes when
    // nothing is reporting says "this server does not report it", and typing
    // that would be a joke at somebody's expense.
    if (project_tab_from_server(project, PROJECT_TAB_DO)) {
        const project_page_t *page = &project->page[PROJECT_TAB_DO];
        for (unsigned i = 0; i < page->count && n < max; i++) {
            (void)snprintf(out[n].label, sizeof(out[n].label), "%s",
                           page->row[i].label);
            (void)snprintf(out[n].body, sizeof(out[n].body), "%s",
                           page->row[i].value);
            out[n].from_server = 1;
            n++;
        }
    }
    for (unsigned i = 0; i < GENERIC_COUNT && n < max; i++) out[n++] = GENERIC[i];
    return n;
}

unsigned project_actions_dropped(const project_t *project)
{
    if (!project) return 0;
    const unsigned server = project_tab_from_server(project, PROJECT_TAB_DO) ?
        server_action_count(project) : 0u;
    const unsigned wanted = server + GENERIC_COUNT;
    return wanted > PROJECT_ACTIONS_MAX ? wanted - PROJECT_ACTIONS_MAX : 0u;
}

int project_action_keyable(const char *body)
{
    if (!body || !body[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)body; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) return 0;
    }
    return 1;
}

void project_action_select(project_t *project, int delta)
{
    if (!project || delta == 0) return;
    project_action_t actions[PROJECT_ACTIONS_MAX];
    const unsigned n = project_actions(project, actions, PROJECT_ACTIONS_MAX);
    if (n == 0) return;
    const int step = delta > 0 ? 1 : -1;
    int next = (int)project->action_sel + step;
    if (next < 0) next = (int)n - 1;
    if (next >= (int)n) next = 0;
    project->action_sel = (uint8_t)next;
}

#define ACTION_COLS 2u
#define ACTION_X 4
#define ACTION_W 122
#define ACTION_GAP 4

int project_action_rect(unsigned index, int *x, int *y, int *w, int *h)
{
    if (index >= PROJECT_ACTIONS_MAX) return 0;
    const unsigned col = index % ACTION_COLS;
    const unsigned row = index / ACTION_COLS;
    if (x) *x = ACTION_X + (int)col * (ACTION_W + ACTION_GAP);
    if (y) *y = PROJECT_ACTION_Y + (int)row * PROJECT_ACTION_PITCH;
    if (w) *w = ACTION_W;
    if (h) *h = PROJECT_ACTION_H;
    return 1;
}

unsigned project_action_hit(int x, int y)
{
    for (unsigned i = 0; i < PROJECT_ACTIONS_MAX; i++) {
        int rx, ry, rw, rh;
        (void)project_action_rect(i, &rx, &ry, &rw, &rh);
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) return i;
    }
    return PROJECT_ACTIONS_MAX;
}

int project_tab_rect(project_tab_t tab, int *x, int *y, int *w, int *h)
{
    if ((unsigned)tab >= PROJECT_TAB_COUNT) return 0;
    if (x) *x = TAB_STRIP_X + (int)tab * TAB_W;
    if (y) *y = PROJECT_TAB_STRIP_Y;
    if (w) *w = TAB_W;
    if (h) *h = PROJECT_TAB_STRIP_H;
    return 1;
}

project_tab_t project_tab_hit(int x, int y)
{
    for (int i = 0; i < PROJECT_TAB_COUNT; i++) {
        int rx = 0, ry = 0, rw = 0, rh = 0;
        (void)project_tab_rect((project_tab_t)i, &rx, &ry, &rw, &rh);
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh) {
            return (project_tab_t)i;
        }
    }
    return PROJECT_TAB_COUNT;
}
