// Token.h
// =============================================================================
// THE GODDAMN TOKENS OF THE COMPAT DSL — KIND, SPAN, LEXEME
// =============================================================================
// Written by: Eriumsss
//
// We hand-rolled a tiny rule language so designers can override the
// compat matrix without touching the goddamn C++. A `.compat` file
// looks something like:
//
//   rule "score-on-capture" {
//       allow CapturePoint.OnCapture -> ScoreManager.AddScore
//       require sameLayer
//       weight 1.20
//   }
//   deny  CapturePoint.* -> *.Move with reason "spatial entities don't take Move actions"
//
// The DSL needs a lexer, a parser, an AST, sema checks, an IR, a VM,
// and a stdlib of built-in helpers (sameLayer, distLessThan, ...).
// All of that lives in this folder. Token.h is the bottom: every
// downstream file knows ONE thing about the DSL — what a token looks
// like — and that one thing comes from here.
//
// We do NOT try to support every Lua-ish or JSON-ish construct. The
// language is deliberately minimal. If a designer needs something we
// did not anticipate, they file a request and we add a token. We did
// NOT write this DSL to be everything; we wrote it to be enough.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_TOKEN_H_
#define VESPUCCI_COMPAT_DSL_TOKEN_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"

namespace Vespucci {
namespace Compat {
namespace DSL {

enum TokenKind {
    TOK_EOF             = 0,    // end of input
    TOK_INVALID         = 1,    // lexer error sentinel — token's lexeme spans the bad bytes

    // Literals
    TOK_IDENTIFIER      = 10,   // [a-zA-Z_][a-zA-Z0-9_]*
    TOK_STRING          = 11,   // "..." / '...'
    TOK_INT             = 12,   // 0, 42, 0x1F
    TOK_FLOAT           = 13,   // 1.5, 3.14e-2

    // Keywords
    TOK_KW_RULE         = 20,
    TOK_KW_ALLOW        = 21,
    TOK_KW_DENY         = 22,
    TOK_KW_REQUIRE      = 23,
    TOK_KW_WEIGHT       = 24,
    TOK_KW_REASON       = 25,
    TOK_KW_WITH         = 26,
    TOK_KW_AND          = 27,
    TOK_KW_OR           = 28,
    TOK_KW_NOT          = 29,
    TOK_KW_TRUE         = 30,
    TOK_KW_FALSE        = 31,
    TOK_KW_IF           = 32,
    TOK_KW_ELSE         = 33,

    // Punctuation
    TOK_LBRACE          = 50,   // {
    TOK_RBRACE          = 51,   // }
    TOK_LPAREN          = 52,   // (
    TOK_RPAREN          = 53,   // )
    TOK_LBRACKET        = 54,   // [
    TOK_RBRACKET        = 55,   // ]
    TOK_COMMA           = 56,   // ,
    TOK_SEMI            = 57,   // ;
    TOK_DOT             = 58,   // .
    TOK_ARROW           = 59,   // ->
    TOK_COLON           = 60,   // :
    TOK_STAR            = 61,   // *  (wildcard / multiply)

    // Operators
    TOK_EQ              = 70,   // ==
    TOK_NEQ             = 71,   // !=
    TOK_LT              = 72,   // <
    TOK_LE              = 73,   // <=
    TOK_GT              = 74,   // >
    TOK_GE              = 75,   // >=
    TOK_PLUS            = 76,
    TOK_MINUS           = 77,
    TOK_SLASH           = 78,
    TOK_PERCENT         = 79,
    TOK_BANG            = 80,   // !
    TOK_AMPAMP          = 81,   // &&
    TOK_PIPEPIPE        = 82,   // ||
    TOK_ASSIGN          = 83    // =
};

// SourceSpan — byte offsets into the original source. Diagnostics
// turn these into line/column via SourceMap. We do NOT track line
// numbers per token because that doubles per-token memory and the
// few tokens that actually hit a diagnostic eat the conversion cost.
struct SourceSpan {
    u32 begin;    // byte offset of first char
    u32 end;      // byte offset one past last char
};

struct Token {
    TokenKind        kind;
    SourceSpan       span;
    Core::StringRef  lexeme;     // points into the source buffer (must outlive token)

    // Literal payloads. Only the field matching `kind` is valid.
    union {
        i64 i;
        f64 d;
        bool b;
    } value;
};

// Human-readable name for a kind. Used by Diagnostics and AstPrinter.
const char* TokenKindName(TokenKind k);

// True if `k` is a keyword. Lexer flips IDENTIFIER → TOK_KW_* via this.
bool IsKeyword(TokenKind k);

// Look up keyword kind by spelling. Returns TOK_IDENTIFIER if no match.
TokenKind KeywordKind(const char* lexeme, usize n);

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_TOKEN_H_
