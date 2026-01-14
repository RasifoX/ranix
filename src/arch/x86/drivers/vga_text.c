#include "vga_text.h"
#include "io.h"
#include "string.h"
#include <stddef.h>

#define BIOS_BDA_WIDTH_ADDR 0x044A
#define BIOS_BDA_PORT_ADDR 0x0463

#define VGA_MEMORY 0xB8000

static size_t vga_width = 80;
static size_t vga_height = 25;
static uint16_t vga_crtc_port = 0x3D4;

enum vga_color
{
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg)
{
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color)
{
    return (uint16_t)uc | (uint16_t)color << 8;
}

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t *terminal_buffer;

void terminal_enable_cursor(uint8_t cursor_start, uint8_t cursor_end)
{
    outb(vga_crtc_port, 0x0A);
    outb(vga_crtc_port + 1, (inb(vga_crtc_port + 1) & 0xC0) | cursor_start);

    outb(vga_crtc_port, 0x0B);
    outb(vga_crtc_port + 1, (inb(vga_crtc_port + 1) & 0xE0) | cursor_end);
}

void terminal_update_cursor(int x, int y)
{
    uint16_t pos = y * vga_width + x;

    outb(vga_crtc_port, 0x0F);
    outb(vga_crtc_port + 1, (uint8_t)(pos & 0xFF));
    outb(vga_crtc_port, 0x0E);
    outb(vga_crtc_port + 1, (uint8_t)((pos >> 8) & 0xFF));
}

static void terminal_scroll()
{
    memcpy(terminal_buffer, terminal_buffer + vga_width, (vga_height - 1) * vga_width * 2);

    uint16_t blank = vga_entry(' ', terminal_color);
    for (size_t x = 0; x < vga_width; x++)
    {
        terminal_buffer[(vga_height - 1) * vga_width + x] = blank;
    }

    terminal_row = vga_height - 1;
}

void terminal_initialize(void)
{
    uint16_t bda_width = *(volatile uint16_t *)BIOS_BDA_WIDTH_ADDR;
    uint16_t bda_port = *(volatile uint16_t *)BIOS_BDA_PORT_ADDR;

    if (bda_width > 0 && bda_width <= 255)
    {
        vga_width = bda_width;
    }

    if (bda_port != 0)
    {
        vga_crtc_port = bda_port;
    }

    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t *)VGA_MEMORY;

    terminal_clear();

    terminal_enable_cursor(14, 15);
}

void terminal_set_color(uint8_t color)
{
    terminal_color = color;
}

void terminal_clear(void)
{
    for (size_t y = 0; y < vga_height; y++)
    {
        for (size_t x = 0; x < vga_width; x++)
        {
            const size_t index = y * vga_width + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    terminal_row = 0;
    terminal_column = 0;
    terminal_update_cursor(0, 0);
}

void terminal_putchar(char c)
{
    if (c == '\n')
    {
        terminal_column = 0;
        terminal_row++;
    }
    else if (c == '\b')
    {
        if (terminal_column > 0)
        {
            terminal_column--;
        }
        else if (terminal_row > 0)
        {
            terminal_row--;
            terminal_column = vga_width - 1;
        }
        const size_t index = terminal_row * vga_width + terminal_column;
        terminal_buffer[index] = vga_entry(' ', terminal_color);
    }
    else
    {
        const size_t index = terminal_row * vga_width + terminal_column;
        terminal_buffer[index] = vga_entry(c, terminal_color);
        terminal_column++;
    }

    if (terminal_column >= vga_width)
    {
        terminal_column = 0;
        terminal_row++;
    }

    if (terminal_row >= vga_height)
    {
        terminal_scroll();
    }

    terminal_update_cursor(terminal_column, terminal_row);
}

void terminal_write(const char *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_print(const char *data)
{
    terminal_write(data, strlen(data));
}