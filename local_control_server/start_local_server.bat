@echo off
setlocal

cd /d "%~dp0\.."

echo Starting local Xiaozhi PC-side control server...
echo.
python -m local_control_server.server --host 0.0.0.0 --port 8000

