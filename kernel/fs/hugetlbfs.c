#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>

/*
 * hugetlbfs — a filesystem whose regular files are backed by 2 MB huge pages
 * drawn from the nr_hugepages pool (kernel/mm/hugetlb.c). It is the portable
 * interface AI/DB frameworks use for big buffers: open a file here, ftruncate
 * to size, mmap it, and the mapping covers physically-contiguous 2 MB blocks,
 * cutting TLB pressure.
 *
 * Backing is an array of 2 MB block physical addresses, one per 2 MB of the
 * file's huge-rounded length; the inode owns one page reference per backing
 * page. The mmap fop (vfs_fops_t.mmap) hands the covering block's physical page
 * to the generic file-mmap path in sys_mmap, taking an extra page reference so
 * the unmap there drops only the mapping's reference and the file keeps its own.
 * Freeing a file drops its references; a block still mapped survives until the
 * last unmap, so unlink-while-mapped is safe.
 *
 * Limitation: sys_mmap's file path maps a returned page at 4 KB granularity and
 * with VM_NOCACHE, so the user PTEs are 4 KB and non-cacheable even though the
 * backing is one contiguous 2 MB block. Real 2 MB leaf PTEs (and write-back)
 * would need the shared mmap path to special-case this fop, which this fs does
 * not own. The backing contiguity and the pool accounting — what the use case
 * needs — are correct regardless.
 */

#define HPAGES_PER_BLOCK  (HUGE_2M_SIZE / PAGE_SIZE)   /* 512 */

typedef struct {
	uint64_t *blocks;   /* physical base of each 2 MB block */
	uint64_t  nblocks;  /* allocated blocks */
	uint64_t  size;     /* logical size in bytes */
} hugetlbfs_file_t;

typedef struct hugetlbfs_entry {
	char                    name[256];
	vfs_inode_t            *inode;
	struct hugetlbfs_entry *next;
} hugetlbfs_entry_t;

typedef struct {
	hugetlbfs_entry_t *entries;
} hugetlbfs_dir_t;

static uint64_t   next_ino = 1;
static spinlock_t ino_lock = SPINLOCK_INIT;
static vfs_iops_t hugetlbfs_iops;
static vfs_fops_t hugetlbfs_fops;
static vfs_fops_t hugetlbfs_dir_fops = { 0 };

static vfs_inode_t *hugetlbfs_inode_alloc(mode_t mode)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	uint64_t f = spin_lock_irq(&ino_lock);
	in->ino = next_ino++;
	spin_unlock_irq(&ino_lock, f);
	in->mode     = mode;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &hugetlbfs_iops;
	process_t *p = task_current();
	in->uid = p ? p->euid : 0;
	in->gid = p ? p->egid : 0;
	int64_t s, ns; ranix_walltime(&s, &ns);
	in->atime = in->mtime = in->ctime = s;
	in->atime_nsec = in->mtime_nsec = in->ctime_nsec = (uint32_t)ns;
	return in;
}

/* Grow/shrink a file's huge-page backing to cover `bytes` (rounded up to 2 MB).
 * Returns 0 or -ENOSPC when the pool is exhausted. */
static int file_resize(hugetlbfs_file_t *hf, uint64_t bytes)
{
	uint64_t want = (bytes + HUGE_2M_SIZE - 1) / HUGE_2M_SIZE;

	if (want > hf->nblocks) {
		uint64_t *nb = kmalloc((size_t)want * sizeof(uint64_t));
		if (!nb) return -28;
		if (hf->blocks) memcpy(nb, hf->blocks, (size_t)hf->nblocks * sizeof(uint64_t));
		kfree(hf->blocks);
		hf->blocks = nb;
		while (hf->nblocks < want) {
			uint64_t blk = huge2m_alloc();
			if (!blk) return -28;
			memset((void *)PHYS_TO_VIRT(blk), 0, HUGE_2M_SIZE);
			hf->blocks[hf->nblocks++] = blk;
		}
	} else if (want < hf->nblocks) {
		while (hf->nblocks > want)
			huge2m_free(hf->blocks[--hf->nblocks]);
	}
	hf->size = bytes;
	return 0;
}

static void file_free(hugetlbfs_file_t *hf)
{
	for (uint64_t i = 0; i < hf->nblocks; i++)
		huge2m_free(hf->blocks[i]);
	kfree(hf->blocks);
	kfree(hf);
}

/* ── file ops ────────────────────────────────────────────── */

static ssize_t hugetlbfs_read(vfs_file_t *f, void *buf, size_t len)
{
	hugetlbfs_file_t *hf = f->inode->priv;
	if (!hf || f->pos >= (off_t)hf->size) return 0;
	uint64_t avail = hf->size - (uint64_t)f->pos;
	if (len > avail) len = (size_t)avail;
	uint64_t pos = (uint64_t)f->pos, done = 0;
	while (done < len) {
		uint64_t blk = pos / HUGE_2M_SIZE, boff = pos % HUGE_2M_SIZE;
		uint64_t n = HUGE_2M_SIZE - boff;
		if (n > len - done) n = len - done;
		memcpy((uint8_t *)buf + done, (void *)(PHYS_TO_VIRT(hf->blocks[blk]) + boff), (size_t)n);
		pos += n; done += n;
	}
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t hugetlbfs_write(vfs_file_t *f, const void *buf, size_t len)
{
	hugetlbfs_file_t *hf = f->inode->priv;
	if (!hf) return -1;
	if ((uint64_t)f->pos + len > hf->size) {
		int r = file_resize(hf, (uint64_t)f->pos + len);
		if (r != 0) return r;
		f->inode->size = hf->size;
	}
	uint64_t pos = (uint64_t)f->pos, done = 0;
	while (done < len) {
		uint64_t blk = pos / HUGE_2M_SIZE, boff = pos % HUGE_2M_SIZE;
		uint64_t n = HUGE_2M_SIZE - boff;
		if (n > len - done) n = len - done;
		memcpy((void *)(PHYS_TO_VIRT(hf->blocks[blk]) + boff), (const uint8_t *)buf + done, (size_t)n);
		pos += n; done += n;
	}
	f->pos += (off_t)len;
	int64_t s, ns; ranix_walltime(&s, &ns);
	f->inode->mtime = f->inode->ctime = s;
	f->inode->mtime_nsec = f->inode->ctime_nsec = (uint32_t)ns;
	return (ssize_t)len;
}

/* Hand the generic mmap path the physical page covering `off`, holding an extra
 * reference so its per-page unmap leaves the file's own reference intact. */
static uint64_t hugetlbfs_mmap(vfs_file_t *f, off_t off)
{
	hugetlbfs_file_t *hf = f->inode->priv;
	if (!hf || off < 0) return 0;
	uint64_t blk = (uint64_t)off / HUGE_2M_SIZE;
	if (blk >= hf->nblocks) return 0;
	uint64_t pa = hf->blocks[blk] + ((uint64_t)off % HUGE_2M_SIZE);
	pmm_hold((void *)pa);
	return pa;
}

static vfs_fops_t hugetlbfs_fops = {
	.read  = hugetlbfs_read,
	.write = hugetlbfs_write,
	.mmap  = hugetlbfs_mmap,
};

/* ── inode ops ───────────────────────────────────────────── */

static vfs_inode_t *hugetlbfs_lookup(vfs_inode_t *dir, const char *name)
{
	hugetlbfs_dir_t *hd = dir->priv;
	if (!hd) return NULL;
	for (hugetlbfs_entry_t *e = hd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return e->inode;
	return NULL;
}

static hugetlbfs_entry_t *dir_new_entry(hugetlbfs_dir_t *hd, const char *name, vfs_inode_t *in)
{
	hugetlbfs_entry_t *ent = kmalloc(sizeof(*ent));
	if (!ent) return NULL;
	int nlen = 0; while (name[nlen] && nlen < 255) nlen++;
	memcpy(ent->name, name, nlen); ent->name[nlen] = '\0';
	ent->inode  = in;
	ent->next   = hd->entries;
	hd->entries = ent;
	return ent;
}

static int hugetlbfs_create(vfs_inode_t *dir, const char *name, mode_t mode)
{
	hugetlbfs_dir_t *hd = dir->priv;
	if (!hd) return -1;
	for (hugetlbfs_entry_t *e = hd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;

	vfs_inode_t *in = hugetlbfs_inode_alloc(mode | S_IFREG);
	if (!in) return -12;
	hugetlbfs_file_t *hf = kmalloc(sizeof(*hf));
	if (!hf) { kfree(in); return -12; }
	memset(hf, 0, sizeof(*hf));
	in->priv = hf;
	in->fops = &hugetlbfs_fops;

	if (!dir_new_entry(hd, name, in)) { kfree(hf); kfree(in); return -12; }
	return 0;
}

static int hugetlbfs_mkdir(vfs_inode_t *dir, const char *name, mode_t mode)
{
	hugetlbfs_dir_t *hd = dir->priv;
	if (!hd) return -1;
	for (hugetlbfs_entry_t *e = hd->entries; e; e = e->next)
		if (strcmp(e->name, name) == 0) return -17;

	vfs_inode_t *in = hugetlbfs_inode_alloc(mode | S_IFDIR);
	if (!in) return -12;
	hugetlbfs_dir_t *nhd = kmalloc(sizeof(*nhd));
	if (!nhd) { kfree(in); return -12; }
	memset(nhd, 0, sizeof(*nhd));
	in->priv = nhd;
	in->fops = &hugetlbfs_dir_fops;

	if (!dir_new_entry(hd, name, in)) { kfree(nhd); kfree(in); return -12; }
	dir->nlink++;
	return 0;
}

static int hugetlbfs_unlink(vfs_inode_t *dir, const char *name)
{
	hugetlbfs_dir_t *hd = dir->priv;
	if (!hd) return -1;
	hugetlbfs_entry_t **pp = &hd->entries;
	while (*pp) {
		if (strcmp((*pp)->name, name) == 0) {
			hugetlbfs_entry_t *e = *pp;
			*pp = e->next;
			vfs_inode_t *in = e->inode;
			if (S_ISDIR(in->mode)) {
				dir->nlink--;
				kfree(in->priv);
				kfree(in);
			} else if (in->nlink > 0 && --in->nlink == 0) {
				if (in->priv) file_free(in->priv);
				kfree(in);
			}
			kfree(e);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -2;
}

static int hugetlbfs_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	hugetlbfs_dir_t *hd = dir->priv;
	if (!hd) return -1;
	if (idx == 0 || idx == 1) {
		out->ino      = dir->ino;
		out->mode     = dir->mode;
		out->next_off = idx + 1;
		out->name[0]  = '.';
		out->name[1]  = (idx == 1) ? '.' : '\0';
		out->name[2]  = '\0';
		return 0;
	}
	hugetlbfs_entry_t *e = hd->entries;
	for (uint32_t i = 0; i < idx - 2 && e; i++) e = e->next;
	if (!e) return -1;
	out->ino      = e->inode->ino;
	out->mode     = e->inode->mode;
	out->next_off = idx + 1;
	int nlen = 0; while (e->name[nlen] && nlen < 255) nlen++;
	memcpy(out->name, e->name, nlen); out->name[nlen] = '\0';
	return 0;
}

static int hugetlbfs_stat(vfs_inode_t *inode, vfs_stat_t *out)
{
	out->ino   = inode->ino;
	out->mode  = inode->mode;
	out->nlink = inode->nlink ? inode->nlink : 1;
	out->uid   = inode->uid;
	out->gid   = inode->gid;
	out->size  = inode->size;
	out->rdev  = inode->rdev;
	out->atime = inode->atime; out->atime_nsec = inode->atime_nsec;
	out->mtime = inode->mtime; out->mtime_nsec = inode->mtime_nsec;
	out->ctime = inode->ctime; out->ctime_nsec = inode->ctime_nsec;
	return 0;
}

static int hugetlbfs_truncate(vfs_inode_t *inode, off_t len)
{
	if (!S_ISREG(inode->mode)) return -22;
	hugetlbfs_file_t *hf = inode->priv;
	if (!hf || len < 0) return -22;
	int r = file_resize(hf, (uint64_t)len);
	if (r != 0) return r;
	inode->size = hf->size;
	return 0;
}

static int hugetlbfs_statfs(vfs_inode_t *inode, vfs_statfs_t *out)
{
	(void)inode;
	out->bsize   = HUGE_2M_SIZE;
	out->blocks  = (uint64_t)huge2m_nr_total();
	out->bfree   = (uint64_t)huge2m_nr_free();
	out->bavail  = out->bfree;
	out->files   = 0;
	out->ffree   = 0;
	out->namelen = 255;
	return 0;
}

static vfs_iops_t hugetlbfs_iops = {
	.lookup   = hugetlbfs_lookup,
	.create   = hugetlbfs_create,
	.mkdir    = hugetlbfs_mkdir,
	.unlink   = hugetlbfs_unlink,
	.readdir  = hugetlbfs_readdir,
	.stat     = hugetlbfs_stat,
	.truncate = hugetlbfs_truncate,
	.statfs   = hugetlbfs_statfs,
};

/* ── mount ───────────────────────────────────────────────── */

int hugetlbfs_mount(vfs_mount_t *mnt, void *data)
{
	(void)data;
	vfs_inode_t *root = hugetlbfs_inode_alloc(S_IFDIR | 0755);
	if (!root) return -1;
	hugetlbfs_dir_t *hd = kmalloc(sizeof(*hd));
	if (!hd) { kfree(root); return -1; }
	memset(hd, 0, sizeof(*hd));
	root->priv = hd;
	root->fops = &hugetlbfs_dir_fops;

	vfs_dentry_t *rd = vfs_dentry_create(NULL, "/", root);
	if (!rd) { kfree(hd); kfree(root); return -1; }
	mnt->root = rd;
	return 0;
}

void hugetlbfs_register(void)
{
	extern void vfs_register_fs(const char *, int (*)(vfs_mount_t *, void *));
	vfs_register_fs("hugetlbfs", hugetlbfs_mount);
}
