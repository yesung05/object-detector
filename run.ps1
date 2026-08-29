<#
.SYNOPSIS
    카메라 또는 파일 입력으로 YOLO11 person detector를 실행합니다.

.EXAMPLE
    .\run.ps1                              # 기본 카메라, 미리보기
    .\run.ps1 -Input test.mp4             # 파일 입력
    .\run.ps1 -Input test.mp4 -Output out.mp4
    .\run.ps1 -DetectEvery 5 -NoTrack     # 추론 주기 조정
    .\run.ps1 -Config config\store.example.ini -EventLog events.log
    .\run.ps1 -Metrics m.json             # 성능 지표 JSON 저장
#>

param(
    # 입력 소스
    [string]$Input      = "",         # 파일 경로. 비어 있으면 카메라 사용
    [string]$CameraDevice = "",       # 카메라 장치명 (비어 있으면 자동 감지)

    # 출력
    [string]$Output     = "",         # 어노테이션 영상 저장 경로
    [switch]$Preview    = $true,      # ffplay 미리보기 (기본 켜짐)
    [string]$Metrics    = "",         # 성능 지표 JSON 경로
    [string]$Detections = "",         # 프레임별 검출 CSV 경로
    [string]$EventLog   = "auto",      # 이벤트 로그 경로 (기본: logs\YYYYMMDD_HHMMSS.log)

    # 감지 설정
    [int]   $DetectEvery = 3,         # N 프레임마다 YOLO 실행
    [switch]$NoTrack,                 # 중간 프레임 추적 비활성화
    [float] $Confidence  = 0.25,
    [int]   $Warmup      = 2,
    [int]   $Threads     = 1,

    # 매장 설정
    [string]$Config = "",             # key=value 설정 파일 경로

    # 모델 / 경로 (기본값은 프로젝트 루트 기준)
    [string]$Model    = "",
    [string]$Provider = "cpu"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path

# ── 경로 기본값 ───────────────────────────────────────────────────────────
if (-not $Model)    { $Model = "$ROOT\yolo11n-416.onnx" }
$EXE = "$ROOT\build-windows\Release\yolo11-person.exe"

# ── 사전 확인 ─────────────────────────────────────────────────────────────
if (-not (Test-Path $EXE)) {
    Write-Host "실행 파일이 없습니다. 먼저 빌드하세요:" -ForegroundColor Red
    Write-Host "  .\scripts\test.ps1 -Full -OrtRoot C:\deps\onnxruntime -FfmpegRoot C:\deps\ffmpeg"
    exit 1
}
if (-not (Test-Path $Model)) {
    Write-Host "모델 파일을 찾을 수 없습니다: $Model" -ForegroundColor Red
    exit 1
}

# ── DLL 경로 자동 탐색 ────────────────────────────────────────────────────
$candidates = @(
    "C:\deps\ffmpeg\bin",
    "$ROOT\deps\ffmpeg\bin",
    (Split-Path (Get-Command ffmpeg.exe -ErrorAction SilentlyContinue).Source -ErrorAction SilentlyContinue)
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

$ortCandidates = @(
    "C:\deps\onnxruntime\lib",
    "$ROOT\deps\onnxruntime\lib"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

if (-not $candidates) {
    Write-Host "FFmpeg DLL을 찾을 수 없습니다. C:\deps\ffmpeg 에 설치하세요." -ForegroundColor Red
    exit 1
}
$env:PATH = "$candidates;$ortCandidates;" + $env:PATH

# ── 이벤트 로그 경로 결정 ─────────────────────────────────────────────────
if ($EventLog -eq "auto") {
    $logsDir = "$ROOT\logs"
    if (-not (Test-Path $logsDir)) { New-Item -ItemType Directory -Force $logsDir | Out-Null }
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $EventLog = "$logsDir\$stamp.log"
    Write-Host "이벤트 로그: $EventLog" -ForegroundColor DarkGray
}

# ── 인자 구성 ─────────────────────────────────────────────────────────────
$args_list = @("--model", $Model, "--provider", $Provider)

if ($Input) {
    $args_list += @("--input", $Input)
} else {
    $args_list += "--camera"
    if ($CameraDevice) { $args_list += @("--camera-device", $CameraDevice) }
}

if ($Output)     { $args_list += @("--output",     $Output)     }
if ($Preview)    { $args_list += "--preview" }
if ($Metrics)    { $args_list += @("--metrics",    $Metrics)    }
if ($Detections) { $args_list += @("--detections", $Detections) }
if ($EventLog)   { $args_list += @("--event-log",  $EventLog)   }
if ($Config)     { $args_list += @("--config",     $Config)     }

$args_list += @("--detect-every", $DetectEvery)
$args_list += @("--confidence",   $Confidence)
$args_list += @("--warmup",       $Warmup)
$args_list += @("--threads",      $Threads)
if (-not $NoTrack) { $args_list += "--track" }

# ── 실행 ──────────────────────────────────────────────────────────────────
Write-Host "실행: $EXE $args_list" -ForegroundColor Cyan
& $EXE @args_list
