---
name: gece
description: Monolith fork'u üzerinde gece otonom çalışmasını (Night Protocol) başlat ve yönet. Kullanıcı "/gece", "gece çalışmasını başlat", "night run", "gece protokolü", "bu gece şunu yap" dediğinde kullan. Orkestratör-işçi mimarisiyle sabaha kadar iterasyon yapar; işçi ajanlar Opus modeliyle çalışır.
---

# Gece Çalışması (Night Run) — Orkestratör Talimatları

Sen ORKESTRATÖRSÜN. Önce `Docs/NIGHT_PROTOCOL.md` dosyasını OKU ve tamamına uy. Bu skill sadece akışın özetidir; çelişki olursa protokol dokümanı kazanır.

## Devam kontrolü (her çağrıda İLK adım)

İlgili gecenin `Docs/nightruns/<tarih>/` klasörü VAR ve içinde `MORNING_REPORT.md` YOKSA → yarım kalmış gece var demektir. Sıfırdan BAŞLAMA: night dalına geç, LOG.md'nin son kaydını oku, LOG'a "resume" kaydı düş, döngüye kaldığı yerden gir.

## Başlatma (kullanıcı akşam tetikler)

1. `Docs/NIGHT_PROTOCOL.md` ve `Docs/GOLDENSTONE_ROADMAP.md` oku. Önceki gecenin `Docs/nightruns/` klasörü varsa son MORNING_REPORT'a göz at.
2. Kullanıcıdan hedefi al (vermediyse roadmap'teki sıradaki fazdan öner). Hedefi GOAL.md şablonuna dök: `Docs/nightruns/YYYY-MM-DD/GOAL.md`. Görevleri tek-işçi-boyutunda parçala (gecede ~15-30 iterasyon kapasitesi var).
3. **GOAL.md'yi kullanıcıya onaylat.** Onay olmadan gece çalışması başlamaz. Onaylanan GOAL, o dala gece boyu commit/push yetkisi demektir (global kuralın belgeli istisnası) — gece yarısı commit onayı sorma.
4. `night/YYYY-MM-DD` dalını aç, GOAL.md'yi commit'le, push'la.
5. Faz 0 düzeneği (`Scripts/nightrun/`) yoksa veya çalışmıyorsa: gece çalışmasını BAŞLATMA, kullanıcıya söyle — önce düzenek gündüz kurulmalı. (İstisna: prova modu, aşağıda.)

## Gece döngüsü

Her turda:
1. GOAL'daki sıradaki görevi al. İşçi promptunu protokoldeki şablonla (§4) hazırla.
2. İşçiyi başlat: Agent tool, `model: "opus"`, **aynı anda tek işçi**. Sen Fable olarak kalırsın, orkestrasyonu asla devretme.
3. İşçi çağrısı senkron dönüyorsa bekleme gerekmez; arka planda çalışıyorsa ScheduleWakeup ile uzun yedek aralık (~1800s) kur, bildirim seni uyandırır. (Hangisi geçerli — prova sonucuna bak, MORNING_REPORT'ta yazar.)
4. Rapor gelince: **kontrol ajanı başlat** (Agent tool, `model: "haiku"`) — commit gerçekten var mı (`git log`), ağaç temiz mi (`git status`), test/smoke JSON sonuçları raporla uyuşuyor mu. Hüküm: `verified` / `mismatch` (+ ne). Doğrulama okumalarını ASLA kendin yapma.
5. LOG.md'ye zaman damgalı kayıt yaz (protokol formatı; işçinin `notes_for_next_worker` notunu AYNEN aktar), commit'le, push'la.
6. Karar ver — hata politikası (§5) aynen: 3 deneme → alternatif işçi (kirli ağaçta ilk adımı temizlik) → revert+park → üst üste 2 park = gece biter. `mismatch` = görev geçmedi. Agent null/kota hatası = altyapı sorunu, park sayılmaz: bir kez yeniden dene, yine olursa geceyi zarifçe bitir (rapor + bildirim).

## Bağlam hijyeni (kritik)

- Kaynak dosya OKUMA, derleme çıktısı GÖRME, kod YAZMA — bunlar işçinin işi.
- Küçük doğrulama okumaları (git teyidi, test JSON) bile senin işin değil — kontrol ajanına yaptır. Sabaha kadar onlarca tur var; her küçük okuma bağlamını şişirir.
- Senin bağlamın sadece: GOAL, LOG, işçi raporları, kontrol hükümleri, kararların.
- Bağlam sıkışırsa panik yok: tüm durum LOG.md + commit'lerde. Özetleme sonrası LOG.md'yi tekrar okuyarak devam et.

## Sabah

1. `MORNING_REPORT.md` yaz (yapılanlar, test durumu, insan testi gerekenler, park edilenler + gerekçeler), commit'le, push'la.
2. **PushNotification gönder** (ToolSearch ile `select:PushNotification` yükleyip çağır): tek cümlelik sonuç — "gece bitti: X/Y görev tamam" veya "gece erken durdu: <neden>".
3. Kullanıcıya Türkçe, okunabilir bir özet mesaj bırak: sonuç önce, detay sonra. Kabul testi için ne yapması gerektiğini net söyle.
4. Merge ETME — night dalının master'a birleşmesi kullanıcı onayına bağlı.

## Prova modu (`/gece prova`)

Protokol §8: gerçek geceden önce mekaniğin gündüz tatbikatı. Görevler önemsizdir, editör/derleme GEREKTİRMEZ — Faz 0 şartı aranmaz. Akış aynen işler: dal (`night/<tarih>-prova`), GOAL ilk commit, işçi → kontrol ajanı → LOG-commit-push, kasıtlı başarısızlık göreviyle blocked→alternatif→park tatbikatı, MORNING_REPORT + PushNotification. Zamanlama gözlemini (Agent senkron mu, ScheduleWakeup gerekti mi/kullanılabildi mi) MORNING_REPORT'a MUTLAKA yaz — gerçek gecelerin bekleme talimatı buna göre netleşecek. Prova dalı merge edilmez.

## Güvenlik sınırları (ihlal edilemez)

- `master` dalına gece yazılmaz; force-push yasak.
- Kullanıcının oyun projelerine (`D:\UnrealProjects\*`, MonolithDev hariç) dokunulmaz.
- En fazla 6 eşzamanlı ajan (gece protokolünde zaten 1).
- Gameplay mantığı asla BP node grafiği olarak üretilmez; data-driven, hardcoded yasak.
