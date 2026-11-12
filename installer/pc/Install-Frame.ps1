<#
  Steam Link Pyrowave - copy the headset part to the Steam Frame over SSH and install it there.

  Optional: the usual way is to download the Frame package on the headset itself (README.txt). This needs SSH on the
  Frame (Desktop Mode, Konsole: "passwd" to set a password once, then "sudo systemctl enable --now sshd") and the
  Windows OpenSSH client (built into Windows 10/11). You type the Frame password yourself when ssh asks; it is never
  stored. Nothing on the Frame is changed outside ~/.local (see install.sh).

    .\Install-Frame.ps1 -Frame <frame-address>              e.g. -Frame 192.168.1.50 or -Frame steamframe.local
    .\Install-Frame.ps1 -Frame <frame-address> -FullColour  also enables full colour (4:4:4, 10-bit)
    .\Install-Frame.ps1 -Frame <frame-address> -Uninstall   removes it from the Frame
#>
param(
    [Parameter(Mandatory = $true)][string]$Frame,
    [string]$User = 'steamos',
    [switch]$FullColour,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue
$scp = Get-Command scp.exe -ErrorAction SilentlyContinue
if (-not $ssh -or -not $scp) { throw 'The OpenSSH client is missing: Settings > System > Optional features > OpenSSH Client.' }
$target = "$User@$Frame"

if ($Uninstall) {
    & ssh.exe -t $target 'bash ~/.local/lib/slp/uninstall.sh'
    exit $LASTEXITCODE
}

$pkg = Get-ChildItem -Path (Join-Path $PSScriptRoot 'frame') -Filter 'SteamLinkPyrowave-Frame-*.tar.gz' | Select-Object -First 1
if (-not $pkg) { throw 'Frame package not found next to this script (frame\SteamLinkPyrowave-Frame-*.tar.gz).' }
$dir = $pkg.Name -replace '\.tar\.gz$', ''
Write-Host "Copying $($pkg.Name) to $target (ssh asks for the Frame password)..."
& scp.exe $pkg.FullName "${target}:/tmp/$($pkg.Name)"
if ($LASTEXITCODE -ne 0) { throw 'Copy failed (is SSH enabled on the Frame, and the address right?).' }
$opt = if ($FullColour) { ' --full-colour' } else { '' }
$cmd = "set -e; cd /tmp && rm -rf '$dir' && tar -xzf '$($pkg.Name)' && bash '$dir/install.sh'$opt; rm -rf '/tmp/$dir' '/tmp/$($pkg.Name)'"
Write-Host 'Installing on the Frame...'
& ssh.exe -t $target $cmd
if ($LASTEXITCODE -ne 0) { throw 'Install on the Frame failed (see the messages above).' }
Write-Host 'Headset part installed. Restart SteamVR on the Frame (or reboot it).' -ForegroundColor Green
