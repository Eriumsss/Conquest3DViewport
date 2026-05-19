// StringRef.h
// =============================================================================
// STRINGREF — NON-OWNING STRING VIEW. THE OPPOSITE OF std::string.
// =============================================================================
// Written by: Eriumsss
//
// Vespucci passes thousands of string-shaped things per frame: entity
// names, type names, event names, action names, layer names, reason
// chip text. If every one of those was a std::string we would be
// running malloc/free at multi-kilohertz frequency and the editor
// would feel like swimming through wet concrete.
//
// std::string_view is C++17 and works fine for VS2022 DLL code, but
// the EXE side is fucking VS2005 with a half-broken C++ stdlib that
// pretends C++0x doesn't exist. We have no choice: roll our own.
// Same shape as std::string_view (ptr + length, non-owning), same
// invariants (the pointed-to bytes must outlive any StringRef
// referencing them).
//
// Lifetime contract: the caller is responsible for keeping the
// underlying buffer alive. In Vespucci practice, that means
// StringRef's mostly point at:
//   - The args struct's gameObj*Names arrays (owned by host, valid
//     until the next DrawFrame).
//   - Strings interned in StringPool (owned for the lifetime of
//     the snapshot, which in turn outlives any per-frame query).
//   - Static const char* literals (live forever).
// Pointing a StringRef at a stack-local std::string and then storing
// the StringRef beyond the std::string's scope is a use-after-free
// every fucking time. There's no way for us to catch that statically,
// so document the rule and pray.
//
// Includes a hash specialization so StringRef can be a map key
// without anyone reaching for std::string conversions.
// =============================================================================

#ifndef VESPUCCI_CORE_STRINGREF_H_
#define VESPUCCI_CORE_STRINGREF_H_

#include "VespucciTypes.h"
#include "Hash.h"

#include <cstring>
#include <functional>

namespace Vespucci {
namespace Core {

class StringRef {
public:
    StringRef() : m_ptr(""), m_len(0) {}
    StringRef(const char* s) : m_ptr(s ? s : ""), m_len(s ? std::strlen(s) : 0) {}
    StringRef(const char* s, usize n) : m_ptr(s ? s : ""), m_len(s ? n : 0) {}

    const char* data() const  { return m_ptr; }
    usize       size() const  { return m_len; }
    bool        empty() const { return m_len == 0; }

    // Bounded char access. No bounds check in release - hot path.
    // If you need bounds checks, the call site grew teeth, use them.
    char operator[](usize i) const { return m_ptr[i]; }

    // Comparison. Length-first short-circuits the bulk of the
    // hash-table-collision path where lengths usually differ.
    bool operator==(const StringRef& o) const {
        if (m_len != o.m_len) return false;
        return std::memcmp(m_ptr, o.m_ptr, m_len) == 0;
    }
    bool operator!=(const StringRef& o) const { return !(*this == o); }

    bool operator<(const StringRef& o) const {
        usize n = (m_len < o.m_len) ? m_len : o.m_len;
        int c = std::memcmp(m_ptr, o.m_ptr, n);
        if (c != 0) return c < 0;
        return m_len < o.m_len;
    }

    // Substring. Cheap because we don't copy bytes.
    StringRef substr(usize off, usize n) const {
        if (off >= m_len) return StringRef();
        usize avail = m_len - off;
        if (n > avail) n = avail;
        return StringRef(m_ptr + off, n);
    }

    // Prefix / suffix tests.
    bool startsWith(const StringRef& pre) const {
        if (pre.m_len > m_len) return false;
        return std::memcmp(m_ptr, pre.m_ptr, pre.m_len) == 0;
    }
    bool endsWith(const StringRef& suf) const {
        if (suf.m_len > m_len) return false;
        return std::memcmp(m_ptr + m_len - suf.m_len, suf.m_ptr, suf.m_len) == 0;
    }

    // Linear search for a char. Returns offset or -1.
    isize find(char c) const {
        for (usize i = 0; i < m_len; ++i) {
            if (m_ptr[i] == c) return (isize)i;
        }
        return -1;
    }

    // Trim helpers - return new StringRef, do not mutate. Cheap.
    StringRef trimLeft() const {
        usize i = 0;
        while (i < m_len && (m_ptr[i] == ' ' || m_ptr[i] == '\t' ||
                             m_ptr[i] == '\r' || m_ptr[i] == '\n')) i++;
        return StringRef(m_ptr + i, m_len - i);
    }
    StringRef trimRight() const {
        usize n = m_len;
        while (n > 0 && (m_ptr[n-1] == ' ' || m_ptr[n-1] == '\t' ||
                         m_ptr[n-1] == '\r' || m_ptr[n-1] == '\n')) n--;
        return StringRef(m_ptr, n);
    }
    StringRef trim() const { return trimLeft().trimRight(); }

    // Case-insensitive ASCII compare. Used by the autocomplete
    // fuzzy match and the "starts with" filter in the sidebar.
    // Locale-blind on purpose - level data is ASCII, this is not
    // the place to drag in <cctype>'s locale baggage.
    bool equalsIgnoreCase(const StringRef& o) const {
        if (m_len != o.m_len) return false;
        for (usize i = 0; i < m_len; ++i) {
            char a = m_ptr[i];
            char b = o.m_ptr[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return false;
        }
        return true;
    }

    bool startsWithIgnoreCase(const StringRef& pre) const {
        if (pre.m_len > m_len) return false;
        for (usize i = 0; i < pre.m_len; ++i) {
            char a = m_ptr[i];
            char b = pre.m_ptr[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) return false;
        }
        return true;
    }

private:
    const char* m_ptr;
    usize       m_len;
};

// Hash for use as map key. Salted with a session-stable constant.
struct StringRefHash {
    usize operator()(const StringRef& s) const {
        return (usize)XxHash32(s.data(), s.size(), 0xC1C2C3C4u);
    }
};

} // namespace Core
} // namespace Vespucci

namespace std {
    template<> struct hash<Vespucci::Core::StringRef> {
        std::size_t operator()(const Vespucci::Core::StringRef& s) const {
            return Vespucci::Core::StringRefHash()(s);
        }
    };
}

#endif // VESPUCCI_CORE_STRINGREF_H_
