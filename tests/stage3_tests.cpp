#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include "../src/updater/updater_core.h"
#include "../src/common/paths.h"

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

int main() {
    printf("=== STAGE 3 TESTS: SAFE delete.txt PROCESSING ===\n");

    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(MAX_PATH, tempRoot);
    wchar_t sandbox[MAX_PATH];
    swprintf_s(sandbox, L"%sgtavc_sandbox_stage3", tempRoot);
    SafeDeleteDirectoryRecursive(sandbox);
    CreateDirectoryW(sandbox, nullptr);

    wchar_t stageDir[MAX_PATH];
    swprintf_s(stageDir, L"%sgtavc_sandbox_stage3_stage", tempRoot);
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);

    // Setup initial files in sandbox
    wchar_t fExe[MAX_PATH], fDinput[MAX_PATH], fUpdater[MAX_PATH], fCleoOld[MAX_PATH], fOutside[MAX_PATH];
    swprintf_s(fExe, L"%s\\gta-vc.exe", sandbox);
    swprintf_s(fDinput, L"%s\\dinput8.dll", sandbox);
    swprintf_s(fUpdater, L"%s\\updater.exe", sandbox);
    swprintf_s(fCleoOld, L"%s\\CLEO\\old_mod.cs", sandbox);
    swprintf_s(fOutside, L"%s\\outside_canary.txt", tempRoot); // canary file outside root

    WriteFileContent(fExe, "ORIGINAL_GTAVC_EXE");
    WriteFileContent(fDinput, "ORIGINAL_DINPUT8");
    WriteFileContent(fUpdater, "ORIGINAL_UPDATER_EXE");
    WriteFileContent(fCleoOld, "CLEO_OLD_MOD_DATA");
    WriteFileContent(fOutside, "CANARY_OUTSIDE_MUST_NOT_BE_DELETED");

    printf("[TEST 1] Testing malicious and traversal paths in delete.txt...\n");
    // Write delete.txt containing malicious entries + protected files + valid old_mod.cs
    wchar_t delFile[MAX_PATH];
    swprintf_s(delFile, L"%s\\delete.txt", stageDir);

    const char* deleteTxtContent =
        "# Comment line\n"
        "../outside_canary.txt\n"
        "..\\..\\outside_canary.txt\n"
        "/absolute/path/file.txt\n"
        "C:\\Windows\\system32\\cmd.exe\n"
        "CON\n"
        "NUL\n"
        "gta-vc.exe\n"
        "dinput8.dll\n"
        "updater.exe\n"
        "cleo\n"
        "CLEO/old_mod.cs\n";

    WriteFileContent(delFile, deleteTxtContent);

    UpdateTransactionResult res;
    bool ok = ProcessDeletions(sandbox, stageDir, L"1.0.0", &res);
    if (!ok) {
        printf("FAIL: ProcessDeletions returned false.\n");
        return 1;
    }

    // 1. Canary outside must be untouched
    if (ReadFileContent(fOutside) != "CANARY_OUTSIDE_MUST_NOT_BE_DELETED") {
        printf("FAIL: File outside sandbox was touched or deleted!\n");
        return 1;
    }

    // 2. Protected files must be untouched
    if (ReadFileContent(fExe) != "ORIGINAL_GTAVC_EXE") {
        printf("FAIL: gta-vc.exe was deleted!\n");
        return 1;
    }
    if (ReadFileContent(fDinput) != "ORIGINAL_DINPUT8") {
        printf("FAIL: dinput8.dll was deleted!\n");
        return 1;
    }
    if (ReadFileContent(fUpdater) != "ORIGINAL_UPDATER_EXE") {
        printf("FAIL: updater.exe was deleted!\n");
        return 1;
    }

    // 3. Directory "cleo" must NOT be deleted
    wchar_t cleoDir[MAX_PATH];
    swprintf_s(cleoDir, L"%s\\CLEO", sandbox);
    DWORD dirAttrs = GetFileAttributesW(cleoDir);
    if (dirAttrs == INVALID_FILE_ATTRIBUTES || !(dirAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("FAIL: Entire CLEO directory was deleted!\n");
        return 1;
    }

    // 4. Legitimate target CLEO/old_mod.cs must be deleted from game root
    if (GetFileAttributesW(fCleoOld) != INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: CLEO/old_mod.cs was not deleted!\n");
        return 1;
    }

    // 5. Check that CLEO/old_mod.cs was safely backed up in .updaterbackups\1.0.0\deleted
    wchar_t bOldMod[MAX_PATH];
    swprintf_s(bOldMod, L"%s\\.updaterbackups\\1.0.0\\deleted\\CLEO\\old_mod.cs", sandbox);
    if (ReadFileContent(bOldMod) != "CLEO_OLD_MOD_DATA") {
        printf("FAIL: CLEO/old_mod.cs was not saved into backup! Path: %ls\n", bOldMod);
        return 1;
    }
    printf("PASS: Traversal rejected, protected files preserved, target deleted and backed up.\n");

    printf("[TEST 2] Testing rollback restoration from deleted backup...\n");
    ok = RestoreDeletedBackups(sandbox, L"1.0.0");
    if (!ok) {
        printf("FAIL: RestoreDeletedBackups returned false.\n");
        return 1;
    }

    if (ReadFileContent(fCleoOld) != "CLEO_OLD_MOD_DATA") {
        printf("FAIL: Rollback did not restore CLEO/old_mod.cs.\n");
        return 1;
    }
    printf("PASS: Rollback successfully restored deleted file from backup.\n");

    // Clean up
    SafeDeleteDirectoryRecursive(sandbox);
    SafeDeleteDirectoryRecursive(stageDir);
    DeleteFileW(fOutside);

    printf("\nALL STAGE 3 TESTS PASSED (2/2).\n");
    return 0;
}
