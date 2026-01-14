#include <stdint.h>
#include <stddef.h>
#include "vga_text.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

enum vga_color
{
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_WHITE = 15,
};

static inline uint16_t vga_entry(unsigned char uc, uint8_t color)
{
    return (uint16_t)uc | (uint16_t)color << 8;
}

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t *terminal_buffer;

void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = VGA_COLOR_WHITE | VGA_COLOR_BLACK << 4;
    terminal_buffer = (uint16_t *)VGA_MEMORY;

    for (size_t y = 0; y < VGA_HEIGHT; y++)
    {
        for (size_t x = 0; x < VGA_WIDTH; x++)
        {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
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
        if (terminal_column > 0) terminal_column--;
    }
    else
    {
        const size_t index = terminal_row * VGA_WIDTH + terminal_column;
        terminal_buffer[index] = vga_entry(c, terminal_color);
        terminal_column++;
    }

    if (terminal_column >= VGA_WIDTH)
    {
        terminal_column = 0;
        terminal_row++;
    }

    if (terminal_row >= VGA_HEIGHT)
    {
        terminal_row = 0;
    }
}

void terminal_print(const char *str)
{
    for (size_t i = 0; str[i] != '\0'; i++)
        terminal_putchar(str[i]);
}