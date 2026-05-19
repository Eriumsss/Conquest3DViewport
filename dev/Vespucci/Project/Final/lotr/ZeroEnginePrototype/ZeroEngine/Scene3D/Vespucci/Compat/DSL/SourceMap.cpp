// SourceMap.cpp
// =============================================================================
// Build line-start table by scanning for newlines once. Bin-search
// converts offset to line; column is offset minus that line's start.
// =============================================================================
// Written by: Eriumsss

#include "SourceMap.h"

namespace Vespucci {
namespace Compat {
namespace DSL {

SourceMap::SourceMap() : m_sourceLen(0) {}

void SourceMap::build(const char* src, usize n) {
    m_lineStarts.clear();
    m_sourceLen = (u32)n;
    if (!src || n == 0) {
        m_lineStarts.push_back(0);
        return;
    }
    m_lineStarts.reserve(n / 32 + 8);
    m_lineStarts.push_back(0);
    for (usize i = 0; i < n; ++i) {
        if (src[i] == '\n' && (i + 1) < n) {
            m_lineStarts.push_back((u32)(i + 1));
        }
    }
}

void SourceMap::offsetToLineCol(u32 offset, i32& outLine, i32& outCol) const {
    if (m_lineStarts.empty()) {
        outLine = 1; outCol = 1; return;
    }
    if (offset > m_sourceLen) offset = m_sourceLen;

    // Binary search for the largest lineStart <= offset.
    i32 lo = 0;
    i32 hi = (i32)m_lineStarts.size() - 1;
    i32 best = 0;
    while (lo <= hi) {
        i32 mid = (lo + hi) >> 1;
        if (m_lineStarts[mid] <= offset) { best = mid; lo = mid + 1; }
        else                              { hi = mid - 1; }
    }
    outLine = best + 1;
    outCol  = (i32)(offset - m_lineStarts[best]) + 1;
}

i32 SourceMap::lineColToOffset(i32 line, i32 col) const {
    if (line < 1 || line > (i32)m_lineStarts.size()) return -1;
    u32 off = m_lineStarts[line - 1] + (u32)(col > 0 ? col - 1 : 0);
    if (off > m_sourceLen) return -1;
    return (i32)off;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
