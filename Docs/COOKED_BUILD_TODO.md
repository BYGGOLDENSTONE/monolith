# Runtime cook desteği — güncel durum

**6 Eylül 2026: UE 5.7.4 Win64 Development ve Shipping paketleri doğrulandı.** Ayrıntılı sonuç ve host temizliği için [tamamlama raporuna](COMPLETION_2026_09_06.md) bak.

| Runtime içerik | Modül | Gerçek paket sonucu |
|---|---|---|
| GAS widget binding extension/tipleri | MonolithRuntime, Runtime/PreDefault | İlk değer, delegate, owner retry, smoothing/tick ve WidgetComponent owner çalıştı. |
| Dört BT task ve BehaviorTree AIController | MonolithRuntime, Runtime/PreDefault | Kaydedilen BT kökü/task cook'ta korundu; ability senkron bitişi, abort/cleanup ve controller başlatması geçti. |
| Sound perception user-data/subsystem/helper | MonolithAudioRuntime, Runtime | Binding cook'ta korundu; gerçek hearing, tekrar çalma ve zaten çalan non-pawn component kaydı geçti. |

İki pakette de 24 runtime kontrolü, sıfır hata, üç hearing olayı ve exit 0. Cook aşamaları 0 hata/0 uyarıyla tamamlandı. CommonUI menü katman/focus/navigation ve generated Enhanced Input save/load da aynı paketlerde doğrulandı.

## Migrasyon ve authoring

Editor tool modülleri editor'da kalır. Oyun tarafından referanslanan sınıflar `MonolithRuntime` içindedir; eski `/Script/MonolithAI` ve `/Script/MonolithGAS` class/struct/enum yollarına redirect uygulanır. C++ tüketicisi `MonolithRuntime` Build.cs bağımlılığını eklemelidir.

Runtime BT node'larının Outer'ı BehaviorTree asset'i olmalıdır. Yeni authoring bunu yapar; eski graph-owned node'lar açık yazma/rebuild/duplicate işlemlerinde onarılır. Eski BT'yi hiç düzenlemeden cook etmek için otomatik read-load migrasyonu iddia edilmez.

GAS extension'ın tick helper'ı game-thread Construct'ta oluşturulur; async UObject yüklemesi sırasında tick registry'ye girilmez. Attribute soft class referansları cook bağımlılığı taşır; legacy string fallback korunur.

## Test koşulları

Cook edilmemiş `UnrealEditor -game` editor compiler extension'ları yüklenmeden Blueprint regeneration yapabildiğinden binding doğrulaması için kullanılmaz; PIE veya gerçek paket kullanılır. `-benchmark` audio rendering'i kapatır. Sonradan oluşturulan ses component'leri perception subsystem'e açıkça kaydedilir; fire-and-forget sesler için `PlaySoundAndReportNoise` kullanılır.

Bu doğrulama multiplayer/dedicated-server, diğer engine sürümleri veya diğer platformlar için test sonucu değildir. Önceki editor-only sınıf yerleşimine dayalı TODO teşhisi bu değişiklikle geçersizdir; geçmiş ayrıntılar Git geçmişindedir.
