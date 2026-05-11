#include <kernel/input.h>
#include <kernel/fs/devfs.h>
#include <kernel/sync.h>
#include <kernel/proc.h>
#include <kernel/lib/string.h>

#define EV_BUF_SIZE 256

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
	return (ev_head != ev_tail) ? 1 : 0; /* POLLIN */
}

static vfs_fops_t evdev_fops = {
	.read = evdev_read,
	.poll = evdev_poll,
};

void input_core_init(void)
{
	devfs_register_device("event0", S_IFCHR | 0444, 0x0D60, &evdev_fops);
}
