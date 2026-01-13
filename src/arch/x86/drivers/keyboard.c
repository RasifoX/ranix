#include <stdint.h>
#include "io.h"
#include "hal.h"
#include "keyboard.h"
#include "keymap.h"

keymap_t *current_layout = 0;

void keyboard_set_map(keymap_t *new_map)
{
    current_layout = new_map;
}

void keyboard_handler()
{
    uint8_t scancode = inb(0x60);

    if (scancode & 0x80)
        return;

    if (current_layout && scancode < 128)
    {
        char c = current_layout->map[scancode];
        if (c)
        {
            char str[2] = {c, '\0'};
            hal_kprint(str);
        }
    }
}