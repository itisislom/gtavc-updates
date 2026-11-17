#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

enum class PathError {
    Ok = 0,
    NullOrEmpty,
    TraversalDetected,
    AbsolutePath,
    AlternateDataStream,
    ReservedDeviceName,
    InvalidSegmentEnding,
    ControlCharacters,
    ReparsePoint,
    PathTooLong,
    OutsideRoot
};

const char* PathErrorToString(PathError err);

bool ResolveInsideRoot(
    const wchar_t* root,
    const wchar_t* rel,
    wchar_t* out,
    size_t cap,
    PathError* err = nullptr
);

bool IsProtectedPath(
    const wchar_t* root,
    const wchar_t* fullOrRelPath
);

bool HasReparsePointInChain(const wchar_t* path);
