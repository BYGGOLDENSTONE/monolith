# Gece Kaydı — 2026-07-24

Orkestratör: Fable (ana oturum). İşçiler: Opus. Kontrol ajanı: Haiku.
Dal: `night/2026-07-24`. GOAL: `Docs/nightruns/2026-07-24/GOAL.md`.

---

## [00:05] Gece başladı
- Durum: in-progress
- Ne oldu: GOAL kullanıcı tarafından onaylandı. Kullanıcı ayrıca devam yetkisi verdi — görev listesi biterse roadmap'teki sıradaki maddelerle onay sormadan devam edilecek (GOAL "Devam yetkisi" bölümüne işlendi). `night/2026-07-24` dalı açıldı, GOAL + bekleyen roadmap düzenlemesi ilk commit olarak push'landı.
- Karar: Görev 1 (ParseLinearColorHex) ile başlanıyor — 4 kırmızı test birbirinden bağımsız, en dar kapsamlı olan önce.
- Commit: 9acf3fd
- Sıradaki: Görev 1 işçisi çalışıyor; rapor gelince kontrol ajanı (haiku) doğrulayacak.

## [00:05] Görev 1 gönderildi — MonolithUI.Reflection.ParseLinearColorHex
- Durum: in-progress
- Ne oldu: Opus işçi asenkron başlatıldı. İşçiye "önce hangi taraf hatalı — test mi implementasyon mu — karar ver ve gerekçelendir" talimatı verildi; testi zayıflatarak geçirmek yasaklandı. Kapsam disiplini: diğer 3 kırmızı teste dokunmayacak.
- Karar: Bekleme mekanizması protokol §1'e göre — bildirim birincil, 1800 sn yedek uyanma.
- Commit: -
- Sıradaki: İşçi raporu.

## [00:11] Görev 1 TAMAM — ParseLinearColorHex yeşil
- Durum: done
- Ne oldu: İşçi hatanın implementasyonda olduğuna karar verdi ve gerekçelendirdi: `#FF8800` bir sRGB literali, motor widget `FLinearColor` property'leri (UImage.ColorAndOpacity) linear değer tutuyor; `UIReflectionHelper.cpp::ParseLinearColor` hex'i degamma yapan `ParseColor` yerine bayt/255 geçiren `TryParseColor`'a sokuyordu, altındaki degamma dalı ölü koddu — fonksiyonun kendi yorumu bile degamma iddia ediyordu. Zaten yeşil olan `MonolithUI.Common.ParseColor.Hex` testi aynı round-trip'i doğruluyor. `TryParseColor` davranışı (MD_UI materyal parametreleri için pass-through) KASITLI olduğu için elle tutulmadı. Test 173→174.
- Karar: Kabul. Kontrol ajanı (haiku) bağımsız doğruladı → **verified**: HEAD=43aa9a7, ağaç temiz, commit yalnız `UIReflectionHelper.cpp`'ye dokunuyor, `run_tests.json` 174/3/0 ve kalan 3 kırmızı tam olarak bilinen listeyle aynı, build.json yeşil.
- Commit: 43aa9a7
- Sıradaki: Görev 2 (Allowlist.UnknownTypeDenied).

### notes_for_next_worker (görev 1 işçisinden, AYNEN)
- **HARNESS TRAP — tam paket ikinci koşuda çöküyor.** İlk `run_tests.ps1` 46 sn'de `passed=0 failed=-1` (`index.json missing`) ile öldü; fatal: *"Asset '…/Tests/Monolith/UI/ErrorFormatting/WBP_BadAnchorPreset.uasset' cannot be saved as it has only been partially loaded"* → `UIErrorFormattingTests.cpp:126` (`CreateScratchWBP` → `UPackage::SavePackage`). Değişiklikle ilgisi yok; 2026-07-23 taban koşusundan kalan durum. Kök sebep: `CreateScratchWBP` (ve `UISpecRoundtripTests.cpp` / `UISpecBuilderTests.cpp` içindeki aynı biçimli yerel yardımcılar) `CreatePackage()` çağırıp `Package->FullyLoad()` yapmadan kaydediyor. Doğru desen ortak yardımcıda var: `Private/Tests/Hoisted/MonolithUITestFixtureUtils.h::CreateOrReuseTestWidgetBlueprint` (satır 112 `FullyLoad()` + `FindObject` geri kazanımı) — `WBP_Shadow*`/`WBP_Anim*` fixture'ları bu yüzden sorunsuz yeniden kaydediliyor.
- **Kullanılan geçici çözüm** (paket `passed=0` ile ölürse tekrarla): `Remove-Item -Recurse -Force D:\UnrealProjects\MonolithDev\Content\Tests\Monolith\UI\{ErrorFormatting,Roundtrip,SpecBuilder}`. Bunlar testlerin yeniden ürettiği tek kullanımlık fixture'lar; hiçbir test oradan hazır asset yüklemiyor. **Üç yardımcı düzeltilene kadar her tam paket koşusundan önce tekrarlanmalı.**
- `run_tests.ps1` HERHANGİ bir çökmede `passed=0, failed=-1, reason="UE report index.json missing"` yazıyor — kendi değişikliğini suçlamadan önce `Scripts/nightrun/results/run_tests.json` içindeki `logTail`'i oku.
- Süreler: artımlı derleme ≈ 10 sn (UBA); tam paket ≈ 6 dk; çöken paket ≈ 46 sn. Scriptler `pwsh -NoProfile -File <mutlak yol>` ile çağrılmalı (ajan cwd'si sıfırlanabiliyor).
- Foreground `sleep` engelli; arka plan test koşusunu beklemek için `Wait-Process -Name UnrealEditor-Cmd -Timeout N` temiz yol.

## [00:12] GOAL'a görev 7 eklendi — fixture kaydetme hatası
- Durum: in-progress
- Ne oldu: Görev 1'in ortaya çıkardığı harness tuzağı (üç test yardımcısında eksik `FullyLoad()`) GOAL'a 7. görev olarak eklendi.
- Karar: Kullanıcının verdiği devam yetkisi kapsamında eklendi. Gerekçe: her işçiyi yavaşlatan ve yanlış alarm üreten altyapı hatası; erken düzeltmek gecenin kalanını hızlandırır. Sıraya 4 test düzeltmesinden sonra, Faz 1 (görev 5-6) öncesine konuyor — Faz 1 testleri de aynı paketten geçecek.
- Commit: -
- Sıradaki: Görev 2 raporu.

## [00:12] Görev 2 gönderildi — MonolithUI.Allowlist.UnknownTypeDenied
- Durum: in-progress
- Ne oldu: Opus işçi başlatıldı; görev 1'in harness tuzağı + geçici çözümü ve zamanlama notları promptuna aynen aktarıldı. Güvenlik biçimli test olduğu için "aksi sağlam kanıtlanmadıkça test haklıdır" talimatı verildi.
- Karar: Regresyon eşiği 175/177 olarak belirlendi.
- Commit: -
- Sıradaki: Görev 2 raporu.

## [00:17] Görev 2 TAMAM — Allowlist.UnknownTypeDenied yeşil
- Durum: done
- Ne oldu: Yine implementasyon hatalıydı. `UIPropertyAllowlist.cpp::BuildCacheFor`, kayıtlı olsun olmasın HER token için 7 ortak `UWidget` taban property'sini enjekte ediyordu; bu yüzden bilinmeyen tip için `GetAllowedPaths` 0 yerine 7 döndürüyordu. Testi doğrulayan üç bağımsız kaynak: `UIPropertyAllowlist.h` ("Returns false for unknown types", "empty array for unknown tokens"), `MonolithUIRegistryActions.cpp:634` (`ui_describe_widget_type` zaten boş liste + "not in registry" döndürüyor) ve testin kendi yorumu. İşçi özelliği SİLMEDİ — `da2bd31`'in niyeti meşru, sadece token-kör uygulanmış; token'ın gerçek bir widget tipini adlandırmasına bağladı (`TokenNamesWidgetClass()`: önce registry, sonra `FindFirstObjectSafe<UClass>`). Düz `if (!Entry) return;` yapılmadı çünkü `PopulateFromReflectionWalk` Blueprint widget sınıflarını (`WBP_*_C`) kasten dışlıyor — o durumda BP widget'larda `Visibility`/`RenderOpacity` yazımı sessizce kırılırdı. Test 174→175.
- Karar: Kabul. Kontrol ajanı **verified**: HEAD=0078aeb, ağaç temiz, tek dosya (`UIPropertyAllowlist.cpp`, +69/-8), run_tests.json 175/2/0, logda 177 "Test Completed" (yani 22 sn'lik hızlı koşu gerçekten tam paketti), hedef test `Result={Success}`, build yeşil (exitCode 0).
- Commit: 0078aeb
- Sıradaki: Görev 3 (CursorPagination.QueryMismatchRejection).

### notes_for_next_worker (görev 2 işçisinden, AYNEN)
- Fixture temizleme geçici çözümü gerçek ve işe yaradı; çökme yaşanmadı. Paket duvar süresi bu sefer sadece ~22 sn (sıcak DDC / `-nullrhi`), 6 dk değil — hızlı dönmesi koşmadı anlamına GELMEZ; `grep -c "Test Completed"` ile `results/run_tests.log` içinde 177 olmalı.
- `results/run_tests.json`, `results/testreport/` ve `results/build.json` **paylaşımlı, tek slotlu** dosyalar; `run_tests.ps1` `Stop-MonolithProcesses` ile başlıyor. İki işçi harness'ı aynı anda koşarsa birbirinin editörünü öldürür ve hükmünün üzerine yazar. Harness koşuları SIRALI olmalı.
- Kasten düzeltilmeyen mevcut gizli hata (kapsam dışı, ayrı görevlik): `MonolithUIActions.cpp:763` allowlist token'ını `MakeTokenFromClassName(...)` yerine `FName(*Widget->GetClass()->GetName())` ile kuruyor; bu yüzden `NotInAllowlist` hatasının `valid_options` alanı tipin küratörlü eşlemelerini değil sadece ortak taban property'lerini listeliyor.
- `Scripts/nightrun/README.md` "Known issues" bu testi hâlâ kırmızı listeliyor; dört test bitince orkestratör listeyi tazelemek isteyebilir.
- needs_human: Editörde göz kontrolü isteğe bağlı — `set_widget_property` (raw olmayan), sınıfı ne native ne yüklü olan bir widget'ta artık `NotInAllowlist` dönüyor (önce 7 taban property'yi kabul ediyordu). Kasıtlı ama kullanıcıya görünür bir sıkılaştırma.

## [00:18] Görev 3 gönderildi — Monolith.CursorPagination.QueryMismatchRejection
- Durum: in-progress
- Ne oldu: Opus işçi başlatıldı. Görev 1-2'nin harness notları ve "her iki işçi de implementasyonu hatalı buldu, aynı tür teyit ara" yönlendirmesi promptuna eklendi. Regresyon eşiği ≥176/177.
- Karar: Kontrol ajanı doğrulaması bitene kadar bu işçi kasten bekletildi — harness sonuç dosyaları tek slotlu, eşzamanlı koşu hükmü bozardı.
- Commit: -
- Sıradaki: Görev 3 raporu.

## [00:18] Sıraya alınan ek işler (gece sonu boşluk olursa)
- Durum: parked (bilgi amaçlı)
- Ne oldu: İşçilerin bulduğu, kapsam dışı bırakılan iki küçük iş: (a) `MonolithUIActions.cpp:763` token kurma hatası — `NotInAllowlist` hatasının `valid_options` alanı eksik listeliyor; (b) `Scripts/nightrun/README.md` "Known issues" listesinin 4 test yeşile dönünce tazelenmesi.
- Karar: Ana görev listesinin arkasına alındı; Faz 1'den önce yapılmayacak.
- Commit: -
- Sıradaki: -

## [00:22] Görev 3 TAMAM — CursorPagination.QueryMismatchRejection yeşil
- Durum: done
- Ne oldu: **README'nin teşhisi yanlışmış.** "Hata ErrorData taşımıyor" değil — reddetme hiç üretilmiyormuş: görüntüsüz (`-nullrhi`) koşuda motor kaynak DB'si açık olmadığı için `HandleSearchSource` en baştaki `GetDB()/IsOpen()` guard'ında "Engine source DB not available." ile dönüyor, `INVALID_CURSOR` dalına hiç ulaşmıyordu (o dal zaten doğru payload'ı kuruyor). İşçi guard'ı cursor çözme/query-hash doğrulama bloğunun ARKASINA taşıdı. Testi doğrulayan kaynaklar: `Docs/SPEC_CORE.md:847,851` (bozuk cursor koşulsuz "clean INVALID_CURSOR" döner — backend durumuna istisna yok), `MonolithCursorCodec.h:37-45` (hash karşılaştırma ve mismatch'te INVALID_CURSOR üretmek dispatcher'ın görevi). Gerekçe ayrıca ilkesel: cursor geçerliliği saf parametre doğrulaması (JSON-RPC -32602), DB'siz karar verilebilir; "DB yok" ise geçici kaynak hatası ve çağıranı kalıcı ölü bir cursor'ı tekrar denemeye davet ediyordu. Komşu testler (`HardCap` vb.) index yoksa kendini atlıyor ama bu test kasten atlamıyor — tek kapsama o sözleşmede. Test 175→176.
- Karar: Kabul. Kontrol ajanı **verified**: HEAD=b59c4ca (ebeveyn 0078aeb), ağaç temiz, tek dosya `MonolithSourceActions.cpp`, run_tests.json 176/1/0, logda 177 "Test Completed", beş `CursorPagination.*` testinin HEPSİ `{Success}`, build yeşil.
- Commit: b59c4ca
- Sıradaki: Görev 4 (ReflectionIntel.Decision.HeuristicAccuracy) — son kırmızı.

### notes_for_next_worker (görev 3 işçisinden, AYNEN)
- **Bash-tool tuzağı**: `pwsh -NoProfile -File D:\yol\script.ps1` Bash tool üzerinden çağrılırsa ters bölü işaretlerini sessizce yutuyor (`D:htmlprojectsmcp...`, exit 64). PowerShell tool'unu tek tırnaklı yollarla kullan.
- Fixture silme geçici çözümü işe yaradı; paket ~90 sn'de koştu, `passed=0/failed=-1` ile ölmedi.
- **Bir tur kazandıran teşhis ipucu**: `Scripts/nightrun/README.md` "Known issues" ifadesine gerçek hata diye güvenme. Bir önceki `results/run_tests.log` içinde `Test Completed.*<TestinAdı>` ara ve hemen ardındaki `BeginEvents…EndEvents` bloğunu oku — tam assertion + satır numarası orada. Komşu testlerin uyarıları ("Skipping: source index not available") burada gerçek kök sebebi ele verdi.
- **Akılda tutulacak ortam gerçeği**: motor kaynak DB'si (`EngineSource.db`) görüntüsüz test koşularında AÇIK DEĞİL. `GetDB()/IsOpen()` ile korunan her aksiyon (yalnız `MonolithSourceActions.cpp` içinde 15 yer, ayrıca tüm `FDecisionQueryAdapter`/`FRiskQueryAdapter`/`FNetworkQueryAdapter`/`FCppReflectQueryAdapter` handler'ları) orada erken dönüyor. Bu aksiyonların testleri ya atlamalı ya da yalnız DB-öncesi davranışı doğrulamalı.
- **Aynı hata biçiminin kalan örneği (kasten bırakıldı, kapsam dışı)**: `FMonolithSourceActions::HandleFindExampleUsage` (~satır 2126) DB kontrolünü hâlâ `INVALID_CURSOR` dalından önce yapıyor; onun cursor reddi de görüntüsüz koşuda ulaşılamaz. Kapsama eklenirse tek satırlık sıra değişikliği.

## [00:23] Görev 4 gönderildi — Monolith.ReflectionIntel.Decision.HeuristicAccuracy (son kırmızı)
- Durum: in-progress
- Ne oldu: Opus işçi başlatıldı. Promptta özel uyarı: görev 3'te README teşhisi YANLIŞ çıktı, bu yüzden "heuristik fazla geniş" ifadesine güvenme, önce logdaki `BeginEvents…EndEvents` bloğundan kanıtla teşhis et. Görev 3'ün keşfettiği "görüntüsüz koşuda EngineSource.db kapalı" ortam gerçeği de aktarıldı — hata bunun yan etkisi olabilir.
- Karar: Bu son kırmızı olduğu için eşik 177/177; altı regresyon sayılır.
- Commit: -
- Sıradaki: Görev 4 raporu.

## [00:28] Görev 4 TAMAM — HeuristicAccuracy yeşil → **177/177, dört kırmızı test kapandı**
- Durum: done
- Ne oldu: İki ayrı sebep vardı; README'nin teşhisi yine yarı yanlıştı. Logdaki tam assertion: `Expected '03_non_decision contributes zero rows' to be 0, but it was 2` (`DecisionRecordIndexerTests.cpp:172`). DB-kapalı teorisi burada geçerli değil — test kendi tek kullanımlık SQLite dosyasını `AutomationTransientDir` içinde açıyor, `EngineSource.db`'ye hiç dokunmuyor; saf mantık hatası. (1) **Gerçek implementasyon hatası**: 8 satırlık ileri-bakış `## Some Sub-Heading` başlığından sonra bir SONRAKİ başlığın içine taşıp oradaki metni bu başlığın gerekçesi sanıyordu — spec §3.2'ye aykırı (paragraf başlığı TAKİP etmeli) ve gerçek `Docs/` korpusunda da yanlış pozitif üretirdi. `FindRationaleWithin` artık `OwnerHeaderLevel` alıyor ve aynı/daha üst seviyeli ilk başlıkta duruyor. (2) **Fixture kendi önermesini çürütüyordu**: son cümlesi tetikleyici token'ların üçünü kendi ikinci bölümünde harfiyen yazıyordu. İşçi ilkeli bir daraltma arayıp bulamadı (kelime sınırı: üçü de tam kelime; tırnak/backtick soyma: `rationale` tırnaksız da geçiyor; olumsuzlama tespiti: dayanaksız NLP tahmini) — spec'e göre bu katman kasten aptal substring eşleşmesi (0.65 güven, sorgu anında filtreleniyor), yani kapı haklı, fixture hatalıydı. Fixture yeniden yazıldı + gelecekteki editörlere uyarı notu eklendi. Indexer artık 8 yerine 6 kayıt üretiyor (5 dosyadan) — testin kendi başlık yorumuyla uyumlu.
- Karar: Kabul. Fixture değişikliği içerdiği için kontrol ajanına assertion bütünlüğü ayrıca doğrulatıldı → **verified**: `DecisionRecordIndexerTests.cpp` bu commit'te HİÇ değişmemiş (assertion'lar dokunulmamış), fixture diff'i yalnız düzyazıyı değiştiriyor (`0 satır` önermesi yerinde), HEAD=c6d73c6, ağaç temiz, üç dosya beyanla birebir, run_tests.json 177/0/0, logda 177 "Test Completed", dört `ReflectionIntel.Decision.*` testinin hepsi `{Success}`, build yeşil.
- Commit: c6d73c6
- Sıradaki: Görev 7 (harness fixture kaydetme hatası) — Faz 1'den önce, çünkü Faz 1 testleri de aynı paketten geçecek.

### notes_for_next_worker (görev 4 işçisinden, AYNEN)
- `Scripts/nightrun/README.md` "Known issues" hâlâ dört testi kırmızı listeliyor ve `HeuristicAccuracy` için yanlış teşhisi ("indexer non-decision fixture'dan 2 satır yutuyor") taşıyor — liste tazelenirken "fixture heuristiği kendi kendine tetikledi + ileri-bakış bölüm sınırını aştı" diye düzeltilmeli.
- Elle doğrulandı: bölüm sınırlama tüm gerçek pozitifleri koruyor (01→1, 02→2, 04→1 frontmatter yolu sınırsız, 05→2). Toplam 6, log da 6 diyor.
- Harness sorunsuz: fixture ön-temizliği gerekliydi, paket ~25 sn, artımlı derleme birkaç saniye.
- needs_human: Bir davranış değişikliği göz kontrolü isteyebilir — gerçek `Docs/` markdown'ında gerekçe paragrafı BİR SONRAKİ başlıktan sonra gelen bölümler artık `decision_query("list_decisions")` satırı üretmeyecek. Kasıtlı sıkılaştırma ama canlı korpusta satır sayısı bir miktar düşecek.

## [00:29] Görev 7 gönderildi — test fixture kaydetme hatası (harness güvenilirliği)
- Durum: in-progress
- Ne oldu: Opus işçi başlatıldı. Üç yardımcının ortak `CreateOrReuseTestWidgetBlueprint` desenine taşınması istendi (üç yerde kopyalamak yerine tekilleştirme tercihi belirtildi). Kabul kriteri sıkı tutuldu: **arada fixture silmeden paketi arka arkaya İKİ KEZ koşup ikisinin de 177/177 olduğunu kanıtlayacak** — yoksa düzeltme kanıtlanmış sayılmaz. Ayrıca `Scripts/nightrun/README.md` "Known issues" bölümünü tazeleme ve görev 3-4'te ortaya çıkan yanlış teşhisleri düzeltme işi de bu göreve bağlandı (aynı alan, küçük iş).
- Karar: Faz 1'den (görev 5-6) ÖNCE yapılıyor. Gerekçe: Faz 1'in yeni testleri de aynı paketten geçecek; her koşuda elle fixture silmek gereken bir harness ile ilerlemek hem yavaş hem yanlış alarm üretiyor.
- Commit: -
- Sıradaki: Görev 7 raporu.
