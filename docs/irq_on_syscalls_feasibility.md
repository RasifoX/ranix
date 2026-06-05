# IRQ-on syscalls: feasibility (read-only analysis)

Goal: run x86_64 syscall bodies with IRQs enabled (the Linux model), so a
synchronous cross-CPU TLB shootdown can wait for sibling-CPU acks without
deadlocking (siblings can take the shootdown IPI while in their own syscalls).

Verdict up front: **aarch64 already does this and is the working in-tree
reference.** x86 is the only laggard. The change is feasible and moderate —
but it depends on one prerequisite fix (the x86 IRQ stubs preempt the kernel,
which they must not) that is a latent bug today, masked only because syscalls
are currently IRQ-off.

---

## (a) Current IRQ state per arch

### x86_64 — syscalls run IRQ-OFF (confirmed)
- `syscall` masks IF via SFMASK = `0x200` set in `arch/x86_64/cpu.c:86-93`.
- `syscall_entry` (`arch/x86_64/exceptions_asm.S:171-224`) never executes `sti`.
  The entire body — `syscall_handle`, `signal_dispatch`, the resched check,
  `schedule` — runs with IF clear, then `iretq` (line 224) restores the user
  RFLAGS (IF set) on the way out.
- IRQ gates are interrupt gates (`0x8E`/`0xEE`, `arch/x86_64/idt.c:68-85`), so
  the CPU auto-clears IF on IRQ entry; IRQ handlers also run IRQ-off and no
  handler re-enables. So today nothing fires mid-syscall.

### aarch64 — syscalls run IRQ-ON (confirmed; this is the reference)
- `sync_handler` EL0-syscall path explicitly unmasks IRQ around the body:
  `arch/aarch64/exceptions_asm.S:256` `msr daifclr, #2` before `bl syscall_handle`,
  then `msr daifset, #2` at line 258. The syscall body runs with IRQs enabled.
- The safety that makes this correct is the **EL0/EL1 split in `irq_handler`**
  (`arch/aarch64/exceptions_asm.S:196-238`). When an IRQ is taken:
  - It reads `spsr_el1` and tests the mode bits (`tst x2, #0xF`, line 204-206).
  - **EL1 (interrupted the kernel, e.g. mid-syscall): NON-PREEMPTIBLE.** It jumps
    to `.irq_ret_kernel` (233-238), which restores the interrupted kernel path's
    saved `task->ctx` (held in callee-saved `x21`, captured at line 190) and
    `eret`s straight back. need_resched is left set and honoured later at the
    next syscall-return / idle turn. It does **not** call `schedule`.
  - **EL0 (interrupted user): PREEMPTIBLE.** Falls through to the need_resched
    check (208-223) and may `schedule`.
  - The big comment at 196-203 documents *why*: if a nested EL1 IRQ reached the
    reschedule path it would leave `task->ctx` dangling at the IRQ frame, a later
    `fork`'s `memcpy(parent->ctx)` would copy garbage, and the child would `eret`
    into junk. This is the exact bug from commit 63ab3e2 (memory note "IRQ-off
    syscall derin kökü"). **The fix was: never involuntarily preempt a kernel
    (EL1) context.**
- The fault path (`.sync_fault`, 304-325) is also nested-fault-safe: it saves
  `task->ctx`, publishes the fault frame only for the duration of `sync_dispatch`,
  then restores it — so a fault taken while the syscall-return path is mid-flight
  (e.g. `signal_dispatch` growing the user stack) doesn't clobber the outer
  frame. x86 will want the analogous property once #PF / IRQ can nest a syscall.

**So the aarch64 model x86 must copy is: "IRQ-on syscall body + IRQ handler that
reschedules only when it interrupted user mode (CPL/EL == user); when it
interrupted the kernel, just restore and return, leaving need_resched pending."**

## (b) Infrastructure we HAVE

- **IRQ-saving spinlocks: yes, and they save/restore the flag** so they're
  correct regardless of caller IRQ state.
  - `spin_lock_irq` / `spin_unlock_irq` — `kernel/sync.c:252-263`.
  - x86 `irq_save` does `pushfq; pop; cli` and `irq_restore` does `push; popfq`
    (`arch/x86_64/irq.c:6-16`) — saves the real IF state, so nesting an
    already-IRQ-off region is safe (restore leaves IF off).
  - aarch64 `irq_save` saves DAIF and masks (`arch/aarch64/irq.c:11-20`).
- **All locks shared between syscall and IRQ context already use the IRQ-saving
  variant.** Audited the registered IRQ handlers:
  - timer → `sched_tick` takes `sched_lock` via `spin_lock_irq`
    (`kernel/sched.c:563`); `itimer_charge_cpu` likewise (541, 550).
  - net RX IRQ → `vnet_irq_handler`/`vnet_pci_irq_handler` →
    `net_rx_wake` → `spin_lock_irq(&net_rx_lock)` then `task_wake`
    (→ `spin_lock_irq(&sched_lock)`) (`kernel/net/net.c:225-233`,
    `kernel/sched.c:806-816`).
  - tty/uart poll → `t->lock` always via `spin_lock_irq`
    (`kernel/tty/tty_core.c:94`, 199, 241, 298, 319, …).
  - ps2 kbd IRQ → no locks (`drivers/input/ps2/ps2.c`).
  - IPI → `smp_ipi_membarrier_ack` / `arch_smp_ipi_ack` (atomics only).
  - **No plain `spin_lock(&sched_lock)` anywhere** — grep returns none; every
    `sched_lock` acquisition masks IRQs for the hold. So the timer can never
    fire while a syscall on the same CPU holds `sched_lock` (the lock masks IF
    around the whole critical section). This is the property that makes IRQ-on
    syscalls safe with the single global `sched_lock`.
- The 11 plain `spin_lock()` callers (`kernel/fs/{procfs,tmpfs,ext2,devfs,
  ramfs,hugetlbfs}.c`, `kernel/net/tcp.c:474`) are inode-number / rand /
  ephemeral-port allocators. **None are taken in any IRQ or tick handler**, so
  none can deadlock against an interrupt. (They're only unsafe under
  *involuntary* preemption — see (c) — which we are NOT introducing.)

## (c) What BREAKS, exactly where (ranked)

### 1. (BLOCKER) x86 IRQ stubs reschedule regardless of interrupted privilege
The x86 IRQ paths reschedule on need_resched with **no CPL check** on the
interrupted frame:
- `common_irq` — `arch/x86_64/exceptions_asm.S:97-112` (tests `%gs:80`, then
  `schedule` + stack switch unconditionally).
- `irq_stub_lapic` — `arch/x86_64/exceptions_asm.S:272-284` (same).
- `irq_stub_ipi` — `arch/x86_64/exceptions_asm.S:299-318` (forces `schedule`
  every time: `movl $0, %gs:80` then `schedule`, no CPL gate at all).

Today this is harmless because IRQs never fire inside a syscall (IF is off).
The instant the syscall body runs IRQ-on, a timer/IPI taken mid-syscall would
reach `schedule` and **involuntarily preempt the kernel** — the precise failure
class that produced the aarch64 fork-garbage bug (63ab3e2). x86 has no
equivalent of aarch64's `.irq_ret_kernel`. This MUST be fixed first.

There is no preempt-count / preempt_disable concept anywhere (grep: none), and
`kernel/main.c:424-425` states the design intent outright: *"Non-preemptible
kernel … irq_handler preempts only user space."* aarch64 enforces that; x86
does not.

### 2. (must verify) the swapgs / kstack-switch window in `syscall_entry`
`syscall_entry` does `swapgs` (line 172), stashes user RSP in `%gs:48`
(`scratch`, line 174), then loads the kernel stack from `%gs:16` (line 176).
If IF were enabled before this sequence completes, an IRQ in that window would
run on a half-set-up state and the IRQ stub's own `swapgs`/scratch use would
corrupt it. Mitigation is trivial and matches Linux: keep IF off across entry;
only `sti` *after* the trapframe is fully built and `task->ctx` published (after
line 190), and `cli` again before the resched/exit sequence. `scratch` (`%gs:48`)
is used only in this pre-`sti` window, so it's safe.

### 3. (must verify) re-entrancy of the resched/exit tail
The need_resched check + `schedule` + `sched_get_current_ctx` + stack-switch
tail in `syscall_entry` (lines 203-224) must run IRQ-off (a timer landing
mid-stack-switch would be catastrophic). So the body is IRQ-on but the
entry prologue and the exit/reschedule epilogue stay IRQ-off — again the Linux
shape.

### 4. (low) per-CPU `need_resched` (`%gs:80`) races
`need_resched` is a plain `volatile int` set by IRQ context and the tick, read
by the entry path. With IRQ-on bodies it can now be set *during* the body. That
is fine and desired (it's just a hint; the runqueue is the truth, per
`idle_loop` comment `kernel/sched.c:452-453`) — but the entry path must re-check
it after the body with IF off, which lines 203-207 already do.

### 5. (informational, this is the WHOLE POINT) TLB is local-only on x86
x86 has no cross-CPU shootdown: `invlpg` / CR3-reload are local
(`arch/x86_64/vm.c:169,202,227,246,265-277,322-330`). Cross-CPU correctness
relies on the other CPU reloading CR3 on its next context switch (full flush).
`smp_membarrier` (`kernel/smp.c:176-200`) already shows the workaround scar: its
ack wait is **bounded** (`t < 100000000UL`, line 194) precisely "so a wedged CPU
never hangs the caller" — i.e. a sibling IRQ-off in a syscall can't ack. IRQ-on
syscalls remove that constraint and let a future synchronous shootdown wait
unbounded for acks safely.

## (d) Minimal correct change set for x86 IRQ-on (mirror aarch64)

1. **Fix the IRQ stubs to be non-preemptible-in-kernel (PREREQUISITE, do first
   and ship/soak on its own).** In `common_irq`, `irq_stub_lapic`, and
   `irq_stub_ipi`, before consuming need_resched / calling `schedule`, test the
   interrupted CS for ring 3 — the saved CS is at a known trapframe offset
   (`testq $3, <CS off>(frame)`, the same predicate already used for swapgs at
   e.g. lines 124, 163, 293). If the interrupted frame is ring 0 (kernel),
   **skip schedule, restore the interrupted `task->ctx`, and `iretq`**, leaving
   need_resched set — exactly aarch64's `.irq_ret_kernel` (`exceptions_asm.S:
   233-238`). Note `irq_stub_ipi` currently *always* schedules (line 312-317);
   it must gain the same gate, and capture/restore the interrupted ctx the way
   aarch64 captures it in `x21` (line 190) — x86 will need a callee-saved reg or
   a stack slot for the pre-IRQ `task->ctx`.
   This fix is correct and beneficial even before enabling IRQ-on syscalls.

2. **Enable IRQs only around the syscall body in `syscall_entry`.** After
   `task->ctx` is published (after line 190) add `sti`; add `cli` immediately
   before the need_resched check (before line 204). Body = `syscall_handle`
   (+ `signal_dispatch` — decide whether signal frame setup runs IRQ-on; aarch64
   runs `signal_dispatch` IRQ-off at `.syscall_ret`, line 280, after re-masking
   — mirror that: re-mask before `signal_dispatch`). Net: prologue off → body on
   → (re-mask) → signal/resched/exit off. This is a ~2-instruction change once
   #1 is in.

3. **Make the #PF path nested-safe** (mirror aarch64 `.sync_fault`,
   `exceptions_asm.S:304-325`). With IRQ-on bodies, a #PF can now nest a syscall
   that's mid-return (e.g. `signal_dispatch` faulting on a user stack grow).
   Verify `common_exc` (`exceptions_asm.S:62-80`) preserves/restores the
   outer `task->ctx` rather than clobbering it. Today `common_exc` overwrites
   `task->ctx` unconditionally (line 70-72) with no save/restore — this is the
   x86 analogue of the bug aarch64 fixed in `.sync_fault`. **This needs the
   save-publish-restore dance added.** (Strictly this is already latent for
   nested kernel faults; IRQ-on makes it reachable via the timer→signal path.)

4. **No lock changes needed.** All IRQ-shared locks are already `spin_lock_irq`
   (see (b)); the single `sched_lock` is always taken IRQ-off, so the timer
   can't reenter it. Leave the 11 plain `spin_lock()` allocator sites alone —
   they're never IRQ-shared and we are NOT adding involuntary preemption.

### Landmines, ranked
1. **Involuntary kernel preemption** — the #1 risk; fully avoided by change #1
   (the aarch64 EL1 gate). Skipping it = the 63ab3e2 fork-garbage class on x86.
2. **`common_exc` clobbering `task->ctx` on a nested fault** — change #3.
3. **`sti` placed too early in `syscall_entry`** (before kstack switch / ctx
   publish) — corrupts the `%gs:48` scratch + half-built frame. Place it after
   line 190, re-mask before the exit tail.
4. **`irq_stub_ipi` forcing schedule** — it must honour the kernel gate too, or
   a shootdown/resched IPI taken mid-syscall preempts the kernel.

## (e) Go / No-Go + effort

**GO**, conditional on landing change #1 (and #3) first and soaking them.

- The model is proven in-tree on aarch64; x86 is copying a sibling arch, not
  inventing. The locking infrastructure (IRQ-saving spinlocks, IRQ-off
  `sched_lock`) is already in place and needs no change.
- Effort: small-to-moderate, almost entirely in `arch/x86_64/exceptions_asm.S`
  (+ no C changes expected).
  - Change #1 (IRQ-stub kernel gate, 3 stubs + ctx save/restore): ~½–1 day, plus
    soak — this is the delicate one and should be a separate commit, green on
    both arches, before #2.
  - Change #2 (sti/cli around body): ~1–2 lines, trivial after #1.
  - Change #3 (`common_exc` nested-ctx save/restore): ~½ day, mirror `.sync_fault`.
- Sequencing: (1) land IRQ-stub kernel-preemption gate; soak. (2) land
  `common_exc` nested-ctx fix; soak. (3) flip `syscall_entry` to IRQ-on. (4)
  only then build the synchronous cross-CPU TLB shootdown that this unblocks.
- Risk class is "rare, non-deterministic context corruption" (the suite won't
  reliably catch a missed invariant), so each step must be its own commit, both
  arches green, conservative — matching the project's autonomy/trust rule.
