#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cassert>
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
    printf("=== STAGE 3 / ITEM 2.3 TESTS: SAFE delete.txt PROCESSING, 8.3 CANONICALIZATION & LOGGING ===\n");

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
    wchar_t fExe[MAX_PATH], fDinput[MAX_PATH], fUpdater[MAX_PATH], fChecker[MAX_PATH], fCleoOld[MAX_PATH], fOutside[MAX_PATH];
    swprintf_s(fExe, L"%s\\gta-vc.exe", sandbox);
    swprintf_s(fDinput, L"%s\\dinput8.dll", sandbox);
    swprintf_s(fUpdater, L"%s\\updater.exe", sandbox);
    swprintf_s(fChecker, L"%s\\UpdateChecker.asi", sandbox);
    swprintf_s(fCleoOld, L"%s\\CLEO\\old_mod.cs", sandbox);
    swprintf_s(fOutside, L"%s\\outside_canary.txt", tempRoot);

    WriteFileContent(fExe, "ORIGINAL_GTAVC_EXE");
    WriteFileContent(fDinput, "ORIGINAL_DINPUT8");
    WriteFileContent(fUpdater, "ORIGINAL_UPDATER_EXE");
    WriteFileContent(fChecker, "ORIGINAL_UPDATECHECKER_ASI");
    WriteFileContent(fCleoOld, "CLEO_OLD_MOD_DATA");
    WriteFileContent(fOutside, "CANARY_OUTSIDE_MUST_NOT_BE_DELETED");

    // Also create a nested folder to test recursive directory deletion
    wchar_t fNestedFile[MAX_PATH];
    swprintf_s(fNestedFile, L"%s\\custom_mod\\sub\\data.bin", sandbox);
    WriteFileContent(fNestedFile, "CUSTOM_MOD_SUB_DATA");

    printf("[TEST 1] Testing malicious paths, protected files, 8.3 alias, and recursive folder deletion...\n");
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
        "GTA-VC.EXE\n"
        "dinput8.dll\n"
        "UPDATE~1.ASI\n"
        "cleo\n"
        "CLEO/old_mod.cs\n"
        "custom_mod\n";

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
        printf("FAIL: gta-vc.exe was deleted (case insensitivity failed)!\n");
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
    if (ReadFileContent(fChecker) != "ORIGINAL_UPDATECHECKER_ASI") {
        printf("FAIL: UpdateChecker.asi was deleted by 8.3 alias UPDATE~1.ASI!\n");
        return 1;
    }

    // 3. Protected system directory "cleo" must NOT be deleted
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

    // 5. Recursive directory deletion of custom_mod must remove directory and files
    wchar_t customModDir[MAX_PATH];
    swprintf_s(customModDir, L"%s\\custom_mod", sandbox);
    if (GetFileAttributesW(customModDir) != INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: custom_mod directory was not recursively deleted!\n");
        return 1;
    }

    // 6. Check that deleted files were safely backed up in .updaterbackups\1.0.0\deleted
    wchar_t bOldMod[MAX_PATH];
    swprintf_s(bOldMod, L"%s\\.updaterbackups\\1.0.0\\deleted\\CLEO\\old_mod.cs", sandbox);
    if (ReadFileContent(bOldMod) != "CLEO_OLD_MOD_DATA") {
        printf("FAIL: CLEO/old_mod.cs was not saved into backup! Path: %ls\n", bOldMod);
        return 1;
    }

    wchar_t bNested[MAX_PATH];
    swprintf_s(bNested, L"%s\\.updaterbackups\\1.0.0\\deleted\\custom_mod\\sub\\data.bin", sandbox);
    if (ReadFileContent(bNested) != "CUSTOM_MOD_SUB_DATA") {
        printf("FAIL: custom_mod/sub/data.bin was not saved into backup! Path: %ls\n", bNested);
        return 1;
    }
    printf("PASS: Traversal rejected, protected files & 8.3 aliases preserved, recursive folder deletion & backup verified.\n");

    printf("[TEST 2] Testing updater.log logging content...\n");
    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%s\\updater.log", sandbox);
    std::string logContent = ReadFileContent(logPath);
    if (logContent.empty()) {
        printf("FAIL: updater.log was not created or is empty!\n");
        return 1;
    }
    if (logContent.find("[DELETE]") == std::string::npos) {
        printf("FAIL: updater.log missing [DELETE] records! Content:\n%s\n", logContent.c_str());
        return 1;
    }
    printf("PASS: updater.log verified successfully.\n");

    printf("[TEST 3] Testing rollback restoration from deleted backup...\n");
    ok = RestoreDeletedBackups(sandbox, L"1.0.0");
    if (!ok) {
        printf("FAIL: RestoreDeletedBackups returned false.\n");
        return 1;
    }

    if (ReadFileContent(fCleoOld) != "CLEO_OLD_MOD_DATA") {
        printf("FAIL: Rollback did not restore CLEO/old_mod.cs.\n");
        return 1;
    }
    if (ReadFileContent(fNestedFile) != "CUSTOM_MOD_SUB_DATA") {
        printf("FAIL: Rollback did not restore custom_mod/sub/data.bin.\n");
        return 1;
    }
    printf("PASS: Rollback successfully restored all deleted files from backup.\n");

    // Clean up
    SafeDeleteDirectoryRecursive(sandbox);
    SafeDeleteDirectoryRecursive(stageDir);
    DeleteFileW(fOutside);

    printf("\nALL STAGE 3 / ITEM 2.3 TESTS PASSED (3/3).\n");
    return 0;
}
