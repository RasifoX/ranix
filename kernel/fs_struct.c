/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A task's root, working directory and umask, shared between tasks made with
 * CLONE_FS and copied for the rest. See include/kernel/fs_struct.h. */
#include <kernel/fs_struct.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/errno.h>

static fs_path_t slash_path = { .ref = -1, .s = "/" };

/* pid 1 inherits umask 022 from here; 0 would make every file world-writable */
fs_struct_t init_fs = { .users = 1, .lock = SPINLOCK_INIT, .cwd = &slash_path, .umask = 022 };

static fs_path_t *path_new(const char *s, size_t n)
{
	fs_path_t *c = kmalloc(sizeof(*c) + n + 1);
	if (!c) return NULL;
	c->ref = 1;
	memcpy(c->s, s, n);
	c->s[n] = '\0';
	return c;
}

void fs_path_put(fs_path_t *c)
{
	if (!c || c->ref < 0) return;
	if (__atomic_sub_fetch(&c->ref, 1, __ATOMIC_ACQ_REL) == 0) kfree(c);
}

fs_struct_t *fs_new(void)
{
	fs_struct_t *fs = kmalloc(sizeof(*fs));
	if (!fs) return NULL;
	memset(fs, 0, sizeof(*fs));
	fs->users = 1;
	fs->lock  = (spinlock_t)SPINLOCK_INIT;
	fs->cwd   = &slash_path;
	fs->umask = 022;
	return fs;
}

fs_struct_t *fs_copy(fs_struct_t *old)
{
	fs_struct_t *fs = fs_new();
	if (!fs || !old) return fs;
	fs->cwd   = fs_cwd_get(old);
	fs->root  = old->root;      /* roots are kept dentries: no reference to take */
	fs->umask = old->umask;
	return fs;
}

fs_struct_t *fs_get(fs_struct_t *fs)
{
	uint64_t f = spin_lock_irq(&fs->lock);
	fs->users++;
	spin_unlock_irq(&fs->lock, f);
	return fs;
}

void fs_put(fs_struct_t *fs)
{
	if (!fs) return;
	uint64_t f = spin_lock_irq(&fs->lock);
	int last = (--fs->users == 0);
	spin_unlock_irq(&fs->lock, f);
	if (!last) return;
	fs_path_put(fs->cwd);
	kfree(fs);
}

fs_path_t *fs_cwd_get(fs_struct_t *fs)
{
	if (!fs) return &slash_path;
	uint64_t f = spin_lock_irq(&fs->lock);
	fs_path_t *c = fs->cwd;
	if (c->ref >= 0) __atomic_add_fetch(&c->ref, 1, __ATOMIC_RELAXED);
	spin_unlock_irq(&fs->lock, f);
	return c;
}

int fs_set_cwd(fs_struct_t *fs, const char *path)
{
	size_t n = strlen(path);
	if (n + 1 > PATH_MAX) return -ENAMETOOLONG;
	fs_path_t *c = (n == 1 && path[0] == '/') ? &slash_path : path_new(path, n);
	if (!c) return -ENOMEM;
	uint64_t f = spin_lock_irq(&fs->lock);
	fs_path_t *old = fs->cwd;
	fs->cwd = c;
	spin_unlock_irq(&fs->lock, f);
	fs_path_put(old);
	return 0;
}

void *fs_root_of(struct process *p)
{
	process_t *t = (process_t *)p;
	return t && t->fs ? t->fs->root : NULL;
}

mode_t fs_umask_of(struct process *p)
{
	process_t *t = (process_t *)p;
	return t && t->fs ? t->fs->umask : 022;
}

/* exit takes p->fs under sched_lock, so a reference taken under it is safe */
fs_struct_t *fs_of_task(struct process *p)
{
	process_t *t = (process_t *)p;
	uint64_t f = spin_lock_irq(&sched_lock);
	fs_struct_t *fs = t->fs ? fs_get(t->fs) : NULL;
	spin_unlock_irq(&sched_lock, f);
	return fs;
}

/* fs's cwd at or under old moves to the same place under new. A chdir in
 * between wins: the swap happens only if the cwd is still the one looked at. */
static void fs_rename_one(fs_struct_t *fs, const char *old, size_t on, const char *new)
{
	fs_path_t *c = fs_cwd_get(fs);
	const char *s = c->s;
	if (strncmp(s, old, on) != 0 || (s[on] != '\0' && s[on] != '/')) { fs_path_put(c); return; }
	size_t nn = strlen(new), rest = strlen(s + on);
	char *buf = nn + rest + 1 <= PATH_MAX ? kmalloc(nn + rest + 1) : NULL;
	fs_path_t *nc = NULL;
	if (buf) {
		memcpy(buf, new, nn);
		memcpy(buf + nn, s + on, rest + 1);
		nc = path_new(buf, nn + rest);
		kfree(buf);
	}
	if (nc) {
		uint64_t f = spin_lock_irq(&fs->lock);
		int swapped = fs->cwd == c;
		if (swapped) fs->cwd = nc;
		spin_unlock_irq(&fs->lock, f);
		if (swapped) fs_path_put(c);   /* the fs's reference */
		else         fs_path_put(nc);
	}
	fs_path_put(c);                    /* ours */
}

/* A task's cwd is kept as a path, so after directory old is renamed to new
 * (both as tasks under root see them) a task at or under old is moved along,
 * as it would be on Linux, where the cwd is the directory itself. Tasks with
 * another root name these directories differently and are left alone. */
void fs_rename_cwds(void *root, const char *old, const char *new)
{
	size_t on = strlen(old);
	if (on <= 1) return;               /* "/" is never renamed */
	int n = task_count();
	for (int i = 0; i < n; i++) {
		process_t *t = task_get(i);
		process_t *p = t ? proc_get(t->pid) : NULL;
		if (!p) continue;
		fs_struct_t *fs = fs_of_task(p);
		proc_put(p);
		if (!fs) continue;
		if (fs->root == root) fs_rename_one(fs, old, on, new);
		fs_put(fs);
	}
}
