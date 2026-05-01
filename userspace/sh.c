#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#define ARGV_MAX  32
#define INPUT_MAX 512
#define PATH_MAX  280

static char input_buf[INPUT_MAX];
static char *argv_buf[ARGV_MAX + 1];
static char path_buf[PATH_MAX];
static char cwd_buf[PATH_MAX];
static int  wait_status;

static void print(const char *s)   { write(1, s, strlen(s)); }
static void println(const char *s) { print(s); write(1, "\n", 1); }

/* split input_buf in-place; returns argc */
static int parse_args(char *line)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= ARGV_MAX) break;
        argv_buf[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv_buf[argc] = NULL;
    return argc;
}

/* ── built-ins ──────────────────────────────────────────── */

/* linux_dirent64 struct layout used by getdents64 */
struct dirent64 {
    unsigned long long d_ino;
    long long          d_off;
    unsigned short     d_reclen;
    unsigned char      d_type;
    char               d_name[];
};
#define DT_DIR 4

static long raw_open(const char *path, int flags)
{
#if defined(__aarch64__)
    long fd;
    __asm__ volatile (
        "mov x8, #56\n"         /* openat */
        "mov x0, #-100\n"       /* AT_FDCWD */
        "mov x1, %1\n"
        "mov x2, %2\n"
        "mov x3, #0\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(fd) : "r"(path), "r"((long)flags) : "x0","x1","x2","x3","x8","memory"
    );
    return fd;
#elif defined(__x86_64__)
    long fd;
    __asm__ volatile (
        "mov $257, %%rax\n"     /* openat */
        "mov $-100, %%rdi\n"    /* AT_FDCWD */
        "mov %1, %%rsi\n"
        "mov %2, %%rdx\n"
        "xor %%r10, %%r10\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(fd) : "r"(path), "r"((long)flags) : "rax","rdi","rsi","rdx","r10","memory"
    );
    return fd;
#else
    return open(path, flags);
#endif
}

static long raw_getdents64(int fd, void *buf, long len)
{
#if defined(__aarch64__)
    long ret;
    __asm__ volatile (
        "mov x8, #61\n"
        "mov x0, %1\n"
        "mov x1, %2\n"
        "mov x2, %3\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(len) : "x0","x1","x2","x8","memory"
    );
    return ret;
#elif defined(__x86_64__)
    long ret;
    __asm__ volatile (
        "mov $217, %%rax\n"
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov %3, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(len) : "rax","rdi","rsi","rdx","memory"
    );
    return ret;
#else
    return -1;
#endif
}

static void builtin_ls(const char *path)
{
    if (!path) path = ".";
    /* O_RDONLY=0, O_DIRECTORY=0x4000, O_CLOEXEC=0x80000 */
    long fd = raw_open(path, 0x84000);
    if (fd < 0) { print("ls: cannot open: "); println(path); return; }

    char buf[2048];
    long n;
    while ((n = raw_getdents64((int)fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *de = (struct dirent64 *)(buf + off);
            const char *name = de->d_name;
            if (!(name[0] == '.' && (name[1] == '\0' ||
                  (name[1] == '.' && name[2] == '\0')))) {
                if (de->d_type == DT_DIR) { print(name); print("/\n"); }
                else                      println(name);
            }
            off += de->d_reclen;
        }
    }
    close((int)fd);
}

static void builtin_cat(const char *path)
{
    if (!path) { println("cat: missing argument"); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("cat: cannot open: "); println(path); return; }
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, (size_t)n);
    close(fd);
}

static int mystrncpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static void builtin_cd(const char *path)
{
    if (!path) path = "/";
    if (chdir(path) < 0) { print("cd: cannot cd to: "); println(path); return; }
    if (!getcwd(cwd_buf, sizeof(cwd_buf)))
        mystrncpy(cwd_buf, path, PATH_MAX);
}

static void builtin_pwd(void)
{
    println(cwd_buf);
}

static void builtin_help(void)
{
    println("Built-ins: ls [path], cat <file>, cd [path], pwd, exit");
    println("Other commands run from /bin/<cmd>");
}

/* ── external command ───────────────────────────────────── */

static void run_external(int argc, char **argv)
{
    (void)argc;
    const char *cmd = argv[0];

    if (cmd[0] == '/') {
        strncpy(path_buf, cmd, PATH_MAX - 1);
    } else {
        snprintf(path_buf, PATH_MAX, "/bin/%s", cmd);
    }

    long pid;

#if defined(__aarch64__)
    __asm__ volatile (
        "mov x8, #220\n"
        "mov x0, #0x4111\n"
        "mov x1, #0\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(pid) : : "x0", "x1", "x8", "memory"
    );
#elif defined(__x86_64__)
    __asm__ volatile (
        "mov $56, %%rax\n"
        "mov $0x4111, %%rdi\n"
        "xor %%rsi, %%rsi\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(pid) : : "rax", "rdi", "rsi", "rdx", "r10", "r8", "memory"
    );
#else
    pid = vfork();
#endif

    if (pid == 0) {
        argv[0] = path_buf;
#if defined(__aarch64__)
        __asm__ volatile (
            "mov x0, %0\n"
            "mov x1, %1\n"
            "mov x2, #0\n"
            "mov x8, #221\n"
            "svc #0\n"
            : : "r"(path_buf), "r"(argv_buf) : "x0", "x1", "x2", "x8", "memory"
        );
#elif defined(__x86_64__)
        __asm__ volatile (
            "mov %0, %%rdi\n"
            "mov %1, %%rsi\n"
            "xor %%rdx, %%rdx\n"
            "mov $59, %%rax\n"
            "syscall\n"
            : : "r"(path_buf), "r"(argv_buf) : "rax", "rdi", "rsi", "rdx", "memory"
        );
#else
        execve(path_buf, argv_buf, NULL);
#endif
        print(path_buf); println(": not found");
        _exit(127);
    }

    if (pid > 0)
        waitpid(pid, &wait_status, 0);
}

/* ── main ───────────────────────────────────────────────── */

int main(void)
{
    println("\nRanix shell — type 'help' for commands");

    if (!getcwd(cwd_buf, sizeof(cwd_buf)))
        strncpy(cwd_buf, "/", 2);

    while (1) {
        print(cwd_buf); print(" $ ");

        memset(input_buf, 0, sizeof(input_buf));
        int n = read(0, input_buf, sizeof(input_buf) - 1);
        if (n <= 0) continue;

        while (n > 0 && (input_buf[n-1] == '\n' || input_buf[n-1] == '\r'))
            input_buf[--n] = '\0';
        if (n == 0) continue;

        int argc = parse_args(input_buf);
        if (argc == 0) continue;

        const char *cmd = argv_buf[0];

        if (strcmp(cmd, "exit") == 0) break;
        if (strcmp(cmd, "help") == 0) { builtin_help(); continue; }
        if (strcmp(cmd, "ls")   == 0) { builtin_ls(argv_buf[1]);  continue; }
        if (strcmp(cmd, "cat")  == 0) { builtin_cat(argv_buf[1]); continue; }
        if (strcmp(cmd, "cd")   == 0) { builtin_cd(argv_buf[1]);  continue; }
        if (strcmp(cmd, "pwd")  == 0) { builtin_pwd();            continue; }

        run_external(argc, argv_buf);
    }

    return 0;
}
