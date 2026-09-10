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
    $cams  = $raw | Select-String '\(video\)' | ForEach-Object {
        if ($_ -match '"(.+?)" \(video\)') { $Matches[1] }
    } | Where-Object { $_ }
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

# ── Tier 1 model (pose): INT8 first, then models\ auto-select, then FP32 ─────
$model = $null
foreach ($f in @("$ROOT\yolo11n-pose-416-int8.onnx")) {
    if (Test-Path $f) { $model = $f; break }
}
if ($model) {
    Write-Host "[model] $model (INT8)"
} elseif (Test-Path "$ROOT\models\") {
    $model = "$ROOT\models"
    Write-Host "[model] models\ (FP32, auto aspect-ratio)"
} else {
    foreach ($f in @("$ROOT\yolo11n-pose-416.onnx", "$ROOT\yolo11n-416.onnx", "$ROOT\yolo11n.onnx")) {
        if (Test-Path $f) { $model = $f; break }
    }
    if (-not $model) {
        Write-Host "[ERROR] No model found. Put *.onnx in models\ folder." -ForegroundColor Red
        Read-Host "Press Enter to close"
        exit 1
    }
    Write-Host "[model] $model"
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
    $cmdArgs += "--camera-size", "1280x720", "--camera-fps", "15"
}

# Tier 2 (object): INT8 first, FP32 fallback
$objModel = $null
foreach ($f in @("$ROOT\models\yolo11n_tier2_int8.onnx",
                  "$ROOT\models\yolo11n_tier2_fp32.onnx")) {
    if (Test-Path $f) { $objModel = $f; break }
}
if ($objModel) {
    Write-Host "[tier2] $objModel"
    $cmdArgs += "--obj-model", $objModel
} else {
    Write-Host "[tier2] not found -- chair/table/animal detection disabled" -ForegroundColor Yellow
}

# ── run ───────────────────────────────────────────────────────────────────────
$ErrorActionPreference = 'Continue'
& $exe @cmdArgs

Write-Host ""
Write-Host "[done] Log: $logFile" -ForegroundColor Cyan
