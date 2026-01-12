CC = i686-elf-gcc
AS = i686-elf-as 
CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc/include
LDFLAGS = -ffreestanding -O2 -nostdlib -lgcc

C_SOURCES = $(wildcard src/kernel/*.c)
ASM_SOURCES = $(wildcard src/boot/*.s src/kernel/*.s)

C_OBJECTS = $(C_SOURCES:.c=.o)
ASM_OBJECTS = $(ASM_SOURCES:.s=.o)

OBJS = $(C_OBJECTS) $(ASM_OBJECTS)

all: ranix.bin

ranix.bin: $(OBJS) linker.ld
	$(CC) -T linker.ld -o ranix.bin $(LDFLAGS) $(OBJS)
	grub-file --is-x86-multiboot ranix.bin

%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

%.o: %.s
		$(AS) $< -o $@

iso: ranix.bin
	mkdir -p isodir/boot/grub
	cp ranix.bin isodir/boot/ranix.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o ranix.iso isodir

clean: 
	rm -f ranix.bin ranix.iso
	find src -name "*.o" -type f -delete
	rm -rf isodir

run: iso
	qemu-system-i386 -cdrom ranix.iso
