@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-association.ps1" %*
set ERR=%ERRORLEVEL%
echo.
pause
exit /b %ERR%
