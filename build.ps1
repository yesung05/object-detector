# object-detector Windows 빌드 스크립트
# 사전 준비: VS Build Tools 2022 + CMake 설치 완료 상태에서 실행

$ORT_ROOT   = "C:\dev\onnxruntime-win-x64-1.26.0"
$FFMPEG_ROOT = "C:\dev\ffmpeg-master-latest-win64-gpl-shared"
$BUILD_DIR  = "$PSScriptRoot\build"
$SRC_DIR    = $PSScriptRoot

# cmake PATH 갱신 (새로 설치된 경우 필요)
$env:PATH = "C:\Program Files\CMake\bin;" + $env:PATH

Write-Host "=== object-detector 빌드 시작 ===" -ForegroundColor Cyan

# CMake 구성
Write-Host "[1/2] CMake 구성..." -ForegroundColor Yellow
cmake -S $SRC_DIR -B $BUILD_DIR `
    -G "Visual Studio 17 2022" -A x64 `
    -DORT_ROOT="$ORT_ROOT" `
    -DFFMPEG_ROOT="$FFMPEG_ROOT"

if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake 구성 실패" -ForegroundColor Red
    exit 1
}

# 빌드
Write-Host "[2/2] 빌드..." -ForegroundColor Yellow
cmake --build $BUILD_DIR --config Release

if ($LASTEXITCODE -ne 0) {
    Write-Host "빌드 실패" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "=== 빌드 성공! ===" -ForegroundColor Green
Write-Host "실행 파일: $BUILD_DIR\Release\yolo11-person.exe"
Write-Host ""
Write-Host "테스트 실행 예시 (--provider cpu 필수: 현재 ORT는 CPU-only 빌드):"
Write-Host "  cd $BUILD_DIR\Release"
Write-Host "  .\yolo11-person.exe --model yolo11n-416.onnx --camera --output out.mp4 --provider cpu"
Write-Host "  .\yolo11-person.exe --model yolo11n-416.onnx --input video.mp4 --output out.mp4 --provider cpu"
Write-Host "  .\yolo11-person.exe --model yolo11n-416.onnx --input video.mp4 --output out.mp4 --provider cpu --detect-every 5 --track --metrics metrics.json"
