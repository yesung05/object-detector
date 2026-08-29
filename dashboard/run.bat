@echo off
setlocal
title HUNIK Dashboard

set ROOT=%~dp0..
set EXE=%ROOT%\build-windows\Release\hunik-dashboard.exe

if not exist "%EXE%" (
    echo [ERROR] hunik-dashboard.exe not found.
    echo         Run: scripts\test.ps1 -Full ... to build first.
    pause & exit /b 1
)

start "" "http://localhost:8080"
"%EXE%" --root "%ROOT%"

pause
