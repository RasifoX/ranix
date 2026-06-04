/* Robust futexes (NPTL robust-mutex handoff). A thread registers its own
 * robust_list_head via set_robust_list, "locks" a raw futex word by writing its
 * tid, links the lock onto the list, then dies via raw SYS_exit without
 * unlocking (bypassing musl's pthread cleanup). The kernel must walk the dead
 * thread's robust list on exit and set FUTEX_OWNER_DIED on the word so this,
 * the surviving thread, can recover the lock. A second futex left unlocked must
 * be untouched, and list_op_pending (a lock mid-acquire, not yet linked) must
 * also get the OWNER_DIED handoff. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>

#define FUTEX_WAITERS    0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK   0x3fffffffu

struct robust_list { struct robust_list *next; };
struct robust_list_head {
	struct robust_list  list;            /* head node: .next is the first lock */
	long                futex_offset;    /* node -> futex word */
	struct robust_list *list_op_pending;
};

/* Each "mutex": a robust_list node immediately followed by its futex word, so
 * futex_offset is the byte gap between them. */
struct rmutex { struct robust_list node; uint32_t futex; };

static struct robust_list_head head;
static struct rmutex locked;   /* held by the dying thread, linked on the list */
static struct rmutex pending;  /* held mid-acquire: named by list_op_pending */
static struct rmutex other;    /* unlocked: must stay untouched */
static volatile int dead;

static void *owner(void *a)
{
	(void)a;
	uint32_t tid = (uint32_t)syscall(SYS_gettid);

	head.list.next      = &head.list;          /* empty list */
	head.futex_offset   = (long)((char *)&locked.futex - (char *)&locked.node);
	head.list_op_pending = NULL;
	syscall(SYS_set_robust_list, &head, sizeof(head));

	other.futex = 0;

	/* lock + link: take the futex, then push the node onto the list */
	locked.futex   = tid;
	locked.node.next = head.list.next;
	head.list.next = &locked.node;

	/* a lock mid-acquire: futex taken, node not yet linked, recorded as pending */
	pending.futex = tid;
	head.list_op_pending = &pending.node;

	dead = 1;
	syscall(SYS_exit, 0);   /* die owning both, no unlock, no musl cleanup */
	return NULL;            /* unreachable */
}

int main(void)
{
	pthread_t th;
	pthread_create(&th, NULL, owner, NULL);
	while (!dead) usleep(1000);
	usleep(50000);   /* let the kernel run task_exit's robust walk */

	uint32_t tid = (uint32_t)syscall(SYS_gettid);
	(void)tid;

	if (!(locked.futex & FUTEX_OWNER_DIED)) {
		printf("FAIL locked not OWNER_DIED: %08x\n", locked.futex); return 1;
	}
	if (!(pending.futex & FUTEX_OWNER_DIED)) {
		printf("FAIL pending not OWNER_DIED: %08x\n", pending.futex); return 1;
	}
	if (other.futex & FUTEX_OWNER_DIED) {
		printf("FAIL untouched got OWNER_DIED: %08x\n", other.futex); return 1;
	}

	/* recover: a survivor clears the tid+DIED and writes its own tid */
	locked.futex = tid;
	if ((locked.futex & FUTEX_TID_MASK) != (tid & FUTEX_TID_MASK)) {
		printf("FAIL recover\n"); return 1;
	}

	puts("OK");
	return 0;
}
