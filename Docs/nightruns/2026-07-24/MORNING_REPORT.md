# Sabah Raporu — 2026-07-24

**Sonuç: gece hedefine ulaşıldı ve aşıldı. 21 görev, 21 tamam, 0 park, 0 blocked.**
Dal: `night/2026-07-24` (master'a merge EDİLMEDİ — senin onayını bekliyor).

| | |
|---|---|
| Testler | **173/177 → 261/261 yeşil** (84 yeni test) |
| Faz 1 (Job System) | ✅ tamamlandı |
| Faz 2 (Level + Işık) | ✅ ana kapsam tamamlandı |
| Derleme | her adımda yeşil, ağaç her adımda temiz |
| Doğrulama | her görev bağımsız bir kontrol ajanınca denetlendi; 9 görev canlı editörde kanıtlandı |

---

## 1. Ne yapıldı

### Isınma — 4 kırmızı test (görev 1-4)
Dördü de yeşile döndü. Üçünde hata **testte değil koddaydı**, ve `Scripts/nightrun/README.md`'deki teşhislerin ikisi yanlış çıktı:
- `ParseLinearColorHex` — hex renkler degamma edilmeden yazılıyordu (renkler yanlış çıkıyordu).
- `Allowlist.UnknownTypeDenied` — bilinmeyen widget tipleri reddedilmiyor, 7 taban property'ye izin veriliyordu.
- `CursorPagination.QueryMismatchRejection` — README "hata eksik alan taşıyor" diyordu; gerçekte hata **hiç üretilmiyordu** (DB kontrolü doğrulamadan önce çalışıyordu).
- `ReflectionIntel.HeuristicAccuracy` — iki ayrı sebep: ileri-bakış bölüm sınırını aşıyordu (gerçek `Docs/` içeriğinde de yanlış pozitif üretirdi) + test fixture'ı kendi önermesini çürütüyordu.

### Faz 1 — İş Yöneticisi (görev 5, 6, 8, 9, 10, 19)
Uzun işler artık editörü dondurmuyor.
- `FMonolithJobManager` çekirdeği (tek ticker pompası, ayarlardan gelen saklama limitleri, kapanış güvenliği).
- `jobs` namespace: `list` / `poll` / `cancel` / `clear`.
- Çalıştırma katmanı: arka plan iş parçacığı + tick-dilimli oyun-iş-parçacığı işleri.
- `animation.rebuild_pose_search_index` artık **varsayılan async** — hemen `job_id` dönüyor.
- İki okuma (`get_database_stats`, `validate_pose_search_database`) artık beklemiyor, dürüst "kurulum sürüyor" bildiriyor.
- **Proxy artık zaman aşımında yalan söylemiyor**: "editör kapalı" yerine "istek zaman aşımına uğradı, editör muhtemelen hâlâ çalışıyor, tekrar deneme, `jobs_query` ile yokla".

### Faz 2 — Level Tasarımı + Işıklandırma (görev 12, 13, 14, 15, 16, 17, 18, 21)
Senin birinci önceliğin. Roadmap'in "yok" dediği şeylerin bir kısmı aslında vardı; asıl boşluklar başkaydı.
- **Işıklar**: SkyLight'a hiç ulaşılamıyormuş (motor sınıf hiyerarşisi yüzünden her yol onu atlıyordu) — açıldı. Serbest property yazımı, tipli geri-okuma ve **16 hazır ışık preset'i** (öğle güneşi, altın saat, ay ışığı, ampuller…) eklendi.
- **Atmosfer**: sis, gökyüzü atmosferi, post-process ve **hacimsel bulutlar** (4 preset) + Lumen okuma/yazma. 20 atmosfer preset'i.
- **Canlı viewport ekran görüntüsü**: `editor.capture_viewport` — açık haritanın, senin gördüğün hâli. Bu olmadan üretilen hiçbir görsel iş doğrulanamıyordu.
- **Data-driven level yerleşim sistemi**: düzenler JSON belgesi olarak tanımlanıyor, `mesh.apply_level_layout` ile uygulanıyor. İki kez uygulamak kopya üretmiyor; bozuk bir düzen **hiçbir şey** yerleştirmiyor; `mesh.capture_level_layout` ile elle düzenlediğin sahneyi belgeye geri alabiliyorsun. Tur kayıpsız: yakala → yeniden uygula → tekrar yakala, belge birebir aynı.

### Yan kazanç — bulunan gerçek hatalar
Bunlar aranmıyordu, iş sırasında çıktı:
- **31 editör-öldüren çökme noktası** (10 modülde): `SavePackage` öncesi eksik `FullyLoad()`. Diskte var olup açık olmayan bir asset'e yazan her çağrı editörü kapatıyordu — `niagara::add_emitter`, `gas::add_attribute`, `editor::import_texture`, `mesh::merge_actors` ve `ui::build_ui_from_spec` dahil.
- **`place_light` `rotation` parametresini hiç uygulamıyormuş** — motorun ışık aktörlerinde taşıdığı gizli dönüş yüzünden. Her yakala→uygula turunda 46° daha kayıyordu.
- **Post-process ayarları sessizce etkisizmiş**: Unreal'de her alan, yanındaki "override" biti açılmadıkça yok sayılıyor. Yakalanmasaydı aksiyonlar "başarılı" deyip ekranda hiçbir şey değiştirmeyecekti.
- `editor.get_viewport_info` gizli bir 0×0 viewport'u okuyup uydurma değerler döndürüyormuş.
- Test paketi ikinci koşuda çöküyormuş (aynı `FullyLoad` hatası).

---

## 2. Senin yapman gerekenler (kabul testi)

Editörü aç, bir level yükle. Sırayla:

**A. Level yerleşim sistemi — asıl özellik**
1. `mesh.apply_level_layout layout="demo_lit_room"` → 15 aktörlük aydınlatılmış oda kurulmalı.
2. **Aynı komutu bir daha çalıştır** → hâlâ 15 aktör olmalı, 30 değil.
3. `Ctrl+Z` **bir kez** → tüm yerleşim tek hamlede gitmeli. (Otomasyonun kontrol edemediği tek şey bu — geri alma girdi-başına çalışıyorsa söyle.)
4. Bir ışığı elle sürükle, parlaklığını değiştir → `mesh.capture_level_layout layout="benim_odam" from_layout="demo_lit_room" save=true` → `Plugins/Monolith/Saved/Monolith/LevelLayouts/benim_odam.json` dosyasını aç: elle düzenlemek isteyeceğin gibi okunuyor mu?
5. `mesh.remove_level_layout layout="demo_lit_room"` → `mesh.apply_level_layout layout="benim_odam"` → `editor.capture_viewport` ile bak: düzenlediğin gibi mi?

**B. Işık ve atmosfer — zevk kontrolü**
6. `mesh.place_light type="sky"` (yepyeni), sonra `preset="sun_golden_hour"`, `preset="bulb_warm_60w"` dene.
7. `mesh.spawn_atmosphere type=post_process` + `preset="look_neutral_manual_exposure"` → viewport'un otomatik parlaklık ayarı durmalı. **Bu en kritik test** — durmuyorsa override bitleri çalışmıyor demektir.
8. `mesh.spawn_atmosphere type="volumetric_cloud" preset="clouds_overcast"` → gökyüzünde bulut katmanı. Sonra `clouds_storm_towering` → daha yüksek ve yoğun olmalı.
9. **Preset değerleri işçilerin zevki, senin değil.** Beğenmediklerini `Config/MonolithLightPresets.json` ve `Config/MonolithAtmospherePresets.json` içinde düzenle — yeniden derleme gerekmiyor.

**C. Job sistemi (gerçek bir motion-matching veritabanın varsa)**
10. `animation.rebuild_pose_search_index` → hemen `job_id` dönmeli ve **editör donmamalı**. `jobs_query action="poll"` ile ilerlemeyi izle.

---

## 3. Bilmen gereken davranış değişiklikleri

| Değişiklik | Etkisi |
|---|---|
| `rebuild_pose_search_index` artık async | Eski bloke eden davranış için `wait: true` ekle |
| `get_database_stats` / `validate_pose_search_database` artık beklemiyor | Kurulum sürerken eksik ama anlık cevap; `wait: true` eskisi gibi |
| **`place_light` rotasyonu artık doğru** | `demo_lit_room`'un güneşi −81° yerine belgedeki −35°'de; sahne daha aydınlık görünecek. Eski görünümü istersen fiyatı belgedeki sayıyı değiştir, düzeltmeyi geri alma |
| `set_widget_property` sıkılaştı | Sınıfı ne native ne yüklü olan widget artık reddediliyor |
| `spawn_volume` sıkılaştı ve genişledi | Tanınmayan anahtar artık sessizce yok sayılmıyor, hata veriyor; ama torba artık her UPROPERTY'yi kabul ediyor. Eski `snake_case` yazımlar çalışmaya devam ediyor |
| `get_viewport_info` gerçek değerler döndürüyor | Eskiden `0x0` alan bir kod artık hata görecek |
| `select_actors sub_action=focus` | Kadraja alınacak şey yoksa artık hata veriyor (eskiden sessizce başarı diyordu) |
| Karar kaydı sorguları biraz az satır dönebilir | Heuristik daralması kasıtlı; yanlış pozitifler gitti |

**Bir iş sende:** proxy düzeltmesinin etkili olması için C++ ikilisini yeniden derlemen gerekiyor — `Tools\MonolithProxy\build.bat`. (`build_proxy.bat` bu makinede çalışmaz, VS2022 yolunu arıyor ama sende VS2019 BuildTools var.) Python proxy'si hazır, derleme gerektirmiyor.

---

## 4. Kasten yapılmayanlar (park değil, sıradaki işler)

- Yerleşim turunda kalan küçük kayıplar: geçici pakette duran mesh/Blueprint, ışık/atmosfer/volume'da birim dışı ölçek, kutu kurucusuyla yapılmamış brush.
- `spawn_actor` hâlâ property yazarken Blueprint'in SimpleConstructionScript kökünü ön-doğrulayamıyor (spawn sonrası yazıma düşüyor, geri alma çalışıyor).
- Faz 1'de tam iş-kimliği keşfi: editörün kendi başlattığı indeks kurulumları `job_id` üretmiyor (`FMonolithJob`'a hedef alanı eklemek gerekir).
- `capture_viewport`'un kamera parametresi yok ve dosya adları saniyeye yuvarlı — aynı saniyede iki görüntü birbirini eziyor.
- `MonolithBlueprintCompileActions.cpp:455` — belgelenmiş bir gerekçeyle `FullyLoad` uygulanmadı; test edebilecek birinin bakması iyi olur.

---

## 5. Protokol notları

- **Bekleme mekanizması** yine sorunsuz: 21 işçi + 21 kontrol ajanı bildirimi eksiksiz geldi, 1800 sn'lik yedek uyanma hiç tetiklenmedi.
- **Kontrol ajanı işe yaradı ve pasif kalmadı**: git/test okumanın ötesinde 9 kez pencereli editör açıp iddiaları kendisi üretti (ekran görüntüsü aldı, yerleşimi iki kez uygulayıp aktör saydı, proxy'yi sahte soketlere karşı koşturdu, eski parametre yazımlarını denedi). Bir kez `mismatch` verdi (görev 11); incelendi, farkın raporun özet cümlesindeki dosya sayımı olduğu görüldü, içerik doğruydu — LOG'a aynen kaydedildi.
- **İşçiler ipuçlarını sorguladı**: roadmap'in üç iddiası ve README'nin iki teşhisi yanlış çıktı, hepsi kanıtla düzeltildi. Görev 20'de üç teşhisten ikisi doğrulanmadı ve işçi bunu bildirdi — körü körüne "düzelttim" demedi.
- Roadmap (`Docs/GOLDENSTONE_ROADMAP.md`) gece içinde güncellendi: Faz 1 tamam, Faz 2 ilerlemesi ve üç maddi hata düzeltmesi işlendi.

**Merge kararı sende.** Dalı incelemek için: `git log --oneline master..night/2026-07-24`
