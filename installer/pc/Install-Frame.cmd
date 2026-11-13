@echo off
set /p FRAME=Steam Frame address (IP or hostname): 
set FC=
set FCQ=
set /p FCQ=Also enable full colour (4:4:4, 10-bit)? [y/N]: 
if /i "%FCQ%"=="y" set FC=-FullColour
echo The Frame needs a reboot afterwards. Without it, it may not shut down cleanly the first time.
set RB=-Reboot
set RBQ=
set /p RBQ=Reboot the Frame when the install is done? [Y/n]: 
if /i "%RBQ%"=="n" set RB=-NoReboot
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Frame.ps1" -Frame %FRAME% %FC% %RB% %*
pause
