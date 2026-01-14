# Ranix Kernel

**Ranix** is a modular monolithic kernel built from scratch. It is designed to separate tasks and drivers with a hardware-specific architecture.

Unlike typical hobby kernels that tightly integrate logic with the x86 architecture, Ranix implements a robust **Hardware Abstraction Layer (HAL)**, making it a “portability-first” design ready for future architectures such as ARM or x86_64.

#### Website: [ranix.org](https://ranix.org/)

## Technology Stack
* **Architecture:**
  - ***Primary Target:** x86 (i686).*
  - ***Planned Targets:** ARM (Cortex-M), x86_64*
  - ***Portability:** Multi-arch support via HAL (Hardware Abstraction Layer).*

* **Bootloader:** GRUB (Multiboot compliant)
* **Design:** Modular Monolithic Kernel with a Hardware Abstraction Layer (HAL)

*HAL is designed to abstract the fundamental elements of the CPU, interrupts, and timers.*

*HAL does not abstract memory management or scheduling logic, which remain part of the kernel.*

# Roadmap

- **Phase 1 (Done):** Booting, GDT, IDT, PIC, PIT drivers.

- **Phase 2 (Done):** Memory Management (PMM/VMM/Heap), Keyboard Driver, Shell.

- **Phase 3 (Current):** Virtual File System (VFS), Initrd, File Operations.

- **Phase 4 (Future):** User Space, Multitasking, ELF Loading.

## The Toolchain (Critical)

Do not attempt to compile Ranix using your main system's gcc (e.g., /usr/bin/gcc). This will fail because Ranix does not use the standard C library (libc).

You **must** use a Cross-Compiler targeting i686-elf.

[**How to build a Cross-Compiler**](https://wiki.osdev.org/GCC_Cross-Compiler)

* [**i686-elf-gcc & binutils**](https://wiki.osdev.org/GCC_Cross-Compiler) - Kernel Complier
* [**xorriso**](https://www.gnu.org/software/xorriso/) - ISO Image Creation
* [**GRUB**](https://www.gnu.org/software/grub/)
* [**QEMU**](https://www.qemu.org/) - Emulation/Testing

## Quick Start
Everything is automated via Makefile.

*Requires GRUB tools and xorriso to be installed on the host system.*

```bash
git clone https://github.com/RasifoX/ranix.git

cd ranix

make run
```

## License

Ranix is open-source software licensed under the GPLv2. See [LICENSE](LICENSE).

Copyright (C) 2026–present RasifoX



