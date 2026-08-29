<#
.SYNOPSIS
    단위 테스트(test_core)를 빌드하고 실행합니다.

.DESCRIPTION
    ORT/FFmpeg 없이 빌드 가능한 test_core 40개 단위 테스트를 실행합니다.
    -Full 플래그를 주면 메인 실행 파일 + test_detector 까지 빌드합니다.

.EXAMPLE
    .\scripts\test.ps1                          # 단위 테스트만
    .\scripts\test.ps1 -Rebuild                 # build-windows 지우고 새로 구성
    .\scripts\test.ps1 -Full `
        -OrtRoot   C:\deps\Microsoft.ML.OnnxRuntime.DirectML `
        -DmlRoot   C:\deps\Microsoft.AI.DirectML `
        -FfmpegRoot C:\deps\ffmpeg              # 전체 빌드 + 통합 테스트
#>

param(
    [switch]$Rebuild,

    # 전체 빌드(메인 실행 파일 + test_detector)에 필요한 경로
    [switch]$Full,
    [string]$OrtRoot    = "",
    [string]$DmlRoot    = "",
    [string]$FfmpegRoot = "",

    [string]$BuildDir   = "$PSScriptRoot\..\build-windows",
    [string]$Config     = "Release"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ── 색상 출력 헬퍼 ────────────────────────────────────────────────────────
function Write-Step  { param($msg) Write-Host "`n>> $msg" -ForegroundColor Cyan }
function Write-Ok    { param($msg) Write-Host "   OK: $msg" -ForegroundColor Green }
function Write-Fail  { param($msg) Write-Host "   FAIL: $msg" -ForegroundColor Red }

# ── Visual Studio 환경 설정 ───────────────────────────────────────────────
# cmake.exe는 이미 PATH에 있으나, MSBuild는 VS 환경이 필요합니다.
# vswhere로 설치 경로를 찾아 VS 환경 스크립트를 임포트합니다.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Fail "vswhere.exe 를 찾을 수 없습니다. Visual Studio 또는 Build Tools가 설치되어 있는지 확인하세요."
    exit 1
}
$vsRoot = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsRoot) {
    Write-Fail "MSVC 툴체인을 찾을 수 없습니다."
    exit 1
}

$vcvars = "$vsRoot\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Fail "vcvars64.bat 를 찾을 수 없습니다: $vcvars"
    exit 1
}

# vcvars64.bat 환경변수를 현재 PowerShell 세션에 임포트합니다.
# (cmd.exe 에서 set 출력을 파싱하는 표준 기법)
Write-Step "VS 빌드 환경 설정 ($vsRoot)"
$envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envDump) {
    if ($line -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
}
Write-Ok "vcvars64 로드 완료"

# ── CMake 구성 ────────────────────────────────────────────────────────────
$BuildDir = (Resolve-Path -LiteralPath "$PSScriptRoot\..").Path + "\build-windows"
$SourceDir = (Resolve-Path -LiteralPath "$PSScriptRoot\..").Path

if ($Rebuild -and (Test-Path $BuildDir)) {
    Write-Step "기존 빌드 디렉터리 삭제"
    Remove-Item -Recurse -Force $BuildDir
    Write-Ok "삭제 완료"
}

if (-not (Test-Path "$BuildDir\CMakeCache.txt")) {
    Write-Step "CMake 구성"

    $cmakeArgs = @(
        "-S", $SourceDir,
        "-B", $BuildDir,
        "-G", "Visual Studio 17 2022",
        "-A", "x64"
    )

    if ($Full) {
        if ($OrtRoot)    { $cmakeArgs += "-DORT_ROOT=$OrtRoot" }
        if ($DmlRoot)    { $cmakeArgs += "-DDIRECTML_ROOT=$DmlRoot" }
        if ($FfmpegRoot) { $cmakeArgs += "-DFFMPEG_ROOT=$FfmpegRoot" }
    }

    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Write-Fail "CMake 구성 실패"; exit 1 }
    Write-Ok "구성 완료"
} else {
    Write-Step "기존 CMake 캐시 재사용 ($BuildDir)"
}

# ── 빌드 ──────────────────────────────────────────────────────────────────
Write-Step "빌드 ($Config)"
$targets = if ($Full) { @() } else { @("--target", "test_core") }
& cmake --build $BuildDir --config $Config @targets
if ($LASTEXITCODE -ne 0) { Write-Fail "빌드 실패"; exit 1 }
Write-Ok "빌드 완료"

# ── 테스트 실행 ───────────────────────────────────────────────────────────
Write-Step "테스트 실행"
$ctestArgs = @(
    "--test-dir", $BuildDir,
    "-C", $Config,
    "--output-on-failure"
)
if (-not $Full) {
    # 단위 테스트만 (ORT 없이 실행 가능한 것)
    $ctestArgs += @("-L", "unit")
}

& ctest @ctestArgs
$exitCode = $LASTEXITCODE

Write-Host ""
if ($exitCode -eq 0) {
    Write-Ok "모든 테스트 통과"
} else {
    Write-Fail "테스트 실패 (exit $exitCode)"
}
exit $exitCode
