# Ranix Roadmap

> "Ranix akar." — Hobiden çıkıyoruz.

Hedef: statik binary çalıştırabilen, ağa bağlanabilen, çok çekirdekli,
gerçek donanımda çalışan, Linux ile kıyaslanabilir bir kernel.

Kural: her versiyon kendi başına anlamlı bir şey yapabilmeli.
Kural: her versiyon hem x86_64 hem aarch64'te build ve test edilmeli.

---

## Neredeyiz (v0.6 tamamlandı — pthreads + inotify + ptrace + 172-test suite)

**Linux karşılaştırması**: Syscall yüzeyi ~Linux 4.11 (2017) seviyesi (statx/copy_file_range/memfd/inotify/epoll/futex/clone var), ama mimari olarak Linux 1.x (single-core, statik binary, ext2, dinamik linker yok).

**v1.0 hedefi**: glibc çalışan, dpkg kurulabilen, Claude CLI çalıştırabilen bir sistem.

- Boot: x86_64 + aarch64 (Raspi4, QEMU virt)
- Memory: buddy + COW fork + mmap + brk + mprotect (on-demand page alloc); alloc_table OOM → ENOMEM; heap slab
- Process: fork/exec/wait, signals, pipes, scheduler (nice); PID geri dönüşümü; kstack canary; sys_mount + sys_umount2; ptrace (ATTACH/DETACH/CONT/SYSCALL/GETREGS/SETREGS/PEEKDATA/POKEDATA); tgid/tid thread group (musl pthread uyumlu); futex REQUEUE+WAIT_BITSET+PRIVATE_FLAG; exit_group; fd_table refcount (thread-safe shared fd table); tkill/tgkill
- FS: ramfs, ext2 read+write (direct+single+double+triple indirect), procfs, devfs, initramfs (CPIO); VFS permission enforcement + umask + sticky bit; dentry inode lifecycle fix; /dev/stdin/stdout/stderr (fd-proxy); /dev/fd symlink; /dev/urandom + /dev/random
- /proc: /proc/cmdline, /proc/self/maps, /proc/net/{dev,tcp,udp,route}, /proc/sys/{kernel,net/core,fs} tree; statfs/df; /proc/sys/kernel/hostname yazılabilir (sysctl)
- /etc: /etc/passwd, /etc/hosts, /etc/resolv.conf, /etc/environment
- Userspace: busybox 60+ applet, bash-5.2, statik ELF, DOOM; musl pthreads (create/join/mutex) çalışıyor
- Network: ARP + ICMP + UDP + TCP tam stack + TCP flow control; AF_UNIX socketpair + path-based; ping + echo_server
- Syscall: eventfd2, timerfd, memfd_create, signalfd4 (stub), epoll/select/poll/ppoll, statfs, capget/capset, xattr stubs; inotify gerçek (IN_CREATE/DELETE/MOVED_FROM/TO); getrandom; statx, copy_file_range, splice, pread64/pwrite64; auxv: AT_RANDOM/AT_EXECFN/AT_HWCAP/AT_PHDR/AT_ENTRY/AT_PAGESZ
- Kernel cmdline: Multiboot2 (x86_64) + DTB bootargs (aarch64); /proc/cmdline; init= ; quiet/debug
- Donanım: virtio-blk, virtio-net MMIO (aarch64) + PCI (x86_64), sdhost (Pi4), PL011, GIC400
- Test: **172/172 ALL_PASS** her iki arch'ta

---

## v0.5 — TCP Stack  ✅ TAMAMLANDI

- [x] TCP state machine: CLOSED → SYN_SENT/LISTEN → ESTABLISHED → FIN_WAIT/CLOSE_WAIT → TIME_WAIT
- [x] 3-way handshake; veri transferi; bağlantı kapama; temel retransmit
- [x] connect() + listen() + accept() — client + server socket
- [x] recv() / send() (MSG_PEEK, MSG_WAITALL); SO_REUSEADDR; blocking connect timeout
- [x] nc/wget/echo_server testleri geçiyor

### TCP İyileştirmeler (v0.9'a ertelendi)
- [ ] TCP congestion control: Reno (slow start, fast retransmit, fast recovery)
- [ ] receive window scaling (RFC 1323)
- [ ] SRTT/RTO hesaplama (Karn algoritması)
- [ ] out-of-order segment buffer
- [ ] SO_KEEPALIVE + TCP_KEEPIDLE/INTVL/CNT
- [ ] SO_REUSEPORT
- [ ] TCP_NODELAY (Nagle disable)

---

## v0.5.1 — Linux 1.0 Paritesi & Yapısal Dinamizm  ✅ TAMAMLANDI

- [x] VFS permission enforcement — mode bits, EACCES/EPERM, sticky bit
- [x] Kernel cmdline — Multiboot2 + DTB bootargs; /proc/cmdline; init=; quiet/debug
- [x] AF_UNIX SOCK_STREAM + SOCK_DGRAM (path-based bind/connect/accept)
- [x] ext2 read+write (direct+single+double+triple indirect, superblock güncelleme)
- [x] ptrace (ATTACH/DETACH/CONT/SYSCALL/GETREGS/SETREGS/PEEKDATA/POKEDATA + waitpid WIFSTOPPED)

---

## v0.6 — Pthreads, IPC & Modern Event Loop  ✅ TAMAMLANDI

- [x] clone(CLONE_THREAD | CLONE_VM | CLONE_SIGHAND) — adres alanı paylaşımı
- [x] Thread-local storage: x86_64 %fs (ARCH_SET_FS), aarch64 TPIDR_EL0
- [x] set_tid_address; exit_group; CLONE_CHILD_CLEARTID (__tl_sync)
- [x] tkill / tgkill
- [x] futex FUTEX_WAIT / FUTEX_WAKE / FUTEX_REQUEUE / FUTEX_WAIT_BITSET / FUTEX_PRIVATE_FLAG
- [x] fd_table refcount — thread'ler fd tablosunu güvenli paylaşıyor
- [x] AF_UNIX SOCK_STREAM socketpair + path-based; SOCK_DGRAM
- [x] eventfd2, timerfd_create/settime/gettime, memfd_create
- [x] signalfd4 — stub (fd döner, event yok — gerçek impl v0.6.5'te)
- [x] epoll_create1 / epoll_ctl / epoll_pwait — level-trigger
- [x] ppoll / pselect6 — signal mask ile bekleme
- [x] mlock / munlock / mlock2 — stub (no-op)
- [x] inotify — gerçek (IN_CREATE/DELETE/MOVED_FROM/TO, VFS hook)
- [x] getrandom syscall; /dev/urandom + /dev/random
- [x] musl pthreads create/join/mutex 172/172 test geçiyor

---

## v0.6.5 — Eksikler + Güvenlik & Çok Kullanıcı

**Neden burada?**
v0.6 ve altında bilinçli olarak ertelenen ya da keşfedilen eksikler.
Aynı zamanda login, su, setuid — bunlar SMP'den önce olmalı,
yoksa uid/gid race'lerini SMP'de sonradan çözmek zorunda kalırız.

### v0.5.1 / v0.6 Kalan Eksikler

**AF_UNIX abstract namespace**
- [x] `\0` prefix ile başlayan path → filesystem'e dokunmayan socket adresi (length-aware registry, memcmp)
- [x] dbus, systemd, journald bunu kullanıyor

**Epoll tamamlama**
- [x] EPOLLET (edge-trigger) — `last_ready[]` bitmask ile sadece state geçişinde event
- [x] EPOLLONESHOT — ilk event sonrası fd disarm, EPOLL_CTL_MOD ile yeniden arm
- [x] epoll_ctl MOD gözden geçirildi ve düzeltildi

**signalfd4 gerçek implementasyon**
- [x] signalfd okuması sig_pending'ten signal alıp `signalfd_siginfo` struct dolduruyor
- [x] read() blokluyor (sinyal gelene kadar bekliyor)
- [x] epoll ile birlikte çalışıyor (POLLIN döndürüyor)

**FUTEX_WAKE_OP**
- [x] glibc mutex için gerekli — implement edildi
- [x] val3 decode: op/cmp/oparg/cmparg + FUTEX_OP_ARG_SHIFT; uaddr2'de atomic op, koşullu wake

**FUTEX_WAKE race fix**
- [x] `task_wake_no_resched()` — runqueue'a ekler ama need_resched=1 set etmez
- [x] pthread_join + munmap race düzeltildi

**SysV IPC**
- [x] `shmget` / `shmat` / `shmdt` / `shmctl` — shared memory (buddy-backed, vm_map per page)
- [x] `semget` / `semop` / `semctl` — semaphore dizileri, blocking semop
- [x] Python multiprocessing, PostgreSQL, Redis bunları kullanıyor

**POSIX IPC**
- [x] `mq_open` / `mq_send` / `mq_receive` / `mq_close` — message queue (priority-ordered, blocking)
- [x] `/dev/shm` → `/run/shm` symlink; `/run/shm` ramfs mount; `shm_open` için hazır

**vmalloc**
- [x] buddy-backed vmalloc/vfree/vzalloc; PHYS_TO_VIRT; 256-slot size table
- [x] DMA buffer, driver mapping için hazır

**Yapısal config (v0.5.1'den kalan)**
- [x] `ip=<addr>/<prefix>/<gw>` cmdline — CIDR veya dotted mask; tek helper tüm netdev'leri kapsıyor
- [x] `kstack_size=N` cmdline — buddy order runtime hesaplanıyor; idle/irq stack sabit kalıyor
- [x] `sched_quantum=N` cmdline — implement edildi
- [x] `vfs_dentry_max=N` cmdline — implement edildi
- [x] Gerçek DTB binary parse — boot.S ARM64 header ile QEMU DTB'yi x0'da veriyor, fdt.c FDT magic validate ederek binary parse ediyor

### Güvenlik & Çok Kullanıcı

**Kullanıcı/Grup Yönetimi**
- [ ] ruid/euid/suid üçlüsü (process_t'de şu an tek uid var)
- [ ] setuid / setgid / setreuid / setregid / setresuid / setresuid
- [ ] getgroups / setgroups — supplementary groups
- [ ] setuid-bit ELF exec: execve sırasında euid = file owner
- [ ] su çalışması (passwd hash doğrulama)

**Yetenekler (Capabilities)**
- [ ] CAP_NET_ADMIN, CAP_NET_RAW, CAP_SYS_ADMIN
- [ ] capget / capset gerçek implementasyon (şu an stub — root hepsine sahip)
- [ ] capability inheritable/permitted/effective üçlüsü
- [ ] /proc/[pid]/status → CapPrm/CapEff satırları

**Bellek Güvenliği**
- [ ] ASLR: mmap + stack için randomization
- [ ] AT_RANDOM'ı getrandom ile güçlendir (şu an LCG PRNG — yeterli değil)
- [ ] prctl(PR_SET_NAME) — /proc/[pid]/comm
- [ ] prctl(PR_SET_DUMPABLE)

### Test hedefleri
- [ ] `su` ile kullanıcı değiştiriliyor; yanlış şifre reddediliyor
- [ ] normal kullanıcı /dev/sda'yı açamıyor (EACCES)
- [ ] dbus abstract socket üzerinden haberleşiyor
- [ ] `python3 -c "import multiprocessing"` crash etmiyor (SysV IPC)
- [ ] signalfd + epoll birlikte çalışıyor (test_inotify benzeri test)

---

## v0.7 — SMP

**Neden burada?**
Artık doğru locking primitive'lerimiz var.
Thread'ler zaten var, onları farklı CPU'larda çalıştırmak doğal adım.
SMP olmadan gerçek performans yok; "Linux rakibi" tek çekirdekle olmaz.

### CPU başlatma
- [ ] aarch64: MPIDR CPU topolojisi okuma, PSCI CPU_ON ile secondary boot
- [ ] x86_64: ACPI MADT'tan CPU listesi, INIT+SIPI sequence
- [ ] per-CPU boot stack + trampoline kodu
- [ ] per-CPU idle task
- [ ] smp_processor_id() — hangi CPU'dayım

### Scheduler
- [ ] per-CPU runqueue (şu an global tek kuyruk)
- [ ] IPI: sched_ipi → remote CPU'yu uyandır (need_resched)
- [ ] load balancing: basit — en boş CPU'ya push
- [ ] sched_setaffinity / sched_getaffinity gerçek implementasyon
- [ ] SCHED_FIFO / SCHED_RR — real-time scheduling class

### Locking
- [ ] ticket spinlock → daha fair
- [ ] TLB shootdown IPI (fork/mmap sonrası remote flush)
- [ ] per-CPU variables (this_cpu_read/write)
- [ ] atomic_t + atomic64_t gerçek implementasyon (LOCK prefix / LDXR-STXR)

### x86_64 RAM genişletme
- [ ] boot.S direct map: 4 entry → 64 entry (4 GB → 64 GB)
- [ ] PHYS_TO_VIRT sınırını kaldır — 64-bit PML4 tam kullanımı

### Test hedefleri
- [ ] nproc → 4 (QEMU -smp 4)
- [ ] 4 pthread farklı CPU'larda çalışıyor (affinity test)
- [ ] stress -c 4 — sistem kilitlenmiyor

---

## v0.7.5 — Konteyner Altyapısı & Kaynak Yönetimi

**Neden burada?**
Docker, Podman, LXC hepsi namespace + cgroups üstünde çalışıyor.
Namespaces SMP'den sonra geliyor çünkü per-CPU struct'lar değişiyor.

### Namespaces
- [ ] PID namespace: clone(CLONE_NEWPID), /proc içeriği izole
- [ ] mount namespace: clone(CLONE_NEWNS), her container kendi mount tree'si
- [ ] UTS namespace: clone(CLONE_NEWUTS), hostname izole
- [ ] network namespace: clone(CLONE_NEWNET), izole network stack
- [ ] IPC namespace: clone(CLONE_NEWIPC), SysV IPC izolasyonu
- [ ] user namespace: clone(CLONE_NEWUSER), uid/gid mapping
- [ ] unshare syscall; setns syscall
- [ ] /proc/[pid]/ns/ — namespace fd'leri

### cgroups v2
- [ ] cgroupfs mount (/sys/fs/cgroup)
- [ ] memory controller: memory.max, memory.current
- [ ] CPU controller: cpu.weight, cpu.max
- [ ] PID controller: pids.max — fork bomb koruması
- [ ] freezer: cgroup.freeze

### sysfs (/sys)
- [ ] /sys/fs/, /sys/devices/, /sys/class/ temel iskelet
- [ ] glibc ve udev /sys'e bağlı — dinamik linker'dan önce gerekli
- [ ] /sys/kernel/mm/transparent_hugepage stub
- [ ] /sys/bus/pci/devices — PCI device listing

### Seccomp
- [ ] prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT)
- [ ] seccomp(SECCOMP_SET_MODE_FILTER, ...) — BPF filtre

### Test hedefleri
- [ ] unshare --pid --fork --mount-proc /bin/sh çalışıyor
- [ ] memory limit aşılınca OOM killer doğru process'i öldürüyor
- [ ] ls /sys/devices çalışıyor

---

## v0.8 — Dinamik Linker & glibc Hazırlığı

**Neden burada?**
Statik binary sınırı kırmak için dinamik linker şart.
glibc'nin çalışması için önce dinamik linker ve
bir dizi /proc + syscall önkoşulu tamamlanmalı.
Dinamik linker + glibc birlikte gelir — sıra önemli.

### ELF Dinamik Linker

- [ ] PT_INTERP segment işleme — interpreter path okuma (/lib/ld-musl veya /lib/ld-linux)
- [ ] execve: interpreter'ı ELF gibi yükle, AT_BASE'i interpreter load adresine set et
- [ ] execveat — dirfd + relative path ile exec
- [ ] R_X86_64_RELATIVE / R_AARCH64_RELATIVE — base relocation
- [ ] R_X86_64_GLOB_DAT / R_AARCH64_GLOB_DAT — GOT symbol relocation
- [ ] R_X86_64_JUMP_SLOT / R_AARCH64_JUMP_SLOT — PLT lazy binding
- [ ] R_X86_64_IRELATIVE / R_AARCH64_IRELATIVE — IFUNC resolver (glibc string functions)
- [ ] PT_GNU_RELRO: mprotect(PROT_READ) ile RELRO segment'i kilitle
- [ ] TLS static model: x86_64 FS, aarch64 TPIDR_EL0 (zaten var, loader entegrasyonu)
- [ ] dlopen / dlsym / dlclose — runtime shared library yükleme

### mmap & memory tamamlama
- [ ] MAP_FIXED güvenilir (şu an temel var)
- [ ] mremap temel destek
- [ ] MAP_POPULATE — prefault pages
- [ ] msync — MAP_SHARED dirty page flush
- [ ] MAP_GROWSDOWN thread stack — SIGSEGV yerine otomatik büyüme

### auxv tamamlama
- [ ] AT_HWCAP doğru değer: x86_64'te CPUID'den SSE2/AVX bitlerini oku; aarch64'te ID_AA64ISAR0_EL1 oku
- [ ] AT_HWCAP2 — x86_64 AVX512 ve aarch64 ek feature'ları
- [ ] AT_SYSINFO_EHDR — VDSO load adresi (VDSO v0.9'da; şimdilik 0)
- [ ] AT_BASE — interpreter load adresi (dinamik linker yüklenince düzeltilmeli)

### glibc için gerekli /proc eksikleri
- [ ] /proc/self/exe → readlink — glibc runtime path için
- [ ] /proc/self/status: Tgid, Threads, SigBlk, SigIgn satırları ekle
- [ ] /proc/self/cmdline — null-separated argv
- [ ] /proc/sys/vm/overcommit_memory stub (glibc memory check eder)
- [ ] /proc/sys/kernel/osrelease — glibc bu satırı okur

### glibc için gerekli yeni syscall'lar
- [ ] clone3 — glibc 2.36+ thread creation (clone'a graceful fallback var ama olsun)
- [ ] rseq — glibc 2.35+ restartable sequences (register etmezse çalışmaya devam eder)
- [ ] pidfd_open / pidfd_send_signal — glibc 2.36+ thread sync
- [ ] fadvise64 / posix_fadvise — memory hint (stub yeterli)
- [ ] process_vm_readv / process_vm_writev — cross-process memory (stub)
- [ ] prlimit64 — setrlimit64 + getrlimit64 kombinasyonu

### VDSO (temel)
- [ ] clock_gettime(CLOCK_REALTIME/MONOTONIC) VDSO'dan — syscall overhead yok
- [ ] gettimeofday VDSO'dan
- [ ] AT_SYSINFO_EHDR auxv entry

### Test hedefleri
- [ ] musl dinamik linked "hello world" çalışıyor
- [ ] glibc 2.38 dinamik linked "hello world" çalışıyor (x86_64 + aarch64)
- [ ] ldd libc.so.6 → doğru path
- [ ] dynamically linked pthread test geçiyor (glibc ile)

---

## v0.8.5 — glibc Porting & Dağıtım Altyapısı

**Neden ayrı versiyon?**
Dinamik linker çalışır ama glibc'yi yüklemek ayrı bir iş:
ABI farkları, TLS düzeni, IFUNC chain, locale, timezone.
Bu olmadan "Ranix Debian" hedefi boş laftan ibaret.

### glibc 2.38 portu

**Build altyapısı**
- [ ] glibc 2.38 cross-compile: x86_64-ranix-gnu ve aarch64-ranix-gnu target
- [ ] sysdeps/ranix dizini — Linux'tan fork, Ranix uyarlaması
- [ ] `configure --host=x86_64-ranix-gnu --with-headers=ranix/include`
- [ ] libc.so.6, ld-linux-x86-64.so.2, libpthread.so.0, libm.so.6 üretiliyor

**Runtime doğrulama**
- [ ] IFUNC resolver chain — glibc memcpy/strlen/memset CPU-optimized variant seçiyor
- [ ] locale: glibc locale/charset altyapısı çalışıyor (en_US.UTF-8 yeterli)
- [ ] timezone: /etc/localtime okuma (tzdata paketi kurulabilmeli)
- [ ] NSS (Name Service Switch): /etc/passwd, /etc/hosts — getpwnam, gethostbyname
- [ ] math: libm.so.6 temel fonksiyonlar çalışıyor

**Paket sistemi temeli**
- [ ] dpkg statik derleme — Ranix'te dpkg komutu çalışıyor
- [ ] .deb paketi açma + kurma (preinst/postinst script yürütme)
- [ ] /var/lib/dpkg/status güncelleniyor
- [ ] `dpkg -i bash_x.y_amd64.deb` → bash kurulumu
- [ ] apt-get gerektirmez; dpkg yeterli bu aşamada

**Temel paketler (glibc ile derlenmeli)**
- [ ] bash (dinamik linked glibc ile)
- [ ] coreutils (ls/cp/mv/cat/...)
- [ ] libc6 (glibc runtime paketi)

### Test hedefleri
- [ ] `ldd /bin/bash` → libc.so.6 gösteriyor
- [ ] `dpkg -i libc6_2.38_amd64.deb` çalışıyor
- [ ] `python3 -c "import ctypes; ctypes.CDLL('libc.so.6')"` çalışıyor
- [ ] `/etc/timezone → Europe/Istanbul; date` doğru saat dilimiyle çalışıyor

---

## v0.9 — Driver Ekosistemi, TLS & Debug Altyapısı

Gerçek donanımda gerçek işler yapabilmek.
Claude CLI için TLS şart.

### TLS / HTTPS

- [ ] mbedTLS 3.x statik link — kernel değil, userspace
- [ ] libssl.a + libcrypto.a statik binary'lere link ediliyor
- [ ] curl statik derleme: curl --ssl-native (mbedTLS backend)
- [ ] `curl https://api.anthropic.com` çalışıyor
- [ ] SNI (Server Name Indication) — modern HTTPS siteler

### DNS çözücü

- [ ] Gerçek UDP DNS query: /etc/resolv.conf okuma + A/AAAA sorgusu
- [ ] `getaddrinfo` musl'daki implementasyon çalışıyor mu? — test et
- [ ] ndots, search domain, timeout/retry
- [ ] DNS cache (basit TTL cache, kernel tarafı veya userspace)

### Debugging & Tracing
- [ ] x86_64: TF (trap flag) ile single-step; aarch64: MDSCR_EL1.SS
- [ ] strace çalışıyor — ptrace tabanlı, statik derlenmiş
- [ ] gdb temel breakpoint + step (dinamik linker ile birlikte)
- [ ] core dump: SIGSEGV'de core file (/proc/sys/kernel/core_pattern)
- [ ] /proc/[pid]/mem — cross-process memory R/W (gdb için)

### ACPI (x86_64)
- [ ] RSDP/RSDT/XSDT parse — ACPI table discovery
- [ ] MADT okuma — CPU listesi
- [ ] HPET timer — PIT'e alternatif, yüksek çözünürlük
- [ ] ACPI_POWEROFF (S5) — shutdown -h gerçekten kapanmalı

### PCIe İyileştirmeleri
- [ ] MSI/MSI-X interrupt — NVMe için şart
- [ ] PCIe capability walk — device feature discovery

### Terminal (pseudo-terminal)
- [ ] ptmx / pts — pseudo-terminal (SSH için şart)
- [ ] proper TTY line discipline (canonical mod, erase, kill, intr)
- [ ] TIOCSWINSZ window resize
- [ ] openpty / forkpty

### Depolama
- [ ] NVMe (PCIe SSD) — MSI-X gerektirir
- [ ] AHCI (SATA) — eski PC
- [ ] bcmgenet RPi4 ethernet

### Test hedefleri
- [ ] `curl https://api.anthropic.com` → 401 (bağlandı, auth yok — doğru)
- [ ] `strace ls` çalışıyor, syscall listesi görünüyor
- [ ] `gdb /bin/hello` → breakpoint koyduk, step attık
- [ ] shutdown -h → sistem kapandı (ACPI poweroff)
- [ ] NVMe diskten ext2 mount + read/write

---

## v1.0 — "Çalışan Sistem" (Claude CLI dahil)

**Tanım**: glibc çalışıyor, dpkg var, TLS var, SSH var.
Terminalde `claude` komutu çalışıyor.

### Zorunlular
- [ ] glibc 2.38 dinamik linked programlar çalışıyor ✓ (v0.8.5'ten)
- [ ] SSH server: dropbear statik + glibc dinamik (birini seç)
- [ ] SSH client: `ssh user@host` çalışıyor
- [ ] HTTPS: `curl https://...` çalışıyor ✓ (v0.9'dan)
- [ ] Python 3.11+ statik veya dinamik linked çalışıyor
- [ ] Node.js 20 LTS çalışıyor (Claude CLI için)
- [ ] Claude CLI: `npm install -g @anthropic-ai/claude-code` veya statik binary
- [ ] `claude` komutu terminalde çalışıyor — API'ye bağlanıyor

### Claude CLI önkoşulları (özetle)
- [ ] Node.js 20 dinamik linked → glibc + libstdc++ + libz gerekiyor
- [ ] DNS çalışıyor (api.anthropic.com çözülüyor) ✓ (v0.9)
- [ ] TLS çalışıyor ✓ (v0.9)
- [ ] /tmp yazılabilir ✓; $HOME var; environment variables ✓

### Sistem olgunluğu
- [ ] init system: PID 1 service manager (sinit veya s6 — systemd değil)
- [ ] /etc/fstab okuma, boot'ta otomatik mount
- [ ] timezone: TZ=Europe/Istanbul date → doğru
- [ ] HTTP server: nginx veya lighttpd çalışıyor
- [ ] top / htop çalışıyor — CPU + memory canlı görünüm
- [ ] Raspberry Pi 4'te SD karttan boot, ethernet çalışıyor
- [ ] x86_64 PC'de USB boot, NVMe/SATA disk, ethernet

### Test hedefleri
- [ ] `claude "merhaba"` → yanıt alındı
- [ ] `python3 -c "import requests; print(requests.get('http://example.com').status_code)"`
- [ ] `ssh localhost` → bağlantı kuruldu
- [ ] `dpkg -l | wc -l` → en az 10 paket kurulu
- [ ] Pi4'te soğuk boot → bash prompt → 60 saniye içinde

---

## Uzun Vade (v1.x — v2.0)

### Ranix Dağıtımı (v1.5 hedefi)
- [ ] Ranix musl edition: statik derlenmiş paket deposu — Alpine mantığı, Ranix kernel
- [ ] Ranix glibc edition: glibc + dpkg + temel Debian paketleri ✓ (v0.8.5'ten)
- [ ] apt-get temel implementasyon (deb indirme + dpkg -i)
- [ ] Ranix ISO: GRUB + installer script
- [ ] /etc/apt/sources.list → Ranix paket deposu

### io_uring (Linux 5.1+)
- [ ] io_uring_setup / io_uring_enter — async I/O ring buffer
- [ ] IORING_OP_READ, WRITE, ACCEPT, SEND
- [ ] Node.js / Rust tokio performans için kullanıyor

### Gelişmiş Bellek Yönetimi
- [ ] huge pages: mmap(MAP_HUGETLB)
- [ ] swap: /swapfile + swapon
- [ ] OOM killer: oom_score hesaplama
- [ ] KSM (kernel samepage merging)

### Gelişmiş Scheduler
- [ ] CFS (vruntime tabanlı fair scheduling)
- [ ] CPU frequency scaling: aarch64 CPUFREQ, x86_64 P-state

### Ağ (ileri seviye)
- [ ] Netfilter benzeri hook'lar — iptables eşdeğeri
- [ ] IPv6 tam destek (SLAAC, DHCPv6)
- [ ] WireGuard-like VPN (in-kernel)

### Sanallaştırma & Donanım
- [ ] KVM host: /dev/kvm — Ranix üstünde VM çalıştırma
- [ ] virtio-gpu + DRM/KMS — GPU accelerated framebuffer
- [ ] Bluetooth HCI over USB

### Güvenlik (ileri)
- [ ] KASLR — kernel address space randomization
- [ ] kernel stack canaries
- [ ] CFI (control flow integrity)
- [ ] landlock LSM — sandboxing

---

## Teknik Notlar

- `exceptions_asm.S` → `process_t.ctx` offset 16 hardcode. struct başı değişmesin.
- Her değişiklikten sonra hem x86_64 hem aarch64 build edilmeli.
- aarch64 virt max RAM: 512 GB (T1SZ=25). x86_64 max RAM: 4 GB (boot.S direct map 4 entry — v0.7'de genişlet).
- SMP için spinlock'lar IRQ-disable ile çalışıyor; v0.7'de atomik olmalı (LDAXR/STLXR, LOCK CMPXCHG).
- TCP retransmit: sabit timeout yeterli. SRTT/Karn v0.9'a bırak.
- Doom: `make ARCH=x86_64 qemu-doom`
- Namespace + cgroups birlikte implement edilmeli; yarım namespace tehlikeli.
- io_uring'i SMP'den önce yazmak hata — ring buffer per-CPU cache'e bağlı.
- glibc için IFUNC (R_*_IRELATIVE) dinamik linker'da desteklenmeli — musl kullanmaz, glibc her string fonksiyonunda kullanır.
- Node.js v20 glibc 2.17+ gerektiriyor (x86_64), glibc 2.17 aarch64 için 2.31+. glibc 2.38 yeterli.
- clone3 + rseq + pidfd_* glibc fallback'i var, geçici stub yeterli.
