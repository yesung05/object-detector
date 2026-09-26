param(
    [string]$Version = '0.1.0-alpha.1',
    [string]$BuildDir = 'build-g',
    [string]$FfmpegRoot = '',
    [string]$OrtRoot = '',
    [string]$VcRuntimeDir = '',
    [string]$Iscc = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
if ($Version -notmatch '^(\d+\.\d+\.\d+)(?:-alpha\.(\d+))?$') { throw 'Use x.y.z or x.y.z-alpha.n' }
$NumericVersion = $Matches[1] + '.' + $(if ($Matches[2]) { $Matches[2] } else { '0' })
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
# Prefer the exact dependency roots of an existing build over another installed version.
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $cache = Get-Content $cacheFile
    if (-not $FfmpegRoot) {
        $entry = $cache | Where-Object { $_ -match '^FFMPEG_INCLUDE_DIR:PATH=' } | Select-Object -First 1
        if ($entry) { $FfmpegRoot = Split-Path ($entry -replace '^[^=]*=', '') }
    }
    if (-not $OrtRoot) {
        $entry = $cache | Where-Object { $_ -match '^ORT_INCLUDE_DIR:PATH=' } | Select-Object -First 1
        if ($entry) { $OrtRoot = Split-Path ($entry -replace '^[^=]*=', '') }
    }
}
if (-not $FfmpegRoot) { $FfmpegRoot = $env:FFMPEG_ROOT }
if (-not $OrtRoot) { $OrtRoot = $env:ORT_ROOT }
if (-not $FfmpegRoot) { $FfmpegRoot = (Get-ChildItem C:\dev -Directory -Filter 'ffmpeg*win64*shared*' | Select-Object -First 1).FullName }
if (-not $OrtRoot) { $OrtRoot = (Get-ChildItem C:\dev -Directory -Filter 'onnxruntime-win-x64-*' | Sort-Object Name -Descending | Select-Object -First 1).FullName }
foreach ($dependency in @("$FfmpegRoot\bin\ffmpeg.exe", "$OrtRoot\lib\onnxruntime.dll")) {
    if (-not (Test-Path -LiteralPath $dependency)) { throw "Missing dependency: $dependency (set FfmpegRoot / OrtRoot)" }
}
if (-not $VcRuntimeDir) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -property installationPath
    $VcRuntimeDir = (Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1).FullName
}
if (-not (Test-Path "$VcRuntimeDir\vcruntime140.dll")) { throw 'Set VcRuntimeDir to the x64 Visual C++ redistributable CRT directory.' }
if (-not $Iscc) {
    foreach ($candidate in @("$BuildDir\installer-tools\Inno\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
        if (Test-Path $candidate) { $Iscc = $candidate; break }
    }
}
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw 'Install Inno Setup 6 (https://jrsoftware.org/isdl.php), or pass -Iscc path\ISCC.exe.' }
# Normalize PATH casing for MSBuild when launched from tool-hosted PowerShell.
$buildPath = $env:PATH
Remove-Item Env:PATH
$env:Path = $buildPath
if (-not $SkipBuild) {
    & cmake -S $root -B $BuildDir -A x64 '-UORT_LIBRARY' '-UORT_INCLUDE_DIR' '-UFFMPEG_*_LIBRARY' '-UFFMPEG_INCLUDE_DIR' '-DTARGET_ISA=baseline' "-DFFMPEG_ROOT=$FfmpegRoot" "-DORT_ROOT=$OrtRoot" "-DHUNIK_VERSION=$Version"
    if ($LASTEXITCODE) { throw 'CMake configure failed' }
    & cmake --build $BuildDir --config Release --clean-first --parallel 4
    if ($LASTEXITCODE) { throw 'Full Release build failed' }
}
# Refuse to package a different runtime than the one used to build/test.
$cache = Get-Content (Join-Path $BuildDir 'CMakeCache.txt')
foreach ($pair in @(@('ORT_INCLUDE_DIR', "$OrtRoot/include"), @('FFMPEG_INCLUDE_DIR', "$FfmpegRoot/include"))) {
    $entry = $cache | Where-Object { $_ -match ('^' + $pair[0] + ':PATH=') } | Select-Object -First 1
    $actual = [IO.Path]::GetFullPath(($entry -replace '^[^=]*=', ''))
    if ($actual -ne [IO.Path]::GetFullPath($pair[1])) { throw 'Runtime does not match CMake cache; rerun without -SkipBuild.' }
}
& ctest --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed; no installer produced' }
$stage = Join-Path $BuildDir ('installer-payload-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($dir in @('models','dashboard','licenses','tools','docs')) { New-Item -ItemType Directory -Path "$stage\$dir" | Out-Null }
foreach ($exe in @('unmanned_detector.exe','unmanned_detector-dashboard.exe','unmanned_detector-launcher.exe')) { Copy-Item "$BuildDir\Release\$exe" $stage }
Copy-Item "$FfmpegRoot\bin\*.dll" $stage
Copy-Item "$FfmpegRoot\bin\ffmpeg.exe" $stage
Copy-Item "$OrtRoot\lib\*.dll" $stage
Copy-Item "$VcRuntimeDir\*.dll" $stage
foreach ($model in @('yolo11n-pose-416x224.onnx','yolo11n-pose-416x288.onnx','yolo11n-pose-416x416.onnx','yolo11n_tier2_fp32.onnx')) {
    Copy-Item "$root\models\$model" "$stage\models"
}
Copy-Item "$root\dashboard\*.html" "$stage\dashboard"
Copy-Item "$root\dashboard\*.js" "$stage\dashboard"
Copy-Item "$root\run-all.ps1", "$root\start-installed.ps1" $stage
Copy-Item "$root\scripts\export-coordinate-trial.py" "$stage\tools"
Copy-Item "$root\docs\coordinate-field-trial.md" "$stage\docs"
Copy-Item "$root\installer\README.txt" $stage
Copy-Item "$FfmpegRoot\LICENSE.txt" "$stage\licenses\FFmpeg-LICENSE.txt"
Copy-Item "$OrtRoot\LICENSE" "$stage\licenses\ONNX-Runtime-LICENSE.txt"
Copy-Item "$OrtRoot\ThirdPartyNotices.txt" "$stage\licenses\ONNX-Runtime-ThirdPartyNotices.txt"
Copy-Item "$root\installer\DEPENDENCIES.txt" "$stage\licenses"
$ffVersion = & "$stage\ffmpeg.exe" -version
if ($LASTEXITCODE) { throw 'Packaged FFmpeg runtime could not start' }
$ffVersion | Set-Content "$stage\licenses\FFmpeg-build.txt" -Encoding utf8
& "$stage\unmanned_detector.exe" --help | Out-Null
if ($LASTEXITCODE) { throw 'Packaged detector could not start' }
# Immutable inventory for support and reproducible field trials. No runtime state.
$files = @(Get-ChildItem $stage -Recurse -File | ForEach-Object {
    @{ path=$_.FullName.Substring($stage.Length+1); bytes=$_.Length; sha256=(Get-FileHash $_.FullName -Algorithm SHA256).Hash }
})
@{ version=$Version; created_utc=[DateTime]::UtcNow.ToString('o'); architecture='x64'; files=$files } | ConvertTo-Json -Depth 5 | Set-Content "$stage\build-manifest.json" -Encoding utf8
& $Iscc "/DStageDir=$stage" "/DAppVersion=$Version" "/DNumericVersion=$NumericVersion" "$root\installer\HUNIK.iss"
if ($LASTEXITCODE) { throw 'Installer compilation failed' }
$setup = "$root\dist\unmanned_detector-Setup-$Version-x64.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash
"$hash  $([IO.Path]::GetFileName($setup))" | Set-Content "$setup.sha256" -Encoding ascii
Write-Host "Installer: $setup"
Write-Host "Payload: $stage"
