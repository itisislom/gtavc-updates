#include "crypto.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <vector>

bool ValidateUrl(const wchar_t* url) {
    if (!url || !*url) return false;

    // 1. Enforce HTTPS scheme
    static const wchar_t httpsPrefix[] = L"https://";
    size_t prefixLen = wcslen(httpsPrefix);
    if (_wcsnicmp(url, httpsPrefix, prefixLen) != 0) {
        return false;
    }

    // 2. Extract hostname
    const wchar_t* hostStart = url + prefixLen;
    const wchar_t* hostEnd = hostStart;
    while (*hostEnd && *hostEnd != L'/' && *hostEnd != L':' && *hostEnd != L'?' && *hostEnd != L'#') {
        hostEnd++;
    }

    size_t hostLen = hostEnd - hostStart;
    if (hostLen == 0 || hostLen >= 256) return false;

    wchar_t host[256];
    wcsncpy_s(host, hostStart, hostLen);
    host[hostLen] = L'\0';

    // Convert hostname to lowercase
    for (wchar_t* p = host; *p; p++) {
        *p = (wchar_t)towlower(*p);
    }

    // 3. Strict allowlist
    static const wchar_t* allowlist[] = {
        L"github.com",
        L"api.github.com",
        L"raw.githubusercontent.com",
        L"objects.githubusercontent.com",
        L"release-assets.githubusercontent.com"
    };

    for (const auto& allowed : allowlist) {
        if (wcscmp(host, allowed) == 0) {
            return true;
        }
    }

    return false;
}

bool ValidateSemVer(const wchar_t* version) {
    if (!version || !*version) return false;

    const wchar_t* p = version;
    if (*p == L'v' || *p == L'V') p++;

    // Major digits
    if (!iswdigit(*p)) return false;
    while (iswdigit(*p)) p++;

    // Dot
    if (*p != L'.') return false;
    p++;

    // Minor digits
    if (!iswdigit(*p)) return false;
    while (iswdigit(*p)) p++;

    // Dot
    if (*p != L'.') return false;
    p++;

    // Patch digits
    if (!iswdigit(*p)) return false;
    while (iswdigit(*p)) p++;

    // Optional prerelease suffix: ([-.][A-Za-z0-9.]{1,16})?
    if (*p == L'-' || *p == L'.') {
        p++;
        const wchar_t* sufStart = p;
        while (*p && (iswalnum(*p) || *p == L'.' || *p == L'-')) {
            p++;
        }
        size_t sufLen = p - sufStart;
        if (sufLen == 0 || sufLen > 16) return false;
    }

    // Must be at end of string (no trailing garbage, semicolons, commands)
    return (*p == L'\0');
}

bool ComputeFileSha256(const wchar_t* filePath, wchar_t* outHexHash, size_t maxLen) {
    if (!filePath || !outHexHash || maxLen < 65) return false;

    BCRYPT_ALG_HANDLE hAlg = NULL;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!BCRYPT_SUCCESS(status)) return false;

    DWORD cbHashObject = 0;
    DWORD cbData = 0;
    status = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    std::vector<BYTE> hashObject(cbHashObject);
    BCRYPT_HASH_HANDLE hHash = NULL;
    status = BCryptCreateHash(hAlg, &hHash, hashObject.data(), cbHashObject, NULL, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, filePath, L"rb");
    if (!f) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    std::vector<BYTE> buffer(65536);
    size_t bytesRead = 0;
    while ((bytesRead = fread(buffer.data(), 1, buffer.size(), f)) > 0) {
        status = BCryptHashData(hHash, buffer.data(), (ULONG)bytesRead, 0);
        if (!BCRYPT_SUCCESS(status)) {
            fclose(f);
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return false;
        }
    }
    fclose(f);

    BYTE hashBytes[32] = { 0 };
    status = BCryptFinishHash(hHash, hashBytes, sizeof(hashBytes), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(status)) return false;

    for (int i = 0; i < 32; i++) {
        swprintf_s(outHexHash + (i * 2), 3, L"%02x", hashBytes[i]);
    }
    outHexHash[64] = L'\0';
    return true;
}

bool VerifySha256(const wchar_t* filePath, const wchar_t* expectedHexHash) {
    if (!filePath || !expectedHexHash) return false;

    // Trim expected hash
    const wchar_t* p = expectedHexHash;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;

    wchar_t cleanExpected[65];
    size_t count = 0;
    while (*p && count < 64 && iswxdigit(*p)) {
        cleanExpected[count++] = (wchar_t)towlower(*p);
        p++;
    }
    cleanExpected[count] = L'\0';

    if (count != 64) return false;

    wchar_t computedHash[65];
    if (!ComputeFileSha256(filePath, computedHash, 65)) {
        return false;
    }

    return (_wcsicmp(computedHash, cleanExpected) == 0);
}
