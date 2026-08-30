@echo off
setlocal EnableDelayedExpansion
title YOLO11 Person Detector

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

set EXE=%ROOT%\build-windows\Release\yolo11-person.exe
if not exist "%EXE%" (
    echo [ERROR] Executable not found. Run: scripts\test.ps1 -Full ...
    pause & exit /b 1
)

set MODEL=
for %%f in (
    "%ROOT%\yolo11n-pose-416.onnx"
    "%ROOT%\yolo11n-416.onnx"
    "%ROOT%\yolo11n.onnx"
) do (
    if not defined MODEL if exist "%%~f" set "MODEL=%%~f"
)
if not defined MODEL (
    echo [ERROR] No ONNX model found in project root.
    pause & exit /b 1
)
echo [model] %MODEL%

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

if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOGFILE=%ROOT%\logs\%STAMP%.log
echo [log] %LOGFILE%

echo [start] Press Ctrl+C to stop.
echo.

rem --preview 는 기본으로 켜지 않습니다. ffplay 는 별도 프로세스로 코어 하나를
rem 차지하고, 프레임마다 raw RGB 를 파이프로 보냅니다(720p 15fps 기준 41MB/s).
rem 배포 대상 i5-4200U 에서는 이것만으로 예산의 상당 부분이 사라집니다.
rem 화면 확인이 필요하면 대시보드(http://localhost:8081/stream)를 쓰거나
rem 아래 줄에 --preview 를 직접 붙이세요.
"%EXE%" --model "%MODEL%" --camera --camera-size 1280x720 --camera-fps 15 --provider cpu --detect-every 3 --track --warmup 2 --event-log "%LOGFILE%" --config "%ROOT%\config.json"

echo.
echo [done] Event log: %LOGFILE%
pause
