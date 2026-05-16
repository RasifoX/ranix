#include <drivers/input/ps2.h>
#include <kernel/irq.h>
#include <asm/io.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/input.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64

#define KBD_BUF_SIZE 256
static char kbd_buffer[KBD_BUF_SIZE];
static int  kbd_head = 0;
static int  kbd_tail = 0;

static int shift_pressed = 0;
static int ctrl_pressed  = 0;
static int e0_prefix     = 0;

/* Scancode set 1 ASCII mappings */
static const char map_lower[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char map_upper[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static void push_str(const char *s)
{
    while (*s) {
        int next = (kbd_head + 1) % KBD_BUF_SIZE;
        if (next == kbd_tail) break;
        kbd_buffer[kbd_head] = *s++;
        kbd_head = next;
    }
}

static void kbd_irq_handler(void)
{
    uint8_t sc = inb(PS2_DATA_PORT);

    if (sc == 0xE0) { e0_prefix = 1; return; }

    if (e0_prefix) {
        e0_prefix = 0;
        input_report_key(sc & 0x7F, !(sc & 0x80));
        input_sync();
        if (!(sc & 0x80)) {
            switch (sc) {
            case 0x48: push_str("\033[A"); break; /* Up    */
            case 0x50: push_str("\033[B"); break; /* Down  */
            case 0x4D: push_str("\033[C"); break; /* Right */
            case 0x4B: push_str("\033[D"); break; /* Left  */
            case 0x47: push_str("\033[H"); break; /* Home  */
            case 0x4F: push_str("\033[F"); break; /* End   */
            case 0x49: push_str("\033[5~"); break; /* PgUp */
            case 0x51: push_str("\033[6~"); break; /* PgDn */
            case 0x53: push_str("\033[3~"); break; /* Del  */
            }
        }
        return;
    }

    int pressed = (sc & 0x80) ? 0 : 1;
    input_report_key(sc & 0x7F, pressed);
    input_sync();

    if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; return; }
    if (sc == 0x1D) { ctrl_pressed = 1; return; }
    if (sc == 0x9D) { ctrl_pressed = 0; return; }

    if (sc & 0x80) return;

    if (sc < sizeof(map_lower)) {
        char c;
        if (ctrl_pressed) {
            char base = map_lower[sc];
            c = (base >= 'a' && base <= 'z') ? (char)(base - 'a' + 1) : 0;
        } else {
            c = shift_pressed ? map_upper[sc] : map_lower[sc];
        }
        if (c) {
            int next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buffer[kbd_head] = c;
                kbd_head = next;
            }
        }
    }
}

#include <drivers/irqchip/irqchip.h>

void ps2_kbd_init(void)
{
    /* Clear current buffer on controller */
    while (inb(PS2_STATUS_PORT) & 0x01) {
        inb(PS2_DATA_PORT);
    }

    /* PIC IRQ 1 is mapped to interrupt 0x21 */
    irq_register(0x21, kbd_irq_handler);
    irqchip_enable_irq(0x21);
}

int ps2_kbd_has_data(void)
{
    return kbd_head != kbd_tail;
}

char ps2_kbd_getc(void)
{
    if (kbd_head == kbd_tail) {
        return 0;
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}
