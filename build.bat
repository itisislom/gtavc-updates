@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0"

if not defined VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
    ) else (
        echo [ERROR] MSVC build environment not found.
        exit /b 1
    )
)

if not exist "build" mkdir "build"

echo [BUILD] Building UpdateChecker.asi (x86, /MT, /W4, /O2, /utf-8)...
cl /nologo /O2 /MT /W4 /utf-8 /LD /Fe"build\UpdateChecker.asi" /Fo"build/" src\checker\main.cpp src\common\paths.cpp src\common\crypto.cpp user32.lib gdi32.lib wininet.lib bcrypt.lib
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build UpdateChecker.asi
    exit /b %ERRORLEVEL%
)

echo [BUILD] Building updater.exe (x86, /MT, /W4, /O2, /utf-8)...
cl /nologo /O2 /MT /W4 /utf-8 /DTESTHOOKS /Fe"build\updater.exe" /Fo"build/" src\updater\main.cpp src\updater\updater_core.cpp src\common\paths.cpp src\common\crypto.cpp user32.lib gdi32.lib comctl32.lib wininet.lib shell32.lib advapi32.lib bcrypt.lib
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to build updater.exe
    exit /b %ERRORLEVEL%
)

echo [BUILD] Build successful. Calculating SHA-256...
powershell -NoProfile -Command "Get-FileHash -Algorithm SHA256 build\UpdateChecker.asi, build\updater.exe | Format-Table -AutoSize"
