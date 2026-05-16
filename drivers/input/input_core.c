#include <kernel/input.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>

#define EV_BUF_SIZE 256

#define IOC_TYPE(c)  (((c) >> 8) & 0xff)
#define IOC_NR(c)    ((c) & 0xff)
#define IOC_SIZE(c)  (((c) >> 16) & 0x3fff)

#define KEY_ENTER  28
#define KEY_A      30

static struct input_event event_buf[EV_BUF_SIZE];
static int ev_head = 0;
static int ev_tail = 0;
static spinlock_t ev_lock = SPINLOCK_INIT;

extern volatile uint64_t kernel_ticks;
#define TIMER_HZ 100 /* Assuming from context */

void input_report_key(uint16_t code, int32_t value)
{
	uint64_t f = spin_lock_irq(&ev_lock);
	int next = (ev_head + 1) % EV_BUF_SIZE;
	if (next != ev_tail) {
		struct input_event *e = &event_buf[ev_head];
		e->time.tv_sec = kernel_ticks / TIMER_HZ;
		e->time.tv_usec = (kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ);
		e->type = EV_KEY;
		e->code = code;
		e->value = value;
		ev_head = next;
	}
	spin_unlock_irq(&ev_lock, f);
}

void input_sync(void)
{
	uint64_t f = spin_lock_irq(&ev_lock);
	int next = (ev_head + 1) % EV_BUF_SIZE;
	if (next != ev_tail) {
		struct input_event *e = &event_buf[ev_head];
		e->time.tv_sec = kernel_ticks / TIMER_HZ;
		e->time.tv_usec = (kernel_ticks % TIMER_HZ) * (1000000 / TIMER_HZ);
		e->type = EV_SYN;
		e->code = 0;
		e->value = 0;
		ev_head = next;
	}
	spin_unlock_irq(&ev_lock, f);
}

static ssize_t evdev_read(vfs_file_t *file, void *buf, size_t len)
{
	(void)file;
	if (len < sizeof(struct input_event)) return -22;
	
	while (ev_head == ev_tail) {
		if (file->flags & O_NONBLOCK) return -11; /* EAGAIN */
		task_sleep();
	}

	uint64_t f = spin_lock_irq(&ev_lock);
	int copied = 0;
	uint8_t *dbuf = buf;
	
	while (ev_head != ev_tail && len >= sizeof(struct input_event)) {
		struct input_event *e = &event_buf[ev_tail];
		
		/* Switch to user context? Wait, we are in kernel, user_access_begin needed if copying to user. 
		 * Assuming VFS layer does it or we rely on the same setup as tty_read */
		memcpy(dbuf, e, sizeof(*e));
		
		dbuf += sizeof(*e);
		len -= sizeof(*e);
		copied += sizeof(*e);
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
		if (ev_type == 0) {
			buf[0] = (1 << EV_SYN) | (1 << EV_KEY);
		} else if (ev_type == EV_KEY) {
			if (size >= 4)
				buf[3] = (1 << (KEY_ENTER % 8)) | (1 << (KEY_A % 8));
		}
		return 0;
	}
	if (nr == 0x90) return 0; /* EVIOCGRAB */
	return -25; /* ENOTTY */
}

static vfs_fops_t evdev_fops = {
	.read  = evdev_read,
	.poll  = evdev_poll,
	.ioctl = evdev_ioctl,
};

/* Linux keycodes for ASCII bridge */
#define KEY_ESC    1
#define KEY_TAB    15
#define KEY_W      17
#define KEY_E      18
#define KEY_Y      21
#define KEY_S      31
#define KEY_D      32
#define KEY_F      33
/* KEY_ENTER=28 and KEY_A=30 already defined above */
#define KEY_SPACE  57
#define KEY_BKSP   14
#define KEY_UP    103
#define KEY_LEFT  105
#define KEY_RIGHT 106
#define KEY_DOWN  108

static void inject_key(uint16_t code)
{
	input_report_key(code, 1);
	input_sync();
	input_report_key(code, 0);
	input_sync();
}

void input_inject_ascii(uint8_t c)
{
	static int esc_st = 0;

	if (esc_st == 1) {
		if (c == '[') { esc_st = 2; return; }
		esc_st = 0;
		inject_key(KEY_ESC);
		if (c == 0x1B) { esc_st = 1; return; }
	} else if (esc_st == 2) {
		esc_st = 0;
		switch (c) {
		case 'A': inject_key(KEY_UP);    return;
		case 'B': inject_key(KEY_DOWN);  return;
		case 'C': inject_key(KEY_RIGHT); return;
		case 'D': inject_key(KEY_LEFT);  return;
		default:  return;
		}
	}

	if (c == 0x1B) { esc_st = 1; return; }

	uint16_t k = 0;
	switch (c) {
	case '\r': case '\n': k = KEY_ENTER; break;
	case ' ':             k = KEY_SPACE; break;
	case 0x7F: case 0x08: k = KEY_BKSP;  break;
	case '\t':            k = KEY_TAB;   break;
	case 'w': case 'W':   k = KEY_UP;    break;
	case 'a': case 'A':   k = KEY_LEFT;  break;
	case 's': case 'S':   k = KEY_DOWN;  break;
	case 'd': case 'D':   k = KEY_RIGHT; break;
	case 'e': case 'E':   k = KEY_SPACE; break;
	case 'f': case 'F':   k = 29; /* KEY_LEFTCTRL for Fire */ break;
	case 'y': case 'Y':   k = KEY_Y;     break;
	case '1': k = 2; break; case '2': k = 3; break;
	case '3': k = 4; break; case '4': k = 5; break;
	case '5': k = 6; break; case '6': k = 7; break;
	case '7': k = 8; break;
	}
	if (k) inject_key(k);
}

void input_core_init(void)
{
	devfs_register_device("input/event0", S_IFCHR | 0444, 0x0D60, &evdev_fops);
}
