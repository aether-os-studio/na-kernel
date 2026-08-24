#include <drivers/fbcon.h>
#include <drivers/tty.h>

#define FBCON_FONT_WIDTH 8
#define FBCON_FONT_HEIGHT 16
#define FBCON_CSI_PARAMETERS 16

extern const uint8_t fb_console_font[256 * FBCON_FONT_HEIGHT];

typedef struct fb_console_cell {
    uint32_t glyph;
    uint32_t foreground;
    uint32_t background;
} fb_console_cell_t;

typedef struct fb_console_surface {
    volatile uint32_t *pixels;
    size_t width;
    size_t height;
    size_t pitch;
    uint8_t red_size;
    uint8_t red_shift;
    uint8_t green_size;
    uint8_t green_shift;
    uint8_t blue_size;
    uint8_t blue_shift;
} fb_console_surface_t;

typedef struct fb_console_layout {
    fb_console_surface_t surface;
    size_t scale;
    size_t glyph_width;
    size_t glyph_height;
    size_t offset_x;
    size_t offset_y;
    size_t columns;
    size_t rows;
} fb_console_layout_t;

enum fb_console_parser_state {
    FBCON_PARSER_TEXT,
    FBCON_PARSER_ESCAPE,
    FBCON_PARSER_CSI,
    FBCON_PARSER_OSC,
    FBCON_PARSER_OSC_ESCAPE,
    FBCON_PARSER_CHARSET,
};

struct fb_console {
    spinlock_t lock;
    fb_console_layout_t layout;
    fb_console_cell_t *cells;
    uint32_t *row_buffer;
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_cursor_x;
    size_t saved_cursor_y;
    size_t scroll_top;
    size_t scroll_bottom;
    uint32_t foreground;
    uint32_t background;
    uint32_t saved_foreground;
    uint32_t saved_background;
    uint32_t csi_parameters[FBCON_CSI_PARAMETERS];
    uint32_t unicode;
    uint32_t last_glyph;
    uint8_t csi_count;
    uint8_t unicode_remaining;
    char csi_private;
    char escape_intermediate;
    enum fb_console_parser_state parser;
    bool active;
    bool cursor_visible;
    bool bold;
    bool reverse;
    bool saved_bold;
    bool saved_reverse;
    bool insert_mode;
    bool autowrap;
    bool origin_mode;
    bool dec_graphics[2];
    uint8_t charset;
};

static const uint32_t fb_console_palette[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa,
    0x00aaaa, 0xaaaaaa, 0x555555, 0xff5555, 0x55ff55, 0xffff55,
    0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
};

static uint32_t fb_console_pack_component(uint8_t value, uint8_t size,
                                          uint8_t shift) {
    uint64_t maximum = (1ULL << size) - 1;
    return (uint32_t)(((value * maximum + 127) / 255) << shift);
}

static uint32_t fb_console_pack_colour(const fb_console_surface_t *surface,
                                       uint32_t colour) {
    return fb_console_pack_component((colour >> 16) & 0xff, surface->red_size,
                                     surface->red_shift) |
           fb_console_pack_component((colour >> 8) & 0xff, surface->green_size,
                                     surface->green_shift) |
           fb_console_pack_component(colour & 0xff, surface->blue_size,
                                     surface->blue_shift);
}

static int fb_console_make_layout(const struct tty_graphics_ *framebuffer,
                                  fb_console_layout_t *layout) {
    size_t scale = 1;

    if (!framebuffer || !layout || !framebuffer->address ||
        framebuffer->bpp != 32 || !framebuffer->width || !framebuffer->height ||
        framebuffer->width > SIZE_MAX || framebuffer->height > SIZE_MAX ||
        framebuffer->pitch > SIZE_MAX ||
        framebuffer->pitch % sizeof(uint32_t) ||
        framebuffer->width > SIZE_MAX / sizeof(uint32_t) ||
        framebuffer->pitch < framebuffer->width * sizeof(uint32_t) ||
        !framebuffer->red_mask_size || !framebuffer->green_mask_size ||
        !framebuffer->blue_mask_size || framebuffer->red_mask_size > 16 ||
        framebuffer->green_mask_size > 16 || framebuffer->blue_mask_size > 16 ||
        framebuffer->red_mask_shift + framebuffer->red_mask_size > 32 ||
        framebuffer->green_mask_shift + framebuffer->green_mask_size > 32 ||
        framebuffer->blue_mask_shift + framebuffer->blue_mask_size > 32)
        return -EINVAL;

    if (framebuffer->width >= 5120 && framebuffer->height >= 2880)
        scale = 4;
    else if (framebuffer->width >= 2560 && framebuffer->height >= 1440)
        scale = 2;

    memset(layout, 0, sizeof(*layout));
    layout->surface = (fb_console_surface_t){
        .pixels = framebuffer->address,
        .width = (size_t)framebuffer->width,
        .height = (size_t)framebuffer->height,
        .pitch = (size_t)framebuffer->pitch,
        .red_size = framebuffer->red_mask_size,
        .red_shift = framebuffer->red_mask_shift,
        .green_size = framebuffer->green_mask_size,
        .green_shift = framebuffer->green_mask_shift,
        .blue_size = framebuffer->blue_mask_size,
        .blue_shift = framebuffer->blue_mask_shift,
    };
    layout->scale = scale;
    layout->glyph_width = FBCON_FONT_WIDTH * scale;
    layout->glyph_height = FBCON_FONT_HEIGHT * scale;
    layout->columns = layout->surface.width / layout->glyph_width;
    layout->rows = layout->surface.height / layout->glyph_height;
    if (!layout->columns || !layout->rows ||
        layout->rows > SIZE_MAX / layout->columns)
        return -ERANGE;

    layout->offset_x =
        (layout->surface.width - layout->columns * layout->glyph_width) / 2;
    layout->offset_y =
        (layout->surface.height - layout->rows * layout->glyph_height) / 2;
    return 0;
}

static fb_console_cell_t fb_console_empty_cell(const fb_console_t *console) {
    return (fb_console_cell_t){
        .glyph = ' ',
        .foreground = console->foreground,
        .background = console->background,
    };
}

static void fb_console_fill_cells(fb_console_cell_t *cells, size_t count,
                                  fb_console_cell_t value) {
    for (size_t i = 0; i < count; i++)
        cells[i] = value;
}

static void fb_console_plot_cell(fb_console_t *console, size_t column,
                                 size_t row, const fb_console_cell_t *cell,
                                 bool cursor) {
    const fb_console_layout_t *layout = &console->layout;
    uint32_t foreground;
    uint32_t background;
    uint32_t glyph;
    size_t origin_x;
    size_t origin_y;

    if (!console->active || column >= layout->columns || row >= layout->rows)
        return;

    foreground = cell->foreground;
    background = cell->background;
    if (cursor) {
        uint32_t temporary = foreground;
        foreground = background;
        background = temporary;
    }
    foreground = fb_console_pack_colour(&layout->surface, foreground);
    background = fb_console_pack_colour(&layout->surface, background);
    glyph = cell->glyph < 256 ? cell->glyph : '?';
    origin_x = layout->offset_x + column * layout->glyph_width;
    origin_y = layout->offset_y + row * layout->glyph_height;

    for (size_t font_y = 0; font_y < FBCON_FONT_HEIGHT; font_y++) {
        uint8_t bits = fb_console_font[glyph * FBCON_FONT_HEIGHT + font_y];
        for (size_t scale_y = 0; scale_y < layout->scale; scale_y++) {
            size_t pixel_y = origin_y + font_y * layout->scale + scale_y;
            volatile uint32_t *destination =
                (volatile uint32_t *)((volatile uint8_t *)
                                          layout->surface.pixels +
                                      pixel_y * layout->surface.pitch) +
                origin_x;

            for (size_t font_x = 0; font_x < FBCON_FONT_WIDTH; font_x++) {
                uint32_t colour =
                    bits & (0x80 >> font_x) ? foreground : background;
                for (size_t scale_x = 0; scale_x < layout->scale; scale_x++)
                    *destination++ = colour;
            }
        }
    }
}

static void fb_console_draw_range(fb_console_t *console, size_t first,
                                  size_t count) {
    size_t cell_count = console->layout.rows * console->layout.columns;

    if (!console->active || first >= cell_count)
        return;
    count = MIN(count, cell_count - first);
    for (size_t i = first; i < first + count; i++)
        fb_console_plot_cell(console, i % console->layout.columns,
                             i / console->layout.columns, &console->cells[i],
                             false);
}

static void fb_console_draw_cursor(fb_console_t *console, bool cursor) {
    if (!console->active || !console->cursor_visible ||
        console->cursor_x >= console->layout.columns ||
        console->cursor_y >= console->layout.rows)
        return;

    size_t index =
        console->cursor_y * console->layout.columns + console->cursor_x;
    fb_console_plot_cell(console, console->cursor_x, console->cursor_y,
                         &console->cells[index], cursor);
}

static void fb_console_copy_row(fb_console_t *console, size_t pixel_y) {
    const fb_console_surface_t *surface = &console->layout.surface;
    volatile void *destination =
        (volatile uint8_t *)surface->pixels + pixel_y * surface->pitch;

    memcpy((void *)destination, console->row_buffer,
           surface->width * sizeof(*console->row_buffer));
}

static void fb_console_fill_row_buffer(fb_console_t *console, uint32_t colour) {
    uint32_t packed = fb_console_pack_colour(&console->layout.surface, colour);

    for (size_t x = 0; x < console->layout.surface.width; x++)
        console->row_buffer[x] = packed;
}

static void fb_console_render_rows(fb_console_t *console, size_t top,
                                   size_t bottom) {
    const fb_console_layout_t *layout = &console->layout;

    if (!console->active || top >= bottom || bottom > layout->rows)
        return;

    for (size_t row = top; row < bottom; row++) {
        for (size_t font_y = 0; font_y < FBCON_FONT_HEIGHT; font_y++) {
            fb_console_fill_row_buffer(console, fb_console_palette[0]);
            uint32_t *output = &console->row_buffer[layout->offset_x];

            for (size_t column = 0; column < layout->columns; column++) {
                const fb_console_cell_t *cell =
                    &console->cells[row * layout->columns + column];
                uint32_t foreground =
                    fb_console_pack_colour(&layout->surface, cell->foreground);
                uint32_t background =
                    fb_console_pack_colour(&layout->surface, cell->background);
                uint32_t glyph = cell->glyph < 256 ? cell->glyph : '?';
                uint8_t bits =
                    fb_console_font[glyph * FBCON_FONT_HEIGHT + font_y];

                for (size_t font_x = 0; font_x < FBCON_FONT_WIDTH; font_x++) {
                    uint32_t colour =
                        bits & (0x80 >> font_x) ? foreground : background;
                    for (size_t scale_x = 0; scale_x < layout->scale; scale_x++)
                        *output++ = colour;
                }
            }

            size_t first_y = layout->offset_y + row * layout->glyph_height +
                             font_y * layout->scale;
            for (size_t scale_y = 0; scale_y < layout->scale; scale_y++)
                fb_console_copy_row(console, first_y + scale_y);
        }
    }
}

static void fb_console_refresh_locked(fb_console_t *console) {
    if (!console->active)
        return;

    fb_console_fill_row_buffer(console, fb_console_palette[0]);
    for (size_t y = 0; y < console->layout.offset_y; y++)
        fb_console_copy_row(console, y);
    fb_console_render_rows(console, 0, console->layout.rows);
    size_t text_bottom = console->layout.offset_y +
                         console->layout.rows * console->layout.glyph_height;
    fb_console_fill_row_buffer(console, fb_console_palette[0]);
    for (size_t y = text_bottom; y < console->layout.surface.height; y++)
        fb_console_copy_row(console, y);
    fb_console_draw_cursor(console, true);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void fb_console_scroll(fb_console_t *console, size_t top, size_t bottom,
                              size_t lines, bool down) {
    size_t columns = console->layout.columns;
    size_t region_rows;
    size_t moved_cells;
    fb_console_cell_t empty = fb_console_empty_cell(console);

    if (top >= bottom || bottom > console->layout.rows || !lines)
        return;
    region_rows = bottom - top;
    lines = MIN(lines, region_rows);
    moved_cells = (region_rows - lines) * columns;

    if (down) {
        memmove(&console->cells[(top + lines) * columns],
                &console->cells[top * columns],
                moved_cells * sizeof(*console->cells));
        fb_console_fill_cells(&console->cells[top * columns], lines * columns,
                              empty);
    } else {
        memmove(&console->cells[top * columns],
                &console->cells[(top + lines) * columns],
                moved_cells * sizeof(*console->cells));
        fb_console_fill_cells(&console->cells[(bottom - lines) * columns],
                              lines * columns, empty);
    }

    fb_console_render_rows(console, top, bottom);
}

static void fb_console_line_feed(fb_console_t *console) {
    if (console->cursor_y + 1 < console->scroll_bottom) {
        console->cursor_y++;
        return;
    }

    if (console->cursor_y >= console->scroll_top &&
        console->cursor_y < console->scroll_bottom) {
        fb_console_scroll(console, console->scroll_top, console->scroll_bottom,
                          1, false);
        console->cursor_y = console->scroll_bottom - 1;
    } else if (console->cursor_y + 1 < console->layout.rows) {
        console->cursor_y++;
    }
}

static void fb_console_reverse_line_feed(fb_console_t *console) {
    if (console->cursor_y > console->scroll_top) {
        console->cursor_y--;
        return;
    }
    if (console->cursor_y == console->scroll_top)
        fb_console_scroll(console, console->scroll_top, console->scroll_bottom,
                          1, true);
}

static uint32_t fb_console_dec_glyph(uint32_t glyph) {
    switch (glyph) {
    case 'j':
        return 0xd9;
    case 'k':
        return 0xbf;
    case 'l':
        return 0xda;
    case 'm':
        return 0xc0;
    case 'n':
        return 0xc5;
    case 'q':
        return 0xc4;
    case 't':
        return 0xc3;
    case 'u':
        return 0xb4;
    case 'v':
        return 0xc1;
    case 'w':
        return 0xc2;
    case 'x':
        return 0xb3;
    default:
        return glyph;
    }
}

static void fb_console_insert_characters(fb_console_t *console, size_t count) {
    size_t columns = console->layout.columns;
    size_t x = MIN(console->cursor_x, columns - 1);
    size_t row = console->cursor_y * columns;
    fb_console_cell_t empty = fb_console_empty_cell(console);

    count = MIN(count, columns - x);
    memmove(&console->cells[row + x + count], &console->cells[row + x],
            (columns - x - count) * sizeof(*console->cells));
    fb_console_fill_cells(&console->cells[row + x], count, empty);
    fb_console_draw_range(console, row + x, columns - x);
}

static void fb_console_delete_characters(fb_console_t *console, size_t count) {
    size_t columns = console->layout.columns;
    size_t x = MIN(console->cursor_x, columns - 1);
    size_t row = console->cursor_y * columns;
    fb_console_cell_t empty = fb_console_empty_cell(console);

    count = MIN(count, columns - x);
    memmove(&console->cells[row + x], &console->cells[row + x + count],
            (columns - x - count) * sizeof(*console->cells));
    fb_console_fill_cells(&console->cells[row + columns - count], count, empty);
    fb_console_draw_range(console, row + x, columns - x);
}

static void fb_console_erase(fb_console_t *console, size_t first,
                             size_t count) {
    size_t cells = console->layout.rows * console->layout.columns;

    if (first >= cells)
        return;
    count = MIN(count, cells - first);
    fb_console_fill_cells(&console->cells[first], count,
                          fb_console_empty_cell(console));
    fb_console_draw_range(console, first, count);
}

static void fb_console_put_glyph(fb_console_t *console, uint32_t glyph) {
    size_t columns = console->layout.columns;
    size_t index;

    if (console->cursor_x >= columns) {
        if (console->autowrap) {
            console->cursor_x = 0;
            fb_console_line_feed(console);
        } else {
            console->cursor_x = columns - 1;
        }
    }

    if (console->insert_mode)
        fb_console_insert_characters(console, 1);
    if (console->dec_graphics[console->charset])
        glyph = fb_console_dec_glyph(glyph);
    if (glyph > 255)
        glyph = '?';
    console->last_glyph = glyph;

    index = console->cursor_y * columns + console->cursor_x;
    console->cells[index] = (fb_console_cell_t){
        .glyph = glyph,
        .foreground =
            console->reverse ? console->background : console->foreground,
        .background =
            console->reverse ? console->foreground : console->background,
    };
    fb_console_plot_cell(console, console->cursor_x, console->cursor_y,
                         &console->cells[index], false);
    console->cursor_x++;
}

static void fb_console_save_state(fb_console_t *console) {
    console->saved_cursor_x = console->cursor_x;
    console->saved_cursor_y = console->cursor_y;
    console->saved_foreground = console->foreground;
    console->saved_background = console->background;
    console->saved_bold = console->bold;
    console->saved_reverse = console->reverse;
}

static void fb_console_restore_state(fb_console_t *console) {
    console->cursor_x =
        MIN(console->saved_cursor_x, console->layout.columns - 1);
    console->cursor_y = MIN(console->saved_cursor_y, console->layout.rows - 1);
    console->foreground = console->saved_foreground;
    console->background = console->saved_background;
    console->bold = console->saved_bold;
    console->reverse = console->saved_reverse;
}

static void fb_console_reset_attributes(fb_console_t *console) {
    console->foreground = fb_console_palette[7];
    console->background = fb_console_palette[0];
    console->bold = false;
    console->reverse = false;
}

static void fb_console_reset_locked(fb_console_t *console, bool clear) {
    console->cursor_x = 0;
    console->cursor_y = 0;
    console->saved_cursor_x = 0;
    console->saved_cursor_y = 0;
    console->scroll_top = 0;
    console->scroll_bottom = console->layout.rows;
    console->parser = FBCON_PARSER_TEXT;
    console->unicode = 0;
    console->unicode_remaining = 0;
    console->cursor_visible = true;
    console->insert_mode = false;
    console->autowrap = true;
    console->origin_mode = false;
    console->dec_graphics[0] = false;
    console->dec_graphics[1] = false;
    console->charset = 0;
    console->last_glyph = ' ';
    fb_console_reset_attributes(console);
    fb_console_save_state(console);
    if (clear)
        fb_console_erase(console, 0,
                         console->layout.rows * console->layout.columns);
}

static uint32_t fb_console_csi_parameter(const fb_console_t *console,
                                         size_t index, uint32_t fallback) {
    if (index >= console->csi_count ||
        console->csi_parameters[index] == UINT32_MAX)
        return fallback;
    return console->csi_parameters[index];
}

static void fb_console_sgr(fb_console_t *console) {
    if (!console->csi_count) {
        fb_console_reset_attributes(console);
        return;
    }

    for (size_t i = 0; i < console->csi_count; i++) {
        uint32_t value = fb_console_csi_parameter(console, i, 0);

        if (value == 0) {
            fb_console_reset_attributes(console);
        } else if (value == 1) {
            console->bold = true;
        } else if (value == 7) {
            console->reverse = true;
        } else if (value == 22) {
            console->bold = false;
        } else if (value == 27) {
            console->reverse = false;
        } else if (value >= 30 && value <= 37) {
            console->foreground =
                fb_console_palette[value - 30 + (console->bold ? 8 : 0)];
        } else if (value == 39) {
            console->foreground = fb_console_palette[console->bold ? 15 : 7];
        } else if (value >= 40 && value <= 47) {
            console->background = fb_console_palette[value - 40];
        } else if (value == 49) {
            console->background = fb_console_palette[0];
        } else if (value >= 90 && value <= 97) {
            console->foreground = fb_console_palette[value - 90 + 8];
        } else if (value >= 100 && value <= 107) {
            console->background = fb_console_palette[value - 100 + 8];
        } else if ((value == 38 || value == 48) && i + 1 < console->csi_count) {
            bool foreground = value == 38;
            uint32_t mode = fb_console_csi_parameter(console, ++i, 0);
            uint32_t colour;

            if (mode == 5 && i + 1 < console->csi_count) {
                uint32_t index = fb_console_csi_parameter(console, ++i, 0);
                if (index < 16) {
                    colour = fb_console_palette[index];
                } else if (index < 232) {
                    index -= 16;
                    uint32_t red = index / 36;
                    uint32_t green = (index / 6) % 6;
                    uint32_t blue = index % 6;
                    red = red ? red * 40 + 55 : 0;
                    green = green ? green * 40 + 55 : 0;
                    blue = blue ? blue * 40 + 55 : 0;
                    colour = (red << 16) | (green << 8) | blue;
                } else {
                    uint32_t component = MIN(index - 232, 23U) * 10 + 8;
                    colour = (component << 16) | (component << 8) | component;
                }
            } else if (mode == 2 && i + 3 < console->csi_count) {
                uint32_t red =
                    MIN(fb_console_csi_parameter(console, ++i, 0), 255U);
                uint32_t green =
                    MIN(fb_console_csi_parameter(console, ++i, 0), 255U);
                uint32_t blue =
                    MIN(fb_console_csi_parameter(console, ++i, 0), 255U);
                colour = (red << 16) | (green << 8) | blue;
            } else {
                continue;
            }

            if (foreground)
                console->foreground = colour;
            else
                console->background = colour;
        }
    }
}

static void fb_console_set_cursor(fb_console_t *console, uint32_t row,
                                  uint32_t column) {
    size_t top = console->origin_mode ? console->scroll_top : 0;
    size_t bottom =
        console->origin_mode ? console->scroll_bottom : console->layout.rows;
    size_t target = top + (row ? row - 1 : 0);

    console->cursor_y = MIN(target, bottom - 1);
    console->cursor_x =
        MIN(column ? column - 1 : 0, console->layout.columns - 1);
}

static void fb_console_erase_display(fb_console_t *console, uint32_t mode) {
    size_t columns = console->layout.columns;
    size_t cursor =
        console->cursor_y * columns + MIN(console->cursor_x, columns - 1);
    size_t cells = console->layout.rows * columns;

    if (mode == 0)
        fb_console_erase(console, cursor, cells - cursor);
    else if (mode == 1)
        fb_console_erase(console, 0, cursor + 1);
    else if (mode == 2 || mode == 3)
        fb_console_erase(console, 0, cells);
}

static void fb_console_erase_line(fb_console_t *console, uint32_t mode) {
    size_t columns = console->layout.columns;
    size_t x = MIN(console->cursor_x, columns - 1);
    size_t row = console->cursor_y * columns;

    if (mode == 0)
        fb_console_erase(console, row + x, columns - x);
    else if (mode == 1)
        fb_console_erase(console, row, x + 1);
    else if (mode == 2)
        fb_console_erase(console, row, columns);
}

static void fb_console_set_mode(fb_console_t *console, bool enabled) {
    for (size_t i = 0; i < console->csi_count; i++) {
        uint32_t mode = fb_console_csi_parameter(console, i, 0);

        if (console->csi_private == '?') {
            if (mode == 6) {
                console->origin_mode = enabled;
                console->cursor_x = 0;
                console->cursor_y = enabled ? console->scroll_top : 0;
            } else if (mode == 7) {
                console->autowrap = enabled;
            } else if (mode == 25) {
                console->cursor_visible = enabled;
            }
        } else if (mode == 4) {
            console->insert_mode = enabled;
        }
    }
}

static void fb_console_handle_csi(fb_console_t *console, uint8_t command) {
    size_t columns = console->layout.columns;
    size_t rows = console->layout.rows;
    size_t amount = fb_console_csi_parameter(console, 0, 1);
    size_t vertical_top = console->origin_mode ? console->scroll_top : 0;
    size_t vertical_bottom =
        console->origin_mode ? console->scroll_bottom : rows;

    if (!amount)
        amount = 1;

    switch (command) {
    case '@':
        fb_console_insert_characters(console, amount);
        break;
    case 'A':
        console->cursor_y -= MIN(
            console->cursor_y - MIN(console->cursor_y, vertical_top), amount);
        break;
    case 'B':
    case 'e':
        console->cursor_y =
            MIN(console->cursor_y + amount, vertical_bottom - 1);
        break;
    case 'C':
    case 'a':
        console->cursor_x = MIN(console->cursor_x + amount, columns - 1);
        break;
    case 'D':
        console->cursor_x -= MIN(console->cursor_x, amount);
        break;
    case 'E':
        console->cursor_y =
            MIN(console->cursor_y + amount, vertical_bottom - 1);
        console->cursor_x = 0;
        break;
    case 'F':
        console->cursor_y -= MIN(
            console->cursor_y - MIN(console->cursor_y, vertical_top), amount);
        console->cursor_x = 0;
        break;
    case 'G':
    case '`':
        console->cursor_x = MIN(amount ? amount - 1 : 0, columns - 1);
        break;
    case 'H':
    case 'f':
        fb_console_set_cursor(console, fb_console_csi_parameter(console, 0, 1),
                              fb_console_csi_parameter(console, 1, 1));
        break;
    case 'J':
        fb_console_erase_display(console,
                                 fb_console_csi_parameter(console, 0, 0));
        break;
    case 'K':
        fb_console_erase_line(console, fb_console_csi_parameter(console, 0, 0));
        break;
    case 'L':
        if (console->cursor_y >= console->scroll_top &&
            console->cursor_y < console->scroll_bottom)
            fb_console_scroll(console, console->cursor_y,
                              console->scroll_bottom, amount, true);
        break;
    case 'M':
        if (console->cursor_y >= console->scroll_top &&
            console->cursor_y < console->scroll_bottom)
            fb_console_scroll(console, console->cursor_y,
                              console->scroll_bottom, amount, false);
        break;
    case 'P':
        fb_console_delete_characters(console, amount);
        break;
    case 'I': {
        size_t next = console->cursor_x;
        while (amount-- && next < columns - 1)
            next = MIN((next + 8) & ~(size_t)7, columns - 1);
        console->cursor_x = next;
        break;
    }
    case 'S':
        fb_console_scroll(console, console->scroll_top, console->scroll_bottom,
                          amount, false);
        break;
    case 'T':
        fb_console_scroll(console, console->scroll_top, console->scroll_bottom,
                          amount, true);
        break;
    case 'X': {
        size_t x = MIN(console->cursor_x, columns - 1);
        fb_console_erase(console, console->cursor_y * columns + x,
                         MIN(amount, columns - x));
        break;
    }
    case 'Z':
        while (amount-- && console->cursor_x)
            console->cursor_x = console->cursor_x < 8
                                    ? 0
                                    : (console->cursor_x - 1) & ~(size_t)7;
        break;
    case 'b':
        while (amount--)
            fb_console_put_glyph(console, console->last_glyph);
        break;
    case 'd':
        console->cursor_y = MIN(vertical_top + amount - 1, vertical_bottom - 1);
        break;
    case 'h':
        fb_console_set_mode(console, true);
        break;
    case 'l':
        fb_console_set_mode(console, false);
        break;
    case 'm':
        fb_console_sgr(console);
        break;
    case 'r': {
        size_t top = fb_console_csi_parameter(console, 0, 1);
        size_t bottom = fb_console_csi_parameter(console, 1, rows);
        if (!top)
            top = 1;
        if (!bottom)
            bottom = rows;
        if (top && bottom > top && bottom <= rows) {
            console->scroll_top = top - 1;
            console->scroll_bottom = bottom;
            console->cursor_x = 0;
            console->cursor_y = console->origin_mode ? console->scroll_top : 0;
        }
        break;
    }
    case 's':
        fb_console_save_state(console);
        break;
    case 'u':
        fb_console_restore_state(console);
        break;
    default:
        break;
    }
}

static void fb_console_begin_csi(fb_console_t *console) {
    for (size_t i = 0; i < FBCON_CSI_PARAMETERS; i++)
        console->csi_parameters[i] = UINT32_MAX;
    console->csi_count = 1;
    console->csi_private = 0;
    console->parser = FBCON_PARSER_CSI;
}

static void fb_console_parse_csi(fb_console_t *console, uint8_t value) {
    if ((value == '?' || value == '>' || value == '!') &&
        console->csi_count == 1 && console->csi_parameters[0] == UINT32_MAX) {
        console->csi_private = value;
        return;
    }
    if (value >= '0' && value <= '9') {
        uint32_t *parameter = &console->csi_parameters[console->csi_count - 1];
        if (*parameter == UINT32_MAX)
            *parameter = 0;
        if (*parameter <= (UINT32_MAX - 9) / 10)
            *parameter = *parameter * 10 + value - '0';
        return;
    }
    if (value == ';' || value == ':') {
        if (console->csi_count < FBCON_CSI_PARAMETERS)
            console->csi_count++;
        return;
    }
    if (value >= 0x40 && value <= 0x7e) {
        fb_console_handle_csi(console, value);
        console->parser = FBCON_PARSER_TEXT;
    }
}

static uint32_t fb_console_unicode_glyph(uint32_t codepoint) {
    switch (codepoint) {
    case 0x00a0:
        return ' ';
    case 0x00b0:
        return 0xf8;
    case 0x00b1:
        return 0xf1;
    case 0x2022:
        return 0x07;
    case 0x2190:
        return 0x1b;
    case 0x2191:
        return 0x18;
    case 0x2192:
        return 0x1a;
    case 0x2193:
        return 0x19;
    case 0x2500:
        return 0xc4;
    case 0x2502:
        return 0xb3;
    case 0x250c:
        return 0xda;
    case 0x2510:
        return 0xbf;
    case 0x2514:
        return 0xc0;
    case 0x2518:
        return 0xd9;
    case 0x251c:
        return 0xc3;
    case 0x2524:
        return 0xb4;
    case 0x252c:
        return 0xc2;
    case 0x2534:
        return 0xc1;
    case 0x253c:
        return 0xc5;
    case 0x2580:
        return 0xdf;
    case 0x2584:
        return 0xdc;
    case 0x2588:
        return 0xdb;
    case 0x2591:
        return 0xb0;
    case 0x2592:
        return 0xb1;
    case 0x2593:
        return 0xb2;
    default:
        return codepoint < 256 ? codepoint : '?';
    }
}

static void fb_console_parse_escape(fb_console_t *console, uint8_t value) {
    console->parser = FBCON_PARSER_TEXT;
    switch (value) {
    case '[':
        fb_console_begin_csi(console);
        break;
    case ']':
        console->parser = FBCON_PARSER_OSC;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
    case '#':
    case '%':
        console->escape_intermediate = value;
        console->parser = FBCON_PARSER_CHARSET;
        break;
    case '7':
        fb_console_save_state(console);
        break;
    case '8':
        fb_console_restore_state(console);
        break;
    case 'D':
        fb_console_line_feed(console);
        break;
    case 'E':
        console->cursor_x = 0;
        fb_console_line_feed(console);
        break;
    case 'M':
        fb_console_reverse_line_feed(console);
        break;
    case 'c':
        fb_console_reset_locked(console, true);
        break;
    default:
        break;
    }
}

static void fb_console_parse_charset(fb_console_t *console, uint8_t value) {
    if (console->escape_intermediate == '(')
        console->dec_graphics[0] = value == '0';
    else if (console->escape_intermediate == ')')
        console->dec_graphics[1] = value == '0';
    else if (console->escape_intermediate == '#' && value == '8') {
        fb_console_cell_t alignment = {
            .glyph = 'E',
            .foreground = console->foreground,
            .background = console->background,
        };
        size_t cells = console->layout.rows * console->layout.columns;
        fb_console_fill_cells(console->cells, cells, alignment);
        fb_console_draw_range(console, 0, cells);
    }
    console->escape_intermediate = 0;
    console->parser = FBCON_PARSER_TEXT;
}

static void fb_console_parse_control(fb_console_t *console, uint8_t value) {
    switch (value) {
    case '\a':
        break;
    case '\b':
        if (console->cursor_x)
            console->cursor_x--;
        break;
    case '\t': {
        size_t next = (console->cursor_x + 8) & ~(size_t)7;
        console->cursor_x = MIN(next, console->layout.columns - 1);
        break;
    }
    case '\n':
    case '\v':
    case '\f':
        console->cursor_x = 0;
        fb_console_line_feed(console);
        break;
    case '\r':
        console->cursor_x = 0;
        break;
    case 0x0e:
        console->charset = 1;
        break;
    case 0x0f:
        console->charset = 0;
        break;
    case 0x18:
    case 0x1a:
        console->parser = FBCON_PARSER_TEXT;
        console->unicode = 0;
        console->unicode_remaining = 0;
        break;
    case 0x1b:
        console->parser = FBCON_PARSER_ESCAPE;
        break;
    default:
        break;
    }
}

static void fb_console_parse_text(fb_console_t *console, uint8_t value) {
    if (console->unicode_remaining) {
        if ((value & 0xc0) != 0x80) {
            console->unicode = 0;
            console->unicode_remaining = 0;
            fb_console_put_glyph(console, '?');
            fb_console_parse_text(console, value);
            return;
        }
        console->unicode = (console->unicode << 6) | (value & 0x3f);
        if (--console->unicode_remaining == 0) {
            fb_console_put_glyph(console,
                                 fb_console_unicode_glyph(console->unicode));
            console->unicode = 0;
        }
        return;
    }

    if (value < 0x20 || value == 0x7f) {
        fb_console_parse_control(console, value);
    } else if (value < 0x80) {
        fb_console_put_glyph(console, value);
    } else if ((value & 0xe0) == 0xc0) {
        console->unicode = value & 0x1f;
        console->unicode_remaining = 1;
    } else if ((value & 0xf0) == 0xe0) {
        console->unicode = value & 0x0f;
        console->unicode_remaining = 2;
    } else if ((value & 0xf8) == 0xf0) {
        console->unicode = value & 0x07;
        console->unicode_remaining = 3;
    } else if (value == 0x9b) {
        fb_console_begin_csi(console);
    } else if (value == 0x9d) {
        console->parser = FBCON_PARSER_OSC;
    } else {
        fb_console_put_glyph(console, '?');
    }
}

static void fb_console_parse_byte(fb_console_t *console, uint8_t value) {
    if (value == 0x18 || value == 0x1a) {
        console->parser = FBCON_PARSER_TEXT;
        console->unicode = 0;
        console->unicode_remaining = 0;
        return;
    }
    if (value == 0x1b && console->parser != FBCON_PARSER_TEXT &&
        console->parser != FBCON_PARSER_OSC) {
        console->parser = FBCON_PARSER_ESCAPE;
        return;
    }
    if (value < 0x20 && console->parser != FBCON_PARSER_TEXT &&
        console->parser != FBCON_PARSER_OSC &&
        console->parser != FBCON_PARSER_OSC_ESCAPE) {
        fb_console_parse_control(console, value);
        return;
    }

    switch (console->parser) {
    case FBCON_PARSER_TEXT:
        fb_console_parse_text(console, value);
        break;
    case FBCON_PARSER_ESCAPE:
        fb_console_parse_escape(console, value);
        break;
    case FBCON_PARSER_CSI:
        fb_console_parse_csi(console, value);
        break;
    case FBCON_PARSER_OSC:
        if (value == '\a')
            console->parser = FBCON_PARSER_TEXT;
        else if (value == 0x1b)
            console->parser = FBCON_PARSER_OSC_ESCAPE;
        break;
    case FBCON_PARSER_OSC_ESCAPE:
        console->parser = value == '\\' ? FBCON_PARSER_TEXT : FBCON_PARSER_OSC;
        break;
    case FBCON_PARSER_CHARSET:
        fb_console_parse_charset(console, value);
        break;
    }
}

fb_console_t *fb_console_create(const struct tty_graphics_ *framebuffer) {
    fb_console_layout_t layout;
    fb_console_t *console;
    size_t cells;

    if (fb_console_make_layout(framebuffer, &layout) < 0)
        return NULL;
    cells = layout.rows * layout.columns;
    console = calloc(1, sizeof(*console));
    if (!console)
        return NULL;
    console->cells = malloc(cells * sizeof(*console->cells));
    if (!console->cells) {
        free(console);
        return NULL;
    }
    console->row_buffer =
        malloc(layout.surface.width * sizeof(*console->row_buffer));
    if (!console->row_buffer) {
        free(console->cells);
        free(console);
        return NULL;
    }

    spin_init(&console->lock);
    console->layout = layout;
    fb_console_reset_locked(console, false);
    fb_console_fill_cells(console->cells, cells,
                          fb_console_empty_cell(console));
    return console;
}

void fb_console_destroy(fb_console_t *console) {
    if (!console)
        return;
    free(console->row_buffer);
    free(console->cells);
    free(console);
}

int fb_console_bind(fb_console_t *console,
                    const struct tty_graphics_ *framebuffer) {
    fb_console_layout_t layout;
    fb_console_cell_t *new_cells = NULL;
    fb_console_cell_t *old_cells = NULL;
    uint32_t *new_row_buffer = NULL;
    uint32_t *old_row_buffer = NULL;
    int ret;

    if (!console)
        return -EINVAL;
    ret = fb_console_make_layout(framebuffer, &layout);
    if (ret < 0)
        return ret;

    spin_lock(&console->lock);
    bool resize = layout.columns != console->layout.columns ||
                  layout.rows != console->layout.rows;
    bool resize_row_buffer =
        layout.surface.width != console->layout.surface.width;
    spin_unlock(&console->lock);
    if (resize) {
        new_cells = malloc(layout.rows * layout.columns * sizeof(*new_cells));
        if (!new_cells)
            return -ENOMEM;
    }
    if (resize_row_buffer) {
        new_row_buffer = malloc(layout.surface.width * sizeof(*new_row_buffer));
        if (!new_row_buffer) {
            free(new_cells);
            return -ENOMEM;
        }
    }

    spin_lock(&console->lock);
    if (resize) {
        size_t copied_rows = MIN(console->layout.rows, layout.rows);
        size_t copied_columns = MIN(console->layout.columns, layout.columns);
        size_t old_row = console->layout.rows - copied_rows;
        size_t new_row = layout.rows - copied_rows;
        size_t cursor_row = console->cursor_y < old_row
                                ? 0
                                : new_row + console->cursor_y - old_row;

        fb_console_fill_cells(new_cells, layout.rows * layout.columns,
                              fb_console_empty_cell(console));
        for (size_t row = 0; row < copied_rows; row++)
            memcpy(&new_cells[(new_row + row) * layout.columns],
                   &console->cells[(old_row + row) * console->layout.columns],
                   copied_columns * sizeof(*new_cells));

        old_cells = console->cells;
        console->cells = new_cells;
        console->cursor_x = MIN(console->cursor_x, layout.columns - 1);
        console->cursor_y = MIN(cursor_row, layout.rows - 1);
        console->saved_cursor_x =
            MIN(console->saved_cursor_x, layout.columns - 1);
        console->saved_cursor_y =
            MIN(console->saved_cursor_y < old_row
                    ? 0
                    : new_row + console->saved_cursor_y - old_row,
                layout.rows - 1);
        console->scroll_top = 0;
        console->scroll_bottom = layout.rows;
    }
    if (resize_row_buffer) {
        old_row_buffer = console->row_buffer;
        console->row_buffer = new_row_buffer;
    }
    console->layout = layout;
    spin_unlock(&console->lock);

    free(old_row_buffer);
    free(old_cells);
    return 0;
}

void fb_console_set_active(fb_console_t *console, bool active) {
    if (!console)
        return;

    spin_lock(&console->lock);
    if (!active && console->active)
        fb_console_draw_cursor(console, false);
    console->active = active;
    if (active)
        fb_console_refresh_locked(console);
    spin_unlock(&console->lock);
}

void fb_console_get_geometry(fb_console_t *console,
                             fb_console_geometry_t *geometry) {
    if (!console || !geometry)
        return;

    spin_lock(&console->lock);
    *geometry = (fb_console_geometry_t){
        .width = console->layout.surface.width,
        .height = console->layout.surface.height,
        .columns = console->layout.columns,
        .rows = console->layout.rows,
    };
    spin_unlock(&console->lock);
}

void fb_console_write(fb_console_t *console, const char *buffer, size_t count) {
    if (!console || !buffer || !count)
        return;

    spin_lock(&console->lock);
    fb_console_draw_cursor(console, false);
    for (size_t i = 0; i < count; i++)
        fb_console_parse_byte(console, (uint8_t)buffer[i]);
    fb_console_draw_cursor(console, true);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    spin_unlock(&console->lock);
}

void fb_console_flush(fb_console_t *console) {
    if (!console)
        return;
    spin_lock(&console->lock);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    spin_unlock(&console->lock);
}
