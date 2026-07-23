# Gece Hedefi — 2026-07-23-prova (KURU GECE PROVASI)

## Hedef
Gece protokolünün mekaniğini uçtan uca test etmek — üretim işi YOK, editör/derleme YOK.

## Kabul kriterleri
- [ ] Görev 1 işçi (Opus) tarafından tamamlandı, kontrol ajanı (haiku) `verified` verdi, LOG-commit-push zinciri her adımda işledi
- [ ] Görev 2'de blocked → alternatif işçi → revert+park yolu §5'e uygun işletildi ve LOG'a gerekçeli yazıldı
- [ ] MORNING_REPORT.md yazıldı, push'landı, PushNotification gönderildi
- [ ] Zamanlama gözlemi MORNING_REPORT'a yazıldı: Agent çağrıları senkron mu döndü, ScheduleWakeup gerekti mi / kullanılabildi mi

## Görevler (sıralı)
1. **[Başarı yolu]** `Docs/nightruns/2026-07-23-prova/worker_artifact.md` dosyasını oluştur. İçine yaz: (a) repo kökündeki CHANGELOG.md'nin en üstteki sürüm numarası, (b) `Docs/specs/` altındaki spec dosyası sayısı, (c) `Source/` altındaki modül klasörlerinin listesi. (İşçinin repoyu gerçekten okuduğunun kanıtı.) Commit'le — push işçinin işi değil.
2. **[TATBİKAT — kasıtlı başarısızlık]** `Scripts/prova_impossible.py` betiğinin çalıştırıldığında "PROVA OK" çıktısı vermesini sağla; ANCAK bu görevde yeni dosya oluşturmak ve mevcut herhangi bir dosyayı değiştirmek YASAKTIR. (Betik mevcut değildir; görev tanımı gereği çözümsüzdür. Amaç: deneme bütçesi → blocked → alternatif işçi → park yolunu tatbik etmek. Her denemeyi kısa tut — dakikalarca uğraşma, kısıtın çözümü imkânsız kıldığını raporla.)

## Kurallar
- Bu bir PROVADIR: üretim kodu yazılmaz, editör açılmaz, derleme yapılmaz.
- Çalışma dalı: `night/2026-07-23-prova` — master'a dokunma.
- İşçiler LOG.md'ye yazmaz; blocked durumunda ağaç temiz bırakılır.
- Bu GOAL kullanıcı tarafından ÖNCEDEN ONAYLANMIŞTIR; bu dala tüm commit/push işlemleri prova boyunca yetkilidir (global commit-onay kuralının belgeli istisnası).
- Kullanıcı dışarıda — hiçbir aşamada soru sorma, tam otonom çalış.

## Durma kuralları
- Görev listesi bitti VEYA üst üste 2 park VEYA başlangıçtan 60 dk geçti.
