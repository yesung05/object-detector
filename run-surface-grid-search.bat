@echo off
setlocal
cd /d "%~dp0"
set "searchNode=node"
if exist "C:\Program Files\nodejs\node.exe" set "searchNode=C:\Program Files\nodejs\node.exe"
set "initialSnapshot=runs\surface-search\baseline-EQYvBO"
"%searchNode%" scripts\surface_grid_search.js --plan "%initialSnapshot%"
echo This may take several hours. Videos and operational settings will not be modified.
choice /m "Start the 18-candidate development grid search"
if errorlevel 2 exit /b
"%searchNode%" scripts\surface_grid_search.js --run "%initialSnapshot%"
pause
