#include <kernel/input.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>

#define EV_BUF_SIZE 256

#define IOC_TYPE(c)  (((c) >> 8) & 0xff)
#define IOC_NR(c)    ((c) & 0xff)
#define IOC_SIZE(c)  (((c) >> 16) & 0x3fff)

static struct input_event event_buf[EV_BUF_SIZE];
static int ev_head = 0;
static int ev_tail = 0;
static spinlock_t ev_lock = SPINLOCK_INIT;

extern volatile uint64_t kernel_ticks;
#define TIMER_HZ 100

void input_report_key(uint16_t code, int32_t value)
{
    uint64_t f = spin_lock_irq(&ev_lock);
    int next = (ev_head + 1) % EV_BUF_SIZE;
    if (next != ev_tail) {
        struct input_event *e = &event_buf[ev_head];
        e->time.tv_sec  = kernel_ticks / TIMER_HZ;
        e->time.tv_usec = (kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ);
        e->type  = EV_KEY;
        e->code  = code;
        e->value = value;
        ev_head  = next;
    }
    spin_unlock_irq(&ev_lock, f);
}

void input_sync(void)
{
    uint64_t f = spin_lock_irq(&ev_lock);
    int next = (ev_head + 1) % EV_BUF_SIZE;
    if (next != ev_tail) {
        struct input_event *e = &event_buf[ev_head];
        e->time.tv_sec  = kernel_ticks / TIMER_HZ;
        e->time.tv_usec = (kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ);
        e->type  = EV_SYN;
        e->code  = 0;
        e->value = 0;
        ev_head  = next;
    }
    spin_unlock_irq(&ev_lock, f);
}

/* Map ASCII/escape sequences from serial input to Linux keycodes for evdev */
void input_inject_ascii(uint8_t c)
{
    static int esc_st = 0;

    uint16_t key = 0;
    int      pressed = 1;

    if (esc_st == 1) {
        if (c == '[') { esc_st = 2; return; }
        esc_st = 0;
        /* bare ESC */
        input_report_key(1 /* KEY_ESC */, 1);
        input_sync();
        input_report_key(1, 0);
        input_sync();
        if (c == 0x1B) { esc_st = 1; return; }
    } else if (esc_st == 2) {
        esc_st = 0;
        switch (c) {
        case 'A': key = 103; break; /* KEY_UP    */
        case 'B': key = 108; break; /* KEY_DOWN  */
        case 'C': key = 106; break; /* KEY_RIGHT */
        case 'D': key = 105; break; /* KEY_LEFT  */
        default:  return;
        }
        input_report_key(key, 1); input_sync();
        input_report_key(key, 0); input_sync();
        return;
    }

    if (c == 0x1B) { esc_st = 1; return; }

    switch (c) {
    case '\r': case '\n': key = 28; break; /* KEY_ENTER */
    case ' ':             key = 57; break; /* KEY_SPACE */
    case 0x7F: case 0x08: key = 14; break; /* KEY_BACKSPACE */
    case '\t':            key = 15; break; /* KEY_TAB */
    default: return;
    }

    if (key) {
        input_report_key(key, pressed); input_sync();
        input_report_key(key, 0);       input_sync();
    }
}

static ssize_t evdev_read(vfs_file_t *file, void *buf, size_t len)
{
    (void)file;
    if (len < sizeof(struct input_event)) return -22;

    while (ev_head == ev_tail) {
        if (file->flags & O_NONBLOCK) return -11;
        task_sleep();
    }

    uint64_t f = spin_lock_irq(&ev_lock);
    uint8_t *dst = buf;
    int copied = 0;
    while (ev_head != ev_tail && len >= sizeof(struct input_event)) {
        memcpy(dst, &event_buf[ev_tail], sizeof(struct input_event));
        dst    += sizeof(struct input_event);
        len    -= sizeof(struct input_event);
        copied += sizeof(struct input_event);
        ev_tail = (ev_tail + 1) % EV_BUF_SIZE;
    }
    spin_unlock_irq(&ev_lock, f);
    return copied;
}

static int evdev_poll(vfs_file_t *file)
{
    (void)file;
    return (ev_head != ev_tail) ? 1 : 0;
}

static int evdev_ioctl(vfs_file_t *file, uint32_t cmd, uint64_t arg)
{
    (void)file;
    if (IOC_TYPE(cmd) != 'E') return -22;

    int nr   = IOC_NR(cmd);
    int size = (int)IOC_SIZE(cmd);
    uint8_t *buf = (uint8_t *)(uintptr_t)arg;

    if (nr >= 0x20 && nr <= 0x3f) {
        int ev_type = nr - 0x20;
        if (!buf || size <= 0) return -22;
        memset(buf, 0, (size_t)size);
        if (ev_type == 0 && size >= 1)
            buf[0] = (1 << EV_SYN) | (1 << EV_KEY);
        return 0;
    }
    if (nr == 0x90) return 0; /* EVIOCGRAB */
    return -25;
}

static vfs_fops_t evdev_fops = {
    .read  = evdev_read,
    .poll  = evdev_poll,
    .ioctl = evdev_ioctl,
};

void input_core_init(void)
{
    devfs_register_device("input/event0", S_IFCHR | 0444, 0x0D60, &evdev_fops);
}
