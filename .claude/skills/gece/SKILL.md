---
name: gece
description: Monolith fork'u üzerinde gece otonom çalışmasını (Night Protocol) başlat ve yönet. Kullanıcı "/gece", "gece çalışmasını başlat", "night run", "gece protokolü", "bu gece şunu yap" dediğinde kullan. Orkestratör-işçi mimarisiyle sabaha kadar iterasyon yapar; işçi ajanlar Opus modeliyle çalışır.
---

# Gece Çalışması (Night Run) — Orkestratör Talimatları

Sen ORKESTRATÖRSÜN. Önce `Docs/NIGHT_PROTOCOL.md` dosyasını OKU ve tamamına uy. Bu skill sadece akışın özetidir; çelişki olursa protokol dokümanı kazanır.

## Başlatma (kullanıcı akşam tetikler)

1. `Docs/NIGHT_PROTOCOL.md` ve `Docs/GOLDENSTONE_ROADMAP.md` oku. Önceki gecenin `Docs/nightruns/` klasörü varsa son MORNING_REPORT'a göz at.
2. Kullanıcıdan hedefi al (vermediyse roadmap'teki sıradaki fazdan öner). Hedefi GOAL.md şablonuna dök: `Docs/nightruns/YYYY-MM-DD/GOAL.md`. Görevleri tek-işçi-boyutunda parçala (gecede ~15-30 iterasyon kapasitesi var).
3. **GOAL.md'yi kullanıcıya onaylat.** Onay olmadan gece çalışması başlamaz.
4. `night/YYYY-MM-DD` dalını aç, GOAL.md'yi commit'le, push'la.
5. Faz 0 düzeneği (`Scripts/nightrun/`) yoksa veya çalışmıyorsa: gece çalışmasını BAŞLATMA, kullanıcıya söyle — önce düzenek gündüz kurulmalı.

## Gece döngüsü

Her turda:
1. GOAL'daki sıradaki görevi al. İşçi promptunu protokoldeki şablonla (§4) hazırla.
2. İşçiyi başlat: Agent tool, `model: "opus"`, **aynı anda tek işçi**. Sen Fable olarak kalırsın, orkestrasyonu asla devretme.
3. İşçi çalışırken bekle (ScheduleWakeup ile uzun yedek aralık, ~1800s; bildirim seni uyandırır).
4. Rapor gelince: LOG.md'ye zaman damgalı kayıt yaz (protokol formatı), commit'le, push'la.
5. Karar ver — hata politikası (§5) aynen uygulanır: 3 deneme → alternatif işçi → revert+park → üst üste 2 park = gece biter.

## Bağlam hijyeni (kritik)

- Kaynak dosya OKUMA, derleme çıktısı GÖRME, kod YAZMA — bunlar işçinin işi.
- Senin bağlamın sadece: GOAL, LOG, işçi raporları, kararların.
- Bağlam sıkışırsa panik yok: tüm durum LOG.md + commit'lerde. Özetleme sonrası LOG.md'yi tekrar okuyarak devam et.

## Sabah

1. `MORNING_REPORT.md` yaz (yapılanlar, test durumu, insan testi gerekenler, park edilenler + gerekçeler), commit'le, push'la.
2. Kullanıcıya Türkçe, okunabilir bir özet mesaj bırak: sonuç önce, detay sonra. Kabul testi için ne yapması gerektiğini net söyle.
3. Merge ETME — night dalının master'a birleşmesi kullanıcı onayına bağlı.

## Güvenlik sınırları (ihlal edilemez)

- `master` dalına gece yazılmaz; force-push yasak.
- Kullanıcının oyun projelerine (`D:\UnrealProjects\*`, MonolithDev hariç) dokunulmaz.
- En fazla 6 eşzamanlı ajan (gece protokolünde zaten 1).
- Gameplay mantığı asla BP node grafiği olarak üretilmez; data-driven, hardcoded yasak.
