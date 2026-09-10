@echo off
chcp 65001 > nul
setlocal EnableDelayedExpansion
title HUNIK Detector

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

set EXE=%ROOT%\build-windows\Release\yolo11-person.exe
if not exist "%EXE%" (
    echo [ERROR] Executable not found. Build first:
    echo         cmake --build build-windows --config Release
    pause & exit /b 1
)

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

rem ---- Tier 1 (pose): INT8 first, then FP32 ----
set MODEL=
for %%f in (
    "%ROOT%\yolo11n-pose-416-int8.onnx"
    "%ROOT%\yolo11n-pose-416.onnx"
    "%ROOT%\yolo11n-416.onnx"
    "%ROOT%\yolo11n.onnx"
) do (
    if not defined MODEL if exist "%%~f" set "MODEL=%%~f"
)
if not defined MODEL if exist "%ROOT%\models\" set "MODEL=%ROOT%\models"
if not defined MODEL (
    echo [ERROR] No ONNX model found.
    pause & exit /b 1
)
echo [model] %MODEL%

rem ---- Tier 2 (obj): INT8 first, then FP32 ----
set OBJMODEL=
for %%f in (
    "%ROOT%\models\yolo11n_tier2_int8.onnx"
    "%ROOT%\models\yolo11n_tier2_fp32.onnx"
) do (
    if not defined OBJMODEL if exist "%%~f" set "OBJMODEL=%%~f"
)
if defined OBJMODEL (
    echo [tier2] %OBJMODEL%
) else (
    echo [tier2] not found -- chair/table/animal detection disabled
)

if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOGFILE=%ROOT%\logs\%STAMP%.db
echo [log]   %LOGFILE%

echo [start] Ctrl+C to stop.
echo.

if defined OBJMODEL (
    "%EXE%" --model "%MODEL%" --camera --camera-size 1280x720 --camera-fps 15 --provider cpu --detect-every 3 --track --warmup 2 --stream-port 8081 --event-log "%LOGFILE%" --config "%ROOT%\config.json" --obj-model "%OBJMODEL%"
) else (
    "%EXE%" --model "%MODEL%" --camera --camera-size 1280x720 --camera-fps 15 --provider cpu --detect-every 3 --track --warmup 2 --stream-port 8081 --event-log "%LOGFILE%" --config "%ROOT%\config.json"
)

echo.
echo [done] Log: %LOGFILE%
pause
