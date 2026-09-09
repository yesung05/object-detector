# ffmpeg stderr 등 외부 exe 출력이 ErrorRecord로 잡혀 중단되지 않도록 Continue로 설정
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding          = [System.Text.Encoding]::UTF8
$ROOT = Split-Path $MyInvocation.MyCommand.Path

# ── EXE 탐색 ──────────────────────────────────────────────────────────────
$exe = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release")) {
    if (Test-Path "$d\yolo11-person.exe") { $exe = "$d\yolo11-person.exe"; break }
}
if (-not $exe) {
    Write-Host "[ERROR] 실행 파일 없음. 먼저 빌드하세요:" -ForegroundColor Red
    Write-Host "        cmake --build build-windows --config Release"
    Read-Host "엔터 키를 눌러 닫기"
    exit 1
}

$dashboard = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release")) {
    if (Test-Path "$d\hunik-dashboard.exe") { $dashboard = "$d\hunik-dashboard.exe"; break }
}

# ── DLL PATH 추가 ──────────────────────────────────────────────────────────
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

# ── ffmpeg.exe 탐색 ────────────────────────────────────────────────────────
$ffmpeg = $null
foreach ($d in @($ffmpegBin, "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin",
                 "C:\deps\ffmpeg\bin")) {
    if ($d -and (Test-Path "$d\ffmpeg.exe")) { $ffmpeg = "$d\ffmpeg.exe"; break }
}

# ── 카메라 선택 ────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "===== hunik 무인매장 감지 시스템 =====" -ForegroundColor Cyan
Write-Host ""

$cameraDevice = $null

if ($ffmpeg) {
    Write-Host "카메라 목록 확인 중..." -NoNewline
    # 2>&1 은 stderr를 ErrorRecord로 감싸므로 .ToString()으로 문자열 추출
    $raw   = & $ffmpeg -f dshow -list_devices true -i dummy 2>&1 |
             ForEach-Object { $_.ToString() }
    $cams  = $raw | Select-String '\(video\)' | ForEach-Object {
        if ($_ -match '"(.+?)" \(video\)') { $Matches[1] }
    } | Where-Object { $_ }
    Write-Host ""

    if ($cams.Count -eq 0) {
        Write-Host "[warn] 카메라를 찾을 수 없습니다 — 기본 카메라 사용" -ForegroundColor Yellow
    } elseif ($cams.Count -eq 1) {
        Write-Host "카메라: $($cams[0]) (1개뿐이므로 자동 선택)" -ForegroundColor Green
        $cameraDevice = "video=$($cams[0])"
    } else {
        Write-Host "카메라 선택:"
        for ($i = 0; $i -lt $cams.Count; $i++) {
            Write-Host "  [$($i+1)] $($cams[$i])"
        }
        Write-Host ""
        $sel = Read-Host "번호 입력"
        $idx = [int]$sel - 1
        if ($idx -lt 0 -or $idx -ge $cams.Count) {
            Write-Host "[warn] 잘못된 번호 — 기본 카메라 사용" -ForegroundColor Yellow
        } else {
            Write-Host "선택: $($cams[$idx])" -ForegroundColor Green
            $cameraDevice = "video=$($cams[$idx])"
        }
    }
} else {
    Write-Host "[warn] ffmpeg.exe 없음 — 기본 카메라 사용" -ForegroundColor Yellow
}

# ── 모델 경로 ──────────────────────────────────────────────────────────────
$model = $null
if (Test-Path "$ROOT\models\") {
    $model = "$ROOT\models"
    Write-Host "[model] models\ (자동 화면비 선택)"
} else {
    foreach ($f in @("$ROOT\yolo11n-416.onnx", "$ROOT\yolo11n.onnx")) {
        if (Test-Path $f) { $model = $f; break }
    }
    if (-not $model) {
        Write-Host "[ERROR] 모델 파일 없음. models\ 폴더에 *.onnx 를 넣으세요." -ForegroundColor Red
        Read-Host "엔터 키를 눌러 닫기"
        exit 1
    }
    Write-Host "[model] $model"
}

# ── 이벤트 로그 ────────────────────────────────────────────────────────────
$logsDir = "$ROOT\logs"
if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory $logsDir | Out-Null }
$stamp   = Get-Date -Format "yyyyMMdd_HHmmss"
$logFile = "$logsDir\$stamp.db"
Write-Host "[log]   $logFile"

# ── 대시보드 백그라운드 실행 ───────────────────────────────────────────────
if ($dashboard) {
    Write-Host "[dash]  http://localhost:8080 (백그라운드)"
    Start-Process -FilePath $dashboard -ArgumentList "--root `"$ROOT`" --config `"$ROOT\config.json`"" -WindowStyle Hidden
} else {
    Write-Host "[dash]  dashboard 바이너리 없음"
}

Write-Host "[start] Ctrl+C 로 종료."
Write-Host ""

# ── 인자 구성 ──────────────────────────────────────────────────────────────
$args = @(
    "--model", $model,
    "--camera",
    "--provider", "cpu",
    "--detect-every", "3",
    "--track",
    "--warmup", "2",
    "--stream-port", "8081",
    "--event-log", $logFile,
    "--config", "$ROOT\config.json"
)
if ($cameraDevice) {
    $args += "--camera-format", "dshow", "--camera-device", $cameraDevice
    # QHD 이상 카메라는 버퍼 넘침 방지를 위해 해상도·fps 상한을 설정합니다.
    # 모델 입력이 416×224 수준이므로 1280×720 이상은 추론에 기여하지 않습니다.
    $args += "--camera-size", "1280x720", "--camera-fps", "15"
}
$objModel = "$ROOT\models\yolo11n_tier2_fp32.onnx"
if (Test-Path $objModel) {
    $args += "--obj-model", $objModel
}

# ── 실행 ──────────────────────────────────────────────────────────────────
$ErrorActionPreference = 'Continue'
& $exe @args

Write-Host ""
Write-Host "[done] 이벤트 로그: $logFile" -ForegroundColor Cyan
