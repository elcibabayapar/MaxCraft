# Third-party notices

## SkyCraft

MaxCraft speaks SkyCraft's shared-memory protocol and uses its Minecraft-side mod unchanged. The source is included as the `extern/SkyCraft` git submodule (pinned to v0.1.2). Parts of `plugin/src/Link.cpp`, `Input.cpp` (the scan-code table) and `Launcher.cpp` are adapted from SkyCraft's SKSE plugin.

- https://github.com/chasmlol/SkyCraft
- MIT License. The release zip carries its license text as `MaxCraft/SkyCraft-LICENSE.txt`.

## The bundled Minecraft side (release zip only)

`MaxCraft/SkyCraft-Minecraft.zip` is SkyCraft's own release bundle, redistributed unmodified. It contains:

- **Prism Launcher** (portable), GPL-3.0. Source: https://github.com/PrismLauncher/PrismLauncher
- **Fabric Loader** and **Fabric API**, Apache-2.0. Source: https://github.com/FabricMC
- **e4mc**, MIT. Source: https://github.com/vgskye/e4mc-minecraft-architectury
- **SkyCraft's Fabric mod**, MIT (above)

Minecraft itself and Java are not included: Prism downloads them with the player's own Microsoft account.

## MinHook

The plugin uses MinHook for function hooking (fetched at build time, not vendored).

- https://github.com/TsudaKageyu/minhook
- BSD 2-Clause License, Copyright (C) 2009-2017 Tsuda Kageyu.

## Trademarks

Max Payne is a trademark of Take-Two Interactive / Remedy Entertainment. Minecraft is a trademark of Mojang Synergies AB. This project is not affiliated with or endorsed by any of them.
