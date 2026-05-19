// FileIO.cpp
// =============================================================================
// Win32 binary I/O. Atomic write via temp+rename. mmap via CreateFileMapping.
// Failure paths log Warn and return clean - we do NOT crash the editor
// because a corpus file went missing; we just suggest worse for a frame
// while the user figures out where they put it.
// =============================================================================
// Written by: Eriumsss

#include "FileIO.h"
#include "PathUtils.h"
#include "Logging.h"
#include "VespucciAssert.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace Vespucci {
namespace Core {

Blob ReadBlob(const char* path) {
    Blob result; result.data = 0; result.size = 0;
    if (!path || !*path) return result;

    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        Logging::Warn("FileIO: ReadBlob open failed: %s", path);
        return result;
    }
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    if (sz < 0) {
        std::fclose(fp);
        Logging::Warn("FileIO: ftell failed: %s", path);
        return result;
    }
    std::fseek(fp, 0, SEEK_SET);

    u8* buf = (u8*)std::malloc((size_t)sz);
    if (!buf) {
        std::fclose(fp);
        Logging::Error("FileIO: blob malloc %ld failed: %s", sz, path);
        return result;
    }
    if (sz > 0) {
        size_t got = std::fread(buf, 1, (size_t)sz, fp);
        if (got != (size_t)sz) {
            std::free(buf);
            std::fclose(fp);
            Logging::Warn("FileIO: short read %zu/%ld: %s", got, sz, path);
            return result;
        }
    }
    std::fclose(fp);
    result.data = buf;
    result.size = (i64)sz;
    return result;
}

void FreeBlob(Blob& blob) {
    if (blob.data) std::free(blob.data);
    blob.data = 0;
    blob.size = 0;
}

bool MapReadOnly(const char* path, Mapped& outMap) {
    outMap.data = 0;
    outMap.size = 0;
    outMap.hFile = 0;
    outMap.hMapping = 0;

#if defined(_WIN32)
    if (!path || !*path) return false;

    HANDLE hFile = CreateFileA(path,
        GENERIC_READ, FILE_SHARE_READ, 0,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hFile == INVALID_HANDLE_VALUE) {
        Logging::Warn("FileIO: MapReadOnly open failed: %s (err=%lu)",
            path, (unsigned long)GetLastError());
        return false;
    }

    LARGE_INTEGER li;
    if (!GetFileSizeEx(hFile, &li)) {
        CloseHandle(hFile);
        Logging::Warn("FileIO: GetFileSizeEx failed: %s", path);
        return false;
    }
    if (li.QuadPart == 0) {
        // Empty file: cannot map a 0-byte mapping. Return success
        // with zero size and no handles.
        CloseHandle(hFile);
        outMap.size = 0;
        return true;
    }

    HANDLE hMap = CreateFileMappingA(hFile, 0, PAGE_READONLY,
        li.HighPart, li.LowPart, 0);
    if (!hMap) {
        CloseHandle(hFile);
        Logging::Warn("FileIO: CreateFileMapping failed: %s (err=%lu)",
            path, (unsigned long)GetLastError());
        return false;
    }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        Logging::Warn("FileIO: MapViewOfFile failed: %s (err=%lu)",
            path, (unsigned long)GetLastError());
        return false;
    }

    outMap.data     = (const u8*)view;
    outMap.size     = (i64)li.QuadPart;
    outMap.hFile    = hFile;
    outMap.hMapping = hMap;
    return true;
#else
    return false;
#endif
}

void Unmap(Mapped& m) {
#if defined(_WIN32)
    if (m.data) UnmapViewOfFile((LPCVOID)m.data);
    if (m.hMapping) CloseHandle((HANDLE)m.hMapping);
    if (m.hFile) CloseHandle((HANDLE)m.hFile);
#endif
    m.data = 0; m.size = 0; m.hFile = 0; m.hMapping = 0;
}

bool WriteAtomic(const char* path, const void* data, i64 size) {
    if (!path || !*path) return false;

    // Compose temp path: ${path}.tmp. We do not use a unique-id
    // suffix because the only consumer is single-threaded and a
    // racing second writer is a programmer bug we want to surface.
    std::string tmp = path;
    tmp += ".tmp";

    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) {
        Logging::Warn("FileIO: WriteAtomic open temp failed: %s", tmp.c_str());
        return false;
    }
    if (size > 0 && data) {
        size_t put = std::fwrite(data, 1, (size_t)size, fp);
        if (put != (size_t)size) {
            std::fclose(fp);
            std::remove(tmp.c_str());
            Logging::Error("FileIO: WriteAtomic short write %zu/%lld: %s",
                put, (long long)size, tmp.c_str());
            return false;
        }
    }
    std::fflush(fp);
    std::fclose(fp);

#if defined(_WIN32)
    // MoveFileExA with MOVEFILE_REPLACE_EXISTING is atomic on NTFS.
    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING)) {
        Logging::Error("FileIO: WriteAtomic rename failed: %s -> %s (err=%lu)",
            tmp.c_str(), path, (unsigned long)GetLastError());
        std::remove(tmp.c_str());
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
#endif
    return true;
}

bool AppendBlob(const char* path, const void* data, i64 size) {
    if (!path || !*path) return false;
    FILE* fp = std::fopen(path, "ab");
    if (!fp) return false;
    if (size > 0 && data) {
        size_t put = std::fwrite(data, 1, (size_t)size, fp);
        if (put != (size_t)size) { std::fclose(fp); return false; }
    }
    std::fflush(fp);
    std::fclose(fp);
    return true;
}

bool ExistsAndNonEmpty(const char* path) {
    return GetSizeBytes(path) > 0;
}

i64 GetSizeBytes(const char* path) {
    return Path::GetSize(path);
}

u64 GetMTimeUnix(const char* path) {
#if defined(_WIN32)
    if (!path || !*path) return 0;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    // Convert FILETIME (100ns since 1601) to Unix seconds.
    ULARGE_INTEGER u;
    u.LowPart  = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    static const u64 kEpochDelta = 116444736000000000ull;
    if (u.QuadPart < kEpochDelta) return 0;
    return (u64)((u.QuadPart - kEpochDelta) / 10000000ull);
#else
    return 0;
#endif
}

} // namespace Core
} // namespace Vespucci
