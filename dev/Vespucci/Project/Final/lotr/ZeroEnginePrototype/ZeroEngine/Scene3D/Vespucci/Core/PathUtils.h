// PathUtils.h
// =============================================================================
// PATHUTILS — WINDOWS PATH MANIPULATION WITHOUT THE WIN32 RABBIT HOLE
// =============================================================================
// Written by: Eriumsss
//
// We touch paths in maybe 12 places: corpus file load, golden test
// fixture lookup, replay record file, doc generator output, plugin
// directory scan. Each one needs Join, Normalize, Basename, Dirname,
// Extension, Exists. The Win32 PathCanonicalize / PathCombine
// functions exist but they pull shlwapi.lib and have edge-case bugs
// older than the engine itself, plus the EXE side is VS2005 with a
// half-broken shell SDK. So this is a small, predictable Win32-aware
// path module that does what we need and nothing else.
//
// All paths are stored as plain char* / std::string. We do not deal
// with UTF-16 here - level files came from ANSI, the editor's UI is
// rendered through ImGui which is UTF-8, and the std::filesystem
// wide-char story is a goddamn portability nightmare.
// =============================================================================

#ifndef VESPUCCI_CORE_PATHUTILS_H_
#define VESPUCCI_CORE_PATHUTILS_H_

#include "VespucciTypes.h"

#include <string>

namespace Vespucci {
namespace Core {
namespace Path {

// Join two path segments with a backslash. Strips trailing slashes
// from `a` and leading slashes from `b` so "C:/foo/" + "/bar" =
// "C:\foo\bar". Returns empty string if both inputs are empty.
std::string Join(const char* a, const char* b);
std::string Join(const std::string& a, const std::string& b);

// Normalize: convert forward slashes to backslashes, collapse
// duplicate separators, resolve "." and ".." segments where possible.
// Does NOT call the filesystem - this is purely lexical.
std::string Normalize(const char* p);

// Strip directory component, return last segment.
std::string Basename(const char* p);

// Strip last segment, return directory.
std::string Dirname(const char* p);

// Return file extension including leading dot ("foo.bar.json" -> ".json").
// Returns empty string if no extension.
std::string Extension(const char* p);

// Strip extension. ("foo.bar.json" -> "foo.bar").
std::string StripExtension(const char* p);

// Replace extension. ("foo.txt", ".json" -> "foo.json").
// New ext should include the leading dot or be empty.
std::string ReplaceExtension(const char* p, const char* newExt);

// Filesystem checks. These DO touch the disk. Keep callers aware.
bool Exists(const char* p);
bool IsFile(const char* p);
bool IsDir(const char* p);

// Create directory. Returns true on success or "already exists".
// Does NOT recursively create parents - use MkdirRecursive for that.
bool Mkdir(const char* p);

// Recursively create all missing parent directories.
bool MkdirRecursive(const char* p);

// Read file contents as a single string. Empty result on error
// (caller decides whether that's fatal).
bool ReadFile(const char* p, std::string& outContents);

// Write file (overwrite). True on success.
bool WriteFile(const char* p, const char* data, usize n);

// Get file size in bytes. Returns -1 on error.
i64 GetSize(const char* p);

} // namespace Path
} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_PATHUTILS_H_
