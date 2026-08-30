@echo off
setlocal EnableDelayedExpansion
title YOLO11 Person Detector

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

set EXE=%ROOT%\build-windows\Release\yolo11-person.exe
set DASHBOARD=%ROOT%\build-windows\Release\hunik-dashboard.exe

if not exist "%EXE%" (
    echo [ERROR] 실행 파일이 없습니다. 먼저 빌드하세요:
    echo         scripts\test.ps1 -Full -OrtRoot C:\deps\onnxruntime -FfmpegRoot C:\deps\ffmpeg
    pause & exit /b 1
)

rem ── DLL 경로 탐색 ─────────────────────────────────────────────────────────
set FFMPEG_BIN=
for %%d in ("C:\deps\ffmpeg\bin" "%ROOT%\deps\ffmpeg\bin") do (
    if not defined FFMPEG_BIN if exist "%%~d\ffmpeg.exe" set "FFMPEG_BIN=%%~d"
)
set ORT_LIB=
for %%d in ("C:\deps\onnxruntime\lib" "%ROOT%\deps\onnxruntime\lib") do (
    if not defined ORT_LIB if exist "%%~d\onnxruntime.dll" set "ORT_LIB=%%~d"
)
if defined FFMPEG_BIN set "PATH=%FFMPEG_BIN%;%PATH%"
if defined ORT_LIB   set "PATH=%ORT_LIB%;%PATH%"

rem ── 모델 경로 결정 ────────────────────────────────────────────────────────
rem   models\ 폴더가 있으면 비율 자동 선택, 없으면 루트의 단일 파일로 fallback
set MODEL=
if exist "%ROOT%\models\" (
    set "MODEL=%ROOT%\models"
    echo [model] models\ ^(비율 자동 선택^)
) else (
    for %%f in (
        "%ROOT%\yolo11n-416.onnx"
        "%ROOT%\yolo11n.onnx"
    ) do (
        if not defined MODEL if exist "%%~f" set "MODEL=%%~f"
    )
    if not defined MODEL (
        echo [ERROR] models\ 폴더와 루트 ONNX 파일을 모두 찾지 못했습니다.
        pause & exit /b 1
    )
    echo [model] %MODEL%
)

rem ── 이벤트 로그 경로 ──────────────────────────────────────────────────────
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOGFILE=%ROOT%\logs\%STAMP%.log
echo [log]   %LOGFILE%

rem ── 대시보드 백그라운드 실행 ──────────────────────────────────────────────
if exist "%DASHBOARD%" (
    echo [dash]  http://localhost:8080 ^(백그라운드^)
    start "" /B "%DASHBOARD%" --root "%ROOT%\dashboard" --config "%ROOT%\config.json"
) else (
    echo [dash]  대시보드 실행 파일 없음 ^(빌드하면 자동으로 시작됩니다^)
)

echo [start] Ctrl+C 로 종료합니다.
echo.

rem --preview 는 기본으로 켜지 않습니다. ffplay 는 별도 프로세스로 코어 하나를
rem 차지하고, 프레임마다 raw RGB 를 파이프로 보냅니다(720p 15fps 기준 41MB/s).
rem 화면 확인은 대시보드 스트림(http://localhost:8081/stream)을 쓰세요.
"%EXE%" ^
    --model "%MODEL%" ^
    --camera ^
    --camera-fps 15 ^
    --provider cpu ^
    --detect-every 3 ^
    --track ^
    --warmup 2 ^
    --stream-port 8081 ^
    --event-log "%LOGFILE%" ^
    --config "%ROOT%\config.json"

echo.
echo [done] 이벤트 로그: %LOGFILE%
pause
