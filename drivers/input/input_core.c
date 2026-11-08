#include <kernel/input.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>
#include <kernel/hal.h>
#include <kernel/kout.h>
#include <kernel/mm/heap.h>

int input_keymap = KEYMAP_US;

#define EV_BUF_SIZE 256

#define IOC_TYPE(c)  (((c) >> 8) & 0xff)
#define IOC_NR(c)    ((c) & 0xff)
#define IOC_SIZE(c)  (((c) >> 16) & 0x3fff)

static struct input_event event_buf[EV_BUF_SIZE];
static int ev_head = 0;
static int ev_tail = 0;
static spinlock_t ev_lock = SPINLOCK_INIT;

extern volatile uint64_t kernel_ticks;
#include <kernel/board.h>   /* TIMER_HZ: never redefine it locally */

/* ---- input source: hw (a device) or injected (written by software) ---- */
static volatile int input_source = INPUT_SRC_HW;
static spinlock_t src_lock = SPINLOCK_INIT;
static struct {
    uint32_t hw_keys, hw_ptr, inj_keys, inj_ptr, inj_marks;
    uint64_t hw_at, inj_at;           /* kernel_ticks of the last one, 0 = never */
    int last;                           /* -1 none, else INPUT_SRC_* */
    uint64_t gen, hw_gen;               /* gen: anything changed; hw_gen: a person did something */
} st = { .last = -1 };

void input_set_source(int src) { input_source = src; }
int  input_get_source(void) { return input_source; }

void input_activity(int src, int pointer)
{
    uint64_t f = spin_lock_irq(&src_lock);
    if (src == INPUT_SRC_INJECTED) {
        if (pointer) st.inj_ptr++; else st.inj_keys++;
        st.inj_at = kernel_ticks ? kernel_ticks : 1;
    } else {
        if (pointer) st.hw_ptr++; else st.hw_keys++;
        st.hw_at = kernel_ticks ? kernel_ticks : 1;
        st.hw_gen++;
    }
    st.last = src;
    st.gen++;
    spin_unlock_irq(&src_lock, f);
    if (src != INPUT_SRC_INJECTED) poll_notify();      /* /dev/input/source readers */
}

static void ev_put(uint16_t type, uint16_t code, int32_t value)   /* ev_lock held */
{
    int next = (ev_head + 1) % EV_BUF_SIZE;
    if (next == ev_tail) return;
    struct input_event *e = &event_buf[ev_head];
    e->time.tv_sec  = kernel_ticks / TIMER_HZ;
    e->time.tv_usec = (kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ);
    e->type  = type;
    e->code  = code;
    e->value = value;
    ev_head  = next;
}

void input_report_key(uint16_t code, int32_t value)
{
    int src = input_source;
    input_activity(src, 0);
    uint64_t f = spin_lock_irq(&ev_lock);
    if (src == INPUT_SRC_INJECTED)
        ev_put(EV_MSC, MSC_RANIX_SOURCE, 1);
    ev_put(EV_KEY, code, value);
    spin_unlock_irq(&ev_lock, f);
}

void input_sync(void)
{
    uint64_t f = spin_lock_irq(&ev_lock);
    ev_put(EV_SYN, 0, 0);
    spin_unlock_irq(&ev_lock, f);
}

/* Map ASCII/escape sequences from serial input to evdev keycodes */
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
        if (task_has_actionable_signal()) return -4;
        task_sleep();
    }

    /* Drain into a kernel buffer under the IRQ-off lock, then copy_to_user once
     * outside it — a faulting user store cannot run in the lock window. */
    struct input_event evs[8];
    int n = 0;
    uint64_t f = spin_lock_irq(&ev_lock);
    while (ev_head != ev_tail && n < (int)(sizeof(evs) / sizeof(evs[0])) &&
           (size_t)(n + 1) * sizeof(struct input_event) <= len) {
        evs[n++] = event_buf[ev_tail];
        ev_tail = (ev_tail + 1) % EV_BUF_SIZE;
    }
    spin_unlock_irq(&ev_lock, f);

    if (n && copy_fop_to_user(buf, evs, (size_t)n * sizeof(struct input_event)))
        return -14;
    return n * (int)sizeof(struct input_event);
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
        uint8_t kb[64];
        if (size > (int)sizeof(kb)) size = (int)sizeof(kb);
        memset(kb, 0, (size_t)size);
        if (ev_type == 0 && size >= 1)
            kb[0] = (1 << EV_SYN) | (1 << EV_KEY);
        if (copy_to_user(buf, kb, (size_t)size)) return -14;
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

/* ---- /dev/input/source ----
 * One line of counters, no key contents:
 *   hw_keys=N hw_ptr=N inj_keys=N inj_ptr=N inj_marks=N hw_ms=MS inj_ms=MS last=hw|injected|none
 * *_ms is how long ago the last one was (-1: never). A read gives the line to EOF;
 * once consumed the next read gives a fresh line if anything changed, else EOF.
 * poll: readable when a person (source hw) did something since this fd's last read,
 * so a program that injects input can step back the moment a person touches a key.
 * write "injected": a program that delivers synthetic input itself (not through
 * rawkbd/mice, e.g. the desktop on behalf of a client) says so; counted as an
 * injected key. The word is the only thing accepted: nothing written here can make
 * input count as hw. Mode 0660 root:input, like the other input nodes: the timing
 * of key presses is not for everyone. */
struct srcfile { uint64_t seen_gen, seen_hw; size_t len; char line[224]; };

static void src_snapshot(struct srcfile *sf)
{
    kout_t o; kout_init(&o, sf->line, sizeof sf->line);
    uint64_t f = spin_lock_irq(&src_lock);
    typeof(st) c = st;
    uint64_t now = kernel_ticks;
    spin_unlock_irq(&src_lock, f);
    kout_s(&o, "hw_keys="); kout_u(&o, c.hw_keys);
    kout_s(&o, " hw_ptr="); kout_u(&o, c.hw_ptr);
    kout_s(&o, " inj_keys="); kout_u(&o, c.inj_keys);
    kout_s(&o, " inj_ptr="); kout_u(&o, c.inj_ptr);
    kout_s(&o, " inj_marks="); kout_u(&o, c.inj_marks);
    kout_s(&o, " hw_ms="); kout_i(&o, c.hw_at ? (int64_t)((now - c.hw_at) * 1000 / TIMER_HZ) : -1);
    kout_s(&o, " inj_ms="); kout_i(&o, c.inj_at ? (int64_t)((now - c.inj_at) * 1000 / TIMER_HZ) : -1);
    kout_s(&o, " last="); kout_s(&o, c.last < 0 ? "none" : c.last == INPUT_SRC_INJECTED ? "injected" : "hw");
    kout_c(&o, '\n');
    sf->len = o.len;
    sf->seen_gen = c.gen;
    sf->seen_hw = c.hw_gen;
}

static int src_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
    (void)in; (void)flags;
    struct srcfile *sf = kmalloc(sizeof *sf);
    if (!sf) return -12;
    memset(sf, 0, sizeof *sf);
    sf->seen_gen = (uint64_t)-1;
    f->priv = sf;
    return 0;
}

static int src_close(vfs_file_t *f)
{
    if (f->priv) { kfree(f->priv); f->priv = 0; }
    return 0;
}

static ssize_t src_read(vfs_file_t *f, void *buf, size_t len)
{
    struct srcfile *sf = f->priv;
    if (!sf) return -9;
    if (sf->seen_gen == (uint64_t)-1 || ((size_t)f->pos >= sf->len && sf->seen_gen != st.gen)) {
        src_snapshot(sf);
        f->pos = 0;
    }
    if ((size_t)f->pos >= sf->len) return 0;
    size_t n = sf->len - (size_t)f->pos;
    if (n > len) n = len;
    if (copy_fop_to_user(buf, sf->line + f->pos, n)) return -14;
    f->pos += (off_t)n;
    return (ssize_t)n;
}

static ssize_t src_write(vfs_file_t *f, const void *buf, size_t len)
{
    (void)f;
    char k[16] = {0};
    size_t n = len < sizeof k - 1 ? len : sizeof k - 1;
    if (copy_fop_from_user(k, buf, n)) return -14;
    if (strncmp(k, "injected", 8) != 0 || (k[8] && k[8] != '\n')) return -22;
    uint64_t fl = spin_lock_irq(&src_lock);
    st.inj_marks++;
    st.inj_at = kernel_ticks ? kernel_ticks : 1;
    st.last = INPUT_SRC_INJECTED;
    st.gen++;
    spin_unlock_irq(&src_lock, fl);
    return (ssize_t)len;
}

static int src_poll(vfs_file_t *f)
{
    struct srcfile *sf = f->priv;
    if (!sf || sf->seen_gen == (uint64_t)-1) return 1;
    if ((size_t)f->pos < sf->len) return 1;
    return sf->seen_hw != st.hw_gen;
}

static vfs_fops_t src_fops = {
    .open = src_open, .close = src_close, .read = src_read, .write = src_write, .poll = src_poll,
};

void input_core_init(void)
{
    devfs_register_device_gid("input/event0", S_IFCHR | 0660, DEVFS_GID_INPUT, 0x0D60, &evdev_fops);
    devfs_register_device_gid("input/source", S_IFCHR | 0660, DEVFS_GID_INPUT, 0x0D61, &src_fops);
}
