# Ranix Syscall Semantic-Conformance Audit

**Date:** 2026-06-05
**Scope:** the ~230 syscalls Ranix actually dispatches (see `kernel/syscall.c:338`).
**Method:** read every implementation in `kernel/sys/syscall_*.c` (+ `kernel/vfs.c`
for the open/path layer) and compared flag handling, errno paths, and edge cases
against Linux man-page / kernel semantics. This is the **depth** companion to
`docs/linux_parity_audit.md` (which covers breadth — "does syscall X exist").
Here the question is: for the syscalls that DO exist, do they behave like Linux?

READ-ONLY audit. Severity legend:
- **Correctness** — wrong/dangerous result, silent data loss, or a success return
  that lies (real software misbehaves, not just degrades).
- **Compat** — a flag/errno gap that a portable program can usually survive but
  that diverges from Linux (often a stub returning 0/success).
- **Cosmetic** — technically divergent but unlikely to matter.

Every row is grounded in `file:line`. Syscalls examined and found
**Linux-faithful** are listed at the end so they need not be re-audited.

---

## fs / path / open

| Syscall | Ranix behavior (file:line) | Linux behavior | Gap | Severity |
|---|---|---|---|---|
| `open`/`openat`/`creat` | `vfs_open_fds` `kernel/vfs.c:982` honors O_CREAT, O_ACCMODE perm check, O_CLOEXEC; fs `open` fop applies O_TRUNC (`kernel/fs/ramfs.c:132`) | full flag set | **O_EXCL not enforced** — O_CREAT\|O_EXCL on an existing file does NOT return EEXIST (no check; `vfs.c:1014` just opens it). **O_DIRECTORY** ignored (open of a non-dir succeeds, no ENOTDIR). **O_NOFOLLOW**, **O_TMPFILE**, **O_PATH**, **O_NOCTTY**, **O_DIRECT** ignored. Opening a directory O_WRONLY/O_RDWR does not return **EISDIR**. | Correctness |
| `openat` errno masking | `sys_openat` `kernel/sys/syscall_io.c:544` returns `-2` (ENOENT) for ANY `vfs_open<0` | distinct EACCES/EISDIR/ELOOP/EMFILE/ENFILE | All open failures collapse to ENOENT, hiding EACCES/EMFILE/ENOSPC. (`sys_open` at `:536` passes the real code; only `openat` masks — inconsistent.) | Compat |
| `unlinkat` | `sys_unlinkat` `kernel/sys/syscall_fs.c:64` does `(void)flags` | AT_REMOVEDIR routes to rmdir | **AT_REMOVEDIR ignored**; `unlinkat(...,AT_REMOVEDIR)` won't remove a dir / won't reject a file. | Compat |
| `rmdir` / `unlink` | both call `vfs_unlink` (`syscall_fs.c:107`, `:56`) and map any failure to `-2` | rmdir: ENOTEMPTY/ENOTDIR; unlink: EISDIR on a dir | No **ENOTEMPTY**, **ENOTDIR**, **EISDIR** distinction; rmdir doesn't verify the target is a dir. | Compat |
| `linkat` | `sys_linkat` `syscall_io.c:391` `(void)od;(void)nd;(void)f` | resolves dirfds, AT_SYMLINK_FOLLOW, AT_EMPTY_PATH | **dirfds and flags ignored** — only AT_FDCWD-relative hardlinks work. | Compat |
| `renameat2` | `sys_renameat2` `syscall_misc.c:740` `(void)flags` → plain rename | RENAME_NOREPLACE (EEXIST), RENAME_EXCHANGE, RENAME_WHITEOUT | **All renameat2 flags ignored.** RENAME_NOREPLACE silently overwrites; RENAME_EXCHANGE does a one-way move (data loss). | Correctness |
| `fchmodat` | `sys_fchmodat` `syscall_io.c:425` `(void)d;(void)f` | resolves dirfd, AT_SYMLINK_NOFOLLOW | dirfd + flags ignored. | Compat |
| `lchown` | `sys_lchown` `syscall_misc.c:394` is literally `return sys_chown(...)` (follows symlink) | does NOT follow the final symlink | **Wrong target**: chowns the link's referent, not the link. | Correctness |
| `fchownat` | `syscall_io.c:447` ignores AT_SYMLINK_NOFOLLOW (always `vfs_chown`, follows) | NOFOLLOW → lchown semantics | NOFOLLOW ignored. AT_EMPTY_PATH handled. | Compat |
| `fcntl(F_SETLK)` | `syscall_fs.c:389` validates fd; **does not validate `l_type`/`l_whence`** | EINVAL on bad whence | no EINVAL on garbage whence; F_GETLK/F_SETLK/F_SETLKW logic otherwise plausible. F_SETLKW polls via `yield()` (no priority inheritance). | Compat |
| `fcntl` default | `syscall_fs.c:423` `default: return 0` | EINVAL on unknown cmd | F_SETOWN/F_GETOWN/F_SETLEASE/F_NOTIFY/F_SETPIPE_SZ etc. silently return 0 (claim success). | Compat |
| `fcntl(F_SETFL)` | `syscall_fs.c:383` masks `arg & 0xFFF` into flags | only O_APPEND/O_NONBLOCK/O_DIRECT/O_ASYNC are settable; others ignored | accepts/stores arbitrary low bits; O_ASYNC has no effect. | Cosmetic |
| `getdents64` | `syscall_fs.c:258` caps the kernel bounce buffer at 4096 (`:268`) | fills the whole user buffer | a >4 KB `getdents64` returns at most ~4 KB/call (more iterations, but correct). `d_off` uses readdir index, not a real cookie. | Cosmetic |
| `truncate`/`ftruncate` | `syscall_fs.c:226`,`:238` return EINVAL if no truncate iop | ETXTBSY, EISDIR, negative-len EINVAL | no EISDIR / negative-length check (delegated to iop). | Cosmetic |
| `fstat` on fd 0–2 | `sys_fstat` `syscall_io.c:588` returns a **hardcoded fake** char-dev stat for fd 0/1/2 regardless of what they are | stats the actual open file | If stdout is redirected to a regular file, `fstat(1)` still reports a chardev (st_mode 0x2190) — breaks libc full/line-buffering decisions and `isatty`-by-stat. | Correctness |
| `stat`/`fstat` `st_dev` | `fill_linux_stat` `syscall_fs.c:118` hardcodes `st_dev=1` | per-mount device id | all files report the same st_dev → hardlink/`find -xdev`/`du` cross-device logic breaks. | Compat |
| `access`/`faccessat` | `access_check` `syscall_fs.c:186` | AT_SYMLINK_NOFOLLOW, AT_EACCESS supported; raw `access` uses real uid | faithful for the common path; AT_SYMLINK_NOFOLLOW for faccessat is not handled (always follows). | Cosmetic |
| `statx` | `syscall_misc.c:1023` `(void)mask` | honors request mask, btime, STATX_ATTR_* | mask ignored (fills everything); stx_attributes/btime are zero. Functionally OK. | Cosmetic |
| `fallocate` | `syscall_misc.c:734` — **no-op `return 0`** | actually allocates/extends; FALLOC_FL_PUNCH_HOLE etc. | **Lies**: `posix_fallocate` reports success but the file is not extended → later writes/ftruncate-free assumptions fail; PUNCH_HOLE doesn't zero. | Correctness |
| `copy_file_range` | `syscall_misc.c:746` `(void)off_in;(void)off_out` | uses/updates the explicit offsets when given | **explicit offsets ignored** — always copies from/to the fds' current positions and does NOT write back `*off_out`. Wrong results when caller passes offsets (the common case). | Correctness |
| `splice` | `syscall_misc.c:700` generic read/write loop; `(void)flags` | requires one end to be a pipe; SPLICE_F_MOVE/MORE/NONBLOCK | no pipe requirement (works on any fd pair — more permissive), flags ignored, no real zero-copy. | Compat |
| `sendfile` | `syscall_io.c:494` | EINVAL if in_fd not mmap-able/seekable; count overflow | works via copy loop; no offset-overflow check; `out_fd` may be any fd (Linux is lenient post-2.6.33). | Cosmetic |
| `fsync`/`fdatasync`/`sync`/`msync` | `syscall_misc.c:374`,`:375`,`:604`; `sys_msync` `syscall_mm.c:560` — all no-op `return 0` | flush dirty data | acceptable while there is no write-back cache, but `msync(MS_INVALIDATE)`/`MS_SYNC` give no error checking and `fsync` of a bad fd returns 0 instead of EBADF. | Compat |
| `mount` | `syscall_misc.c:606` `(void)flags` | MS_RDONLY/MS_BIND/MS_REMOUNT/MS_NOSUID/... | **mount flags entirely ignored** (no read-only, bind, nosuid, noexec); many fs types accepted-silently as success. | Compat |
| `utimensat` | `syscall_misc.c:804` `(void)flags` | AT_SYMLINK_NOFOLLOW | NOFOLLOW ignored (follows symlink); UTIME_OMIT/UTIME_NOW handled. | Cosmetic |
| `getcwd` | `syscall_fs.c:527` returns from `p->cwd` string | ERANGE if too small; detects unlinked cwd | no detection of a deleted cwd ("(unreachable)"); otherwise OK. | Cosmetic |

## mm

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `mmap` | `sys_mmap` `kernel/sys/syscall_mm.c:59` | MAP_FIXED_NOREPLACE, MAP_NORESERVE, MAP_STACK, file EACCES, offset align | **MAP_FIXED_NOREPLACE not honored** (behaves like MAP_FIXED, clobbers existing mapping). No **EINVAL** when `off` is not page-aligned. No **EACCES** when PROT_WRITE\|MAP_SHARED maps an O_RDONLY fd. File-backed `read` error ignored (`:235` `(void)nr`) → zero-fill instead of SIGBUS/short map. MAP_POPULATE/MAP_NORESERVE/MAP_STACK accepted but lazy/ignored. | Correctness |
| `mmap` non-fixed errno | `:153` returns `-12` ENOMEM when window full | ENOMEM | OK. | — |
| `mprotect` | `:286` | EACCES (exec on noexec fs), PROT_GROWSDOWN/UP | no prot validation against original mapping; PROT_GROWS* ignored; `prot==0` unmaps (loses page on later re-protect, but VMA kept). On unmapped pages with prot!=0 it **allocates a fresh page** (Linux returns ENOMEM for a gap) — silently maps over a hole. | Compat |
| `mremap` | `sys_mremap` `syscall_misc.c:284` `(void)flags;(void)new_addr` | MREMAP_MAYMOVE, MREMAP_FIXED; shrink unmaps tail | **MREMAP_FIXED/MAYMOVE ignored**; shrink (`new<=old`) just returns old_addr **without unmapping the tail** (leak + stale mapping). Grow always moves even when not requested. | Correctness |
| `madvise` | `:446` | MADV_DONTNEED on file map; MADV_REMOVE; MADV_DONTFORK | unknown advices return 0 (Linux EINVAL for some); MADV_FREE drops eagerly (permitted). Reasonable. | Cosmetic |
| `brk` | `:30` returns current brk on failure | returns current brk on failure | faithful. | — |
| `mlock*`/`mlockall` | `:489`–`:549` | flag validation present, prefault works | reasonable given no reclaim. | — |

## proc / sched / exec

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `execve` | `sys_execve` `kernel/sys/syscall_proc.c:982` | **#! shebang** interpreter, ENOEXEC, X-perm EACCES, ETXTBSY | **No shebang support** — running a `#!/bin/sh` script fails (ENOEXEC from elf_load `:1076` → `-8`). No **execute-permission check** (a non-+x file runs if it's a valid ELF). E2BIG/ENAMETOOLONG/ENOMEM handled well. | Correctness |
| `clone`/`clone3` flags | `kernel/syscall.c:461`,`:504` | CLONE_NEWPID/NEWNS/FILES/FS/SIGHAND/PARENT/IO/PIDFD via clone | thread (CLONE_VM) and fork paths handle VM/THREAD/SETTLS/PTID/CTID/CSETTID; but a plain `proc_fork(flags)` does NOT split files/fs/sighand per CLONE_FILES/FS/SIGHAND when CLONE_VM is absent — namespace+resource-sharing clone flags below VM are not individually honored. clone3's `set_tid`/`cgroup`/`pidfd` fields unused. | Compat |
| `wait4`/`waitpid` | `syscall_proc.c:1521` | pid==0 (caller's pgrp), pid<-1 (pgrp), pid>0 any descendant; WUNTRACED/WCONTINUED/__WALL; status EFAULT | **pid==0 and pid<-1 (process-group waits) not handled** — fall into the `pidns_find` branch and fail/misbehave. Only direct children (`child->ppid != p->pid` → ECHILD `:1557`); a reparented/`__WALL` wait won't match. `status`/`rusage` validated by `>0x1000` heuristic, not access_ok → can silently skip a valid low write or fault. | Correctness |
| `waitid` | `sys_waitid` `syscall_misc.c:691` `(void)infop` | fills siginfo_t, supports WNOWAIT, WEXITED/WSTOPPED selection | **siginfo not filled** (infop ignored) — callers reading `si_code`/`si_pid`/`si_status` get garbage. WNOWAIT not honored (always reaps). | Correctness |
| `nanosleep` | `syscall_proc.c:740` `(void)rem` | writes remaining time to `rem` on EINTR | **`rem` never written** — interrupted sleeps can't be correctly resumed. | Compat |
| `clock_nanosleep` | `syscall_misc.c:684` `(void)clkid;(void)flags` → nanosleep | TIMER_ABSTIME makes `req` an absolute deadline; per-clock | **TIMER_ABSTIME ignored** — an absolute-deadline sleep is treated as a relative span (sleeps far too long). clkid ignored. | Correctness |
| `set_tid_address` | `kernel/syscall.c:424` | records clear_child_tid, returns tid | faithful. | — |
| `setpgid` | `syscall_fs.c:607` | EPERM if target is session leader / not child / different session; ESRCH | **no permission checks** — any process can set any other's pgid; no session validation. | Compat |
| `setsid` | `syscall_fs.c:599` | EPERM if caller is already a pgrp leader | **no EPERM check** when already a group leader. | Compat |
| `setuid`/`setgid`/`setreuid`/`setresuid` | `syscall_fs.c:614`, `syscall_misc.c:417`+ | full saved-set-id rules | core rules present; do not go through `from_kuid`/user-ns mapping on the *set* path (getters do, `syscall_fs.c:539`). No supplementary-group/CAP_SETUID nuance. | Compat |
| `getrlimit`/`setrlimit`/`prlimit64` | `syscall_fs.c:578`, `:585`, `syscall_misc.c:840` | per-resource real limits; setrlimit enforced | **setrlimit/prlimit-set are no-ops** (`:585` ignores, `:842` `(void)new_limit`). RLIMIT_NOFILE/STACK report fixed values; RLIMIT_CPU/AS/CORE/NPROC not enforced → a program lowering its limits sees no effect. | Compat |
| `sched_setaffinity` | `syscall_misc.c:857` — no-op `return 0` | actually pins the task | **lies**: claims success, task not pinned. `getaffinity` returns the full online mask. | Compat |
| `getpriority` | `syscall_fs.c:640` | per-which/who lookup; offset 20 to avoid negative ret | ignores `which`/`who` (always the caller); returns `20-nice`. | Compat |
| `sched_setscheduler`/`param` | `syscall_proc.c:895` | SCHED_DEADLINE, SCHED_BATCH/IDLE, EPERM rt-without-cap | NORMAL/FIFO/RR + prio range validated well; no CAP_SYS_NICE check (any task can go SCHED_FIFO); SCHED_BATCH/IDLE/DEADLINE → EINVAL. | Compat |
| `personality` | `syscall_misc.c:532` | many bits; ADDR_NO_RANDOMIZE honored in execve | stores value; only ADDR_NO_RANDOMIZE acted on (execve `:1090`). OK. | — |
| `process_vm_readv/writev` | `syscall_proc.c:1413` caps at 16 iovecs | IOV_MAX (1024) iovecs | caps at 16 (returns EINVAL above) vs Linux 1024; partial-transfer + EPERM/ESRCH handled. | Cosmetic |
| `prctl` | `syscall_misc.c:910` | dozens of options | only NAME/DUMPABLE/SECCOMP handled; **all other options return 0 (claim success)** incl. PR_SET_PDEATHSIG, PR_SET_NO_NEW_PRIVS, PR_CAPBSET_*, PR_SET_CHILD_SUBREAPER → security-relevant ones silently ignored. | Compat |
| `rseq` | `kernel/syscall.c:451` returns `-22` (EINVAL) | registers rseq area | unsupported; glibc tolerates EINVAL/ENOSYS. | Cosmetic |

## signal

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `kill` | `sys_kill` `kernel/sys/syscall_sig.c:60` resolves a single pid | **pid==0** → caller's pgrp; **pid==-1** → all permitted procs; **pid<-1** → pgrp `-pid`; sig==0 existence test | **process-group / broadcast kill not implemented** — `kill(-pgrp,sig)` and `kill(0,sig)` resolve a bogus pid → ESRCH. `kill(p,0)` returns 0 but doesn't verify permission. Breaks shell job control (`kill %job`), `killpg`, and `kill -1`. | Correctness |
| `rt_sigaction` | `syscall_sig.c:12` | rejects SIGKILL/SIGSTOP, EINVAL sig range; SA_* flags stored | only the first 64 bits of sa_mask stored (RT signals 33–64 mask bits dropped). sigsetsize check is `<8` not `!=8`. Otherwise faithful. | Cosmetic |
| `rt_sigprocmask` | `syscall_sig.c:37` | 64-bit mask, SIG_BLOCK/UNBLOCK/SETMASK, unblockable SIGKILL/STOP | faithful (single u64; RT high signals not maskable separately). | — |
| `tgkill`/`tkill` | `syscall_sig.c:73`; `tkill`=`sys_kill` (`syscall_misc.c:673`) | tgkill thread-targeted; tkill thread-targeted | tgkill validates tgid membership (good). **`tkill` is aliased to process-directed `kill`** — signals the group, not the single thread. | Compat |
| `rt_sigqueueinfo` | `syscall_sig.c:88` | reads full siginfo; EPERM cross-uid | reads only si_code+si_value; no EPERM permission gate on the target. | Compat |
| `rt_sigreturn` | `syscall_sig.c:120` | restores frame | faithful + sanitizes PSTATE/RFLAGS (security-correct). | — |
| `pause` / `rt_sigsuspend` | `syscall_misc.c:298`,`:560` | block until handler runs, return EINTR | faithful. | — |
| `rt_sigpending` | `syscall_misc.c:552` | returns pending-and-blocked set | returns full `sig_pending` (incl. unblocked) rather than blocked-only; minor. | Cosmetic |
| `sigaltstack` | `syscall_fs.c:655` | SS_DISABLE, SS_AUTODISARM, EPERM when on-stack | SS_DISABLE handled; min size 2048 vs Linux MINSIGSTKSZ; no EPERM when altering an in-use stack. | Cosmetic |

## net

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `socket`/`socketpair`/`accept4` | `syscall_net.c:440`,`syscall_misc.c:322`,`syscall_net.c:1280` | type flags validated | SOCK_NONBLOCK/SOCK_CLOEXEC honored; **`accept4` does not reject unknown flag bits** (`fl & ~(CLOEXEC\|NONBLOCK)` not EINVAL'd). socketpair only AF_UNIX. | Cosmetic |
| `send`/`sendto`/`sendmsg` | `sys_sendto` `syscall_net.c:619` `(void)flags` | MSG_DONTWAIT, MSG_NOSIGNAL, MSG_MORE, MSG_OOB, MSG_CONFIRM | **send-side MSG_* flags ignored**: no MSG_DONTWAIT (always blocking semantics), **no MSG_NOSIGNAL/SIGPIPE+EPIPE on a closed peer** (write to a reset TCP socket won't EPIPE), MSG_MORE/OOB ignored. | Correctness |
| `recv`/`recvfrom`/`recvmsg` | `syscall_net.c:785`,`:997` | MSG_DONTWAIT(✓), MSG_PEEK, MSG_WAITALL, MSG_TRUNC, MSG_CMSG_CLOEXEC | MSG_DONTWAIT handled; **MSG_PEEK, MSG_WAITALL, MSG_TRUNC not implemented**. recvmsg cmsg/SCM_RIGHTS path exists; MSG_CTRUNC set on overflow. | Compat |
| `connect` | `syscall_net.c:529` | EINPROGRESS on O_NONBLOCK; EISCONN; EALREADY | **non-blocking connect ignored** — always blocks to ESTABLISHED/RST regardless of O_NONBLOCK (no EINPROGRESS). No EISCONN on a second connect. EINTR handled. | Compat |
| `shutdown` | `syscall_net.c:1335` `(void)how` | SHUT_RD/SHUT_WR/SHUT_RDWR distinct; ENOTCONN | **`how` ignored** — always full close. Returns 0 even on a non-stream or unconnected socket (no ENOTCONN). UDP/UNIX shutdown is a silent no-op. | Compat |
| `setsockopt` | `syscall_net.c:1155` | ENOPROTOOPT on unknown opt | **unknown options return 0** (claim success) — `:1191`. Common opts (REUSEADDR, KEEPALIVE, RCV/SNDBUF, RCV/SNDTIMEO, TCP_NODELAY) implemented. SO_LINGER, SO_BINDTODEVICE, IP_TTL, IP_MULTICAST_* etc. silently accepted-and-dropped. | Compat |
| `getsockopt` | `syscall_net.c:1200` | ENOPROTOOPT on unknown | returns ENOPROTOOPT for unknown (good); SO_ERROR read-and-clear correct. | — |
| `bind` | `syscall_net.c:478` `(void)addrlen` | EADDRINUSE, EACCES <1024, EINVAL bad addrlen | EADDRINUSE via reuse check; **no privileged-port (<1024) EACCES**; addrlen not validated. | Compat |
| `getsockname`/`getpeername` | (`syscall_net.c`) | ENOTCONN for unconnected peer | (examined) return the stored addr; peername on unconnected socket may not ENOTCONN. | Cosmetic |

## time

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `clock_gettime`/`getres` | `syscall_proc.c:588`,`:622` | per-clock, namespace offsets | faithful — REALTIME/MONOTONIC/BOOTTIME/CPUTIME/RAW/COARSE/TAI all handled, EINVAL on unknown, ns-resolution from HW counter. | — |
| `gettimeofday`/`settimeofday`/`time` | `syscall_proc.c:646`,`:679`, `syscall_misc.c:675` | CAP_SYS_TIME for set | faithful; settimeofday persists to RTC. | — |
| `adjtimex` | `syscall_proc.c:711` | full NTP discipline | ADJ_SETOFFSET + ADJ_FREQUENCY honored; most status/PLL fields reported as 0. Adequate for ntpd stepping. | Cosmetic |
| `setitimer`/`getitimer`/`alarm` | `syscall_proc.c:760`, `syscall_misc.c:306` | REAL/VIRTUAL/PROF | faithful (all three timers, old-value report, race-safe under sched_lock). | — |
| `times` | `syscall_proc.c:803` | tms in clock ticks | faithful. | — |
| `timerfd_settime` | `syscall_event.c:288` | TFD_TIMER_ABSTIME against CLOCK_REALTIME | ABSTIME handled but measured against `kernel_ticks` (boot-relative), not the selected clock's epoch — an absolute REALTIME deadline is wrong after a clock step. | Compat |

## ipc

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| SysV `msgsnd`/`msgrcv`/`msgget`/`msgctl` | `syscall_ipc.c:447`–`:528` | full | faithful: msgtyp 0/>0/<0, MSG_EXCEPT, MSG_NOERROR, IPC_NOWAIT→EAGAIN/ENOMSG, E2BIG, EIDRM. Missing: **EINTR** on a signal while blocked (sleeps without signal check). | Compat |
| SysV `semget`/`semop`/`semctl` | `syscall_ipc.c:234`–`:334` | SEM_UNDO, IPC_NOWAIT, GETALL/SETALL | (examined) core ops present; verify SEM_UNDO bookkeeping on exit. IPC_NOWAIT honored. | Cosmetic |
| SysV `shmget`/`shmat`/`shmdt`/`shmctl` | `syscall_ipc.c:40`–`:185` | SHM_RDONLY, SHM_RND, IPC_RMID-while-attached | core attach/detach present; check SHM_RDONLY/SHM_RND flag handling on shmat. | Cosmetic |
| POSIX `mq_*` | `syscall_ipc.c:675`–`:768` | O_NONBLOCK, timed send/recv, notify | mq_open/send/recv/getsetattr present; mq_notify absent; timed variants honor the abs timeout. | Cosmetic |

## misc / security

| Syscall | Ranix behavior (file:line) | Linux | Gap | Severity |
|---|---|---|---|---|
| `getrandom` | `syscall_misc.c:981` `(void)flags` | GRND_RANDOM/GRND_NONBLOCK; CSPRNG | **uses splitmix64 seeded with `kernel_ticks`** (`syscall.c:192`) — NOT cryptographically secure, and flags ignored. Anything relying on getrandom for keys gets predictable bytes. | Correctness |
| `seccomp`/`prctl(PR_SET_SECCOMP)` | `syscall_misc.c:901`,`:939` | SECCOMP_FILTER_FLAG_TSYNC/LOG/SPEC_ALLOW | `flags` ignored; cBPF subset interpreter (`syscall.c:259`) lacks BPF_ALU MUL/DIV/LSH/RSH, X-register JMPs, BPF_MEM — a non-trivial filter (e.g. libseccomp's) may mis-evaluate → unknown insn returns KILL (fail-closed, safe but can break a legitimate sandbox). | Compat |
| `capget`/`capset` | `syscall_misc.c:95`,`:121` | v3 caps, pP/pI/pE sets | (examined) handles the cap header/data; verify it rejects raising caps not in the permitted set. | Cosmetic |
| `umask` | `syscall_fs.c:677` | faithful | — | — |
| `uname` | `syscall_proc.c:12` | reports sysname "Ranix", release "6.6.0-ranix" | faithful layout; sysname is "Ranix" not "Linux" (intentional — may trip code that string-matches "Linux"). | Cosmetic |
| `sysinfo` | `syscall_proc.c:817` | loads[] populated | load averages reported as 0; ram/procs OK. | Cosmetic |
| `reboot` | `syscall_misc.c:770` | magic + CAP_SYS_BOOT | faithful. | — |
| `eventfd2` | `syscall_event.c:94` | EFD_CLOEXEC on the fd | **EFD_CLOEXEC not applied to the fd** (only EFD_NONBLOCK → f->flags); EFD_SEMAPHORE stored in efd->flags (used by read). | Compat |

---

## Top 15 semantic-conformance gaps most likely to break real software

1. **`kill` has no process-group / broadcast support** (`syscall_sig.c:60`).
   `kill(-pgrp)`, `kill(0)`, `kill(-1)`, `killpg`, and shell job control (`kill %1`)
   all fail with ESRCH. Pervasive in shells, `make -j`, service managers.

2. **`open` does not enforce O_EXCL / O_DIRECTORY / EISDIR** (`vfs.c:1014`).
   `O_CREAT|O_EXCL` silently opens an existing file (lockfile/`mkstemp` races,
   security checks defeated); opening a dir for write succeeds.

3. **`execve` has no `#!` shebang handling and no execute-permission check**
   (`syscall_proc.c:982`). Script interpreters can't be exec'd → any
   `#!/bin/sh`/`#!/usr/bin/env python` program fails with ENOEXEC.

4. **`fallocate` is a no-op that returns success** (`syscall_misc.c:734`).
   `posix_fallocate`/`posix_fallocate64` believe space is reserved; databases,
   SQLite, mmap-backed allocators then hit unexpected short writes / SIGBUS.

5. **`copy_file_range` ignores explicit offsets** (`syscall_misc.c:746`).
   Modern coreutils `cp`, Go, and Rust std use it with offsets; results are wrong
   (reads/writes at fd position, never updates `*off_out`).

6. **`renameat2` ignores RENAME_NOREPLACE / RENAME_EXCHANGE** (`syscall_misc.c:740`).
   NOREPLACE silently overwrites (atomic-create-or-fail breaks); EXCHANGE
   becomes a one-way move → **data loss**.

7. **`clock_nanosleep` ignores TIMER_ABSTIME** (`syscall_misc.c:684`).
   An absolute deadline is treated as a relative duration → threads sleep for
   "now + deadline" (often hours). Used by condvars / `pthread_cond_timedwait`
   on some libcs and by rate-limited loops.

8. **`waitid` does not fill siginfo and ignores WNOWAIT** (`syscall_misc.c:691`);
   **`wait4` lacks pid==0 / pid<-1 (process-group) waits** (`syscall_proc.c:1521`).
   Breaks `posix_spawn` error reporting and any group-wait supervisor.

9. **TCP send gives no EPIPE/SIGPIPE and no MSG_NOSIGNAL/MSG_DONTWAIT**
   (`syscall_net.c:619`). Writing to a peer-reset socket doesn't error as Linux
   does; non-blocking senders can't get EAGAIN via MSG_DONTWAIT.

10. **Non-blocking `connect` has no EINPROGRESS; `shutdown` ignores `how`**
    (`syscall_net.c:529`,`:1335`). Event-loop servers (libuv/asyncio style) that
    do non-blocking connect + poll, or half-close with SHUT_WR, misbehave.

11. **`fstat` on fd 0/1/2 returns a fake chardev** (`syscall_io.c:588`).
    With stdout redirected to a file, libc still thinks it's a tty → wrong
    buffering, and stat-based `isatty`/size queries are wrong.

12. **`setrlimit`/`prlimit64` are no-ops** (`syscall_fs.c:585`, `syscall_misc.c:840`).
    Programs that lower RLIMIT_AS/NOFILE/CORE/CPU for sandboxing or testing see no
    enforcement; `ulimit` in a shell appears to work but doesn't.

13. **`getrandom` is not cryptographically secure and ignores flags**
    (`syscall_misc.c:981`). Key/nonce/cookie generation in TLS, OpenSSH, etc. is
    predictable — a security gap, not just a compat one.

14. **`mremap` can't shrink (leaks the tail) and ignores MREMAP_FIXED/MAYMOVE**
    (`syscall_misc.c:284`). `realloc`-heavy code on large mappings and anything
    using MREMAP_MAYMOVE for in-place grow gets stale/leaked mappings.

15. **`lchown` follows the symlink** (`syscall_misc.c:394`) and the `*at` family
    broadly ignores AT_SYMLINK_NOFOLLOW / dirfds (`linkat`, `fchmodat`, `fchownat`,
    `unlinkat` AT_REMOVEDIR). `tar`/`rsync`/`cp -a` restoring symlinks chown the
    wrong inode and can't selectively remove directories.

---

## Checked and found Linux-faithful (no re-audit needed)

- `read`/`write`/`readv`/`writev` (EFAULT via access_ok, IOV_MAX, short-write loop)
  — note writev/readv are **not atomic** across iovecs but the per-buffer result
  is correct.
- `lseek`, `dup`/`dup2`/`dup3` (CLOEXEC), `pipe2` (O_NONBLOCK/O_CLOEXEC),
  `close_range` (CLOSE_RANGE_CLOEXEC), `fcntl` F_DUPFD/F_GETFD/F_SETFD/F_GETFL.
- `brk`, `mlock`/`munlock`/`mlock2`/`mlockall`/`munlockall` (flag validation),
  `mmap` anonymous/PROT_NONE/hugetlb paths.
- `rt_sigreturn` (with PSTATE/RFLAGS sanitization), `rt_sigprocmask`, `pause`,
  `rt_sigsuspend`, `sigaltstack` (common path).
- `clock_gettime`/`clock_getres`/`gettimeofday`/`settimeofday`/`time`,
  `setitimer`/`getitimer`/`alarm`, `times`.
- SysV message queues (`msgsnd`/`msgrcv`/`msgctl`) — only missing EINTR-while-blocked.
- `futex` FUTEX_WAIT/WAKE/WAKE_OP/REQUEUE/CMP_REQUEUE/WAIT_BITSET, robust lists,
  `set_robust_list`/`get_robust_list`.
- `getuid`/`geteuid`/`getgid`/`getegid`/`getpid`/`getppid`/`gettid`/`getpgid`/
  `getsid`/`getgroups`/`setgroups` (user-ns mapped on the get path).
- `membarrier` (real cross-CPU barrier), `getcpu`, `umask`, `reboot`, `uname`.
- `chroot`/`pivot_root` (EPERM, ENOENT, ENOTDIR, put_old-under-new check),
  `chdir`/`fchdir` (ENOTDIR), path normalization.
- `accept4`/`listen`/`getsockopt` errno paths; `bind` EADDRINUSE; SO_REUSEADDR/PORT,
  SO_RCVTIMEO/SNDTIMEO, TCP_NODELAY.
- `flock` (LOCK_SH/EX/UN/NB, EWOULDBLOCK, EINTR), `fcntl` POSIX locks (F_GETLK/SETLK/SETLKW).
