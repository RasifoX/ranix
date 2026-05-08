# Ranix Roadmap: v0.16 → v0.17

Ranix, Linux'a rakip olarak tasarlanmış monolitik bir işletim sistemi çekirdeği.
Hedef: doğruluk, kararlılık, tüm donanımda çalışma — sonra özellik.

---

## v0.16 Serisi — Temel Altyapı Sağlamlaştırma

### v0.16.1 — Bellek & Zamanlayıcı Düzeltmeleri ✅ TAMAMLANDI
- [x] SLAB/Object Pool allocator (`kernel/mm/heap.c`) — O(N) yerine O(1)
- [x] Dinamik timer frekansı (250 Hz, hardcoded kaldırıldı)
- [x] Ext2 superblock magic doğrulaması (kör mount engellendi)

### v0.16.2 — VFS Kararlılığı & Symlink Desteği 🔧 DEVAM EDİYOR
**Sorun:** `ls /mnt` sonrası kernel crash, symlink'li dizinler açılamıyor.

- [x] `vfs_close`: ext2 dizin inode'larında `fops = NULL` → NULL dereference crash (`kernel/vfs.c`)
- [x] `sys_getdents64`: symlink'ler için `DT_LNK` eksikti, `d_type = 0` dönüyordu
- [x] `vfs_dentry_resolve`: lookup'a tam path yerine tek component geçiriliyordu (`"usr/bin"` → `usr` bulunamıyor)
- [x] VFS symlink takibi — `vfs_dentry_resolve` artık symlink'leri inline takip ediyor (hem göreceli hem mutlak, max 8 seviye)
- [x] `ext2_readlink` — fast symlink (i_block[], <60 byte) ve slow symlink (data block) desteği
- [ ] EL0 fault tolerans: process EL0'da crash edince `kpanic` yerine process sonlandırılmalı (`arch/aarch64/exceptions.c`, `arch/x86_64/exceptions.c`)
- [ ] `vfs_open`: `O_CREAT` ile dizin oluşturma PATH_MAX overflow kontrolü

### v0.16.3 — SMP (Symmetric Multiprocessing) Temeli
**Hedef:** RPi4'ün 4 çekirdeğini ve QEMU'nun `-smp N` bayrağını kullanmaya başlamak.

- [ ] Atomik primitifler — AArch64 LDXR/STXR ve x86_64 LOCK prefix ile CAS ve fetch-add
- [ ] Lock granülaritesi — `vfs_lock`, `sched_lock`, `buddy_lock` global lock'larını parçala (per-inode, per-runqueue)
- [ ] Per-CPU yapılar — `rq_head`, `need_resched`, `current` process'i `cpu_local_t` struct'a taşı; AArch64'te `tpidr_el1`, x86_64'te `GS` register'ı ile erişim
- [ ] İkincil çekirdek uyanışı — AArch64'te PSCI/spin-table, x86_64'te APIC/SIPI mantığı; çekirdekler `idle_loop`'a yönlendirilmeli

### v0.16.4 — Linux ABI Uyumluluğu (BusyBox Hedefi)
**Hedef:** Derlenmemiş musl libc statik BusyBox binary'sini `/mnt/bin/busybox` olarak çalıştırabilmek.

- [ ] Auxiliary Vector (AuxV) — `sys_execve` ve `elf_load`'da stack'e `AT_PHDR`, `AT_PAGESZ`, `AT_RANDOM` vb. yazılmalı; modern libc başlatması için zorunlu
- [ ] File-backed `mmap` — `sys_mmap`'i genişlet, anonim mmap'in yanı sıra dosya segment'larını belleğe map'le (ELF dinamik yükleme için gerekli)
- [ ] `sys_clone` ile thread desteği — `CLONE_VM` flag'ini düzgün destekle; tam fork yerine adres alanı paylaşan thread oluşturma

---

## v0.17 Serisi — POSIX Olgunluk & Kullanılabilirlik

### v0.17.0 — Gelişmiş Süreç Yönetimi
- [ ] Tam POSIX sinyalleri — `SIGTERM`, `SIGSEGV`, `SIGCHLD` ve `sigaction` semantiği; mevcut altyapı var, doğru yönlendirme eksik
- [ ] `waitpid` semantiği — wait flag'leri (`WNOHANG`, `WUNTRACED`) ve status encoding'i (`WIFEXITED`, `WIFSIGNALED`)
- [ ] Session ve process group yönetimi — `setsid`, `setpgid`, terminal I/O ile birlikte
- [ ] `/proc/[pid]/` genişletmesi — `cmdline`, `maps`, `status`, `fd/` dizinleri

### v0.17.1 — Dosya Sistemi Genişletme
- [ ] Ext2 yazma desteği — `create`, `mkdir`, `unlink`, `write` operasyonları; journal yok, fsck ile kurtarılabilir
- [ ] Pipe semantiği düzeltmesi — `SIGPIPE`, `O_NONBLOCK`, doğru EOF davranışı
- [ ] Sembolik bağ oluşturma — `sys_symlink` ve `sys_readlink` syscall'ları
- [ ] Dosya kilit mekanizması — `flock` / `fcntl` temel implementasyonu

### v0.17.2 — Shell & Userspace Modülaritesi
- [ ] Shell built-in'leri bağımsız ELF'e taşı — `userspace/ls`, `userspace/cat` vb.; `sh.c` sadece REPL kalmalı
- [ ] Init süreci (PID 1) yeniden canlandırma — shell crash edince kernel otomatik respawn etmeli
- [ ] Basit `init` daemon — `/etc/inittab` veya hardcoded service listesi ile
- [ ] `dmesg` tarzı kernel log arabelleği — `kprintf` çıktısı ring buffer'a yazılmalı, `/proc/kmsg` veya `dmesg` komutuyla okunabilmeli

### v0.17.3 — Donanım & Sürücü Genişletmesi
- [ ] USB HID klavye desteği — RPi4 üzerinde USB host controller (xHCI) temel implementasyonu
- [ ] Ağ altyapısı iskeleti — `net/` alt sistemi, socket syscall'ları (`AF_INET`, `SOCK_STREAM`), loopback interface
- [ ] Virtio-net sürücüsü — QEMU için paket gönderme/alma, `ping`/`nc` gibi araçlarla test edilebilir

---

## Test Kriterleri

| Versiyon | Geçme Kriteri |
|----------|---------------|
| v0.16.2  | `ls /mnt/bin` açılıyor; symlink takibi doğru; process crash kpanic'e yol açmıyor |
| v0.16.3  | `qemu -smp 4` ile 4 çekirdek boot log'da görünüyor; idle task her çekirdekte koşuyor |
| v0.16.4  | `run /mnt/bin/busybox sh` Ranix shell'den çalışıyor |
| v0.17.0  | SIGTERM/SIGSEGV doğru iletiliyor; `waitpid(WNOHANG)` beklemeden dönüyor |
| v0.17.1  | `/mnt`'ye dosya oluşturup kaydedilip okunabiliyor |
| v0.17.2  | `/bin/ls` bağımsız ELF; init crash edince otomatik yeniden başlıyor |
