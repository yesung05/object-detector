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

"%EXE%" --model "%MODEL%" --camera --camera-size 1280x720 --camera-fps 15 --preview --provider cpu --detect-every 3 --track --warmup 2 --event-log "%LOGFILE%" --stream-port 8081

echo.
echo [done] Event log: %LOGFILE%
pause
