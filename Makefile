ARCH  ?= aarch64
BOARD ?= raspi4

CC  = clang --target=$(ARCH)-none-elf
AS  = clang --target=$(ARCH)-none-elf
LD  = ld.lld

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/ranix.elf

INCLUDES = -Iinclude -Idrivers -I. -Iarch/$(ARCH)/include

ifeq ($(ARCH),aarch64)
    ARCH_CFLAGS = -mgeneral-regs-only
    ifeq ($(BOARD),raspi4)
        MACH_DIR    = arch/aarch64/mach-bcm2711
        LINKER      = arch/aarch64/linker_raspi4.ld
        QEMU_CMD    = qemu-system-aarch64 -M raspi4b -cpu cortex-a72
    else
        MACH_DIR    = arch/aarch64/mach-virt
        LINKER      = arch/aarch64/linker.ld
        QEMU_CMD    = qemu-system-aarch64 -M virt -cpu cortex-a72
    endif
    SRCS_ARCH = \
        arch/aarch64/exceptions.c \
        arch/aarch64/irq.c \
        arch/aarch64/cpu.c \
        arch/aarch64/mmu.c \
        arch/aarch64/vm.c \
        $(MACH_DIR)/board.c
    SRCS_ARCH_ASM = \
        arch/aarch64/boot.S \
        arch/aarch64/exceptions_asm.S \
        arch/aarch64/mmu_asm.S
    SRCS_DRIVERS = \
        drivers/tty/serial/pl011/pl011.c \
        drivers/irqchip/gic400/gic400.c \
        drivers/irqchip/arm-timer/arm_timer.c \
        drivers/mailbox/bcm2711_mbox.c
else ifeq ($(ARCH),x86_64)
    ARCH_CFLAGS = -mno-sse -mno-avx -mno-red-zone -m64
    LINKER      = arch/x86_64/linker.ld
    QEMU_CMD    = qemu-system-x86_64 -M q35
    SRCS_ARCH = \
        arch/x86_64/cpu.c \
        arch/x86_64/irq.c \
        arch/x86_64/gdt.c \
        arch/x86_64/idt.c \
        arch/x86_64/mmu.c \
        arch/x86_64/vm.c \
        arch/x86_64/exceptions.c \
        arch/x86_64/mach-pc/board.c
    SRCS_ARCH_ASM = \
        arch/x86_64/boot.S \
        arch/x86_64/exceptions_asm.S
    SRCS_DRIVERS = \
        drivers/serial/uart16550/uart16550.c \
        drivers/irqchip/pic8259/pic8259.c \
        drivers/irqchip/pit/pit.c
endif

SRCS_KERNEL = \
    kernel/main.c \
    kernel/driver.c \
    kernel/fdt.c \
    kernel/irq.c \
    kernel/kprintf.c \
    kernel/sched.c \
    kernel/shell.c \
    kernel/tty/fb_tty.c \
    kernel/sync.c \
    kernel/mm/buddy.c \
    kernel/mm/vm.c \
    kernel/mm/pmm.c \
    kernel/mm/heap.c \
    kernel/lib/string.c

SRCS_C   = $(SRCS_KERNEL) $(SRCS_ARCH) $(SRCS_DRIVERS)
SRCS_ASM = $(SRCS_ARCH_ASM)

CFLAGS = -ffreestanding -nostdlib -nostdinc -O2 -Wall $(ARCH_CFLAGS) $(INCLUDES)
LDFLAGS = -nostdlib

OBJS  = $(SRCS_C:%.c=$(BUILD_DIR)/%.o)
OBJS += $(SRCS_ASM:%.S=$(BUILD_DIR)/%.o)

.PHONY: all clean qemu image

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJS) -o $@
	@echo "[OK] Ranix built → arch=$(ARCH) board=$(BOARD)"

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) ranix.iso

qemu: $(TARGET)
	$(QEMU_CMD) -nographic -kernel $(TARGET)

iso: $(TARGET)
	@mkdir -p iso/boot/grub
	@cp $(TARGET) iso/boot/ranix.elf
	@printf 'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o ranix.iso iso/ 2>/dev/null
	@echo "[OK] ranix.iso hazır"

qemu-iso: iso
	$(QEMU_CMD) -nographic -cdrom ranix.iso

image: all
	llvm-objcopy -O binary $(TARGET) kernel8.img
	@echo "[OK] kernel8.img hazır"
