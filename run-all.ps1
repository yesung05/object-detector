# ffmpeg stderr captured as ErrorRecord -- keep Continue so it doesn't abort
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding          = [System.Text.Encoding]::UTF8
$ROOT = Split-Path $MyInvocation.MyCommand.Path

# ── find exe ──────────────────────────────────────────────────────────────────
$exe = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release")) {
    if (Test-Path "$d\yolo11-person.exe") { $exe = "$d\yolo11-person.exe"; break }
}
if (-not $exe) {
    Write-Host "[ERROR] Executable not found. Build first:" -ForegroundColor Red
    Write-Host "        cmake --build build-windows --config Release"
    Read-Host "Press Enter to close"
    exit 1
}

$dashboard = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release")) {
    if (Test-Path "$d\hunik-dashboard.exe") { $dashboard = "$d\hunik-dashboard.exe"; break }
}

# ── DLL paths ─────────────────────────────────────────────────────────────────
$ffmpegBin = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release",
                 "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin",
                 "C:\deps\ffmpeg\bin")) {
    if (Test-Path "$d\avcodec-63.dll") { $ffmpegBin = $d; break }
}
$ortLib = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release",
                 "C:\dev\onnxruntime-win-x64-1.26.0\lib",
                 "C:\deps\onnxruntime\lib")) {
    if (Test-Path "$d\onnxruntime.dll") { $ortLib = $d; break }
}
if ($ffmpegBin) { $env:PATH = "$ffmpegBin;$env:PATH" }
if ($ortLib)    { $env:PATH = "$ortLib;$env:PATH" }

# ── ffmpeg.exe ────────────────────────────────────────────────────────────────
$ffmpeg = $null
foreach ($d in @($ffmpegBin, "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin",
                 "C:\deps\ffmpeg\bin")) {
    if ($d -and (Test-Path "$d\ffmpeg.exe")) { $ffmpeg = "$d\ffmpeg.exe"; break }
}

# ── camera selection ──────────────────────────────────────────────────────────
Write-Host ""
Write-Host "===== HUNIK unmanned store detection system =====" -ForegroundColor Cyan
Write-Host ""

$cameraDevice = $null

if ($ffmpeg) {
    Write-Host "Scanning cameras..." -NoNewline
    $raw   = & $ffmpeg -f dshow -list_devices true -i dummy 2>&1 |
             ForEach-Object { $_.ToString() }
    $cams  = @()
    foreach ($line in $raw) {
        if ($line -match '"(.+?)" \(video\)' -and $Matches[1].Length -gt 1) {
            $cams += $Matches[1]
        }
    }
    Write-Host ""

    if ($cams.Count -eq 0) {
        Write-Host "[warn] No cameras found -- using default" -ForegroundColor Yellow
    } elseif ($cams.Count -eq 1) {
        Write-Host "Camera: $($cams[0]) (auto-selected)" -ForegroundColor Green
        $cameraDevice = "video=$($cams[0])"
    } else {
        Write-Host "Select camera:"
        for ($i = 0; $i -lt $cams.Count; $i++) {
            Write-Host "  [$($i+1)] $($cams[$i])"
        }
        Write-Host ""
        $sel = Read-Host "Enter number"
        $idx = [int]$sel - 1
        if ($idx -lt 0 -or $idx -ge $cams.Count) {
            Write-Host "[warn] Invalid -- using default" -ForegroundColor Yellow
        } else {
            Write-Host "Selected: $($cams[$idx])" -ForegroundColor Green
            $cameraDevice = "video=$($cams[$idx])"
        }
    }
} else {
    Write-Host "[warn] ffmpeg.exe not found -- using default camera" -ForegroundColor Yellow
}

# ── Tier 1 model (pose): FP32 first ───────────────────────────────────────────
# INT8 is NOT the default. Measured on this codebase (docs/quantization-benchmark-2026-09-20.md
# and re-measured independently): at the same 416x416 input the C pipeline inference p50 is
#   FP32 26.6 ms  vs  INT8 46.2 ms   -- INT8 is ~1.7x SLOWER, not faster.
# ORT's CPU INT8 kernels only pay off with VNNI instructions. Neither the dev machine
# (i7-8700, Coffee Lake) nor the deployment target (i5-4200U, Haswell) has VNNI, so the
# quantize/dequantize overhead around each op dominates. Memory saving is only ~7 MB RSS.
# INT8 accuracy (mAP / pose AP) has never been evaluated either -- see the doc.
#
# If you deploy on newer hardware, re-measure with scripts\bench-models.ps1 and, only if
# INT8 actually wins there, move it back to the front of this list.
#
# models\ is preferred over a single file because model_select picks the input size closest
# to the camera aspect ratio, which matters more than precision: 416x416 26.7 ms -> 416x224 14.0 ms.
# On equal aspect fit it prefers the LARGEST file, i.e. the original FP32 over distilled variants.
$model = $null
if ((Test-Path "$ROOT\models") -and
    (Get-ChildItem "$ROOT\models" -Filter "*-*x*.onnx" -File -ErrorAction SilentlyContinue)) {
    $model = "$ROOT\models"
    Write-Host "[model] models\ (FP32, auto aspect-ratio, non-distilled)"
}
if (-not $model) {
    foreach ($f in @("$ROOT\yolo11n-pose-416.onnx", "$ROOT\yolo11n-416.onnx", "$ROOT\yolo11n.onnx")) {
        if (Test-Path $f) { $model = $f; break }
    }
    if ($model) { Write-Host "[model] $model (FP32)" }
}
if (-not $model) {
    # Last resort only -- slower here, but better than not running at all.
    if (Test-Path "$ROOT\yolo11n-pose-416-int8.onnx") {
        $model = "$ROOT\yolo11n-pose-416-int8.onnx"
        Write-Host "[model] $model (INT8 fallback -- no FP32 model found)" -ForegroundColor Yellow
    }
}
if (-not $model) {
    Write-Host "[ERROR] No model found. Put *.onnx in models\ folder." -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

# ── event log ─────────────────────────────────────────────────────────────────
$logsDir = "$ROOT\logs"
if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory $logsDir | Out-Null }
$stamp   = Get-Date -Format "yyyyMMdd_HHmmss"
$logFile = "$logsDir\$stamp.db"
Write-Host "[log]   $logFile"

# ── dashboard (background) ────────────────────────────────────────────────────
if ($dashboard) {
    Write-Host "[dash]  http://localhost:8080 (background)"
    Start-Process -FilePath $dashboard -ArgumentList "--root `"$ROOT`" --config `"$ROOT\config.json`"" -WindowStyle Hidden
} else {
    Write-Host "[dash]  dashboard binary not found"
}

Write-Host "[start] Press Ctrl+C to stop."
Write-Host ""

# ── build argument list ───────────────────────────────────────────────────────
$cmdArgs = @(
    "--model", $model,
    "--camera",
    "--provider", "cpu",
    "--detect-every", "5",
    "--track",
    "--warmup", "2",
    "--confidence", "0.20",
    "--threads", "3",
    "--stream-port", "8081",
    "--event-log", $logFile,
    "--config", "$ROOT\config.json"
)
if ($cameraDevice) {
    $cmdArgs += "--camera-format", "dshow", "--camera-device", $cameraDevice
    # [수정 2026-09-21] HP TrueVision 등 내장 카메라는 YUY2에서 15fps를 지원하지 않아
    # avformat_open_input I/O error가 발생했음. 30fps는 거의 모든 dshow 카메라가 지원.
    # 발열 억제(15fps)는 media_ffmpeg.c 3차 폴백 + --detect-every 5 조합으로 대응.
    $cmdArgs += "--camera-size", "1280x720", "--camera-fps", "30"
}

# Tier 2 (object): FP32 first, INT8 fallback -- same reasoning as Tier 1 above.
$objModel = $null
foreach ($f in @("$ROOT\models\yolo11n_tier2_fp32.onnx",
                  "$ROOT\models\yolo11n_tier2_int8.onnx")) {
    if (Test-Path $f) { $objModel = $f; break }
}
if ($objModel) {
    Write-Host "[tier2] $objModel"
    $cmdArgs += "--obj-model", $objModel
} else {
    Write-Host "[tier2] not found -- chair/table/animal detection disabled" -ForegroundColor Yellow
}

# ── run (supervisor loop) ─────────────────────────────────────────────────────
# detector cannot restart itself, so it signals intent through the exit code:
#   0   normal stop (user pressed Ctrl+C)  -> we stop too
#   10  scheduled restart                  -> relaunch immediately, no backoff
#   3   camera lost (reconnect gave up)    -> relaunch; new process re-detects the camera
#   any other                              -> crash; relaunch with growing backoff
#
# The log file stays the same across restarts so the dashboard does not lose the
# day's history when a scheduled restart happens mid-session.
#
# Crash backoff exists to avoid a spin loop when startup itself fails (missing
# model, camera in use). We never give up permanently: this is an unattended
# store, and a camera that is unplugged at boot must be picked up once someone
# plugs it back in. Repeated quick failures only stretch the wait (max 300s) and
# are written to logs\restarts.log so the problem is still visible.
# Remote stop: create logs\STOP; the loop exits after the current run ends.
$ErrorActionPreference = 'Continue'
$backoff  = 5
$quickFails = 0
$restartLog = Join-Path $ROOT 'logs\restarts.log'
$stopFile   = Join-Path $ROOT 'logs\STOP'
if (Test-Path $stopFile) { Remove-Item $stopFile -Force }
while ($true) {
    $startedAt = Get-Date
    & $exe @cmdArgs
    $code = $LASTEXITCODE
    $ranSeconds = ((Get-Date) - $startedAt).TotalSeconds
    $when = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')

    if ($code -eq 0) {
        Add-Content -Path $restartLog -Value "$when exit=0 runtime=$([int]$ranSeconds)s action=stop"
        break
    }
    if (Test-Path $stopFile) {
        Add-Content -Path $restartLog -Value "$when exit=$code action=stop reason=STOP_file"
        Remove-Item $stopFile -Force
        break
    }

    if ($code -eq 10) {
        Write-Host ""
        Write-Host "[restart] scheduled restart -- relaunching" -ForegroundColor Cyan
        Add-Content -Path $restartLog -Value "$when exit=10 runtime=$([int]$ranSeconds)s action=scheduled_restart"
        $backoff = 5; $quickFails = 0
        Start-Sleep -Seconds 2
        continue
    }

    # Crash / camera-lost path.
    if ($ranSeconds -lt 30) { $quickFails++ } else { $quickFails = 0; $backoff = 5 }
    if ($quickFails -ge 5) {
        # Keep trying, but slowly, and say so loudly.
        $backoff = 300
        Write-Host ""
        Write-Host "[ERROR] detector exited $code within 30s, $quickFails times in a row." -ForegroundColor Red
        Write-Host "        Retrying every ${backoff}s. Check the model path, camera, and the log:" -ForegroundColor Red
        Write-Host "        $logFile"
    }
    $reason = if ($code -eq 3) { 'camera_lost' } else { 'crash' }
    Add-Content -Path $restartLog -Value "$when exit=$code runtime=$([int]$ranSeconds)s action=restart reason=$reason wait=${backoff}s quick_fails=$quickFails"
    Write-Host ""
    Write-Host "[warn] detector exited with code $code ($reason) -- relaunching in ${backoff}s" -ForegroundColor Yellow
    Start-Sleep -Seconds $backoff
    if (Test-Path $stopFile) {
        Add-Content -Path $restartLog -Value "$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss')) action=stop reason=STOP_file"
        Remove-Item $stopFile -Force
        break
    }
    $backoff = [Math]::Min($backoff * 2, 300)
}

Write-Host ""
Write-Host "[done] Log: $logFile" -ForegroundColor Cyan
