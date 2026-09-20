@echo off
setlocal
cd /d "%~dp0"
set "recorderNode=node"
if exist "C:\Program Files\nodejs\node.exe" set "recorderNode=C:\Program Files\nodejs\node.exe"
"%recorderNode%" scripts\serve_scenario_recorder.js --open
pause
