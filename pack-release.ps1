<#
.SYNOPSIS
    Packages a clean RCO distributable (~180 MB) from a build output folder.
    Copies only runtime essentials; excludes heavy assets (models/textures/anims),
    dev state (.db*, .log, .py, thumbcache) and PM/dev tooling.

.EXAMPLE
    .\pack-release.ps1
    .\pack-release.ps1 -Source dist2 -Output release
#>
param(
    [string]$Source = "$PSScriptRoot\dist2",
    [string]$Output = "$PSScriptRoot\dist_clean"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Normalize to absolute paths
# ---------------------------------------------------------------------------
function Abs([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p.TrimEnd('\') }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $p)).TrimEnd('\')
}
$srcAbs = Abs $Source
$outAbs = Abs $Output

# ---------------------------------------------------------------------------
# Guard: output must not be inside source
# ---------------------------------------------------------------------------
if ([string]::Equals($outAbs, $srcAbs, [System.StringComparison]::OrdinalIgnoreCase) -or
    $outAbs.StartsWith($srcAbs + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "ERROR: Output '$Output' is inside source '$Source'. Aborting." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $srcAbs)) {
    Write-Host "ERROR: Source not found: $srcAbs" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== RCO Release Packager ===" -ForegroundColor Cyan
Write-Host "  Source : $srcAbs"
Write-Host "  Output : $outAbs"
Write-Host ""

# ---------------------------------------------------------------------------
# Clean output
# ---------------------------------------------------------------------------
if (Test-Path $outAbs) {
    Write-Host "Cleaning existing output..." -ForegroundColor Yellow
    Remove-Item $outAbs -Recurse -Force -Confirm:$false
}

$warnings = [System.Collections.Generic.List[string]]::new()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Copy-File-Checked([string]$src, [string]$dst) {
    if (-not (Test-Path $src -PathType Leaf)) {
        $script:warnings.Add("MISSING file : $src")
        return
    }
    $dir = Split-Path $dst -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    Copy-Item $src $dst -Force
}

function Copy-Dir-Checked([string]$src, [string]$dst) {
    if (-not (Test-Path $src -PathType Container)) {
        $script:warnings.Add("MISSING dir  : $src")
        return
    }
    if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force $dst | Out-Null }
    Copy-Item "$src\*" $dst -Recurse -Force
}

# ---------------------------------------------------------------------------
# SERVER
# ---------------------------------------------------------------------------
Write-Host "[ server ]" -ForegroundColor Green
Copy-File-Checked "$srcAbs\server\server.exe"  "$outAbs\server\server.exe"
Copy-File-Checked "$srcAbs\server\config.toml" "$outAbs\server\config.toml"
Copy-Dir-Checked  "$srcAbs\server\scripts"     "$outAbs\server\scripts"
# Excluded: server_migtest.exe, *.db*, *.log, *.py

# ---------------------------------------------------------------------------
# CLIENT
# ---------------------------------------------------------------------------
Write-Host "[ client ]" -ForegroundColor Green
Copy-File-Checked "$srcAbs\client\rco_client.exe" "$outAbs\client\rco_client.exe"
Copy-File-Checked "$srcAbs\client\config.toml"    "$outAbs\client\config.toml"

$clientDlls = @(
    'assimp-vc143-mt.dll', 'glfw3.dll', 'msquic.dll', 'kubazip.dll',
    'minizip.dll', 'poly2tri.dll', 'pugixml.dll', 'zlib1.dll'
)
foreach ($dll in $clientDlls) {
    Copy-File-Checked "$srcAbs\client\$dll" "$outAbs\client\$dll"
}

Copy-Dir-Checked "$srcAbs\client\shaders"      "$outAbs\client\shaders"
Copy-Dir-Checked "$srcAbs\client\assets\ibl"   "$outAbs\client\assets\ibl"
Copy-Dir-Checked "$srcAbs\client\assets\ui"    "$outAbs\client\assets\ui"
Copy-Dir-Checked "$srcAbs\client\data"         "$outAbs\client\data"
# Excluded: assets/models/ (7.4 GB), assets/textures/ (92 MB), assets/anims/ (195 MB)

# ---------------------------------------------------------------------------
# TOOLS
# ---------------------------------------------------------------------------
Write-Host "[ tools  ]" -ForegroundColor Green
Copy-File-Checked "$srcAbs\tools\rco_gue.exe" "$outAbs\tools\rco_gue.exe"

$toolsDlls = @(
    'assimp-vc143-mt.dll', 'glfw3.dll', 'kubazip.dll', 'minizip.dll',
    'poly2tri.dll', 'pugixml.dll', 'sqlite3.dll', 'zlib1.dll'
)
foreach ($dll in $toolsDlls) {
    Copy-File-Checked "$srcAbs\tools\$dll" "$outAbs\tools\$dll"
}
# Excluded: rco_project_manager.exe, rco_launcher.exe, thumbcache/, *.log

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
if ($warnings.Count -gt 0) {
    Write-Host ""
    Write-Host "WARNINGS -- missing items (dist may be incomplete):" -ForegroundColor Yellow
    foreach ($w in $warnings) { Write-Host "  $w" -ForegroundColor Yellow }
}

# ---------------------------------------------------------------------------
# Output tree (grouped by top-level dir)
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Output ===" -ForegroundColor Cyan

foreach ($sub in @('server', 'client', 'tools')) {
    $dir = Join-Path $outAbs $sub
    if (-not (Test-Path $dir)) { continue }

    $allFiles = Get-ChildItem $dir -Recurse -File
    $subBytes = ($allFiles | Measure-Object -Property Length -Sum).Sum
    $subMB    = [math]::Round($subBytes / 1MB, 1)
    Write-Host ("  " + $sub + "/   " + $subMB + " MB  [" + $allFiles.Count + " files]") -ForegroundColor White

    foreach ($entry in (Get-ChildItem $dir | Sort-Object Name)) {
        if ($entry.PSIsContainer) {
            $children = Get-ChildItem $entry.FullName -Recurse -File
            $bytes    = ($children | Measure-Object -Property Length -Sum).Sum
            if ($bytes -ge 1MB) {
                $szStr = ([math]::Round($bytes / 1MB, 1)).ToString() + " MB"
            } else {
                $szStr = ([math]::Round($bytes / 1KB)).ToString() + " KB"
            }
            $label = ($entry.Name + "/").PadRight(38)
            Write-Host ("    " + $label + $szStr + "  [" + $children.Count + " files]")
        } else {
            if ($entry.Length -ge 1MB) {
                $szStr = ([math]::Round($entry.Length / 1MB, 1)).ToString() + " MB"
            } else {
                $szStr = ([math]::Round($entry.Length / 1KB)).ToString() + " KB"
            }
            $label = $entry.Name.PadRight(38)
            Write-Host ("    " + $label + $szStr)
        }
    }
}

$totalBytes = (Get-ChildItem $outAbs -Recurse -File | Measure-Object -Property Length -Sum).Sum
$totalMB    = [math]::Round($totalBytes / 1MB, 1)
Write-Host ""
Write-Host "Total: $totalMB MB" -ForegroundColor Cyan
Write-Host ""

if ($warnings.Count -gt 0) {
    $wc = $warnings.Count
    Write-Host "Finished with $wc warning(s). Review items above." -ForegroundColor Yellow
    exit 1
} else {
    Write-Host "Done. Release ready at: $outAbs" -ForegroundColor Green
}
