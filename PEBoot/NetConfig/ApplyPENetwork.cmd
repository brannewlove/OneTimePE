@echo off
setlocal
set "BASE_DIR=%~dp0.."
if exist "%BASE_DIR%\OneTimePE.exe" (
    "%BASE_DIR%\OneTimePE.exe" --pe-apply-net
    exit /b %ERRORLEVEL%
)
for /r "%~dp0Drivers" %%F in (*.inf) do drvload.exe "%%~fF" >nul 2>&1
wpeutil.exe InitializeNetwork >nul 2>&1
for %%X in ("%~dp0WiFi\*.xml") do netsh.exe wlan add profile filename="%%~fX" user=all >nul 2>&1
endlocal
