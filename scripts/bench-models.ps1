<#
.SYNOPSIS
  같은 영상으로 여러 ONNX 모델의 추론 지연과 메모리를 재서 표로 비교합니다.

.DESCRIPTION
  "INT8 이 빠르면 INT8, FP32 가 빠르면 FP32" 를 매 기동마다 벤치마크로 결정하지 않는 이유:
  같은 기기에서 답은 바뀌지 않는데 시작 시간과 복잡도만 영구히 늘어나고, 무엇보다 이 측정은
  속도만 봅니다. 양자화 모델의 검출 정확도(mAP)는 평가된 적이 없어서, 몇 % 빠르다는 이유로
  자동 전환하는 것은 감시 시스템에서 위험한 거래입니다.

  그래서 배포 기기에서 이 스크립트를 한 번 돌리고, 결과가 현재 기본값과 다르면
  run-all.ps1 의 모델 우선순위 한 줄만 바꾸는 방식을 씁니다.

  측정 조건: 모션/블록 게이트를 끄고 --detect-every 1 로 두어 프레임마다 추론이 돌게 합니다.
  게이트가 켜져 있으면 모델마다 추론 횟수가 달라져 비교가 불가능합니다.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\bench-models.ps1
  powershell -ExecutionPolicy Bypass -File scripts\bench-models.ps1 -Video C:\clips\store.mp4
#>
param(
    [string]$Video,
    [int]$Seconds = 20,
    [string]$Size = "1280x720",
    [int]$Threads = 3
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$ROOT = Split-Path (Split-Path $MyInvocation.MyCommand.Path)

# ── 실행 파일 · DLL ───────────────────────────────────────────────────────────
$exe = $null
foreach ($d in @("$ROOT\build-windows\Release", "$ROOT\build\Release")) {
    if (Test-Path "$d\yolo11-person.exe") { $exe = "$d\yolo11-person.exe"; $bin = $d; break }
}
if (-not $exe) { Write-Host "[ERROR] yolo11-person.exe 없음. 먼저 빌드하세요." -ForegroundColor Red; exit 1 }
$env:PATH = "$bin;$env:PATH"
foreach ($d in @("C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin", "C:\deps\ffmpeg\bin",
                 "C:\dev\onnxruntime-win-x64-1.26.0\lib", "C:\deps\onnxruntime\lib")) {
    if (Test-Path $d) { $env:PATH = "$d;$env:PATH" }
}

# ── 입력 영상 ─────────────────────────────────────────────────────────────────
if (-not $Video) {
    $Video = Join-Path $env:TEMP "bench_input.mp4"
    if (-not (Test-Path $Video)) {
        $ff = $null
        foreach ($d in @($bin, "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin", "C:\deps\ffmpeg\bin")) {
            if ($d -and (Test-Path "$d\ffmpeg.exe")) { $ff = "$d\ffmpeg.exe"; break }
        }
        if (-not $ff) { Write-Host "[ERROR] ffmpeg.exe 없음. -Video 로 영상을 지정하세요." -ForegroundColor Red; exit 1 }
        Write-Host "합성 영상 생성 중 ($Size, ${Seconds}s)..."
        & $ff -y -loglevel error -f lavfi -i "testsrc2=size=${Size}:rate=15" -t $Seconds `
              -pix_fmt yuv420p -c:v libx264 -preset ultrafast $Video | Out-Null
    }
}
if (-not (Test-Path $Video)) { Write-Host "[ERROR] 영상 없음: $Video" -ForegroundColor Red; exit 1 }

# ── 게이트를 끈 임시 설정 ─────────────────────────────────────────────────────
$cfgPath = Join-Path $env:TEMP "bench_models_config.json"
$cfg = if (Test-Path "$ROOT\config.json") { Get-Content "$ROOT\config.json" -Raw | ConvertFrom-Json }
       else { [pscustomobject]@{} }
foreach ($kv in @{ motion_gate = 0; block_gate = 0; door_enabled = 0; residue_enabled = 0 }.GetEnumerator()) {
    $cfg | Add-Member -NotePropertyName $kv.Key -NotePropertyValue $kv.Value -Force
}
$cfg | ConvertTo-Json -Depth 6 | Set-Content $cfgPath -Encoding utf8

# ── 후보 모델 수집 ────────────────────────────────────────────────────────────
$models = @()
foreach ($p in @("$ROOT\models", $ROOT)) {
    if (-not (Test-Path $p)) { continue }
    foreach ($f in Get-ChildItem $p -Filter *.onnx -File) {
        if ($f.Name -match 'tier2') { continue }   # Tier 2 는 pose 와 입출력이 달라 따로 재야 합니다
        $models += $f
    }
}
if (-not $models) { Write-Host "[ERROR] *.onnx 를 찾지 못했습니다." -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "입력 : $Video"
Write-Host "설정 : 게이트 off, --detect-every 1, --provider cpu, --threads $Threads"
Write-Host "대상 : $($models.Count) 개 모델"
Write-Host ""

# ── 측정 ──────────────────────────────────────────────────────────────────────
$results = @()
foreach ($m in $models) {
    $tag  = [IO.Path]::GetFileNameWithoutExtension($m.Name)
    $mj   = Join-Path $env:TEMP "bm_$tag.json"
    $errf = Join-Path $env:TEMP "bm_$tag.log"
    Write-Host ("  측정 중: {0,-42}" -f $m.Name) -NoNewline
    $p = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden `
        -RedirectStandardError $errf -RedirectStandardOutput (Join-Path $env:TEMP "bm_${tag}_o.log") `
        -ArgumentList @("--input","`"$Video`"","--model","`"$($m.FullName)`"","--provider","cpu",
                        "--threads","$Threads","--config","`"$cfgPath`"",
                        "--event-log","`"$(Join-Path $env:TEMP "bm_$tag.db")`"",
                        "--detect-every","1","--metrics","`"$mj`"")
    $peak = 0
    while (-not $p.HasExited) {
        try { $p.Refresh(); if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 } } catch {}
        Start-Sleep -Milliseconds 60
    }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $mj)) {
        $why = (Get-Content $errf -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -Last 1)
        Write-Host " 실패 — $why" -ForegroundColor Yellow
        continue
    }
    $j = Get-Content $mj -Raw | ConvertFrom-Json
    $inp = (Get-Content $errf -Encoding UTF8 | Where-Object { $_ -match '^model input:' }) -replace 'model input: ','' -replace ',.*',''
    $results += [pscustomobject]@{
        Model = $m.Name; Input = $inp; SizeMB = [math]::Round($m.Length/1MB,1)
        P50 = [math]::Round($j.inference_p50_ms,1); P95 = [math]::Round($j.inference_p95_ms,1)
        PeakMB = [math]::Round($peak/1MB,1); Runs = $j.inference_runs
    }
    Write-Host (" p50={0,6:N1}ms  peak={1,6:N1}MB" -f $j.inference_p50_ms, ($peak/1MB))
}

if (-not $results) { Write-Host "[ERROR] 성공한 측정이 없습니다." -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "=== 결과 (추론 p50 오름차순) ===" -ForegroundColor Cyan
$results | Sort-Object P50 | Format-Table Model, Input, SizeMB, P50, P95, PeakMB, Runs -AutoSize

# ── 권고 ──────────────────────────────────────────────────────────────────────
# 같은 입력 크기끼리만 비교해야 정밀도 차이를 볼 수 있습니다. 입력이 다르면 픽셀 수가 달라
# "FP32 가 빠르다"가 사실은 "letterbox 가 적다"일 뿐인 경우가 생깁니다.
Write-Host "=== 정밀도 판단 (같은 입력 크기끼리 비교) ===" -ForegroundColor Cyan
$anyPair = $false
foreach ($g in $results | Group-Object Input) {
    if ($g.Count -lt 2) { continue }
    $anyPair = $true
    $best = $g.Group | Sort-Object P50 | Select-Object -First 1
    Write-Host ("  [$($g.Name)] 최속: {0}  (p50 {1} ms)" -f $best.Model, $best.P50)
    foreach ($r in $g.Group | Sort-Object P50 | Select-Object -Skip 1) {
        $pct = [math]::Round(($r.P50 - $best.P50) / $best.P50 * 100, 0)
        Write-Host ("            {0,-44} p50 {1,6} ms  (+{2}%)" -f $r.Model, $r.P50, $pct)
    }
}
if (-not $anyPair) { Write-Host "  같은 입력 크기의 모델이 2개 이상 있어야 정밀도 비교가 됩니다." }
Write-Host ""
Write-Host "현재 run-all.ps1 기본값은 FP32 우선입니다." -ForegroundColor Yellow
Write-Host "위 표에서 INT8/양자화 모델이 같은 입력 크기의 FP32보다 확실히 빠르면"
Write-Host "run-all.ps1 의 Tier 1 / Tier 2 모델 목록 순서를 바꾸세요."
Write-Host "단, 이 표는 속도만 봅니다 — 양자화 모델의 검출 정확도는 별도 검증이 필요합니다." -ForegroundColor Yellow
