#include <stdint.h>
#include <stdbool.h>
#include "io.h"
#include "hal.h"
#include "keyboard.h"
#include "keymap.h"

keymap_t *current_layout = 0;

static bool shift_pressed = false;
static bool altgr_pressed = false;

static volatile char kb_buffer[KB_BUFFER_SIZE];
static volatile uint16_t kb_head = 0;
static volatile uint16_t kb_tail = 0;


void keyboard_init(void)
{
    kb_head = 0;
    kb_tail = 0;
}

void keyboard_push_char(char c) {
    uint16_t next_head = (kb_head + 1) % KB_BUFFER_SIZE;

    if (next_head != kb_tail) {
        kb_buffer[kb_head] = c;
        kb_head = next_head;
    }
}

char keyboard_get_char(void)
{
    if (kb_head == kb_tail)
    {
        return 0;
    }

    char c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return c;
}

int keyboard_has_chars(void)
{
    return kb_head != kb_tail;
}

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
    if (scancode == 0xAA || scancode == 0xB6)
    {
        shift_pressed = false;
        return;
    }
    if (scancode & 0x80)
        return;

    if (current_layout && scancode < 128)
    {
        char c = 0;

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
            keyboard_push_char(c);
        }
    }
}