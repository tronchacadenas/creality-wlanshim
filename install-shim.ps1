<#
.SYNOPSIS
    Installs or removes the wlanapi.dll shim next to the scanner application.

.DESCRIPTION
    Run in an elevated PowerShell on the Windows machine (or VM) where the scanner software runs.
    Only copies files into the application's own folder; System32 is never touched.

.PARAMETER AppDir
    Folder containing CrealityScan.exe. Auto-detected when omitted.

.PARAMETER Ssid
    Wi-Fi network name (max 32 bytes) the virtual interface should report as "connected".
    Use YOUR scanner's Wi-Fi name. If omitted, the built-in default 'Lite Bridge-233A39'
    (the author's scanner) is used and a warning is printed.

.PARAMETER NoPlugin
    Install only next to CrealityScan.exe, not in CrealityScan_Data\Plugins\x86_64.

.PARAMETER Remove
    Uninstall: removes every copy of THIS shim under the app folder (other wlanapi.dll files are left alone).

.PARAMETER Force
    Replace an existing wlanapi.dll that is not this shim (it is backed up first).

.PARAMETER StopApp
    Stop running Creality Scan processes instead of refusing to continue.

.EXAMPLE
    .\install-shim.ps1 -Ssid "My Scanner-123456"
    .\install-shim.ps1 -Ssid "My Scanner-123456" -AppDir "C:\Program Files\CrealityScan"
    .\install-shim.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$AppDir,
    [string]$Ssid,
    [switch]$NoPlugin,
    [switch]$Remove,
    [switch]$Force,
    [switch]$StopApp
)
$ErrorActionPreference = "Stop"

$DefaultSsid = "Lite Bridge-233A39"
$Marker = "WLANSHIM_FORCE_FAKE"      # string that only builds of this shim contain
$ProcNames = @("CrealityScan", "CrealityUSBMeta")

function Test-IsShim([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    return [System.Text.Encoding]::GetEncoding(28591).GetString($bytes).Contains($Marker)
}

function Assert-AppStopped {
    $running = Get-Process -Name $ProcNames -ErrorAction SilentlyContinue
    if (-not $running) { return }
    if (-not $StopApp) {
        throw "Creality Scan is running. Close it first, or re-run with -StopApp to stop it."
    }
    $running | Stop-Process -Force
    Start-Sleep -Seconds 1
}

# ---- locate the application folder ----
if (-not $AppDir) {
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, (Join-Path $env:LOCALAPPDATA "Programs")) |
             Where-Object { $_ -and (Test-Path $_) }
    $found = @(Get-ChildItem -Path $roots -Filter "CrealityScan.exe" -Recurse -ErrorAction SilentlyContinue)
    if ($found.Count -eq 0) { throw "CrealityScan.exe not found. Pass -AppDir explicitly." }
    if ($found.Count -gt 1) {
        $list = ($found | ForEach-Object { $_.DirectoryName }) -join "`n  "
        throw "Several CrealityScan.exe found; pass -AppDir explicitly:`n  $list"
    }
    $AppDir = $found[0].DirectoryName
}
if (-not (Test-Path $AppDir)) { throw "Folder not found: $AppDir" }
Write-Host "Application folder: $AppDir"
$log = Join-Path $env:ProgramData "wlanshim.log"

# ---- uninstall ----
if ($Remove) {
    Assert-AppStopped
    $removed = 0
    Get-ChildItem -Path $AppDir -Filter "wlanapi.dll" -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
        if (Test-IsShim $_.FullName) {
            Remove-Item $_.FullName -Force
            $cfg = Join-Path $_.DirectoryName "wlanshim.txt"
            if (Test-Path $cfg) { Remove-Item $cfg -Force }
            Write-Host "Removed $($_.FullName)"
            $removed++
            $backups = @(Get-ChildItem -Path $_.DirectoryName -Filter "wlanapi.dll.orig-*" -ErrorAction SilentlyContinue)
            foreach ($b in $backups) { Write-Host "  A backup of a previous wlanapi.dll exists: $($b.FullName)" }
        } else {
            Write-Warning "Skipping $($_.FullName): not this shim."
        }
    }
    Write-Host "Removed $removed copy/copies. Log (if any) kept at $log"
    return
}

# ---- install ----
$src = Join-Path $PSScriptRoot "wlanapi.dll"
if (-not (Test-Path $src)) {
    throw "wlanapi.dll not found next to this script. Build it first (see README) or take it from a release."
}
if (-not (Test-IsShim $src)) { throw "$src does not look like a build of this shim." }

if (-not $Ssid) {
    $Ssid = $DefaultSsid
    Write-Warning "No -Ssid given: using the default '$DefaultSsid', the Wi-Fi name of the author's scanner. Pass -Ssid with the Wi-Fi name of YOUR scanner."
}
$ssidBytes = [System.Text.Encoding]::UTF8.GetByteCount($Ssid)
if ($ssidBytes -lt 1 -or $ssidBytes -gt 32) { throw "The SSID must be 1-32 bytes in UTF-8 (got $ssidBytes)." }

Assert-AppStopped

function Install-One([string]$Dir) {
    $dst = Join-Path $Dir "wlanapi.dll"
    if ((Test-Path $dst) -and -not (Test-IsShim $dst)) {
        if (-not $Force) {
            throw "$dst already exists and is not this shim. Re-run with -Force to back it up and replace it."
        }
        $bak = "$dst.orig-" + (Get-Date -Format "yyyyMMddHHmmss")
        Copy-Item $dst $bak
        Write-Host "Backed up existing file to $bak"
    }
    Copy-Item $src $dst -Force
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $Dir "wlanshim.txt"), $Ssid + "`r`n", $utf8NoBom)
    Write-Host "Installed in $Dir"
}

Install-One $AppDir
if (-not $NoPlugin) {
    $plug = Join-Path $AppDir "CrealityScan_Data\Plugins\x86_64"
    if (Test-Path $plug) { Install-One $plug }
}
Remove-Item $log -Force -ErrorAction SilentlyContinue

Write-Host "SSID reported as connected: '$Ssid'"
Write-Host "Done. Start Creality Scan, let it search for ~30 s, then run:"
Write-Host "    Get-Content '$log'"
