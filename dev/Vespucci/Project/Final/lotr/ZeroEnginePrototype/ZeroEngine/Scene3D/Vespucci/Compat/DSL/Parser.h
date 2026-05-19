// Parser.h
// =============================================================================
// PARSER — RECURSIVE-DESCENT, ARENA-BACKED, NO BACKTRACKING BULLSHIT
// =============================================================================
// Written by: Eriumsss
//
// Pulls tokens from Lexer, builds AST nodes into the supplied arena.
// Errors go to the Diagnostic bag. We do NOT throw exceptions in this
// codebase — the editor pulls in the C++ runtime EH machinery already
// for the rest of the engine but we keep the DSL pipeline exception-
// free so a malformed rule cannot unwind into the renderer's frame
// state and explode something downstream.
//
// Recovery strategy: when we hit an error, we skip tokens until the
// next obvious resync point ('}', ';', start of 'rule'/'allow'/'deny')
// then keep parsing. That gets us multiple errors per file in one
// shot instead of "fix one, run again, fix the next, run again, ...".
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_PARSER_H_
#define VESPUCCI_COMPAT_DSL_PARSER_H_

#include "Lexer.h"
#include "Ast.h"
#include "Diagnostics.h"

namespace Vespucci {
namespace Core { class Arena; }
namespace Compat {
namespace DSL {

class Parser {
public:
    Parser();

    // Parse `source` into an AST module rooted at the returned node.
    // Returns NULL on hard failure (no source / no arena). Soft errors
    // accumulate in `diags` and may produce a partial tree.
    AstNode* parse(const char* source, usize length,
                    Core::Arena* arena,
                    DiagnosticBag* diags);

private:
    // Top-level
    AstNode* parseModule();
    AstNode* parseTopStmt();
    AstNode* parseRuleBlock();
    AstNode* parseAllowOrDeny(bool allow);
    AstNode* parseRequireStmt();
    AstNode* parseWeightStmt();
    AstNode* parseReasonStmt();

    // Wire spec & refs
    AstNode* parseWireSpec();
    AstNode* parseTypeOrWildcard();
    AstNode* parseEventOrWildcard();
    AstNode* parseActionOrWildcard();

    // Expressions
    AstNode* parseExpr();
    AstNode* parseOr();
    AstNode* parseAnd();
    AstNode* parseEquality();
    AstNode* parseComparison();
    AstNode* parseAddSub();
    AstNode* parseMulDiv();
    AstNode* parseUnary();
    AstNode* parsePrimary();
    AstNode* parseCallSuffix(AstNode* callee);
    AstNode* parseIfExpr();

    // Helpers
    bool     accept(TokenKind k);
    bool     expect(TokenKind k, i32 errCode, const char* msg);
    Token    eat();
    void     resyncToTopLevel();
    void     resyncToRuleEnd();

    // Allocate a node into the arena.
    AstNode* makeNode(AstKind k, SourceSpan span);
    AstNode* makeBinary(AstKind k, AstNode* lhs, AstNode* rhs, SourceSpan span);
    AstNode* makeUnary(AstKind k, AstNode* operand, SourceSpan span,
                        const Core::StringRef& opText);

    Lexer           m_lex;
    Token           m_cur;
    Core::Arena*    m_arena;
    DiagnosticBag*  m_diags;
};

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_PARSER_H_
