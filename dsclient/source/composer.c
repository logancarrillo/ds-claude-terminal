#include "composer.h"

#include <string.h>

#define BODY_FRAME_MAX 64

void composer_init(composer_t *c)
{
    memset(c, 0, sizeof(*c));
}

void composer_open(composer_t *c)
{
    c->len = 0;
    c->body[0] = '\0';
    c->mode = COMPOSER_EDIT;
}

void composer_cancel(composer_t *c)
{
    c->mode = COMPOSER_CLOSED;
}

int composer_append(composer_t *c, uint8_t byte)
{
    if (c->mode != COMPOSER_EDIT || c->len >= COMPOSER_MAX) return 0;
    if (!((byte >= 0x20 && byte <= 0x7e) || byte == '\n' || byte == '\t')) return 0;
    c->body[c->len++] = (char)byte;
    c->body[c->len] = '\0';
    return 1;
}

int composer_backspace(composer_t *c)
{
    if (c->mode != COMPOSER_EDIT || c->len == 0) return 0;
    c->body[--c->len] = '\0';
    return 1;
}

int composer_load(composer_t *c, const char *body)
{
    const size_t n = strlen(body);
    if (n > COMPOSER_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = (uint8_t)body[i];
        if (!((b >= 0x20 && b <= 0x7e) || b == '\n' || b == '\t')) return 0;
    }
    memcpy(c->body, body, n + 1);
    c->len = n;
    c->mode = COMPOSER_EDIT;
    return 1;
}

size_t composer_append_text(composer_t *c, const char *text, size_t len)
{
    if (!c || !text || c->mode != COMPOSER_EDIT) return 0;
    if (len > 0 && c->len > 0 &&
        c->body[c->len - 1] != ' ' && c->body[c->len - 1] != '\n') {
        (void)composer_append(c, ' ');
    }
    size_t landed = 0;
    for (size_t i = 0; i < len; i++) {
        if (composer_append(c, (uint8_t)text[i])) landed++;
    }
    return landed;
}

size_t composer_frame_count(const composer_t *c)
{
    return (c->len + BODY_FRAME_MAX - 1) / BODY_FRAME_MAX + 1;
}

int composer_frame(const composer_t *c, size_t index,
                   const uint8_t **data, size_t *len)
{
    static const uint8_t cr = '\r';
    const size_t body_frames = (c->len + BODY_FRAME_MAX - 1) / BODY_FRAME_MAX;
    if (index < body_frames) {
        const size_t off = index * BODY_FRAME_MAX;
        const size_t remaining = c->len - off;
        *data = (const uint8_t *)c->body + off;
        *len = remaining > BODY_FRAME_MAX ? BODY_FRAME_MAX : remaining;
        return 1;
    }
    if (index == body_frames) {
        *data = &cr;
        *len = 1;
        return 1;
    }
    return 0;
}
