// Diagnostics.h
// =============================================================================
// COMPAT-DSL DIAGNOSTICS — CARET-POINTING ERRORS THAT DON'T FUCK AROUND
// =============================================================================
// Written by: Eriumsss
//
// When a designer's `.compat` rule blows up, the loader cannot just
// say "syntax error" and walk away like a coward. The error message
// has to point AT the line, AT the column, with a damn caret under
// the offending token, and a one-line explanation of what we expected
// vs what we got. Anything less and the designer fires up a debugger
// to figure out what the rule engine wanted, and at that point we
// have failed our entire purpose for existing.
//
// Diagnostic levels:
//   Error    — file is rejected, this rule does not load
//   Warning  — file loads, this rule was probably wrong but we kept going
//   Note     — auxiliary info attached to a parent diagnostic
//
// Output format (rendered by Render()):
//
//   error[E0042]: expected '->' between source and target
//     --> rules/score.compat:14:38
//        |
//     14 |     allow CapturePoint.OnCapture =? ScoreManager.AddScore
//        |                                  ^^ expected '->' here
//        |
//     note: rules use the syntax  src.event -> tgt.action
//
// Codes (E0001+) are stable across versions so users can bookmark them.
// Add new codes at the END of the table in Diagnostics.cpp; existing
// codes never get reused.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_DIAGNOSTICS_H_
#define VESPUCCI_COMPAT_DSL_DIAGNOSTICS_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"
#include "Token.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Compat {
namespace DSL {

class SourceMap; // fwd

enum DiagLevel {
    DIAG_Error   = 0,
    DIAG_Warning = 1,
    DIAG_Note    = 2
};

struct Diagnostic {
    DiagLevel    level;
    i32          code;        // EXXXX number, 0 if anonymous
    SourceSpan   span;
    std::string  message;     // primary message
    std::string  hint;        // optional one-line explanation under the caret
    std::vector<Diagnostic> notes; // attached follow-up notes
};

// Collector. The parser/sema/codegen all dump into one of these and
// the loader renders it at the end. NOT thread-safe — DSL parse runs
// single-threaded by design.
class DiagnosticBag {
public:
    DiagnosticBag();

    void emit(DiagLevel lvl, i32 code, SourceSpan span,
              const std::string& msg, const std::string& hint = "");

    // Attach a note to the LAST emitted diagnostic. Call this
    // immediately after the parent emit() to keep the structure
    // sane. If there is no parent, the note becomes a standalone Note.
    void note(SourceSpan span, const std::string& msg);

    // Convenience: shorthand for the most common emit shapes.
    void error(i32 code, SourceSpan span, const std::string& msg, const std::string& hint = "");
    void warn (i32 code, SourceSpan span, const std::string& msg, const std::string& hint = "");

    bool hasErrors() const;
    i32  errorCount() const;
    i32  warningCount() const;

    const std::vector<Diagnostic>& list() const;

    // Format every diagnostic as one big rendered string. Pass the
    // source buffer and SourceMap so we can show the surrounding
    // line + caret. `filename` shows up in the "--> file:L:C" header.
    std::string render(const char* filename,
                       const char* source, usize sourceLen,
                       const SourceMap& smap) const;

    void clear();

private:
    std::vector<Diagnostic> m_diags;
    i32                     m_errors;
    i32                     m_warnings;
};

// Stable diagnostic codes. Add at the END only; never recycle. The
// numeric prefix is just E#### so users can pin them in muscle memory.
namespace Codes {
    static const i32 kE_UnexpectedChar          = 1;
    static const i32 kE_UnterminatedString      = 2;
    static const i32 kE_BadNumber               = 3;
    static const i32 kE_ExpectedToken           = 4;
    static const i32 kE_ExpectedExpr            = 5;
    static const i32 kE_ExpectedRule            = 6;
    static const i32 kE_UnknownType             = 7;
    static const i32 kE_UnknownEventOrAction    = 8;
    static const i32 kE_BadStdLibCall           = 9;
    static const i32 kE_TypeMismatch            = 10;
    static const i32 kE_ArityMismatch           = 11;
    static const i32 kE_DuplicateRuleName       = 12;
    static const i32 kW_ShadowedRule            = 13;
    static const i32 kW_DeprecatedType          = 14;
    static const i32 kE_UnreachableRule         = 15;
    static const i32 kE_RuntimeFault            = 16;
    static const i32 kE_OverflowOrUnderflow     = 17;
    static const i32 kW_NoEffect                = 18;
    static const i32 kE_BadWildcard             = 19;
    static const i32 kE_BadAlloc                = 20;
} // namespace Codes

// Tiny helper: turn a code into a human-readable label like "E0042".
const char* DiagCodeLabel(i32 code);

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_DIAGNOSTICS_H_
