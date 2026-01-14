#include <stdint.h>
#include <stdbool.h>
#include "io.h"
#include "hal.h"
#include "keyboard.h"
#include "keymap.h"

keymap_t *current_layout = 0;

bool shift_pressed = false;
bool altgr_pressed = false;

void keyboard_set_map(keymap_t *new_map)
{
    current_layout = new_map;
}

void keyboard_handler()
{
    uint8_t scancode = inb(0x60);

    if (scancode == 0x2A || scancode == 0x36) 
    {
        shift_pressed = true;
        return;
    }
    else if (scancode == 0xAA || scancode == 0xB6)
    {
        shift_pressed = false;
        return;
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
            hal_kputc(c);
        }
    }
}