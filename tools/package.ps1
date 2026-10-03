# Builds MaxCraft and writes the release zip to dist\MaxCraft-<version>.zip:
#
#   MaxCraft.asi                      the Max Payne 2 plugin (x86)
#   MaxCraft.ini                      settings
#   MaxCraft\SkyCraft-Minecraft.zip   the Minecraft side: SkyCraft's portable Prism Launcher with its
#                                     ready "SkyCraft" instance (Minecraft, Fabric, SkyCraft's mod),
#                                     taken from the SkyCraft release matching extern/SkyCraft
#   MaxCraft\LICENSE.txt, THIRD-PARTY-NOTICES.md
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild] [-SkyCraftVersion 0.1.2]
param(
    [switch]$NoBuild,
    [string]$SkyCraftVersion = "0.1.2",
    [string]$Version = "0.1.1"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$stage = Join-Path $dist "stage"
$cache = Join-Path $dist "cache"
New-Item -ItemType Directory -Force $dist, $cache | Out-Null
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage "MaxCraft") | Out-Null

# 1. The plugin.
if (-not $NoBuild) {
    Push-Location (Join-Path $root "plugin")
    cmake --preset x86-release
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build --preset x86-release
    if ($LASTEXITCODE) { throw "build failed" }
    Pop-Location
}
$asi = Get-ChildItem -Recurse (Join-Path $root "plugin\build\x86-release") -Filter MaxCraft.asi | Select-Object -First 1
if (-not $asi) { throw "MaxCraft.asi not found: build first (or drop -NoBuild)" }
Copy-Item $asi.FullName $stage
Copy-Item (Join-Path $root "plugin\MaxCraft.ini") $stage

# 2. The Minecraft side, from SkyCraft's release (its protocol must match extern/SkyCraft).
$release = Join-Path $cache "SkyCraft-$SkyCraftVersion.zip"
if (-not (Test-Path $release)) {
    $url = "https://github.com/chasmlol/SkyCraft/releases/download/v$SkyCraftVersion/SkyCraft-$SkyCraftVersion.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest $url -OutFile $release
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($release)
try {
    $entry = $zip.Entries | Where-Object { $_.FullName -like "*SkyCraft-Minecraft.zip" } | Select-Object -First 1
    if (-not $entry) { throw "SkyCraft-Minecraft.zip not found in $release" }
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $stage "MaxCraft\SkyCraft-Minecraft.zip"), $true)
} finally {
    $zip.Dispose()
}

# 3. Licences.
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "MaxCraft\LICENSE.txt")
Copy-Item (Join-Path $root "THIRD-PARTY-NOTICES.md") (Join-Path $stage "MaxCraft")
Copy-Item (Join-Path $root "extern\SkyCraft\LICENSE") (Join-Path $stage "MaxCraft\SkyCraft-LICENSE.txt")

$out = Join-Path $dist "MaxCraft-$Version.zip"
if (Test-Path $out) { Remove-Item $out }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $out)
Write-Host "Wrote $out"
