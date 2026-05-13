#include <kernel/vfs.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/mm/mmu.h>

uint64_t boot_initrd_start = 0;
uint64_t boot_initrd_size  = 0;

struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

static uint32_t parse_hex8(const char *str) {
    uint32_t val = 0;
    for (int i = 0; i < 8; i++) {
        val <<= 4;
        if (str[i] >= '0' && str[i] <= '9') val |= (str[i] - '0');
        else if (str[i] >= 'A' && str[i] <= 'F') val |= (str[i] - 'A' + 10);
        else if (str[i] >= 'a' && str[i] <= 'f') val |= (str[i] - 'a' + 10);
    }
    return val;
}

static void install(const char *path, const uint8_t *data, unsigned int len, uint32_t mode)
{
    /* CPIO contains full paths like "bin/sh". Prepend "/" if missing */
    char fullpath[256];
    if (path[0] != '/') {
        fullpath[0] = '/';
        int i = 0;
        while (path[i] && i < 254) {
            fullpath[i+1] = path[i];
            i++;
        }
        fullpath[i+1] = '\0';
        path = fullpath;
    }

    if ((mode & 0170000) == 0040000) {
        /* Directory */
        vfs_mkdir(path, mode & 0777);
    } else if ((mode & 0170000) == 0100000) {
        /* Regular file */
        if (vfs_kernel_install(path, data, len) != 0) {
            /* parent might not exist, ensure it does roughly */
            /* simplified: we could parse directories from path */
            kprintf("initrd: failed to install file: %s\n", path);
        } else {
            /* Update mode if possible. Since ramfs creates files with default 0644,
               we should ideally chmod, but vfs_kernel_install doesn't take mode.
               For now we ignore permissions as ramfs root is owner. */
        }
    }
}

void initrd_init(void)
{
    if (boot_initrd_start == 0 || boot_initrd_size == 0) {
        kprintf("initrd: no boot initramfs found\n");
        return;
    }

    uint64_t vaddr = PHYS_TO_VIRT(boot_initrd_start);

    kprintf("initrd: parsing cpio at 0x%lx (size %lu)\n", vaddr, boot_initrd_size);

    uint8_t *db = (uint8_t *)vaddr;
    kprintf("initrd: dump: %02x %02x %02x %02x %02x %02x %02x %02x\n",
            db[0], db[1], db[2], db[3], db[4], db[5], db[6], db[7]);

    uint64_t ptr = vaddr;
    uint64_t end = vaddr + boot_initrd_size;

    while (ptr < end) {
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)ptr;
        if (ptr + sizeof(*hdr) > end) break;

        if (hdr->c_magic[0] != '0' || hdr->c_magic[1] != '7' ||
            hdr->c_magic[2] != '0' || hdr->c_magic[3] != '7' ||
            hdr->c_magic[4] != '0' || hdr->c_magic[5] != '1') {
            kprintf("initrd: bad cpio magic\n");
            break;
        }

        uint32_t namesize = parse_hex8(hdr->c_namesize);
        uint32_t filesize = parse_hex8(hdr->c_filesize);
        uint32_t mode     = parse_hex8(hdr->c_mode);

        if (ptr + sizeof(*hdr) + namesize > end) break;

        const char *name = (const char *)(ptr + sizeof(*hdr));
        
        /* Stop on TRAILER!!! */
        if (strcmp(name, "TRAILER!!!") == 0) break;

        /* Skip header and name, rounded up to 4 bytes */
        uint64_t data_offset = ptr + sizeof(*hdr) + namesize;
        data_offset = (data_offset + 3) & ~3ULL;

        if (data_offset + filesize > end) break;

        const uint8_t *data = (const uint8_t *)data_offset;

        if (strcmp(name, ".") != 0) {
            kprintf("initrd: extracted %s (size %u)\n", name, filesize);
            install(name, data, filesize, mode);
        }

        /* Next header is after data, rounded up to 4 bytes */
        ptr = data_offset + filesize;
        ptr = (ptr + 3) & ~3ULL;
    }

    kprintf("initrd: unpack complete\n");

    /* Minimal /etc skeleton for userspace tools */
    vfs_mkdir("/etc", 0755);
    static const char passwd[] = "root:x:0:0:root:/root:/bin/sh\n";
    static const char group[]  = "root:x:0:\n";
    static const char hosts[]  = "127.0.0.1 localhost\n";
    vfs_kernel_install("/etc/passwd", (const uint8_t *)passwd, sizeof(passwd) - 1);
    vfs_kernel_install("/etc/group",  (const uint8_t *)group,  sizeof(group)  - 1);
    vfs_kernel_install("/etc/hosts",  (const uint8_t *)hosts,  sizeof(hosts)  - 1);
    vfs_mkdir("/root", 0700);
    vfs_mkdir("/tmp", 0777);
    vfs_mkdir("/proc", 0555);
}
