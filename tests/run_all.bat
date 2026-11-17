@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

if not defined VCINSTALLDIR (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsamd64_x86.bat"
    ) else (
        echo [ERROR] MSVC build environment not found.
        exit /b 1
    )
)

echo [RUN_ALL] 1. Building core binaries via build.bat...
call build.bat
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Core build failed!
    exit /b %ERRORLEVEL%
)

echo.
echo [RUN_ALL] 2. Compiling automated test suites...

cl /nologo /O2 /MT /W4 /utf-8 /Fe"build\path_tests.exe" /Fo"build/" tests\path_tests.cpp src\common\paths.cpp
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling path_tests & exit /b 1 )

cl /nologo /O2 /MT /W4 /utf-8 /DTESTHOOKS /Fe"build\stage2_tests.exe" /Fo"build/" tests\stage2_tests.cpp src\updater\updater_core.cpp src\common\paths.cpp user32.lib shell32.lib advapi32.lib
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling stage2_tests & exit /b 1 )

cl /nologo /O2 /MT /W4 /utf-8 /Fe"build\stage3_tests.exe" /Fo"build/" tests\stage3_tests.cpp src\updater\updater_core.cpp src\common\paths.cpp user32.lib shell32.lib advapi32.lib
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling stage3_tests & exit /b 1 )

cl /nologo /O2 /MT /W4 /utf-8 /Fe"build\stage4_tests.exe" /Fo"build/" tests\stage4_tests.cpp src\common\crypto.cpp wininet.lib bcrypt.lib
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling stage4_tests & exit /b 1 )

cl /nologo /O2 /MT /W4 /utf-8 /Fe"build\stage5_7_tests.exe" /Fo"build/" tests\stage5_7_tests.cpp src\updater\updater_core.cpp src\common\paths.cpp src\common\crypto.cpp user32.lib shell32.lib advapi32.lib bcrypt.lib
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling stage5_7_tests & exit /b 1 )

cl /nologo /O2 /MT /W4 /utf-8 /DTESTHOOKS /Fe"build\e2e_sandbox_tests.exe" /Fo"build/" tests\e2e_sandbox_tests.cpp src\updater\updater_core.cpp src\common\paths.cpp src\common\crypto.cpp user32.lib shell32.lib advapi32.lib bcrypt.lib
if %ERRORLEVEL% neq 0 ( echo [ERROR] Failed compiling e2e_sandbox_tests & exit /b 1 )

echo.
echo [RUN_ALL] 3. Running test suites...
echo.

build\path_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] path_tests failed! & exit /b 1 )

build\stage2_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] stage2_tests failed! & exit /b 1 )

build\stage3_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] stage3_tests failed! & exit /b 1 )

build\stage4_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] stage4_tests failed! & exit /b 1 )

build\stage5_7_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] stage5_7_tests failed! & exit /b 1 )

build\e2e_sandbox_tests.exe
if %ERRORLEVEL% neq 0 ( echo [FAIL] e2e_sandbox_tests failed! & exit /b 1 )

echo.
echo ========================================================
echo   ALL AUTOMATED TEST SUITES PASSED CLEANLY (6/6)!
echo ========================================================
exit /b 0
