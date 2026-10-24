#ifndef DSTERM_COMPOSER_H
#define DSTERM_COMPOSER_H

#include <stddef.h>
#include <stdint.h>

// 1024, raised from 255 when the desktop clipboard arrived. A clipboard is
// commonly a whole prompt, and a buffer that could not hold one would have
// made the paste truncate -- which for a command means running something other
// than what was copied, the exact failure the DO tab drops an over-long action
// to avoid. The cost is 769 bytes of static RAM and nothing else: the send
// plan already chunks the body into 64-byte KEY payloads and simply loops
// more times.
#define COMPOSER_MAX 1024

// Two states, not three. There used to be a REVIEW state between editing and
// sending, so you could look at the body before committing it -- but the
// overlay shows the body the whole time it is being edited, so REVIEW
// displayed what was already on screen and cost a second meaning for A. A
// sends, always; X cancels, always.
typedef enum {
    COMPOSER_CLOSED = 0,
    COMPOSER_EDIT,
} composer_mode_t;

typedef struct {
    char body[COMPOSER_MAX + 1];
    size_t len;
    composer_mode_t mode;
} composer_t;

void composer_init(composer_t *c);
void composer_open(composer_t *c);
void composer_cancel(composer_t *c);
int composer_append(composer_t *c, uint8_t byte);
int composer_backspace(composer_t *c);
// Load a snippet straight into EDIT: it is already written, so the next
// press should be Send.
int composer_load(composer_t *c, const char *body);
// Add dictated text after whatever is already written, with one space
// between them when the buffer does not already end in whitespace.  Bytes
// the composer would refuse from the keyboard are refused here too, and the
// buffer's limit cuts the tail.  Returns how many bytes of `text` landed.
size_t composer_append_text(composer_t *c, const char *text, size_t len);

// The send plan is the body in <=64-byte KEY payloads followed by CR in its
// own final frame. An empty body therefore still has one frame: the CR.
size_t composer_frame_count(const composer_t *c);
int composer_frame(const composer_t *c, size_t index,
                   const uint8_t **data, size_t *len);

#endif
