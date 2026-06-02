/* Directory sweep: readdir must enumerate entries with correct d_type for
 * regular files, subdirectories, and symlinks (find -type / ls -F / rm -r rely
 * on it), and include '.' and '..'. First failure or OK. */
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	const char *d = "/tmp/.dirsweep";
	/* clean slate */
	unlink("/tmp/.dirsweep/reg"); unlink("/tmp/.dirsweep/lnk");
	rmdir("/tmp/.dirsweep/sub"); rmdir(d);

	CHECK(mkdir(d, 0755) == 0);
	int fd = open("/tmp/.dirsweep/reg", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0); close(fd);
	CHECK(mkdir("/tmp/.dirsweep/sub", 0755) == 0);
	CHECK(symlink("reg", "/tmp/.dirsweep/lnk") == 0);

	DIR *dp = opendir(d);
	CHECK(dp != NULL);
	int n_reg = 0, n_dir = 0, n_lnk = 0, n_dot = 0, n_dotdot = 0, n_unknown = 0;
	struct dirent *e;
	while ((e = readdir(dp)) != NULL) {
		if (!strcmp(e->d_name, "."))  { n_dot++;    continue; }
		if (!strcmp(e->d_name, "..")) { n_dotdot++; continue; }
		if (!strcmp(e->d_name, "reg")) { CHECK(e->d_type == DT_REG); n_reg++; }
		else if (!strcmp(e->d_name, "sub")) { CHECK(e->d_type == DT_DIR); n_dir++; }
		else if (!strcmp(e->d_name, "lnk")) { CHECK(e->d_type == DT_LNK); n_lnk++; }
		if (e->d_type == DT_UNKNOWN) n_unknown++;
	}
	closedir(dp);

	CHECK(n_dot == 1 && n_dotdot == 1);
	CHECK(n_reg == 1 && n_dir == 1 && n_lnk == 1);
	CHECK(n_unknown == 0);

	/* cleanup */
	unlink("/tmp/.dirsweep/reg"); unlink("/tmp/.dirsweep/lnk");
	rmdir("/tmp/.dirsweep/sub"); rmdir(d);

	puts("OK");
	return 0;
}
