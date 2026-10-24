// What main.c lends to the parts of the client that live in other files.
//
// main.c owns the things only it can own: the status log and the two dirty
// flags behind it, the overlays, the session list, and the facts about what
// the top screen is showing.  Code that used to be a static in main.c and
// reached for those directly now lives elsewhere and asks through here.
//
// Deliberately small, and deliberately read-mostly.  Nothing here hands out
// the socket, the PTY key path, or a writable grid: a caller can say a line,
// ask for a repaint, and learn what is on screen.  It cannot type.  The one
// send here, host_send_camera(), takes nothing but the camera's own message
// types, and is the PHOTO screen's (photo.c) only way onto the wire.
#ifndef DSTERM_HOST_H
#define DSTERM_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"
#include "sessions.h"
#include "settings.h"

// One line on the boot/status log, and a sub-screen repaint on the next
// frame.  The same thing status() is inside main.c.
void host_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Mark the sub screen for repaint on the next frame.
void host_refresh_sub(void);

// Wait for the next frame the one way this program may: yielding to the
// network stack's cooperative thread, then painting whatever is dirty.  See
// frame_wait() in main.c -- a BIOS wait here starves lwIP and every socket
// stops.  The boot menu runs its own loop before the main one and waits
// through this.
void host_frame_wait(void);

// Frames since boot.
unsigned long host_frame_count(void);

// frame_wait() without counting the frame: a wait inside one pass of the
// main loop, which counts its own.  The camera's drains use it, so their
// time does not move the frame clock the rest of the program reads.
void host_frame_yield(void);

// Reopen or close whichever local overlay owns the top screen, and repaint
// both screens.  Call after a picker opens, closes, or changes its selection.
void host_overlay_sync(void);

// Re-read the link, battery and signal for the Control screen's top line.
void host_link_refresh(void);

// The L+R chord, as main.c reads it: both shoulders down and one of them just
// pressed.  One definition, because a chord read two ways is two chords.
int host_shoulder_chord(unsigned raw, unsigned edge);

// The UI settings as they stand.  Read-only.
const settings_t *host_settings(void);

// The key click, as a key the person typed sounds.
void host_key_tick(int backspace);

// The touch screen as main.c reads it, a tap `dsterm view` injected
// included: whether it is held, and where.
int host_touch_held(void);
void host_touch_point(int *x, int *y);

// The link is up: there is a socket to send on.
int host_link_up(void);

// One whole camera frame (CAMERA, CAMERA_FRAME, PHOTO_CHUNK, PHOTO_END or
// PHOTO_DISCARD), sealed and sent, counted in *tx when given.  Any other
// type is refused.  A failed send is the same dead link a key's is: it says
// so and reconnects, and returns 0.
int host_send_camera(const uint8_t *frame, size_t n, unsigned long *tx);

// One whole voice frame (VOICE_START, VOICE_CHUNK or VOICE_END), sealed and
// sent.  Any other type is refused.
int host_send_voice(const uint8_t *frame, size_t n, unsigned long *tx);

// The session list the server last advertised.  Read-only.
const sessions_t *host_sessions(void);

// What the top screen is showing, for anything that has to place itself on it.
typedef struct {
    const grid_t *terminal;     // the terminal's cells
    const grid_t *page;         // the PROJECT/SETTINGS page's cells
    int page_mode;              // the lower screen is on a page mode
    int composer_open;          // the composer overlay owns the top screen
    int session_menu_open;      // the session picker owns the input
} host_view_t;
void host_view(host_view_t *out);

#endif  // DSTERM_HOST_H
