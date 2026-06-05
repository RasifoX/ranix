# Ranix ↔ Linux Parity Audit

**Date:** 2026-06-05
**Scope:** Every way Ranix diverges from Linux — feature/UX gaps (A) and structural/architectural divergences (B), across both arches (x86_64 + aarch64).
**Method:** Pure read-only source review (no build, no QEMU). Every finding cites `path:line` in actual Ranix code; Linux behavior stated for contrast. Severities: **Correctness** (wrong/unsafe result), **Perf** (works but ceiling), **Feature** (missing capability), **Cosmetic**. Effort: S/M/L/XL.

**Stale memory corrected:** project memory says "ASID yok (full TLB flush)" and "active high-order buddy corruption bug #48". As of this tree: aarch64 **has** an 8-bit ASID allocator (`arch/aarch64/vm.c:49-126`), and the buddy allocator has poison + double-free guards and scales `mem_map[]` to total RAM (`kernel/mm/buddy.c:338-365`) — no corruption found on review. The remaining TLB gap is **x86-only** (see MM-2).

---

## 1. Memory Management

| # | Title | Linux | Ranix (file:line) | Gap / Divergence | Sev | Eff |
|---|-------|-------|-------------------|------------------|-----|-----|
| MM-1 | **No unified page cache** | All file I/O flows through `address_space`/page cache; mmap, read(), write() share the same cached pages; clean pages are reclaimable. | `kernel/sys/syscall_mm.c:211-223` file-backed mmap `buddy_alloc`s a **private anonymous page per page** and `read()`s file contents into it. ext2 has only a small per-fs LRU **block** cache (`kernel/fs/ext2.c:183`). | No shared page cache; read+mmap of same file get independent copies; no cache coherence between processes; double memory for mmap'd files. | Correctness/Perf | XL |
| MM-2 | **No x86 TLB shootdown (cross-CPU)** | `flush_tlb_mm`/`flush_tlb_page` send IPIs to all CPUs sharing the mm so a unmap/protect is globally visible. | x86 `vm_unmap`/`vm_map` issue only a local `invlpg` (`arch/x86_64/vm.c:202,262,272,277` etc.); grep finds **no** shootdown/IPI anywhere. `arch_vm_switch` notes "PCID is later" (`:320`). | A munmap/mprotect/COW write on a multithreaded process running on >1 CPU leaves **stale, writable TLB entries on the other CPUs** → use-after-free / stale-data reads. aarch64 is fine (broadcast `tlbi ...is`, `arch/aarch64/vm.c:214,238,276`). | **Correctness** | L |
| MM-3 | **No swap / no reclaim** | kswapd + direct reclaim evict clean file pages and swap anon pages under pressure. | Only response to OOM is the OOM-killer (`kernel/mm/oom.c`). No `kswapd`, no writeback, no `MADV_DONTNEED` reclaim on `MAP_SHARED` (`syscall_mm.c:416` comment "We lack reclaim"). | Memory pressure SIGKILLs processes instead of reclaiming; no swapfile/swapon. | Feature | XL |
| MM-4 | **MAP_SHARED file mmap has no writeback** | Stores to a `MAP_SHARED` file mapping are written back to the backing file (msync/page cache). | File mmap copies file into a private page (`syscall_mm.c:214-223`); `VMA_SHARED` flag is recorded (`:101,138,177`) but there is no page-cache linkage, so stores never reach disk; `sys_msync` (`:548`) has nothing to flush. | `MAP_SHARED` of a real file silently behaves like `MAP_PRIVATE` for writeback → data loss expectation mismatch (mmap'd databases, dbm, etc.). | **Correctness** | L |
| MM-5 | **No NUMA / no zones** | Buddy has zones (DMA/DMA32/Normal/Movable) + per-node free lists; `mbind`/`set_mempolicy`. | Single flat buddy region, `_base/_npages` (`kernel/mm/buddy.c:22-24`); no zone concept. | No DMA-zone-constrained allocation guarantee for legacy 32-bit-DMA devices; no NUMA locality. Acceptable for current targets but a ceiling. | Perf/Feature | L |
| MM-6 | **Buddy max order 14 (32 MB cap)** | `MAX_ORDER` 11 default but CMA/giant pages handled separately. | `BUDDY_MAX_ORDER=14` → 32 MB max single block (`include/kernel/mm/buddy.h:6`). Single contiguous alloc capped; large DMA buffers fail. | Mostly fine; no CMA for big contiguous device buffers. | Perf | M |
| MM-7 | **Single global buddy lock + per-CPU magazine only for order-0** | Per-zone, per-order locks + per-CPU pcp lists for small orders. | One `buddy_lock` (`buddy.c:25`); per-CPU magazine fast path only for **order 0** (`:183-313`); every order ≥1 alloc/free contends the global lock. | Page-table / higher-order allocs serialize globally under heavy fork/exec on many CPUs. | Perf | M |
| MM-8 | **No transparent huge page daemon (khugepaged)** | khugepaged collapses anon ranges to THP in the background. | THP collapse only happens inline in fault/map path (`arch/x86_64/vm.c:222` "THP collapse" on explicit huge map); no background collapse, no `/sys/kernel/mm/transparent_hugepage`. | Anon memory never opportunistically promoted. | Feature | L |
| MM-9 | **OOM victim selection is footprint-only** | `oom_badness` weights `oom_score_adj`, RSS, swap; respects cgroup limits, `OOM_SCORE_ADJ_MIN` immunity. | Largest-`mem_charged` process chosen (`kernel/mm/oom.c` header + proc.c selection); no `oom_score_adj`. | No `/proc/<pid>/oom_score_adj` tuning; can kill the wrong (critical) process. | Feature | M |

---

## 2. Scheduler

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| SCH-1 | **Single global `sched_lock`** | Per-rq locks; per-CPU runqueues lock independently; cross-CPU ops use double-rq-lock ordering. | One `sched_lock` guards all runqueues, state, proctab, pid alloc, signals, context switch (`kernel/sched.c:108-156`, lock at `:156`). Per-CPU rq lists exist but every op takes the one lock. | Hard SMP scalability ceiling; every wakeup/enqueue/signal/switch serializes globally. The split design is documented but deliberately unshipped. | Perf | XL |
| SCH-2 | **No CFS/EEVDF — round-robin with nice-scaled quantum** | EEVDF (6.6+) / CFS: `vruntime`, weighted fair sharing, latency targets. | Plain FIFO runqueue, RT tasks pushed to head (`sched.c:344-351`); quantum = `base − nice*base/40`, scaled by cgroup weight (`:711-716`). No vruntime, no fairness accounting, no latency target. | Not fair under mixed load; nice is a crude slice multiplier, not proportional CPU share; interactive latency not guaranteed. | Perf/Feature | XL |
| SCH-3 | **Load balancing is naive pick-least-loaded + steal-on-idle** | Periodic `load_balance` over sched domains, NUMA-aware, imbalance/migration cost. | `pick_cpu` scans all rqs counting length each enqueue (`sched.c:386-399`); `runqueue_pop` work-steals the longest rq when idle (`:418-446`). O(ncpus) walk per enqueue under the global lock; no domains, no cache/NUMA affinity in balancing. | Migration thrash, no topology awareness; O(n) under the global lock. | Perf | L |
| SCH-4 | **No deadline scheduler (SCHED_DEADLINE)** | `SCHED_DEADLINE` (EDF+CBS). | Only SCHED_NORMAL + FIFO/RR head-insertion (`sched.c:344`); `sched_setattr`/SCHED_DEADLINE absent from dispatch (`kernel/syscall.c:436-441` only set/getscheduler/param). | No EDF real-time class. | Feature | L |
| SCH-5 | **RT class has no priority levels / no preemption of RT by higher RT** | 100 RT priorities, FIFO/RR per priority, RT throttling. | RT tasks merely inserted at rq head (`sched.c:344-347`); a single FIFO list, no per-priority ordering among RT tasks, no `rt_runtime`/throttling. | `sched_priority` not honored between two RT tasks; runaway RT can starve everything (no throttle). | Correctness/Feature | M |
| SCH-6 | **Fixed 100 Hz tick; no high-res timers / NOHZ** | hrtimers (ns resolution), tickless idle (NOHZ), per-CPU clockevents. | 100 Hz tick assumed throughout (`sched.c:190,231`); quantum/itimers/nanosleep are tick-quantized. No hrtimer subsystem. | `nanosleep`/`clock_nanosleep`/timerfd granularity ≈ 10 ms; no sub-tick timers; power: always-on tick. | Perf/Feature | L |

---

## 3. SMP / Per-CPU

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| SMP-1 | **MAX_CPUS static (256), static per-CPU arrays** | `cpumask`/`percpu` dynamically sized to possible CPUs; nr_cpu_ids. | `MAX_CPUS = CONFIG_MAX_CPUS = 256` (`include/kernel/smp.h:14`, `defconfig:11`); static `last_seen[MAX_CPUS]`, `frozen[MAX_CPUS]` in watchdogs (`sched.c:185-186,264-265`); `all_cpus[]` fixed. | Wastes memory at small core counts, hard cap at large; no true cpumask/percpu allocator. | Perf | M |
| SMP-2 | **x86 secondaries have no per-CPU timer** | LAPIC timer per CPU; per-CPU clockevents. | Comment: "x86 secondaries lack a per-CPU timer" — idle CPUs only woken by IPI (`sched.c:203-204`). aarch64 uses per-CPU local timer (`tick_notify_local`). | x86 idle-CPU wakeup latency depends on IPI; uneven tick accounting between arches. | Perf | M |
| SMP-3 | **No CPU hotplug** | `cpu_up`/`cpu_down`, hotplug state machine. | `c->online` flag exists (`sched.c:201`) but no offline path / `/sys/devices/system/cpu/cpuN/online` write handling found. | Can't offline a CPU at runtime. | Feature | M |

---

## 4. VFS

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| VFS-1 | **dcache is a linked list under a global IRQ-off spinlock** | RCU-protected hashed dcache, per-dentry seqlock, lockless `__d_lookup_rcu` path walk. | `vfs_dentry_resolve` holds one global `vfs_lock` with IRQs off across the **whole** path walk including `iops->lookup` (`kernel/vfs.c:300-395`); children are a `->next` linked list compared with `strcmp` (`:312-316`). | Path lookup serializes globally and is O(children) per component; `iops->lookup` (can do block I/O) runs under an IRQ-off spinlock → potentially long IRQ-off holds + no concurrency. | Perf/Correctness | XL |
| VFS-2 | **dentry cache count-capped, no LRU eviction** | dcache shrinks under memory pressure (shrinker). | `dentry_count < dentry_max_val` gate; past the cap, lookups go "volatile" (uncached) each time (`vfs.c:310,328-338`). No reclaim of cold dentries. | Hot working set beyond the cap loses all caching; no shrinker integration. | Perf | M |
| VFS-3 | **No inode cache / writeback list** | Global inode cache, dirty inode writeback, `sync` walks superblocks. | inodes are owned by the dentry tree or freed immediately (`vfs.c:142`); `sys_sync` exists but there's no dirty-inode list to flush. | `sync`/`fsync` correctness depends entirely on FS driver; no VFS-level dirty tracking. | Correctness | L |
| VFS-4 | **No fanotify, limited inotify** | inotify + fanotify (mount/sb-wide, permission events). | inotify present (`syscall.c:631-635`); no fanotify, no mark on mount/superblock. | Antivirus/backup-class watchers unsupported. | Feature | L |

---

## 5. Filesystems

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| FS-1 | **ext2 only — no ext4/journaling** | ext4 with journal (ordered/writeback), extents, htree dirs. | `kernel/fs/ext2.c` is classic ext2 (no journal, no extents, linear dir blocks). FAT/iso9660/overlay/tmpfs/ramfs also present. | No crash-consistency journal; power loss can corrupt the fs (no fsck either). | Correctness/Feature | XL |
| FS-2 | **No xattr storage backend for most fs** | xattrs persisted per-fs; ACLs via `system.posix_acl_*`. | xattr syscalls dispatch (`syscall.c:660-667`) but persistence is per-fs; ext2 driver review shows no xattr block support → likely ENOTSUP/RAM-only. | POSIX ACLs / SELinux labels / capabilities-on-files don't persist. | Feature | L |
| FS-3 | **No quota, no project quota** | `quotactl`, disk quotas. | No `quotactl` in dispatch (`kernel/syscall.c`). | No per-user disk limits. | Feature | M |
| FS-4 | **ext2 block cache is small fixed LRU, not page cache** | Buffer/page cache scales with RAM. | `EXT2_BCACHE_N` fixed entries per-fs (`ext2.c:183-249`). | Large working sets thrash; see MM-1. | Perf | M |
| FS-5 | **No fscrypt / no loop devices / no device-mapper** | dm-crypt, loop, LVM, dm-verity. | None present. | No encrypted fs, no loopback mounts, no LVM. | Feature | XL |

---

## 6. Block Layer

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| BLK-1 | **No bio / request queue / elevator (blk-mq)** | blk-mq multi-queue, I/O schedulers (mq-deadline/bfd/kyber), request merging, plugging. | `block.c` is synchronous `dev->read/write` under a per-device spinlock (`kernel/block.c:10-27`); no queue, no merge, no async, no scheduler. | No I/O reordering/merging/fairness; every request serialized per device; no async submission (io_uring path still funnels here). | Perf | L |
| BLK-2 | **Byte device I/O bounces one 512 B sector at a time** | Page-cache-backed, multi-block. | `blkdev_read/write` loop a single `sect[BLOCK_SIZE]` buffer per sector (`block.c:36-88`); writes do read-modify-write per partial sector. | `dd`/raw disk access extremely slow; no batching. | Perf | M |
| BLK-3 | **No I/O accounting beyond read counters** | `/proc/diskstats`, ioprio, cgroup io controller. | `nr_reads`/`nr_read_secs` only (`block.c:15,100`); no write counters, no diskstats, no `ioprio_set`, no `io.max` cgroup. | No I/O observability/limits. | Feature | M |

---

## 7. Signals & Job Control

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| SIG-1 | **Signal state is a 64-bit pending bitmask — no queue, no RT signal queueing** | Per-process + per-thread sigqueue; RT signals (SIGRTMIN..SIGRTMAX) queue multiple instances each with siginfo. | `sig_pending |= (1UL<<(sig-1))` (`kernel/signal.c:18,50,65`); a second identical signal before delivery is coalesced. | RT signals can't queue; only 64 signals representable; `sigqueue` payload (`si_value`) not preserved per instance. | Correctness/Feature | L |
| SIG-2 | **No `rt_sigqueueinfo` / `rt_tgsigqueueinfo`** | Present; lets userspace send siginfo + value. | Absent from dispatch (`kernel/syscall.c`); grep finds no `sigqueue`. | `sigqueue(3)` (glibc/musl) fails → POSIX RT-signal IPC broken. | Feature | M |
| SIG-3 | **No SIGTTOU/SIGTTIN background-job stop** | Background pgrp reading/writing a ctty gets SIGTTIN/SIGTTOU and stops. | tty delivers SIGINT/SIGQUIT/SIGTSTP to fg pgrp (`tty_core.c:95-101`); pty has a partial SIGTTIN note (`fs/pty.c:322`) but no enforced background read/write block on the console tty. | Background jobs aren't stopped on terminal access → shell job-control semantics incomplete. | Correctness | M |
| SIG-4 | **No SIGHUP on hangup / no TIOCNOTTY** | Closing controlling terminal / session leader exit sends SIGHUP to the fg pgrp; `TIOCNOTTY` detaches. | SIGHUP is defined (`signal.c:83`) but grep shows no hangup generation; no `TIOCNOTTY`. (Memory note "controlling-tty hangup missing" confirmed.) | Daemons relying on SIGHUP-on-disconnect, ssh-style hangup, won't get it. | Correctness/Feature | M |
| SIG-5 | **siginfo richness for synchronous signals** | SIGSEGV/SIGBUS carry `si_addr`/`si_code`; SIGCHLD carries `si_pid/si_status/si_code`. | Frame build in `signal.c:224-246` — need to verify si_addr/si_code population for faults; bitmask model (SIG-1) limits per-instance info. | Handlers using `sa_sigaction` siginfo may see incomplete fields. | Correctness | M |

---

## 8. Time / Timers

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| TMR-1 | **No hrtimer (see SCH-6)** | ns-resolution hrtimers back nanosleep/timerfd/posix-timers. | Tick-quantized at 100 Hz. | ≥10 ms granularity. | Perf | L |
| TMR-2 | **No POSIX per-process timers (`timer_create`)** | `timer_create`/`timer_settime` with SIGEV. | Not in dispatch (`kernel/syscall.c`); only itimer (`setitimer`) + timerfd. | `timer_create(2)` ENOSYS. | Feature | M |
| TMR-3 | **ITIMER_PROF/VIRTUAL tick-charged, coarse** | High-res CPU-time charging. | itimer charged per tick in `itimer_charge_cpu` (`sched.c:164,247`). | Profiling timers coarse (10 ms). | Perf | S |

---

## 9. IPC

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| IPC-1 | SysV + POSIX IPC mostly present | shm/sem/msg + mq. | Full SysV (`syscall.c:670-680`) + POSIX mq (`:682-686`). **Largely Linux-equivalent.** | Verify limits (`/proc/sys/kernel/shmmax`, semmni) and `IPC_INFO`. | Feature | S |
| IPC-2 | **No `pipe` size tuning / packetized pipes** | `F_SETPIPE_SZ`, `O_DIRECT` packet mode. | `kernel/fs/pipe.c` fixed buffer; check `fcntl` F_SETPIPE_SZ. | `F_SETPIPE_SZ` likely ignored. | Feature | S |

---

## 10. Network Stack

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| NET-1 | **No routing table — single default device** | FIB/routing table, multiple routes, metrics, policy routing, `ip route`. | `net_route_dev`/`net_route_dev6` **always return the default device** (`kernel/net/net.c:314-323`). | Cannot reach hosts off the default path; no multi-subnet, no per-destination gateway selection. | Feature | L |
| NET-2 | **No AF_NETLINK / rtnetlink** | netlink sockets drive `ip`, NetworkManager, etc. | grep: no `AF_NETLINK`/rtnetlink anywhere. | `ip`/`ss`/`iproute2` can't configure the stack; tooling falls back or fails. | Feature | XL |
| NET-3 | **No IP forwarding / no bridge / no veth** | `ip_forward`, bridges, veth pairs (containers!). | grep: no forward/bridge/veth. (Containers exist via namespaces but lack veth networking.) | Container networking (CNI/veth/bridge) impossible; no router role. | Feature | XL |
| NET-4 | **netfilter present but no nftables / limited** | iptables + nftables (nf_tables). | `kernel/net/netfilter.c` exists (315 lines) — hook-based; no `nf_tables`/`NFNETLINK`. | `nft` unsupported; iptables-legacy depends on the netfilter ABI present. | Feature | L |
| NET-5 | **DHCP client only (no server, no IPv6 PD beyond RA)** | dhclient/systemd-networkd; DHCPv6. | Built-in DHCP client (`kernel/net/dhcp.c`, `net.c:392`). | Fine for a leaf node; no static-route/multi-NIC config story. | Feature | M |
| NET-6 | **TCP: verify congestion control & SACK** | Pluggable CC (cubic/bbr), SACK, window scaling, TSO/GSO. | `kernel/net/tcp.c` (1161 lines) — single CC algorithm, check SACK/window-scaling/timestamps options. | Likely no pluggable CC, possibly no SACK → poor throughput on lossy/high-BDP links. | Perf | L |
| NET-7 | **No GSO/GRO/checksum offload abstraction** | Per-NIC offload features negotiated. | Drivers (`virtio_net`, `bcmgenet`) likely do per-packet; no offload feature flags. | CPU-bound at high pps. | Perf | M |

---

## 11. TTY / PTS / Console

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| TTY-1 | **No real virtual terminals / VT switching** | tty1..N are independent VTs; Ctrl+Alt+F1..F6, `chvt`, `VT_ACTIVATE`/`VT_GETSTATE`, `KDSETMODE` for graphics handoff. | `/dev/tty1..tty6` are **aliases** sharing one physical console line discipline (`kernel/tty/tty_core.c:388-395`, explicit comment "nodes exist for ABI compatibility rather than independent VTs"). No `VT_*` ioctls, no Ctrl+Alt+Fn. | Can't switch consoles; multi-getty/multi-session-on-VT, X-VT handoff impossible. | Feature | L |
| TTY-2 | **Line discipline mostly complete** | N_TTY full. | ISIG/ICANON/ECHO/ECHOE/ECHOK/ECHOCTL/IEXTEN, VINTR/VQUIT/VSUSP→signals, erase handling (`tty_core.c:13-159`), TIOCGPGRP/SPGRP/SCTTY (`:257-321`). **Close to Linux for the supported subset.** | Verify IXON/IXOFF flow control, IUTF8, c_cc completeness, TCSETS vs TCSETSW timing. | Feature | M |
| TTY-3 | **Background read/write job control (see SIG-3)** | — | fg_pgrp tracked but no SIGTTIN/SIGTTOU enforcement on the console. | Job control incomplete. | Correctness | M |
| TTY-4 | **No framebuffer VT console with scrollback / multiple consoles** | fbcon with multiple VTs, scrollback. | `kernel/tty/fb_tty.c` single fb console (font8x16/spleen). | Single console surface. | Feature | M |

---

## 12. USB / Input / HID

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| USB-1 | **USB stack = xHCI bulk-only mass storage only** | Full USB core: hubs, HID, CDC-ACM, serial converters, audio, etc. | `drivers/usb/xhci.c` header: "xHCI + enumeration + bulk-only mass-storage. Polling." (`:10-12`). No class drivers beyond BOT mass storage. | No USB keyboard, no USB-serial, no hub topology, no interrupt endpoints. | Feature | XL |
| USB-2 | **No USB HID keyboard — critical for Pi (no PS/2)** | usbhid + hid-generic. | Input is **PS/2 only** (`drivers/input/ps2.c`); no USB HID. The user has no keyboard on Pi → cannot get a keyboard at all on real hardware. | Pi4 interactive console blocked on input. | **Feature** | L |
| USB-3 | **No USB-serial (CDC-ACM / FTDI / pl2303)** | cdc_acm, ftdi_sio, pl2303 drivers. | None (only 16550 hardware UART `drivers/serial/uart16550`). | TTL USB-serial adapters unusable. | Feature | L |
| USB-4 | **No USB hotplug / no interrupt-driven xHCI** | Hotplug via port-change events + udev. | xHCI is **polling** (`xhci.c:10`); hotplug mount story for USB MSC likely absent. | No hot insert/remove handling. | Feature | M |

---

## 13. Raspberry Pi / Hardware (aarch64 bcm2711)

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| PI-1 | SD card boot + persistence | mmc/sdhci + ext4 root. | `drivers/block/sdhost_pi4.c` exists; `pi4-dist`/`pi4-sd`/`rpi-boot` present. Persistence proven in memory. **Likely working.** | Verify SD R/W persistence end-to-end on real HW; depends on FS-1 (no journal → power-loss risk). | Feature | M |
| PI-2 | HDMI/framebuffer console | vc4/simplefb fbcon. | `drivers/video/fbdev.c` + `fb_tty.c`. **Present.** | Single VT (TTY-1/TTY-4). | Feature | M |
| PI-3 | GPIO / I2C / SPI / PWM | gpiochip, i2c-dev, spidev. | grep none — no GPIO/I2C/SPI char devices found. | No `/dev/gpiochip*`, sensors/peripherals unusable. | Feature | L |
| PI-4 | No keyboard input on Pi | usbhid. | See USB-2 — blocks interactive use without UART/SSH. | Core UX blocker for the stated Pi goal. | **Feature** | L |
| PI-5 | bcmgenet Ethernet | genet driver. | `drivers/net/bcmgenet.c` present. **Likely working.** | Verify with NET-1 routing limits. | Feature | S |

---

## 14. Syscall ABI Surface

| # | Title | Linux | Ranix (file:line) | Gap | Sev | Eff |
|---|-------|-------|-------------------|-----|-----|-----|
| ABI-1 | **`membarrier` is a no-op, `rseq` returns EINVAL** | Real membarrier (global/private expedited); rseq for per-CPU critical sections. | `membarrier → 0` (no-op), `rseq → -22` (`kernel/syscall.c:445-446`). | membarrier returning success without doing the barrier is a **correctness lie** for libraries (e.g. liburcu, jemalloc fast paths) that rely on it; rseq absence is benign (glibc falls back). | **Correctness** | M |
| ABI-2 | **`fadvise64` ignored** | Honors WILLNEED/DONTNEED/SEQUENTIAL. | `fadvise64 → 0` no-op (`syscall.c:531`). | Hint-only; benign but no readahead control. | Cosmetic | S |
| ABI-3 | Missing: `timer_create`, `quotactl`, `rt_sigqueueinfo`, `sched_setattr`, `kexec`, `bpf`, `perf_event_open`, `userfaultfd`, `landlock`, `pidfd_getfd`, `clock_adjtime` variants, `name_to_handle_at`/`open_by_handle_at`, `fanotify_*`, `io_pgetevents` | All present in Linux. | None in dispatch (`kernel/syscall.c:400-686`). | Tooling/perf/sandboxing gaps; many libs probe-and-fallback so mostly Feature-level except where assumed present. | Feature | varies |
| ABI-4 | **`clone3`/`clone` only support a prefix of struct/flags** | Full clone_args, CLONE_NEWTIME, CLONE_INTO_CGROUP, set_tid array, CLONE_CLEAR_SIGHAND, CLONE_PIDFD. | Fixed 8-field `clone_args` prefix (`syscall.c:502`); pidfd-at-clone, CLONE_INTO_CGROUP, CLONE_NEWTIME unhandled. | Newer container runtimes (CLONE_INTO_CGROUP, CLONE_PIDFD) unsupported. | Feature | M |
| ABI-5 | **unimplemented syscall logs + returns ENOSYS** | ENOSYS silently. | Prints `kprintf("syscall: unimplemented %ld")` (`syscall.c:688`). | Noisy but harmless; could leak probing pattern. | Cosmetic | S |

---

## 15. Misc / Observability

| # | Title | Linux | Ranix | Gap | Sev | Eff |
|---|-------|-------|-------|-----|-----|-----|
| OBS-1 | No `perf_event_open` / no tracing (ftrace/eBPF) | Rich. | None. | No profiling/tracing. | Feature | XL |
| OBS-2 | `/proc` & `/sys` partial | Vast. | Large procfs (`kernel/fs/procfs.c` 2189 lines) + sysfs (655). Verify `/proc/sys/*` writability, `/proc/<pid>/maps,smaps,status,stat`, `/proc/diskstats`, `/proc/interrupts`. | Probably missing diskstats, smaps_rollout, pressure (PSI), cgroup v2 files. | Feature | L |
| OBS-3 | No PSI (pressure stall info) | `/proc/pressure/*`. | None. | No memory/io/cpu pressure metrics. | Feature | M |
| OBS-4 | No `reboot(2)` flag coverage / kexec | LINUX_REBOOT_CMD_*. | Verify reboot/poweroff path + ACPI/PSCI. | Power management completeness unknown. | Feature | M |
| OBS-5 | No loadable BPF/seccomp-bpf JIT | cBPF/eBPF. | `seccomp` syscall present (`syscall.c:498`) — verify it's filter-mode or stub. | seccomp filters may be unenforced. | Correctness | M |

---

## Top 20 Foundation-Hardening Priorities (ranked)

**Correctness bugs first (silent wrong behavior / data loss / unsafe):**

1. **MM-2 — x86 cross-CPU TLB shootdown missing.** Multithreaded munmap/mprotect/COW on >1 CPU leaves stale writable TLBs on other CPUs → UAF/stale-data. aarch64 already correct; this is the single most serious correctness divergence. (L)
2. **MM-4 — MAP_SHARED file mmap has no writeback.** Stores never reach disk; behaves silently like MAP_PRIVATE. Data-loss surprise for any mmap-writer. (L)
3. **ABI-1 — `membarrier` returns success without doing the barrier.** A correctness lie to userspace memory-ordering libraries. Either implement (IPI-based, ties to MM-2) or return ENOSYS. (M)
4. **SIG-1/SIG-2 — RT signals can't queue; no `rt_sigqueueinfo`.** Coalescing + missing siginfo breaks POSIX RT-signal IPC (`sigqueue`). (L–M)
5. **SIG-3/SIG-4 — Job control: no SIGTTIN/SIGTTOU stop, no SIGHUP-on-hangup.** Breaks shell job control and daemon hangup semantics. (M)

**Then structural ceilings (works, but caps correctness-under-scale / perf):**

6. **MM-1 — No unified page cache.** Underpins FS perf, mmap coherence, and reclaim; the biggest structural debt. (XL)
7. **MM-3 — No swap / no reclaim.** OOM-kill is the only pressure response. (XL)
8. **VFS-1 — dcache linked-list under one IRQ-off global lock across lookup().** Perf ceiling + long IRQ-off holds during block I/O. (XL)
9. **SCH-1 — Single global sched_lock.** Documented split design; the SMP scalability ceiling. (XL)
10. **SCH-2 — Round-robin instead of CFS/EEVDF.** No fairness/latency guarantees. (XL)
11. **BLK-1/BLK-2 — No bio/request queue/elevator; byte I/O bounces per sector.** Serializes and slows all disk I/O. (L–M)
12. **FS-1 — ext2 only, no journaling.** Power-loss can corrupt the root fs; no fsck. (XL)
13. **SCH-6/TMR-1 — Fixed 100 Hz, no hrtimers/NOHZ.** 10 ms timer granularity ceiling + always-on tick. (L)
14. **SMP-1 — MAX_CPUS=256 static, no cpumask/percpu allocator.** (M)
15. **SCH-5 — RT class has no priority levels / no throttling.** sched_priority unhonored; runaway RT starves system. (M)

**Then features (capability gaps, ranked by user goals: Pi + container parity):**

16. **USB-2/PI-4 — No USB HID keyboard.** Hard blocker for interactive use on real Pi hardware (no PS/2 there). (L)
17. **TTY-1 — No real virtual terminals / VT switching (Ctrl+Alt+Fn, chvt, VT_*).** A signature Linux console UX. (L)
18. **NET-2/NET-3/NET-1 — No netlink, no routing table, no veth/bridge.** Blocks iproute2 tooling and container networking. (XL)
19. **USB-1/USB-3 — USB stack is mass-storage-only; no CDC-ACM/FTDI/pl2303 USB-serial, no hubs/hotplug.** (XL)
20. **ABI-3/ABI-4 — Missing modern syscalls** (timer_create, fanotify, perf_event_open, userfaultfd, CLONE_INTO_CGROUP/PIDFD) that newer userspace and container runtimes increasingly assume. (varies)

---

### Things checked and found already Linux-equivalent (don't spend effort)

- **aarch64 ASID + broadcast TLB invalidation** — proper 8-bit ASID allocator, non-global user PTEs, `tlbi ...is` on all unmap/protect paths (`arch/aarch64/vm.c:49-126,214-398`). Correct.
- **Buddy allocator scaling + safety** — `mem_map[]` sized to total RAM, per-CPU order-0 magazine, poison-on-free + double-free guards (`kernel/mm/buddy.c`). No corruption found; memory's "#48" note appears resolved.
- **Futex** — per-address hash buckets, atomic value-check + enqueue, WAKE/REQUEUE/WAKE_OP (`kernel/sys/syscall_proc.c:195-208`). Linux-like model.
- **TTY line discipline (N_TTY subset)** — ICANON/ISIG/ECHO* + VINTR/VQUIT/VSUSP signal generation + erase handling are solid (`kernel/tty/tty_core.c`).
- **SysV + POSIX IPC** — full shm/sem/msg/mq surface present.
- **Syscall ABI breadth** — ~270 syscalls dispatched including statx, io_uring, pidfd, seccomp, namespaces, xattr, splice/copy_file_range, eventfd/timerfd/signalfd/inotify/memfd.
