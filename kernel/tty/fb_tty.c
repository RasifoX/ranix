#include <kernel/tty/fb_tty.h>
#include <kernel/fb.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include "font8x16.h"

#define FG_COLOR  0xFFFFFFFF
#define BG_COLOR  0xFF000000

#define FONT_W  8
#define FONT_H  16

static fb_info_t *_fb;
static u32  _cols, _rows;
static u32  _cur_col, _cur_row;
static u8  *shadow;
static u32  _dirty_y0, _dirty_y1;
static u32  _scroll_y;

static void flush_region(u32 y_start, u32 y_end)
{
    if (y_end > _fb->height) y_end = _fb->height;
    if (y_start >= y_end) return;
    u32 abs_y = _scroll_y + y_start;
    u8 *dst = (u8 *)_fb->fb + abs_y * _fb->pitch;
    u8 *src = shadow + y_start * _fb->pitch;
    memcpy(dst, src, (y_end - y_start) * _fb->pitch);
}

static void flush_all(void)
{
    u8 *dst = (u8 *)_fb->fb + _scroll_y * _fb->pitch;
    memcpy(dst, shadow, _fb->height * _fb->pitch);
}

static void dirty_flush(void)
{
    if (_dirty_y1 > _dirty_y0)
        flush_region(_dirty_y0, _dirty_y1);
    _dirty_y0 = _fb->height;
    _dirty_y1 = 0;
}

static void scroll_up(void)
{
    u32 row_bytes = FONT_H * _fb->pitch;
    u32 max_scroll = _fb->virt_height - _fb->height;

    /* flush any pending dirty content before modifying shadow */
    dirty_flush();

    memcpy(shadow, shadow + row_bytes, (_rows - 1) * row_bytes);

    u64 bg = (u64)BG_COLOR | ((u64)BG_COLOR << 32);
    u64 *last = (u64 *)(shadow + (_rows - 1) * row_bytes);
    u32 count = row_bytes / 8;
    for (u32 i = 0; i < count; i++)
        last[i] = bg;

    _scroll_y += FONT_H;

    if (_scroll_y > max_scroll) {
        _scroll_y = 0;
        flush_all();
    } else {
        /* flush shifted content to the new viewport position */
        u8 *dst = (u8 *)_fb->fb + _scroll_y * _fb->pitch;
        memcpy(dst, shadow, (_rows - 1) * row_bytes);
        /* clear new bottom row in virtual FB */
        u64 *fb_last = (u64 *)((u8 *)_fb->fb + (_scroll_y + (_rows-1)*FONT_H) * _fb->pitch);
        for (u32 i = 0; i < count; i++)
            fb_last[i] = bg;
    }

    if (_fb->set_virt_offset)
        _fb->set_virt_offset(0, _scroll_y);
}

static void draw_char(u32 col, u32 row, char c)
{
    if (c < 32 || c > 127) c = '?';
    const u8 *glyph = font8x16[c - 32];
    u32 px = col * FONT_W;
    u32 py = row * FONT_H;

    for (u32 y = 0; y < FONT_H; y++) {
        u32 *line = (u32 *)(shadow + (py + y) * _fb->pitch);
        u8 bits = glyph[y];
        for (u32 x = 0; x < FONT_W; x++)
            line[px + x] = (bits & (0x80 >> x)) ? FG_COLOR : BG_COLOR;
    }

    if (py < _dirty_y0) _dirty_y0 = py;
    if (py + FONT_H > _dirty_y1) _dirty_y1 = py + FONT_H;
}

void fb_tty_init(fb_info_t *fb)
{
    _fb       = fb;
    _cols     = fb->width  / FONT_W;
    _rows     = fb->height / FONT_H;
    _cur_col  = 0;
    _cur_row  = 0;
    _scroll_y = 0;
    _dirty_y0 = fb->height;
    _dirty_y1 = 0;

    /* allocate shadow buffer from heap based on actual resolution */
    u32 shadow_size = fb->pitch * fb->height;
    shadow = kmalloc(shadow_size);
    if (!shadow) return;

    u64 bg = (u64)BG_COLOR | ((u64)BG_COLOR << 32);
    u64 *p = (u64 *)shadow;
    for (u32 i = 0; i < shadow_size / 8; i++)
        p[i] = bg;

    flush_all();
}

void fb_tty_flush(void) { dirty_flush(); }

void fb_tty_putchar(char c)
{
    if (c == '\n') {
        dirty_flush();
        _cur_col = 0;
        _cur_row++;
    } else if (c == '\r') {
        _cur_col = 0;
    } else if (c == '\b') {
        if (_cur_col > 0) {
            _cur_col--;
            draw_char(_cur_col, _cur_row, ' ');
        }
    } else {
        draw_char(_cur_col, _cur_row, c);
        if (++_cur_col >= _cols) {
            _cur_col = 0;
            _cur_row++;
        }
    }

    if (_cur_row >= _rows) {
        scroll_up();
        _cur_row = _rows - 1;
    }
}

void fb_tty_puts(const char *s)
{
    while (*s) fb_tty_putchar(*s++);
}

void fb_tty_clear(void)
{
    u32 shadow_size = _fb->pitch * _fb->height;
    u64 bg = (u64)BG_COLOR | ((u64)BG_COLOR << 32);

    u64 *p = (u64 *)shadow;
    for (u32 i = 0; i < shadow_size / 8; i++)
        p[i] = bg;

    _cur_col  = 0;
    _cur_row  = 0;
    _scroll_y = 0;
    _dirty_y0 = _fb->height;
    _dirty_y1 = 0;

    /* clear entire virtual FB and reset GPU offset */
    u32 vfb_size = _fb->pitch * _fb->virt_height;
    u64 *vfb = (u64 *)_fb->fb;
    for (u32 i = 0; i < vfb_size / 8; i++)
        vfb[i] = bg;

    if (_fb->set_virt_offset)
        _fb->set_virt_offset(0, 0);
}
