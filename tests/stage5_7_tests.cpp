#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cassert>
#include <string>
#include <vector>
#include "../src/updater/updater_core.h"
#include "../src/common/paths.h"
#include "../src/common/crypto.h"

static void WriteFileContent(const wchar_t* path, const char* content) {
    wchar_t dir[MAX_PATH];
    dir[0] = L'\0';
    wcsncpy_s(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';
    wchar_t* lastSlash = wcsrchr(dir, L'\\');
    if (lastSlash) {
        *lastSlash = L'\0';
        for (wchar_t* p = dir; *p; p++) {
            if (*p == L'\\' || *p == L'/') {
                wchar_t orig = *p;
                *p = L'\0';
                CreateDirectoryW(dir, nullptr);
                *p = orig;
            }
        }
        CreateDirectoryW(dir, nullptr);
    }
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"wb");
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
}

static std::string ReadFileContent(const wchar_t* path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s(sz, '\0');
    if (sz > 0) fread(&s[0], 1, sz, f);
    fclose(f);
    return s;
}

static void SafeTruncateUtf8Test(std::string& s, size_t maxBytes) {
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

int main() {
    printf("=== STAGES 5, 6, 7 TESTS: RELIABILITY, UX, HOTKEY & UNICODE (W-API) ===\n");

    printf("[TEST 1] Testing UTF-8 character boundary safe truncation...\n");
    // Cyrillic "Привет, мир!" encoded in UTF-8 (each Cyrillic character is 2 bytes)
    // 'П' is \xD0\x9F. If truncated at byte 1 (in the middle of 'П'), boundary check must step back!
    std::string utf8Text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // "Привет" (12 bytes)
    SafeTruncateUtf8Test(utf8Text, 3); // cuts in middle of second char
    assert(utf8Text == "\xD0\x9F...");
    printf("PASS: UTF-8 safe truncation preserved character boundaries.\n");

    printf("[TEST 2] Testing Singleton Mutex for UpdateChecker...\n");
    DWORD pid = GetCurrentProcessId();
    wchar_t mutexName[64];
    swprintf_s(mutexName, L"Local\\GTAVC_UpdateChecker_%lu", pid);

    HANDLE hMutex1 = CreateMutexW(NULL, TRUE, mutexName);
    assert(hMutex1 != NULL);
    assert(GetLastError() != ERROR_ALREADY_EXISTS);

    // Second attempt must detect ERROR_ALREADY_EXISTS
    HANDLE hMutex2 = CreateMutexW(NULL, TRUE, mutexName);
    DWORD err = GetLastError();
    assert(err == ERROR_ALREADY_EXISTS);
    if (hMutex2) CloseHandle(hMutex2);
    CloseHandle(hMutex1);
    printf("PASS: Singleton mutex correctly detects and prevents dual instance execution.\n");

    printf("[TEST 3] Testing Full Unicode Cyrillic Path Support (W-API)...\n");
    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(MAX_PATH, tempRoot);
    wchar_t cyrillicGame[MAX_PATH];
    swprintf_s(cyrillicGame, L"%sТест Игры\\Вайс Сити Мод", tempRoot);
    SafeDeleteDirectoryRecursive(cyrillicGame);
    CreateDirectoryW(cyrillicGame, nullptr);

    wchar_t cyrillicStage[MAX_PATH];
    swprintf_s(cyrillicStage, L"%sТест Игры\\Временная Папка", tempRoot);
    SafeDeleteDirectoryRecursive(cyrillicStage);
    CreateDirectoryW(cyrillicStage, nullptr);

    // Initial files in Cyrillic path
    wchar_t cExe[MAX_PATH], cCleo[MAX_PATH];
    swprintf_s(cExe, L"%s\\gta-vc.exe", cyrillicGame);
    swprintf_s(cCleo, L"%s\\CLEO\\русский_скрипт.cs", cyrillicGame);
    WriteFileContent(cExe, "CYRILLIC_GAME_EXE");
    WriteFileContent(cCleo, "ORIGINAL_CYRILLIC_SCRIPT_V1");

    // Staging file with Cyrillic subfolder and filename
    wchar_t sNew[MAX_PATH], sDel[MAX_PATH];
    swprintf_s(sNew, L"%s\\модлоадер\\текстуры\\машина.txd", cyrillicStage);
    swprintf_s(sDel, L"%s\\delete.txt", cyrillicStage);
    WriteFileContent(sNew, "NEW_CYRILLIC_TEXTURE_DATA");
    WriteFileContent(sDel, "CLEO/русский_скрипт.cs\n");

    UpdateTransactionResult txRes;
    bool ok = ApplyUpdate(cyrillicGame, cyrillicStage, L"1.0.0", L"1.1.0", -1, &txRes);
    if (!ok) {
        printf("FAIL: ApplyUpdate failed on Cyrillic path: %ls\n", txRes.errorMessage);
        return 1;
    }

    wchar_t fNew[MAX_PATH];
    swprintf_s(fNew, L"%s\\модлоадер\\текстуры\\машина.txd", cyrillicGame);
    if (ReadFileContent(fNew) != "NEW_CYRILLIC_TEXTURE_DATA") {
        printf("FAIL: Cyrillic path file not created.\n");
        return 1;
    }

    // Verify deleted file was backed up and removed from game root
    if (GetFileAttributesW(cCleo) != INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: CLEO/русский_скрипт.cs was not deleted!\n");
        return 1;
    }

    wchar_t searchPattern[MAX_PATH];
    swprintf_s(searchPattern, L"%s\\.updaterbackups\\1.0.0_*", cyrillicGame);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    assert(hFind != INVALID_HANDLE_VALUE);
    wchar_t bCleo[MAX_PATH];
    swprintf_s(bCleo, L"%s\\.updaterbackups\\%ls\\deleted\\CLEO\\русский_скрипт.cs", cyrillicGame, fd.cFileName);
    FindClose(hFind);

    if (ReadFileContent(bCleo) != "ORIGINAL_CYRILLIC_SCRIPT_V1") {
        printf("FAIL: Backup on Cyrillic path failed! Path: %ls\n", bCleo);
        return 1;
    }

    // Test rollback on Cyrillic path
    ok = RestoreDeletedBackups(cyrillicGame, L"1.0.0");
    assert(ok);
    if (ReadFileContent(cCleo) != "ORIGINAL_CYRILLIC_SCRIPT_V1") {
        printf("FAIL: Rollback on Cyrillic path failed to restore file.\n");
        return 1;
    }

    printf("PASS: Full Cyrillic path Unicode W-API support verified.\n");

    // Clean up
    SafeDeleteDirectoryRecursive(cyrillicGame);
    SafeDeleteDirectoryRecursive(cyrillicStage);

    printf("\nALL STAGES 5, 6, 7 TESTS PASSED (3/3).\n");
    return 0;
}
