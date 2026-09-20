# Public publisher/author copies only. Existing files are never overwritten.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
# Build the Korean folder name without depending on Windows PowerShell 5 source encoding.
$paperFolder = [string][char]0xB17C + [char]0xBB38
$targetDir = Join-Path (Join-Path (Join-Path $repoRoot 'docs') $paperFolder) 'references'
New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
$papers = @(
    @('01_luna_2018_abandoned_object_survey.pdf', 'https://mdpi-res.com/sensors/sensors-18-04290/article_deploy/sensors-18-04290-v2.pdf'),
    @('02_smeureanu_2018_abandoned_luggage.pdf', 'https://arxiv.org/pdf/1803.01160'),
    @('03_kang_2017_noscope.pdf', 'https://www.vldb.org/pvldb/vol10/p1586-kang.pdf'),
    @('04_xu_2018_deepcache.pdf', 'https://xumengwei.github.io/files/MobiCom18-DeepCache.pdf'),
    @('05_ghodrati_2021_frameexit.pdf', 'https://openaccess.thecvf.com/content/CVPR2021/papers/Ghodrati_FrameExit_Conditional_Early_Exiting_for_Efficient_Video_Recognition_CVPR_2021_paper.pdf'),
    @('06_proenca_2020_taco.pdf', 'https://arxiv.org/pdf/2003.06975'),
    @('07_wang_2014_cdnet.pdf', 'https://openaccess.thecvf.com/content_cvpr_workshops_2014/W12/papers/Wang_CDnet_2014_An_2014_CVPR_paper.pdf')
)
function Test-PdfHeader([string]$filePath) {
    $fileStream = [IO.File]::OpenRead($filePath)
    try {
        $header = New-Object byte[] 5
        return ($fileStream.Read($header,0,5) -eq 5 -and [Text.Encoding]::ASCII.GetString($header) -eq '%PDF-')
    } finally { $fileStream.Dispose() }
}
$failures = 0
foreach ($paper in $papers) {
    $destination = Join-Path $targetDir $paper[0]
    try {
        if (Test-Path -LiteralPath $destination) {
            if (-not (Test-PdfHeader $destination)) { throw 'Existing file is not a PDF; refusing to overwrite.' }
            Write-Output "Already present: $($paper[0])"
            continue
        }
        $temporary = Join-Path $targetDir ($paper[0] + '.' + [guid]::NewGuid().ToString('N') + '.download')
        & curl.exe --fail --location --silent --show-error --connect-timeout 15 --max-time 60 --output $temporary $paper[1]
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($paper[1])" }
        if (-not (Test-PdfHeader $temporary)) { throw "Not a PDF response: $temporary" }
        Move-Item -LiteralPath $temporary -Destination $destination
        $file = Get-Item -LiteralPath $destination
        $hash = Get-FileHash -LiteralPath $destination -Algorithm SHA256
        Write-Output "$($paper[0]) bytes=$($file.Length) SHA256=$($hash.Hash)"
    } catch { $failures++; Write-Warning "$($paper[0]): $_" }
}
if ($failures -gt 0) { exit 1 }
