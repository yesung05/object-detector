# Compatibility entry point; the GUI host owns background lifetime.
Start-Process -FilePath (Join-Path $PSScriptRoot 'unmanned_detector-launcher.exe')
