#include <unistd.h>
#include <sys/wait.h>

static int mystrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void mystrcpy(char *d, const char *s) { while ((*d++ = *s++)); }
static int mystrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void print(const char *s) { write(1, s, mystrlen(s)); }

static void run(const char *path)
{
    pid_t pid = vfork();
    if (pid == 0) {
        /* child */
        char *argv[] = { (char *)path, 0 };
        execve(path, argv, 0);
        print("sh: not found\n");
        _exit(127);
    }
    /* parent */
    int status;
    waitpid(pid, &status, 0);
}

int main(void)
{
    print("\nRanix shell — type 'help' for commands\n");

    char buf[256];
    char path[280];

    while (1) {
        print("$ ");

        /* read a line */
        int n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) continue;

        /* strip trailing newline/CR */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) n--;
        if (n == 0) continue;
        buf[n] = '\0';

        if (mystrcmp(buf, "exit") == 0) break;

        if (mystrcmp(buf, "help") == 0) {
            print("Commands: exit, and any binary in /bin/\n");
            continue;
        }

        /* build path: /bin/<cmd> */
        if (buf[0] == '/') {
            mystrcpy(path, buf);
        } else {
            mystrcpy(path, "/bin/");
            mystrcpy(path + 5, buf);
        }

        run(path);
    }

    return 0;
}
