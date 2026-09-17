@echo off
rem Builds build\MapTester.exe with MinGW-w64 gcc (TDM-GCC works). Run from this folder.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
windres -i res\app.rc -o build\app_res.o || goto :fail
gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -municode -mwindows ^
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    src\main.c src\upload.c src\install.c src\net.c src\json.c src\config.c src\gfx.c src\util.c ^
    build\app_res.o -o build\MapTester.exe ^
    -lwinhttp -lgdiplus -lbcrypt -lcomdlg32 -lole32 -loleaut32 -lshell32 -luuid -ldwmapi -luxtheme ^
    -lgdi32 -luser32 -lkernel32 -static -static-libgcc || goto :fail
echo Built build\MapTester.exe
exit /b 0
:fail
echo Build failed.
exit /b 1
