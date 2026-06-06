/* /proc/self/cmdline must return the full NUL-separated argv, not the comm. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* find a token (a whole NUL-separated segment) in a cmdline buffer */
static int has_token(const char *buf, ssize_t n, const char *want)
{
	ssize_t i = 0;
	while (i < n) {
		const char *seg = buf + i;
		size_t len = strlen(seg);
		if (strstr(seg, want)) return 1;
		i += (ssize_t)len + 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd = open("/proc/self/cmdline", O_RDONLY);
	if (fd < 0) { puts("FAIL open"); return 1; }
	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) { puts("FAIL read"); return 1; }
	buf[n] = '\0';

	if (!has_token(buf, n, "test_proccmdline")) { puts("FAIL argv0"); return 1; }

	if (argc < 2) {
		char *av[] = { argv[0], "ranix-marker", NULL };
		char *ev[] = { NULL };
		execve(argv[0], av, ev);
		puts("FAIL execve");
		return 1;
	}

	size_t toks = 0;
	for (ssize_t i = 0; i < n; i++) if (buf[i] == '\0') toks++;
	if (toks < 2) { puts("FAIL tokens"); return 1; }
	if (!has_token(buf, n, "ranix-marker")) { puts("FAIL arg1"); return 1; }
	puts("OK");
	return 0;
}
