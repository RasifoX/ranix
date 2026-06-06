# Unified Page Cache (address_space + filemap) — Design

**Date:** 2026-06-06
**Status:** Design only. No code changed, nothing built or tested. Every "current
state" claim cites `path:line` in this tree.
**Scope:** The keystone structural ceiling from `docs/structural_roadmap.md` §1 —
a per-inode page cache (`address_space` + `filemap_read`/`filemap_write`),
page-cache-backed mmap (shared read-only text, COW private), dirty tracking +
writeback, readahead, and the seam reclaim (`docs/structural_roadmap.md` §2)
plugs into later.

This doc expands roadmap §1 into a buildable design: data structures, the new
fop/aops surface, how `vfs_read`/`vfs_write`/mmap reroute through the cache, the
dual-arch concerns, a phased migration that keeps both arches green at every
step, and a verdict on effort / risk / Node ordering.

---

## 0. TL;DR verdict

- **One epic, internally staged into 5 phases.** Each phase is independently
  landable and leaves both arches green. Total effort **XL** (largest single
  foundation item remaining).
- **Biggest risks:** mmap/read aliasing coherence, shared-page refcount/free
  ordering (the same UAF class that COW free-while-mapped already burned on —
  see `MEMORY.md` "COW free-while-mapped UAF"), and writeback ordering.
- **Does NOT block the Node port (#43).** Per `docs/nodejs_readiness.md:39,195`
  the single hard Node blocker is the **2 GB user-VA / sub-1 GB mmap arena**
  (addressed by `docs/user_va_widening_design.md`), not the page cache. The page
  cache is a *memory-efficiency and correctness* win for Node (shared read-only
  text across processes, coherent mmap+read), not a prerequisite — Node can ship
  first and benefit from the cache afterward.
- **Page size is a uniform 4 KB on both arches** (`include/kernel/mm/mmu.h:7`
  `PAGE_SHIFT 12`), so the page-cache unit needs no per-arch sizing — a real
  simplification over Linux.

---

## 1. Current state

### 1.1 The read/write data path

`sys_read`/`sys_write` `access_ok` the user buffer and hand the **raw user
pointer** to `vfs_read`/`vfs_write` (`kernel/vfs.c:1200-1227`), which call
`f->inode->fops->read(f, buf, len)` / `->write(...)` directly
(`vfs.c:1205-1206`, `:1223`). There is **no cache layer** between the fop and
the backing store; each filesystem `memcpy`s into/out of its own private buffer.
(This raw-user-pointer flow is also the SMAP/PAN funnel problem documented in
`docs/uaccess_funnel_inventory.md` §2b — see §6 for the interaction.)

**ramfs** — each regular file is one contiguous power-of-2 buddy block:
`ramfs_file_t { data, size, capacity }` (`kernel/fs/ramfs.c:21-25`).
`ramfs_read` is `memcpy(buf, rf->data + f->pos, len)` (`ramfs.c:71`);
`ramfs_write` `memcpy`s in and grows by alloc-bigger-block + `memcpy` + free-old
(`ramfs.c:83-101`, `:103`). `ramfs_create_anon` (`ramfs.c:507`) gives
`memfd_create` the same struct.

**tmpfs** — identical model: `tmpfs_read` `memcpy` at `kernel/fs/tmpfs.c:123`,
`tmpfs_write` grow + `memcpy` at `:130-160`, contiguous buffer.

**ext2/ext4** — has a private **fixed-size LRU *block* cache**, `EXT2_BCACHE_N`
slots (`kernel/fs/ext2.c:183-214`), with sequential readahead
(`readahead_fill`, `ext2.c:255-266`; demand/window logic `read_block`,
`ext2.c:268-309`). `ext2_read` (`ext2.c:518-553`) allocs a scratch page,
walks logical→physical blocks (`inode_map_block`, `ext2.c:538`), `read_block`s
each fs-block into the scratch, then `memcpy`s the slice to the user buffer
(`ext2.c:544`). This cache is **per-fs, block-granular, and not shared with
mmap or with `read()`'s output** — it caches disk blocks, not file pages, and
the data is copied out of it on every read.

**FAT** — *no cache at all*: `fat_read` reads each 512 B sector fresh
(`fat_rd_sec`, `kernel/fs/fat.c:277`) and `memcpy`s the slice (`fat.c:281`).

**Block device byte view** — `blkdev_read`/`blkdev_write` (`kernel/block.c:36-88`)
loop a single 512 B stack buffer per sector; partial writes do per-sector
read-modify-write (`block.c:79-83`). `block_read`/`block_write`
(`block.c:10-27`) are synchronous, per-device-locked, multi-block-capable. There
is **no global block cache** — a tree search finds none; the only data cache is
ext2's per-fs `bcache`. So the page cache builds on a clean `block_read(dev, lba,
buf, count)` primitive, not on top of an existing buffer cache.

### 1.2 How file mmap is backed today

`sys_mmap` file path (`kernel/sys/syscall_mm.c:201-278`) has **two** routes:

1. **Cacheable + (device OR MAP_SHARED)** (`syscall_mm.c:213-239`): calls
   `fops->mmap(f, off)` which returns the **inode's own backing page** physical
   address (`ramfs_mmap`, `ramfs.c:120-128`; `tmpfs_mmap`, `tmpfs.c:165`). It
   `pmm_hold`s the page (`ramfs.c:126`), maps it `VM_NOFREE` (`syscall_mm.c:217`),
   and sets `inode->mmap_pinned = 1` (`syscall_mm.c:228`) so a later grow can't
   relocate the buffer (`ramfs.c:84`, `ramfs.c:436`; `tmpfs.c`). This **only
   works because ramfs/tmpfs data is one contiguous buddy run** — `ramfs_mmap`
   computes `VIRT_TO_PHYS(rf->data + off)` (`ramfs.c:125`). It **cannot** work
   for ext2 (block-scattered, no single phys page) or FAT, and the
   `mmap_pinned`+contiguous constraint blocks file growth while mapped.

2. **Copy path** (`syscall_mm.c:242-277`): for non-cacheable or MAP_PRIVATE —
   allocate a fresh anon page per file page, `fops->read` into it
   (`syscall_mm.c:252-254` via a throwaway `tmp_f`), map it private. So `read()`
   and a MAP_PRIVATE `mmap()` of the **same ext2 file get independent copies**;
   a MAP_PRIVATE write COWs the anon page via the generic fault path
   (`vm.c:202-219`). This is roadmap audit "MM-1" (read+mmap divergence).

`sys_msync` (`syscall_mm.c:588-605`) is a **no-op flush**: it validates the
range is mapped, then returns 0 with the comment "pages are write-through to the
ram-backed store: nothing to flush" (`syscall_mm.c:603`) — true only because the
shared mapping *is* the ramfs/tmpfs buffer. There is no dirty tracking and no
writeback path, so an ext2 MAP_SHARED could never be flushed (it never reaches
route 1).

### 1.3 What's missing / what's reusable

- **No `address_space`:** `vfs_inode_t` (`include/kernel/vfs.h:128-152`) has no
  page list/tree — only `size`, `priv`, timestamps, `mmap_pinned`. `vfs_fops_t`
  (`vfs.h:113-125`) is `read`/`write`/`mmap`-returns-phys; no
  `read_page`/`write_page`/`readahead`/`writepage`.
- **Reusable substrate (important):**
  - `page_t` (`include/kernel/mm/pmm.h:11-16`) already has `refcount` **and a
    `flags` field** with room beyond `PG_MANAGED`/`PG_CACHED` (`pmm.h:18-19`) —
    the cache page-state flags (UPTODATE/DIRTY/WRITEBACK) can live there or in a
    parallel cache descriptor.
  - `pmm_hold`/`pmm_free` are refcounted (`kernel/mm/pmm.c:12-39`): a page is
    freed only when refcount hits 0 (`pmm.c:33-36`). This is exactly the
    primitive shared cache pages need — mmap can `pmm_hold` a cache page and
    teardown drops it without freeing a page the cache still owns.
  - `paddr_to_page`/`page_to_paddr` (`pmm.h:29-30`) map phys↔descriptor.
  - The COW fault path already special-cases file-backed shared pages
    (`vm.c:187-196`): a VMA that is `VMA_SHARED` and **not** `VMA_ANON` is
    remapped writable with `VM_NOFREE` so `vm_destroy` won't free a page the
    file owns (`vm.c:192-194`). The page cache slots into exactly this seam.
  - ext2 already has logical→physical block mapping (`inode_map_block`,
    `ext2.c:538`; ext4 extents `ext4_extent_lookup`, `ext2.c:357-422`) and
    readahead heuristics (`ext2.c:255-309`) to reuse for `read_folio`.

---

## 2. Target model (Linux, adapted not cloned)

Linux: per-inode `struct address_space` holding an xarray of `struct page`;
`read`/`write`/`mmap`/sendfile funnel through `filemap_*`;
`a_ops->read_folio`/`->writepages`/`->write_begin/end` fill and flush pages;
page flags `PG_uptodate`/`PG_dirty`/`PG_locked`/`PG_writeback` drive coherence;
dirty pages are written back by flusher threads; clean pages are reclaimable.

**Adaptation for Ranix's scale (modular monolith, non-preemptible kernel, no
huge file working sets yet):**

| Linux concept | Ranix proposal | Where it slots |
|---|---|---|
| `struct address_space` per inode | `struct address_space` (new), pointed to by `vfs_inode_t.i_mapping` | new `include/kernel/mm/filemap.h`; field added to `vfs.h:128` inode |
| xarray of `struct page` keyed by page index | **3-level radix** keyed by file page index → `fpage*` (xarray is overkill at our scale; a fixed 3-level radix of 4 KB index pages covers 512³·4 KB ≫ any file we host) | `address_space.i_root` |
| `struct page` (cache page) | reuse `page_t` (`pmm.h:11`) for the physical page; add a thin `fpage { page_t *pg; void *kva; uint64_t index; uint16_t flags; list lru; }` cache descriptor | `filemap.c` (new) |
| `struct address_space_operations` | `vfs_aops_t { int (*read_folio)(inode,index,kva); int (*write_folio)(inode,index,kva); }` | `vfs.h` near `vfs_fops_t:125`; ptr on inode |
| `PG_uptodate/dirty/locked/writeback` | `FP_UPTODATE / FP_DIRTY / FP_LOCKED / FP_WRITEBACK` in `fpage.flags` | `filemap.h` |
| `filemap_read` / `generic_file_read_iter` | `filemap_read(inode, pos, buf, len)` — lookup/alloc page, `read_folio` on miss, `copy_to_user` out | `filemap.c` |
| `generic_file_write` / `write_begin/end` | `filemap_write(inode, pos, buf, len)` — lookup/alloc page, `copy_from_user` in, mark `FP_DIRTY` | `filemap.c` |
| filemap page-fault / `filemap_map_pages` | mmap fault looks up the cache page, `pmm_hold`s it, maps shared-writable or private-RO-then-COW | `syscall_mm.c` mmap + `vm.c` fault |
| writeback (flusher) | `filemap_writeback(inode)` walks dirty `fpage`s → `write_folio` → clears `FP_DIRTY`; called by `fsync`/`msync`/`sync` synchronously first (a kthread flusher is a later optimization, fits the non-preemptible kthread model `sched.c:223-237`) | `filemap.c` |
| readahead window | `filemap_readahead(inode, index, n)` — same growing-window heuristic ext2 already uses (`ext2.c:276-308`), but at page granularity into the cache | `filemap.c` |
| LRU + reclaim | per-`address_space` and global clean/dirty LRU lists; eviction is roadmap §2 (this design only builds the *seam*) | `filemap.c` lists; reclaim later |

Cache unit = **one 4 KB page**, both arches (no per-arch sizing thanks to
`PAGE_SHIFT 12`). fs block sizes (ext2 1–4 KB, FAT 512 B sectors) are handled
*inside* `read_folio`/`write_folio`, which assemble a full page from N blocks.

---

## 3. Proposed Ranix design

### 3.1 Data structures (`include/kernel/mm/filemap.h`, new)

```
typedef struct fpage {
    page_t        *pg;        /* the physical cache page (pmm refcounted)   */
    void          *kva;       /* PHYS_TO_VIRT kernel alias for memcpy/copy  */
    uint64_t       index;     /* file page index (offset >> PAGE_SHIFT)     */
    uint16_t       flags;     /* FP_UPTODATE|FP_DIRTY|FP_LOCKED|FP_WRITEBACK */
    struct fpage  *lru_next, *lru_prev;   /* global clean/dirty LRU link    */
} fpage_t;

#define FP_UPTODATE   (1<<0)
#define FP_DIRTY      (1<<1)
#define FP_LOCKED     (1<<2)   /* I/O in flight; waiters sleep on it         */
#define FP_WRITEBACK  (1<<3)

typedef struct address_space {
    struct vfs_inode *host;      /* owning inode                            */
    void             *i_root;    /* 3-level radix root: index -> fpage*      */
    uint64_t          nrpages;
    spinlock_t        lock;      /* guards radix + flags + lru splice        */
    vfs_aops_t       *a_ops;     /* read_folio / write_folio                 */
} address_space_t;
```

`vfs_aops_t` (next to `vfs_fops_t`, `include/kernel/vfs.h:125`):

```
typedef struct {
    int (*read_folio)(struct vfs_inode *inode, uint64_t index, void *kva);
    int (*write_folio)(struct vfs_inode *inode, uint64_t index, void *kva);
} vfs_aops_t;
```

`vfs_inode_t` (`vfs.h:128`) gains one pointer:
`struct address_space *i_mapping;` (NULL = legacy uncached path, so non-converted
filesystems keep working during migration). `i_aops` can hang off `i_mapping`.

**Why a radix, not a hash:** index→page locality matters for readahead and
range writeback (walk a contiguous index range); a hash randomizes that. A
3-level radix of 4 KB index-node pages is `512³` entries ≈ 512 GB of file at
4 KB/page — far past any file Ranix hosts — at 3 pointer chases. The radix node
pages come from `buddy_alloc(0)` like everything else.

### 3.2 The generic read/write path (`kernel/mm/filemap.c`, new)

`filemap_read(inode, pos, buf, len)`:
1. For each page touched: `idx = (pos+done) >> PAGE_SHIFT`.
2. Radix lookup under `as->lock`. **Miss** → alloc a cache page
   (`buddy_alloc(0)`, wrap an `fpage`, `pmm` refcount = 1), set `FP_LOCKED`,
   drop the lock, call `a_ops->read_folio(inode, idx, kva)` to fill it from the
   backing store, set `FP_UPTODATE`, clear `FP_LOCKED`, wake waiters, insert into
   radix. **Hit but `FP_LOCKED`** → sleep until the in-flight I/O clears it
   (lost-wakeup-safe per `MEMORY.md` sleep/wake notes).
3. `copy_to_user(buf+done, kva+pageoff, n)` (the SMAP/PAN funnel point, §6).
4. Trigger `filemap_readahead` when the access looks sequential (reuse ext2's
   window heuristic, now at page granularity).

`filemap_write(inode, pos, buf, len)`:
1. Same lookup/alloc. On a partial-page write to a not-`FP_UPTODATE` page, first
   `read_folio` the page (read-modify-write at page, not sector, granularity —
   kills the per-sector RMW in `block.c:79-83` and `ext2_write`).
2. `copy_from_user(kva+pageoff, buf+done, n)`, set `FP_DIRTY`, splice onto the
   dirty LRU, bump `inode->size`/timestamps as the fs write paths do today
   (`ramfs.c:105-111`).
3. `write_folio` is deferred to `fsync`/`msync`/`sync`/reclaim.

`vfs_read`/`vfs_write` reroute (`kernel/vfs.c:1200-1227`): if
`f->inode->i_mapping` is set, call `filemap_read`/`filemap_write`; else fall to
the legacy `fops->read`/`->write`. This is the **per-inode feature flag** that
keeps unconverted filesystems green.

### 3.3 Per-filesystem aops

- **ramfs/tmpfs:** they already *own* the data pages. Two options:
  (a) keep the contiguous buffer and make `read_folio` a `memcpy` from
  `rf->data + index*PAGE` (cheapest first step, roadmap §1 step 1); or
  (b) eventually make the cache page *be* the storage (drop the contiguous
  buffer), making `write_folio` a no-op (roadmap §1 step 4 note "data IS the
  cache"). Start with (a); converge to (b) when mmap unifies (§3.4).
- **ext2/ext4:** `read_folio` = map logical→physical blocks
  (`inode_map_block`/`ext4_extent_lookup` already exist, `ext2.c:357-422`,
  `:538`) and `block_read` a page-worth of blocks into the cache page;
  `write_folio` = write that page's blocks back. The existing `EXT2_BCACHE_N`
  cache (`ext2.c:183-214`) **stays, but only for metadata** (bitmaps, inode
  tables, indirect/extent-index blocks); file *data* moves to the unified cache.
- **FAT:** `read_folio`/`write_folio` over cluster/sector walk (`fat.c:273-288`)
  — gives FAT a cache it has never had.

### 3.4 mmap on the page cache

Replace the `fops->mmap`-returns-phys route (`syscall_mm.c:213-239`,
`ramfs.c:120`, `tmpfs.c:165`) with cache-page lookup:

- **MAP_SHARED:** on map (or on fault, if demand-paged), `filemap` looks up/fills
  the cache page for the file page, `pmm_hold`s it, and `vm_map`s it
  **writable** with `VM_NOFREE` (so teardown unmaps but never frees a page the
  cache owns — exactly the existing `vm.c:192-194` rule for non-anon shared
  pages). A store hits the cache page directly → mark `FP_DIRTY` (a write fault
  on a clean-mapped page is the natural dirty-trap; or mark dirty eagerly on a
  writable shared map). `msync`/`fsync` then `write_folio`s it. This **removes
  the `mmap_pinned`/contiguous-buffer constraint** (`ramfs.c:84,124,436`;
  `tmpfs.c`) and makes MAP_SHARED work for ext2/FAT.
- **MAP_PRIVATE:** map the cache page **read-only**; a write fault COWs into a
  fresh anon page via the *existing* COW path (`vm.c:202-219`) — no new COW
  machinery. `read()` and MAP_PRIVATE now share the same cache page until first
  write (closes audit MM-1).
- **Coherence:** because `read()`/`write()` and every mapping reference the *one*
  cache page, the read/mmap aliasing divergence (§1.2) disappears by
  construction. Shared writes are visible to readers immediately; private writes
  fork off at COW time.
- The copy path (`syscall_mm.c:242-277`) is deleted once all mapped filesystems
  have aops.

### 3.5 Writeback / msync / fsync

`filemap_writeback(inode)`: walk the dirty LRU (or a dirty range for ranged
msync), for each `FP_DIRTY` page set `FP_WRITEBACK`, `write_folio`, clear both,
move to the clean LRU. `sys_msync` (`syscall_mm.c:588-605`) becomes a real flush
for MS_SYNC (call `filemap_writeback` over the mapped inodes in the range)
instead of the current no-op (`syscall_mm.c:603`). `fsync`/`fdatasync`/`sync`
route here too. ramfs/tmpfs `write_folio` is a no-op (volatile). Ordering:
flush data pages, then let the fs flush its own metadata (ext2 already does
metadata writes); journaling ordering is roadmap §5's problem, not this one.

### 3.6 Buddy supply + reclaim seam

Cache pages come from `buddy_alloc(0)` and are `pmm`-refcounted like every other
page (`pmm.c:12-39`) — no separate pool, so they compete with anon allocations
and a future reclaimer can free them back to the same buddy. The **reclaim seam**
(roadmap §2 steps 2-3): maintain the global clean/dirty LRU lists now (phase 5);
a later reclaimer evicts a clean, unmapped (`pg->refcount == 1`) cache page by
unhooking it from the radix and `pmm_free`ing it, and writes back dirty pages
first via `write_folio`. This design *builds the lists and the refcount
invariant* so reclaim is a pure add-on; it does **not** implement eviction.

### 3.7 Dual-arch concerns

- **Page size:** uniform 4 KB (`mmu.h:7`) — cache unit is arch-independent. No
  16 KB/64 KB aarch64 variance to handle.
- **PTE encoding / NX / PA mask:** mmap mapping of cache pages uses the existing
  `vm_map` with `VM_READ/WRITE/EXEC/USER/NOFREE/NOCACHE` (`vm.h:7-13`); the
  arch `vm.c` already encodes these into PTEs for the contiguous-buffer mmap
  today, so cache-page mapping reuses the identical encoding path. Shared
  read-only **text** pages map with `VM_READ|VM_EXEC` (no `VM_WRITE`) so
  multiple processes share one physical text page — the memory-bloat win for
  Node/Python.
- **I-cache coherence (aarch64):** the existing exec-mmap path already does
  `dc cvac` + `ic ialluis` when mapping executable file pages
  (`syscall_mm.c:260-267`) and on exec-COW (`vm.c:211-218`). Cache-page exec
  mappings must perform the same maintenance the first time a file page becomes
  executable (track per-`fpage` "i-cache clean" or just do it on every exec
  map, as today).
- **TLB coherence on shared-page remap:** when a MAP_SHARED page is upgraded
  RO→RW (the `vm.c:187-196` shared-writable remap) or a private page COWs
  (`vm.c:206-219`), the existing fault path already remaps and the arch layer
  flushes. Note `MEMORY.md` "x86 TLB shootdown": x86 uses correct full-flush;
  the cache must not assume cross-CPU shootdown exists — a shared page that
  changes permissions on one CPU relies on the existing flush semantics, which
  are conservative (correct) today.

---

## 4. Migration plan (phased, both arches green every step)

Each phase ends green on the full 192/192 suite, both arches. The per-inode
`i_mapping` flag means converted and unconverted filesystems coexist.

### Phase 1 — `address_space` + `filemap_read`, ramfs/tmpfs only
- **Scope:** add `address_space`, the radix, `fpage`, `vfs_aops_t`, `i_mapping`
  on the inode (`vfs.h:128`); implement `filemap_read`; ramfs/tmpfs `read_folio`
  = `memcpy` from `rf->data`; reroute `vfs_read` (`vfs.c:1205`) through filemap
  when `i_mapping` is set. Writes still legacy.
- **Risk:** Low — internally invisible, same data, same tests.
- **Proves it:** `test_mmap`, `test_fssweep`, `test_bigdir`, `test_pathsweep`
  stay green; data byte-identical. This is roadmap §1 step 1, the keystone that
  lays every struct the other phases need.
- **Effort:** M.

### Phase 2 — ext2 (and FAT) reads through the cache
- **Scope:** ext2 `read_folio` (logical→physical map + `block_read` a page of
  blocks), keeping `EXT2_BCACHE_N` for metadata only; FAT `read_folio`. Route
  their `vfs_read` through filemap.
- **Risk:** Low-Medium — reuses existing block-map + readahead logic
  (`ext2.c:357-422,538,276-308`); closes the read side of MM-1.
- **Proves it:** ext2/FAT read tests, `test_dirsweep`, disk-root persist
  (`make persisttest`) stay green; readahead still effective.
- **Effort:** L.

### Phase 3 — unify mmap on the cache
- **Scope:** replace `fops->mmap`-returns-phys (`syscall_mm.c:213-239`) and
  delete the copy path (`syscall_mm.c:242-277`) with cache-page lookup; MAP_SHARED
  maps cache page writable (`VM_NOFREE`), MAP_PRIVATE maps RO + existing COW
  (`vm.c:202-219`). Remove `mmap_pinned`/contiguous constraint
  (`ramfs.c:84,124,436`). Shared read-only text pages now shared across
  processes.
- **Risk:** **High** — mmap/read aliasing, shared-page refcount/free ordering
  (the COW free-while-mapped UAF class, `MEMORY.md`). This is where coherence
  bugs live.
- **Proves it:** `test_mmap`, `test_cowrace`, `test_aslr`, `test_dlopen`,
  `test_dynamic`, `test_execstress`, `test_hugepage`; add a read-vs-MAP_SHARED
  coherence assertion and a shared-text-page-count assertion (two processes
  mapping the same lib → one physical page).
- **Effort:** L.

### Phase 4 — write through cache + writeback (msync/fsync)
- **Scope:** route `vfs_write` (`vfs.c:1223`) through `filemap_write` (dirty the
  page); add ext2/FAT `write_folio`; implement `filemap_writeback` and make
  `sys_msync` MS_SYNC (`syscall_mm.c:588-605`) and `fsync`/`sync` real flushes;
  ramfs/tmpfs `write_folio` no-op. Closes MM-4 (MAP_SHARED writeback) for ext2.
- **Risk:** Medium — writeback ordering, partial-page RMW correctness, dirty
  accounting under concurrent writers.
- **Proves it:** write/persist tests, `make persisttest`, `test_fcntllock`,
  `test_flock`, `test_mtime`; crash-then-remount data integrity on ext2;
  MAP_SHARED-write→msync→read-back on ext2.
- **Effort:** M.

### Phase 5 — LRU lists (reclaim seam, no eviction)
- **Scope:** splice every `fpage` onto a global clean/dirty LRU on
  insert/dirty/clean; maintain `pg->refcount == 1` "unmapped & cache-only"
  invariant; export an `/proc` page-cache counter. **No eviction.**
- **Risk:** Low — list bookkeeping only; behavior unchanged.
- **Proves it:** suite green; counter sane. Unblocks roadmap §2 (reclaim) the
  moment it lands.
- **Effort:** S.

---

## 5. Sequencing notes

- **Do not overlap with the scheduler lock-split** (roadmap §3 step 4): both are
  invasive, and concurrent churn defeats the both-arch-green discipline
  (`MEMORY.md` autonomy/trust). The page cache is the keystone; land it as a
  contiguous block.
- **Block bio (roadmap §4)** is *synergistic, not required*: `read_folio`/
  `write_folio` issue page-sized `block_read(dev,lba,buf,count)` on today's sync
  layer (`block.c:10-27`) — multi-block-capable already. bio later makes each
  folio fill one async request instead of looping; do bio first only if
  convenient, but the cache does not block on it.
- **ext4 journaling (roadmap §5)** interacts only at writeback ordering (flush
  journal before data/metadata) — out of scope here; phase 4 leaves a clean hook.

## 6. Interaction with in-flight work

- **uaccess funnel / SMAP/PAN (#77, `docs/uaccess_funnel_inventory.md`):** the
  page cache **helps** this. Today every fop read/write `memcpy`s a *raw user
  pointer* (the RAW class, `uaccess_funnel_inventory.md` §2b: `ramfs.c:71,103`,
  `tmpfs.c:123,154`, `ext2.c:544,916`, `fat.c:281`). Once reads/writes funnel
  through `filemap_read`/`filemap_write`, the **only** user-pointer touch is the
  single `copy_to_user`/`copy_from_user` in filemap (§3.2) — the per-fs RAW
  sites collapse into one funnel point. Sequence: if the uaccess conversion
  lands first, filemap inherits clean accessors; if filemap lands first, it
  *reduces* the uaccess surface to convert. Either order is fine; they
  reinforce.
- **MAP_SHARED writeback (already done):** `sys_msync` today is a no-op that is
  *correct only* because the shared mapping IS the ramfs/tmpfs buffer
  (`syscall_mm.c:603`). Phase 3 preserves that for ramfs/tmpfs (write_folio
  no-op) and phase 4 makes it a real flush for ext2 — no regression, strict
  superset.
- **User-VA widening (`docs/user_va_widening_design.md`, the Node blocker):**
  independent. The page cache neither needs nor blocks it.

## 7. Verdict

- **Shape:** one epic, 5 internally-sequenced phases. Not several independent
  epics — phases 2-5 all depend on phase 1's structs — but each phase is a
  green, shippable increment.
- **Total effort:** **XL** (the single largest remaining foundation item).
  Phase 1 M, phase 2 L, phase 3 L, phase 4 M, phase 5 S.
- **Biggest risks, in order:** (1) **mmap/read aliasing + shared-page
  refcount/free ordering** in phase 3 — the COW free-while-mapped UAF class
  (`MEMORY.md`); mitigated by reusing the proven `pmm` refcount + `VM_NOFREE`
  seam (`vm.c:192-194`, `pmm.c:33-36`) rather than inventing new sharing.
  (2) **Writeback ordering / partial-page RMW** in phase 4. (3) lost-wakeup on
  `FP_LOCKED` I/O waits (phase 1) — use the established sleep/wake discipline.
- **Node (#43):** **not a prerequisite.** The hard Node blocker is the user-VA
  arena (`docs/nodejs_readiness.md:39,195`), not the cache. Node can ship first;
  the page cache then delivers shared read-only text (lower RSS for many Node
  workers) and coherent mmap+read as a follow-on quality improvement.
- **Single highest-value first step:** Phase 1 — `address_space` + `filemap_read`
  routed through ramfs/tmpfs. Internally invisible, low risk, and it lays the one
  set of structures (`i_mapping`, radix, `fpage`, `vfs_aops_t`) that every later
  phase — and reclaim (roadmap §2) — builds on.
