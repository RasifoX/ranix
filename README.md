# Ranix

Ranix is a small kernel written from scratch in C. It runs Linux userspace
programs (static and dynamic binaries, BusyBox, bash, Node.js) on x86_64. An
aarch64 port exists in the tree but is not working yet. It is not Linux and not
a Linux fork.

Status: experimental. Expect bugs. Do not put it on a machine whose data you
cannot afford to lose.

## Build (x86_64)

You need: `clang`, `ld.lld`, `make`, a musl cross compiler
(`x86_64-linux-musl-gcc`), `grub-mkrescue`, `qemu-system-x86_64`, and network
access on the first build (it downloads and builds userspace packages).

    make ARCH=x86_64 MUSL_CC_X86_64=/path/to/x86_64-linux-musl-gcc iso

Output: `build/ranix.elf` (kernel), `build/initramfs.cpio`, `ranix.iso`.

## Run in QEMU

    make ARCH=x86_64 qemu-iso      # boots ranix.iso, console on this terminal
    make ARCH=x86_64 test          # full test suite, prints PASS/FAIL lines

Quit QEMU with `Ctrl-A` then `X`.

## More

Wiki: `<WIKI-LINK>` (Building, Running in QEMU, Installing on a PC,
Kernel Swap, Troubleshooting, Hardware).

## License

Not decided yet. Until a license is published, no rights are granted.
