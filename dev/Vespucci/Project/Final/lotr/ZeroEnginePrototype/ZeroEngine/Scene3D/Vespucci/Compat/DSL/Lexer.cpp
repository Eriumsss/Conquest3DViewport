// Lexer.cpp
// =============================================================================
// Hand-rolled lexer. Single-pass over the byte buffer. Emits Token
// values; bad input becomes TOK_INVALID + a Diagnostics::error. No
// table-driven horseshit — this language has 30 token kinds and a
// switch statement is the right tool for that scale.
// =============================================================================
// Written by: Eriumsss

#include "Lexer.h"

#include "../../Core/Logging.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    inline bool IsIdentStart(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    inline bool IsIdentCont(char c) {
        return IsIdentStart(c) || (c >= '0' && c <= '9');
    }
    inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }
    inline bool IsHexDigit(char c) {
        return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
} // namespace

// ── Token kind helpers (defined here, not in a Token.cpp because the
// table is small enough to live with the lexer code that uses it) ──

const char* TokenKindName(TokenKind k) {
    switch (k) {
        case TOK_EOF:        return "EOF";
        case TOK_INVALID:    return "INVALID";
        case TOK_IDENTIFIER: return "identifier";
        case TOK_STRING:     return "string";
        case TOK_INT:        return "int";
        case TOK_FLOAT:      return "float";
        case TOK_KW_RULE:    return "'rule'";
        case TOK_KW_ALLOW:   return "'allow'";
        case TOK_KW_DENY:    return "'deny'";
        case TOK_KW_REQUIRE: return "'require'";
        case TOK_KW_WEIGHT:  return "'weight'";
        case TOK_KW_REASON:  return "'reason'";
        case TOK_KW_WITH:    return "'with'";
        case TOK_KW_AND:     return "'and'";
        case TOK_KW_OR:      return "'or'";
        case TOK_KW_NOT:     return "'not'";
        case TOK_KW_TRUE:    return "'true'";
        case TOK_KW_FALSE:   return "'false'";
        case TOK_KW_IF:      return "'if'";
        case TOK_KW_ELSE:    return "'else'";
        case TOK_LBRACE:     return "'{'";
        case TOK_RBRACE:     return "'}'";
        case TOK_LPAREN:     return "'('";
        case TOK_RPAREN:     return "')'";
        case TOK_LBRACKET:   return "'['";
        case TOK_RBRACKET:   return "']'";
        case TOK_COMMA:      return "','";
        case TOK_SEMI:       return "';'";
        case TOK_DOT:        return "'.'";
        case TOK_ARROW:      return "'->'";
        case TOK_COLON:      return "':'";
        case TOK_STAR:       return "'*'";
        case TOK_EQ:         return "'=='";
        case TOK_NEQ:        return "'!='";
        case TOK_LT:         return "'<'";
        case TOK_LE:         return "'<='";
        case TOK_GT:         return "'>'";
        case TOK_GE:         return "'>='";
        case TOK_PLUS:       return "'+'";
        case TOK_MINUS:      return "'-'";
        case TOK_SLASH:      return "'/'";
        case TOK_PERCENT:    return "'%'";
        case TOK_BANG:       return "'!'";
        case TOK_AMPAMP:     return "'&&'";
        case TOK_PIPEPIPE:   return "'||'";
        case TOK_ASSIGN:     return "'='";
        default:             return "?";
    }
}

bool IsKeyword(TokenKind k) {
    return k >= TOK_KW_RULE && k <= TOK_KW_ELSE;
}

TokenKind KeywordKind(const char* lexeme, usize n) {
    struct KW { const char* spelling; TokenKind kind; usize len; };
    // Static table, walk linearly. ~13 entries — branch predictor and
    // cache locality crush this. We do not need a perfect hash here.
    static const KW kKws[] = {
        { "rule",    TOK_KW_RULE,    4 },
        { "allow",   TOK_KW_ALLOW,   5 },
        { "deny",    TOK_KW_DENY,    4 },
        { "require", TOK_KW_REQUIRE, 7 },
        { "weight",  TOK_KW_WEIGHT,  6 },
        { "reason",  TOK_KW_REASON,  6 },
        { "with",    TOK_KW_WITH,    4 },
        { "and",     TOK_KW_AND,     3 },
        { "or",      TOK_KW_OR,      2 },
        { "not",     TOK_KW_NOT,     3 },
        { "true",    TOK_KW_TRUE,    4 },
        { "false",   TOK_KW_FALSE,   5 },
        { "if",      TOK_KW_IF,      2 },
        { "else",    TOK_KW_ELSE,    4 }
    };
    static const i32 kCount = (i32)(sizeof(kKws) / sizeof(kKws[0]));
    for (i32 i = 0; i < kCount; ++i) {
        if (kKws[i].len == n && std::memcmp(kKws[i].spelling, lexeme, n) == 0) {
            return kKws[i].kind;
        }
    }
    return TOK_IDENTIFIER;
}

// ── Lexer impl ────────────────────────────────────────────────────────

Lexer::Lexer()
    : m_src(0), m_len(0), m_cursor(0),
      m_diags(0), m_havePeek(false)
{
    // Initialize peek slot to a known-empty token.
    m_peekTok.kind     = TOK_EOF;
    m_peekTok.span.begin = 0;
    m_peekTok.span.end   = 0;
    m_peekTok.lexeme   = Core::StringRef();
    m_peekTok.value.i  = 0;
}

void Lexer::reset(const char* source, usize length, DiagnosticBag* diags) {
    m_src      = source ? source : "";
    m_len      = source ? length : 0;
    m_cursor   = 0;
    m_diags    = diags;
    m_havePeek = false;
}

Token Lexer::makeToken(TokenKind k, u32 begin) const {
    Token t;
    t.kind = k;
    t.span.begin = begin;
    t.span.end   = m_cursor;
    t.lexeme = Core::StringRef(m_src + begin, (usize)(m_cursor - begin));
    t.value.i = 0;
    return t;
}

void Lexer::emitErrorAt(i32 code, u32 begin, u32 end, const char* msg) {
    if (!m_diags) return;
    SourceSpan sp; sp.begin = begin; sp.end = end;
    m_diags->error(code, sp, std::string(msg));
}

bool Lexer::startsWithLineComment() const {
    return m_cursor + 1 < m_len && m_src[m_cursor] == '/' && m_src[m_cursor + 1] == '/';
}

bool Lexer::startsWithBlockComment() const {
    return m_cursor + 1 < m_len && m_src[m_cursor] == '/' && m_src[m_cursor + 1] == '*';
}

void Lexer::consumeBlockComment() {
    u32 begin = m_cursor;
    m_cursor += 2;
    while (m_cursor + 1 < m_len) {
        if (m_src[m_cursor] == '*' && m_src[m_cursor + 1] == '/') {
            m_cursor += 2;
            return;
        }
        m_cursor++;
    }
    // Unterminated block comment — eat the rest, scream.
    emitErrorAt(Codes::kE_UnterminatedString, begin, (u32)m_len,
        "unterminated block comment, file ends inside /* ... */");
    m_cursor = (u32)m_len;
}

void Lexer::skipTriviaIncludingComments() {
    while (m_cursor < m_len) {
        char c = m_src[m_cursor];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            m_cursor++;
        } else if (startsWithLineComment()) {
            while (m_cursor < m_len && m_src[m_cursor] != '\n') m_cursor++;
        } else if (startsWithBlockComment()) {
            consumeBlockComment();
        } else {
            return;
        }
    }
}

Token Lexer::scanIdentifier() {
    u32 begin = m_cursor;
    while (m_cursor < m_len && IsIdentCont(m_src[m_cursor])) m_cursor++;
    Token t = makeToken(TOK_IDENTIFIER, begin);
    TokenKind kw = KeywordKind(m_src + begin, (usize)(m_cursor - begin));
    if (kw != TOK_IDENTIFIER) {
        t.kind = kw;
        if (kw == TOK_KW_TRUE)  t.value.b = true;
        if (kw == TOK_KW_FALSE) t.value.b = false;
    }
    return t;
}

Token Lexer::scanString(char quote) {
    u32 begin = m_cursor;
    m_cursor++; // skip opening quote
    while (m_cursor < m_len && m_src[m_cursor] != quote) {
        if (m_src[m_cursor] == '\\' && m_cursor + 1 < m_len) {
            m_cursor += 2;
        } else if (m_src[m_cursor] == '\n') {
            emitErrorAt(Codes::kE_UnterminatedString, begin, m_cursor,
                "string literal contains a raw newline (close the quote first)");
            // Treat as terminated at newline so we recover.
            return makeToken(TOK_INVALID, begin);
        } else {
            m_cursor++;
        }
    }
    if (m_cursor >= m_len) {
        emitErrorAt(Codes::kE_UnterminatedString, begin, m_cursor,
            "unterminated string literal, file ends with no closing quote");
        return makeToken(TOK_INVALID, begin);
    }
    m_cursor++; // closing quote
    return makeToken(TOK_STRING, begin);
}

Token Lexer::scanNumber() {
    u32 begin = m_cursor;
    bool isFloat = false;
    bool isHex   = false;

    if (m_src[m_cursor] == '0' && m_cursor + 1 < m_len &&
        (m_src[m_cursor + 1] == 'x' || m_src[m_cursor + 1] == 'X'))
    {
        isHex = true;
        m_cursor += 2;
        while (m_cursor < m_len && IsHexDigit(m_src[m_cursor])) m_cursor++;
    } else {
        while (m_cursor < m_len && IsDigit(m_src[m_cursor])) m_cursor++;
        if (m_cursor < m_len && m_src[m_cursor] == '.') {
            isFloat = true;
            m_cursor++;
            while (m_cursor < m_len && IsDigit(m_src[m_cursor])) m_cursor++;
        }
        if (m_cursor < m_len && (m_src[m_cursor] == 'e' || m_src[m_cursor] == 'E')) {
            isFloat = true;
            m_cursor++;
            if (m_cursor < m_len && (m_src[m_cursor] == '+' || m_src[m_cursor] == '-')) m_cursor++;
            while (m_cursor < m_len && IsDigit(m_src[m_cursor])) m_cursor++;
        }
    }

    u32 end = m_cursor;
    usize n = end - begin;
    if (n == 0) {
        emitErrorAt(Codes::kE_BadNumber, begin, end, "expected a number");
        return makeToken(TOK_INVALID, begin);
    }

    char buf[64];
    if (n >= sizeof(buf)) {
        emitErrorAt(Codes::kE_BadNumber, begin, end,
            "numeric literal is suspiciously long, refusing to parse");
        return makeToken(TOK_INVALID, begin);
    }
    std::memcpy(buf, m_src + begin, n);
    buf[n] = 0;

    Token t;
    t.span.begin = begin;
    t.span.end   = end;
    t.lexeme     = Core::StringRef(m_src + begin, n);
    if (isFloat) {
        t.kind    = TOK_FLOAT;
        t.value.d = std::strtod(buf, 0);
    } else {
        t.kind = TOK_INT;
        t.value.i = isHex ? (i64)std::strtoll(buf, 0, 16)
                          : (i64)std::strtoll(buf, 0, 10);
    }
    return t;
}

Token Lexer::scanOne() {
    skipTriviaIncludingComments();
    if (m_cursor >= m_len) {
        Token t;
        t.kind = TOK_EOF;
        t.span.begin = (u32)m_len;
        t.span.end   = (u32)m_len;
        t.lexeme = Core::StringRef("", 0);
        t.value.i = 0;
        return t;
    }

    u32 begin = m_cursor;
    char c = m_src[m_cursor];

    if (IsIdentStart(c)) return scanIdentifier();
    if (IsDigit(c))      return scanNumber();
    if (c == '"' || c == '\'') return scanString(c);

    // Single / multi-char punctuation. Switch dominates a table here.
    switch (c) {
        case '{': m_cursor++; return makeToken(TOK_LBRACE, begin);
        case '}': m_cursor++; return makeToken(TOK_RBRACE, begin);
        case '(': m_cursor++; return makeToken(TOK_LPAREN, begin);
        case ')': m_cursor++; return makeToken(TOK_RPAREN, begin);
        case '[': m_cursor++; return makeToken(TOK_LBRACKET, begin);
        case ']': m_cursor++; return makeToken(TOK_RBRACKET, begin);
        case ',': m_cursor++; return makeToken(TOK_COMMA, begin);
        case ';': m_cursor++; return makeToken(TOK_SEMI, begin);
        case '.': m_cursor++; return makeToken(TOK_DOT, begin);
        case ':': m_cursor++; return makeToken(TOK_COLON, begin);
        case '*': m_cursor++; return makeToken(TOK_STAR, begin);
        case '+': m_cursor++; return makeToken(TOK_PLUS, begin);
        case '/': m_cursor++; return makeToken(TOK_SLASH, begin);
        case '%': m_cursor++; return makeToken(TOK_PERCENT, begin);
        case '-':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '>') { m_cursor++; return makeToken(TOK_ARROW, begin); }
            return makeToken(TOK_MINUS, begin);
        case '!':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '=') { m_cursor++; return makeToken(TOK_NEQ, begin); }
            return makeToken(TOK_BANG, begin);
        case '=':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '=') { m_cursor++; return makeToken(TOK_EQ, begin); }
            return makeToken(TOK_ASSIGN, begin);
        case '<':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '=') { m_cursor++; return makeToken(TOK_LE, begin); }
            return makeToken(TOK_LT, begin);
        case '>':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '=') { m_cursor++; return makeToken(TOK_GE, begin); }
            return makeToken(TOK_GT, begin);
        case '&':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '&') { m_cursor++; return makeToken(TOK_AMPAMP, begin); }
            // Bare & is not a token in this DSL. Scream.
            emitErrorAt(Codes::kE_UnexpectedChar, begin, m_cursor,
                "lone '&' is not allowed (did you mean '&&'?)");
            return makeToken(TOK_INVALID, begin);
        case '|':
            m_cursor++;
            if (m_cursor < m_len && m_src[m_cursor] == '|') { m_cursor++; return makeToken(TOK_PIPEPIPE, begin); }
            emitErrorAt(Codes::kE_UnexpectedChar, begin, m_cursor,
                "lone '|' is not allowed (did you mean '||'?)");
            return makeToken(TOK_INVALID, begin);
        default:
            m_cursor++;
            emitErrorAt(Codes::kE_UnexpectedChar, begin, m_cursor,
                "this character is not part of the compat DSL — strip it or quote it as a string");
            return makeToken(TOK_INVALID, begin);
    }
}

Token Lexer::next() {
    if (m_havePeek) {
        m_havePeek = false;
        return m_peekTok;
    }
    return scanOne();
}

const Token& Lexer::peek() {
    if (!m_havePeek) {
        m_peekTok = scanOne();
        m_havePeek = true;
    }
    return m_peekTok;
}

void Lexer::resyncTo(const char* anyOf) {
    if (!anyOf || !*anyOf) return;
    while (m_cursor < m_len) {
        char c = m_src[m_cursor];
        for (const char* p = anyOf; *p; ++p) {
            if (*p == c) return;
        }
        m_cursor++;
    }
    m_havePeek = false;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
