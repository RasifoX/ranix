/* PCIe capability walk: each /sys/bus/pci/devices/<addr>/capabilities lists the
 * device's capability IDs (PM/MSI/MSI-X/PCIe/vendor) discovered by walking the
 * config-space cap chain. On a PCI platform at least one device (the AHCI or
 * virtio controller) advertises capabilities; platforms without PCI (aarch64
 * virt uses MMIO transports) have no devices and the test skips. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int main(void)
{
	DIR *d = opendir("/sys/bus/pci/devices");
	if (!d) { printf("OK\n"); return 0; }   /* no PCI sysfs → skip */

	int ndev = 0, with_caps = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		ndev++;
		char path[128];
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/capabilities", e->d_name);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		char buf[128] = {0};
		long r = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		/* a hex pair like "01 05 11" → at least two hex chars before newline */
		if (r >= 2 && buf[0] != '\n') with_caps++;
	}
	closedir(d);

	if (ndev == 0) { printf("OK\n"); return 0; }       /* no PCI devices → skip */
	if (with_caps == 0) { printf("pcicaps-fail ndev=%d none-with-caps\n", ndev); return 0; }

	printf("OK\n");
	return 0;
}
