/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* slot: run a command in one of N slots of a shared resource (Metastaz slot standard).
 * Kernel side: slot claims on /dev/claim (include/uapi/claim.h, kernel/claim.c).
 *
 *   slot NAME N [--mem MB] [--mem-pressure PCT] [--wait SEC] [--purpose TEXT] -- CMD...
 *       takes the first free of N slots of NAME and runs CMD while holding it; when all
 *       N are held, waits for the first to free (at most SEC, default 1800; exit 75 after).
 *       --mem MB: start only while at least MB of memory is available (MemAvailable).
 *       --mem-pressure PCT: start only while memory pressure is under PCT (the "some
 *       avg10" of /proc/pressure/memory: the share of the last 10 s someone waited
 *       for memory). A machine already struggling takes no new heavy job.
 *       CMD gets SLOT_NO (1..N) in its environment. The slot ends when CMD ends, or
 *       when slot itself dies; "ranix-ajan stop ID" ends CMD.
 *   slot [NAME] --status
 *       who holds which slot (all slot claims, or NAME's).
 * The holder's name is $RANIX_AGENT, else $USER, else the process name. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define CLAIM_F_WAIT 0x4
struct claim_req {
	char res[16]; char holder[32]; char purpose[80];
	uint32_t secs, flags; int32_t stop_pid; uint32_t slots; uint64_t id;
	char busy_holder[32]; char busy_purpose[80]; int64_t busy_until; uint64_t busy_id;
};
#define C_TAKE 0xC1A0

static void usage(void)
{
	fprintf(stderr, "usage: slot NAME N [--mem MB] [--mem-pressure PCT] [--wait SEC] [--purpose TEXT] -- CMD...\n"
	                "       slot [NAME] --status\n");
}

static long mem_available_mb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char l[128]; long kb = -1;
	if (!f) return -1;
	while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %ld kB", &kb) == 1) break;
	fclose(f);
	return kb < 0 ? -1 : kb / 1024;
}

/* "some avg10" of /proc/pressure/memory, in percent; -1 without it */
static double mem_pressure(void)
{
	FILE *f = fopen("/proc/pressure/memory", "r");
	double v = -1;
	if (!f) return -1;
	if (fscanf(f, "some avg10=%lf", &v) != 1) v = -1;
	fclose(f);
	return v;
}

static int memory_ok(long mem_mb, double max_pressure)
{
	if (mem_mb > 0) { long a = mem_available_mb(); if (a >= 0 && a < mem_mb) return 0; }
	if (max_pressure > 0) { double p = mem_pressure(); if (p >= 0 && p >= max_pressure) return 0; }
	return 1;
}

/* tiny scanner over the kernel's own JSON: "key": value after p, before end */
static int jget(const char *p, const char *end, const char *key, char *out, size_t cap)
{
	char k[32]; snprintf(k, sizeof k, "\"%s\":", key);
	const char *q = strstr(p, k);
	out[0] = 0;
	if (!q || q > end) return -1;
	q += strlen(k);
	size_t n = 0;
	if (*q == '"') { q++; while (*q && *q != '"' && n + 1 < cap) { if (*q == '\\' && q[1]) q++; out[n++] = *q++; } }
	else while (*q && *q != ',' && *q != '}' && n + 1 < cap) out[n++] = *q++;
	out[n] = 0;
	return 0;
}

static int status(const char *name)
{
	static char b[32768];
	int fd = open("/dev/claim", O_RDONLY);
	if (fd < 0) { fprintf(stderr, "slot: /dev/claim: %s\n", strerror(errno)); return 2; }
	size_t n = 0; ssize_t r;
	while (n + 1 < sizeof b && (r = read(fd, b + n, sizeof b - 1 - n)) > 0) n += (size_t)r;
	b[n] = 0; close(fd);
	int shown = 0;
	const char *p = strstr(b, "\"claims\":[");
	for (p = p ? strchr(p, '{') : NULL; p; p = strchr(p + 1, '{')) {
		const char *end = strchr(p, '}');
		char mode[16], res[24], no[8], of[8], holder[40], purpose[90], pid[16], since[24];
		jget(p, end, "mode", mode, sizeof mode);
		if (strcmp(mode, "slot")) continue;
		jget(p, end, "res", res, sizeof res);
		if (name && strcmp(name, res)) continue;
		jget(p, end, "slot", no, sizeof no); jget(p, end, "slots", of, sizeof of);
		jget(p, end, "holder", holder, sizeof holder); jget(p, end, "purpose", purpose, sizeof purpose);
		jget(p, end, "pid", pid, sizeof pid); jget(p, end, "since", since, sizeof since);
		char at[16] = "-"; time_t t = (time_t)atol(since); struct tm tm;
		if (t > 0) { localtime_r(&t, &tm); strftime(at, sizeof at, "%H:%M:%S", &tm); }
		printf("%s slot %s/%s: %s, pid %s, since %s: %s\n", res, no, of, holder, pid, at, purpose);
		shown++;
	}
	if (!shown) printf("%s: no slot held\n", name ? name : "slot");
	return 0;
}

static void on_alarm(int s) { (void)s; }

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[argc - 1], "--status")) return status(argc >= 3 ? argv[1] : NULL);
	if (argc < 5) { usage(); return 2; }
	const char *name = argv[1];
	char *e; long n = strtol(argv[2], &e, 10);
	if (*e || n < 1 || n > 64) { fprintf(stderr, "slot: N must be 1..64\n"); return 2; }
	long mem = 0, wait = 1800; double max_pressure = 0; const char *purpose = NULL; int argi = 0;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { argi = i + 1; break; }
		else if (!strcmp(argv[i], "--mem") && i + 1 < argc) mem = atol(argv[++i]);
		else if (!strcmp(argv[i], "--mem-pressure") && i + 1 < argc) max_pressure = atof(argv[++i]);
		else if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atol(argv[++i]);
		else if (!strcmp(argv[i], "--purpose") && i + 1 < argc) purpose = argv[++i];
		else { usage(); return 2; }
	}
	if (!argi || argi >= argc) { usage(); return 2; }
	int fd = open("/dev/claim", O_RDWR);
	if (fd < 0) { fprintf(stderr, "slot: /dev/claim: %s\n", strerror(errno)); return 2; }

	/* the command waits on the gate until it holds a slot; it reads its number there */
	int gate[2]; if (pipe(gate)) return 2;
	pid_t ch = fork();
	if (ch < 0) return 2;
	if (ch == 0) {
		setpgid(0, 0);
		close(gate[1]); close(fd);
		char no[8] = { 0 };
		if (read(gate[0], no, sizeof no - 1) <= 0) _exit(75);
		setenv("SLOT_NO", no, 1);
		execvp(argv[argi], argv + argi);
		perror("slot: exec"); _exit(127);
	}
	close(gate[0]);
	setpgid(ch, ch);

	struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);              /* no SA_RESTART: the wait returns EINTR */
	time_t deadline = time(NULL) + wait;
	alarm((unsigned)(wait > 0 ? wait : 1));

	struct claim_req q;
	int got = 0;
	while (!got) {
		while (!memory_ok(mem, max_pressure)) {
			if (time(NULL) >= deadline) goto timeout;
			sleep(2);
		}
		memset(&q, 0, sizeof q);
		snprintf(q.res, sizeof q.res, "%s", name);
		const char *who = getenv("RANIX_AGENT");
		if (!who || !*who) who = getenv("USER");
		if (who && *who) snprintf(q.holder, sizeof q.holder, "%s", who);   /* empty: the kernel names the process */
		if (purpose) snprintf(q.purpose, sizeof q.purpose, "%s", purpose);
		else {
			size_t k = 0;
			for (int i = argi; i < argc && k + 2 < sizeof q.purpose; i++)
				k += (size_t)snprintf(q.purpose + k, sizeof q.purpose - k, "%s%s", i > argi ? " " : "", argv[i]);
		}
		q.slots = (uint32_t)n; q.flags = CLAIM_F_WAIT; q.stop_pid = ch;
		if (ioctl(fd, C_TAKE, &q) == 0) { got = 1; break; }
		if (errno == EINTR && time(NULL) >= deadline) goto timeout;
		if (errno != EINTR) {
			fprintf(stderr, "slot: %s: %s\n", name, errno == EBUSY ? "held by another kind of claim" : strerror(errno));
			if (errno == EBUSY) fprintf(stderr, "slot: in the way: \"%s\" (%s)\n", q.busy_holder, q.busy_purpose);
			kill(ch, SIGKILL); waitpid(ch, NULL, 0);
			return errno == EBUSY ? 75 : 2;
		}
	}
	alarm(0);
	char no[8]; snprintf(no, sizeof no, "%u", q.slots);
	if (write(gate[1], no, strlen(no)) != (ssize_t)strlen(no)) return 2;
	close(gate[1]);
	signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
	int st = 0;
	while (waitpid(ch, &st, 0) < 0 && errno == EINTR) ;
	close(fd);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);

timeout:
	fprintf(stderr, "slot %s: no free slot after %ld s\n", name, wait);
	kill(ch, SIGKILL); waitpid(ch, NULL, 0);
	return 75;
}
