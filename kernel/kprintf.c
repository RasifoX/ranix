#include <drivers/serial/uart.h>
#include <kernel/board.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/ksyms.h>
#include <kernel/proc.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/lib/string.h>
#include <kernel/sync.h>
#include <asm/context.h>

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, l)   __builtin_va_arg(v, l)

static int fb_ready        = 0;
static int at_line_start   = 1;
static spinlock_t kprintf_lock = SPINLOCK_INIT;

void kprintf_set_fb(void) { fb_ready = 1; }

extern volatile uint64_t kernel_ticks;

/* ── kernel log ring buffer ───────────────────────────────── */

#define LOG_BUF_SIZE (1 << 17)  /* 128 KB */

static char   log_buf[LOG_BUF_SIZE];
static size_t log_head = 0;
static size_t log_len  = 0;

size_t kprintf_read_log(char *dst, size_t max)
{
    size_t n = log_len < max ? log_len : max;
    size_t start = (log_head + LOG_BUF_SIZE - log_len) % LOG_BUF_SIZE;
    if (start + n <= LOG_BUF_SIZE) {
        memcpy(dst, log_buf + start, n);
    } else {
        size_t first = LOG_BUF_SIZE - start;
        memcpy(dst, log_buf + start, first);
        memcpy(dst + first, log_buf, n - first);
    }
    return n;
}

size_t kprintf_log_len(void) { return log_len; }

static void _emit(char c)
{
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
    if (fb_ready)
        fb_tty_putchar(c);
    log_buf[log_head] = c;
    log_head = (log_head + 1) % LOG_BUF_SIZE;
    if (log_len < LOG_BUF_SIZE) log_len++;
}

static int suppress_ts = 0;   /* panic banner: no [   S.UUUUUU] prefix */

static void _putc(char c)
{
    if (at_line_start && c != '\n' && !suppress_ts) {
        /* prepend [    S.UUUUUU] timestamp */
        uint64_t t    = kernel_ticks;
        uint32_t secs = (uint32_t)(t / TIMER_HZ);
        uint32_t usec = (uint32_t)((t % TIMER_HZ) * (1000000 / TIMER_HZ));
        char buf[16]; int i;
        _emit('[');
        /* 5-digit seconds, space-padded */
        i = 0;
        char sbuf[6]; uint32_t s2 = secs;
        do { sbuf[i++] = '0' + s2 % 10; s2 /= 10; } while (s2);
        while (i < 5) sbuf[i++] = ' ';
        while (i--) _emit(sbuf[i]);
        _emit('.');
        /* 6-digit microseconds, zero-padded */
        i = 0;
        char ubuf[6]; uint32_t u2 = usec;
        do { ubuf[i++] = '0' + u2 % 10; u2 /= 10; } while (u2 && i < 6);
        while (i < 6) ubuf[i++] = '0';
        while (i--) _emit(ubuf[i]);
        _emit(']'); _emit(' ');
        at_line_start = 0;
    }
    if (c == '\n')
        at_line_start = 1;
    _emit(c);
}

static void _puts(const char *s)
{
    while (*s)
        _putc(*s++);
}

static void fmt_uint(uint64_t n, int base, int width, char pad, int upper)
{
    char buf[64];
    int  i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (n == 0)
        buf[i++] = '0';
    else
        while (n) { buf[i++] = digits[n % (unsigned)base]; n /= (unsigned)base; }

    while (i < width)
        buf[i++] = pad;

    while (i--)
        _putc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    uint64_t flags = spin_lock_irq(&kprintf_lock);
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') { _putc(*fmt++); continue; }
        fmt++;

        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? (int64_t)va_arg(args, long) : (int64_t)va_arg(args, int);
            if (v < 0) { _putc('-'); fmt_uint((uint64_t)-v, 10, width, pad, 0); }
            else        fmt_uint((uint64_t)v,  10, width, pad, 0);
            break;
        }
        case 'u':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     10, width, pad, 0);
            break;
        case 'x':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     16, width, pad, 0);
            break;
        case 'X':
            fmt_uint(lng ? va_arg(args, unsigned long) : (uint64_t)va_arg(args, unsigned int),
                     16, width, pad, 1);
            break;
        case 'p': {
            uint64_t v = va_arg(args, unsigned long);
            _puts("0x");
            fmt_uint(v, 16, 16, '0', 0);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char *);
            _puts(s ? s : "(null)");
            break;
        }
        case 'c':
            _putc((char)va_arg(args, int));
            break;
        case '%':
            _putc('%');
            break;
        default:
            _putc('%');
            _putc(*fmt);
            break;
        }
        fmt++;
    }

    va_end(args);
    spin_unlock_irq(&kprintf_lock, flags);
}

void kprintf_flush(void)
{
    if (fb_ready)
        fb_tty_flush();
}

static void panic_sym(uint64_t pc)
{
    uint64_t off = 0;
    const char *s = ksym_lookup(pc, &off);
    if (s) kprintf("    %s+0x%lx  (0x%016lx)\n", s, off, pc);
    else   kprintf("    0x%016lx\n", pc);
}

/* Branded panic screen — plain ASCII, no color and no timestamps, so it reads
 * identically on a monochrome framebuffer and a serial line. Rich enough that a
 * photo of the screen is sufficient to diagnose: symbolized fault site and call
 * trace (kallsyms), full register file, and the exact build identity. */
void panic_report(const char *msg, arch_context_t *ctx)
{
    static volatile int panic_active = 0;

    irq_disable();
    if (panic_active)            /* a fault while painting the panic — just stop */
        for (;;) arch_halt();
    panic_active = 1;
    suppress_ts = 1;

    /* Fit the screen: clear it (start at the top, nothing scrolls off) and, if the
     * framebuffer console is short, drop the optional detail so the essentials —
     * banner, fault, location, build, call trace — are always fully visible and
     * photo-able. Serial (rows == 0) always gets the full dump. */
    int pcols = 0, prows = 0;
    if (fb_ready) {
        fb_tty_clear();
        fb_tty_get_size(&pcols, &prows);
    }
    /* Tiers by screen height (rows == 0 means serial → always full):
     *   >= 30 : everything (~27 lines)
     *   >= 20 : drop the optional stack + code dumps
     *   <  20 : also collapse the register file to one summary line
     * The always-shown core (banner, fault, location, build, call trace) is
     * ~16 lines, so it fits any console down to ~17 rows. */
    int compact = (prows > 0 && prows < 30);   /* drop stack + code */
    int tiny    = (prows > 0 && prows < 20);   /* collapse registers */

    process_t *p     = task_current();
    cpu_context_t *c = arch_get_cpu_ctx();
    int p_ok = ((uint64_t)p >= 0xFFFF000000000000UL);   /* kernel pointer? */

#if defined(__x86_64__)
    uint64_t pc = ctx ? ctx->rip : (uint64_t)__builtin_return_address(0);
    uint64_t fp = ctx ? ctx->rbp : (uint64_t)__builtin_frame_address(0);
    uint64_t sp = ctx ? ctx->rsp : (uint64_t)__builtin_frame_address(0);
    uint64_t cr2, cr3;
    __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3,%0" : "=r"(cr3));
#else
    uint64_t pc = ctx ? ctx->elr : (uint64_t)__builtin_return_address(0);
    uint64_t fp = ctx ? ctx->regs[29] : (uint64_t)__builtin_frame_address(0);
    uint64_t sp = ctx ? ctx->sp : (uint64_t)__builtin_frame_address(0);
    uint64_t far, esr;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
#endif
    uint64_t up_s  = kernel_ticks / TIMER_HZ;
    uint64_t up_ms = (kernel_ticks % TIMER_HZ) * 1000 / TIMER_HZ;

    /* Essentials first — anything below that walks memory could itself fault and
     * trip the recursion guard, so the must-have facts are already on screen. */
    kprintf("\n  RANIX kernel panic\n\n");
    kprintf("  The good news: I caught it. The bad news: I can't continue.\n");
    kprintf("  Snap a photo of this screen and we'll know what happened.\n\n");

    kprintf("  stop    %s\n", msg);
#if defined(__x86_64__)
    if (ctx && ctx->vector == 14)
        kprintf("  fault   %s %s at 0x%016lx in %s mode\n",
                (ctx->error_code & 2)  ? "write" : (ctx->error_code & 16 ? "exec" : "read"),
                (ctx->error_code & 1)  ? "(protection)" : "(not present)",
                cr2, (ctx->error_code & 4) ? "user" : "kernel");
#else
    if (ctx)
        kprintf("  fault   ESR 0x%08lx  FAR 0x%016lx\n", esr, far);
#endif
    uint64_t off = 0;
    const char *sym = ksym_lookup(pc, &off);
    if (sym) kprintf("  where   %s+0x%lx  (0x%016lx)\n", sym, off, pc);
    else     kprintf("  where   0x%016lx\n", pc);

    kprintf("  who     CPU %lu  PID %d (%s)  in %s\n",
            c ? c->cpu_id : 0, p_ok ? (int)p->pid : -1,
            p_ok ? p->name : "?", c && c->wd_where ? c->wd_where : "?");
    kprintf("  build   ranix %s [%s] %s  %s\n", ranix_version, ranix_gitrev, ranix_arch, ranix_builddate);
    kprintf("  uptime  %lu.%03lus\n", up_s, up_ms);

    if (ctx) {
        kprintf("  registers\n");
#if defined(__x86_64__)
        kprintf("    RIP %016lx RSP %016lx RFL %08lx CS %lx ERR %lx\n", ctx->rip, ctx->rsp, ctx->rflags, ctx->cs, ctx->error_code);
        if (!tiny) {
            kprintf("    RAX %016lx RBX %016lx RCX %016lx RDX %016lx\n", ctx->rax, ctx->rbx, ctx->rcx, ctx->rdx);
            kprintf("    RSI %016lx RDI %016lx RBP %016lx\n", ctx->rsi, ctx->rdi, ctx->rbp);
            kprintf("    R8  %016lx R9  %016lx R10 %016lx R11 %016lx\n", ctx->r8, ctx->r9, ctx->r10, ctx->r11);
            kprintf("    R12 %016lx R13 %016lx R14 %016lx R15 %016lx\n", ctx->r12, ctx->r13, ctx->r14, ctx->r15);
            kprintf("    CR2 %016lx CR3 %016lx\n", cr2, cr3);
        }
#else
        kprintf("    ELR %016lx SP %016lx PSTATE %08lx\n", ctx->elr, ctx->sp, ctx->spsr);
        if (!tiny)
            for (int i = 0; i < 31; i += 4) {
                kprintf("    ");
                for (int j = i; j < i + 4 && j < 31; j++)
                    kprintf("X%02d %016lx ", j, ctx->regs[j]);
                kprintf("\n");
            }
#endif
    }

    /* Instruction bytes at the fault site — only when pc is a known text symbol,
     * so we never fetch from an unmapped page and double-fault. */
    if (sym && !compact) {
        kprintf("  code    ");
        const unsigned char *ip = (const unsigned char *)pc;
        for (int i = 0; i < 16; i++)
            kprintf("%02x ", ip[i]);
        kprintf("\n");
    }

    /* Raw stack words — a backstop when the frame-pointer unwind can't proceed. */
    if (!compact && sp >= 0xFFFF000000000000UL && (sp & 7) == 0) {
        const uint64_t *s = (const uint64_t *)sp;
        kprintf("  stack   %016lx %016lx %016lx\n          %016lx %016lx %016lx\n",
                s[0], s[1], s[2], s[3], s[4], s[5]);
    }

    kprintf("  call trace\n");
    panic_sym(pc);
    for (int i = 0; i < 24 && fp >= 0xFFFF000000000000UL && (fp & 7) == 0; i++) {
        uint64_t next = ((uint64_t *)fp)[0];
        uint64_t ret  = ((uint64_t *)fp)[1];
        if (!ret) break;
        panic_sym(ret);
        if (next <= fp) break;       /* frames must grow toward the stack base */
        fp = next;
    }

    kprintf("\n  state frozen -- please power-cycle the machine.   [ end of panic ]\n");
    if (fb_ready)
        fb_tty_flush();
    for (;;)
        arch_halt();
}

void kpanic(const char *msg)
{
    panic_report(msg, 0);
}
