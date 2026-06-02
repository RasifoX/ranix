# Ranix Roadmap

> "Ranix akar." — Hobiden çıkıyoruz.

Hedef: statik binary çalıştırabilen, ağa bağlanabilen, çok çekirdekli,
gerçek donanımda çalışan, Linux ile kıyaslanabilir bir kernel.

Kural: her versiyon kendi başına anlamlı bir şey yapabilmeli.
Kural: her versiyon hem x86_64 hem aarch64'te build ve test edilmeli.

---

## Neredeyiz (v0.8.5 — glibc TEMEL ÇALIŞIYOR + ağ Linux-sınıfı + güvenlik/ABI olgun — iki arch)

**Linux karşılaştırması**: Syscall yüzeyi ~Linux 4.11+ ; dinamik binary + **8/8 namespace** + cgroups v2 + container + **seccomp (STRICT+BPF)** + **gerçek glibc (static+dynamic)** var. Mimari boşluk daralıyor: çok çekirdek ✅, dinamik linker ✅, namespaces ✅ (8/8), TLS/DNS/DHCP ✅, IPv6 loopback dual-stack ✅, **glibc temel ✅**; **kalan büyük blok: gerçek dağıtım runtime'ı (Python/Node → Claude CLI)**.

**2026-06-03 — glibc port temel oturumu (bu oturum, 8 commit, iki arch yeşil):** glibc'ye TEK SATIR dokunmadan, her fatal'i Ranix'in eksik bir Linux davranışına işaret olarak okuyup çekirdeği standarda çektik.
- **glibc static + dynamic ÇALIŞIYOR** (ld-linux + libc.so.6 + reloc), hem execve hem init= yoluyla.
- **glibc NPTL pthread** create/join/mutex/cond/**cond_timedwait** (doğru timeout), malloc arena, libm, **dlopen/dlsym/dlclose**, stdio file I/O.
- **fork + COW + exec + popen/system** çalışıyor (gfork/gposix testleri).
- Düzeltmeler: (1) pid 1'e fd 0/1/2=/dev/console + mutlak /proc/self/exe; (2) proc_exec INTERP (dynamic init); (3) futex FUTEX_CMD_MASK + WAIT_BITSET absolute timeout; (4) userspace segfault'ta kernel panic→force_sigsegv (fp-walk + signal-frame guard); (5) mmap MAP_FIXED VMA-replace (vma_clip) + proc_exec VMA → fork-COW; (6) clone3 CLONE_VFORK + child stack → posix_spawn.
- **panic ekranı** Linux oops'u solladı: decode/region/reason teşhis satırları (ekran-fit kademeleri korundu).
- DERS: Ranix incremental build GÜVENİLMEZ (WSL2 mtime skew) → her değişiklikte `make clean`.

**2026-06-02 — büyük ağ + ABI + güvenlik oturumu (~11 commit, iki arch yeşil):**

**2026-06-02 — büyük ağ + ABI + güvenlik oturumu (bu oturum, ~11 commit, iki arch yeşil):**
- **Ağ:** TCP timestamps (RFC 7323); SACK; **skb headroom kök-bug** (64→128, latent buffer-underflow); **IPv6 loopback dual-stack** — IPv6 başlık + ICMPv6 + UDP + TCP over ::1 (pseudo-header cksum, family-aware tcp.c, c-ping6/udp6/tcp6). NDP (komşu keşfi) + SLAAC + virtio-net multicast RX yazıldı (working-tree) — gerçek-ağ doğrulaması Pi4+modem bekliyor (WSL2 IPv6 kapalı, slirp ipv6=on bozuk).
- **v0.8 ABI:** execveat, clone3, pidfd_open/send_signal, /proc Threads gerçek sayım.
- **v0.7 çekirdek:** atomic_t/atomic64_t Linux API; **SCHED_FIFO/RR** + sched_setscheduler ailesi (RT runqueue-önce).
- **v0.7.5:** **seccomp** STRICT + BPF FILTER (cBPF interpreter; Docker/systemd sandboxing).
- **v0.9 debug:** **/proc/pid/mem** cross-process R/W (gdb; ptrace-vm-switch mekanizması). Terminal (TTY line discipline ICANON/ISIG + TIOCGWINSZ/TIOCSWINSZ) zaten vardı.
- DERS: musl `sched_*`/`seccomp` wrapper'ları libc-side short-circuit → testlerde RAW syscall().

**(eski) v0.8 + kapasite ölçeği — x86 241 / aarch64 240**: dinamik binary, 8/8 namespace, cgroups v2, dinamik linker, ASID, 5-level/48-bit, GPT+ext4, 1G hugepage.

**2026-06-01 — büyük oturum (bu oturum):**
- **Event-driven çekirdek:** per-tick blanket-wake SİLİNDİ; her blocking primitive event-driven. sched.c → sched/proc/signal + syscall.c kategori split.
- **x86_64 SMP ÇALIŞIYOR** (4/4 CPU) — eski "triple fault" maddesi geçersiz.
- **mprotect/W^X enforcement** (güvenlik) + loopback net + TCP/UDP testleri.
- **v0.7.5 TAM:** 8/8 namespace (uts/ipc/time/mount/pid/user/net/cgroup) + cgroups v2 (pids/memory/cpu/freezer) + gerçek rootless container. Ranix container çalıştırıyor.
- **Dinamik linker (v0.8) ÇALIŞIYOR:** PT_INTERP + ld.so + dinamik bash + dlopen/multi-DSO. Kernel relokasyon yapmıyor — ld.so userspace'de.
- **aarch64 ASID** (ctx-switch TLB flush yok); **procfs compat** (exe/cwd/root/comm/ns/*/cgroup/uid_map/...).
- Test 192 → **238/238** (iki arch).

**2026-06-01 — kapasite/ölçek oturumu (bu oturum, devamı):** "o kadar çekirdek/RAM/disk/sayfa" hedefi kapatıldı.
- **Çekirdek:** x86 ACPI MADT auto-detect + MAX_CPUS→1024 + **dinamik per-CPU** (büyük diziler çekirdek sayısına göre). aarch64 -smp8→8/8, x86 -smp16→16/16.
- **RAM:** x86 4GB→512GB (direct-map PDPT), -m6G doğrulandı.
- **Sayfa:** 2M + **1G hugepage** (MAP_HUGE_1GB, iki arch, fork-COW; havuz+refcount).
- **Disk:** **GPT** (CRC32 + backup-header) + **ext4 extent read + 64BIT** (>16TB adresleme); GPT→ext4 /mnt mount x86 test'li.
- Test → **x86 241 / aarch64 240** (gpt-ext4 yalnız x86'da disk var). THP + glibc bilinçli ertelendi.

**2026-05-30 — kronik bellek bozulması çözüldü** (3 temel bug: COW double-free, FP/SIMD signal'da korunmuyordu, exec I-cache CPU-lokal). + fork/unmap kilit tamamlandı, x86 FP paritesi. Gerçek Pi4'te 5/5 koşu 192/192. Detay: v0.7 bölümü.

**v1.0 hedefi (NETLEŞTİ 2026-05-30)**: üstüne gerçek dağıtım kurulabilen TAM SÜRÜM —
"Ranix Debian" (glibc/dinamik) veya "Ranix Arch" mümkün; üstüne masaüstü giydirilebilir
(KDE / XFCE / Hyprland), Wine çalışır. Yani sadece headless değil, masaüstü-yetenekli.
Claude CLI / SSH / dpkg "headless tam" alt-kilometre taşı (v0.9.5); masaüstü+dağıtım = v1.0.

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
- Security: ruid/euid/suid, capabilities v3 ABI, ASLR (splitmix64), su + /etc/shadow, CPIO uid/gid/mode initrd fix
- SMP: gerçek Pi4'te 4-core (per-CPU runqueue + IPI + per-core timer); yield/idle latency fix (~1440×); buddy free-list metadata (allocator-cascade fix)
- Test: **192/192 ALL_PASS** — virt (aarch64) + x86_64 + GERÇEK Raspberry Pi 4 (5/5 koşu)

---

## v0.5 — TCP Stack  ✅ TAMAMLANDI

- [x] TCP state machine: CLOSED → SYN_SENT/LISTEN → ESTABLISHED → FIN_WAIT/CLOSE_WAIT → TIME_WAIT
- [x] 3-way handshake; veri transferi; bağlantı kapama; temel retransmit
- [x] connect() + listen() + accept() — client + server socket
- [x] recv() / send() (MSG_PEEK, MSG_WAITALL); SO_REUSEADDR; blocking connect timeout
- [x] nc/wget/echo_server testleri geçiyor

### TCP İyileştirmeler ✅ (v0.9 "ağ tam" son ayağı, 2026-06-02)
- [x] TCP congestion control: Reno (slow start, congestion avoidance, fast retransmit/recovery)
- [x] sliding window — çok-segment in-flight (eski stop-and-wait 1seg/RTT yerine min(cwnd,rwnd))
- [x] SRTT/RTO hesaplama (Karn + RFC 6298 + exp backoff)
- [x] out-of-order segment buffer (reassembly; drop+re-ACK yerine tampon)
- [x] SO_KEEPALIVE / SO_REUSEPORT / TCP_NODELAY — setsockopt kabul ediyor (Nagle yok zaten)
- [x] **receive window scaling (RFC 1323)** — SYN'de MSS+WScale option, 256KB pencere (gigabit)
- [x] **flow control** (advertised window = buffer-doluluğu, OOM-safe) + window-update ACK
- [x] **SACK (RFC 2018)** — negotiate + receiver blocks + sender skip-on-retransmit (loss injector'la test'li)
- [x] KRİTİK: write() >MSS byte'ı sessizce düşürüyordu — düzeltildi (test_tcpbulk korur)
- [x] KRİTİK: recv kısa-okumada skb kalanını atıyordu (TLS framing) — düzeltildi
- [~] **event-driven RX** (24x) DENENDİ ama GERİ ALINDI: kilitsiz tcp_state race'i (close↔RX FIN) →
      ~1/3 hang. Stabilite için timer-paced'e dönüldü (8x). Sıradaki: per-socket TCP state lock,
      sonra event-driven'ı geri aç. Test harness hazır (netdrop loss injector + loop-repro).
- TCP Linux-sınıfı (Reno + window scaling + SACK + flow control), STABİL. KALAN: TCP timestamps, IPv6,
  IP forwarding/NAT (router) — her biri ayrı feature.

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
- [x] ruid/euid/suid üçlüsü — process_t'e euid/suid/egid/sgid eklendi
- [x] setuid / setgid / setreuid / setregid / setresuid / setresgid — Linux semantiği
- [x] getgroups / setgroups — groups[32] + ngroups, VFS group match güncellendi
- [x] setuid-bit ELF exec — S_ISUID/S_ISGID: euid/egid = file owner/group
- [x] su çalışması — busybox su + /etc/shadow SHA-512; root→test user uid/gid drop; shadow-no-read (640)

**Yetenekler (Capabilities)**
- [x] CAP_NET_ADMIN, CAP_NET_RAW, CAP_SYS_ADMIN — capabilities.h'de tanımlı
- [x] capget / capset — v3 ABI, gerçek triplet okuma/yazma, EPERM kontrolü
- [x] capability inheritable/permitted/effective üçlüsü — fork + exec transition
- [x] /proc/[pid]/status → CapInh/CapPrm/CapEff satırları + gerçek Uid/Gid/Groups

**Bellek Güvenliği**
- [x] ASLR: mmap_next 1 GB window içinde randomize; stack top sayfa içi randomize
- [x] AT_RANDOM splitmix64 ile güçlendirildi (LCG kaldırıldı)
- [x] prctl(PR_SET_NAME) — zaten çalışıyordu
- [x] prctl(PR_SET_DUMPABLE) — implement edildi

### Test hedefleri
- [x] `su` ile kullanıcı değiştiriliyor — su-switches-uid/gid, shadow-no-read geçiyor
- [x] signalfd + epoll birlikte çalışıyor — c-signalfd testi geçiyor
- [ ] normal kullanıcı /dev/sda'yı açamıyor (EACCES)
- [ ] dbus abstract socket üzerinden haberleşiyor
- [ ] `python3 -c "import multiprocessing"` crash etmiyor (SysV IPC)

---

## v0.7 — SMP  ✅ TAMAMLANDI (gerçek Pi4'te 4-core, 5/5 koşu 192/192 SIFIR çökme; kronik bellek bozulması çözüldü)

> 🏆 **Kronik bellek bozulması çözüldü (2026-05-30).** "Hep vardı, hiç çözülemedi"
> denen temel bozulma üç kök nedenin birleşimiymiş, üçü de çözüldü ve gerçek Pi4'te
> 5/5 koşu tertemiz doğrulandı:
> 1. **COW double-free** — paylaşımlı PTE'de eşzamanlı COW, refcount çift düşüyor → free-while-mapped.
> 2. **FP/SIMD signal'da korunmuyordu** — musl'un vektörel strlen/strchr'si signal ortasında bozuluyordu.
> 3. **exec I-cache CPU-lokal** — `ic iallu` yerine `ic ialluis`; başka çekirdekte bayat kod → çöp PC.


**Neden burada?**
Artık doğru locking primitive'lerimiz var.
Thread'ler zaten var, onları farklı CPU'larda çalıştırmak doğal adım.
SMP olmadan gerçek performans yok; "Linux rakibi" tek çekirdekle olmaz.

### CPU başlatma
- [x] aarch64: secondary boot (trampoline + per-CPU ctx), 4/4 CPU online
- [x] x86_64: secondary boot, 4/4 CPU online
- [x] per-CPU boot stack + trampoline
- [x] per-CPU idle task (ap_idle_procs)
- [x] smp_processor_id() (arch_get_cpu_ctx()->cpu_id)

### Scheduler
- [x] per-CPU runqueue (rq_head/rq_tail) + work-stealing
- [x] IPI: smp_ipi_resched → remote CPU uyandırma (GICv2 SGI / x86 IPI)
- [x] load balancing: pick_cpu — en boş CPU'ya push
- [x] sched_setaffinity / sched_getaffinity (byte-count fix)
- [x] SCHED_FIFO / SCHED_RR — real-time scheduling class ✅ (sched_setscheduler ailesi, RT runqueue-önce)

### SMP kararlılık düzeltmeleri (bu oturum — 2026-05-29)
- [x] **CLONE_PARENT_SETTID**: clone artık ptid (arg2) yazıyor. Eksikken worker
      thread tid=0 kalıyordu → musl `__tl_lock` (`if(val==tid)return`) no-op →
      SMP'de thread listesi bozuluyor → `pthread_exit`'te self->prev=NULL crash.
- [x] **vfork → fork**: vfork parent'ın stack'ini paylaşıyordu; SMP'de parent
      ve child eşzamanlı koşup ortak user stack'teki dönüş adreslerini bozuyordu
      → busybox `timeout` `ret` ile .bss'e atlıyor → EC=0x0 (undefined). Artık
      vfork tam fork (ayrı adres uzayı). Tüm timeout/busybox segfault'ları gitti.
- [x] **schedule() ölü-process guard**: dönüş yolunda READY olarak requeue edilen
      bir process ölümcül sinyal alıp task_exit çağırırsa, runqueue'da PROC_DEAD
      kalabiliyordu; schedule onu çalıştırınca task_exit "geri dönüyor" ve sinyal
      sonsuz yeniden tetikleniyordu (SIGCHLD storm → CPU IRQ-masked kilitleniyor).
      schedule artık PROC_DEAD/ZOMBIE process'i asla çalıştırmıyor; task_exit
      re-entry guard'lı.
- [x] **.sync_fault task->ctx koruması (EN BÜYÜK FIX)**: aarch64 `.sync_fault`
      (exceptions_asm.S) bir fault'ta `task->ctx`'i fault-frame'e yazıp geri
      koymuyordu. signal_dispatch (syscall-return yolunda) sinyal frame'ini user
      stack'e yazarken stack-grow fault'u tetikleyince, iç içe fault `task->ctx`'i
      bozuyordu; dıştaki dönüş yolu `sched_get_current_ctx` ile bayat frame'i
      restore edip bozuk state'e eret ediyor → CPU IRQ-masked kilitleniyordu.
      `.sync_fault` artık task->ctx'i kaydedip geri koyuyor, kendi frame'ine eret
      ediyor. **Hang'ler ~%17 → ~%2-5'e düştü** (asıl freeze nedeni buydu).
- [x] **smp_mb() proc_clone_thread'de** (runqueue_add öncesi): tid'in child
      runnable olmadan önce globalde görünmesini garanti. (tid'in doğru yazıldığı
      segfault dump'ında `tid@tp-0xa8=pid` ile DOĞRULANDI — tid=0 değil.)
- [~] **TLB lazy-switch GERİ ALINDI**: arch_vm_switch (aarch64) artık her AS
      değişiminde tam flush yapıyor — lazy-skip ASID'siz, freed-root reuse'da
      stale-TLB riski taşıyordu. Doğru hâli ASID'li TLB (v0.8 hedefi).

### v0.7 tamamlama (2026-05-30) — GERÇEK Pi4'te 4-core, 191/191 KARARLI
- [x] **Gerçek Raspberry Pi 4'te 4-core SMP boot** (spin-table; EL2'de SMPEN;
      ap_boot_sync PoC clean — secondary cache-kapalı `ttbr0=0` okuyordu;
      SGI0 IPI handler register; secondary per-core timer route + per-core
      IRQ-source dispatch). **pthread crash'in QEMU-MTTCG artefaktı olduğu
      KANITLANDI**: gerçek 4 çekirdekte c-threads / smp-threads-ok / c-ptrace
      HEPSİ PASS. (Yukarıdaki "pthread thread-list race" hipotezi MTTCG bellek-
      modeli quirk'iymiş; Ranix bariyer mantığı doğru.)
- [x] **yield/idle latency fix**: idle_loop `need_resched` görünce hemen
      reschedule (eskiden bir sonraki 100Hz tick'e kadar WFI'de bekliyordu →
      yield/fork ~10ms tick-bound). Gerçek Pi4: sched-yield 9.4ms→6.5µs
      (~1440×), fork+wait 10ms→545µs (~18×), ctxsw 53µs→12µs, mmap 196µs→53µs.
- [x] **buddy free-list → mem_map metadata (allocator-cascade fix)**: free-list
      boş sayfa İÇERİĞİNDE saklanıyordu; heavy SMP churn'de bir UAF-write onu
      ezip tüm allocator'ı çökertiyordu (çığ → suite fail + kernel panic). Linux-
      tarzı metadata free-list (page_t.free_next/prev, page index) → allocator
      içerik-bozulmasına BAĞIŞIK. **Çığ gitti; suite gerçek Pi4'te kararlı
      191/191 (5/5 koşu), panic yok.** + buddy double-free guard (güvenlik neti).
- [x] **Ölçüm + ABI**: `/bin/bench` mikro-benchmark suite; clock_gettime ns
      çözünürlük (aarch64 cntvct + CNTKCTL EL0 erişimi); getsockopt gerçek
      (SO_TYPE/ERROR/RCVBUF…); test_clock regresyon testi.
- [x] **Test: 192/192 ALL_PASS** — virt (aarch64) + x86_64 + GERÇEK Pi4 (cow-race dahil).

### free-while-mapped UAF ✅ ÇÖZÜLDÜ (2026-05-30)
- [x] **Kök neden**: thread'ler tek adres uzayını paylaşıyor; iki CPU aynı
      PTE'de aynı anda fault atınca COW yolu (refcount oku → kopyala → eşle →
      orijinali serbest bırak) serileştirilmemişti. İkisi de copy-and-free
      yapıyor, paylaşılan sayfanın refcount'unu tek mantıksal referans için
      İKİ KEZ düşürüyordu → sıfıra inip allocator'a dönüyor, oysa fork'lanmış
      kardeş hâlâ map'liyor. Tek-thread fork her zaman güvenliydi (her process
      kendi PTE'sini bir kez serbest bırakır); yalnızca paylaşılan PTE'de
      eşzamanlı fault çiftliyordu. Gerçek Pi4'te register/pointer bozulması,
      QEMU MTTCG'de pthread "artifact"ı sandığımız şey buydu.
- [x] **Çözüm**: fault çözümü adres-uzayı başına kilit (`vm_space.fault_lock`)
      altında; kilit içinde PTE yeniden okunuyor → ikinci gelen zaten-private
      sayfayı görüp sadece yeniden eşliyor. Paylaşımlı kod (`vm_handle_fault`),
      iki mimari de kazanıyor.
- [x] **Kanıt**: `test_cowrace` — guardian COW-snapshot'ı RO tutar, 4 thread
      aynı sayfaları aynı anda yazar. Fix YOKKEN aarch64'te ilk iterasyonda her
      koşuda CORRUPT + kernel double-free guard ateşliyor; fix VARKEN 5+ koşu
      tertemiz. Suite'e bağlandı (smp-cow-race, 192/192).

### Locking
- [ ] ticket spinlock → daha fair
- [ ] TLB shootdown IPI (fork/mmap sonrası remote flush) — şu an her vm_switch'te
      tam `tlbi vmalle1is` (ağır ama doğru)
- [ ] per-CPU variables (this_cpu_read/write)
- [x] atomic_t + atomic64_t gerçek implementasyon (LOCK prefix / LDXR-STXR) ✅ (kernel/sync.c)

### x86_64 RAM genişletme (→ Linux seviyesi, bkz Kapasite hedefleri)
- [x] boot.S direct map: 4 entry → **512 entry (512 GB)** ✅; **1G huge page** ✅ (kullanıcı MAP_HUGE_1GB + direct-map). ≥128TB/5-level kaldı.
- [ ] 5-level paging (LA57) opsiyonu — PB-sınıfı adresleme
- [ ] PHYS_TO_VIRT sınırını kaldır — 64-bit PML4 tam kullanımı

### Test hedefleri
- [x] nproc → 4 (QEMU -smp 4)
- [x] 4 pthread farklı CPU'larda çalışıyor (affinity test) — x86_64 kararlı,
      aarch64 çoğunlukla geçiyor (yukarıdaki race hariç)
- [ ] stress -c 4 — sistem kilitlenmiyor (aarch64 hang race kapanınca)

---

## v0.7.x — Taban Sağlamlaştırma  ⏳ SIRADAKİ (foundation-first, "bina gibi")

**Neden burada?** Üstüne kat çıkmadan önce temel taş gibi olmalı. Çatlak taban,
yukarıda çözülmesi imkânsız hayalet bug'lara dönüşür. Kronik bozulmayı çözdük ama
sıfır-flake değiliz ve gelecekteki temel bug'ları (boot.S dahil) anında yakalayan
bir ağ yok. Dinamik-ölçeklenme yeniden-yazımı da bu sağlam taban üstüne oturmalı.

### Sıfır-flake (önce bunu)
- [ ] cow-race 1/10 flake (Pi'de got='' sessiz ölüm) — kökü bulunmalı, sebep gizli
- [ ] signal-storm wedge — child sürekli `kill` → full-suite'te sistem kilitleniyor
      (gerçek signal/IPI scheduler soundness bug'ı; 1/10 flake'le aynı kök olabilir)
- [ ] "10'da 1 = sorun var, çözülür" — non-determinizm sıfırlanmadan üste inşa yok

### Sessiz bozulmayı gürültülü yap (invariant katmanı)
- [x] poison-on-free: serbest sayfaya pattern bas, alloc'ta kontrol (UAF-read yakala) ✅ (buddy_poison + buddy_poison_ok + double-free guard)
- [x] process_t / vm_space_t / kstack magic alanları — bozulma anında lokalize panic ✅ (PROC_MAGIC, VM_SPACE_MAGIC, cpu->magic+BUG_ON, HEAP_MAGIC)
- [x] kstack canary trap-giriş/çıkışta da kontrol (şu an sadece schedule'da) ✅ (2026-06-03: syscall_handle girişi + signal_dispatch çıkışı)
- [ ] ucuz invariant'lar KALICI (gerekçeyi belgeler; AI slop değil) — pahalılar CONFIG_DEBUG

### Robustness modeli (Linux'u takip et)
- [ ] kernel-invariant ihlali → gürültülü panic (BUG()); bozulmuş halde limp etme
- [ ] DIŞ hata (userspace çöp pointer, driver probe fail, I/O hata, OOM) → contain et,
      o process/driver'ı öldür, kernel ASLA düşmesin. "Ranix taş gibi" = bu.

### Dinamik runtime-growable ölçeklenme (compile-time config YASAK)
- [x] MAX_CPUS sabiti → **dinamik per-CPU** ✅ (büyük diziler buddy'den çekirdek sayısına göre; tavan 1024, ≥2048 vmalloc kaldı)
- [ ] proctab/runqueue/scheduler tam dinamik (1 ↔ binlerce çekirdek, aynı binary)
- [ ] bellek yapıları RAM miktarına uyarlanır (128MB ↔ TB+, mem_map/buddy dinamik)
- [ ] per-CPU değişkenler + RCU-benzeri primitif (Linux'tan miras, üstüne iyileştir;
      dinamik AMA kilitsiz hot-path — global kmalloc'a bağlama, "akmalı")
- [ ] devtmpfs + ttyN dinamik (sabit sayı yok)
- [ ] global sched_lock → per-CPU rq-lock (ölçeklenme engeli)
- [x] **ASID'li TLB (aarch64)** ✅ — per-space 8-bit tag + PTE_NG; switch'te flush YOK (asid!=0). asid 0 = flush-sentinel (kernel/fallback); free'de `tlbi aside1is` (SMP-safe reuse). 5x temiz suite; gerçek-Pi4 stress kaldı.

### Kapasite hedefleri — Linux'a EŞİT veya FAZLA (compile-time sabit YOK, hepsi runtime)
> Kural: Ranix donanımın verdiğini sonuna kadar kullanmalı; "yapılıyorsa daha fazlası".
- [x] **Çekirdek:** MAX_CPUS→**1024** + **dinamik per-CPU** ✅ (kstack/irq/ctx/idle çekirdek sayısına göre buddy'den; statik yalnız all_cpus+boot_sync ~50KB). aarch64 FDT + x86 ACPI MADT auto-detect; aarch64 -smp8→8/8, x86 -smp16→16/16. ≥2048 için kstack vmalloc gerek (follow-up).
- [x] **RAM (x86):** 4-level ~127 TB direct map ✅ + **5-level paging (LA57) ✅** — CPUID-gated, **128 PB VA** (Linux LA57 paritesi/fazlası). pml5 shim (kernel pml4 yeniden kullanılır), user_pml4() walk root'u çözer, SMP AP'ler 5-level'da. -cpu max→5-level 243/243, -cpu qemu64→4-level 241/241.
  - [x] **RAM/VA (aarch64): 48-bit / 256 TB** ✅ (3→4 level, T0SZ/T1SZ=16, kernel_l0 shim + l1_for vaddr-indexed walk; **Linux paritesi**). Direct map tüm RAM'i 1GB block ile haritalar (eskiden 8GB'de tıkalıydı). 241/241, SMP 4/4. 52-bit (FEAT_LPA) = follow-up.
- [x] **Disk:** 64-bit LBA ✅ + **GPT** ✅ (CRC32 header + entry-array CRC + backup fallback, >2TB) + **ext4 extent read + 64BIT (>16TB)** ✅ + **ext4 write-64bit metadata** ✅ + **journaled ext4 mount/read/write** ✅ (has_journal, inode 8). Kalan: jbd2 replay (crash recovery), ext4 write extent (>4TB dosya).
- [~] **Sayfa boyutu:** 4K + **2M hugepage** ✅ + **1G hugepage** ✅ (MAP_HUGE_1GB, iki arch; boot-time refcount'lu havuz, ayrı yüksek VA penceresi, fork 1G-COW; x86 -m6G/aarch64 -m4G test'li). THP (otomatik promosyon/khugepaged) = follow-up.
- [ ] **Açık dosya / process / PID:** dinamik tablo, RAM'le ölçeklenen üst sınır (statik cap yok)
- [ ] Ölçüt: aynı binary 1 çekirdek/128 MB cihazda da, binlerce çekirdek/TB sunucuda da çalışır

---

## v0.7.5 — Konteyner Altyapısı & Kaynak Yönetimi

**Neden burada?**
Docker, Podman, LXC hepsi namespace + cgroups üstünde çalışıyor.
Namespaces SMP'den sonra geliyor çünkü per-CPU struct'lar değişiyor.

### Namespaces — 8/8 TAM ✅ (UTS, IPC, time, mount, PID, user, net, cgroup; TÜM Linux namespace seti; 2026-06-01)
- [x] **UTS namespace**: unshare/clone(CLONE_NEWUTS), hostname/domainname izole; current_uts()
- [x] **IPC namespace**: unshare/clone(CLONE_NEWIPC), SysV shm/sem + POSIX mqueue izole (owner-tag + ns destroy sweep)
- [x] **time namespace**: unshare(CLONE_NEWTIME), CLOCK_MONOTONIC/BOOTTIME offset; /proc/<pid>/timens_offsets
- [x] **unshare + setns** syscall (8 ns için); clone bayrakları (time hariç — 0x80 signal-byte çakışması, Linux de yasak)
- [x] **/proc/[pid]/ns/{uts,ipc,time,mnt,pid,user,net,cgroup}** — nsfs identity + openable fd (setns); namei "fops'lu symlink follow etme"
- [x] **cgroup namespace**: clone/unshare(CLONE_NEWCGROUP) ✅ — /proc/self/cgroup view re-root, /proc/ns/cgroup+setns (f3aa3c8)
- [x] **mount namespace**: unshare(CLONE_NEWNS) ✅ — splice→per-ns mount-following (namei), unshare/setns/identity, izolasyon testli (95d5398)
- [x] **PID namespace**: clone/unshare(CLONE_NEWPID) ✅ — tam Linux modeli: per-ns (ns,nr) vektörü, getpid/getppid/kill/waitpid çevirisi, init(1) reaper (yetim reparent + ns-init ölünce ns SIGKILL), /proc/ns/pid+setns (af35c9b). kernel/pid.c
- [x] **network namespace**: clone/unshare(CLONE_NEWNET) ✅ — per-ns device registry + per-ns loopback + socket→ns + ns-scoped RX matching (tcp/udp/icmp); /proc/ns/net+setns (cde040b)
- [x] **user namespace**: clone/unshare(CLONE_NEWUSER) ✅ — kuid modeli, uid_map/gid_map (write-once), getuid/getgid çevirisi, /proc/ns/user+setns, rootless temeli (2842b85). kernel/user_ns.c
- Container için mount+pid şart; kalanlar somut ihtiyaç çıkınca

### cgroups v2
- [x] **cgroupfs (cgroup2)** — hierarchy, mkdir subgroups, cgroup.procs; kernel/cgroup.c (512d53a)
- [x] **PID controller: pids.max / pids.current** — subtree sayımı + fork'ta EAGAIN (fork-bomb koruması) ✅
- [x] /sys/fs/cgroup kanonik path (şu an /cgroup; sysfs'in fs/cgroup mountpoint sunması gerek) ✅ (sysfs /sys/fs/cgroup sunuyor)
- [x] **memory controller: memory.max / memory.current** ✅ — user-page charge (mmap + fault stack/COW), limit aşımı kill (4da2eb0). Not: munmap uncharge exit-granular.
- [x] **CPU controller: cpu.weight** ✅ — scheduler quantum'una ölçek (ec6baa5); cpu.max (bandwidth throttle) ileride
- [x] **freezer: cgroup.freeze** ✅ — frozen subtree scheduler'da requeue-skip ile parklanır (a94d1b2)

### sysfs (/sys) — temel iskelet ✅ (2026-06-01)
- [x] /sys pseudo-fs (vfs_register_fs); /sys/kernel/{ostype,osrelease,hostname}
- [x] /sys/class/net/<dev>/{address,mtu,operstate}; /sys/block/<dev>/size
- [x] /sys/devices/system/cpu/{online,possible}
- [x] /sys/fs/cgroup (cgroups ile birlikte) ✅ (2026-06-03: cgroup.controllers/procs)
- [x] /sys/kernel/mm/transparent_hugepage stub ✅ (2026-06-03: enabled = always [madvise] never)
- [x] /sys/bus/pci/devices — PCI device listing ✅ (2026-06-03: ls + vendor/device, x86 6 device)

### Seccomp
- [x] prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) ✅
- [x] seccomp(SECCOMP_SET_MODE_FILTER, ...) — BPF filtre ✅ (cBPF interpreter)

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

### ELF Dinamik Linker — ÇALIŞIYOR (musl ld.so ile, 2026-06-01, iki arch)

> KİLİT İÇGÖRÜ: kernel relokasyon motoru YAZMIYOR. Kernel ana binary + interp'i
> (musl libc.so = ld.so) yükleyip interp entry'sine atlar; ld.so userspace'de TÜM
> relokasyonları + library yüklemeyi yapar. Aşağıdaki R_* relokasyonları KERNEL'de
> değil, ld.so'da — bizim yazmamıza gerek yok.

- [x] PT_INTERP segment işleme — interpreter path okuma (out->interp[128])
- [x] execve: interpreter'ı ELF gibi yükle (mmap base'e), AT_BASE = interp load adresi, jump = interp entry, AT_ENTRY = ana binary
- [x] elf_load load_bias_hint param (interp'i ana binary'den ayrı yere koymak için)
- [x] ld-musl-<arch>.so.1 initramfs /lib/'de (toolchain libc.so kopyalanıyor)
- [x] R_*_RELATIVE / GLOB_DAT / JUMP_SLOT — **ld.so yapıyor** (kernel'de değil)
- [x] TLS: x86_64 FS, aarch64 TPIDR_EL0 (zaten var; dinamik binary'lerde ld.so kuruyor)
- [x] execveat — dirfd + relative path ile exec ✅
- [x] R_*_IRELATIVE — IFUNC (musl kullanmaz; glibc gelince ld.so tarafında gerekli) ✅ (dynamic glibc çalışıyor)
- [ ] PT_GNU_RELRO mprotect(PROT_READ) — ld.so yapar; bizde mprotect var, doğrula
- [x] **dinamik bash** — bash dinamik + shared libc, tüm 221-test suite'ini dinamik bash yürütüyor (init `/bin/bash /test.sh`). + mprotect VMA-split fix (RELRO write-strip bug'ı)
- [x] **dlopen / dlsym / dlclose** + load-time multi-DSO — libfoo.so (test_sharedlib + test_dlopen), iki arch ✅

### mmap & memory tamamlama
- [x] MAP_FIXED güvenilir (şu an temel var) ✅ (2026-06-03 vma_clip overlap-replace)
- [x] mremap temel destek ✅
- [ ] MAP_POPULATE — prefault pages
- [x] msync — MAP_SHARED dirty page flush ✅
- [ ] MAP_GROWSDOWN thread stack — SIGSEGV yerine otomatik büyüme

### auxv tamamlama
- [ ] AT_HWCAP doğru değer: x86_64'te CPUID'den SSE2/AVX bitlerini oku; aarch64'te ID_AA64ISAR0_EL1 oku
- [ ] AT_HWCAP2 — x86_64 AVX512 ve aarch64 ek feature'ları
- [ ] AT_SYSINFO_EHDR — VDSO load adresi (VDSO v0.9'da; şimdilik 0)
- [x] AT_BASE — interpreter load adresi (dinamik linker yüklenince düzeltilmeli) ✅ (proc_exec+execve veriyor)

### glibc için gerekli /proc eksikleri
- [x] /proc/self/exe → readlink (+ /proc/<pid>/{cwd,root,comm,ns/*}) (2026-06-01)
- [x] /proc/self/cmdline — null-separated argv
- [x] /proc/sys/vm/overcommit_memory + /proc/sys/kernel/osrelease (zaten var)
- [ ] /proc/self/status: Tgid var; Threads, SigBlk, SigIgn satırları ekle

### glibc için gerekli yeni syscall'lar
- [x] clone3 — glibc 2.36+ thread creation (clone'a graceful fallback var ama olsun) ✅ (+CLONE_VFORK posix_spawn)
- [ ] rseq — glibc 2.35+ restartable sequences (register etmezse çalışmaya devam eder)
- [x] pidfd_open / pidfd_send_signal — glibc 2.36+ thread sync ✅
- [x] fadvise64 / posix_fadvise — memory hint (stub yeterli) ✅ (2026-06-03, no-op hint)
- [x] process_vm_readv / process_vm_writev — cross-process memory (stub) ✅ (2026-06-03, gerçek impl — iovec+vm-switch)
- [x] prlimit64 — setrlimit64 + getrlimit64 kombinasyonu ✅

### VDSO (temel)
- [ ] clock_gettime(CLOCK_REALTIME/MONOTONIC) VDSO'dan — syscall overhead yok
- [ ] gettimeofday VDSO'dan
- [ ] AT_SYSINFO_EHDR auxv entry

### Test hedefleri
- [x] musl dinamik linked "hello world" çalışıyor (test_dynamic, iki arch) ✅
- [x] **dinamik bash çalışıyor** — tüm suite'i dinamik bash yürütüyor ✅
- [x] dlopen / çoklu shared .so senaryosu (libfoo.so) ✅
- [x] glibc dinamik linked "hello world" ✅ (2026-06-03, iki arch — GLIBC-DYN-OK)
- [x] dynamically linked pthread test geçiyor ✅ (gstress: create/join/mutex/cond + libm)

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
- [x] IFUNC resolver chain — dynamic glibc çalışıyor (her string fn IFUNC; reloc + resolver OK) ✅
- [ ] locale: glibc locale/charset altyapısı çalışıyor (en_US.UTF-8 yeterli)
- [ ] timezone: /etc/localtime okuma (tzdata paketi kurulabilmeli)
- [ ] NSS (Name Service Switch): /etc/passwd, /etc/hosts — getpwnam, gethostbyname
- [x] math: libm.so.6 temel fonksiyonlar çalışıyor ✅ (gstress sqrt, gdl dlopen libm cos)

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

### TLS / HTTPS ✅ (2026-06-02, iki arch, gerçek internet)

- [x] mbedTLS 3.6.2 statik cross-build — userspace (Makefile rule, .cache, bash gibi)
- [x] TLS handshake + HTTPS GET çalışıyor → `HTTP/1.1 200 OK` gerçek sunucudan
      (aarch64 TLS 1.2, x86 TLS 1.3; ECDHE + ChaCha20-Poly1305). test_tls diagnostic.
- [x] SNI (mbedtls_ssl_set_hostname) çalışıyor
- [x] gerçek-internet TCP doğrulandı: HTTP GET (off-link routing→gateway→NAT), test_http
- [ ] curl statik (mbedTLS backend) — bir sonraki (binary diagnostic var, curl portu kaldı)
- [ ] cert doğrulama (CA bundle + time) — şimdilik VERIFY_NONE (handshake/crypto kanıtlandı)
- KRİTİK enabler: TCP short-read bug fix (986a96e) — TLS record framing'i bu açtı

### Ağ konfigürasyonu — DHCP ✅ (2026-06-02)
- [x] DHCP client: DISCOVER/OFFER/REQUEST/ACK, boot'ta NIC'i konfigüre (ip/gw/mask), iki arch
- [x] /etc/resolv.conf'u DHCP'nin verdiği nameserver'a yazar (yazılabilir dosya, elle de değişir)

### DNS çözücü ✅ (2026-06-02, uçtan uca iki arch)

- [x] Gerçek UDP DNS query: /etc/resolv.conf okuma + A sorgusu (musl getaddrinfo, UDP:53)
- [x] `getaddrinfo` çalışıyor — `DNS OK example.com -> ...` gerçek NIC üzerinden
  (bunun için 5 UDP/loopback bug'ı düzeltildi: ephemeral port, socket kaydı, recvfrom kaynak
   adresi, ARP retry, 127/8→lo routing — bkz git log + [[project_ranix_status]])
- [ ] ndots, search domain, timeout/retry (musl default'ları çalışıyor; ince ayar kaldı)
- [ ] DNS cache (basit TTL cache, kernel tarafı veya userspace)

### Debugging & Tracing
- [ ] x86_64: TF (trap flag) ile single-step; aarch64: MDSCR_EL1.SS
- [ ] strace çalışıyor — ptrace tabanlı, statik derlenmiş
- [ ] gdb temel breakpoint + step (dinamik linker ile birlikte)
- [ ] core dump: SIGSEGV'de core file (/proc/sys/kernel/core_pattern)
- [x] /proc/[pid]/mem — cross-process memory R/W (gdb için) ✅ (procfs_mem_rw)

### ACPI (x86_64)
- [x] RSDP/RSDT/XSDT parse — ACPI table discovery ✅ (arch/x86_64/acpi.c)
- [x] MADT okuma — CPU listesi ✅ (LAPIC/x2APIC say; -smp16→16/16)
- [ ] HPET timer — PIT'e alternatif, yüksek çözünürlük
- [x] ACPI_POWEROFF (S5) ✅ (board_poweroff: isa-debug-exit + PIIX4 0x604/0xb004)

### PCIe İyileştirmeleri
- [ ] MSI/MSI-X interrupt — NVMe için şart
- [ ] PCIe capability walk — device feature discovery

### Terminal (pseudo-terminal)
- [x] ptmx / pts — pseudo-terminal ✅ (/dev/ptmx + /dev/pts/N, çift yönlü, TIOCGPTN; raw mode)
- [ ] proper TTY line discipline (canonical mod, erase, kill, intr)
- [ ] TIOCSWINSZ window resize
- [ ] openpty / forkpty

### Depolama
- [ ] NVMe (PCIe SSD) — MSI-X gerektirir
- [x] AHCI (SATA) ✅ (drivers/block/ahci.c — polling, IDENTIFY+RW DMA EXT, block_device sata0; ext4 r/w test'li) — eski PC
- [ ] bcmgenet RPi4 ethernet

### Test hedefleri
- [ ] `curl https://api.anthropic.com` → 401 (bağlandı, auth yok — doğru)
- [ ] `strace ls` çalışıyor, syscall listesi görünüyor
- [ ] `gdb /bin/hello` → breakpoint koyduk, step attık
- [ ] shutdown -h → sistem kapandı (ACPI poweroff)
- [ ] NVMe diskten ext2 mount + read/write

---

## v0.9.5 — "Headless Tam Sürüm" (Claude CLI dahil)

**Tanım**: glibc çalışıyor, dpkg var, TLS var, SSH var.
Terminalde `claude` komutu çalışıyor. Masaüstü YOK — bu, v1.0'a giden son headless taş.

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
- [ ] **Kalıcı root (switch_root/pivot_root)** — root şu an initramfs (ramfs, uçucu).
      ext2/ext4 rw + /mnt auto-mount VAR; eksik: switch_root mekanizması + diske kurulu
      tam rootfs + ext4-write-as-live-root doğrulama. (Linux de initramfs→switch_root yapar.)
      SIRA: ağ tam + SSH/TLS bittikten SONRA (kullanıcı kararı 2026-06-02).
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

## v1.0 — TAM SÜRÜM: Masaüstü & Dağıtım

**Tanım (kullanıcı, 2026-05-30)**: üstüne gerçek dağıtım kurulabilir ve masaüstü
giydirilebilir. "Ranix Debian" / "Ranix Arch" yapılabilir; KDE / XFCE / Hyprland
sorunsuz açılır; Wine çalışır. İki libc yolu da desteklenir: glibc/dinamik (Debian-tarzı)
ve musl/statik (Alpine-tarzı). Fark = linker + paket yöneticisi + üstündeki GUI; taban aynı.

### Masaüstü sütunu (YENİ — eskiden "Uzun Vade"deydi, v1.0'a taşındı)
**Ekran / GPU**
- [ ] DRM/KMS subsystem: `/dev/dri/card0` + `/dev/dri/renderD128`, mode setting, dumb buffer
- [ ] GBM (Generic Buffer Management) — Wayland compositor buffer ayırma
- [ ] Mesa + **llvmpipe** (yazılım GL/GLES tabanı) — gerçek GPU olmadan Hyprland/KDE çalışsın
- [ ] virtio-gpu (QEMU) + Pi4 VC4/V3D — donanım hızlandırma (yazılım taban sonrası)
- [ ] EGL/GLES kütüphane yolu (userspace, Mesa) — compositor'lar GL'e dayanıyor

**Girdi**
- [ ] evdev: `/dev/input/eventN` Linux ABI (ioctl + read) — libinput buradan okur
- [ ] klavye / fare / touch; PS/2 + USB HID
- [ ] /dev/input/mice uyumluluk

**Cihaz modeli (userspace beklentisi)**
- [ ] devtmpfs — dinamik /dev (udev'in temeli) [bkz v0.7.x dinamik]
- [ ] sysfs tam (/sys/class, /sys/devices) — udev + libinput device enumeration
- [ ] udev (veya eudev) çalışıyor — hotplug + device node

**Pencere sistemi (userspace, kernel'e bağımlılıkları test et)**
- [ ] Wayland: Unix socket + **SCM_RIGHTS fd-passing** (ZORUNLU) + memfd/shm + epoll
- [ ] Weston (referans compositor) açılıyor → sonra wlroots/Hyprland
- [ ] X11 (Xorg veya Xwayland) — KDE/XFCE X11 yolu + eski uygulamalar
- [ ] Ses: ALSA `/dev/snd` (en azından stub + bir HDA/virtio-snd sürücü)

### Wine
- [ ] Wine'ın talep ettiği syscall/mmap semantiği (MAP_FIXED, geniş VA, robust futex,
      ptrace, arch_prctl, sinyal davranışı) — Wine en zorlu userspace hedefi
- [ ] `wine notepad` → pencere açılıyor (Wayland/X üstünde)

### Dağıtım yapılabilirliği
- [x] ext4 (extent + 64BIT) + GPT bölümleme ✅ (GPT→ext4 /mnt mount test'li); ext4 **write**-64bit + journal kaldı
- [ ] init kararı: "Ranix Debian" systemd ister (cgroups v2 + namespaces, v0.7.5) VEYA
      systemd'siz varyant (Devuan/Artix tarzı: OpenRC/runit) — KARAR netleşmeli
- [ ] apt + dpkg (glibc) / pacman benzeri (statik) — paket deposu
- [ ] Ranix ISO + installer (GRUB/U-Boot) [bkz Uzun Vade]

### Test hedefleri
- [ ] Weston QEMU virtio-gpu'da açılıyor; sonra Hyprland (llvmpipe)
- [ ] KDE Plasma veya XFCE oturumu açılıyor
- [ ] `wine notepad` çalışıyor
- [ ] Üçüncü taraf biri "Ranix Debian" kurup masaüstü açabiliyor (gerçek fark görülür)

---

## Kapsam Tamamlama — v1.0'dan ÖNCE kapanmalı (2026-06-01 Linux karşılaştırmasından)

> Kullanıcı kararı: karşılaştırmada çıkan tüm eksikler tam sürümden önce tamamlanmalı.
> Bazıları ilgili versiyon bölümlerinde de var; burada toplu takip + eksik olanlar.

### Donanım / sürücü
- [ ] **USB host stack:** xHCI (USB3) + EHCI (USB2) controller — şu an yalnız "USB HID" bahsi var, host controller YOK
- [ ] USB hub + enumeration + mass storage (USB'den boot)
- [ ] **NUMA** topolojisi (çok soketli) — node-aware buddy + scheduler

### Bellek
- [ ] **THP** (transparent huge pages) — otomatik 2M promotion (MAP_HUGETLB + swap zaten Uzun Vade'de)

### Güvenlik (landlock dışı)
- [ ] **LSM framework** (hook altyapısı) + **SELinux veya AppArmor** (en az biri) — MAC
- [ ] **kTLS** (in-kernel TLS) — opsiyonel, perf

### Güç yönetimi
- [ ] **suspend/resume** (S3 / S0ix) — cpufreq zaten v0.9/Uzun Vade'de

### Ağ (v1.0 öncesi netleşsin)
- [ ] **TCP congestion control** (Reno/CUBIC) + pencere ölçekleme — şu an LAN-only, WAN zayıf
- [ ] **IPv6** temel (SLAAC) — şu an yalnız IPv4

### Mimari KARAR gerektirenler (yapılacak mı?)
- [ ] **32-bit userspace** (ia32/aarch32 compat) — muhtemelen NON-GOAL (saf 64-bit); karar netleşsin
- [ ] **LKM** (yüklenebilir modül) — Ranix monolitik-statik felsefesi; muhtemelen NON-GOAL (driver=derleme-içi), gerekirse imzalı modül

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
- [ ] GPU hızlandırma ileri: Vulkan, virgl/venus, gerçek 3D (v1.0 yazılım-GL tabanından sonra)
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

### Kararlar (2026-05-30)
- **Robustness:** kernel-invariant ihlali → BUG()/panic (gürültülü, limp etme); DIŞ hata
  (userspace/driver/IO/OOM) → contain, kernel asla düşmez. Linux modeli.
- **Dinamik = runtime-growable:** compile-time config YASAK; per-CPU + RCU (Linux'tan
  miras, üstüne iyileştir); dinamik AMA kilitsiz hot-path (global kmalloc'a bağlama).
- **Driver:** native-first temiz/stable API gerçek; ileride (post-1.0) AÇIK Linux driver'ları
  için kaynak-seviyesi şim (LinuxKPI/BSD tarzı). Binary/kapalı ABI taklidi REDDEDİLDİ
  (ReactOS tuzağı). Kapalı driver (nvidia) hiçbir non-Linux kernelde çözülmez; nouveau hedef.
- **Instrumentation = gerekçe:** ucuz invariant/poison/magic kalıcı olabilir; "okunacak şey"
  budur, AI slop değil. Pahalı kontroller CONFIG_DEBUG.
- **v1.0 = masaüstü+dağıtım** (KDE/Hyprland/Wine, Ranix Debian/Arch); Claude-CLI/headless = v0.9.5.
- **GUI tabanı yazılım-GL (llvmpipe + DRM dumb buffer)** — gerçek GPU hızlandırma post-v1.
