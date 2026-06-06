# Latent Hazard Audit (forward planning, not fixing)

**Scope:** Read-only audit of the Ranix dual-arch (x86_64 + aarch64) kernel for the
*next* latent bugs, focused on the bug classes that have already bitten this codebase:
non-volatile DMA/MMIO in poll loops, IRQ-safety after the IRQ-on-syscall flip,
`->priv` type confusion, fixed-size stack buffers, refcount/UAF, integer overflow,
unchecked user pointers, and improvable/future-fragile points.

**Method:** Grounded code reading of every driver poll loop, every plain `spin_lock`
call site, every `void *priv` cast, on-stack `char[N]` buffers in deep paths, every
`fd_get`/`fget` call site and refcount mutation, the mmap/read/write/iov/cmsg arithmetic,
and `copy_*_user` usage. Each finding cites `path:line`.

**Date:** 2026-06-06. **Branch:** master (worktree). Recent context: x86 syscalls now run
with IRQs enabled; SMP is live on both arches.

**Headline:** The two historically catastrophic classes — (1) non-volatile device reads
and (3) `->priv` type confusion — are now **clean**: every device poll loop uses a
`volatile` accessor or a per-iteration memory-clobber barrier, and every `->priv` cast is
reached either through a fops dispatch (so the kind is guaranteed) or behind an explicit
`f->inode->fops != &X_fops` guard. The live exposure has shifted to **(5) raw `fd_get`
pointers held across a block** and a **non-atomic file refcount in SCM_RIGHTS**, both newly
reachable now that syscalls can be preempted on SMP.

---

## Class 1 — Non-volatile DMA / MMIO reads in poll loops

Status: **CLEAN.** No remaining hoistable device-shared read found.

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| xHCI event ring read | `drivers/usb/xhci.c:109,195,234` | `erst_seg` is `volatile trb_t *`; each loop iteration reloads `e->status`. | — (already correct) | none |
| NVMe CQE phase poll | `drivers/block/nvme.c:140-141` | `volatile uint16_t *st = &c->status` reloaded every iteration. | — (already correct) | none |
| AHCI PxTFD/PxCI poll | `drivers/block/ahci.c:90,94,175` | reads go through `volatile uint32_t *p`. | — (already correct) | none |
| virtio-blk used-ring poll | `drivers/block/virtio_blk.c:158,220` | `p->used->idx` is non-volatile but the loop body has `__asm__ volatile(... ::: "memory")`, forcing a reload. | — (correct, but see Class 8 portability) | none |
| virtio-net used-ring poll | `drivers/net/virtio_net.c:274-278,295-298` | non-volatile `tx_used->idx`/`rx_used->idx`, but `dsb sy` memory clobber inside/around the loop. | — (already correct) | none |
| GENET prod/cons + RX desc | `drivers/net/bcmgenet.c:339-340,351-373` | all via `gr()` → `reg_read32` → `*(volatile uint32_t*)`. | — (already correct) | none |
| GIC / register accessors | `drivers/irqchip/gic400/gic400.c:11-12,70,76`; `include/kernel/hal.h:92-97` | `reg_read32`/`reg_write32` are `volatile`. | — (already correct) | none |

---

## Class 2 — Lock / IRQ-safety after the IRQ-on-syscall flip

Status: **MOSTLY CLEAN.** The sched/signal/itimer/net/tty hot paths are all
`spin_lock_irq`. Every plain `spin_lock` found is on a lock that is **only** ever taken in
syscall/VFS context (never from an IRQ/timer/IPI), so the flip does not endanger them — but
two are worth recording as fragile.

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| `ephemeral_lock` uses plain `spin_lock` | `kernel/net/tcp.c:474-477` | Ephemeral-port allocation. Today reached only from `connect`/`bind` (syscall ctx). If a future timer/softirq retry path ever allocates a port, an IRQ mid-hold deadlocks. | Hang (latent) | Switch to `spin_lock_irq` for uniformity, or document the syscall-only invariant. |
| fs inode-number allocators use plain `spin_lock` | `kernel/fs/{ramfs:47,tmpfs:99,devfs:290/371/420,procfs:622,ext2:510,hugetlbfs:63}.c` | `ino_lock` is held very briefly to bump a counter; reached only via VFS/syscall create paths. Safe today; brittle if any inode is ever created from an IRQ (e.g. a future async create/notify). | Low / latent Hang | Prefer `spin_lock_irq`; these counters could also be `atomic_fetch_add`. |
| `sched_itimer_armed` non-atomic `++/--` | `kernel/sched.c:325-329` | Read in `sched_tick` (timer IRQ). Verified all three callers (`syscall_misc.c:315`, `syscall_proc.c:790`, `proc.c:1020`) hold `sched_lock` while calling, so it is serialized. | — (correct) | Keep the "caller holds sched_lock" contract documented. |
| signal pending bits set from both ctx | `kernel/signal.c` (all sites), `kernel/sched.c:570,574,613` | `sig_pending` mutated from kill syscalls and from `sched_tick`; **all** sites take `sched_lock`. | — (correct) | none |

---

## Class 3 — Type confusion via `->priv`

Status: **CLEAN.** Every syscall entry that casts `file->inode->priv` first verifies the
object kind via `f->inode->fops != &X_fops`; every `*_vfs` callback is only reachable
through that fops, so its cast is sound.

| Finding | Location | Guard | Severity |
|---|---|---|---|
| socket cast | `kernel/sys/syscall_net.c:7,…` | `fd_to_sock` checks `fops != &sock_fops` (`:357-359`) | — |
| eventfd / timerfd / inotify / signalfd | `kernel/sys/syscall_event.c` | helpers check `fops != &*_fops` (`:283-284,555-556,591-592,709-710`) | — |
| mqueue cast | `kernel/sys/syscall_ipc.c:585,…` | entries check `fops != &mq_fops` (`:753-754,763-764,772`) | — |
| epoll cast | `kernel/sys/syscall_io.c:212,…` | `epfd_get` checks `fops != &epoll_fops` (`:224`) | — |
| pipe / symlink target casts | `kernel/fs/pipe.c`, `kernel/fs/{ramfs,tmpfs,devfs}.c` | reached only via the inode's own fops/iops | — |

---

## Class 4 — Fixed-size stack buffers in deep/recursive paths

Status: **MOSTLY CLEAN.** Kernel stack is 16 KiB (`defconfig CONFIG_KSTACK_SIZE=16384`).
The historical `dir_path[256]` smash is fixed: the VFS resolver now heaps its `PATH_MAX`
work buffer precisely because overlayfs re-enters it. Remaining on-stack buffers are bounded
and shallow, but two carry notable stack pressure.

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| Resolver work buffer heap-backed | `kernel/vfs.c:251-252` | Correct mitigation; documents the overlay re-entry reason. | — | none |
| `tmp[256]` + `target[256]` co-resident per resolver frame | `kernel/vfs.c:276,385` | ~0.5 KiB locals; resolver re-enters via overlay lookup and symlink restart (depth ≤ 8). Bounded today, but two big arrays per frame plus overlay nesting is the closest remaining frame to the old bug. | Low (Crash if frame depth grows) | Consider sharing one scratch buffer or moving `target` to the heap alongside `work`. |
| `sendmsg`/`recvmsg` large stack frame | `kernel/sys/syscall_net.c:945,961-962,968` | `iov[64]` (~1 KiB) + `dbuf[SKB_DATA_MAX=1536]` + `cb[...]` ≈ 2.6 KiB in one frame; recvmsg similar. Within 16 KiB but the single largest syscall frame; a future SKB_DATA_MAX bump (it is Kconfig) narrows the margin. | Low (Crash at scale) | Cap or heap the data bounce buffer; assert `SKB_DATA_MAX` stays small. |
| `readlink` target buffers | `kernel/vfs.c:385` consumers; impls `ramfs.c:367`, `tmpfs.c:390`, `ext2.c:1283`, `procfs.c:710` | All impls NUL-terminate within `bufsize`, so `strlen(target)` after the call is safe. | — | none |
| overlay copy-up / mount buffers | `kernel/fs/overlay.c:110` (`buf[1024]`), `:361` (`lower/upper/work[192]`) | copy-up not recursive; mount runs once. Safe. | — | none |

---

## Class 5 — Refcount / UAF — **HIGHEST LIVE EXPOSURE**

Status: **REAL HAZARDS.** `fd_get` returns a **raw, unpinned** `vfs_file_t *`
(`kernel/vfs.c:891-899`); `fget` pins it atomically under the table lock
(`:916-924`). `vfs_read`/`vfs_write` were already converted to `fget`+`fput`
(`:1200-1227`). But `fd_get` is still used at ~45 sites, several of which **block** while
holding the raw pointer, and one increments the file refcount **non-atomically**. With x86
syscalls now interruptible and SMP live, a sibling thread's `close(fd)` can free the file
mid-syscall.

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| `flock` holds raw `fd_get` pointer across `yield()` loop | `kernel/sys/syscall_misc.c:360,365-371` | `f = fd_get(...)`, then `for(;;){ vfs_flock_try(f); yield(); }`. A sibling thread closing the same fd frees `f`; the next `vfs_flock_try(f,...)` is a UAF. Trigger: two threads share an fd table, one blocks on `LOCK_EX`, the other `close()`s. | **Crash / Corruption** | Use `fget`/`fput` around the wait loop (re-validate the fd each retry, or pin once). |
| SCM_RIGHTS non-atomic `vf->refcount++` on raw `fd_get` | `kernel/sys/syscall_net.c:964-966` | `vf = fd_get(...); vf->refcount++;` — plain increment, not under `t->lock`, racing `fput`'s `atomic_sub_fetch32`. Lost update → premature free (UAF) or permanent leak. Also `vf` itself is unpinned, so a concurrent close can free it before the `++`. Trigger: pass an fd over a unix socket while another thread closes it. | **Crash / Corruption / Leak** | Use `fget` (atomic pin under the lock) to collect the in-flight files; never plain `++`. |
| file-backed `mmap` uses raw `fd_get`, no `fput` | `kernel/sys/syscall_mm.c:194-230` | `f = fd_get(p->fds, fd)` then loops calling `f->inode->fops->mmap()` + `vm_map()` (may allocate / OOM-kill / block) without pinning `f`. A concurrent close frees `f` mid-loop. The cacheable branch pins the *inode* (`mmap_pinned`) but never the *file*. | **Crash** | `fget`/`fput` around the mapping loop. |
| `vfs_seek` derefs raw `fd_get` pointer | `kernel/vfs.c:1230-1251` | `seek` fop is non-blocking today, so low risk, but it is the same raw-pointer pattern; a future seek fop that allocates/blocks would be exposed. | Low (Crash latent) | Pin with `fget`/`fput` for consistency. |
| broad `fd_get` usage in fcntl/ioctl/fadvise/statx/xattr | `kernel/sys/syscall_fs.c:253,274,374,382`; `syscall_misc.c:81,253,388,754,899,1158` | Most operate without blocking, so benign **today**; the class-level risk is that any of these fops gains a blocking step and silently becomes a UAF. | Low–Med (latent) | Establish a rule: any path that touches `f->inode->fops->*` should `fget` first; reserve raw `fd_get` for "exists?" probes. |

---

## Class 6 — Integer overflow / missing bounds

Status: **MOSTLY CLEAN.** brk/mmap-anon guard the VA bump against wrap; iov is capped at
IOV_MAX and copied element-wise; cmsg length is clamped. Two huge-page rounding paths lack
the wrap guard the normal path has.

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| `MAP_HUGETLB` length rounding can wrap; no window-ceiling check | `kernel/sys/syscall_mm.c:86` (1 GB), `:120` (2 MB) | `len = (len + HUGE_SIZE - 1) & MASK`: a `len` within `HUGE_SIZE-1` of `SIZE_MAX` wraps to a tiny value; and unlike the 4 KiB path (`:160-162` checks `p->mmap_next + len < p->mmap_next` and the stack ceiling), the huge paths bump `huge1g_next`/`mmap_next` with no overflow/ceiling guard, so a crafted `len` can place the region over the stack or wrap the cursor. Requires `MAP_HUGETLB` (privileged-ish but reachable). | Corruption (latent) | Reject `len > SIZE_MAX - HUGE_SIZE` before rounding; apply the same ceiling/`+len < cursor` guard as the normal path. |
| brk page-round wrap | `kernel/sys/syscall_mm.c:36` | `(addr + PAGE_SIZE - 1)` could wrap, but `access_ok(addr,1)` at `:34` already rejects kernel-range/near-top addresses first. | — (guarded) | none |
| iov / cmsg arithmetic | `kernel/sys/syscall_io.c:484,525`; `syscall_net.c:944,956-963` | `cnt`/`iovlen` capped (1024 / 64); `clen`/`cmsg_len`/`nfd` all clamped to buffer/`SKB_SCM_MAX`. | — (correct) | none |

---

## Class 7 — Unchecked user pointers

Status: **CLEAN.** `copy_from_user`/`copy_to_user`/`access_ok` are used pervasively
(~245 call sites across `kernel/sys/`), including struct-by-struct copies of `msghdr`,
`iovec`, `itimerval`, `statx`, etc. No direct user-pointer deref without a copy was found in
the syscall layer. `MAP_FIXED` validates the target with `access_ok` before writing PTEs
(`syscall_mm.c:74`).

---

## Class 8 — Improvable / future-fragile

| Finding | Location | Hazard + trigger | Severity | Fix direction |
|---|---|---|---|---|
| `virtio_blk` uses x86 `pause` but is compiled for **aarch64** too | `drivers/block/virtio_blk.c:159,221`; `Makefile:86` (aarch64 `SRCS_DRIVERS`) | `pause` is an x86 mnemonic; on aarch64 it is at best a no-op alias and semantically wrong (should be `yield`/`isb`). Either it silently miscompiles or wastes the spin hint on Pi/virt-arm. A portability landmine; the spin also has no `cpu_relax()` abstraction. | Perf / Portability (Crash if assembler rejects it) | Introduce `cpu_relax()` in `hal.h` (`pause` on x86, `yield`/`isb` on aarch64) and use it in all driver spin loops. |
| Raw `fd_get` as the default fd-fetch idiom | whole `kernel/sys/` layer | 45 raw sites vs 9 pinned (`fget`) — the safe pattern is the minority. Every new blocking fop is a future UAF (see Class 5). | Med (latent) | Make `fget`/`fput` the default; lint/grep-gate new raw `fd_get` next to `fops->*`. |
| Namespace refcounts use plain `++` | `cgroup.c:112`, `user_ns.c:24`, `pid.c:34/114`, `nsproxy.c:36/77`, `net.c:72`, `syscall_ipc.c:809`, `vfs.c:490` | All currently under their subsystem lock (comments assert "lock held"), but the increment is not self-defending; a future lockless caller silently races. | Low (latent) | Either keep the lock invariant documented at the field, or switch to `atomic_fetch_add`. |
| O(n) full-table scans on shared hot paths | `proc.c:838-852` (`oom_select_victim` scans `proctab` under `sched_lock`), `syscall_net.c:393` (`unix_reg[UNIX_REG_MAX]` linear scan) | Fine at current process/socket counts; under load these hold a global lock across a full scan. | Perf (at scale) | Acceptable now; revisit if proc/socket counts grow. |
| `MSGQ`/`MQ` message bounce buffers on stack | `syscall_ipc.c:453,513,599,624` (`tmp[MSGQ_MSGSZ]`/`tmp[MQ_MSGSIZE]`) | On-stack; safe at current sizes but tied to Kconfig-tunable limits. | Low | Assert the size constants stay small, or heap them. |

---

## Top 15 to fix before they blow up (crash/corruption first)

1. **`flock` UAF** — raw `fd_get` pointer held across `yield()` loop. `syscall_misc.c:360-371`. *(Crash/Corruption — directly reachable with two threads sharing an fd.)*
2. **SCM_RIGHTS non-atomic `vf->refcount++` on unpinned file** — `syscall_net.c:964-966`. *(Crash/Corruption/Leak — fd-passing + concurrent close.)*
3. **file-backed `mmap` uses raw `fd_get`, no pin/`fput`** — `syscall_mm.c:194-230`. *(Crash — concurrent close during the map loop.)*
4. **`MAP_HUGETLB` length-rounding wrap + missing VA-ceiling guard** — `syscall_mm.c:86,120`. *(Corruption — crafted huge `len`.)*
5. **virtio_blk x86 `pause` linked into the aarch64 kernel** — `virtio_blk.c:159,221`. *(Portability/Crash — wrong/illegal instruction on ARM; add `cpu_relax()`.)*
6. **`vfs_seek` raw `fd_get` deref** — `vfs.c:1230`. *(Latent crash — becomes live the moment a seek fop blocks.)*
7. **Adopt `fget`/`fput` as the default fd idiom** across `kernel/sys/`; reserve raw `fd_get` for existence probes. *(Eliminates the whole Class-5 footgun.)*
8. **`tcp.c ephemeral_lock` → `spin_lock_irq`** — `tcp.c:474`. *(Latent hang post-flip; cheap.)*
9. **fs `ino_lock` plain locks → `spin_lock_irq` or atomics** — ramfs/tmpfs/devfs/procfs/ext2/hugetlbfs. *(Latent hang; uniformity.)*
10. **Bound `sendmsg`/`recvmsg` stack frame** (`iov[64]`+`dbuf[1536]`) — `syscall_net.c:945,961`. *(Crash at scale; cap or heap the bounce buffer.)*
11. **Share/heap the resolver `target[256]`** to keep the overlay-re-entrant frame small — `vfs.c:385`. *(Crash if frame depth grows.)*
12. **Namespace refcount `++` → atomic or documented-lock** — multiple. *(Latent corruption if a lockless caller appears.)*
13. **Introduce a single `cpu_relax()` HAL primitive** and route all driver spin loops through it. *(Portability/perf hygiene; prerequisite for #5.)*
14. **Document/enforce the "caller holds sched_lock" contract on `sched_itimer_account` / `sched_itimer_armed`** — `sched.c:325`. *(Prevents a future unlocked caller.)*
15. **Revisit O(n) `oom_select_victim` / `unix_reg` scans** under a global lock — `proc.c:838`, `syscall_net.c:393`. *(Perf at scale.)*
