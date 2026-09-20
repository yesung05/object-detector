@echo off
setlocal
cd /d "%~dp0"
set "trialNode=node"
if exist "C:\Program Files\nodejs\node.exe" set "trialNode=C:\Program Files\nodejs\node.exe"
echo Automatic evidence recording for table-1, port 8081, up to 10 minutes.
echo Camera images are saved locally. Use only with consent.
echo Start the detector first. Type q then Enter to finish.
echo Open review.html in the printed Saved folder after recording.
"%trialNode%" scripts\record_surface_trial.js --surface table-1 --scenario table_residue --seconds 600 --evidence on
pause
