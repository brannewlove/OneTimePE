@echo off
setlocal
title OneTimePE Build

echo ==================================================================
echo [OneTimePE] Starting build script...
echo ==================================================================

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if errorlevel 1 goto BUILD_FAIL

echo ==================================================================
echo [BUILD SUCCESS] Build finished successfully!
echo ==================================================================
pause
exit /b 0

:BUILD_FAIL
echo ==================================================================
echo [BUILD FAILED] An error occurred during build.
echo Please check the error messages above.
echo ==================================================================
pause
exit /b 1