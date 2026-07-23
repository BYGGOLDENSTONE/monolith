# Gece Hedefi — 2026-07-24

## Hedef
Isınma gecesi: 4 önceden var olan kırmızı testi yeşile çevir ve Faz 1 iş yöneticisinin (Job System) iskeletini kur.

## Kabul kriterleri
- [ ] `Scripts/nightrun/run_tests.ps1` sonucu 177/177 yeşil (taban 173/177).
- [ ] `FMonolithJobManager` MonolithCore içinde derleniyor; durum makinesi (running/complete/error/cancelled) otomasyon testiyle kaplı.
- [ ] `jobs` namespace'i discover'da görünüyor; `list`/`poll`/`cancel`/`clear` aksiyonları HTTP smoke'ta yanıt veriyor.
- [ ] Her görev sonunda derleme yeşil, ağaç temiz, commit atılmış.

## Görevler (sıralı, her biri tek işçiye verilebilir boyutta)

1. **`MonolithUI.Reflection.ParseLinearColorHex` testini düzelt** — hex renk ayrıştırma davranış hatası. İşaretçi: `Source/MonolithUI/Private/Tests/` altındaki ilgili test + ayrıştırmayı yapan reflection yardımcı fonksiyonu. Testin beklentisi mi kod mu haklı, önce onu belirle; kararı rapora yaz.

2. **`MonolithUI.Allowlist.UnknownTypeDenied` testini düzelt** — bilinmeyen tip allowlist tarafından reddedilmeli ama edilmiyor (ya da hata biçimi beklenenden farklı). İşaretçi: MonolithUI allowlist/tip doğrulama katmanı.

3. **`Monolith.CursorPagination.QueryMismatchRejection` testini düzelt** — reddetme hatası `ErrorData` alanını taşımıyor. Hata yükünü standart biçime tamamla; başka çağıranları bozmadığından emin ol.

4. **`Monolith.ReflectionIntel.Decision.HeuristicAccuracy` testini düzelt** — indexer, karar-olmayan fixture'dan 2 satır yutuyor (heuristik fazla geniş). Heuristiği daralt; diğer ReflectionIntel testleri yeşil kalmalı.

5. **`FMonolithJobManager` çekirdeği** — `Source/MonolithCore/` içine yeni dosyalar (`MonolithJobManager.h/.cpp`). `FPieSmokeSessionManager` (`Source/MonolithEditor/Private/MonolithPieSmokeSession.h`) genelleştirilmiş klonu: Meyers singleton, `TMap<FString, FMonolithJob>`, tek `FTSTicker` pompası, monoton id, durumlar running/complete/error/cancelled, ilerleme mesajı + sonuç payload'ı. Aksiyon bağlama YOK — sadece çekirdek + `Private/Tests/` altına durum makinesi testi (job oluştur → poll → complete/error/cancel yolları).

6. **`jobs` namespace'i** — `list`, `poll`, `cancel`, `clear` aksiyonları. Genişletme şablonu: `Source/MonolithConfig/` desenini izle (Modules[] kaydı gerekiyorsa `Monolith.uplugin`, `MonolithSettings.h`'e toggle, `Registry.RegisterAction()`, `Private/Tests/` altına aksiyon testi). Görev 5'in çekirdeğini kullanır; gerçek uzun iş bağlanmaz (o Faz 1'in sonraki adımı).

**Stretch (5 ve 6 erken biterse):** Proxy dürüstlük düzeltmesi — 30 sn aşan işte "editör kapalı" yerine "iş sürüyor, job_id ile sorgula". Dosyalar: `Tools/MonolithProxy/monolith_proxy.cpp` ve `Scripts/monolith_proxy.py` (TIMEOUT=30, satır ~37). Yinelenen istek tuzağını da kapat.

## Kurallar
- Sadece C++, data-driven, hardcoded değer yok. Gameplay mantığı asla BP node grafiği değil.
- Çalışma dalı: `night/2026-07-24` — `master`'a dokunma, force-push yok.
- Bu GOAL onaylandığında, bu dala gece boyu tüm commit/push işlemleri önceden yetkilendirilmiş sayılır (global commit-onay kuralının belgeli istisnası).
- Derleme her zaman `Scripts/nightrun/build.ps1` ile ve arka planda (`run_in_background`).
- Test her zaman `Scripts/nightrun/run_tests.ps1` ile; her işçi bitirmeden `Scripts/nightrun/teardown.ps1` çalıştırır.
- Deneme bütçesi görev başına 3; tükenirse ağacı TEMİZ bırakıp `blocked` raporla.
- `Docs/nightruns/2026-07-24/LOG.md` dosyasına işçi asla yazmaz.
- 1-4 numaralı testler birbirinden bağımsız; bir tanesi park edilirse diğerleri denenmeye devam eder.
- Görev 6, görev 5 yeşil olmadan başlatılmaz.

## Devam yetkisi (kullanıcı onayı, 2026-07-24 akşamı)
Görev 1-6 + stretch biterse gece DURMAZ: orkestratör `Docs/GOLDENSTONE_ROADMAP.md`'deki sıradaki
maddelerle devam eder (Faz 1'in kalanı: PoseSearch `build_search_index`'in job'a çevrilmesi →
CPU-bound `FRunnableThread` + `AsyncTask(GameThread)` kalıbı → ardından Faz 2 ışık aksiyonları).
Yeni görev seçimi için kullanıcı onayı ARANMAZ; seçilen her ek görev LOG'a gerekçesiyle yazılır.
Aynı boyutlandırma ve kurallar geçerlidir.

## Durma kuralları
- Üst üste 2 görev park edildi VEYA sabah 08:00. (Görev listesinin bitmesi artık durma sebebi değil —
  bkz. Devam yetkisi.)
