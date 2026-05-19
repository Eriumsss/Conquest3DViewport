// Lexer.h
// =============================================================================
// LEXER — TURNS BYTES INTO TOKENS, OR DIES TRYING (LOUDLY)
// =============================================================================
// Written by: Eriumsss
//
// Recursive-descent friend. Streams Token values out one at a time
// via next(); peek() looks ahead without consuming. Bad input does
// not crash — we emit a TOK_INVALID with the offending span and a
// diagnostic, then resync at the next obvious boundary (newline or
// matched punctuation).
//
// We do not produce a token stream up front and pass it to the parser.
// The parser pulls tokens lazily so the lexer's internal state machine
// stays simple and we can throw error spans at it from the top.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_LEXER_H_
#define VESPUCCI_COMPAT_DSL_LEXER_H_

#include "Token.h"
#include "Diagnostics.h"

namespace Vespucci {
namespace Compat {
namespace DSL {

class Lexer {
public:
    Lexer();

    // Reset to scan a fresh source buffer. The buffer must outlive
    // every Token returned (the lexeme StringRef points into it).
    void reset(const char* source, usize length, DiagnosticBag* diags);

    // Pop the next token. Returns TOK_EOF forever once exhausted.
    Token next();

    // Peek without consuming. Multiple peeks return the SAME token
    // until next() is called.
    const Token& peek();

    // Cheap accessors used by the parser for context.
    u32 currentOffset() const { return m_cursor; }

    // Force-skip until we see one of the given chars (parser uses
    // this for error recovery).
    void resyncTo(const char* anyOf);

private:
    Token scanOne();
    Token scanIdentifier();
    Token scanString(char quote);
    Token scanNumber();
    Token makeToken(TokenKind k, u32 begin) const;
    void  skipTriviaIncludingComments();
    bool  startsWithLineComment() const;
    bool  startsWithBlockComment() const;
    void  consumeBlockComment();

    void emitErrorAt(i32 code, u32 begin, u32 end, const char* msg);

    const char*    m_src;
    usize          m_len;
    u32            m_cursor;
    DiagnosticBag* m_diags;
    bool           m_havePeek;
    Token          m_peekTok;
};

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_LEXER_H_
