
ARCH ?= x86

ifeq ($(ARCH),x86)
	CC = i686-elf-gcc
	AS = i686-elf-as
	QEMU = qemu-system-i386
	ARCH_DIR = src/arch/x86
	CFLAGS_ARCH = -Isrc/arch/x86/include
	ASFLAGS = --32 -march=i686
	CHECK_MULTIBOOT = @grub-file --is-x86-multiboot2
else
	CHECK_MULTIBOOT = @true
endif

BUILD_DIR = build/$(ARCH)

CFLAGS = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc/include -Isrc/lib $(CFLAGS_ARCH)
LDFLAGS = -ffreestanding -O2 -nostdlib

C_SOURCES = $(shell find src/kernel src/drivers src/hal src/lib -name '*.c' 2>/dev/null)
C_SOURCES += $(shell find $(ARCH_DIR) -name '*.c' 2>/dev/null)
ASM_SOURCES = $(shell find $(ARCH_DIR) -name '*.s' 2>/dev/null)

C_OBJECTS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
ASM_OBJECTS = $(patsubst src/%.s, $(BUILD_DIR)/%.o, $(ASM_SOURCES))
OBJS = $(C_OBJECTS) $(ASM_OBJECTS)

LINKER_SCRIPT = $(ARCH_DIR)/linker.ld
GRUB_CFG = $(ARCH_DIR)/boot/grub.cfg
KERNEL_BIN = $(BUILD_DIR)/ranix.bin
KERNEL_ISO = $(BUILD_DIR)/ranix.iso

all: $(KERNEL_ISO)

$(KERNEL_BIN): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(BUILD_DIR)
	$(CC) -T $(LINKER_SCRIPT) -o $@ $(OBJS) $(LDFLAGS) -lgcc
	$(CHECK_MULTIBOOT) $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(CFLAGS)

$(BUILD_DIR)/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(KERNEL_ISO): $(KERNEL_BIN) $(GRUB_CFG)
	@mkdir -p $(BUILD_DIR)/isodir/boot/grub
	cp $(KERNEL_BIN) $(BUILD_DIR)/isodir/boot/ranix.bin
	cp $(GRUB_CFG) $(BUILD_DIR)/isodir/boot/grub/grub.cfg
	echo "Test string file for testing VFS." > $(BUILD_DIR)/isodir/boot/test.txt
	grub-mkrescue -o $@ $(BUILD_DIR)/isodir
	@echo "ISO OK for $(ARCH)"
     
clean: 
	rm -rf build/

run: $(KERNEL_ISO)
	$(QEMU) -cdrom $(KERNEL_ISO)

.PHONY: all clean run