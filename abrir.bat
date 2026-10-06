@echo off
rem Serves this folder on http://localhost:8791 and opens it. Close the server window to stop.
cd /d "%~dp0"
start "Viewmodels server" python -m http.server 8791
timeout /t 1 /nobreak >nul
start "" "http://localhost:8791/"
