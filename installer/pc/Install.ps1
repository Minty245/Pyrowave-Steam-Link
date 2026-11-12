<#
  Steam Link Pyrowave - PC install (no administrator rights needed).

  Installs to %LOCALAPPDATA%\Programs\SteamLinkPyrowave:
    driver\slp\   SteamVR driver (loads the Pyrowave backend into SteamVR; registered with vrpathreg)
    app\          Pyrowave Link, the settings app (Start menu shortcut; adds itself to the SteamVR dashboard)
  plus an entry in Settings > Apps so it can be uninstalled from there. Your settings
  (%LOCALAPPDATA%\SteamLinkPyrowave\settings.json) are kept across installs and updates.

  Run with SteamVR closed. Reinstalling updates in place. Undo: Uninstall.ps1 (or Settings > Apps).
  The headset needs its own part too: see README.txt (Install-Frame.ps1 can copy it over SSH).
#>
param([switch]$Launch)   # -Launch opens Pyrowave Link at the end (off: it can bring SteamVR up)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$version = (Get-Content (Join-Path $here 'VERSION') -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $version) { $version = 'dev' }
$dest = Join-Path $env:LOCALAPPDATA 'Programs\SteamLinkPyrowave'

function Find-VrPathReg {
    $steam = $null
    try { $steam = (Get-ItemProperty 'HKCU:\SOFTWARE\Valve\Steam' -Name SteamPath).SteamPath } catch {}
    if (-not $steam) { $steam = Join-Path ${Env:ProgramFiles(x86)} 'Steam' }
    $candidates = @((Join-Path $steam 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe'))
    # SteamVR can live in another Steam library folder
    $lib = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $lib) {
        foreach ($m in (Select-String -Path $lib -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches) {
            $p = $m.Groups[1].Value -replace '\\\\', '\'
            $candidates += (Join-Path $p 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe')
        }
    }
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    throw 'SteamVR not found (vrpathreg.exe). Install SteamVR from Steam first.'
}

Write-Host "Steam Link Pyrowave $version - PC install" -ForegroundColor Cyan
# This script ships inside the release package next to driver\ and app\. In the source tree (installer\pc) those
# folders do not exist yet: build the package (scripts/build-release.sh) and run Install.cmd from the extracted zip.
foreach ($d in 'driver\slp\bin\win64\driver_slp.dll', 'app\PyrowaveLink\PyrowaveLink.exe') {
    if (-not (Test-Path (Join-Path $here $d))) {
        throw "$d is missing next to this script. Run Install.cmd from the extracted release package (SteamLinkPyrowave-PC-<version>.zip, e.g. from dist\), not from the source folder installer\pc."
    }
}
foreach ($p in 'vrserver', 'vrmonitor', 'vrcompositor') {
    if (Get-Process $p -ErrorAction SilentlyContinue) { throw 'SteamVR is running. Close SteamVR, then run the installer again.' }
}
$vrpathreg = Find-VrPathReg

# The settings app may be open (its files would be locked)
Get-Process PyrowaveLink -ErrorAction SilentlyContinue | ForEach-Object {
    Write-Host 'Closing Pyrowave Link...'
    $_.CloseMainWindow() | Out-Null
    if (-not $_.WaitForExit(3000)) { $_ | Stop-Process -Force }
}

# Copy the files
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($d in 'driver', 'app') {
    $target = Join-Path $dest $d
    if (Test-Path $target) { Remove-Item -Recurse -Force $target }
    Copy-Item -Recurse -Force (Join-Path $here $d) $target
}
foreach ($f in 'Uninstall.ps1', 'Uninstall.cmd', 'VERSION', 'README.txt', 'LICENSE.txt', 'THIRD_PARTY_NOTICES.txt') {
    $src = Join-Path $here $f
    if (Test-Path $src) { Copy-Item -Force $src $dest }
}
Write-Host "Installed files to $dest"

# SteamVR driver. SteamVR keeps its external driver list in %LOCALAPPDATA%\openvr\openvrpaths.vrpath (JSON);
# vrpathreg edits it. Drop any other registration of an "slp" driver folder (older copies, also ones that no longer
# exist), register ours, then check the file really lists it.
$driverDir = Join-Path $dest 'driver\slp'
$vrpaths = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
function Get-ExternalDrivers {
    if (-not (Test-Path $vrpaths)) { return @() }
    try { return @((Get-Content -Raw $vrpaths | ConvertFrom-Json).external_drivers) } catch { return @() }
}
foreach ($p in (Get-ExternalDrivers)) {
    if ($p -and ((Split-Path -Leaf $p.TrimEnd('\')) -ieq 'slp') -and ($p.TrimEnd('\') -ine $driverDir)) {
        & $vrpathreg removedriver $p
        Write-Host "Unregistered an older copy of the driver: $p"
    }
}
& $vrpathreg adddriver $driverDir
if ($LASTEXITCODE -ne 0) { Write-Warning "vrpathreg adddriver returned $LASTEXITCODE" }
$now = Get-ExternalDrivers
if (-not ($now | Where-Object { $_ -and $_.TrimEnd('\') -ieq $driverDir })) {
    throw "SteamVR's driver list ($vrpaths) does not contain $driverDir after vrpathreg. Please report this with the messages above."
}
foreach ($p in $now) {
    if ($p -and ((Split-Path -Leaf $p.TrimEnd('\')) -ieq 'slp') -and ($p.TrimEnd('\') -ine $driverDir)) {
        Write-Warning "SteamVR still lists another slp driver folder: $p (remove it with: vrpathreg removedriver `"$p`")"
    }
}
Write-Host "Registered the SteamVR driver: $driverDir"

# Pyrowave Link's SteamVR dashboard entry: drop a manifest that points at an older copy of the app; the app writes
# a fresh one (with this install's path) the next time it runs
Remove-Item -Force (Join-Path $env:LOCALAPPDATA 'SteamLinkPyrowave\pyrowave_link.vrmanifest') -ErrorAction SilentlyContinue

# Start menu shortcut
$exe = Join-Path $dest 'app\PyrowaveLink\PyrowaveLink.exe'
$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Pyrowave Link.lnk'
$sh = New-Object -ComObject WScript.Shell
$lnk = $sh.CreateShortcut($menu)
$lnk.TargetPath = $exe
$lnk.WorkingDirectory = Split-Path $exe
$lnk.Description = 'Steam Link Pyrowave settings'
$lnk.Save()

# Settings > Apps entry
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SteamLinkPyrowave'
New-Item -Force -Path $key | Out-Null
$un = Join-Path $dest 'Uninstall.ps1'
Set-ItemProperty -Path $key -Name DisplayName -Value 'Steam Link Pyrowave'
Set-ItemProperty -Path $key -Name DisplayVersion -Value $version
Set-ItemProperty -Path $key -Name Publisher -Value 'Steam Link Pyrowave'
Set-ItemProperty -Path $key -Name InstallLocation -Value $dest
Set-ItemProperty -Path $key -Name DisplayIcon -Value $exe
Set-ItemProperty -Path $key -Name UninstallString -Value "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$un`""
Set-ItemProperty -Path $key -Name NoModify -Value 1 -Type DWord
Set-ItemProperty -Path $key -Name NoRepair -Value 1 -Type DWord

Write-Host ''
Write-Host 'PC part installed.' -ForegroundColor Green
Write-Host 'Next: install the headset part (README.txt), then start SteamVR and connect the Frame with Steam Link.'
Write-Host 'Pyrowave Link (Start menu) changes the settings; it also adds itself to the SteamVR dashboard.'
if ($Launch) { Start-Process $exe }
