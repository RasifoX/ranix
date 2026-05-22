#include <drivers/input/ps2.h>
#include <drivers/irqchip/irqchip.h>
#include <kernel/irq.h>
#include <asm/io.h>
#include <kernel/input.h>
#include <kernel/tty/tty_core.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

static int shift_pressed = 0;
static int ctrl_pressed  = 0;
static int alt_pressed   = 0;
static int caps_lock     = 0;
static int e0_prefix     = 0;

static const char map_lower[] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t','q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`',  0,  '\\',
    'z', 'x',  'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '
};

static const char map_upper[] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t','Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A',  'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',  0,  '|',
    'Z', 'X',  'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '
};

static void kbd_irq_handler(void)
{
    uint8_t sc = inb(PS2_DATA);

    if (sc == 0xE0) { e0_prefix = 1; return; }

    if (e0_prefix) {
        e0_prefix = 0;
        int pressed = !(sc & 0x80);
        input_report_key(sc & 0x7F, pressed);
        input_sync();
        if (pressed) {
            switch (sc & 0x7F) {
            case 0x48: tty_input_str("\033[A"); break;
            case 0x50: tty_input_str("\033[B"); break;
            case 0x4D: tty_input_str("\033[C"); break;
            case 0x4B: tty_input_str("\033[D"); break;
            case 0x47: tty_input_str("\033[H"); break;
            case 0x4F: tty_input_str("\033[F"); break;
            case 0x49: tty_input_str("\033[5~"); break;
            case 0x51: tty_input_str("\033[6~"); break;
            case 0x53: tty_input_str("\033[3~"); break;
            case 0x38: alt_pressed = 1; break;
            }
        } else {
            if ((sc & 0x7F) == 0x38) alt_pressed = 0;
        }
        return;
    }

    int pressed = !(sc & 0x80);
    uint8_t key = sc & 0x7F;

    input_report_key(key, pressed);
    input_sync();

    switch (key) {
    case 0x2A: case 0x36: shift_pressed = pressed; return;
    case 0x1D:            ctrl_pressed  = pressed; return;
    case 0x38:            alt_pressed   = pressed; return;
    case 0x3A:
        if (pressed) caps_lock ^= 1;
        return;
    }

    if (!pressed) return;

    if (key < sizeof(map_lower)) {
        char c;
        if (ctrl_pressed) {
            char base = map_lower[key];
            c = (base >= 'a' && base <= 'z') ? (char)(base - 'a' + 1) : 0;
        } else {
            int upper = shift_pressed ^ caps_lock;
            c = upper ? map_upper[key] : map_lower[key];
        }
        if (c) tty_input_char(c);
    }
}

void ps2_kbd_init(void)
{
    while (inb(PS2_STATUS) & 0x01)
        inb(PS2_DATA);

    irq_register(0x21, kbd_irq_handler);
    irqchip_enable_irq(0x21);
}
