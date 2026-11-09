# Ranix kod stili

Amaç: kodu herkes okuyup anlayabilsin. Stil Rasi'nin ilk kodundan gelir, kurallar `kod-kalitesi-denetimi.md` §5'ten. Birleştirmeden önce `tools/kapi.sh` çalışır ve tam takım %100 geçer.

## Korunan stil
- **Adlar kısa ve düz.** `kout`, `pb_u64`, `vm_space_t`. Kısaltma gerekiyorsa dosya içinde hep aynı kısaltmayı kullan.
- **Yorum "neden"i anlatır.** Kodun ne yaptığı koddan okunur, yorum gerekçeyi söyler. Her dosya, ne yaptığını düz dille anlatan bir yorumla başlar.
- **Girinti:** dosyanın mevcut girintisine uy (çoğu dosyada tab). Fonksiyonun `{` işareti kendi satırında durur.
- **Sistem metni İngilizcedir:** yorum, log ve hata metni.

## Kurallar
1. **Önce ara, sonra yaz.** Yeni yardımcı yazmadan önce `kernel/lib` ve `include/` altına bak.
2. **`.c` içinde `extern` olmaz.** Bildirim, tanımın ait olduğu başlığa gider. Tanımlayan dosya da o başlığı içerir, böylece imza değişirse derleyici uyarır. Zaman `kernel/time.h`, açılış giriş noktaları `kernel/init.h`, hata kodları `kernel/errno.h`, SMP `kernel/smp.h`.
3. **Sabitler adlıdır.** Hata `return -EINVAL;` diye döner, `return -22;` yazılmaz. `HZ`, `RLIMIT_*`, `SIG*` ya da errno yerel `#define` ile yeniden tanımlanmaz.
4. **Fonksiyon en fazla 120 satır olur, en fazla 5 seviye iç içe girer.** 120 satırı geçen bir fonksiyona dal eklenmez. Önce davranışı değiştirmeden bölünür, ayrı commit yapılır.
5. **Bir dosya tek iş yapar.** ~1500 satırı geçen dosya bölünür. Sürücüye politika konmaz: kısayol, ses ve klavye düzeni Fjord'un işidir.
6. **Hata yolunda `goto out` kullanılır.** Temizlik makrosu kullanılmaz. Panik yerine hata döner.
7. **Genel kodda `#if __x86_64__` olmaz.** Bunun yerine bir `arch_*()` fonksiyonu eklenir.
8. **Her kapı kapalı başlar.** Aygıt 0600 root ya da 0660 grup olur, asla herkese yazılabilir olmaz. Kullanıcının verdiği pid, uid ya da yol bir yetki denetiminden geçer. Her yeni kapının bir negatif testi olur.
9. **Test kancası üretime girmez.** Yeni bir `ranix.*` cmdline anahtarı onay ister.
10. **Çekirdekte "agent", "ajan" ya da "AI" adı geçmez.** Bunlar yerine session, lease, holder, injected gibi adlar kullanılır.
11. **Yorum olay günlüğü değildir.** "Casper'da şu gün görüldü" bilgisi commit mesajına yazılır. Yorum kodla çelişirse biri düzeltilir.
12. **Her davranış değişikliğinin testi olur.** Test `userspace/test_<ad>.c` dosyasıdır ve `tests/` altındaki bir betikte çalıştırılır.
