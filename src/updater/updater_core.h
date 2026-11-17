#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

struct UpdateTransactionResult {
    bool success;
    int filesCopied;
    int filesRolledBack;
    wchar_t errorMessage[512];
};

// Generates a secure, random temporary directory path under %TEMP%
bool CreateSecureTempDir(wchar_t* outDir, size_t maxLen);

// Validates archive contents before extraction using System32\tar.exe -tf
bool ValidateZipContents(const wchar_t* zipPath,
                         const wchar_t* gameRoot,
                         std::vector<std::wstring>& outFiles,
                         wchar_t* outError,
                         size_t errLen);

// Extracts zip into staging directory
bool ExtractZipToStage(const wchar_t* zipPath,
                       const wchar_t* stageDir,
                       wchar_t* outError,
                       size_t errLen);

// Validates staging limits (max 20,000 files, max 2 GB, no reparse points)
bool ValidateStageDirectory(const wchar_t* stageDir,
                            size_t maxFiles,
                            unsigned long long maxBytes,
                            wchar_t* outError,
                            size_t errLen);

// Transactional apply of files from stageDir into gameRoot
bool ApplyUpdate(const wchar_t* gameRoot,
                 const wchar_t* stageDir,
                 const wchar_t* fromVersion,
                 const wchar_t* toVersion,
                 int failAtFileIndex,
                 UpdateTransactionResult* outResult);

// Crash recovery: restores files from backup recorded in pending.journal
bool RecoverPendingTransaction(const wchar_t* gameRoot,
                               UpdateTransactionResult* outResult);

// Checks if pending.journal exists
bool HasPendingJournal(const wchar_t* gameRoot);

// Processes delete.txt safely from staging directory
bool ProcessDeletions(const wchar_t* gameRoot,
                      const wchar_t* stageDir,
                      const wchar_t* fromVersion,
                      UpdateTransactionResult* outResult);

// Restores deleted files from .updaterbackups/<version>/deleted back to gameRoot (for rollback)
bool RestoreDeletedBackups(const wchar_t* gameRoot,
                           const wchar_t* fromVersion);

// Removes directory recursively with reparse point safety
void SafeDeleteDirectoryRecursive(const wchar_t* path);

// Prunes backups keeping latest maxSlots
void PruneOldBackups(const wchar_t* gameRoot, int maxSlots);
