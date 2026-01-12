CC = i686-elf-gcc
AS = i686-elf-as 
CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra
LDFLAGS = -ffreestanding -O2 -nostdlib -lgcc


OBJS = src/boot/boot.o src/kernel/kernel.o

all: ranix.bin

ranix.bin: $(OBJS) linker.ld
		$(CC) -T linker.ld -o ranix.bin $(LDFLAGS) $(OBJS)
		grub-file --is-x86-multiboot ranix.bin

src/boot/boot.o: src/boot/boot.s
		$(AS) src/boot/boot.s -o src/boot/boot.o 

src/kernel/kernel.o: src/kernel/kernel.c
		$(CC) -c src/kernel/kernel.c -o src/kernel/kernel.o  ${CFLAGS}

iso: ranix.bin
		mkdir -p isodir/boot/grub
		cp ranix.bin isodir/boot/ranix.bin
		cp grub.cfg isodir/boot/grub/grub.cfg
		grub-mkrescue -o ranix.iso isodir

clean: 
		rm -f ranix.bin ranix.iso
		rm -f src/boot/*.o src/kernel/*.o 
		rm -rf isodir 

run: iso
		qemu-system-i386 -cdrom ranix.iso
