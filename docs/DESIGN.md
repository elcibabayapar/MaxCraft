# MaxCraft — Tasarım Dokümanı

> Max Payne 2'yi ana oyun olarak oynarken bir Minecraft oyuncusu *olmak*: Minecraft hareketi, envanteri, blokları ve dövüşü, Max Payne 2'nin gerçek seviyelerinde, MP2 düşmanlarına karşı.

v0.2 · 2026-10-03 · Mimari [SkyCraft DESIGN.md](https://github.com/chasmlol/SkyCraft/blob/main/docs/DESIGN.md)'den.

## 1. Temel prensip

**İki oyun da yeniden yazılmaz.** Minecraft kendi fiziğini, çarpışmasını, dövüş hesabını, envanterini ve blok mantığını çalıştırır. Max Payne 2 kendi seviyelerini, düşmanlarını, yapay zekasını ve senaryosunu çalıştırır. Aradaki eklenti yalnızca **çeviri** yapar.

MaxCraft bunu bir adım ileri götürür: **SkyCraft'ın Minecraft modu olduğu gibi kullanılır.** MaxCraft eklentisi, SkyCraft'ın Skyrim eklentisinin protokoldeki rolünü (`skycraft_protocol.h`, v11) Max Payne 2 için üstlenir. Minecraft tarafı protokol sürümünü, sihirli sayıyı ve yapı düzenini kontrol eder; karşıdaki oyunu bilmez.

## 2. Bileşenler

```
┌──────────── maxpayne2.exe (x86) ───────────────┐            ┌──────── javaw.exe (Minecraft + SkyCraft Fabric modu) ────────┐
│ MaxCraft.asi (Ultimate ASI Loader, dinput.dll) │            │                                                              │
│                                                │            │                                                              │
│ Collision  ─ oda üçgenleri + 1/8 voxel ────────┼──────────▶ │ SkyCollision / SkyCollider                                   │
│ Characters ─ düşman tablosu, hasar ────────────┼──────────▶ │ SkyrimActorEntity (görünmez, vurulabilir)                    │
│ Input      ─ DirectInput'tan tuş/fare ─────────┼──────────▶ │ InputBridge                                                  │
│ Characters ─ SkyState (konum, bakış) ──────────┼──────────▶ │ SkyClient (ışınlama, bakış)                                  │
│                                                │            │                                                              │
│ Characters ◀─ McState (ayak, göz, tick) ───────┼─────────── │ gerçek LocalPlayer fiziği                                    │
│ Camera     ◀─ göz + FOV ───────────────────────┼─────────── │                                                              │
│ Characters ◀─ HitActor / PlayerDied / patlama ─┼─────────── │ vanilla dövüş                                                │
│ Render     ◀─ blok mesh'leri, atlas, overlay ──┼─────────── │ WorldExporter / FrameExporter                                │
└────────────────────────────────────────────────┘            └──────────────────────────────────────────────────────────────┘
                     paylaşılan bellek Local\SkyCraft_v1 (SkyCraft ile birebir aynı)
```

| Dosya | Görev |
|---|---|
| `Engine.*` | MP2 motor fonksiyonlarını DLL export isimleriyle bağlar; korumalı yardımcılar |
| `Link.*` | Paylaşılan bellek. SkyCraft'ınkiyle aynı, ama **parça parça eşlenir** (§6) |
| `Mapping.*` | MP2 uzayı ↔ Minecraft blokları; Max'in kapsülünden kalibrasyon |
| `Characters.*` | Oyuncu tespiti, kare başı güncelleme, kukla (puppet), düşman tablosu, hasar köprüsü, SkyState |
| `Camera.*` | MP2 kamerasını Minecraft gözüne bağlar, FOV, seviye giriş/çıkış |
| `Collision.*` | Oda geometrisini yakalar, bölgeleri arka planda Minecraft'a akıtır |
| `Input.*` | DirectInput kancaları; tuş/fare yönlendirme, metin girişi |
| `Render.*` | D3D8 `Present`: bloklar, seçim çerçevesi, overlay, imleç |
| `Launcher.*` | SkyCraft'ın Prism paketini açar ve Minecraft'ı başlatır |

## 3. Max Payne 2 motoru

MP2'nin motor DLL'leri binlerce isimli C++ fonksiyonunu dışa açar. Adres tablosu veya imza taraması gerekmez. Tam liste için `python tools/dump_exports.py "<MP2 klasörü>"` çalıştırın.

| Gereken | MP2'de kullanılan |
|---|---|
| Kare başı güncelleme | `X_Character::updatePrePhysics` / `updatePostPhysics` |
| MP2'nin Max'i hareket ettirmesini durdurmak | `X_Character::updateCharacterPhysics` atlanır |
| Max'i taşımak | `X_CharacterProperties::setTransform` + phantom vtable[13] `setDisplayToWorldTransform` |
| Oyuncuyu bulmak | `getCharacterInput() != null` ve `!X_CharacterProperties::isAIActive()` |
| Ölçek ve eksenler | `X_RigidBodyCharacter::getCapsuleExtent` |
| Kamera | `X_CameraImplementation::update` / `getCurrentMatrix`, `X_LevelRuntimeCamera::setFOV` |
| Ara sahne algılama | `X_Character::isInCinematicMode`, `X_CameraImplementation::isCameraPathActive` |
| Seviye değişimi | `X_CameraImplementation::initLevel` / `deinitLevel` |
| Çarpışma geometrisi | `X_RigidBodyRoom::allocateRigidBodyRoom(geometry, matrix)` + `X_HavokGeometry::getVertex/getVertexIndex` |
| Hasar | `X_Character::causeDamage` (iki yönde), yedek olarak `setHealth` |
| Çizim | Direct3D **8** (`e2driver/e2_d3d8_driver_mfc.dll` → `d3d8.dll`). `maxpayne2.exe` D3D9'u yalnızca donanım tespiti için kullanır |
| Girdi | DirectInput (`X_Inputmfc.dll` → `DINPUT.dll`) |

`X_RigidBodyCharacter`'ın bir phantom olduğu ve 13. sanal slotunun `setDisplayToWorldTransform` olduğu, DLL içindeki `??_7X_RigidBodyCharacter@@6B@` vtable'ı okunarak doğrulandı.

## 4. Kare akışı

1. **`updatePrePhysics` (oyuncu):** Minecraft durumunu okur, gerekirse kalibre eder, ara sahne/ölüm kontrolü, ışınlama, fare bakışı, kukla kararı, Minecraft tick'lerinin ara değerlemesi, kamera matrisi, Minecraft olayları (vuruşlar, ölüm, patlamalar), düşman tablosu, çarpışma kuyruğu.
2. **`updateCharacterPhysics`:** **atlanmaz**, koşulsuz çağrılır. MP2 orada Max'in hangi odada olduğunu takip edip yalnızca o odadan görünen odaları çiziyor; atlandığında başka odalar siyah ya da kırık çiziliyordu. Konum `updatePostPhysics`'te ezilir.
3. **`updatePostPhysics` (oyuncu):** Kukla açıksa Max ve phantom'u Minecraft'ın ayak konumuna ve bakış yönüne taşır.
4. **`X_CameraImplementation::update`:** MP2'nin hesapladığı matrisin üzerine Minecraft gözü yazılır.
5. **`IDirect3DDevice8::Present`:** Heartbeat ve SkyState yazılır (Minecraft kare hızını buna göre ayarlar), render halkası boşaltılır, bloklar MP2'nin kendi view/projection matrisleri ve derinlik tamponuyla çizilir, ardından overlay.

Oyuncu güncellemesi 250 ms gelmezse MP2 duraklatılmış veya menüde sayılır. Bu durumda bütün girdi MP2'ye gider ve Minecraft'a `kSkyMenuOpen` bildirilir.

## 5. Koordinatlar

`mc = (mp × B) / unitsPerBlock`. B, MP2'nin yukarı eksenini Minecraft Y'ye taşıyan döngüsel bir permütasyon (det +1). MP2 solak (Direct3D) olduğu için Minecraft Z ayrıca ters çevrilir. Y-yukarı bir MP2 için sonuç `(x, y, −z)`.

- Ölçek: `fUnitsPerBlock > 0` elle sabitlenir; `0` (kod varsayılanı) ise Max'in kapsül boyundan ölçülür: `unitsPerBlock = kapsülYüksekliği / 1,8 blok`. Ölçülen değer 0,25–4 aralığı dışındaysa reddedilir ve MaxFX'in metri korunur. Kullanılan değer ve kaynağı ("auto" / "manual") `MaxCraft.log`'a yazılır.
- Yukarı eksen: kapsül ekseninin en büyük bileşeni.
- İkisi de `MaxCraft.ini` ile sabitlenebilir. Kalibrasyon değişirse çarpışma yeni epoch ile baştan gönderilir.

## 6. 32-bit adres alanı

SkyCraft paylaşılan belleği tek görünümde eşler. Büyük adres alanı bayrağı olmayan 32-bit MP2'de bu başarısız olabilir. MaxCraft üç parça eşler: küçük yapılar ve çarpışma halkası (~32 MB), render halkası (64 MB), overlay slotlarının yalnızca ekran çözünürlüğümüzün gerektirdiği kısmı (1080p'de 3 × 8 MB). Toplam ~120 MB. Görünümler 64 KB'ye hizalanır. Paylaşılan belleğinin toplamı `proto::kMappingBytes` = 200.327.168 bayt (191,06 MiB); yalnızca görünümler eşlenir.

## 7. Çarpışma

- Seviye yüklenirken her `allocateRigidBodyRoom` çağrısında odanın üçgenleri dünya uzayında kopyalanır (aynı geometri ve matris iki kez eklenmez).
- Arka plan iş parçacığı oyuncunun çevresindeki 9×5×9 bölgeyi (8³ blok) yakından uzağa doğru gönderir: önce kesin üçgenler (`kColTris`, SkyCraft'ın pürüzsüz çarpıştırıcısı için), sonra her bloğun 8×8×8 doluluk maskesi (`kColRegion`, üçgen–kutu SAT testiyle).
- Minecraft, ışınlamadan sonra ayağının altında zemin gelene kadar (en fazla 6 sn) oyuncuyu bekletir.

## 8. Oyunda doğrulanması gerekenler

Kod derleyiciden temiz geçiyor, ama şu varsayımlar MP2 çalışırken doğrulanmadı. Her biri log'a yazılır ve çoğu `MaxCraft.ini` ile düzeltilebilir.

| # | Varsayım | Yanlışsa belirti | Düzeltme |
|---|---|---|---|
| 1 | `M_Matrix4x3`: 3 satır dönüş + öteleme, satır-vektör | Konumlar anlamsız, çarpışma dağınık | Kod (Engine.h) |
| 2 | Karakter/kamera satırları (sağ, yukarı, ileri) | Kamera yana bakar, Max yan yürür | `iForwardRow` |
| 3 | Kapsül uçları Max'in boyunu verir | Dünya çok büyük veya küçük | `fUnitsPerBlock`, `iUpAxis` |
| 4 | MP2 solak | Dünya aynalı | `bFlipZ = 0` |
| 5 | Karakter orijini ayakta | Max yerin içinde veya havada | `fFeetOffset` |
| 6 | `getVertexIndex(üçgen, köşe)` sırası | "unreadable" veya darmadağın geometri | Kod (Collision.cpp) |
| 7 | MP2 FOV'u yatay; radyan/derece log'dan seçiliyor | Görüntü gerilmiş | Kod (Camera.cpp) |
| 8 | MP2 sahneyi `SetTransform(VIEW/PROJECTION)` ile çiziyor | Bloklar kayıyor (yedek kamera devreye girer) | Kod (Render.cpp) |
| 9 | `causeDamage(miktar, 0, 0, saldıran, null)` kabul ediliyor | Düşman ölmüyor (yedek: `setHealth`) | Kod (Characters.cpp) |
| 10 | Oyuncu = girdi var ve AI kapalı | "player is" log'u yanlış karakteri gösterir | Kod (Characters.cpp) |

## 9. Sonraki adımlar

1. Oyunda ilk çalıştırma: §8'deki varsayımları log'a bakarak doğrulamak.
2. Dinamik nesneler (kapılar, kutular): `X_RigidBodyDynamicObject` dönüşümleriyle her kare çarpışmayı güncellemek.
3. Minecraft varlıklarını (düşen eşyalar, oklar, moblar, oyuncu modeli) MP2 içinde çizmek: `kRenScene`, `kRenAvatar`.
4. Bullet-time senkronu: protokole bir "tick hızı" mesajı ekleyen küçük bir SkyCraft çatalı.
5. Oyuncunun Minecraft bloklarına çarpması için MP2 düşmanlarının da bloklardan etkilenmesi (`kRenSolids`).
