param(
    [string]$Data = "datasets\tier2_v3\tier2_v2.yaml",
    [switch]$Resume
)

# Native ML libraries write progress and warnings to stderr. With "Stop",
# PowerShell turns a harmless warning into NativeCommandError and kills training.
$ErrorActionPreference = "Continue"
$projectRoot = $PSScriptRoot
$python = Join-Path $projectRoot ".venv-train\Scripts\python.exe"
$logPath = Join-Path $projectRoot "logs\tier2_v3_fresh_training.log"

Set-Location $projectRoot
New-Item -ItemType Directory -Force (Split-Path $logPath) | Out-Null

if ($Resume) {
    $checkpoint = "runs\detect\runs\tier2_v3_fresh\train\weights\last.pt"
    "[launcher] resuming from $checkpoint at $(Get-Date -Format o)" |
        Tee-Object -FilePath $logPath -Append
    & $python scripts\run_train_tier2.py `
        --data $Data `
        --resume $checkpoint 2>&1 | Tee-Object -FilePath $logPath -Append
} else {
    & $python scripts\run_train_tier2.py `
        --data $Data `
        --weights yolo11n.pt `
        --epochs 120 `
        --batch 128 `
        --device 0 `
        --workers 8 `
        --amp `
        --patience 35 `
        --optimizer AdamW `
        --lr0 0.001 `
        --lrf 0.01 `
        --warmup-epochs 5 `
        --warmup-bias-lr 0.001 `
        --weight-decay 0.0005 `
        --project runs/tier2_v3_fresh 2>&1 | Tee-Object -FilePath $logPath
}

$exitCode = $LASTEXITCODE
"[launcher] exit_code=$exitCode finished=$(Get-Date -Format o)" | Tee-Object -FilePath $logPath -Append
exit $exitCode
