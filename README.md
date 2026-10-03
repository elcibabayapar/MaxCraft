# MaxCraft

Max Payne 2'yi bir Minecraft oyuncusu olarak oynayın: Minecraft fiziğiyle koşup zıplayın, Minecraft envanteri ve hotbar'ı kullanın, MP2'nin seviyelerine blok koyup kırın, düşmanlara Minecraft silahlarıyla vurun.

[SkyCraft](https://github.com/chasmlol/SkyCraft)'ın (Skyrim için) Max Payne 2 uyarlamasıdır ve aynı prensibi izler: **iki oyun da yeniden yazılmaz.** Minecraft arka planda gizli çalışır ve kendi mantığını yürütür. Max Payne 2 de kendi seviyelerini, düşmanlarını ve senaryosunu çalıştırır. Max Payne 2'ye yüklenen `MaxCraft.asi` eklentisi, SkyCraft'ın paylaşılan bellek protokolünü birebir konuşur. Bu sayede SkyCraft'ın Minecraft modu **hiç değiştirilmeden** kullanılır.

> **Durum: deneysel, henüz oyunda denenmedi.** Bütün parçalar yazıldı, ama motorla ilgili bazı varsayımların oyunda doğrulanması gerekiyor (bkz. [Bilinen sınırlar](#bilinen-sınırlar) ve [docs/DESIGN.md](docs/DESIGN.md) §8). Kayıtlarınızı yedekleyin.
>
> Bu bir hayran projesidir; Mojang, Microsoft, Remedy veya Rockstar ile bağlantısı yoktur. İki oyuna da sahip olmanız gerekir.

## Neler var

- **Hareket:** Max'i Minecraft fiziği yönetir (yürüme, koşma, zıplama, eğilme, yüzme, düşme). MP2 seviyelerinin çarpışma geometrisi Minecraft'a aktarılır.
- **Kamera:** MP2'nin kamerası Minecraft'ın gözüne bağlanır. F5 ile üçüncü şahıs kamerada Max'in kendi modeli görünür.
- **Arayüz:** Minecraft'ın eli, hotbar'ı, can/açlık barları, envanter ve bütün menüleri MP2 görüntüsünün üstünde çizilir.
- **Bloklar:** MP2 yüzeylerine blok koyup kırabilirsiniz. Bloklar MP2'nin derinlik tamponuyla çizildiği için MP2 duvarları onları doğru şekilde örter.
- **Dövüş:** MP2 düşmanları Minecraft'ta vurulabilir görünmez varlıklar olarak bulunur. Kılıç, yay, mızrak ve TNT dahil her Minecraft silahıyla vurulabilirler. Düşmanların mermileri Minecraft canınızdan düşer.
- **MP2'ye dönüş:** Ara sahneler ve scriptli kamera sırasında, ayrıca Max ölünce kontrol otomatik olarak MP2'ye geçer.

## Gereksinimler

| | |
|---|---|
| Max Payne 2: The Fall of Max Payne | Steam sürümü (1.01). Eklenti fonksiyonları isimleriyle bulur, eşleşmezse etkisiz kalır |
| [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) | **x86** sürümü |
| Minecraft: Java Edition sahibi bir Microsoft hesabı | Gerisi (Prism Launcher, Minecraft, Fabric, Java) paketle gelir |
| Yaklaşık 3 GB boş RAM | Minecraft arka planda çalışır |

## Kurulum

1. [Releases](../../releases) sayfasından `MaxCraft-<sürüm>.zip` dosyasını indirip içeriğini Max Payne 2 klasörüne çıkarın (`maxpayne2.exe` ile aynı yere).
2. Ultimate ASI Loader'ın x86 `dinput8.dll` dosyasını aynı klasöre **`dinput.dll`** adıyla kopyalayın. MP2 DirectInput'u `DINPUT.dll` üzerinden yüklediği için bu ad her zaman yüklenir.
3. Oyunu başlatın. İlk açılışta MaxCraft, Minecraft tarafını `%LOCALAPPDATA%\SkyCraft` klasörüne açar ve küçük bir Prism Launcher penceresi Microsoft hesabınızla giriş yapmanızı ister. Alt-Tab ile o pencereye geçip giriş yapın, sonra oyuna dönün. Prism, Minecraft'ı ve Java'yı ilk seferde birkaç dakikada indirir.
4. Bir bölüm başlatın. Minecraft hazır olduğunda kontrolü kendiliğinden alır.

Bir sorun olursa oyun klasöründeki `MaxCraft.log` dosyası ne olduğunu yazar. Daha ayrıntılı log için `MaxCraft.ini` içinde `bDiagnostics = 1` yapın.

SkyCraft'ı Skyrim için zaten kurduysanız iki mod aynı Minecraft kurulumunu ve dünyasını paylaşır.

## Kontroller

Öncelik Minecraft'ındır. Şu tuşlar Max Payne 2'de kalır:

| Tuş | İşlev |
|---|---|
| Esc | MP2 menüsü (açık bir Minecraft ekranını da kapatır) |
| G | MP2 eylem tuşu: kapılar, düğmeler (MP2'nin kendi tuşu) |
| E | Minecraft envanteri |
| V | MP2 Özellikleri |
| F6 | MP2 hızlı kayıt (F5 Minecraft'ın kamera tuşu olduğu için) |
| F9 | MP2 hızlı yükleme |
| O | Minecraft duraklatma ve ayarlar menüsü |

Diğer bütün tuşlar Minecraft'ındır: F5 kamera, T sohbet, / komutlar, Shift eğilme vb. E ve I tuşları `MaxCraft.ini` içindeki `iUseKey` ve `iInventoryKey` ile, diğer tuşlar Minecraft'ın kendi ayarlarından değiştirilebilir.

## Ayarlar

`MaxCraft.ini` (oyun klasöründe) her ayarı açıklamasıyla birlikte içerir. Önemli olanlar:

- **`fUnitsPerBlock`, `iUpAxis`, `bFlipZ`:** MP2 uzayının Minecraft'a nasıl eşlendiği. Varsayılan olarak Max'in çarpışma kapsülünden ölçülür (Max = 1,8 blok). Dünya aynalanmış görünürse `bFlipZ = 0` deneyin.
- **`iForwardRow`:** Kamera yana bakıyorsa veya Max yan yürüyorsa 0 ya da 1 deneyin.
- **`fEnemyDamageScale`, `fPlayerDamageScale`:** Dövüş dengesi.

## Bilinen sınırlar

- **Henüz oyunda denenmedi.** İlk denemede en olası sorunlar ayar dosyasından düzeltilebilir: dünyanın ölçeği, yönü, kameranın baktığı yön. `MaxCraft.log` dosyasını bir issue'ya ekleyin.
- Sadece statik seviye geometrisi çarpışmaya aktarılıyor. Kapılar, kutular ve hareketli nesneler henüz yok, yani kapalı kapılardan geçebilirsiniz.
- MP2 geometrisi kazılamaz. Bloklar onun üzerine ve yanına konabilir.
- Minecraft'ın düşen eşyaları, okları, mobları ve oyuncu modeli henüz MP2 içinde çizilmiyor. Bloklar, seçim çerçevesi, el ve arayüz çiziliyor.
- Bullet-time Minecraft'ı yavaşlatmıyor. Bunun için SkyCraft protokolüne bir mesaj eklenmesi gerekiyor.
- MP2'nin silahları ve envanteri, Minecraft oyuncuyu yönetirken kullanılamaz.

## Derleme

Gereken araçlar: Visual Studio 2022 (C++ masaüstü iş yükü), CMake 3.25+, Git.

```bat
git clone https://github.com/<siz>/MaxCraft.git
cd MaxCraft
git submodule update --init
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

Paket `dist\MaxCraft-<sürüm>.zip` olarak yazılır. Script eklentiyi **x86** olarak derler ve Minecraft tarafını `extern/SkyCraft` alt modülüyle eşleşen SkyCraft sürümünden indirir.

Sadece eklenti için: `cd plugin`, sonra `cmake --preset x86-release` ve `cmake --build --preset x86-release`. `MAXPAYNE2_DIR` ortam değişkeni ayarlıysa `.asi` dosyası her derlemeden sonra oyun klasörüne kopyalanır.

GitHub'a her push'ta Actions aynı paketi bulutta derler ve indirilebilir dosya olarak ekler.

## Klasörler

| Klasör | İçerik |
|---|---|
| `plugin/` | Max Payne 2 eklentisi (C++20, x86, MinHook) |
| `extern/SkyCraft/` | SkyCraft (git alt modülü, v0.1.2'ye sabit): protokol başlığı ve Minecraft modu |
| `fabric/` | Minecraft tarafı hakkında not |
| `tools/` | `package.ps1` (sürüm paketi), `dump_exports.py` (motor fonksiyon listesi) |
| `docs/` | Tasarım dokümanı ve motor notları |

## Lisans

MIT. Ayrıntılar için [LICENSE](LICENSE) ve [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) dosyalarına bakın. Bu depoda Max Payne 2 veya Minecraft'a ait hiçbir oyun dosyası yoktur ve olmamalıdır.
