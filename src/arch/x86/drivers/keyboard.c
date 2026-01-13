#include <stdint.h>
#include <stdbool.h>
#include "io.h"
#include "hal.h"
#include "keyboard.h"
#include "keymap.h"

keymap_t *current_layout = 0;

bool shift_pressed = 0;
bool altgr_pressed = 0;

void keyboard_set_map(keymap_t *new_map)
{
    current_layout = new_map;
}

void keyboard_handler()
{
    uint8_t scancode = inb(0x60);

    if (scancode == 0x36 || scancode == 0x2A)
    {
        shift_pressed = 1;
    }
    else if (scancode == 0xAA || scancode == 0xB6)
    {
        shift_pressed = 0;
    }

    if (scancode & 0x80)
        return;

    if (current_layout && scancode < 128)
    {
        char c;

        if (shift_pressed)
        {
            c = current_layout->map[scancode][1];
        }
        else if (altgr_pressed)
        {
            c = current_layout->map[scancode][2];
        }
        else
        {
            c = current_layout->map[scancode][0];
        }

        if (c)
        {
            char str[2] = {c, '\0'};
            hal_kprint(str);
        }
    }
}