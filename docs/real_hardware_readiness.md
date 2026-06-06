# Real-hardware boot readiness

Gap analysis: **QEMU-works vs real-silicon-needs**, for the project's headline goal — a
Raspberry Pi 4 (BCM2711) graphical-terminal experience — plus a secondary x86 PC target.
Read-only audit; planning, not fixing. Status taken at `6a79be9`.

Status legend used in the **Gap** column:
- **works** — implemented and proven on real silicon (per `roadmap1.md`).
- **likely-works** — implemented to the real-HW spec but **not yet exercised on silicon** (or only
  partially), so a residual tuning/bring-up risk remains.
- **untested** — implemented for QEMU's model only; real-silicon behaviour unknown.
- **missing** — not implemented at all.

Cross-references: `docs/linux_parity_audit.md` §12 (USB/Input/HID) and §13 (Pi), and `roadmap1.md`
v0.7 (real-Pi4 SMP proven, 5/5 runs).

---

## 1. Pi 4 boot chain

| Capability | Ranix (QEMU status, file:line) | Real-HW need | Gap | Blocker? | Notes |
|---|---|---|---|---|---|
| Load address / image header | `_start` branch + ARM64 image header, magic at 0x38, text load `0x80000` (`arch/aarch64/boot.S:9-27`); linker `PHYS_BASE=0x80000` (`arch/aarch64/linker_raspi4.ld:3`) | VideoCore firmware loads `kernel8.img` at 0x80000 with `arm_64bit=1` | **works** | No | Matches firmware default. `config.txt` has `arm_64bit=1`, `kernel=kernel8.img` (`pi4-dist/config.txt`). |
| Firmware-passed DTB | x0 saved to `_dtb_ptr` (`boot.S:30-32`); parsed in `fdt_init` (`kernel/fdt.c`) | Firmware passes a populated DTB in x0 | **works** | No | Same path QEMU uses; the real firmware DTB is richer (see memory map). |
| EL2→EL1 drop | `boot.S:35-58` sets HCR.RW, spsr, `cnthctl_el2=3`, `cntvoff_el2=0`, eret | Pi4 firmware enters kernel at EL2 | **works** | No | `cntvoff=0` is why the virtual-timer path works at EL1 (see §5). |
| Memory map | FDT `/memory reg` walk, multi-region (`fdt.c:161-183`); `board_foreach_mem_region` prefers FDT, falls back to `config.mem_size` (`mach-bcm2711/board.c` `board_foreach_mem_region`) | Real Pi4 has RAM split into ≥2 banks; 2/4/8 GB SKUs; a bank above 1 GB | **likely-works** | No | FDT path sums all regions, so >1 GB is handled. **Risk:** `config.mem_size=0x3C000000` (960 MB) is only the no-FDT fallback — fine since firmware always passes a DTB. |
| SD-card (not `-kernel`) boot | `pi4-dist/` (config.txt, cmdline.txt, initramfs.cpio) | Firmware reads FAT partition: `config.txt`+`kernel8.img`+`initramfs.cpio` | **likely-works** | No | The required `start4.elf`/`bootcode` are firmware (not in repo, must be on the card). initramfs is `followkernel`. cmdline is just `panic_test` placeholder (`pi4-dist/cmdline.txt`). |
| initramfs / rootfs | `initrd_init()` (`kernel/main.c`), cpio in `pi4-dist/` | Firmware loads initramfs to RAM, passes via DTB `/chosen` | **likely-works** | No | Proven indirectly by the real-Pi4 test runs (roadmap v0.7). |
| Secondary-CPU release | spin-table release addr read from DTB, fallback `smp_release_addr={0xd8..0xf0}` (`mach-bcm2711/board.c`), publish+`sev`, or PSCI-via-**smc** (`arch/aarch64/smp.c:16-51`); CPUECTLR SMPEN set at EL2 (`boot.S` `#ifdef BOARD_RASPI4`) | Real Pi4 armstub uses spin-table; PSCI must be SMC (kernel owns EL2) | **works** | No | Explicitly proven: 4-core SMP on real Pi4, 5/5 runs (`roadmap1.md` v0.7). |

## 2. Pi 4 drivers (real silicon)

| Capability | Ranix (QEMU status, file:line) | Real-HW need | Gap | Blocker? | Notes |
|---|---|---|---|---|---|
| UART (PL011) | base `0xFE201000` (`mach-bcm2711/board.c:11`), `uart_init()` in `board_early_init` | PL011 on real GPIO14/15, `enable_uart=1` | **works** | No | `enable_uart=1` in config.txt. Console proven via the real-Pi4 runs. GPIO/clock muxing assumed done by firmware. |
| Mailbox / property iface | `drivers/mailbox/bcm2711_mbox.c`, base `0xFE00B880`, bus offset `0xC0000000`; `dc civac` flush around the buffer + `dsb sy` (`bcm2711_mbox.c:25-52`) | Real VideoCore property channel for fb/clocks/power | **likely-works** | No | Cache maintenance is present and correct. Same protocol QEMU models, but real firmware actually honours SET_CLOCK etc. |
| GIC-400 interrupt ctrl | programmed as GICv2: dist `0xFF841000`, cpu `0xFF842000` (`mach-bcm2711/board.c:13-14`); `gic400.c` prints "GIC: v2"; per-core timer via BCM2711 **local** controller `local_base+0x60` (`gic400.c:55-74`) | GIC-400 is GICv2-compatible; per-core CNTV IRQ routed via local block | **works** | No | The local-controller routing (`+0x40`/`+0x60`) is BCM2711-specific and already handled. Proven on real Pi4. |
| SD / EMMC2 (Arasan SDHCI) | `drivers/block/sdhost_pi4.c`, base `0xFE340000`, **PIO read-only, no DMA**; clock via mailbox `MBOX_CLK_EMMC2` (`sdhost_pi4.c` `pi4_sd_init`) | Real EMMC2 timing, voltage switch, HS modes | **likely-works** | No | Read-only (`pi4_sd_write` returns -1 → **no SD persistence/write**). Timeouts are spin-count loops tuned blind (`sd_wait`), a real-silicon timing risk. Persistence on real Pi unverified end-to-end (see `linux_parity_audit.md` PI-1). |
| GENET gigabit ethernet | `drivers/net/bcmgenet.c`, base `0xFD580000`, polling, MDIO PHY BCM54213PE@addr1, TX/RX DMA with `dc cvac`/`ivac`; **no QEMU model** | Real GENET v5 + RGMII + PHY bring-up; MAC from firmware | **untested** | No (UART/SSH suffices) | Header self-documents the likely tuning points: DMA bus-offset (1:1 assumed), cache-line, PHY speed decode (`bcmgenet.c:7-17`). Written blind from docs; **never run on hardware** (`roadmap1.md:643`). |
| USB (VL805 xHCI behind PCIe) | **xHCI is `#if defined(__x86_64__)`** (`drivers/usb/xhci.c:16`) and uses **port-I/O PCI** (`kernel/pci.c:4-5`, `0xCF8/0xCFC`, `inl/outl`) | Pi4 VL805 xHCI sits behind the **brcmstb PCIe RC** (`0xFD500000`), config via **ECAM (MMIO)** | **missing** | **Yes (for local USB)** | Two independent gaps: (a) no ARM/ECAM PCIe host bridge — `kernel/pci.c` is x86-legacy-only; (b) xHCI compiled out on aarch64. No grep hits for `pcie`/`vl805`/`brcmstb`/ECAM anywhere. |

## 3. Input / display for the interactive terminal

| Capability | Ranix (QEMU status, file:line) | Real-HW need | Gap | Blocker? | Notes |
|---|---|---|---|---|---|
| HDMI framebuffer alloc | mailbox SET_PHYS/VIRT_WH, depth 32, ALLOC_FB, GET_PITCH (`bcm2711_mbox.c:75-148`); `hdmi_force_hotplug=1` in config | Real HDMI mode + GPU-allocated fb | **likely-works** | No | `hdmi_force_hotplug=1` forces output even with no EDID. Default 800×480, but queries GPU for real size (`mbox_get_display_size`). **Risk:** fb address masked `& 0x3FFFFFFFU` (`bcm2711_mbox.c:142`) — on a 4/8 GB Pi the GPU may place the fb **above 1 GB**, where this mask corrupts the pointer. |
| fb console rendering | `kernel/tty/fb_tty.c`, shadow buffer + per-glyph blit, uses `pitch`/`red_shift` etc. from fb_info | 32bpp RGB linear fb | **likely-works** | No | Pixel order set to RGB (PIXEL_ORD=1). Real HDMI may report BGR on some firmware → colour swap risk, not a blocker. |
| Local keyboard | **PS/2 only** (`drivers/input/ps2/ps2.c`; x86 board wires `ps2_kbd_init`). **No USB HID anywhere.** | Pi4 has **no PS/2** — keyboard must be USB-HID | **missing** | **Yes (hard)** | Confirmed `linux_parity_audit.md` USB-2/PI-4. Even if PCIe+xHCI existed, there is no HID class driver (xHCI is mass-storage/BOT only, `xhci.c:10-12`). No local keyboard on real Pi at all. |
| Virtual terminals (VT switch) | single fb console; no VT layer (`fb_tty.c`) | Multiple VTs + switching for the "VT-switch to `claude`" experience | **missing** | Partial | Cross-ref `linux_parity_audit.md` §"No real VTs/scrollback/fonts". Not a boot blocker; blocks the full UX. |

## 4. x86 real PC

| Capability | Ranix (QEMU status, file:line) | Real-HW need | Gap | Blocker? | Notes |
|---|---|---|---|---|---|
| Boot (multiboot2) | `.multiboot2` header + fb request type 5 (`arch/x86_64/boot.S:6-13`); `multiboot2_parse` (`mach-pc/board.c:24`) | GRUB on real HW supplies mem map + GOP fb | **likely-works** | No | Standard multiboot2; GRUB on real HW provides the fb tag if it can set a GOP/VBE mode. |
| ACPI (RSDP/MADT/HPET) | `arch/x86_64/acpi.c`: RSDP scan EBDA + `0xE0000-0x100000` (`:126-145`), MADT LAPIC/x2APIC parse (`:159-174`), HPET (`:205`) | Real ACPI tables (often via EBDA / UEFI) | **likely-works** | No | EBDA+BIOS-ROM scan works on legacy BIOS. **Risk:** pure-UEFI machines pass RSDP via the EFI system table, not the scan range → MADT/HPET would be missed. No EFI/`-acpitable` path. |
| SMP bring-up | MADT-driven (`acpi.c` enabled-CPU count), `arch/x86_64/smp.c` | Real LAPIC INIT-SIPI-SIPI | **likely-works** | No | Logic is standard; untested on real silicon timing. |
| Timer calibration | PIT @ `TIMER_HZ` (`mach-pc/board.c:46-47`); HPET as clocksource (`acpi.c:205`) | Real TSC/LAPIC-timer calibration | **untested** | No | No TSC calibration loop found; relies on PIT/HPET. Likely OK but unverified on real HW. |
| Storage (AHCI/NVMe) | AHCI `pci_find_class(01,06,01)` (`ahci.c:132-134`), NVMe `(01,08,02)` (`nvme.c:205`), both x86-gated | Real AHCI/NVMe controllers | **untested** | No | PCI discovery via port-I/O config works on real x86. NVMe needs MSI-X on real HW (`roadmap1.md:641`); MSI/MSI-X cap walk exists but interrupt delivery is polling. |
| Keyboard | PS/2 i8042 (`drivers/input/ps2/ps2.c`) | Most real PCs still expose PS/2 emulation; some are USB-only | **likely-works** | No | PS/2 fine on most desktops; USB-only laptops would need USB-HID (missing, same as Pi). |
| Framebuffer console | `mb2_get_fb` (`mach-pc/board.c:31`) | GRUB GOP/VBE linear fb | **likely-works** | No | Depends on GRUB setting a mode; if text-only multiboot, no fb. |

## 5. Firmware / timing assumptions

| Capability | Ranix (file:line) | Real-HW need | Gap | Notes |
|---|---|---|---|---|
| Timer frequency | reads `cntfrq_el0` at runtime (`arm-timer/arm_timer.c:14,44,61`) — **not hardcoded** | Firmware programs CNTFRQ (Pi4: 54 MHz) | **works** | Correct: no QEMU-fixed rate assumption. Uses **virtual** timer (`cntv*`) which is valid at EL1 because `cntvoff_el2=0`. |
| Cache maintenance for DMA/mailbox | mailbox `dc civac`+`dsb sy` (`bcm2711_mbox.c:29-52`); GENET TX `dc cvac` / RX `dc ivac`; SD is PIO (no DMA, no maintenance needed) | Real HW is not cache-coherent for these DMA masters | **present** | The hard part (non-coherent DMA) is already addressed where it matters. GENET specifics untested. |
| Secondary-core cache/MMU state | secondary disables M/C/I, invalidates page-table lines before MMU on, sets SMPEN (`boot.S` secondary_trampoline) | Real armstub may hand over with MMU/caches live | **works** | Already hardened for real Pi4 (proven). |
| Polling timeouts | SD `sd_wait` spin-count loops, mailbox/`xhci`/GENET busy-spins | Real silicon is slower/faster than QEMU | **risk** | SD/xHCI/GENET timeouts are loop-count based, blind-tuned. Could spuriously time out or hang on real silicon. |
| MMIO bases | hardcoded `0xFE…`/`0xFF…`/`0xFD…` in `mach-bcm2711/board.c` + drivers, not from DTB | Real Pi4 uses the same low-peripheral map (config.txt default) | **works** | Hardcoded but correct for BCM2711 low-peripheral mode. Not DTB-driven (would break on a different SoC, fine for Pi4). |
| Reset/watchdog | PM_WDOG/PM_RSTC (`mach-bcm2711/board.c` `board_reset`) | Real BCM2711 watchdog | **likely-works** | Standard BCM2711 reset sequence. |
| RTC | `arch_rtc_read` returns 0 (`mach-bcm2711/board.c`) — Pi4 has no PL031 | I2C RTC (optional HAT) | **missing** | No wall-clock epoch on Pi without NTP. Not a boot blocker. |

---

## Hard blockers to first real Pi 4 boot (ranked)

The boot chain itself is **not** blocked — kernel load, DTB parse, memory map, GIC, timer, UART
console, SD read, and 4-core SMP are all already proven on real Pi4 hardware (`roadmap1.md` v0.7,
5/5 runs). So "first boot to a UART/SSH login" is effectively **achieved**, not blocked.

The blockers are to a *usable local* experience, ranked by severity:

1. **No PCIe host bridge on aarch64** (`kernel/pci.c` is x86 port-I/O only; no brcmstb PCIe / ECAM).
   Gates everything behind the Pi4's PCIe: the VL805 USB controller.
2. **xHCI compiled out on aarch64** (`drivers/usb/xhci.c:16` `#if defined(__x86_64__)`). Even with a
   PCIe bridge, USB would not enumerate on Pi.
3. **No USB-HID class driver** (`drivers/input/ps2/` is the only input; xHCI is mass-storage-only).
   No local keyboard on a Pi (no PS/2 there) under any circumstance today.
4. **SD is read-only** (`pi4_sd_write` returns -1) — no on-device persistence/writes via SD.

## Needed for the interactive terminal experience (fb-boot → graphical terminal → keyboard)

- **USB-HID keyboard** — the single largest gap. Requires, in order: (a) ARM PCIe/ECAM host bridge,
  (b) aarch64 xHCI, (c) a USB hub + HID class + interrupt-endpoint driver. Today: all three missing.
- **HDMI console** — `likely-works` via the mailbox fb; verify the **>1 GB fb-address mask** issue
  (`bcm2711_mbox.c:142`) on a 4/8 GB Pi, and RGB-vs-BGR pixel order on real HDMI.
- **Virtual terminals / VT switch** — missing; needed for the "switch to `claude`" UX, not for boot.

## Likely-works-but-untested / risky on real silicon (cache / DMA / timing)

- **GENET ethernet** — written blind from docs, never run on hardware; suspect DMA bus-offset,
  cache-line handling, PHY speed decode (`bcmgenet.c:7-17`). Needed for the Pi's built-in NIC.
- **SD EMMC2 timing** — blind-tuned spin-count timeouts (`sd_wait`); high-speed modes, voltage
  switch, and write path all untested/absent.
- **HDMI fb >1 GB address mask** — `& 0x3FFFFFFFU` can corrupt a GPU fb placed above 1 GB.
- **x86 UEFI ACPI** — RSDP scan only covers legacy BIOS ranges; pure-UEFI machines would miss MADT/HPET.
- **x86 NVMe** — needs real MSI-X delivery; current model is polling.
- **All polling timeouts** — loop-count based, calibrated against QEMU speed.

---

### Single most important thing to get a real Pi 4 to *boot*

Nothing — first boot to a serial/SSH console is already proven on real Pi4 (`roadmap1.md` v0.7).
**The single most important thing for the headline *local* experience is the USB path: an aarch64
PCIe (ECAM/brcmstb) host bridge + aarch64 xHCI + a USB-HID keyboard driver** — without it there is no
local keyboard on a Pi (no PS/2 exists on the board), so the graphical-terminal-with-keyboard goal
cannot be reached regardless of how well HDMI works.
