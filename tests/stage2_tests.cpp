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
    printf("=== STAGE 2 / ITEM 2.2 & 2.4 TESTS: TRANSACTIONAL UPDATE, PHASES & ROLLBACK ===\n");

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

    // Verify unique timestamped backup folder
    wchar_t searchPattern[MAX_PATH];
    swprintf_s(searchPattern, L"%s\\.updaterbackups\\1.0.0_*", sandbox);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        printf("FAIL: Timestamped backup folder 1.0.0_<ts> was not created.\n");
        return 1;
    }
    FindClose(hFind);
    printf("PASS: Successful ApplyUpdate and timestamped backup verified.\n");

    printf("[TEST 2] Testing forced failure at file #1 with automatic rollback (--fail-at 1)...\n");
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
    wchar_t jDir[MAX_PATH], jPath[MAX_PATH], bDir[MAX_PATH], bFile[MAX_PATH];
    swprintf_s(jDir, L"%s\\.updaterbackups", sandbox);
    swprintf_s(jPath, L"%s\\pending.journal", jDir);
    swprintf_s(bDir, L"%s\\recovery_test", jDir);
    swprintf_s(bFile, L"%s\\files\\CLEO\\mod.cs", bDir);

    WriteFileContent(bFile, "CLEO_RESTORED_BY_CRASH_RECOVERY");
    WriteFileContent(fCleo, "CORRUPTED_INCOMPLETE_COPY");

    FILE* jf = nullptr;
    _wfopen_s(&jf, jPath, L"w, ccs=UTF-8");
    assert(jf != nullptr);
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

    printf("[TEST 4] Testing recovery rejection on path traversal in journal...\n");
    _wfopen_s(&jf, jPath, L"w, ccs=UTF-8");
    assert(jf != nullptr);
    fwprintf(jf, L"[JOURNAL_V1]\n");
    fwprintf(jf, L"STATUS=APPLYING\n");
    fwprintf(jf, L"BACKUP_DIR=%ls\n", bDir);
    fwprintf(jf, L"COPIED=..\\outside_game.txt\n");
    fclose(jf);

    ok = RecoverPendingTransaction(sandbox, &res);
    if (ok) {
        printf("FAIL: RecoverPendingTransaction should reject path traversal in journal!\n");
        return 1;
    }
    DeleteFileW(jPath);
    printf("PASS: Malicious traversal in pending.journal safely rejected.\n");

    printf("[TEST 5] Testing Item 2.4: Rollback via installed_manifest.txt (downgrade test)...\n");
    // Scenario 2.4:
    // v1 installs a.txt and b.txt
    // v2 installs a.txt (v2) and c.txt (b.txt deleted as orphan)
    // Downgrade v2 -> v1: c.txt deleted, b.txt restored, a.txt restored to v1
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);
    wchar_t sa[MAX_PATH], sb[MAX_PATH], sc[MAX_PATH];
    swprintf_s(sa, L"%s\\data\\a.txt", stageDir);
    swprintf_s(sb, L"%s\\data\\b.txt", stageDir);
    WriteFileContent(sa, "A_CONTENT_V1");
    WriteFileContent(sb, "B_CONTENT_V1");

    ok = ApplyUpdate(sandbox, stageDir, L"1.0.0", L"1.1.0", -1, &res);
    assert(ok);

    wchar_t fa[MAX_PATH], fb[MAX_PATH], fc[MAX_PATH];
    swprintf_s(fa, L"%s\\data\\a.txt", sandbox);
    swprintf_s(fb, L"%s\\data\\b.txt", sandbox);
    swprintf_s(fc, L"%s\\data\\c.txt", sandbox);

    assert(ReadFileContent(fa) == "A_CONTENT_V1");
    assert(ReadFileContent(fb) == "B_CONTENT_V1");

    // Now update to v2 (has a.txt v2 and c.txt, but no b.txt)
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);
    swprintf_s(sa, L"%s\\data\\a.txt", stageDir);
    swprintf_s(sc, L"%s\\data\\c.txt", stageDir);
    WriteFileContent(sa, "A_CONTENT_V2");
    WriteFileContent(sc, "C_CONTENT_V2");

    ok = ApplyUpdate(sandbox, stageDir, L"1.1.0", L"1.2.0", -1, &res);
    assert(ok);

    assert(ReadFileContent(fa) == "A_CONTENT_V2");
    assert(ReadFileContent(fc) == "C_CONTENT_V2");
    assert(GetFileAttributesW(fb) == INVALID_FILE_ATTRIBUTES); // b.txt was cleanly removed as orphan!

    // Now downgrade to v1 (archive has a.txt v1 and b.txt v1)
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);
    swprintf_s(sa, L"%s\\data\\a.txt", stageDir);
    swprintf_s(sb, L"%s\\data\\b.txt", stageDir);
    WriteFileContent(sa, "A_CONTENT_V1");
    WriteFileContent(sb, "B_CONTENT_V1");

    ok = ApplyUpdate(sandbox, stageDir, L"1.2.0", L"1.1.0", -1, &res);
    assert(ok);

    assert(ReadFileContent(fa) == "A_CONTENT_V1");
    assert(ReadFileContent(fb) == "B_CONTENT_V1"); // b.txt restored!
    assert(GetFileAttributesW(fc) == INVALID_FILE_ATTRIBUTES); // c.txt orphan cleanly removed on downgrade!
    printf("PASS: Item 2.4 downgrade test with installed_manifest.txt orphan cleanup succeeded.\n");

    printf("[TEST 6] Testing Item 4(a): Failure on ATTEMPT file & recovery of unconfirmed ATTEMPT...\n");
    // (a) Crash recovery of an ATTEMPT record without COPIED
    wchar_t fAttempt[MAX_PATH], bAttempt[MAX_PATH];
    swprintf_s(fAttempt, L"%s\\data\\attempt.txt", sandbox);
    swprintf_s(bAttempt, L"%s\\files\\data\\attempt.txt", bDir);
    WriteFileContent(bAttempt, "ORIGINAL_ATTEMPT_FILE_BEFORE_UPDATE");
    WriteFileContent(fAttempt, "PARTIALLY_COPIED_OR_CORRUPT_NEW_DATA");

    _wfopen_s(&jf, jPath, L"w, ccs=UTF-8");
    assert(jf != nullptr);
    fwprintf(jf, L"[JOURNAL_V1]\n");
    fwprintf(jf, L"STATUS=APPLYING\n");
    fwprintf(jf, L"FROM_VERSION=1.0.0\n");
    fwprintf(jf, L"BACKUP_DIR=%ls\n", bDir);
    fwprintf(jf, L"ATTEMPT=data\\attempt.txt\n"); // ATTEMPT without COPIED!
    fclose(jf);

    ok = RecoverPendingTransaction(sandbox, &res);
    if (!ok) {
        printf("FAIL: RecoverPendingTransaction failed for ATTEMPT without COPIED.\n");
        return 1;
    }
    if (ReadFileContent(fAttempt) != "ORIGINAL_ATTEMPT_FILE_BEFORE_UPDATE") {
        printf("FAIL: ATTEMPT file was not restored from backup! Got: %s\n", ReadFileContent(fAttempt).c_str());
        return 1;
    }
    printf("PASS: Item 4(a) ATTEMPT file restoration verified.\n");

    printf("[TEST 7] Testing Item 4(b): Crash in FINALIZING & recovery of version.txt / manifest...\n");
    // Write pre-update manifest and version
    wchar_t fManifest[MAX_PATH];
    swprintf_s(fManifest, L"%s\\installed_manifest.txt", sandbox);
    WriteFileContent(fVer, "1.0.0\n");
    WriteFileContent(fManifest, "data\\a.txt\ndata\\b.txt\n");

    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);
    wchar_t sFinalFile[MAX_PATH];
    swprintf_s(sFinalFile, L"%s\\data\\c.txt", stageDir);
    WriteFileContent(sFinalFile, "C_FINALIZING_DATA");

    // failAtFileIndex = 9999 triggers crash right in FINALIZING
    ok = ApplyUpdate(sandbox, stageDir, L"1.0.0", L"2.0.0", 9999, &res);
    if (ok) {
        printf("FAIL: ApplyUpdate should have failed with testhook 9999 in FINALIZING.\n");
        return 1;
    }

    if (!HasPendingJournal(sandbox)) {
        printf("FAIL: pending.journal should exist after simulated FINALIZING crash.\n");
        return 1;
    }

    // Now run crash recovery
    ok = RecoverPendingTransaction(sandbox, &res);
    if (!ok) {
        printf("FAIL: RecoverPendingTransaction failed after FINALIZING crash.\n");
        return 1;
    }

    std::string recoveredVer = ReadFileContent(fVer);
    if (recoveredVer.find("1.0.0") == std::string::npos) {
        printf("FAIL: version.txt was not restored to 1.0.0! Content: %s\n", recoveredVer.c_str());
        return 1;
    }
    std::string recoveredManifest = ReadFileContent(fManifest);
    if (recoveredManifest.find("data\\a.txt") == std::string::npos || recoveredManifest.find("data\\b.txt") == std::string::npos) {
        printf("FAIL: installed_manifest.txt was not restored to original content! Content:\n%s\n", recoveredManifest.c_str());
        return 1;
    }
    printf("PASS: Item 4(b) crash in FINALIZING recovered version.txt and manifest cleanly.\n");

    // Clean up sandbox
    SafeDeleteDirectoryRecursive(sandbox);
    SafeDeleteDirectoryRecursive(stageDir);

    printf("\nALL STAGE 2 / ITEM 2.2 & 2.4 TESTS PASSED (7/7).\n");
    return 0;
}
