#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include <memory>
#include "../common/paths.h"
#include "../common/crypto.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "wininet.lib")

static const wchar_t* GITHUB_RELEASES_API = L"https://api.github.com/repos/itisislom/gtavc-updates/releases?per_page=5";
static const wchar_t* GITHUB_RAW_MANIFEST = L"https://raw.githubusercontent.com/itisislom/gtavc-updates/main/version.json";

#define IDC_LIST_RELEASES   2001
#define IDC_EDIT_DESC       2002
#define IDC_BTN_INSTALL     2003
#define IDC_BTN_PLAY        2004

typedef struct ReleaseItem {
    wchar_t tag[64];
    wchar_t version[64];
    wchar_t name[128];
    wchar_t date[32];
    wchar_t downloadUrl[1024];
    wchar_t sha256[65];
    std::string body;
} ReleaseItem;

struct ManagerContext {
    wchar_t localVer[64];
    wchar_t gameDir[MAX_PATH];
    HWND hParent;
    ReleaseItem releases[10];
    int releaseCount;
    int selectedIndex;

    HWND hList;
    HWND hDesc;
    HWND hBtnInstall;
    HWND hHeader;
};

static void GetGameDirW(wchar_t* outDir, size_t maxLen) {
    GetModuleFileNameW(NULL, outDir, (DWORD)maxLen);
    wchar_t* lastSlash = wcsrchr(outDir, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
    } else {
        wcscpy_s(outDir, maxLen, L".");
    }
}

static void GetUpdaterIniPath(const wchar_t* gameDir, wchar_t* outIniPath, size_t maxLen) {
    wchar_t testFile[MAX_PATH];
    swprintf_s(testFile, L"%s\\.write_test", gameDir);
    HANDLE h = CreateFileW(testFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        DeleteFileW(testFile);
        swprintf_s(outIniPath, maxLen, L"%s\\updater.ini", gameDir);
        return;
    }

    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        swprintf_s(outIniPath, maxLen, L"%s\\GTAVC-UpdateManager", localAppData);
        CreateDirectoryW(outIniPath, NULL);
        swprintf_s(outIniPath, maxLen, L"%s\\GTAVC-UpdateManager\\updater.ini", localAppData);
        return;
    }
    swprintf_s(outIniPath, maxLen, L"%s\\updater.ini", gameDir);
}

static void ReadLocalVersionW(const wchar_t* gameDir, wchar_t* outVer, size_t maxLen) {
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

static BOOL HasShownUpdaterInfo(const wchar_t* iniPath) {
    return GetPrivateProfileIntW(L"Updater", L"InfoShown", 0, iniPath) == 1;
}

static void MarkUpdaterInfoShown(const wchar_t* iniPath) {
    WritePrivateProfileStringW(L"Updater", L"InfoShown", L"1", iniPath);
}

static int GetConfiguredHotkey(const wchar_t* iniPath) {
    wchar_t keyStr[32] = { 0 };
    GetPrivateProfileStringW(L"Updater", L"Hotkey", L"0", keyStr, 32, iniPath);

    if (_wcsicmp(keyStr, L"disabled") == 0 || _wcsicmp(keyStr, L"none") == 0 || _wcsicmp(keyStr, L"off") == 0) {
        return -1; // Disabled
    }
    if (keyStr[0] >= L'0' && keyStr[0] <= L'9' && keyStr[1] == L'\0') {
        return '0' + (keyStr[0] - L'0');
    }
    if ((keyStr[0] == L'f' || keyStr[0] == L'F') && keyStr[1] >= L'1' && keyStr[1] <= L'9') {
        int fNum = _wtoi(keyStr + 1);
        if (fNum >= 1 && fNum <= 12) {
            return VK_F1 + (fNum - 1);
        }
    }
    return '0';
}

static int CompareSemanticVersions(const wchar_t* v1, const wchar_t* v2) {
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;

    const wchar_t* p1 = v1;
    if (*p1 == L'v' || *p1 == L'V') p1++;
    const wchar_t* p2 = v2;
    if (*p2 == L'v' || *p2 == L'V') p2++;

    swscanf_s(p1, L"%d.%d.%d", &a1, &a2, &a3);
    swscanf_s(p2, L"%d.%d.%d", &b1, &b2, &b3);

    if (a1 > b1) return 1;
    if (a1 < b1) return -1;
    if (a2 > b2) return 1;
    if (a2 < b2) return -1;
    if (a3 > b3) return 1;
    if (a3 < b3) return -1;
    return 0;
}

static void SafeTruncateUtf8(std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return;

    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) {
        cut--;
    }

    if (cut < maxBytes) {
        unsigned char lead = (unsigned char)s[cut];
        size_t charLen = 1;
        if ((lead & 0xE0) == 0xC0) charLen = 2;
        else if ((lead & 0xF0) == 0xE0) charLen = 3;
        else if ((lead & 0xF8) == 0xF0) charLen = 4;

        if (cut + charLen > maxBytes) {
            // Incomplete character does not fit: drop it
        } else {
            cut += charLen;
        }
    }

    s.resize(cut);
    s.append("...");
}

static bool ExtractJsonString(const std::string& json, size_t startPos, size_t endPos, const char* key, std::string& outVal) {
    outVal.clear();
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern, startPos);
    if (p == std::string::npos || (endPos != std::string::npos && p >= endPos)) return false;

    p = json.find(':', p + pattern.size());
    if (p == std::string::npos || (endPos != std::string::npos && p >= endPos)) return false;
    p++;

    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
    if (p >= json.size() || json[p] != '\"') return false;
    p++;

    while (p < json.size() && (endPos == std::string::npos || p < endPos)) {
        if (json[p] == '\\') {
            p++;
            if (p >= json.size()) break;
            if (json[p] == '\"') outVal.push_back('\"');
            else if (json[p] == 'n') { outVal.push_back('\r'); outVal.push_back('\n'); }
            else if (json[p] == 'r') { /* ignore */ }
            else if (json[p] == 't') outVal.push_back('\t');
            else outVal.push_back(json[p]);
            p++;
        } else if (json[p] == '\"') {
            break;
        } else {
            outVal.push_back(json[p]);
            p++;
        }
    }
    return true;
}

static bool FetchUrlDataDynamic(HINTERNET hInternet, const wchar_t* url, std::string& outData, DWORD& outHttpStatus, wchar_t* outErrorMsg, size_t errLen) {
    outData.clear();
    outHttpStatus = 0;

    HINTERNET hUrl = InternetOpenUrlW(hInternet, url, NULL, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        DWORD err = GetLastError();
        if (outErrorMsg) {
            if (err == ERROR_INTERNET_TIMEOUT) {
                swprintf_s(outErrorMsg, errLen, L"Сервер не отвечает (таймаут соединения).");
            } else {
                swprintf_s(outErrorMsg, errLen, L"Нет интернета для обновления.");
            }
        }
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusLen, NULL)) {
        outHttpStatus = statusCode;
    }

    if (outHttpStatus != 200) {
        InternetCloseHandle(hUrl);
        if (outErrorMsg) {
            if (outHttpStatus == 404) {
                swprintf_s(outErrorMsg, errLen, L"Релиз не найден на сервере (HTTP 404).");
            } else if (outHttpStatus == 403 || outHttpStatus == 429) {
                swprintf_s(outErrorMsg, errLen, L"Превышен лимит запросов к GitHub (rate limit). Подождите 1 час.");
            } else {
                swprintf_s(outErrorMsg, errLen, L"Ошибка сервера (HTTP %lu).", outHttpStatus);
            }
        }
        return false;
    }

    std::vector<char> buffer(65536);
    DWORD bytesRead = 0;
    size_t totalBytes = 0;
    const size_t MAX_JSON_BYTES = 4 * 1024 * 1024; // 4 MB

    while (InternetReadFile(hUrl, buffer.data(), (DWORD)buffer.size(), &bytesRead) && bytesRead > 0) {
        totalBytes += bytesRead;
        if (totalBytes > MAX_JSON_BYTES) break;
        outData.append(buffer.data(), bytesRead);
    }

    InternetCloseHandle(hUrl);
    return !outData.empty();
}

static int ParseReleasesFromJson(const std::string& json, ReleaseItem* items, int maxItems) {
    int count = 0;
    size_t p = 0;

    while (count < maxItems) {
        size_t tagPos = json.find("\"tag_name\"", p);
        if (tagPos == std::string::npos) break;

        size_t nextRelease = json.find("{\"", tagPos + 10);
        size_t blockEnd = (nextRelease != std::string::npos) ? nextRelease : json.size();

        ReleaseItem item;
        memset(&item, 0, sizeof(item));

        std::string rawTag, rawName, rawDate, rawBody;
        if (ExtractJsonString(json, tagPos, blockEnd, "tag_name", rawTag)) {
            MultiByteToWideChar(CP_UTF8, 0, rawTag.c_str(), -1, item.tag, 64);
            const wchar_t* verPart = item.tag;
            if (*verPart == L'v' || *verPart == L'V') verPart++;
            wcscpy_s(item.version, verPart);
        }

        if (ExtractJsonString(json, tagPos, blockEnd, "name", rawName)) {
            MultiByteToWideChar(CP_UTF8, 0, rawName.c_str(), -1, item.name, 128);
        } else {
            wcscpy_s(item.name, item.tag);
        }

        if (ExtractJsonString(json, tagPos, blockEnd, "published_at", rawDate)) {
            if (rawDate.size() >= 10) rawDate.resize(10);
            MultiByteToWideChar(CP_UTF8, 0, rawDate.c_str(), -1, item.date, 32);
        }

        if (ExtractJsonString(json, tagPos, blockEnd, "body", rawBody)) {
            SafeTruncateUtf8(rawBody, 2048);
            item.body = rawBody;
        }

        // Find browser_download_url for update.zip in assets
        size_t assetPos = json.find("\"browser_download_url\"", tagPos);
        while (assetPos != std::string::npos && assetPos < blockEnd) {
            std::string url;
            if (ExtractJsonString(json, assetPos, blockEnd, "browser_download_url", url)) {
                if (url.find("update.zip") != std::string::npos && url.find(".sha256") == std::string::npos) {
                    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, item.downloadUrl, 1024);
                    break;
                }
            }
            assetPos = json.find("\"browser_download_url\"", assetPos + 22);
        }

        // Fallback default url if not explicitly listed in assets
        if (item.downloadUrl[0] == L'\0' && item.tag[0] != L'\0') {
            swprintf_s(item.downloadUrl, L"https://github.com/itisislom/gtavc-updates/releases/download/%ls/update.zip", item.tag);
        }

        if (item.version[0] != L'\0' && ValidateSemVer(item.version)) {
            items[count++] = item;
        }

        p = blockEnd;
    }
    return count;
}

static void AppendEscapedArg(const std::wstring& arg, std::wstring& cmdLine) {
    if (!cmdLine.empty()) cmdLine.push_back(L' ');
    cmdLine.push_back(L'\"');
    for (size_t i = 0; i < arg.size(); i++) {
        wchar_t c = arg[i];
        if (c == L'\"') {
            cmdLine.append(L"\\\"");
        } else if (c == L'\\') {
            size_t count = 1;
            while (i + 1 < arg.size() && arg[i + 1] == L'\\') {
                count++;
                i++;
            }
            if (i + 1 == arg.size()) {
                cmdLine.append(count * 2, L'\\');
            } else if (arg[i + 1] == L'\"') {
                cmdLine.append(count * 2 + 1, L'\\');
            } else {
                cmdLine.append(count, L'\\');
            }
        } else {
            cmdLine.push_back(c);
        }
    }
    cmdLine.push_back(L'\"');
}

static void UpdateSelectionView(ManagerContext* ctx) {
    if (ctx->selectedIndex < 0 || ctx->selectedIndex >= ctx->releaseCount) return;

    ReleaseItem* item = &ctx->releases[ctx->selectedIndex];

    wchar_t descBuf[3000];
    wchar_t wBody[2500] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, item->body.c_str(), -1, wBody, 2500);

    swprintf_s(descBuf,
               L"Version: %ls\r\n"
               L"Release Date: %ls\r\n"
               L"Title: %ls\r\n\r\n"
               L"Notes:\r\n%ls",
               item->tag,
               item->date[0] ? item->date : L"Recent",
               item->name,
               wBody[0] ? wBody : L"No description provided for this release.");

    SetWindowTextW(ctx->hDesc, descBuf);

    int cmp = CompareSemanticVersions(item->version, ctx->localVer);
    wchar_t btnText[64];
    if (cmp > 0) {
        swprintf_s(btnText, L"Установить %ls", item->tag);
    } else if (cmp < 0) {
        swprintf_s(btnText, L"Откатиться на %ls", item->tag);
    } else {
        swprintf_s(btnText, L"Переустановить %ls", item->tag);
    }
    SetWindowTextW(ctx->hBtnInstall, btnText);
    EnableWindow(ctx->hBtnInstall, TRUE);
}

static LRESULT CALLBACK ManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ManagerContext* ctx = (ManagerContext*)GetWindowLongPtr(hWnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            ctx = (ManagerContext*)cs->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)ctx);

            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            wchar_t headerBuf[256];
            swprintf_s(headerBuf,
                       L"Текущая установленная: v%ls    |    Последняя на GitHub: %ls",
                       ctx->localVer,
                       ctx->releaseCount > 0 ? ctx->releases[0].tag : L"Неизвестно");

            ctx->hHeader = CreateWindowExW(0, L"STATIC", headerBuf,
                                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                                           20, 15, 520, 20, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(ctx->hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hLblList = CreateWindowExW(0, L"STATIC", L"Доступные версии (последние 5):",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            20, 42, 210, 18, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hLblList, WM_SETFONT, (WPARAM)hFont, TRUE);

            ctx->hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
                                         20, 62, 210, 270, hWnd, (HMENU)IDC_LIST_RELEASES, cs->hInstance, NULL);
            SendMessage(ctx->hList, WM_SETFONT, (WPARAM)hFont, TRUE);

            for (int i = 0; i < ctx->releaseCount; i++) {
                int cmp = CompareSemanticVersions(ctx->releases[i].version, ctx->localVer);
                wchar_t lineBuf[128];
                if (cmp > 0) {
                    swprintf_s(lineBuf, L"%ls  [Новая]", ctx->releases[i].tag);
                } else if (cmp == 0) {
                    swprintf_s(lineBuf, L"%ls  [Текущая]", ctx->releases[i].tag);
                } else {
                    swprintf_s(lineBuf, L"%ls  [Откат]", ctx->releases[i].tag);
                }
                SendMessageW(ctx->hList, LB_ADDSTRING, 0, (LPARAM)lineBuf);
            }
            SendMessage(ctx->hList, LB_SETCURSEL, ctx->selectedIndex, 0);

            HWND hLblDesc = CreateWindowExW(0, L"STATIC", L"Описание и список изменений:",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                                            245, 42, 300, 18, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hLblDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

            ctx->hDesc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                         245, 62, 300, 270, hWnd, (HMENU)IDC_EDIT_DESC, cs->hInstance, NULL);
            SendMessage(ctx->hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

            ctx->hBtnInstall = CreateWindowExW(0, L"BUTTON", L"Установить",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                               245, 345, 160, 32, hWnd, (HMENU)IDC_BTN_INSTALL, cs->hInstance, NULL);
            SendMessage(ctx->hBtnInstall, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnPlay = CreateWindowExW(0, L"BUTTON", L"Играть / Закрыть",
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

                wchar_t confirmMsg[512];
                if (cmp > 0) {
                    swprintf_s(confirmMsg,
                               L"Установить новую версию %ls?\n\n"
                               L"Игра будет закрыта, несохранённый прогресс будет потерян.", item->tag);
                } else if (cmp < 0) {
                    swprintf_s(confirmMsg,
                               L"Откатиться на предыдущую версию %ls?\n\n"
                               L"Новые файлы будут удалены, а сохранённые бэкапы восстановлены.\n\n"
                               L"Игра будет закрыта, несохранённый прогресс будет потерян.", item->tag);
                } else {
                    swprintf_s(confirmMsg,
                               L"Переустановить версию %ls?\n\n"
                               L"Игра будет закрыта, несохранённый прогресс будет потерян.", item->tag);
                }

                int res = MessageBoxW(hWnd, confirmMsg, L"Подтверждение обновления",
                                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
                if (res == IDYES) {
                    wchar_t updaterPath[MAX_PATH];
                    swprintf_s(updaterPath, L"%s\\updater.exe", ctx->gameDir);

                    // Validate URL and version before executing updater
                    if (!ValidateUrl(item->downloadUrl) || !ValidateSemVer(item->version)) {
                        MessageBoxW(hWnd, L"Ошибка безопасности: некорректный URL или формат версии релиза.",
                                    L"Ошибка", MB_ICONERROR | MB_OK);
                        return 0;
                    }

                    // Build escaped command line
                    std::wstring cmdLine;
                    AppendEscapedArg(updaterPath, cmdLine);
                    AppendEscapedArg(item->downloadUrl, cmdLine);
                    AppendEscapedArg(item->version, cmdLine);
                    AppendEscapedArg(std::to_wstring(GetCurrentProcessId()), cmdLine);
                    AppendEscapedArg(L"--no-restart", cmdLine);

                    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
                    cmdLineBuf.push_back(L'\0');

                    STARTUPINFOW si = { sizeof(si) };
                    PROCESS_INFORMATION pi;
                    if (CreateProcessW(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, ctx->gameDir, &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    }

                    // Graceful game exit instead of ExitProcess(0)
                    DestroyWindow(hWnd);

                    HWND hGame = ctx->hParent;
                    if (hGame && IsWindow(hGame)) {
                        PostMessage(hGame, WM_CLOSE, 0, 0);

                        ULONGLONG startTick = GetTickCount64();
                        BOOL closed = FALSE;
                        while (GetTickCount64() - startTick < 3000) {
                            if (!IsWindow(hGame)) {
                                closed = TRUE;
                                break;
                            }
                            Sleep(100);
                        }

                        if (!closed && IsWindow(hGame)) {
                            int ans = MessageBoxW(NULL,
                                L"Пожалуйста, сохраните игру и закройте её для продолжения обновления.\n\n"
                                L"Нажмите ОК для выхода из игры прямо сейчас.",
                                L"GTA: Vice City - Закрытие игры",
                                MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
                            if (ans == IDOK) {
                                ExitProcess(0);
                            }
                        }
                    } else {
                        ExitProcess(0);
                    }
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
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowUpdateManagerDialog(ManagerContext* ctx) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ManagerWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"GTAVC_UpdateManagerDialog";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

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

    HWND hDlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
                                L"GTAVC_UpdateManagerDialog", L"GTA: Vice City - Менеджер обновлений",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                posX, posY, winW, winH,
                                hParent, NULL, wc.hInstance, ctx);

    SetForegroundWindow(hDlg);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
}

static LRESULT CALLBACK InfoDialogWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            const wchar_t* infoText =
                L"GTA: Vice City - Система обновлений\n"
                L"Разработчик: Ислом Садриддинов\n\n"
                L"- Нажмите '0' во время игры в любой момент для открытия Менеджера обновлений.\n"
                L"- У вас есть полная свобода выбора: установка обновлений, переустановка или откат.\n"
                L"- Все обновления загружаются безопасно из официального репозитория GitHub.";

            HWND hText = CreateWindowExW(0, L"STATIC", infoText,
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         30, 20, 390, 105, hWnd, NULL, cs->hInstance, NULL);
            SendMessage(hText, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBtnOk = CreateWindowExW(0, L"BUTTON", L"OK",
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
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void ShowUpdaterInfoDialog(HWND hParent) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = InfoDialogWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"GTAVC_UpdaterInfoDialog";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

    int winW = 450;
    int winH = 220;
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (scrW - winW) / 2;
    int posY = (scrH - winH) / 2;

    HWND hDlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
                                L"GTAVC_UpdaterInfoDialog", L"GTA: Vice City - Обновления",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                posX, posY, winW, winH,
                                hParent, NULL, wc.hInstance, NULL);

    SetForegroundWindow(hDlg);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
        SetFocus(hParent);
    }
}

static int FetchReleaseList(ReleaseItem* items, int maxItems, wchar_t* outErrorMsg, size_t errLen) {
    HINTERNET hInternet = InternetOpenW(L"GTAVC-UpdateChecker", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) {
        if (outErrorMsg) swprintf_s(outErrorMsg, errLen, L"Нет интернета для обновления.");
        return 0;
    }

    std::string jsonBuf;
    DWORD httpStatus = 0;
    int count = 0;

    // 1. Query GitHub API for 5 releases
    if (FetchUrlDataDynamic(hInternet, GITHUB_RELEASES_API, jsonBuf, httpStatus, outErrorMsg, errLen)) {
        count = ParseReleasesFromJson(jsonBuf, items, maxItems);
    }

    // 2. Fallback to raw manifest if API fails
    if (count == 0) {
        if (FetchUrlDataDynamic(hInternet, GITHUB_RAW_MANIFEST, jsonBuf, httpStatus, nullptr, 0)) {
            ReleaseItem single;
            memset(&single, 0, sizeof(single));
            std::string verStr, dlStr;
            if (ExtractJsonString(jsonBuf, 0, std::string::npos, "version", verStr)) {
                MultiByteToWideChar(CP_UTF8, 0, verStr.c_str(), -1, single.version, 64);
                swprintf_s(single.tag, L"v%ls", single.version);
            }
            if (ExtractJsonString(jsonBuf, 0, std::string::npos, "download_url", dlStr)) {
                MultiByteToWideChar(CP_UTF8, 0, dlStr.c_str(), -1, single.downloadUrl, 1024);
            }
            wcscpy_s(single.name, L"Релиз из version.json");
            single.body = "Последняя опубликованная версия из манифеста.";
            if (single.version[0] != L'\0' && ValidateSemVer(single.version)) {
                items[count++] = single;
            }
        }
    }

    InternetCloseHandle(hInternet);
    return count;
}

static void OpenUpdateManager(HWND hGameWnd, BOOL showAlways) {
    wchar_t gameDir[MAX_PATH];
    GetGameDirW(gameDir, MAX_PATH);

    wchar_t iniPath[MAX_PATH];
    GetUpdaterIniPath(gameDir, iniPath, MAX_PATH);

    if (!showAlways && !HasShownUpdaterInfo(iniPath)) {
        ShowUpdaterInfoDialog(hGameWnd);
        MarkUpdaterInfoShown(iniPath);
    }

    std::unique_ptr<ManagerContext> ctx = std::make_unique<ManagerContext>();
    wcscpy_s(ctx->gameDir, gameDir);
    ctx->hParent = hGameWnd;
    ReadLocalVersionW(gameDir, ctx->localVer, 64);

    wchar_t netErrMsg[512] = { 0 };
    ctx->releaseCount = FetchReleaseList(ctx->releases, 10, netErrMsg, 512);

    if (ctx->releaseCount == 0) {
        if (showAlways) {
            MessageBoxW(hGameWnd,
                        netErrMsg[0] ? netErrMsg : L"Нет интернета для обновления.",
                        L"GTA: Vice City - Обновление",
                        MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        }
        return;
    }

    // Auto-select first item
    ctx->selectedIndex = 0;
    ShowUpdateManagerDialog(ctx.get());
}

static HWND FindGameWindowByPid(DWORD targetPid) {
    struct SearchCtx {
        DWORD pid;
        HWND hWndResult;
    } sCtx = { targetPid, NULL };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        SearchCtx* c = (SearchCtx*)lParam;
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid == c->pid && IsWindowVisible(hwnd)) {
            wchar_t clsName[128];
            GetClassNameW(hwnd, clsName, 128);
            if (wcscmp(clsName, L"Grand Theft Auto") == 0 || wcscmp(clsName, L"GTAVC") == 0) {
                c->hWndResult = hwnd;
                return FALSE;
            }
            if (!c->hWndResult) c->hWndResult = hwnd;
        }
        return TRUE;
    }, (LPARAM)&sCtx);

    return sCtx.hWndResult;
}

static DWORD WINAPI CheckerThread(LPVOID lpParam) {
    (void)lpParam;
    Sleep(2000);

    DWORD myPid = GetCurrentProcessId();
    HWND hGameWnd = NULL;
    for (int i = 0; i < 30; i++) {
        hGameWnd = FindGameWindowByPid(myPid);
        if (hGameWnd) break;
        Sleep(500);
    }

    wchar_t gameDir[MAX_PATH];
    GetGameDirW(gameDir, MAX_PATH);

    wchar_t iniPath[MAX_PATH];
    GetUpdaterIniPath(gameDir, iniPath, MAX_PATH);

    // Initial silent check / startup info dialog
    OpenUpdateManager(hGameWnd, FALSE);

    // Cache configured hotkey once at thread start
    int targetVk = GetConfiguredHotkey(iniPath);

    ULONGLONG lastTriggerTick = 0;
    while (true) {
        Sleep(50);
        if (targetVk < 0) continue; // Hotkey=disabled: ignore hotkey polling

        bool keyPressed = (GetAsyncKeyState(targetVk) & 0x8000) != 0;
        if (targetVk == '0') {
            keyPressed = keyPressed || ((GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0);
        }

        if (keyPressed) {
            ULONGLONG now = GetTickCount64();
            if (now - lastTriggerTick >= 300) { // 300 ms debounce
                lastTriggerTick = now;
                HWND hFore = GetForegroundWindow();
                if (hFore == hGameWnd || (hGameWnd == NULL && hFore != NULL)) {
                    OpenUpdateManager(hGameWnd, TRUE);
                }
            }
        }
    }
}

static HANDLE g_hSingletonMutex = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        DWORD pid = GetCurrentProcessId();
        wchar_t mutexName[64];
        swprintf_s(mutexName, L"Local\\GTAVC_UpdateChecker_%lu", pid);

        g_hSingletonMutex = CreateMutexW(NULL, TRUE, mutexName);
        if (g_hSingletonMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_hSingletonMutex) {
                CloseHandle(g_hSingletonMutex);
                g_hSingletonMutex = NULL;
            }
            return TRUE;
        }

        CreateThread(NULL, 0, CheckerThread, NULL, 0, NULL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_hSingletonMutex) {
            CloseHandle(g_hSingletonMutex);
            g_hSingletonMutex = NULL;
        }
    }
    return TRUE;
}
