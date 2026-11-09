/* A task's root, working directory and umask, shared between tasks made with
 * CLONE_FS and copied for the rest. See include/kernel/fs_struct.h. */
#include <kernel/fs_struct.h>
#include <kernel/proc.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

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
	if (n + 1 > PATH_MAX) return -36;          /* ENAMETOOLONG */
	fs_path_t *c = (n == 1 && path[0] == '/') ? &slash_path : path_new(path, n);
	if (!c) return -12;
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
