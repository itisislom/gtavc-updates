#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "wininet.lib")

static const char* GITHUB_RELEASES_API = "https://api.github.com/repos/itisislom/gtavc-updates/releases?per_page=5";
static const char* GITHUB_RAW_MANIFEST = "https://raw.githubusercontent.com/itisislom/gtavc-updates/main/version.json";

#define IDC_LIST_RELEASES   2001
#define IDC_EDIT_DESC       2002
#define IDC_BTN_INSTALL     2003
#define IDC_BTN_PLAY        2004

typedef struct ReleaseItem {
    char tag[64];
    char version[64];
    char name[128];
    char date[32];
    char downloadUrl[1024];
    char body[2048];
} ReleaseItem;

struct ManagerContext {
    char localVer[64];
    char gameDir[MAX_PATH];
    HWND hParent;
    ReleaseItem releases[10];
    int releaseCount;
    int selectedIndex;

    HWND hList;
    HWND hDesc;
    HWND hBtnInstall;
    HWND hHeader;
};

static void Log(const char* msg) {
    FILE* f = fopen("UpdateChecker.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
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

static void ReadLocalVersion(const char* gameDir, char* outVer, size_t maxLen) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\version.txt", gameDir);
    FILE* f = fopen(path, "r");
    if (f) {
        if (fgets(outVer, (int)maxLen, f)) {
            char* nl = strpbrk(outVer, "\r\n");
            if (nl) *nl = '\0';
        }
        fclose(f);
    } else {
        strcpy_s(outVer, maxLen, "0.0.0");
    }
}

static BOOL HasShownUpdaterInfo(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\updater.ini", gameDir);
    return GetPrivateProfileIntA("Updater", "InfoShown", 0, path) == 1;
}

static void MarkUpdaterInfoShown(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\updater.ini", gameDir);
    WritePrivateProfileStringA("Updater", "InfoShown", "1", path);
}

// Version comparison helper: returns 1 if v1 > v2, -1 if v1 < v2, 0 if equal
static int CompareSemanticVersions(const char* v1, const char* v2) {
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;

    const char* p1 = v1;
    if (*p1 == 'v' || *p1 == 'V') p1++;
    const char* p2 = v2;
    if (*p2 == 'v' || *p2 == 'V') p2++;

    sscanf_s(p1, "%d.%d.%d", &a1, &a2, &a3);
    sscanf_s(p2, "%d.%d.%d", &b1, &b2, &b3);

    if (a1 > b1) return 1;
    if (a1 < b1) return -1;
    if (a2 > b2) return 1;
    if (a2 < b2) return -1;
    if (a3 > b3) return 1;
    if (a3 < b3) return -1;
    return 0;
}

static BOOL ExtractJsonString(const char* start, const char* end, const char* key, char* outVal, size_t maxLen) {
    char pattern[128];
    sprintf_s(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(start, pattern);
    if (!p || (end && p >= end)) return FALSE;

    p = strchr(p + strlen(pattern), ':');
    if (!p || (end && p >= end)) return FALSE;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '\"') return FALSE;
    p++;

    const char* q = p;
    size_t outIdx = 0;
    while (*q && (end == NULL || q < end)) {
        if (*q == '\\') {
            q++;
            if (*q == '\"') { if (outIdx + 1 < maxLen) outVal[outIdx++] = '\"'; }
            else if (*q == 'n') { if (outIdx + 2 < maxLen) { outVal[outIdx++] = '\r'; outVal[outIdx++] = '\n'; } }
            else if (*q == 'r') { /* ignore */ }
            else if (*q == 't') { if (outIdx + 1 < maxLen) outVal[outIdx++] = '\t'; }
            else { if (outIdx + 1 < maxLen) outVal[outIdx++] = *q; }
            q++;
        } else if (*q == '\"') {
            break;
        } else {
            if (outIdx + 1 < maxLen) outVal[outIdx++] = *q;
            q++;
        }
    }
    outVal[outIdx] = '\0';
    return TRUE;
}

static int ParseReleasesFromJson(const char* json, ReleaseItem* items, int maxItems) {
    int count = 0;
    const char* p = json;

    while (count < maxItems) {
        const char* tagPos = strstr(p, "\"tag_name\"");
        if (!tagPos) break;

        const char* nextTag = strstr(tagPos + 10, "\"tag_name\"");

        ReleaseItem item;
        memset(&item, 0, sizeof(item));

        ExtractJsonString(tagPos, nextTag, "tag_name", item.tag, sizeof(item.tag));
        const char* v = item.tag;
        if (*v == 'v' || *v == 'V') v++;
        strcpy_s(item.version, sizeof(item.version), v);

        ExtractJsonString(tagPos, nextTag, "name", item.name, sizeof(item.name));
        ExtractJsonString(tagPos, nextTag, "published_at", item.date, sizeof(item.date));
        if (strlen(item.date) >= 10) item.date[10] = '\0';

        ExtractJsonString(tagPos, nextTag, "body", item.body, sizeof(item.body));
        ExtractJsonString(tagPos, nextTag, "browser_download_url", item.downloadUrl, sizeof(item.downloadUrl));

        items[count++] = item;

        if (!nextTag) break;
        p = nextTag;
    }
    return count;
}

static BOOL FetchUrlData(HINTERNET hInternet, const char* url, char* outBuf, DWORD maxLen) {
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url, "User-Agent: GTAVC-UpdateChecker\r\n", -1,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) return FALSE;

    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    if (HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusLen, NULL)) {
        if (statusCode != 200) {
            InternetCloseHandle(hUrl);
            return FALSE;
        }
    }

    DWORD bytesRead = 0;
    DWORD totalRead = 0;
    while (InternetReadFile(hUrl, outBuf + totalRead, maxLen - 1 - totalRead, &bytesRead) && bytesRead > 0) {
        totalRead += bytesRead;
        if (totalRead >= maxLen - 1) break;
    }
    outBuf[totalRead] = '\0';

    InternetCloseHandle(hUrl);
    return (totalRead > 0);
}

static void UpdateSelectionView(ManagerContext* ctx) {
    if (ctx->selectedIndex < 0 || ctx->selectedIndex >= ctx->releaseCount) return;

    ReleaseItem* item = &ctx->releases[ctx->selectedIndex];
    int cmp = CompareSemanticVersions(item->version, ctx->localVer);

    char descText[3072];
    sprintf_s(descText, sizeof(descText),
              "Release:  %s\r\n"
              "Tag:      %s\r\n"
              "Date:     %s\r\n"
              "Status:   %s\r\n"
              "--------------------------------------------------\r\n"
              "%s",
              item->name, item->tag, item->date,
              (cmp > 0 ? "New Update Available" : (cmp == 0 ? "Currently Installed" : "Older Version (Rollback Available)")),
              item->body[0] ? item->body : "(No changelog description provided)");

    SetWindowTextA(ctx->hDesc, descText);

    char btnText[64];
    if (cmp > 0) {
        sprintf_s(btnText, sizeof(btnText), "Install %s", item->tag);
        EnableWindow(ctx->hBtnInstall, TRUE);
    } else if (cmp < 0) {
        sprintf_s(btnText, sizeof(btnText), "Rollback to %s", item->tag);
        EnableWindow(ctx->hBtnInstall, TRUE);
    } else {
        sprintf_s(btnText, sizeof(btnText), "Reinstall %s", item->tag);
        EnableWindow(ctx->hBtnInstall, TRUE);
    }
    SetWindowTextA(ctx->hBtnInstall, btnText);
}

static LRESULT CALLBACK ManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ManagerContext* ctx = (ManagerContext*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            ctx = (ManagerContext*)cs->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);

            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            // Header info
            char headerBuf[256];
            sprintf_s(headerBuf, sizeof(headerBuf),
                      "Current Installed: v%s    |    Latest on GitHub: %s",
                      ctx->localVer,
                      ctx->releaseCount > 0 ? ctx->releases[0].tag : "Unknown");

            ctx->hHeader = CreateWindowExA(0, "STATIC", headerBuf,
                                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           20, 15, 520, 20, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(ctx->hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Left: Releases label & Listbox
            HWND hLblList = CreateWindowExA(0, "STATIC", "Available Releases (Latest 5):",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            20, 42, 200, 18, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hLblList, WM_SETFONT, (WPARAM)hFont, TRUE);

            ctx->hList = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL,
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
                                         20, 62, 210, 270, hWnd, (HMENU)IDC_LIST_RELEASES, cs->hInstance, NULL);
            SendMessage(ctx->hList, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Populate Listbox
            for (int i = 0; i < ctx->releaseCount; i++) {
                int cmp = CompareSemanticVersions(ctx->releases[i].version, ctx->localVer);
                char lineBuf[128];
                if (cmp > 0) {
                    sprintf_s(lineBuf, sizeof(lineBuf), "%s  [New]", ctx->releases[i].tag);
                } else if (cmp == 0) {
                    sprintf_s(lineBuf, sizeof(lineBuf), "%s  [Installed]", ctx->releases[i].tag);
                } else {
                    sprintf_s(lineBuf, sizeof(lineBuf), "%s  [Rollback]", ctx->releases[i].tag);
                }
                SendMessageA(ctx->hList, LB_ADDSTRING, 0, (LPARAM)lineBuf);
            }
            SendMessage(ctx->hList, LB_SETCURSEL, ctx->selectedIndex, 0);

            // Right: Description label & Edit control
            HWND hLblDesc = CreateWindowExA(0, "STATIC", "Release Notes & Details:",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            245, 42, 300, 18, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hLblDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

            ctx->hDesc = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         245, 62, 300, 270, hWnd, (HMENU)IDC_EDIT_DESC, cs->hInstance, NULL);
            SendMessage(ctx->hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Bottom Buttons
            ctx->hBtnInstall = CreateWindowExA(0, "BUTTON", "Install Selected",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                               245, 345, 160, 32, hWnd, (HMENU)IDC_BTN_INSTALL, cs->hInstance, NULL);
            SendMessage(ctx->hBtnInstall, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnPlay = CreateWindowExA(0, "BUTTON", "Play / Close",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                            415, 345, 130, 32, hWnd, (HMENU)IDC_BTN_PLAY, cs->hInstance, NULL);
            SendMessage(hBtnPlay, WM_SETFONT, (WPARAM)hFont, TRUE);

            UpdateSelectionView(ctx);
            SetFocus(hBtnPlay);
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDC_LIST_RELEASES && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
                int curSel = (int)SendMessage(ctx->hList, LB_GETCURSEL, 0, 0);
                if (curSel != LB_ERR) {
                    ctx->selectedIndex = curSel;
                    UpdateSelectionView(ctx);
                }
                return 0;
            }

            if (id == IDC_BTN_PLAY) {
                DestroyWindow(hWnd);
                return 0;
            }

            if (id == IDC_BTN_INSTALL) {
                if (ctx->selectedIndex < 0 || ctx->selectedIndex >= ctx->releaseCount) return 0;

                ReleaseItem* item = &ctx->releases[ctx->selectedIndex];
                int cmp = CompareSemanticVersions(item->version, ctx->localVer);

                char confirmMsg[512];
                if (cmp > 0) {
                    sprintf_s(confirmMsg, sizeof(confirmMsg),
                              "Install update to %s?\n\n"
                              "The game will close to apply the update.", item->tag);
                } else if (cmp < 0) {
                    sprintf_s(confirmMsg, sizeof(confirmMsg),
                              "Roll back to older version %s?\n\n"
                              "Files introduced in newer versions will be cleanly removed and the game will close.", item->tag);
                } else {
                    sprintf_s(confirmMsg, sizeof(confirmMsg),
                              "Reinstall version %s?\n\n"
                              "The game will close to reinstall files.", item->tag);
                }

                int res = MessageBoxA(hWnd, confirmMsg, "Confirm Version Switch",
                                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
                if (res == IDYES) {
                    char updaterPath[MAX_PATH];
                    sprintf_s(updaterPath, "%s\\updater.exe", ctx->gameDir);

                    char cmdLine[2048];
                    sprintf_s(cmdLine, "\"%s\" \"%s\" \"%s\" %lu --no-restart",
                              updaterPath, item->downloadUrl, item->version, GetCurrentProcessId());

                    STARTUPINFOA si = { sizeof(si) };
                    PROCESS_INFORMATION pi;
                    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, ctx->gameDir, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    }
                    ExitProcess(0);
                }
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void ShowUpdateManagerDialog(ManagerContext* ctx) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = ManagerWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "GTAVC_UpdateManagerDialog";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hParent = ctx->hParent;
    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

    int winW = 580;
    int winH = 430;
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hDlg = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
                                "GTAVC_UpdateManagerDialog", "GTA: Vice City - Update Manager",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                posX, posY, winW, winH,
                                hParent, NULL, wc.hInstance, ctx);

    SetForegroundWindow(hDlg);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
}

// Startup one-time info dialog
static LRESULT CALLBACK InfoDialogWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            const char* infoText =
                "GTA: Vice City - Update System\n"
                "Created by Islom Sadriddinov\n\n"
                "- Press '0' during gameplay at any time to open Update Manager.\n"
                "- You have full freedom of choice: install new updates or roll back.\n"
                "- Updates are downloaded and applied automatically from GitHub.";

            HWND hText = CreateWindowExA(0, "STATIC", infoText,
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         30, 20, 390, 105, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hText, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnOk = CreateWindowExA(0, "BUTTON", "OK",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                          160, 135, 120, 32, hWnd, (HMENU)IDOK, cs->hInstance, NULL);
            SendMessage(hBtnOk, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetFocus(hBtnOk);
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hWnd);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hWnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static void ShowUpdaterInfoDialog(HWND hParent) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = InfoDialogWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "GTAVC_UpdaterInfoDialog";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

    int winW = 450;
    int winH = 220;
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hDlg = CreateWindowExA(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
                                "GTAVC_UpdaterInfoDialog", "GTA: Vice City - Updater Info",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                posX, posY, winW, winH,
                                hParent, NULL, wc.hInstance, NULL);

    SetForegroundWindow(hDlg);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
}

static int FetchReleaseList(ReleaseItem* items, int maxItems) {
    HINTERNET hInternet = InternetOpenA("GTAVC-UpdateChecker", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return 0;

    char jsonBuf[32768] = {0};
    int count = 0;

    // 1. Query GitHub API for 5 releases
    if (FetchUrlData(hInternet, GITHUB_RELEASES_API, jsonBuf, sizeof(jsonBuf))) {
        count = ParseReleasesFromJson(jsonBuf, items, maxItems);
    }

    // 2. Fallback to raw manifest if API fails
    if (count == 0) {
        if (FetchUrlData(hInternet, GITHUB_RAW_MANIFEST, jsonBuf, sizeof(jsonBuf))) {
            ReleaseItem single;
            memset(&single, 0, sizeof(single));
            ExtractJsonString(jsonBuf, NULL, "version", single.version, sizeof(single.version));
            sprintf_s(single.tag, sizeof(single.tag), "v%s", single.version);
            ExtractJsonString(jsonBuf, NULL, "download_url", single.downloadUrl, sizeof(single.downloadUrl));
            strcpy_s(single.name, sizeof(single.name), "Release from raw manifest");
            strcpy_s(single.body, sizeof(single.body), "Latest published update.");
            items[count++] = single;
        }
    }

    InternetCloseHandle(hInternet);
    return count;
}

static void OpenUpdateManager(HWND hGameWnd, BOOL showAlways) {
    ManagerContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    GetGameDir(ctx.gameDir, sizeof(ctx.gameDir));
    ReadLocalVersion(ctx.gameDir, ctx.localVer, sizeof(ctx.localVer));
    ctx.hParent = hGameWnd;

    ctx.releaseCount = FetchReleaseList(ctx.releases, 5);
    ctx.selectedIndex = 0;

    if (ctx.releaseCount == 0) {
        if (showAlways) {
            MessageBoxA(hGameWnd, "No releases found or network unreachable.", "Update Manager", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    // Check if newest release > localVer
    BOOL hasNewer = (CompareSemanticVersions(ctx.releases[0].version, ctx.localVer) > 0);

    // If triggered by key '0' -> showAlways is TRUE (always show)
    // If triggered on startup -> only show if hasNewer is TRUE
    if (showAlways || hasNewer) {
        ShowUpdateManagerDialog(&ctx);
    }
}

static DWORD WINAPI MonitorKeysThread(LPVOID lpParam) {
    Sleep(2500);

    HWND hGameWnd = FindWindowA("Grand Theft Auto", NULL);
    if (!hGameWnd) hGameWnd = GetActiveWindow();

    char gameDir[MAX_PATH];
    GetGameDir(gameDir, sizeof(gameDir));

    // 1. One-time Startup Info Dialog (only once ever across all relaunches)
    if (!HasShownUpdaterInfo(gameDir)) {
        Log("Showing one-time Updater Info dialog...");
        ShowUpdaterInfoDialog(hGameWnd);
        MarkUpdaterInfoShown(gameDir);
    }

    // 2. On startup: if a newer version is available, show the Update Manager dialog
    // (with complete freedom of choice: "Play / Close" or "Install")
    OpenUpdateManager(hGameWnd, FALSE);

    Log("MonitorKeysThread active: listening for key '0'...");

    // 3. Continuous listener for key '0'
    BOOL wasDown = FALSE;
    while (TRUE) {
        Sleep(50);

        HWND fg = GetForegroundWindow();
        if (fg == hGameWnd || hGameWnd == NULL) {
            BOOL isDown = ((GetAsyncKeyState('0') & 0x8000) != 0) ||
                          ((GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0);

            if (isDown && !wasDown) {
                wasDown = TRUE;
                Log("Key '0' pressed. Opening Update Manager dialog...");
                OpenUpdateManager(fg, TRUE);
            } else if (!isDown) {
                wasDown = FALSE;
            }
        }
    }
    return 0;
}

#ifdef STANDALONE_TEST
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    OpenUpdateManager(NULL, TRUE);
    return 0;
}
#else
BOOL WINAPI DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        Log("UpdateChecker.asi attached. Starting monitor thread...");
        CreateThread(NULL, 0, MonitorKeysThread, NULL, 0, NULL);
    }
    return TRUE;
}
#endif
