#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cassert>
#include <string>
#include <vector>
#include "../src/common/paths.h"
#include "../src/common/crypto.h"
#include "../src/updater/updater_core.h"

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

static DWORD RunUpdaterProcess(const wchar_t* updaterExe, const wchar_t* args, const wchar_t* workDir) {
    wchar_t cmd[MAX_PATH * 3];
    swprintf_s(cmd, L"\"%s\" %s", updaterExe, args);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };
    BOOL ok = CreateProcessW(updaterExe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, workDir, &si, &pi);
    if (!ok) {
        return (DWORD)-1;
    }

    WaitForSingleObject(pi.hProcess, 60000); // 1 minute max
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

int main() {
    printf("=== STAGE 7 / ITEM 2.7 TESTS: END-TO-END CLI SANDBOX TESTS (--local-zip) ===\n");

    // Enable test automation environment variable so updater doesn't block on modal dialogs
    SetEnvironmentVariableW(L"GTAVC_TEST_AUTOMATION", L"1");

    wchar_t tempRoot[MAX_PATH];
    GetTempPathW(MAX_PATH, tempRoot);

    wchar_t sandbox[MAX_PATH];
    swprintf_s(sandbox, L"%sgtavc_sandbox_e2e", tempRoot);
    SafeDeleteDirectoryRecursive(sandbox);
    CreateDirectoryW(sandbox, nullptr);

    wchar_t repoRoot[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, repoRoot);

    wchar_t builtUpdater[MAX_PATH];
    swprintf_s(builtUpdater, L"%s\\build\\updater.exe", repoRoot);
    if (GetFileAttributesW(builtUpdater) == INVALID_FILE_ATTRIBUTES) {
        printf("FAIL: build\\updater.exe not found. Run build.bat first!\n");
        return 1;
    }

    // Copy updater.exe into sandbox
    wchar_t sandboxUpdater[MAX_PATH];
    swprintf_s(sandboxUpdater, L"%s\\updater.exe", sandbox);
    CopyFileW(builtUpdater, sandboxUpdater, FALSE);

    // Initial game files
    wchar_t fExe[MAX_PATH], fVer[MAX_PATH];
    swprintf_s(fExe, L"%s\\gta-vc.exe", sandbox);
    swprintf_s(fVer, L"%s\\version.txt", sandbox);
    WriteFileContent(fExe, "MZ_FAKE_VC_EXE");
    WriteFileContent(fVer, "1.0.0\n");

    // Temp workspace for zip packaging
    wchar_t pkgDir[MAX_PATH];
    swprintf_s(pkgDir, L"%sgtavc_e2e_pkg", tempRoot);
    SafeDeleteDirectoryRecursive(pkgDir);
    CreateDirectoryW(pkgDir, nullptr);

    printf("[TEST 1] Testing Scenario 1: Positive end-to-end update with valid zip and sha256...\n");
    wchar_t stageDir[MAX_PATH];
    swprintf_s(stageDir, L"%s\\stage", pkgDir);
    CreateDirectoryW(stageDir, nullptr);

    wchar_t sCleo[MAX_PATH], sData[MAX_PATH];
    swprintf_s(sCleo, L"%s\\CLEO\\radar.cs", stageDir);
    swprintf_s(sData, L"%s\\data\\carcols.dat", stageDir);
    WriteFileContent(sCleo, "RADAR_CLEO_BYTECODE_V110");
    WriteFileContent(sData, "CARCOLS_NEW_DATA_V110");

    wchar_t validZip[MAX_PATH];
    swprintf_s(validZip, L"%s\\valid_v110.zip", pkgDir);

    wchar_t tarCmd[MAX_PATH * 3];
    swprintf_s(tarCmd, L"tar.exe -cf \"%s\" -C \"%s\" .", validZip, stageDir);
    _wsystem(tarCmd);

    // Compute sha256 and write valid_v110.zip.sha256
    wchar_t validSha[65];
    assert(ComputeFileSha256(validZip, validSha, 65));
    wchar_t shaFile[MAX_PATH];
    swprintf_s(shaFile, L"%s\\valid_v110.zip.sha256", pkgDir);
    FILE* sf = nullptr;
    _wfopen_s(&sf, shaFile, L"w");
    assert(sf != nullptr);
    fwprintf(sf, L"%ls\n", validSha);
    fclose(sf);

    wchar_t args[MAX_PATH * 2];
    swprintf_s(args, L"--local-zip \"%s\" --version 1.1.0 --no-restart --game-pid 0", validZip);
    DWORD code = RunUpdaterProcess(sandboxUpdater, args, sandbox);
    if (code != 0) {
        printf("FAIL: Updater returned code %lu on valid update.\n", code);
        return 1;
    }

    // Verify version.txt, manifest, and installed files
    std::string verRead = ReadFileContent(fVer);
    if (verRead.find("1.1.0") == std::string::npos) {
        printf("FAIL: version.txt not updated to 1.1.0! Content: %s\n", verRead.c_str());
        return 1;
    }
    wchar_t fCleoDst[MAX_PATH], fDataDst[MAX_PATH], fManifest[MAX_PATH];
    swprintf_s(fCleoDst, L"%s\\CLEO\\radar.cs", sandbox);
    swprintf_s(fDataDst, L"%s\\data\\carcols.dat", sandbox);
    swprintf_s(fManifest, L"%s\\installed_manifest.txt", sandbox);

    if (ReadFileContent(fCleoDst) != "RADAR_CLEO_BYTECODE_V110") {
        printf("FAIL: CLEO/radar.cs not installed.\n");
        return 1;
    }
    if (ReadFileContent(fDataDst) != "CARCOLS_NEW_DATA_V110") {
        printf("FAIL: data/carcols.dat not installed.\n");
        return 1;
    }
    std::string manifestContent = ReadFileContent(fManifest);
    if (manifestContent.find("CLEO\\radar.cs") == std::string::npos && manifestContent.find("CLEO/radar.cs") == std::string::npos) {
        printf("FAIL: installed_manifest.txt does not contain CLEO/radar.cs. Content:\n%s\n", manifestContent.c_str());
        return 1;
    }
    printf("PASS: Scenario 1 positive update, manifest, and version.txt verified.\n");

    printf("[TEST 2] Testing Scenario 2: Tampered SHA-256 (archive must not be applied)...\n");
    // Write corrupted sha256
    _wfopen_s(&sf, shaFile, L"w");
    assert(sf != nullptr);
    fwprintf(sf, L"0000000000000000000000000000000000000000000000000000000000000000\n");
    fclose(sf);

    swprintf_s(args, L"--local-zip \"%s\" --version 1.2.0 --no-restart --game-pid 0", validZip);
    code = RunUpdaterProcess(sandboxUpdater, args, sandbox);
    if (code == 0) {
        printf("FAIL: Updater should fail on SHA-256 mismatch!\n");
        return 1;
    }
    if (ReadFileContent(fVer).find("1.1.0") == std::string::npos) {
        printf("FAIL: version.txt was modified after SHA-256 mismatch!\n");
        return 1;
    }
    printf("PASS: Scenario 2 corrupted SHA-256 rejected, sandbox untouched.\n");

    printf("[TEST 3] Testing Scenario 3: Zip-slip / directory traversal in archive (tar -tf reject)...\n");
    // Create zip with zip-slip entry
    SafeDeleteDirectoryRecursive(stageDir);
    CreateDirectoryW(stageDir, nullptr);

    wchar_t slipZip[MAX_PATH];
    swprintf_s(slipZip, L"%s\\slip.zip", pkgDir);

    // Build a zip containing ..\outside.txt using python or tar
    // tar.exe -cf slip.zip outside.txt
    wchar_t slipFile[MAX_PATH];
    swprintf_s(slipFile, L"%s\\outside.txt", stageDir);
    WriteFileContent(slipFile, "SLIP_DATA");

    // Create zip via python to embed literal traversal header "..\\escape.txt"
    wchar_t pyCmd[MAX_PATH * 3];
    swprintf_s(pyCmd, L"python -c \"import zipfile; z = zipfile.ZipFile(r'%ls', 'w'); z.writestr('../escape.txt', b'MALICIOUS'); z.close()\"", slipZip);
    _wsystem(pyCmd);

    // Generate sha256 for slip.zip
    wchar_t slipSha[65];
    assert(ComputeFileSha256(slipZip, slipSha, 65));
    wchar_t slipShaFile[MAX_PATH];
    swprintf_s(slipShaFile, L"%s\\slip.zip.sha256", pkgDir);
    _wfopen_s(&sf, slipShaFile, L"w");
    assert(sf != nullptr);
    fwprintf(sf, L"%ls\n", slipSha);
    fclose(sf);

    swprintf_s(args, L"--local-zip \"%s\" --version 1.3.0 --no-restart --game-pid 0", slipZip);
    code = RunUpdaterProcess(sandboxUpdater, args, sandbox);
    if (code == 0) {
        printf("FAIL: Updater should reject zip-slip archive!\n");
        return 1;
    }
    wchar_t canaryOutside[MAX_PATH];
    swprintf_s(canaryOutside, L"%s\\..\\escape.txt", sandbox);
    if (GetFileAttributesW(canaryOutside) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(canaryOutside);
        printf("FAIL: Zip-slip escaped game root!\n");
        return 1;
    }
    printf("PASS: Scenario 3 zip-slip archive safely rejected by tar -tf pre-validation.\n");

    printf("[TEST 4] Testing Scenario 4: Archive targeting .updaterbackups\\pending.journal (rejected)...\n");
    wchar_t forbiddenZip[MAX_PATH];
    swprintf_s(forbiddenZip, L"%s\\forbidden.zip", pkgDir);

    swprintf_s(pyCmd, L"python -c \"import zipfile; z = zipfile.ZipFile(r'%ls', 'w'); z.writestr('.updaterbackups/pending.journal', b'FORBIDDEN'); z.close()\"", forbiddenZip);
    _wsystem(pyCmd);

    wchar_t forbSha[65];
    assert(ComputeFileSha256(forbiddenZip, forbSha, 65));
    wchar_t forbShaFile[MAX_PATH];
    swprintf_s(forbShaFile, L"%s\\forbidden.zip.sha256", pkgDir);
    _wfopen_s(&sf, forbShaFile, L"w");
    assert(sf != nullptr);
    fwprintf(sf, L"%ls\n", forbSha);
    fclose(sf);

    swprintf_s(args, L"--local-zip \"%s\" --version 1.4.0 --no-restart --game-pid 0", forbiddenZip);
    code = RunUpdaterProcess(sandboxUpdater, args, sandbox);
    if (code == 0) {
        printf("FAIL: Updater should reject forbidden internal file (.updaterbackups/pending.journal)!\n");
        return 1;
    }
    printf("PASS: Scenario 4 forbidden file in archive safely rejected before applying.\n");

    // Clean up
    SafeDeleteDirectoryRecursive(sandbox);
    SafeDeleteDirectoryRecursive(pkgDir);

    printf("\nALL STAGE 7 / ITEM 2.7 TESTS PASSED (4/4).\n");
    return 0;
}
