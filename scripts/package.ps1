#requires -Version 5.1
# Builds a distributable ZIP from an explicit allowlist (no real config, logs, build tree, handover docs).
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Version = '2.0.0'
)
$ErrorActionPreference = 'Stop'

# Stage outside the (possibly cloud-synced) project tree to avoid file locks.
$stage = Join-Path $env:TEMP "LLM-Gateway-pkg\LLM-Gateway-$Version"
New-Item -ItemType Directory -Path (Join-Path $Root 'dist') -Force | Out-Null
if (Test-Path $stage) {
    Get-ChildItem $stage -Recurse -Force | ForEach-Object { $_.Attributes = 'Normal' }
    Remove-Item $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage, "$stage\config", "$stage\THIRD_PARTY_LICENSES" | Out-Null

$allow = @(
    @{ Src = 'bin\llm-gateway.exe';       Dst = '' },
    @{ Src = 'bin\MaintenanceTool.exe';   Dst = '' },
    @{ Src = 'README.md';                 Dst = '' },
    @{ Src = 'NOTICE.md';                 Dst = '' },
    @{ Src = 'CONFIG_COMPATIBILITY.md';   Dst = '' },
    @{ Src = 'config\llm-gateway.external-only.example.json'; Dst = 'config' },
    @{ Src = 'config\llm-gateway.example.json';               Dst = 'config' }
)
$allow += @{ Src = 'LICENSE'; Dst = '' }

foreach ($item in $allow) {
    $src = Join-Path $Root $item.Src
    if (-not (Test-Path $src)) { throw "Missing allowlisted file: $($item.Src)" }
    Copy-Item $src (Join-Path $stage $item.Dst) -Force
}

# Collect third-party license texts from fetched dependency sources.
$deps = Join-Path $Root 'build\_deps'
Get-ChildItem $deps -Directory -Filter '*-src' -ErrorAction SilentlyContinue | ForEach-Object {
    $lic = Get-ChildItem $_.FullName -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|LICENCE)' } | Select-Object -First 1
    if (-not $lic -and $_.Name -eq 'wxwidgets-src') {
        $lic = Get-ChildItem (Join-Path $_.FullName 'docs') -File -ErrorAction SilentlyContinue | Where-Object Name -match '^licence\.txt$' | Select-Object -First 1
    }
    if ($lic) { Copy-Item $lic.FullName (Join-Path "$stage\THIRD_PARTY_LICENSES" "$($_.Name -replace '-src$','')-$($lic.Name)") -Force }
    else { Write-Warning "No license file found for $($_.Name)" }
}

# Dependency files may be read-only; make the staged copies writable.
Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.Attributes = 'Normal' }

# Libraries bundled inside wxWidgets that are statically linked (libpng, zlib, PCRE).
$wx = Join-Path $deps 'wxwidgets-src'
foreach ($e in @(@{ P = 'src\png\LICENSE'; N = 'wxwidgets-libpng-LICENSE' }, @{ P = 'src\zlib\LICENSE'; N = 'wxwidgets-zlib-LICENSE' }, @{ P = '3rdparty\pcre\LICENCE'; N = 'wxwidgets-pcre-LICENCE' })) {
    $f = Join-Path $wx $e.P
    if (-not (Test-Path $f)) { throw "Missing bundled license: $($e.P)" }
    Copy-Item $f (Join-Path "$stage\THIRD_PARTY_LICENSES" $e.N) -Force
}

# Safety check: refuse to package anything that looks like a private config or secret.
$bad = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -match '\.(log|gguf)$' -or $_.Name -eq 'llm-gateway.json' }
if ($bad) { throw "Forbidden file in package: $($bad.FullName -join ', ')" }

$zip = Join-Path $Root "dist\LLM-Gateway-$Version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$hashLines = Get-ChildItem $stage -Recurse -File | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.FullName.Substring($stage.Length + 1)
}
$hashLines += "{0}  {1}" -f (Get-FileHash $zip -Algorithm SHA256).Hash, (Split-Path $zip -Leaf)
Set-Content (Join-Path $Root "dist\SHA256SUMS.txt") $hashLines -Encoding ASCII
Write-Host "Package: $zip"





