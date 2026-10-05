# Builds MaxCraft and writes the release zip to dist\MaxCraft-<version>.zip:
#
#   MaxCraft.asi                      the Max Payne 2 plugin (x86)
#   MaxCraft.ini                      settings
#   MaxCraft\SkyCraft-Minecraft.zip   the Minecraft side: SkyCraft's portable Prism Launcher with its
#                                     ready "SkyCraft" instance (Minecraft, Fabric, SkyCraft's mod),
#                                     taken from the SkyCraft release that the extern/SkyCraft
#                                     submodule is pinned at - the script refuses to build a pair
#                                     whose protocol cannot match byte for byte
#   MaxCraft\LICENSE.txt, THIRD-PARTY-NOTICES.md
#
# The version is not written here: it is the VERSION of the project() line in plugin\CMakeLists.txt,
# which CMake also hands to main.cpp for the log banner, so the zip name and the .asi's own first log
# line cannot disagree.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-NoBuild] [-SkyCraftVersion 0.1.2]
param(
    [switch]$NoBuild,
    [string]$SkyCraftVersion = "0.1.2",
    [string]$Version
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$stage = Join-Path $dist "stage"
$cache = Join-Path $dist "cache"

# $ErrorActionPreference is Stop and PowerShell 5.1 turns a native command's stderr into a
# terminating NativeCommandError, so stderr is dropped for the call and the exit code decides.
function Invoke-GitQuiet([string]$WorkingDirectory, [string[]]$GitArguments) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $null }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & git -C $WorkingDirectory @GitArguments 2>$null
        if ($LASTEXITCODE -ne 0) { return $null }
        return (($output | Out-String) -replace "\s+$", "")
    } finally {
        $ErrorActionPreference = $previous
    }
}

if (-not $Version) {
    $cmakeLists = Get-Content (Join-Path $root "plugin\CMakeLists.txt") -Raw
    if ($cmakeLists -notmatch 'project\(\s*MaxCraft\s+VERSION\s+([0-9]+(?:\.[0-9]+)*)') {
        throw "could not read the version out of plugin\CMakeLists.txt: pass -Version 0.1.1"
    }
    $Version = $Matches[1]
}

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

# 2. The Minecraft side, from SkyCraft's release. The two halves speak one protocol byte for byte -
# same layout, same kMappingName, same kVersion - so the release has to be the one built from the
# commit extern/SkyCraft is pinned at. Nothing in the protocol makes a mismatch fail loudly: a Minecraft
# built for another SkyCraft simply reads garbage, so this is checked rather than documented.
#   * the checkout against the superproject's pin catches a submodule left behind by a branch switch
#   * the pin against the tag catches -SkyCraftVersion being moved without the submodule following
# A shallow CI checkout has no tags, which is a warning here and not a failure.
$skyCraft  = Join-Path $root "extern\SkyCraft"
$pinned     = Invoke-GitQuiet $root @("ls-tree", "HEAD", "extern/SkyCraft")
$checkedOut = Invoke-GitQuiet $skyCraft @("rev-parse", "HEAD")
$tagged     = Invoke-GitQuiet $skyCraft @("rev-parse", "v$SkyCraftVersion^{commit}")
if ($pinned -and $pinned -match "commit ([0-9a-f]{40})") { $pinnedSha = $Matches[1] } else { $pinnedSha = $null }
if ($pinnedSha -and $checkedOut -and $pinnedSha -ne $checkedOut) {
    throw "extern/SkyCraft is checked out at $checkedOut but this repository pins $pinnedSha. Run: git submodule update --init"
}
if ($pinnedSha -and $tagged -and $pinnedSha -ne $tagged) {
    throw "extern/SkyCraft is pinned at $pinnedSha, which is not SkyCraft v$SkyCraftVersion ($tagged). The protocol has to match byte for byte: move the submodule to the tag, or pass the -SkyCraftVersion it was pinned at."
}
if ($pinnedSha -and -not $tagged) {
    Write-Warning "SkyCraft tag v$SkyCraftVersion is not fetched in extern/SkyCraft (CI clones it shallow): cannot check it against the pin $pinnedSha"
}
# PowerShell 5.1 has to work too: the README tells users to run this with powershell, not pwsh.
$skyCraftCommit = if ($pinnedSha) { $pinnedSha } else { $checkedOut }
Write-Host "MaxCraft $Version with SkyCraft v$SkyCraftVersion (extern/SkyCraft at $skyCraftCommit)"

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
