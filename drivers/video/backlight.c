/*
 * Panel brightness on Intel graphics from Sunrise Point on: the PWM that feeds the
 * backlight sits in the south display registers, BLC_PWM_PCH_CTL2 holding the period and
 * the duty. The firmware has already set it up, so only the duty is touched, and only if
 * the PWM is running. Mapped on first use: a machine that never asks never touches it.
 */
#include <kernel/pci.h>
#include <kernel/mm/vm.h>
#include <kernel/hal.h>
#include <asm/mmu.h>

#if defined(__x86_64__)
#define BLC_PWM_PCH_CTL1 0xC8250
#define BLC_PWM_PCH_CTL2 0xC8254
#define BLC_ENABLE       (1u << 31)
#define MIN_PCT          5              /* black is not a brightness: nobody could fix it */

static volatile uint8_t *mmio;
static volatile int blank_duty = -1;      /* see backlight_unblank */
static int probed;

static int probe(void)
{
	if (probed) return mmio != 0;
	probed = 1;
	uint8_t bus, dev, fn;
	if (pci_find_class(0x03, 0x00, 0x00, &bus, &dev, &fn) != 0) return 0;
	if (pci_read16(bus, dev, fn, 0) != 0x8086) return 0;
	uint16_t id = pci_read16(bus, dev, fn, 2);
	if ((id >> 8) != 0x59 && (id >> 8) != 0x19) return 0;   /* Sky Lake, Kaby Lake */
	uint64_t bar = (pci_read32(bus, dev, fn, PCI_BAR0) & ~0xFUL) |
	               (uint64_t)pci_read32(bus, dev, fn, PCI_BAR1) << 32;
	if (!bar) return 0;
	mmio = ioremap(bar, 0x100000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	return mmio != 0;
}

static uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(mmio + off); }

/* percent, -1 when this machine has no such control */
int backlight_get(void)
{
	if (!probe() || !(rd(BLC_PWM_PCH_CTL1) & BLC_ENABLE)) return -1;
	uint32_t v = rd(BLC_PWM_PCH_CTL2), period = v >> 16, duty = v & 0xFFFF;
	if (!period) return -1;
	if (blank_duty >= 0) return 0;
	return (int)((duty * 100 + period / 2) / period);
}

/* Blanked: duty 0, what it was kept. Any key, touch or USB input lights it again
 * from the kernel, so a desktop that dies with the screen off cannot leave it dark. */

void backlight_unblank(void)
{
	if (blank_duty < 0 || !mmio) return;
	uint32_t period = rd(BLC_PWM_PCH_CTL2) >> 16;
	*(volatile uint32_t *)(mmio + BLC_PWM_PCH_CTL2) = period << 16 | (uint32_t)blank_duty;
	blank_duty = -1;
}

static int backlight_blank(void)
{
	if (blank_duty >= 0) return 0;
	uint32_t v = rd(BLC_PWM_PCH_CTL2);
	blank_duty = (int)(v & 0xFFFF);
	*(volatile uint32_t *)(mmio + BLC_PWM_PCH_CTL2) = v & 0xFFFF0000u;
	return 0;
}

int backlight_set(int pct)
{
	if (backlight_get() < 0) return -19;
	if (pct == 0) return backlight_blank();    /* 0 is off, until any input */
	blank_duty = -1;
	if (pct < MIN_PCT) pct = MIN_PCT;
	if (pct > 100) pct = 100;
	uint32_t period = rd(BLC_PWM_PCH_CTL2) >> 16;
	*(volatile uint32_t *)(mmio + BLC_PWM_PCH_CTL2) = period << 16 | (period * (uint32_t)pct / 100);
	return 0;
}
/* The line the display is scanning now, and how many lines a frame has. A program that
 * paints where the beam is not does not tear, and that needs no page flips. */
int igd_scanline(uint32_t *total)
{
	static int pipe = -1;
	if (!probe()) return -1;
	if (pipe < 0) {
		for (int i = 0; i < 3; i++)
			if (rd(0x70180 + (uint32_t)i * 0x1000) & (1u << 31)) {   /* PLANE_CTL_1 */
				pipe = i;
				break;
			}
		if (pipe < 0) return -1;
	}
	uint32_t base = (uint32_t)pipe * 0x1000;
	/* the edp transcoder holds the timing when it drives this pipe */
	uint32_t vt = rd(0x6F00C);
	if (!vt) vt = rd(0x6000C + base);
	*total = (vt >> 16) + 1;
	return (int)(rd(0x70000 + base) & 0x1FFF);
}
#else
void backlight_unblank(void) {}
int igd_scanline(uint32_t *total) { (void)total; return -1; }
int backlight_get(void) { return -1; }
int backlight_set(int pct) { (void)pct; return -19; }
#endif
