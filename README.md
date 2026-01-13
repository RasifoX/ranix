# Ranix

**Ranix** is a "modular monolithic"(Planned) kernel built from scratch.

The goal is clearly defined: **Architecture-independent core design with HAL support** No unnecessary overhead, just control.

Website: [ranix.org](https://ranix.org/)

## Technology Stack
* **Architecture:**
  - ***Primary Target:** x86 (i686).*
  - ***Planned Targets:** ARM (Cortex-M), x86_64*
  - ***Portability:** Multi-arch support via HAL (Hardware Abstraction Layer).*

* **Bootloader:** GRUB (Multiboot compliant)
* **Design:** Modular Monolithic Kernel with a Hardware Abstraction Layer (HAL)

*HAL is designed to abstract the fundamental elements of the CPU, interrupts, and timers.*

*HAL does not abstract memory management or scheduling logic, which remain part of the kernel.*

## Requirements
* [**i686-elf-gcc & binutils**](https://wiki.osdev.org/GCC_Cross-Compiler)
* [**xorriso**](https://www.gnu.org/software/xorriso/)
* [**GRUB**](https://www.gnu.org/software/grub/)
* [**QEMU**](https://www.qemu.org/)

## Quick Start
```bash
git clone https://github.com/RasifoX/ranix.git

cd ranix

make run
```

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

## Compliance

Fully compliant with the Multiboot Specification.

Copyright (C) 2026–present RasifoX



