# Ranix Temizlik & Geliştirme Roadmap

## Tamamlananlar

### Layer violations
- [x] `sched.c` → `fb_tty.h` + `tty_core.h` kaldırıldı
- [x] `include/kernel/tick.h` + `tick_register/tick_notify` sistemi eklendi
- [x] `fb_tty_init` → `tick_register(fb_tty_tick)` kaydı
- [x] `tty_core_init` → `tick_register(tty_poll_inputs)` kaydı
- [x] `fb_tty.c` → `uart.h` kaldırıldı, `uart_esc` + `fb_tty_cursor_flush` silindi
- [x] `fb_tty_tick` + `fb_tty_cursor_flush` public header'dan kaldırıldı
- [x] `pit.c` + `arm_timer.c` → `sched.h` → `proc.h`, sleep-wake loop `sched_tick`'e taşındı
- [x] `pipe.c` → `sched.h` → `proc.h`
- [x] `input_core.c` → `tty_core.h` kaldırıldı, inline `tty_poll_inputs()` çağrıları silindi

### Dinamik yapılar
- [x] `driver_t` → linked list (`next` pointer, `DRIVER_MAX` kaldırıldı)
- [x] VFS fstype tablosu → `kmalloc` + linked list (limit yok)
- [x] VFS mount tablosu → `kmalloc` + linked list (`MOUNT_MAX` kaldırıldı)

---

## Yapılacaklar

### Dinamikleştirme
- [ ] `proctab[256]` → dinamik yapı (dikkat: `exceptions_asm.S` ctx offset'i hardcode)
- [ ] `fd_table_t` → `files[64]` sabit array → dinamik büyüyen tablo
- [ ] `tty_core.c` → tek TTY → `tty_t` struct, `/dev/tty0` `/dev/tty1` vs çalışsın
- [ ] `input_core.c` → tek `event_buf[256]` → input device kayıt sistemi, çok device
- [ ] `heap.c` → 7 sabit slab class → runtime eklenebilir
- [ ] `ioremap_free()` ekle (şu an sadece alloc var)
- [ ] `MAX_STACK_SIZE` process başına ayarlanabilir olsun

### IPC
- [ ] `mmap(MAP_SHARED)` → shared memory
- [ ] eventfd / futex → userspace sync primitive
- [x] Tam signal delivery: `SA_SIGACTION` + `sa_restorer`, x86_64 48-byte frame, `rt_sigreturn` doğru `orig_rax`/`rip`/`rsp` restore, `SIGCHLD` + `SIGPIPE` aktif
- [x] Wait queue döngü hatası: `sleeping_on_queue` flag, `sched_tick` yalnızca poll-uyku task'larını uyandırıyor
- [ ] `sigaltstack`, `sigprocmask` tam implementasyon

### Scheduler
- [ ] Process priority / nice değeri
- [ ] Scheduler policy (SCHED_FIFO, SCHED_RR, SCHED_NORMAL)
- [ ] Per-process quantum

### VFS & Proc
- [ ] `/proc/self/fd`, `/proc/meminfo`, `/proc/cpuinfo`, `/proc/self/maps`
- [ ] symlink desteği
- [ ] `dup2` + `fcntl(F_DUPFD, F_GETFL, F_SETFL)`

### Device model
- [ ] `/dev/fb0` düzgün `mmap` ile userspace erişim
- [ ] Birden fazla FB: `/dev/fb0`, `/dev/fb1`
- [ ] `/dev/null`, `/dev/zero`, `/dev/urandom`

### Bellek yönetimi
- [ ] vmalloc (şu an yok, sadece ioremap var)
- [ ] Copy-on-write fork (`MAP_PRIVATE` sayfalar)
- [ ] `mprotect` syscall

---

## Notlar
- `exceptions_asm.S` → `process_t.ctx` offset 16 hardcode. struct başındaki ilk 8 alanı değiştirme.
- Her değişiklikten sonra hem x86_64 hem aarch64 build edilmeli.
- Doom: `make ARCH=x86_64 qemu-doom` → UART log `serial.log`'a gider.
