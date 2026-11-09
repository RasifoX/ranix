/* Shared mappings of files that have no page cache to map.
 * A file whose filesystem cannot hand out its own pages (ext2 with blocks
 * smaller than a page, FAT, ...) is mapped from copies of its pages. For
 * MAP_SHARED the bytes a process changes in those copies are written back to
 * the file on msync, munmap, a mapping laid over them, and when the address
 * space goes (exit, exec), as Linux does through its page cache. */
#ifndef KERNEL_MM_FILECOPY_H
#define KERNEL_MM_FILECOPY_H

#include <kernel/mm/vm.h>

struct vfs_file;

/* [va, va+len) of vs holds the file from off on: remember it, with a snapshot
 * of what the pages hold now. Takes a reference on f. 0 or -ENOMEM. */
int  filecopy_add(vm_space_t *vs, uint64_t va, uint64_t len, struct vfs_file *f, uint64_t off);
/* Write back what changed in [va, va+len). */
void filecopy_sync(vm_space_t *vs, uint64_t va, uint64_t len);
/* Write back, then forget [va, va+len): it is being unmapped or replaced. */
void filecopy_drop(vm_space_t *vs, uint64_t va, uint64_t len);

#endif
