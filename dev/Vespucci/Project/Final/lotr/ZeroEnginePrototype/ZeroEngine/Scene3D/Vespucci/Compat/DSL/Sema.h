// Sema.h
// =============================================================================
// SEMANTIC PASS — VALIDATE THE AST BEFORE WE LOWER IT TO IR
// =============================================================================
// Written by: Eriumsss
//
// Sema asks: are the types real (registry resolves them), do the
// referenced events / actions exist in the signature DB, are the
// stdlib calls used with valid arities, are there duplicate rule
// names that mask each other, are deny rules unreachable behind
// allow rules with broader patterns, etc. Anything that survives
// sema is safe to lower; anything that doesn't either becomes an
// Error (rule discarded) or a Warning (rule loaded but flagged).
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_SEMA_H_
#define VESPUCCI_COMPAT_DSL_SEMA_H_

#include "../../Core/VespucciTypes.h"
#include "Ast.h"
#include "Diagnostics.h"

namespace Vespucci {
namespace Schema { class ZETypeRegistry; class EventActionSignatureDB; }
namespace Compat {
namespace DSL {

class Sema {
public:
    Sema();

    // Run sema on a parsed Module AST.
    // Returns true if no Error-level diagnostics were emitted.
    bool run(AstNode* module,
             const Schema::ZETypeRegistry* reg,
             const Schema::EventActionSignatureDB* sigDB,
             DiagnosticBag* diags);

private:
    void   walkModule(AstNode* mod);
    void   walkRule(AstNode* rule);
    void   walkAllowOrDeny(AstNode* stmt, bool allow);
    void   walkRequireExpr(AstNode* expr);
    void   walkExpr(AstNode* expr);
    void   walkWireSpec(AstNode* spec);
    void   walkCall(AstNode* call);

    bool   resolveType(AstNode* typeRef, bool wildcardOK);
    bool   resolveEvent(AstNode* eventRef, AstNode* sourceTypeRef, bool wildcardOK);
    bool   resolveAction(AstNode* actionRef, AstNode* targetTypeRef, bool wildcardOK);

    const Schema::ZETypeRegistry*         m_reg;
    const Schema::EventActionSignatureDB* m_sigDB;
    DiagnosticBag*                        m_diags;
    i32                                   m_depth;
};

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_SEMA_H_
