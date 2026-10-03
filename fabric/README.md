# fabric/ — Minecraft tarafı

MaxCraft'ın kendi Minecraft modu yok. Bunun yerine SkyCraft'ın Fabric modunu **değiştirmeden** kullanır.

Bu mod Skyrim'e özgü değildir: yalnızca paylaşılan bellek protokolünü konuşur (Minecraft koordinatları, can oranları, girdi olayları, çarpışma üçgenleri ve blokları). MaxCraft eklentisi aynı protokolü Max Payne 2 tarafında birebir uyguladığı için Minecraft, karşısında Skyrim mi MP2 mi olduğunu bilmez.

- **Kaynak kodu:** `extern/SkyCraft/fabric/` (git alt modülü, SkyCraft v0.1.2'ye sabit)
- **Hazır paket:** `tools/package.ps1`, SkyCraft'ın aynı sürümünün yayınından `SkyCraft-Minecraft.zip` dosyasını (portable Prism Launcher ve hazır "SkyCraft" kurulumu) alır ve MaxCraft paketine koyar.

SkyCraft güncellenirken alt modül ve `package.ps1` içindeki `-SkyCraftVersion` birlikte değiştirilmelidir. Protokol sürümleri (`kVersion`) eşleşmezse Minecraft bağlanmaz. Bunu Minecraft'ın kendi log'u (`%LOCALAPPDATA%\SkyCraft\Prism\instances\SkyCraft\.minecraft\logs\latest.log`) yazar.

İleride MP2'ye özgü özellikler (örneğin bullet-time'da Minecraft'ın yavaşlaması) gerekirse burada SkyCraft modunun bir çatalı tutulacak.
