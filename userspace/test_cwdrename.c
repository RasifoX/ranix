/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The working directory is a place, not a name: renaming it, or a directory
 * above it, leaves the task standing in the same directory under its new
 * name, so relative paths and getcwd follow (LTP inotify02 renames its own
 * cwd, then unlinks a file by a relative name). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int bad;

static void at(const char *what, const char *want)
{
	char cwd[256];
	if (!getcwd(cwd, sizeof cwd) || strcmp(cwd, want)) { printf("%s: cwd '%s', want '%s'\n", what, cwd, want); bad++; }
	if (access("f", F_OK) != 0) { printf("%s: relative 'f' is gone\n", what); bad++; }
}

static void clean(void)
{
	const char *f[] = { "/tmp/cwdr/a/f", "/tmp/cwdr/b/f", "/tmp/cwdr2/b/f" };
	const char *d[] = { "/tmp/cwdr/a", "/tmp/cwdr/b", "/tmp/cwdr2/b", "/tmp/cwdr", "/tmp/cwdr2" };
	for (unsigned i = 0; i < 3; i++) unlink(f[i]);
	for (unsigned i = 0; i < 5; i++) rmdir(d[i]);
}

int main(void)
{
	clean();
	mkdir("/tmp/cwdr", 0755);
	mkdir("/tmp/cwdr/a", 0755);
	chdir("/tmp/cwdr/a");
	close(open("f", O_CREAT | O_WRONLY, 0644));
	rename("/tmp/cwdr/a", "/tmp/cwdr/b");          /* the cwd itself */
	at("cwd renamed", "/tmp/cwdr/b");
	rename("/tmp/cwdr", "/tmp/cwdr2");             /* a directory above it */
	at("parent renamed", "/tmp/cwdr2/b");
	if (unlink("f") != 0) { puts("unlink of relative 'f' failed"); bad++; }
	chdir("/");
	clean();
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
