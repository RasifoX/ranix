#include <kernel/block.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/vfs.h>
#include <kernel/fs/devfs.h>
#include <kernel/hal.h>
#include <kernel/cmdline.h>
#include <kernel/kprintf.h>
#include <kernel/capabilities.h>
#include <kernel/proc.h>
#include <kernel/errno.h>

#define BLKDEV_INIT_CAP 8
/* Largest single block_read/write from the device node. A polled driver keeps
 * IRQs off for the whole transfer, so this bounds that window. */
#define BLKDEV_BULK_BYTES (128u * 1024)

/* An out-of-range request never reaches a driver: a partition must not read or
 * write its neighbour because a filesystem believed a bad number. */
static int block_in_range(const block_device_t *dev, uint64_t lba, uint64_t count)
{
    if (!dev->size_blocks) return 1;     /* a driver that could not size its disk */
    return count <= dev->size_blocks && lba <= dev->size_blocks - count;
}

int block_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
    if (!dev || !dev->read) return -EIO;
    if (!block_in_range(dev, lba, count)) return -EIO;
    mutex_lock(&dev->lock);
    int r = dev->read(dev, lba, buf, count);
    if (r == 0) { dev->nr_reads++; dev->nr_read_secs += count; }
    mutex_unlock(&dev->lock);
    return r;
}

/* The driver call, for a request the protection check has passed. */
static int block_write_through(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
    if (!block_in_range(dev, lba, count)) return -EIO;
    mutex_lock(&dev->lock);
    int r = dev->drv_write(dev, lba, buf, count);
    if (r == 0) dev->dirty = 1;
    mutex_unlock(&dev->lock);
    return r;
}

/* The one place a write to a disk is decided. A protected device refuses here,
 * before any driver runs, so no command leaves for the drive. */
int block_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
    if (!dev || !dev->drv_write) return -EIO;
    if (dev->ro) {
        if (!dev->ro_denied++)
            kprintf(KERN_WARN "block: %s is read-only, write at lba %lu refused\n",
                    dev->name, (unsigned long)lba);
        return -EROFS;
    }
    return block_write_through(dev, lba, buf, count);
}

static block_device_t **devices;
static int              dev_cnt = 0;
static int              dev_cap = 0;
static spinlock_t       block_lock = SPINLOCK_INIT;

/* Whole sectors go through this kernel buffer, one command per chunk: the drivers
 * poll with IRQs off, where a fault on the caller's buffer could not be taken.
 * NULL (short I/O, or no memory) falls back to the one-sector path. */
static uint8_t *blkdev_bulk_alloc(uint64_t pos, size_t len)
{
    size_t off = (size_t)(pos % BLOCK_SIZE);
    if (off) off = BLOCK_SIZE - off;      /* the unaligned head stays on the stack */
    if (len < off + 2 * BLOCK_SIZE) return NULL;
    size_t sz = (len - off) & ~(size_t)(BLOCK_SIZE - 1);
    return kmalloc(sz < BLKDEV_BULK_BYTES ? sz : BLKDEV_BULK_BYTES);
}

/* byte-addressable view of a block device, exposed as a /dev/<name> node so
 * mount(2), dd and friends can open the disk by path (devtmpfs behaviour). */
static ssize_t blkdev_read(vfs_file_t *f, void *buf, size_t len)
{
    block_device_t *bd = f->inode ? f->inode->priv : NULL;
    if (!bd || !bd->read) return -EIO;
    uint64_t total = bd->size_blocks * BLOCK_SIZE;
    uint64_t pos = (uint64_t)f->pos;
    if (pos >= total) return 0;
    if (len > total - pos) len = (size_t)(total - pos);

    uint8_t  sect[BLOCK_SIZE];
    uint8_t *out  = buf;
    uint8_t *bulk = blkdev_bulk_alloc(pos, len);
    size_t   done = 0;
    ssize_t  err  = -5;
    while (done < len) {
        uint64_t lba = (pos + done) / BLOCK_SIZE;
        uint32_t off = (uint32_t)((pos + done) % BLOCK_SIZE);
        size_t whole = len - done;
        if (whole > BLKDEV_BULK_BYTES) whole = BLKDEV_BULK_BYTES;
        whole &= ~(size_t)(BLOCK_SIZE - 1);
        size_t chunk;
        if (bulk && !off && whole) {
            if (block_read(bd, lba, bulk, whole / BLOCK_SIZE) != 0) break;
            chunk = whole;
            if (copy_fop_to_user(out + done, bulk, chunk) != 0) { err = -14; break; }
        } else {
            if (block_read(bd, lba, sect, 1) != 0) break;
            chunk = BLOCK_SIZE - off;
            if (chunk > len - done) chunk = len - done;
            if (copy_fop_to_user(out + done, sect + off, chunk) != 0) { err = -14; break; }
        }
        done += chunk;
    }
    kfree(bulk);
    if (!done && len) return err;
    f->pos += (off_t)done;
    return (ssize_t)done;
}

static ssize_t blkdev_write(vfs_file_t *f, const void *buf, size_t len)
{
    block_device_t *bd = f->inode ? f->inode->priv : NULL;
    if (!bd || !bd->write || !bd->read) return -EIO;
    if (bd->ro) {
        bd->ro_denied++;
        return -EROFS;
    }
    uint64_t total = bd->size_blocks * BLOCK_SIZE;
    uint64_t pos = (uint64_t)f->pos;
    if (pos >= total) return -ENOSPC;
    if (len > total - pos) len = (size_t)(total - pos);

    uint8_t        sect[BLOCK_SIZE];
    const uint8_t *in   = buf;
    uint8_t       *bulk = blkdev_bulk_alloc(pos, len);
    size_t         done = 0;
    ssize_t        err  = -5;
    while (done < len) {
        uint64_t lba   = (pos + done) / BLOCK_SIZE;
        uint32_t off   = (uint32_t)((pos + done) % BLOCK_SIZE);
        size_t   whole = len - done;
        if (whole > BLKDEV_BULK_BYTES) whole = BLKDEV_BULK_BYTES;
        whole &= ~(size_t)(BLOCK_SIZE - 1);
        size_t chunk;
        if (bulk && !off && whole) {
            chunk = whole;
            if (copy_fop_from_user(bulk, in + done, chunk) != 0) { err = -14; break; }
            if (block_write(bd, lba, bulk, chunk / BLOCK_SIZE) != 0) break;
        } else {
            chunk = BLOCK_SIZE - off;
            if (chunk > len - done) chunk = len - done;
            if (chunk != BLOCK_SIZE && block_read(bd, lba, sect, 1) != 0) break;
            if (copy_fop_from_user(sect + off, in + done, chunk) != 0) { err = -14; break; }
            if (block_write(bd, lba, sect, 1) != 0) break;
        }
        done += chunk;
    }
    kfree(bulk);
    if (!done && len) return err;
    f->pos += (off_t)done;
    return (ssize_t)done;
}

/* Linux block ioctls, plus the Ranix one that lifts protection. BLKROSET can set
 * protection but not clear it: a generic tool or script must not be able to make an
 * unknown disk writable, so that takes BLKRANIXALLOW with the disk's identity spelled
 * out, which `fjord disk allow` does. */
#define BLKROSET       0x125D
#define BLKROGET       0x125E
#define BLKGETSIZE     0x1260
#define BLKSSZGET      0x1268
#define BLKGETSIZE64   0x80081272u
#define BLKRANIXALLOW  0x12F0
#define BLK_ID_MAX     64

static int blkdev_ioctl(vfs_file_t *f, uint32_t cmd, uint64_t arg)
{
    block_device_t *bd = f->inode ? f->inode->priv : NULL;
    if (!bd) return -EIO;
    void *ua = (void *)(uintptr_t)arg;
    switch (cmd) {
    case BLKROGET: {
        int v = bd->ro ? 1 : 0;
        return copy_to_user(ua, &v, sizeof(v)) ? -14 : 0;
    }
    case BLKGETSIZE: {
        unsigned long v = (unsigned long)bd->size_blocks;
        return copy_to_user(ua, &v, sizeof(v)) ? -14 : 0;
    }
    case BLKGETSIZE64: {
        uint64_t v = bd->size_blocks * BLOCK_SIZE;
        return copy_to_user(ua, &v, sizeof(v)) ? -14 : 0;
    }
    case BLKSSZGET: {
        int v = bd->lba_size ? (int)bd->lba_size : BLOCK_SIZE;
        return copy_to_user(ua, &v, sizeof(v)) ? -14 : 0;
    }
    case BLKROSET: {
        int v;
        if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
        if (copy_from_user(&v, ua, sizeof(v))) return -EFAULT;
        if (!v) return bd->ro ? -1 : 0;        /* EPERM to clear, see above; already clear: nothing */
        /* Setting what is already set changes nothing. Tools write back the value
         * they read (stress-ng, blockdev --setro on a protected disk), and a whole
         * disk that is protected would otherwise take its writable partitions,
         * the root among them, down with it. */
        if (bd->ro) return 0;
        block_protect(bd);
        return 0;
    }
    case BLKRANIXALLOW: {
        char id[BLK_ID_MAX];
        if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -1;
        if (strncpy_from_user(id, ua, sizeof(id)) < 0) return -EFAULT;
        id[sizeof(id) - 1] = '\0';
        return block_allow(bd, id);
    }
    }
    return -ENOTTY;
}

/* An open node keeps its disk from being handed to other hardware. */
static int blkdev_open(vfs_inode_t *inode, vfs_file_t *f, int flags)
{
    (void)f; (void)flags;
    if (inode && inode->priv) block_get(inode->priv);
    return 0;
}

static int blkdev_close(vfs_file_t *f)
{
    if (f->inode && f->inode->priv) block_put(f->inode->priv);
    return 0;
}

static vfs_fops_t blkdev_fops = {
    .open  = blkdev_open,
    .close = blkdev_close,
    .read  = blkdev_read,
    .write = blkdev_write,
    .ioctl = blkdev_ioctl,
};

/* Filesystems write through, so a drive cache flush is what makes data durable.
 * The dirty flag keeps fsync on RAM filesystems from touching any disk. */
int block_flush_all(void)
{
    int rc = 0;
    for (int i = 0; i < dev_cnt; i++) {
        block_device_t *bd = devices[i];
        if (!bd->flush) continue;
        /* a second caller must not see dirty clear before this flush has finished,
         * and I/O must queue behind it: a drive flush can take long enough to look
         * like a deadlock to anything spinning on the device */
        mutex_lock(&bd->lock);
        if (bd->dirty) {
            bd->dirty = 0;
            if (bd->flush(bd) != 0) { bd->dirty = 1; if (!rc) rc = -5; }
        }
        mutex_unlock(&bd->lock);
    }
    return rc;
}

static int blkdev_minor = 0;

/* ── which disks may be written ─────────────────────────────────
 * Default read-only. ranix.disk.rw on the command line is the allow list, comma
 * separated: serial:<drive serial>, gptdisk:<guid>, partuuid:<guid>. These name the
 * disk itself, not the slot it sits in; a device name could be a different disk
 * tomorrow, so none is accepted. "all" is for throwaway test VMs. A partition's
 * entry opens that partition only, the raw disk under it stays protected. */
static char rw_list[CONFIG_CMDLINE_MAX];
static int  rw_list_read;

static int rw_listed(const char *prefix, const char *value)
{
    if (!rw_list_read) {
        rw_list_read = 1;
        if (!cmdline_get("ranix.disk.rw", rw_list, sizeof(rw_list))) rw_list[0] = '\0';
    }
    size_t pl = strlen(prefix), vl = strlen(value);
    for (const char *p = rw_list; *p; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t n = (size_t)(e - p);
        if (n == 3 && !memcmp(p, "all", 3)) return 1;
        if (vl && n == pl + vl && !memcmp(p, prefix, pl) && !memcmp(p + pl, value, vl)) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int rw_allowed(const block_device_t *dev)
{
    if (rw_listed("serial:", dev->serial))      return 1;
    if (rw_listed("gptdisk:", dev->gpt_disk))   return 1;
    if (rw_listed("partuuid:", dev->part_uuid)) return 1;
    return 0;
}

static void ro_set_children(block_device_t *dev, int ro)
{
    for (int i = 0; i < dev_cnt; i++) {
        block_device_t *c = devices[i];
        for (block_device_t *p = c->parent; p; p = p->parent)
            if (p == dev) { c->ro = ro; break; }
    }
}

void block_copy_serial(char *out, size_t cap, const char *raw, size_t len)
{
    size_t n = 0;
    while (len && (raw[len - 1] == ' ' || !raw[len - 1])) len--;
    while (len && *raw == ' ') { raw++; len--; }
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        char c = raw[i];
        /* a comma or space would break the allow list syntax */
        out[n++] = (c > ' ' && c < 0x7F && c != ',') ? c : '_';
    }
    out[n] = '\0';
}

void block_guid_text(const uint8_t *g, char *out)
{
    static const char hex[] = "0123456789abcdef";
    /* the first three fields are little-endian, the rest is in byte order */
    static const uint8_t order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[o++] = '-';
        out[o++] = hex[g[order[i]] >> 4];
        out[o++] = hex[g[order[i]] & 15];
    }
    out[o] = '\0';
}

void block_set_gpt_disk(block_device_t *dev, const char *guid)
{
    memcpy(dev->gpt_disk, guid, sizeof(dev->gpt_disk));
    dev->gpt_disk[sizeof(dev->gpt_disk) - 1] = '\0';
    if (dev->ro && rw_allowed(dev)) dev->ro = 0;
}

static int id_put(char *buf, int cap, int n, const char *prefix, const char *value)
{
    if (!value[0]) return n;
    int pl = (int)strlen(prefix), vl = (int)strlen(value);
    if (n + pl + vl + 2 > cap) return n;
    memcpy(buf + n, prefix, (size_t)pl); n += pl;
    memcpy(buf + n, value, (size_t)vl);  n += vl;
    buf[n++] = '\n';
    buf[n] = '\0';
    return n;
}

int block_device_ids(const block_device_t *dev, char *buf, int cap)
{
    int n = 0;
    if (cap) buf[0] = '\0';
    n = id_put(buf, cap, n, "serial:", dev->serial);
    n = id_put(buf, cap, n, "gptdisk:", dev->gpt_disk);
    n = id_put(buf, cap, n, "partuuid:", dev->part_uuid);
    return n;
}

int block_allow(block_device_t *dev, const char *id)
{
    char ids[160];
    int n = block_device_ids(dev, ids, sizeof(ids));
    size_t il = strlen(id);
    if (!il) return -1;
    for (char *p = ids; p < ids + n; ) {
        char *e = p;
        while (*e != '\n') e++;
        if ((size_t)(e - p) == il && !memcmp(p, id, il)) {
            uint64_t f = spin_lock_irq(&block_lock);
            dev->ro = 0;
            ro_set_children(dev, 0);
            spin_unlock_irq(&block_lock, f);
            kprintf(KERN_WARN "block: %s made writable by explicit command (%s)\n", dev->name, id);
            return 0;
        }
        p = e + 1;
    }
    return -EPERM;   /* not this disk */
}

void block_protect(block_device_t *dev)
{
    uint64_t f = spin_lock_irq(&block_lock);
    dev->ro = 1;
    ro_set_children(dev, 1);
    spin_unlock_irq(&block_lock, f);
}

static int gate_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
    return block_write(dev, lba, buf, count);
}

int block_device_register(block_device_t *dev)
{
    /* Before the device can be seen: protection is on from its first moment. */
    dev->drv_write = dev->write;
    dev->write     = dev->drv_write ? gate_write : NULL;
    dev->ro        = (dev->parent ? dev->parent->ro : 1) && !rw_allowed(dev);
    if (dev->ro && dev->drv_write)
        kprintf(KERN_INFO "block: %s read-only until allowed\n", dev->name);

    dev->lock     = (mutex_t)MUTEX_INIT;
    dev->nr_reads = dev->nr_read_secs = 0;

    if (!dev->kernel_only)
        devfs_register_device_priv(dev->name, S_IFBLK | 0660,
                                   (dev_t)((8 << 8) | (blkdev_minor++ & 0xff)),
                                   &blkdev_fops, dev);

    uint64_t f = spin_lock_irq(&block_lock);
    if (dev_cnt >= dev_cap) {
        int new_cap = dev_cap ? dev_cap * 2 : BLKDEV_INIT_CAP;
        block_device_t **nb = kmalloc((size_t)new_cap * sizeof(block_device_t *));
        if (!nb) { spin_unlock_irq(&block_lock, f); return -1; }
        if (devices) {
            memcpy(nb, devices, (size_t)dev_cnt * sizeof(block_device_t *));
            kfree(devices);
        }
        devices = nb;
        dev_cap = new_cap;
    }
    devices[dev_cnt++] = dev;
    spin_unlock_irq(&block_lock, f);
    return 0;
}

typedef struct {
    block_device_t *parent;
    uint64_t        start;
} part_priv_t;

static int part_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
    part_priv_t *p = dev->priv;
    if (lba >= dev->size_blocks || count > dev->size_blocks - lba) return -EIO;
    return block_read(p->parent, p->start + lba, buf, count);
}

/* Reached only through block_write of the partition, whose own flag has passed: an
 * allowed partition of a protected disk writes, and stays inside its bounds. */
static int part_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
    part_priv_t *p = dev->priv;
    if (lba >= dev->size_blocks || count > dev->size_blocks - lba) return -EIO;   /* the neighbour may be another OS */
    return block_write_through(p->parent, p->start + lba, buf, count);
}

block_device_t *block_partition_add(block_device_t *parent, uint64_t start_lba,
                                    uint64_t nr_sectors, int idx,
                                    const block_part_info_t *info)
{
    char nm[32];
    int n = 0;
    while (parent->name[n] && n < 28) { nm[n] = parent->name[n]; n++; }
    nm[n++] = 'p';
    if (idx >= 10) nm[n++] = (char)('0' + idx / 10);
    nm[n++] = (char)('0' + idx % 10);
    nm[n]   = '\0';

    /* Probed again (a hot-plug disk given new media): the same name keeps its
     * device, node and index, and takes the new extent. */
    block_device_t *pd = NULL;
    uint64_t f = spin_lock_irq(&block_lock);
    for (int i = 0; i < dev_cnt && !pd; i++)
        if (devices[i]->parent == parent && strcmp(devices[i]->name, nm) == 0) pd = devices[i];
    if (pd) {
        part_priv_t *op = pd->priv;
        op->start = start_lba;
        memset(pd->part_name, 0, sizeof(pd->part_name));
        memset(pd->part_type, 0, sizeof(pd->part_type));
        pd->part_uuid[0] = '\0';
        pd->lba_size = parent->lba_size;
        memcpy(pd->gpt_disk, parent->gpt_disk, sizeof(pd->gpt_disk));
        if (info) {
            for (n = 0; info->name[n] && n < BLOCK_PART_NAME_MAX; n++) pd->part_name[n] = info->name[n];
            memcpy(pd->part_type, info->type_guid, sizeof(pd->part_type));
            if (info->part_guid) block_guid_text(info->part_guid, pd->part_uuid);
        }
        pd->ro = parent->ro && !rw_allowed(pd);
        pd->dirty = 0;
        pd->nr_reads = pd->nr_read_secs = 0;
        pd->size_blocks = nr_sectors;          /* last: until here it read as empty */
        spin_unlock_irq(&block_lock, f);
        return pd;
    }
    spin_unlock_irq(&block_lock, f);

    part_priv_t *pp = kmalloc(sizeof(*pp));
    pd = kmalloc(sizeof(*pd));
    if (!pp || !pd) { kfree(pp); kfree(pd); return NULL; }

    pp->parent = parent;
    pp->start  = start_lba;

    memset(pd, 0, sizeof(*pd));
    memcpy(pd->name, nm, sizeof(nm));

    pd->size_blocks = nr_sectors;
    pd->read        = part_read;
    pd->write       = part_write;
    pd->priv        = pp;
    pd->parent      = parent;
    pd->lba_size    = parent->lba_size;
    memcpy(pd->gpt_disk, parent->gpt_disk, sizeof(pd->gpt_disk));
    if (info) {
        for (n = 0; info->name[n] && n < BLOCK_PART_NAME_MAX; n++) pd->part_name[n] = info->name[n];
        memcpy(pd->part_type, info->type_guid, sizeof(pd->part_type));
        pd->kernel_only = info->kernel_only;
        if (info->part_guid) block_guid_text(info->part_guid, pd->part_uuid);
    }

    if (block_device_register(pd) != 0) { kfree(pp); kfree(pd); return NULL; }
    return pd;
}

int block_is_device(const void *p)
{
    int found = 0;
    uint64_t f = spin_lock_irq(&block_lock);
    for (int i = 0; i < dev_cnt && !found; i++) found = devices[i] == p;
    spin_unlock_irq(&block_lock, f);
    return found;
}

void block_get(block_device_t *dev)
{
    for (block_device_t *b = dev; b; b = b->parent)
        __atomic_add_fetch(&b->users, 1, __ATOMIC_SEQ_CST);
}

void block_put(block_device_t *dev)
{
    for (block_device_t *b = dev; b; b = b->parent)
        __atomic_sub_fetch(&b->users, 1, __ATOMIC_SEQ_CST);
}

void block_device_rebind(block_device_t *dev)
{
    uint64_t f = spin_lock_irq(&block_lock);
    for (int i = 0; i < dev_cnt; i++) {
        block_device_t *c = devices[i];
        if (c->parent == dev) c->size_blocks = 0;   /* gone until the probe finds it again */
    }
    dev->gpt_disk[0] = '\0';
    dev->part_uuid[0] = '\0';
    dev->dirty = 0;
    dev->nr_reads = dev->nr_read_secs = 0;
    dev->ro = !rw_allowed(dev);
    spin_unlock_irq(&block_lock, f);
}

block_device_t *block_device_find(const char *name)
{
    uint64_t f = spin_lock_irq(&block_lock);
    for (int i = 0; i < dev_cnt; i++) {
        if (strcmp(devices[i]->name, name) == 0) {
            spin_unlock_irq(&block_lock, f);
            return devices[i];
        }
    }
    spin_unlock_irq(&block_lock, f);
    return NULL;
}

block_device_t *block_device_find_part(const char *label)
{
    if (!label || !label[0]) return NULL;
    uint64_t f = spin_lock_irq(&block_lock);
    block_device_t *found = NULL;
    for (int i = 0; i < dev_cnt && !found; i++)
        if (strcmp(devices[i]->part_name, label) == 0) found = devices[i];
    spin_unlock_irq(&block_lock, f);
    return found;
}

int block_device_count(void)
{
    return dev_cnt;
}

block_device_t *block_device_get(int index)
{
    if (index < 0 || index >= dev_cnt) return NULL;
    return devices[index];
}
