/* ranix-ajan: claims on machine resources, for agents and people.
 * Kernel side: kernel/claim.c, include/uapi/claim.h (/dev/claim).
 *
 *   ranix-ajan [list] [--json]          who holds what (JSON: the /dev/claim snapshot)
 *   ranix-ajan claim RES "PURPOSE" [-t 30m] [-a NAME] [--shared] [--wait] [--pid PID]
 *                                        detached claim, ends at expiry or release; prints id
 *   ranix-ajan run RES "PURPOSE" [-t DUR] [-a NAME] [--shared] [--wait] -- CMD...
 *                                        claim held while CMD runs; "stop" ends CMD
 *   ranix-ajan release ID|RES [-a NAME]  ranix-ajan extend ID DUR   ranix-ajan stop ID
 *   ranix-ajan check RES [-a NAME]       exit 0 free (or ours), 1 busy (says who)
 *   ranix-ajan watch                     one JSON line per change
 * NAME defaults to $RANIX_AGENT, else $USER. Resources: display input gpu reboot
 * network wifi disk audio usb camera, or any [a-z0-9._:-]{1,15}. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define CLAIM_F_SHARED 0x1
#define CLAIM_F_DETACH 0x2
#define CLAIM_F_WAIT   0x4
struct claim_req {
	char res[16]; char holder[32]; char purpose[80];
	uint32_t secs, flags; int32_t stop_pid; uint32_t pad; uint64_t id;
	char busy_holder[32]; char busy_purpose[80]; int64_t busy_until; uint64_t busy_id;
};
#define C_TAKE 0xC1A0
#define C_RELEASE 0xC1A1
#define C_EXTEND 0xC1A2
#define C_STOP 0xC1A3
#define C_CHECK 0xC1A4

static int dev(void)
{
	int fd = open("/dev/claim", O_RDWR);
	if (fd < 0) { fprintf(stderr, "ranix-ajan: /dev/claim: %s (kernel without claims?)\n", strerror(errno)); exit(2); }
	return fd;
}

static long dur(const char *s)
{
	char *e; long v = strtol(s, &e, 10);
	if (e == s || v <= 0) return -1;
	switch (*e) { case 0: case 's': return v; case 'm': return v * 60; case 'h': return v * 3600; case 'd': return v * 86400; }
	return -1;
}

static const char *me_name(const char *opt)
{
	if (opt) return opt;
	const char *a = getenv("RANIX_AGENT");
	if (a && *a) return a;
	a = getenv("USER");
	return a && *a ? a : "unknown";
}

static void busy_msg(const struct claim_req *q)
{
	char until[32] = "no expiry";
	if (q->busy_until) { time_t t = (time_t)q->busy_until; struct tm tm; localtime_r(&t, &tm); strftime(until, sizeof until, "until %H:%M", &tm); }
	fprintf(stderr, "busy: %s held by \"%s\" (%s), %s, claim %llu\n", q->res, q->busy_holder, q->busy_purpose,
	        until, (unsigned long long)q->busy_id);
}

static char *snapshot(int fd, char *b, size_t cap)
{
	size_t n = 0; ssize_t r;
	while (n + 1 < cap && (r = read(fd, b + n, cap - 1 - n)) > 0) n += (size_t)r;
	b[n] = 0;
	return b;
}

/* tiny scanner over our own JSON: finds "key": value after p */
static const char *jget(const char *p, const char *end, const char *key, char *out, size_t cap)
{
	char k[32]; snprintf(k, sizeof k, "\"%s\":", key);
	const char *q = strstr(p, k);
	out[0] = 0;
	if (!q || q > end) return NULL;
	q += strlen(k);
	size_t n = 0;
	if (*q == '"') { q++; while (*q && *q != '"' && n + 1 < cap) { if (*q == '\\' && q[1]) q++; out[n++] = *q++; } }
	else while (*q && *q != ',' && *q != '}' && n + 1 < cap) out[n++] = *q++;
	out[n] = 0;
	return q;
}

static int cmd_list(int json)
{
	static char b[32768];
	int fd = dev();
	snapshot(fd, b, sizeof b);
	close(fd);
	if (json) { fputs(b, stdout); return 0; }
	const char *p = strstr(b, "\"claims\":[");
	if (!p || !strchr(p, '{')) { printf("no claims\n"); }
	else {
		printf("%-4s %-10s %-9s %-18s %-30s %s\n", "ID", "RES", "MODE", "HOLDER", "PURPOSE", "UNTIL");
		for (p = strchr(p, '{'); p; p = strchr(p + 1, '{')) {
			const char *end = strchr(p, '}');
			char id[24], res[24], mode[16], holder[40], purpose[90], until[24], left[24], att[8];
			jget(p, end, "id", id, sizeof id); jget(p, end, "res", res, sizeof res);
			jget(p, end, "mode", mode, sizeof mode); jget(p, end, "holder", holder, sizeof holder);
			jget(p, end, "purpose", purpose, sizeof purpose); jget(p, end, "until", until, sizeof until);
			jget(p, end, "left_s", left, sizeof left); jget(p, end, "attached", att, sizeof att);
			char u[48] = "-";
			long ut = atol(until), ls = atol(left);
			if (ut > 0) { time_t t = (time_t)ut; struct tm tm; localtime_r(&t, &tm); strftime(u, sizeof u, "%H:%M", &tm);
				snprintf(u + strlen(u), sizeof u - strlen(u), " (%ld min)", (ls + 59) / 60); }
			if (!strcmp(att, "true")) strncat(u, " while running", sizeof u - strlen(u) - 1);
			printf("%-4s %-10s %-9s %-18s %-30.30s %s\n", id, res, mode, holder, purpose, u);
		}
	}
	FILE *r = fopen("/dev/rescue", "r");
	if (r) {
		char l[512];
		while (fgets(l, sizeof l, r)) if (!strncmp(l, "kwatch ", 7) || !strncmp(l, "port=", 5)) printf("%s", !strncmp(l, "port=", 5) ? "rescue: " : ""), fputs(l, stdout);
		fclose(r);
	}
	return 0;
}

struct opts { const char *name; long secs; unsigned flags; int pid; int argi; };

static int parse_opts(int argc, char **argv, int i, struct opts *o)
{
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--")) { o->argi = i + 1; return 0; }
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) { o->secs = dur(argv[++i]); if (o->secs < 0) { fprintf(stderr, "bad duration\n"); return -1; } }
		else if (!strcmp(argv[i], "-a") && i + 1 < argc) o->name = argv[++i];
		else if (!strcmp(argv[i], "--pid") && i + 1 < argc) o->pid = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shared")) o->flags |= CLAIM_F_SHARED;
		else if (!strcmp(argv[i], "--wait")) o->flags |= CLAIM_F_WAIT;
		else { fprintf(stderr, "unknown option %s\n", argv[i]); return -1; }
	}
	return 0;
}

static void fill(struct claim_req *q, const char *res, const char *purpose, const struct opts *o)
{
	memset(q, 0, sizeof *q);
	snprintf(q->res, sizeof q->res, "%s", res);
	snprintf(q->holder, sizeof q->holder, "%s", me_name(o->name));
	snprintf(q->purpose, sizeof q->purpose, "%s", purpose);
	q->secs = (uint32_t)(o->secs > 0 ? o->secs : 0);
	q->flags = o->flags;
	q->stop_pid = o->pid;
}

static int find_id(int fd, const char *arg, const char *holder, uint64_t *id)
{
	if (arg[0] >= '0' && arg[0] <= '9') { *id = strtoull(arg, NULL, 10); return 0; }
	static char b[32768];
	snapshot(fd, b, sizeof b);
	for (const char *p = strchr(b, '{'); p && (p = strchr(p + 1, '{')); ) {
		const char *end = strchr(p, '}');
		char res[24], h[40], ids[24];
		jget(p, end, "res", res, sizeof res); jget(p, end, "holder", h, sizeof h); jget(p, end, "id", ids, sizeof ids);
		if (!strcmp(res, arg) && !strcmp(h, holder)) { *id = strtoull(ids, NULL, 10); return 0; }
	}
	fprintf(stderr, "no claim on %s by \"%s\"\n", arg, holder);
	return -1;
}

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "list";
	if (!strcmp(c, "list") || !strcmp(c, "--json")) return cmd_list((argc > 1 && !strcmp(argv[argc - 1], "--json")));
	if (!strcmp(c, "-h") || !strcmp(c, "--help") || !strcmp(c, "help")) {
		fprintf(stderr, "usage: ranix-ajan [list] [--json] | claim RES PURPOSE [-t DUR] [-a NAME] [--shared] [--wait] [--pid PID]\n"
		                "       | run RES PURPOSE [opts] -- CMD... | release ID|RES | extend ID DUR | stop ID | check RES | watch\n");
		return 0;
	}
	if (!strcmp(c, "watch")) {
		int fd = dev();
		static char b[32768];
		for (;;) {
			snapshot(fd, b, sizeof b);
			if (b[0]) { fputs(b, stdout); fflush(stdout); }
			struct pollfd p = { fd, POLLIN, 0 };
			if (poll(&p, 1, -1) < 0 && errno != EINTR) return 1;
		}
	}
	if ((!strcmp(c, "claim") || !strcmp(c, "run")) && argc >= 4) {
		struct opts o = { 0, -1, 0, 0, 0 };
		if (parse_opts(argc, argv, 4, &o)) return 2;
		struct claim_req q;
		int fd = dev();
		if (!strcmp(c, "claim")) {
			if (o.secs < 0) o.secs = 30 * 60;
			o.flags |= CLAIM_F_DETACH;
			fill(&q, argv[2], argv[3], &o);
			if (ioctl(fd, C_TAKE, &q) < 0) {
				if (errno == EBUSY) { busy_msg(&q); return 1; }
				fprintf(stderr, "claim: %s\n", strerror(errno)); return 2;
			}
			printf("%llu\n", (unsigned long long)q.id);
			return 0;
		}
		if (!o.argi || o.argi >= argc) { fprintf(stderr, "run: missing -- CMD\n"); return 2; }
		int gate[2]; if (pipe(gate)) return 2;
		pid_t ch = fork();
		if (ch == 0) {
			setpgid(0, 0);
			close(gate[1]);
			char x; if (read(gate[0], &x, 1) != 1) _exit(75);
			close(fd);
			execvp(argv[o.argi], argv + o.argi);
			perror("exec"); _exit(127);
		}
		close(gate[0]);
		setpgid(ch, ch);
		o.pid = ch;
		fill(&q, argv[2], argv[3], &o);
		if (ioctl(fd, C_TAKE, &q) < 0) {
			int e = errno;
			kill(ch, SIGKILL); waitpid(ch, NULL, 0);
			if (e == EBUSY) { busy_msg(&q); return 75; }
			fprintf(stderr, "claim: %s\n", strerror(e)); return 2;
		}
		fprintf(stderr, "claim %llu on %s held while the command runs\n", (unsigned long long)q.id, q.res);
		if (write(gate[1], "g", 1) != 1) return 2;
		close(gate[1]);
		signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN);
		int st = 0;
		while (waitpid(ch, &st, 0) < 0 && errno == EINTR) ;
		close(fd);
		return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
	}
	if ((!strcmp(c, "release") || !strcmp(c, "stop")) && argc >= 3) {
		struct opts o = { 0, -1, 0, 0, 0 };
		if (parse_opts(argc, argv, 3, &o)) return 2;
		int fd = dev();
		struct claim_req q; memset(&q, 0, sizeof q);
		if (find_id(fd, argv[2], me_name(o.name), &q.id)) return 1;
		if (ioctl(fd, !strcmp(c, "stop") ? C_STOP : C_RELEASE, &q) < 0) { fprintf(stderr, "%s: %s\n", c, strerror(errno)); return 1; }
		return 0;
	}
	if (!strcmp(c, "extend") && argc >= 4) {
		int fd = dev();
		struct claim_req q; memset(&q, 0, sizeof q);
		q.id = strtoull(argv[2], NULL, 10);
		long s = dur(argv[3]); if (s < 0) { fprintf(stderr, "bad duration\n"); return 2; }
		q.secs = (uint32_t)s;
		if (ioctl(fd, C_EXTEND, &q) < 0) { fprintf(stderr, "extend: %s\n", strerror(errno)); return 1; }
		return 0;
	}
	if (!strcmp(c, "check") && argc >= 3) {
		struct opts o = { 0, -1, 0, 0, 0 };
		if (parse_opts(argc, argv, 3, &o)) return 2;
		int fd = dev();
		struct claim_req q; fill(&q, argv[2], "", &o);
		if (ioctl(fd, C_CHECK, &q) < 0) { if (errno == EBUSY) { busy_msg(&q); return 1; } perror("check"); return 2; }
		printf("free\n");
		return 0;
	}
	fprintf(stderr, "ranix-ajan: bad command (try --help)\n");
	return 2;
}
