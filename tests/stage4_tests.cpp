#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cassert>
#include "../src/common/crypto.h"

int main() {
    printf("=== STAGE 4 / ITEM 2.1 TESTS: SOURCE TRUST & INTEGRITY ===\n");

    printf("[TEST 1] Testing ValidateUrl allowlist, credentials rejection & path prefix enforcement...\n");
    // MUST ACCEPT:
    assert(ValidateUrl(L"https://github.com/itisislom/gtavc-updates/releases/download/v1.0.0/update.zip"));
    assert(ValidateUrl(L"https://api.github.com/repos/itisislom/gtavc-updates/releases?per_page=5"));
    assert(ValidateUrl(L"https://raw.githubusercontent.com/itisislom/gtavc-updates/main/version.json"));

    // MUST REJECT:
    assert(!ValidateUrl(L"https://github.com:x@evil.com/a")); // Host colon credentials bypass
    assert(!ValidateUrl(L"https://github.com@evil.com/a")); // Userinfo bypass
    assert(!ValidateUrl(L"https://evil.com/github.com/")); // Path confusion
    assert(!ValidateUrl(L"https://github.com.evil.com/")); // Subdomain spoofing
    assert(!ValidateUrl(L"https://github.com/someoneelse/repo/releases/download/v1/update.zip")); // Other repository
    assert(!ValidateUrl(L"http://github.com/itisislom/gtavc-updates/releases/download/v1.0.0/update.zip")); // HTTP scheme
    assert(!ValidateUrl(L"ftp://github.com/file.zip")); // Non-HTTP protocol
    assert(!ValidateUrl(L"")); // Empty
    assert(!ValidateUrl(nullptr)); // Null

    // Redirect target validation
    assert(ValidateRedirectTarget(L"https://objects.githubusercontent.com/github-production-release-asset-2e65be/12345"));
    assert(ValidateRedirectTarget(L"https://release-assets.githubusercontent.com/download/file.zip"));
    assert(ValidateRedirectTarget(L"https://github.com/itisislom/gtavc-updates/releases/download/v1.0.0/update.zip"));
    assert(!ValidateRedirectTarget(L"https://evil.com/a")); // Redirect to evil.com rejected
    assert(!ValidateRedirectTarget(L"https://github.com@evil.com/a")); // Redirect with userinfo rejected
    assert(!ValidateRedirectTarget(L"http://objects.githubusercontent.com/file.zip")); // HTTP redirect rejected
    printf("PASS: ValidateUrl & ValidateRedirectTarget tests passed.\n");

    printf("[TEST 2] Testing ValidateSemVer format verification...\n");
    // MUST ACCEPT:
    assert(ValidateSemVer(L"1.0.0"));
    assert(ValidateSemVer(L"v1.0.0"));
    assert(ValidateSemVer(L"V2.15.3"));
    assert(ValidateSemVer(L"v1.0.0-beta"));
    assert(ValidateSemVer(L"1.2.3.4"));
    assert(ValidateSemVer(L"1.2.3-rc1"));

    // MUST REJECT:
    assert(!ValidateSemVer(L"1.0.0;calc.exe")); // Command injection
    assert(!ValidateSemVer(L"1.0.0\" --fail-at 1")); // Quote injection
    assert(!ValidateSemVer(L"1.0")); // Missing patch
    assert(!ValidateSemVer(L"version1")); // Non-digits
    assert(!ValidateSemVer(L"")); // Empty
    assert(!ValidateSemVer(nullptr)); // Null
    printf("PASS: ValidateSemVer tests passed.\n");

    printf("[TEST 3] Testing CNG BCrypt SHA-256 verification & bit-flip detection...\n");
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    wchar_t sampleFile[MAX_PATH];
    swprintf_s(sampleFile, L"%sgtavc_sha_test.bin", tempPath);

    FILE* f = nullptr;
    _wfopen_s(&f, sampleFile, L"wb");
    assert(f != nullptr);
    if (f) {
        for (int i = 0; i < 1024; i++) {
            fputc((i * 31) & 0xFF, f);
        }
        fclose(f);
    }

    wchar_t originalHash[65];
    bool ok = ComputeFileSha256(sampleFile, originalHash, 65);
    assert(ok);
    printf("Computed SHA-256: %ls\n", originalHash);

    // Verify valid hash passes
    assert(VerifySha256(sampleFile, originalHash));

    // Flip 1 byte in the file
    _wfopen_s(&f, sampleFile, L"r+b");
    assert(f != nullptr);
    if (f) {
        fseek(f, 512, SEEK_SET);
        int b = fgetc(f);
        fseek(f, 512, SEEK_SET);
        fputc(b ^ 0x01, f);
        fclose(f);
    }

    // Verify flipped file FAILS against original hash
    bool tamperedPassed = VerifySha256(sampleFile, originalHash);
    assert(!tamperedPassed);
    printf("PASS: Bit-flip detected: tampered archive correctly rejected by SHA-256.\n");

    DeleteFileW(sampleFile);

    printf("\nALL STAGE 4 / ITEM 2.1 TESTS PASSED (3/3).\n");
    return 0;
}
