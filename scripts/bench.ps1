# FP32 vs INT8 추론 성능 비교 스크립트
#
# 사용법:
#   .\scripts\bench.ps1 -Input test.mp4 -Fp32Model yolo11n-pose-416.onnx -Int8Model yolo11n-pose-416-int8.onnx
#
# --metrics JSON을 임시 파일에 쓰고 inference_seconds / inference_p95_ms / cpu_ms_per_frame 를
# 표로 비교합니다. INT8 모델이 없으면 FP32만 측정합니다.

param (
    [Parameter(Mandatory=$true)]  [string]$Input,
    [Parameter(Mandatory=$true)]  [string]$Fp32Model,
    [string]$Int8Model   = "",
    [string]$Exe         = ".\build\Release\yolo11_person.exe",
    [int]   $DetectEvery = 1,
    [int]   $MaxFrames   = 300,
    [int]   $Warmup      = 3
)

function Measure-Model {
    param([string]$Label, [string]$Model)

    $metrics = [System.IO.Path]::GetTempFileName() + ".json"
    $args_list = @(
        "--model", $Model,
        "--input", $Input,
        "--detect-every", $DetectEvery,
        "--warmup", $Warmup,
        "--max-frames", $MaxFrames,
        "--provider", "cpu",
        "--metrics", $metrics
    )
    Write-Host "측정 중: $Label ..."
    & $Exe @args_list 2>$null
    if (-not (Test-Path $metrics)) {
        Write-Warning "$Label: metrics 파일을 찾을 수 없습니다."
        return $null
    }
    $data = Get-Content $metrics -Raw | ConvertFrom-Json
    Remove-Item $metrics -ErrorAction SilentlyContinue

    [PSCustomObject]@{
        Label          = $Label
        Frames         = $data.frames
        InferenceRuns  = $data.inference_runs
        InferenceSec   = [math]::Round($data.inference_seconds, 3)
        P50ms          = [math]::Round($data.inference_p50_ms, 2)
        P95ms          = [math]::Round($data.inference_p95_ms, 2)
        MaxMs          = [math]::Round($data.inference_max_ms, 2)
        CpuMsPerFrame  = [math]::Round($data.cpu_ms_per_frame, 2)
    }
}

$results = @()
$results += Measure-Model -Label "FP32" -Model $Fp32Model

if ($Int8Model -ne "") {
    $results += Measure-Model -Label "INT8" -Model $Int8Model
}

Write-Host ""
Write-Host ("=" * 70)
Write-Host ("{0,-8} {1,8} {2,12} {3,8} {4,8} {5,8} {6,12}" -f `
    "모델", "추론횟수", "총추론(s)", "p50(ms)", "p95(ms)", "max(ms)", "CPU ms/frame")
Write-Host ("-" * 70)
foreach ($r in $results) {
    Write-Host ("{0,-8} {1,8} {2,12} {3,8} {4,8} {5,8} {6,12}" -f `
        $r.Label, $r.InferenceRuns, $r.InferenceSec,
        $r.P50ms, $r.P95ms, $r.MaxMs, $r.CpuMsPerFrame)
}
Write-Host ("=" * 70)

if ($results.Count -eq 2) {
    $speedup = if ($results[1].InferenceSec -gt 0) {
        [math]::Round($results[0].InferenceSec / $results[1].InferenceSec, 2)
    } else { "N/A" }
    Write-Host ""
    Write-Host "INT8 속도 향상 (총 추론 시간): ${speedup}x"
    Write-Host "참고: i5-4200U (AVX2, VNNI 없음) 기대치는 1.5~2.5x 입니다."
}
