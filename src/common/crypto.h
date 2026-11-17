#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")

// Validates initial URL via InternetCrackUrlW:
// - Strictly HTTPS (port 443)
// - No credentials (no UserName/Password, no '@')
// - Host strictly in allowlist (github.com, api.github.com, raw.githubusercontent.com)
// - Path strictly prefixed with /itisislom/gtavc-updates/
bool ValidateUrl(const wchar_t* url);

// Validates redirect target Location header via InternetCrackUrlW:
// - Strictly HTTPS (port 443)
// - No credentials (no UserName/Password, no '@')
// - Host strictly in allowlist (github.com, objects.githubusercontent.com, release-assets.githubusercontent.com)
bool ValidateRedirectTarget(const wchar_t* url);

// Validates semantic version string against ^v?\d+\.\d+\.\d+([-.][A-Za-z0-9.]{1,16})?$
bool ValidateSemVer(const wchar_t* version);

// Computes SHA-256 hash of a file using Windows CNG (bcrypt.lib)
bool ComputeFileSha256(const wchar_t* filePath, wchar_t* outHexHash, size_t maxLen);

// Verifies SHA-256 hash of a file against expected 64-character hex string
bool VerifySha256(const wchar_t* filePath, const wchar_t* expectedHexHash);
