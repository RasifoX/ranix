CC = i686-elf-gcc
AS = i686-elf-as 

BUILD_DIR = build

ASFLAGS = --32 -march=i686
CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc/include
LDFLAGS = -ffreestanding -O2 -nostdlib 

C_SOURCES := $(shell find src -name '*.c')
ASM_SOURCES := $(shell find src -name '*.s')


C_OBJECTS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst src/%.s, $(BUILD_DIR)/%.o, $(ASM_SOURCES))

OBJS = $(C_OBJECTS) $(ASM_OBJECTS)

KERNEL_BIN = $(BUILD_DIR)/ranix.bin
KERNEL_ISO = $(BUILD_DIR)/ranix.iso

all: $(KERNEL_ISO)

$(KERNEL_BIN): $(OBJS) src/linker.ld
	@mkdir -p $(BUILD_DIR)
	$(CC) -T src/linker.ld -o $@ $(OBJS) $(LDFLAGS) -lgcc	
	@grub-file --is-x86-multiboot $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILD_DIR)/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ISO): $(KERNEL_BIN) src/boot/grub.cfg
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	cp $(KERNEL_BIN) $(BUILD_DIR)/isodir/boot/ranix.bin
	cp src/boot/grub.cfg $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD_DIR)/isodir
	@echo "ISO OK"
	
clean: 
	rm -rf $(BUILD_DIR)

run: $(KERNEL_ISO)
	qemu-system-i386 -cdrom $(KERNEL_ISO)

.PHONY: all clean run