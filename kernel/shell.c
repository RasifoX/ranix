#include <kernel/shell.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/driver.h>
#include <kernel/proc.h>
#include <kernel/fdt.h>
#include <drivers/serial/uart.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/vfs.h>
#include "userspace/hello_elf.h"

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
    kprintf("hello    - run hello binary\n");
    kprintf("run <path> - run ELF from VFS\n");
}

static void cmd_fault(void)
{
    volatile uint32_t *p = (volatile uint32_t *)0xdeadbeef;
    *p = 0;
}

static void cmd_info(void)
{
    kprintf("Ranix kernel\n");
#if defined(__aarch64__)
    kprintf("Arch: aarch64\n");
#elif defined(__x86_64__)
    kprintf("Arch: x86_64\n");
#else
    kprintf("Arch: unknown\n");
#endif
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
    kprintf("DTB at: 0x%lx (%d bytes)\n", (uint64_t)fdt_get_base(), (int)fdt_get_size());
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
    else if (strcmp(line, "hello") == 0) {
        pid_t pid = proc_exec("hello", hello_musl_elf, hello_musl_elf_len);
        if (pid >= 0) proc_wait(pid);
    }
    else if (strncmp(line, "run ", 4) == 0) {
        /* run /bin/foo — load ELF from VFS */
        const char *path = line + 4;
        vfs_stat_t st = {0};
        if (vfs_stat(path, &st) != 0 || st.size == 0) {
            kprintf("run: not found: %s\n", path);
        } else {
            uint8_t *buf = kmalloc(st.size);
            if (buf) {
                int fd = vfs_open(path, O_RDONLY, 0);
                if (fd >= 0) {
                    vfs_read(fd, buf, st.size);
                    vfs_close(fd);
                    pid_t pid = proc_exec(path, buf, st.size);
                    if (pid >= 0) proc_wait(pid);
                }
                kfree(buf);
            }
        }
    }
    else
        kprintf("Unknown command: %s\n", line);
}

void shell_init(void)
{
    cmd_len = 0;
    cmd_buf[0] = '\0';
    kprintf(PROMPT);
    kprintf_flush();
}

#ifdef __x86_64__
#include <drivers/input/ps2.h>
#endif

void shell_poll(void)
{
    while (1) {
        char c = 0;
        if (uart_has_data()) {
            c = uart_getc();
        }
#ifdef __x86_64__
        else if (ps2_kbd_has_data()) {
            c = ps2_kbd_getc();
        }
#endif
        else {
            break;
        }

        if (c == '\r' || c == '\n') {
            kprintf("\n");
            cmd_buf[cmd_len] = '\0';
            execute(cmd_buf);
            cmd_len = 0;
            kprintf(PROMPT);
            kprintf_flush();
        } else if (c == 127 || c == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                kprintf("\b \b");
            }
        } else if (c >= 32 && c < 127 && cmd_len < CMD_MAX - 1) {
            cmd_buf[cmd_len++] = c;
            kprintf("%c", c);
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
