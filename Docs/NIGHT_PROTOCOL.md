# Gece Protokolü (Night Protocol)

**Amaç:** Kullanıcı gündüz kendi işinde çalışırken, gece boyunca Monolith fork'u üzerinde otonom, güvenli ve bağlam-taşmasız iterasyon yapmak. Sabah kullanıcı hazır bir raporla uyanır ve asıl kabul testini kendisi yapar.

**Temel ilke:** Hafıza konuşmada değil, diskte yaşar. Tek doğruluk kaynağı git repo'sundaki durum dokümanıdır; konuşma bağlamı her an ölebilirmiş gibi çalışılır.

---

## 1. Roller

### Orkestratör (karar verici)
- Ana Claude Code oturumu, **Fable modeli** — asla işçi modeline devredilmez.
- **Bağlamını temiz tutar:** Kaynak dosya okumaz, derleme çıktısı görmez, kod yazmaz, küçük doğrulama okumalarını bile yapmaz (onlar kontrol ajanının işi — sabaha kadar bağlam şişmemeli). Sadece: LOG okur → karar verir → işçi görevlendirir → rapor + kontrol hükmü alır → LOG günceller → commit/push → sıradaki karar.
- Yoldan çıkma durumlarında yönlendirme kararları orkestratöründür (bkz. §5).
- Bekleme mekanizması (2026-07-23 provasında doğrulandı): Agent çağrıları SENKRON DEĞİL — işçi arka planda başlar, orkestratörün turn'ü biter; işçi tamamlanınca task-notification orkestratörü otomatik uyandırır. Birincil sinyal bu bildirimdir; ek polling yapılmaz. Her işçi gönderiminde ScheduleWakeup ile ~1800 sn'lik yedek uyanma kurulur (bildirim kaybolur/işçi asılı kalırsa güvenlik ağı); gece kapanışında iptal edilir. Provada 6/6 ajan bildirimi sorunsuz geldi, yedek hiç tetiklenmedi.

### İşçi ajanlar
- Agent tool ile, **`model: "opus"`** ile başlatılır. Her işçi taze bağlamla doğar.
- **Aynı anda tek işçi** çalışır (sıralı zincir). Kullanıcı kuralı: hiçbir zaman 6'dan fazla eşzamanlı ajan.
- Her işçiye verilen: tek sınırlı görev + GOAL/LOG dosya yolları + proje kuralları + rapor formatı (§4).
- İşçi görevi bitirince yapılandırılmış rapor döner; ağır bağlam (dosya içerikleri, derleme logları, deneme-yanılma) işçide kalır, orkestratöre sadece özet gider.
- İşçi LOG.md'ye ASLA yazmaz — LOG'un tek yazarı orkestratördür.

### Kontrol ajanı (bağımsız doğrulayıcı)
- Her işçi raporundan sonra Agent tool ile **`model: "haiku"`** olarak başlatılır (ucuz, mekanik iş).
- Görevi: işçinin beyanını bağımsız doğrulamak — `git log`/`git status` (commit gerçekten var mı, ağaç temiz mi), test/smoke JSON sonuç dosyaları, değişen dosya listesinin rapora uyup uymadığı.
- Orkestratöre en fazla ~10 satırlık hüküm döner: `verified` veya `mismatch` (+ neyin uyuşmadığı).
- Amaç: orkestratör işçiye körü körüne güvenmez ama doğrulama okumalarıyla da bağlamını doldurmaz.

---

## 2. Durum dosyaları

Her gece çalışması için `Docs/nightruns/YYYY-MM-DD/` klasörü açılır:

| Dosya | Kim yazar | İçerik |
|---|---|---|
| `GOAL.md` | Orkestratör (akşam, kullanıcı onayıyla) | Hedef, kabul kriterleri, görev listesi, durma kuralları, ilgili dosya işaretçileri |
| `LOG.md` | Orkestratör (her karar/rapor sonrası) | Zaman damgalı kayıt: tamamlananlar, mevcut durum, sıradaki adım, bilinen sorunlar, kararlar + gerekçeleri |
| `MORNING_REPORT.md` | Orkestratör (gece sonunda) | Kullanıcıya özet: yapılanlar, geçen/kalan testler, insan testi gereken maddeler, takılınan yerler |

### GOAL.md şablonu
```markdown
# Gece Hedefi — YYYY-MM-DD
## Hedef
<tek cümlelik ana hedef>
## Kabul kriterleri
- [ ] <ölçülebilir kriter>
## Görevler (sıralı, her biri tek işçiye verilebilir boyutta)
1. <görev — dokunulacak dosyalar/modüller işaretçileriyle>
## Kurallar
- Sadece C++, data-driven, hardcoded yasak. Gameplay mantığı asla BP node grafiği değil.
- Çalışma dalı: night/YYYY-MM-DD — master'a dokunma.
- Bu GOAL onaylandığında, bu dala gece boyu tüm commit/push işlemleri önceden yetkilendirilmiş sayılır (global commit-onay kuralının belgeli istisnası).
- <göreve özel ek kurallar>
## Durma kuralları
- Hedef tamamlandı VEYA üst üste 2 görev park edildi VEYA <saat sınırı>.
```

### LOG.md kayıt formatı
```markdown
## [HH:MM] <olay başlığı>
- Durum: <in-progress|done|parked|reverted>
- Ne oldu: <1-3 cümle>
- Karar: <orkestratörün kararı + gerekçe>
- Commit: <hash veya "-">
- Sıradaki: <bir sonraki adım>
```

---

## 3. Gece akışı

```
AKŞAM (kullanıcıyla birlikte, ~5 dk)
1. Kullanıcı hedefi söyler.
2. Orkestratör GOAL.md yazar, kullanıcıya onaylatır.
3. night/YYYY-MM-DD dalı açılır, GOAL.md ilk commit olarak push'lanır.

GECE (otonom döngü)
4. Orkestratör GOAL'daki sıradaki görevi alır, işçi promptu hazırlar (§4), Opus işçi başlatır.
5. İşçi: uygula → derle → test → düzelt (kendi iç iterasyonu, deneme bütçesi 3) → commit → raporla.
6. Kontrol ajanı (haiku): commit var mı, ağaç temiz mi, test sonuçları raporla uyuşuyor mu → kısa hüküm.
7. Orkestratör rapor + hükmü değerlendirir → LOG günceller → push → karar:
   - Geçti (verified) → sıradaki görev.
   - Mismatch → işçi beyanına değil kontrol hükmüne güven; §5 hata politikası.
   - Takıldı → §5 hata politikası.
8. Durma kuralı tetiklenene kadar 4-7 tekrar.

SABAH
9. Orkestratör MORNING_REPORT.md yazar, push'lar, PushNotification gönderir (ToolSearch ile yüklenir), kullanıcıya özet mesaj bırakır.
10. Kullanıcı editörde kabul testi yapar; onaylarsa night dalı master'a merge edilir.
```

**Commit/push eşikleri:** Her yeşil derleme + test sonrası işçi commit'ler; orkestratör her işçi raporu sonrası LOG'u commit'leyip push'lar. Push edilmemiş hiçbir ilerleme "var" sayılmaz.

### Devam (resume) — oturum ölürse gece kaybolmaz
Tüm durum LOG.md + commit'lerdedir; herhangi bir yeni oturum geceyi devralabilir. `/gece` çağrıldığında ilgili gecenin `Docs/nightruns/` klasörü var ve `MORNING_REPORT.md` YOKSA → sıfırdan başlanmaz: night dalına geçilir, LOG'un son kaydı okunur, döngüye kaldığı yerden girilir. Devralma LOG'a "resume" kaydı olarak işlenir.

---

## 4. İşçi görev promptu şablonu

İşçiye verilen prompt şu iskeletle yazılır (İngilizce yazılabilir; işçi raporunu İngilizce verir, orkestratör kullanıcıya Türkçe özetler):

```
You are a worker agent in an overnight autonomous run on the Monolith UE plugin fork.
Repo: D:\htmlprojects\mcp (branch night/YYYY-MM-DD). Host project: D:\UnrealProjects\MonolithDev.
Read first: Docs/nightruns/YYYY-MM-DD/GOAL.md (rules) and LOG.md (current state). Do NOT re-explore the whole repo.

TASK: <single bounded task, with file/module pointers>

Constraints:
- C++ only, data-driven, no hardcoded values, no BP node graphs.
- Build: D:\UE_5.7\Engine\Build\BatchFiles\Build.bat MonolithDevEditor Win64 Development -project="D:\UnrealProjects\MonolithDev\MonolithDev.uproject" -WaitMutex
- Run builds in the BACKGROUND (run_in_background) — UE builds can exceed the 10-minute foreground tool timeout. Prefer Scripts/nightrun/build.ps1 (has its own timeout + stale-process cleanup).
- Retry budget: 3 attempts to get green. If still failing, STOP and report status=blocked with full error context.
- If blocked: leave the working tree CLEAN — revert your uncommitted changes before reporting; describe the failed attempt in your report instead. Never leave half-done uncommitted work for the next worker.
- On green: run the smoke suite + automation tests (Scripts/nightrun/ — see Faz 0), then commit with a descriptive message. Do not push.
- Always run teardown (Scripts/nightrun/teardown.ps1 — kill editor processes) before finishing, success or failure.
- NEVER edit Docs/nightruns/*/LOG.md — the orchestrator is its only writer.

REPORT (your final message, exactly this structure):
- status: done | blocked | partial
- what_changed: <files + one-line each>
- build: green | red (<son hata özeti>)
- tests: <passed/failed/skipped counts + failing test names>
- commit: <hash or "-">
- notes_for_next_worker: <traps discovered, decisions made>
- needs_human: <anything only the user can verify>
```

---

## 5. Hata politikası (yoldan çıkma yönetimi)

| Durum | Orkestratör kararı |
|---|---|
| İşçi "blocked" döndü (3 deneme tükendi) | Kontrol ajanına ağacı teyit ettir (temiz mi?); kirliyse alternatif işçinin İLK adımı ağacı son bilinen-iyi commit'e döndürmek olur. Raporu incele; farklı yaklaşım tanımlanabiliyorsa YENİ işçiyle alternatif dene (aynı yaklaşımı tekrarlatma). |
| Alternatif de takıldı | Bilinen-iyi son commit'e revert; görevi "parked" işaretle, gerekçeyi LOG'a yaz, sıradaki göreve geç. |
| Üst üste 2 görev park edildi | Gece çalışmasını DURDUR. MORNING_REPORT yaz. Kötü gecede erken durmak, zarar vermekten iyidir. |
| Kontrol ajanı "mismatch" döndü (işçi beyanı doğrulanamadı) | İşçi raporuna değil hükme güven. Görevi geçmiş sayma; uyuşmazlığı LOG'a yaz, görevi aynı işçi tipiyle düzelttir veya blocked muamelesi yap. |
| Editör/derleme ortamı bozuldu (ör. kilitli dosya, disk) | Ortam sorununu tek işçiyle teşhis ettir; çözülmezse durdur. |
| Aynı hata iki farklı işçide aynı şekilde tekrarladı | Yaklaşım hatası varsay — görevi parçala veya park et, token yakmaya devam etme. |
| Agent çağrısı null döndü veya API/kota hatası | ALTYAPI hatası say, görev hatası DEĞİL — park sayacını tetiklemez. Kısa bekleme sonrası bir kez yeniden dene; yine olursa geceyi zarifçe durdur: MORNING_REPORT + push + PushNotification. |

**Güvenlik sınırları:** Kullanıcının oyun projelerine (`D:\UnrealProjects\*` — MonolithDev hariç) asla dokunulmaz. `master` dalına gece asla yazılmaz. Force-push yasak. Kullanıcının başka repo/dosyaları göreve dahil edilmez.

---

## 6. Faz 0 ön koşulu — insansız test düzeneği

Gece protokolü, `Scripts/nightrun/` altında kurulacak şu düzenek olmadan İŞLEMEZ (ilk gece çalışmasından önce gündüz oturumunda kurulur ve elle doğrulanır):

1. **`build.ps1`** — UBT derlemesini sarar; çıkışta yeşil/kırmızı + hata özeti üretir. Zaman aşımı içerir (varsayılan 45 dk — `-WaitMutex` sahipsiz bir süreç yüzünden sonsuza kadar bekleyebilir); derleme öncesi MonolithDev'e ait artık UnrealEditor/UBT süreçlerini tespit edip öldürür.
2. **`launch_editor.ps1`** — Editörü insansız başlatır (`-unattended -nosplash`, crash reporter penceresi kapalı, log dosyaya); MCP sunucusunun ayağa kalkmasını bekler (port 9316 health check, kendi zaman aşımıyla).
3. **`smoke.ps1`** — HTTP üzerinden standart aksiyon serisini çalıştırır (discover, temel namespace çağrıları, o gece geliştirilen özelliğin aksiyonları); sonuçları JSON olarak yazar. Doğrudan port 9316'ya bağlanır — proxy'nin 30 sn zaman aşımı tuzağını atlar.
4. **`run_tests.ps1`** — `Private/Tests/` otomasyon testlerini insansız çalıştırır (`UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests Monolith" -unattended -nullrhi`, log + JSON özet). HTTP smoke'tan daha güçlü sinyal; kontrol ajanının okuyacağı sonuç dosyasını üretir.
5. **Çökme yakalama** — editör süreci ölürse exit code + `Saved/Crashes/` + son log kuyruğu otomatik toplanır; işçi raporuna girer.
6. **`teardown.ps1`** — Editörü temiz kapatır, artık süreçleri öldürür. Her işçi, başarılı ya da başarısız, bitirmeden önce çalıştırır — yoksa bir sonraki derleme Live Coding mutex'ine takılır.

Not: Editör açılışı + indeksleme gecede iterasyon başına dakikalar alır; gerçekçi kapasite gecede ~15-30 tam tur. GOAL bu kapasiteye göre boyutlandırılır.

---

## 7. Model politikası

- **Orkestratör: Fable** (ana oturum modeli). Orkestrasyon asla devredilmez.
- **İşçiler: Opus** — Agent tool çağrısında `model: "opus"` açıkça belirtilir.
- **Kontrol ajanı: Haiku** — `model: "haiku"`; iş mekanik (git teyidi + JSON okuma). Hüküm kalitesi yetersiz kalırsa sonnet'e yükseltilir.
- Küçük mekanik işler (LOG formatlama vb.) orkestratör kendisi yapar, ajan harcamaz — ama doğrulama OKUMALARI kontrol ajanına gider (bağlam hijyeni).

---

## 8. Prova (kuru gece)

İlk gerçek geceden önce protokolün MEKANİĞİ gündüz, kullanıcı yokken (~30-60 dk) test edilir:

- Görevler önemsizdir ve **editör/derleme GEREKTİRMEZ** — bu yüzden Faz 0 düzeneği şartı provada aranmaz.
- Tam akış aynen işletilir: dal açma (`night/<tarih>-prova`), GOAL ilk commit, işçi (Opus) → kontrol ajanı (haiku) → LOG-commit-push zinciri, kasıtlı bir başarısızlık göreviyle blocked→alternatif→park yolu, MORNING_REPORT + PushNotification.
- Prova ayrıca **zamanlama mekanizmasını ölçer**: Agent çağrıları senkron mu dönüyor, ScheduleWakeup gerekli/kullanılabilir mi. Gözlem MORNING_REPORT'a yazılır ve §1'deki bekleme talimatı buna göre netleştirilir. (2026-07-23 provasında ölçüldü ve §1'e işlendi: asenkron + bildirim birincil + 1800 sn yedek. Kayıt: `Docs/nightruns/2026-07-23-prova/MORNING_REPORT.md`.)
- Prova dalı kabulden sonra merge edilmez; incelenip silinir (kayıt `Docs/nightruns/<tarih>-prova/` klasöründe yaşar).
