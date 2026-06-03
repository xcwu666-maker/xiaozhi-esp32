@echo off
setlocal

rem Map this project directory to X: to avoid non-ASCII path issues in Git and ESP-IDF.
subst X: /D >nul 2>nul
subst X: "%~dp0"

rem Force Python tools to read generated UTF-8 files correctly on Windows.
set PYTHONUTF8=1
set PYTHONIOENCODING=utf-8

rem Activate the ESP-IDF Python environment if it exists.
if exist "D:\miniconda3\Scripts\activate.bat" (
    call "D:\miniconda3\Scripts\activate.bat" idf-py311
)

rem Export ESP-IDF environment.
if exist "D:\esp\v5.5.4\esp-idf\export.bat" (
    call "D:\esp\v5.5.4\esp-idf\export.bat"
)

cd /d X:\
echo.
echo Project mapped to X:\
echo Common commands:
echo   idf.py build
echo   idf.py -p COM4 flash monitor
echo   idf.py -p COM4 monitor
echo.
cmd /k
