/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SysV IPC as Linux reports it (LTP msgctl01/02/04/12, semctl01..07,
 * shmctl01..08, msgget02, semget02, shmget04, semop03): IPC_STAT fills the
 * whole record, IPC_SET changes it for the owner only, IPC_INFO and the
 * *_STAT commands work, a get that the mode forbids is EACCES, SHM_LOCK marks
 * the segment, shm_nattch follows fork and exit, a semop sleeper whose array
 * goes is told EIDRM, and /proc/sys/kernel and /proc/sysvipc describe it all. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SHM_LOCKED
#define SHM_LOCKED 02000
#endif
#ifndef MSG_STAT
#define MSG_STAT 11
#endif
#ifndef SEM_STAT
#define SEM_STAT 18
#endif
#ifndef SHM_STAT
#define SHM_STAT 13
#endif

union semun { int val; struct semid_ds *buf; unsigned short *array; struct seminfo *__buf; };

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); putchar('\n'); bad++; } } while (0)

/* run f as nobody in a child, return its exit code (the errno it saw) */
static int as_nobody(int (*f)(void *), void *a)
{
	pid_t k = fork();
	if (k == 0) {
		if (setgid(65534) || setuid(65534)) _exit(255);
		_exit(f(a));
	}
	int st = 0;
	waitpid(k, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 254;
}
static int nb_msgget(void *k)  { return msgget(*(key_t *)k, 0400) < 0 ? errno : 0; }
static int nb_semget(void *k)  { return semget(*(key_t *)k, 1, 0400) < 0 ? errno : 0; }
static int nb_shmget(void *k)  { return shmget(*(key_t *)k, 4096, 0400) < 0 ? errno : 0; }
static int nb_msgset(void *id) { struct msqid_ds d; memset(&d, 0, sizeof d); d.msg_perm.mode = 0666;
	return msgctl(*(int *)id, IPC_SET, &d) < 0 ? errno : 0; }
static int nb_msgstat(void *id) { struct msqid_ds d; return msgctl(*(int *)id, IPC_STAT, &d) < 0 ? errno : 0; }

static unsigned long proc_long(const char *path)
{
	unsigned long v = (unsigned long)-1;
	FILE *f = fopen(path, "r");
	if (f) { if (fscanf(f, "%lu", &v) != 1) v = (unsigned long)-1; fclose(f); }
	return v;
}

static int in_sysvipc(const char *kind, key_t key)
{
	char path[64], line[512];
	snprintf(path, sizeof path, "/proc/sysvipc/%s", kind);
	FILE *f = fopen(path, "r");
	int found = 0;
	while (f && fgets(line, sizeof line, f)) {
		long k;
		if (sscanf(line, "%ld", &k) == 1 && (key_t)k == key) found = 1;
	}
	if (f) fclose(f);
	return found;
}

static void msg_tests(void)
{
	key_t key = 0x52580001;
	time_t t0 = time(NULL);
	int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
	CHECK(id >= 0, "msgget: %s", strerror(errno));
	struct msqid_ds d;
	memset(&d, 'a', sizeof d);
	CHECK(msgctl(id, IPC_STAT, &d) == 0, "msgctl IPC_STAT: %s", strerror(errno));
	CHECK(d.msg_qnum == 0 && d.msg_stime == 0 && d.msg_lspid == 0 && d.msg_qbytes > 0,
	      "msg fresh: qnum %lu stime %ld lspid %d qbytes %lu", (unsigned long)d.msg_qnum,
	      (long)d.msg_stime, d.msg_lspid, (unsigned long)d.msg_qbytes);
	CHECK(d.msg_ctime >= t0 - 1 && d.msg_ctime <= time(NULL) + 1, "msg_ctime %ld", (long)d.msg_ctime);
	CHECK(d.msg_perm.__key == key && d.msg_perm.uid == getuid() && d.msg_perm.cuid == getuid() &&
	      (d.msg_perm.mode & 0777) == 0600, "msg_perm key %x uid %d mode %o",
	      d.msg_perm.__key, d.msg_perm.uid, d.msg_perm.mode);
	struct { long t; char s[8]; } m = { 1, "hello" };
	msgsnd(id, &m, 6, 0);
	msgctl(id, IPC_STAT, &d);
	CHECK(d.msg_qnum == 1 && d.msg_lspid == getpid() && d.msg_stime >= t0 - 1 && d.msg_cbytes == 6,
	      "msg after send: qnum %lu lspid %d cbytes %lu", (unsigned long)d.msg_qnum, d.msg_lspid,
	      (unsigned long)d.msg_cbytes);
	CHECK(as_nobody(nb_msgget, &key) == EACCES, "msgget as nobody on 0600: not EACCES");
	CHECK(as_nobody(nb_msgstat, &id) == EACCES, "msgctl IPC_STAT as nobody: not EACCES");
	CHECK(as_nobody(nb_msgset, &id) == EPERM, "msgctl IPC_SET as nobody: not EPERM");
	CHECK(msgctl(id, IPC_STAT, (struct msqid_ds *)-1) < 0 && errno == EFAULT, "IPC_STAT bad buf: not EFAULT");
	d.msg_perm.mode = 0640;
	d.msg_qbytes -= 1;
	CHECK(msgctl(id, IPC_SET, &d) == 0, "msgctl IPC_SET: %s", strerror(errno));
	unsigned long qb = d.msg_qbytes;
	msgctl(id, IPC_STAT, &d);
	CHECK((d.msg_perm.mode & 0777) == 0640 && d.msg_qbytes == qb, "IPC_SET mode %o qbytes %lu",
	      d.msg_perm.mode, (unsigned long)d.msg_qbytes);
	struct msginfo mi;
	int hi = msgctl(id, IPC_INFO, (struct msqid_ds *)&mi);
	CHECK(hi >= id && mi.msgmni > 0 && mi.msgmax > 0, "msg IPC_INFO: %d (%s)", hi, strerror(errno));
	CHECK(msgctl(id, MSG_STAT, &d) == id, "MSG_STAT of index %d", id);
	CHECK(proc_long("/proc/sys/kernel/msgmni") == (unsigned long)mi.msgmni, "msgmni in /proc differs");
	CHECK(in_sysvipc("msg", key), "/proc/sysvipc/msg lacks the queue");
	msgctl(id, IPC_RMID, NULL);
}

static void sem_tests(void)
{
	key_t key = 0x52580002;
	int id = semget(key, 3, IPC_CREAT | IPC_EXCL | 0600);
	CHECK(id >= 0, "semget: %s", strerror(errno));
	CHECK(semget(key, 4, 0600) < 0 && errno == EINVAL, "semget more sems than it has: not EINVAL");
	CHECK(as_nobody(nb_semget, &key) == EACCES, "semget as nobody on 0600: not EACCES");
	struct semid_ds d;
	union semun u = { .buf = &d };
	CHECK(semctl(id, 0, IPC_STAT, u) == 0 && d.sem_nsems == 3 && d.sem_perm.uid == getuid(),
	      "semctl IPC_STAT nsems %lu", (unsigned long)d.sem_nsems);
	struct sembuf op = { 1, 2, 0 };
	semop(id, &op, 1);
	CHECK(semctl(id, 1, GETPID) == getpid(), "GETPID %d", semctl(id, 1, GETPID));
	semctl(id, 0, IPC_STAT, u);
	CHECK(d.sem_otime > 0, "sem_otime not set by semop");
	struct seminfo si;
	union semun ui = { .__buf = &si };
	CHECK(semctl(id, 0, IPC_INFO, ui) >= id && si.semmsl > 0, "sem IPC_INFO");
	CHECK(semctl(id, 0, SEM_STAT, u) == id, "SEM_STAT of index %d", id);
	CHECK(in_sysvipc("sem", key), "/proc/sysvipc/sem lacks the array");

	/* a sleeper whose array goes is told EIDRM */
	pid_t k = fork();
	if (k == 0) {
		struct sembuf w = { 0, -1, 0 };
		_exit(semop(id, &w, 1) < 0 && errno == EIDRM ? 0 : 1);
	}
	for (int i = 0; i < 100 && semctl(id, 0, GETNCNT) != 1; i++) usleep(10000);
	CHECK(semctl(id, 0, GETNCNT) == 1, "GETNCNT with one sleeper: %d", semctl(id, 0, GETNCNT));
	semctl(id, 0, IPC_RMID);
	int st = 0;
	waitpid(k, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "semop sleeper: not EIDRM after IPC_RMID");
}

static void shm_tests(void)
{
	key_t key = 0x52580003;
	int id = shmget(key, 8192, IPC_CREAT | IPC_EXCL | 0600);
	CHECK(id >= 0, "shmget: %s", strerror(errno));
	CHECK(shmget(key, 16384, 0600) < 0 && errno == EINVAL, "shmget bigger than it is: not EINVAL");
	CHECK(as_nobody(nb_shmget, &key) == EACCES, "shmget as nobody on 0600: not EACCES");
	struct shmid_ds d;
	CHECK(shmctl(id, IPC_STAT, &d) == 0 && d.shm_cpid == getpid() && d.shm_segsz == 8192 &&
	      d.shm_nattch == 0, "shm IPC_STAT cpid %d segsz %lu nattch %lu", d.shm_cpid,
	      (unsigned long)d.shm_segsz, (unsigned long)d.shm_nattch);
	char *p = shmat(id, NULL, 0);
	pid_t k = fork();
	if (k == 0) { sleep(1); _exit(0); }
	usleep(100000);
	shmctl(id, IPC_STAT, &d);
	CHECK(d.shm_nattch == 2, "nattch with a forked child: %lu, want 2", (unsigned long)d.shm_nattch);
	waitpid(k, NULL, 0);
	shmctl(id, IPC_STAT, &d);
	CHECK(d.shm_nattch == 1, "nattch after the child exits: %lu, want 1", (unsigned long)d.shm_nattch);
	CHECK(d.shm_atime > 0, "shm_atime not set by shmat");
	shmdt(p);
	shmctl(id, IPC_STAT, &d);
	CHECK(d.shm_lpid == getpid() && d.shm_dtime > 0, "after shmdt: shm_lpid %d dtime %ld", d.shm_lpid, (long)d.shm_dtime);
	CHECK(shmctl(id, SHM_LOCK, NULL) == 0, "SHM_LOCK: %s", strerror(errno));
	shmctl(id, IPC_STAT, &d);
	CHECK(d.shm_perm.mode & SHM_LOCKED, "SHM_LOCKED not in mode %o", d.shm_perm.mode);
	shmctl(id, SHM_UNLOCK, NULL);
	struct shminfo si;
	int hi = shmctl(id, IPC_INFO, (struct shmid_ds *)&si);
	CHECK(hi >= id && si.shmmin == 1, "shm IPC_INFO %d shmmin %lu", hi, (unsigned long)si.shmmin);
	CHECK(proc_long("/proc/sys/kernel/shmmax") == si.shmmax &&
	      proc_long("/proc/sys/kernel/shmmni") == si.shmmni &&
	      proc_long("/proc/sys/kernel/shmall") == si.shmall, "shm limits in /proc differ");
	CHECK(shmctl(id, SHM_STAT, &d) == id, "SHM_STAT of index %d", id);
	CHECK(in_sysvipc("shm", key), "/proc/sysvipc/shm lacks the segment");
	shmctl(id, IPC_RMID, NULL);
}

int main(void)
{
	msg_tests();
	sem_tests();
	shm_tests();
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
