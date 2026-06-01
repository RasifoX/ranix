ARCH  ?= aarch64
BOARD ?= raspi4

MUSL_CC_AARCH64 ?= $(HOME)/opt/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
MUSL_CC_X86_64  ?= $(HOME)/opt/x86_64-linux-musl-cross/bin/x86_64-linux-musl-gcc

ifeq ($(ARCH),aarch64)
    MUSL_CC = $(MUSL_CC_AARCH64)
    MUSL_SYSROOT = $(HOME)/opt/aarch64-linux-musl-cross/aarch64-linux-musl
else ifeq ($(ARCH),x86_64)
    MUSL_CC = $(MUSL_CC_X86_64)
    MUSL_SYSROOT = $(HOME)/opt/x86_64-linux-musl-cross/x86_64-linux-musl
endif

# musl's libc.so IS the dynamic linker; ship it where PT_INTERP points. (The
# toolchain's ld-musl-*.so.1 is a symlink to an absolute install path that does
# not exist here, so copy the real libc.so.)
LDSO_NAME = ld-musl-$(ARCH).so.1
LDSO_SRC  = $(MUSL_SYSROOT)/lib/libc.so

CC  = clang --target=$(ARCH)-none-elf
AS  = clang --target=$(ARCH)-none-elf
LD  = ld.lld

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/ranix.elf

INCLUDES = -Iinclude -Idrivers -I. -Iarch/$(ARCH)/include

ifeq ($(ARCH),aarch64)
    ARCH_CFLAGS = -mgeneral-regs-only -mcmodel=large -march=armv8.1-a
    ifeq ($(BOARD),raspi4)
        MACH_DIR    = arch/aarch64/mach-bcm2711
        LINKER      = arch/aarch64/linker_raspi4.ld
        QEMU_CMD    = qemu-system-aarch64 -M raspi4b -cpu cortex-a72
        QEMU_KERNEL = $(TARGET)
        ARCH_CFLAGS += -DBOARD_RASPI4=1
    else
        MACH_DIR    = arch/aarch64/mach-virt
        LINKER      = arch/aarch64/linker.ld
        QEMU_CMD    = qemu-system-aarch64 -M virt -cpu cortex-a72
        QEMU_KERNEL = $(BUILD_DIR)/ranix.bin
        QEMU_NET    = -netdev user,id=n0 -device virtio-net-device,netdev=n0
    endif
    SRCS_ARCH = \
        arch/aarch64/exceptions.c \
        arch/aarch64/irq.c \
        arch/aarch64/cpu.c \
        arch/aarch64/mmu.c \
        arch/aarch64/vm.c \
        arch/aarch64/smp.c \
        $(MACH_DIR)/board.c
    SRCS_ARCH_ASM = \
        arch/aarch64/boot.S \
        arch/aarch64/exceptions_asm.S \
        arch/aarch64/mmu_asm.S
    SRCS_DRIVERS = \
        drivers/tty/serial/pl011/pl011.c \
        drivers/irqchip/gic400/gic400.c \
        drivers/irqchip/arm-timer/arm_timer.c \
        drivers/mailbox/bcm2711_mbox.c \
        drivers/block/sdhost_pi4.c \
        drivers/block/virtio_blk.c \
        drivers/net/virtio_net.c
else ifeq ($(ARCH),x86_64)
    ARCH_CFLAGS = -mno-sse -mno-avx -mno-red-zone -m64 -mcmodel=kernel
    LINKER      = arch/x86_64/linker.ld
    QEMU_CMD    = qemu-system-x86_64 -cpu max -device isa-debug-exit,iobase=0x501,iosize=1
    QEMU_KERNEL = $(TARGET)
    SRCS_ARCH = \
        arch/x86_64/cpu.c \
        arch/x86_64/irq.c \
        arch/x86_64/gdt.c \
        arch/x86_64/idt.c \
        arch/x86_64/mmu.c \
        arch/x86_64/vm.c \
        arch/x86_64/exceptions.c \
        arch/x86_64/smp.c \
        arch/x86_64/mach-pc/board.c \
        arch/x86_64/acpi.c \
        arch/x86_64/multiboot2.c
    SRCS_ARCH_ASM = \
        arch/x86_64/boot.S \
        arch/x86_64/exceptions_asm.S \
        arch/x86_64/trampoline.S
    SRCS_DRIVERS = \
        drivers/serial/uart16550/uart16550.c \
        drivers/irqchip/pic8259/pic8259.c \
        drivers/irqchip/pit/pit.c \
        drivers/input/ps2/ps2.c \
        drivers/block/virtio_blk.c \
        drivers/net/virtio_net.c
endif

SRCS_NET = \
    kernel/net/net.c \
    kernel/net/eth.c \
    kernel/net/arp.c \
    kernel/net/ip.c \
    kernel/net/icmp.c \
    kernel/net/udp.c \
    kernel/net/tcp.c \
    kernel/net/socket.c

SRCS_KERNEL = \
    kernel/main.c \
    kernel/smp.c \
    kernel/initrd.c \
    kernel/cmdline.c \
    kernel/driver.c \
    kernel/fdt.c \
    kernel/irq.c \
    kernel/kprintf.c \
    kernel/sched.c \
    kernel/nsproxy.c \
    kernel/cgroup.c \
    kernel/pid.c \
    kernel/user_ns.c \
    kernel/tty/fb_tty.c \
    kernel/sync.c \
    kernel/elf.c \
    kernel/syscall.c \
    kernel/vfs.c \
    kernel/fs/ramfs.c \
    kernel/fs/pipe.c \
    kernel/fs/procfs.c \
    kernel/fs/sysfs.c \
    kernel/fs/devfs.c \
    kernel/fs/mbr.c \
    kernel/fs/gpt.c \
    kernel/fs/ext2.c \
    kernel/pci.c \
    kernel/block.c \
    kernel/mm/buddy.c \
    kernel/mm/vm.c \
    kernel/mm/pmm.c \
    kernel/mm/heap.c \
    kernel/mm/vmalloc.c \
    kernel/lib/string.c \
    kernel/tty/tty_core.c \
    drivers/video/fbdev.c \
    drivers/input/input_core.c

SRCS_C   = $(SRCS_KERNEL) $(SRCS_NET) $(SRCS_ARCH) $(SRCS_DRIVERS)
SRCS_ASM = $(SRCS_ARCH_ASM)

CFLAGS = -ffreestanding -nostdlib -nostdinc -O2 -Wall $(ARCH_CFLAGS) $(INCLUDES) -MMD -MP
LDFLAGS = -nostdlib

OBJS  = $(SRCS_C:%.c=$(BUILD_DIR)/%.o)
OBJS += $(SRCS_ASM:%.S=$(BUILD_DIR)/%.o)
DEPS  = $(SRCS_C:%.c=$(BUILD_DIR)/%.d)
-include $(DEPS)

BUSYBOX_APK_aarch64 = https://dl-cdn.alpinelinux.org/alpine/v3.21/main/aarch64/busybox-static-1.37.0-r14.apk
BUSYBOX_APK_x86_64  = https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64/busybox-static-1.37.0-r14.apk

BASH_VER           = 5.2.37
BASH_SRC_URL       = https://ftp.gnu.org/gnu/bash/bash-$(BASH_VER).tar.gz

.PHONY: all clean qemu image initramfs bash-static test

all: initramfs $(TARGET)

# Userspace programs in the initramfs. Core always ships; tests ship unless DEMO=1.
USER_CORE_BINS = userspace/init userspace/hello userspace/echo_server
USER_TEST_BINS = userspace/test_threads userspace/test_inotify userspace/test_ptrace \
                 userspace/test_unix userspace/test_mqueue userspace/test_creds \
                 userspace/test_aslr userspace/test_signalfd userspace/test_affinity \
                 userspace/test_fpsig userspace/test_cowrace userspace/bench userspace/test_clock \
                 userspace/test_futex_timed userspace/test_poll userspace/test_timerfd userspace/test_eventfd \
                 userspace/test_mmap userspace/test_tcp userspace/test_udp userspace/test_pipestress \
                 userspace/test_uts userspace/test_setns userspace/test_clonens \
                 userspace/test_ipcns userspace/test_ipcsetns userspace/test_timens \
                 userspace/test_dynamic userspace/test_sharedlib userspace/test_dlopen \
                 userspace/test_cgroup userspace/test_mountns userspace/test_pidns \
                 userspace/test_userns userspace/test_netns userspace/test_cgroupns userspace/test_container userspace/test_memcg userspace/test_freeze userspace/test_cpuweight userspace/test_hugepage

initramfs: $(BUILD_DIR)/initramfs.cpio

CACHE_DIR          = .cache
BUSYBOX_APK_CACHED = $(CACHE_DIR)/busybox-$(ARCH).apk
BUSYBOX_EXTRACT    = $(CACHE_DIR)/busybox-$(ARCH)
BASH_CACHED        = $(CACHE_DIR)/bash-dynamic-$(ARCH)

bash-static: $(BASH_CACHED)

$(BASH_CACHED):
	@mkdir -p $(CACHE_DIR)
	@if [ ! -f "$(CACHE_DIR)/bash-$(BASH_VER).tar.gz" ]; then \
		echo "Downloading bash $(BASH_VER) source..."; \
		wget -q "$(BASH_SRC_URL)" -O "$(CACHE_DIR)/bash-$(BASH_VER).tar.gz" || \
		    { rm -f "$(CACHE_DIR)/bash-$(BASH_VER).tar.gz"; echo "ERROR: bash download failed"; exit 1; }; \
	fi
	@echo "Building dynamic bash for $(ARCH)..."
	@rm -rf "$(CACHE_DIR)/bash-build-$(ARCH)"
	@mkdir -p "$(CACHE_DIR)/bash-build-$(ARCH)"
	@tar xzf "$(CACHE_DIR)/bash-$(BASH_VER).tar.gz" -C "$(CACHE_DIR)/bash-build-$(ARCH)" --strip-components=1
	@cd "$(CACHE_DIR)/bash-build-$(ARCH)" && \
	    CC="$(MUSL_CC)" \
	    CFLAGS="-O2" \
	    ./configure --host=$(ARCH)-linux-musl \
	                --without-bash-malloc \
	                --disable-nls \
	                > /dev/null 2>&1 && \
	    $(MAKE) -j$$(nproc) > /dev/null 2>&1
	@cp "$(CACHE_DIR)/bash-build-$(ARCH)/bash" "$@"
	@llvm-strip "$@" 2>/dev/null || strip "$@" 2>/dev/null || true
	@echo "[OK] bash-dynamic for $(ARCH) ready ($$(du -h $@ | cut -f1)), interp $$(readelf -l "$@" 2>/dev/null | grep -o '/lib/ld-musl[^]]*' | head -1)"

$(BUILD_DIR)/initramfs.cpio: $(USER_CORE_BINS) $(USER_TEST_BINS) $(BASH_CACHED) $(wildcard tests/ranix_test.sh)
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/etc \
	           $(BUILD_DIR)/initramfs/home/test $(CACHE_DIR)
	@cp $(USER_CORE_BINS) $(BUILD_DIR)/initramfs/bin/
	@if [ -z "$(DEMO)" ]; then cp $(USER_TEST_BINS) $(BUILD_DIR)/initramfs/bin/; fi
	@mkdir -p $(BUILD_DIR)/initramfs/lib
	@cp $(LDSO_SRC) $(BUILD_DIR)/initramfs/lib/$(LDSO_NAME)
	@if [ -z "$(DEMO)" ]; then cp userspace/libfoo.so $(BUILD_DIR)/initramfs/lib/; fi
	@if [ ! -s "$(BUSYBOX_APK_CACHED)" ]; then \
		echo "Downloading busybox for $(ARCH)..."; \
		if [ "$(ARCH)" = "aarch64" ]; then \
			wget -q "$(BUSYBOX_APK_aarch64)" -O "$(BUSYBOX_APK_CACHED)" || { rm -f "$(BUSYBOX_APK_CACHED)"; echo "ERROR: busybox aarch64 download failed"; exit 1; }; \
		else \
			wget -q "$(BUSYBOX_APK_x86_64)"  -O "$(BUSYBOX_APK_CACHED)" || { rm -f "$(BUSYBOX_APK_CACHED)"; echo "ERROR: busybox x86_64 download failed";  exit 1; }; \
		fi; \
		mkdir -p "$(BUSYBOX_EXTRACT)" && \
		tar xzf "$(BUSYBOX_APK_CACHED)" -C "$(BUSYBOX_EXTRACT)" 2>/dev/null; \
	fi
	@cp "$(BUSYBOX_EXTRACT)/bin/busybox.static" $(BUILD_DIR)/initramfs/bin/busybox
	@chmod +x $(BUILD_DIR)/initramfs/bin/busybox
	@for cmd in sh ls cat touch mkdir rmdir rm cp mv ln chmod grep sed find \
	            ps kill df free uname head tail wc echo printf vi \
	            reboot poweroff halt mount umount dmesg \
	            awk sort uniq cut tr rev xargs dd stat du diff \
	            env tee seq sleep date id whoami hostname \
	            true false dirname basename pwd uptime nproc \
	            timeout test expr yes which readlink ping nc wget \
	            su login passwd; do \
		ln -sf busybox $(BUILD_DIR)/initramfs/bin/$$cmd; \
	done
	@chmod u+s $(BUILD_DIR)/initramfs/bin/su
	@cp "$(BASH_CACHED)" $(BUILD_DIR)/initramfs/bin/bash
	@chmod +x $(BUILD_DIR)/initramfs/bin/bash
	@if [ -z "$(DEMO)" ] && [ -f doomgeneric/doomgeneric ]; then \
		DOOM_ARCH=$$(readelf -h doomgeneric/doomgeneric 2>/dev/null | awk '/Machine:/{print $$NF}'); \
		NEED_ARCH="$(ARCH)"; \
		if [ "$$NEED_ARCH" = "x86_64" ] && [ "$$DOOM_ARCH" = "X86-64" ]; then \
			cp doomgeneric/doomgeneric $(BUILD_DIR)/initramfs/bin/doom; \
		elif [ "$$NEED_ARCH" = "aarch64" ] && [ "$$DOOM_ARCH" = "AArch64" ]; then \
			cp doomgeneric/doomgeneric $(BUILD_DIR)/initramfs/bin/doom; \
		fi \
	fi
	@if [ -z "$(DEMO)" ] && [ -f doom1.wad ]; then \
		cp doom1.wad $(BUILD_DIR)/initramfs/bin/doom1.wad; \
	fi
	@if [ -z "$(DEMO)" ] && [ -f tests/ranix_test.sh ]; then cp tests/ranix_test.sh $(BUILD_DIR)/initramfs/test.sh; fi
	@printf 'root:x:0:0:root:/root:/bin/bash\ntest:x:1000:1000:Test User:/home/test:/bin/sh\n' \
	    > $(BUILD_DIR)/initramfs/etc/passwd
	@rm -f $(BUILD_DIR)/initramfs/etc/shadow
	@printf 'root:$$6$$ranixsalt$$.hGbivmRHVhZxfxORfv1MD0sy40LgabzFqZTIyXG4KhrIN/YF.rMrSGyJrMMXSi8loXADZl.uX7/8UWcfZA2q1:19900:0:99999:7:::\ntest:$$6$$testsalt1$$6cTOkKTn8ZS0yaLedMWewUyP2wRoYelzbeIvjGKJ4GsHcHnp0m2DU87odsL9e90OCGfvadlarpnMVWZoOMysH0:19900:0:99999:7:::\n' \
	    > $(BUILD_DIR)/initramfs/etc/shadow
	@chmod 640 $(BUILD_DIR)/initramfs/etc/shadow
	@printf 'root:x:0:\ntest:x:1000:\n' > $(BUILD_DIR)/initramfs/etc/group
	@cd $(BUILD_DIR)/initramfs && find . -print0 | cpio --null -o -H newc --owner=0:0 > ../initramfs.cpio 2>/dev/null
	@echo "[OK] initramfs.cpio created"

# One rule for every userspace program. -lpthread is a no-op on musl (pthread
# lives in libc), so it is safe for the non-threaded ones too.
userspace/%: userspace/%.c
	$(MUSL_CC) -static -no-pie -O2 -o $@ $< -lpthread

# dynamically linked (PT_INTERP → /lib/ld-musl-*.so.1); exercises the in-kernel
# dynamic-linker loading path. Explicit rule overrides the static one above.
userspace/test_dynamic: userspace/test_dynamic.c
	$(MUSL_CC) -O2 -o $@ $<

# extra shared library + a load-time and a runtime (dlopen) consumer
userspace/libfoo.so: userspace/libfoo.c
	$(MUSL_CC) -shared -fPIC -O2 -Wl,-soname,libfoo.so -o $@ $<
userspace/test_sharedlib: userspace/test_sharedlib.c userspace/libfoo.so
	$(MUSL_CC) -O2 -o $@ $< -Luserspace -lfoo
userspace/test_dlopen: userspace/test_dlopen.c
	$(MUSL_CC) -O2 -o $@ $<

$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJS) -o $@
	@if [ "$(ARCH)" = "aarch64" ] && [ "$(BOARD)" != "raspi4" ]; then \
		llvm-objcopy -O binary $@ $(BUILD_DIR)/ranix.bin; \
	fi
	@if [ "$(ARCH)" = "x86_64" ]; then echo "[OK] Ranix built → arch=$(ARCH)"; else echo "[OK] Ranix built → arch=$(ARCH) board=$(BOARD)"; fi

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(AS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) ranix.iso ranix-disk.img $(USER_CORE_BINS) $(USER_TEST_BINS) userspace/sh userspace/*_elf.h userspace/libfoo.so

qemu: $(TARGET) initramfs
	$(QEMU_CMD) $(QEMU_NET) -m 512M -nographic -kernel $(QEMU_KERNEL) -initrd $(BUILD_DIR)/initramfs.cpio

iso: initramfs $(TARGET)
	@mkdir -p iso/boot/grub
	@cp $(TARGET) iso/boot/ranix.elf
	@cp $(BUILD_DIR)/initramfs.cpio iso/boot/initramfs.cpio
	@printf 'set timeout=0\nset default=0\nmenuentry "Ranix" {\n  multiboot2 /boot/ranix.elf\n  module2 /boot/initramfs.cpio\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o ranix.iso iso/ 2>/dev/null
	@echo "[OK] ranix.iso hazır"

qemu-iso: iso
	$(QEMU_CMD) -m 512M -nographic -cdrom ranix.iso

doom: doomgeneric/doomgeneric

doomgeneric/doomgeneric: doomgeneric/doomgeneric_linuxvt.c
	$(MAKE) -C doomgeneric -f Makefile.ranix ARCH=$(ARCH) \
	    CC=$(MUSL_CC)

qemu-doom: doom
	rm -f $(BUILD_DIR)/initramfs.cpio
	$(MAKE) ARCH=$(ARCH) iso
	$(QEMU_CMD) -vga std -m 256M -cdrom ranix.iso -serial file:serial.log

# Create a 32 MB ext2 test disk image with some files
disk-image: ranix-disk.img

ranix-disk.img:
	dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null
	mkfs.ext2 -b 1024 -L ranix-disk $@ 2>/dev/null
	@echo "[OK] ranix-disk.img ready"

# GPT-partitioned disk with an ext4 partition holding /hello.txt — used by the
# test suite to exercise GPT parsing + ext4 read end to end (x86_64 only, where
# virtio-blk is available).
$(BUILD_DIR)/gpt-test.img:
	@mkdir -p $(BUILD_DIR)/diskseed
	@printf 'ranix-gpt-ok' > $(BUILD_DIR)/diskseed/hello.txt
	dd if=/dev/zero of=$(BUILD_DIR)/part.img bs=1M count=12 2>/dev/null
	mkfs.ext4 -F -q -O ^has_journal,^metadata_csum,^64bit -b 1024 \
	    -d $(BUILD_DIR)/diskseed $(BUILD_DIR)/part.img >/dev/null 2>&1
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	sgdisk -n 1:2048:+12M -t 1:8300 $@ >/dev/null 2>&1
	dd if=$(BUILD_DIR)/part.img of=$@ bs=512 seek=2048 conv=notrunc 2>/dev/null
	@echo "[OK] gpt-test.img"

# Boot with virtio-blk disk attached (x86_64 only)
qemu-disk: iso disk-image
	$(QEMU_CMD) -nographic -cdrom ranix.iso \
	  -drive file=ranix-disk.img,format=raw,if=virtio

image: all
	llvm-objcopy -O binary $(TARGET) kernel8.img
	@echo "[OK] kernel8.img hazır"

# Minimal bash-only Ranix for Raspberry Pi 4 — no tests, just a shell.
# Output: pi4-dist/ {kernel8.img, initramfs.cpio, config.txt}. Copy onto an SD
# alongside the Pi firmware (start4.elf, fixup4.dat, bcm2711-rpi-4-b.dtb).
.PHONY: pi-demo
pi-demo:
	$(MAKE) clean
	$(MAKE) ARCH=aarch64 BOARD=raspi4 DEMO=1 image
	@rm -f $(BUILD_DIR)/initramfs.cpio
	$(MAKE) ARCH=aarch64 BOARD=raspi4 DEMO=1 initramfs
	@mkdir -p pi4-dist
	@cp kernel8.img $(BUILD_DIR)/initramfs.cpio pi4-dist/
	@printf 'arm_64bit=1\nkernel=kernel8.img\ninitramfs initramfs.cpio followkernel\nenable_uart=1\n' > pi4-dist/config.txt
	@echo "[OK] Ranix Pi demo → pi4-dist/ (kernel8.img + initramfs.cpio + config.txt)"

TEST_TIMEOUT ?= 180
TEST_LOG = $(BUILD_DIR)/serial-test.log

ifeq ($(ARCH),x86_64)
test: iso $(BUILD_DIR)/gpt-test.img
	@rm -f $(TEST_LOG)
	timeout $(TEST_TIMEOUT) $(QEMU_CMD) -smp 4 -m 512M -display none \
	    -serial file:$(TEST_LOG) -boot d -cdrom ranix.iso \
	    -drive file=$(BUILD_DIR)/gpt-test.img,format=raw,if=virtio || true
else
test: initramfs $(TARGET)
	@rm -f $(TEST_LOG)
	timeout $(TEST_TIMEOUT) $(QEMU_CMD) -smp 4 -m 512M -display none \
	    -serial file:$(TEST_LOG) \
	    -kernel $(QEMU_KERNEL) -initrd $(BUILD_DIR)/initramfs.cpio || true
endif
	@echo "--- test output ---"
	@grep -E "^(PASS|FAIL):" $(TEST_LOG) || true
	@echo "-------------------"
	@grep "Results:" $(TEST_LOG) || echo "Results: (not found)"
	@grep -q "ALL_PASS" $(TEST_LOG) && echo "[OK] All tests passed" || \
	    { echo "[FAIL] Some tests failed"; exit 1; }
