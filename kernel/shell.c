#include <kernel/shell.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/driver.h>
#include <kernel/proc.h>
#include <kernel/fdt.h>
#include <drivers/serial/uart.h>
#include <kernel/mm/pmm.h>

#define CMD_MAX 256
#define PROMPT  "ranix> "

static char cmd_buf[CMD_MAX];
static int  cmd_len;

/*
 * Minimal ELF64 test binaries — hand-assembled.
 * Both do: sys_write(1, "hello\n", 6)  then  sys_exit(0).
 *
 * aarch64: SVC #1, syscall nr in x8  (Linux aarch64 ABI)
 * x86_64:  INT 0x80, syscall nr in rax (Linux x86_64 ABI)
 */

#if defined(__aarch64__)
static const uint8_t test_elf[] = {
    /* ELF header (64 bytes) */
    0x7F,'E','L','F',           /* magic */
    2, 1, 1, 0,                 /* class64, LSB, ver1, ELFOSABI_NONE */
    0,0,0,0, 0,0,0,0,           /* pad */
    2,0,                        /* ET_EXEC */
    0xB7,0,                     /* EM_AARCH64 = 183 */
    1,0,0,0,                    /* version */
    0x00,0x00,0x00,0x00, 0x01,0,0,0,  /* entry = 0x100000000 */
    0x40,0,0,0, 0,0,0,0,        /* phoff = 64 */
    0,0,0,0, 0,0,0,0,           /* shoff = 0 */
    0,0,0,0,                    /* flags */
    0x40,0,                     /* ehsize = 64 */
    0x38,0,                     /* phentsize = 56 */
    1,0,                        /* phnum = 1 */
    0x40,0,                     /* shentsize = 64 */
    0,0,                        /* shnum = 0 */
    0,0,                        /* shstrndx = 0 */

    /* Program header (56 bytes, offset 64) */
    1,0,0,0,                    /* PT_LOAD */
    5,0,0,0,                    /* PF_R|PF_X */
    0x78,0,0,0, 0,0,0,0,        /* offset = 120 (= 64+56) */
    0x00,0x00,0x00,0x00, 0x01,0,0,0, /* vaddr = 0x100000000 */
    0x00,0x00,0x00,0x00, 0x01,0,0,0, /* paddr = 0x100000000 */
    0x26,0,0,0, 0,0,0,0,        /* filesz = 38 */
    0x26,0,0,0, 0,0,0,0,        /* memsz  = 38 */
    0x00,0x10,0,0, 0,0,0,0,     /* align = 0x1000 */

    /* Code + data at offset 120 (vaddr 0x400000) */
    /* mov x8, #64  (SYS_WRITE) */ 0x08,0x08,0x80,0xD2,
    /* mov x0, #1   (fd=1)      */ 0x20,0x00,0x80,0xD2,
    /* adr x1, +24  (&msg)      */ 0xC1,0x00,0x00,0x10,
    /* mov x2, #6   (len)       */ 0xC2,0x00,0x80,0xD2,
    /* svc #1                   */ 0x21,0x00,0x00,0xD4,
    /* mov x8, #93  (SYS_EXIT)  */ 0xA8,0x0B,0x80,0xD2,
    /* mov x0, #0               */ 0x00,0x00,0x80,0xD2,
    /* svc #1                   */ 0x21,0x00,0x00,0xD4,
    /* msg: "hello\n"           */ 'h','e','l','l','o','\n',
};
#elif defined(__x86_64__)
static const uint8_t test_elf[] = {
    /* ELF header (64 bytes) */
    0x7F,'E','L','F',
    2, 1, 1, 0,
    0,0,0,0, 0,0,0,0,
    2,0,
    0x3E,0,                     /* EM_X86_64 = 62 */
    1,0,0,0,
    0x00,0x00,0x00,0x00, 0x01,0,0,0,  /* entry = 0x100000000 */
    0x40,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0,
    0,0,0,0,
    0x40,0,
    0x38,0,
    1,0,
    0x40,0,
    0,0,
    0,0,

    /* Program header */
    1,0,0,0,
    5,0,0,0,
    0x78,0,0,0, 0,0,0,0,        /* offset = 120 */
    0x00,0x00,0x00,0x00, 0x01,0,0,0,
    0x00,0x00,0x00,0x00, 0x01,0,0,0,
    0x30,0,0,0, 0,0,0,0,        /* filesz = 48 */
    0x30,0,0,0, 0,0,0,0,
    0x00,0x10,0,0, 0,0,0,0,

    /* Code at offset 120 (vaddr 0x400000) */
    /* mov $1, %rax          */ 0x48,0xC7,0xC0,0x01,0,0,0,
    /* mov $1, %rdi          */ 0x48,0xC7,0xC7,0x01,0,0,0,
    /* lea 0x15(%rip), %rsi  */ 0x48,0x8D,0x35,0x15,0,0,0,
    /* mov $6, %rdx          */ 0x48,0xC7,0xC2,0x06,0,0,0,
    /* int $0x80             */ 0xCD,0x80,
    /* mov $60, %rax         */ 0x48,0xC7,0xC0,0x3C,0,0,0,
    /* xor %rdi, %rdi        */ 0x48,0x31,0xFF,
    /* int $0x80             */ 0xCD,0x80,
    /* msg: "hello\n"        */ 'h','e','l','l','o','\n',
};
#endif

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
    kprintf("hello    - run test user process\n");
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
    else if (strcmp(line, "hello") == 0)
        proc_exec("hello", test_elf, sizeof(test_elf));
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
