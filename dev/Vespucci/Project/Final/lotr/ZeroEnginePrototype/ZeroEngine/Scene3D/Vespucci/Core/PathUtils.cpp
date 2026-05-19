// PathUtils.cpp
// =============================================================================
// Win32-only path manipulation. ANSI in, ANSI out. We ducked the wide-
// char nightmare on purpose - level files are ANSI, the editor is
// English-only for now, and if we ever ship Unicode paths the fix is
// to swap MultiByteToWideChar in three places, not rewrite this file.
// =============================================================================
// Written by: Eriumsss

#include "PathUtils.h"
#include "VespucciAssert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <sys/stat.h>
    #include <direct.h>
#endif

namespace Vespucci {
namespace Core {
namespace Path {

namespace {
    inline bool IsSep(char c) { return c == '\\' || c == '/'; }

    // Mutate slashes in-place to backslash.
    void NormalizeSlashes(std::string& s) {
        for (usize i = 0; i < s.size(); ++i) {
            if (s[i] == '/') s[i] = '\\';
        }
    }

    // Strip duplicate adjacent separators.
    void CollapseSlashes(std::string& s) {
        std::string out;
        out.reserve(s.size());
        bool prevSep = false;
        for (usize i = 0; i < s.size(); ++i) {
            char c = s[i];
            bool sep = (c == '\\');
            // Allow duplicate at start (UNC \\server\...) - keep first two.
            if (sep && prevSep && i >= 2) continue;
            out.push_back(c);
            prevSep = sep;
        }
        s.swap(out);
    }

    // Strip trailing slashes (except for root like "C:\").
    void StripTrailingSlashes(std::string& s) {
        while (s.size() > 1 && IsSep(s[s.size() - 1])) {
            // Don't strip the slash after a drive letter ("C:\").
            if (s.size() == 3 && s[1] == ':') break;
            s.pop_back();
        }
    }

    void StripLeadingSlashes(std::string& s) {
        usize i = 0;
        while (i < s.size() && IsSep(s[i])) i++;
        if (i > 0) s.erase(0, i);
    }
} // namespace

std::string Join(const char* a, const char* b) {
    if (!a || !*a) return b ? std::string(b) : std::string();
    if (!b || !*b) return std::string(a);
    std::string left  = a;
    std::string right = b;
    NormalizeSlashes(left);
    NormalizeSlashes(right);
    StripTrailingSlashes(left);
    StripLeadingSlashes(right);
    if (left.empty())  return right;
    if (right.empty()) return left;
    return left + "\\" + right;
}

std::string Join(const std::string& a, const std::string& b) {
    return Join(a.c_str(), b.c_str());
}

std::string Normalize(const char* p) {
    if (!p || !*p) return std::string();
    std::string s = p;
    NormalizeSlashes(s);
    CollapseSlashes(s);

    // Resolve "." and ".." lexically. Split, walk, join.
    std::vector<std::string> parts;
    parts.reserve(16);
    usize start = 0;
    for (usize i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\\') {
            if (i > start) {
                std::string seg = s.substr(start, i - start);
                if (seg == ".") {
                    // skip
                } else if (seg == "..") {
                    if (!parts.empty() && parts.back() != "..") {
                        parts.pop_back();
                    } else {
                        parts.push_back(seg);
                    }
                } else {
                    parts.push_back(seg);
                }
            } else if (i == 0) {
                // leading slash — preserve as empty leading segment so
                // the join keeps the root prefix.
                parts.push_back("");
            }
            start = i + 1;
        }
    }

    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i > 0) out += "\\";
        out += parts[i];
    }
    if (out.empty()) out = ".";
    return out;
}

std::string Basename(const char* p) {
    if (!p || !*p) return std::string();
    const char* last = p;
    for (const char* q = p; *q; ++q) {
        if (IsSep(*q)) last = q + 1;
    }
    return std::string(last);
}

std::string Dirname(const char* p) {
    if (!p || !*p) return std::string();
    const char* lastSep = 0;
    for (const char* q = p; *q; ++q) {
        if (IsSep(*q)) lastSep = q;
    }
    if (!lastSep) return std::string();
    return std::string(p, (size_t)(lastSep - p));
}

std::string Extension(const char* p) {
    if (!p || !*p) return std::string();
    std::string base = Basename(p);
    usize dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return std::string();
    return base.substr(dot);
}

std::string StripExtension(const char* p) {
    if (!p || !*p) return std::string();
    std::string s = p;
    std::string base = Basename(p);
    usize dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return s;
    usize total = s.size() - (base.size() - dot);
    return s.substr(0, total);
}

std::string ReplaceExtension(const char* p, const char* newExt) {
    std::string stripped = StripExtension(p);
    if (!newExt || !*newExt) return stripped;
    if (newExt[0] != '.') stripped += ".";
    stripped += newExt;
    return stripped;
}

bool Exists(const char* p) {
#if defined(_WIN32)
    if (!p || !*p) return false;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES;
#else
    return false;
#endif
}

bool IsFile(const char* p) {
#if defined(_WIN32)
    if (!p || !*p) return false;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

bool IsDir(const char* p) {
#if defined(_WIN32)
    if (!p || !*p) return false;
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    return false;
#endif
}

bool Mkdir(const char* p) {
#if defined(_WIN32)
    if (!p || !*p) return false;
    if (CreateDirectoryA(p, 0)) return true;
    DWORD err = GetLastError();
    return err == ERROR_ALREADY_EXISTS;
#else
    return false;
#endif
}

bool MkdirRecursive(const char* p) {
    if (!p || !*p) return false;
    if (Exists(p)) return IsDir(p);
    std::string parent = Dirname(p);
    if (!parent.empty() && !Exists(parent.c_str())) {
        if (!MkdirRecursive(parent.c_str())) return false;
    }
    return Mkdir(p);
}

bool ReadFile(const char* p, std::string& outContents) {
    outContents.clear();
    if (!p || !*p) return false;
    FILE* fp = std::fopen(p, "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    if (sz < 0) { std::fclose(fp); return false; }
    std::fseek(fp, 0, SEEK_SET);
    outContents.resize((size_t)sz);
    if (sz > 0) {
        size_t got = std::fread(&outContents[0], 1, (size_t)sz, fp);
        if (got != (size_t)sz) {
            std::fclose(fp);
            return false;
        }
    }
    std::fclose(fp);
    return true;
}

bool WriteFile(const char* p, const char* data, usize n) {
    if (!p || !*p) return false;
    FILE* fp = std::fopen(p, "wb");
    if (!fp) return false;
    if (n > 0) {
        size_t put = std::fwrite(data, 1, n, fp);
        if (put != n) { std::fclose(fp); return false; }
    }
    std::fclose(fp);
    return true;
}

i64 GetSize(const char* p) {
#if defined(_WIN32)
    if (!p || !*p) return -1;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &d)) return -1;
    LARGE_INTEGER li;
    li.HighPart = (LONG)d.nFileSizeHigh;
    li.LowPart  = d.nFileSizeLow;
    return (i64)li.QuadPart;
#else
    return -1;
#endif
}

} // namespace Path
} // namespace Core
} // namespace Vespucci
