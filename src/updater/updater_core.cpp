#include "updater_core.h"
#include "../common/paths.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <vector>
#include <string>
#include <io.h>

void LogUpdate(const wchar_t* gameRoot,
               const wchar_t* action,
               const wchar_t* status,
               const wchar_t* targetPath,
               const wchar_t* detail) {
    if (!gameRoot || !gameRoot[0]) return;
    wchar_t logPath[MAX_PATH];
    if (swprintf_s(logPath, L"%s\\updater.log", gameRoot) <= 0) return;

    FILE* f = nullptr;
    _wfopen_s(&f, logPath, L"a, ccs=UTF-8");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    if (detail && detail[0]) {
        fwprintf(f, L"[%04u-%02u-%02u %02u:%02u:%02u] [%ls] [%ls] %ls (%ls)\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 action ? action : L"INFO", status ? status : L"OK",
                 targetPath ? targetPath : L"", detail);
    } else {
        fwprintf(f, L"[%04u-%02u-%02u %02u:%02u:%02u] [%ls] [%ls] %ls\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 action ? action : L"INFO", status ? status : L"OK",
                 targetPath ? targetPath : L"");
    }
    fclose(f);
}

static void FlushJournal(FILE* f) {
    if (!f) return;
    fflush(f);
    int fd = _fileno(f);
    if (fd >= 0) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
        }
    }
}

static bool BuildSafePath(const std::wstring& base, const std::wstring& rel, std::wstring& out) {
    if (base.empty() || rel.empty()) return false;
    std::wstring res = base;
    if (res.back() != L'\\' && res.back() != L'/') {
        res.push_back(L'\\');
    }
    for (wchar_t c : rel) {
        if (c == L'/') res.push_back(L'\\');
        else res.push_back(c);
    }
    if (res.size() >= MAX_PATH) {
        return false;
    }
    out = res;
    return true;
}

static bool IsForbiddenArchiveEntry(const std::wstring& rel) {
    std::wstring norm = rel;
    for (wchar_t& c : norm) {
        if (c == L'/') c = L'\\';
    }
    while (!norm.empty() && norm.front() == L'\\') norm.erase(norm.begin());
    while (!norm.empty() && norm.back() == L'\\') norm.pop_back();

    if (_wcsicmp(norm.c_str(), L"updater.ini") == 0) return true;
    if (_wcsicmp(norm.c_str(), L"installed_manifest.txt") == 0) return true;
    if (_wcsicmp(norm.c_str(), L"pending.journal") == 0) return true;
    if (_wcsicmp(norm.c_str(), L".updaterbackups") == 0) return true;
    if (_wcsnicmp(norm.c_str(), L".updaterbackups\\", 16) == 0) return true;
    return false;
}

static void EnsureParentDirectoryExists(const wchar_t* filePath) {
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, filePath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = L'\0';

    wchar_t* lastSlash = wcsrchr(dir, L'\\');
    if (!lastSlash) return;
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

void SafeDeleteDirectoryRecursive(const wchar_t* path) {
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return;

    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        RemoveDirectoryW(path);
        return;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        wchar_t searchPath[MAX_PATH];
        swprintf_s(searchPath, L"%s\\*.*", path);

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(searchPath, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                    wchar_t subPath[MAX_PATH];
                    swprintf_s(subPath, L"%s\\%s", path, fd.cFileName);
                    SafeDeleteDirectoryRecursive(subPath);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryW(path);
    } else {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path);
    }
}

bool CreateSecureTempDir(wchar_t* outDir, size_t maxLen) {
    wchar_t tempPath[MAX_PATH];
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len >= MAX_PATH) return false;

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    DWORD pid = GetCurrentProcessId();

    swprintf_s(outDir, maxLen, L"%sgtavc_upd_%lu_%llx", tempPath, pid, (unsigned long long)qpc.QuadPart);
    return (CreateDirectoryW(outDir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

static bool GetTarPath(wchar_t* outTarPath, size_t maxLen) {
    wchar_t sysDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    swprintf_s(outTarPath, maxLen, L"%s\\tar.exe", sysDir);
    DWORD attrs = GetFileAttributesW(outTarPath);
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

bool ValidateZipContents(const wchar_t* zipPath,
                         const wchar_t* gameRoot,
                         std::vector<std::wstring>& outFiles,
                         wchar_t* outError,
                         size_t errLen) {
    outFiles.clear();
    wchar_t tarPath[MAX_PATH];
    if (!GetTarPath(tarPath, MAX_PATH)) {
        if (outError) swprintf_s(outError, errLen, L"System utility tar.exe was not found.");
        return false;
    }

    HANDLE hPipeRead = NULL;
    HANDLE hPipeWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
        if (outError) swprintf_s(outError, errLen, L"Failed to create pipe for archive validation.");
        return false;
    }
    SetHandleInformation(hPipeRead, HANDLE_FLAG_INHERIT, 0);

    wchar_t cmdLine[MAX_PATH * 3];
    swprintf_s(cmdLine, L"\"%s\" -tf \"%s\"", tarPath, zipPath);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hPipeWrite;
    si.hStdError = hPipeWrite;

    PROCESS_INFORMATION pi = { 0 };
    BOOL ok = CreateProcessW(tarPath, cmdLine, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hPipeWrite);

    if (!ok) {
        CloseHandle(hPipeRead);
        if (outError) swprintf_s(outError, errLen, L"Failed to launch tar.exe to inspect archive.");
        return false;
    }

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hPipeRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer, bytesRead);
    }
    CloseHandle(hPipeRead);

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        if (outError) swprintf_s(outError, errLen, L"Archive is corrupted or not a valid zip (tar code: %lu).", exitCode);
        return false;
    }

    // Parse lines from output
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find_first_of("\r\n", start);
        if (end == std::string::npos) end = output.size();

        std::string rawLine = output.substr(start, end - start);
        start = end + 1;
        while (start < output.size() && (output[start] == '\r' || output[start] == '\n')) start++;

        while (!rawLine.empty() && (rawLine.back() == ' ' || rawLine.back() == '\t')) rawLine.pop_back();
        size_t lstart = 0;
        while (lstart < rawLine.size() && (rawLine[lstart] == ' ' || rawLine[lstart] == '\t')) lstart++;
        if (lstart > 0) rawLine = rawLine.substr(lstart);

        if (rawLine.empty()) continue;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, rawLine.c_str(), (int)rawLine.size(), nullptr, 0);
        if (wlen <= 0) continue;
        std::wstring wline(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, rawLine.c_str(), (int)rawLine.size(), &wline[0], wlen);

        // Strip leading dot-slash prefixes
        while (wline.size() >= 2 && wline[0] == L'.' && (wline[1] == L'/' || wline[1] == L'\\')) {
            wline.erase(0, 2);
        }
        while (!wline.empty() && (wline.front() == L'/' || wline.front() == L'\\')) {
            wline.erase(0, 1);
        }

        bool isDir = (!wline.empty() && (wline.back() == L'/' || wline.back() == L'\\'));
        if (isDir) {
            wline.pop_back();
        }
        if (wline.empty() || wline == L".") continue;

        if (IsForbiddenArchiveEntry(wline)) {
            if (outError) {
                swprintf_s(outError, errLen, L"Forbidden system file in archive: %ls", wline.c_str());
            }
            return false;
        }

        wchar_t canon[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, wline.c_str(), canon, MAX_PATH, &pErr)) {
            if (outError) {
                swprintf_s(outError, errLen, L"Zip-slip or unsafe path in archive: %ls (error %d)", wline.c_str(), (int)pErr);
            }
            return false;
        }

        if (!isDir) {
            outFiles.push_back(wline);
        }
    }

    return true;
}

bool ExtractZipToStage(const wchar_t* zipPath,
                       const wchar_t* stageDir,
                       wchar_t* outError,
                       size_t errLen) {
    wchar_t tarPath[MAX_PATH];
    if (!GetTarPath(tarPath, MAX_PATH)) {
        if (outError) swprintf_s(outError, errLen, L"System utility tar.exe was not found.");
        return false;
    }

    CreateDirectoryW(stageDir, nullptr);

    wchar_t cmdLine[MAX_PATH * 3];
    swprintf_s(cmdLine, L"\"%s\" -xf \"%s\" -C \"%s\"", tarPath, zipPath, stageDir);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(tarPath, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (outError) swprintf_s(outError, errLen, L"Failed to launch extraction process.");
        return false;
    }

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        if (outError) swprintf_s(outError, errLen, L"Failed to extract archive via tar.exe (code: %lu).", exitCode);
        return false;
    }

    return true;
}

static bool ScanDirectoryLimits(const wchar_t* dirPath,
                                size_t& fileCount,
                                unsigned long long& totalBytes,
                                size_t maxFiles,
                                unsigned long long maxBytes,
                                wchar_t* outError,
                                size_t errLen) {
    wchar_t searchPattern[MAX_PATH];
    swprintf_s(searchPattern, L"%s\\*.*", dirPath);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (outError) swprintf_s(outError, errLen, L"Reparse point (symlink/junction) detected in stage: %ls", fd.cFileName);
            FindClose(hFind);
            return false;
        }

        if (IsForbiddenArchiveEntry(fd.cFileName)) {
            if (outError) swprintf_s(outError, errLen, L"Forbidden system file in stage: %ls", fd.cFileName);
            FindClose(hFind);
            return false;
        }

        wchar_t subPath[MAX_PATH];
        swprintf_s(subPath, L"%s\\%s", dirPath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!ScanDirectoryLimits(subPath, fileCount, totalBytes, maxFiles, maxBytes, outError, errLen)) {
                FindClose(hFind);
                return false;
            }
        } else {
            fileCount++;
            if (fileCount > maxFiles) {
                if (outError) swprintf_s(outError, errLen, L"Archive file count limit exceeded (> %zu).", maxFiles);
                FindClose(hFind);
                return false;
            }

            ULARGE_INTEGER fileSize;
            fileSize.LowPart = fd.nFileSizeLow;
            fileSize.HighPart = fd.nFileSizeHigh;
            totalBytes += fileSize.QuadPart;

            if (totalBytes > maxBytes) {
                if (outError) swprintf_s(outError, errLen, L"Archive uncompressed size limit exceeded (> %llu bytes).", maxBytes);
                FindClose(hFind);
                return false;
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return true;
}

bool ValidateStageDirectory(const wchar_t* stageDir,
                            size_t maxFiles,
                            unsigned long long maxBytes,
                            wchar_t* outError,
                            size_t errLen) {
    size_t fileCount = 0;
    unsigned long long totalBytes = 0;
    return ScanDirectoryLimits(stageDir, fileCount, totalBytes, maxFiles, maxBytes, outError, errLen);
}

static void CollectFilesRecursive(const wchar_t* dirPath,
                                  const wchar_t* baseStageDir,
                                  std::vector<std::wstring>& outRelFiles) {
    std::wstring searchPattern = std::wstring(dirPath) + L"\\*.*";
    if (searchPattern.size() >= MAX_PATH) return;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    size_t baseLen = wcslen(baseStageDir);

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring fullPath = std::wstring(dirPath) + L"\\" + fd.cFileName;
        if (fullPath.size() >= MAX_PATH) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFilesRecursive(fullPath.c_str(), baseStageDir, outRelFiles);
        } else {
            const wchar_t* rel = fullPath.c_str() + baseLen;
            while (*rel == L'\\' || *rel == L'/') rel++;
            outRelFiles.push_back(rel);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

bool HasPendingJournal(const wchar_t* gameRoot) {
    std::wstring journalPath = std::wstring(gameRoot) + L"\\.updaterbackups\\pending.journal";
    if (journalPath.size() >= MAX_PATH) return false;
    DWORD attrs = GetFileAttributesW(journalPath.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
}

static bool DeleteDirectoryTreeWithBackup(const wchar_t* gameRoot,
                                          const wchar_t* dirPath,
                                          const wchar_t* relDir,
                                          const wchar_t* backupDeletedDir,
                                          FILE* jf,
                                          int currentDepth,
                                          int maxDepth) {
    if (currentDepth > maxDepth) return false;

    std::wstring searchPath = std::wstring(dirPath) + L"\\*.*";
    if (searchPath.size() >= MAX_PATH) return false;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        std::wstring itemPath = std::wstring(dirPath) + L"\\" + fd.cFileName;
        if (itemPath.size() >= MAX_PATH) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(itemPath.c_str());
            } else {
                DeleteFileW(itemPath.c_str());
            }
            continue;
        }

        std::wstring subRel = relDir;
        if (!subRel.empty() && subRel.back() != L'\\') subRel.push_back(L'\\');
        subRel.append(fd.cFileName);
        if (subRel.size() >= MAX_PATH) continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryTreeWithBackup(gameRoot, itemPath.c_str(), subRel.c_str(), backupDeletedDir, jf, currentDepth + 1, maxDepth);
            RemoveDirectoryW(itemPath.c_str());
        } else {
            if (!IsProtectedPath(gameRoot, subRel.c_str())) {
                if (backupDeletedDir && backupDeletedDir[0]) {
                    std::wstring bDst = std::wstring(backupDeletedDir) + L"\\" + subRel;
                    if (bDst.size() < MAX_PATH) {
                        EnsureParentDirectoryExists(bDst.c_str());
                        MoveFileExW(itemPath.c_str(), bDst.c_str(), MOVEFILE_REPLACE_EXISTING);
                    } else {
                        DeleteFileW(itemPath.c_str());
                    }
                } else {
                    DeleteFileW(itemPath.c_str());
                }
                if (jf) {
                    fwprintf(jf, L"DELETED_TXT=%ls\n", subRel.c_str());
                    fflush(jf);
                }
                LogUpdate(gameRoot, L"DELETE", L"SUCCESS", subRel.c_str());
            } else {
                LogUpdate(gameRoot, L"DELETE", L"PROTECTED", subRel.c_str());
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    RemoveDirectoryW(dirPath);
    return true;
}

bool ProcessDeletionsWithJournal(const wchar_t* gameRoot,
                                const wchar_t* stageDir,
                                const wchar_t* backupDeletedDir,
                                FILE* jf,
                                UpdateTransactionResult* outResult) {
    if (outResult) {
        outResult->success = true;
    }

    std::wstring delPath = std::wstring(stageDir) + L"\\delete.txt";
    if (delPath.size() >= MAX_PATH) {
        if (outResult) {
            outResult->success = false;
            swprintf_s(outResult->errorMessage, L"Path to delete.txt exceeds MAX_PATH");
        }
        return false;
    }

    DWORD delAttrs = GetFileAttributesW(delPath.c_str());
    if (delAttrs == INVALID_FILE_ATTRIBUTES || (delAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, delPath.c_str(), L"r, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, delPath.c_str(), L"r");
        if (!f) return true;
    }

    wchar_t line[MAX_PATH];
    while (fgetws(line, MAX_PATH, f)) {
        wchar_t* nl = wcspbrk(line, L"\r\n");
        if (nl) *nl = L'\0';

        wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') p++;
        size_t len = wcslen(p);
        while (len > 0 && (p[len - 1] == L' ' || p[len - 1] == L'\t')) {
            p[--len] = L'\0';
        }
        if (p[0] == L'\0' || p[0] == L'#') continue;

        for (wchar_t* c = p; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }

        std::wstring rawRel = p;
        if (rawRel.size() >= MAX_PATH) {
            LogUpdate(gameRoot, L"DELETE", L"ERROR", p, L"Path exceeds MAX_PATH limit");
            continue;
        }

        wchar_t canonDst[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, p, canonDst, MAX_PATH, &pErr)) {
            LogUpdate(gameRoot, L"DELETE", L"ERROR", p, L"Path traversal or invalid path");
            continue;
        }

        wchar_t longTarget[MAX_PATH];
        DWORD lres = GetLongPathNameW(canonDst, longTarget, MAX_PATH);
        const wchar_t* checkTarget = (lres > 0 && lres < MAX_PATH) ? longTarget : canonDst;

        if (IsProtectedPath(gameRoot, checkTarget)) {
            LogUpdate(gameRoot, L"DELETE", L"PROTECTED", p);
            continue;
        }

        DWORD attrs = GetFileAttributesW(canonDst);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            LogUpdate(gameRoot, L"DELETE", L"SKIPPED", p, L"File or directory not found");
            continue;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryTreeWithBackup(gameRoot, canonDst, p, backupDeletedDir, jf, 1, 32);
            LogUpdate(gameRoot, L"DELETE", L"SUCCESS", p, L"Directory removed");
            continue;
        }

        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            DeleteFileW(canonDst);
            LogUpdate(gameRoot, L"DELETE", L"SUCCESS", p, L"Reparse point deleted");
            continue;
        }

        if (backupDeletedDir && backupDeletedDir[0]) {
            std::wstring backupDst = std::wstring(backupDeletedDir) + L"\\" + p;
            if (backupDst.size() < MAX_PATH) {
                EnsureParentDirectoryExists(backupDst.c_str());

                if (!MoveFileExW(canonDst, backupDst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    CopyFileW(canonDst, backupDst.c_str(), FALSE);
                    DeleteFileW(canonDst);
                }
            } else {
                DeleteFileW(canonDst);
            }
        } else {
            DeleteFileW(canonDst);
        }

        if (jf) {
            fwprintf(jf, L"DELETED_TXT=%ls\n", p);
            fflush(jf);
        }
        LogUpdate(gameRoot, L"DELETE", L"SUCCESS", p);
    }

    fclose(f);
    DeleteFileW(delPath.c_str());
    return true;
}

bool ProcessDeletions(const wchar_t* gameRoot,
                      const wchar_t* stageDir,
                      const wchar_t* fromVersionOrBackupDir,
                      UpdateTransactionResult* outResult) {
    std::wstring bDir;
    if (fromVersionOrBackupDir && wcschr(fromVersionOrBackupDir, L'\\') != nullptr) {
        bDir = fromVersionOrBackupDir;
    } else {
        wchar_t cleanVer[64] = { 0 };
        wcsncpy_s(cleanVer, (fromVersionOrBackupDir && fromVersionOrBackupDir[0]) ? fromVersionOrBackupDir : L"prev", 63);
        cleanVer[63] = L'\0';
        for (wchar_t* c = cleanVer; *c; c++) {
            if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
        }
        bDir = std::wstring(gameRoot) + L"\\.updaterbackups\\" + cleanVer + L"\\deleted";
    }
    CreateDirectoryW(bDir.c_str(), nullptr);
    return ProcessDeletionsWithJournal(gameRoot, stageDir, bDir.c_str(), nullptr, outResult);
}

bool RestoreDeletedBackups(const wchar_t* gameRoot, const wchar_t* fromVersionOrBackupDir) {
    std::wstring backupDeletedDir;

    if (fromVersionOrBackupDir && wcschr(fromVersionOrBackupDir, L'\\') != nullptr) {
        backupDeletedDir = fromVersionOrBackupDir;
    } else {
        wchar_t cleanVer[64] = { 0 };
        wcsncpy_s(cleanVer, (fromVersionOrBackupDir && fromVersionOrBackupDir[0]) ? fromVersionOrBackupDir : L"prev", 63);
        cleanVer[63] = L'\0';
        for (wchar_t* c = cleanVer; *c; c++) {
            if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
        }

        // Check exact folder first
        backupDeletedDir = std::wstring(gameRoot) + L"\\.updaterbackups\\" + cleanVer + L"\\deleted";
        if (GetFileAttributesW(backupDeletedDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // Find most recent matching folder with prefix cleanVer
            wchar_t searchPattern[MAX_PATH];
            swprintf_s(searchPattern, L"%s\\.updaterbackups\\%s_*", gameRoot, cleanVer);
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(searchPattern, &fd);
            std::wstring latestMatch;
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        std::wstring cand = std::wstring(gameRoot) + L"\\.updaterbackups\\" + fd.cFileName + L"\\deleted";
                        if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            latestMatch = cand;
                        }
                    }
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            if (!latestMatch.empty()) {
                backupDeletedDir = latestMatch;
            }
        }
    }

    DWORD attrs = GetFileAttributesW(backupDeletedDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    std::vector<std::wstring> delFiles;
    CollectFilesRecursive(backupDeletedDir.c_str(), backupDeletedDir.c_str(), delFiles);
    for (const auto& rel : delFiles) {
        std::wstring srcPath = backupDeletedDir + L"\\" + rel;
        std::wstring dstPath;
        if (!BuildSafePath(gameRoot, rel, dstPath)) continue;

        EnsureParentDirectoryExists(dstPath.c_str());
        MoveFileExW(srcPath.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return true;
}

bool ApplyUpdate(const wchar_t* gameRoot,
                 const wchar_t* stageDir,
                 const wchar_t* fromVersion,
                 const wchar_t* toVersion,
                 int failAtFileIndex,
                 UpdateTransactionResult* outResult) {
    if (outResult) {
        outResult->success = false;
        outResult->filesCopied = 0;
        outResult->filesRolledBack = 0;
        outResult->errorMessage[0] = L'\0';
    }

    std::wstring backupBase = std::wstring(gameRoot) + L"\\.updaterbackups";
    CreateDirectoryW(backupBase.c_str(), nullptr);

    wchar_t cleanVer[64] = { 0 };
    wcsncpy_s(cleanVer, (fromVersion && fromVersion[0]) ? fromVersion : L"prev", 63);
    cleanVer[63] = L'\0';
    for (wchar_t* c = cleanVer; *c; c++) {
        if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
    }

    // 2.2 a: Unique timestamped backup folder per attempt (UTC yyyymmddhhmmss)
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t ts[32];
    swprintf_s(ts, L"%04u%02u%02u%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring backupVerDir = backupBase + L"\\" + cleanVer + L"_" + ts;
    CreateDirectoryW(backupVerDir.c_str(), nullptr);

    std::wstring backupFilesDir = backupVerDir + L"\\files";
    CreateDirectoryW(backupFilesDir.c_str(), nullptr);

    std::wstring backupDeletedDir = backupVerDir + L"\\deleted";
    CreateDirectoryW(backupDeletedDir.c_str(), nullptr);

    std::wstring journalPath = backupBase + L"\\pending.journal";

    // 1. Collect all files from stageDir, filter out delete.txt
    std::vector<std::wstring> rawStageFiles;
    CollectFilesRecursive(stageDir, stageDir, rawStageFiles);

    std::vector<std::wstring> stageFiles;
    for (const auto& rel : rawStageFiles) {
        if (_wcsicmp(rel.c_str(), L"delete.txt") == 0) {
            continue; // delete.txt is processed separately and never copied
        }
        if (IsForbiddenArchiveEntry(rel)) {
            if (outResult) swprintf_s(outResult->errorMessage, L"Forbidden file in archive: %ls", rel.c_str());
            LogUpdate(gameRoot, L"APPLY", L"ERROR", rel.c_str(), L"Forbidden archive entry");
            return false;
        }
        stageFiles.push_back(rel);
    }

    // 2. Validate paths, length, reparse points, and backup existing files
    std::vector<std::wstring> backedUpRelFiles;
    std::vector<std::wstring> createdRelFiles;

    for (const auto& rel : stageFiles) {
        std::wstring dstPath;
        if (!BuildSafePath(gameRoot, rel, dstPath)) {
            if (outResult) swprintf_s(outResult->errorMessage, L"Path exceeds MAX_PATH limit: %ls", rel.c_str());
            return false;
        }

        wchar_t canonDst[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, rel.c_str(), canonDst, MAX_PATH, &pErr)) {
            if (outResult) swprintf_s(outResult->errorMessage, L"Invalid target path: %ls", rel.c_str());
            return false;
        }

        DWORD attrs = GetFileAttributesW(canonDst);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (outResult) swprintf_s(outResult->errorMessage, L"Target file is a reparse point: %ls", rel.c_str());
                return false;
            }

            std::wstring backupDst = backupFilesDir + L"\\" + rel;
            if (backupDst.size() >= MAX_PATH) {
                if (outResult) swprintf_s(outResult->errorMessage, L"Backup path exceeds MAX_PATH: %ls", rel.c_str());
                return false;
            }
            EnsureParentDirectoryExists(backupDst.c_str());

            if (!CopyFileW(canonDst, backupDst.c_str(), FALSE)) {
                if (outResult) swprintf_s(outResult->errorMessage, L"Failed to create backup copy: %ls", rel.c_str());
                return false;
            }
            backedUpRelFiles.push_back(rel);
        } else {
            createdRelFiles.push_back(rel);
        }
    }

    // 2b. Backup installed_manifest.txt and version.txt before modifying anything
    std::wstring liveManifestPath = std::wstring(gameRoot) + L"\\installed_manifest.txt";
    std::wstring backupManifestPath = backupVerDir + L"\\installed_manifest.txt";
    bool manifestBackedUp = false;
    if (GetFileAttributesW(liveManifestPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(liveManifestPath.c_str(), backupManifestPath.c_str(), FALSE)) {
            manifestBackedUp = true;
        }
    }

    std::wstring liveVerPath = std::wstring(gameRoot) + L"\\version.txt";
    std::wstring backupVerPath = backupVerDir + L"\\version.txt";
    bool versionBackedUp = false;
    if (GetFileAttributesW(liveVerPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(liveVerPath.c_str(), backupVerPath.c_str(), FALSE)) {
            versionBackedUp = true;
        }
    }

    // 3. Open pending.journal and write initial records
    FILE* jf = nullptr;
    _wfopen_s(&jf, journalPath.c_str(), L"w, ccs=UTF-8");
    if (!jf) {
        if (outResult) swprintf_s(outResult->errorMessage, L"Failed to open pending.journal for writing.");
        return false;
    }

    fwprintf(jf, L"[JOURNAL_V1]\n");
    fwprintf(jf, L"STATUS=APPLYING\n");
    fwprintf(jf, L"FROM_VERSION=%ls\n", fromVersion ? fromVersion : L"0.0.0");
    fwprintf(jf, L"TO_VERSION=%ls\n", toVersion ? toVersion : L"unknown");
    fwprintf(jf, L"BACKUP_DIR=%ls\n", backupVerDir.c_str());
    fwprintf(jf, L"STAGE_DIR=%ls\n", stageDir);
    fwprintf(jf, L"BACKUP_MANIFEST=%d\n", manifestBackedUp ? 1 : 0);
    fwprintf(jf, L"BACKUP_VERSION=%d\n", versionBackedUp ? 1 : 0);

    for (const auto& rel : backedUpRelFiles) {
        fwprintf(jf, L"BACKED_UP=%ls\n", rel.c_str());
    }
    for (const auto& rel : createdRelFiles) {
        fwprintf(jf, L"CREATED=%ls\n", rel.c_str());
    }
    FlushJournal(jf);

    // 4. Phase APPLYING: Copy files from stage to gameRoot
    std::vector<std::wstring> successfullyCopied;
    bool failed = false;
    int failedAttemptIndex = -1;

    // Rollback helper lambda
    auto doFullRollback = [&](int failedIdx) -> bool {
        fwprintf(jf, L"STATUS=ROLLING_BACK\n");
        FlushJournal(jf);

        bool rollbackIncomplete = false;
        int rollbackCount = 0;

        // Rollback successfully copied files PLUS the file from failed attempt (Point 3)
        std::vector<std::wstring> filesToRollback = successfullyCopied;
        if (failedIdx >= 0 && failedIdx < (int)stageFiles.size()) {
            const auto& failedRel = stageFiles[failedIdx];
            if (std::find(filesToRollback.begin(), filesToRollback.end(), failedRel) == filesToRollback.end()) {
                filesToRollback.push_back(failedRel);
            }
        }

        for (const auto& rel : filesToRollback) {
            std::wstring dstPath = std::wstring(gameRoot) + L"\\" + rel;
            std::wstring backupSrc = backupFilesDir + L"\\" + rel;

            bool wasBackedUp = (std::find(backedUpRelFiles.begin(), backedUpRelFiles.end(), rel) != backedUpRelFiles.end());
            if (wasBackedUp) {
                if (!CopyFileW(backupSrc.c_str(), dstPath.c_str(), FALSE)) {
                    rollbackIncomplete = true;
                }
            } else {
                if (!DeleteFileW(dstPath.c_str())) {
                    rollbackIncomplete = true;
                }
            }
            rollbackCount++;
        }

        // Restore any deleted files from backup
        DWORD delAttrs = GetFileAttributesW(backupDeletedDir.c_str());
        if (delAttrs != INVALID_FILE_ATTRIBUTES && (delAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            std::vector<std::wstring> allDelFiles;
            CollectFilesRecursive(backupDeletedDir.c_str(), backupDeletedDir.c_str(), allDelFiles);
            for (const auto& rel : allDelFiles) {
                std::wstring delSrc = backupDeletedDir + L"\\" + rel;
                std::wstring dstPath = std::wstring(gameRoot) + L"\\" + rel;
                EnsureParentDirectoryExists(dstPath.c_str());
                if (!MoveFileExW(delSrc.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    rollbackIncomplete = true;
                }
            }
        }

        std::wstring oldPath = std::wstring(gameRoot) + L"\\updater.exe.old";
        std::wstring livePath = std::wstring(gameRoot) + L"\\updater.exe";
        if (GetFileAttributesW(oldPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            MoveFileExW(oldPath.c_str(), livePath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        // Point 3: restore installed_manifest.txt and version.txt
        if (manifestBackedUp) {
            CopyFileW(backupManifestPath.c_str(), liveManifestPath.c_str(), FALSE);
        } else {
            DeleteFileW(liveManifestPath.c_str());
        }
        if (versionBackedUp) {
            CopyFileW(backupVerPath.c_str(), liveVerPath.c_str(), FALSE);
        } else {
            DeleteFileW(liveVerPath.c_str());
        }

        if (rollbackIncomplete) {
            fwprintf(jf, L"STATUS=ROLLBACK_INCOMPLETE\n");
            FlushJournal(jf);
            fclose(jf);
            LogUpdate(gameRoot, L"APPLY", L"FAILED", L"Rollback incomplete, pending.journal retained");
            if (outResult) {
                outResult->success = false;
                outResult->filesCopied = (int)successfullyCopied.size();
                outResult->filesRolledBack = rollbackCount;
                swprintf_s(outResult->errorMessage, L"Update failure and incomplete rollback: failed to restore some files.");
            }
            return false;
        }

        fclose(jf);
        DeleteFileW(journalPath.c_str());
        LogUpdate(gameRoot, L"APPLY", L"FAILED", L"Update failed, rolled back cleanly");

        if (outResult) {
            outResult->success = false;
            outResult->filesCopied = (int)successfullyCopied.size();
            outResult->filesRolledBack = rollbackCount;
        }
        return false;
    };

    for (int i = 0; i < (int)stageFiles.size(); i++) {
        const auto& rel = stageFiles[i];

        fwprintf(jf, L"ATTEMPT=%ls\n", rel.c_str());
        FlushJournal(jf);

#ifdef TESTHOOKS
        if (failAtFileIndex >= 0 && i == failAtFileIndex) {
            failed = true;
            failedAttemptIndex = i;
            if (outResult) swprintf_s(outResult->errorMessage, L"Simulated failure at file #%d (testhook --fail-at).", i);
            break;
        }
#else
        (void)failAtFileIndex;
#endif

        std::wstring srcPath = std::wstring(stageDir) + L"\\" + rel;
        std::wstring dstPath = std::wstring(gameRoot) + L"\\" + rel;
        EnsureParentDirectoryExists(dstPath.c_str());

        // Special handling for updater.exe self-update
        if (_wcsicmp(rel.c_str(), L"updater.exe") == 0) {
            std::wstring oldPath = std::wstring(gameRoot) + L"\\updater.exe.old";
            DeleteFileW(oldPath.c_str());
            MoveFileExW(dstPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        if (!CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE)) {
            failed = true;
            failedAttemptIndex = i;
            if (outResult) swprintf_s(outResult->errorMessage, L"Failed to copy file: %ls (GetLastError: %lu)", rel.c_str(), GetLastError());
            break;
        }

        successfullyCopied.push_back(rel);
        fwprintf(jf, L"COPIED=%ls\n", rel.c_str());
        FlushJournal(jf);
    }

    if (failed) {
        return doFullRollback(failedAttemptIndex);
    }

    // 5. Phase DELETING: old installed_manifest.txt orphans + delete.txt
    fwprintf(jf, L"STATUS=DELETING\n");
    FlushJournal(jf);

    // 2.4: Clean orphans from previous installed_manifest.txt
    std::wstring manifestPath = liveManifestPath;
    FILE* oldMf = nullptr;
    _wfopen_s(&oldMf, manifestPath.c_str(), L"r, ccs=UTF-8");
    if (!oldMf) {
        _wfopen_s(&oldMf, manifestPath.c_str(), L"r");
    }
    if (oldMf) {
        wchar_t mLine[MAX_PATH];
        while (fgetws(mLine, MAX_PATH, oldMf)) {
            wchar_t* nl = wcspbrk(mLine, L"\r\n");
            if (nl) *nl = L'\0';
            wchar_t* p = mLine;
            while (*p == L' ' || *p == L'\t') p++;
            size_t l = wcslen(p);
            while (l > 0 && (p[l - 1] == L' ' || p[l - 1] == L'\t')) p[--l] = L'\0';
            if (p[0] == L'\0' || p[0] == L'#') continue;

            for (wchar_t* c = p; *c; c++) {
                if (*c == L'/') *c = L'\\';
            }

            // If file is not present in new stageFiles and not protected, remove it
            bool inNewStage = false;
            for (const auto& sf : stageFiles) {
                if (_wcsicmp(sf.c_str(), p) == 0) {
                    inNewStage = true;
                    break;
                }
            }

            if (!inNewStage && !IsProtectedPath(gameRoot, p)) {
                wchar_t canonDst[MAX_PATH];
                PathError pErr = PathError::Ok;
                if (ResolveInsideRoot(gameRoot, p, canonDst, MAX_PATH, &pErr)) {
                    DWORD attrs = GetFileAttributesW(canonDst);
                    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                        std::wstring bDst = backupDeletedDir + L"\\" + p;
                        EnsureParentDirectoryExists(bDst.c_str());
                        if (!MoveFileExW(canonDst, bDst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                            CopyFileW(canonDst, bDst.c_str(), FALSE);
                            DeleteFileW(canonDst);
                        }
                        fwprintf(jf, L"DELETED_MANIFEST=%ls\n", p);
                        FlushJournal(jf);
                        LogUpdate(gameRoot, L"DELETE_MANIFEST", L"SUCCESS", p);
                    }
                }
            }
        }
        fclose(oldMf);
    }

    // Process delete.txt safely
    ProcessDeletionsWithJournal(gameRoot, stageDir, backupDeletedDir.c_str(), jf, outResult);
    FlushJournal(jf);

    // 6. Phase FINALIZING: write version.txt, installed_manifest.txt, remove staging
    fwprintf(jf, L"STATUS=FINALIZING\n");
    FlushJournal(jf);

#ifdef TESTHOOKS
    if (failAtFileIndex == 9999) { // testhook for crash in FINALIZING
        fclose(jf);
        if (outResult) swprintf_s(outResult->errorMessage, L"Simulated crash in FINALIZING (testhook).");
        return false;
    }
#endif

    // Write new installed_manifest.txt atomically via .tmp + MoveFileExW
    std::wstring manifestTmp = manifestPath + L".tmp";
    FILE* newMf = nullptr;
    _wfopen_s(&newMf, manifestTmp.c_str(), L"w, ccs=UTF-8");
    if (!newMf) {
        if (outResult) swprintf_s(outResult->errorMessage, L"Failed to create temporary manifest file.");
        return doFullRollback(-1);
    }
    for (const auto& rel : stageFiles) {
        std::wstring norm = rel;
        for (wchar_t& c : norm) {
            if (c == L'/') c = L'\\';
        }
        fwprintf(newMf, L"%ls\n", norm.c_str());
    }
    fclose(newMf);
    if (!MoveFileExW(manifestTmp.c_str(), manifestPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(manifestTmp.c_str());
        if (outResult) swprintf_s(outResult->errorMessage, L"Failed to write installed_manifest.txt (MoveFileExW)");
        return doFullRollback(-1);
    }

    // Write new version.txt atomically via .tmp + MoveFileExW
    std::wstring verFile = liveVerPath;
    std::wstring verTmp = verFile + L".tmp";
    FILE* vf = nullptr;
    _wfopen_s(&vf, verTmp.c_str(), L"w");
    if (!vf) {
        if (outResult) swprintf_s(outResult->errorMessage, L"Failed to create temporary version.txt file.");
        return doFullRollback(-1);
    }
    fwprintf(vf, L"%ls\n", toVersion ? toVersion : L"1.0.0");
    fclose(vf);
    if (!MoveFileExW(verTmp.c_str(), verFile.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(verTmp.c_str());
        if (outResult) swprintf_s(outResult->errorMessage, L"Failed to write version.txt (MoveFileExW)");
        return doFullRollback(-1);
    }

    SafeDeleteDirectoryRecursive(stageDir);
    PruneOldBackups(gameRoot, 5);

    // 7. Phase COMPLETED: remove pending.journal
    fwprintf(jf, L"STATUS=COMPLETED\n");
    FlushJournal(jf);
    fclose(jf);
    DeleteFileW(journalPath.c_str());

    LogUpdate(gameRoot, L"APPLY", L"SUCCESS", toVersion ? toVersion : L"");

    if (outResult) {
        outResult->success = true;
        outResult->filesCopied = (int)stageFiles.size();
        outResult->filesRolledBack = 0;
    }

    return true;
}

bool RecoverPendingTransaction(const wchar_t* gameRoot,
                               UpdateTransactionResult* outResult) {
    if (outResult) {
        outResult->success = false;
        outResult->filesCopied = 0;
        outResult->filesRolledBack = 0;
        outResult->errorMessage[0] = L'\0';
    }

    std::wstring journalPath = std::wstring(gameRoot) + L"\\.updaterbackups\\pending.journal";

    FILE* jf = nullptr;
    _wfopen_s(&jf, journalPath.c_str(), L"r, ccs=UTF-8");
    if (!jf) return false;

    wchar_t line[1024];
    wchar_t backupDir[MAX_PATH] = { 0 };
    std::wstring lastAttempt;
    int manifestBackedUp = -1;
    int versionBackedUp = -1;
    std::vector<std::wstring> copiedFiles;
    std::vector<std::wstring> backedUpFiles;
    std::vector<std::wstring> createdFiles;
    std::vector<std::wstring> deletedFiles;

    while (fgetws(line, 1024, jf)) {
        wchar_t* nl = wcspbrk(line, L"\r\n");
        if (nl) *nl = L'\0';

        if (wcsncmp(line, L"BACKUP_DIR=", 11) == 0) {
            wcsncpy_s(backupDir, line + 11, MAX_PATH - 1);
        } else if (wcsncmp(line, L"ATTEMPT=", 8) == 0) {
            lastAttempt = line + 8;
        } else if (wcsncmp(line, L"COPIED=", 7) == 0) {
            copiedFiles.push_back(line + 7);
            if (lastAttempt == (line + 7)) {
                lastAttempt.clear();
            }
        } else if (wcsncmp(line, L"BACKED_UP=", 10) == 0) {
            backedUpFiles.push_back(line + 10);
        } else if (wcsncmp(line, L"CREATED=", 8) == 0) {
            createdFiles.push_back(line + 8);
        } else if (wcsncmp(line, L"DELETED_MANIFEST=", 17) == 0) {
            deletedFiles.push_back(line + 17);
        } else if (wcsncmp(line, L"DELETED_TXT=", 12) == 0) {
            deletedFiles.push_back(line + 12);
        } else if (wcsncmp(line, L"BACKUP_MANIFEST=", 16) == 0) {
            manifestBackedUp = _wtoi(line + 16);
        } else if (wcsncmp(line, L"BACKUP_VERSION=", 15) == 0) {
            versionBackedUp = _wtoi(line + 15);
        }
    }
    fclose(jf);

    if (backupDir[0] == L'\0') {
        DeleteFileW(journalPath.c_str());
        return false;
    }

    // Point 3: if there was an ATTEMPT without COPIED, include it in files to restore
    if (!lastAttempt.empty()) {
        if (std::find(copiedFiles.begin(), copiedFiles.end(), lastAttempt) == copiedFiles.end()) {
            copiedFiles.push_back(lastAttempt);
        }
    }

    // 2.2 e: Path validation on every path in the journal before touching anything!
    auto validatePath = [&](const std::wstring& p) -> bool {
        wchar_t canonDst[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, p.c_str(), canonDst, MAX_PATH, &pErr)) {
            LogUpdate(gameRoot, L"RECOVERY", L"ERROR", p.c_str(), L"Dangerous path in pending.journal! Aborting recovery.");
            if (outResult) {
                swprintf_s(outResult->errorMessage, L"Unsafe path detected in pending.journal: %ls. Recovery aborted.", p.c_str());
            }
            return false;
        }
        return true;
    };

    for (const auto& p : copiedFiles) {
        if (!validatePath(p)) return false;
    }
    for (const auto& p : backedUpFiles) {
        if (!validatePath(p)) return false;
    }
    for (const auto& p : createdFiles) {
        if (!validatePath(p)) return false;
    }
    for (const auto& p : deletedFiles) {
        if (!validatePath(p)) return false;
    }

    std::wstring backupFilesDir = std::wstring(backupDir) + L"\\files";
    std::wstring backupDeletedDir = std::wstring(backupDir) + L"\\deleted";

    int restored = 0;

    // Restore or remove copied files
    for (const auto& rel : copiedFiles) {
        std::wstring dstPath = std::wstring(gameRoot) + L"\\" + rel;
        std::wstring backupSrc = backupFilesDir + L"\\" + rel;

        DWORD attrs = GetFileAttributesW(backupSrc.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            CopyFileW(backupSrc.c_str(), dstPath.c_str(), FALSE);
        } else {
            DeleteFileW(dstPath.c_str());
        }
        restored++;
    }

    // Restore deleted files from backup
    DWORD delAttrs = GetFileAttributesW(backupDeletedDir.c_str());
    if (delAttrs != INVALID_FILE_ATTRIBUTES && (delAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::vector<std::wstring> allDelFiles;
        CollectFilesRecursive(backupDeletedDir.c_str(), backupDeletedDir.c_str(), allDelFiles);
        for (const auto& rel : allDelFiles) {
            std::wstring delSrc = backupDeletedDir + L"\\" + rel;
            std::wstring dstPath = std::wstring(gameRoot) + L"\\" + rel;
            EnsureParentDirectoryExists(dstPath.c_str());
            MoveFileExW(delSrc.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            restored++;
        }
    }

    std::wstring oldUpdater = std::wstring(gameRoot) + L"\\updater.exe.old";
    std::wstring liveUpdater = std::wstring(gameRoot) + L"\\updater.exe";
    if (GetFileAttributesW(oldUpdater.c_str()) != INVALID_FILE_ATTRIBUTES) {
        MoveFileExW(oldUpdater.c_str(), liveUpdater.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // Point 3: Restore installed_manifest.txt
    std::wstring liveManifest = std::wstring(gameRoot) + L"\\installed_manifest.txt";
    std::wstring backupManifest = std::wstring(backupDir) + L"\\installed_manifest.txt";
    if (manifestBackedUp == 1 || (manifestBackedUp == -1 && GetFileAttributesW(backupManifest.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        CopyFileW(backupManifest.c_str(), liveManifest.c_str(), FALSE);
    } else if (manifestBackedUp == 0) {
        DeleteFileW(liveManifest.c_str());
    }

    // Point 3: Restore version.txt
    std::wstring liveVersion = std::wstring(gameRoot) + L"\\version.txt";
    std::wstring backupVersion = std::wstring(backupDir) + L"\\version.txt";
    if (versionBackedUp == 1 || (versionBackedUp == -1 && GetFileAttributesW(backupVersion.c_str()) != INVALID_FILE_ATTRIBUTES)) {
        CopyFileW(backupVersion.c_str(), liveVersion.c_str(), FALSE);
    } else if (versionBackedUp == 0) {
        DeleteFileW(liveVersion.c_str());
    }

    DeleteFileW(journalPath.c_str());
    LogUpdate(gameRoot, L"RECOVERY", L"SUCCESS", L"Previous state restored cleanly");

    if (outResult) {
        outResult->success = true;
        outResult->filesRolledBack = restored;
    }
    return true;
}

void PruneOldBackups(const wchar_t* gameRoot, int maxSlots) {
    wchar_t searchPath[MAX_PATH];
    swprintf_s(searchPath, L"%s\\.updaterbackups\\*.*", gameRoot);

    struct FolderEntry {
        std::wstring path;
        FILETIME ft;
    };
    std::vector<FolderEntry> entries;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t fullP[MAX_PATH];
            swprintf_s(fullP, L"%s\\.updaterbackups\\%ls", gameRoot, fd.cFileName);
            FolderEntry entry;
            entry.path = fullP;
            entry.ft = fd.ftCreationTime;
            entries.push_back(entry);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    if ((int)entries.size() <= maxSlots) return;

    std::sort(entries.begin(), entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
        return CompareFileTime(&a.ft, &b.ft) < 0;
    });

    int toDelete = (int)entries.size() - maxSlots;
    for (int i = 0; i < toDelete; i++) {
        SafeDeleteDirectoryRecursive(entries[i].path.c_str());
    }
}
