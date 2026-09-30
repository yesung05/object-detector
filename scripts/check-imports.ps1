# Fails if any packaged binary statically imports a DLL that is neither shipped in
# $Dir nor part of the Windows 10 1607 (14393, installer MinVersion) base system.
# A missing static import kills the process at load (0xC0000135) before any of our
# logging runs, so this is checked at packaging time rather than discovered in a store.
# Function-level gaps (DLL present, API missing) are caught on the target machine by
# the installer's post-install --help probe (installer/HUNIK.iss).
param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Dumpbin
)
$ErrorActionPreference = 'Stop'
# Only DLLs observed in the payload and verified to exist on 1607. dxcore.dll is
# deliberately absent: Windows 10 1809 does not have it (issue #5, ORT 1.22).
# Add an entry only after confirming the DLL ships with the minimum supported build.
$system = @(
    'kernel32.dll','ntdll.dll','advapi32.dll','user32.dll','gdi32.dll','shell32.dll',
    'shlwapi.dll','ole32.dll','oleaut32.dll','ws2_32.dll','iphlpapi.dll','bcrypt.dll',
    'ncrypt.dll','crypt32.dll','secur32.dll','rpcrt4.dll','usp10.dll','d2d1.dll',
    'dwrite.dll','avrt.dll','avicap32.dll','winmm.dll','cfgmgr32.dll','setupapi.dll',
    'dxgi.dll','dbghelp.dll',
    'api-ms-win-core-path-l1-1-0.dll','api-ms-win-core-synch-l1-2-0.dll',
    'api-ms-win-core-winrt-l1-1-0.dll','api-ms-win-core-winrt-string-l1-1-0.dll',
    'api-ms-win-core-winrt-error-l1-1-0.dll'
)
$binaries = @(Get-ChildItem $Dir -Recurse -File | Where-Object Extension -in '.dll','.exe')
$shipped = $binaries | ForEach-Object { $_.Name.ToLowerInvariant() }
$bad = @()
foreach ($bin in $binaries) {
    $out = & $Dumpbin /nologo /dependents $bin.FullName
    if ($LASTEXITCODE) { throw "dumpbin failed on $($bin.FullName)" }
    foreach ($line in $out) {
        if ($line -notmatch '^\s+(\S+\.dll)\s*$') { continue }
        $dep = $Matches[1].ToLowerInvariant()
        # UCRT api sets are part of every Windows 10 build.
        if ($shipped -contains $dep -or $system -contains $dep -or $dep -like 'api-ms-win-crt-*') { continue }
        $bad += "$($bin.Name) -> $dep"
    }
}
if ($bad) { throw ("Imports missing on Windows 10 1607 base system:`n  " + ($bad -join "`n  ")) }
Write-Host "Import check passed: $($binaries.Count) binaries"
