// Diagnostics.cpp
// =============================================================================
// Render diagnostics with caret + surrounding line. The output is
// human-shaped on purpose — the alternative is "syntax error at offset
// 1287" and that is cowardly horseshit nobody can act on.
// =============================================================================
// Written by: Eriumsss

#include "Diagnostics.h"
#include "SourceMap.h"

#include "../../Core/Logging.h"
#include "../../Core/VespucciAssert.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace Compat {
namespace DSL {

DiagnosticBag::DiagnosticBag() : m_errors(0), m_warnings(0) {}

void DiagnosticBag::emit(DiagLevel lvl, i32 code, SourceSpan span,
                         const std::string& msg, const std::string& hint)
{
    Diagnostic d;
    d.level   = lvl;
    d.code    = code;
    d.span    = span;
    d.message = msg;
    d.hint    = hint;
    m_diags.push_back(d);
    if (lvl == DIAG_Error)        m_errors++;
    else if (lvl == DIAG_Warning) m_warnings++;
}

void DiagnosticBag::note(SourceSpan span, const std::string& msg) {
    if (m_diags.empty()) {
        emit(DIAG_Note, 0, span, msg);
        return;
    }
    Diagnostic n;
    n.level   = DIAG_Note;
    n.code    = 0;
    n.span    = span;
    n.message = msg;
    m_diags.back().notes.push_back(n);
}

void DiagnosticBag::error(i32 code, SourceSpan span,
                          const std::string& msg, const std::string& hint)
{
    emit(DIAG_Error, code, span, msg, hint);
}

void DiagnosticBag::warn(i32 code, SourceSpan span,
                         const std::string& msg, const std::string& hint)
{
    emit(DIAG_Warning, code, span, msg, hint);
}

bool DiagnosticBag::hasErrors()    const { return m_errors > 0; }
i32  DiagnosticBag::errorCount()   const { return m_errors; }
i32  DiagnosticBag::warningCount() const { return m_warnings; }

const std::vector<Diagnostic>& DiagnosticBag::list() const { return m_diags; }

void DiagnosticBag::clear() {
    m_diags.clear();
    m_errors   = 0;
    m_warnings = 0;
}

const char* DiagCodeLabel(i32 code) {
    static char buf[8];
    if (code <= 0) return "    ";
    std::snprintf(buf, sizeof(buf), "E%04d", code);
    return buf;
}

namespace {
    const char* LevelTag(DiagLevel l) {
        switch (l) {
            case DIAG_Error:   return "error";
            case DIAG_Warning: return "warning";
            case DIAG_Note:    return "note";
            default:           return "?";
        }
    }

    // Find line bounds containing byte offset `off`. Returns (start, end)
    // where end is exclusive. Lines are 1-indexed externally; this
    // helper deals only in byte offsets.
    void LineBounds(const char* src, usize n, usize off,
                    usize& outBegin, usize& outEnd)
    {
        outBegin = off;
        while (outBegin > 0 && src[outBegin - 1] != '\n') outBegin--;
        outEnd = off;
        while (outEnd < n && src[outEnd] != '\n') outEnd++;
    }
} // namespace

std::string DiagnosticBag::render(const char* filename,
                                   const char* source, usize sourceLen,
                                   const SourceMap& smap) const
{
    std::string out;
    char tmp[512];

    for (size_t i = 0; i < m_diags.size(); ++i) {
        const Diagnostic& d = m_diags[i];

        // Header line: "error[E0042]: msg"
        const char* tag = LevelTag(d.level);
        if (d.code > 0) {
            std::snprintf(tmp, sizeof(tmp), "%s[%s]: %s\n",
                tag, DiagCodeLabel(d.code), d.message.c_str());
        } else {
            std::snprintf(tmp, sizeof(tmp), "%s: %s\n", tag, d.message.c_str());
        }
        out += tmp;

        // Location line: "  --> file:line:col"
        i32 line = 1, col = 1;
        smap.offsetToLineCol(d.span.begin, line, col);
        std::snprintf(tmp, sizeof(tmp), "  --> %s:%d:%d\n",
            filename ? filename : "<input>", line, col);
        out += tmp;

        // Source preview + caret
        if (source && sourceLen > 0 && d.span.begin < sourceLen) {
            usize lb, le;
            LineBounds(source, sourceLen, d.span.begin, lb, le);
            std::snprintf(tmp, sizeof(tmp), "%5d | ", line);
            out += tmp;
            out.append(source + lb, le - lb);
            out.push_back('\n');

            // Caret line: spaces up to col, then ^^^^ for the span width
            out += "      | ";
            i32 padCol = col - 1;
            for (i32 k = 0; k < padCol; ++k) out.push_back(' ');
            i32 width = (i32)(d.span.end - d.span.begin);
            if (width <= 0) width = 1;
            // Don't run past the line end with carets.
            i32 maxWidth = (i32)(le - d.span.begin);
            if (width > maxWidth) width = maxWidth;
            for (i32 k = 0; k < width; ++k) out.push_back('^');
            if (!d.hint.empty()) {
                out.push_back(' ');
                out.append(d.hint);
            }
            out.push_back('\n');
        }

        // Notes
        for (size_t j = 0; j < d.notes.size(); ++j) {
            const Diagnostic& nn = d.notes[j];
            i32 nl = 1, nc = 1;
            smap.offsetToLineCol(nn.span.begin, nl, nc);
            std::snprintf(tmp, sizeof(tmp), "  note: %s (at %s:%d:%d)\n",
                nn.message.c_str(),
                filename ? filename : "<input>", nl, nc);
            out += tmp;
        }

        out.push_back('\n');
    }

    return out;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
