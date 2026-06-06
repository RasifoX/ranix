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

# musl's ld-musl-*.so.1 IS both the dynamic linker and libc; ship it where
# PT_INTERP points. We use Alpine's musl 1.2.5 loader (extracted from the cached
# musl.apk) rather than the cross-toolchain's 1.2.2 libc.so: 1.2.5 understands
# DT_RELR relative relocations, so modern Alpine binaries run in-place. musl's
# ABI is stable, so userspace built against the 1.2.2 toolchain runs unchanged.
LDSO_NAME = ld-musl-$(ARCH).so.1
LDSO_SRC  = $(APK_DIR)/lib/$(LDSO_NAME)

# Stock glibc smoke test: a trivial binary built with a stock glibc gcc, shipped
# alongside the real ld.so + libc.so.6 it dynamically links to, so the suite
# exercises a genuine glibc ld.so/__libc_start_main startup. x86_64 uses the host
# gcc; aarch64 uses the cross glibc toolchain + its sysroot ld.so/libc.
ifeq ($(ARCH),aarch64)
    GLIBC_HOST_CC = aarch64-linux-gnu-gcc
    GLIBC_LDSO    = /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1
    GLIBC_LIBC    = /usr/aarch64-linux-gnu/lib/libc.so.6
    GLIBC_LDSO_DST = lib/ld-linux-aarch64.so.1
    GLIBC_LIBC_DST = lib/libc.so.6
else
    GLIBC_HOST_CC = gcc
    GLIBC_LDSO    = /lib64/ld-linux-x86-64.so.2
    GLIBC_LIBC    = /lib/x86_64-linux-gnu/libc.so.6
    GLIBC_LDSO_DST = lib64/ld-linux-x86-64.so.2
    GLIBC_LIBC_DST = lib/x86_64-linux-gnu/libc.so.6
endif
GLIBC_TEST_SRC = userspace/c_glibc.c
GLIBC_TEST_BIN = $(BUILD_DIR)/c-glibc
GLIBC_SPAWN_SRC = userspace/c_glibcspawn.c
GLIBC_SPAWN_BIN = $(BUILD_DIR)/c-glibcspawn

# Stock GNU coreutils proof: ship an unmodified host coreutils binary (echo),
# which dynamically links the same glibc shipped above. Exercises the full
# __libc_start_main path (locale, argv/environ) beyond the trivial c-glibc.
GNU_COREUTILS_BIN = /bin/echo

CC  = clang --target=$(ARCH)-none-elf
AS  = clang --target=$(ARCH)-none-elf
LD  = ld.lld

BUILD_DIR = build
TARGET    = $(BUILD_DIR)/ranix.elf

# ── build configuration (Kconfig-style) ─────────────────────────────────────
# .config (KEY=value) is the single source of truth. genconfig.sh turns it into
# a C header (CONFIG_* #defines, pulled in via include/kernel/config.h) and a
# Make fragment (CONFIG_* variables, included below so SRCS_* can drop gated
# sources). A missing .config seeds from defconfig — the full build. `make
# defconfig` / `make tinyconfig` switch presets. Both generated files are
# git-ignored; defconfig + tinyconfig are committed.
DOTCONFIG   = .config
AUTOCONF_H  = include/generated/autoconf.h
AUTOCONF_MK = $(BUILD_DIR)/auto.conf

$(DOTCONFIG):
	@cp defconfig $@
	@echo "[OK] seeded $@ from defconfig"

$(AUTOCONF_H) $(AUTOCONF_MK) &: $(DOTCONFIG) scripts/genconfig.sh
	@sh scripts/genconfig.sh $(DOTCONFIG) $(AUTOCONF_H) $(AUTOCONF_MK)

# Housekeeping/preset goals must not pull in (and thus regenerate) the config.
ifeq ($(filter clean defconfig tinyconfig,$(MAKECMDGOALS)),)
-include $(AUTOCONF_MK)
endif

INCLUDES = -Iinclude -Idrivers -I. -Iarch/$(ARCH)/include -I$(BUILD_DIR)

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
        drivers/net/virtio_net.c \
        drivers/net/bcmgenet.c
else ifeq ($(ARCH),x86_64)
    ARCH_CFLAGS = -mno-sse -mno-avx -mno-red-zone -m64 -mcmodel=kernel
    LINKER      = arch/x86_64/linker.ld
    QEMU_CMD    = qemu-system-x86_64 -cpu max -device isa-debug-exit,iobase=0x501,iosize=1
    QEMU_NET    = -netdev user,id=n0 -device virtio-net-pci,netdev=n0
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
        drivers/block/ahci.c \
        drivers/block/nvme.c \
        drivers/usb/xhci.c \
        drivers/net/virtio_net.c
endif

# Auto-discovered: every .c in kernel/net is compiled (no textual-includes here).
SRCS_NET = $(wildcard kernel/net/*.c)

# Optional filesystems: dropped from the build when CONFIG_FS_x != y (their
# *_register() calls in vfs.c are #if IS_ENABLED(CONFIG_FS_x)-guarded so the
# kernel still links). Anything not listed here is always built.
FS_GATED_OFF =
ifneq ($(CONFIG_FS_EXT2),y)
    FS_GATED_OFF += kernel/fs/ext2.c
endif
ifneq ($(CONFIG_FS_FAT),y)
    FS_GATED_OFF += kernel/fs/fat.c
endif
ifneq ($(CONFIG_FS_ISO9660),y)
    FS_GATED_OFF += kernel/fs/iso9660.c
endif
ifneq ($(CONFIG_FS_OVERLAY),y)
    FS_GATED_OFF += kernel/fs/overlay.c
endif
ifneq ($(CONFIG_FS_TMPFS),y)
    FS_GATED_OFF += kernel/fs/tmpfs.c
endif

# Auto-discovered. proc.c + signal.c are textual-#included by sched.c, and
# mm/mmu.c is not built standalone, so they are filtered out. kernel/sys/*.c
# (syscall_*.c) are #included by syscall.c and live in a subdir the kernel/*.c
# glob doesn't reach. New files under kernel/{,fs,mm,lib,tty} are picked up
# automatically — no hand-editing this list.
SRCS_KERNEL = $(filter-out kernel/proc.c kernel/signal.c,$(wildcard kernel/*.c)) \
    $(filter-out $(FS_GATED_OFF),$(wildcard kernel/fs/*.c)) \
    $(filter-out kernel/mm/mmu.c,$(wildcard kernel/mm/*.c)) \
    $(wildcard kernel/lib/*.c) \
    $(wildcard kernel/tty/*.c) \
    drivers/video/fbdev.c \
    drivers/input/input_core.c

SRCS_C   = $(SRCS_KERNEL) $(SRCS_NET) $(SRCS_ARCH) $(SRCS_DRIVERS)
SRCS_ASM = $(SRCS_ARCH_ASM)

CFLAGS = -ffreestanding -nostdlib -nostdinc -O2 -Wall -fno-omit-frame-pointer $(ARCH_CFLAGS) $(INCLUDES) -MMD -MP
LDFLAGS = -nostdlib

OBJS  = $(SRCS_C:%.c=$(BUILD_DIR)/%.o)
OBJS += $(SRCS_ASM:%.S=$(BUILD_DIR)/%.o)
DEPS  = $(SRCS_C:%.c=$(BUILD_DIR)/%.d)
-include $(DEPS)

BUSYBOX_APK_aarch64 = https://dl-cdn.alpinelinux.org/alpine/v3.21/main/aarch64/busybox-static-1.37.0-r14.apk
BUSYBOX_APK_x86_64  = https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64/busybox-static-1.37.0-r14.apk

# apk.static (statically-linked musl apk-tools) + a tiny target package and its
# only dependency (musl). Fetched on the HOST at build time and shipped into the
# initramfs so the package-install path can be exercised on-device, offline.
APK_BASE          = https://dl-cdn.alpinelinux.org/alpine/v3.21/main/$(ARCH)
APK_TOOLS_APK     = $(APK_BASE)/apk-tools-static-2.14.6-r3.apk
APK_MUSL_APK      = $(APK_BASE)/musl-1.2.5-r11.apk
APK_TREE_APK      = $(APK_BASE)/tree-2.2.1-r0.apk
# Alpine's package-signing public keys. Shipped into /etc/apk/keys so a live
# `apk add` verifies the signed APKINDEX + packages without --allow-untrusted.
APK_KEYS_APK      = $(APK_BASE)/alpine-keys-2.5-r0.apk
# Mozilla CA bundle (extracted from Alpine's ca-certificates-bundle apk) shipped
# to /etc/ssl/certs so a live `apk add` over HTTPS verifies the mirror's TLS cert
# (apk-tools' OpenSSL looks under OPENSSLDIR=/etc/ssl).
APK_CACERTS_APK   = $(APK_BASE)/ca-certificates-bundle-20260413-r0.apk

# Debian (glibc) package path: dpkg + a tiny target package (hello) and the
# runtime-library closure, fetched as .deb files on the HOST at build time and
# shipped into the initramfs so `dpkg -i` can run on-device, offline. The
# binaries are amd64 ELF + interp /lib64/ld-linux-x86-64.so.2, so this only
# applies to the x86_64 build (mirrors the x86_64-only gnu-echo); aarch64 skips.
DEB_SUITE          = bookworm
DEB_PKGS           = dpkg hello

BASH_VER           = 5.2.37
BASH_SRC_URL       = https://ftp.gnu.org/gnu/bash/bash-$(BASH_VER).tar.gz

CURL_VER           = 8.11.1
CURL_SRC_URL       = https://curl.se/download/curl-$(CURL_VER).tar.gz

PY_VER             = 3.12.3
PY_XY              = 3.12
PY_SRC_URL         = https://www.python.org/ftp/python/$(PY_VER)/Python-$(PY_VER).tgz

.PHONY: all clean qemu image initramfs bash-static curl python apk-static test persisttest flakehunt vdso defconfig tinyconfig

# Switch the active .config to a committed preset and drop the stale generated
# files so the next build regenerates them. defconfig = full build; tinyconfig =
# minimal bootable core.
defconfig tinyconfig:
	@cp $@ $(DOTCONFIG)
	@rm -f $(AUTOCONF_H) $(AUTOCONF_MK)
	@echo "[OK] .config <- $@"

# Run the suite N times, report any test that isn't 100% green (flake hunter).
flakehunt:
	@ITERS=$(or $(ITERS),20) ARCH=$(ARCH) BOARD=$(BOARD) bash tests/flakehunt.sh

# vDSO: a freestanding PIC shared object the kernel maps into every process for
# syscall-free clock_gettime/gettimeofday. Linked with a version script and no
# relocations so it can be mapped without a dynamic loader. (Kernel wiring —
# mapping + vvar updater + AT_SYSINFO_EHDR — lands separately.)
VDSO_SO = $(BUILD_DIR)/vdso-$(ARCH).so
vdso: $(VDSO_SO)
$(VDSO_SO): vdso/vdso.c vdso/vdso_$(ARCH).lds vdso/vdso_layout.lds include/uapi/vdso.h
	@mkdir -p $(BUILD_DIR)
	$(MUSL_CC) -shared -fPIC -fno-stack-protector -nostdlib -O2 -Iinclude \
	    -Wl,--version-script=vdso/vdso_$(ARCH).lds -Wl,-T,vdso/vdso_layout.lds \
	    -Wl,-soname,linux-vdso.so.1 -Wl,--hash-style=both -Wl,-Bsymbolic \
	    -Wl,-z,max-page-size=4096 -o $@ vdso/vdso.c
	@echo "[OK] $@ ($$($(MUSL_CC:gcc=nm) -D $@ 2>/dev/null | grep -c @@) vDSO symbols, no relocs)"

# Embed the vDSO image as a byte array for the kernel to map.
$(BUILD_DIR)/vdso_image.h: $(VDSO_SO)
	@printf 'static const unsigned char vdso_image[] = {\n' > $@
	@od -An -v -tx1 $(VDSO_SO) | sed 's/[0-9a-f][0-9a-f]/0x&,/g' >> $@
	@printf '};\nstatic const unsigned long vdso_image_len = sizeof(vdso_image);\n' >> $@

$(BUILD_DIR)/kernel/vdso.o: $(BUILD_DIR)/vdso_image.h

all: initramfs $(TARGET)

# Userspace programs in the initramfs. Core always ships; tests ship unless DEMO=1.
USER_CORE_BINS = userspace/init userspace/hello userspace/echo_server userspace/httpd_test
# Auto-discovered: the bench harness plus every userspace/test_*.c. Adding a
# test_*.c is enough — no hand-editing this list (and no more merge conflicts
# from everyone appending to it).
USER_TEST_BINS = userspace/bench $(patsubst %.c,%,$(wildcard userspace/test_*.c))

initramfs: $(BUILD_DIR)/initramfs.cpio

CACHE_DIR          = .cache
BUSYBOX_APK_CACHED = $(CACHE_DIR)/busybox-$(ARCH).apk
BUSYBOX_EXTRACT    = $(CACHE_DIR)/busybox-$(ARCH)
APK_DIR            = $(CACHE_DIR)/apk-$(ARCH)
APK_STATIC_BIN     = $(APK_DIR)/apk.static
APK_PKGS           = $(APK_DIR)/musl.apk $(APK_DIR)/tree.apk
APK_KEYS_DIR       = $(APK_DIR)/keys
APK_CACERTS        = $(APK_DIR)/ca-certificates.crt
BASH_CACHED        = $(CACHE_DIR)/bash-dynamic-$(ARCH)
CURL_BIN           = $(CACHE_DIR)/curl-$(ARCH)/src/curl
SSL_SERVER_BIN     = $(CACHE_DIR)/mbedtls-$(ARCH)/ssl_server
PYTHON_HOST        = $(CACHE_DIR)/python-host/python
PYTHON_BIN         = $(CACHE_DIR)/python-$(ARCH)/python
PYTHON_ZIP         = $(CACHE_DIR)/python-$(ARCH)/python$(subst .,,$(PY_XY)).zip
# Debian dpkg staging (x86_64 only). DEB_ROOTFS is the extracted .deb closure
# laid out at real multiarch paths (/usr/bin, /bin, /lib/x86_64-linux-gnu, ...)
# ready to copy straight into the initramfs; the marker file gates the rule.
DEB_DIR            = $(CACHE_DIR)/deb-amd64
DEB_ROOTFS         = $(CACHE_DIR)/deb-rootfs-amd64
DEB_STAMP          = $(DEB_ROOTFS)/.stamp

# mbedTLS — userspace TLS/HTTPS, cross-built static like bash/busybox
MBEDTLS_VER    = 3.6.2
MBEDTLS_URL    = https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBEDTLS_VER)/mbedtls-$(MBEDTLS_VER).tar.bz2
MBEDTLS_TAR    = $(CACHE_DIR)/mbedtls-$(MBEDTLS_VER).tar.bz2
MBEDTLS_DIR    = $(CACHE_DIR)/mbedtls-$(ARCH)
MBEDTLS_A      = $(MBEDTLS_DIR)/library/libmbedtls.a
MUSL_AR        = $(MUSL_CC:gcc=ar)

# dropbear — SSH server + client (dbclient), cross-built static multi-binary.
# Self-contained crypto (libtomcrypt/libtommath), no external deps.
DROPBEAR_VER    = 2026.91
DROPBEAR_URL    = https://matt.ucc.asn.au/dropbear/releases/dropbear-$(DROPBEAR_VER).tar.bz2
DROPBEAR_TAR    = $(CACHE_DIR)/dropbear-$(DROPBEAR_VER).tar.bz2
DROPBEAR_DIR    = $(CACHE_DIR)/dropbear-$(ARCH)
DROPBEAR_BIN    = $(DROPBEAR_DIR)/dropbearmulti

dropbear: $(DROPBEAR_BIN)
$(DROPBEAR_BIN):
	@mkdir -p $(CACHE_DIR)
	@if [ ! -f "$(DROPBEAR_TAR)" ]; then echo "Downloading dropbear $(DROPBEAR_VER)..."; \
		wget -q "$(DROPBEAR_URL)" -O "$(DROPBEAR_TAR)" || { rm -f "$(DROPBEAR_TAR)"; echo "ERROR: dropbear download failed"; exit 1; }; fi
	@rm -rf "$(DROPBEAR_DIR)"; mkdir -p "$(DROPBEAR_DIR)"
	@tar xjf "$(DROPBEAR_TAR)" -C "$(DROPBEAR_DIR)" --strip-components=1
	@echo "Building dropbear (static multi) for $(ARCH)..."
	@cd "$(DROPBEAR_DIR)" && CC="$(MUSL_CC)" ./configure --host=$(ARCH)-linux-musl \
		--disable-zlib --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp >/dev/null 2>&1
	@cd "$(DROPBEAR_DIR)" && $(MAKE) CC="$(MUSL_CC)" MULTI=1 STATIC=1 \
		PROGRAMS="dropbear dbclient dropbearkey scp" -j$$(nproc) >/dev/null 2>&1
	@llvm-strip "$@" 2>/dev/null || true
	@echo "[OK] dropbear $(DROPBEAR_VER) for $(ARCH) ($$(du -h $@ | cut -f1))"

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

apk-static: $(APK_STATIC_BIN)

$(APK_STATIC_BIN):
	@mkdir -p $(APK_DIR)
	@if [ ! -f "$(APK_DIR)/apk-tools-static.apk" ]; then \
		echo "Downloading apk-tools-static for $(ARCH)..."; \
		wget -q "$(APK_TOOLS_APK)" -O "$(APK_DIR)/apk-tools-static.apk" || \
		    { rm -f "$(APK_DIR)/apk-tools-static.apk"; echo "ERROR: apk-tools-static download failed"; exit 1; }; \
	fi
	@if [ ! -f "$(APK_DIR)/musl.apk" ]; then \
		wget -q "$(APK_MUSL_APK)" -O "$(APK_DIR)/musl.apk" || \
		    { rm -f "$(APK_DIR)/musl.apk"; echo "ERROR: musl.apk download failed"; exit 1; }; \
	fi
	@if [ ! -f "$(APK_DIR)/tree.apk" ]; then \
		wget -q "$(APK_TREE_APK)" -O "$(APK_DIR)/tree.apk" || \
		    { rm -f "$(APK_DIR)/tree.apk"; echo "ERROR: tree.apk download failed"; exit 1; }; \
	fi
	@tar xzf "$(APK_DIR)/apk-tools-static.apk" -C "$(APK_DIR)" sbin/apk.static 2>/dev/null
	@cp "$(APK_DIR)/sbin/apk.static" "$@"
	@chmod +x "$@"
	@echo "[OK] apk.static for $(ARCH) ready ($$(du -h $@ | cut -f1))"

# Alpine signing keys for the target arch, extracted from the cached alpine-keys
# apk. The per-arch set under usr/share/apk/keys/$(ARCH) carries the key that
# signs the v3.21 APKINDEX, so a live `apk add` verifies without --allow-untrusted.
$(APK_KEYS_DIR):
	@mkdir -p "$(APK_DIR)"
	@if [ ! -f "$(APK_DIR)/alpine-keys.apk" ]; then \
		echo "Downloading alpine-keys for $(ARCH)..."; \
		wget -q "$(APK_KEYS_APK)" -O "$(APK_DIR)/alpine-keys.apk" || \
		    { rm -f "$(APK_DIR)/alpine-keys.apk"; echo "ERROR: alpine-keys download failed"; exit 1; }; \
	fi
	@rm -rf "$@" "$(APK_DIR)/usr" && mkdir -p "$@"
	@gzip -dc "$(APK_DIR)/alpine-keys.apk" 2>/dev/null | \
	    tar x -C "$(APK_DIR)" usr/share/apk/keys 2>/dev/null
	@cp "$(APK_DIR)"/usr/share/apk/keys/$(ARCH)/*.rsa.pub "$@/"

# Mozilla CA bundle extracted from Alpine's ca-certificates-bundle apk, cached for
# the initramfs so live HTTPS package fetches verify the mirror certificate.
$(APK_CACERTS):
	@mkdir -p "$(APK_DIR)"
	@if [ ! -f "$(APK_DIR)/ca-certificates-bundle.apk" ]; then \
		echo "Downloading ca-certificates-bundle for $(ARCH)..."; \
		wget -q "$(APK_CACERTS_APK)" -O "$(APK_DIR)/ca-certificates-bundle.apk" || \
		    { rm -f "$(APK_DIR)/ca-certificates-bundle.apk"; echo "ERROR: ca-certificates download failed"; exit 1; }; \
	fi
	@tar xzf "$(APK_DIR)/ca-certificates-bundle.apk" -C "$(APK_DIR)" etc/ssl/certs/ca-certificates.crt 2>/dev/null
	@cp "$(APK_DIR)/etc/ssl/certs/ca-certificates.crt" "$@"

# Debian dpkg + closure: fetch the .deb set on the host, extract it into a rootfs
# laid out at real multiarch paths, and seed a minimal /var/lib/dpkg admin dir so
# `dpkg -i` can configure packages offline. libc6 is pre-marked installed (Ranix
# ships the host glibc, which satisfies the libc6 dependency); ldconfig and
# start-stop-daemon are no-op stubs dpkg requires on PATH. x86_64 only.
$(DEB_STAMP):
ifeq ($(ARCH),x86_64)
	@if command -v dpkg-deb >/dev/null 2>&1; then \
		sh scripts/fetch-deb.sh "$(CACHE_DIR)" "$(DEB_SUITE)" amd64 $(DEB_PKGS) >/dev/null || \
		    { echo "[skip] dpkg: Debian package fetch failed"; exit 0; }; \
		rm -rf "$(DEB_ROOTFS)"; mkdir -p "$(DEB_ROOTFS)"; \
		for d in "$(DEB_DIR)"/*.deb; do \
			case "$$d" in *hello_*) continue;; esac; \
			dpkg-deb -x "$$d" "$(DEB_ROOTFS)" 2>/dev/null; \
		done; \
		mkdir -p "$(DEB_ROOTFS)/sbin"; \
		printf '#!/bin/sh\nexit 0\n' > "$(DEB_ROOTFS)/sbin/ldconfig"; \
		printf '#!/bin/sh\nexit 0\n' > "$(DEB_ROOTFS)/sbin/start-stop-daemon"; \
		chmod +x "$(DEB_ROOTFS)/sbin/ldconfig" "$(DEB_ROOTFS)/sbin/start-stop-daemon"; \
		mkdir -p "$(DEB_ROOTFS)/var/lib/dpkg/info" "$(DEB_ROOTFS)/var/lib/dpkg/updates" \
		         "$(DEB_ROOTFS)/var/lib/dpkg/triggers" "$(DEB_ROOTFS)/var/log"; \
		printf 'Package: libc6\nStatus: install ok installed\nPriority: required\nSection: libs\nMaintainer: Ranix <root@ranix>\nArchitecture: amd64\nVersion: 2.36-9\nDescription: GNU C Library shared libraries (provided by Ranix host glibc)\n\n' \
		    > "$(DEB_ROOTFS)/var/lib/dpkg/status"; \
		: > "$(DEB_ROOTFS)/var/lib/dpkg/available"; \
		cp "$(DEB_DIR)"/hello_*.deb "$(DEB_ROOTFS)/hello.deb"; \
		touch "$@"; \
		echo "[OK] Debian dpkg rootfs staged (x86_64)"; \
	else echo "[skip] dpkg: dpkg-deb not on host, Debian package path unavailable"; fi
else
	@true
endif

# Extract the musl 1.2.5 ld-musl loader from the cached Alpine musl.apk; this is
# the system loader (LDSO_SRC) shipped to /lib. Depends on apk-static, which
# downloads musl.apk.
$(LDSO_SRC): $(APK_STATIC_BIN)
	@tar xzf "$(APK_DIR)/musl.apk" -C "$(APK_DIR)" lib/$(LDSO_NAME) 2>/dev/null
	@chmod +x "$@"

$(GLIBC_TEST_BIN): $(GLIBC_TEST_SRC)
	@if command -v $(GLIBC_HOST_CC) >/dev/null && [ -e $(GLIBC_LDSO) ] && [ -e $(GLIBC_LIBC) ]; then \
		$(GLIBC_HOST_CC) -O2 -o $@ $< && \
		echo "[OK] c-glibc (stock glibc) built for $(ARCH)"; \
	else echo "[skip] c-glibc: stock glibc toolchain/loader unavailable"; fi

$(GLIBC_SPAWN_BIN): $(GLIBC_SPAWN_SRC)
	@if command -v $(GLIBC_HOST_CC) >/dev/null && [ -e $(GLIBC_LDSO) ] && [ -e $(GLIBC_LIBC) ]; then \
		$(GLIBC_HOST_CC) -O2 -pthread -o $@ $< && \
		echo "[OK] c-glibcspawn (stock glibc) built for $(ARCH)"; \
	else echo "[skip] c-glibcspawn: stock glibc toolchain/loader unavailable"; fi
GLIBC_TEST_DEP = $(GLIBC_TEST_BIN) $(GLIBC_SPAWN_BIN)
ifeq ($(ARCH),x86_64)
DEB_DEP = $(DEB_STAMP)
endif

$(BUILD_DIR)/initramfs.cpio: $(USER_CORE_BINS) $(USER_TEST_BINS) $(BASH_CACHED) $(DROPBEAR_BIN) $(CURL_BIN) $(SSL_SERVER_BIN) $(PYTHON_BIN) $(APK_STATIC_BIN) $(APK_KEYS_DIR) $(APK_CACERTS) $(LDSO_SRC) $(GLIBC_TEST_DEP) $(DEB_DEP) $(wildcard tests/ranix_test.sh)
	@mkdir -p $(BUILD_DIR)/initramfs/bin $(BUILD_DIR)/initramfs/etc \
	           $(BUILD_DIR)/initramfs/home/test $(CACHE_DIR)
	@cp $(USER_CORE_BINS) $(BUILD_DIR)/initramfs/bin/
	@if [ -z "$(DEMO)" ]; then cp $(USER_TEST_BINS) $(BUILD_DIR)/initramfs/bin/; fi
	@if [ -z "$(DEMO)" ] && [ -f $(BUILD_DIR)/initramfs/bin/test_suidhelper ]; then \
		chmod u+s $(BUILD_DIR)/initramfs/bin/test_suidhelper; fi
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
	            ps kill df free top uname head tail wc echo printf vi \
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
	@cp "$(CURL_BIN)" $(BUILD_DIR)/initramfs/bin/curl
	@chmod +x $(BUILD_DIR)/initramfs/bin/curl
	@cp "$(SSL_SERVER_BIN)" $(BUILD_DIR)/initramfs/bin/ssl_server
	@chmod +x $(BUILD_DIR)/initramfs/bin/ssl_server
	@if [ -x "$(PYTHON_BIN)" ]; then \
		cp "$(PYTHON_BIN)" $(BUILD_DIR)/initramfs/bin/python3; \
		chmod +x $(BUILD_DIR)/initramfs/bin/python3; \
		ln -sf python3 $(BUILD_DIR)/initramfs/bin/python; \
		mkdir -p $(BUILD_DIR)/initramfs/usr/lib; \
		cp "$(PYTHON_ZIP)" $(BUILD_DIR)/initramfs/usr/lib/python$(subst .,,$(PY_XY)).zip; \
	fi
	@if [ -x "$(GLIBC_TEST_BIN)" ]; then \
		cp "$(GLIBC_TEST_BIN)" $(BUILD_DIR)/initramfs/bin/c-glibc; \
		chmod +x $(BUILD_DIR)/initramfs/bin/c-glibc; \
		mkdir -p $(BUILD_DIR)/initramfs/$(dir $(GLIBC_LDSO_DST)) $(BUILD_DIR)/initramfs/$(dir $(GLIBC_LIBC_DST)); \
		cp "$(GLIBC_LDSO)" $(BUILD_DIR)/initramfs/$(GLIBC_LDSO_DST); \
		cp "$(GLIBC_LIBC)" $(BUILD_DIR)/initramfs/$(GLIBC_LIBC_DST); \
		if [ "$(ARCH)" = "x86_64" ] && [ -x "$(GNU_COREUTILS_BIN)" ]; then \
			cp "$(GNU_COREUTILS_BIN)" $(BUILD_DIR)/initramfs/bin/gnu-echo; \
			chmod +x $(BUILD_DIR)/initramfs/bin/gnu-echo; \
		fi; \
		if [ -x "$(GLIBC_SPAWN_BIN)" ]; then \
			cp "$(GLIBC_SPAWN_BIN)" $(BUILD_DIR)/initramfs/bin/c-glibcspawn; \
			chmod +x $(BUILD_DIR)/initramfs/bin/c-glibcspawn; \
		fi; \
	fi
	@if [ -f "$(DEB_STAMP)" ]; then \
		(cd "$(DEB_ROOTFS)" && find . -path ./hello.deb -prune -o -print | cpio -pdu --quiet "$(CURDIR)/$(BUILD_DIR)/initramfs" 2>/dev/null); \
		mkdir -p $(BUILD_DIR)/initramfs/var/cache; \
		cp "$(DEB_ROOTFS)/hello.deb" $(BUILD_DIR)/initramfs/var/cache/hello.deb; \
	fi
	@if [ -x "$(APK_STATIC_BIN)" ]; then \
		cp "$(APK_STATIC_BIN)" $(BUILD_DIR)/initramfs/bin/apk.static; \
		chmod +x $(BUILD_DIR)/initramfs/bin/apk.static; \
		mkdir -p $(BUILD_DIR)/initramfs/etc/apk/keys \
		         $(BUILD_DIR)/initramfs/var/apkrepo; \
		echo "$(ARCH)" > $(BUILD_DIR)/initramfs/etc/apk/arch; \
		cp "$(APK_DIR)/musl.apk" $(BUILD_DIR)/initramfs/var/apkrepo/musl.apk; \
		cp "$(APK_DIR)/tree.apk" $(BUILD_DIR)/initramfs/var/apkrepo/tree.apk; \
		cp "$(APK_KEYS_DIR)"/*.rsa.pub $(BUILD_DIR)/initramfs/etc/apk/keys/ 2>/dev/null; \
		if [ -f "$(APK_CACERTS)" ]; then \
			mkdir -p $(BUILD_DIR)/initramfs/etc/ssl/certs; \
			cp "$(APK_CACERTS)" $(BUILD_DIR)/initramfs/etc/ssl/certs/ca-certificates.crt; \
			cp "$(APK_CACERTS)" $(BUILD_DIR)/initramfs/etc/ssl/cert.pem; \
		fi; \
	fi
	@cp "$(DROPBEAR_BIN)" $(BUILD_DIR)/initramfs/bin/dropbearmulti
	@chmod +x $(BUILD_DIR)/initramfs/bin/dropbearmulti
	@for c in dropbear dbclient dropbearkey scp ssh; do ln -sf dropbearmulti $(BUILD_DIR)/initramfs/bin/$$c; done
	@mkdir -p $(BUILD_DIR)/initramfs/etc/dropbear $(BUILD_DIR)/initramfs/root/.ssh
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
	@printf '/bin/sh\n/bin/bash\n' > $(BUILD_DIR)/initramfs/etc/shells
	@rm -f $(BUILD_DIR)/initramfs/etc/shadow
	@printf 'root:$$6$$ranixsalt$$.hGbivmRHVhZxfxORfv1MD0sy40LgabzFqZTIyXG4KhrIN/YF.rMrSGyJrMMXSi8loXADZl.uX7/8UWcfZA2q1:19900:0:99999:7:::\ntest:$$6$$testsalt1$$lXcMUCMFONojgODTdrEnx7EPsgOI/v0k3TnAx/ahXxEkWgqhD/FFSAdlwaBXL3Ofgj/mPWy5.k8RlqZTAGA1T1:19900:0:99999:7:::\n' \
	    > $(BUILD_DIR)/initramfs/etc/shadow
	@chmod 640 $(BUILD_DIR)/initramfs/etc/shadow
	@printf 'root:x:0:\ntest:x:1000:\n' > $(BUILD_DIR)/initramfs/etc/group
	@mkdir -p $(BUILD_DIR)/initramfs/fstabtmp
	@printf 'tmpfs /fstabtmp tmpfs defaults 0 0\nproc /proc proc defaults 0 0\n' > $(BUILD_DIR)/initramfs/etc/fstab
	@printf 'nameserver 10.0.2.3\nsearch localdomain\noptions ndots:1 timeout:2 attempts:2\n' > $(BUILD_DIR)/initramfs/etc/resolv.conf
	@mkdir -p $(BUILD_DIR)/initramfs/etc/ssl/certs
	@if [ -f /etc/ssl/certs/ca-certificates.crt ]; then \
		cp /etc/ssl/certs/ca-certificates.crt $(BUILD_DIR)/initramfs/etc/ssl/certs/ca-certificates.crt; fi
	@mkdir -p $(BUILD_DIR)/initramfs/usr/share/zoneinfo/Europe $(BUILD_DIR)/initramfs/usr/share/zoneinfo/Etc
	@if [ -f /usr/share/zoneinfo/Europe/Istanbul ]; then \
		cp /usr/share/zoneinfo/Europe/Istanbul $(BUILD_DIR)/initramfs/usr/share/zoneinfo/Europe/Istanbul; \
		cp -L /usr/share/zoneinfo/UTC          $(BUILD_DIR)/initramfs/usr/share/zoneinfo/Etc/UTC 2>/dev/null || true; \
		cp /usr/share/zoneinfo/Europe/Istanbul $(BUILD_DIR)/initramfs/etc/localtime; \
		printf 'Europe/Istanbul\n' > $(BUILD_DIR)/initramfs/etc/timezone; \
	fi
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

# mbedTLS static libs (downloaded + cross-built once per arch, cached)
$(MBEDTLS_A):
	@mkdir -p $(CACHE_DIR)
	@if [ ! -f "$(MBEDTLS_TAR)" ]; then \
		echo "Downloading mbedTLS $(MBEDTLS_VER)..."; \
		wget -q "$(MBEDTLS_URL)" -O "$(MBEDTLS_TAR)" || \
		  { rm -f "$(MBEDTLS_TAR)"; echo "ERROR: mbedTLS download failed"; exit 1; }; \
	fi
	@rm -rf $(MBEDTLS_DIR)
	@tar xjf "$(MBEDTLS_TAR)" -C $(CACHE_DIR)
	@mv $(CACHE_DIR)/mbedtls-$(MBEDTLS_VER) $(MBEDTLS_DIR)
	@$(MAKE) -C $(MBEDTLS_DIR) lib CC=$(MUSL_CC) AR=$(MUSL_AR) >/dev/null 2>&1
	@echo "[OK] mbedTLS built ($(ARCH))"

# curl — real command-line HTTP/HTTPS client, cross-built against musl + mbedTLS.
curl: $(CURL_BIN)
$(CURL_BIN): $(MBEDTLS_A)
	@mkdir -p $(CACHE_DIR)
	@if [ ! -f "$(CACHE_DIR)/curl-$(CURL_VER).tar.gz" ]; then \
		echo "Downloading curl $(CURL_VER) source..."; \
		wget -q "$(CURL_SRC_URL)" -O "$(CACHE_DIR)/curl-$(CURL_VER).tar.gz" || \
		    { rm -f "$(CACHE_DIR)/curl-$(CURL_VER).tar.gz"; echo "ERROR: curl download failed"; exit 1; }; \
	fi
	@echo "Building curl for $(ARCH)..."
	@rm -rf "$(CACHE_DIR)/curl-$(ARCH)"
	@mkdir -p "$(CACHE_DIR)/curl-$(ARCH)"
	@tar xzf "$(CACHE_DIR)/curl-$(CURL_VER).tar.gz" -C "$(CACHE_DIR)/curl-$(ARCH)" --strip-components=1
	@cd "$(CACHE_DIR)/curl-$(ARCH)" && \
	    CC="$(MUSL_CC)" \
	    CPPFLAGS="-I$(CURDIR)/$(MBEDTLS_DIR)/include" \
	    LDFLAGS="-L$(CURDIR)/$(MBEDTLS_DIR)/library" \
	    LIBS="-lmbedtls -lmbedx509 -lmbedcrypto" \
	    ./configure --host=$(ARCH)-linux-musl \
	                --with-mbedtls \
	                --disable-shared --enable-static \
	                --without-libpsl --without-zlib --without-nghttp2 --disable-ldap \
	                --without-brotli --without-zstd --without-libidn2 \
	                --disable-docs --disable-manual \
	                > /dev/null 2>&1 && \
	    $(MAKE) -j$$(nproc) > /dev/null 2>&1
	@llvm-strip "$@" 2>/dev/null || strip "$@" 2>/dev/null || true
	@echo "[OK] curl $(CURL_VER) for $(ARCH) ready ($$(du -h $@ | cut -f1))"

# CPython — a real Python 3 interpreter. The cross-build needs a host
# interpreter of the same version to freeze modules, so build that first.
$(PYTHON_HOST):
	@mkdir -p $(CACHE_DIR)
	@if [ ! -f "$(CACHE_DIR)/Python-$(PY_VER).tgz" ]; then \
		echo "Downloading CPython $(PY_VER) source..."; \
		wget -q "$(PY_SRC_URL)" -O "$(CACHE_DIR)/Python-$(PY_VER).tgz" || \
		    { rm -f "$(CACHE_DIR)/Python-$(PY_VER).tgz"; echo "ERROR: python download failed"; exit 1; }; \
	fi
	@echo "Building host CPython $(PY_VER)..."
	@rm -rf "$(CACHE_DIR)/python-host"
	@mkdir -p "$(CACHE_DIR)/python-host"
	@tar xzf "$(CACHE_DIR)/Python-$(PY_VER).tgz" -C "$(CACHE_DIR)/python-host" --strip-components=1
	@cd "$(CACHE_DIR)/python-host" && ./configure >/dev/null 2>&1 && $(MAKE) -j$$(nproc) >/dev/null 2>&1
	@test -x "$@" && echo "[OK] host CPython ready"

# Cross-build: musl, static C extensions (no shared loader search), every
# optional module needing an external lib (zlib/ssl/ctypes/curses/...) dropped.
python: $(PYTHON_BIN)
$(PYTHON_BIN): $(PYTHON_HOST)
	@echo "Building CPython $(PY_VER) for $(ARCH)..."
	@rm -rf "$(CACHE_DIR)/python-$(ARCH)"
	@mkdir -p "$(CACHE_DIR)/python-$(ARCH)"
	@tar xzf "$(CACHE_DIR)/Python-$(PY_VER).tgz" -C "$(CACHE_DIR)/python-$(ARCH)" --strip-components=1
	@cd "$(CACHE_DIR)/python-$(ARCH)" && \
	    CC="$(MUSL_CC)" CONFIG_SITE=/dev/null \
	    PKG_CONFIG_LIBDIR=/nonexistent PKG_CONFIG_PATH= \
	    ./configure --host=$(ARCH)-linux-musl --build=$$(uname -m)-linux-gnu \
	                --with-build-python=$(CURDIR)/$(PYTHON_HOST) \
	                --disable-test-modules --without-ensurepip \
	                --disable-shared --disable-ipv6 --prefix=/usr \
	                ac_cv_file__dev_ptmx=yes ac_cv_file__dev_ptc=no \
	                ac_cv_buggy_getaddrinfo=no ac_cv_header_zlib_h=no \
	                py_cv_module_zlib=n/a py_cv_module__ctypes=n/a \
	                py_cv_module__curses=n/a py_cv_module__curses_panel=n/a \
	                py_cv_module_readline=n/a py_cv_module__tkinter=n/a \
	                py_cv_module_nis=n/a py_cv_module_ossaudiodev=n/a \
	                py_cv_module_spwd=n/a py_cv_module__dbm=n/a \
	                py_cv_module__gdbm=n/a py_cv_module__lzma=n/a \
	                py_cv_module__bz2=n/a py_cv_module__ssl=n/a \
	                py_cv_module__hashlib=n/a py_cv_module__sqlite3=n/a \
	                py_cv_module__uuid=n/a \
	                >/dev/null 2>&1
	@cd "$(CACHE_DIR)/python-$(ARCH)" && $(MAKE) python -j$$(nproc) >/dev/null 2>&1
	@$(CURDIR)/$(PYTHON_HOST) scripts/py_stdlib_zip.py \
	    "$(CURDIR)/$(CACHE_DIR)/python-$(ARCH)/Lib" "$(CURDIR)/$(PYTHON_ZIP)"
	@llvm-strip "$(PYTHON_BIN)" 2>/dev/null || strip "$(PYTHON_BIN)" 2>/dev/null || true
	@echo "[OK] CPython $(PY_VER) for $(ARCH) ready (bin $$(du -h $(PYTHON_BIN)|cut -f1), stdlib $$(du -h $(PYTHON_ZIP)|cut -f1))"

# mbedTLS ssl_server — a loopback HTTPS server for the offline curl test. Uses
# mbedTLS's built-in test cert/key, so no external PKI files are needed. Pinned
# to 127.0.0.1 so it doesn't bind IPv6-only (AI_PASSIVE picks :: first) and leave
# curl's IPv4 connect refused.
$(SSL_SERVER_BIN): $(MBEDTLS_A)
	@sed -i 's/mbedtls_net_bind(&listen_fd, NULL, "4433"/mbedtls_net_bind(\&listen_fd, "127.0.0.1", "4433"/' \
	    $(MBEDTLS_DIR)/programs/ssl/ssl_server.c
	$(MUSL_CC) -static -O2 -I$(MBEDTLS_DIR)/include -I$(MBEDTLS_DIR)/tests/include \
	    -I$(MBEDTLS_DIR)/library -o $@ \
	    $(MBEDTLS_DIR)/programs/ssl/ssl_server.c $(MBEDTLS_DIR)/tests/src/certs.c \
	    $(MBEDTLS_DIR)/library/libmbedtls.a \
	    $(MBEDTLS_DIR)/library/libmbedx509.a \
	    $(MBEDTLS_DIR)/library/libmbedcrypto.a
	@llvm-strip "$@" 2>/dev/null || strip "$@" 2>/dev/null || true

# HTTPS client diagnostic — statically links mbedTLS
userspace/test_tls: userspace/test_tls.c $(MBEDTLS_A)
	$(MUSL_CC) -static -O2 -I$(MBEDTLS_DIR)/include -o $@ $< \
	    $(MBEDTLS_DIR)/library/libmbedtls.a \
	    $(MBEDTLS_DIR)/library/libmbedx509.a \
	    $(MBEDTLS_DIR)/library/libmbedcrypto.a

# Two-pass link for in-kernel panic symbolization: stage1 fixes .text addresses,
# gen_ksyms emits the symbol table + build identity into .rodata (after .text,
# so addresses stay valid), and the final link folds it in.
$(BUILD_DIR)/ranix.stage1.elf: $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJS) -o $@

$(BUILD_DIR)/ksyms.o: $(BUILD_DIR)/ranix.stage1.elf scripts/gen_ksyms.sh
	sh scripts/gen_ksyms.sh $< $(ARCH) > $(BUILD_DIR)/ksyms_gen.c
	$(CC) $(CFLAGS) -c $(BUILD_DIR)/ksyms_gen.c -o $@

$(TARGET): $(OBJS) $(BUILD_DIR)/ksyms.o
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER) $(OBJS) $(BUILD_DIR)/ksyms.o -o $@
	@if [ "$(ARCH)" = "aarch64" ] && [ "$(BOARD)" != "raspi4" ]; then \
		llvm-objcopy -O binary $@ $(BUILD_DIR)/ranix.bin; \
	fi
	@if [ "$(ARCH)" = "x86_64" ]; then echo "[OK] Ranix built → arch=$(ARCH)"; else echo "[OK] Ranix built → arch=$(ARCH) board=$(BOARD)"; fi

$(BUILD_DIR)/%.o: %.c $(AUTOCONF_H)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S $(AUTOCONF_H)
	@mkdir -p $(dir $@)
	$(AS) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) include/generated ranix.iso ranix-disk.img $(USER_CORE_BINS) $(USER_TEST_BINS) userspace/sh userspace/*_elf.h userspace/libfoo.so

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
	@yes ranixreadaheaddata | head -c 262144 > $(BUILD_DIR)/diskseed/seq.dat
	dd if=/dev/zero of=$(BUILD_DIR)/part.img bs=1M count=32 2>/dev/null
	mkfs.ext4 -F -q -O has_journal,^metadata_csum,64bit -b 1024 \
	    -d $(BUILD_DIR)/diskseed $(BUILD_DIR)/part.img >/dev/null 2>&1
	dd if=/dev/zero of=$@ bs=1M count=40 2>/dev/null
	sgdisk -n 1:2048:+32M -t 1:8300 $@ >/dev/null 2>&1
	dd if=$(BUILD_DIR)/part.img of=$@ bs=512 seek=2048 conv=notrunc 2>/dev/null
	@echo "[OK] gpt-test.img"

# Small raw image on a USB mass-storage device (xHCI), x86_64 only. A marker at
# a known sector is read back over the BOT/SCSI READ(10) path via dd /dev/usb0
# to prove enumeration + block I/O. Deliberately not a filesystem so the kernel
# auto-mounter (which owns /mnt for the disk gpt-ext4 test) ignores it.
$(BUILD_DIR)/usb-test.img:
	dd if=/dev/zero of=$@ bs=1M count=8 2>/dev/null
	printf 'RANIX-USB-DISK-OK' | dd of=$@ bs=512 seek=2 conv=notrunc 2>/dev/null
	@echo "[OK] usb-test.img"

# NVMe namespace image (x86_64 only), a second ext2 filesystem proving the ext2
# driver is multi-instance: the AHCI gpt-ext4 disk stays auto-mounted at /mnt
# while the suite mounts this one at /mnt2 (mount -t ext2 /dev/nvme0n1 /mnt2) and
# read/writes both independently. The ext2 fs occupies the first 4 MB; raw block
# markers for the NVM READ/WRITE round-trip test live past the fs at high sectors.
$(BUILD_DIR)/nvme-test.img:
	@rm -rf $(BUILD_DIR)/nvmeseed
	@mkdir -p $(BUILD_DIR)/nvmeseed
	@printf 'ranix-nvme-ext2-ok' > $(BUILD_DIR)/nvmeseed/marker.txt
	dd if=/dev/zero of=$(BUILD_DIR)/nvme-fs.img bs=1M count=4 2>/dev/null
	mkfs.ext2 -F -q -b 1024 -d $(BUILD_DIR)/nvmeseed $(BUILD_DIR)/nvme-fs.img >/dev/null 2>&1
	dd if=/dev/zero of=$@ bs=1M count=8 2>/dev/null
	dd if=$(BUILD_DIR)/nvme-fs.img of=$@ bs=512 conv=notrunc 2>/dev/null
	printf 'RANIX-NVME-DISK-OK' | dd of=$@ bs=512 seek=12000 conv=notrunc 2>/dev/null
	@echo "[OK] nvme-test.img"

# A full disk rootfs (a copy of the initramfs tree) for the switch_root /
# disk-root path: init pivots into this and runs it as the real /. The marker
# script replaces the suite's test.sh. tests/diskroot.sh boots twice against it.
$(BUILD_DIR)/diskroot.img: $(BUILD_DIR)/initramfs.cpio tests/diskroot_init.sh
	@rm -rf $(BUILD_DIR)/diskroot-seed
	@cp -a $(BUILD_DIR)/initramfs $(BUILD_DIR)/diskroot-seed
	@mkdir -p $(BUILD_DIR)/diskroot-seed/oldroot $(BUILD_DIR)/diskroot-seed/dev \
	          $(BUILD_DIR)/diskroot-seed/proc $(BUILD_DIR)/diskroot-seed/sys \
	          $(BUILD_DIR)/diskroot-seed/var $(BUILD_DIR)/diskroot-seed/root
	@cp tests/diskroot_init.sh $(BUILD_DIR)/diskroot-seed/test.sh
	@printf 'RANIX_DISK_ROOT\n' > $(BUILD_DIR)/diskroot-seed/etc/diskroot-id
	@rm -f $@
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	mkfs.ext2 -F -q -b 1024 -d $(BUILD_DIR)/diskroot-seed $@ >/dev/null 2>&1
	@echo "[OK] diskroot.img"

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
# -m 3G so the 1 GB hugepage pool reserves a block, letting c-hugepage1g
# exercise 1 GB map/write/read/unmap (fork COW needs 2 blocks → -m 6G one-off).
test: iso $(BUILD_DIR)/gpt-test.img $(BUILD_DIR)/usb-test.img $(BUILD_DIR)/nvme-test.img
	@rm -f $(TEST_LOG)
	timeout $(TEST_TIMEOUT) $(QEMU_CMD) $(QEMU_NET) -smp 4 -m 3G -display none \
	    -serial file:$(TEST_LOG) -boot d -cdrom ranix.iso \
	    -drive id=d0,file=$(BUILD_DIR)/gpt-test.img,format=raw,if=none \
	    -device ich9-ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 \
	    -drive id=u0,file=$(BUILD_DIR)/usb-test.img,format=raw,if=none \
	    -device qemu-xhci,id=xhci,streams=off \
	    -device usb-storage,bus=xhci.0,drive=u0 \
	    -drive file=$(BUILD_DIR)/nvme-test.img,if=none,id=nvm0,format=raw \
	    -device nvme,drive=nvm0,serial=ranix0 || true
else
test: initramfs $(TARGET)
	@rm -f $(TEST_LOG)
	timeout $(TEST_TIMEOUT) $(QEMU_CMD) $(QEMU_NET) -smp 4 -m 512M -display none \
	    -serial file:$(TEST_LOG) \
	    -kernel $(QEMU_KERNEL) -initrd $(BUILD_DIR)/initramfs.cpio || true
endif
	@echo "--- test output ---"
	@grep -E "^(PASS|FAIL):" $(TEST_LOG) || true
	@echo "-------------------"
	@grep "Results:" $(TEST_LOG) || echo "Results: (not found)"
	@grep -q "ALL_PASS" $(TEST_LOG) && echo "[OK] All tests passed" || \
	    { echo "[FAIL] Some tests failed"; exit 1; }

# Two-boot persistence proofs (x86_64 only, ~4 boots, kept out of the single-boot
# `test`). persist = a disk ext2 write at /mnt survives a reboot; diskroot = init
# pivots into a full disk rootfs and / itself persists. Both must print PASS.
persisttest:
	@bash tests/persist.sh
	@bash tests/diskroot.sh
