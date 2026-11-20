@echo off
setlocal
set "PATH=%~dp0tools\w64devkit\bin;%PATH%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set ERR=%ERRORLEVEL%
echo.
if %ERR% neq 0 (
  echo Build failed.
) else (
  echo Build complete: dist\openmd.exe
)
pause
exit /b %ERR%
