/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <drivers/input/ps2.h>
#include <drivers/irqchip/irqchip.h>
#include <kernel/irq.h>
#include <asm/io.h>
#include <kernel/input.h>
#include <kernel/cmdline.h>
#include "synaptics.h"
#include <kernel/board.h>
#include <kernel/lib/string.h>

#include <kernel/tty/tty_core.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/sound.h>
#include <kernel/kprintf.h>
#include <kernel/capabilities.h>
#include <kernel/lib/string.h>
#include <kernel/time.h>
#include <kernel/fb.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define ST_OBF     0x01        /* output buffer full: a byte waits at 0x60 */
#define ST_IBF     0x02        /* input buffer full: controller busy */
#define ST_AUX     0x20        /* the waiting byte came from the aux port */

static int shift_pressed = 0;
static int ctrl_pressed  = 0;
static int alt_pressed   = 0;
static int caps_lock     = 0;
static int e0_prefix     = 0;
static int altgr_pressed = 0;

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

/* Turkish Q, as printed on the keys. Keys not listed stay as in the map above. */
static const struct { uint8_t key, letter; const char *lo, *up, *alt; } tr_keys[] = {
    {0x02, 0, "1", "!", ">"},  {0x03, 0, "2", "'", "\xC2\xA3"}, {0x04, 0, "3", "^", "#"},
    {0x05, 0, "4", "+", "$"},  {0x06, 0, "5", "%", "\xC2\xBD"}, {0x07, 0, "6", "&", NULL},
    {0x08, 0, "7", "/", "{"},  {0x09, 0, "8", "(", "["},  {0x0A, 0, "9", ")", "]"},
    {0x0B, 0, "0", "=", "}"},  {0x0C, 0, "*", "?", "\\"}, {0x0D, 0, "-", "_", "|"},
    {0x10, 1, "q", "Q", "@"},  {0x12, 1, "e", "E", "\xE2\x82\xAC"},
    {0x17, 1, "\xC4\xB1", "I", NULL},
    {0x1A, 1, "\xC4\x9F", "\xC4\x9E", NULL},
    {0x1B, 1, "\xC3\xBC", "\xC3\x9C", "~"},
    {0x27, 1, "\xC5\x9F", "\xC5\x9E", NULL},
    {0x28, 1, "i", "\xC4\xB0", NULL},
    {0x29, 0, "\"", "\xC3\xA9", "<"},
    {0x2B, 0, ",", ";", "`"},
    {0x33, 1, "\xC3\xB6", "\xC3\x96", NULL},
    {0x34, 1, "\xC3\xA7", "\xC3\x87", NULL},
    {0x35, 0, ".", ":", NULL},
    {0x56, 0, "<", ">", "|"},
};

/* Returns 1 when the Turkish table had the key. */
static int tr_key(uint8_t key)
{
    for (unsigned i = 0; i < sizeof(tr_keys) / sizeof(tr_keys[0]); i++) {
        if (tr_keys[i].key != key) continue;
        const char *out;
        if (altgr_pressed) out = tr_keys[i].alt;
        else out = (tr_keys[i].letter ? shift_pressed ^ caps_lock : shift_pressed)
                   ? tr_keys[i].up : tr_keys[i].lo;
        if (out) tty_input_str(out);
        return 1;
    }
    return 0;
}


/* The Windows key pressed and let go with nothing else in between: the desk takes it
 * to switch between Home and the desktop. As a key-up so Super+key stays free. */
static int super_alone;

static void kbd_byte(uint8_t sc)
{
    backlight_unblank();

    if (sc == 0xE0) { e0_prefix = 1; return; }

    int is_super = e0_prefix && ((sc & 0x7F) == 0x5B || (sc & 0x7F) == 0x5C);
    if (is_super) {
        if (!(sc & 0x80)) super_alone = 1;
        else if (super_alone) { super_alone = 0; tty_input_str("\033[57444u"); }
    } else if (!(sc & 0x80)) {
        super_alone = 0;
    }

    if (e0_prefix) {
        e0_prefix = 0;
        int pressed = !(sc & 0x80);
        /* the shifts a keyboard fakes around Print Screen and the arrows: not keys */
        if ((sc & 0x7F) == 0x2A || (sc & 0x7F) == 0x36) return;
        {   /* extended keys have codes of their own, or an arrow reads as a keypad digit */
            static const uint8_t E0[128] = {
                [0x1C] = 96, [0x1D] = 97, [0x35] = 98, [0x38] = 100, [0x47] = 102, [0x48] = 103,
                [0x49] = 104, [0x4B] = 105, [0x4D] = 106, [0x4F] = 107, [0x50] = 108, [0x51] = 109,
                [0x52] = 110, [0x53] = 111, [0x5B] = 125, [0x5C] = 126, [0x5D] = 127,
                [0x37] = 99 /* KEY_SYSRQ: Print Screen */,
                /* media keys: their evdev names, or a program reads 'd', 'c', 'b' */
                [0x20] = 113 /* KEY_MUTE */, [0x2E] = 114 /* KEY_VOLUMEDOWN */, [0x30] = 115 /* KEY_VOLUMEUP */,
                [0x22] = 164 /* KEY_PLAYPAUSE */, [0x24] = 166 /* KEY_STOPCD */,
                [0x19] = 163 /* KEY_NEXTSONG */, [0x10] = 165 /* KEY_PREVIOUSSONG */,
                [0x5E] = 116 /* KEY_POWER */, [0x5F] = 142 /* KEY_SLEEP */, [0x63] = 143 /* KEY_WAKEUP */};
            uint8_t code = E0[sc & 0x7F];
            input_report_key(code ? code : sc & 0x7F, pressed);
        }
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
            case 0x37:                                       /* Print Screen: the desk takes it */
                tty_input_str(alt_pressed ? "\033[32;3~" : shift_pressed ? "\033[32;2~" : "\033[32~");
                break;
            case 0x52: tty_input_str(shift_pressed ? "\033[2;2~" : "\033[2~"); break;   /* Insert */
            /* the laptop's own sound keys */
            case 0x20: { static int muted_at;
                if (sound_volume()) { muted_at = sound_volume(); sound_set_volume(0); }
                else sound_set_volume(muted_at ? muted_at : 60);
                break; }
            case 0x2E: sound_set_volume(sound_volume() > 5 ? sound_volume() - 5 : 0); break;
            case 0x30: sound_set_volume(sound_volume() < 95 ? sound_volume() + 5 : 100); break;
            case 0x38: alt_pressed = 1; altgr_pressed = 1; break;
            }
        } else {
            if ((sc & 0x7F) == 0x38) { alt_pressed = 0; altgr_pressed = 0; }
        }
        return;
    }

    int pressed = !(sc & 0x80);
    uint8_t key = sc & 0x7F;

    /* Alt+Print Screen comes as SysRq (0x54, no E0): the same key to programs */
    input_report_key(key == 0x54 ? 99 : key, pressed);
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

    switch (key) {                      /* function keys, as xterm spells them */
    case 0x54: tty_input_str("\033[32;3~"); return;   /* Alt+Print Screen: the window, to the desk */
    case 0x3B: tty_input_str("\033OP"); return;
    case 0x3C: tty_input_str("\033OQ"); return;
    case 0x3D: tty_input_str("\033OR"); return;
    case 0x3E: tty_input_str(alt_pressed ? "\033[1;3S" : "\033OS"); return;
    case 0x3F: tty_input_str("\033[15~"); return;
    case 0x40: tty_input_str("\033[17~"); return;
    case 0x41: tty_input_str("\033[18~"); return;
    case 0x42: tty_input_str("\033[19~"); return;
    case 0x43: tty_input_str("\033[20~"); return;
    case 0x44: tty_input_str("\033[21~"); return;
    case 0x57: tty_input_str("\033[23~"); return;
    case 0x58: tty_input_str("\033[24~"); return;
    case 0x0F:
        if (alt_pressed) { tty_input_str("\033\t"); return; }
        break;
    case 0x2E:      /* Ctrl+Shift+C: copy */
        if (ctrl_pressed && shift_pressed) { tty_input_str("\033[99;6u"); return; }
        break;
    case 0x2F:      /* Ctrl+Shift+V: paste, as terminals take it; plain Ctrl+V stays ^V */
        if (ctrl_pressed && shift_pressed) { tty_input_str("\033[118;6u"); return; }
        break;
    }

    if (input_keymap == KEYMAP_TR && !ctrl_pressed && tr_key(key)) return;

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

/* ---- aux port: mouse or touchpad ----
 * A mouse stays in its power-on protocol, the 3-byte PS/2 packet. A Synaptics
 * touchpad goes to absolute mode, six bytes per packet, and synaptics.c makes
 * motion, taps and scrolling of them; ranix.touchpad=rel keeps it in mouse mode.
 * /dev/input/mice hands out whole packets, 3 bytes each, in arrival order.
 * A wheel step is a packet with both overflow bits set, so a reader that knows
 * nothing of it sees no motion: byte 1 the signed steps, byte 2 WHEEL_MARK. */
#define WHEEL_MARK 0x57
#define AUX_BUF 768                     /* bytes, a multiple of 3 */
static uint8_t aux_buf[AUX_BUF];
static int aux_head, aux_tail;
static uint8_t aux_pkt[3];
static int aux_n, aux_ready;
static uint32_t aux_packets, aux_dropped, aux_resync;
static spinlock_t aux_lock = SPINLOCK_INIT;
static int aux_absolute;
static uint8_t syn_pkt[6];
static int syn_n;
static syn_t syn;

/* The last raw packets, 8 bytes each: milliseconds, then the six as received.
 * /dev/input/rawpad gives them to whoever tunes the pointer against a real finger. */
#define RAW_N 8192
static uint8_t rawlog[RAW_N * 8];
static uint32_t rawlog_n;

static void aux_put(uint8_t a, uint8_t b, uint8_t c)    /* aux_lock held */
{
    int used = (aux_head - aux_tail + AUX_BUF) % AUX_BUF;
    if (used + 3 >= AUX_BUF) { aux_dropped++; return; }
    aux_buf[aux_head] = a; aux_head = (aux_head + 1) % AUX_BUF;
    aux_buf[aux_head] = b; aux_head = (aux_head + 1) % AUX_BUF;
    aux_buf[aux_head] = c; aux_head = (aux_head + 1) % AUX_BUF;
    aux_packets++;
    input_activity(input_get_source(), 1);
}

static void syn_emit(int dx, int dy, int wheel, int buttons)
{
    if (wheel) {
        wheel = wheel > 127 ? 127 : wheel < -127 ? -127 : wheel;
        aux_put((uint8_t)(0xC8 | buttons), (uint8_t)wheel, WHEEL_MARK);
    }
    do {                                /* one packet moves at most 255 */
        int sx = dx > 255 ? 255 : dx < -255 ? -255 : dx;
        int sy = dy > 255 ? 255 : dy < -255 ? -255 : dy;
        if (!wheel || sx || sy)
            aux_put((uint8_t)(0x08 | buttons | (sx < 0 ? 0x10 : 0) | (sy < 0 ? 0x20 : 0)),
                    (uint8_t)sx, (uint8_t)sy);
        dx -= sx; dy -= sy;
    } while (dx || dy);
}

/* Header bits fix bytes 0 and 3 of the six: 10xx0xxx and 11xx0xxx */
static void syn_byte(uint8_t b)
{
    uint64_t f = spin_lock_irq(&aux_lock);
    if ((syn_n == 0 && (b & 0xC8) != 0x80) || (syn_n == 3 && (b & 0xC8) != 0xC0)) {
        aux_resync++;
        syn_n = 0;
        spin_unlock_irq(&aux_lock, f);
        return;
    }
    syn_pkt[syn_n++] = b;
    if (syn_n == 6) {
        syn_n = 0;
        {
            uint8_t *r = rawlog + (rawlog_n++ % RAW_N) * 8;
            uint32_t now = (uint32_t)(kernel_ticks * (1000 / TIMER_HZ));
            r[0] = (uint8_t)now; r[1] = (uint8_t)(now >> 8);
            memcpy(r + 2, syn_pkt, 6);
        }
        syn_packet(&syn, syn_pkt, kernel_ticks * (1000 / TIMER_HZ), syn_emit);
    }
    spin_unlock_irq(&aux_lock, f);
    poll_notify();                      /* or a poller sees it at its 100 ms backstop */
}

uint32_t ps2_aux_bytes;
uint8_t  ps2_aux_last[8];
static uint8_t aux_modeback, aux_up, aux_cfg;
static uint32_t aux_caps[2];

/* For /sys/kernel/touchpad: what the pad has really said */
int ps2_aux_report(char *out, int cap)
{
    static const char hex[] = "0123456789abcdef";
    int n = 0;
    uint32_t v[7] = {(uint32_t)aux_absolute, ps2_aux_bytes, aux_packets, aux_resync,
                     (uint32_t)aux_modeback << 16 | (uint32_t)aux_up << 8 | aux_cfg,
                     aux_caps[0], aux_caps[1]};
    for (int i = 0; i < 7 && n + 9 < cap; i++) {
        for (int s = 28; s >= 0; s -= 4) out[n++] = hex[(v[i] >> s) & 15];
        out[n++] = ' ';
    }
    for (int i = 0; i < 8 && n + 3 < cap; i++) {
        out[n++] = hex[ps2_aux_last[i] >> 4];
        out[n++] = hex[ps2_aux_last[i] & 15];
    }
    out[n++] = '\n';
    return n;
}

static void aux_byte(uint8_t b)
{
    backlight_unblank();
    ps2_aux_last[ps2_aux_bytes & 7] = b;
    ps2_aux_bytes++;
    if (aux_absolute) { syn_byte(b); return; }
    uint64_t f = spin_lock_irq(&aux_lock);
    if (aux_n == 0 && !(b & 0x08)) {    /* bit 3 is always set in byte 0 */
        aux_resync++;
        spin_unlock_irq(&aux_lock, f);
        return;
    }
    aux_pkt[aux_n++] = b;
    if (aux_n == 3) {
        aux_n = 0;
        aux_put(aux_pkt[0], aux_pkt[1], aux_pkt[2]);
    }
    spin_unlock_irq(&aux_lock, f);
    poll_notify();
}

/* Bytes written into rawkbd/mice come back through the controller like a device's.
 * The writers count them here first; the next that many bytes on that line are
 * injected (source injected), as long as they come within 200 ms. A real key landing
 * in the same instant can be counted as injected; it is one byte. */
static spinlock_t inj_lock = SPINLOCK_INIT;
static int inj_n[2];                    /* 0 keyboard, 1 aux */
static uint64_t inj_at[2];

static void inj_expect(int line)
{
    uint64_t f = spin_lock_irq(&inj_lock);
    if (inj_n[line] && kernel_ticks - inj_at[line] > TIMER_HZ / 5) inj_n[line] = 0;
    inj_n[line]++;
    inj_at[line] = kernel_ticks;
    spin_unlock_irq(&inj_lock, f);
}

static int inj_take(int line)
{
    int injected = 0;
    uint64_t f = spin_lock_irq(&inj_lock);
    if (inj_n[line]) {
        if (kernel_ticks - inj_at[line] <= TIMER_HZ / 5) { inj_n[line]--; injected = 1; }
        else inj_n[line] = 0;
    }
    spin_unlock_irq(&inj_lock, f);
    return injected;
}

/* One byte from the controller, wherever the interrupt came from: both lines
 * share port 0x60, and the status register says whose byte it is. */
static void ps2_service(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & ST_OBF))
            return;
        uint8_t b = inb(PS2_DATA);
        int aux = (st & ST_AUX) != 0;
        input_set_source(inj_take(aux) ? INPUT_SRC_INJECTED : INPUT_SRC_HW);
        if (aux) {
            if (aux_ready)
                aux_byte(b);
        } else {
            kbd_byte(b);
        }
        input_set_source(INPUT_SRC_HW);
    }
}

static void kbd_irq_handler(void) { ps2_service(); }
static void aux_irq_handler(void) { ps2_service(); }

/* Polled helpers for set-up only, before the interrupts are on. Each wait is
 * bounded: a missing controller or device costs at most a few hundred ms. */
static int wait_write(void)
{
    for (int i = 0; i < 200000; i++)
        if (!(inb(PS2_STATUS) & ST_IBF))
            return 1;
    return 0;
}

static int read_aux(uint8_t *out, int spins)
{
    for (int i = 0; i < spins; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (st & ST_OBF) {
            uint8_t b = inb(PS2_DATA);
            if (st & ST_AUX) {
                *out = b;
                return 1;
            }
            kbd_byte(b);            /* a key pressed during boot stays a key */
        }
    }
    return 0;
}

static int ctl_cmd(uint8_t c)
{
    if (!wait_write()) return 0;
    outb(PS2_STATUS, c);
    return 1;
}

static int ctl_read(uint8_t *out)
{
    for (int i = 0; i < 200000; i++)
        if (inb(PS2_STATUS) & ST_OBF) {
            *out = inb(PS2_DATA);
            return 1;
        }
    return 0;
}

/* Sends one byte to the aux device; 1 when it answered ACK (0xFA). */
static int aux_send(uint8_t b)
{
    for (int tries = 0; tries < 3; tries++) {
        uint8_t r;
        if (!ctl_cmd(0xD4) || !wait_write()) return 0;
        outb(PS2_DATA, b);
        if (!read_aux(&r, 400000)) return 0;
        if (r == 0xFA) return 1;
        if (r != 0xFE) return 0;    /* 0xFE: resend */
    }
    return 0;
}

static vfs_fops_t mice_fops, rawpad_fops, rawkbd_fops;
static uint8_t aux_id, syn_info[3];
static int aux_present, aux_synaptics;

static void aux_init(void)
{
    uint8_t cfg, r;
    if (inb(PS2_STATUS) == 0xFF) {      /* no controller on the bus */
        kprintf(KERN_INFO "ps2: no i8042 controller, no aux port\n");
        return;
    }
    if (!ctl_cmd(0xA8)) return;         /* enable the aux port */
    if (!ctl_cmd(0x20) || !ctl_read(&cfg)) {
        kprintf(KERN_WARN "ps2: controller config unreadable, aux left off\n");
        return;
    }
    if (!aux_send(0xF5)) {              /* stop streaming while we ask */
        kprintf(KERN_INFO "ps2: no aux device answered\n");
        return;
    }
    aux_send(0xF6);                     /* defaults: 100 Hz, 4 counts/mm, stream */
    if (aux_send(0xF2) && read_aux(&r, 400000))
        aux_id = r;
    /* Synaptics identify: resolution 0 four times, then status request.
     * A Synaptics pad answers with 0x47 in the middle byte. */
    int ok = 1;
    for (int i = 0; i < 4 && ok; i++)
        ok = aux_send(0xE8) && aux_send(0x00);
    if (ok && aux_send(0xE9) && read_aux(&syn_info[0], 400000) &&
        read_aux(&syn_info[1], 400000) && read_aux(&syn_info[2], 400000))
        aux_synaptics = syn_info[1] == 0x47;
    aux_send(0xF6);                     /* the identify sequence changed resolution */
    if (aux_synaptics) {
        static const uint8_t qs[2] = {0x02, 0x0C};
        for (int k = 0; k < 2; k++) {
            int q = aux_send(0xE6);
            for (int shift = 6; shift >= 0 && q; shift -= 2)
                q = aux_send(0xE8) && aux_send((qs[k] >> shift) & 3);
            uint8_t m[3] = {0, 0, 0};
            if (q && aux_send(0xE9) && read_aux(&m[0], 400000) &&
                read_aux(&m[1], 400000) && read_aux(&m[2], 400000))
                aux_caps[k] = (uint32_t)m[0] << 16 | (uint32_t)m[1] << 8 | m[2];
        }
        aux_send(0xF6);
    }
    /* Absolute mode: the mode byte goes out two bits at a time as resolutions,
     * and sample rate 20 stores it. Absolute, 80 packets a second, W reported. */
    char tpm[8] = "";
    cmdline_get("ranix.touchpad", tpm, sizeof tpm);
    if (aux_synaptics && strcmp(tpm, "rel") != 0) {
        const uint8_t mode = 0xC5;   /* absolute, 80 Hz, no gestures of its own, W */
        int set = aux_send(0xE6);       /* the four that follow must be a clean run */
        for (int shift = 6; shift >= 0 && set; shift -= 2)
            set = aux_send(0xE8) && aux_send((mode >> shift) & 3);
        if (set && aux_send(0xF3) && aux_send(0x14))
            aux_absolute = 1;
        /* An image sensor counts a second finger only once told to report it */
        if (aux_absolute && (aux_caps[1] & (1u << 11 | 1u << 19))) {
            int q = aux_send(0xE6);
            for (int shift = 6; shift >= 0 && q; shift -= 2)
                q = aux_send(0xE8) && aux_send((0x03 >> shift) & 3);
            if (q && aux_send(0xF3)) aux_send(0xC8);
        }
        /* Ask for the mode back. A pad that did not take it is left relative,
         * where it is known to speak, rather than silent. */
        if (aux_absolute) {
            int q = aux_send(0xE6);
            for (int shift = 6; shift >= 0 && q; shift -= 2)
                q = aux_send(0xE8) && aux_send((0x01 >> shift) & 3);
            uint8_t m[3] = {0, 0, 0};
            q = q && aux_send(0xE9) && read_aux(&m[0], 400000) &&
                read_aux(&m[1], 400000) && read_aux(&m[2], 400000);
            aux_modeback = q ? m[2] : 0xFF;
            if (!q || m[2] != mode) {
                aux_absolute = 0;
                aux_send(0xF6);
            }
        }
    }
    /* 8 counts/mm: slow finger motion stops arriving as lone steps of two pixels */
    if (!aux_absolute && aux_send(0xE8)) aux_send(0x03);
    if (!aux_send(0xF4)) {
        kprintf(KERN_WARN "ps2: aux device did not enable streaming\n");
        return;
    }
    if (!ctl_cmd(0x20) || !ctl_read(&cfg)) return;
    cfg = (uint8_t)((cfg | 0x02) & ~0x20);  /* aux interrupt on, aux clock on */
    if (!ctl_cmd(0x60) || !wait_write()) return;
    outb(PS2_DATA, cfg);
    aux_present = 1;
    aux_up = 1;
    aux_cfg = cfg;
    if (aux_synaptics)
        kprintf(KERN_INFO "ps2: aux id %u, Synaptics touchpad v%u.%u, %s mode\n",
                aux_id, syn_info[2] & 0x0F, syn_info[0], aux_absolute ? "absolute" : "relative");
    else
        kprintf(KERN_INFO "ps2: aux id %u, mouse protocol\n", aux_id);
}

void ps2_kbd_init(void)
{
    while (inb(PS2_STATUS) & 0x01)
        inb(PS2_DATA);

    aux_init();
    irq_register(0x21, kbd_irq_handler);
    irqchip_enable_irq(0x21);
    if (aux_present) {
        irq_register(0x2C, aux_irq_handler);
        irqchip_enable_irq(0x2C);
        aux_ready = 1;
        devfs_register_device_gid("input/mice", S_IFCHR | 0660, DEVFS_GID_INPUT, 0x0D3F, &mice_fops);
        devfs_register_device_gid("input/rawpad", S_IFCHR | 0660, DEVFS_GID_INPUT, 0x0D40, &rawpad_fops);
        devfs_register_device("input/rawkbd", S_IFCHR | 0600, 0x0D41, &rawkbd_fops);
    }
}

/* ---- /dev/input/mice ---- */
static ssize_t mice_read(vfs_file_t *file, void *buf, size_t len)
{
    if (len < 3) return -22;
    while (aux_head == aux_tail) {
        if (file->flags & O_NONBLOCK) return -11;
        if (task_has_actionable_signal()) return -4;
        task_sleep();
    }
    uint8_t tmp[96];
    size_t n = 0, want = len - len % 3;
    if (want > sizeof tmp) want = sizeof tmp;
    uint64_t f = spin_lock_irq(&aux_lock);
    while (aux_head != aux_tail && n < want) {
        tmp[n++] = aux_buf[aux_tail];
        aux_tail = (aux_tail + 1) % AUX_BUF;
    }
    spin_unlock_irq(&aux_lock, f);
    if (copy_fop_to_user(buf, tmp, n)) return -14;
    return (ssize_t)n;
}

static int mice_poll(vfs_file_t *file)
{
    (void)file;
    return aux_head != aux_tail;
}

/* Self-test path, root only: each byte written goes to the controller's
 * "write aux output buffer" command (0xD3) and comes back through IRQ12 as
 * if the device had sent it. Real i8042 parts and QEMU both have it. */
static ssize_t mice_write(vfs_file_t *file, const void *buf, size_t len)
{
    (void)file;
    if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
    uint8_t tmp[24];
    if (len > sizeof tmp) len = sizeof tmp;
    if (copy_fop_from_user(tmp, buf, len)) return -14;
    for (size_t i = 0; i < len; i++) {
        if (!ctl_cmd(0xD3) || !wait_write()) return i ? (ssize_t)i : -5;
        inj_expect(1);
        outb(PS2_DATA, tmp[i]);
        for (int k = 0; k < 200000 && (inb(PS2_STATUS) & ST_OBF); k++)
            ;                           /* let IRQ12 take it before the next */
    }
    return (ssize_t)len;
}

/* Scancodes in through the controller's own "as from the keyboard" path: what a
 * person types and what this writes take the same road. For testing a live desk. */
static ssize_t rawkbd_write(vfs_file_t *file, const void *buf, size_t len)
{
    (void)file;
    if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
    uint8_t tmp[24];
    if (len > sizeof tmp) len = sizeof tmp;
    if (copy_fop_from_user(tmp, buf, len)) return -14;
    for (size_t i = 0; i < len; i++) {
        if (!ctl_cmd(0xD2) || !wait_write()) return i ? (ssize_t)i : -5;
        inj_expect(0);
        outb(PS2_DATA, tmp[i]);
        for (int k = 0; k < 200000 && (inb(PS2_STATUS) & ST_OBF); k++)
            ;
    }
    return (ssize_t)len;
}

static vfs_fops_t rawkbd_fops = { .write = rawkbd_write };

static ssize_t rawpad_read(vfs_file_t *file, void *buf, size_t len)
{
    size_t have = (rawlog_n > RAW_N ? RAW_N : rawlog_n) * 8;
    if ((size_t)file->pos >= have) return 0;
    if (len > have - (size_t)file->pos) len = have - (size_t)file->pos;
    if (copy_fop_to_user(buf, rawlog + file->pos, len)) return -14;
    file->pos += (off_t)len;
    return (ssize_t)len;
}

static vfs_fops_t rawpad_fops = { .read = rawpad_read };

static vfs_fops_t mice_fops = {
    .read  = mice_read,
    .write = mice_write,
    .poll  = mice_poll,
};

/* USB keyboards and mice speak through the same path as the PS/2 ones, so the console,
 * evdev and /dev/input/mice do not know the difference. */
static spinlock_t usb_key_lock = SPINLOCK_INIT;

void input_usb_key(uint16_t code, int down)
{
    backlight_unblank();
    uint64_t f = spin_lock_irq(&usb_key_lock);
    if (code & 0x100)
        kbd_byte(0xE0);
    kbd_byte((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
    spin_unlock_irq(&usb_key_lock, f);
}

void input_usb_mouse(int dx, int dy, int wheel, int buttons)
{
    backlight_unblank();
    uint64_t f = spin_lock_irq(&aux_lock);
    if (wheel) {          /* its own mark: "natural" scrolling is about fingers, not wheels */
        wheel = wheel > 127 ? 127 : wheel < -127 ? -127 : wheel;
        aux_put((uint8_t)(0xC8 | (buttons & 7)), (uint8_t)wheel, WHEEL_MARK + 1);
    }
    if (dx || dy || !wheel)
        syn_emit(dx, dy, 0, buttons & 7);
    spin_unlock_irq(&aux_lock, f);
    poll_notify();
}
