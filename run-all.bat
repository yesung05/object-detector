@echo off
chcp 65001 > nul
setlocal EnableDelayedExpansion
title YOLO11 Person Detector

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

set EXE=%ROOT%\build-windows\Release\yolo11-person.exe
set DASHBOARD=%ROOT%\build-windows\Release\hunik-dashboard.exe

if not exist "%EXE%" (
    echo [ERROR] Executable not found. Build first:
    echo         scripts\test.ps1 -Full -OrtRoot C:\deps\onnxruntime -FfmpegRoot C:\deps\ffmpeg
    pause & exit /b 1
)

rem -- DLL path search --
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

rem -- Model path: use models\ dir for auto aspect-ratio selection,
rem    fall back to single file in project root --
set MODEL=
if exist "%ROOT%\models\" (
    set "MODEL=%ROOT%\models"
    echo [model] models\ ^(auto aspect-ratio selection^)
) else (
    for %%f in (
        "%ROOT%\yolo11n-416.onnx"
        "%ROOT%\yolo11n.onnx"
    ) do (
        if not defined MODEL if exist "%%~f" set "MODEL=%%~f"
    )
    if not defined MODEL (
        echo [ERROR] No model found. Put *.onnx files in models\ or project root.
        pause & exit /b 1
    )
    echo [model] %MODEL%
)

rem -- Event log path --
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOGFILE=%ROOT%\logs\%STAMP%.log
echo [log]   %LOGFILE%

rem -- Start dashboard in background --
if exist "%DASHBOARD%" (
    echo [dash]  http://localhost:8080 ^(background^)
    start "" /B "%DASHBOARD%" --root "%ROOT%" --config "%ROOT%\config.json"
) else (
    echo [dash]  dashboard binary not found ^(build to enable^)
)

echo [start] Press Ctrl+C to stop.
echo.

rem -- Preview (ffplay) is off by default.
rem    ffplay uses a full core + 41MB/s pipe writes at 720p 15fps.
rem    Use the dashboard stream at http://localhost:8081/stream instead.
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
echo [done] Event log: %LOGFILE%
pause
