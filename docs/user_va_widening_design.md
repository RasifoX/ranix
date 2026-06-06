# User Virtual-Address Widening Design (task #68)

Status: design only (READ-ONLY analysis). No code changed by this document.

## 1. Problem

Ranix caps the usable user virtual-address span at **2 GB**
(`ARCH_USER_STACK_TOP = 0x80000000`, `arch/x86_64/include/asm/mmu.h:17`,
`arch/aarch64/include/asm/mmu.h:14`). Linux gives userspace 128 TB
(low-canonical 47-bit) on both architectures. The immediate, hard blocker is
Node/V8: its pointer-compression heap **cage** reserves a single contiguous
~4 GB `PROT_NONE` region up front, which cannot exist under a 2 GB ceiling.

This is the lone remaining blocker for that workload and a glaring parity gap.

## 2. Current layout (per arch)

Both arches share the same user constants, so the user layout is identical; only
the page-table machinery and the kernel half differ.

### 2.1 User VA regions (both arches, today)

| Region | VA | Source |
| --- | --- | --- |
| ELF text/PIE load base | `0x400000` (fixed, even for PIE) | `kernel/elf.c:52`, `ARCH_USER_CODE_BASE` |
| brk / heap | grows up from `load_end` | `kernel/proc.c:169`, `sys_brk` `kernel/sys/syscall_mm.c:30` |
| vvar page | `0x3FFF0000` | `include/uapi/vdso.h:12` |
| vDSO image | `0x3FFF1000` | `include/uapi/vdso.h:13` |
| mmap window base | `0x40000000` (1 GB), bump-up | `ARCH_USER_MMAP_BASE`, `p->mmap_next` `kernel/proc.c:170` |
| mmap window ceiling | `0x80000000 − 256·4K` ≈ 2 GB − 1 MB | `kernel/sys/syscall_mm.c:169-170` |
| 2 MB hugetlb window | shares the mmap window (`p->mmap_next`) | `kernel/sys/syscall_mm.c:130-133` |
| user stack | top `0x80000000` (2 GB), 256 pages = 1 MB | `ARCH_USER_STACK_TOP/PAGES`, `kernel/proc.c:204-230` |
| arg/env window | top 64 pages (256 KB) of the stack | `ARG_WIN_PAGES=64` `kernel/sys/syscall_proc.c:1082` |
| **1 GB hugetlb window** | base `0x100000000` (**4 GB**), bump-up | `HUGE1G_VA_BASE` `include/kernel/mm/vm.h:21`, `p->huge1g_next` `kernel/proc.c:171` |

Note the 1 GB-hugepage window already sits **above** the 2 GB stack (at 4 GB),
proving the page tables and `access_ok` already function above 2 GB — the 2 GB
number is purely a layout/policy choice, not a hardware or paging limit. The
comment at `include/kernel/mm/vm.h:18-20` ("clear of … stack (both under 2 GB)")
documents exactly this assumption.

### 2.2 Kernel half / split

* **x86_64**: kernel is high-half. Direct map at `DIRECT_MAP_BASE =
  0xFFFF800000000000` (PML4 slot 256+), kernel text at
  `0xFFFFFFFF80000000` (PML4 slot 511), ioremap at `0xFFFFFFFFC0000000`
  (`arch/x86_64/include/asm/mmu.h:4-5`, `kernel/mm/vm.c:51`). **All user VA
  lives under PML4 slot 0** — `vm_create`/`vm_copy`/`vm_destroy` only touch
  index 0 (`arch/x86_64/vm.c:115`, `:399`, and the la57 `pml5[0]` user slot
  `:78`). Under 5-level the user PML4 hangs off `pml5[0]`; kernel mappings come
  from `pml5[504]`/`pml5[511]`.
* **aarch64**: TTBR0 (user) and TTBR1 (kernel) are **separate registers**
  (`arch/aarch64/mmu_asm.S:13-14`). TTBR1 roots the kernel at L0 slot 256+
  (`arch/aarch64/mmu.c:6-7`, `VIRT_OFFSET = 0xFFFF800000000000`). User VA lives
  entirely under the TTBR0 L0 table, and `vm_create` only populates `L0[0]`
  (`arch/aarch64/vm.c:119`). **Widening TTBR0 cannot touch TTBR1** — different
  register, different table root.

## 3. What the paging hardware already supports

* **aarch64**: `TCR_EL1` is set to `T0SZ=16 T1SZ=16` — i.e. **full 48-bit /
  256 TB TTBR0** is already configured (`arch/aarch64/mmu_asm.S:9`). No TCR
  change is needed to reach 256 TB of user VA. The only limit is software: the
  L0 table has 512 slots (one per 512 GB), `vm_create` wires `L0[0]`, and
  `vm_copy`/`vm_destroy` walk **only `L0[0]`** (`arch/aarch64/vm.c:466-471`,
  `:146-149`). `l1_for` (`:52`) *will* lazily allocate other L0 slots on a
  `vm_map` above 512 GB, but those slots are never copied on fork and never
  freed on exit → **leak + COW-bypass UAF** above 512 GB today.
* **x86_64**: `get_or_alloc` (`arch/x86_64/vm.c:162`) builds all four levels
  (PML4→PDPT→PD→PT) for an arbitrary user VA via the `(vaddr>>39)&0x1FF` PML4
  index, so `vm_map` itself already reaches the full 512 GB of PML4 slot 0 — and
  technically other slots. But `vm_copy`/`vm_destroy` walk **only PML4 index 0**
  (`for (i=0;i<1;i++)`, `:115`, `:399`), so the safe reach is **one PML4 slot =
  512 GB**. la57 (5-level) only adds a PML5 layer above an otherwise-identical
  walk; it does not change the user reach (still `pml5[0]`→one PML4).

**Conclusion: both arches can safely address the first 512 GB of user VA with no
page-table changes, as long as the new ceiling stays inside PML4/L0 slot 0.**
256 GB or even just-under-512 GB is "free" from the walker's perspective.

## 4. Things that assume the 2 GB ceiling / fixed layout

| # | Assumption | Location | Impact of widening |
| --- | --- | --- | --- |
| A | `access_ok` bounds user range only against `VIRT_OFFSET`, **not** 2 GB | `kernel/lib/string.c:69-78` | **None / already correct** — any VA below the kernel half already passes. No change needed. |
| B | mmap bump ceiling = `STACK_TOP − STACK_PAGES·PAGE` | `kernel/sys/syscall_mm.c:169-170`, `:130-133` | Must rise with the new ceiling; this is what gates the cage. |
| C | mmap base = 1 GB | `ARCH_USER_MMAP_BASE`, `kernel/proc.c:170`, `kernel/sys/syscall_proc.c:1202` | Should move so cage fits below stack; raise. |
| D | 1 GB hugetlb window at 4 GB | `HUGE1G_VA_BASE`, `kernel/sys/syscall_mm.c:94`, `vm.h:21` | **Collides** with a widened mmap window — must relocate above the new mmap ceiling. The comment at `vm.h:18-20` is the canary. |
| E | stack fixed 256 pages (1 MB) at fixed top; fault grows only inside that 1 MB window | `kernel/mm/vm.c:86-87`, `:114`; `kernel/proc.c:204` | Stack top moves with ceiling; auto-grow band still hardcoded to `MAX_STACK_SIZE = 256·PAGE` — fine for the cage, but V8/threads want bigger. |
| F | vvar/vDSO at fixed `0x3FFF_0000` (just below 1 GB mmap base) | `include/uapi/vdso.h:12-13` | If mmap base drops below 1 GB, vDSO could collide; if mmap base only rises, no collision. Keep vDSO where it is or move with mmap base. |
| G | ASLR mmap entropy = 16 bits, `(rng & 0xffff)<<12` | `kernel/sys/syscall_proc.c:1202-1203` | Capped by 2 GB today; widening *enables* more entropy (see §7). |
| H | PIE main image fixed at `0x400000` (no bias) | `kernel/elf.c:52`, execve passes bias 0 | Independent of ceiling; widening lets a future PIE bias use the new span. |
| I | `vm_copy`/`vm_destroy` walk only slot 0 | `arch/x86_64/vm.c:115`,`:399`; `arch/aarch64/vm.c:146`,`:471` | Hard limit at 512 GB. Must extend the walk to lift beyond 512 GB. |
| J | Address typing | `process_t.brk/mmap_next/huge1g_next/vaddr` all `uint64_t` (`include/kernel/proc.h:145,151,152`); `vma_t.vaddr` `uint64_t`; mmap returns `long` | **No 32-bit truncation anywhere.** The `uint32_t`s in `syscall_mm.c` are flag bitmasks, not addresses. No widening risk. |
| K | PROT_NONE "reservation" just unmaps pages and records a VMA | `kernel/sys/syscall_mm.c:175-178`, `:189-197` | A PROT_NONE mmap consumes `mmap_next` and adds a VMA, so the bump allocator won't reissue the VA — adequate as a cage reservation. |

## 5. Target layout

**Recommended ceiling: 256 GB** (`ARCH_USER_STACK_TOP = 0x40_0000_0000`).

Rationale:
* Comfortably inside PML4/L0 **slot 0 (512 GB)** → **zero page-table-walk
  changes** on either arch (§3); the riskiest class of change is avoided.
* 128× the 4 GB cage requirement, with megabytes of slack for stack, vDSO, and
  multiple cages/threads.
* Gives ASLR a 38-bit span to draw from (vs 31 today).
* aarch64 needs no TCR change (already 48-bit); x86 needs no la57 change.

Full Linux-scale 128 TB (47-bit) is deferred: it requires extending the
`vm_copy`/`vm_destroy` walk across many PML4/L0 slots (item I) and a sparse
walk to avoid O(slots) fork cost — real work for no Node benefit. 256 GB is the
pragmatic interim; 128 TB can follow as a separate epic if a workload demands
it.

### Proposed region map (256 GB ceiling)

| Region | Proposed VA |
| --- | --- |
| ELF/PIE base | `0x400000` (unchanged; PIE bias optional later) |
| brk/heap | from `load_end` (unchanged) |
| vvar/vDSO | unchanged at `0x3FFF_0000` (still below mmap base) |
| mmap base | keep `0x40000000` (1 GB) **or** raise to e.g. `0x10_0000_0000` (64 GB) to leave a clean low gap for cages; either works |
| mmap ceiling | `STACK_TOP − stack reserve` ≈ 256 GB − a few MB |
| 1 GB hugetlb window | **relocate** to just under the stack, e.g. base `0x3F_0000_0000` (252 GB), above the mmap ceiling |
| stack top | `0x40_0000_0000` (256 GB) |
| stack auto-grow band | unchanged 1 MB (raise later for V8 threads) |

Constraint: `mmap_ceiling < HUGE1G_VA_BASE_new < stack_bottom`, and all of it
`< 512 GB` so it stays in slot 0.

## 6. Staged plan (each step both-arch-green, independently landable)

**Step 1 — Extend/verify page-table reach (no behavior change).**
Confirm (and, if going past 512 GB later, fix) that `vm_copy`/`vm_destroy` cover
the chosen ceiling. For the 256 GB target this is a **verification step only**:
everything stays in slot 0, so x86 `for(i<1)` and aarch64 `L0[0]`-only are
already sufficient. Land a comment/assert documenting the slot-0 invariant.
Test: existing `test_fork`/`test_cowrace`/`test_mmap` unaffected.

**Step 2 — Relocate the 1 GB hugetlb window (item D).**
Change `HUGE1G_VA_BASE` from 4 GB to its new home above the future mmap ceiling
(e.g. `0x3F_0000_0000`). Must precede Step 3 so the widened mmap window never
collides with the hugepage window.
Test: `test_hugepage1g`, `test_hugepage`.

**Step 3 — Raise the ceiling + mmap window (items B, C, E-top).**
Bump `ARCH_USER_STACK_TOP` to `0x40_0000_0000` on both arches; the mmap ceiling
formula (`syscall_mm.c:169-170`, `:130-133`) follows automatically. Optionally
raise `ARCH_USER_MMAP_BASE`. Stack setup in `proc.c:204-230` and
`syscall_proc.c:1238-1356` rebases off `ARCH_USER_STACK_TOP`, so it moves for
free. **This is the step that unblocks the V8 cage.**
Test: `test_mmap`, `test_mmsweep`, `test_mmapshared`, `test_dynamic`,
`test_aslr`; the cage acceptance check (§8).

**Step 4 — Wider ASLR (item G).**
Widen the mmap-base randomization mask from `0xffff` to use the new span
(`syscall_proc.c:1203`), and optionally give the PIE main image a randomized
load bias (`elf.c:52` + execve callsite). Independent of the cage; security win.
Test: `test_aslr` (entropy assertions), `test_dynamic`.

**Step 5 (optional, later) — Stack auto-grow to RLIMIT_STACK (item E).**
Replace the fixed `MAX_STACK_SIZE = 256·PAGE` band (`kernel/mm/vm.c:87`,`:114`)
with growth bounded by `RLIMIT_STACK`, so deep/thread stacks grow on fault like
Linux. Not required for Node's cage; needed for large native stacks.
Test: a new deep-recursion stack-grow test; `test_pthread`/thread tests.

## 7. ASLR entropy note

Today mmap entropy is 16 bits (`(rng_next() & 0xffff) << 12`,
`syscall_proc.c:1203`), and `security_audit.md:31` flags that the **2 GB
ceiling caps total entropy**. A 256 GB ceiling permits ~26 page-bits of mmap
randomization and a randomized PIE bias (`elf.c:52`, currently fixed at
`0x400000`, flagged `security_audit.md:32`). Step 4 turns the widening into a
concrete ASLR improvement. (Orthogonal: the ASLR PRNG is non-cryptographic
splitmix64 — out of scope here.)

## 8. Risk map & acceptance

| Step | What could break | Mitigation / test |
| --- | --- | --- |
| 1 | None (verification) | fork/COW/mmap regression suite |
| 2 | A program hardcoding the old 4 GB 1 GB-hugepage VA (none in tree) | `test_hugepage1g` |
| 3 | mmap/stack/hugepage VA collisions; stack guard placement; arg-window math (`ARG_WIN_BYTES` vs new top) | `test_mmap`, `test_mmsweep`, `test_dynamic`, `test_aslr`, stack smoke; verify guard page at `STACK_TOP − (PAGES+1)·PAGE` |
| 4 | ld.so/PIE relocation breakage from a too-wide or misaligned base | `test_dynamic`, `test_aslr` |
| 5 | Stack grow racing the mmap window; unbounded growth | RLIMIT_STACK bound + guard gap; deep-recursion test |

**V8-cage acceptance check:** a process must be able to
`mmap(NULL, 4 GiB, PROT_NONE, MAP_ANON|MAP_PRIVATE)` and get a valid pointer,
then `mprotect` sub-regions to RW and touch them without SIGSEGV, then
`munmap` cleanly. Today this returns `-ENOMEM` at `syscall_mm.c:170` because
`mmap_next (≈1 GB) + 4 GB > ceil (≈2 GB)`. After Step 3 it succeeds. This single
test is the task #68 done-criterion.

## 9. Minimum change to unblock Node specifically

Node's cage needs ~4 GB contiguous below the stack. The minimal cut is
**Steps 2 + 3 with a modest ceiling**: set `ARCH_USER_STACK_TOP` to e.g.
`0x4_0000_0000` (16 GB) and relocate `HUGE1G_VA_BASE` just under it. That alone
satisfies the 4 GB-cage acceptance check on both arches, stays trivially inside
slot 0, and requires no page-table-walk changes. The 256 GB target in §5 is the
recommended *clean* landing (more headroom, real ASLR win); 16 GB is the
bare-minimum unblock if conservatism is preferred. Steps 1, 4, 5 are not
required for Node.

## 10. Single riskiest step

**Step 3 (raise the ceiling + mmap window).** It is the one step that changes
observable user-visible geometry: every hardcoded offset relative to
`ARCH_USER_STACK_TOP` (stack pages, guard page, arg/env window, mmap ceiling
formula, `vm_handle_fault`'s stack-grow band at `kernel/mm/vm.c:114`) must move
coherently on **both** arches at once, and a mistake manifests as silent stack
corruption or SIGSEGV at process start rather than a clean error. Steps 1, 2, 4
are mechanically contained; Step 3 touches the live stack/mmap boundary that
every process exercises on its first instruction.
