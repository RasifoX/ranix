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

static void inode_apply_meta(const char *path, uint32_t uid, uint32_t gid, uint32_t mode)
{
    vfs_dentry_t *d = vfs_dentry_resolve(path);
    if (!d || !d->inode) return;
    d->inode->uid  = uid;
    d->inode->gid  = gid;
    d->inode->mode = (d->inode->mode & 0170000) | (mode & 07777);
}

static void install(const char *path, const uint8_t *data, unsigned int len,
                    uint32_t mode, uint32_t uid, uint32_t gid)
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
        vfs_mkdir(path, mode & 0777);
    } else if ((mode & 0170000) == 0120000) {
        char target[256];
        int tlen = (len < 255) ? (int)len : 255;
        memcpy(target, data, tlen);
        target[tlen] = '\0';
        vfs_symlink(target, path);
    } else if ((mode & 0170000) == 0100000) {
        if (vfs_kernel_install(path, data, len) != 0)
            kprintf("initrd: failed to install file: %s\n", path);
    }
    inode_apply_meta(path, uid, gid, mode);
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
        uint32_t uid      = parse_hex8(hdr->c_uid);
        uint32_t gid      = parse_hex8(hdr->c_gid);

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
            install(name, data, filesize, mode, uid, gid);
        }

        /* Next header is after data, rounded up to 4 bytes */
        ptr = data_offset + filesize;
        ptr = (ptr + 3) & ~3ULL;
    }

    kprintf("initrd: unpack complete\n");

    /* /etc skeleton — only install if not already provided by initramfs */
    vfs_mkdir("/etc", 0755);
    static const char hosts[]   = "127.0.0.1\tlocalhost\n";
    static const char resolv[]  = "nameserver 8.8.8.8\n";
    static const char environ[] = "LANG=C\nPATH=/bin:/usr/bin\nTERM=vt100\n";
    if (!vfs_dentry_resolve("/etc/passwd"))
        vfs_kernel_install("/etc/passwd",      (const uint8_t *)"root:x:0:0:root:/root:/bin/bash\n", 32);
    if (!vfs_dentry_resolve("/etc/group"))
        vfs_kernel_install("/etc/group",       (const uint8_t *)"root:x:0:\n", 10);
    if (!vfs_dentry_resolve("/etc/hosts"))
        vfs_kernel_install("/etc/hosts",       (const uint8_t *)hosts,   sizeof(hosts)   - 1);
    if (!vfs_dentry_resolve("/etc/resolv.conf"))
        vfs_kernel_install("/etc/resolv.conf", (const uint8_t *)resolv,  sizeof(resolv)  - 1);
    if (!vfs_dentry_resolve("/etc/environment"))
        vfs_kernel_install("/etc/environment", (const uint8_t *)environ, sizeof(environ) - 1);
    /* standard directories */
    vfs_mkdir("/root", 0700);
    vfs_mkdir("/tmp",  0777);
    vfs_mkdir("/run",  0755);
    vfs_mkdir("/sys",  0555);
    vfs_mkdir("/var",  0755);
    vfs_mkdir("/usr",  0755);
    vfs_mkdir("/proc", 0555);
}
