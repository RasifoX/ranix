#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#define ARGV_MAX  32
#define INPUT_MAX 1024
#define PATH_MAX  280
#define TIOCSPROMPT 0x541F

static char input_buf[INPUT_MAX];
static char path_buf[PATH_MAX];
static char cwd_buf[PATH_MAX];
static int  wait_status;

static void print(const char *s)   { write(1, s, strlen(s)); }
static void println(const char *s) { print(s); write(1, "\n", 1); }

static void strip_quotes(char *s) {
    if (!s) return;
    int len = strlen(s);
    if (len >= 2 && s[0] == '"' && s[len-1] == '"') {
        memmove(s, s + 1, (size_t)len - 2);
        s[len-2] = '\0';
    } else if (len >= 2 && s[0] == '\'' && s[len-1] == '\'') {
        memmove(s, s + 1, (size_t)len - 2);
        s[len-2] = '\0';
    }
}

static int parse_args(char *line, char **argv_buf)
{
    int argc = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (argc >= ARGV_MAX) break;
        argv_buf[argc++] = p;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) p++;
            if (*p) p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '|' && *p != ';') p++;
        }
        if (*p) *p++ = '\0';
        strip_quotes(argv_buf[argc-1]);
    }
    argv_buf[argc] = NULL;
    return argc;
}

static void builtin_help(void) { println("Built-ins: ls [path], cat <file>, cd [path], pwd, help, exit. Supported: |, ;"); }
static void builtin_pwd(void) { println(cwd_buf); }

#define DT_DIR 4
struct dirent64 { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; };
static long raw_open(const char *path, int flags) {
#if defined(__x86_64__)
    long fd; asm volatile ("mov $257, %%rax; mov $-100, %%rdi; mov %1, %%rsi; mov %2, %%rdx; xor %%r10, %%r10; syscall; mov %%rax, %0;" : "=r"(fd) : "r"(path), "r"((long)flags) : "rax","rdi","rsi","rdx","r10","memory"); return fd;
#else
    return open(path, flags);
#endif
}
static long raw_getdents64(int fd, void *buf, long len) {
#if defined(__x86_64__)
    long ret; asm volatile ("mov $217, %%rax; mov %1, %%rdi; mov %2, %%rsi; mov %3, %%rdx; syscall; mov %%rax, %0;" : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(len) : "rax","rdi","rsi","rdx","memory"); return ret;
#else
    return -1;
#endif
}
static void builtin_ls(const char *path) {
    if (!path) path = ".";
    long fd = raw_open(path, 0x84000);
    if (fd < 0) { print("ls: cannot open: "); println(path); return; }
    char buf[2048]; long n;
    while ((n = raw_getdents64((int)fd, buf, sizeof(buf))) > 0) {
        long off = 0;
        while (off < n) {
            struct dirent64 *de = (struct dirent64 *)(buf + off);
            if (!(de->d_name[0] == '.' && (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))) {
                print(de->d_name); if (de->d_type == DT_DIR) print("/"); println("");
            }
            off += de->d_reclen;
        }
    }
    close((int)fd);
}
static void builtin_cat(const char *path) {
    if (!path) { println("cat: missing argument"); return; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) { print("cat: cannot open: "); println(path); return; }
    char buf[1024]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
    close(fd);
}
static void builtin_cd(const char *path) {
    if (!path) path = "/";
    if (chdir(path) < 0) { print("cd: cannot cd to: "); println(path); return; }
    getcwd(cwd_buf, sizeof(cwd_buf));
}

static void run_pipeline(char *line) {
    char *cmds[16]; int ncmds = 0; char *p = line; cmds[ncmds++] = p;
    while (*p) { if (*p == '|') { *p = '\0'; if (ncmds < 16) cmds[ncmds++] = p + 1; } p++; }
    if (ncmds == 1) {
        char *argv[ARGV_MAX + 1]; int argc = parse_args(cmds[0], argv);
        if (argc == 0) return;
        const char *cmd = argv[0];
        if (strcmp(cmd, "exit") == 0) exit(0);
        if (strcmp(cmd, "help") == 0) { builtin_help(); return; }
        if (strcmp(cmd, "ls")   == 0) { builtin_ls(argv[1]);  return; }
        if (strcmp(cmd, "cat")  == 0) { builtin_cat(argv[1]); return; }
        if (strcmp(cmd, "cd")   == 0) { builtin_cd(argv[1]);  return; }
        if (strcmp(cmd, "pwd")  == 0) { builtin_pwd();        return; }
    }
    int pipes[16][2]; long pids[16];
    for (int i = 0; i < ncmds; i++) {
        if (i < ncmds - 1) { if (pipe(pipes[i]) < 0) return; }
        long pid = vfork(); pids[i] = pid;
        if (pid == 0) {
            if (i > 0) dup2(pipes[i-1][0], 0);
            if (i < ncmds - 1) dup2(pipes[i][1], 1);
            for (int j = 0; j <= i; j++) { if (j < ncmds - 1) { close(pipes[j][0]); close(pipes[j][1]); } }
            char *argv[ARGV_MAX + 1]; int argc = parse_args(cmds[i], argv);
            if (argc == 0) exit(0);
            const char *cmd = argv[0];
            if (strcmp(cmd, "help") == 0) { builtin_help(); exit(0); }
            if (strcmp(cmd, "ls") == 0) { builtin_ls(argv[1]); exit(0); }
            if (strcmp(cmd, "cat") == 0) { builtin_cat(argv[1]); exit(0); }
            if (strcmp(cmd, "pwd") == 0) { builtin_pwd(); exit(0); }
            if (strcmp(cmd, "cd") == 0 || strcmp(cmd, "exit") == 0) exit(0);
            if (cmd[0] != '/') snprintf(path_buf, PATH_MAX, "/bin/%s", cmd);
            else strncpy(path_buf, cmd, PATH_MAX - 1);
            argv[0] = path_buf;
            execve(path_buf, argv, NULL);
            print(path_buf); println(": not found"); exit(127);
        }
        if (i > 0) { close(pipes[i-1][0]); close(pipes[i-1][1]); }
    }
    for (int i = 0; i < ncmds; i++) { if (pids[i] > 0) waitpid((pid_t)pids[i], &wait_status, 0); }
}

int main(void) {
    println("\nRanix shell (v2.1) — type 'help'");
    if (!getcwd(cwd_buf, sizeof(cwd_buf))) strncpy(cwd_buf, "/", 2);
    while (1) {
        char prompt[PATH_MAX + 16]; snprintf(prompt, sizeof(prompt), "%s $ ", cwd_buf);
        ioctl(0, TIOCSPROMPT, prompt); print(prompt);
        memset(input_buf, 0, sizeof(input_buf));
        int n = read(0, input_buf, sizeof(input_buf) - 1);
        if (n <= 0) continue;
        input_buf[n] = '\0';
        char *line = input_buf;
        while (line && *line) {
            char *next = strchr(line, ';'); if (next) *next = '\0';
            while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r') line++;
            if (*line) run_pipeline(line);
            if (next) line = next + 1; else break;
        }
    }
    return 0;
}
