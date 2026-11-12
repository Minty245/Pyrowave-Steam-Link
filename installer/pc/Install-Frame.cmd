@echo off
set /p FRAME=Steam Frame address (IP or hostname): 
set FC=
set FCQ=
set /p FCQ=Also enable full colour (4:4:4, 10-bit)? [y/N]: 
if /i "%FCQ%"=="y" set FC=-FullColour
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Frame.ps1" -Frame %FRAME% %FC% %*
pause
