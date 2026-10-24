#ifndef DSTERM_CHIME_H
#define DSTERM_CHIME_H

#include <stddef.h>

// The chime is a reading of observable client facts, not a claim about an
// unseen model.  It knows that a line left this device, cells changed, a
// session announced new output, or the link changed.  It does not know who is
// waiting for input, what a terminal screen means, or what is being thought.

// Its posture: the one-word summary of that reading, for anything that wants
// to show or react to it.
typedef enum {
    CHIME_POSTURE_HOLDING_LINE = 0, // connected quiet; no invented inner state
    CHIME_POSTURE_LEANING_IN,       // a line left this device and is still quiet
    CHIME_POSTURE_ANSWERING,        // enough post-quiet cells are changing
    CHIME_POSTURE_SETTLED,          // observed output has been quiet long enough
    CHIME_POSTURE_LANTERN,          // another session reported new output
    CHIME_POSTURE_STILL_HERE,       // true quiet while Control stayed visible
} chime_posture_t;

// Thresholds, in frames at 60 fps.
#define CHIME_THINK_QUIET 45       // 0.75s of quiet after send -> THINK
#define CHIME_DONE_QUIET 150       // 2.5s of quiet after output -> DONE
#define CHIME_MURMUR_GAP 45        // sparse texture floor; Field uses 60
// How long nothing may repaint before the timer-driven murmur takes over from
// the repaint-driven one.  Claude Code animates its own spinner while it
// works, and that ticks faster than this, so during a normal think the murmur
// stays in step with the spinner and this fallback never fires.
#define CHIME_MURMUR_SILENCE 90
#define CHIME_MIN_CELLS 4          // a stray one-cell redraw is not output
#define CHIME_SUPPRESS_QUIET 45    // resync/history redraw is ours
#define CHIME_STILL_HERE_QUIET 5400 // 90 seconds on the Control screen
#define CHIME_LANTERN_FRAMES 120   // brief visual state; no animation loop
// Blips (the BLIPS setting).  Between blips, a gap drawn at random from this
// range -- never a fixed beat -- and longer the longer the answer has run:
// half again after the first BLIP_TIRED frames, three times after
// BLIP_WEARY.  A short answer chatters; a long one settles into an occasional
// word, because forty seconds of anything at a steady rate is a nag.
#define CHIME_BLIP_GAP_MIN 28
#define CHIME_BLIP_GAP_MAX 64
#define CHIME_BLIP_TIRED 1200      // 20 s
#define CHIME_BLIP_WEARY 3600      // 60 s
#define CHIME_FAMILIAR_FRAMES 120  // transient Companion visual, never a loop

void chime_init(void);
// The buttons that accept and refuse a prompt, as Settings' BUTTONS tab has
// them, for the headline ("A yes   X no").  `no` may be "" when no button
// sends Esc.
void chime_set_answer_buttons(const char *yes, const char *no);
void chime_tick(void);                    // once per frame
void chime_sent(void);                    // a line was submitted (a CR sent)
// Every key sequence sent to the session passes through here first. Returns 1
// when it is a submission: a lone Enter after something was typed on the
// line, or one answering a question Claude asked (chime_needs_you). An Enter
// on an empty line is still sent -- Claude uses it to pick menu items -- but
// it is not a prompt, so it must not sound SENT or start the thinking arc:
// pressing A a few times on nothing played a whole "request" that never was.
int chime_typed(const char *bytes, size_t n);
void chime_cells(unsigned char glyph);    // screen cells changed (once per message)
void chime_view_sent(void);               // a user-initiated resync is coming
void chime_history(int scrolled_back);    // the view is scrolled into history

// UI/activity hooks.  Control visibility gates the one quiet visual check-in;
// it is not a background timer that follows the user across screens.  Call
// chime_user_activity for a real button/touch action, not every render.
void chime_control_visible(int visible);
void chime_user_activity(void);

// Profile-aware local events.  sound.c decides whether the active profile is
// allowed to voice them; these calls also keep the posture's activity clock
// awake.  Session code calls lantern only on a false->true new-output edge.
void chime_focus(void);
void chime_session_switch(void);
// Short alias retained for the input path while the UI naming settles.
void chime_switch(void);
void chime_session_lantern(void);
void chime_link_changed(int connected);

// Hardware diagnostics for the resync guard.  A positive value says exactly
// how many VBlanks of client-requested redraw suppression remain; it lets the
// STATUS screen distinguish an expected reconnect/View quiet window from a
// guard that is being re-armed unexpectedly by ordinary traffic.
unsigned chime_suppress_remaining(void);

// Frames since init, on the chime's own clock: one per chime_tick().
unsigned chime_frame(void);

// A short, UI-ready posture string.  It intentionally says only what the
// client observed; e.g. "settled" means cells became quiet, not that a model
// has decided anything.
chime_posture_t chime_posture(void);
const char *chime_posture_text(void);

// ------------------------------------------------- what Claude is doing ---
//
// When the server reports it (MSG_STATE, docs/REMOTE.md), the card knows the
// truth instead of inferring it from traffic: Claude is working, finished,
// or waiting for an approval or an answer.  The cues then come from those
// edges -- THINK when work starts, APPROVE and QUESTION when Claude is
// waiting on you, DONE when it really finished rather than when the screen
// went quiet -- and the traffic heuristics stand down for anything the state
// already says.  A server that never sends STATE leaves the heuristics in
// charge, exactly as before.
//
// `state` is a PROTO_STATE_* value; 0 means "nobody is reporting".
void chime_claude_state(unsigned state);
unsigned chime_claude(void);
// Forget the reading: the link is new and the server will send it again.
void chime_claude_reset(void);

// The line the Control screen leads with, in plain words for the person
// holding the card: what is happening and, when Claude is waiting, which
// button answers.  Unlike chime_posture_text() it may use the server's
// state, so it can say "needs your OK" rather than "settled".
const char *chime_headline(void);
// 1 while Claude is waiting on the person: the headline is a call to act.
int chime_needs_you(void);

// One reminder, never more: an approval or question left waiting this long
// without a press gets one soft LANTERN, and then the card is quiet about it.
#define CHIME_REMIND_FRAMES 5400   // 90 s

// A small pixel familiar is a Companion-only punctuation mark at meaningful
// observed transitions.  It is time-bounded; callers must not turn it into a
// permanent mascot or a looping animation.
int chime_familiar(void);

#endif  // DSTERM_CHIME_H
