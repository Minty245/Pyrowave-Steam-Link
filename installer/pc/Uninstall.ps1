<#
  Steam Link Pyrowave - PC uninstall (no administrator rights needed). Run with SteamVR closed.
  Removes the SteamVR driver registration, the installed files, the Start menu shortcut and the Settings > Apps entry.
  Keeps your settings and logs in %LOCALAPPDATA%\SteamLinkPyrowave unless you pass -RemoveSettings.
  The headset part is removed on the Frame: ~/.local/lib/slp/uninstall.sh
#>
param([switch]$RemoveSettings)
$ErrorActionPreference = 'Stop'
$dest = Join-Path $env:LOCALAPPDATA 'Programs\SteamLinkPyrowave'
foreach ($p in 'vrserver', 'vrmonitor', 'vrcompositor') {
    if (Get-Process $p -ErrorAction SilentlyContinue) { throw 'SteamVR is running. Close SteamVR, then run the uninstaller again.' }
}
Get-Process PyrowaveLink -ErrorAction SilentlyContinue | Stop-Process -Force

$steam = $null
try { $steam = (Get-ItemProperty 'HKCU:\SOFTWARE\Valve\Steam' -Name SteamPath).SteamPath } catch {}
if (-not $steam) { $steam = Join-Path ${Env:ProgramFiles(x86)} 'Steam' }
$vrpathreg = Join-Path $steam 'steamapps\common\SteamVR\bin\win64\vrpathreg.exe'
if (Test-Path $vrpathreg) {
    & $vrpathreg removedriver (Join-Path $dest 'driver\slp') | Out-Null
    Write-Host 'Unregistered the SteamVR driver.'
} else {
    Write-Warning 'vrpathreg.exe not found; if SteamVR lives in another library, the driver entry stays (its folder is gone, so SteamVR skips it).'
}

$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Pyrowave Link.lnk'
Remove-Item -Force $menu -ErrorAction SilentlyContinue
Remove-Item -Force -Recurse 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SteamLinkPyrowave' -ErrorAction SilentlyContinue
$data = Join-Path $env:LOCALAPPDATA 'SteamLinkPyrowave'
# SteamVR dashboard entry: without its manifest file SteamVR drops the overlay
Remove-Item -Force (Join-Path $data 'pyrowave_link.vrmanifest') -ErrorAction SilentlyContinue
if ($RemoveSettings) { Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue; Write-Host 'Removed settings and logs.' }

# The script itself lives in $dest: remove the folder from a temporary copy of PowerShell's working directory
Set-Location $env:TEMP
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
Write-Host 'Steam Link Pyrowave removed from this PC.' -ForegroundColor Green
if (-not $RemoveSettings) { Write-Host "Settings kept in $data (Uninstall.ps1 -RemoveSettings removes them)." }
