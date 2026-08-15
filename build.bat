@echo off
where gcc >nul 2>nul
if errorlevel 1 (
    echo gcc not found in PATH. Install w64devkit or any MinGW GCC.
    exit /b 1
)
gcc -O2 -Wall -Wextra -o raycaster.exe raycaster.c -luser32 -lgdi32
if errorlevel 1 exit /b 1
echo Built raycaster.exe
