@echo off
rem Explorer-friendly wrapper only. The canonical build script is build.ps1.
setlocal
set "PATH=%~dp0tools\w64devkit\bin;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set ERR=%ERRORLEVEL%
echo.
if %ERR% neq 0 (
  echo Build failed.
) else (
  echo Build complete: dist\lithemd.exe
)
pause
exit /b %ERR%
