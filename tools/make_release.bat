@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

echo ========================================================
echo   GTA: Vice City Update System - Release Packager
echo ========================================================
echo.

if exist "dist" rmdir /s /q "dist"
mkdir "dist"
mkdir "dist\staging"

echo [1/4] Building release binaries via build.bat...
call build.bat
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed!
    exit /b %ERRORLEVEL%
)

echo [2/4] Packaging release files...
copy /y "build\UpdateChecker.asi" "dist\staging\" >nul
copy /y "build\updater.exe" "dist\staging\" >nul

if exist "version.txt" (
    copy /y "version.txt" "dist\staging\" >nul
)

echo [3/4] Creating dist\update.zip...
tar.exe -cf "dist\update.zip" -C "dist\staging" .
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to create dist\update.zip
    exit /b %ERRORLEVEL%
)

echo [4/4] Generating SHA-256 checksums...
powershell -NoProfile -Command "$hash = (Get-FileHash -Algorithm SHA256 dist\update.zip).Hash.ToLower(); Set-Content -Path dist\update.zip.sha256 -Value $hash -NoNewline; Write-Host ('update.zip SHA256: ' + $hash)"
powershell -NoProfile -Command "Get-FileHash -Algorithm SHA256 dist\update.zip, dist\staging\UpdateChecker.asi, dist\staging\updater.exe | ForEach-Object { ($_.Hash.ToLower() + '  ' + [System.IO.Path]::GetFileName($_.Path)) } | Set-Content dist\SHA256SUMS.txt; Get-Content dist\SHA256SUMS.txt"

echo.
echo ========================================================
echo   Release Package Created Successfully!
echo   Output files in dist\:
echo     - dist\update.zip
echo     - dist\update.zip.sha256
echo     - dist\SHA256SUMS.txt
echo ========================================================
