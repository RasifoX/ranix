/* statfs(2): f_type reflects the filesystem the path is actually on (was a
 * hardcoded RAMFS magic regardless of path). */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/vfs.h>

int main(void)
{
	struct statfs s;
	if (statfs("/proc", &s) != 0 || (unsigned long)s.f_type != 0x9fa0UL) {
		printf("statfs-fail proc %lx\n", (unsigned long)s.f_type); return 0; }
	if (statfs("/sys", &s) != 0 || (unsigned long)s.f_type != 0x62656572UL) {
		printf("statfs-fail sys %lx\n", (unsigned long)s.f_type); return 0; }
	if (statfs("/", &s) != 0 || (unsigned long)s.f_type != 0x858458f6UL) {
		printf("statfs-fail root %lx\n", (unsigned long)s.f_type); return 0; }
	printf("OK\n");
	return 0;
}
