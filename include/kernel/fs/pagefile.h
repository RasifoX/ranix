#ifndef KERNEL_FS_PAGEFILE_H
#define KERNEL_FS_PAGEFILE_H

#include <kernel/vfs.h>
#include <kernel/sync.h>

/* A file's data as single pages behind a radix index, for the in-memory
 * filesystems. Nothing needs contiguous memory; a page never moves once it is in,
 * so a shared mapping stays valid while the file grows. Absent pages read as zero. */

typedef struct {
	int  (*charge)(void *ctx, uint64_t n);    /* 0 ok, nonzero: over the limit */
	void (*uncharge)(void *ctx, uint64_t n);
} pagefile_limit_t;

typedef struct pagefile pagefile_t;

/* limit may be NULL. inode->priv must be the result. */
pagefile_t *pagefile_new(const pagefile_limit_t *limit, void *ctx);
void pagefile_evict(pagefile_t *pf);

ssize_t  pagefile_read(vfs_file_t *f, void *buf, size_t len);
ssize_t  pagefile_write(vfs_file_t *f, const void *buf, size_t len);
uint64_t pagefile_mmap(vfs_file_t *f, off_t off);
int      pagefile_open(vfs_inode_t *inode, vfs_file_t *file, int flags);
int      pagefile_truncate(vfs_inode_t *inode, off_t len);

#endif
