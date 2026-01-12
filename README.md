# Ranix

A monolithic kernel implementation for x86 architecture using Multiboot specification.

## Prerequisites (Gereksinimler)
* GCC Cross-Compiler (i686-elf-gcc)
* GNU Make
* XORRISO (for creating ISO)
* QEMU (for emulation)

## Building (Derleme)
To compile the kernel and create the ISO image:

```bash
make iso
```

## Running (Çalıştırma)

To run the OS in QEMU:

```bash
make run
```

## Cleaning (Temizlik)

To remove build artifacts:

```bash
make clean
```