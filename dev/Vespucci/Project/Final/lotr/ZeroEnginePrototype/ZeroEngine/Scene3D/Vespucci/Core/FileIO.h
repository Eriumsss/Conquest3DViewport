// FileIO.h
// =============================================================================
// FILEIO — BINARY BLOB READING + WRITING + MMAP. CORPUS PATH IS HOT.
// =============================================================================
// Written by: Eriumsss
//
// Corpus files run 5-50 MB on big maps. Loading them via fread + alloc
// is fine on cold-start but the editor's "switch level" path wants to
// drop the old corpus and bring in a new one without a multi-second
// stall. Memory-mapped I/O fixes that: the OS pages bytes in lazily,
// the corpus reader walks the mmap'd region as if it were already
// in RAM, and the page cache amortizes across runs.
//
// Two paths:
//   - Blob: read entire file into a heap-allocated u8 buffer the
//     caller owns. Use for small inputs (compat rules, weights JSON).
//   - Map:  mmap the file read-only via CreateFileMapping. Use for
//     corpus files. Caller calls Unmap when done.
//
// Write path is fwrite-and-rename for atomic durability: write to
// "${path}.tmp", flush, close, rename to "${path}". This way a power
// loss or BSOD mid-write leaves the OLD file intact, never half-
// written corpus. Rename is atomic on NTFS.
// =============================================================================

#ifndef VESPUCCI_CORE_FILEIO_H_
#define VESPUCCI_CORE_FILEIO_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {

// Heap-owned binary blob. Caller frees via FreeBlob().
struct Blob {
    u8*  data;
    i64  size;
};

// Read whole file into a heap blob. blob.data is NULL on error.
Blob ReadBlob(const char* path);
void FreeBlob(Blob& blob);

// Memory-mapped file. View is read-only. Caller MUST call Unmap.
struct Mapped {
    const u8* data;
    i64       size;
    void*     hFile;     // platform handle (HANDLE on win32)
    void*     hMapping;  // platform handle
};

bool MapReadOnly(const char* path, Mapped& outMap);
void Unmap(Mapped& m);

// Atomic write: temp file + rename. Returns true on success.
bool WriteAtomic(const char* path, const void* data, i64 size);

// Append to file (created if missing). For log-style writers.
bool AppendBlob(const char* path, const void* data, i64 size);

// Quick predicates / metadata.
bool ExistsAndNonEmpty(const char* path);
i64  GetSizeBytes(const char* path);
u64  GetMTimeUnix(const char* path);  // 0 on error

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_FILEIO_H_
