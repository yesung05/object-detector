# object-detector Windows 환경 구성 스크립트
# 관리자 권한으로 실행 필요 (VS Build Tools 설치 때문)
# 실행: powershell -ExecutionPolicy Bypass -File setup-windows.ps1

param(
    [switch]$SkipVS,       # VS Build Tools 설치 건너뛰기 (이미 설치된 경우)
    [switch]$SkipCMake,    # CMake 설치 건너뛰기
    [switch]$SkipDeps      # ORT/FFmpeg 다운로드 건너뛰기
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "`n>>> $msg" -ForegroundColor Cyan }
function Write-OK($msg)   { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Fail($msg) { Write-Host "    FAIL: $msg" -ForegroundColor Red; exit 1 }

# ── 관리자 권한 확인 ──────────────────────────────────────────────
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "관리자 권한이 필요합니다. 관리자 PowerShell에서 다시 실행하세요." -ForegroundColor Red
    Write-Host 'Start-Process powershell -Verb RunAs -ArgumentList "-ExecutionPolicy Bypass -File setup-windows.ps1"'
    exit 1
}

# ── 1. Visual Studio Build Tools 2022 ────────────────────────────
if (-not $SkipVS) {
    Write-Step "Visual Studio Build Tools 2022 설치 (C++ 워크로드 포함)"
    $vsInstalled = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
        -products Microsoft.VisualStudio.Product.BuildTools `
        -version "[17,18)" -property installationPath 2>$null
    if ($vsInstalled) {
        Write-OK "이미 설치됨: $vsInstalled"
    } else {
        winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --accept-package-agreements --accept-source-agreements `
            --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
        if ($LASTEXITCODE -ne 0) { Write-Fail "VS Build Tools 설치 실패" }
        Write-OK "VS Build Tools 2022 설치 완료"
    }
} else {
    Write-Host ">>> VS Build Tools 설치 건너뜀 (-SkipVS)" -ForegroundColor Yellow
}

# ── 2. CMake ─────────────────────────────────────────────────────
if (-not $SkipCMake) {
    Write-Step "CMake 설치"
    $cmakeVer = cmake --version 2>$null
    if ($cmakeVer) {
        Write-OK "이미 설치됨: $($cmakeVer | Select-Object -First 1)"
    } else {
        winget install --id Kitware.CMake --exact --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) { Write-Fail "CMake 설치 실패" }
        # PATH 갱신
        $env:PATH = "C:\Program Files\CMake\bin;" + $env:PATH
        Write-OK "CMake 설치 완료"
    }
} else {
    Write-Host ">>> CMake 설치 건너뜀 (-SkipCMake)" -ForegroundColor Yellow
}

# ── 3. 의존성 다운로드 (ORT + FFmpeg) ────────────────────────────
if (-not $SkipDeps) {
    $devDir = "C:\dev"
    New-Item -ItemType Directory -Force -Path $devDir | Out-Null

    # ONNX Runtime 1.26.0 (CPU, win-x64)
    $ortDest = "$devDir\onnxruntime-win-x64-1.26.0"
    if (Test-Path "$ortDest\include\onnxruntime_c_api.h") {
        Write-Step "ONNX Runtime 1.26.0"
        Write-OK "이미 존재: $ortDest"
    } else {
        Write-Step "ONNX Runtime 1.26.0 다운로드"
        $ortUrl = "https://github.com/microsoft/onnxruntime/releases/download/v1.26.0/onnxruntime-win-x64-1.26.0.zip"
        $ortZip = "$devDir\ort.zip"
        Write-Host "    다운로드 중: $ortUrl"
        Invoke-WebRequest -Uri $ortUrl -OutFile $ortZip -UseBasicParsing
        Write-Host "    압축 해제 중..."
        Expand-Archive -Path $ortZip -DestinationPath $devDir -Force
        Remove-Item $ortZip
        if (-not (Test-Path "$ortDest\include\onnxruntime_c_api.h")) {
            Write-Fail "ORT 압축 해제 후 파일을 찾을 수 없음: $ortDest"
        }
        Write-OK "ORT → $ortDest"
    }

    # FFmpeg (gpl-shared, win64, latest master)
    $ffmpegDest = "$devDir\ffmpeg-master-latest-win64-gpl-shared"
    if (Test-Path "$ffmpegDest\include\libavformat\avformat.h") {
        Write-Step "FFmpeg"
        Write-OK "이미 존재: $ffmpegDest"
    } else {
        Write-Step "FFmpeg win64-gpl-shared 다운로드"
        $ffmpegUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip"
        $ffmpegZip = "$devDir\ffmpeg.zip"
        Write-Host "    다운로드 중: $ffmpegUrl"
        Invoke-WebRequest -Uri $ffmpegUrl -OutFile $ffmpegZip -UseBasicParsing
        Write-Host "    압축 해제 중..."
        Expand-Archive -Path $ffmpegZip -DestinationPath $devDir -Force
        Remove-Item $ffmpegZip
        if (-not (Test-Path "$ffmpegDest\include\libavformat\avformat.h")) {
            Write-Fail "FFmpeg 압축 해제 후 파일을 찾을 수 없음: $ffmpegDest"
        }
        Write-OK "FFmpeg → $ffmpegDest"
    }
} else {
    Write-Host ">>> ORT/FFmpeg 다운로드 건너뜀 (-SkipDeps)" -ForegroundColor Yellow
}

# ── 4. pose 모델 aspect-ratio 변환 ──────────────────────────────
Write-Step "YOLO11n-pose 해상도별 ONNX export"
$modelsDir = "$PSScriptRoot\models"
$allPresent = (Test-Path "$modelsDir\yolo11n-pose-416x224.onnx") -and
              (Test-Path "$modelsDir\yolo11n-pose-416x288.onnx") -and
              (Test-Path "$modelsDir\yolo11n-pose-416x416.onnx")
if ($allPresent) {
    Write-OK "이미 존재: models\yolo11n-pose-416x{224,288,416}.onnx"
} else {
    $python = Get-Command python -ErrorAction SilentlyContinue
    if (-not $python) { Write-Fail "Python이 없습니다. python.org에서 설치 후 재실행하세요." }

    Write-Host "    ultralytics 설치 중..."
    & python -m pip install ultralytics --quiet
    if ($LASTEXITCODE -ne 0) { Write-Fail "ultralytics 설치 실패" }

    $exportScript = @'
from ultralytics import YOLO
import shutil, os, sys
os.makedirs("models", exist_ok=True)
model = YOLO("yolo11n-pose.pt")
for w, h in [(416,224),(416,288),(416,416)]:
    out = f"models/yolo11n-pose-{w}x{h}.onnx"
    if os.path.exists(out):
        print(f"skip: {out}"); continue
    print(f"exporting {w}x{h}...")
    model.export(format="onnx", imgsz=[h,w], simplify=True, opset=17)
    if os.path.exists("yolo11n-pose.onnx"):
        shutil.move("yolo11n-pose.onnx", out)
        print(f"  -> {out}")
    else:
        print(f"  ERROR: export failed for {w}x{h}"); sys.exit(1)
# pt 파일은 로컬 캐시 — 삭제하지 않고 둠
print("export done")
'@
    $exportScript | & python
    if ($LASTEXITCODE -ne 0) { Write-Fail "pose 모델 export 실패" }
    Write-OK "models\yolo11n-pose-416x{224,288,416}.onnx 생성 완료"
}

# ── 5. DLL → build\Release 복사 (빌드 완료 후에만) ─────────────
$releaseDir = "$PSScriptRoot\build\Release"
if (Test-Path "$releaseDir\yolo11-person.exe") {
    Write-Step "DLL 및 모델을 build\Release 에 복사"
    Copy-Item "C:\dev\ffmpeg-master-latest-win64-gpl-shared\bin\*.dll" $releaseDir -Force
    Copy-Item "C:\dev\onnxruntime-win-x64-1.26.0\lib\*.dll"           $releaseDir -Force
    Copy-Item "$PSScriptRoot\models\*.onnx"                            $releaseDir -Force
    Write-OK "복사 완료 → $releaseDir"
} else {
    Write-Host "    build\Release\yolo11-person.exe 없음 — 빌드 후 수동으로 실행하거나 build.ps1 을 먼저 실행하세요." -ForegroundColor Yellow
}

# ── 완료 안내 ────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== 환경 구성 완료 ===" -ForegroundColor Green
Write-Host ""
Write-Host "빌드하려면 새 터미널을 열고 프로젝트 폴더에서:" -ForegroundColor White
Write-Host "  powershell -ExecutionPolicy Bypass -File build.ps1" -ForegroundColor Yellow
Write-Host ""
Write-Host "의존성 위치:" -ForegroundColor White
Write-Host "  ORT    : C:\dev\onnxruntime-win-x64-1.26.0"
Write-Host "  FFmpeg : C:\dev\ffmpeg-master-latest-win64-gpl-shared"
Write-Host "  models : $PSScriptRoot\models"
