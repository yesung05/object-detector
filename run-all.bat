@echo off
chcp 65001 > nul
setlocal EnableDelayedExpansion
title YOLO11 Person Detector

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%

rem -- EXE: build-windows\Release (CMake/MSVC) 우선, 없으면 build\Release (Make/GCC) --
set EXE=
for %%d in ("%ROOT%\build-windows\Release" "%ROOT%\build\Release") do (
    if not defined EXE if exist "%%~d\yolo11-person.exe" set "EXE=%%~d\yolo11-person.exe"
)
set DASHBOARD=
for %%d in ("%ROOT%\build-windows\Release" "%ROOT%\build\Release") do (
    if not defined DASHBOARD if exist "%%~d\hunik-dashboard.exe" set "DASHBOARD=%%~d\hunik-dashboard.exe"
)

if not defined EXE (
    echo [ERROR] 실행 파일 없음. 먼저 빌드하세요:
    echo         cmake --build build-windows --config Release
    pause & exit /b 1
)

rem -- DLL 탐색 --
set FFMPEG_BIN=
for %%d in ("%ROOT%\build-windows\Release" "%ROOT%\build\Release" "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin" "C:\deps\ffmpeg\bin" "%ROOT%\deps\ffmpeg\bin") do (
    if not defined FFMPEG_BIN if exist "%%~d\avcodec-63.dll" set "FFMPEG_BIN=%%~d"
)
set ORT_LIB=
for %%d in ("%ROOT%\build-windows\Release" "%ROOT%\build\Release" "C:\dev\onnxruntime-win-x64-1.26.0\lib" "C:\deps\onnxruntime\lib" "%ROOT%\deps\onnxruntime\lib") do (
    if not defined ORT_LIB if exist "%%~d\onnxruntime.dll" set "ORT_LIB=%%~d"
)
if defined FFMPEG_BIN set "PATH=%FFMPEG_BIN%;%PATH%"
if defined ORT_LIB   set "PATH=%ORT_LIB%;%PATH%"

rem -- 카메라 선택 (PowerShell로 dshow 장치 목록 파싱) --
echo.
echo ===== hunik 무인매장 감지 시스템 =====
echo.

set FFMPEG_EXE=
for %%d in ("%ROOT%\build-windows\Release" "%ROOT%\build\Release" "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin" "C:\deps\ffmpeg\bin") do (
    if not defined FFMPEG_EXE if exist "%%~d\ffmpeg.exe" set "FFMPEG_EXE=%%~d\ffmpeg.exe"
)

set CAMERA_DEVICE=
if not defined FFMPEG_EXE (
    echo [warn] ffmpeg.exe 없음 — 기본 카메라 사용
    goto :skip_cam_select
)

echo 카메라 목록 확인 중...
set CAM_LIST_PS=%TEMP%\hunik_camlist.ps1
echo $ff = '%FFMPEG_EXE%' > "%CAM_LIST_PS%"
echo $raw = ^& $ff -f dshow -list_devices true -i dummy 2^>^&1 >> "%CAM_LIST_PS%"
echo $cams = $raw ^| Select-String '\(video\)' ^| ForEach-Object { if ($_ -match '"(.+?)" \(video\)') { $Matches[1] } } ^| Where-Object { $_ } >> "%CAM_LIST_PS%"
echo if ($cams.Count -eq 0) { Write-Host '카메라 없음'; exit 1 } >> "%CAM_LIST_PS%"
echo Write-Host '' >> "%CAM_LIST_PS%"
echo Write-Host '카메라 선택:' >> "%CAM_LIST_PS%"
echo for ($i = 0; $i -lt $cams.Count; $i++) { Write-Host "  [$($i+1)] $($cams[$i])" } >> "%CAM_LIST_PS%"
echo Write-Host '' >> "%CAM_LIST_PS%"
echo if ($cams.Count -eq 1) { Write-Host "  카메라가 1개뿐이므로 자동 선택합니다."; $idx = 0 } >> "%CAM_LIST_PS%"
echo else { $sel = Read-Host '번호 입력'; $idx = [int]$sel - 1 } >> "%CAM_LIST_PS%"
echo if ($idx -lt 0 -or $idx -ge $cams.Count) { Write-Host '잘못된 번호.'; exit 1 } >> "%CAM_LIST_PS%"
echo Write-Host "선택: $($cams[$idx])" >> "%CAM_LIST_PS%"
echo "video=$($cams[$idx])" ^| Out-File -Encoding ascii '%TEMP%\hunik_camsel.txt' -NoNewline >> "%CAM_LIST_PS%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%CAM_LIST_PS%"
if errorlevel 1 (
    echo [warn] 카메라 선택 실패 — 기본 카메라 사용
    goto :skip_cam_select
)
set /p CAMERA_DEVICE=<"%TEMP%\hunik_camsel.txt"
del "%TEMP%\hunik_camsel.txt" 2>nul
del "%CAM_LIST_PS%" 2>nul

:skip_cam_select

rem -- 모델 경로 --
set MODEL=
if exist "%ROOT%\models\" (
    set "MODEL=%ROOT%\models"
    echo [model] models\ ^(자동 화면비 선택^)
) else (
    for %%f in ("%ROOT%\yolo11n-416.onnx" "%ROOT%\yolo11n.onnx") do (
        if not defined MODEL if exist "%%~f" set "MODEL=%%~f"
    )
    if not defined MODEL (
        echo [ERROR] 모델 파일 없음. models\ 폴더에 *.onnx 를 넣으세요.
        pause & exit /b 1
    )
    echo [model] %MODEL%
)

rem -- Tier 2 모델 --
set OBJ_MODEL_ARG=
if exist "%ROOT%\models\yolo11n_tier2_fp32.onnx" (
    set "OBJ_MODEL_ARG=--obj-model "%ROOT%\models\yolo11n_tier2_fp32.onnx""
)

rem -- 이벤트 로그 --
if not exist "%ROOT%\logs" mkdir "%ROOT%\logs"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOGFILE=%ROOT%\logs\%STAMP%.log
echo [log]   %LOGFILE%

rem -- 대시보드 백그라운드 실행 --
if defined DASHBOARD (
    echo [dash]  http://localhost:8080 ^(백그라운드^)
    start "" /B "%DASHBOARD%" --root "%ROOT%" --config "%ROOT%\config.json"
) else (
    echo [dash]  dashboard 바이너리 없음
)

echo [start] Ctrl+C 로 종료.
echo.

rem -- 카메라 장치 인자 구성 --
set CAM_ARG=
if defined CAMERA_DEVICE set "CAM_ARG=--camera-format dshow --camera-device "%CAMERA_DEVICE%""

"%EXE%" ^
    --model "%MODEL%" ^
    --camera ^
    %CAM_ARG% ^
    --provider cpu ^
    --detect-every 3 ^
    --track ^
    --warmup 2 ^
    --stream-port 8081 ^
    --event-log "%LOGFILE%" ^
    --config "%ROOT%\config.json" ^
    %OBJ_MODEL_ARG%

echo.
echo [done] 이벤트 로그: %LOGFILE%
pause
