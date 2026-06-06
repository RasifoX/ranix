/* lstat()/stat() on the volatile (no_cache) procfs subdirectories
 * /proc/self/{ns,fd,task} must return a directory without wedging — these
 * dentries are resolved as function-scoped volatiles, so a stale re-lookup
 * used to walk freed memory and hang the kernel. */
#include <stdio.h>
#include <sys/stat.h>

static int check_dir(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0) { printf("FAIL lstat %s\n", path); return 1; }
	if (!S_ISDIR(st.st_mode)) { printf("FAIL notdir lstat %s\n", path); return 1; }
	if (stat(path, &st) != 0) { printf("FAIL stat %s\n", path); return 1; }
	if (!S_ISDIR(st.st_mode)) { printf("FAIL notdir stat %s\n", path); return 1; }
	return 0;
}

int main(void)
{
	if (check_dir("/proc/self/ns"))   return 1;
	if (check_dir("/proc/self/fd"))   return 1;
	if (check_dir("/proc/self/task")) return 1;
	puts("OK");
	return 0;
}
