# Monolith — güncel devir durumu

Son güncelleme: **6 Eylül 2026**. İstenen eksik tamamlama, test ve RecycleCo temizliği tamamlandı. Bu belge sonraki oturumun giriş noktasıdır; ayrıntılı sonuç ve sınırlar [tamamlama raporunda](COMPLETION_2026_09_06.md).

## Amaç ve çalışma yerleri

- Monolith **kendi oyun geliştirme işlerimizde kullandığımız kişisel araçtır**. Satış, gelir veya pazarlama hedefi yok; kullanıcıya ticari hedef atfedilmez. Kalıcı çalışma kapsamı [AGENTS.md](../AGENTS.md) içindedir.
- Tek hedef **UE 5.7.4 / Windows**. Diğer sürümler/platformlar için uyumluluk çalışması hedef değildir. Yeni işler yalnız kullanıcının somut ihtiyacına göre seçilir; tüm mevcut aksiyonları düzeltmek veya genel amaçlı bir ürün yapmak hedef değildir.
- Repo ve kalıcı sonuçlar: `D:/UnrealProjects/monolith`. Yeni durum/test belgelerini burada tut; RecycleCo'daki eski raporlar tarihsel kayıttır.
- Branch: `fix/phase1-safety-honesty`; push hedefi `fork` (`BYGGOLDENSTONE/monolith`). Önceki kamera/PIE düzeltmeleri `0cd50f0` içinde; bu belgeyle birlikte kaydedilen değişiklikler onu tamamlar. Canlı commit/çalışma ağacı için `git log -1` ve `git status` kontrol edilir.
- Test host'u: `D:/UnrealProjects/RecycleCo/RecycleCo.uproject`. `Plugins/Monolith` junction'ı kaynak depoya gider; ayrı plugin kopyası değildir.
- Engine: `D:/UE_5.7`, **UE 5.7.4, CL 51494982**, Win64/D3D12.

## UnhallowedGround kurulumu — 6 Eylül 2026

- Aktif oyun: `D:/UnrealProjects/UnhallowedGround/UnhallowedGround.uproject`. `Plugins/Monolith` artık `D:/UnrealProjects/monolith` deposuna junction; bağımsız plugin kopyası yok. `.mcp.json` içindeki mevcut native proxy yolu aynı bağlantı üzerinden çalışır.
- Önceki kopya oyun projesinin `Saved/PluginBackups/Monolith-before-local-20260906-140149` dizinine taşındı. Plugin değişiklikleri yalnız Monolith deposunda commit/push edilir.
- `UnhallowedGroundEditor Win64 Development`, UE 5.7.4 ile derlendi. İlk derlemede görülen `MonolithAbpWriteActions.cpp` başlık sırası teşhisi düzeltildi; son derleme kontrolü başarılı. Tek pipeline, `-MaxParallelActions=2` kullanıldı.
- Editör açıldı; native proxy ile proje kimliği, tool listesi ve workflow lease desteği doğrulandı. Aynı anda gönderilen **24 salt okunur status çağrısının 24'ü geçti** (yaklaşık 5,4 saniye). Bu host'ta 31 MCP tool / 26 namespace / 1.486 aktif aksiyon görüldü; repo schema taramasının 1.585 kaydıyla aynı ölçüm değildir.
- Bu kurulumda oyun asset'lerine yazma veya yeni gameplay testi yapılmadı. Editör açık bırakıldı; sonraki işlem öncesi canlı süreç/proje durumu yeniden kontrol edilir. Kanıtlar `Saved/unhallowed-local-monolith-build-final.log`, `Saved/unhallowed-local-monolith-editor.log` ve `Saved/unhallowed-local-monolith-proxy.json` altında.

## Tamamlanan geliştirmeler

Blueprint yazma ön doğrulaması ve GUID/node property işlemleri; typed anim-layer input ve transition graph authoring; GAS/MetaSound/AI indeks dispatch, transaction ve recovery; native AI discovery; GIF encoder job lifecycle ve undo/redo; CommonUI focus/navigation/stack, token resolver graph ve gerçek Enhanced Input save/load scaffold tamamlandı.

GAS widget binding, dört BT task ve controller yeni **MonolithRuntime** modülüne taşındı. Eski reflected class yolları için redirect var. Ability delegate/abort, cook sırasında BT node sahipliği, async yüklemede tick kaydı, widget owner retry/smoothing ve Audio→AI hearing sorunları gerçek paket testleriyle giderildi. Taşınan sınıfları doğrudan kullanan C++ modülleri `MonolithRuntime` bağımlılığı eklemelidir.

## Son doğrulama

| Kontrol | Sonuç |
|---|---|
| Python | **178 benzersiz test, 0 hata**; ortam nedeniyle ilk atlanan testler native query ve canlı MCP ile ayrıca çalıştırıldı. |
| Editor | Genişletilmiş test host'u ve geri yüklenen başlangıç host ayarlarıyla derleme başarılı. Her iki son otomasyon: **173 test, 154 temiz + 19 uyarılı başarı, 0 hata, 0 notRun, self-skip yok**. |
| PIE | **24/24**, 3 gerçek hearing olayı; normal stop ve temiz kapanış. |
| Development ve Shipping | Her iki build/cook/stage/archive başarılı; cook 0 hata/0 uyarı. Her iki gerçek paket **24/24**, 3 hearing olayı, exit 0. Final Development log'unda async ensure yok. |
| Jobs / indeks | Python/Pillow ve ffmpeg encoding, çalışan işi iptal, gerçek undo/redo geçti. GAS/MetaSound/AI tam indeks fixture'ları doğrulandı. |
| Repo | Lint başarılı; **1.585 kayıt, 0 schema drift**, 1.074 skill action referansı. |

Bu kapsam bütün tool'ların tek tek doğrulandığı anlamına gelmez; bütün tool'ları doğrulama hedefi de yoktur. Gerçek Tokenforge sağlayıcısı, Marketplace LogicDriver/ComboGraph ve multiplayer/dedicated server bu çalışmada sınanmadı; yalnız ihtiyaç duyulursa ele alınır. Diğer platformlar ve UE 5.8 kapsam dışıdır. Menüye oyuna özgü davranış/stil bağlamak oyun geliştirme işidir. GIF frame capture senkron, encoder arka plandadır. Detaylı sınırlar ana raporda.

## RecycleCo son durumu

- Geçici host C++ sınıfları, descriptor/config değişiklikleri, fixture asset'leri ve test çıktıları kaldırıldı. **22/22 yedek dosya başlangıç SHA-256 değeriyle eşleşiyor**; Source yalnız başlangıçtaki 5 dosya.
- Final dosya audit'inde **yeni dosya 0, Content dosyası 0**. Engine içinden `/Game` ve fixture sınıf listesi boş doğrulandı. Başlangıçtaki bazı CEF log/cache dosyalarının doğal rotasyonu ayrıntılı temizlik raporunda kayıtlıdır.
- Başlangıç ayarlarıyla son derleme/testten sonra host binary/ayar yedekleri tekrar geri yüklendi. Monolith plugin kodu/DLL'leri güncel; test editörü/oyunu kapalı.
- Monolith proje indeksi temiz host için yeniden üretildi: **1 engine native AttributeSet asset / 1 node**, test fixture kaydı yok.
- Ham loglar, paketler ve backup Git dışında `Monolith/Saved/Completion20260906/` altında. Kalıcı bulgular `Docs`, tekrar kullanılabilir fixture/runner'lar `Scripts` altında.

## Sonraki oturum için

Önce güncel repo/süreç durumunu kontrol et ve kullanıcının yeni oyun geliştirme ihtiyacını takip et. Genel eklenti kapsamını genişletmek başlı başına hedef değil. Bu tamamlanmış rapor tek başına yeni test/derleme veya proje değişikliği talimatı değildir. Eski Phase 0/1 iş emirleri, genel iyileştirme analizi ve ilk audit/validation belgeleri kaldırıldı; geçmiş içerikleri Git geçmişindedir. Phase 0/1 sonuç raporları yalnız tarihsel kanıttır, güncel görev listesi değildir.

UE işlerinde [derleme skill'ini](../Skills/unreal-build/SKILL.md) izle: tek UE pipeline, `-MaxParallelActions=2`; ortak DLL'ler nedeniyle açık editörü kontrol et. MCP yazmalarından önce proje kimliğini doğrula ve lease kullan. Test host'una yeni geçici değişiklik yapılırsa kendi başlangıç snapshot'ını al; eski snapshot restore betiğini yeni kullanıcı değişikliklerinin üstüne uygulama.

Python 3.12: `C:/Users/PC/AppData/Local/Programs/Python/Python312/python.exe`; Windows metin işlemlerinde UTF-8 kullan. GIF smoke doğrulayıcısı Pillow gerektirir; bu oturumda PATH Python 3.10/Pillow 12.1 kullanıldı.

- [Ayrıntılı sonuç, sınırlar ve kanıtlar](COMPLETION_2026_09_06.md)
- [Cook/runtime desteği ve migration](COOKED_BUILD_TODO.md)
- [Fixture ve test runner kullanımı](../Scripts/fixtures/README.md)
- [Blueprint/animasyon](testing/2026-09-06-blueprint-animation.md), [indeks/jobs](testing/2026-09-06-index-jobs.md), [UI](testing/2026-09-06-ui.md)
