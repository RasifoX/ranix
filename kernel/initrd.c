#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include "userspace/hello_elf.h"
#include "userspace/sh_elf.h"

static void install(const char *path, const uint8_t *data, unsigned int len)
{
	if (vfs_kernel_install(path, data, len) != 0) {
		kprintf("[initrd] failed: %s\n", path);
		return;
	}
	kprintf("[initrd] %s (%u B)\n", path, len);
}

void initrd_init(void)
{
	vfs_mkdir("/bin", 0755);
	install("/bin/hello", hello_musl_elf, hello_musl_elf_len);
	install("/bin/sh",    sh_elf,         sh_elf_len);
}
