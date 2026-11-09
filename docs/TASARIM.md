# Ranix katmanları: kim kimi çağırabilir

Bir alt sistem yalnız tabloda kendi satırında izin verilen katmanları çağırır. Çağrı, o katmanın `include/` altındaki başlığı üzerinden yapılır, `.c` içine `extern` yazılmaz. Aşağıdan yukarıya çağrı gerekiyorsa (örneğin zamanlayıcı kesmesinden TCP zamanlayıcısına), katman bir kanca kaydı sunar: `tick_register()`, `vfs_register_fs()` gibi.

| Katman | Yer | Çağırabilir | Çağıramaz |
|---|---|---|---|
| 1. arch, board | `arch/*`, `arch/*/mach-*` | lib, kendi `asm/` başlıkları; board kendi platform sürücülerini başlatır (`kernel/init.h`) | mm, sched ve üstü (yalnız kancalarla) |
| 2. lib | `kernel/lib` | hiçbir alt sistemi | hepsini |
| 3. mm | `kernel/mm`, `arch/*/vm.c` | arch, lib, sync | vfs, net, sürücü |
| 4. sched, proc, signal, sync, time | `kernel/sched.c`, `proc.c`, `signal.c`, `sync.c`, `kernel/time.h` | arch, lib, mm | vfs içi, fs/*, net içi, sürücü |
| 5. vfs, net çekirdeği, block | `kernel/vfs.c`, `kernel/net/*`, `kernel/block.c` | 1-4 | fs/* içi, sürücü içi |
| 6. dosya sistemleri, sürücüler | `kernel/fs/*`, `drivers/*` | 1-5 | başka bir sürücü ya da dosya sistemi |
| 7. syscall | `kernel/syscall.c`, `kernel/sys/*` | 1-6, yalnız başlıklar üzerinden | — |
| 8. procfs, sysfs | `kernel/fs/procfs.c`, `kernel/fs/sysfs.c` | herkesin `*_report(kout_t *)` fonksiyonunu | veri üretmek, başka alt sistemin iç yapısını gezmek |

Kurallar:
- **Sürücü başka sürücüyü çağırmaz.** Ortak iş için bir çekirdek katmanı yazılır (giriş için `input_*`, blok için `block_*`).
- **Çekirdek katmanı belirli bir sürücüyü tanımaz.** DRM, Intel'i bir `drm_driver_t` işlemi üzerinden çağırır.
- **procfs ve sysfs yalnız okur.** Bir değer gösterilecekse sahibi `*_report()` ya da `*_get()` sunar. Yazılabilir düğüm bir kapıdır ve kurallara uyar (STIL.md, kural 8).

Bugün bilinen istisnalar (taşınacak):
- USB HID, tuşları PS/2 sürücüsüne veriyor: `input_usb_key` (`drivers/input/ps2.h`). Hedef `input_report_key()`.
- `kernel/fs/sysfs.c` ve `procfs.c` sürücü değişkenlerine zayıf referansla erişiyor: `syn_tap`, `ps2_aux_report`, `wdog_deadline_*`.
- Çekirdek, konsol için `drivers/serial/uart.h` dosyasını doğrudan içeriyor (`kprintf.c`, `tty_core.c`, `devfs.c`, `syscall.c`).
- `drivers/gpu/drm` genel KMS kodu Intel'i doğrudan çağırıyor (denetim #11).
- `kernel/hungtask.c` başka CPU'ların çalıştırma kuyruklarını geziyor (denetim #13).
