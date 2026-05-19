// Parser.cpp
// =============================================================================
// Recursive-descent parser. AST nodes live in the supplied arena.
// Errors do NOT throw — they go in the diag bag and the parser keeps
// going through resync points so a designer sees every busted rule
// in one save+reload cycle, not fifteen.
// =============================================================================
// Written by: Eriumsss

#include "Parser.h"
#include "Ast.h"

#include "../../Core/Arena.h"
#include "../../Core/Logging.h"
#include "../../Core/VespucciAssert.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    SourceSpan SpanFromToken(const Token& t) { return t.span; }

    SourceSpan SpanUnion(SourceSpan a, SourceSpan b) {
        SourceSpan s; s.begin = a.begin; s.end = b.end > a.end ? b.end : a.end;
        return s;
    }
} // namespace

// ── AST helpers ───────────────────────────────────────────────────────

AstNode* AstNew(Core::Arena* arena, AstKind kind, SourceSpan span) {
    if (!arena) return 0;
    AstNode* n = (AstNode*)arena->alloc(sizeof(AstNode), alignof(AstNode));
    if (!n) return 0;
    std::memset(n, 0, sizeof(AstNode));
    n->kind = kind;
    n->span = span;
    return n;
}

AstNode* AstNewWithChildren(Core::Arena* arena, AstKind kind, SourceSpan span,
                             AstNode** kids, i32 n)
{
    AstNode* node = AstNew(arena, kind, span);
    if (!node) return 0;
    if (n > 0 && kids) {
        AstNode** dst = (AstNode**)arena->alloc(sizeof(AstNode*) * (size_t)n, alignof(AstNode*));
        if (!dst) return node;
        std::memcpy(dst, kids, sizeof(AstNode*) * (size_t)n);
        node->children   = dst;
        node->childCount = n;
    }
    return node;
}

// ── Parser impl ───────────────────────────────────────────────────────

Parser::Parser() : m_arena(0), m_diags(0) {}

AstNode* Parser::parse(const char* source, usize length,
                        Core::Arena* arena, DiagnosticBag* diags)
{
    if (!arena) return 0;
    m_arena = arena;
    m_diags = diags;
    m_lex.reset(source, length, diags);
    m_cur = m_lex.next();
    return parseModule();
}

AstNode* Parser::makeNode(AstKind k, SourceSpan span) {
    return AstNew(m_arena, k, span);
}

AstNode* Parser::makeBinary(AstKind k, AstNode* lhs, AstNode* rhs, SourceSpan span) {
    AstNode* n = makeNode(k, span);
    if (!n) return 0;
    n->lhs = lhs; n->rhs = rhs;
    return n;
}

AstNode* Parser::makeUnary(AstKind k, AstNode* operand, SourceSpan span,
                            const Core::StringRef& opText)
{
    AstNode* n = makeNode(k, span);
    if (!n) return 0;
    n->lhs = operand;
    n->text = opText;
    return n;
}

bool Parser::accept(TokenKind k) {
    if (m_cur.kind == k) {
        m_cur = m_lex.next();
        return true;
    }
    return false;
}

bool Parser::expect(TokenKind k, i32 errCode, const char* msg) {
    if (m_cur.kind == k) {
        m_cur = m_lex.next();
        return true;
    }
    if (m_diags) {
        std::string s = msg ? msg : "expected ";
        s += TokenKindName(k);
        s += ", got ";
        s += TokenKindName(m_cur.kind);
        m_diags->error(errCode, m_cur.span, s);
    }
    return false;
}

Token Parser::eat() {
    Token t = m_cur;
    m_cur = m_lex.next();
    return t;
}

void Parser::resyncToTopLevel() {
    while (m_cur.kind != TOK_EOF) {
        if (m_cur.kind == TOK_KW_RULE  ||
            m_cur.kind == TOK_KW_ALLOW ||
            m_cur.kind == TOK_KW_DENY) return;
        m_cur = m_lex.next();
    }
}

void Parser::resyncToRuleEnd() {
    i32 depth = 0;
    while (m_cur.kind != TOK_EOF) {
        if (m_cur.kind == TOK_LBRACE) depth++;
        else if (m_cur.kind == TOK_RBRACE) {
            if (depth == 0) { m_cur = m_lex.next(); return; }
            depth--;
        }
        m_cur = m_lex.next();
    }
}

// ── Top-level ─────────────────────────────────────────────────────────

AstNode* Parser::parseModule() {
    SourceSpan span = m_cur.span;
    AstNode* mod = makeNode(AST_Module, span);
    if (!mod) return 0;

    // Collect into a vector-like temp array allocated from the arena.
    static const i32 kCap = 4096;
    AstNode** tmp = (AstNode**)m_arena->alloc(sizeof(AstNode*) * kCap, alignof(AstNode*));
    i32 n = 0;

    while (m_cur.kind != TOK_EOF) {
        AstNode* stmt = parseTopStmt();
        if (stmt && n < kCap) {
            tmp[n++] = stmt;
        } else if (!stmt) {
            // Hit a hard parse fail — resync and continue so we
            // get more diagnostics in one pass.
            resyncToTopLevel();
        }
    }

    if (n > 0) {
        AstNode** dst = (AstNode**)m_arena->alloc(sizeof(AstNode*) * (size_t)n, alignof(AstNode*));
        std::memcpy(dst, tmp, sizeof(AstNode*) * (size_t)n);
        mod->children   = dst;
        mod->childCount = n;
    }
    mod->span.end = m_cur.span.end;
    return mod;
}

AstNode* Parser::parseTopStmt() {
    if (m_cur.kind == TOK_KW_RULE)  return parseRuleBlock();
    if (m_cur.kind == TOK_KW_ALLOW) return parseAllowOrDeny(true);
    if (m_cur.kind == TOK_KW_DENY)  return parseAllowOrDeny(false);

    if (m_diags) {
        m_diags->error(Codes::kE_ExpectedRule, m_cur.span,
            "expected 'rule', 'allow', or 'deny' at top level",
            "files start with a rule block or a bare allow/deny");
    }
    return 0;
}

AstNode* Parser::parseRuleBlock() {
    SourceSpan begin = m_cur.span;
    eat(); // 'rule'

    // Optional name string.
    Core::StringRef name;
    if (m_cur.kind == TOK_STRING) {
        name = m_cur.lexeme;
        // Strip surrounding quotes from lexeme.
        if (name.size() >= 2) {
            name = Core::StringRef(name.data() + 1, name.size() - 2);
        }
        eat();
    }

    if (!expect(TOK_LBRACE, Codes::kE_ExpectedToken, "expected '{' to open rule body")) {
        resyncToRuleEnd();
        return 0;
    }

    AstNode* rule = makeNode(AST_Rule, begin);
    rule->text = name;

    // Statements inside the block.
    static const i32 kCap = 64;
    AstNode** tmp = (AstNode**)m_arena->alloc(sizeof(AstNode*) * kCap, alignof(AstNode*));
    i32 n = 0;

    while (m_cur.kind != TOK_RBRACE && m_cur.kind != TOK_EOF) {
        AstNode* s = 0;
        switch (m_cur.kind) {
            case TOK_KW_ALLOW:   s = parseAllowOrDeny(true);  break;
            case TOK_KW_DENY:    s = parseAllowOrDeny(false); break;
            case TOK_KW_REQUIRE: s = parseRequireStmt();      break;
            case TOK_KW_WEIGHT:  s = parseWeightStmt();       break;
            case TOK_KW_REASON:  s = parseReasonStmt();       break;
            default:
                if (m_diags) {
                    m_diags->error(Codes::kE_ExpectedRule, m_cur.span,
                        "expected 'allow', 'deny', 'require', 'weight', or 'reason' inside rule",
                        "rule body statements only — anything else belongs outside the braces");
                }
                m_cur = m_lex.next();
                continue;
        }
        if (s && n < kCap) tmp[n++] = s;
    }
    if (m_cur.kind == TOK_RBRACE) eat();

    if (n > 0) {
        AstNode** dst = (AstNode**)m_arena->alloc(sizeof(AstNode*) * (size_t)n, alignof(AstNode*));
        std::memcpy(dst, tmp, sizeof(AstNode*) * (size_t)n);
        rule->children   = dst;
        rule->childCount = n;
    }
    rule->span.end = m_cur.span.begin;
    return rule;
}

AstNode* Parser::parseAllowOrDeny(bool allow) {
    SourceSpan begin = m_cur.span;
    eat(); // 'allow' / 'deny'

    AstNode* spec = parseWireSpec();
    if (!spec) return 0;

    // Optional 'with' clause: 'with reason "..."' or 'with weight 1.2'.
    AstNode* reasonNode = 0;
    AstNode* weightNode = 0;
    if (accept(TOK_KW_WITH)) {
        if (m_cur.kind == TOK_KW_REASON) {
            reasonNode = parseReasonStmt();
        } else if (m_cur.kind == TOK_KW_WEIGHT) {
            weightNode = parseWeightStmt();
        } else if (m_diags) {
            m_diags->error(Codes::kE_ExpectedRule, m_cur.span,
                "expected 'reason' or 'weight' after 'with'");
        }
    }

    AstKind k = allow ? AST_AllowStmt : AST_DenyStmt;
    AstNode* node = makeNode(k, SpanUnion(begin, m_cur.span));
    node->lhs = spec;
    node->thenB = reasonNode;
    node->elseB = weightNode;

    accept(TOK_SEMI);
    return node;
}

AstNode* Parser::parseRequireStmt() {
    SourceSpan begin = m_cur.span;
    eat(); // 'require'
    AstNode* expr = parseExpr();
    if (!expr) return 0;
    AstNode* node = makeNode(AST_RequireStmt, SpanUnion(begin, m_cur.span));
    node->lhs = expr;
    accept(TOK_SEMI);
    return node;
}

AstNode* Parser::parseWeightStmt() {
    SourceSpan begin = m_cur.span;
    eat(); // 'weight'
    AstNode* expr = parseExpr();
    if (!expr) return 0;
    AstNode* node = makeNode(AST_WeightStmt, SpanUnion(begin, m_cur.span));
    node->lhs = expr;
    accept(TOK_SEMI);
    return node;
}

AstNode* Parser::parseReasonStmt() {
    SourceSpan begin = m_cur.span;
    eat(); // 'reason'
    if (m_cur.kind != TOK_STRING) {
        if (m_diags) {
            m_diags->error(Codes::kE_ExpectedToken, m_cur.span,
                "expected string literal after 'reason'");
        }
        return 0;
    }
    AstNode* node = makeNode(AST_ReasonStmt, SpanUnion(begin, m_cur.span));
    Core::StringRef body = m_cur.lexeme;
    if (body.size() >= 2) body = Core::StringRef(body.data() + 1, body.size() - 2);
    node->text = body;
    eat();
    accept(TOK_SEMI);
    return node;
}

AstNode* Parser::parseWireSpec() {
    AstNode* src = parseTypeOrWildcard();
    if (!src) return 0;
    if (!expect(TOK_DOT, Codes::kE_ExpectedToken, "expected '.' after source type")) return 0;
    AstNode* event = parseEventOrWildcard();
    if (!event) return 0;
    if (!expect(TOK_ARROW, Codes::kE_ExpectedToken, "expected '->' between source.event and target")) return 0;
    AstNode* tgt = parseTypeOrWildcard();
    if (!tgt) return 0;
    if (!expect(TOK_DOT, Codes::kE_ExpectedToken, "expected '.' after target type")) return 0;
    AstNode* action = parseActionOrWildcard();
    if (!action) return 0;

    AstNode* spec = makeNode(AST_WireSpec, SpanUnion(src->span, action->span));
    AstNode* kids[4] = { src, event, tgt, action };
    spec->children   = (AstNode**)m_arena->alloc(sizeof(AstNode*) * 4, alignof(AstNode*));
    std::memcpy(spec->children, kids, sizeof(AstNode*) * 4);
    spec->childCount = 4;
    spec->lhs = src;
    spec->rhs = tgt;
    return spec;
}

namespace {
    AstNode* MakeRefNode(Parser* /*p*/, Core::Arena* arena, AstKind kind,
                          const Token& t)
    {
        AstNode* n = AstNew(arena, kind, t.span);
        if (!n) return 0;
        n->text = t.lexeme;
        return n;
    }
} // namespace

AstNode* Parser::parseTypeOrWildcard() {
    if (m_cur.kind == TOK_STAR) {
        Token t = eat();
        AstNode* n = makeNode(AST_TypeRef, t.span);
        n->text = Core::StringRef("*", 1);
        return n;
    }
    if (m_cur.kind != TOK_IDENTIFIER) {
        if (m_diags) {
            m_diags->error(Codes::kE_UnknownType, m_cur.span,
                "expected type name or '*'");
        }
        return 0;
    }
    Token t = eat();
    return MakeRefNode(this, m_arena, AST_TypeRef, t);
}

AstNode* Parser::parseEventOrWildcard() {
    if (m_cur.kind == TOK_STAR) {
        Token t = eat();
        AstNode* n = makeNode(AST_EventRef, t.span);
        n->text = Core::StringRef("*", 1);
        return n;
    }
    if (m_cur.kind != TOK_IDENTIFIER) {
        if (m_diags) {
            m_diags->error(Codes::kE_UnknownEventOrAction, m_cur.span,
                "expected event name or '*'");
        }
        return 0;
    }
    Token t = eat();
    return MakeRefNode(this, m_arena, AST_EventRef, t);
}

AstNode* Parser::parseActionOrWildcard() {
    if (m_cur.kind == TOK_STAR) {
        Token t = eat();
        AstNode* n = makeNode(AST_ActionRef, t.span);
        n->text = Core::StringRef("*", 1);
        return n;
    }
    if (m_cur.kind != TOK_IDENTIFIER) {
        if (m_diags) {
            m_diags->error(Codes::kE_UnknownEventOrAction, m_cur.span,
                "expected action name or '*'");
        }
        return 0;
    }
    Token t = eat();
    return MakeRefNode(this, m_arena, AST_ActionRef, t);
}

// ── Expression precedence climber ─────────────────────────────────────

AstNode* Parser::parseExpr() {
    return parseOr();
}

AstNode* Parser::parseOr() {
    AstNode* lhs = parseAnd();
    while (m_cur.kind == TOK_KW_OR || m_cur.kind == TOK_PIPEPIPE) {
        Token op = eat();
        AstNode* rhs = parseAnd();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        bin->text = Core::StringRef("or", 2);
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseAnd() {
    AstNode* lhs = parseEquality();
    while (m_cur.kind == TOK_KW_AND || m_cur.kind == TOK_AMPAMP) {
        Token op = eat();
        AstNode* rhs = parseEquality();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        bin->text = Core::StringRef("and", 3);
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseEquality() {
    AstNode* lhs = parseComparison();
    while (m_cur.kind == TOK_EQ || m_cur.kind == TOK_NEQ) {
        Token op = eat();
        AstNode* rhs = parseComparison();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        bin->text = (op.kind == TOK_EQ) ? Core::StringRef("==", 2) : Core::StringRef("!=", 2);
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseComparison() {
    AstNode* lhs = parseAddSub();
    while (m_cur.kind == TOK_LT || m_cur.kind == TOK_LE ||
           m_cur.kind == TOK_GT || m_cur.kind == TOK_GE)
    {
        Token op = eat();
        AstNode* rhs = parseAddSub();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        switch (op.kind) {
            case TOK_LT: bin->text = Core::StringRef("<", 1); break;
            case TOK_LE: bin->text = Core::StringRef("<=", 2); break;
            case TOK_GT: bin->text = Core::StringRef(">", 1); break;
            case TOK_GE: bin->text = Core::StringRef(">=", 2); break;
            default: break;
        }
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseAddSub() {
    AstNode* lhs = parseMulDiv();
    while (m_cur.kind == TOK_PLUS || m_cur.kind == TOK_MINUS) {
        Token op = eat();
        AstNode* rhs = parseMulDiv();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        bin->text = (op.kind == TOK_PLUS) ? Core::StringRef("+", 1) : Core::StringRef("-", 1);
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseMulDiv() {
    AstNode* lhs = parseUnary();
    while (m_cur.kind == TOK_STAR || m_cur.kind == TOK_SLASH || m_cur.kind == TOK_PERCENT) {
        Token op = eat();
        AstNode* rhs = parseUnary();
        AstNode* bin = makeBinary(AST_BinaryOp, lhs, rhs,
                                  SpanUnion(lhs->span, rhs ? rhs->span : op.span));
        switch (op.kind) {
            case TOK_STAR:    bin->text = Core::StringRef("*", 1); break;
            case TOK_SLASH:   bin->text = Core::StringRef("/", 1); break;
            case TOK_PERCENT: bin->text = Core::StringRef("%", 1); break;
            default: break;
        }
        lhs = bin;
    }
    return lhs;
}

AstNode* Parser::parseUnary() {
    if (m_cur.kind == TOK_BANG || m_cur.kind == TOK_KW_NOT || m_cur.kind == TOK_MINUS) {
        Token op = eat();
        AstNode* operand = parseUnary();
        if (!operand) return 0;
        return makeUnary(AST_UnaryOp, operand,
                         SpanUnion(op.span, operand->span),
                         op.lexeme);
    }
    return parsePrimary();
}

AstNode* Parser::parsePrimary() {
    Token t = m_cur;
    switch (t.kind) {
        case TOK_INT: {
            eat();
            AstNode* n = makeNode(AST_IntLit, t.span);
            n->intVal = t.value.i;
            return n;
        }
        case TOK_FLOAT: {
            eat();
            AstNode* n = makeNode(AST_FloatLit, t.span);
            n->floatVal = t.value.d;
            return n;
        }
        case TOK_STRING: {
            eat();
            AstNode* n = makeNode(AST_StringLit, t.span);
            Core::StringRef body = t.lexeme;
            if (body.size() >= 2) body = Core::StringRef(body.data() + 1, body.size() - 2);
            n->text = body;
            return n;
        }
        case TOK_KW_TRUE: case TOK_KW_FALSE: {
            eat();
            AstNode* n = makeNode(AST_BoolLit, t.span);
            n->boolVal = (t.kind == TOK_KW_TRUE);
            return n;
        }
        case TOK_LPAREN: {
            eat();
            AstNode* inner = parseExpr();
            expect(TOK_RPAREN, Codes::kE_ExpectedToken, "expected ')'");
            return inner;
        }
        case TOK_KW_IF: return parseIfExpr();
        case TOK_IDENTIFIER: {
            eat();
            AstNode* id = makeNode(AST_Ident, t.span);
            id->text = t.lexeme;
            // Optional call suffix or field access chain.
            for (;;) {
                if (m_cur.kind == TOK_LPAREN) {
                    id = parseCallSuffix(id);
                    if (!id) return 0;
                } else if (m_cur.kind == TOK_DOT) {
                    eat();
                    if (m_cur.kind != TOK_IDENTIFIER) {
                        if (m_diags) {
                            m_diags->error(Codes::kE_ExpectedExpr, m_cur.span,
                                "expected field name after '.'");
                        }
                        return 0;
                    }
                    Token fld = eat();
                    AstNode* fa = makeNode(AST_FieldAccess, SpanUnion(id->span, fld.span));
                    fa->lhs = id;
                    fa->text = fld.lexeme;
                    id = fa;
                } else break;
            }
            return id;
        }
        default:
            if (m_diags) {
                std::string s = "expected an expression, got ";
                s += TokenKindName(t.kind);
                m_diags->error(Codes::kE_ExpectedExpr, t.span, s);
            }
            return 0;
    }
}

AstNode* Parser::parseCallSuffix(AstNode* callee) {
    if (m_cur.kind != TOK_LPAREN) return callee;
    eat();
    static const i32 kCap = 16;
    AstNode** args = (AstNode**)m_arena->alloc(sizeof(AstNode*) * kCap, alignof(AstNode*));
    i32 n = 0;
    if (m_cur.kind != TOK_RPAREN) {
        for (;;) {
            AstNode* a = parseExpr();
            if (a && n < kCap) args[n++] = a;
            if (!accept(TOK_COMMA)) break;
        }
    }
    expect(TOK_RPAREN, Codes::kE_ExpectedToken, "expected ')' to close call");
    AstNode* call = makeNode(AST_Call, SpanUnion(callee->span, m_cur.span));
    call->lhs = callee;
    call->text = callee->text;
    if (n > 0) {
        AstNode** dst = (AstNode**)m_arena->alloc(sizeof(AstNode*) * (size_t)n, alignof(AstNode*));
        std::memcpy(dst, args, sizeof(AstNode*) * (size_t)n);
        call->children   = dst;
        call->childCount = n;
    }
    return call;
}

AstNode* Parser::parseIfExpr() {
    SourceSpan begin = m_cur.span;
    eat(); // 'if'
    expect(TOK_LPAREN, Codes::kE_ExpectedToken, "expected '(' after 'if'");
    AstNode* cond = parseExpr();
    expect(TOK_RPAREN, Codes::kE_ExpectedToken, "expected ')' after if-condition");
    AstNode* th = parseExpr();
    AstNode* el = 0;
    if (accept(TOK_KW_ELSE)) {
        el = parseExpr();
    }
    AstNode* node = makeNode(AST_If, SpanUnion(begin,
        el ? el->span : (th ? th->span : cond->span)));
    node->cond  = cond;
    node->thenB = th;
    node->elseB = el;
    return node;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
