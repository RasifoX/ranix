# Ranix Roadmap: v0.16 → v0.17

Ranix, Linux'a rakip olarak tasarlanmış monolitik bir işletim sistemi çekirdeği.
Hedef: doğruluk, kararlılık, tüm donanımda çalışma — sonra özellik.

---

## v0.16 Serisi — Temel Altyapı Sağlamlaştırma

### v0.16.1 — Bellek & Zamanlayıcı Düzeltmeleri ✅ TAMAMLANDI
- [x] SLAB/Object Pool allocator (`kernel/mm/heap.c`) — O(N) yerine O(1)
- [x] Dinamik timer frekansı (250 Hz, hardcoded kaldırıldı)
- [x] Ext2 superblock magic doğrulaması (kör mount engellendi)

### v0.16.2 — VFS Kararlılığı & Symlink Desteği ✅ TAMAMLANDI
- [x] `vfs_close`: ext2 dizin inode'larında `fops = NULL` → NULL dereference crash
- [x] `sys_getdents64`: symlink'ler için `DT_LNK` eksikti
- [x] `vfs_dentry_resolve`: lookup'a tam path yerine tek component geçiriliyordu
- [x] VFS symlink takibi — hem göreceli hem mutlak, max 8 seviye
- [x] `ext2_readlink` — fast symlink ve slow symlink desteği
- [x] EL0 fault tolerans: process EL0'da crash edince `task_exit()` çağrılıyor (`arch/aarch64/exceptions.c`, `arch/x86_64/exceptions.c`)
- [x] ext2 readdir O(N²) → O(1): byte-offset parametresi + `next_off` cursor
- [x] ext4 extent tree stack overflow fix (`inode_map_block`: 240B stack → buddy heap)
- [x] ext2 1-block read cache: ardışık readdir çağrılarında SD okuma sayısı dramatik azalma
- [x] VFS lock: `iops->lookup` (disk I/O) süresince spinlock+IRQ disable kaldırıldı → scheduler açık

### v0.16.3 — Linux ABI Uyumluluğu ✅ TAMAMLANDI
**Hedef:** Herhangi bir Linux AArch64 statik binary'yi çalıştırabilmek (sadece musl değil, glibc, zig, rustc çıktısı dahil).

- [x] Auxiliary Vector (AuxV) — `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_RANDOM`, `AT_HWCAP`, `AT_ENTRY`, `AT_EXECFN`, `AT_UID`, `AT_GID`, `AT_SECURE`, `AT_CLKTCK`
- [x] `elf_load` → `elf_info_t` ile phdr VA, phnum, phentsize döndürüyor
- [x] ET_DYN (PIE) binary desteği — load bias 0x400000
- [x] File-backed `mmap` — fd + offset ile dosya segment'larını belleğe map'le
- [x] `sys_mmap` dispatch: arg4 (fd) ve arg5 (offset) artık geçiriliyor
- [x] `sys_mprotect` gerçek implementasyon — page table permission'larını güncelliyor
- [x] Stack growth on fault — `vm_handle_fault` translation fault + stack zone → otomatik page map
- [x] Initial stack 2 sayfa → 8 sayfa (32KB), max 1MB otomatik büyüme
- [x] `vm_unmap` fiziksel sayfayı serbest bırakıyor (bellek sızıntısı kapatıldı)

### v0.16.4 — Bellek Verimliliği & Süreç Temizliği ✅ TAMAMLANDI
- [x] Ext2: 1-blok cache → 16-girişli LRU block cache (EXT2_BCACHE_N=16) — SD okuma sayısı dramatik düşüş
- [x] VFS: tüm küçük struct tahsisleri (vfs_inode_t, vfs_dentry_t, vfs_file_t, fd_table_t, pipe_t, priv'ler) buddy_alloc(4KB) → kmalloc/slab (~45 inode/sayfa yerine 1)
- [x] pipe: son kapatmada spin_unlock eksikliği düzeltildi (lock tutularak free ediliyordu)
- [x] `task_exit`: VM ve fd tablosu process ölümünde hemen serbest bırakılıyor (reap'e kadar sızıyor değil)
- [x] VMA takibi: `sys_mmap`, `sys_execve` → `process_t->vmas` dolduruluyor; `/proc/[pid]/maps` çalışıyor
- [x] `sys_munmap`: VMA listesini temizliyor
- [x] Thread desteği (`proc_clone_thread`, CLONE_VM): VM refcount ile güvenli paylaşım

### v0.16.5 — SMP (Symmetric Multiprocessing) Temeli
**Hedef:** RPi4'ün 4 çekirdeğini ve QEMU'nun `-smp N` bayrağını kullanmaya başlamak.

- [ ] Atomik primitifler — AArch64 LDXR/STXR ve x86_64 LOCK prefix ile CAS ve fetch-add
- [ ] Lock granülaritesi — `vfs_lock`, `sched_lock`, `buddy_lock` global lock'larını parçala
- [ ] Per-CPU yapılar — `rq_head`, `need_resched`, `current` process'i `cpu_local_t` struct'a taşı
- [ ] İkincil çekirdek uyanışı — AArch64'te PSCI/spin-table, x86_64'te APIC/SIPI

---

## v0.17 Serisi — POSIX Olgunluk & Kullanılabilirlik

### v0.17.0 — Gelişmiş Süreç Yönetimi ✅ TAMAMLANDI
- [x] Thread desteği (`sys_clone` CLONE_VM+CLONE_THREAD, `proc_clone_thread`) — musl pthreads için
- [x] Fork/exec düzeltmeleri: vm_copy kernel identity L1 kopyası, TLS kaydı, GIC timer EOI sıralaması
- [x] Tam POSIX sinyalleri — `SIGTERM`, `SIGSEGV`, `SIGCHLD`, `SIGPIPE` ve `sigaction` semantiği; x86_64 `sa_restorer` + `rt_sigreturn` tam implementasyonu
- [x] `waitpid` semantiği — `WNOHANG`, `WIFEXITED`, `WIFSIGNALED` (v0.16.17)
- [ ] Session ve process group yönetimi — `setsid`, `setpgid`, terminal I/O

### v0.17.1 — Dosya Sistemi Genişletme
- [x] Ext2 yazma desteği — `create`, `mkdir`, `unlink`, `write`; journal yok, fsck ile kurtarılabilir
- [x] Pipe semantiği — `SIGPIPE`, `O_NONBLOCK`, doğru EOF davranışı, çok aşamalı pipeline (`a|b|c|d`) çalışıyor
- [ ] Dosya kilitleme — `flock` / `fcntl` temel implementasyonu
- [ ] `/proc/self/fd/` dizini

### v0.17.2 — Shell & Userspace Modülaritesi
- [ ] Shell built-in'leri bağımsız ELF'e taşı — `userspace/ls`, `userspace/cat`
- [x] Init süreci (PID 1) — `userspace/init.c`: fork+exec döngüsü, shell crash edince respawn
- [x] Linux-standart boot log formatı — `[    T.TTTTTT] mesaj`, tüm `[  tag]` ön ekleri kaldırıldı
- [ ] `dmesg` tarzı kernel ring buffer — `/proc/kmsg`

### v0.17.3 — Donanım & Ağ
- [ ] USB HID klavye desteği — RPi4 xHCI
- [x] Ağ altyapısı iskeleti — `net/` alt sistemi, `AF_INET`, loopback
- [x] Virtio-net sürücüsü — QEMU paket gönderme/alma

---

## Test Kriterleri

| Versiyon | Geçme Kriteri |
|----------|---------------|
| v0.16.2  | `ls /mnt/bin` açılıyor; symlink takibi doğru ✅ |
| v0.16.3  | `run /mnt/bin/busybox sh` Ranix shell'den çalışıyor; herhangi bir Linux AArch64 statik binary doğrudan çalışabilir |
| v0.16.4  | `qemu -smp 4` ile 4 çekirdek boot log'da görünüyor; idle task her çekirdekte koşuyor |
| v0.17.0  | SIGTERM/SIGSEGV doğru iletiliyor; pthreads çalışıyor |
| v0.17.1  | `/mnt`'ye dosya oluşturup kaydedilip okunabiliyor |
| v0.17.2  | `/bin/ls` bağımsız ELF; init crash edince otomatik yeniden başlıyor |
