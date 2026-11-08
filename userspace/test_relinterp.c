/* A relative PT_INTERP ("./ld.so") is found from the working directory, as
 * on Linux. VS Code's C/C++ language server ships its own musl that way and
 * failed with ENOEXEC ("cpptools: ELF: not found" from the shell fallback).
 * Copies /bin/test_dynamic, points its interpreter at "./L", puts the real
 * loader there as L, and runs it from that directory. */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int copy(const char *a, const char *b, char **data, size_t *n)
{
	int f = open(a, O_RDONLY);
	if (f < 0) return -1;
	struct stat st;
	fstat(f, &st);
	char *d = malloc(st.st_size);
	if (!d || read(f, d, st.st_size) != st.st_size) { close(f); return -1; }
	close(f);
	if (data) { *data = d; *n = st.st_size; return 0; }
	int o = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (o < 0 || write(o, d, st.st_size) != st.st_size) return -1;
	close(o);
	free(d);
	return 0;
}

int main(int argc, char **argv)
{
	char *d; size_t n;
	const char *src = argc > 1 ? argv[1] : "/bin/test_dynamic";   /* elsewhere: any dynamic program printing dynamic-ok */
	if (copy(src, NULL, &d, &n)) { printf("FAIL read test_dynamic\n"); return 0; }
	Elf64_Ehdr *eh = (Elf64_Ehdr *)d;
	Elf64_Phdr *ph = (Elf64_Phdr *)(d + eh->e_phoff);
	char interp[256] = "";
	int done = 0;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_INTERP && ph[i].p_filesz > 4) {
			snprintf(interp, sizeof interp, "%s", d + ph[i].p_offset);
			memset(d + ph[i].p_offset, 0, ph[i].p_filesz);
			memcpy(d + ph[i].p_offset, "./L", 4);
			done = 1;
		}
	if (!done) { printf("FAIL no PT_INTERP\n"); return 0; }
	char dir[] = "/tmp/relinterpXXXXXX";
	if (!mkdtemp(dir) || chdir(dir)) { printf("FAIL tmpdir\n"); return 0; }
	int o = open("prog", O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (o < 0 || write(o, d, n) != (ssize_t)n) { printf("FAIL write\n"); return 0; }
	close(o);
	if (copy(interp, "L", NULL, NULL)) { printf("FAIL copy loader %s\n", interp); return 0; }
	int p[2];
	if (pipe(p)) return 0;
	pid_t c = fork();
	if (c == 0) {
		dup2(p[1], 1);
		execl("./prog", "prog", (char *)0);
		_exit(9);
	}
	close(p[1]);
	char out[64] = "";
	ssize_t r = read(p[0], out, sizeof out - 1);
	out[r > 0 ? r : 0] = 0;
	int st;
	waitpid(c, &st, 0);
	unlink("prog"); unlink("L"); chdir("/"); rmdir(dir);
	if (strncmp(out, "dynamic-ok", 10)) { printf("FAIL relative interpreter: '%s' status %d\n", out, st); return 0; }
	printf("OK\n");
	return 0;
}
