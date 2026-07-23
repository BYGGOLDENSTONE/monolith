# Sabah Raporu — 2026-07-23-prova (Kuru Gece Provası)

## Sonuç
**Prova başarılı: mekanik uçtan uca çalıştı.** 1/1 gerçek görev tamamlandı+doğrulandı; 1/1 kasıtlı başarısızlık tatbikatı planlandığı gibi blocked → alternatif → park yolunu izledi. Süre: ~5 dakika (20:53–20:58). Toplam 6 ajan çağrısı (3 Opus işçi + 3 haiku kontrol), hepsi sıralı — eşzamanlılık hiç 1'i aşmadı.

## Yapılanlar
1. **Dal + ilk commit'ler:** `night/2026-07-23-prova` master'dan açıldı; GOAL.md (cd7e164), kurulum düzenlemeleri (9769f0e), PROMPT.md (eb15059) push'landı.
2. **Görev 1 (başarı yolu):** Opus işçi `worker_artifact.md` üretti (CHANGELOG 0.21.2, 19 spec dosyası, 20 Source/ modülü) ve commit'ledi (5014f5c). Haiku kontrol ajanı bağımsız doğruladı: **verified** — commit tek dosya, ağaç temiz, veriler doğru.
3. **Görev 2 (tatbikat):** İlk Opus işçi görevi çözümsüz tespit edip temiz ağaçla **blocked** döndü. Haiku teyit etti. §5 gereği ALTERNATİF Opus işçi gönderildi; 4 farklı açıyı ampirik denedi (git geçmişi restore, symlink, Python interpreter hook'ları, shell alias) — hepsi yasağa indirgendi, blocked. Haiku tekrar teyit etti. Görev **parked**, gerekçe LOG'da. Park sayısı 1'de kaldı; görev listesi bittiği için gece kapandı.
4. Her adımda LOG-commit-push zinciri işledi (eeac7ad → d44c72b → abd981f → c298df0). Push edilmemiş ilerleme kalmadı.

## Zamanlama gözlemi (GERÇEK GECELER İÇİN KRİTİK)
- **Agent çağrıları SENKRON DEĞİL — asenkron/arka plan.** Her Agent çağrısı anında "launched" dönüyor; sonuç, işçi bitince gelen otomatik task-notification ile orkestratörü uyandırıyor. Orkestratör turn'ü işçi çalışırken sona eriyor; bekleme/blocking yok.
- **Bildirimler güvenilir çalıştı:** 6/6 ajan tamamlanması bildirimle geldi (süreler 16–37 sn; gerçek gecede derlemeli işçiler dakikalar-onlarca dakika sürecek, mekanizma aynı).
- **ScheduleWakeup KULLANILABİLİR ve kuruldu:** İşçi 1 gönderilirken 1800 sn'lik yedek uyanma kuruldu (21:25). Bildirimler hep önce geldiği için yedek hiç tetiklenmedi; prova sonunda iptal edildi.
- **§1'e işlenecek netleştirme:** Birincil uyanma sinyali = task-notification (otomatik). ScheduleWakeup = uzun yedek (~1800 sn), işçi gönderiminde kurulur; bildirim kaybolursa/işçi asılı kalırsa orkestratör yine uyanır. Bu düzen gerçek gece için yeterli — ek polling gerekmez.

## Kontrol ajanı performansı
Haiku 3/3 doğru hüküm verdi (2 verified + 1 verified-clean-tree), ~17–23 sn, ≤10 satır formatına uydu. Sonnet'e yükseltme gerekmedi.

## Park edilenler
- **Görev 2** — tasarım gereği çözümsüz (tatbikat görevi). Gerçek bir iş kaybı yok; park mekanizmasının kanıtıdır.

## İnsan testi gerekenler
- Prova dalını (`night/2026-07-23-prova`) GitHub'da incele: commit zinciri, LOG akışı, worker_artifact.md içeriği beklentine uyuyor mu?
- Kabul sonrası dal MERGE EDİLMEZ — incelenip silinir (kayıt bu klasörde yaşamaya devam eder; protokol §8).

## Protokole önerilen güncellemeler (gündüz oturumunda işlenecek)
1. §1 bekleme mekanizması maddesini yukarıdaki zamanlama gözlemiyle netleştir (asenkron + bildirim birincil + 1800 sn yedek).
2. İşçi promptuna "commit hash'ini TAM (40 karakter) raporla" eklenebilir — kontrol ajanı kısa hash'le de çalıştı ama tam hash teyidi kolaylaştırıyor.
