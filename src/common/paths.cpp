#include "paths.h"
#include <cwchar>
#include <cstdio>
#include <algorithm>

const char* PathErrorToString(PathError err) {
    switch (err) {
        case PathError::Ok: return "OK";
        case PathError::NullOrEmpty: return "Path is null or empty";
        case PathError::TraversalDetected: return "Directory traversal '..' detected";
        case PathError::AbsolutePath: return "Absolute or UNC path detected";
        case PathError::AlternateDataStream: return "Alternate data stream ':' detected";
        case PathError::ReservedDeviceName: return "DOS reserved device name detected";
        case PathError::InvalidSegmentEnding: return "Segment ends with dot or space";
        case PathError::ControlCharacters: return "Control characters detected";
        case PathError::ReparsePoint: return "Reparse point / symlink / junction detected in path";
        case PathError::PathTooLong: return "Path exceeds buffer capacity";
        case PathError::OutsideRoot: return "Resolved path escapes root directory";
        default: return "Unknown path error";
    }
}

static bool IsDosDeviceName(const wchar_t* seg, size_t segLen) {
    if (segLen == 0) return false;

    // Strip extension if present: "nul.txt" -> "nul"
    size_t baseLen = segLen;
    for (size_t i = 0; i < segLen; i++) {
        if (seg[i] == L'.') {
            baseLen = i;
            break;
        }
    }
    if (baseLen == 0) return false;

    static const wchar_t* devices[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };

    for (const auto& dev : devices) {
        if (baseLen == wcslen(dev) && _wcsnicmp(seg, dev, baseLen) == 0) {
            return true;
        }
    }
    return false;
}

bool HasReparsePointInChain(const wchar_t* path) {
    if (!path || !path[0]) return false;

    wchar_t probe[MAX_PATH];
    probe[0] = L'\0';
    wcsncpy_s(probe, path, MAX_PATH - 1);
    probe[MAX_PATH - 1] = L'\0';

    // Step down directory hierarchy
    for (wchar_t* p = probe; *p; p++) {
        if (*p == L'\\') {
            *p = L'\0';
            DWORD attrs = GetFileAttributesW(probe);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                return true;
            }
            *p = L'\\';
        }
    }

    DWORD finalAttrs = GetFileAttributesW(probe);
    if (finalAttrs != INVALID_FILE_ATTRIBUTES && (finalAttrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
    return false;
}

bool ResolveInsideRoot(
    const wchar_t* root,
    const wchar_t* rel,
    wchar_t* out,
    size_t cap,
    PathError* err
) {
    auto setErr = [err](PathError code) {
        if (err) *err = code;
        return false;
    };

    if (!root || !root[0] || !rel || !rel[0] || !out || cap < 4) {
        return setErr(PathError::NullOrEmpty);
    }

    out[0] = L'\0';

    // 1. Check control characters (< 32)
    for (const wchar_t* c = rel; *c; c++) {
        if (*c < 32) return setErr(PathError::ControlCharacters);
    }

    // 2. Reject absolute paths, drive letters, UNC, ADS
    if (rel[0] == L'\\' || rel[0] == L'/') {
        return setErr(PathError::AbsolutePath);
    }
    if (wcschr(rel, L':') != nullptr) {
        return setErr(PathError::AlternateDataStream);
    }
    if (wcsncmp(rel, L"\\\\", 2) == 0 || wcsncmp(rel, L"//", 2) == 0) {
        return setErr(PathError::AbsolutePath);
    }

    // 3. Scan each segment
    const wchar_t* segStart = rel;
    while (*segStart) {
        while (*segStart == L'\\' || *segStart == L'/') segStart++;
        if (!*segStart) break;

        const wchar_t* segEnd = segStart;
        while (*segEnd && *segEnd != L'\\' && *segEnd != L'/') segEnd++;

        size_t segLen = segEnd - segStart;
        if (segLen == 2 && segStart[0] == L'.' && segStart[1] == L'.') {
            return setErr(PathError::TraversalDetected);
        }
        if (segLen == 1 && segStart[0] == L'.') {
            return setErr(PathError::TraversalDetected);
        }

        // Segments ending with dot or space
        if (segStart[segLen - 1] == L'.' || segStart[segLen - 1] == L' ') {
            return setErr(PathError::InvalidSegmentEnding);
        }

        if (IsDosDeviceName(segStart, segLen)) {
            return setErr(PathError::ReservedDeviceName);
        }

        segStart = segEnd;
    }

    // 4. Resolve canonical root
    wchar_t canonRoot[MAX_PATH];
    DWORD lenRoot = GetFullPathNameW(root, MAX_PATH, canonRoot, nullptr);
    if (lenRoot == 0 || lenRoot >= MAX_PATH) {
        return setErr(PathError::PathTooLong);
    }
    while (lenRoot > 0 && (canonRoot[lenRoot - 1] == L'\\' || canonRoot[lenRoot - 1] == L'/')) {
        canonRoot[--lenRoot] = L'\0';
    }

    // 5. Build candidate path
    wchar_t candidate[MAX_PATH * 2];
    int written = swprintf_s(candidate, L"%s\\%s", canonRoot, rel);
    if (written < 0 || (size_t)written >= sizeof(candidate) / sizeof(candidate[0])) {
        return setErr(PathError::PathTooLong);
    }

    // Normalize forward slashes in candidate
    for (wchar_t* p = candidate; *p; p++) {
        if (*p == L'/') *p = L'\\';
    }

    // 6. Get canonical full path
    wchar_t canonCandidate[MAX_PATH];
    DWORD lenCand = GetFullPathNameW(candidate, MAX_PATH, canonCandidate, nullptr);
    if (lenCand == 0 || lenCand >= MAX_PATH) {
        return setErr(PathError::PathTooLong);
    }
    while (lenCand > 0 && (canonCandidate[lenCand - 1] == L'\\' || canonCandidate[lenCand - 1] == L'/')) {
        canonCandidate[--lenCand] = L'\0';
    }

    // 7. Verify candidate is inside canonRoot
    if (_wcsnicmp(canonCandidate, canonRoot, lenRoot) != 0) {
        return setErr(PathError::OutsideRoot);
    }
    // Must be either equal to root or have a backslash right after root
    if (canonCandidate[lenRoot] != L'\\' && canonCandidate[lenRoot] != L'\0') {
        return setErr(PathError::OutsideRoot);
    }

    // 8. Check reparse point
    if (HasReparsePointInChain(canonCandidate)) {
        return setErr(PathError::ReparsePoint);
    }

    if (lenCand + 1 > cap) {
        return setErr(PathError::PathTooLong);
    }

    wcscpy_s(out, cap, canonCandidate);
    if (err) *err = PathError::Ok;
    return true;
}

bool IsProtectedPath(const wchar_t* root, const wchar_t* fullOrRelPath) {
    if (!root || !fullOrRelPath) return true;

    wchar_t resolved[MAX_PATH];
    PathError pErr;
    const wchar_t* target = fullOrRelPath;

    // If it's relative, resolve it
    if (ResolveInsideRoot(root, fullOrRelPath, resolved, MAX_PATH, &pErr)) {
        target = resolved;
    }

    wchar_t canonRoot[MAX_PATH];
    DWORD lenRoot = GetFullPathNameW(root, MAX_PATH, canonRoot, nullptr);
    if (lenRoot == 0) return true;
    while (lenRoot > 0 && (canonRoot[lenRoot - 1] == L'\\' || canonRoot[lenRoot - 1] == L'/')) {
        canonRoot[--lenRoot] = L'\0';
    }

    // Relative to canonRoot
    const wchar_t* rel = target;
    if (_wcsnicmp(target, canonRoot, lenRoot) == 0) {
        rel = target + lenRoot;
        while (*rel == L'\\') rel++;
    }

    // Protected exact files
    static const wchar_t* protectedFiles[] = {
        L"gta-vc.exe",
        L"updater.exe",
        L"UpdateChecker.asi",
        L"dinput8.dll",
        L"updater.ini",
        L"installed_manifest.txt"
    };

    for (const auto& file : protectedFiles) {
        if (_wcsicmp(rel, file) == 0) {
            return true;
        }
    }

    // Protected directories
    static const wchar_t* protectedDirs[] = {
        L"",
        L".",
        L".updaterbackups",
        L"scripts",
        L"modloader",
        L"cleo",
        L"data",
        L"models",
        L"txd",
        L"audio",
        L"anim",
        L"movies",
        L"text",
        L"_tools"
    };

    for (const auto& dir : protectedDirs) {
        if (_wcsicmp(rel, dir) == 0) {
            return true;
        }
    }

    // Never allow deleting the root directory itself
    if (_wcsicmp(target, canonRoot) == 0) {
        return true;
    }

    return false;
}
