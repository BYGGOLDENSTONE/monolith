# Goldenstone Fork — Geliştirme Yol Haritası

**Fork:** BYGGOLDENSTONE/monolith (upstream: tumourlove/monolith)
**Tarih:** 2026-07-23 · **Temel sürüm:** v0.21.2 · **Motor:** UE 5.7 (`D:\UE_5.7`)
**Ev sahibi proje:** `D:\UnrealProjects\MonolithDev` (Plugins\Monolith → `D:\htmlprojects\mcp` junction)

Bu doküman, fork'un kendi ihtiyaçlarımıza göre geliştirme planıdır. Amaç: Monolith'i Claude için mükemmel bir in-engine araca çevirmek — oyun geliştirmeyi kolaylaştırmak.

## Çalışma İlkeleri

- **Sadece C++** — gameplay mantığı asla Blueprint node grafiği olarak üretilmez (BP asset'i oluşturmak serbest, görsel scripting yasak).
- **Data-driven** — sistemler DataAsset/DataTable/config tabanlı tasarlanır, hardcoded değer kullanılmaz.
- **Upstream ile uyum** — orijinal repo aktif; `git fetch upstream && git merge upstream/main` ile güncellemeler alınır. Yeni özellikler yeni modül/dosyalarda tutulur ki merge çakışması minimum olsun.
- **Genişletme şablonu** — yeni namespace = yeni modül; `Source/MonolithConfig/` (en küçük modül, 6 aksiyon) kopyalanıp büyütülür. `Monolith.uplugin` Modules[] kaydı + `MonolithSettings.h`'e `bEnableFoo` toggle + `Registry.RegisterAction()` + `Private/Tests/` altına otomasyon testi.

## Analiz Özeti (2026-07-23 derin inceleme)

**Güçlü:** ~1.400 aksiyon; varlık üretimi çok geniş (blueprint 111, animation 145, mesh ~195, ui 138, ai 221, gas 135, audio 98, niagara 119); görsel doğrulama (capture_* → PNG); async PIE smoke test; canlı PIE property/function/input erişimi; ~1M sembollük motor kaynak indeksi.

**Ana kısıt — tek şeritli sunucu:** Her aksiyon editörün game thread'inde senkron çalışır; uzun iş editörü + sunucuyu dondurur. Proxy'nin 30 sn zaman aşımı, iş sürerken yanıltıcı "editör kapalı" hatası döner (iş arkada devam eder, sonuç çöpe gider). Tek gerçek async kalıp: `FPieSmokeSessionManager` (`Source/MonolithEditor/Private/MonolithPieSmokeSession.h`) — session_id + poll. Genelleştirilecek şablon budur.

**Kapsam boşlukları:** Landscape/Foliage/ışık yerleştirme/PostProcess/World Partition yok; Sequencer salt-okunur (authoring yok); Functional Test/Gauntlet/cooked-build test yolu yok; canlı editör viewport ekran görüntüsü yok (sadece preview-scene/PIE); cooked-build'de 3 runtime sınıfı sessizce devre dışı (`Docs/COOKED_BUILD_TODO.md` — Steam öncesi çözülmeli).

## Çalışma Modeli — Gece Protokolü

İterasyon ağırlıklı işler (crash testi gerektiren her şey) **gece otonom çalışmasıyla** yürür: kullanıcı akşam hedefi tanımlar, orkestratör (Fable, ana oturum) sabaha kadar sıralı Opus işçi ajanlarla iterasyon yapar, sabah kullanıcı kabul testini yapar. Tam kurallar: `Docs/NIGHT_PROTOCOL.md`; başlatma: `/gece` skill'i (`.claude/skills/gece/`). Bağlam taşmasına karşı tüm durum `Docs/nightruns/<tarih>/LOG.md` + commit'lerde tutulur — konuşma bağlamı her an ölebilirmiş gibi çalışılır.

## Fazlar

### Faz 0 — İnsansız Test Düzeneği ← SONRAKİ OTURUM BURADAN BAŞLA

Gece protokolünün ön koşulu. `Scripts/nightrun/` altına: `build.ps1` (UBT sarmalayıcı, yeşil/kırmızı + hata özeti), `launch_editor.ps1` (insansız editör başlatma + port 9316 health check), `smoke.ps1` (HTTP üzerinden standart aksiyon serisi, JSON sonuç), çökme yakalama (exit code + `Saved/Crashes/` + log kuyruğu), `teardown.ps1` (temiz kapatma). Detay: NIGHT_PROTOCOL §6. Gündüz oturumunda kurulur ve elle doğrulanır; ilk gece denemesi kasıtlı KÜÇÜK bir hedefle yapılır (protokolün kendisini test etmek için).

### Faz 1 — İş Yöneticisi (Job System)

Multitasking'in temeli. Plan kullanıcıya sunuldu ve onay bekliyor durumda değil — kullanıcı yönü onayladı, uygulama sonraki oturumda.

1. `FMonolithJobManager` (MonolithCore içine) — `FPieSmokeSessionManager`'ın genelleştirilmiş klonu: Meyers singleton, `TMap<FString, FMonolithJob>`, tek `FTSTicker` pompası, monoton id. Durumlar: running / complete / error / cancelled; ilerleme mesajı + sonuç payload'ı.
2. Yeni `jobs` namespace: `list`, `poll`, `cancel`, `clear`.
3. Bloke eden işlerin dönüşümü — ilk hedef: PoseSearch `build_search_index` (`MonolithPoseSearchActions.cpp:594` — WaitForCompletion ile editörü donduruyor); kaynak reindex'e job id ver.
4. Proxy düzeltmesi — 30 sn aşan işte "editör kapalı" yerine dürüst "iş sürüyor, job_id ile sorgula" cevabı; yinelenen istek tuzağını kapat. Hem C++ proxy (`Tools/MonolithProxy/monolith_proxy.cpp`) hem Python fallback (`Scripts/monolith_proxy.py`, TIMEOUT=30 satır 37) elden geçecek.
5. CPU-bound işler için `FRunnableThread` + `AsyncTask(GameThread)` kalıbı (`MonolithIndexSubsystem` örneği); UObject işleri için FTSTicker dilimleme.

**Test:** `Private/Tests/` otomasyon testleri + canlı editörde uzun iş sırasında paralel komut doğrulaması.

### Faz 2 — Level Tasarımı + Işıklandırma (data-driven)

Kullanıcının birincil ihtiyacı: detaylı level tasarımlarını Claude'a yaptırabilmek.

- Işık aksiyonları: DirectionalLight, PointLight, SpotLight, RectLight, SkyLight, PostProcessVolume, ExponentialHeightFog spawn/ayar; Lumen ayarları okuma/yazma.
- Canlı editör viewport ekran görüntüsü (açık haritanın o anki hali) — mevcut capture_* ailesine ek.
- Data-driven level yerleşim sistemi: level düzenleri DataAsset/DataTable ile tanımlanır (hardcoded değil), Claude bu verileri üretir + uygular + görsel doğrular.
- Ağır işler (ışık bake, büyük yerleşim) Faz 1 job sistemi üstünde koşar.

### Faz 3 — Prosedürel Animasyon Üretimi (IK rig tabanlı)

Kullanıcı fikri (2026-07-23): tek bir T/A-pose referansından IK rig'ler kullanarak mantıklı animasyonlar üretmek — en azından etrafta iş yapan NPC'lerin temel animasyonları (yürüme, eğilme, taşıma, el işleri vb.).

- Mevcut altyapı güçlü bir başlangıç: `animation` namespace'inde IK Rig (`set_ik_rig_bone_settings`), IK Retargeter (retarget pose authoring, `align_retarget_pose`, chain settings), Control Rig anim-graph düğümleri, bone transform okuma (`get_animated_bone_transform`), curve/sync marker yazımı hazır.
- Eksik katman: keyframe düzeyinde animasyon SEQUENCE yazma (bone track'lere key basma), IK hedef çözümlemeyle poz üretme, poz→poz interpolasyonla klip inşası.
- Doğrulama: `capture_anim_frames` ile üretilen animasyonun kare kare görsel kontrolü zaten mevcut.

### Faz 4 — VFX Güçlendirme

Kullanıcı tespiti (2026-07-23): VFX tarafı zayıf, sağlam geliştirilmesi lazım.

- `niagara` namespace'i geniş (119 aksiyon, HLSL yazımı dahil) ama pratik üretim akışı zayıf: hazır efekt tarifleri (recipe/template katmanı), data-driven efekt varyasyon sistemi, materyal + Niagara + ses birleşik efekt paketleri.
- `capture_system_gif` ile görsel doğrulama mevcut — üretim döngüsüne bağlanacak.
- Detay kapsamı Faz 3 sonunda netleştirilecek.

### Faz 5 — Sequencer Yazma + Test Derinleştirme (ileride)

- Sequencer authoring (sekans oluştur, kamera track, keyframe) — fragman/devlog üretimi için.
- Functional Test / cooked-build test yolu; `COOKED_BUILD_TODO.md` runtime boşluğu Steam öncesi.

## Operasyonel Notlar

- Derleme: `D:\UE_5.7\Engine\Build\BatchFiles\Build.bat MonolithDevEditor Win64 Development -project="D:\UnrealProjects\MonolithDev\MonolithDev.uproject" -WaitMutex` — **başka bir UE editörü açıkken çalışmaz** (Live Coding mutex). İlk derleme henüz tamamlanmadı (kullanıcının necropunk1 oturumu açıktı).
- Kullanıcı kuralı: aynı anda en fazla 6 subagent.
- Dokümantasyon (API_REFERENCE, spec'ler, Skills tabloları) upstream'de elle güncelleniyor — kendi eklediklerimizde de aynı disipline uy.
