/* /proc/self/stat: starttime (field 22) must be non-zero and rss (field 24)
 * must be > 0 — ps/top elapsed-time and memory columns depend on them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(void)
{
	int fd = open("/proc/self/stat", O_RDONLY);
	if (fd < 0) { puts("FAIL open"); return 1; }
	char buf[1024];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) { puts("FAIL read"); return 1; }
	buf[n] = '\0';

	/* comm is parenthesized; fields after ')' are space-separated. Field 3 is
	 * the first token after ')'. starttime is field 22, rss is field 24. */
	char *rp = strrchr(buf, ')');
	if (!rp) { puts("FAIL fmt"); return 1; }
	rp++;

	long fields[60];
	int nf = 0;
	char *tok = strtok(rp, " \t\n");
	while (tok && nf < 60) { fields[nf++] = atol(tok); tok = strtok(NULL, " \t\n"); }

	/* fields[0] corresponds to stat field 3 (state-as-number after strtol = 0
	 * for 'R'); index = stat_field - 3. starttime=22 -> idx 19, rss=24 -> idx 21 */
	if (nf < 22) { puts("FAIL nfields"); return 1; }
	long starttime = fields[19];
	long rss = fields[21];
	if (starttime == 0) { puts("FAIL starttime"); return 1; }
	if (rss <= 0)       { puts("FAIL rss"); return 1; }
	puts("OK");
	return 0;
}
