#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")

static HWND g_hWnd = NULL;
static HWND g_hStatus = NULL;
static HWND g_hProgress = NULL;

static void SetStatus(const char* text) {
    if (g_hStatus) {
        SetWindowTextA(g_hStatus, text);
        UpdateWindow(g_hStatus);
    }
}

static void SetProgress(int percent) {
    if (g_hProgress) {
        SendMessage(g_hProgress, PBM_SETPOS, percent, 0);
    }
}

static void GetGameDir(char* outDir, size_t maxLen) {
    GetModuleFileNameA(NULL, outDir, (DWORD)maxLen);
    char* lastSlash = strrchr(outDir, '\\');
    if (lastSlash) {
        *lastSlash = '\0';
    } else {
        strcpy_s(outDir, maxLen, ".");
    }
}

static BOOL WaitForGameExit(DWORD pid, DWORD maxWaitMs) {
    if (pid == 0) return TRUE;
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hProcess) return TRUE;

    DWORD res = WaitForSingleObject(hProcess, maxWaitMs);
    CloseHandle(hProcess);
    return (res == WAIT_OBJECT_0);
}

static BOOL DownloadFileWithProgress(const char* url, const char* destPath) {
    HINTERNET hInternet = InternetOpenA("GTAVC-Updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return FALSE;

    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusLen, NULL)) {
        if (statusCode != 200) {
            InternetCloseHandle(hUrl);
            InternetCloseHandle(hInternet);
            return FALSE;
        }
    }

    DWORD totalBytes = 0;
    DWORD len = sizeof(totalBytes);
    if (!HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &totalBytes, &len, NULL)) {
        totalBytes = 0;
    }

    FILE* f = fopen(destPath, "wb");
    if (!f) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    char buffer[32768];
    DWORD bytesRead = 0;
    DWORD downloadedBytes = 0;
    BOOL success = TRUE;

    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        if (fwrite(buffer, 1, bytesRead, f) != bytesRead) {
            success = FALSE;
            break;
        }
        downloadedBytes += bytesRead;

        if (totalBytes > 0) {
            int pct = (int)((downloadedBytes * 100ULL) / totalBytes);
            SetProgress(pct);

            char statusBuf[128];
            sprintf_s(statusBuf, "Downloading: %d%% (%lu / %lu KB)...",
                      pct, downloadedBytes / 1024, totalBytes / 1024);
            SetStatus(statusBuf);
        } else {
            char statusBuf[128];
            sprintf_s(statusBuf, "Downloading: %lu KB...", downloadedBytes / 1024);
            SetStatus(statusBuf);
        }

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    fclose(f);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (!success || downloadedBytes < 4) {
        DeleteFileA(destPath);
        return FALSE;
    }

    unsigned char magic[4] = {0};
    FILE* fCheck = fopen(destPath, "rb");
    if (fCheck) {
        fread(magic, 1, 4, fCheck);
        fclose(fCheck);
    }
    if (magic[0] != 'P' || magic[1] != 'K' || magic[2] != 3 || magic[3] != 4) {
        DeleteFileA(destPath);
        return FALSE;
    }

    return TRUE;
}

static BOOL ExtractZip(const char* zipPath, const char* targetDir) {
    char cmd[1024];
    sprintf_s(cmd, "tar.exe -xf \"%s\" -C \"%s\"", zipPath, targetDir);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, targetDir, &si, &pi)) {
        sprintf_s(cmd, "powershell.exe -NoProfile -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"",
                  zipPath, targetDir);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, targetDir, &si, &pi)) {
            return FALSE;
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode == 0);
}

static void DeletePathRecursive(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return;

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        char searchPath[MAX_PATH];
        sprintf_s(searchPath, "%s\\*.*", path);
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(searchPath, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
                    char subPath[MAX_PATH];
                    sprintf_s(subPath, "%s\\%s", path, fd.cFileName);
                    DeletePathRecursive(subPath);
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryA(path);
    } else {
        DeleteFileA(path);
    }
}

static void NormalizePath(char* p) {
    for (char* c = p; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == '\\' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
        p[len - 1] = '\0';
        len--;
    }
}

static BOOL IsProtectedGameDirectory(const char* relPath) {
    static const char* protectedDirs[] = {
        "", ".", "scripts", "modloader", "cleo", "data", "models",
        "txd", "audio", "anim", "movies", "text", "_tools"
    };
    for (size_t i = 0; i < sizeof(protectedDirs) / sizeof(protectedDirs[0]); i++) {
        if (_stricmp(relPath, protectedDirs[i]) == 0) return TRUE;
    }
    return FALSE;
}

static void ProcessDeletions(const char* gameDir) {
    char delFile[MAX_PATH];
    sprintf_s(delFile, "%s\\delete.txt", gameDir);
    FILE* f = fopen(delFile, "r");
    if (!f) return;

    char line[MAX_PATH];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        NormalizePath(p);
        if (p[0] == '\0' || p[0] == '#' || IsProtectedGameDirectory(p)) continue;

        char targetPath[MAX_PATH];
        sprintf_s(targetPath, "%s\\%s", gameDir, p);
        DeletePathRecursive(targetPath);
    }
    fclose(f);
    DeleteFileA(delFile);
}

static BOOL GenerateManifestFromZip(const char* zipPath, const char* outManifestPath) {
    char cmd[1024];
    sprintf_s(cmd, "cmd.exe /c \"tar.exe -tf \"%s\" > \"%s\"\"", zipPath, outManifestPath);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return TRUE;
    }
    return FALSE;
}

static BOOL IsLineInFile(const char* filePath, const char* lineToFind) {
    FILE* f = fopen(filePath, "r");
    if (!f) return FALSE;

    char targetNorm[MAX_PATH];
    strcpy_s(targetNorm, lineToFind);
    NormalizePath(targetNorm);

    char buf[MAX_PATH];
    BOOL found = FALSE;
    while (fgets(buf, sizeof(buf), f)) {
        char* nl = strpbrk(buf, "\r\n");
        if (nl) *nl = '\0';
        char* p = buf;
        while (*p == ' ' || *p == '\t') p++;
        NormalizePath(p);
        if (_stricmp(p, targetNorm) == 0) {
            found = TRUE;
            break;
        }
    }
    fclose(f);
    return found;
}

static void CleanOrphanFilesOnRollback(const char* gameDir, const char* currentManifest, const char* targetManifest) {
    FILE* fOld = fopen(currentManifest, "r");
    if (!fOld) return;

    char line[MAX_PATH];
    while (fgets(line, sizeof(line), fOld)) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        NormalizePath(p);
        if (p[0] == '\0' || p[0] == '#') continue;

        if (_stricmp(p, "delete.txt") == 0) continue;
        if (IsProtectedGameDirectory(p)) continue;

        if (!IsLineInFile(targetManifest, p)) {
            char targetPath[MAX_PATH];
            sprintf_s(targetPath, "%s\\%s", gameDir, p);
            DWORD attrs = GetFileAttributesA(targetPath);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                    RemoveDirectoryA(targetPath);
                } else {
                    DeleteFileA(targetPath);

                    char parentRel[MAX_PATH];
                    strcpy_s(parentRel, p);
                    char* lastSlash = strrchr(parentRel, '\\');
                    if (lastSlash) {
                        *lastSlash = '\0';
                        if (!IsProtectedGameDirectory(parentRel)) {
                            char parentAbs[MAX_PATH];
                            sprintf_s(parentAbs, "%s\\%s", gameDir, parentRel);
                            RemoveDirectoryA(parentAbs);
                        }
                    }
                }
            }
        }
    }
    fclose(fOld);
}

struct UpdateParams {
    char downloadUrl[1024];
    char newVersion[64];
    DWORD gamePid;
    BOOL restartGame;
};

static DWORD WINAPI UpdateWorkerThread(LPVOID lpParam) {
    UpdateParams* p = (UpdateParams*)lpParam;
    char gameDir[MAX_PATH];
    GetGameDir(gameDir, sizeof(gameDir));

    SetStatus("Waiting for GTA Vice City to exit...");
    WaitForGameExit(p->gamePid, 10000);
    Sleep(500);

    char tempZip[MAX_PATH];
    char tempPath[MAX_PATH];
    GetTempPathA(sizeof(tempPath), tempPath);
    sprintf_s(tempZip, "%sgtavc_update.zip", tempPath);

    SetStatus("Connecting to GitHub...");
    SetProgress(5);

    if (!DownloadFileWithProgress(p->downloadUrl, tempZip)) {
        SetStatus("Нет интернета для обновления.");
        MessageBoxW(g_hWnd, L"Нет интернета для обновления.",
                    L"GTA: Vice City - Update Error", MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    SetProgress(80);
    SetStatus("Preparing update files...");

    char toolsDir[MAX_PATH];
    sprintf_s(toolsDir, "%s\\_tools\\UpdateSystem", gameDir);
    CreateDirectoryA(toolsDir, NULL);

    char installedManifest[MAX_PATH];
    sprintf_s(installedManifest, "%s\\installed_manifest.txt", toolsDir);

    char tempManifest[MAX_PATH];
    sprintf_s(tempManifest, "%s\\temp_manifest.txt", toolsDir);

    // 1. Generate file list of incoming version
    GenerateManifestFromZip(tempZip, tempManifest);

    // 2. If rolling back: delete files present in old version that are absent in incoming version
    CleanOrphanFilesOnRollback(gameDir, installedManifest, tempManifest);

    SetProgress(90);
    SetStatus("Extracting update files...");

    if (!ExtractZip(tempZip, gameDir)) {
        SetStatus("Extraction failed!");
        MessageBoxA(g_hWnd, "Failed to extract update files into game folder.",
                    "Update Error", MB_ICONERROR | MB_OK);
        DeleteFileA(tempZip);
        DeleteFileA(tempManifest);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    DeleteFileA(tempZip);

    // 3. Process delete.txt if provided in package
    ProcessDeletions(gameDir);

    // 4. Save incoming manifest as installed manifest
    CopyFileA(tempManifest, installedManifest, FALSE);
    DeleteFileA(tempManifest);

    // 5. Update local version.txt
    char verFile[MAX_PATH];
    sprintf_s(verFile, "%s\\version.txt", gameDir);
    FILE* vf = fopen(verFile, "w");
    if (vf) {
        fprintf(vf, "%s\n", p->newVersion);
        fclose(vf);
    }

    SetProgress(100);
    if (p->restartGame) {
        SetStatus("Complete! Restarting GTA Vice City...");
        Sleep(1200);

        char exePath[MAX_PATH];
        sprintf_s(exePath, "%s\\gta-vc.exe", gameDir);
        ShellExecuteA(NULL, "open", exePath, NULL, gameDir, SW_SHOWNORMAL);
    } else {
        SetStatus("Complete! Version updated.");
        Sleep(1200);
    }

    PostMessage(g_hWnd, WM_CLOSE, 0, 0);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);

    UpdateParams params;
    memset(&params, 0, sizeof(params));

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 4) {
        WideCharToMultiByte(CP_UTF8, 0, argvW[1], -1, params.downloadUrl, sizeof(params.downloadUrl), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, argvW[2], -1, params.newVersion, sizeof(params.newVersion), NULL, NULL);
        params.gamePid = (DWORD)_wtoi(argvW[3]);
        params.restartGame = TRUE;
        if (argc >= 5 && _wcsicmp(argvW[4], L"--no-restart") == 0) {
            params.restartGame = FALSE;
        }
    } else {
        strcpy_s(params.downloadUrl, "https://github.com/itisislom/gtavc-updates/releases/download/v1.0.0/update.zip");
        strcpy_s(params.newVersion, "1.0.0");
        params.gamePid = 0;
        params.restartGame = TRUE;
    }
    if (argvW) LocalFree(argvW);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "GTAVC_Updater_Class";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 420;
    int winH = 140;
    int winX = (scrW - winW) / 2;
    int winY = (scrH - winH) / 2;

    g_hWnd = CreateWindowExA(WS_EX_TOPMOST, "GTAVC_Updater_Class",
                             "GTA: Vice City - Update from Islom Sadriddinov",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             winX, winY, winW, winH, NULL, NULL, hInstance, NULL);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_hStatus = CreateWindowExA(0, "STATIC", "Initializing updater...",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                20, 18, 370, 22, g_hWnd, NULL, hInstance, NULL);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

    g_hProgress = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
                                 WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                 20, 48, 365, 24, g_hWnd, NULL, hInstance, NULL);
    SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    CreateThread(NULL, 0, UpdateWorkerThread, &params, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
