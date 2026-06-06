# uaccess funnel inventory (task #77 — SMAP/PAN enable)

Status: audit only. No code changed. This is the complete inventory of every
kernel site that touches a user-space pointer, classified for the SMAP (x86,
CR4.SMAP + STAC/CLAC) / PAN (aarch64, PSTATE.PAN) enable work.

When SMAP/PAN are on, a plain load/store to a user address from EL1/ring-0
faults. Every user-memory access must go through an explicit accessor
(`copy_from_user`/`copy_to_user`/`strncpy_from_user`) or an STAC/CLAC- /
PAN-bracketed window. The earlier enable attempt fault-looped because the fops
read/write path and the tty path dereference the user buffer directly.

---

## 1. Existing uaccess machinery

### Accessors
`kernel/lib/string.c`:
- `__copy_user()` (lines 14-53) — the byte-copy primitive. Hand-written asm
  loop, x86 (`movb`) / aarch64 (`ldrb`/`strb`), with `_ASM_EXTABLE` entries on
  both the load and the store so a fault on a bad user address branches to the
  exit label and returns the not-yet-copied count instead of panicking.
- `access_ok()` (69-78) — range check: rejects `addr >= VIRT_OFFSET` /
  wrap-around. Pure bounds check, no page-table walk.
- `user_pages_mapped()` (55-67) — optional pre-check walking
  `arch_vm_get_paddr()` per page. Used by the signal-frame path.
- `copy_from_user()` (80-84), `copy_to_user()` (86-90) — `access_ok` then
  `__copy_user`. Return 0 on success, -1 on fault/bad-range.
- `strncpy_from_user()` (92-109) — byte loop with a per-page
  `arch_vm_get_paddr` check (does NOT use the extable loop; it pre-walks).

Declarations: `include/kernel/hal.h:90-94`.

### Exception-fixup (extable)
- `include/kernel/extable.h` — `struct ex_entry {insn; fixup}` and the
  `_ASM_EXTABLE(insn, fixup)` macro that emits absolute-address pairs into
  section `.__ex_table`.
- `kernel/lib/extable.c` — `search_extable(pc)` linear scan of
  `__start___ex_table .. __stop___ex_table`.
- Linker glue: `arch/x86_64/linker.ld:53-57`, `arch/aarch64/linker.ld:49-53`,
  `arch/aarch64/linker_raspi4.ld:50-54`.
- Fault dispatch: `arch/x86_64/exceptions.c:87-91` and
  `arch/aarch64/exceptions.c:104-108` call `search_extable` on the faulting
  PC/ELR and, if found, set `rip`/`elr` to the fixup and return. Note: this
  only runs **after** the ring-3 segfault branch (x86 71-85, aarch64 70-102);
  an EL1/ring-0 fault on a user address with no extable entry falls through to
  `panic_report` — which is exactly the fault-loop the first SMAP attempt hit.

### STAC/CLAC / PAN state
None. A tree-wide search for `SMAP`, `STAC`, `CLAC`, `PAN`, `CR4`-SMAP,
`set_pan` finds nothing. CR4.SMAP is never set; PSTATE.PAN is never toggled.
`__copy_user` does not bracket its loop with STAC/CLAC.

**Where the hooks must go:**
1. Inside `__copy_user` (and the `strncpy_from_user` byte loop) — bracket the
   actual user touches with `stac()`/`clac()` (x86) and PAN-clear/PAN-set
   (aarch64, `msr PSTATE.PAN`). This is the single funnel point for the bulk of
   conversions.
2. A new pair of explicit bracket helpers (proposed `uaccess_begin()` /
   `uaccess_end()`) for the user-stack and cross-space cases that cannot use
   `copy_*_user` (signal frame, ptrace PEEK/POKE) — see §3a.
3. CR4.SMAP enable at boot (x86 trampoline/CPU init) and TCR/SCTLR PAN enable
   on aarch64, gated behind a Kconfig + a runtime debug-warn mode (§3d).

---

## 2. The inventory

### How user buffers reach the kernel
`syscall_handle()` (`kernel/syscall.c:291-701`) casts the raw register args to
typed pointers and passes them straight into the per-syscall handlers. There is
NO copy at the dispatch boundary; each handler is responsible for funneling.
Most scalar-struct syscalls already do (`copy_from_user`/`copy_to_user` counts:
net 63, misc 45, proc 35, io 19, fs 13, event 13, ipc 12, sig 8, ptrace 2).

The danger is **not** the syscall arg structs — it is the read/write data path:
`sys_read`/`sys_write` (`kernel/sys/syscall_io.c:472-482`) only `access_ok` the
buffer, then hand the **raw user pointer** to `vfs_read`/`vfs_write`
(`kernel/vfs.c:1200-1224`), which call `f->inode->fops->read(f, buf, len)` /
`->write(...)` with that same raw pointer. Every fops read/write implementation
therefore receives a user pointer and `memcpy`s it. That is the RAW class.

---

### 2a. OK — already funneled

| Site | File:line | Notes |
|------|-----------|-------|
| poll/select/ppoll/pselect | `syscall_io.c:80-181` | kmalloc bounce + copy_from/to_user |
| epoll_ctl/epoll_pwait | `syscall_io.c:258-359` | copy_from/to_user on events |
| ioctl fallback (winsize/termios/pgrp) | `syscall_io.c:29-44` | copy_to/from_user |
| sendfile offset | `syscall_io.c:500-522` | copy_from/to_user; in-kernel data |
| writev/readv iovec descriptors | `syscall_io.c:484-540` | copy_from_user per iovec (but the per-iovec *data* goes via sys_read/write → RAW fop, see 2b) |
| fstat / newfstatat / statx | `syscall_fs.c:160-193`, `syscall_misc.c` | copy_to_user of kernel stat struct |
| getdents64 | `syscall_fs.c:270-314` | builds kbuf, copy_to_user at :314 |
| path args (open/at, chmod, chown, symlink…) | via `copy_path_from_user`/`resolve_path`/`strncpy_from_user` | OK |
| futex uaddr/uaddr2/timeout | `syscall_proc.c:309,330,362,369,389` | copy_from/to_user |
| robust-list walk | `syscall_proc.c:461,473` | copy_from_user |
| clock/timer copyin/out | `syscall_proc.c:663,685,715,745`, misc clock paths | copy_from/to_user |
| uname/sysinfo/getrusage/getres*/getcpu | `syscall_misc.c:74,90,424,486-512,564` | copy_to_user |
| sigaction/sigprocmask/sigpending/sigsuspend | `syscall_sig.c` | copy_from/to_user |
| sigreturn FP round-trip on x86 fxsave dest | n/a | (the frame itself is RAW — see 2c) |
| socket bind/connect/sockaddr | `syscall_net.c:485-583` | copy_from_user of sockaddr |
| sendto/recvfrom buf | `syscall_net.c:156,633,658,725,772,807-891` | kmalloc bounce + copy |
| sendmsg/recvmsg iovec + data + cmsg/SCM_RIGHTS | `syscall_net.c:938-1095` | msghdr, iovec array, iov bases, and cmsg all via copy_from/to_user (971,999,1006,1044,1080) |
| ioctl SIOCGIFCONF/HWADDR | `syscall_net.c:241-339` | copy_from/to_user |
| SysV msgsnd/msgrcv mtext | `syscall_ipc.c:451-519` | copy_from/to_user + kernel bounce |
| SysV semop/semctl/shmctl | `syscall_ipc.c:210,276,361,367` | copy_from/to_user |
| POSIX mqueue send/recv/attr | `syscall_ipc.c:601-787` | copy_from/to_user |
| ptrace GETREGS/SETREGS | `syscall_ptrace.c:161,170` | copy_to/from_user |
| clone3 args | `syscall.c:508` | copy_from_user |
| process_vm_readv/writev | `syscall.c:528-533` → handler | copies via iovec (verify on convert) |
| fbdev FBIOGET_*SCREENINFO | `drivers/video/fbdev.c:78,87` | copy_to_user |
| /dev/rtc RTC_RD_TIME | `kernel/fs/devfs.c:184` | copy_to_user |
| /dev/kmsg via copy paths | partial — see 2b | |
| pty (ptmx/pts) data + ioctl | `kernel/fs/pty.c` | 9 copy_*_user uses |
| procfs mem/timens/some files | `procfs.c:708,1342` | copy_to_user |

### 2b. RAW (must fix) — direct memcpy/deref on a user buffer

The dominant class. Funnel = `copy_to_user` for reads (kernel→user dest) and
`copy_from_user` for writes (user src→kernel). Bulk fops can also use an
`uaccess_begin/end` bracket around the existing memcpy if a bounce buffer is too
costly, but copy_*_user is the mechanical, lowest-risk conversion.

**Core fops read/write (every filesystem + char dev):**

| Function | File:line | User ptr | Funnel |
|----------|-----------|----------|--------|
| `ramfs_read` | `kernel/fs/ramfs.c:71` | `memcpy(buf, …)` dest | copy_to_user |
| `ramfs_write` | `kernel/fs/ramfs.c:103` | `memcpy(…, buf, len)` src | copy_from_user |
| `tmpfs_read` | `kernel/fs/tmpfs.c:123` | dest buf | copy_to_user |
| `tmpfs_write` | `kernel/fs/tmpfs.c:154` | src buf | copy_from_user |
| `ext2_read` | `kernel/fs/ext2.c:544` | `memcpy((u8*)buf+done,…)` | copy_to_user |
| `ext2_read` (inline symlink / fast path) | `kernel/fs/ext2.c:1292,1306` | dest buf | copy_to_user |
| `ext2_write` | `kernel/fs/ext2.c:916` | `memcpy(blkbuf+boff, src+done,…)` src=buf | copy_from_user |
| `fat_read` | `kernel/fs/fat.c:281` | dest buf | copy_to_user |
| `hugetlbfs_read` | `kernel/fs/hugetlbfs.c:126` | dest buf | copy_to_user |
| `hugetlbfs_write` | `kernel/fs/hugetlbfs.c:147` | src buf | copy_from_user |
| `procfs_file_read` | `kernel/fs/procfs.c:608` | dest buf (version/meminfo/stat/maps/… all flow here) | copy_to_user |
| `procfs` cmdline/readlink/etc. | `procfs.c:300,778,913,989` | dest buf | copy_to_user (some siblings already use it at :708,1342) |
| `sysfs` read | `kernel/fs/sysfs.c` (memcpy=1) | dest buf | copy_to_user |
| `zero_read` (/dev/zero,/dev/full) | `kernel/fs/devfs.c:53` | `memset(buf,0,len)` | needs zeroing accessor / bracket |
| `urandom_read` | `kernel/fs/devfs.c:81` | `csprng_bytes(buf,len)` writes user buf | bounce + copy_to_user, or bracket csprng |
| `kmsg_read` | `kernel/fs/devfs.c:114` | `memcpy(buf,…)` dest | copy_to_user |
| `kmsg_write` | `kernel/fs/devfs.c:124` | reads `s[i]` from user | copy_from_user (bounce) |
| `fdproxy_read/write` | `kernel/fs/devfs.c:207,220` | proxied buf | inherits target fop |
| devfs symlink/mknod copyout | `kernel/fs/devfs.c:404,420` | `memcpy(buf,…)` dest | copy_to_user |

**TTY (console + serial — high traffic):**

| Function | File:line | User ptr | Funnel |
|----------|-----------|----------|--------|
| `tty_read` | `kernel/tty/tty_core.c:201` | `b[copied++]=…` raw store to user buf | copy_to_user (per chunk) |
| `tty_write` | `kernel/tty/tty_core.c:227,238` | reads `p[i]` from user buf | copy_from_user (bounce) |
| `tty_echo_str` arg | `tty_core.c:252` | passes user `buf` onward | depends on above |

(termios ioctl in tty_core at :292-314 is already copy_*_user — OK.)

**Input (evdev):**

| Function | File:line | User ptr | Funnel |
|----------|-----------|----------|--------|
| `evdev_read` | `drivers/input/input_core.c:114` | `memcpy(dst,…)` dest=user buf, **under ev_lock IRQs-off** | copy_to_user, but the lock window means a faulting copy is unsafe — copy outside the lock (bounce) or pre-fault |
| `evdev_ioctl` EVIOCGBIT | `drivers/input/input_core.c:142,144` | `memset(buf,…)` / `buf[0]=` raw | copy_to_user |

**execve argv/envp (RAW array + RAW strings):**

| Site | File:line | User ptr |
|------|-----------|----------|
| count argv/envp | `syscall_proc.c:1093,1096` | derefs `argv[i]`/`envp[i]` (the pointer array) |
| stage argv strings | `syscall_proc.c:1106,1108,1112` | derefs `s` (user string), `memcpy(kstrs,s,n)` |
| stage envp strings | `syscall_proc.c:1124,1126,1130` | same |
| sys_execve path scan | `syscall_proc.c:1008` | `path[i]` raw (only when called directly via SYS_ARCH_EXECVE with a user path; execveat pre-copies) |

Funnel: `get_user`-style read of each array slot, then `strncpy_from_user` for
each string. There is no `get_user`/`put_user` macro today — they must be added
(see §3) or replaced with `copy_from_user(&ptr, &argv[i], 8)` + bounded
`strncpy_from_user`.

### 2c. AMBIGUOUS — needs a bracketed window, not copy_*_user

These run on / against a live user mapping where a bounce copy is wrong or
impossible (they must touch the user page in place). They need an explicit
`uaccess_begin()/end()` (STAC/CLAC, PAN-clear/set) bracket and a pre-fault /
`access_ok` + `user_pages_mapped` guard (most already have the guard).

| Site | File:line | Why bracket, not copy |
|------|-----------|-----------------------|
| signal frame **build** (aarch64) | `kernel/signal.c:504-525` | writes ~30 fields directly to the user stack `usp`; already guards with `access_ok`+`user_pages_mapped` at :499. Wrap 504-525 in a window. |
| signal frame **build** (x86_64) | `kernel/signal.c:571-602` | same; guard at :566; `memcpy((void*)(usp+168), fxsave,…)` at :592 is a user-stack store. |
| signal frame **restore** (sigreturn, aarch64) | `kernel/sys/syscall_sig.c:150-164` | reads ~30 fields directly from user stack `usp` |
| signal frame **restore** (sigreturn, x86_64) | `kernel/sys/syscall_sig.c:170-192` | reads from user stack `base` |
| ptrace PEEKDATA | `kernel/sys/syscall_ptrace.c:207` | `*(volatile u64*)addr` after switching to the **tracee's** address space; copy_*_user targets current space, so cannot be used as-is. Bracket + keep the existing access_ok/paddr guard at :202. |
| ptrace POKEDATA | `kernel/sys/syscall_ptrace.c:220` | `*(volatile u64*)addr = data` in tracee space; same. |
| AT_RANDOM / auxv / argv-pointer-array **store** onto the new user stack | `syscall_proc.c` exec stack-build (staged in kbuf then published; see :1326-1371) | currently staged in a kernel buffer and copied via the direct-map alias of the freshly-allocated stack pages, NOT through the user VA — verify on convert; if it writes via user VA it needs a bracket, if via direct-map it is OK. |
| vDSO | `vdso/` + mmap of vdso page | user reads the vdso; kernel does not write it through the user VA at runtime → likely OK, but the data-page update path (gettimeofday tick) must be confirmed to use the kernel alias. |
| fault-handler stack-walk deref | `arch/aarch64/exceptions.c:90-91` | reads user fp-chain `*(u64*)fp`; already guarded by access_ok+user_pages_mapped at :89; with PAN on, the read still faults unless bracketed. Bracket the two derefs. |
| evdev_read under ev_lock | `drivers/input/input_core.c:114` | (also listed in 2b) the IRQs-off lock window makes a faulting copy unsafe; either bracket+pre-fault or move the copy out of the lock. |

---

## 3. Funnel design recommendation

### 3a. New bracket helpers (small, arch-specific)
Add to `kernel/hal.h` + arch impls:
```
static inline void uaccess_begin(void);  /* x86: stac;  arm: msr PSTATE.PAN,#0 (clear) */
static inline void uaccess_end(void);    /* x86: clac;  arm: msr PSTATE.PAN,#1 (set)  */
```
Use these for the §2c cases (signal frame, sigreturn, ptrace PEEK/POKE, fault
stack-walk). Also bracket the inner loop of `__copy_user` and the
`strncpy_from_user` byte loop so all copy_*_user traffic keeps working with PAN
on. Optionally add `get_user`/`put_user` scalar macros (extable-protected
single load/store + bracket) for the execve argv/envp array walk.

### 3b. Mechanically convertible to copy_*_user (the bulk)
All of §2b. The per-fs read/write fops are the same shape: replace
`memcpy(buf, kbuf, n)` → `copy_to_user(buf, kbuf, n)` and
`memcpy(kbuf, buf, n)` → `copy_from_user(kbuf, buf, n)`. ramfs/tmpfs read/write
already memcpy from/to a contiguous kernel `rf->data`, so the conversion is a
one-line swap each. ext2/fat/hugetlbfs already stage through a block/tmp buffer
(`tmp`, `blkbuf`, sector buffer) — convert the final user-facing memcpy only.
procfs/sysfs/devfs likewise stage in a kernel `tmp` page. tty_read/tty_write and
evdev need a small bounce buffer (they currently byte-poke the user buffer
directly under a lock).

### 3c. Ordering / risk
1. **First, safest:** add the bracket helpers + bracket `__copy_user` /
   `strncpy_from_user`, but do NOT enable SMAP/PAN yet. No behavior change.
2. Convert the §2b fops read/write — these are independent, one-line swaps, and
   are exercised by nearly every test (file I/O, /proc, tty). Convert
   filesystem-by-filesystem and run the suite after each.
3. Convert tty_read/tty_write and evdev (move the copy out of the IRQ-off lock
   or pre-fault). **Most dangerous** because of the lock window and because the
   console is the failure-reporting channel — a regression here blinds you.
4. Convert execve argv/envp (add get_user/strncpy_from_user walk).
5. **Last, highest blast radius:** bracket the §2c signal/ptrace/fault-walk
   sites, then flip SMAP/PAN on.

The single most dangerous site is **tty** (it is both very high traffic and the
log channel); the second is the **signal frame** (every delivered signal, on the
user stack, and a fault there panics). evdev's IRQs-off copy is a correctness
trap independent of SMAP.

### 3d. Incremental testing (avoid the all-or-nothing fault-loop)
Add a Kconfig `CONFIG_UACCESS_HARDEN` with two runtime modes:
- **warn mode:** SMAP/PAN stays OFF, but instrument the page-fault handler so an
  EL1/ring-0 fault whose faulting address is a user VA (`< VIRT_OFFSET`) that is
  NOT inside a `uaccess_begin/end` window and has NO extable entry prints a
  one-line `WARN: raw user access at <pc>` (rate-limited) and continues. Run the
  full 192/192 suite; every warning is an unconverted RAW site. This finds
  anything this audit missed without bricking the kernel.
- **enforce mode:** set CR4.SMAP / PSTATE.PAN. Only flip after warn mode is
  clean across both arches.

The fault handler already distinguishes ring-3 from ring-0 and already consults
the extable (`exceptions.c:71-93` / `:70-110`); the warn hook slots in right
before `panic_report`.

---

## 4. Effort estimate

| Subsystem | RAW sites | Effort | Risk |
|-----------|-----------|--------|------|
| bracket helpers + `__copy_user`/`strncpy_from_user` instrumenting | n/a | 0.5 session | low |
| warn-mode fault hook + Kconfig | n/a | 0.5 session | low |
| ramfs/tmpfs read/write | 4 | trivial | low |
| ext2/fat/hugetlbfs read/write | ~7 | small | low (staged already) |
| procfs/sysfs/devfs read/write/copyout | ~12 | small | low |
| tty_read/tty_write | 3 | medium (lock + bounce) | **high** (log channel) |
| evdev read/ioctl | 3 | medium (IRQ-off lock) | medium |
| execve argv/envp | 4 | medium (array+string walk) | medium |
| signal frame build/restore (×2 arch) | 4 regions | medium (brackets) | **high** |
| ptrace PEEK/POKE | 2 | small (bracket, cross-space) | medium |
| fault-handler stack-walk | 1 (aarch64) | trivial | low |
| **RAW sites total** | **~40 distinct sites** (≈25 fop read/write + tty/evdev/execve/signal/ptrace) | | |

### Verdict
**Multi-agent epic, not a single session** — but a well-bounded one. The bulk
(§2b, ~25 sites) is mechanical one-line copy_*_user swaps that any agent can do
in parallel, filesystem-by-filesystem, behind the warn-mode safety net. The hard
core is small and concentrated: tty, the signal frame, ptrace cross-space, and
evdev's IRQ-off copy. The decisive de-risking move is to land the bracket
helpers + the warn-mode fault hook **first** (SMAP/PAN still off), convert under
that net until warn mode is silent on both arches, and only then flip enforce.
That converts the original all-or-nothing fault-loop into an incremental,
observable migration.
