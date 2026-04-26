#include <kernel/tty/fb_tty.h>
#include <kernel/fb.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include "font_spleen.h"

#define FG_COLOR  0xFFFFFFFF
#define BG_COLOR  0xFF000000

#define FONT_W  8
#define FONT_H  16

static fb_info_t *_fb;
static u32  _cols, _rows;
static u32  _cur_col, _cur_row;
static u8  *shadow;
static u32  _dirty_y0, _dirty_y1;

/* cursor state — drawn directly on fb, not in shadow */
static int  _cursor_visible  = 0;
static u32  _cursor_fb_col   = 0;
static u32  _cursor_fb_row   = 0;
static int  _cursor_on_fb    = 0;

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

static void cursor_erase_from_fb(void)
{
    if (!_cursor_on_fb || !_fb) return;
    u32 px = _cursor_fb_col * FONT_W;
    u32 py = _cursor_fb_row * FONT_H;
    for (u32 y = 0; y < FONT_H && py + y < _fb->height; y++) {
        u32 *dst = (u32 *)((u8 *)_fb->fb + (py + y) * _fb->pitch) + px;
        u32 *src = (u32 *)(shadow         + (py + y) * _fb->pitch) + px;
        for (u32 x = 0; x < FONT_W; x++) dst[x] = src[x];
    }
    _cursor_on_fb = 0;
}

static void cursor_draw_on_fb(void)
{
    if (!_cursor_visible || !_fb) return;
    u32 px = _cur_col * FONT_W;
    u32 py = _cur_row * FONT_H;
    for (u32 y = 0; y < FONT_H && py + y < _fb->height; y++) {
        u32 *line = (u32 *)((u8 *)_fb->fb + (py + y) * _fb->pitch) + px;
        for (u32 x = 0; x < FONT_W; x++) line[x] ^= 0x00FFFFFF;
    }
    _cursor_fb_col = _cur_col;
    _cursor_fb_row = _cur_row;
    _cursor_on_fb  = 1;
}

static void dirty_flush(void)
{
    if (_cursor_visible) {
        u32 py = _cur_row * FONT_H;
        if (py < _dirty_y0) _dirty_y0 = py;
        if (py + FONT_H > _dirty_y1) _dirty_y1 = py + FONT_H;
    }
    if (_dirty_y1 > _dirty_y0) {
        cursor_erase_from_fb();
        flush_region(_dirty_y0, _dirty_y1);
        cursor_draw_on_fb();
    }
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

static void put_codepoint(u32 cp);

static void draw_glyph(u32 col, u32 row, u32 cp)
{
    const u8 *glyph = spleen_lookup(cp);
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

static u32  utf8_state = 0;
static u32  utf8_cp = 0;

void fb_tty_putchar(char c)
{
    unsigned char u = (unsigned char)c;
    if (u < 0x80) {
        utf8_state = 0;
        put_codepoint(u);
    } else if (u >= 0xC0 && u <= 0xDF) {
        utf8_cp = u & 0x1F;
        utf8_state = 1;
    } else if (u >= 0xE0 && u <= 0xEF) {
        utf8_cp = u & 0x0F;
        utf8_state = 2;
    } else if (u >= 0x80 && u <= 0xBF) {
        if (utf8_state > 0) {
            utf8_cp = (utf8_cp << 6) | (u & 0x3F);
            if (--utf8_state == 0) put_codepoint(utf8_cp);
        } else {
            put_codepoint('?');
        }
    } else {
        utf8_state = 0;
        put_codepoint('?');
    }
}

void fb_tty_puts(const char *s)
{
    while (*s) fb_tty_putchar(*s++);
}

void fb_tty_write(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        fb_tty_putchar(buf[i]);
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

void fb_tty_show_cursor(int show)
{
    if (!show) cursor_erase_from_fb();
    _cursor_visible = show;
}

void fb_tty_erase_eol(void)
{
    u32 saved = _cur_col;
    for (u32 c = _cur_col; c < _cols; c++)
        draw_glyph(c, _cur_row, ' ');
    _cur_col = saved;
}
