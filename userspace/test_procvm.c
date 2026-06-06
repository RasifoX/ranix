/* /proc/sys/vm/: max_map_count (JVM/ES/Redis) and overcommit_memory must be
 * readable and live under vm/, not kernel/. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static long read_long(const char *path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	char buf[64];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = '\0';
	return atol(buf);
}

int main(void)
{
	if (read_long("/proc/sys/vm/max_map_count") != 65530) { puts("FAIL max_map_count"); return 1; }
	if (read_long("/proc/sys/vm/overcommit_memory") < 0)  { puts("FAIL overcommit"); return 1; }
	if (read_long("/proc/sys/vm/swappiness") != 0)        { puts("FAIL swappiness"); return 1; }
	puts("OK");
	return 0;
}
