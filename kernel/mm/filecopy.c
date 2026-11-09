/* ── shared mappings of files without a page cache ───────────────────────────
 * Most filesystems here cannot hand out a page of their own for a file (ext2
 * with 1 or 2 KiB blocks, FAT, ...), so mmap gives the process copies of the
 * file's pages. For MAP_SHARED those copies must still end up in the file, as on
 * Linux, where the mapping and read()/write() share one page cache.
 *
 * Each such mapping is an entry: the address range, the file and its offset,
 * and a snapshot of every page as the file last held it. Write-back compares a
 * page with its snapshot and writes only the bytes that differ, so a write() to
 * other bytes of the file since the mapping was made is kept. It runs on msync,
 * on munmap or a mapping laid over the range (drop), and when the address space
 * goes at exit or exec (vma_free_space). The file does not grow: bytes past EOF
 * stay in the mapping, as Linux drops them.
 *
 * Not done (unlike Linux): read() does not see a store before one of those
 * points, and a second mapping does not see the first one's stores (each has
 * its copy). A fork shares the pages (VMA_SHARED), and the parent's entry
 * writes back what the child stored; the child keeps no entry of its own.
 *
 * Entries sit on one list for the whole system, under one mutex: these
 * mappings are rare, and every lookup is O(entries). An empty list costs a
 * munmap one load.
 * ───────────────────────────────────────────────────────────────────────── */
#include <kernel/mm/filecopy.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/sync.h>
#include <kernel/errno.h>
#include <kernel/vfs.h>
#include <kernel/lib/string.h>
#include <kernel/mm/mmu.h>
#include <asm/mmu.h>

typedef struct filecopy {
	vm_space_t      *vs;
	uint64_t         va, len;   /* page aligned */
	uint64_t         off;       /* file offset of va */
	vfs_file_t      *f;         /* one reference */
	uint64_t        *orig;      /* len / PAGE_SIZE physical pages: the snapshot */
	struct filecopy *next;
} filecopy_t;

static filecopy_t *fc_list;
static mutex_t     fc_lock = MUTEX_INIT;

/* Copy the page mapped at va in vs into buf. 0 if nothing is mapped there. */
static int fc_read_page(vm_space_t *vs, uint64_t va, uint8_t *buf)
{
	uint64_t lf = spin_lock_irq(&vs->fault_lock);
	uint64_t pa = arch_vm_get_paddr(vs->root, va);
	if (pa) memcpy(buf, (void *)PHYS_TO_VIRT(pa), PAGE_SIZE);
	spin_unlock_irq(&vs->fault_lock, lf);
	return pa != 0;
}

/* Write back page i of e: each run of bytes that differs from the snapshot,
 * up to EOF. buf is a page of scratch. */
static void fc_write_page(filecopy_t *e, uint64_t i, uint8_t *buf)
{
	uint64_t pos = e->off + i * PAGE_SIZE;
	uint64_t size = (uint64_t)e->f->inode->size;
	if (pos >= size) return;
	size_t n = size - pos < PAGE_SIZE ? (size_t)(size - pos) : PAGE_SIZE;
	if (!fc_read_page(e->vs, e->va + i * PAGE_SIZE, buf)) return;
	uint8_t *old = (uint8_t *)PHYS_TO_VIRT(e->orig[i]);
	size_t j = 0;
	while (j < n) {
		if (buf[j] == old[j]) { j++; continue; }
		size_t k = j;
		while (k < n && buf[k] != old[k]) k++;
		vfs_file_t tmp = *e->f;   /* own position; O_APPEND is the VFS's, not the fop's */
		tmp.pos = (off_t)(pos + j);
		if (e->f->inode->fops->write(&tmp, buf + j, k - j) == (ssize_t)(k - j))
			memcpy(old + j, buf + j, k - j);
		j = k;
	}
}

/* Pages [a, b) of e as an entry of their own, with its own file reference.
 * NULL when out of memory: those pages are then freed and no longer written. */
static filecopy_t *fc_part(const filecopy_t *e, uint64_t a, uint64_t b)
{
	filecopy_t *p = kmalloc(sizeof(*p));
	uint64_t *o = kmalloc((size_t)(b - a) * sizeof(uint64_t));
	if (!p || !o) {
		kfree(p);
		kfree(o);
		for (uint64_t i = a; i < b; i++) buddy_free(e->orig[i], 0);
		return NULL;
	}
	memcpy(o, e->orig + a, (size_t)(b - a) * sizeof(uint64_t));
	p->vs   = e->vs;
	p->va   = e->va + a * PAGE_SIZE;
	p->len  = (b - a) * PAGE_SIZE;
	p->off  = e->off + a * PAGE_SIZE;
	p->f    = e->f;
	p->orig = o;
	p->next = NULL;
	atomic_fetch_add32((volatile uint32_t *)&e->f->refcount, 1);
	return p;
}

/* Pages [first, last) of e are gone: free e, and link in at *pp what stays on
 * either side. */
static void fc_cut(filecopy_t *e, uint64_t first, uint64_t last, filecopy_t **pp)
{
	uint64_t n = e->len / PAGE_SIZE;
	for (uint64_t i = first; i < last; i++) buddy_free(e->orig[i], 0);
	filecopy_t *l = first ? fc_part(e, 0, first) : NULL;
	filecopy_t *r = last < n ? fc_part(e, last, n) : NULL;
	if (r) { r->next = *pp; *pp = r; }
	if (l) { l->next = *pp; *pp = l; }
	fput(e->f);
	kfree(e->orig);
	kfree(e);
}

/* Write back [va, va+len) of vs; with drop, forget it as well. */
static void fc_range(vm_space_t *vs, uint64_t va, uint64_t len, int drop)
{
	if (!fc_list) return;
	uint64_t end = va + len < va ? (uint64_t)-1 : va + len;
	uint8_t *buf = kmalloc(PAGE_SIZE);   /* none: no write-back, but drop still forgets */
	mutex_lock(&fc_lock);
	filecopy_t **pp = &fc_list;
	while (*pp) {
		filecopy_t *e = *pp;
		uint64_t lo = e->va > va ? e->va : va;
		uint64_t hi = e->va + e->len < end ? e->va + e->len : end;
		if (e->vs != vs || lo >= hi) { pp = &e->next; continue; }
		uint64_t first = (lo - e->va) / PAGE_SIZE, last = (hi - e->va) / PAGE_SIZE;
		for (uint64_t i = first; buf && i < last; i++) fc_write_page(e, i, buf);
		if (!drop) { pp = &e->next; continue; }
		*pp = e->next;
		fc_cut(e, first, last, pp);
	}
	mutex_unlock(&fc_lock);
	kfree(buf);
}

void filecopy_sync(vm_space_t *vs, uint64_t va, uint64_t len)
{
	fc_range(vs, va, len, 0);
}

void filecopy_drop(vm_space_t *vs, uint64_t va, uint64_t len)
{
	fc_range(vs, va, len, 1);
}

int filecopy_add(vm_space_t *vs, uint64_t va, uint64_t len, vfs_file_t *f, uint64_t off)
{
	uint64_t n = len / PAGE_SIZE;
	filecopy_t *e = kmalloc(sizeof(*e));
	uint64_t *orig = kmalloc((size_t)n * sizeof(uint64_t));
	uint64_t got = 0;
	if (!e || !orig) goto oom;
	for (; got < n; got++) {
		void *pg = buddy_alloc(0);
		if (!pg) goto oom;
		orig[got] = (uint64_t)pg;
		uint8_t *s = (uint8_t *)PHYS_TO_VIRT((uint64_t)pg);
		if (!fc_read_page(vs, va + got * PAGE_SIZE, s)) memset(s, 0, PAGE_SIZE);
	}
	e->vs   = vs;
	e->va   = va;
	e->len  = len;
	e->off  = off;
	e->f    = f;
	e->orig = orig;
	atomic_fetch_add32((volatile uint32_t *)&f->refcount, 1);
	mutex_lock(&fc_lock);
	e->next = fc_list;
	fc_list = e;
	mutex_unlock(&fc_lock);
	return 0;
oom:
	while (got) buddy_free(orig[--got], 0);
	kfree(orig);
	kfree(e);
	return -ENOMEM;
}
