#pragma once

#include <libs/klibc.h>

struct tty_graphics_;

typedef struct fb_console fb_console_t;

typedef struct fb_console_geometry {
    size_t width;
    size_t height;
    size_t columns;
    size_t rows;
} fb_console_geometry_t;

fb_console_t *fb_console_create(const struct tty_graphics_ *framebuffer);
void fb_console_destroy(fb_console_t *console);
int fb_console_bind(fb_console_t *console,
                    const struct tty_graphics_ *framebuffer);
void fb_console_set_active(fb_console_t *console, bool active);
void fb_console_get_geometry(fb_console_t *console,
                             fb_console_geometry_t *geometry);
void fb_console_write(fb_console_t *console, const char *buffer, size_t count);
void fb_console_flush(fb_console_t *console);
