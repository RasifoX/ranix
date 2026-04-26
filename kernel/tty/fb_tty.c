#include <kernel/tty/fb_tty.h>
#include <kernel/fb.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include "font8x16.h"
#include "font_latin_ext.h"

#define FG_COLOR  0xFFFFFFFF
#define BG_COLOR  0xFF000000

#define FONT_W  8
#define FONT_H  16

static fb_info_t *_fb;
static u32  _cols, _rows;
static u32  _cur_col, _cur_row;
static u8  *shadow;
static u32  _dirty_y0, _dirty_y1;

static void flush_region(u32 y_start, u32 y_end)
{
    if (y_end > _fb->height) y_end = _fb->height;
    if (y_start >= y_end) return;
    u8 *dst = (u8 *)_fb->fb + y_start * _fb->pitch;
    u8 *src = shadow + y_start * _fb->pitch;
    memcpy(dst, src, (y_end - y_start) * _fb->pitch);
}

static void flush_all(void)
{
    memcpy(_fb->fb, shadow, _fb->height * _fb->pitch);
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

    dirty_flush();

    memcpy(shadow, shadow + row_bytes, (_rows - 1) * row_bytes);

    u64 bg = (u64)BG_COLOR | ((u64)BG_COLOR << 32);
    u64 *last = (u64 *)(shadow + (_rows - 1) * row_bytes);
    u32 count = row_bytes / 8;
    for (u32 i = 0; i < count; i++)
        last[i] = bg;

    flush_all();
}

/* decode one UTF-8 codepoint; returns bytes consumed */
static int utf8_next(const unsigned char *s, u32 *cp)
{
    if (s[0] < 0x80)                     { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && s[1])  { *cp = ((u32)(s[0]&0x1F)<<6)|(s[1]&0x3F); return 2; }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *cp = ((u32)(s[0]&0x0F)<<12)|((u32)(s[1]&0x3F)<<6)|(s[2]&0x3F);
        return 3;
    }
    *cp = '?'; return 1;
}

static void draw_glyph(u32 col, u32 row, u32 cp)
{
    const u8 *glyph = font_lookup(cp);
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

static void put_codepoint(u32 cp)
{
    if (cp == '\n') {
        dirty_flush(); _cur_col = 0; _cur_row++;
    } else if (cp == '\r') {
        _cur_col = 0;
    } else if (cp == '\b') {
        if (_cur_col > 0) { _cur_col--; draw_glyph(_cur_col, _cur_row, ' '); }
    } else {
        draw_glyph(_cur_col, _cur_row, cp);
        if (++_cur_col >= _cols) { _cur_col = 0; _cur_row++; }
    }
    if (_cur_row >= _rows) { scroll_up(); _cur_row = _rows - 1; }
}

void fb_tty_init(fb_info_t *fb)
{
    _fb       = fb;
    _cols     = fb->width  / FONT_W;
    _rows     = fb->height / FONT_H;
    _cur_col  = 0;
    _cur_row  = 0;
    _dirty_y0 = fb->height;
    _dirty_y1 = 0;

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
    put_codepoint((unsigned char)c);
}

void fb_tty_puts(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    while (*u) {
        u32 cp;
        int n = utf8_next(u, &cp);
        put_codepoint(cp);
        u += n;
    }
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
    _dirty_y0 = _fb->height;
    _dirty_y1 = 0;

    flush_all();
}
