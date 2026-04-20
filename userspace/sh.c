#include <unistd.h>
#include <sys/wait.h>

static int mystrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void mystrcpy(char *d, const char *s) { while ((*d++ = *s++)); }
static int mystrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void print(const char *s) { write(1, s, mystrlen(s)); }

static char path_buf[280];
static char input_buf[256];
static char *child_argv[2];
static int  wait_status;

static void run(const char *cmd)
{
    if (cmd[0] == '\0') return;

    for (int i = 0; i < 280; i++) path_buf[i] = 0;

    if (cmd[0] == '/') {
        mystrcpy(path_buf, cmd);
    } else {
        mystrcpy(path_buf, "/bin/");
        mystrcpy(path_buf + 5, cmd);
    }

    child_argv[0] = path_buf;
    child_argv[1] = 0;

    long pid;
    
#if defined(__aarch64__)
    __asm__ volatile (
        "mov x8, #220\n"  /* clone (vfork) */
        "mov x0, #0x4111\n" /* flags: VM | VFORK | SIGCHLD */
        "mov x1, #0\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(pid) : : "x0", "x1", "x8", "memory"
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "mov $56, %%rax\n"   /* clone */
        "mov $0x4111, %%rdi\n" /* flags: VM | VFORK | SIGCHLD */
        "xor %%rsi, %%rsi\n"   /* child_stack = NULL */
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(pid) : : "rax", "rdi", "rsi", "rdx", "r10", "r8", "memory"
    );
#else
    pid = vfork();
#endif

    if (pid == 0) {
#if defined(__aarch64__)
        __asm__ volatile (
            "mov x0, %0\n"
            "mov x1, %1\n"
            "mov x2, #0\n"
            "mov x8, #221\n" /* execve */
            "svc #0\n"
            : : "r"(path_buf), "r"(child_argv) : "x0", "x1", "x2", "x8", "memory"
        );
#elif defined(__x86_64__)
        __asm__ volatile (
            "mov %0, %%rdi\n"  /* path */
            "mov %1, %%rsi\n"  /* argv */
            "xor %%rdx, %%rdx\n" /* envp = NULL */
            "mov $59, %%rax\n"  /* execve */
            "syscall\n"
            : : "r"(path_buf), "r"(child_argv) : "rax", "rdi", "rsi", "rdx", "memory"
        );
#else
        execve(path_buf, child_argv, 0);
#endif
        print(path_buf);
        print(": not found\n");
        _exit(127);
    }

    if (pid > 0) {
        waitpid(pid, &wait_status, 0);
    }
}

int main(void)
{
    print("\nRanix shell — type 'help' for commands\n");

    while (1) {
        print("$ ");

        for (int i = 0; i < 256; i++) input_buf[i] = 0;

        int n = read(0, input_buf, sizeof(input_buf) - 1);
        if (n <= 0) continue;

        while (n > 0 && (input_buf[n-1] == '\n' || input_buf[n-1] == '\r')) {
            input_buf[n-1] = '\0';
            n--;
        }

        if (n == 0) continue;

        if (mystrcmp(input_buf, "exit") == 0) break;
        if (mystrcmp(input_buf, "help") == 0) {
            print("Commands: exit, and any binary in /bin/\n");
            continue;
        }

        run(input_buf);
    }

    return 0;
}
