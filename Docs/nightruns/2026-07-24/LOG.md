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
