// Ast.h
// =============================================================================
// AST NODES — THE INTERMEDIATE FORM BEFORE WE LOWER TO IR
// =============================================================================
// Written by: Eriumsss
//
// We could lex straight to bytecode and skip the AST. We do not, for
// two reasons:
//   1. Sema runs over the AST. Type-checking, variable scoping,
//      duplicate-rule detection, deprecated-type warnings — those
//      need a tree, not a stream.
//   2. AstPrinter regenerates source from the AST so we can round-
//      trip a parsed file and confirm we did not lose anything. That
//      is the cheap built-in fuzz test for the parser.
//
// Nodes are POD-shaped. Children live in arena-allocated arrays
// pointed to by the parent. The AST owns nothing — the AstArena
// passed into the parser owns everything. When the arena dies, the
// AST dies, and there is nothing to leak.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_AST_H_
#define VESPUCCI_COMPAT_DSL_AST_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"
#include "Token.h"

namespace Vespucci {
namespace Core { class Arena; }
namespace Compat {
namespace DSL {

enum AstKind {
    AST_None        = 0,

    // Top-level
    AST_Module      = 1,
    AST_Rule        = 2,
    AST_AllowStmt   = 3,
    AST_DenyStmt    = 4,
    AST_RequireStmt = 5,
    AST_WeightStmt  = 6,
    AST_ReasonStmt  = 7,

    // Wire spec components
    AST_TypeRef     = 10,
    AST_EventRef    = 11,
    AST_ActionRef   = 12,
    AST_WireSpec    = 13,    // src.event -> tgt.action  (with optional wildcards)

    // Expressions
    AST_BoolLit     = 30,
    AST_IntLit      = 31,
    AST_FloatLit    = 32,
    AST_StringLit   = 33,
    AST_Ident       = 34,
    AST_Call        = 35,
    AST_BinaryOp    = 36,
    AST_UnaryOp     = 37,
    AST_FieldAccess = 38,
    AST_If          = 39
};

struct AstNode;

// Forward-declared variant fields. The kind tag dispatches which
// fields are valid; everything else stays zero. C++ unions of POD
// trees are fine here — no destructors fire on AstNode itself, and
// arena owns all storage.
struct AstNode {
    AstKind     kind;
    SourceSpan  span;

    // Common fields used by multiple node kinds.
    Core::StringRef text;       // identifier name, string literal body, op spelling
    i64             intVal;
    f64             floatVal;
    bool            boolVal;

    // Children — pointer + count. Arena-allocated arrays. Specific
    // kinds use specific slots; see AstPrinter / Sema for the per-kind
    // grammar. Slot count cap is intentionally loose; the parser
    // checks per-node.
    AstNode**       children;
    i32             childCount;

    // Convenience: per-kind structured pointers that aliases into
    // children[]. Parser sets them on node construction.
    AstNode*        lhs;        // BinaryOp / FieldAccess / WireSpec.src
    AstNode*        rhs;        // BinaryOp / WireSpec.tgt
    AstNode*        cond;       // If
    AstNode*        thenB;      // If
    AstNode*        elseB;      // If

    // For Call: function name lives in `text`; arguments in children[].
    // For TypeRef / EventRef / ActionRef: name lives in `text`. The
    // type may be wildcarded ('*' lexeme).
};

// Builder helpers — used by the parser. Allocate from the supplied
// arena. childCount may be 0; children pointer is arena-owned.
AstNode* AstNew(Core::Arena* arena, AstKind kind, SourceSpan span);
AstNode* AstNewWithChildren(Core::Arena* arena, AstKind kind, SourceSpan span,
                             AstNode** kids, i32 n);

// Pretty-print an AST back to compat-DSL source. Used by AstPrinter
// for round-trip fuzz tests.
void AstPrint(const AstNode* root, char* out, usize outCap);

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_AST_H_
