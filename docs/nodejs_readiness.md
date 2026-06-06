# Node.js Readiness Pre-Flight (V8 + libuv)

**Date:** 2026-06-06
**Goal:** Map what Node.js (V8 engine + libuv event loop) needs from the kernel and
check Ranix (dual-arch x86_64 + aarch64) against it, so the eventual port — the
product milestone, and the substrate the original `claude` CLI runs on — has its
blockers mapped before we start. **Planning, not coding. READ-ONLY.**

**Method:** Grounded code reading (`file:line` in actual Ranix code) cross-referenced
against the existing master audits:
`docs/linux_parity_audit.md` (breadth), `docs/syscall_conformance_audit.md` (depth),
`docs/procfs_sysfs_audit.md`, `docs/latent_hazards_audit.md`. Linux/V8/libuv
requirements stated for contrast.

**Bottom line up front:** The one fatal kernel gap is the **2 GB user virtual-address
span** (mmap arena < 1 GB). V8 reserves multi-GB contiguous virtual regions for its
heap cage / pointer-compression sandbox; that reservation cannot fit. Everything else
Node needs is present or only soft/degraded. **One hard blocker, and it is structural.**

> **Two stale-audit corrections found while grounding this doc** (the live code is
> better than the audits claim — do not treat these as gaps):
> - `membarrier` is **real**, not a no-op: `sys_membarrier` calls `smp_membarrier()`
>   for the EXPEDITED/GLOBAL commands (`kernel/sys/syscall_misc.c:1107-1127`).
>   `docs/linux_parity_audit.md` ABI-1 ("returns success without doing the barrier")
>   is **stale/wrong**; `docs/syscall_conformance_audit.md` is correct.
> - `getrandom` is a **CSPRNG** now (`csprng_bytes`, `kernel/lib/csprng.c`), with flag
>   validation (`syscall_misc.c:1130-1136`). The conformance audit's "splitmix64 /
>   predictable" note is stale.
> - `execve` now does **`#!` shebang** resolution + execute-permission enforcement
>   (`kernel/sys/syscall_proc.c:1013-1049`). The conformance audit's "no shebang" row
>   is stale.

---

## 1. V8 JIT / codegen / heap cage

| Need | V8 use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| Multi-GB contiguous VA reservation | V8 reserves a **4 GB cage** for pointer compression (default on 64-bit) + a larger sandbox region; reserves with `PROT_NONE` then commits sub-regions. | **MISSING.** mmap arena = `[ARCH_USER_MMAP_BASE 0x40000000 .. ARCH_USER_STACK_TOP 0x80000000 − 1 MB stack]` ≈ **< 1 GB**, both arches (`arch/*/include/asm/mmu.h:14-19`; `proc.c:170`; ceiling `syscall_mm.c:161`). | **HARD** | The single fatal gap. A 4 GB `mmap(PROT_NONE)` returns ENOMEM (`syscall_mm.c:162`). V8 init aborts. |
| W^X: PROT_EXEC pages | JIT writes code, flips to executable. | **HAVE.** `mprotect` honors PROT_EXEC → VM_EXEC (`syscall_mm.c:306,337`); mmap maps PROT_EXEC. | no | — |
| `mprotect` RW↔RX flips | V8 toggles code pages between writable and executable. | **HAVE.** Reprotects PTEs + syncs VMA flags, splits partial VMAs, and **shoots down the range cross-CPU** (`syscall_mm.c:295-345,327`). | no | Better than the conformance audit implies. One quirk: `prot!=0` over an unmapped hole allocates a fresh zero page instead of ENOMEM (`:316-321`) — benign for V8. |
| RWX pages | V8 uses RWX on some configs / older codegen. | **HAVE** (PROT_READ\|WRITE\|EXEC all map). | no | No W^X enforcement policy blocks it. |
| `madvise(MADV_DONTNEED/FREE)` | Scavenger/GC returns freed pages. | **HAVE.** Both drop present pages of a private anon range (`syscall_mm.c:439-473`). | no | No lazy MADV_FREE (drops eagerly) — permitted outcome. |
| Guard pages (PROT_NONE) | V8 brackets the cage / stack with PROT_NONE. | **HAVE.** `mmap(PROT_NONE)` maps a no-access VMA (anon path, `prot==0`); fault handler enforces. | no | Works, but consumes the tiny VA arena. |
| `MAP_NORESERVE` (overcommit big reservation) | V8 reserves cage without committing RAM. | **PARTIAL.** Flag accepted, mappings are lazy/demand-paged anyway (`syscall_mm.c:166`, conformance audit mmap row). | soft | Lazy commit exists, but the **VA span**, not RAM, is the wall. |
| `MAP_FIXED` sub-region commit | V8 commits pages inside the reserved cage at fixed addresses. | **HAVE** (`syscall_mm.c:73-75,156`). | no | MAP_FIXED_NOREPLACE behaves like MAP_FIXED (conformance) — irrelevant for V8. |

**Verdict for V8:** every page-permission and GC primitive V8 needs is present and
correct — **except** there is nowhere to put the heap. The 2 GB user VA / sub-1 GB mmap
arena cannot hold V8's cage. This is the gate.

---

## 2. Threads (libuv threadpool + V8 background threads)

| Need | Use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| `clone(CLONE_VM\|CLONE_THREAD…)` | pthread_create → real threads sharing the address space. | **HAVE.** `CLONE_VM` path → `proc_clone_thread` with new sp/tls/ptid/ctid/setttid (`kernel/syscall.c:470-488`); `clone3` prefix too (`:502-520`). | no | clone3 `set_tid`/`cgroup`/`pidfd` fields unused (parity ABI-4) — irrelevant to pthreads. |
| TLS base (FS / TPIDR) | per-thread `errno`, TLS. | **HAVE.** `arch_prctl(ARCH_SET_FS)` (`syscall_proc.c:1677-1681`); aarch64 `tpidr_el0` (`cpu.c:63`); `CLONE_SETTLS` wired. | no | — |
| `futex` (WAIT/WAKE/REQUEUE/WAKE_OP/BITSET) | pthread mutex/condvar, V8 sync. | **HAVE.** Per-address buckets, robust lists (`syscall_proc.c:195-208`; conformance "Linux-faithful"). | no | No PI-futex (`FUTEX_LOCK_PI`) — Node/V8 don't require it. |
| Thread-directed signals (`tgkill`) | V8 sampling/profiler, libuv signal handling. | **HAVE.** `tgkill` validates tgid membership (`syscall_sig.c:73`). | no | `tkill` is aliased to process-directed kill (conformance) — Node uses tgkill. |
| Per-thread stacks | pthread sets stack via clone sp. | **HAVE** (newsp threaded through). | no | But see below: **1 MB default user stack** + no auto-grow (parity matrix). |
| Robust futex lists | glibc thread cleanup. | **HAVE** (`set_robust_list`/`get_robust_list`, conformance). | no | — |

**Soft concern:** user stack is **1 MB** (`ARCH_USER_STACK_PAGES=256`) and does **not**
auto-grow on fault (Linux grows to RLIMIT_STACK 8 MB). pthreads that pass an explicit
larger stack via mmap are fine; the **main thread** deep-recursion (V8 parser/regexp) at
1 MB is a soft risk. `setrlimit` is a no-op (conformance #12) so it can't be raised.

---

## 3. Event loop (libuv)

| Need | Use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| `epoll_create1` / `_ctl` / `_wait` / `_pwait` | libuv's primary backend on Linux. | **HAVE** (`syscall.c:353-355,569-570`; impl `syscall_io.c`). | no | — |
| `EPOLLET` (edge-trigger) | libuv uses level-trigger by default but edge for some fds. | **HAVE.** Edge state tracked per watch (`syscall_io.c:188,207,331-332`). | no | — |
| `EPOLLONESHOT` | — | **HAVE** (`syscall_io.c:189,345`). | no | — |
| `EPOLLEXCLUSIVE` | thundering-herd avoidance on shared listen fds (cluster). | **MISSING** (no `EPOLLEXCLUSIVE` symbol). | soft/degraded | Node falls back; only matters for multi-process accept on one fd. |
| **epoll watch capacity** | Node servers watch thousands of fds. | **DEGRADED.** `EPOLL_MAX_WATCH = 64` (`syscall_io.c:200`) — **hard cap of 64 fds per epoll instance.** | **soft→hard at scale** | A real Node HTTP server with >64 concurrent connections + timers + signals **overflows** one epoll set. Linux is effectively unbounded. This is the most likely *practical* limiter after VA. |
| `eventfd2` | libuv async handle / threadpool wakeup. | **HAVE** (`syscall.c:629`). | no | EFD_CLOEXEC not applied to the fd (conformance) — minor leak across exec. |
| `timerfd_create/settime/gettime` | libuv timer backend. | **HAVE** (`syscall.c:630-632`). | soft | ABSTIME measured against boot-ticks not the clock epoch (conformance) → wrong after a clock step; 10 ms granularity (no hrtimers, SCH-6). |
| `signalfd4` | libuv signal handling. | **HAVE** (`syscall.c:639`). | no | — |
| `pipe2` / `dup3` (O_NONBLOCK\|O_CLOEXEC) | libuv self-pipe / fd setup. | **HAVE** (`syscall.c:347,349,351`). | no | — |
| `accept4` (SOCK_NONBLOCK\|CLOEXEC) | libuv accepts. | **HAVE** (`syscall.c:577-578`). | no | Doesn't EINVAL unknown flag bits (conformance) — benign. |
| Non-blocking sockets / `O_NONBLOCK` | the whole async model. | **PARTIAL.** Sockets nonblock for read/recv (MSG_DONTWAIT honored), but **non-blocking `connect` has no EINPROGRESS** (always blocks, `syscall_net.c:529`; conformance #10). | **degraded** | libuv connect() expects EINPROGRESS + writable-poll. A blocking connect stalls the loop until TCP completes/RSTs. Outbound client sockets (http.request, DNS-over-TCP) degrade badly. |
| `poll` | libuv fallback / `process.binding`. | **HAVE** (present in dispatch). | no | — |
| TCP write error semantics | EPIPE/SIGPIPE on dead peer; MSG_NOSIGNAL. | **MISSING** (`syscall_net.c:619`; conformance #9). | **degraded** | Node sets MSG_NOSIGNAL and relies on EPIPE to detect a dropped peer; a write to a reset socket won't error → hung/incorrect stream teardown. |
| `shutdown(SHUT_WR)` half-close | `socket.end()`. | **DEGRADED.** `how` ignored, always full close (`syscall_net.c:1335`; conformance #10). | **degraded** | Half-close streaming protocols break. |

**Verdict for libuv:** the backend exists and is wired (epoll/eventfd/timerfd/signalfd
all present) — **but** three socket-semantics gaps (non-blocking connect → no
EINPROGRESS, no EPIPE/MSG_NOSIGNAL, shutdown ignores `how`) plus the **64-fd epoll cap**
make a real network workload misbehave. None block startup; all degrade or break
servers/clients under load. These are M-effort conformance fixes, not structural.

---

## 4. Process / child_process

| Need | Use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| `fork` + `execve` / `posix_spawn` (clone3+VFORK) | `child_process.spawn`. | **HAVE.** `clone3` VFORK → `sys_vfork` spawn path (`syscall.c:511-515`); execve with shebang (`syscall_proc.c:1013`). | no | posix_spawn error reporting relies on waitid siginfo — see below. |
| `pipe` for stdio | stdio plumbing. | **HAVE.** | no | Fixed 4 KB pipe buffer, F_SETPIPE_SZ no-op (parity IPC-2) — soft. |
| `waitpid` / `WNOHANG` | child reaping. | **PARTIAL.** Direct-child waits + WNOHANG work; **pid==0 / pid<-1 (process-group) waits NOT handled** (`syscall_proc.c:1521`; conformance #8). | **degraded** | Node reaps by explicit child pid (common path OK); group-wait supervisors misbehave. |
| `kill` + process groups | `child.kill()`, `process.kill(-pgid)`, signaling a tree. | **DEGRADED.** **No process-group / broadcast kill** — `kill(-pgrp)`, `kill(0)`, `kill(-1)` → ESRCH (`syscall_sig.c:60`; conformance #1). | **degraded** | `child_process` with `detached:true` + `process.kill(-pid)` to kill a tree fails. Single-pid kill works. |
| `waitid` siginfo | posix_spawn failure detection. | **DEGRADED.** `infop` not filled (`syscall_misc.c:691`; conformance #8). | **degraded** | spawn-error reporting reads garbage si_status. |
| `pidfd_open` / `pidfd_send_signal` | newer Node child handles (optional). | **HAVE** (`syscall.c:498-499`). | no | `pidfd_getfd` missing (parity ABI-3) — Node doesn't need it. |

---

## 5. Filesystem (fs module)

| Need | Use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| `openat` / `*at` family | fs.promises, dirfd-relative ops. | **HAVE** (broad `*at` dispatch). | no | Many `*at` ignore dirfd/AT_* flags (conformance) — common AT_FDCWD path OK. |
| `statx` / `fstat` / `lstat` | fs.stat. | **HAVE.** `statx` present (`syscall.c:447`). | soft | `st_dev` hardcoded to 1 (conformance) → `fs.stat().dev` collisions, breaks hardlink/dedup logic; `fstat` on fd 0/1/2 returns a fake chardev (conformance #11) → `process.stdout.isTTY` / write buffering can be wrong when redirected. |
| `getdents64` / readdir | fs.readdir. | **HAVE** (`syscall.c:363`). | no | 4 KB bounce cap → more iterations, correct result. |
| `inotify` (fs.watch) | file watching, nodemon, dev servers. | **HAVE** (`syscall.c:634-637`). | soft/degraded | No fanotify (parity VFS-4); **`fs.inotify.max_user_watches` sysctl MISSING** (procfs audit #8) → watchers that read/clamp the limit (chokidar, webpack, VS Code-style) may error or misbehave. |
| `sendfile` / `copy_file_range` | fs.copyFile fast path, http static serving. | **HAVE but BUGGY.** `copy_file_range` **ignores explicit offsets** (`syscall_misc.c:746`; conformance #5). | **degraded** | Node's libuv `uv_fs_copyfile` uses copy_file_range with offsets → wrong data. libuv falls back to read/write on EINVAL/ENOSYS, but Ranix returns success with wrong bytes (worse than failing). |
| `realpath` | path resolution. | **HAVE** (`chroot`/path normalization Linux-faithful, conformance). | no | — |
| Large files (>2 GB) | big file I/O. | **HAVE.** ext2 reads 64-bit sizes (parity matrix). | soft | ext2 only, no journaling (FS-1) — durability risk, not a Node blocker. |
| `fallocate` | preallocation (some npm pkgs). | **DEGRADED.** No-op returns success (`syscall_misc.c:734`; conformance #4). | soft | Lies about reservation; rare in core Node. |
| Path length | deep node_modules trees. | **DEGRADED.** `PATH_MAX`-as-used = **256** (parity matrix, `proc.h:12`). | **soft→hard** | Deeply-nested `node_modules` paths routinely exceed 256 bytes. Linux is 4096. Real npm installs **will** hit ENAMETOOLONG-class failures. M-effort but high impact. |

---

## 6. Misc runtime requirements

| Need | Use | Ranix status (file:line) | Blocker? | Notes |
|---|---|---|---|---|
| `getrandom` (CSPRNG) | crypto, V8 hash seed, `crypto.randomBytes`. | **HAVE.** Real CSPRNG (`csprng.c`; `syscall_misc.c:1130`). | no | **Stale-audit correction** (was flagged predictable). |
| `clock_gettime(MONOTONIC)` | libuv timers, perf hooks. | **HAVE.** All clocks faithful, ns from HW counter (conformance); vDSO. | soft | 10 ms timer *granularity* for sleeps/timerfd (no hrtimers, SCH-6) — Node timers coarse. |
| `sysconf(_SC_NPROCESSORS)` | `os.cpus()`, libuv threadpool size, V8 default thread count. | **PARTIAL.** `sched_getaffinity` returns full online mask (`syscall_misc.c:995`); `/sys/devices/system/cpu/{online,possible}` present (sysfs audit); `/proc/cpuinfo` per-cpu (procfs audit). | soft | `sched_setaffinity` is a no-op (conformance) — `os` reports cores OK, pinning silently ignored. |
| `getrlimit(RLIMIT_NOFILE)` | libuv fd-table sizing. | **PARTIAL.** Reports soft 1024 (parity matrix); **setrlimit no-op** (conformance #12). | soft | Node can't raise the fd limit; `fs.nr_open` sysctl missing (procfs #9). 1024 fds caps server concurrency. |
| `uname` | `os.platform()`, `process.arch`. | **HAVE** but sysname = **"Ranix"**, release "6.6.0-ranix" (conformance). | soft | Node maps uname → `os.type()`; code that string-matches "Linux" (rare in Node core, common in native build scripts / `process.platform` is set at compile time anyway) may trip. `process.platform` is baked at build, so runtime impact is low. |
| `gethostname` | `os.hostname()`. | **HAVE** (`kernel.hostname` R/W). | no | — |
| `dlopen` for native addons | npm packages with N-API `.node` binaries. | **HAVE (in-userspace).** Dynamic linker via `PT_INTERP` + MAP_FIXED (`elf.c:29-40`, `syscall.c:134`); `test_dlopen`/`test_dynamic` exist and pass (192/192 per project status). | no | **Node core needs no addons** (statically linkable). Addon-heavy npm packages depend on the userspace ld.so — present, but the `claude` CLI itself is pure JS. |
| `epoll`/socket nonblock (recap) | see §3 | partial | degraded | — |

---

## Hard blockers to fix before Node can run (ranked)

1. **User virtual-address span is 2 GB; mmap arena < 1 GB** —
   `arch/{x86_64,aarch64}/include/asm/mmu.h:14-19` (`ARCH_USER_STACK_TOP 0x80000000`,
   `ARCH_USER_MMAP_BASE 0x40000000`), enforced at `kernel/sys/syscall_mm.c:161-162`,
   `kernel/proc.c:170`. **V8 cannot reserve its 4 GB pointer-compression cage.** This is
   the single hard, *structural* blocker, and it affects **both arches**. Fix path:
   raise the user VA layout to the real 64-bit canonical user range (x86_64 128 TB /
   aarch64 256 TB) — or, as a narrower interim, build V8 **without pointer compression**
   (`v8_enable_pointer_compression=false`), which drops the cage requirement to a far
   smaller reservation but still needs **more than the current ~1 GB arena**. Either way
   the VA layout must grow. Largest item; gates everything.

That is the **only** hard blocker. Everything below is soft/degraded — Node starts and
runs once the VA span is fixed, but networking/scale workloads misbehave.

## Soft / degraded (works but slow, limited, or wrong under load)

- **`EPOLL_MAX_WATCH = 64` per epoll instance** (`syscall_io.c:200`) — any Node server
  with >~60 concurrent fds overflows its event set. Highest practical limiter after VA.
- **Non-blocking `connect` has no EINPROGRESS** (`syscall_net.c:529`) — libuv outbound
  client sockets (http.request, TCP DNS) stall the loop.
- **No EPIPE / MSG_NOSIGNAL on dead-peer write** (`syscall_net.c:619`) — broken stream
  teardown; possible hangs.
- **`shutdown` ignores `how`** (`syscall_net.c:1335`) — `socket.end()` half-close breaks.
- **`copy_file_range` ignores offsets** (`syscall_misc.c:746`) — `fs.copyFile` returns
  *wrong bytes* (worse than ENOSYS, which would trigger libuv fallback).
- **PATH_MAX effectively 256** (`proc.h:12`) — deep `node_modules` trees hit name-length
  failures during real npm installs.
- **`kill` has no process-group support** + **`wait4` no group-wait** + **`waitid` no
  siginfo** (`syscall_sig.c:60`, `syscall_proc.c:1521`, `syscall_misc.c:691`) —
  `child_process` tree-kill and posix_spawn error reporting degrade.
- **`fstat` on fd 0/1/2 fakes a chardev; `st_dev` hardcoded** (`syscall_io.c:588`,
  `syscall_fs.c:118`) — wrong `isTTY` / buffering when stdio is redirected.
- **`fs.inotify.max_user_watches` + `fs.nr_open` + `vm.max_map_count` sysctls missing**
  (procfs audit #3,8,9) — file watchers and fd-limit raising misbehave.
- **1 MB non-growing main-thread stack** (`ARCH_USER_STACK_PAGES=256`) + **setrlimit
  no-op** — deep V8 recursion (parser/regexp) soft risk; can't raise NOFILE.
- **10 ms timer granularity** (no hrtimers, SCH-6/TMR-1) — coarse `setTimeout`.
- **No `EPOLLEXCLUSIVE`** — `cluster` thundering-herd, fallback only.

## Go / No-Go verdict + what to fix first

**No-go today — but the door is narrow and clearly marked.** Ranix is *much* closer to
running Node than expected: V8's page-permission and GC primitives (W^X mprotect with
cross-CPU shootdown, PROT_EXEC, MADV_DONTNEED/FREE, guard pages), the full threading
substrate (clone/clone3 CLONE_VM + SETTLS + arch_prctl FS-base + robust futexes +
tgkill), the entire libuv backend (epoll incl. EPOLLET/ONESHOT, eventfd, timerfd,
signalfd, pipe2/dup3/accept4), a working userspace dynamic linker, and a real CSPRNG are
**all present and correct**. Several things the prior audits flagged as broken
(`membarrier`, `getrandom`, `execve` shebang) are actually fixed in this tree.

There is exactly **one hard blocker: the 2 GB user-VA / sub-1 GB mmap arena cannot hold
V8's heap cage.** Fix that first — widen the user VA layout to the canonical 64-bit user
range on both arches (or, as a faster proof-of-life, cross-build V8 with pointer
compression *disabled* and still enlarge the arena to several GB). Until then V8 aborts
at init and nothing else matters. Once the VA span is fixed, the realistic next tier to
make Node *useful* (not just start) is: raise `EPOLL_MAX_WATCH`, implement non-blocking
connect (EINPROGRESS) + EPIPE/MSG_NOSIGNAL + `shutdown` `how`, raise PATH_MAX to 4096,
and fix `copy_file_range` offsets. None of those are structural — they are the M-effort
conformance gaps already catalogued in `docs/syscall_conformance_audit.md`.

**Separate concern — build feasibility (not a kernel gap):** Node is a multi-hundred-MB
C++ tree; V8's GN/ninja build + libstdc++-on-musl cross-compile is materially heavier
than Python's. That is a toolchain effort, orthogonal to the kernel readiness assessed
here, and does not change the kernel verdict.
