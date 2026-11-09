/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ranix-sess: run a command inside a new agent session, and stage what it writes.
 *
 *   ranix-sess run -r ROOT [-u UID] [-g GID] [-l LABEL] [-t MAX_TASKS] [-m TMP_MIB]
 *                  [-M MEM_MIB] [-w DIR | -S DIR [-n NAME]] -- CMD [ARG...]
 *   ranix-sess diff NAME
 *   ranix-sess snapshot NAME
 *   ranix-sess rollback NAME [SNAP]
 *   ranix-sess commit NAME [-f]
 *
 * ROOT becomes / inside, read-only, with a private /tmp and filtered /proc and /dev
 * if it has those directories. The session keeps stdin, stdout and stderr, which
 * must be pipes, sockets, terminals or /dev/null. ranix-sess stays outside as the
 * supervisor: it waits for the command and exits with its status, and when it dies
 * or is signalled the session dies with it. GID defaults to UID.
 *
 * -w DIR stays writable, and what the session writes there is on the host at once:
 * there is no undo. It keeps its place if it lies under ROOT; otherwise it appears
 * at its own path, which ROOT must have, unless that is under /tmp.
 *
 * -S DIR is staged instead. DIR is copied into a state directory,
 * $RANIX_SESS_DIR/NAME (default /tmp/ranix-sess/NAME, NAME default sPID), and the
 * kernel binds that copy at DIR's place in the view, so DIR itself cannot be
 * written from inside. The copy is snapshot 0. After the command the changes
 * wait for a decision:
 *   diff      lists them against snapshot 0: A added, M modified, D deleted,
 *             T type changed.
 *   snapshot  copies the stage as the next snapshot and prints its number.
 *   rollback  with SNAP puts the stage back to that snapshot, also while the
 *             session runs. Without SNAP it discards the stage and the state
 *             directory: DIR never saw any of it.
 *   commit    applies the changes to DIR as the user running it, then removes the
 *             state. It refuses, applying nothing, when DIR has changed since the
 *             copy at a path the session also changed (-f applies anyway). It never
 *             follows a symlink in DIR, makes staged symlinks as symlinks, and drops
 *             set-user-ID and set-group-ID bits. An interrupted commit is finished
 *             by running commit again. Hard links in the stage become copies.
 * run -S NAME again continues the same stage. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "../include/uapi/asess.h"

static volatile pid_t session_pid;

static void forward(int sig)
{
	(void)sig;
	if (session_pid > 0) kill(session_pid, SIGKILL);
}

static int usage(void)
{
	fputs("usage: ranix-sess run -r ROOT [-u UID] [-g GID] [-l LABEL] [-t MAX_TASKS]"
	      " [-m TMP_MIB] [-M MEM_MIB] [-w DIR | -S DIR [-n NAME]] -- CMD [ARG...]\n"
	      "       ranix-sess diff|snapshot NAME\n"
	      "       ranix-sess rollback NAME [SNAP]\n"
	      "       ranix-sess commit NAME [-f]\n", stderr);
	return 2;
}

static void die(const char *what)
{
	fprintf(stderr, "ranix-sess: %s: %s\n", what, strerror(errno));
	exit(125);
}

/* ── trees ──────────────────────────────────────────────────── */

typedef struct {
	char    *path;      /* relative, no leading slash */
	char     type;      /* d f l p */
	mode_t   mode;      /* permission bits */
	uid_t    uid;
	gid_t    gid;
	off_t    size;
	uint64_t hash;      /* FNV-1a of the bytes, or of a link's target */
} ent_t;

typedef struct { ent_t *v; size_t n, cap; } tree_t;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}
#define FNV0 1469598103934665603ULL

static char type_of(mode_t m)
{
	return S_ISDIR(m) ? 'd' : S_ISREG(m) ? 'f' : S_ISLNK(m) ? 'l' : S_ISFIFO(m) ? 'p' : 0;
}

/* a directory entry opened as a directory, never through a symlink: O_NOFOLLOW is
 * checked with fstatat as well */
static int open_dir_nf(int dir, const char *name)
{
	struct stat st;
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return -1;
	if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
	return openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
}

/* fill e from name in dir; 0, or -1 if absent or of a kind that is not staged */
static int ent_at(int dir, const char *name, ent_t *e)
{
	struct stat st;
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return -1;
	e->type = type_of(st.st_mode);
	if (!e->type) return -1;
	e->mode = st.st_mode & 07777;
	e->uid = st.st_uid;
	e->gid = st.st_gid;
	e->size = e->type == 'f' || e->type == 'l' ? st.st_size : 0;
	e->hash = FNV0;
	if (e->type == 'l') {
		char t[PATH_MAX];
		ssize_t n = readlinkat(dir, name, t, sizeof(t));
		if (n < 0) return -1;
		e->hash = fnv(FNV0, t, (size_t)n);
		e->size = n;
	} else if (e->type == 'f') {
		int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW);
		if (fd < 0) return -1;
		char b[16384];
		ssize_t n;
		off_t total = 0;
		while ((n = read(fd, b, sizeof(b))) > 0) { e->hash = fnv(e->hash, b, (size_t)n); total += n; }
		close(fd);
		if (n < 0) return -1;
		e->size = total;
	}
	return 0;
}

static void tree_add(tree_t *t, const ent_t *e, const char *path)
{
	if (t->n == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 64;
		t->v = realloc(t->v, t->cap * sizeof(ent_t));
		if (!t->v) die("memory");
	}
	t->v[t->n] = *e;
	t->v[t->n].path = strdup(path);
	if (!t->v[t->n].path) die("memory");
	t->n++;
}

static void walk(int dir, const char *prefix, tree_t *t)
{
	int fd = dup(dir);
	DIR *d = fd < 0 ? NULL : fdopendir(fd);
	if (!d) die("open directory");
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		ent_t e;
		if (ent_at(dir, de->d_name, &e)) continue;
		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s%s%s", prefix, *prefix ? "/" : "", de->d_name) >= (int)sizeof(path))
			continue;
		tree_add(t, &e, path);
		if (e.type == 'd') {
			int sub = open_dir_nf(dir, de->d_name);
			if (sub < 0) die(path);
			walk(sub, path, t);
			close(sub);
		}
	}
	closedir(d);
}

static int ent_cmp(const void *a, const void *b)
{
	return strcmp(((const ent_t *)a)->path, ((const ent_t *)b)->path);
}

static void tree_of(int dir, tree_t *t)
{
	memset(t, 0, sizeof(*t));
	walk(dir, "", t);
	qsort(t->v, t->n, sizeof(ent_t), ent_cmp);
}

/* the same content and kind; owners are not compared, a copy made without root
 * cannot keep them */
static int ent_eq(const ent_t *a, const ent_t *b)
{
	if (!a || !b) return a == b;
	return a->type == b->type && a->mode == b->mode && a->size == b->size && a->hash == b->hash;
}

/* origin manifest: "T MODE UID GID SIZE HASH PATH\0" */
static void tree_save(const tree_t *t, int dir, const char *name)
{
	char tmp[64];
	snprintf(tmp, sizeof(tmp), "%s.new", name);
	int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
	if (!f) die("manifest");
	for (size_t i = 0; i < t->n; i++) {
		const ent_t *e = &t->v[i];
		fprintf(f, "%c %o %u %u %lld %016llx %s", e->type, (unsigned)e->mode, (unsigned)e->uid,
		        (unsigned)e->gid, (long long)e->size, (unsigned long long)e->hash, e->path);
		fputc(0, f);
	}
	if (fflush(f) || fsync(fileno(f)) || fclose(f)) die("manifest");
	if (renameat(dir, tmp, dir, name) < 0) die("manifest");
}

static void tree_load(tree_t *t, int dir, const char *name)
{
	memset(t, 0, sizeof(*t));
	int fd = openat(dir, name, O_RDONLY);
	FILE *f = fd < 0 ? NULL : fdopen(fd, "r");
	if (!f) die("manifest");
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getdelim(&line, &cap, 0, f)) > 0) {
		ent_t e;
		unsigned mode, uid, gid;
		long long size;
		unsigned long long hash;
		int off = 0;
		char ty;
		if (sscanf(line, "%c %o %u %u %lld %llx %n", &ty, &mode, &uid, &gid, &size, &hash, &off) < 6 || !off) {
			errno = EINVAL;
			die("manifest");
		}
		e.type = ty; e.mode = mode; e.uid = uid; e.gid = gid; e.size = size; e.hash = hash;
		tree_add(t, &e, line + off);
	}
	free(line);
	fclose(f);
	qsort(t->v, t->n, sizeof(ent_t), ent_cmp);
}

/* ── copying and removing ───────────────────────────────────── */

static int is_root;

static void own(int dir, const char *name, const struct stat *st)
{
	if (is_root) if (fchownat(dir, name, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW)) {}
}

static int copy_bytes(int from, int to)
{
	char b[16384];
	ssize_t n;
	while ((n = read(from, b, sizeof(b))) > 0)
		for (ssize_t w = 0; w < n; ) {
			ssize_t k = write(to, b + w, (size_t)(n - w));
			if (k <= 0) return -1;
			w += k;
		}
	return n < 0 ? -1 : 0;
}

static void copy_into(int from, int to);

/* copy one entry name from dir from to dir to, which must not have it */
static void copy_ent(int from, int to, const char *name)
{
	struct stat st;
	if (fstatat(from, name, &st, AT_SYMLINK_NOFOLLOW) < 0) die(name);
	if (S_ISDIR(st.st_mode)) {
		if (mkdirat(to, name, 0700) < 0) die(name);
		int a = open_dir_nf(from, name);
		int b = open_dir_nf(to, name);
		if (a < 0 || b < 0) die(name);
		copy_into(a, b);
		close(a); close(b);
	} else if (S_ISREG(st.st_mode)) {
		int a = openat(from, name, O_RDONLY | O_NOFOLLOW);
		int b = openat(to, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		if (a < 0 || b < 0 || copy_bytes(a, b) < 0) die(name);
		close(a); close(b);
	} else if (S_ISLNK(st.st_mode)) {
		char t[PATH_MAX];
		ssize_t n = readlinkat(from, name, t, sizeof(t) - 1);
		if (n < 0) die(name);
		t[n] = 0;
		if (symlinkat(t, to, name) < 0) die(name);
		own(to, name, &st);
		return;
	} else if (S_ISFIFO(st.st_mode)) {
		if (mkfifoat(to, name, 0600) < 0) die(name);
	} else {
		return;   /* devices and sockets are not staged */
	}
	own(to, name, &st);
	if (fchmodat(to, name, st.st_mode & 07777, 0) < 0) die(name);
}

static void copy_into(int from, int to)
{
	int fd = dup(from);
	DIR *d = fd < 0 ? NULL : fdopendir(fd);
	if (!d) die("open directory");
	struct dirent *de;
	while ((de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
			copy_ent(from, to, de->d_name);
	closedir(d);
}

static void rm_ent(int dir, const char *name);

static void empty_dir(int fd)
{
	int k = dup(fd);
	DIR *d = k < 0 ? NULL : fdopendir(k);
	if (!d) die("open directory");
	/* removing while reading may skip entries, so read again until empty */
	for (int round = 0; round < 64; round++) {
		rewinddir(d);
		struct dirent *de;
		int any = 0;
		while ((de = readdir(d)))
			if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { rm_ent(fd, de->d_name); any = 1; }
		if (!any) break;
	}
	closedir(d);
}

static void rm_ent(int dir, const char *name)
{
	struct stat st;
	if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) < 0) return;
	if (S_ISDIR(st.st_mode)) {
		int sub = open_dir_nf(dir, name);
		if (sub < 0) die(name);
		(void)fchmod(sub, 0700);
		empty_dir(sub);
		close(sub);
		if (unlinkat(dir, name, AT_REMOVEDIR) < 0) die(name);
	} else if (unlinkat(dir, name, 0) < 0) {
		die(name);
	}
}

/* ── state ──────────────────────────────────────────────────── */

static char state_path[PATH_MAX];

static const char *state_root(void)
{
	const char *r = getenv("RANIX_SESS_DIR");
	return r && *r ? r : "/tmp/ranix-sess";
}

static int name_ok(const char *n)
{
	if (!*n || *n == '.' || strlen(n) > 64) return 0;
	for (const char *p = n; *p; p++)
		if (!(*p == '-' || *p == '_' || *p == '.' || (*p >= '0' && *p <= '9') ||
		      (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return 0;
	return 1;
}

static int state_open(const char *name)
{
	if (!name_ok(name)) { fprintf(stderr, "ranix-sess: bad name '%s'\n", name); exit(2); }
	snprintf(state_path, sizeof(state_path), "%s/%s", state_root(), name);
	int fd = open(state_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) die(state_path);
	return fd;
}

/* the lock run holds while its session lives */
static int lock_take(int st, const char *file, int wait)
{
	int fd = openat(st, file, O_RDWR | O_CREAT, 0600);
	if (fd < 0) die(file);
	if (flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB)) < 0) { close(fd); return -1; }
	return fd;
}

static int running(int st)
{
	int fd = lock_take(st, "lock", 0);
	if (fd < 0) return 1;
	close(fd);
	return 0;
}

static void read_base(int st, char *base)
{
	int fd = openat(st, "base", O_RDONLY);
	ssize_t n = fd < 0 ? -1 : read(fd, base, PATH_MAX - 1);
	if (n <= 0) die("base");
	base[n] = 0;
	close(fd);
}

static int sub_dir(int st, const char *name)
{
	int fd = openat(st, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (fd < 0) die(name);
	return fd;
}

static int next_snap(int snaps)
{
	int fd = dup(snaps);
	DIR *d = fd < 0 ? NULL : fdopendir(fd);
	if (!d) die("snap");
	int max = -1;
	struct dirent *de;
	while ((de = readdir(d))) {
		char *end;
		long v = strtol(de->d_name, &end, 10);
		if (end != de->d_name && !*end && v > max) max = (int)v;
	}
	closedir(d);
	return max + 1;
}

static void snapshot_to(int st, int n)
{
	char name[16];
	snprintf(name, sizeof(name), "%d", n);
	int snaps = sub_dir(st, "snap");
	char tmp[24];
	snprintf(tmp, sizeof(tmp), ".%d.new", n);
	rm_ent(snaps, tmp);
	if (mkdirat(snaps, tmp, 0700) < 0) die("snapshot");
	int stage = sub_dir(st, "stage"), to = sub_dir(snaps, tmp);
	copy_into(stage, to);
	struct stat s;
	if (fstat(stage, &s) == 0) (void)fchmod(to, s.st_mode & 07777);
	close(stage); close(to);
	if (renameat(snaps, tmp, snaps, name) < 0) die("snapshot");
	close(snaps);
}

/* make the state for base, or check that an existing one is for it */
static int state_make(const char *name, const char *base)
{
	char root[PATH_MAX];
	snprintf(root, sizeof(root), "%s", state_root());
	if (mkdir(root, 0700) < 0 && errno != EEXIST) die(root);
	snprintf(state_path, sizeof(state_path), "%s/%s", root, name);
	if (mkdir(state_path, 0700) < 0) {
		if (errno != EEXIST) die(state_path);
		int st = state_open(name);
		char was[PATH_MAX];
		read_base(st, was);
		if (strcmp(was, base)) { fprintf(stderr, "ranix-sess: %s stages %s\n", name, was); exit(125); }
		return st;
	}
	int st = state_open(name);
	int bfd = open(base, O_RDONLY | O_DIRECTORY);
	if (bfd < 0) die(base);
	if (mkdirat(st, "stage", 0700) < 0 || mkdirat(st, "snap", 0700) < 0) die("state");
	int stage = sub_dir(st, "stage");
	copy_into(bfd, stage);
	struct stat s;
	if (fstat(bfd, &s) == 0) {
		if (is_root && fchown(stage, s.st_uid, s.st_gid)) {}
		(void)fchmod(stage, s.st_mode & 07777);
	}
	tree_t t;
	tree_of(stage, &t);
	tree_save(&t, st, "origin");
	close(stage); close(bfd);
	snapshot_to(st, 0);
	/* written last: a state without it is half made and run refuses it */
	int fd = openat(st, "base", O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0 || write(fd, base, strlen(base)) != (ssize_t)strlen(base) || close(fd) < 0) die("base");
	return st;
}

/* ── commands ───────────────────────────────────────────────── */

/* the entry at path below base, never through a symlink; *e set, or absent */
static int base_at(int base, const char *path, ent_t *e)
{
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", path);
	int dir = dup(base);
	char *p = buf, *slash;
	while ((slash = strchr(p, '/'))) {
		*slash = 0;
		int sub = open_dir_nf(dir, p);
		close(dir);
		if (sub < 0) return -1;
		dir = sub;
		p = slash + 1;
	}
	int r = ent_at(dir, p, e);
	close(dir);
	return r;
}

/* the directory holding path below base, opened as for base_at; *leaf its name */
static int base_parent(int base, char *path, const char **leaf)
{
	char *slash = strrchr(path, '/');
	if (!slash) { *leaf = path; return dup(base); }
	*slash = 0;
	*leaf = slash + 1;
	int dir = dup(base);
	char *p = path, *s;
	for (;;) {
		s = strchr(p, '/');
		if (s) *s = 0;
		int sub = open_dir_nf(dir, p);
		close(dir);
		if (s) *s = '/';
		if (sub < 0) return -1;
		dir = sub;
		if (!s) break;
		p = s + 1;
	}
	*slash = '/';
	return dir;
}

typedef struct { const char *path; ent_t *o, *f; } change_t;

static size_t changes(const tree_t *o, const tree_t *f, change_t **out)
{
	size_t n = 0, cap = o->n + f->n + 1, i = 0, j = 0;
	change_t *c = malloc(cap * sizeof(*c));
	if (!c) die("memory");
	while (i < o->n || j < f->n) {
		int k = i == o->n ? 1 : j == f->n ? -1 : strcmp(o->v[i].path, f->v[j].path);
		ent_t *a = k <= 0 ? &o->v[i] : NULL, *b = k >= 0 ? &f->v[j] : NULL;
		if (!ent_eq(a, b)) c[n++] = (change_t){ a ? a->path : b->path, a, b };
		if (k <= 0) i++;
		if (k >= 0) j++;
	}
	*out = c;
	return n;
}

static int cmd_diff(const char *name)
{
	int st = state_open(name);
	tree_t o, f;
	tree_load(&o, st, "origin");
	int stage = sub_dir(st, "stage");
	tree_of(stage, &f);
	change_t *c;
	size_t n = changes(&o, &f, &c);
	for (size_t i = 0; i < n; i++)
		printf("%c %s\n", !c[i].o ? 'A' : !c[i].f ? 'D' : c[i].o->type != c[i].f->type ? 'T' : 'M',
		       c[i].path);
	return 0;
}

static int cmd_snapshot(const char *name)
{
	int st = state_open(name);
	int op = lock_take(st, "oplock", 1);
	int snaps = sub_dir(st, "snap");
	int n = next_snap(snaps);
	close(snaps);
	snapshot_to(st, n);
	close(op);
	printf("%d\n", n);
	return 0;
}

static int cmd_rollback(const char *name, const char *snap)
{
	int st = state_open(name);
	int op = lock_take(st, "oplock", 1);
	if (!snap) {
		if (running(st)) {
			fprintf(stderr, "ranix-sess: %s is running; stop it, or roll back to a snapshot\n", name);
			return 1;
		}
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s", state_path);
		int top = open(state_root(), O_RDONLY | O_DIRECTORY);
		if (top < 0) die(state_root());
		close(op);
		close(st);
		rm_ent(top, name);
		printf("discarded %s\n", name);
		return 0;
	}
	char *end;
	long n = strtol(snap, &end, 10);
	if (end == snap || *end || n < 0) return usage();
	int snaps = sub_dir(st, "snap");
	int from = openat(snaps, snap, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (from < 0) die(snap);
	int stage = sub_dir(st, "stage");
	(void)fchmod(stage, 0700);
	empty_dir(stage);
	copy_into(from, stage);
	struct stat s;
	if (fstat(from, &s) == 0) (void)fchmod(stage, s.st_mode & 07777);
	close(op);
	printf("rolled back %s to %ld\n", name, n);
	return 0;
}

static long fail_after = -1;   /* RANIX_SESS_FAIL_AFTER: stop mid-commit, for the tests */

static void applied(void)
{
	if (fail_after >= 0 && fail_after-- == 0) _exit(99);
}

/* put the staged entry f at path in base; 0 or -1 */
static int apply(int base, int stage, const char *path, const ent_t *f)
{
	char pb[PATH_MAX];
	snprintf(pb, sizeof(pb), "%s", path);
	const char *leaf;
	int dir = base_parent(base, pb, &leaf);
	if (dir < 0) return -1;
	mode_t mode = f->mode & ~(mode_t)(S_ISUID | S_ISGID);
	int r = -1;
	ent_t cur;
	int have = ent_at(dir, leaf, &cur) == 0;
	if (have && cur.type != f->type) goto out;   /* removed in the first pass if staged so */
	if (f->type == 'd') {
		if (!have && mkdirat(dir, leaf, 0700) < 0) goto out;
		if (is_root) if (fchownat(dir, leaf, f->uid, f->gid, AT_SYMLINK_NOFOLLOW)) {}
		r = fchmodat(dir, leaf, mode, 0);
	} else if (f->type == 'f') {
		char tmp[300];
		snprintf(tmp, sizeof(tmp), ".ranix-sess.%d", (int)getpid());
		(void)unlinkat(dir, tmp, 0);
		int to = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
		int from = openat(stage, path, O_RDONLY | O_NOFOLLOW);
		if (to < 0 || from < 0 || copy_bytes(from, to) < 0 || fsync(to) < 0) {
			if (to >= 0) { close(to); unlinkat(dir, tmp, 0); }
			if (from >= 0) close(from);
			goto out;
		}
		close(from);
		if (is_root && fchown(to, f->uid, f->gid)) {}
		(void)fchmod(to, mode);
		close(to);
		r = renameat(dir, tmp, dir, leaf);
		if (r < 0) unlinkat(dir, tmp, 0);
	} else if (f->type == 'l') {
		char t[PATH_MAX];
		ssize_t n = readlinkat(stage, path, t, sizeof(t) - 1);
		if (n < 0) goto out;
		t[n] = 0;
		if (have && unlinkat(dir, leaf, 0) < 0) goto out;
		r = symlinkat(t, dir, leaf);
		if (!r && is_root) if (fchownat(dir, leaf, f->uid, f->gid, AT_SYMLINK_NOFOLLOW)) {}
	} else if (f->type == 'p') {
		if (!have && mkfifoat(dir, leaf, 0600) < 0) goto out;
		if (is_root) if (fchownat(dir, leaf, f->uid, f->gid, AT_SYMLINK_NOFOLLOW)) {}
		r = fchmodat(dir, leaf, mode, 0);
	}
out:
	close(dir);
	return r;
}

static int unapply(int base, const char *path, const ent_t *o)
{
	char pb[PATH_MAX];
	snprintf(pb, sizeof(pb), "%s", path);
	const char *leaf;
	int dir = base_parent(base, pb, &leaf);
	if (dir < 0) return errno == ENOENT ? 0 : -1;
	ent_t cur;
	int r = 0;
	if (ent_at(dir, leaf, &cur) == 0 && cur.type == o->type)
		r = unlinkat(dir, leaf, o->type == 'd' ? AT_REMOVEDIR : 0);
	close(dir);
	return r;
}

static int cmd_commit(const char *name, int force)
{
	int st = state_open(name);
	int op = lock_take(st, "oplock", 1);
	if (running(st)) { fprintf(stderr, "ranix-sess: %s is running\n", name); return 1; }
	char bpath[PATH_MAX];
	read_base(st, bpath);
	int base = open(bpath, O_RDONLY | O_DIRECTORY);
	if (base < 0) die(bpath);
	const char *fa = getenv("RANIX_SESS_FAIL_AFTER");
	if (fa) fail_after = atol(fa);

	tree_t o, f;
	tree_load(&o, st, "origin");
	int stage = sub_dir(st, "stage");
	tree_of(stage, &f);
	change_t *c;
	size_t n = changes(&o, &f, &c);

	/* the base must still be as copied, or already as staged (a commit cut short) */
	size_t conflicts = 0;
	for (size_t i = 0; i < n; i++) {
		ent_t b;
		ent_t *bp = base_at(base, c[i].path, &b) == 0 ? &b : NULL;
		if (ent_eq(bp, c[i].o) || ent_eq(bp, c[i].f)) continue;
		printf("conflict %s\n", c[i].path);
		conflicts++;
	}
	if (conflicts && !force) {
		fprintf(stderr, "ranix-sess: %zu conflicts, nothing applied (-f applies anyway)\n", conflicts);
		return 1;
	}

	size_t errs = 0, done = 0;
	/* removals, children before their directory */
	for (size_t i = n; i-- > 0; )
		if (c[i].o && (!c[i].f || c[i].o->type != c[i].f->type)) {
			if (unapply(base, c[i].path, c[i].o) < 0) {
				fprintf(stderr, "ranix-sess: remove %s: %s\n", c[i].path, strerror(errno));
				errs++;
			} else { done++; applied(); }
		}
	/* additions and changes, directories before what they hold */
	for (size_t i = 0; i < n; i++)
		if (c[i].f) {
			ent_t b;
			if (base_at(base, c[i].path, &b) == 0 && ent_eq(&b, c[i].f)) continue;
			if (apply(base, stage, c[i].path, c[i].f) < 0) {
				fprintf(stderr, "ranix-sess: write %s: %s\n", c[i].path, strerror(errno));
				errs++;
			} else { done++; applied(); }
		}
	if (errs) {
		fprintf(stderr, "ranix-sess: %zu errors; the stage is kept, commit again to finish\n", errs);
		return 1;
	}
	sync();
	close(stage);
	close(op);
	close(st);
	int top = open(state_root(), O_RDONLY | O_DIRECTORY);
	if (top < 0) die(state_root());
	rm_ent(top, name);
	printf("committed %zu changes to %s\n", n, bpath);
	return 0;
}

/* ── run ────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	static const int keep[] = { 0, 1, 2 };
	struct asess_attr a;
	const char *root = NULL, *dir = NULL, *stage_dir = NULL, *name = NULL;
	char real_root[PATH_MAX], real_dir[PATH_MAX], real_stage[PATH_MAX], auto_name[24];
	int c;

	is_root = geteuid() == 0;
	if (argc < 2) return usage();
	if (!strcmp(argv[1], "diff") && argc == 3) return cmd_diff(argv[2]);
	if (!strcmp(argv[1], "snapshot") && argc == 3) return cmd_snapshot(argv[2]);
	if (!strcmp(argv[1], "rollback") && (argc == 3 || argc == 4)) return cmd_rollback(argv[2], argc == 4 ? argv[3] : NULL);
	if (!strcmp(argv[1], "commit") && argc == 3) return cmd_commit(argv[2], 0);
	if (!strcmp(argv[1], "commit") && argc == 4 && !strcmp(argv[3], "-f")) return cmd_commit(argv[2], 1);
	if (strcmp(argv[1], "run") != 0) return usage();
	memset(&a, 0, sizeof(a));
	a.size = sizeof(a);
	a.nkeep = 3;
	a.keep_fds = (unsigned long long)(unsigned long)keep;

	optind = 2;
	while ((c = getopt(argc, argv, "+r:u:g:l:t:m:M:w:S:n:")) != -1) {
		switch (c) {
		case 'r': root = optarg; break;
		case 'w': dir = optarg; break;
		case 'S': stage_dir = optarg; break;
		case 'n': name = optarg; break;
		case 'u': a.uid = (unsigned)atoi(optarg); break;
		case 'g': a.gid = (unsigned)atoi(optarg); break;
		case 'l': strncpy(a.label, optarg, sizeof(a.label) - 1); break;
		case 't': a.max_tasks = (unsigned)atoi(optarg); break;
		case 'm': a.tmp_mib = (unsigned)atoi(optarg); break;
		case 'M': a.mem_max = (unsigned long long)atoll(optarg) << 20; break;
		default:  return usage();
		}
	}
	if (!root || optind >= argc || (dir && stage_dir) || (name && !stage_dir)) return usage();
	if (!a.gid) a.gid = a.uid;
	if (!realpath(root, real_root)) { perror(root); return 125; }
	a.root = (unsigned long long)(unsigned long)real_root;
	if (dir) {
		if (!realpath(dir, real_dir)) { perror(dir); return 125; }
		a.rw_dir = (unsigned long long)(unsigned long)real_dir;
	}
	int st = -1, lock = -1;
	if (stage_dir) {
		if (!realpath(stage_dir, real_dir)) { perror(stage_dir); return 125; }
		if (!name) { snprintf(auto_name, sizeof(auto_name), "s%d", (int)getpid()); name = auto_name; }
		if (!name_ok(name)) { fprintf(stderr, "ranix-sess: bad name '%s'\n", name); return 2; }
		st = state_make(name, real_dir);
		lock = lock_take(st, "lock", 0);
		if (lock < 0) { fprintf(stderr, "ranix-sess: %s is running\n", name); return 125; }
		snprintf(real_stage, sizeof(real_stage), "%s/stage", state_path);
		a.rw_dir = (unsigned long long)(unsigned long)real_dir;
		a.rw_src = (unsigned long long)(unsigned long)real_stage;
	}

	long sfd = syscall(ASESS_SYSCALL, ASESS_CREATE, -1, &a, sizeof(a));
	if (sfd < 0) { perror("ranix-sess: create"); return 125; }

	fflush(NULL);
	long pid = syscall(ASESS_SYSCALL, ASESS_SPAWN, (int)sfd, NULL, 0);
	if (pid < 0) { perror("ranix-sess: spawn"); return 125; }
	if (pid == 0) {
		execvp(argv[optind], argv + optind);
		perror(argv[optind]);
		_exit(127);
	}

	session_pid = (pid_t)pid;
	signal(SIGINT, forward);
	signal(SIGTERM, forward);
	signal(SIGHUP, forward);

	int status;
	while (waitpid((pid_t)pid, &status, 0) < 0)
		if (errno != EINTR) { perror("ranix-sess: wait"); return 125; }
	if (stage_dir) {
		/* the rest of the tree dies with pid 1; the stage is settled once it is gone */
		struct asess_info in;
		for (int i = 0; i < 500; i++) {
			if (syscall(ASESS_SYSCALL, ASESS_INFO, (int)sfd, &in, sizeof(in)) < 0 || !in.tasks) break;
			usleep(10000);
		}
		close((int)sfd);
		fprintf(stderr, "ranix-sess: staged %s; ranix-sess diff|commit|rollback %s\n", name, name);
		close(lock);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
