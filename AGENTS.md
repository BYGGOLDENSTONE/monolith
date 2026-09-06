# Monolith çalışma kapsamı

- Monolith'i kendi oyun geliştirme işlerimizde kullanıyoruz. Eklenti satışı, gelir, pazarlama veya genel ürün yayınlama hedefi yok. Kullanıcıya ticari başarı hedefi atfetme.
- Tek hedef ortam **UE 5.7.4, Windows/Win64** (`D:/UE_5.7`). UE 5.8 veya diğer platformların test edilmemiş olması bu projenin tamamlanması gereken eksiği değildir.
- Yalnız kullanıcının somut ihtiyacını ve bunun için gerekli düzeltmeleri ele al. Genel özellik tamamlama, bütün aksiyonları denetleme veya evrensel araç geliştirme çalışması başlatma.
- Her oturumda önce `Docs/DEVAM_DURUMU.md` dosyasını oku. Son tamamlanan çalışmanın kanıtları `Docs/COMPLETION_2026_09_06.md` içindedir. Tarihsel raporlar ve upstream belgelerdeki planlar yeni iş emri değildir.
- UE derleme/çalıştırma işlerinde `Skills/unreal-build/SKILL.md` dosyasını izle; tek UE pipeline ve `-MaxParallelActions=2` kullan. MCP yazmalarından önce hedef proje kimliğini doğrula ve lease kullan.
- Test kapsamını dürüstçe belirt; yalnız mevcut işi etkileyen sınırları gündeme getir. Doküman güncellemesi tek başına engine derlemesi veya runtime test talebi değildir.
