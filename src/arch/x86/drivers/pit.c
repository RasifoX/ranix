#include "pit.h"
#include "io.h"
#include "hal.h"

#define PIT_CMD_PORT 0x43
#define PIT_CH0_DATA 0x40

#define PIT_BASE_FREQ 1193180

volatile uint64_t ticks = 0;

void init_pit(uint32_t frequency)
{
    if (frequency == 0)
        return;

    uint32_t divisor = PIT_BASE_FREQ / frequency;

    if (divisor > 65535)
        divisor = 65535;

    outb(PIT_CMD_PORT, 0x36);

    uint8_t low = (uint8_t)(divisor & 0xFF);
    uint8_t high = (uint8_t)((divisor >> 8) & 0xFF);

    outb(PIT_CH0_DATA, low);
    outb(PIT_CH0_DATA, high);
}

void timer_handler()
{
    ticks++;
}

void sleep(uint32_t ms)
{

    uint64_t end_ticks = ticks + ms;

    while (ticks < end_ticks)
    {
        asm volatile("hlt");
    }
}