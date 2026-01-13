# Ranix

**Ranix** is a modular monolithic kernel built from scratch.

The goal is clearly defined: **One kernel core for everything.** No unnecessary overhead, just control.

Website: [ranix.org](https://ranix.org/)

## Technology Stack
* **Architecture:** x86 (i686) — *The primary target is x86 (i686). Architectural abstraction is planned via HAL.*
* **Bootloader:** GRUB (Multiboot compliant)
* **Design:** Modular Monolithic Kernel with a Hardware Abstraction Layer (HAL)

*HAL is designed to abstract the fundamental elements of the CPU, interrupts, and timers.*

*HAL does not abstract memory management or scheduling logic, which remain part of the kernel.*

## Requirements
* `i686-elf-gcc` & `binutils`
* `make`
* `xorriso`
* `qemu` (for testing)

## Build & Run

Everything is automated via Makefile.

**Build ISO:**

*Requires GRUB tools and xorriso to be installed on the host system.*

```bash
make all
```

**Run in QEMU:**

```bash
make run
```

**Clean:**

```bash
make clean
```

## License

GPLv2. See [LICENSE](LICENSE).

Copyright (C) 2026–present RasifoX
