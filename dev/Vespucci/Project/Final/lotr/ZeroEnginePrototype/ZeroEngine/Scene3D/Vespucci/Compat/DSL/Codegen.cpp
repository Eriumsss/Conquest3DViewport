// Codegen.cpp
// =============================================================================
// Public entry: lower a parsed Module AST into a vector of IRProgram.
// Most of the actual lowering logic lives in IrLower.cpp; this file is
// the orchestrator that walks the module's top-level rules / bare
// allow-deny statements and produces one IRProgram per output.
// =============================================================================
// Written by: Eriumsss

#include "Ast.h"
#include "Ir.h"

#include "../../Core/Logging.h"

#include <vector>

namespace Vespucci {
namespace Compat {
namespace DSL {

// Defined in IrLower.cpp (same translation unit pair).
bool LowerRuleToIR(AstNode* ruleAST, IRProgram& outProg);

i32 LowerModule(AstNode* module, std::vector<IRProgram>& outPrograms) {
    if (!module || module->kind != AST_Module) return 0;
    i32 added = 0;
    for (i32 i = 0; i < module->childCount; ++i) {
        AstNode* c = module->children[i];
        if (!c) continue;
        if (c->kind == AST_Rule || c->kind == AST_AllowStmt || c->kind == AST_DenyStmt) {
            IRProgram p;
            if (LowerRuleToIR(c, p)) {
                outPrograms.push_back(p);
                added++;
            }
        }
    }
    Core::Logging::Info("Compat DSL: lowered %d rule program%s",
        added, added == 1 ? "" : "s");
    return added;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
