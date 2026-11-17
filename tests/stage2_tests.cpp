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
    printf("=== STAGE 2 TESTS: TRANSACTIONAL UPDATE & ROLLBACK ===\n");

    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(MAX_PATH, tempRoot);
    wchar_t sandbox[MAX_PATH];
    swprintf_s(sandbox, L"%sgtavc_sandbox_stage2", tempRoot);
    SafeDeleteDirectoryRecursive(sandbox);
    CreateDirectoryW(sandbox, nullptr);

    wchar_t stageDir[MAX_PATH];
    swprintf_s(stageDir, L"%sgtavc_sandbox_stage2_stage", tempRoot);
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);

    // Setup initial fake game files
    wchar_t fExe[MAX_PATH], fVer[MAX_PATH], fCleo[MAX_PATH], fData[MAX_PATH];
    swprintf_s(fExe, L"%s\\gta-vc.exe", sandbox);
    swprintf_s(fVer, L"%s\\version.txt", sandbox);
    swprintf_s(fCleo, L"%s\\CLEO\\mod.cs", sandbox);
    swprintf_s(fData, L"%s\\data\\config.dat", sandbox);

    WriteFileContent(fExe, "MZ_FAKE_EXE_BINARY_CONTENT");
    WriteFileContent(fVer, "1.0.0\n");
    WriteFileContent(fCleo, "CLEO_ORIGINAL_BYTECODE_V1");
    WriteFileContent(fData, "DATA_ORIGINAL_LINE");

    printf("[TEST 1] Testing successful ApplyUpdate...\n");
    // Prepare stage files
    wchar_t sCleo[MAX_PATH], sNew[MAX_PATH];
    swprintf_s(sCleo, L"%s\\CLEO\\mod.cs", stageDir);
    swprintf_s(sNew, L"%s\\models\\new_car.dff", stageDir);
    WriteFileContent(sCleo, "CLEO_UPDATED_BYTECODE_V2");
    WriteFileContent(sNew, "DFF_NEW_MODEL_CONTENT");

    UpdateTransactionResult res;
    bool ok = ApplyUpdate(sandbox, stageDir, L"1.0.0", L"1.1.0", -1, &res);
    if (!ok || res.filesCopied != 2) {
        printf("FAIL: ApplyUpdate failed. Error: %ls\n", res.errorMessage);
        return 1;
    }

    if (ReadFileContent(fCleo) != "CLEO_UPDATED_BYTECODE_V2") {
        printf("FAIL: CLEO/mod.cs not updated.\n");
        return 1;
    }
    wchar_t fNew[MAX_PATH];
    swprintf_s(fNew, L"%s\\models\\new_car.dff", sandbox);
    if (ReadFileContent(fNew) != "DFF_NEW_MODEL_CONTENT") {
        printf("FAIL: models/new_car.dff not created.\n");
        return 1;
    }

    // Check backup was created
    wchar_t bCleo[MAX_PATH];
    swprintf_s(bCleo, L"%s\\.updaterbackups\\1.0.0\\files\\CLEO\\mod.cs", sandbox);
    if (ReadFileContent(bCleo) != "CLEO_ORIGINAL_BYTECODE_V1") {
        printf("FAIL: Backup file not preserved. Content: %s\n", ReadFileContent(bCleo).c_str());
        return 1;
    }
    printf("PASS: Successful ApplyUpdate verified.\n");

    printf("[TEST 2] Testing forced failure at file #1 with automatic rollback (--fail-at 1)...\n");
    // Prepare a stage with 3 files
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);
    wchar_t s1[MAX_PATH], s2[MAX_PATH], s3[MAX_PATH];
    swprintf_s(s1, L"%s\\CLEO\\mod.cs", stageDir);
    swprintf_s(s2, L"%s\\data\\config.dat", stageDir);
    swprintf_s(s3, L"%s\\models\\another.dff", stageDir);
    WriteFileContent(s1, "CLEO_V3_FAILED");
    WriteFileContent(s2, "DATA_V3_FAILED");
    WriteFileContent(s3, "MODEL_V3_FAILED");

    std::string preRollbackCleo = ReadFileContent(fCleo);
    std::string preRollbackData = ReadFileContent(fData);

    ok = ApplyUpdate(sandbox, stageDir, L"1.1.0", L"1.2.0", 1, &res);
    if (ok) {
        printf("FAIL: ApplyUpdate should have failed at failAtFileIndex 1.\n");
        return 1;
    }
    if (res.filesRolledBack == 0) {
        printf("FAIL: filesRolledBack must be > 0.\n");
        return 1;
    }

    if (ReadFileContent(fCleo) != preRollbackCleo) {
        printf("FAIL: CLEO/mod.cs was not rolled back to pre-update state.\n");
        return 1;
    }
    if (ReadFileContent(fData) != preRollbackData) {
        printf("FAIL: data/config.dat was not rolled back to pre-update state.\n");
        return 1;
    }
    wchar_t fAnother[MAX_PATH];
    swprintf_s(fAnother, L"%s\\models\\another.dff", sandbox);
    if (GetFileAttributesW(fAnother) != INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: models/another.dff should not exist.\n");
        return 1;
    }
    printf("PASS: Automatic rollback restored 100%% byte identity.\n");

    printf("[TEST 3] Testing Crash Recovery from pending.journal...\n");
    // Manually create pending.journal and corrupt a file
    wchar_t jDir[MAX_PATH], jPath[MAX_PATH], bDir[MAX_PATH], bFile[MAX_PATH];
    swprintf_s(jDir, L"%s\\.updaterbackups", sandbox);
    swprintf_s(jPath, L"%s\\pending.journal", jDir);
    swprintf_s(bDir, L"%s\\recovery_test", jDir);
    swprintf_s(bFile, L"%s\\files\\CLEO\\mod.cs", bDir);

    WriteFileContent(bFile, "CLEO_RESTORED_BY_CRASH_RECOVERY");
    WriteFileContent(fCleo, "CORRUPTED_INCOMPLETE_COPY");

    FILE* jf = nullptr;
    _wfopen_s(&jf, jPath, L"w, ccs=UTF-8");
    if (!jf) {
        printf("FAIL: Could not open mock journal for writing.\n");
        return 1;
    }
    fwprintf(jf, L"[JOURNAL_V1]\n");
    fwprintf(jf, L"STATUS=APPLYING\n");
    fwprintf(jf, L"FROM_VERSION=1.0.0\n");
    fwprintf(jf, L"BACKUP_DIR=%ls\n", bDir);
    fwprintf(jf, L"COPIED=CLEO\\mod.cs\n");
    fclose(jf);

    if (!HasPendingJournal(sandbox)) {
        printf("FAIL: HasPendingJournal returned false.\n");
        return 1;
    }

    ok = RecoverPendingTransaction(sandbox, &res);
    if (!ok) {
        printf("FAIL: RecoverPendingTransaction failed.\n");
        return 1;
    }

    if (ReadFileContent(fCleo) != "CLEO_RESTORED_BY_CRASH_RECOVERY") {
        printf("FAIL: Crash recovery did not restore original content.\n");
        return 1;
    }
    if (HasPendingJournal(sandbox)) {
        printf("FAIL: pending.journal should be deleted after successful recovery.\n");
        return 1;
    }
    printf("PASS: Crash recovery from pending.journal verified.\n");

    // Clean up sandbox
    SafeDeleteDirectoryRecursive(sandbox);
    SafeDeleteDirectoryRecursive(stageDir);

    printf("\nALL STAGE 2 TESTS PASSED (3/3).\n");
    return 0;
}
