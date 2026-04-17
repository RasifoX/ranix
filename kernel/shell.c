#include <kernel/shell.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/driver.h>
#include <kernel/sched.h>
#include <kernel/fdt.h>
#include <drivers/serial/uart.h>
#include <kernel/mm/pmm.h>

#define CMD_MAX 256
#define PROMPT  "ranix> "

static char cmd_buf[CMD_MAX];
static int  cmd_len;

static void cmd_help(void)
{
    kprintf("help     - show commands\n");
    kprintf("info     - system info\n");
    kprintf("mem      - memory stats\n");
    kprintf("clear    - clear screen\n");
    kprintf("uptime   - show ticks\n");
    kprintf("drivers  - list drivers\n");
    kprintf("ps       - list tasks\n");
    kprintf("dtb      - device tree info\n");
    kprintf("fault    - trigger test fault\n");
}

static void cmd_fault(void)
{
    volatile uint32_t *p = (volatile uint32_t *)0xdeadbeef;
    *p = 0;
}

static void cmd_info(void)
{
    kprintf("Ranix kernel\n");
    kprintf("Arch: aarch64\n");
    kprintf("Display: 800x480 32bpp\n");
}

static void cmd_mem(void)
{
    uint64_t free = pmm_free_pages();
    kprintf("PMM: %d KB free\n", (int)(free * 4));
}

extern uint64_t timer_ticks(void);

static void cmd_uptime(void)
{
    uint64_t t = timer_ticks();
    uint32_t sec = (uint32_t)(t / 100);
    kprintf("Uptime: %d ticks (%d s)\n", (int)t, (int)sec);
}

static void cmd_clear(void)
{
    fb_tty_clear();
}

static void cmd_drivers(void)
{
    kprintf("Registered drivers:\n");
    driver_list_all();
}

static void cmd_ps(void)
{
    static const char *state_names[] = { "run", "ready", "dead", "sleep" };
    int n = task_count();
    kprintf("  ID  NAME             STATE\n");
    for (int i = 0; i < n; i++) {
        task_t *t = task_get(i);
        kprintf("  %d   %s  %s\n",
                (int)t->pid, t->name,
                state_names[t->state]);
    }
}

static void cmd_dtb(void)
{
    fdt_info_t *fi = fdt_get_info();
    kprintf("Model:  %s\n", fi->model);
    kprintf("Compat: %s\n", fi->compatible);
    kprintf("RAM:    %d MB\n", (int)(fi->mem_size >> 20));
    kprintf("CPUs:   %d\n", (int)fi->num_cpus);
    kprintf("DTB at: %x (%d bytes)\n",
            (unsigned int)(uint64_t)fdt_get_base(), (int)fdt_get_size());
}

static void execute(const char *line)
{
    if (strlen(line) == 0)
        return;

    if (strcmp(line, "help") == 0)
        cmd_help();
    else if (strcmp(line, "info") == 0)
        cmd_info();
    else if (strcmp(line, "mem") == 0)
        cmd_mem();
    else if (strcmp(line, "uptime") == 0)
        cmd_uptime();
    else if (strcmp(line, "clear") == 0)
        cmd_clear();
    else if (strcmp(line, "drivers") == 0)
        cmd_drivers();
    else if (strcmp(line, "ps") == 0)
        cmd_ps();
    else if (strcmp(line, "dtb") == 0)
        cmd_dtb();
    else if (strcmp(line, "fault") == 0)
        cmd_fault();
    else
        kprintf("Unknown command: %s\n", line);
}

void shell_init(void)
{
    cmd_len = 0;
    cmd_buf[0] = '\0';
    kprintf(PROMPT);
}

void shell_poll(void)
{
    while (uart_has_data()) {
        char c = uart_getc();

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            cmd_buf[cmd_len] = '\0';
            execute(cmd_buf);
            cmd_len = 0;
            kprintf(PROMPT);
        } else if (c == 127 || c == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                kprintf("\b \b");
            }
        } else if (c >= 32 && c < 127 && cmd_len < CMD_MAX - 1) {
            cmd_buf[cmd_len++] = c;
            uart_putc(c);
        }
    }
    kprintf_flush();
}

void shell_task(void)
{
    shell_init();
    while (1) {
        shell_poll();
        yield();
    }
}
