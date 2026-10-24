#include "chime.h"

#include <stdio.h>

#include <stdint.h>

#include "ext.h"
#include "lang.h"
#include "log.h"
#include "sound.h"

typedef enum {
    CHIME_IDLE,
    CHIME_SENT,
    CHIME_WAITING,
    CHIME_STREAMING,
} chime_state_t;

static chime_state_t state;
static unsigned frame;              // frames since init
static unsigned cells_frame;        // frame of the last chime_cells
static int cell_frames;             // cell messages accumulated while WAITING
static unsigned suppress_until;     // frames: resync traffic is ours
static int history;                 // the view is scrolled back into history

// Posture facts.  These are separate from the cue state so a quiet UI
// never pretends it knows more than traffic actually told it.
static int control_visible;
static unsigned last_activity;
static int settled;
static int lantern_active;
static unsigned lantern_frame;
static unsigned familiar_until;

// The server's reading of the session on screen (PROTO_STATE_*; 0 is "nobody
// is reporting"), and the bookkeeping that keeps each cue to once per turn.
static unsigned claude_state;
static int think_played;        // THINK has sounded for this turn
static unsigned waiting_since;  // frame the current wait began
static int reminded;            // that wait has had its one reminder
// After a switch to another window the next reading describes a different
// session; its edge is news about where you went, not something happening,
// and must not chime.  Readings in this window update silently.
static unsigned state_quiet_until;
#define CHIME_SWITCH_QUIET 180      // 3 s: a poll or two after the switch

// Blips: when the next may sound, and when this answer began, on the
// chime's own clock.  The random gap is a small LCG -- deterministic for the
// host tests, irregular enough by ear.
static unsigned blip_next;
static unsigned answer_began;
static uint32_t blip_seed = 0x1234567u;

static unsigned blip_gap(void)
{
    blip_seed = blip_seed * 1103515245u + 12345u;
    unsigned gap = CHIME_BLIP_GAP_MIN +
                   (blip_seed >> 16) % (CHIME_BLIP_GAP_MAX - CHIME_BLIP_GAP_MIN + 1u);
    // Settings' WRITING: how much.  LOW is half as often, HIGH two-thirds
    // again as often and slower to tire.
    const int level = sound_blips();
    if (level == 1) gap *= 2u;
    else if (level >= 3) gap = gap * 3u / 5u;
    const unsigned ran = frame - answer_began;
    if (level >= 3) {
        if (ran >= CHIME_BLIP_WEARY) gap = gap * 9u / 5u;
        else if (ran >= CHIME_BLIP_TIRED) gap = gap * 5u / 4u;
    } else {
        if (ran >= CHIME_BLIP_WEARY) gap *= 3u;
        else if (ran >= CHIME_BLIP_TIRED) gap = gap * 3u / 2u;
    }
    // While Claude is only thinking (the spinner), sparser than while it
    // writes: the texture is for the words arriving.
    if (state != CHIME_STREAMING) gap = gap * 3u / 2u;
    return gap;
}

// A moment fit for texture.  `silent` is the timer-driven fallback, which
// speaks half as often: it has no repaint to keep it in step with anything.
static void blip_moment(unsigned char glyph, int silent)
{
    if (!sound_blips() || frame < blip_next) return;
    if (sound_blip(glyph)) {
        unsigned gap = blip_gap();
        if (silent) gap *= 2u;
        blip_next = frame + gap;
    }
}

static void observe_activity(void)
{
    last_activity = frame;
}

static void show_familiar(void)
{
    familiar_until = frame + CHIME_FAMILIAR_FRAMES;
}

// Characters typed on the session's input line since the last Enter (or
// Ctrl-C), as far as this card's own keys can tell. Saturates; a Backspace
// takes one away. A lone Escape and most escape sequences are not text --
// but Up and Down recall history into the line, so after one the line is
// treated as holding something (it usually does; an empty recall costs one
// SENT cue). LF is not a reset either: in Claude Code it is a newline
// inside the prompt, not a submit, and a composer body may end in one.
static unsigned line_typed;

void chime_init(void)
{
    state = CHIME_IDLE;
    frame = 0;
    cells_frame = 0;
    cell_frames = 0;
    suppress_until = 0;
    history = 0;
    control_visible = 0;
    last_activity = 0;
    settled = 0;
    lantern_active = 0;
    lantern_frame = 0;
    familiar_until = 0;
    blip_next = 0;
    answer_began = 0;
    claude_state = 0;
    think_played = 0;
    waiting_since = 0;
    reminded = 0;
    state_quiet_until = 0;
    line_typed = 0;
}

int chime_typed(const char *bytes, size_t n)
{
    if (!bytes || n == 0) return 0;
    if (n == 1 && bytes[0] == '\r') {
        const int submits = line_typed > 0 || chime_needs_you();
        line_typed = 0;
        return submits;
    }
    if ((unsigned char)bytes[0] == 0x1b) {
        // ESC [ A / ESC [ B, or the application-mode ESC O A / ESC O B.
        if (n == 3 && (bytes[1] == '[' || bytes[1] == 'O') &&
            (bytes[2] == 'A' || bytes[2] == 'B') && line_typed == 0) {
            line_typed = 1;
        }
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char b = (unsigned char)bytes[i];
        if (b == 0x7f || b == 0x08) {
            if (line_typed) line_typed--;
        } else if (b == 0x03 || b == '\r') {
            line_typed = 0;
        } else if (b >= 0x20 && line_typed < 0xFFFFu) {
            line_typed++;
        }
    }
    return 0;
}

void chime_sent(void)
{
    observe_activity();
    lantern_active = 0;
    settled = 0;
    state = CHIME_SENT;
    cells_frame = frame;
    cell_frames = 0;
    answer_began = frame;
    think_played = 0;
    // The first blip waits for the SENT cue and THINK's pause to pass.
    blip_next = frame + CHIME_THINK_QUIET;
    show_familiar();
    sound_cue(SOUND_SENT);
    ext_cue(EXT_CUE_SENT);
}

void chime_view_sent(void)
{
    // A VIEW is either a real reader action or a reconnect resync.  In both
    // cases the subsequent cells are client-requested and cannot honestly be
    // treated as session output; either way, they are not "true quiet".
    observe_activity();
    // Log both first arms and re-arms.  The STATUS screen's short log is the
    // only useful hardware evidence when a reconnect/session-view loop keeps
    // this guard live long enough to suppress all of the response arc.
    const int rearmed = frame < suppress_until;
    suppress_until = frame + CHIME_SUPPRESS_QUIET;
    log_printf("chime guard %s %u", rearmed ? "rearm" : "arm",
               CHIME_SUPPRESS_QUIET);
}

void chime_history(int scrolled_back)
{
    observe_activity();
    if (scrolled_back) {
        history = 1;
    } else {
        // The redraws while reading were ours.  Restart the quiet clock when
        // returning to live, or DONE would fire straight after a long read.
        if (history) cells_frame = frame;
        history = 0;
    }
}

void chime_cells(unsigned char glyph)
{
    // The glyph only pitches a blip; nothing here reads terminal text, and
    // nothing may.
    // Every received screen update is traffic, even if it is later classified
    // as our own resync.  It resets the visual quiet check-in, but only the
    // guarded path below may make a sound or change response state.
    observe_activity();
    settled = 0;
    cells_frame = frame;
    if (frame < suppress_until || history) return;

    // Every state in which work is visibly under way.  WAITING belongs here
    // precisely because it is the thinking state: the spinner's repaints are
    // the anchor that keeps any texture in step with the thing on screen that
    // represents work happening.  Only a moment is offered; what fills it,
    // and how often, is the extension's.
    if (state == CHIME_SENT || state == CHIME_WAITING ||
        state == CHIME_STREAMING) {
        ext_quiet_moment(state == CHIME_STREAMING, 0);
        blip_moment(glyph, 0);
    }

    if (state == CHIME_SENT || state == CHIME_WAITING) {
        // A lone redraw can be a cursor.  A handful of cell messages after a
        // send (with or without a quiet gap) is the only evidence the client
        // has for output.  Waiting only for a quiet gap missed the common
        // fast-answer case, which meant its eventual settled phrase never
        // arrived at all.
        cell_frames++;
        if (cell_frames >= CHIME_MIN_CELLS) {
            cell_frames = 0;
            state = CHIME_STREAMING;
            show_familiar();
            sound_cue(SOUND_OUTPUT);
            ext_cue(EXT_CUE_OUTPUT);
        }
    }
}

void chime_tick(void)
{
    frame++;
    if (frame < suppress_until || history) return;

    // WAITING is the real thinking state: the prompt is gone, THINK has already
    // sounded, and no output has arrived yet.
    //
    // This path is deliberately the *fallback*, not the main cadence.  While
    // the model works, Claude Code animates its own spinner, and every tick of
    // it arrives as a cell update -- so a moment offered from chime_cells
    // lands in step with the thing on screen that represents thinking.  That
    // is the texture worth having.  A timer-driven one has no such anchor,
    // and running it at the same spacing is what turned a long think into a
    // metronome.  So this waits for genuine silence -- nothing repainting at
    // all -- and says so, which the extension takes as "half as often".
    //
    // Still only WAITING.  The pause between SEND and THINK, and the one
    // between the last cell and DONE, are composed silences; speaking into
    // either of them would step on a cue rather than accompany it.
    if (state == CHIME_WAITING && frame - cells_frame >= CHIME_MURMUR_SILENCE) {
        ext_quiet_moment(0, 1);
        blip_moment((unsigned char)(frame & 0x7F), 1);
    }

    if ((claude_state == 3 || claude_state == 4) && !reminded &&
        frame - waiting_since >= CHIME_REMIND_FRAMES &&
        frame - last_activity >= CHIME_REMIND_FRAMES) {
        reminded = 1;
        sound_cue(SOUND_LANTERN);
    }

    switch (state) {
    case CHIME_SENT:
        // Quiet after a send is an observed pause, not proof of thinking.  The
        // visual posture calls it "leaning in"; the legacy cue name remains
        // for API compatibility and musical identity.
        if (frame - cells_frame >= CHIME_THINK_QUIET) {
            state = CHIME_WAITING;
            cell_frames = 0;
            if (!think_played) {
                think_played = 1;
                sound_cue(SOUND_THINK);
            }
            ext_cue(EXT_CUE_THINK);
        }
        break;

    case CHIME_STREAMING:
        if (frame - cells_frame >= CHIME_DONE_QUIET) {
            state = CHIME_IDLE;
            settled = 1;
            show_familiar();
            // With a current server reading, "the screen went quiet" is not
            // the same as "Claude finished" -- a long tool run is quiet too --
            // so DONE waits for the real edge (chime_claude_state).  Without
            // one (an older server, or a window it cannot read, which it
            // reports as 0), the quiet is all there is to go on.  DONE's own
            // cooldown keeps the two from doubling up at a handover.
            if (claude_state == 0) sound_cue(SOUND_DONE);
            ext_cue(EXT_CUE_DONE);
        }
        break;

    default:
        break;   // IDLE and WAITING are deliberately quiet states
    }
}

void chime_control_visible(int visible)
{
    const int next = visible ? 1 : 0;
    if (next && !control_visible) {
        // The check-in belongs to time visibly spent on Control, not quiet
        // accumulated while the user was reading a keyboard or list screen.
        observe_activity();
    }
    control_visible = next;
}

void chime_user_activity(void)
{
    observe_activity();
}

void chime_focus(void)
{
    chime_user_activity();
    sound_cue(SOUND_FOCUS);
}

void chime_session_switch(void)
{
    chime_user_activity();
    state_quiet_until = frame + CHIME_SWITCH_QUIET;
    sound_cue(SOUND_SWITCH);
}

void chime_switch(void)
{
    chime_session_switch();
}

void chime_session_lantern(void)
{
    // Session code is responsible for detecting the false->true flag edge.
    // This small guard-free entry point is useful because the sound conductor
    // independently enforces its lantern cooldown.
    observe_activity();
    settled = 0;
    lantern_active = 1;
    lantern_frame = frame;
    show_familiar();
    sound_cue(SOUND_LANTERN);
}

void chime_link_changed(int connected)
{
    observe_activity();
    lantern_active = 0;
    settled = 0;
    state = CHIME_IDLE;
    cell_frames = 0;
    show_familiar();
    sound_cue(connected ? SOUND_LINK : SOUND_DROP);
    ext_cue(connected ? EXT_CUE_LINK : EXT_CUE_DROP);
}

unsigned chime_suppress_remaining(void)
{
    return frame < suppress_until ? suppress_until - frame : 0;
}

unsigned chime_frame(void)
{
    return frame;
}

chime_posture_t chime_posture(void)
{
    if (lantern_active && frame - lantern_frame < CHIME_LANTERN_FRAMES) {
        return CHIME_POSTURE_LANTERN;
    }
    if (state == CHIME_SENT || state == CHIME_WAITING) {
        return CHIME_POSTURE_LEANING_IN;
    }
    if (state == CHIME_STREAMING) return CHIME_POSTURE_ANSWERING;
    if (control_visible && state == CHIME_IDLE &&
        frame - last_activity >= CHIME_STILL_HERE_QUIET) {
        return CHIME_POSTURE_STILL_HERE;
    }
    return settled ? CHIME_POSTURE_SETTLED : CHIME_POSTURE_HOLDING_LINE;
}

const char *chime_posture_text(void)
{
    switch (chime_posture()) {
    case CHIME_POSTURE_LEANING_IN:   return "leaning in";
    case CHIME_POSTURE_ANSWERING:    return "answering";
    case CHIME_POSTURE_SETTLED:      return "settled";
    case CHIME_POSTURE_LANTERN:      return "another lantern woke";
    case CHIME_POSTURE_STILL_HERE:   return "still here";
    case CHIME_POSTURE_HOLDING_LINE:
    default:                       return "holding the line";
    }
}

int chime_familiar(void)
{
    return sound_profile() == SOUND_PROFILE_COMPANION &&
           frame < familiar_until;
}

// ------------------------------------------------- what Claude is doing ---

void chime_claude_state(unsigned next)
{
    if (next > 4) next = 0;
    const unsigned prev = claude_state;
    claude_state = next;
    if (next == prev) return;
    observe_activity();
    if (frame < state_quiet_until) {
        // Learn the new session's state without announcing it.
        if (next == 3 || next == 4) { waiting_since = frame; reminded = 0; }
        if (next == 1) think_played = 0;
        settled = next == 1;
        return;
    }

    switch (next) {
    case 2:   // working
        if (prev == 1 || prev == 0) {
            // A turn began -- typed here, typed elsewhere, or Claude woke on
            // its own -- and THINK marks it once.
            if (!think_played) {
                think_played = 1;
                sound_cue(SOUND_THINK);
            }
            settled = 0;
        }
        // Back to work after an approval: the press already answered it.
        break;
    case 3:   // needs approval
        waiting_since = frame;
        reminded = 0;
        settled = 0;
        show_familiar();
        sound_cue(SOUND_APPROVE);
        break;
    case 4:   // needs an answer
        waiting_since = frame;
        reminded = 0;
        settled = 0;
        show_familiar();
        sound_cue(SOUND_QUESTION);
        break;
    case 1:   // idle: the turn is over
        if (prev == 2 || prev == 3 || prev == 4) {
            settled = 1;
            state = CHIME_IDLE;
            show_familiar();
            sound_cue(SOUND_DONE);
        }
        think_played = 0;
        break;
    default:
        break;
    }
}

unsigned chime_claude(void)
{
    return claude_state;
}

void chime_claude_reset(void)
{
    // A new connection's input line is not one this card typed on.
    line_typed = 0;
    claude_state = 0;
    reminded = 0;
}

int chime_needs_you(void)
{
    return claude_state == 3 || claude_state == 4;
}

// The approval headline, from the buttons that answer it (Settings'
// BUTTONS tab).  Built when they change, and again only when the language it
// was built in is no longer the one on screen -- so the headline is a plain
// read, and the line is never rebuilt from its own buffers: the button names
// are kept in buffers of their own, and snprintf never reads the string it is
// writing (overlapping source and destination is undefined behaviour).
// Room for a translation in three-byte CJK characters, not just English.
static char answer_line[128] = "Claude needs your OK:  A yes   X no";
static char answer_yes[16] = "A";
static char answer_no[16] = "X";
static unsigned answer_lang = LANG_EN;

static void answer_line_build(void)
{
    answer_lang = lang_current();
    if (answer_no[0]) {
        snprintf(answer_line, sizeof(answer_line),
                 "%s:  %s %s   %s %s", tr(TR_CHIME_APPROVAL),
                 answer_yes, tr(TR_CHIME_YES), answer_no, tr(TR_CHIME_NO));
    } else {
        snprintf(answer_line, sizeof(answer_line),
                 "%s:  %s %s", tr(TR_CHIME_APPROVAL), answer_yes,
                 tr(TR_CHIME_YES));
    }
}

void chime_set_answer_buttons(const char *yes, const char *no)
{
    if (!yes || !yes[0]) yes = "A";
    if (!no) no = "";
    // Callers pass button_name()'s literals; a caller passing these very
    // buffers back would be a no-op, so it is one rather than an overlap.
    if (yes != answer_yes) snprintf(answer_yes, sizeof(answer_yes), "%s", yes);
    if (no != answer_no) snprintf(answer_no, sizeof(answer_no), "%s", no);
    answer_line_build();
}

// One path for every language.  Each state's line is a translation key whose
// English is the line this headline has always shown, so English is the same
// bytes it was, and a language that has no string for a state -- a custom
// pack that skipped it -- falls back to that English line rather than to a
// different state's wording.
const char *chime_headline(void)
{
    switch (claude_state) {
    case 3:
        if (answer_lang != lang_current()) answer_line_build();
        return answer_line;
    case 4: return tr(TR_CHIME_QUESTION);
    case 2: return tr(TR_CHIME_WORKING);
    default: break;
    }
    if (lantern_active && frame - lantern_frame < CHIME_LANTERN_FRAMES) {
        return tr(TR_CHIME_LANTERN);
    }
    if (state == CHIME_SENT || state == CHIME_WAITING) return tr(TR_CHIME_SENT);
    if (state == CHIME_STREAMING) return tr(TR_CHIME_ANSWERING);
    if (claude_state == 1) return settled ? tr(TR_CHIME_DONE) : tr(TR_CHIME_IDLE);
    return settled ? tr(TR_CHIME_ANSWERED) : tr(TR_CHIME_IDLE);
}
