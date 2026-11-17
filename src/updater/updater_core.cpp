#include "updater_core.h"
#include "../common/paths.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <algorithm>
#include <vector>
#include <string>

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

    // Use high-resolution counter and PID to generate unique folder name
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
        if (outError) swprintf_s(outError, errLen, L"Системная утилита tar.exe не найдена.");
        return false;
    }

    HANDLE hPipeRead = NULL;
    HANDLE hPipeWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0)) {
        if (outError) swprintf_s(outError, errLen, L"Не удалось создать pipe для валидации архива.");
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
        if (outError) swprintf_s(outError, errLen, L"Не удалось запустить tar.exe для чтения архива.");
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
        if (outError) swprintf_s(outError, errLen, L"Архив повреждён или не является корректным zip (код tar: %lu).", exitCode);
        return false;
    }

    // Parse lines from output (UTF-8 / ASCII)
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find_first_of("\r\n", start);
        if (end == std::string::npos) end = output.size();

        std::string rawLine = output.substr(start, end - start);
        start = end + 1;
        while (start < output.size() && (output[start] == '\r' || output[start] == '\n')) start++;

        // Trim whitespace
        while (!rawLine.empty() && (rawLine.back() == ' ' || rawLine.back() == '\t')) rawLine.pop_back();
        size_t lstart = 0;
        while (lstart < rawLine.size() && (rawLine[lstart] == ' ' || rawLine[lstart] == '\t')) lstart++;
        if (lstart > 0) rawLine = rawLine.substr(lstart);

        if (rawLine.empty()) continue;

        // Convert to wide string
        int wlen = MultiByteToWideChar(CP_UTF8, 0, rawLine.c_str(), (int)rawLine.size(), nullptr, 0);
        if (wlen <= 0) continue;
        std::wstring wline(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, rawLine.c_str(), (int)rawLine.size(), &wline[0], wlen);

        // Directories ending with / or \ can be skipped in listing validation, but their path segments must still be valid
        bool isDir = (!wline.empty() && (wline.back() == L'/' || wline.back() == L'\\'));
        if (isDir) {
            wline.pop_back();
            if (wline.empty()) continue;
        }

        // Validate via ResolveInsideRoot
        wchar_t canon[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, wline.c_str(), canon, MAX_PATH, &pErr)) {
            if (outError) {
                swprintf_s(outError, errLen, L"Zip-slip или опасный путь в архиве: %ls (ошибка %d)", wline.c_str(), (int)pErr);
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
        if (outError) swprintf_s(outError, errLen, L"Системная утилита tar.exe не найдена.");
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
        if (outError) swprintf_s(outError, errLen, L"Не удалось запустить процесс распаковки.");
        return false;
    }

    WaitForSingleObject(pi.hProcess, 120000); // 2 minutes max
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != 0) {
        if (outError) swprintf_s(outError, errLen, L"Ошибка распаковки архива tar.exe (код: %lu).", exitCode);
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
            if (outError) swprintf_s(outError, errLen, L"Обнаружен reparse point (symlink/junction) в stage: %ls", fd.cFileName);
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
                if (outError) swprintf_s(outError, errLen, L"Превышен лимит количества файлов в архиве (> %zu).", maxFiles);
                FindClose(hFind);
                return false;
            }

            ULARGE_INTEGER fileSize;
            fileSize.LowPart = fd.nFileSizeLow;
            fileSize.HighPart = fd.nFileSizeHigh;
            totalBytes += fileSize.QuadPart;

            if (totalBytes > maxBytes) {
                if (outError) swprintf_s(outError, errLen, L"Превышен лимит распакованного размера (> %llu байт).", maxBytes);
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
    wchar_t searchPattern[MAX_PATH];
    swprintf_s(searchPattern, L"%s\\*.*", dirPath);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    size_t baseLen = wcslen(baseStageDir);

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t fullPath[MAX_PATH];
        swprintf_s(fullPath, L"%s\\%s", dirPath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFilesRecursive(fullPath, baseStageDir, outRelFiles);
        } else {
            const wchar_t* rel = fullPath + baseLen;
            while (*rel == L'\\' || *rel == L'/') rel++;
            outRelFiles.push_back(rel);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

bool HasPendingJournal(const wchar_t* gameRoot) {
    wchar_t journalPath[MAX_PATH];
    swprintf_s(journalPath, L"%s\\.updaterbackups\\pending.journal", gameRoot);
    DWORD attrs = GetFileAttributesW(journalPath);
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
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

    wchar_t backupBase[MAX_PATH];
    swprintf_s(backupBase, L"%s\\.updaterbackups", gameRoot);
    CreateDirectoryW(backupBase, nullptr);

    wchar_t cleanVer[64];
    wcsncpy_s(cleanVer, (fromVersion && fromVersion[0]) ? fromVersion : L"prev", 63);
    for (wchar_t* c = cleanVer; *c; c++) {
        if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
    }

    wchar_t backupVerDir[MAX_PATH];
    swprintf_s(backupVerDir, L"%s\\%s", backupBase, cleanVer);
    CreateDirectoryW(backupVerDir, nullptr);

    wchar_t backupFilesDir[MAX_PATH];
    swprintf_s(backupFilesDir, L"%s\\files", backupVerDir);
    CreateDirectoryW(backupFilesDir, nullptr);

    wchar_t journalPath[MAX_PATH];
    swprintf_s(journalPath, L"%s\\pending.journal", backupBase);

    // 1. Collect all files from stageDir
    std::vector<std::wstring> stageFiles;
    CollectFilesRecursive(stageDir, stageDir, stageFiles);

    // 2. Validate paths and backup existing files
    std::vector<std::wstring> backedUpRelFiles;
    for (const auto& rel : stageFiles) {
        wchar_t canonDst[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, rel.c_str(), canonDst, MAX_PATH, &pErr)) {
            if (outResult) swprintf_s(outResult->errorMessage, L"Недопустимый путь назначения: %ls", rel.c_str());
            return false;
        }

        DWORD attrs = GetFileAttributesW(canonDst);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (outResult) swprintf_s(outResult->errorMessage, L"Целевой файл является reparse point: %ls", rel.c_str());
                return false;
            }

            wchar_t backupDst[MAX_PATH];
            swprintf_s(backupDst, L"%s\\%ls", backupFilesDir, rel.c_str());
            EnsureParentDirectoryExists(backupDst);

            if (!CopyFileW(canonDst, backupDst, FALSE)) {
                if (outResult) swprintf_s(outResult->errorMessage, L"Не удалось создать резервную копию: %ls", rel.c_str());
                return false;
            }
            backedUpRelFiles.push_back(rel);
        }
    }

    // 3. Write initial pending.journal
    FILE* jf = nullptr;
    _wfopen_s(&jf, journalPath, L"w, ccs=UTF-8");
    if (!jf) {
        if (outResult) swprintf_s(outResult->errorMessage, L"Не удалось открыть журнал pending.journal для записи.");
        return false;
    }
    fwprintf(jf, L"[JOURNAL_V1]\n");
    fwprintf(jf, L"STATUS=APPLYING\n");
    fwprintf(jf, L"FROM_VERSION=%ls\n", fromVersion ? fromVersion : L"0.0.0");
    fwprintf(jf, L"TO_VERSION=%ls\n", toVersion ? toVersion : L"unknown");
    fwprintf(jf, L"BACKUP_DIR=%ls\n", backupVerDir);
    fwprintf(jf, L"STAGE_DIR=%ls\n", stageDir);
    fflush(jf);

    // 4. Copy files from stage to gameRoot
    std::vector<std::wstring> successfullyCopied;
    bool failed = false;

    for (int i = 0; i < (int)stageFiles.size(); i++) {
        const auto& rel = stageFiles[i];

        // Testhook injection
#ifdef TESTHOOKS
        if (failAtFileIndex >= 0 && i == failAtFileIndex) {
            failed = true;
            if (outResult) swprintf_s(outResult->errorMessage, L"Имитация сбоя на файле #%d (testhook --fail-at).", i);
            break;
        }
#else
        (void)failAtFileIndex;
#endif

        wchar_t srcPath[MAX_PATH];
        swprintf_s(srcPath, L"%s\\%ls", stageDir, rel.c_str());

        wchar_t dstPath[MAX_PATH];
        swprintf_s(dstPath, L"%s\\%ls", gameRoot, rel.c_str());
        EnsureParentDirectoryExists(dstPath);

        // Special handling for updater.exe self-update
        if (_wcsicmp(rel.c_str(), L"updater.exe") == 0) {
            wchar_t oldPath[MAX_PATH];
            swprintf_s(oldPath, L"%s\\updater.exe.old", gameRoot);
            DeleteFileW(oldPath);
            MoveFileExW(dstPath, oldPath, MOVEFILE_REPLACE_EXISTING);
        }

        if (!CopyFileW(srcPath, dstPath, FALSE)) {
            failed = true;
            if (outResult) swprintf_s(outResult->errorMessage, L"Ошибка копирования файла: %ls (GetLastError: %lu)", rel.c_str(), GetLastError());
            break;
        }

        successfullyCopied.push_back(rel);
        fwprintf(jf, L"COPIED=%ls\n", rel.c_str());
        fflush(jf);
    }

    if (failed) {
        // Rollback transaction!
        fwprintf(jf, L"STATUS=ROLLING_BACK\n");
        fflush(jf);

        int rollbackCount = 0;
        for (const auto& rel : successfullyCopied) {
            wchar_t dstPath[MAX_PATH];
            swprintf_s(dstPath, L"%s\\%ls", gameRoot, rel.c_str());

            wchar_t backupSrc[MAX_PATH];
            swprintf_s(backupSrc, L"%s\\%ls", backupFilesDir, rel.c_str());

            DWORD attrs = GetFileAttributesW(backupSrc);
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                // Was overwritten: restore from backup
                CopyFileW(backupSrc, dstPath, FALSE);
            } else {
                // Was newly created: delete
                DeleteFileW(dstPath);
            }
            rollbackCount++;
        }

        // If updater.exe.old exists, restore it back
        wchar_t oldPath[MAX_PATH];
        swprintf_s(oldPath, L"%s\\updater.exe.old", gameRoot);
        wchar_t livePath[MAX_PATH];
        swprintf_s(livePath, L"%s\\updater.exe", gameRoot);
        if (GetFileAttributesW(oldPath) != INVALID_FILE_ATTRIBUTES) {
            MoveFileExW(oldPath, livePath, MOVEFILE_REPLACE_EXISTING);
        }

        if (outResult) {
            outResult->success = false;
            outResult->filesCopied = (int)successfullyCopied.size();
            outResult->filesRolledBack = rollbackCount;
        }
        fclose(jf);
        DeleteFileW(journalPath);
        return false;
    }

    // Success: close and remove journal
    fwprintf(jf, L"STATUS=COMPLETED\n");
    fclose(jf);
    DeleteFileW(journalPath);

    // Process delete.txt from stageDir safely
    ProcessDeletions(gameRoot, stageDir, fromVersion, outResult);

    if (outResult) {
        outResult->success = true;
        outResult->filesCopied = (int)successfullyCopied.size();
        outResult->filesRolledBack = 0;
    }

    return true;
}

bool ProcessDeletions(const wchar_t* gameRoot,
                      const wchar_t* stageDir,
                      const wchar_t* fromVersion,
                      UpdateTransactionResult* outResult) {
    if (outResult) {
        outResult->success = true;
    }

    wchar_t delPath[MAX_PATH];
    swprintf_s(delPath, L"%s\\delete.txt", stageDir);

    DWORD delAttrs = GetFileAttributesW(delPath);
    if (delAttrs == INVALID_FILE_ATTRIBUTES || (delAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, delPath, L"r, ccs=UTF-8");
    if (!f) {
        _wfopen_s(&f, delPath, L"r");
        if (!f) return true;
    }

    wchar_t backupBase[MAX_PATH];
    swprintf_s(backupBase, L"%s\\.updaterbackups", gameRoot);
    CreateDirectoryW(backupBase, nullptr);

    wchar_t cleanVer[64];
    wcsncpy_s(cleanVer, (fromVersion && fromVersion[0]) ? fromVersion : L"prev", 63);
    for (wchar_t* c = cleanVer; *c; c++) {
        if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
    }

    wchar_t backupVerDir[MAX_PATH];
    swprintf_s(backupVerDir, L"%s\\%s", backupBase, cleanVer);
    CreateDirectoryW(backupVerDir, nullptr);

    wchar_t backupDeletedDir[MAX_PATH];
    swprintf_s(backupDeletedDir, L"%s\\deleted", backupVerDir);
    CreateDirectoryW(backupDeletedDir, nullptr);

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

        // Normalize slashes
        for (wchar_t* c = p; *c; c++) {
            if (*c == L'/') *c = L'\\';
        }

        // 1. ResolveInsideRoot: rejects .., absolute paths, ADS, DOS devices
        wchar_t canonDst[MAX_PATH];
        PathError pErr = PathError::Ok;
        if (!ResolveInsideRoot(gameRoot, p, canonDst, MAX_PATH, &pErr)) {
            continue;
        }

        // 2. IsProtectedPath: rejects gta-vc.exe, updater.exe, UpdateChecker.asi, dinput8.dll, system dirs
        if (IsProtectedPath(gameRoot, p)) {
            continue;
        }

        // 3. Inspect target file
        DWORD attrs = GetFileAttributesW(canonDst);
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;

        // 4. Refuse to delete directories
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        // 5. Refuse to follow reparse points
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }

        // 6. Backup before deleting into .updaterbackups\<fromVer>\deleted\<relPath>
        wchar_t backupDst[MAX_PATH];
        swprintf_s(backupDst, L"%s\\%ls", backupDeletedDir, p);
        EnsureParentDirectoryExists(backupDst);

        if (!MoveFileExW(canonDst, backupDst, MOVEFILE_REPLACE_EXISTING)) {
            CopyFileW(canonDst, backupDst, FALSE);
            DeleteFileW(canonDst);
        }
    }

    fclose(f);
    DeleteFileW(delPath);
    return true;
}

bool RestoreDeletedBackups(const wchar_t* gameRoot, const wchar_t* fromVersion) {
    wchar_t cleanVer[64];
    wcsncpy_s(cleanVer, (fromVersion && fromVersion[0]) ? fromVersion : L"prev", 63);
    for (wchar_t* c = cleanVer; *c; c++) {
        if (*c == L':' || *c == L'/' || *c == L'\\') *c = L'_';
    }

    wchar_t backupDeletedDir[MAX_PATH];
    swprintf_s(backupDeletedDir, L"%s\\.updaterbackups\\%s\\deleted", gameRoot, cleanVer);

    DWORD attrs = GetFileAttributesW(backupDeletedDir);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    std::vector<std::wstring> delFiles;
    CollectFilesRecursive(backupDeletedDir, backupDeletedDir, delFiles);
    for (const auto& rel : delFiles) {
        wchar_t srcPath[MAX_PATH];
        swprintf_s(srcPath, L"%s\\%ls", backupDeletedDir, rel.c_str());

        wchar_t dstPath[MAX_PATH];
        swprintf_s(dstPath, L"%s\\%ls", gameRoot, rel.c_str());
        EnsureParentDirectoryExists(dstPath);

        MoveFileExW(srcPath, dstPath, MOVEFILE_REPLACE_EXISTING);
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

    wchar_t journalPath[MAX_PATH];
    swprintf_s(journalPath, L"%s\\.updaterbackups\\pending.journal", gameRoot);

    FILE* jf = nullptr;
    _wfopen_s(&jf, journalPath, L"r, ccs=UTF-8");
    if (!jf) return false;

    wchar_t line[1024];
    wchar_t backupDir[MAX_PATH] = { 0 };
    std::vector<std::wstring> copiedFiles;

    while (fgetws(line, 1024, jf)) {
        wchar_t* nl = wcspbrk(line, L"\r\n");
        if (nl) *nl = L'\0';

        if (wcsncmp(line, L"BACKUP_DIR=", 11) == 0) {
            wcsncpy_s(backupDir, line + 11, MAX_PATH - 1);
        } else if (wcsncmp(line, L"COPIED=", 7) == 0) {
            copiedFiles.push_back(line + 7);
        }
    }
    fclose(jf);

    if (backupDir[0] == L'\0') {
        DeleteFileW(journalPath);
        return false;
    }

    wchar_t backupFilesDir[MAX_PATH];
    swprintf_s(backupFilesDir, L"%s\\files", backupDir);

    int restored = 0;
    for (const auto& rel : copiedFiles) {
        wchar_t dstPath[MAX_PATH];
        swprintf_s(dstPath, L"%s\\%ls", gameRoot, rel.c_str());

        wchar_t backupSrc[MAX_PATH];
        swprintf_s(backupSrc, L"%s\\%ls", backupFilesDir, rel.c_str());

        DWORD attrs = GetFileAttributesW(backupSrc);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            CopyFileW(backupSrc, dstPath, FALSE);
        } else {
            DeleteFileW(dstPath);
        }
        restored++;
    }

    // Also restore deleted files if any were moved
    wchar_t backupDeletedDir[MAX_PATH];
    swprintf_s(backupDeletedDir, L"%s\\deleted", backupDir);
    DWORD delAttrs = GetFileAttributesW(backupDeletedDir);
    if (delAttrs != INVALID_FILE_ATTRIBUTES && (delAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        std::vector<std::wstring> delFiles;
        CollectFilesRecursive(backupDeletedDir, backupDeletedDir, delFiles);
        for (const auto& rel : delFiles) {
            wchar_t delSrc[MAX_PATH];
            swprintf_s(delSrc, L"%s\\%ls", backupDeletedDir, rel.c_str());

            wchar_t dstPath[MAX_PATH];
            swprintf_s(dstPath, L"%s\\%ls", gameRoot, rel.c_str());
            EnsureParentDirectoryExists(dstPath);

            MoveFileExW(delSrc, dstPath, MOVEFILE_REPLACE_EXISTING);
            restored++;
        }
    }

    DeleteFileW(journalPath);

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

    // Sort ascending (oldest first)
    std::sort(entries.begin(), entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
        return CompareFileTime(&a.ft, &b.ft) < 0;
    });

    int toDelete = (int)entries.size() - maxSlots;
    for (int i = 0; i < toDelete; i++) {
        SafeDeleteDirectoryRecursive(entries[i].path.c_str());
    }
}
