# Ranix Roadmap

> "Ranix akar." — Hobiden çıkıyoruz.

Hedef: statik binary çalıştırabilen, ağa bağlanabilen, çok çekirdekli,
gerçek donanımda çalışan, Linux ile kıyaslanabilir bir kernel.

Kural: her versiyon kendi başına anlamlı bir şey yapabilmeli.
Kural: her versiyon hem x86_64 hem aarch64'te build edilmeli.

---

## Neredeyiz (v0.4 tamamlandı)

- Boot: x86_64 + aarch64 (Raspi4, QEMU virt)
- Memory: buddy + COW fork + mmap + brk
- Process: fork/exec/wait, signals, pipes, scheduler (nice)
- FS: ramfs, ext2 r/w, procfs, devfs, initramfs (CPIO)
- Userspace: busybox 60+ applet, statik ELF, DOOM
- Network: ARP + ICMP + UDP + SOCK_RAW, ping çalışıyor
- Donanım: virtio-blk, virtio-net, sdhost (Pi4), PL011, GIC400

---

## v0.5 — TCP Stack  ✅ TAMAMLANDI

**Neden önce TCP?**
TCP olmadan internet yok. HTTP, SSH, wget, git, her şey TCP üzerinde.
UDP ve ICMP'yi hallettik; TCP state machine bunların üstüne oturuyor.
Bu olmadan SMP'yi yazsan bile ağa bağlanamazsın.

### TCP state machine
- [ ] `tcp_sock` state: CLOSED → SYN_SENT/LISTEN → ESTABLISHED → FIN_WAIT/CLOSE_WAIT → TIME_WAIT
- [ ] 3-way handshake (SYN, SYN-ACK, ACK)
- [ ] veri transferi: segment reassembly, seq/ack takibi
- [ ] bağlantı kapama: FIN/FIN-ACK/ACK (aktif + pasif)
- [ ] temel retransmit (sabit timeout, basit)
- [ ] `connect()` TCP için gerçek SYN göndermeli
- [ ] `listen()` + `accept()` — server socket

### Syscall tamamlama
- [ ] `recv()` / `send()` (MSG_PEEK, MSG_WAITALL temel flaglar)
- [ ] `SO_REUSEADDR` setsockopt
- [ ] blocking connect timeout

### Test hedefleri
- [x] `nc 10.0.2.2 9999` host listener'a bağlanıyor, veri alıyor
- [x] `wget http://10.0.2.2:9000/` çalışıyor — gerçek HTTP GET, HTML response alındı
- [x] RST → ECONNREFUSED doğru
- [ ] clean FIN teardown (wget "connection closed prematurely" sorunu)
- [ ] basit echo server userspace'te yazılabiliyor

---

## v0.6 — Pthreads (CLONE_THREAD + Futex)  ← ŞİMDİ

**Neden SMP'den önce?**
SMP'ye geçtiğinde atomik primitive'lerin zaten doğru olması lazım.
Futex'i single-CPU'da yazıp test etmek çok daha kolay.
Pthreads olmadan SMP yazsan bile userspace onu kullanamaz.
Sıralama: önce doğruluğu garanti et, sonra paralel çalıştır.

### Thread'ler
- [ ] `clone(CLONE_THREAD | CLONE_VM | CLONE_SIGHAND)` — adres alanı paylaşımı
- [ ] thread-local storage: x86_64 `%fs` (ARCH_SET_FS), aarch64 `TPIDR_EL0`
- [ ] `set_tid_address` — musl thread init için gerekli
- [ ] thread exit: grup üyesi, `SIGCHLD` sadece process group ölünce
- [ ] `tkill` / `tgkill`

### Futex
- [ ] `futex(FUTEX_WAIT, FUTEX_WAKE)` — temel
- [ ] `futex(FUTEX_REQUEUE)` — pthread_cond_broadcast için
- [ ] futex hash table (kernel wait queue, userspace addr'e göre)

### Bellek
- [ ] vmalloc — kernel sanal adres alanı allocator (şu an yok)
- [ ] thread stack: `mmap(MAP_ANONYMOUS | MAP_GROWSDOWN)`

### Test hedefleri
- [ ] musl pthreads ile derlenen basit C programı çalışıyor
- [ ] `pthread_create / pthread_join / pthread_mutex` çalışıyor
- [ ] `pthread_cond_wait / signal` çalışıyor

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

### Scheduler
- [ ] per-CPU runqueue (şu an global tek kuyruk)
- [ ] IPI: `sched_ipi` → remote CPU'yu uyandır (`need_resched`)
- [ ] load balancing: basit — en boş CPU'ya push (steal yok henüz)
- [ ] `sched_setaffinity` / `sched_getaffinity` gerçek implementasyon

### Locking
- [ ] ticket spinlock → test-and-set yerine daha fair
- [ ] TLB shootdown IPI (fork/mmap sonrası remote flush)
- [ ] per-CPU variables (`this_cpu_read/write`)

### Test hedefleri
- [ ] `nproc` → 4 (QEMU `-smp 4`)
- [ ] `pthread_create` ile oluşturulan 4 thread 4 ayrı CPU'da çalışıyor
- [ ] race condition testi geçiyor (basic stress test)

---

## v0.8 — Dinamik Linker & Binary Uyumluluk

**Neden burada?**
TCP + pthreads + SMP olunca statik binary sınırı ciddi darboğaz.
Go, Rust, Python gibi derlenen araçları çalıştırabilmek istiyoruz.
musl libc dinamik versiyonu ile çalışmak hedef.

### Loader
- [ ] `PT_INTERP` ELF segment işleme — interpreter path okuma
- [ ] `ld-musl-aarch64.so.1` / `ld-musl-x86_64.so.1` yükleme
- [ ] `execve` sırasında interpreter'ı ELF gibi yükle

### mmap & relocation altyapısı
- [ ] `mmap(MAP_FIXED)` güvenilir (şu an temel var)
- [ ] `mprotect` `PROT_EXEC` tam çalışmalı
- [ ] `mremap` temel destek
- [ ] lazy allocation: MAP_ANONYMOUS page fault on access

### VDSO
- [ ] `clock_gettime(CLOCK_REALTIME/MONOTONIC)` VDSO'dan
- [ ] `gettimeofday` VDSO'dan
- [ ] `AT_SYSINFO_EHDR` auxv entry

### Test hedefleri
- [ ] musl dinamik linked "hello world" çalışıyor
- [ ] `ldd` output gösterebiliyoruz
- [ ] basit Go/Rust binary çalışıyor

---

## v0.9 — Driver Ekosistemi

Gerçek donanımda gerçek işler yapabilmek.

### USB (temel)
- [ ] xHCI host controller (PCIe'den detect)
- [ ] USB HID: klavye (zaten PS/2 var, USB da lazım)
- [ ] USB mass storage (BBB protokol) — USB bellek okuma

### Depolama
- [ ] NVMe (PCIe SSD) — modern laptop/PC için şart
- [ ] AHCI (SATA) — eski PC için

### Ağ
- [ ] e1000 / virtio-net (zaten var) üstüne DNS resolver
- [ ] DHCP client (şu an IP hardcode)
- [ ] IPv6 temel (neighbor discovery, stateless autoconfig)

### Terminal
- [ ] `ptmx` / `pts` — pseudo-terminal (SSH için şart)
- [ ] proper TTY line discipline (canonik mod, erase, kill, intr)
- [ ] `TIOCSWINSZ` window resize

---

## v1.0 — "Çalışan Sistem"

- [ ] SSH server çalışıyor (dropbear statik → dynamic)
- [ ] HTTP server çalışıyor (nginx statik → dynamic)
- [ ] Python veya Lua interpreter çalışıyor
- [ ] Raspberry Pi 4'te SD karttan boot, ağ çalışıyor
- [ ] x86_64 PC'de USB boot, NVMe/SATA disk, ethernet

---

## Teknik Notlar

- `exceptions_asm.S` → `process_t.ctx` offset 16 hardcode. struct başı değişmesin.
- Her değişiklikten sonra hem x86_64 hem aarch64 build edilmeli.
- aarch64 virt max RAM: 512 GB (T1SZ=25). x86_64 max RAM: 4 GB (boot.S direct map 4 entry — v0.7 öncesi genişlet).
- SMP için spinlock'lar şu an IRQ-disable ile çalışıyor; v0.7'de atomik olmalı (LDAXR/STLXR, LOCK CMPXCHG).
- TCP retransmit: basit sabit-timeout yeterli v0.5 için. SRTT/Karn v0.9'a bırak.
- Doom: `make ARCH=x86_64 qemu-doom` → serial.log
