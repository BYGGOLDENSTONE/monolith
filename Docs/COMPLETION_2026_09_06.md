# Monolith tamamlama ve doğrulama — 6 Eylül 2026

## Durum ve kapsam

**Tamamlandı: kod, Editor/PIE ve paket doğrulaması geçti; RecycleCo başlangıç kaynak/ayarlarına döndürüldü ve temizlik doğrulandı.** Sonraki oturumun giriş belgesi [DEVAM_DURUMU.md](DEVAM_DURUMU.md).

Monolith kendi oyun geliştirme işlerimizde kullandığımız kişisel Unreal aracıdır. Satış, gelir veya pazarlama hedefi yoktur. Yalnız UE 5.7.4 / Windows ortamında, kullanıcının somut ihtiyaçları için çalışıyoruz; genel amaçlı bir ürün veya bütün aksiyonları tamamlama hedefi yoktur. Bütün kalıcı sonuçlar bu depodadır; RecycleCo yalnız geçici test host'udur.

- Repo: `D:/UnrealProjects/monolith`, branch `fix/phase1-safety-honesty`; başlangıç commit'i `0cd50f0`.
- Host: `D:/UnrealProjects/RecycleCo/RecycleCo.uproject`. `Plugins/Monolith` junction'ı aynı kaynak depoya gider.
- Engine: `D:/UE_5.7`, **UE 5.7.4, CL 51494982**, Windows x64/D3D12.
- Genişletilmiş testte geçici olarak GameplayAbilities, StateTree, GameplayStateTree, GeometryScripting, Metasound ve CommonUI açıkça etkinleştirildi. Host descriptor'ı temizlikte başlangıç haline geri döndü.
- Üç yardımcı ajan kullanıldı; derleme, Editor, PIE ve paketleme tek koordinatörde, UBT `-MaxParallelActions=2` ile yürütüldü.

## Tamamlanan değişiklikler

| Alan | Son davranış |
|---|---|
| Blueprint | Strict değişken/struct tipi ön doğrulaması; inherited component yazma korumaları; GUID ile node bulma ve düzenlenebilir scalar/dotted-struct node property okuma/yazma; motion scaffold parametre alias'ları. |
| Animasyon | Yapılandırılmış compare/AND/OR transition graph'ları; gerçek typed anim-layer input parametreleri ve linked-layer pinlerinin açılması. Desteklenmeyen ifadeler dürüst `partial/deferred_rules` sonucunu korur. |
| İndeks/AI discovery | GAS, MetaSound ve legacy AI post-pass gerçekten çalışır; transaction/rollback/retry ve sahip olunan satır temizliği; BT/Blackboard/EQS yapı indeksi; native EQS ve StateTree tip keşfi. |
| Editor | GIF encoder işleri için başlatma/durum/sonuç/iptal; sınırlı iş kuyruğu, benzersiz çıktı yolları ve kapanış temizliği; undo/redo. |
| UI | CommonUI katman/stack, desired focus ve kalıcı navigation authoring; çalışan token resolver Construct graph'ı; derlenen ve gerçek Enhanced Input save/load roundtrip yapan settings scaffold. |
| Runtime | Yeni `MonolithRuntime` modülünde GAS widget binding extension/tipleri, dört BT task ve BT controller; eski reflected yollar için PreDefault redirect. |

Ayrıntılar: [Blueprint/animasyon](testing/2026-09-06-blueprint-animation.md), [indeks/jobs/BT](testing/2026-09-06-index-jobs.md), [UI](testing/2026-09-06-ui.md), [cook desteği](COOKED_BUILD_TODO.md).

Runtime düzeltmeleri:

- BT ability bitiş delegesi activation öncesi bağlanır; senkron bitiş yakalanır, abort önce aboneliği kaldırır ve sahip olduğu ability'yi iptal eder. Tag modunda tek spec seçilir; zaten çalışan spec sahiplenilmez.
- BT runtime node'ları editor graph yerine BehaviorTree asset'ine aittir. Eski graph-owned node'lar ilgili açık yazma/rebuild/duplicate işleminde onarılır; okuma sırasında asset değiştirilmez.
- GAS binding geç gelen/değişen owner'ı yeniden çözer; every-frame ve smoothing çalışır; WidgetComponent'in gerçek sahibini bulur. Cook-visible soft class referansı önceliklidir, eski string path fallback olarak korunur.
- UObject async yüklenirken tick kaydı yapılmaz. Game-thread Construct sırasında oluşturulan yardımcı tick nesnesi ve delegate'ler widget/destruction yaşam döngüsünde temizlenir.
- Audio perception yerleştirilmiş ve actor spawn anındaki component'leri kaydeder. Sonradan eklenen component için açık `RegisterAudioComponent` vardır; zaten çalan component'i kaydetmek de hearing olayı üretir. Pawn olmayan owner, tekrar çalma ve authority kontrolü desteklenir; ölü weak kayıtlar budanır.

## Doğrulama sonuçları

| Kontrol | Sonuç |
|---|---|
| Python transport/offline suite | **178 benzersiz test kapsandı, 0 hata.** İlk suite 162 geçti/16 ortam atlaması; native query ortamıyla 6 eksik test ve canlı MCP ile kalan 10 test ayrıca geçti. Risk rerun'ındaki 5 tekrar bu toplama ikinci kez eklenmedi. |
| Genişletilmiş host Editor derlemesi | Build 8 başarılı. Üretilen host settings sınıfı ve test fixture'ı dahil. |
| Tam Editor otomasyonu, genişletilmiş host ayarları | **173 test: 154 temiz + 19 uyarılı başarı, 0 hata, 0 notRun.** Automation5; test gövdesinde self-skip kaydı yok. |
| Yeni kritik regresyonlar | Async extension construction, BT runtime ownership ve bağımsız Widget capture fixture'ı geçti. |
| Gerçek PIE | **24/24, 0 hata, 3 hearing olayı**; `stop_pie` ve Editor kapanışı temiz. |
| Win64 Development paket | Build/cook/stage/archive başarılı; cook **0 hata/0 uyarı**. Runtime **24/24, 0 hata, 3 hearing, exit 0**; eski async ensure yok. |
| Win64 Shipping paket | Build/cook/stage/archive başarılı; cook **0 hata/0 uyarı**. Runtime JSON **24 kontrol, 0 hata, 3 hearing**, **exit 0**. |
| GIF jobs | Python/Pillow ve ffmpeg: üçer gerçek 128×128 frame, decode doğrulaması, benzersiz dizinler, kararlı sonuç okuma. Çalışan iş iptali ve tekrar iptal çağrısı geçti; test çıktıları silindi. |
| Undo/redo | Ayrı geçici map'te gerçek actor label değişikliği geri alındı/yenilendi; önceki map geri açıldı. |
| Canlı tam indeks | **9 asset = 7 proje + 2 native AttributeSet; 33 node, 9 bağlantı, 0 skip.** MetaSound: Asset/Page/4 Node/4 Dependency. BT: kök/composite/task + AIAssetSummary. |
| Repo lint/schema | **Geçti:** 1.074 skill action referansı, 1.585 kayıt, 0 schema drift / 0 hata. |
| Başlangıç host ayarlarıyla final kontrol | **Derleme başarılı (157 işlem); 173 test: 154 temiz + 19 uyarılı başarı, 0 hata / 0 notRun / self-skip yok.** Geçici host sınıfları yüklenmiyor; `/Game` asset listesi boş. |

24 runtime kontrolü; üretilen key save/load ve aktif settings instance'ını, CommonUI stack/focus/nav'ı, GAS ilk değer/delegate/late owner/smoothing/tick/SelfActor+soft ref'i, cook edilmiş BT ve runtime task/controller'ı, senkron ability bitişi/latent abort/delegate temizliğini, eski class redirect'ini ve gerçek audio hearing davranışlarını kapsar.

Automation uyarılarının önemli bölümü kasıtlı negatif/missing-asset kontrolleridir. BT ownership testi ayrıca opsiyonel JSON alanları için 12 log uyarısı üretir; başarı sayısı hata/uyarı yokmuş gibi sunulmaz. Bu suite bütün 1.500+ tool davranışının ayrı ayrı doğrulandığı anlamına gelmez.

## Testlerde yakalanıp giderilen sorunlar

İlk otomasyonun beş hatası GUID çözümleme, gizli linked-layer input pinleri, focus override/skeleton hazırlığı, Blackboard'ın engine SelfActor anahtarı ve eski discovery test objesiydi. Gerçek generated C++ build, protected Enhanced Input API erişimini ortaya çıkardı; public PlayerInput API'sine geçildi.

İlk paket iki önemli runtime sorunu buldu: editor graph altında oluşturulan BT kökü cook'ta eleniyordu; doğrudan FTickableGameObject tabanı async yükleme thread'inde ensure üretiyordu. İkisi de düzeltildi; final Development/Shipping paketleri aynı davranışları geçti.

Cook edilmemiş `UnrealEditor -game`, editor compiler modülü yüklenmeden Blueprint'i yeniden oluşturup GAS extension listesini boşaltabilir. Engine kaynakları ve normal PIE karşılaştırması bu mekanizmayı destekler. Binding doğrulaması için **PIE veya cook edilmiş paket** kullanılır. `-benchmark` Unreal'da sesi kapatır; final runtime/PIE testlerinde kullanılmadı. Bir eski teşhis fixture'ının PIE içinden tüm editörü kapatması Slate teardown crash üretti; fixture şimdi PIE'da sonucu yazıp runner'ın `stop_pie` çağrısını bekler. Final PIE temiz kapandı.

## Temizlik

Başlangıçta junction izlenmeden 391 dosya kaydedildi; mevcut Source/Config/Binaries/uproject ve Saved/Config kapsamındaki **22 dosya SHA-256 ile yedeklendi**. Engine API ile 7 fixture asset silindi. Geçici kaynaklar/ayarlar kaldırıldıktan sonra başlangıç host ayarlarıyla Editor yeniden derlendi ve 173 test tekrar geçti. Engine içinde `/Game` asset listesi ve `/Script/RecycleCo.Completion*` sınıf listesi boş doğrulandı.

Üç temizlik geçişinde sırasıyla 1.428, 6 ve 2 geçici dosya kaldırıldı; sonradan oluşan 19 boş Content dizini de kaldırıldı, başlangıçtaki boş dizinler korundu. Son audit: **22/22 yedek dosya aynı SHA-256, yeni dosya 0, Content dosyası 0, Source yalnız başlangıçtaki 5 dosya**. Host binary dosyaları da başlangıç yedeğine döndü; Monolith kaynakları ve derlenmiş plugin DLL'leri güncel kaldı. Test editörü/oyunu kapalı.

Son indeks temiz host üzerinde yeniden oluşturuldu: yalnız engine'in native `GameplayAbilities.AttributeSet` kaydı (**1 asset, 1 node, 0 bağlantı**); fixture asset/sınıf kaydı yok. Kanıtlar Monolith `Saved` altında kaldı.

Dosya envanteri 391'den 387'ye indi: Unreal çalışırken eski bir CEF log'u ve üç browser cache/WAL dosyası döndürüldü veya kaldırıldı. Bunlar yedeklenen proje kaynak/ayar dosyaları değildir; bütün host klasörünün birebir byte restorasyonu iddia edilmiyor. Dört yol `cleanup-final-audit.json` içinde kayıtlıdır.

## Kullanım sınırları

- Gerçek Tokenforge sağlayıcısı kurulmadı. Doğrulanan şey açık static resolver sözleşmesi ve oluşturulan graph'tır; canlı tema değişimine abonelik değildir.
- Menü `kind` başlangıç düzeni/focus/navigation/stack sağlar. Oyuna özgü Start/Settings/Quit davranışı ve stil proje tarafından bağlanır.
- GIF encoding arka plandadır; frame capture hâlâ senkron ve boyut/frame sınırlarına tabidir.
- Node property yazma; editable scalar ve dotted struct leaf ile sınırlıdır, bütün container/struct değiştirme aracı değildir.
- Marketplace LogicDriver/ComboGraph sağlayıcıları ve multiplayer/dedicated server bu çalışmanın gerçek runtime test matrisinde yoktur; ancak somut ihtiyaç oluşursa ele alınır. Linux/macOS ve UE 5.8 proje kapsamı dışındadır, bekleyen iş değildir.
- C++ kodu taşınan runtime sınıflarını kullanıyorsa `MonolithRuntime` modülüne bağımlılık eklemelidir. Kalıcı asset'lerde eski reflected yollar için redirect vardır.

## Tekrarlanabilirlik ve kanıt

Kalıcı fixture'lar [Scripts/fixtures](../Scripts/fixtures/README.md), authoring/cleanup [validate_completion_runtime.py](../Scripts/validate_completion_runtime.py), jobs [validate_completion_jobs.py](../Scripts/validate_completion_jobs.py), snapshot restoration [restore_completion_host.ps1](../Scripts/restore_completion_host.ps1).

Ham kanıtlar Git dışında Monolith `Saved/Completion20260906/` altında: baseline/backup, build logları, `Automation5/index.json`, `AutomationBaseline/index.json`, `build-editor-baseline.log`, `clean-host-engine.json`, `index-clean-host.json`, `cleanup-final-audit.json`, `runtime-pie.json`, `runtime-development-2-summary.json`, `UserShipping/Saved/MonolithCompletionRuntime.json`, exit kodları, `index-live-2.json`, `jobs_395214cb06aa429bbd0a2416357d2d9b/report.json`, RPC çağrıları ve paketler. RecycleCo'nun eski belgeleri değiştirilmedi; güncel sonuç kaynağı bu depodur.
