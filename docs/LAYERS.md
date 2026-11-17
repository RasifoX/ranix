# Ranix layers: who may call whom

A subsystem calls only the layers its row allows, through that layer's
header under `include/`; no `extern` in a `.c` file. When a lower layer must
reach a higher one (say the timer interrupt and the TCP timers), the higher
layer registers a hook: `tick_register()`, `vfs_register_fs()` and the like.

| Layer | Where | May call | May not call |
|---|---|---|---|
| 1. arch, board | `arch/*`, `arch/*/mach-*` | lib, its own `asm/` headers; a board starts its own platform drivers (`kernel/init.h`) | mm, sched and above (only through hooks) |
| 2. lib | `kernel/lib` | no subsystem | all of them |
| 3. mm | `kernel/mm`, `arch/*/vm.c` | arch, lib, sync | vfs, net, drivers |
| 4. sched, proc, signal, sync, time | `kernel/sched.c`, `proc.c`, `signal.c`, `sync.c`, `kernel/time.h` | arch, lib, mm | vfs internals, fs/*, net internals, drivers |
| 5. vfs, net core, block | `kernel/vfs.c`, `kernel/net/*`, `kernel/block.c` | 1-4 | fs/* internals, driver internals |
| 6. filesystems, drivers | `kernel/fs/*`, `drivers/*` | 1-5 | another driver or filesystem |
| 7. syscalls | `kernel/syscall.c`, `kernel/sys/*` | 1-6, through headers only | — |
| 8. procfs, sysfs | `kernel/fs/procfs.c`, `kernel/fs/sysfs.c` | anyone's `*_report(kout_t *)` | producing data, walking another subsystem's internals |

Rules:
- **A driver does not call another driver.** Shared work gets a core layer
  (`input_*` for input, `block_*` for block devices).
- **A core layer does not know a particular driver.** DRM calls Intel through
  a `drm_driver_t` operation.
- **procfs and sysfs only read.** To show a value, its owner provides
  `*_report()` or `*_get()`. A writable node is a gate and follows the rules
  (CODING-STYLE.md, rule 8).

Known exceptions today (to be moved):
- USB HID hands keys to the PS/2 driver: `input_usb_key`
  (`drivers/input/ps2.h`). Target: `input_report_key()`.
- `kernel/fs/sysfs.c` and `procfs.c` reach driver variables through weak
  references: `syn_tap`, `ps2_aux_report`, `wdog_deadline_*`.
- The kernel includes `drivers/serial/uart.h` directly for the console
  (`kprintf.c`, `tty_core.c`, `devfs.c`, `syscall.c`).
- Generic KMS code in `drivers/gpu/drm` calls Intel directly.
- `kernel/hungtask.c` walks other CPUs' run queues.
