# Ranix Structural Ceiling Implementation Roadmap

**Date:** 2026-06-06
**Scope:** Staged build plans for the six big structural ceilings from `docs/linux_parity_audit.md` (Top-20 structural section): unified page cache, reclaim+swap, real scheduler, block bio/elevator, ext4 journaling, hrtimers+NOHZ.
**Status:** Planning only. Every "current state" cites `path:line` in this tree. This is a blueprint with dependency order and a risk map, not code.

A recurring theme: Ranix is a **modular monolith, IRQ-on but non-preemptible-kernel**, dual-arch (x86_64 + aarch64), with the BSP running the global tick (`kernel/sched.c:223-237`) and per-CPU local ticks for accounting (`sched.c:242-255`). Several plans below lean on that model (a kernel thread can run a reclaim/writeback/commit loop without preemption hazards mid-section), and a few fight it (NOHZ wants the BSP tick to stop).

---

## 1. Unified Page Cache

### Current state
- **ramfs** holds each regular file's data in one contiguous power-of-2 buddy block: `ramfs_file_t { data, size, capacity }` (`kernel/fs/ramfs.c:21-25`); grow = alloc bigger block + `memcpy` + free old (`ramfs.c:83-101`). `read`/`write` are `memcpy` against that buffer (`ramfs.c:64-113`).
- **tmpfs** is the same model (`kernel/fs/tmpfs.c:16`, mmap at `:165`).
- **ext2/ext4** has a private fixed-size LRU **block** cache (`EXT2_BCACHE_N` slots, `kernel/fs/ext2.c:183-214`), with sequential readahead (`ext2.c:255-302`). It is per-fs, block-granular, not page-granular, and not shared with mmap.
- **mmap** of a file: the new cacheable path (`kernel/sys/syscall_mm.c:205-229`) maps the inode's *own backing page* via `fops->mmap` (`ramfs.c:120-128`, `tmpfs.c:165`), pins it with `mmap_pinned` (`vfs.h:143`) + `pmm_hold` (`pmm.c:12-17`) + `VM_NOFREE`. This works **only** because ramfs/tmpfs data is one contiguous buddy run — it cannot work for ext2 (block-scattered) or for files larger than one buddy block cleanly. Non-cacheable / MAP_PRIVATE still falls to the **copy path** (`syscall_mm.c:234-258`): a private page per page, `read()` into it. So `read()` + `mmap()` of an ext2 file get independent copies (audit MM-1).
- There is **no `address_space`** anywhere: `vfs_inode_t` (`vfs.h:128-152`) has no page list/tree; `vfs_fops_t` (`vfs.h:113-125`) has `read`/`write`/`mmap`-returns-phys, no `read_page`/`write_page`/`readahead`/`writepage`.

### Target (Linux model → minimal Ranix)
Linux: per-inode `struct address_space` with an xarray of `struct page`; `read`/`write`/`mmap`/sendfile all go through `filemap_*`; dirty pages tracked + written back by `->writepage`; clean pages reclaimable.

Minimal Ranix: a `page_cache_t` per inode — a **radix/3-level page table keyed by file page index** → `struct fpage { void *kva; uint64_t pgidx; uint16_t flags(DIRTY/UPTODATE/LOCKED); uint32_t refcount; lru_link; }`. fs drivers implement `aops->read_folio(inode, pgidx, kva)` (fill one page from backing store) and `aops->write_folio(inode, pgidx, kva)` (write one page back). Generic `pagecache_read/write/mmap` live in one file (`kernel/mm/filemap.c`, new) and call those.

### Staged plan
1. **Introduce `address_space` + radix and a generic read path, ramfs/tmpfs first.** Add `struct address_space *i_mapping` to `vfs_inode_t` (`vfs.h:128`), a `vfs_aops_t { int (*read_folio)(inode,idx,kva); int (*write_folio)(...); }` ptr on the inode. Implement `filemap_read()` that looks up/allocates a cache page, calls `read_folio` on miss, `memcpy`s out. Convert **ramfs/tmpfs read()** to it (they already own pages — `read_folio` just copies from `rf->data + idx*PAGE`). Both-arch test: existing `test_mmap`, `test_fssweep`, `test_bigdir` must stay green; data identical. **Landable: ramfs/tmpfs functionally unchanged, now routed through filemap.** (effort M)
2. **Route ext2 reads through the page cache.** Implement ext2 `read_folio` = "map logical→physical block (`ext2.c:411-422` extent/indirect logic already exists), `block_read` the page-worth of blocks into the cache page". Keep the existing `EXT2_BCACHE_N` block cache *only* for metadata (bitmaps, inode tables, indirect blocks) — file *data* now lives in the unified cache. This is where the audit's "read+mmap divergence" closes for real files. (effort L)
3. **Unify mmap on the page cache.** Replace `fops->mmap`-returns-phys (`ramfs.c:120`, `syscall_mm.c:205-229`) with: file fault / mmap setup looks up the cache page for the faulting file-page, `pmm_hold`s it, maps it. MAP_SHARED maps the cache page writable (stores hit the cache → marked DIRTY); MAP_PRIVATE maps it read-only and COWs on write (the existing COW path `kernel/mm/vm.c:190` already handles permission faults). This **removes `mmap_pinned`/contiguous-buffer constraint** (`ramfs.c:84,124`) and makes mmap work for ext2. Deletes the copy path (`syscall_mm.c:234-258`). (effort L)
4. **Dirty tracking + writeback hook + `write()` through cache.** Convert `write()` to dirty cache pages; add `aops->write_folio` for ext2 (write a page's blocks back) and a `filemap_writeback(inode)` walk used by `fsync`/`msync` (`syscall_mm.c:548`) and `sync`. ramfs/tmpfs `write_folio` is a no-op (data IS the cache). This closes audit MM-4 (MAP_SHARED writeback). (effort M)
5. **Per-inode LRU link into a global clean/dirty list** (the seam reclaim plugs into — ceiling #2). No eviction yet; just maintain the lists. (effort S)

### What it touches / breaks
`vfs_inode_t`, `vfs_fops_t`/new `vfs_aops_t` (`vfs.h:113-152`), every fs read/write (`ramfs.c`, `tmpfs.c`, `ext2.c`, and check `fat.c`/`iso9660.c`/`overlay.c` read paths), the file-mmap path (`syscall_mm.c:193-266`), COW (`vm.c:190`), `msync`/`fsync`/`sync`. High blast radius — that's why it's staged so each step keeps the suite green.

### Dependencies
Foundational. **Reclaim (#2) depends on this.** Block bio (#4) makes step 2/4 much faster (page-sized I/O instead of per-sector bounce) but is not a hard prerequisite — page cache can sit on today's `block_read(...,count)`.

### Risk + effort
Risk: **High** (mmap coherence, COW interaction, refcount/free ordering on shared pages — the same UAF class memory notes already burned on once). Effort: **XL** overall.

### Suggested first step
Step 1 (address_space + filemap_read, ramfs/tmpfs only). It is internally invisible (same data, same tests) yet lays every struct the other five steps need.

---

## 2. Memory Reclaim + Swap

### Current state
- Only OOM response is the **OOM-killer** (`kernel/mm/oom.c`): on a failed user-page alloc, SIGKILL the largest-`mem_charged` process and retry a bounded number of times (`oom.c:1-6`, `oom_kill_alloc` at `:11`). Victim selection is footprint-only (audit MM-9).
- Allocation entry for user pages is `mm_alloc_page_oom()` (`syscall_mm.c:174`, fault path `kernel/mm/vm.c`). On empty buddy → OOM, no eviction.
- `pmm` has per-page refcounts (`pmm.c:12-33`) — the hook reclaim needs to know a page is unshared.
- No `kswapd`, no LRU, no swap device, no `MADV_DONTNEED` reclaim (`syscall_mm.c:416` comment "We lack reclaim").

### Target (Linux → minimal Ranix)
Linux: active/inactive LRU lists per zone, `kswapd` background reclaim + direct reclaim under pressure, clean file pages dropped, dirty pages written back, anon pages swapped. Minimal Ranix: **two global LRU lists (file, anon)**, a `kswapd`-like kernel thread woken at a low-watermark, eviction of clean file pages (free immediately) and writeback of dirty ones; **swap optional / phase 2**.

### Staged plan
1. **Free-page watermarks + reclaim trigger.** Add `pages_low`/`pages_min` to the buddy (`kernel/mm/buddy.c:22-24`); when free crosses `low`, wake a new `kswapd` kthread (modeled on the existing BSP tick callbacks / kernel threads). No reclaim logic yet — just the plumbing and a `/proc` counter. (effort S)
2. **File-page reclaim (clean drop).** Walk the page-cache LRU from ceiling #1 step 5; for a clean, unmapped (`pmm` refcount==1) file cache page: unhook from the inode radix and free it. This is the high-value, low-risk reclaim — clean file pages are trivially regenerable. Makes the page cache *bounded* instead of growing until OOM. (effort M)
3. **Dirty-page writeback before reclaim.** Before freeing a dirty file page, call `aops->write_folio` (#1 step 4). Reuses the writeback path; reclaim and `fsync` share it. (effort M)
4. **Anon reclaim → swap (phase 2, optional).** Add a swap backend over a block device: a swap bitmap, `swap_out(page)`→write to a swap slot + store the slot in the PTE (a swap-PTE encoding, both arches), `swap_in` on fault (`vm.c:89` fault handler learns the swap-PTE case). Add an anon LRU. Highest risk; defer until file reclaim proves the LRU machinery. (effort XL)
5. **Pressure accounting (PSI-lite).** Export reclaim activity / stall to `/proc/pressure/memory` (audit OBS-3). (effort S)

### What it touches / breaks
Buddy (`buddy.c`), page cache (#1), the fault path (`vm.c:89`) for swap-in, PTE encoding in both `arch/*/vm.c` for swap entries, OOM (becomes the last resort after reclaim fails). cgroup memory controller (`cgroup.c`) should drive per-cgroup reclaim eventually.

### Dependencies
**Hard dependency on ceiling #1** (no page cache → nothing reclaimable but anon). Swap (step 4) wants ceiling #4 (bio) for efficient swap I/O but can use `block_write` initially.

### Risk + effort
File reclaim (steps 1-3): **Medium** risk, **L** effort. Swap (step 4): **High** risk (swap-PTE on two arches, swap-in fault races), **XL**. 

### Suggested first step
Step 1 (watermarks + kswapd kthread skeleton + counter) — independently landable, zero behavior change, unblocks step 2 the moment #1 step 5 exists.

---

## 3. Real Scheduler (CFS/EEVDF) + Per-rq Locking

### Current state
- One global `sched_lock` guards runqueues, process state, proctab, pid alloc, signals, context switch (`kernel/sched.c:157`; the long comment at `:108-156` is an *already-designed* lock-split plan — L1 `proc_lock`, L2 per-`rq->lock`, L3 per-task `pi_lock` — deliberately unshipped pending a soak).
- Runqueues are **already per-CPU** (`cpu->rq_head/tail`), but every op takes the one lock: `cpu_rq_add`/`pop`/`steal` (`sched.c:334-400`), `pick_cpu` walks all rqs counting length (`:412-426`), `runqueue_pop` work-steals the longest rq (`:444-465`).
- Policy is **round-robin**: FIFO list, RT tasks inserted at head (`cpu_rq_add`, `:345-348`), quantum = base − nice·base/40 ×cgroup-weight (`:711-716` per audit SCH-2). No vruntime, no fairness, no per-RT-priority ordering, no RT throttling.
- Per-CPU local tick sets `need_resched` on quantum expiry (`sched_tick_local`, `:242-255`).

### Target (Linux → minimal Ranix)
Linux: per-rq locks, EEVDF (vruntime + lag/eligibility), load balancing over sched domains, RT class with 100 priorities + throttling. Minimal Ranix: **per-rq locks (the documented split)** + a **vruntime fair policy** (weighted by nice/cgroup-weight) replacing round-robin, keeping work-stealing as the balancer. Keep IRQ-on non-preemptible-kernel model.

### Staged plan
1. **vruntime fairness under the existing global lock (policy first, locking later).** Replace the FIFO rq with a per-CPU **min-vruntime ordered structure** (start with a sorted insert or a small pairing/RB heap; the rq is per-CPU already). Each task carries `vruntime += delta / weight`; pick = min vruntime. Nice/cgroup-weight (`sched.c:711`) becomes the weight, not a quantum multiplier. This is *self-contained* — still one lock — and directly testable (`test_sched`, `test_cpuweight`, `test_affinity` must stay green; add a fairness assertion). (effort L)
2. **Per-priority RT ordering + RT throttling.** Give SCHED_FIFO/RR real priority levels (a small per-priority bucket array ahead of the fair tree) and an `rt_runtime` cap so runaway RT can't starve (audit SCH-5). (effort M)
3. **Lockless rq-length snapshot.** Add an atomic `rq->nr_running` updated on enqueue/dequeue; `pick_cpu`/steal read it unlocked (the comment at `sched.c:136-139` already prescribes this). Removes the O(ncpus) walk-under-lock. Still one lock for mutation. (effort S)
4. **Split the lock per the documented design.** Introduce `proc_lock` (L1), per-`rq->lock` (L2, `cpu_context_t` already reserves `rq_lock`), per-task `pi_lock` (L3); enforce L1→L2→L3, double-rq-lock by ascending cpu_id. This is the XL, soak-requiring step the in-tree comment warns about. Land behind heavy stress (`test_execstress`, `test_pipestress`, `test_killgroup`, plus a new long SMP soak) on **both arches**. (effort XL)
5. **Sched domains / topology-aware balancing** (optional, post-split). (effort L)

### What it touches / breaks
`kernel/sched.c` throughout, `cpu_context_t` (`include/kernel/smp.h`), `process_t` sched fields, signal posting / group walks (`kernel/signal.c`), fork/exit. The lock split touches *every* site that takes `sched_lock` today (it guards far more than scheduling).

### Dependencies
Independent of the MM/FS ceilings. Step 4 (lock split) is the riskiest single change in this document and should not overlap the page-cache work (concurrent invasive churn).

### Risk + effort
Steps 1-3: **Medium**, L+M+S. Step 4: **Very high** (lost-wakeup / rq-corruption class the suite "won't deterministically catch" per `sched.c:152-155`), **XL** — gate on a dedicated soak harness.

### Suggested first step
Step 1 (vruntime policy under the existing lock). High value (real fairness), self-contained, no locking risk, and it makes the eventual lock-split a pure-locking change rather than policy+locking at once.

---

## 4. Block Layer: bio / request queue + elevator

### Current state
- Fully **synchronous, per-sector**: `block_read/write` take a per-device spinlock and call `dev->read/write` directly (`kernel/block.c:10-27`). No queue, no merge, no async, no scheduler.
- The byte view `blkdev_read/write` (`block.c:36-88`) loops a **single 512 B `sect[BLOCK_SIZE]`** buffer per sector; partial writes do read-modify-write per sector (`block.c:79-83`) — audit BLK-2.
- Drivers are **polled** (`drivers/usb/xhci.c:10` "Polling"; NVMe/AHCI/virtio-blk submit-and-wait). I/O accounting is read-only counters `nr_reads`/`nr_read_secs` (`block.c:15,100`); no write counters, no diskstats (audit BLK-3).
- `block_device_t` exposes `read(dev,lba,buf,count)`/`write(...)` (`include/kernel/block.h`) — a multi-block interface already, so the page cache (#1) can issue page-sized I/O today without bio.

### Target (Linux → minimal Ranix)
Linux: `bio` → `request` → multi-queue (blk-mq) with an elevator (mq-deadline/kyber), merging, plugging, async completion. Minimal Ranix: a `struct bio { dev, lba, nr_sectors, page list, dir, end_io, status }`, a single per-device request queue with **merge + simple deadline/FIFO elevator**, and an async completion path so callers can wait or get a callback.

### Staged plan
1. **`struct bio` + `submit_bio()` shim over the current sync path.** Define bio; `submit_bio` translates to today's `block_read/write` and completes inline (synchronous). No behavior change — just the new ABI. Convert one caller (the page cache, or `blkdev_read`) to build bios. (effort M)
2. **Multi-page / multi-block bios.** Let a bio carry a page vector so a page-cache flush or readahead becomes one bio of N blocks instead of N per-sector bounces — kills the `block.c:36-88` per-sector loop. Big real-world speedup for `dd`/FS. (effort M)
3. **Request queue + merge + elevator.** Add a per-device queue; `submit_bio` enqueues a request, adjacent LBAs merge, a FIFO/deadline elevator orders dispatch. A queue-runner (kthread or inline drain) calls `dev->read/write`. Add write counters + `/proc/diskstats` (audit BLK-3). (effort L)
4. **Async completion (driver-driven).** Give one driver (virtio-blk is the cleanest — it has real queues) an interrupt/completion path that calls `bio->end_io`; callers either block on a wait or are woken. This is the step that lets io_uring (`kernel/io_uring.c`, today funnels into the sync path) be truly async. (effort L, per-driver)

### What it touches / breaks
`kernel/block.c`, `include/kernel/block.h`, every block driver (`drivers/block/*`, `usb/xhci.c` MSC), the page cache I/O calls (#1), io_uring, ext2 block I/O (`ext2.c` `block_read`). Steps 1-3 keep drivers synchronous, so low risk; step 4 is per-driver.

### Dependencies
Strongly **synergistic with #1** (page cache wants page-sized async I/O) but not a hard prerequisite either direction. Step 4 (async) is what swap (#2 step 4) and io_uring want.

### Risk + effort
Steps 1-3: **Low-Medium** (sync underneath, mostly an abstraction layer), L overall. Step 4: **Medium** per driver (interrupt races, completion vs teardown). Total **L** (audit agrees: BLK-1 is L).

### Suggested first step
Step 1+2 together (bio with a page vector over the sync path, convert the page-cache flush path). Immediately removes the per-sector bounce and gives every later step its data structure.

---

## 5. ext4 Journaling (jbd2-style) + Write Extents

### Current state
- `kernel/fs/ext2.c` is **ext2 RW + ext4 read-only**: reads ext4 extents (`ext4_extent_lookup`, `ext2.c:357-422`) and 64-bit sizes, but **writes** use only ext2 indirect blocks (`ext2.c:1023` "non-extent only").
- Journal is **detected but not replayed**: superblock has `s_feature_compat`/`incompat` + journal fields (`ext2.c:55-69`); on mount, if `INCOMPAT_RECOVER` is set it **only warns** ("jbd2 replay not implemented", `ext2.c:1435-1436`) and reads pre-crash state.
- Block I/O goes through the per-fs cache (`read_block`/`write_block`, `ext2.c:268-302` and the write counterpart) on the synchronous block layer.

### Target (Linux → minimal Ranix)
Linux: jbd2 — ordered/writeback/journal data modes, transaction batching, commit + recovery (scan→revoke→replay). Minimal Ranix: **(a) journal replay on mount** for crash recovery (read-mostly first — makes a journaled ext4 safe to read after a crash) then **(b) ordered-data journaling** for metadata writes, then **(c) extent write support** so ext4 (not just ext2) is RW.

### Staged plan
1. **Parse the jbd2 journal + replay on mount.** Read the journal inode (`s_journal_inum`, `ext2.c:68`), parse the jbd2 superblock + descriptor/commit/revoke blocks, replay committed transactions to their final locations before first use. This converts the current "warn and read stale" (`ext2.c:1435`) into real crash recovery — **read-only journals are now safe**, the single biggest durability win. (effort L)
2. **Journal abstraction for our own writes (metadata, ordered mode).** A `jbd_handle` API: `jbd_start()`, `jbd_get_write_access(block)`, `jbd_dirty_metadata(block)`, `jbd_stop()`; a commit kthread writes the journal then checkpoints. Route ext2's existing metadata writes (bitmaps, inode table, dir blocks) through it in **ordered mode** (data blocks flushed before the metadata commit). Now *our* writes are crash-consistent. (effort XL)
3. **Extent write support.** Implement extent-tree allocation/splitting so writes to an ext4 (extent) inode work, not just ext2 indirect (`ext2.c:1023`). Makes ext4 genuinely RW. (effort L)
4. **`e2fsck`-lite fallback** for an un-journaled or corrupt fs (optional). (effort L)

### What it touches / breaks
`kernel/fs/ext2.c` (becomes ext2/ext4), the write paths, mount. Depends on a reliable write barrier / flush from the block layer (#4) for real crash safety — *journaling without a working cache flush is a false sense of safety*. Page cache (#1) interacts: journaled writeback must flush journal before data/metadata pages.

### Dependencies
**Wants #4 (bio + flush/barrier)** for true ordering guarantees, and **#1** for coherent writeback ordering. Step 1 (replay-only) can land **independently today** (read-time, no writeback ordering needed).

### Risk + effort
Step 1: **Medium** (parsing + idempotent replay; testable against a crashed-then-mounted image), **L**. Step 2: **Very high** (transaction correctness, commit/checkpoint races, crash-injection testing), **XL**. Total **XL** (audit agrees: FS-1 XL).

### Suggested first step
Step 1 (journal replay on mount). It's the highest durability-per-risk move, independently landable, and removes a currently-known data-correctness gap (mounting a crashed ext4 reads stale blocks).

---

## 6. hrtimers + NOHZ

### Current state
- Fixed **100 Hz** tick everywhere: `TIMER_HZ 100` (`include/kernel/board.h:7`), both arches.
- x86: periodic LAPIC timer calibrated to one `TIMER_HZ` tick (`arch/x86_64/smp.c:68-107`), per-CPU; `lapic_timer_irq` → `tick_notify_local` (`smp.c:111-113`). aarch64: per-CPU arm local timer → `tick_notify_local`.
- BSP runs the **global** tick callbacks (sleeper wakeups, itimers, tcp/timerfd tick, watchdog) in `tick_notify` (`sched.c:223-237`); per-CPU `sched_tick_local` does accounting + quantum (`sched.c:242-255`).
- `sched_next_wake` is an atomic earliest-deadline hint (`sched.c:320-323`) — already a seed for next-event programming.
- Everything time-based is **tick-quantized to 10 ms**: nanosleep, itimer (`itimer_charge_cpu`, `sched.c:165,248`), timerfd. No hrtimer subsystem; no `timer_create` (audit TMR-2).

### Target (Linux → minimal Ranix)
Linux: per-CPU clockevent devices in one-shot mode, an hrtimer rbtree per CPU, dynamic-tick (NOHZ) idle that stops the tick. Minimal Ranix: a **per-CPU hrtimer queue** (sorted by ns deadline) backed by reprogramming the local timer one-shot to the next deadline; NOHZ-idle (don't reprogram when idle with no pending timer).

### Staged plan
1. **One-shot clockevent abstraction.** Wrap each arch's local timer in `clockevent_program_next(ns)` (x86: LAPIC one-shot count from the calibrated `lapic_timer_initcnt` ratio, `smp.c:71-96`; aarch64: write the timer compare register). Keep a 100 Hz *periodic* behavior by reprogramming +10 ms each fire — **no behavior change**, but the timer is now one-shot-driven. (effort M)
2. **Per-CPU hrtimer queue.** A sorted list/heap of `{ deadline_ns, callback }`; on each fire, run expired timers, reprogram to the earliest remaining (or the next 10 ms scheduler tick, whichever is sooner). Migrate `sched_next_wake` (`sched.c:320`) and timerfd into it. (effort L)
3. **High-res nanosleep / clock_nanosleep / timerfd.** Back them by hrtimers → sub-tick (µs) granularity instead of 10 ms (audit TMR-1). Add `timer_create`/`timer_settime` POSIX timers (audit TMR-2) on the same queue. (effort M)
4. **NOHZ idle.** When a CPU goes idle (`idle_loop`, `sched.c:471`) with no pending hrtimer and an empty rq, **don't** reprogram the periodic tick — sleep until the next real event (timer deadline or IPI). Keep the BSP's global housekeeping tick alive (sleeper/itimer/watchdog walks assume a steady BSP tick, `sched.c:223-237`) — i.e. **NOHZ-idle on secondaries first**, full-NOHZ later. (effort L)

### What it touches / breaks
`arch/x86_64/smp.c` + aarch64 timer, `kernel/sched.c` tick path, timerfd/itimer/nanosleep, new `timer_create`. The scheduler quantum logic must tolerate variable tick spacing (charge by elapsed ns, not "1 tick"). The watchdog (`sched.c:182-221`) keys on `wd_ticks` — NOHZ must not make an idle CPU look "wedged" (the watchdog already exempts idle CPUs, `:204-207`, but verify under NOHZ).

### Dependencies
Independent of MM/FS/block. Lightly couples to the scheduler (#3): variable-spacing ticks want vruntime charged by ns (which #3 step 1 already introduces). Doing #3 step 1 first makes #6 step 4 cleaner.

### Risk + effort
**Medium** — bounded, arch-local, and each step keeps the suite green (`test_clock`, `test_itimer`, `test_futex_timed`, `test_poll` timeouts). Effort **L** (audit agrees: SCH-6/TMR-1 L).

### Suggested first step
Step 1 (one-shot clockevent wrapper that re-arms +10 ms). Zero behavior change, both-arch, and it's the substrate every later hrtimer/NOHZ step needs.

---

## Global Dependency Graph + Recommended Build Order

```
        ┌─────────────────────────────────────────────┐
        │ #1 Page Cache (address_space + filemap)      │  XL, foundational
        └───────────────┬───────────────┬─────────────┘
                        │ enables         │ writeback wants
                        ▼                 ▼
        ┌──────────────────────┐   ┌─────────────────────┐
        │ #2 Reclaim + Swap     │   │ #5 ext4 journaling   │  XL
        │  (file reclaim → swap)│   │  (replay → ordered)  │
        └──────────────────────┘   └─────────┬───────────┘
                                              │ wants barriers
        ┌──────────────────────┐              │
        │ #4 Block bio/elevator │◄─────────────┘ synergistic with #1, needed by #5/swap
        └──────────────────────┘
        ┌──────────────────────┐
        │ #3 Scheduler          │  vruntime(L) → RT(M) → snapshot(S) → LOCK SPLIT(XL)
        │  (independent)        │  ──┐ step1 helps #6
        └──────────────────────┘    │
        ┌──────────────────────┐    │
        │ #6 hrtimers + NOHZ    │◄───┘  independent, Medium risk
        └──────────────────────┘
```

**Edges:** #2 **hard-depends** on #1 (nothing reclaimable without a page cache). #5 step 2 (our-write journaling) **wants** #4 (flush/barrier) and #1 (writeback ordering); #5 step 1 (replay) is independent. #4 is **synergistic** with #1 and **needed** by swap (#2.4) and real async io_uring. #3 and #6 are independent of the MM/FS stack; #3 step 1 (vruntime) makes #6 step 4 (NOHZ) cleaner.

### Recommended build order (value-per-risk)

1. **#6 step 1 + #4 steps 1-2** — low risk, immediate wins, both arches. One-shot clockevent (no behavior change, unblocks all timer work) and bio-with-page-vector over the sync path (kills the per-sector bounce, gives every later FS/cache step its I/O primitive). Warm-up that de-risks the big items.
2. **#3 step 1 (vruntime fairness under the existing lock)** — high user-visible value (real fair scheduling), self-contained, no locking risk, and it turns the eventual lock-split into a pure-locking change.
3. **#1 (page cache)** — the keystone. Land it staged (ramfs/tmpfs → ext2 reads → mmap → writeback). Do **not** overlap with #3's lock split. This unblocks #2 and de-risks #5.
4. **#2 steps 1-3 (file reclaim)** — once #1 lands, bounded clean-page reclaim is medium risk and removes the "OOM-kill is the only pressure response" ceiling.
5. **#5 step 1 (journal replay)** — can actually slot in early/parallel (independent, read-time), but its full value (ordered journaling, #5.2) needs #1 + #4 and is the hardest correctness work — schedule after the cache + bio are solid.
6. **#3 step 4 (lock split)** and **#2 step 4 (swap)** and **#5 step 2 (ordered journaling)** — the three XL, soak-gated, crash/race-sensitive efforts. Sequence them **one at a time**, each behind a dedicated stress soak on both arches, never concurrently (the in-tree `sched.c:152-155` warning about undetectable rq corruption applies to all three).

### Single highest-value first step
**Ceiling #1, step 1: introduce `address_space` + `filemap_read` and route ramfs/tmpfs reads through it.** It is internally invisible (identical data, existing tests stay green), carries low risk, and lays the single set of structs (`i_mapping`, `vfs_aops_t`, the page radix) that ceilings #2 and #5 both build on — the keystone that unlocks the most downstream work per unit of risk.
