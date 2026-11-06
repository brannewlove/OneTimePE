@echo off
setlocal
chcp 65001 >nul
title OneTimePE 자동 빌드

echo [OneTimePE] 원클릭 빌드를 시작합니다...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
set BUILD_RESULT=%ERRORLEVEL%

if %BUILD_RESULT% neq 0 (
    echo.
    echo ---------------------------------------------------------------
    echo [빌드 실패] 오류가 발생했습니다. (종료 코드: %BUILD_RESULT%)
    echo 위의 상세 로그를 확인해 주세요.
    echo ---------------------------------------------------------------
    pause
    exit /b %BUILD_RESULT%
)

echo.
echo 빌드가 성공적으로 완료되었습니다! 아무 키나 누르면 창을 닫습니다.
pause >nul

