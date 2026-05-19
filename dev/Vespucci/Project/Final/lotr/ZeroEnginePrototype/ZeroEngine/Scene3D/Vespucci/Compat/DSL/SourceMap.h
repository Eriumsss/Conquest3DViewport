// SourceMap.h
// =============================================================================
// SOURCEMAP — BYTE OFFSET ↔ LINE/COLUMN LOOKUP FOR DIAGNOSTICS
// =============================================================================
// Written by: Eriumsss
//
// Diagnostics carry byte-offset spans, but humans read line:col. The
// SourceMap is built ONCE when the source is loaded — a flat array of
// line-start offsets — and then converted O(log N) per diagnostic.
//
// Memory cost: one u32 per line. The biggest LOTR:C compat rule files
// we are likely to see top out at maybe a thousand lines, so this is
// 4 KB tops. Worth not having to rescan the source for every error.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_SOURCEMAP_H_
#define VESPUCCI_COMPAT_DSL_SOURCEMAP_H_

#include "../../Core/VespucciTypes.h"

#include <vector>

namespace Vespucci {
namespace Compat {
namespace DSL {

class SourceMap {
public:
    SourceMap();

    // Build from a freshly-loaded source buffer. Replaces any prior content.
    void build(const char* src, usize n);

    // Convert byte offset to 1-indexed (line, column). Out-of-range
    // offsets clamp to the last line. Always returns a valid pair.
    void offsetToLineCol(u32 offset, i32& outLine, i32& outCol) const;

    // Reverse lookup. Returns -1 if line is out of range.
    i32 lineColToOffset(i32 line, i32 col) const;

    i32  lineCount() const { return (i32)m_lineStarts.size(); }

private:
    std::vector<u32> m_lineStarts;  // offset of first char of each line
    u32              m_sourceLen;
};

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_SOURCEMAP_H_
