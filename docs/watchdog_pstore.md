# Unattended boot: watchdog, panic reset, persistent log

For booting Ranix on a real laptop with no serial port and nobody watching. Three
parts: a hardware watchdog for hangs, a reset at the end of every panic, and a
kernel log that outlives the reset.

All three are x86_64 only. A boot without these options behaves as before, except
that the persistent log is on by default when its RAM is usable.

## Boot options

| Option | Meaning |
|---|---|
| `ranix.watchdog=N` | Reset N seconds after the last pet. Clamped to 10..1227; 0 is off. Also turns on reset-on-panic. |
| `panic=N` | Reset N seconds after a panic. `panic=0` freezes; a negative value resets at once. Without it, `ranix.watchdog` implies 5 s and a plain boot freezes. |
| `ranix.pstore=off` | No persistent log. |
| `ranix.pstore=0xADDR` | Move the log. Must be 1 MiB aligned, in usable RAM, clear of boot data. |
| `ranix.resetvia=cf9\|kbc\|acpi\|triple` | Test: use one reset method only. |
| `ranix.wdtest=hang\|spin\|panic\|nested\|boothang` | Test: inject a failure. |

## Watchdog

`drivers/watchdog/itco.c` is the hardware, `wdog_core.c` the policy.

The TCO timer counts 0.6 s ticks. The first expiry reloads it, the second resets
the platform, so `ranix.watchdog=120` programs 100 ticks per stage. QEMU measured
this at exactly two stages. On Casper, Linux's 30 s heartbeat reads back as
`TCO_TMR` = 50, which agrees with the 0.6 s tick.

### Where the registers are

| | ICH9 (QEMU q35) | Sunrise Point (Casper) |
|---|---|---|
| Identified by | LPC 00:1f.0, id 0x2910..0x291F | SMBus 00:1f.4, id 0x9D23, 0xA123, 0xA2A3 |
| TCO I/O base | PMBASE (LPC 0x40) + 0x60 | TCOBASE (SMBus 0x50), enable TCOCTL bit 8 (0x54) |
| NO_REBOOT | RCBA + 0x3410 bit 5 | SBREG + 0xC6000C bit 1 |

Only those IDs are accepted. Cannon Lake and later moved NO_REBOOT again, and a
wrong write lands in chipset private space, so anything else is a logged no-op.

Sunrise Point's NO_REBOOT is not in the PMC. It is in the SMBus private config
space behind the P2SB, which firmware hides. While hidden the P2SB reads all ones
but still takes writes, so HIDE is flipped with a byte write (`pci_write8`): a dword
read-modify-write would store ones over the neighbouring registers.

**On Casper the TCO counter does not count.** With TMR_HLT clear, TCO_RLD stayed at
50 for 16 s under Debian's iTCO_wdt, read with real `inw`, and at 100 for 30 s under
Ranix. The ACPI PM timer it should be derived from runs at 3.58 MHz, so that is not
the reason, and the actual gate is unknown. Ranix checks this 3 s after boot and logs
that there is no hardware reset, which leaves the software paths: `ranix.deadline` and
panic reset. A stopped tick has no protection on this machine.

### Values read from the real machine (Casper Nirvana, i5-7200U, BIOS 215)

| Register | Value | Meaning |
|---|---|---|
| SMBus 0x50 / 0x54 | 0x0401 / 0x0100 | TCOBASE 0x400, decode on; matches `iTCO_wdt` in /proc/ioports |
| PMC 0x40 | 0x1801 | ACPI base 0x1800 |
| /proc/iomem iTCO_wdt | 0xFDC6000C | NO_REBOOT address, so SBREG is 0xFD000000 |
| TCO1_CNT | 0x1800 | Halted, TCO_LOCK set |
| SMI_EN | 0x80002033 | TCO_EN and GBL_SMI_EN set |
| FADT reset | I/O 0xB2, value 0xFB | Reset goes through SMM |
| COM1 | all ports 0xFF | No UART |

The SMI_EN line is the main open risk. TCO_EN is set and TCO_LOCK stops it being
cleared, so the first expiry enters firmware SMM code before the second stage. Only
a real timeout shows what that handler does.

### What earns a pet

The timer is reloaded only by the `wd_pet` kernel thread, so a dead scheduler or a
stopped tick stops the pets with no further logic. That misses one CPU stuck in a
non-preemptible kernel loop while the others run, so one `wd_canary` thread is
pinned to each CPU, and the pet thread refuses to pet while any canary is older than
half the timeout. Ticks prove nothing here: they keep arriving on a CPU stuck in a
kernel loop with IRQs on. A canary beat proves the run queue was dispatched.

Canary age is measured with the HPET, not the tick, because a wrong tick rate must
not become a false reset.

During boot the threads do not pet. The boot path holds CPU0, so it pets at each
step itself, and a stall stops the pets even with a healthy secondary CPU.

Worst case from a hang to a reset is about 1.75 times the timeout: half for the
canary to go stale, a quarter for the next pet check, then the full timeout. IRQ-off
wedges on an SMP machine are faster, since `sched_watchdog` panics within ~8 s.

### Reset order

`board_reset()` tries CF9 warm, then the keyboard controller, then the ACPI reset
register, then a triple fault. The warm methods go first because they keep DRAM, and
so the log. ACPI goes last because on Casper it is an SMI, the only method that might
not return. While armed, a panic also shrinks the TCO timer to delay + 6 s, so a panic
that wedges while printing still resets.

## Persistent log

### Choice of method

| Method | Verdict |
|---|---|
| Fixed RAM region | **Chosen.** No driver, no firmware call, works from the panic path, cannot damage anything. Lost on power-off. |
| Raw disk region | Rejected. Casper's SSD has no free space (sda1-3 fill it) and holds the only working OS. A raw write from an untested AHCI driver in a panicking kernel can destroy it. |
| EFI variable | Rejected, though not because it is impossible: runtime services do survive ExitBootServices, and multiboot2 can pass the system table. But it means calling firmware from a dying kernel and wearing SPI flash, and Ranix has no runtime-services path. |

### Layout

1 MiB at physical `0x20000000`, two 512 KiB slots used alternately, so a boot never
overwrites the log of the boot before it. Each slot is a 4 KiB header and a 508 KiB
ring. `arch/x86_64/mach-pc/pstore.c` and `scripts/ranix-lastlog.py` must agree on the
header.

0x20000000 was picked from this machine's real layout: usable in E820, below Debian's
initrd (0x2FE4B000, placed there by GRUB) and above Ranix's kernel and page metadata
(about 66 MiB on this RAM size). GRUB loads the Ranix initramfs just above the kernel,
so an initramfs past roughly 440 MiB would reach the region. Ranix then disables the
log and says so, but GRUB has already overwritten the old one.

The region is checked against the RAM map, kernel, initrd, multiboot info and hugepage
pool. Any conflict is a logged no-op.

### Surviving a reset

The direct map is write-back and a reset discards dirty cache lines, so each commit
pushes new bytes to DRAM with CLFLUSH, data before header. A hang loses at most the
line being printed. An uncached mapping would be simpler but is undefined on x86 while
the direct map also maps that RAM write-back.

The header has a CRC-32, and a CRC of the data stream is checkable until the ring
wraps. Garbage after a power-off therefore reads as "no previous log", never as a
false log. The header state tells a panic, a clean reboot, and "no orderly end" apart.

### Reading it from Linux

Linux must keep that RAM out of its allocator, and `STRICT_DEVMEM` refuses `/dev/mem`
on normal RAM (tested: `Operation not permitted`). Add this once, in
`/etc/default/grub`, then run `update-grub`:

    GRUB_CMDLINE_LINUX_DEFAULT="quiet memmap=1M\\\$0x20000000"

After a crash:

    sudo python3 scripts/ranix-lastlog.py          # newest boot
    sudo python3 scripts/ranix-lastlog.py --all    # both slots

Without `memmap=` the reader falls back to `/proc/kcore`, where Linux may already have
reused the memory; only the CRCs can tell. After a TCO reset, Linux's
`/sys/class/watchdog/watchdog0/bootstatus` should also report it, an independent check.

## Do not boot into a loop

The watchdog turns a hang into a reset, so a kernel that hangs every boot would loop
forever if Ranix were GRUB's default. Keep Debian as the default and boot Ranix once:

    # /etc/default/grub: GRUB_DEFAULT=saved, then update-grub, then:
    sudo grub-reboot "Ranix"

Any reset then lands in Debian, where the log can be read.

## Testing

`tests/itco_host/run.sh` runs the Sunrise Point path against a register model built
from the values above. QEMU has no such chipset, so that path cannot run there.

QEMU q35 needs `-global ICH9-LPC.noreboot=false`; otherwise the strap suppresses the
reset while the timer still expires.
