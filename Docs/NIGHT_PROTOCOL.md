# Gece Protokolü (Night Protocol)

**Amaç:** Kullanıcı gündüz kendi işinde çalışırken, gece boyunca Monolith fork'u üzerinde otonom, güvenli ve bağlam-taşmasız iterasyon yapmak. Sabah kullanıcı hazır bir raporla uyanır ve asıl kabul testini kendisi yapar.

**Temel ilke:** Hafıza konuşmada değil, diskte yaşar. Tek doğruluk kaynağı git repo'sundaki durum dokümanıdır; konuşma bağlamı her an ölebilirmiş gibi çalışılır.

---

## 1. Roller

### Orkestratör (karar verici)
- Ana Claude Code oturumu, **Fable modeli** — asla işçi modeline devredilmez.
- **Bağlamını temiz tutar:** Kaynak dosya okumaz, derleme çıktısı görmez, kod yazmaz. Sadece: LOG okur → karar verir → işçi görevlendirir → rapor alır → LOG günceller → commit/push → sıradaki karar.
- Yoldan çıkma durumlarında yönlendirme kararları orkestratöründür (bkz. §5).
- Zamanlamayı kendisi yönetir (self-paced wakeup); işçi çalışırken bekler, bildirimle uyanır.

### İşçi ajanlar
- Agent tool ile, **`model: "opus"`** ile başlatılır. Her işçi taze bağlamla doğar.
- **Aynı anda tek işçi** çalışır (sıralı zincir). Kullanıcı kuralı: hiçbir zaman 6'dan fazla eşzamanlı ajan.
- Her işçiye verilen: tek sınırlı görev + GOAL/LOG dosya yolları + proje kuralları + rapor formatı (§4).
- İşçi görevi bitirince yapılandırılmış rapor döner; ağır bağlam (dosya içerikleri, derleme logları, deneme-yanılma) işçide kalır, orkestratöre sadece özet gider.

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
6. Orkestratör raporu değerlendirir → LOG günceller → push → karar:
   - Geçti → sıradaki görev.
   - Takıldı → §5 hata politikası.
7. Durma kuralı tetiklenene kadar 4-6 tekrar.

SABAH
8. Orkestratör MORNING_REPORT.md yazar, push'lar, kullanıcıya özet mesaj bırakır.
9. Kullanıcı editörde kabul testi yapar; onaylarsa night dalı master'a merge edilir.
```

**Commit/push eşikleri:** Her yeşil derleme + test sonrası işçi commit'ler; orkestratör her işçi raporu sonrası LOG'u commit'leyip push'lar. Push edilmemiş hiçbir ilerleme "var" sayılmaz.

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
- Retry budget: 3 attempts to get green. If still failing, STOP and report status=blocked with full error context.
- On green: run the smoke suite (Scripts/nightrun/ — see Faz 0), then commit with a descriptive message. Do not push.

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
| İşçi "blocked" döndü (3 deneme tükendi) | Raporu incele; farklı yaklaşım tanımlanabiliyorsa YENİ işçiyle alternatif dene (aynı yaklaşımı tekrarlatma). |
| Alternatif de takıldı | Bilinen-iyi son commit'e revert; görevi "parked" işaretle, gerekçeyi LOG'a yaz, sıradaki göreve geç. |
| Üst üste 2 görev park edildi | Gece çalışmasını DURDUR. MORNING_REPORT yaz. Kötü gecede erken durmak, zarar vermekten iyidir. |
| Editör/derleme ortamı bozuldu (ör. kilitli dosya, disk) | Ortam sorununu tek işçiyle teşhis ettir; çözülmezse durdur. |
| Aynı hata iki farklı işçide aynı şekilde tekrarladı | Yaklaşım hatası varsay — görevi parçala veya park et, token yakmaya devam etme. |

**Güvenlik sınırları:** Kullanıcının oyun projelerine (`D:\UnrealProjects\*` — MonolithDev hariç) asla dokunulmaz. `master` dalına gece asla yazılmaz. Force-push yasak. Kullanıcının başka repo/dosyaları göreve dahil edilmez.

---

## 6. Faz 0 ön koşulu — insansız test düzeneği

Gece protokolü, `Scripts/nightrun/` altında kurulacak şu düzenek olmadan İŞLEMEZ (ilk gece çalışmasından önce gündüz oturumunda kurulur ve elle doğrulanır):

1. **`build.ps1`** — UBT derlemesini sarar; çıkışta yeşil/kırmızı + hata özeti üretir.
2. **`launch_editor.ps1`** — Editörü insansız başlatır (`-unattended -nosplash`, crash reporter penceresi kapalı, log dosyaya); MCP sunucusunun ayağa kalkmasını bekler (port 9316 health check).
3. **`smoke.ps1`** — HTTP üzerinden standart aksiyon serisini çalıştırır (discover, temel namespace çağrıları, o gece geliştirilen özelliğin aksiyonları); sonuçları JSON olarak yazar.
4. **Çökme yakalama** — editör süreci ölürse exit code + `Saved/Crashes/` + son log kuyruğu otomatik toplanır; işçi raporuna girer.
5. **`teardown.ps1`** — Editörü temiz kapatır, artık süreçleri öldürür.

Not: Editör açılışı + indeksleme gecede iterasyon başına dakikalar alır; gerçekçi kapasite gecede ~15-30 tam tur. GOAL bu kapasiteye göre boyutlandırılır.

---

## 7. Model politikası

- **Orkestratör: Fable** (ana oturum modeli). Orkestrasyon asla devredilmez.
- **İşçiler: Opus** — Agent tool çağrısında `model: "opus"` açıkça belirtilir.
- Küçük mekanik işler (LOG formatlama vb.) orkestratör kendisi yapar, ajan harcamaz.
