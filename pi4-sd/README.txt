RANIX — Raspberry Pi 4 boot  (SMP yarış avı + ilk gerçek benchmark)
================================================================

1) microSD (FAT32) köküne KOPYALA
---------------------------------
Bu klasörden:
  kernel8.img        (Ranix — bu derleme: 191 test + race dedektörü + /bin/bench)
  initramfs.cpio     (test initramfs; sonda bench de koşar)
  config.txt
Zaten kartında olması gereken Pi firmware (yoksa github.com/raspberrypi/firmware/boot):
  start4.elf
  fixup4.dat
  bcm2711-rpi-4-b.dtb

2) BOOT
-------
Tak, aç. HDMI'da (ve istersen GPIO14/15 UART @115200, enable_uart=1 açik):
  boot logu -> 191 test -> bench -> sistem KAPANIR (tek kosu).
  Tekrar kosmak icin Pi'yi resetle/yeniden ac. (Yaris KESINTILI -> 3-5 kez kos.)

3) NE IZLE / NE YAZ
-------------------
A) YARIS AVI — su satirlardan biri cikarsa AYNEN yaz:
   * "SMP-BUG: double-run pid=.. this=cpuA last_cpu=B"
        -> KESIN KANIT: bir process iki cekirdekte. Kok bulundu; fix = runqueue'ya
           cift-enqueue guard'i (on_rq).
   * "segfault: pid=.. (...) ... PTwalk ... L2[..]=.."
        -> register-copu tekrar oldu; double-run satiri YOKSA -> double-run degil,
           baska context-switch yarisi. PTwalk/L1/L2/L3 satirlarini yaz.
   * "Results: 191 passed" + "ALL_PASS" ve hic segfault YOK
        -> o kosu temiz. Ust uste birkac temiz kosu -> yaris nadirlesmis/cozulmus.

B) BENCHMARK — sonda "BENCH:" satirlari (gercek donanim, ilk Linux-kiyas zemini):
   timer_hz, syscall-getpid, sched-yield, fork+wait, fork+exec+wait,
   ctxsw, pipe-bw, mmap+fault+unmap.  HEPSINI yaz — Linux'la kiyaslayacagiz.
   Beklenti: sched-yield ve fork+wait ~10ms cikabilir = bilinen yield/idle-tick
   latency acigi (scheduler fix'i bekliyor, doktorun notunda).

4) Sadece bench / interaktif shell istersen
-------------------------------------------
Su an init test.sh'i kosup kapatiyor (interaktif shell yok). Boot-sonrasi shell
istersen soyle, init'e ekleyeyim (o zaman elle /bin/bench, ps, vs. kosabilirsin).
