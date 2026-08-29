@echo off
chcp 65001 >nul
echo [INFO] 시스템을 시작합니다...

:: 1. 현재 디렉토리의 run.bat을 새 창에서 실행
start "Main Service" /D "%~dp0" run.bat

:: 2. dashboard 폴더의 run.bat을 해당 폴더를 기준으로 새 창에서 실행
start "Dashboard Service" /D "%~dp0dashboard" run.bat

echo [INFO] 두 프로세스가 백그라운드(새 창)에서 실행 중입니다.
pause