# Monolith — sonraki oturum için mevcut durum

Son güncelleme: **6 Eylül 2026**. Bu belge mevcut yerel çalışma durumudur; önceki faz raporlarını tarihsel kayıt olarak tamamlar.

## Amaç ve çalışma yerleri

- Monolith kullanıcının **kişisel Unreal geliştirme aracı**. Eklentiden gelir beklentisi yok; ticari hedef geliştirilecek oyun için.
- Kaynak repo ve bu oturumun çalışma klasörü: `D:\UnrealProjects\monolith`.
- Test projesi: `D:\UnrealProjects\RecycleCo\RecycleCo.uproject`.
- `RecycleCo\Plugins\Monolith`, bu kaynak depoya giden Windows junction'ıdır. Kaynak/DLL değişiklikleri ortaktır; bağımsız kopya değildir.
- Engine: `D:\UE_5.7`, UE **5.7.4**, CL **51494982**.
- Son kontrol: branch `fix/phase1-safety-honesty`, HEAD **`45283ac`**. Aşağıdaki düzeltmeler **commit edilmemiş çalışma ağacı değişiklikleridir**. Commit, merge veya push yapılmadı; değişiklikleri koru.
- Sonraki oturumda önce güncel `git status` ve süreçleri kontrol et. Bu kayıt, ileride değişmiş olabilecek canlı durumu varsaymak için kullanılmamalı.

## Tamamlanan işler

Phase 0 ve Phase 1 için [Phase 0 raporu](PHASE0_REPORT.md) ve [Phase 1 raporu](PHASE1_REPORT.md) geçerli tarihsel kayıtlardır. Sonrasında RecycleCo'ya eklenti bağlandı, derlendi ve kullanıcı tarafından ayrı oturumda gerçek proje testleri yapıldı.

RecycleCo ilk test sonuçları:

- MCP sözleşmesi: **10/10**, atlama yok.
- Tam `Monolith.` Automation / NullRHI: **144 temiz + 17 uyarılı başarı**, 0 hata, toplamın içinde **14 self-skip**.
- D3D12 Material.RoundTrip: **1 temiz başarı**.
- Blueprint `AgentValue=42`: kaydetme, editörü yeniden açma, PIE ve paket içindeki değer doğrulandı.
- Materyal, Niagara, widget ve motor ses çıkışı için seçilen canlı davranışlar doğrulandı.
- Win64 **Development** build/cook/stage/archive başarılı; küçük sahne paketlenmiş oyunda açılıp görüntülendi. İlk açılıştaki CommonUI hatası geçici viewport override ile giderildi.
- Bu sonuçlar tüm aksiyonların/entegrasyonların kapsandığı veya Shipping build'in doğrulandığı anlamına gelmez.

Kalıcı ayrıntılı rapor: `D:\UnrealProjects\RecycleCo\Docs\MONOLITH_TEST_SONUCLARI.md`. İlk testlerin kapsamı/atlamaları orada; **10. bölüm son düzeltme ve regresyon sonuçlarıdır**.

## Son uygulanan düzeltmeler

1. **Persistent Niagara kamera:** `Source/MonolithEditor/Private/MonolithEditorActions.cpp` içindeki `HandleCaptureSequenceFrames`, `PreviewScene->AddComponent` çağrısına `FTransform(CameraRotation, CameraLocation)` veriyor. Önceki identity transform kamera ayarını eziyordu.
2. **PIE durum/lifecycle:** `MonolithPieSmokeSession.h` içindeki session yapısı kendi PIE dünyasını `TWeakObjectPtr<UWorld> SessionWorld` ile tutuyor; `CreateSession` bunu başlatıyor. `MonolithEditorActions.cpp` içindeki raporlama hem `pie_active` hem `lifecycle` için aynı oturumun dünyasını kontrol ediyor. Önceki session'ın raporu yeni PIE oturumundan etkilenmiyor. **Final çözüm sadece `bPieActive=false` ataması değildir**; ara çözüm yerine dünya kimliği kullanıldı.
3. **CommonUI proje ayarı:** `D:\UnrealProjects\RecycleCo\Config\DefaultEngine.ini`, `[/Script/Engine.Engine]` bölümünde `GameViewportClientClassName=/Script/CommonUI.CommonGameViewportClient` kalıcı olarak ayarlandı. Bu dosya Monolith reposunun dışındadır.
4. **Doküman düzeltmesi:** [COOKED_BUILD_TODO](COOKED_BUILD_TODO.md) ve [SPEC_CORE](SPEC_CORE.md) artık `MonolithAudioRuntime` modülünün RecycleCo Development paketinde yüklendiğini kabul ediyor. UI–GAS, BT–GAS ve Audio–AI stimulus davranışları için açık kapsam korunuyor. [CHANGELOG](../CHANGELOG.md) güncellendi.

## Düzeltmelerin doğrulaması

- Son işlevsel kodla `RecycleCoEditor Win64 Development` derlemesi geçti: **5 adım, 9,06 saniye, exit 0**. Daha sonra yalnız kaynak yorumları ve dokümanlar düzenlendi.
- Python **3.12** ile repository lint geçti: **1.578 kayıt, sıfır şema hatası**. Sistem varsayılanındaki eski Python `tomllib` içermediğinden onu kullanma.
- Niagara persistent/single aynı açık kamera ve 0,2 / 0,8 / 1,5 saniye zamanlarıyla üçer kare üretti. 1,5 saniye görüntüleri incelendi: görünür parçacıklar ve uyumlu kadraj. Parçacık dağılımlarının birebir eşitliği iddia edilmedi.
- İlk ve ikinci PIE doğal tamamlanmada `complete`, `pie_active:false`, `teardown-complete`, `ok:true` döndürdü.
- İkinci PIE gerçekten çalışırken ilk session **inactive ve teardown-complete** kaldı.
- Üçüncü PIE `stop_pie_smoke` ile durduruldu: `stopped`, `pie_active:false`, `stopped-by-tool`. Erken durdurma için `ok:false`, doğal tamamlama başarısı gibi sunulmadı.
- Kalıcı CommonUI ayarıyla Editor/PIE'de önceki viewport hatası görülmedi. **Son düzeltmelerden sonra tam Automation suite veya paketleme tekrarlanmadı; gerçek CommonUI input davranışı ayrıca sınanmadı.**

Final kanıtlar: `D:\UnrealProjects\RecycleCo\Saved\MonolithFixValidation\` altında `build-final.log`, `calls-final.jsonl`, `results-final.json`, `editor-final.log`, `validate_final.py`, `persistent/`, `single/`.

`calls.jsonl` ve `RecycleCo-runtime.log` ara uygulama denemelerini de içerir; final sonuçlar için `*-final` kayıtlarını kullan. `validate_final.py` test sonunda fixture'ları siler; yeniden çalıştırmak için önce yeni Niagara/map fixture'ları hazırlamak gerekir. Tam yeniden çalıştırma gerekmiyorsa mevcut kanıtı incelemek yeterlidir.

## Temizlik ve kalan işler

- Düzeltme testi için oluşturulan `L_Test` ve `NS_Fountain`, `/Game/MonolithTests/FixValidation_20260906_1155/` altında **2/2 Editor API üzerinden silindi**. Asset yokluğu kontrol edildi.
- Açılan test editörü kapatıldı. Kullanıcının ayrı oyun projeleri değiştirilmedi; global MCP istemci ayarları değiştirilmedi.
- Kanıt dosyaları bilerek korundu. İlk geniş testin `C:\Users\PC\AppData\Local\Temp\RecycleCo-Monolith-20260906-111620` klasörü ayrı eski kayıttır; bu oturumda ona yönelik temizlik yapılmadı.
- Source index gerektiren testler ve etkin olmayan GeometryScripting/Metasound/ücretli eklenti entegrasyonları açık kapsamdır.
- UI–GAS / BT–GAS / Audio–AI stimulus binding için gerçek paket davranışı açık. Sadece oyunda kullanılacak özelliklere göre önceliklendir; genel eklenti kapsamını genişletmek başlı başına hedef değil.
- CommonUI kullanılacaksa gerçek input yönlendirmesini ve sonraki paket çalışmasını doğrula. Bugünkü Editor log kontrolünü paket/input testi gibi sunma.
- Sonraki iş başlamadan kullanıcı yönlendirmesini takip et; bu belge kendi başına yeni tam test/derleme/commit/push talimatı değildir.

## Çalışma kuralları ve hızlı erişim

Yalnızca RecycleCo test hedefini kullan; eski geçici validation projesine veya kullanıcının başka oyununa dönme. Aynı anda tek UE pipeline, derlemede `-MaxParallelActions=2`. Açık editörün ve paylaşılan DLL'lerin durumunu doğrula; kaydedilmemiş işi zorla kapatma. Çok adımlı MCP işlemlerinde proje kimliğini doğrula ve lease kullan.

- Derleme rehberi: `Skills/unreal-build/SKILL.md`.
- Test planı: `D:\UnrealProjects\RecycleCo\MONOLITH_TEST_PLANI.md` (kurulum anının planıdır; güncel sonuçlar için yukarıdaki sonuç raporu esas).
- Python 3.12: `C:\Users\PC\AppData\Local\Programs\Python\Python312\python.exe`.
- Gerekirse derleme: `D:\UE_5.7\Engine\Build\BatchFiles\Build.bat RecycleCoEditor Win64 Development -Project=D:\UnrealProjects\RecycleCo\RecycleCo.uproject -WaitMutex -NoHotReloadFromIDE -MaxParallelActions=2`.
