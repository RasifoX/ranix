/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_FS_STRUCT_H
#define KERNEL_FS_STRUCT_H

#include <uapi/types.h>
#include <kernel/sync.h>

/*
 * A task's place in the file tree: its root, its working directory and its
 * umask. Tasks made with CLONE_FS (threads, Chromium's chroot helper) share
 * one, so a chdir, chroot or umask by one is seen by all of them, as on Linux;
 * every other new task gets a copy. unshare(CLONE_FS) takes a private copy.
 *
 * The working directory is an immutable string: a chdir puts a new one in
 * place under the lock, and a reader takes a reference (fs_cwd_get), so a
 * sibling's chdir can never be read half-written.
 */
typedef struct fs_path {
	int  ref;        /* -1: static, never freed */
	char s[];        /* absolute, as the task sees it under its root */
} fs_path_t;

typedef struct fs_struct {
	int        users;
	spinlock_t lock;   /* users and cwd */
	void      *root;   /* vfs_dentry_t*; NULL = the global "/" */
	fs_path_t *cwd;
	mode_t     umask;
} fs_struct_t;

#define CLONE_FS 0x00000200   /* share root, cwd and umask (Linux value) */

struct process;

extern fs_struct_t init_fs;               /* the boot task's; never freed */

fs_struct_t *fs_new(void);                   /* root "/", cwd "/", umask 022 */
fs_struct_t *fs_copy(fs_struct_t *fs);       /* a private copy; NULL: out of memory */
fs_struct_t *fs_get(fs_struct_t *fs);
void         fs_put(fs_struct_t *fs);

fs_path_t   *fs_cwd_get(fs_struct_t *fs);    /* "/" if fs is NULL; release with fs_path_put */
void         fs_path_put(fs_path_t *c);
int          fs_set_cwd(fs_struct_t *fs, const char *path);   /* 0, -12, -36 */

/* the calling task's (or any task's own) view, NULL-safe for kernel tasks */
void        *fs_root_of(struct process *p);
mode_t       fs_umask_of(struct process *p);
/* another task's fs, held against its exit; release with fs_put */
fs_struct_t *fs_of_task(struct process *p);

#endif
