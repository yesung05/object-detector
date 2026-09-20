@echo off
setlocal
cd /d "%~dp0"
set "voiceNode=node"
if exist "C:\Program Files\nodejs\node.exe" set "voiceNode=C:\Program Files\nodejs\node.exe"
"%voiceNode%" scripts\clova_voice.js
echo This generates missing speech files using CLOVA Voice. API charges may apply.
choice /m "Generate missing audio now"
if errorlevel 2 exit /b
"%voiceNode%" scripts\clova_voice.js --generate
pause
