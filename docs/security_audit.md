# Ranix kernel security posture audit

Scope: a read-only security/hardening review of Ranix (dual-arch x86_64 +
aarch64, musl userspace) measured against the Linux baseline. This dimension is
not covered by the existing audit docs; where they overlap (copy_user safety,
buddy hardening, seccomp cBPF, getrandom) they are cross-referenced rather than
re-derived. Severity reflects exploitability: privilege-escalation and info-leak
first.

A recurring theme: Ranix has a *software* permission model (VMA flags, the COW
fault handler, credential checks) that is broadly correct, but the *hardware*
enforcement that backs it on Linux — NX, SMEP/SMAP, KPTI, RO kernel text — is
largely absent. The software checks that exist are good; the missing hardware
floor is where the posture is soft.

---

## 1. W^X / NX

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| User-page NX (no exec on data/heap/stack) | **none** — the NX/XD bit is never defined or set; `vm_map` ignores `VM_EXEC` entirely (`arch/x86_64/vm.c:182-205` builds the PTE from VM_WRITE/USER/NOCACHE/NOFREE only; aarch64 `arch/aarch64/vm.c:227-234` sets no UXN/PXN). No `PTE_NX`/`PTE_UXN`/`PTE_PXN` macro exists (`arch/*/include/asm/mmu.h`). | Every non-exec page mapped NX; W^X enforced in HW. | All user pages are CPU-executable regardless of `PROT_EXEC`. Stack, heap, anonymous data are runnable → trivial shellcode/return-to-data. | **Critical** | The fault handler records `VMA_EXEC` and enforces write-protection in software (`kernel/mm/vm.c:121-122,176-190`) but no execute fault can occur because no PTE is NX. |
| `mprotect` W^X rejection | **none** — `sys_mprotect` (`kernel/sys/syscall_mm.c:295-356`) accepts `PROT_WRITE|PROT_EXEC` and maps the page; there is no EACCES/EINVAL for W+X. | `mprotect` honored by NX HW; SELinux/`execmem` policy can deny W→X transitions. | A region can be writable and executable simultaneously. The "W^X fix" referenced in memory is **not present** in this tree for `mprotect`/`mmap`. | **Critical** | Verified: no W+X check anywhere in `syscall_mm.c`/`syscall.c`/`vm.c`. |
| Kernel `.text` read-only | **none** — boot page tables map the kernel high-half with `0x83` (P+RW+PS) (`arch/x86_64/boot.S:94-133` pdpt_hh→pd; aarch64 boot maps text section without RO/PXN split). | `.text`/`.rodata` mapped RO, RX; `.data` NX. | Kernel code is writable and the entire direct map is RWX → a single kernel write primitive becomes code execution; no defense-in-depth. | **Critical** | Direct map filled `0x83` per 1 GB (`arch/x86_64/mmu.c:54`, `boot.S` `.fill_dm`). |
| Kernel direct-map NX | **none** (same RWX direct map). | Direct map NX on Linux (CONFIG_DEBUG_RODATA / strict RWX). | Kernel data pages are executable. | High | — |

## 2. ASLR

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| mmap base / ld.so load | **partial** — `mmap_base = USER_MMAP_BASE + ((rng_next()&0xffff)<<12)` (`kernel/sys/syscall_proc.c:1173-1175`): **16 bits**, page-granular. ld.so loads at this base (`:1196-1203`). | 28-32 bits of mmap entropy on 64-bit. | Only 16 bits, and the PRNG is **non-cryptographic** splitmix64 seeded from `kernel_ticks` (`kernel/syscall.c:187-198`) → brute-forceable / predictable. The 2 GB user VA (`ARCH_USER_STACK_TOP = 0x80000000`, `arch/*/include/asm/mmu.h`) caps total entropy. | High | mmap region is otherwise a pure bump allocator `p->mmap_next` (`syscall_mm.c:160-163`) — no per-mapping randomization, no gaps. |
| Main executable (PIE) load bias | **none** — `elf_load` uses `load_bias_hint ? hint : 0x400000` (`kernel/elf.c:52`); execve passes `0` for the main image (`syscall_proc.c` elf_load(...,0,...)), so ET_DYN/PIE binaries still land at the fixed `0x400000`. | PIE load randomized (28-32 bits). | The main binary's text/PLT/GOT are at a fixed, known address even for PIE. Defeats ASLR for the primary ROP target set. | High | `load_bias_hint` is only ever the mmap_base, used for the interpreter, not the executable. |
| Stack base | **partial** — `sp = USER_STACK_TOP - ((rng_next()&0xff)<<4)` (`syscall_proc.c:1339`): only the low byte, 16-byte granular → **8 bits, sub-page**. | ~30 bits, page-aligned + sub-page. | Stack is in a single known page; 8 bits of in-page jitter only. Trivially defeated. | High | Same predictable PRNG. |
| brk / heap | **none** — `p->brk` starts at the fixed image end; `sys_brk` never randomizes (`kernel/sys/syscall_mm.c:30-53`). | brk randomized relative to image (~13 bits). | Heap base deterministic. | Med | — |
| vDSO | **none of note** — vDSO mapped without independent randomization (`kernel/vdso.c`). | vDSO randomized. | vDSO gadget addresses predictable. | Med | — |
| KASLR (kernel base) | **none** — kernel linked/loaded at fixed VA; no relocation in `arch/*/boot.S` or `kernel/main.c`. | KASLR (kernel + modules). | Kernel symbol addresses fixed and known (and `ksyms`/`/proc/kallsyms` may expose them). | High | Combined with RWX kernel + no SMEP, a leak is immediately exploitable. |

## 3. CPU hardening

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| x86 SMEP (CR4.20) | **none** — `arch_syscall_init` reads CR4 and writes it **back unchanged** with a "for now just ensure they are handled" comment (`arch/x86_64/cpu.c:48-53`). SMEP/SMAP bits never set. | SMEP enabled when CPUID supports it. | Kernel can fetch+execute user pages → classic ret2usr. With no KASLR and RWX kernel this is the easiest privesc primitive. | **Critical** | — |
| x86 SMAP (CR4.21) + STAC/CLAC | **none** — same no-op; no `stac`/`clac` around user copies (`grep` finds none). | SMAP + AC toggling in copy_user. | Kernel can implicitly read/write user memory anywhere; weakens accidental-deref containment. | High | `copy_*_user` rely on `access_ok` range check only. |
| x86 UMIP | **none** | UMIP set to block SGDT/SIDT/etc. from user. | User can read GDT/IDT/TR base → KASLR-style leaks. | Med | — |
| x86 CET / shadow stack / IBT | **none** | Opt-in CET. | No HW CFI. | Low | Linux itself is opt-in; low priority. |
| x86 KPTI / Meltdown | **none** — single PML4 with kernel half shared into every user CR3 (`arch/x86_64/vm.c` user_pml4 + shared kernel slots); no separate user page table. | KPTI (separate user/kernel CR3) on affected CPUs. | On Meltdown-class CPUs, user can read kernel memory via transient execution. | Med | Hardware-dependent; flag for affected targets. |
| aarch64 PAN | **none** — `arch_user_access_begin/end` are empty, comment "PAN not supported on Cortex-A72" (`arch/aarch64/cpu.c:46-53`). PAN *is* ARMv8.1 (A72 is v8.0, so genuinely absent on Pi4 silicon). | PSTATE.PAN set where available. | Kernel may deref user pointers freely. On A72 this is a HW limitation, not a Ranix bug — but no software mitigation either. | Low (HW-bound) | Other v8.1+ targets would benefit; currently unconditionally off. |
| aarch64 PXN/UXN (W^X) | **none** — see §1; no UXN/PXN bit set in any PTE. | PXN on user pages, UXN on kernel/data. | User code executable from kernel mode is impossible to prevent; kernel data executable. | **Critical** | Folds into the §1 NX gap. |
| aarch64 BTI / PAC | **none** — no `bti`/`pac*` setup; SCTLR_EL1 BT/EnIA bits untouched (`arch/aarch64/boot.S:74-78`). | BTI + PAC where supported. | No HW CFI/return-address signing. | Low | A72 lacks PAC anyway. |
| aarch64 WXN (SCTLR_EL1.WXN) | **none** — SCTLR programmed without WXN (`arch/aarch64/boot.S:74-78,158-162`). | WXN can be set to force W⇒XN globally. | A cheap arch-level W^X is available and unused. | High | Setting SCTLR_EL1.WXN would give kernel W^X almost for free; see Top-10. |

## 4. Credentials

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| setuid/setgid/setre*/setres* + saved-set | **have, mostly correct** — `setreuid`/`setresuid` enforce the {r,e,s}uid membership rule for non-root (`kernel/sys/syscall_misc.c:428-462+`). | Full POSIX semantics. | Looks conformant on review; not exhaustively fuzzed. | Low | — |
| Capabilities (capabilities(7)) | **partial** — real cap sets exist: `cap_effective/permitted/inheritable`, `capget`/`capset` with the permitted-superset and inheritable rules (`kernel/sys/syscall_misc.c:94-147`). | Full 40+ caps, bounding set, ambient, file caps. | Many checks still gate on `euid==0` rather than a specific CAP (e.g. `setns` ns==2 requires `euid!=0` → EPERM, `syscall_misc.c:183`); no file capabilities, no bounding/ambient set, `PR_CAPBSET_*` is a no-op (per syscall_conformance_audit `prctl :86`). | Med | The model is present but coarse; root is still effectively all-powerful and most privilege gates are uid-based. |
| no_new_privs honored at exec | **none (broken)** — `no_new_privs` is stored by `prctl(38)` (`syscall_misc.c:1080-1085`) but **execve never consults it**: S_ISUID/S_ISGID are applied unconditionally (`syscall_proc.c:1141-1144,1272-1274`). | NNP blocks suid/file-cap elevation across exec. | A sandbox that sets NNP (the seccomp precondition, container runtimes) still gets suid elevation on the next exec. | High | Security-relevant flag silently ineffective. |
| suid/sgid exec elevation | **have** — euid/egid set from file owner on S_ISUID/S_ISGID (`syscall_proc.c:1141-1144,1272-1281`); if new euid==0 caps kept. | Same, plus secure-exec hardening (AT_SECURE, env scrub, NNP gate, file caps). | No AT_SECURE / no insecure-env scrubbing on suid exec; no NNP gate (above). | High | Missing the privilege-clears-on-non-owner / secure-exec scrub path means a suid binary inherits attacker-controlled `LD_*`, etc. (mitigated only if libc ignores them, which is fragile). |
| Supplementary groups | **partial** — `setgroups`/`getgroups` present; verify the array is consulted in VFS permission checks. | Full ngroups in access checks. | Group-based access may not consult the supplementary list everywhere. | Med | Not exhaustively traced here. |
| fsuid/fsgid | **none** — `/proc/<pid>/status` reports `fsuid = euid` literally (`kernel/fs/procfs.c:199`); no independent `setfsuid`/`setfsgid` state. | Distinct fsuid/fsgid. | NFS-style fs-only credential split absent. | Low | — |

## 5. ptrace scope

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| PTRACE_ATTACH permission | **none (broken)** — `PTRACE_ATTACH` only checks `tracee->tracer != 0` (already-traced) (`kernel/sys/syscall_ptrace.c:86-92`). **No uid/euid match, no CAP_SYS_PTRACE, no dumpable check.** | Tracer must match credentials (ptrace_may_access) or have CAP_SYS_PTRACE; Yama scope. | **Any process can attach to any other**, single-step it, and read/write its memory and registers → full cross-user info-leak and privesc (attach to a root process). | **Critical** | PEEK/POKE later guards `tracee->tracer == p->pid` (`:123`), but the initial attach is unrestricted. |
| PTRACE_TRACEME | present (`:0`); sets tracer to parent. | Same; NNP/Yama interplay. | Benign. | Low | — |
| Yama ptrace_scope | **none** | `/proc/sys/kernel/yama/ptrace_scope`. | No restricted-attach mode even as a tunable. | Med | Folds into the ATTACH gap. |

## 6. seccomp

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| Enforced on every syscall | **have** — `syscall_handle` runs the filter centrally before dispatch (`kernel/syscall.c:288-326`); STRICT allows only read/write/exit/exit_group/sigreturn. | Same. | Good — single chokepoint, fail-closed. | — | Both arches share `syscall_handle`. |
| cBPF completeness | **partial** — interpreter is a cBPF subset; per syscall_conformance_audit (`:142`) it lacks ALU MUL/DIV/LSH/RSH, X-register JMPs, BPF_MEM; unknown insn → KILL (fail-closed). | Full cBPF. | A real libseccomp filter may use unsupported insns → the whole process is killed (safe but breaks legitimate sandboxes; or a hand-rolled filter mis-evaluates). | Med | Fail-closed direction is correct. |
| SECCOMP_RET_* actions | **partial** — ALLOW / KILL / ERRNO mapped (`syscall.c:248-283`); TRACE/TRAP/LOG/USER_NOTIF coverage unverified. | All RET actions. | TRACE/USER_NOTIF (used by modern container runtimes) likely unsupported. | Med | — |
| Filter flags (TSYNC/LOG/SPEC_ALLOW) | **none** — flags ignored (syscall_conformance_audit `:142`). | Honored. | No TSYNC → filters don't apply to sibling threads atomically. | Med | Thread-sync gap can leave threads unfiltered. |

## 7. Resource / DoS limits

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| RLIMIT_NOFILE | **have** — enforced via `fds->rlim_nofile` (`kernel/sys/syscall_fs.c:591-618`). | Yes. | — | Low | — |
| RLIMIT_AS | **have** — mmap sums VMA sizes and refuses over soft limit (`kernel/sys/syscall_mm.c:64-70`). | Yes. | Only checked in mmap (not brk, not stack growth). | Low | — |
| RLIMIT_NPROC / fork-bomb | **none** — no per-uid process count, no RLIMIT_NPROC enforcement in fork (`kernel/proc.c`, `syscall_proc.c`); fork only fails on OOM. | Per-uid NPROC cap. | A fork bomb is bounded only by PID space + memory → trivial local DoS. | High | — |
| PID exhaustion | **partial** — PID allocator (`kernel/pid.c`); no per-user cap, relies on global PID_MAX. | pid_max + NPROC. | Unprivileged user can exhaust the global PID table. | Med | — |
| RLIMIT_STACK / DATA / CPU / NOFILE-others | **mostly none** — `prlimit64` stores values but few are enforced (syscall_conformance_audit). | Enforced. | Stored-but-ignored limits give false sense of containment. | Med | — |
| Kernel-memory exhaustion from user allocs | **partial** — cgroup memory charging on mmap (`syscall_mm.c` `cgroup_charge_mem`); but unbounded kernel allocations exist (e.g. execve `kmalloc(st.size)` of the **whole binary**, `syscall_proc.c:1147`; whole interpreter too). | Bounded / accounted. | A large binary forces a large contiguous kernel alloc; user-controlled sizes in several paths. | Med | execve reads the entire ELF into a kmalloc buffer. |

## 8. Memory-safety surface

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| Kernel stack canary | **have** — `KSTACK_CANARY` stamped at stack base, checked on syscall entry and context switch (`kernel/sched.c:83-104,748`; `kernel/syscall.c:294-301`). | Stack guard / VMAP_STACK guard pages. | Solid for overflow *detection*; but it's a fixed magic word (`0xDEADBEEFCAFEBABE`), not random, and there is **no guard page** below the kernel stack (overflow detected only after the fact, and only if it hits the base word). | Med | Detection-only, post-hoc; a deep overflow that skips the base word is missed. |
| User stack guard page | **partial** — stack is a fixed `ARCH_USER_STACK_PAGES` (256) region; mmap refuses to overlap it (`syscall_mm.c:160-162`), but there is no explicit unmapped guard below it. | Guard gap + auto-grow with guard. | A user stack overflow runs into whatever is mapped below rather than faulting cleanly. | Med | — |
| AT_RANDOM canary (userspace) | **have, good** — 16 cryptographic bytes drawn from the CSPRNG (`kernel/sys/syscall_proc.c:1357-1396`, `csprng_bytes`). | Same. | None — this is correct and a genuine strength. | — | Distinct from the predictable ASLR PRNG. |
| CSPRNG backing getrandom / /dev/urandom | **have, good** — `getrandom` and `/dev/[u]random` use `csprng_bytes` (`kernel/sys/syscall_misc.c:1130-1139`, `kernel/fs/devfs.c:79-80`, `kernel/lib/csprng.c`). | Yes. | **Supersedes** `syscall_conformance_audit.md:141`, which describes the old splitmix64 path — that doc is stale; getrandom is now CSPRNG-backed. | — | The non-crypto splitmix64 (`syscall.c:187`) now feeds ASLR only, not key material. |
| Buddy poison + double-free guard | **have, good** — poison-on-free + verify-on-alloc + double-free guard (`kernel/mm/buddy.c:37-152`). | SLUB debug / page poison (opt-in). | None — a real strength; confirmed by linux_parity_audit `:222`. | — | — |
| Heap (slab) integrity | **have** — per-alloc magic + slab page magic, bad-ptr kfree detection (`kernel/mm/heap.c:18-160`). | SLUB redzone/freelist hardening. | Magic check only; no freelist pointer obfuscation or full redzone. | Low | — |
| copy_to/from_user bounds | **have, clean** — `access_ok` rejects any range touching ≥ VIRT_OFFSET and overflow (`kernel/lib/string.c:69-90`); fault-safe `__copy_user`. Confirmed clean by latent_hazards_audit `:128`. | Same. | None functionally; weakened only by the absence of SMAP (§3). | — | No TOCTOU: copy uses the fault-safe primitive, not a re-deref. |

## 9. /dev & /proc exposure

| Mechanism | Ranix | Linux baseline | Gap | Severity | Notes |
|---|---|---|---|---|---|
| /dev/mem, /dev/kmem | **absent (good)** — not registered in devfs builtins (`kernel/fs/devfs.c:239-241`); grep finds no `/dev/mem` provider. | Present, root + CAP_SYS_RAWIO + STRICT_DEVMEM gated. | None — not exposing physical memory is the safe default. | — | Strength by omission. |
| /proc/<pid>/mem | **have, gated** — readable only by the tracer (PEEK path checks `tracee->tracer == p->pid`, `syscall_ptrace.c:123`); `procfs_mem_inode` (`procfs.c:1463`). | ptrace_may_access gated. | But the ATTACH gap (§5) means any process can *become* the tracer → effectively any process can read any other's /proc/<pid>/mem. | High | Severity inherited from the ptrace ATTACH hole. |
| /proc/<pid>/maps cross-process | **exposed, ungated** — `fill_proc_maps` does `find_proc(pid)` for any pid with **no uid/permission check** (`kernel/fs/procfs.c:326-352`). | Readable per ptrace_may_access; cross-uid restricted. | Any process can read any other's full VMA layout → defeats what little ASLR exists, info-leak for exploitation. | High | Compounds the weak ASLR (§2). |
| kptr_restrict | **none** — `ksyms`/kallsyms-style symbol exposure not pointer-restricted; no `kptr_restrict` knob. | `kptr_restrict` masks %pK. | Kernel pointers/symbol addresses leak to unprivileged users → defeats (absent) KASLR. | Med | — |
| dmesg_restrict | **none** — kernel log (`kprintf`/`syslog`) not access-restricted by a tunable. | `dmesg_restrict`. | Kernel addresses/diagnostics in the log readable by unprivileged users. | Med | — |

---

## Top 10 security gaps to close (ranked)

1. **No NX anywhere (user + kernel) — W^X is fiction.** `VM_EXEC` is dropped on the floor in both arches' `vm_map`; the NX/UXN/PXN bit is never defined or set; the kernel direct map and `.text` are RWX (`arch/x86_64/vm.c:182-205`, `arch/aarch64/vm.c:227-234`, `arch/x86_64/boot.S:94-133`). Any memory-write bug becomes code execution; shellcode on the stack/heap just runs. **Critical.** (Define `PTE_NX`/`PTE_UXN|PTE_PXN`, set it when `!VM_EXEC`, and split kernel sections RO-text/NX-data. On aarch64, `SCTLR_EL1.WXN` is a near-free kernel-side stopgap.)

2. **ptrace PTRACE_ATTACH has no permission check.** Any process attaches to any other regardless of uid (`kernel/sys/syscall_ptrace.c:86-92`). Direct cross-user privilege escalation and memory disclosure (attach to a root daemon, POKE it). **Critical.** (Require euid match or CAP_SYS_PTRACE; add a Yama-style scope.)

3. **x86 SMEP/SMAP not enabled — ret2usr is open.** `arch/x86_64/cpu.c:48-53` reads CR4 and writes it back unchanged. With RWX kernel and no KASLR, a kernel control-flow bug trivially redirects into a user-controlled page. **Critical.** (Set CR4.SMEP/SMAP when CPUID advertises them; add STAC/CLAC to copy_user.)

4. **mprotect/mmap accept W+X.** No rejection of `PROT_WRITE|PROT_EXEC` (`kernel/sys/syscall_mm.c`); the "W^X fix" in project memory is not in this tree. **High→Critical** (pairs with #1). (Reject W+X unless an explicit `memfd`/MAP_JIT-style opt-in; at minimum gate behind a sysctl.)

5. **no_new_privs ignored by execve → suid elevation in sandboxes.** Stored but never checked; S_ISUID always applies (`syscall_proc.c:1141-1144,1272-1281`). Also no AT_SECURE / insecure-env scrub on suid exec. **High.** (Skip S_ISUID/S_ISGID and file-cap gain when `no_new_privs`; add secure-exec env scrubbing.)

6. **Weak, predictable ASLR + fixed PIE base + no KASLR.** mmap 16 bits, stack 8 sub-page bits, both from a `kernel_ticks`-seeded splitmix64 (`syscall.c:187`); the main executable always loads at `0x400000` (`elf.c:52`); kernel base fixed. Capped by the 2 GB user VA. **High.** (Randomize the executable load bias for PIE, widen entropy, draw ASLR offsets from the CSPRNG, and add KASLR.)

7. **/proc/<pid>/maps (and via #2, /proc/<pid>/mem) readable cross-process with no uid check.** `fill_proc_maps` (`procfs.c:326-352`). Defeats the little ASLR present; full layout disclosure. **High.** (Gate procfs per-pid files on a `ptrace_may_access`-equivalent.)

8. **No fork-bomb / RLIMIT_NPROC and no per-user PID cap.** Local unprivileged DoS bounded only by global PID space and RAM (`kernel/proc.c`). **High.** (Enforce RLIMIT_NPROC per real-uid in fork.)

9. **Kernel .text/.data writable + executable (no RO text, no NX data).** Boot maps everything `0x83` (`arch/x86_64/boot.S`). Removes the last layer between a write primitive and code exec; compounds #1/#3. **High.** (Re-map kernel sections with correct perms after init; aarch64 WXN.)

10. **kptr_restrict / dmesg_restrict absent + no KPTI.** Kernel pointers and log leak to unprivileged users, and (Meltdown-class CPUs) kernel memory is transiently readable from user (single shared PML4, `arch/x86_64/vm.c`). **Med.** (Add %pK masking + restricted dmesg; KPTI for affected x86 targets.)

---

## Posture summary

**What Ranix already does well (genuine, not inflated):**
- **CSPRNG done right.** AT_RANDOM (the libc stack canary / pointer guard) and `getrandom`/`/dev/[u]random` all draw from a real CSPRNG (`kernel/lib/csprng.c`, `syscall_proc.c:1357`, `syscall_misc.c:1130`). This *supersedes* the stale `syscall_conformance_audit.md:141` claim that getrandom uses splitmix64.
- **Allocator hardening.** Buddy poison-on-free + double-free/UAF guard (`buddy.c:37-152`) and slab magic (`heap.c`) — confirmed clean by the parity audit.
- **copy_to/from_user is correct** — strict `access_ok` range + overflow check, fault-safe copy, no TOCTOU (`string.c:69-90`; latent_hazards_audit `:128`).
- **Kernel stack canary** on entry and context switch (`sched.c`, `syscall.c:294`).
- **seccomp enforced at a single fail-closed chokepoint** before dispatch (`syscall.c:288-326`); credential syscalls (setres*/setre*) look POSIX-correct; capability *sets* exist; `/dev/mem` is wisely absent.

**Where it is soft:** the **hardware enforcement floor is missing**. There is no NX on either arch, x86 SMEP/SMAP are no-ops, kernel text/data are RWX, there's no KASLR/KPTI, and ASLR is weak and predictable with a fixed PIE base. Combined, these mean a single memory-corruption or pointer-leak bug escalates to kernel code execution with little friction — Ranix's *software* permission model (VMA flags, COW fault handler, credential checks) is reasonable, but nothing underneath the OS enforces it in silicon. The two stand-alone logic holes that need no exotic primitive — **unrestricted ptrace ATTACH** and **ignored no_new_privs at suid exec** — are the most directly exploitable for privilege escalation and should be closed first alongside turning on the NX/SMEP hardware that the CPUs already provide.

Verdict: **functionally capable, hardening-immature.** The userspace-facing crypto and allocator work is solid; the MMU/CPU hardening that defines a modern kernel's exploit-resistance is largely unimplemented.
