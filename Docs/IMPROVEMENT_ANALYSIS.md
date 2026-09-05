# Monolith MCP — İyileştirme Alanları Analizi (2026-09-05)

Branch: `feat/multi-agent-reliability` · Plugin 0.22.0 · Kaynak: 4 paralel kod denetimi (transport/koordinasyon, LLM ergonomisi, domain aksiyonları, altyapı). Hiçbir dosya değiştirilmedi.

---

## 1. Son commit sweep'inin değerlendirmesi

Multi-agent sweep (proxy concurrency, lease, HTTP hardening, skill entrypoints, reflection testleri) sağlam ve dürüst bir iş. Güçlü noktalar:
- Lease token'ı handler'a girmeden strip ediliyor, status'ta sızmıyor; exempt listesinden sızan mutating tool yok.
- Reentrancy guard (`DispatchDepth`) doğru; `ActiveExecutions>0` iken expiry erteleniyor.
- Proxy'de duplicate-id reddi, bozuk upstream yanıtı doğrulaması, Windows cache rename sorunu çözülmüş.
- Placeholder testler gerçek testlerle değiştirilmiş; AI discovery "boş success" yerine açık `not_implemented` dönüyor.

Sweep'in bıraktığı yapısal sınır: **her istek game thread'de senkron çalışıyor** ve lease **tek global slot**. Bu ikisi aşağıdaki alanların çoğunun kökü.

---

## 2. Altı iyileştirme alanı

### A. Transport + Koordinasyon (proxy ↔ editör)

**Mimari gerçekler (UE 5.7 kaynağından doğrulandı):** FHttpServerModule tick'te çalışır; listener frame başına 1 bağlantı kabul eder, SYN backlog 16, idle timeout 5 s. Proxy başına 8 in-flight + 64 kuyruk; proxy'ler arası global sınır yok. 3 ajan × 8 = 24 bağlantı → backlog taşması → hiç gönderilmemiş istekler "unknown outcome" olarak raporlanıyor.

**Somut bug'lar (S):**
1. Python proxy health probe `urllib` default opener kullanıyor → `HTTP_PROXY` env'e uyuyor, asıl çağrılar proxy'yi atlıyor. Kurumsal proxy'li makinede sonsuz `list_changed` fırtınası. (`monolith_proxy.py:528` vs `:271`)
2. Native proxy `MONOLITH_SPLIT_EDITOR_QUERY=1` ile yeniden yazılmış tools listesini aynı cache anahtarına yazıyor; başka proxy bu cache'i okuyup var olmayan tool ilan ediyor. (`monolith_proxy.cpp:1112-1159`)
3. Origin ret / 413 / protocol-version hataları JSON-RPC değil düz `{"error":…}`; proxy bunu "unknown outcome" sayıyor, oysa kesinlikle yürütülmedi. (`MonolithHttpServer.cpp:233,244,1036`)
4. Bağlantı hatası (connect öncesi) ile yanıt gelmemesi (gönderim sonrası) aynı "unknown" kovasında. İlki güvenle retry edilebilir. (`.py:286`, `.cpp:605-639`)
5. `MaxRequestBodyMB` kontrolü gövde tamamen buffer'landıktan ve NUL taramasından sonra → bellek sınırı değil. (`:216-249`)
6. Legacy batch item'ları arasında lease expire edebilir → 1..k çalışır, k+1 `-32011`. (`:321-329`)
7. `renew` `ttl_seconds` vermezse 120'ye düşüyor (600 ile alınan lease sessizce kısalıyor).

**Tasarım boşlukları:**
- Acquire'da kuyruk/fairness yok; `retry_after` kalan TTL (600 s'e kadar).
- Ajan süreci ölünce lease 10 dakika kilitli kalabilir; proxy EOF'ta release etmiyor.
- Restart ayırt edilemiyor: `server_instance` id yok (health pid veriyor ama status/lease hatası vermiyor).
- Owner içi sıralama yok: Claude Code paralel `add_node` + `connect_pins` üretirse ters sırada çalışabilir.
- Cancellation (`notifications/cancelled`) sessizce düşürülüyor; kuyruktaki istek bile iptal edilemiyor.
- Progress notification hiç yok.
- Timeout sonrası sonucu öğrenme yolu yok (request-id ring buffer önerisi).
- Call-log JSONL: rotation/cleanup yok; native proxy'de hiç yok; `ok:false` üç farklı durumu ayırmıyor.
- Uçtan uca correlation id yok; default log seviyesinde per-request satır yazılmıyor; metrik yok.
- Auth yok: yerel her süreç editörü sürebilir; yanlış projeye bağlanan proxy sessizce yanlış editörü değiştirir.
- Her istek yeni TCP bağlantısı (keep-alive kullanılmıyor).

**Bigger bets:** (A) Long-running job API — engine `AwaitingProcessing`'de timeout uygulamadığından deferred yanıt engine değişikliği gerektirmiyor. (B) Per-asset scoped lease — `kind:AssetPath` param etiketlerinden dokunulan asset kümesi otomatik türetilebilir. (D) Leased istek başına auto-transaction + `undo_last`. (E) Cross-process admission control (named semaphore). Proxy'siz Streamable-HTTP: düşük değer/yüksek maliyet.

### B. LLM Ergonomisi (discover / schema / error)

**Ölçümler:**
- 5940 `Error()` çağrısının ~%99'u default `-32603` (internal error). Hata sınıfı taksonomisi yok. Recovery hint oranı ~%3.
- Kod çakışması: `-32010` hem `lease_busy` hem `ErrOptionalDepUnavailable`.
- Schema drift: örneklenen 3 namespace'te action'ların %2-3'ü schema'da olmayan param okuyor (repo geneli tahmini 40-60 gizli param). 13 kayıt tamamen schema'sız → required check, alias, unknown-key uyarısı hepsini atlıyor.
- Arg'sız `monolith_discover` ~1580 action adı basıyor (8-12k token) — "terse by default" iddiası top-level'da geçersiz.
- Filter tek substring, ranking yok; `discover(namespace="bluprint")` typo'da did_you_mean yok.
- Per-action readOnly/destructive/idempotent alanları struct'ta var ama hiçbir yerde emit edilmiyor; `blueprint_query` içinde `get_variables` ile `delete_*` ayırt edilemiyor.
- Prerequisite/follow-up bilgisi yok (compile→save sırası sadece skill md'de).
- Response size cap / truncation marker yok; pagination 4 farklı şekilde.
- Success yolunda `structuredContent` yok (error yolunda var).
- Unknown-param uyarıları hata durumunda kayboluyor (sadece success'te ekleniyor).
- MCP resources/prompts hiç yok; proxy ve server farklı `instructions` metni gösteriyor.
- Undo yüzeyi yalnız `material.begin/end_transaction`; `editor.undo/redo` yok. Dry-run 18 action (~%1).
- Skills: `unreal-logicdriver.md`'de ~40 var olmayan action adı; sayaçlar stale (blueprint 89 vs 129).
- Kırık referanslar: `Docs/references/`, `Docs/plans/` (70 dosyada yorum referansı), `.claude/rules/scoped/monolith-release.md`.

**Bigger bets:** Structured error taxonomy + fix_hints; schema-drift lint CI + builder v2 (enum/range/items/example); `workflow` namespace + MCP prompts (recipes-as-code); MCP resources + universal `_limit/_cursor/_max_bytes`; per-action traits registry → docs/skills otomatik üretim.

### C. Domain Aksiyonlarında Dürüstlük ve Yazma Güvenliği

**"Success ama sahte" sınıfı (en kritik):**
- UI `build_ui_from_spec`: `layers/focus_table/nav_overrides` kabul edilip uygulanmıyor, `status="stub"` ile Success. LLM "menüyü oluşturdum" der.
- CommonUI button binding: parametre doğrular, BP-graph yazmaz, Success + `status="stub"`.
- LogicDriver discovery: `sm_component_count=-1` ile Success.
- Mesh `integration_hooks_stub` bir action olarak kayıtlı; `analyze_co_op_balance` "P3 placeholder" notuyla numerik skor veriyor.
- Animation `build_state_machine`: desteklenmeyen kurallar per-element `rule_deferred`, top-level `partial` bayrağı yok.
- **Ters durum:** Niagara'da 5 action schema'da "Phase 0 stub. Not yet implemented" diyor ama tam implement edilmiş → LLM kullanmaktan kaçınıyor. (`MonolithNiagaraTimingActions.cpp:64-90`)

**`save` sözleşmesi tutarsız:**
- Blueprint/Mesh/Editor: `save` param var, default false (doğru model).
- Material `set_material_property` / `batch_set_material_property`: `save` yok, her property yazımı diske. GAS attribute/effect/tag, CommonUI helpers, Audio SoundCue, AI navigation aynı.
- Animation `add/remove_compatible_skeleton`: default **true**, mevcut Skeleton'ı değiştirip kaydediyor.

**Path guard yok:** `ValidatePackagePath` yalnız şekil doğruluyor; `/Engine/` yazılabilir sayılıyor. Yanlış path'le `material.set_material_property` engine asset'ini değiştirip koşulsuz kaydeder — MISSING_FEATURES'ın `set_graph_node_property` için reddettiği tehdit mevcut action'larda zaten var.

**Optional plugin pattern'i 3 farklı:** A) namespace kaybolur (LogicDriver 66, ComboGraph, CommonUI 61, MetaSound), B) kalır + açık hata (Chooser, GeometryScript — en iyi), C) sessiz fallback (BlueprintAssist). LLM "namespace yok" ile "plugin kurulu değil"i ayıramıyor.

**Sentinel indexer'lar** (GAS, MetaSound, AI) kayıtlı ama hiç dispatch edilmiyor (MISSING_FEATURES:100).

### D. Uzun İşler / Game-Thread Bloklama (cross-cutting)

Async varyantı olan yalnız 3 şey: project index, source index, PIE-smoke session. Bloklayanlar:
- `risk.*` ilk çağrı: repo başına 30 s git mining, game thread'de (N repo × 30 s editör donması).
- `reflect.rebuild_reflection_index`, `run_automation_tests`, `save_dirty_assets`, `material.batch_recompile`.
- `audio.list_*`, `ai.list_*`, `mesh.validate_naming_conventions`: her asset için senkron `GetAsset()`.
- `capture_system_gif` / `stitch_flipbook`: ffmpeg senkron.

Multi-agent lease sistemi tek uzun action'la tüm client'ları kilitler. Job framework olmadan A alanındaki kazanımlar sınırlı kalır.

### E. Test ve CI

- CI yalnız proxy/installer Python testleri + native proxy build; **path filter yüzünden `Source/**` değişikliği hiçbir CI'ı tetiklemiyor**. macOS workflow dormant.
- 202 UE automation makrosu / 136 isimli test; VALIDATION filtresi 52'sini koştu, **84 isimli test filtre dışında** kaldı.
- **9/20 modülde sıfır test**: Niagara (128 action), GAS (135), Audio (98), Material (63), LogicDriver, ComboGraph, LevelSequence, BABridge, AudioRuntime. AI+Mesh+Animation+Blueprint (802 action) toplam 8 test.
- Fuzz yok (JSON-RPC, FTS5), golden snapshot yok (tools/list, API_REFERENCE), soak yok, auto-updater marker parse testi yok.
- Live test suite timeout senaryosunu hiç denemiyor (AUDIT bunu istiyor).
- SKILL.md validator repoda yok (VALIDATION "geçti" diyor).

### F. Release / Docs / DX / Hijyen

- `make_release.ps1` tek makineye kilitli (`C:\Program Files (x86)\UE_5.7`, `D:\Unreal Projects\FIVEPOINT8`).
- Sürüm 20+ yerde elle (uplugin, header, API_REFERENCE, 19 spec, CHANGELOG); gate yok.
- API_REFERENCE "regenerated from live registry" iddiası — generator yok.
- Release zip'e 11 MB vendored C (sqlite3.c, json.hpp) + 339 KB CHANGELOG + `Docs/testing/` giriyor.
- İmza/provenance yok; updater yalnız release body SHA'sına güveniyor.
- README'de sürüm geçmişi paragrafları; "Unreleased" etiketi 0.20'de yayınlanmış özellik için; 0.20-0.22 girişi yok; "1M+" vs "967K" çelişkisi.
- CONTRIBUTING'deki action ekleme örneği gerçek API ile uyuşmuyor (derlenemez).
- Bir action eklemek 5-8 dosyaya, yeni namespace 10+ dosyaya dokunuyor; generator yok.
- Lint/format/pre-commit yok; `.gitignore` `Tools/`'u yasaklarken 10 dosya tracked.
- JSONL call log'u hiçbir şey okumuyor; bug report bundle action yok; issue template yok.
- Wiki repo dışında; README 15+ wiki sayfasına link; Docs/ ile 4-5 yerde tekrar.
- `MCP/` dizini legacy, `Docs/SPEC.md` 103 byte redirect, FUNDING.yml boş şablon.

---

## 3. Önerilen faz planı

**Faz 0 — Quick wins (1-3 gün, hepsi S):**
1. Proxy health opener + busy/down ayrımı; `not_sent` vs `unknown`.
2. Split-editor cache anahtarı.
3. Origin/413/version hatalarını JSON-RPC + `executed:false` yap; `-32010` çakışmasını çöz.
4. Batch içinde lease pin; renew default = mevcut TTL; NUL-scan sırası.
5. Niagara'daki 5 yanlış "Not yet implemented" açıklaması.
6. UI spec / CommonUI stub'larını `implemented=false` capability error'a çevir; LogicDriver `-1` sayacını kaldır.
7. Top-level discover'dan action adlarını çıkar; discover/action_schema'da did_you_mean.
8. Success'te `structuredContent`; hata yolunda unknown-param uyarılarını taşı.
9. CI path filtrelerini kaldır; Tier-0 lint'ler (uplugin/template parse, sürüm tutarlılığı, link check, seed drift, private-path guard).
10. CONTRIBUTING action örneğini düzelt; README sürüm paragraflarını CHANGELOG'a; `.gitignore` Tools çelişkisi.
11. `unreal-logicdriver.md` action tablosunu yeniden üret.
12. Proxy EOF'ta best-effort lease release; `server_instance` id.

**Faz 1 — Güvenlik ve dürüstlük (1-2 hafta, M):**
- Merkezî `EnsureWritablePackagePath` (`/Game` + AdditionalContentPaths) tüm write action'larda.
- `save` default false sözleşmesini Material/GAS/Audio/CommonUI/Animation'a yay.
- Error taxonomy helper'ları (`NotFound/InvalidParam/Precondition/NotImplemented`) + `error.data{class, retryable, executed, request_id, suggestions, fix_hints}`; codemod ile 786 "not found" otomatik.
- Schema-drift lint CI'da; 13 schema'sız kaydı düzelt.
- Request-id header + Log-seviyesi satır + `_meta`; call-log rotation + native parity.
- `risk.*` ilk çağrıda senkron mining yerine `not_mined` hatası + `risk.mine` job.
- Optional plugin'ler için Chooser pattern'i (kalır + `available:false`); discover'da `availability` alanı.

**Faz 2 — Test/CI altyapısı (2-3 hafta):**
- Disposable `CI/MonolithValidation.uproject` shell → self-hosted nightly UE CI (5.7 + 5.8), tam `Monolith.` prefix, live suite, import smoke; aynı shell `make_release.ps1`'i sabit yollardan kurtarır.
- Registry golden snapshot (`Docs/schema/registry.json`) → API_REFERENCE, spec tabloları, skill tabloları, proxy seed listeleri tek kaynaktan.
- Sıfır-test modüllere birer roundtrip testi (Niagara, GAS, Material, Audio öncelikli).
- JSON-RPC fuzz + FTS5 differential fuzz; nightly soak.
- `Scripts/new_action.py` scaffolding; `bump_version.py`.

**Faz 3 — Bigger bets (kararlı seçim):**
- **Async job framework** (en yüksek kaldıraç; A, C, D alanlarını aynı anda açar).
- Per-asset scoped lease (job framework sonrası anlamlı).
- Per-action traits + prerequisites + examples registry → otomatik docs/skills.
- MCP resources + universal pagination/size cap.
- Local bearer auth (project identity yan kazancı ile).
- `workflow` namespace / recipes-as-code + MCP prompts.
- Leased istek başına auto-transaction + `undo_last`.

---

## 4. Somut bug listesi (hemen düzeltilebilir)

| # | Bug | Dosya | Efor |
|---|---|---|---|
| 1 | Health probe HTTP_PROXY env'e uyuyor, çağrılar uymuyor | `Scripts/monolith_proxy.py:528` vs `:271` | S |
| 2 | Split-editor rewrite'ı ham cache anahtarına yazılıyor | `Tools/MonolithProxy/monolith_proxy.cpp:1112-1159` | S |
| 3 | Origin/413/version ret gövdeleri JSON-RPC değil → proxy "unknown" diyor | `MonolithHttpServer.cpp:233,244,1036` | S |
| 4 | `-32010` iki anlamda kullanılıyor | `MonolithCoordination.cpp:117,141,192` / `MonolithJsonUtils.h:91` | S |
| 5 | Legacy batch ortasında lease expire edebiliyor | `MonolithHttpServer.cpp:321-329` | S |
| 6 | `renew` TTL'siz çağrıda 120'ye düşüyor | `MonolithCoordination.cpp:125` | S |
| 7 | Body size kontrolü buffer'lamadan sonra | `MonolithHttpServer.cpp:216-249` | S |
| 8 | Niagara 5 action "Not yet implemented" ama implement edilmiş | `MonolithNiagaraTimingActions.cpp:64-90`, `MonolithNiagaraActions.cpp:2590` | S |
| 9 | UI spec / CommonUI button stub'ları Success dönüyor | `MonolithUISpecActions.cpp:1152,1213`, `MonolithCommonUIButtonActions.cpp:878-894` | S |
| 10 | LogicDriver discovery `sm_component_count=-1` | `MonolithLogicDriverDiscoveryActions.cpp:147-156` | S |
| 11 | Unknown-param uyarıları hata durumunda kayboluyor | `MonolithToolRegistry.cpp:540` | S |
| 12 | Proxy `instructions` ≠ server `instructions` | `monolith_proxy.cpp:1090` vs `MonolithHttpServer.cpp:538` | S |
| 13 | py seed'de `monolith_reindex` var, cpp'de yok | `monolith_proxy.py:396` vs `.cpp:826` | S |
| 14 | `Source/**` değişikliği CI'ı tetiklemiyor | `.github/workflows/proxy-tests.yml:3-17` | S |
| 15 | CONTRIBUTING action örneği derlenemez | `CONTRIBUTING.md:88-127` | S |
| 16 | `.gitignore` `Tools/` yasaklı ama 10 dosya tracked | `.gitignore:30` | S |
| 17 | `unreal-logicdriver.md` ~40 var olmayan action | `Skills/unreal-logicdriver/unreal-logicdriver.md` | S |
| 18 | `Docs/references/`, `Docs/plans/`, `.claude/rules/...` dangling referanslar | `SPEC_CORE.md:896`, 70 kaynak dosya, `SPEC_MonolithReflectionIntel.md:1302` | S |
