/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* inotify reports what happens to a watched file and in a watched directory,
 * in order (LTP inotify01, inotify02, inotify04): attrib, open, access,
 * close-nowrite, modify, close-write on a file; create (with IN_ISDIR for a
 * directory), moves paired by one cookie, delete; delete-self and ignored
 * when the watched file goes; move-self when the watched directory moves. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad;
static char buf[4096];
static int len, off;

static int next(int fd, struct inotify_event **ev)
{
	if (off >= len) {
		len = (int)read(fd, buf, sizeof buf);
		off = 0;
		if (len <= 0) return 0;
	}
	*ev = (struct inotify_event *)(buf + off);
	off += (int)sizeof(struct inotify_event) + (int)(*ev)->len;
	return 1;
}

/* the next event must be mask (and name, if given); returns its cookie */
static unsigned expect(int fd, const char *what, unsigned mask, const char *name)
{
	struct inotify_event *ev;
	if (!next(fd, &ev)) { printf("%s: no event, want 0x%x\n", what, mask); bad++; return 0; }
	const char *got = ev->len ? ev->name : "";
	if (ev->mask != mask || (name && strcmp(got, name))) {
		printf("%s: got 0x%x '%s', want 0x%x '%s'\n", what, ev->mask, got, mask, name ? name : "");
		bad++;
	}
	return ev->cookie;
}

static void none_left(int fd, const char *what)
{
	struct inotify_event *ev;
	if (next(fd, &ev)) { printf("%s: extra event 0x%x\n", what, ev->mask); bad++; }
}

int main(void)
{
	char c;
	mkdir("/tmp/ie", 0755);
	int w = open("/tmp/ie/f", O_CREAT | O_WRONLY, 0644);   /* a read must find a byte */
	write(w, "abc", 3);
	close(w);

	int in = inotify_init1(IN_NONBLOCK);
	inotify_add_watch(in, "/tmp/ie/f", IN_ALL_EVENTS);
	chmod("/tmp/ie/f", 0600);
	int fd = open("/tmp/ie/f", O_RDONLY);
	read(fd, &c, 1);
	close(fd);
	fd = open("/tmp/ie/f", O_WRONLY);
	write(fd, "x", 1);
	close(fd);
	/* one read returns every queued event that fits, as Linux's does (LTP
	 * inotify01 reads once): seven events here */
	len = (int)read(in, buf, sizeof buf);
	off = 0;
	int n = 0;
	for (int o = 0; o < len; o += (int)sizeof(struct inotify_event) + (int)((struct inotify_event *)(buf + o))->len) n++;
	if (n != 7) { printf("one read gave %d events, want 7\n", n); bad++; }
	expect(in, "file chmod", IN_ATTRIB, NULL);
	expect(in, "file open", IN_OPEN, NULL);
	expect(in, "file read", IN_ACCESS, NULL);
	expect(in, "file close", IN_CLOSE_NOWRITE, NULL);
	expect(in, "file open w", IN_OPEN, NULL);
	expect(in, "file write", IN_MODIFY, NULL);
	expect(in, "file close w", IN_CLOSE_WRITE, NULL);
	none_left(in, "file");
	close(in);

	in = inotify_init1(IN_NONBLOCK);
	inotify_add_watch(in, "/tmp/ie", IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_MOVE_SELF);
	close(open("/tmp/ie/x", O_CREAT | O_WRONLY, 0644));
	mkdir("/tmp/ie/d", 0755);
	rename("/tmp/ie/x", "/tmp/ie/y");
	unlink("/tmp/ie/y");
	rmdir("/tmp/ie/d");
	expect(in, "dir create", IN_CREATE, "x");
	expect(in, "dir close", IN_CLOSE_WRITE, "x");
	expect(in, "dir mkdir", IN_CREATE | IN_ISDIR, "d");
	unsigned c1 = expect(in, "dir move from", IN_MOVED_FROM, "x");
	unsigned c2 = expect(in, "dir move to", IN_MOVED_TO, "y");
	if (!c1 || c1 != c2) { printf("move cookies %u and %u\n", c1, c2); bad++; }
	expect(in, "dir unlink", IN_DELETE, "y");
	expect(in, "dir rmdir", IN_DELETE | IN_ISDIR, "d");
	none_left(in, "dir");
	rename("/tmp/ie", "/tmp/ie2");
	expect(in, "dir moved", IN_MOVE_SELF, NULL);
	rename("/tmp/ie2", "/tmp/ie");
	close(in);

	/* a watch named "." from inside the directory is the directory (LTP inotify02) */
	in = inotify_init1(IN_NONBLOCK);
	chdir("/tmp/ie");
	inotify_add_watch(in, ".", IN_ATTRIB | IN_CREATE);
	chmod(".", 0755);
	close(open("z", O_CREAT | O_WRONLY, 0644));
	expect(in, "dot chmod", IN_ATTRIB | IN_ISDIR, NULL);
	expect(in, "dot create", IN_CREATE, "z");
	unlink("z");
	chdir("/");
	close(in);

	/* unlinking a watched file changes its link count, then it goes (inotify04) */
	in = inotify_init1(IN_NONBLOCK);
	inotify_add_watch(in, "/tmp/ie/f", IN_ATTRIB | IN_DELETE_SELF);
	unlink("/tmp/ie/f");
	expect(in, "file unlinked", IN_ATTRIB, NULL);
	expect(in, "file deleted", IN_DELETE_SELF, NULL);
	expect(in, "watch dropped", IN_IGNORED, NULL);
	close(in);
	rmdir("/tmp/ie");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
