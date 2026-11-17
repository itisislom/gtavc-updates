#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include "updater_core.h"
#include "../common/paths.h"
#include "../common/crypto.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define IDC_BTN_CANCEL 1010

static HWND g_hWnd = NULL;
static HWND g_hStatus = NULL;
static HWND g_hProgress = NULL;
static HWND g_hBtnCancel = NULL;
static volatile BOOL g_bCancelRequested = FALSE;

static void SetStatusW(const wchar_t* text) {
    if (g_hStatus) {
        SetWindowTextW(g_hStatus, text);
        UpdateWindow(g_hStatus);
    }
}

static void SetProgress(int percent) {
    if (g_hProgress) {
        SendMessage(g_hProgress, PBM_SETPOS, percent, 0);
    }
}

static void GetGameDirW(wchar_t* outDir, size_t maxLen) {
    GetModuleFileNameW(NULL, outDir, (DWORD)maxLen);
    wchar_t* lastSlash = wcsrchr(outDir, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    } else {
        wcscpy_s(outDir, maxLen, L".");
    }
}

static BOOL WaitForGameExitWithUI(DWORD pid, DWORD maxWaitMs) {
    if (pid == 0) return TRUE;
    HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hProcess) return TRUE;

    ULONGLONG startTick = GetTickCount64();
    while (!g_bCancelRequested) {
        DWORD res = WaitForSingleObject(hProcess, 250);
        if (res == WAIT_OBJECT_0) {
            CloseHandle(hProcess);
            return TRUE;
        }

        ULONGLONG elapsed = GetTickCount64() - startTick;
        if (elapsed >= maxWaitMs) {
            CloseHandle(hProcess);
            return FALSE;
        }

        ULONGLONG remainSec = (maxWaitMs - elapsed) / 1000;
        wchar_t statusBuf[256];
        swprintf_s(statusBuf, L"Ожидание выхода GTA: Vice City (%llu сек)...", remainSec);
        SetStatusW(statusBuf);

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CloseHandle(hProcess);
    return FALSE;
}

#define MAX_DOWNLOAD_LIMIT (1024ULL * 1024ULL * 1024ULL) // 1 GB

static BOOL DownloadFileWithProgressW(const wchar_t* initialUrl, const wchar_t* destPath, wchar_t* outErrorMsg, size_t errLen) {
    if (!ValidateUrl(initialUrl)) {
        if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Недопустимый URL источника.");
        return FALSE;
    }

    HINTERNET hInternet = InternetOpenW(L"GTAVC-Updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Нет интернета для обновления.");
        return FALSE;
    }

    std::wstring currentUrl = initialUrl;
    HINTERNET hUrl = NULL;
    int redirectCount = 0;
    const int MAX_REDIRECTS = 5;

    while (redirectCount <= MAX_REDIRECTS) {
        hUrl = InternetOpenUrlW(hInternet, currentUrl.c_str(), NULL, 0,
                                INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT, 0);
        if (!hUrl) {
            DWORD err = GetLastError();
            InternetCloseHandle(hInternet);
            if (outErrorMsg) {
                if (err == ERROR_INTERNET_TIMEOUT) {
                    swprintf_s(outErrorMsg, errLen, L"Сервер не отвечает (таймаут соединения).");
                } else {
                    swprintf_s(outErrorMsg, errLen, L"Нет интернета для обновления.");
                }
            }
            return FALSE;
        }

        DWORD statusCode = 0;
        DWORD statusLen = sizeof(statusCode);
        if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusLen, NULL)) {
            if (statusCode == 301 || statusCode == 302 || statusCode == 303 || statusCode == 307 || statusCode == 308) {
                wchar_t locBuf[2048] = { 0 };
                DWORD locLen = sizeof(locBuf);
                if (!HttpQueryInfoW(hUrl, HTTP_QUERY_LOCATION, locBuf, &locLen, NULL) || locBuf[0] == L'\0') {
                    InternetCloseHandle(hUrl);
                    InternetCloseHandle(hInternet);
                    if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Пустой заголовок редиректа от сервера.");
                    return FALSE;
                }

                InternetCloseHandle(hUrl);
                hUrl = NULL;

                if (!ValidateRedirectTarget(locBuf)) {
                    InternetCloseHandle(hInternet);
                    if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Неразрешённый хост редиректа: %ls", locBuf);
                    return FALSE;
                }

                currentUrl = locBuf;
                redirectCount++;
                if (redirectCount > MAX_REDIRECTS) {
                    InternetCloseHandle(hInternet);
                    if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Превышено максимальное количество редиректов (5).");
                    return FALSE;
                }
                continue;
            }

            if (statusCode != 200) {
                InternetCloseHandle(hUrl);
                InternetCloseHandle(hInternet);
                if (outErrorMsg) {
                    if (statusCode == 404) {
                        swprintf_s(outErrorMsg, errLen, L"Релиз не найден на сервере (HTTP 404).");
                    } else if (statusCode == 403 || statusCode == 429) {
                        swprintf_s(outErrorMsg, errLen, L"Превышен лимит запросов к GitHub (rate limit). Подождите 1 час.");
                    } else {
                        swprintf_s(outErrorMsg, errLen, L"Ошибка сервера при скачивании (HTTP %lu).", statusCode);
                    }
                }
                return FALSE;
            }
        }

        break;
    }

    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    DWORD totalBytes = 0;
    DWORD len = sizeof(totalBytes);
    if (!HttpQueryInfoW(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &totalBytes, &len, NULL)) {
        totalBytes = 0;
    }
    if (totalBytes > MAX_DOWNLOAD_LIMIT) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, destPath, L"wb");
    if (!f) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return FALSE;
    }

    std::vector<char> buffer(32768);
    DWORD bytesRead = 0;
    unsigned long long downloadedBytes = 0;
    BOOL success = TRUE;

    while (InternetReadFile(hUrl, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
        if (g_bCancelRequested) {
            success = FALSE;
            break;
        }

        downloadedBytes += bytesRead;
        if (downloadedBytes > MAX_DOWNLOAD_LIMIT) {
            success = FALSE;
            break;
        }

        if (fwrite(buffer.data(), 1, bytesRead, f) != bytesRead) {
            success = FALSE;
            break;
        }

        if (totalBytes > 0) {
            int pct = (int)((downloadedBytes * 100ULL) / totalBytes);
            SetProgress(pct);

            wchar_t statusBuf[128];
            swprintf_s(statusBuf, L"Загрузка: %d%% (%llu / %lu КБ)...",
                       pct, downloadedBytes / 1024, totalBytes / 1024);
            SetStatusW(statusBuf);
        } else {
            wchar_t statusBuf[128];
            swprintf_s(statusBuf, L"Загрузка: %llu КБ...", downloadedBytes / 1024);
            SetStatusW(statusBuf);
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
        DeleteFileW(destPath);
        return FALSE;
    }

    unsigned char magic[4] = {0};
    FILE* fCheck = nullptr;
    _wfopen_s(&fCheck, destPath, L"rb");
    if (fCheck) {
        fread(magic, 1, 4, fCheck);
        fclose(fCheck);
    }
    if (magic[0] != 'P' || magic[1] != 'K' || magic[2] != 3 || magic[3] != 4) {
        DeleteFileW(destPath);
        return FALSE;
    }

    return TRUE;
}

static void ReadCurrentVersionW(const wchar_t* gameDir, wchar_t* outVer, size_t maxLen) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\version.txt", gameDir);
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (f) {
        if (fgetws(outVer, (int)maxLen, f)) {
            wchar_t* nl = wcspbrk(outVer, L"\r\n");
            if (nl) *nl = L'\0';
        }
        fclose(f);
    } else {
        wcscpy_s(outVer, maxLen, L"0.0.0");
    }
}

struct UpdateParams {
    wchar_t downloadUrl[1024];
    wchar_t newVersion[64];
    wchar_t expectedSha256[65];
    DWORD gamePid;
    BOOL restartGame;
    int failAt;
};

static DWORD WINAPI UpdateWorkerThread(LPVOID lpParam) {
    UpdateParams* p = (UpdateParams*)lpParam;
    wchar_t gameDir[MAX_PATH];
    GetGameDirW(gameDir, sizeof(gameDir) / sizeof(gameDir[0]));

    // Security validation of input parameters
    if (!ValidateUrl(p->downloadUrl)) {
        SetStatusW(L"Недопустимый URL источника.");
        MessageBoxW(g_hWnd, L"URL источника обновления не входит в доверенный список GitHub или не использует HTTPS.",
                    L"Ошибка безопасности", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    if (!ValidateSemVer(p->newVersion)) {
        SetStatusW(L"Недопустимый формат версии.");
        MessageBoxW(g_hWnd, L"Формат целевой версии не соответствует стандарту SemVer.",
                    L"Ошибка валидации", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    SetStatusW(L"Ожидание выхода GTA: Vice City (до 5 мин)...");
    if (!WaitForGameExitWithUI(p->gamePid, 300000)) { // 5 minutes max
        if (g_bCancelRequested) {
            SetStatusW(L"Обновление отменено пользователем.");
        } else {
            SetStatusW(L"Таймаут ожидания выхода игры.");
            MessageBoxW(g_hWnd, L"Время ожидания выхода игры истекло.", L"Обновление отменено", MB_ICONWARNING | MB_OK);
        }
        Sleep(1000);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }
    Sleep(300);

    wchar_t tempWorkDir[MAX_PATH];
    if (!CreateSecureTempDir(tempWorkDir, MAX_PATH)) {
        SetStatusW(L"Ошибка создания временной папки.");
        MessageBoxW(g_hWnd, L"Не удалось создать временную директорию.", L"Ошибка", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    wchar_t tempZip[MAX_PATH];
    swprintf_s(tempZip, L"%s\\update.zip", tempWorkDir);

    SetStatusW(L"Подключение к GitHub...");
    SetProgress(5);

    wchar_t downloadErr[512] = { 0 };
    if (!DownloadFileWithProgressW(p->downloadUrl, tempZip, downloadErr, 512)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        if (g_bCancelRequested) {
            SetStatusW(L"Обновление отменено.");
        } else {
            SetStatusW(downloadErr[0] ? downloadErr : L"Нет интернета для обновления.");
            MessageBoxW(g_hWnd, downloadErr[0] ? downloadErr : L"Нет интернета для обновления.",
                        L"GTA: Vice City - Update Error", MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        }
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    // If expectedSha256 was not passed via CLI, attempt to fetch update.zip.sha256 from repository
    if (p->expectedSha256[0] == L'\0') {
        SetStatusW(L"Получение контрольной суммы update.zip.sha256...");
        wchar_t shaUrl[1200];
        swprintf_s(shaUrl, L"%ls.sha256", p->downloadUrl);
        wchar_t shaDest[MAX_PATH];
        swprintf_s(shaDest, L"%s\\update.zip.sha256", tempWorkDir);
        wchar_t shaErr[512] = { 0 };
        if (DownloadFileWithProgressW(shaUrl, shaDest, shaErr, 512)) {
            FILE* sf = nullptr;
            _wfopen_s(&sf, shaDest, L"r");
            if (sf) {
                if (fgetws(p->expectedSha256, 65, sf)) {
                    wchar_t* nl = wcspbrk(p->expectedSha256, L"\r\n ");
                    if (nl) *nl = L'\0';
                }
                fclose(sf);
            }
        }
    }

    // Enforce mandatory SHA-256 check
    if (p->expectedSha256[0] == L'\0') {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Отсутствует контрольная сумма SHA-256.");
        MessageBoxW(g_hWnd, L"Контрольная сумма update.zip.sha256 отсутствует на сервере! Установка отменена в целях безопасности.",
                    L"Ошибка целостности", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    SetStatusW(L"Проверка контрольной суммы SHA-256...");
    if (!VerifySha256(tempZip, p->expectedSha256)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Несовпадение SHA-256 архива.");
        MessageBoxW(g_hWnd, L"Контрольная сумма SHA-256 скачанного обновления не совпадает с ожидаемой!\nАрхив удалён.",
                    L"Ошибка целостности", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    SetProgress(75);
    SetStatusW(L"Проверка содержимого архива...");

    std::vector<std::wstring> zipFiles;
    wchar_t errBuf[512] = { 0 };
    if (!ValidateZipContents(tempZip, gameDir, zipFiles, errBuf, 512)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Ошибка безопасности архива.");
        MessageBoxW(g_hWnd, errBuf, L"Ошибка проверки архива", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    SetProgress(82);
    SetStatusW(L"Распаковка во временную папку...");

    wchar_t stageDir[MAX_PATH];
    swprintf_s(stageDir, L"%s\\stage", tempWorkDir);
    if (!ExtractZipToStage(tempZip, stageDir, errBuf, 512)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Ошибка распаковки.");
        MessageBoxW(g_hWnd, errBuf, L"Ошибка распаковки", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    if (!ValidateStageDirectory(stageDir, 20000, 2ULL * 1024 * 1024 * 1024, errBuf, 512)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Лимит файлов или reparse point.");
        MessageBoxW(g_hWnd, errBuf, L"Превышение лимитов архива", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    wchar_t currentVer[64] = { 0 };
    ReadCurrentVersionW(gameDir, currentVer, sizeof(currentVer) / sizeof(currentVer[0]));

    SetProgress(90);
    SetStatusW(L"Применение обновления...");

    UpdateTransactionResult txRes;
    if (!ApplyUpdate(gameDir, stageDir, currentVer, p->newVersion, p->failAt, &txRes)) {
        SafeDeleteDirectoryRecursive(tempWorkDir);
        SetStatusW(L"Сбой обновления! Произведён откат.");
        wchar_t failMsg[1024];
        swprintf_s(failMsg, L"Не удалось применить обновление:\n%ls\n\nВсе изменения были автоматически откатаны.", txRes.errorMessage);
        MessageBoxW(g_hWnd, failMsg, L"Ошибка обновления", MB_ICONERROR | MB_OK);
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        return 1;
    }

    // Update version.txt
    wchar_t verFile[MAX_PATH];
    swprintf_s(verFile, L"%s\\version.txt", gameDir);
    FILE* vf = nullptr;
    _wfopen_s(&vf, verFile, L"w");
    if (vf) {
        fwprintf(vf, L"%ls\n", p->newVersion);
        fclose(vf);
    }

    SafeDeleteDirectoryRecursive(tempWorkDir);
    PruneOldBackups(gameDir, 5);

    SetProgress(100);
    if (p->restartGame) {
        SetStatusW(L"Готово! Запуск GTA Vice City...");
        Sleep(1200);

        wchar_t exePath[MAX_PATH];
        swprintf_s(exePath, L"%s\\gta-vc.exe", gameDir);
        ShellExecuteW(NULL, L"open", exePath, NULL, gameDir, SW_SHOWNORMAL);
    } else {
        SetStatusW(L"Готово! Версия обновлена.");
        Sleep(1200);
    }

    PostMessage(g_hWnd, WM_CLOSE, 0, 0);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_CANCEL) {
                g_bCancelRequested = TRUE;
                SetStatusW(L"Отмена...");
                EnableWindow(g_hBtnCancel, FALSE);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icex);

    wchar_t gameDir[MAX_PATH];
    GetGameDirW(gameDir, sizeof(gameDir) / sizeof(gameDir[0]));

    // Remove legacy updater.exe.old from previous self-update if present
    wchar_t oldUpdater[MAX_PATH];
    swprintf_s(oldUpdater, L"%s\\updater.exe.old", gameDir);
    DeleteFileW(oldUpdater);

    // Write permission check on game directory
    wchar_t testFile[MAX_PATH];
    swprintf_s(testFile, L"%s\\.write_test", gameDir);
    HANDLE hTest = CreateFileW(testFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hTest == INVALID_HANDLE_VALUE) {
        BOOL isElevated = FALSE;
        HANDLE hToken = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            TOKEN_ELEVATION elev = { 0 };
            DWORD cbSize = sizeof(elev);
            if (GetTokenInformation(hToken, TokenElevation, &elev, sizeof(elev), &cbSize)) {
                isElevated = (elev.TokenIsElevated != 0);
            }
            CloseHandle(hToken);
        }
        if (!isElevated) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            HINSTANCE hInst = ShellExecuteW(NULL, L"runas", exePath, GetCommandLineW(), gameDir, SW_SHOWNORMAL);
            if ((INT_PTR)hInst > 32) {
                return 0; // successfully launched elevated instance
            }
        }
        MessageBoxW(NULL, L"Требуются права администратора для записи в папку игры.",
                    L"Ошибка прав доступа", MB_ICONERROR | MB_OK);
        return 1;
    }
    CloseHandle(hTest);
    DeleteFileW(testFile);

    // Check for pending journal crash recovery on startup
    if (HasPendingJournal(gameDir)) {
        int choice = MessageBoxW(NULL,
            L"Обнаружено незавершённое обновление GTA: Vice City.\n\n"
            L"Восстановить предыдущую версию игры из резервной копии?",
            L"Аварийное восстановление",
            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
        if (choice == IDYES) {
            UpdateTransactionResult recRes;
            if (RecoverPendingTransaction(gameDir, &recRes)) {
                MessageBoxW(NULL, L"Предыдущая версия игры успешно восстановлена.",
                            L"Восстановление завершено", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxW(NULL, L"Ошибка восстановления из резервной копии.",
                            L"Ошибка", MB_OK | MB_ICONERROR);
            }
        }
    }

    UpdateParams params;
    memset(&params, 0, sizeof(params));
    params.failAt = -1;

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 4) {
        wcsncpy_s(params.downloadUrl, argvW[1], 1023);
        wcsncpy_s(params.newVersion, argvW[2], 63);
        params.gamePid = (DWORD)_wtoi(argvW[3]);
        params.restartGame = TRUE;

        for (int i = 4; i < argc; i++) {
            if (_wcsicmp(argvW[i], L"--no-restart") == 0) {
                params.restartGame = FALSE;
            } else if (_wcsicmp(argvW[i], L"--fail-at") == 0 && i + 1 < argc) {
                params.failAt = _wtoi(argvW[++i]);
            } else if (_wcsicmp(argvW[i], L"--sha256") == 0 && i + 1 < argc) {
                wcsncpy_s(params.expectedSha256, argvW[++i], 64);
            }
        }
    } else {
        wcscpy_s(params.downloadUrl, L"https://github.com/itisislom/gtavc-updates/releases/download/v1.0.0/update.zip");
        wcscpy_s(params.newVersion, L"1.0.0");
        params.gamePid = 0;
        params.restartGame = TRUE;
    }
    if (argvW) LocalFree(argvW);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"GTAVC_Updater_Class";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 460;
    int winH = 150;
    int winX = (scrW - winW) / 2;
    int winY = (scrH - winH) / 2;

    g_hWnd = CreateWindowExW(WS_EX_TOPMOST, L"GTAVC_Updater_Class",
                             L"GTA: Vice City - Update from Islom Sadriddinov",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             winX, winY, winW, winH, NULL, NULL, hInstance, NULL);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_hStatus = CreateWindowExW(0, L"STATIC", L"Инициализация апдейтера...",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                20, 18, 410, 22, g_hWnd, NULL, hInstance, NULL);
    SendMessage(g_hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

    g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
                                  WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                  20, 48, 305, 24, g_hWnd, NULL, hInstance, NULL);
    SendMessage(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(g_hProgress, PBM_SETPOS, 0, 0);

    g_hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Отмена",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  335, 47, 95, 26, g_hWnd, (HMENU)IDC_BTN_CANCEL, hInstance, NULL);
    SendMessage(g_hBtnCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    CreateThread(NULL, 0, UpdateWorkerThread, &params, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
