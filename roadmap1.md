# Ranix Roadmap

> "Ranix akar." — Hobiden çıkıyoruz.

Hedef: statik binary çalıştırabilen, ağa bağlanabilen, çok çekirdekli,
gerçek donanımda çalışan, Linux ile kıyaslanabilir bir kernel.

Kural: her versiyon kendi başına anlamlı bir şey yapabilmeli.
Kural: her versiyon hem x86_64 hem aarch64'te build ve test edilmeli.

---

## Neredeyiz (v0.5 + stabilite katmanı tamamlandı)

- Boot: x86_64 + aarch64 (Raspi4, QEMU virt)
- Memory: buddy + COW fork + mmap + brk; `alloc_table` OOM → ENOMEM (kpanic yok), `vm_copy` goto-oom rollback
- Process: fork/exec/wait, signals, pipes, scheduler (nice); PID geri dönüşümü (`alloc_pid`); kstack canary (`0xDEADBEEFCAFEBABE`)
- FS: ramfs, ext2 read-only, procfs, devfs, initramfs (CPIO); dentry inode lifecycle fix, DENTRY_MAX=4096, volatile inode serbest bırakma
- Userspace: busybox 60+ applet, bash, statik ELF, DOOM
- Network: ARP + ICMP + UDP + TCP tam stack, ping + echo_server (x86_64 + aarch64)
- Donanım: virtio-blk, virtio-net MMIO (aarch64) + PCI (x86_64), sdhost (Pi4), PL011, GIC400
- Test: 35/35 bash test suite her iki mimaride geçiyor

---

## v0.5 — TCP Stack  ✅ TAMAMLANDI

**Neden önce TCP?**
TCP olmadan internet yok. HTTP, SSH, wget, git, her şey TCP üzerinde.
UDP ve ICMP'yi hallettik; TCP state machine bunların üstüne oturuyor.
Bu olmadan SMP'yi yazsan bile ağa bağlanamazsın.

### TCP state machine
- [x] `tcp_sock` state: CLOSED → SYN_SENT/LISTEN → ESTABLISHED → FIN_WAIT/CLOSE_WAIT → TIME_WAIT
- [x] 3-way handshake (SYN, SYN-ACK, ACK)
- [x] veri transferi: segment reassembly, seq/ack takibi
- [x] bağlantı kapama: FIN/FIN-ACK/ACK (aktif + pasif)
- [x] temel retransmit (sabit timeout, retx_count give-up)
- [x] `connect()` + `listen()` + `accept()` — client + server socket
- [x] clean FIN teardown — blocking close(), tcp_wake_close()

### Syscall tamamlama
- [x] `recv()` / `send()` (MSG_PEEK, MSG_WAITALL temel flaglar)
- [x] `SO_REUSEADDR` setsockopt
- [x] blocking connect timeout

### Test hedefleri
- [x] `nc 10.0.2.2 9999` host listener'a bağlanıyor, veri alıyor
- [x] `wget http://10.0.2.2:9000/` — gerçek HTTP GET, HTML response
- [x] RST → ECONNREFUSED doğru
- [x] echo_server — 3/3 TCP bağlantı testi geçti

### TCP İyileştirmeler (v0.9'a ertelendi)
- [ ] TCP congestion control: Reno (slow start, fast retransmit, fast recovery)
- [ ] receive window scaling (RFC 1323) — büyük transfer için şart
- [ ] SRTT/RTO hesaplama (Karn algoritması) — sabit timeout yerine
- [ ] out-of-order segment buffer (şu an sırasız paket drop ediliyor)
- [ ] `SO_KEEPALIVE` + `TCP_KEEPIDLE/INTVL/CNT`
- [ ] `SO_REUSEPORT` — aynı porta birden fazla listener (nginx multi-worker)
- [ ] `TCP_NODELAY` (Nagle algoritması disable)

---

## v0.5.1 — Linux 1.0 Paritesi & Yapısal Dinamizm  ← ŞİMDİ

**Neden önce bunlar?**
Özellik eklemeden önce var olan şeylerin doğru çalışması lazım.
Linux 1.0'da olan temel güvenlik ve yapı bileşenleri eksik.
"Hardcoded" mimari uzun vadede SMP, konteyner ve multi-user'ı imkansız kılar.

### Linux 1.0 Paritesi

**Permission enforcement** (Linux 0.12, 1992) ✅ TAMAMLANDI
- [x] `vfs_check_perm(inode, flags, uid, gid)` — mode bits gerçekten kontrol edilmeli
- [x] `vfs_open`, `vfs_mkdir`, `vfs_unlink`, `vfs_rename` → permission check
- [x] `EACCES` / `EPERM` dönüşü yetersiz izinde
- [x] `/tmp` sticky bit (`S_ISVTX`) — sadece sahip silebilir

**Kernel cmdline & init discovery** (Linux 0.99, 1993) ✅ TAMAMLANDI
- [x] Multiboot2 (x86_64) ve DTB `bootargs` (aarch64) cmdline parse
- [x] `/proc/cmdline` — runtime okunabilir
- [x] `init=` parametresi — hangi binary'nin PID 1 olacağını cmdline'dan al
- [x] Init arama sırası: `init=` → `/sbin/init` → `/etc/init` → `/bin/init` → `/bin/sh`
- [x] `quiet` / `debug` log seviyesi parametresi

**AF_UNIX domain sockets** (Linux 1.0, 1994)
- [ ] `AF_UNIX SOCK_STREAM` — pipe gibi ama adreslenebilir
- [ ] `bind` + `connect` + `accept` path-based
- [ ] `send` / `recv` — tam duplex
- [ ] `SOCK_DGRAM` AF_UNIX — syslog, journald
- [ ] abstract namespace (`\0` ile başlayan path)

**ext2 yazma** (Linux 1.0, 1994)
- [ ] inode tahsis — bitmap üzerinden boş inode bul
- [ ] blok tahsis — bitmap üzerinden boş blok bul
- [ ] `create` — dizine yeni dosya girişi ekle, inode yaz
- [ ] `write` — dosya verisini blok(lar)a yaz, inode size güncelle
- [ ] `mkdir` — dizin girişi + `.` ve `..` blokları
- [ ] `unlink` — inode serbest bırak, dizin girişini sil
- [ ] `rename` — dizin girişi taşı
- [ ] superblock + grup tanımlayıcı güncelle (free block/inode sayıları)

**ptrace** (Linux 0.99, 1993)
- [ ] `PTRACE_ATTACH` / `PTRACE_DETACH`
- [ ] `PTRACE_CONT` — traced process'i devam ettir
- [ ] `PTRACE_SYSCALL` — syscall entry/exit'te dur
- [ ] `PTRACE_GETREGS` / `PTRACE_SETREGS` — register okuma/yazma
- [ ] x86_64: TF (trap flag) ile single-step; aarch64: MDSCR_EL1.SS
- [ ] `wait()` → `WIFSTOPPED` / `WSTOPSIG` semantiği
- [ ] `strace` çalışıyor

### Yapısal Dinamizm (Hardcoded → Configurable)

**Şu an kodda sabit olan şeyler ve çözümleri:**

- [ ] **IP adresi** — `virtio_net.c`'de `10.0.2.15` hardcode. `ip=<addr>/<gw>` cmdline parametresi; yoksa `10.0.2.15` default kalır
- [ ] **Hostname** — hiç yok. `hostname=ranix` cmdline parametresi + `/proc/sys/kernel/hostname`
- [ ] **DTB** — `fdt.c` var ama statik C verisi. Gerçek DTB binary parse edilmeli; QEMU `-dtb` ile verilmeli
- [ ] **KSTACK_SIZE** — 8192 sabit. `kstack_size=N` cmdline veya `include/kernel/config.h`
- [ ] **SCHED_QUANTUM** — 10 tick sabit. `sched_quantum=N` cmdline
- [ ] **DENTRY_MAX** — 4096 sabit. `vfs_dentry_max=N` cmdline
- [x] **init search paths** — sadece `/bin/init` ve `/bin/sh` deneniyor. Genişlet + `init=` parametresi

**Kernel config altyapısı:**
- [ ] `include/kernel/config.h` — derleme zamanı sabitlerinin tek yeri
- [x] `cmdline_get(key, buf, len)` — runtime parametreleri oku
- [ ] `/proc/sys/` sysctl stüb — temel parametreleri runtime değiştir (net.ipv4.ip_forward vs.)

### Test hedefleri
- [ ] Normal kullanıcı `/etc/shadow`'u açamıyor (EACCES)
- [ ] `strace ls` çalışıyor
- [ ] `nc -U /tmp/test.sock` — AF_UNIX bağlantısı
- [ ] `echo hello > /mnt/test.txt && cat /mnt/test.txt` çalışıyor (ext2 yazma)
- [ ] `qemu ... -append "init=/bin/sh quiet"` → init path cmdline'dan alınıyor

---

## v0.6 — Pthreads, IPC & Modern Event Loop

**Neden SMP'den önce?**
SMP'ye geçtiğinde atomik primitive'lerin zaten doğru olması lazım.
Futex'i single-CPU'da yazıp test etmek çok daha kolay.
AF_UNIX ve eventfd olmadan gerçek uygulama çalıştırmak mümkün değil —
dbus, X11, postgres, redis, nginx hepsi bunların üstünde.

### Thread'ler
- [ ] `clone(CLONE_THREAD | CLONE_VM | CLONE_SIGHAND)` — adres alanı paylaşımı
- [ ] thread-local storage: x86_64 `%fs` (ARCH_SET_FS), aarch64 `TPIDR_EL0`
- [ ] `set_tid_address` — musl thread init için gerekli
- [ ] thread exit: grup üyesi, `SIGCHLD` sadece process group ölünce
- [ ] `tkill` / `tgkill`
- [ ] `pthread_atfork` desteği (`CLONE_VM` + fork interaction)

### Futex
- [ ] `futex(FUTEX_WAIT, FUTEX_WAKE)` — temel
- [ ] `futex(FUTEX_REQUEUE)` — pthread_cond_broadcast için
- [ ] `futex(FUTEX_WAKE_OP)` — glibc mutex optimize path
- [ ] futex hash table (kernel wait queue, userspace addr'e göre)
- [ ] `FUTEX_PRIVATE_FLAG` optimizasyonu

### AF_UNIX Domain Sockets
- [ ] `AF_UNIX SOCK_STREAM` — dbus, X11, postgres, redis socket
- [ ] `AF_UNIX SOCK_DGRAM` — syslog, journald
- [ ] abstract namespace (`\0` ile başlayan path)
- [ ] `SCM_RIGHTS` — fd transfer (bir process başkasına fd yollar, dbus için şart)
- [ ] `SCM_CREDENTIALS` — peer uid/gid doğrulama

### Modern Event Loop Syscall'ları
- [ ] `eventfd` / `eventfd2` — libuv, tokio, Go runtime kullanıyor
- [ ] `timerfd_create` / `timerfd_settime` / `timerfd_gettime`
- [ ] `signalfd` — signal'ı fd üzerinden okuma (systemd kullanıyor)
- [ ] `epoll` edge-trigger (EPOLLET) + `EPOLLONESHOT` — libuv/nginx
- [ ] `epoll_pwait` — atomik signal mask + wait

### Bellek
- [ ] vmalloc — kernel sanal adres alanı allocator (şu an yok)
- [ ] thread stack: `mmap(MAP_ANONYMOUS | MAP_GROWSDOWN)`
- [ ] `mlock` / `munlock` — bellek kilitleme (database'ler kullanıyor)
- [ ] `memfd_create` — isimsiz dosya, dinamik loader + IPC için lazım

### Test hedefleri
- [ ] musl pthreads ile derlenen basit C programı çalışıyor
- [ ] `pthread_create / pthread_join / pthread_mutex` çalışıyor
- [ ] `pthread_cond_wait / signal` çalışıyor
- [ ] AF_UNIX üzerinden iki process haberleşiyor
- [ ] Node.js veya Python asyncio temel event loop çalışıyor

---

## v0.6.5 — Güvenlik & Çok Kullanıcı

**Neden burada?**
Tek kullanıcılı, root her şeyi yapabilen sistem "çalışan sistem" sayılmaz.
Login, su, sudo, SSH — hepsi uid/gid üstünde duruyor.
ASLR olmadan modern binary'lerin birçoğu güvensiz çalışır.
Capabilities olmadan setuid binary'ler (ping, mount) çalışmaz.

### Kullanıcı/Grup Yönetimi
- [ ] `uid_t` / `gid_t` process_t'ye eklenmeli (ruid, euid, suid × 3)
- [ ] `getuid` / `getgid` / `geteuid` / `getegid`
- [ ] `setuid` / `setgid` / `setreuid` / `setregid` / `setresuid`
- [ ] `getgroups` / `setgroups` — supplementary groups
- [ ] `/etc/passwd` + `/etc/shadow` okuma — libc getpwnam/getspnam
- [ ] setuid-bit ELF exec: execve sırasında euid = file owner
- [ ] `su` çalışması (PAM yok, passwd hash doğrulama yeterli)

### Yetenekler (Capabilities)
- [ ] `CAP_NET_ADMIN`, `CAP_NET_RAW` — ping raw socket root olmadan
- [ ] `CAP_SYS_ADMIN` — mount, namespace
- [ ] `capget` / `capset` syscall
- [ ] capability inheritable/permitted/effective üçlüsü
- [ ] `/proc/[pid]/status` → CapPrm/CapEff satırları

### Bellek Güvenliği
- [ ] ASLR: `mmap` ve stack için randomization (KASLR değil, userspace ASLR)
- [ ] stack base randomization: execve'de random offset
- [ ] `prctl(PR_SET_DUMPABLE)` — core dump kontrolü
- [ ] `prctl(PR_SET_NAME)` — process isim ayarlama (`/proc/[pid]/comm`)
- [ ] `prctl(PR_GET_SECCOMP / PR_SET_SECCOMP)` temel

### Filesystem Güvenliği
- [ ] dosya permission bits (mode_t) VFS'de gerçekten check edilmeli
- [ ] `umask` per-process, `umask()` syscall
- [ ] sticky bit (`+t`) — `/tmp` koruması
- [ ] `chown` / `chmod` — sadece root veya owner

### Test hedefleri
- [ ] `login` çalışıyor, yanlış şifre reddediliyor
- [ ] `su` ile kullanıcı değiştiriliyor
- [ ] normal kullanıcı `/dev/sda`'yı açamıyor (EACCES)
- [ ] `ping` setuid olmadan CAP_NET_RAW ile çalışıyor

---

## v0.7 — SMP

**Neden burada?**
Artık doğru locking primitive'lerimiz var (futex → spinlock → atomic).
Thread'ler zaten var, onları farklı CPU'larda çalıştırmak doğal adım.
SMP olmadan gerçek performans yok; "Linux rakibi" tek çekirdekle olmaz.

### CPU başlatma
- [ ] aarch64: MPIDR CPU topolojisi okuma, PSCI `CPU_ON` ile secondary boot
- [ ] x86_64: ACPI MADT'tan CPU listesi, INIT+SIPI sequence
- [ ] per-CPU boot stack + trampoline kodu
- [ ] per-CPU idle task
- [ ] `smp_processor_id()` — hangi CPU'dayım

### Scheduler
- [ ] per-CPU runqueue (şu an global tek kuyruk)
- [ ] IPI: `sched_ipi` → remote CPU'yu uyandır (`need_resched`)
- [ ] load balancing: basit — en boş CPU'ya push (steal yok henüz)
- [ ] `sched_setaffinity` / `sched_getaffinity` gerçek implementasyon
- [ ] `SCHED_FIFO` / `SCHED_RR` — real-time scheduling class

### Locking
- [ ] ticket spinlock → test-and-set yerine daha fair
- [ ] TLB shootdown IPI (fork/mmap sonrası remote flush)
- [ ] per-CPU variables (`this_cpu_read/write`)
- [ ] `atomic_t` + `atomic64_t` gerçek implementasyon (LOCK prefix / LDXR-STXR)

### x86_64 RAM genişletme
- [ ] boot.S direct map: 4 entry → 64 entry (4 GB → 64 GB)
- [ ] PHYS_TO_VIRT sınırını kaldır — 64-bit PML4 tam kullanımı

### Test hedefleri
- [ ] `nproc` → 4 (QEMU `-smp 4`)
- [ ] `pthread_create` ile oluşturulan 4 thread 4 ayrı CPU'da çalışıyor
- [ ] race condition testi geçiyor (basic stress test)
- [ ] `stress -c 4` — sistemli kilitlenme yok

---

## v0.7.5 — Konteyner Altyapısı & Kaynak Yönetimi

**Neden burada?**
Docker, Podman, LXC hepsi namespace + cgroups üstünde çalışıyor.
Bu olmadan "çalışan sistem" modern uygulamaları izole edemez.
Namespaces SMP'den sonra geliyor çünkü per-CPU data struct'ları değişiyor.

### Namespaces
- [ ] PID namespace: `clone(CLONE_NEWPID)`, /proc içeriği izole
- [ ] mount namespace: `clone(CLONE_NEWNS)`, her container kendi mount tree'si
- [ ] UTS namespace: `clone(CLONE_NEWUTS)`, hostname izole
- [ ] network namespace: `clone(CLONE_NEWNET)`, izole network stack
- [ ] IPC namespace: `clone(CLONE_NEWIPC)`, SysV IPC izolasyonu
- [ ] user namespace: `clone(CLONE_NEWUSER)`, uid/gid mapping (en zor olan)
- [ ] `unshare` syscall — mevcut namespace'ten ayrılma
- [ ] `setns` syscall — başka namespace'e join
- [ ] `/proc/[pid]/ns/` — namespace fd'leri

### cgroups v2
- [ ] cgroupfs mount (`/sys/fs/cgroup`)
- [ ] memory controller: `memory.max`, `memory.current`
- [ ] CPU controller: `cpu.weight`, `cpu.max`
- [ ] PID controller: `pids.max` — fork bomb koruması
- [ ] freezer: `cgroup.freeze` — tüm process'leri dondur/devam

### Seccomp
- [ ] `prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT)` — temel
- [ ] `seccomp(SECCOMP_SET_MODE_FILTER, ...)` — BPF filtre (Docker bunu kullanıyor)

### Overlay Filesystem
- [ ] overlayfs: upper (yazılabilir) + lower (read-only) katmanlar
- [ ] Docker image layer'ları bunun üstünde çalışıyor

### Test hedefleri
- [ ] `unshare --pid --fork --mount-proc /bin/sh` çalışıyor
- [ ] container içindeki process dışarıyı göremiyor
- [ ] memory limit aşılınca OOM killer doğru process'i öldürüyor

---

## v0.8 — Dinamik Linker & Binary Uyumluluk

**Neden burada?**
TCP + pthreads + SMP olunca statik binary sınırı ciddi darboğaz.
Go, Rust, Python gibi araçları çalıştırabilmek istiyoruz.
musl libc dinamik versiyonu ile çalışmak hedef.

### Loader
- [ ] `PT_INTERP` ELF segment işleme — interpreter path okuma
- [ ] `ld-musl-aarch64.so.1` / `ld-musl-x86_64.so.1` yükleme
- [ ] `execve` sırasında interpreter'ı ELF gibi yükle
- [ ] `execveat` — dirfd + relative path ile exec
- [ ] `AT_HWCAP` auxv — CPU feature flags (NEON, AVX vs.)

### mmap & relocation altyapısı
- [ ] `mmap(MAP_FIXED)` güvenilir (şu an temel var)
- [ ] `mprotect` `PROT_EXEC` tam çalışmalı
- [ ] `mremap` temel destek
- [ ] lazy allocation: MAP_ANONYMOUS page fault on access
- [ ] `MAP_POPULATE` — prefault pages
- [ ] `msync` — MAP_SHARED dirty page flush

### VDSO
- [ ] `clock_gettime(CLOCK_REALTIME/MONOTONIC)` VDSO'dan
- [ ] `gettimeofday` VDSO'dan
- [ ] `AT_SYSINFO_EHDR` auxv entry
- [ ] `clock_nanosleep` — precise sleep

### Filesystem: tmpfs & ext4
- [ ] **tmpfs** — `/tmp` ve `/run` için RAM backed FS (birçok program şart koşuyor)
- [ ] **ext2 write** tamamlanmalı (şu an read-only)
- [ ] **ext4** journal (ext2 üstüne writeback journal) — gerçek disk güvenliği
- [ ] `fallocate` — disk alanı önceden rezerve (database'ler kullanıyor)
- [ ] `flock` + `fcntl(F_SETLK)` — dosya kilitleme (SQLite, Postgres)
- [ ] `inotify_init1` / `inotify_add_watch` — FS event notification

### Eksik Syscall'lar (gerçek binary'leri kıran)
- [ ] `statx` — extended stat (glibc ve modern araçlar kullanıyor)
- [ ] `openat2` — openat + resolve flags
- [ ] `close_range` — fd aralığını kapat
- [ ] `splice` / `tee` — zero-copy (nginx, cat)
- [ ] `copy_file_range` — kernel içi dosya kopyalama

### IPC (eksikler)
- [ ] SysV shared memory: `shmget` / `shmat` / `shmdt`
- [ ] SysV semaphores: `semget` / `semop`
- [ ] POSIX shared memory: `shm_open` (tmpfs üstünde `/dev/shm`)
- [ ] POSIX message queues: `mq_open` / `mq_send` / `mq_receive`

### Test hedefleri
- [ ] musl dinamik linked "hello world" çalışıyor
- [ ] `ldd` output gösterebiliyoruz
- [ ] basit Go / Rust binary çalışıyor
- [ ] `python3 -c "import os; os.listdir('/')"` çalışıyor
- [ ] `sqlite3` çalışıyor (flock + mmap kullanan gerçek test)

---

## v0.9 — Driver Ekosistemi & Debug Altyapısı

Gerçek donanımda gerçek işler yapabilmek.
Debugging olmadan gerçek uygulama geliştirmek kör uçuş.

### Debugging & Tracing
- [ ] **ptrace** — `PTRACE_TRACEME`, `PTRACE_CONT`, `PTRACE_SYSCALL`, `PTRACE_GETREGS`
- [ ] `strace` ile syscall trace çalışıyor
- [ ] `gdb` ile temel breakpoint + step çalışıyor
- [ ] core dump: `ulimit -c unlimited` → SIGSEGV'de core file
- [ ] `/proc/sys/kernel/core_pattern` — dump path
- [ ] `perf_event_open` temel (CPU cycle count, cache miss)

### ACPI (x86_64)
- [ ] RSDP/RSDT/XSDT parse — ACPI table discovery
- [ ] MADT okuma — CPU listesi (şu an QEMU varsayımı)
- [ ] HPET timer — PIT'e alternatif, yüksek çözünürlük
- [ ] `ACPI_POWEROFF` (S5 sleep state) — `shutdown -h` gerçekten kapanmalı

### PCIe İyileştirmeleri
- [ ] MSI/MSI-X interrupt — NVMe çalışması için şart (INTx alternatifi yok)
- [ ] PCIe capability walk — device feature discovery
- [ ] IOMMU temel (VT-d / SMMU) — DMA güvenliği

### USB (temel)
- [ ] xHCI host controller (PCIe'den detect)
- [ ] USB HID: klavye (zaten PS/2 var, USB da lazım)
- [ ] USB mass storage (BBB protokol) — USB bellek okuma

### Depolama
- [ ] **NVMe** (PCIe SSD) — modern laptop/PC için şart, MSI-X gerektiriyor
- [ ] **AHCI** (SATA) — eski PC için
- [ ] bcmgenet RPi4 ethernet — gerçek donanımda ağ

### Ağ (devam)
- [ ] DHCP client (şu an IP hardcode) — `udhcpc` veya kernel DHCP
- [ ] DNS resolver: `resolv.conf` okuma, UDP query, `/etc/hosts`
- [ ] IPv6 temel (neighbor discovery, SLAAC)
- [ ] `sendmsg` / `recvmsg` — scatter-gather I/O, AF_UNIX fd transfer
- [ ] TCP congestion control (Reno) + window scaling

### Terminal
- [ ] `ptmx` / `pts` — pseudo-terminal (SSH için şart)
- [ ] proper TTY line discipline (canonik mod, erase, kill, intr)
- [ ] `TIOCSWINSZ` window resize
- [ ] `openpty` / `forkpty` libc desteği

---

## v1.0 — "Çalışan Sistem"

- [ ] SSH server çalışıyor (dropbear statik)
- [ ] SSH client çalışıyor — `ssh user@host`
- [ ] HTTP server çalışıyor (nginx veya lighttpd statik)
- [ ] Python 3 interpreter çalışıyor
- [ ] `curl https://...` çalışıyor (TLS: mbedTLS statik link)
- [ ] `top` / `htop` çalışıyor — CPU + memory canlı görünüm
- [ ] Raspberry Pi 4'te SD karttan boot, ethernet çalışıyor
- [ ] x86_64 PC'de USB boot, NVMe/SATA disk, ethernet
- [ ] init system: PID 1 service manager (systemd değil, basit sinit veya s6)
- [ ] `/etc/fstab` okuma, boot'ta otomatik mount
- [ ] timezone: `TZ=Europe/Istanbul date` doğru çıktı

---

## Uzun Vade (v1.x — v2.0)

Bunlar olmadan da v1.0 "çalışan sistem" sayılır ama "Linux rakibi" için bunlar gerekli.

### io_uring (Linux 5.1+)
- [ ] `io_uring_setup` / `io_uring_enter` — async I/O ring buffer
- [ ] `IORING_OP_READ`, `IORING_OP_WRITE`, `IORING_OP_ACCEPT`, `IORING_OP_SEND`
- [ ] Neden: Node.js, Rust tokio, modern database'ler performans için kullanıyor

### Gelişmiş Bellek Yönetimi
- [ ] huge pages: `mmap(MAP_HUGETLB)`, `/proc/meminfo` HugePages
- [ ] swap: `/swapfile` + `swapon` — fiziksel RAM'i aşan programlar
- [ ] OOM killer: `oom_score` hesaplama, en iyi kurban seçimi
- [ ] KSM (kernel samepage merging) — VM workload için

### Gelişmiş Scheduler
- [ ] CFS (vruntime tabanlı fair scheduling)
- [ ] CPU frequency scaling: aarch64 CPUFREQ, x86_64 P-state

### Ağ (ileri seviye)
- [ ] Netfilter benzeri hook'lar — `iptables` eşdeğeri kural zinciri
- [ ] WireGuard-like VPN (in-kernel)
- [ ] `AF_XDP` — kernel bypass networking

### Sanallaştırma & Donanım
- [ ] KVM host: `/dev/kvm` — Ranix üstünde VM çalıştırma
- [ ] virtio-gpu + DRM/KMS — GPU accelerated framebuffer
- [ ] virtio-snd — ses desteği
- [ ] Bluetooth HCI over USB

### Güvenlik (ileri)
- [ ] KASLR — kernel address space randomization
- [ ] kernel stack canaries
- [ ] CFI (control flow integrity)
- [ ] `landlock` LSM — sandboxing

---

## Teknik Notlar

- `exceptions_asm.S` → `process_t.ctx` offset 16 hardcode. struct başı değişmesin.
- Her değişiklikten sonra hem x86_64 hem aarch64 build edilmeli.
- aarch64 virt max RAM: 512 GB (T1SZ=25). x86_64 max RAM: 4 GB (boot.S direct map 4 entry — v0.7 öncesi genişlet).
- SMP için spinlock'lar şu an IRQ-disable ile çalışıyor; v0.7'de atomik olmalı (LDAXR/STLXR, LOCK CMPXCHG).
- TCP retransmit: sabit timeout şu an yeterli. SRTT/Karn v0.9'a bırak.
- Doom: `make ARCH=x86_64 qemu-doom` → serial.log
- Namespace + cgroups birlikte implement edilmeli; yarım namespace tehlikeli.
- io_uring'i SMP'den önce yazmak hata — ring buffer per-CPU cache'e bağlı.
- ptrace olmadan strace çalışmaz; v0.9'dan önce yapılırsa debug hayat kolaylaşır.
- MSI/MSI-X olmadan NVMe sürücüsü yazılamaz — PCIe iyileştirmeleri önce gelir.
