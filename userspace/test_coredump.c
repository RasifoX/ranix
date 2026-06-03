/* core dump: a process killed by SIGSEGV leaves an ELF ET_CORE file. We verify
 * the structure (ELF magic, e_type==ET_CORE, a PT_NOTE + PT_LOAD program
 * headers) and that real memory was dumped — a sentinel written into an mmap'd
 * page is found somewhere in the core's PT_LOAD bodies. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define SENTINEL "RANIX-CORE-SENTINEL-7f3a"
#define ET_CORE 4

int main(void)
{
	unlink("/tmp/core");

	pid_t pid = fork();
	if (pid == 0) {
		if (chdir("/tmp") != 0) _exit(40);
		char *pg = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (pg == MAP_FAILED) _exit(41);
		memcpy(pg, SENTINEL, sizeof(SENTINEL));
		*(volatile int *)0 = 1;   /* SIGSEGV */
		_exit(42);
	}

	int st;
	waitpid(pid, &st, 0);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != 11) {
		printf("coredump-fail signal st=%d\n", st); return 0;
	}

	int fd = open("/tmp/core", O_RDONLY);
	if (fd < 0) { printf("coredump-fail nocore\n"); return 0; }

	static char buf[2 * 1024 * 1024];
	long total = 0, r;
	while (total < (long)sizeof(buf) && (r = read(fd, buf + total, sizeof(buf) - total)) > 0)
		total += r;
	close(fd);

	if (total < 64 || memcmp(buf, "\177ELF", 4) != 0) { printf("coredump-fail magic\n"); return 0; }
	unsigned short e_type = *(unsigned short *)(buf + 16);
	unsigned short e_phnum = *(unsigned short *)(buf + 56);
	if (e_type != ET_CORE) { printf("coredump-fail type=%d\n", e_type); return 0; }
	if (e_phnum < 2) { printf("coredump-fail phnum=%d\n", e_phnum); return 0; }

	/* PT_NOTE (type 4) present among program headers? */
	unsigned long phoff = *(unsigned long *)(buf + 32);
	int saw_note = 0, saw_load = 0;
	for (int i = 0; i < e_phnum; i++) {
		unsigned int *ph = (unsigned int *)(buf + phoff + (long)i * 56);
		if (ph[0] == 4) saw_note = 1;
		if (ph[0] == 1) saw_load = 1;
	}
	if (!saw_note || !saw_load) { printf("coredump-fail ph note=%d load=%d\n", saw_note, saw_load); return 0; }

	/* the mmap'd sentinel must appear in the dumped memory */
	int found = 0;
	for (long i = 0; i + (long)sizeof(SENTINEL) <= total; i++) {
		if (buf[i] == 'R' && memcmp(buf + i, SENTINEL, sizeof(SENTINEL)) == 0) { found = 1; break; }
	}
	if (!found) { printf("coredump-fail sentinel total=%ld\n", total); return 0; }

	printf("OK\n");
	return 0;
}
