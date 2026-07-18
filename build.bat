@echo off
taskkill /F /IM DXTViewer.exe > nul 2>&1

g++ -std=c++17 -O2 -mwindows -DUNICODE -D_UNICODE src\main.cpp src\logger.cpp src\archive.cpp src\dxt.cpp src\updater.cpp src\gui.cpp -o DXTViewer.exe -lgdi32 -lgdiplus -lcomctl32 -lcomdlg32 -lwinhttp -lshell32 -luuid -lole32 -lpthread > build.log 2>&1

if %ERRORLEVEL% EQU 0 (
    echo OK - DXTViewer.exe собран
) else (
    echo ОШИБКА - смотри build.log
    type build.log
)
pause