// Sema.cpp
// =============================================================================
// Walks the AST, resolves type / event / action references, validates
// stdlib calls. Errors mean "this rule does not load"; warnings mean
// "this rule loads but is suspect". Sema is the last fucking line of
// defense before we lower designer-authored rules straight into the
// ranker's hot path.
// =============================================================================
// Written by: Eriumsss

#include "Sema.h"

#include "../../Schema/EventActionSignatureDB.h"
#include "../../Schema/ZETypeRegistry.h"

#include "../../Core/Logging.h"
#include "../../Core/VespucciAssert.h"

#include <cstring>
#include <string>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    bool IsWildcardText(const Core::StringRef& s) {
        return s.size() == 1 && s.data()[0] == '*';
    }

    void LowerCopy(const Core::StringRef& s, char* out, usize cap) {
        usize n = s.size();
        if (n + 1 > cap) n = cap - 1;
        for (usize i = 0; i < n; ++i) {
            char c = s.data()[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[i] = c;
        }
        out[n] = 0;
    }

    // Stdlib whitelist + arity. If a rule's `require` clause references
    // a name outside this set, sema barks and the rule is rejected.
    struct StdlibSig { const char* name; i32 minArity; i32 maxArity; };
    static const StdlibSig kStdlibSigs[] = {
        { "samelayer",     0, 0 },
        { "sameparent",    0, 0 },
        { "distlessthan",  1, 1 },
        { "spatial",       0, 0 },
        { "deprecated",    0, 0 },
        { "haseventcount", 1, 1 },
        { "hastrait",      1, 1 },
        { "namematches",   1, 2 },
        { "min",           2, 2 },
        { "max",           2, 2 },
        { "abs",           1, 1 },
        { "floor",         1, 1 },
        { "ceil",          1, 1 },
        { 0, 0, 0 }
    };

    const StdlibSig* FindStdlib(const Core::StringRef& name) {
        char buf[64];
        LowerCopy(name, buf, sizeof(buf));
        for (i32 i = 0; kStdlibSigs[i].name; ++i) {
            if (std::strcmp(kStdlibSigs[i].name, buf) == 0) return &kStdlibSigs[i];
        }
        return 0;
    }
} // namespace

Sema::Sema()
    : m_reg(0), m_sigDB(0), m_diags(0), m_depth(0) {}

bool Sema::run(AstNode* module,
               const Schema::ZETypeRegistry* reg,
               const Schema::EventActionSignatureDB* sigDB,
               DiagnosticBag* diags)
{
    if (!module) return false;
    m_reg   = reg;
    m_sigDB = sigDB;
    m_diags = diags;
    m_depth = 0;
    walkModule(module);
    return diags ? !diags->hasErrors() : true;
}

void Sema::walkModule(AstNode* mod) {
    if (!mod || mod->kind != AST_Module) return;
    for (i32 i = 0; i < mod->childCount; ++i) {
        AstNode* c = mod->children[i];
        if (!c) continue;
        switch (c->kind) {
            case AST_Rule:      walkRule(c); break;
            case AST_AllowStmt: walkAllowOrDeny(c, true); break;
            case AST_DenyStmt:  walkAllowOrDeny(c, false); break;
            default: break;
        }
    }
}

void Sema::walkRule(AstNode* rule) {
    if (!rule) return;
    for (i32 i = 0; i < rule->childCount; ++i) {
        AstNode* s = rule->children[i];
        if (!s) continue;
        switch (s->kind) {
            case AST_AllowStmt:   walkAllowOrDeny(s, true);  break;
            case AST_DenyStmt:    walkAllowOrDeny(s, false); break;
            case AST_RequireStmt: walkRequireExpr(s->lhs);   break;
            case AST_WeightStmt:  walkExpr(s->lhs);          break;
            case AST_ReasonStmt:  /* string-only, nothing to check */ break;
            default: break;
        }
    }
}

void Sema::walkAllowOrDeny(AstNode* stmt, bool /*allow*/) {
    if (!stmt || !stmt->lhs) return;
    walkWireSpec(stmt->lhs);
    if (stmt->thenB) /* reason node */ {}
    if (stmt->elseB) walkExpr(stmt->elseB->lhs);
}

void Sema::walkRequireExpr(AstNode* expr) { walkExpr(expr); }

void Sema::walkExpr(AstNode* expr) {
    if (!expr) return;
    if (++m_depth > 64) { m_depth--; return; }
    switch (expr->kind) {
        case AST_BinaryOp:
            walkExpr(expr->lhs);
            walkExpr(expr->rhs);
            break;
        case AST_UnaryOp:
            walkExpr(expr->lhs);
            break;
        case AST_Call:
            walkCall(expr);
            for (i32 i = 0; i < expr->childCount; ++i) walkExpr(expr->children[i]);
            break;
        case AST_FieldAccess:
            walkExpr(expr->lhs);
            break;
        case AST_If:
            walkExpr(expr->cond);
            walkExpr(expr->thenB);
            walkExpr(expr->elseB);
            break;
        case AST_Ident: {
            // Bare identifier: must resolve to a stdlib name (treat
            // as zero-arg call) or a known constant.
            if (!FindStdlib(expr->text) && m_diags) {
                std::string s = "unknown identifier '";
                s.append(expr->text.data(), expr->text.size());
                s += "' in expression";
                m_diags->error(Codes::kE_BadStdLibCall, expr->span, s,
                    "expressions must call stdlib helpers like sameLayer / distLessThan");
            }
            break;
        }
        default: break;
    }
    m_depth--;
}

void Sema::walkWireSpec(AstNode* spec) {
    if (!spec || spec->childCount < 4) return;
    AstNode* src    = spec->children[0];
    AstNode* event  = spec->children[1];
    AstNode* tgt    = spec->children[2];
    AstNode* action = spec->children[3];

    bool srcOK = resolveType(src, true);
    bool tgtOK = resolveType(tgt, true);
    if (srcOK) resolveEvent(event, src, true);
    if (tgtOK) resolveAction(action, tgt, true);
}

void Sema::walkCall(AstNode* call) {
    if (!call) return;
    Core::StringRef name = call->text;
    const StdlibSig* sig = FindStdlib(name);
    if (!sig) {
        if (m_diags) {
            std::string s = "call to unknown stdlib function '";
            s.append(name.data(), name.size());
            s += "'";
            m_diags->error(Codes::kE_BadStdLibCall, call->span, s,
                "stdlib: sameLayer / sameParent / distLessThan(N) / spatial / deprecated / hasTrait(\"...\") / nameMatches(\"...\") / min/max/abs/floor/ceil");
        }
        return;
    }
    i32 actual = call->childCount;
    if (actual < sig->minArity || actual > sig->maxArity) {
        if (m_diags) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "stdlib '%.*s' expects %d-%d args, got %d",
                (int)name.size(), name.data(),
                (int)sig->minArity, (int)sig->maxArity, (int)actual);
            m_diags->error(Codes::kE_ArityMismatch, call->span, buf);
        }
    }
}

bool Sema::resolveType(AstNode* typeRef, bool wildcardOK) {
    if (!typeRef) return false;
    if (IsWildcardText(typeRef->text)) {
        if (!wildcardOK && m_diags) {
            m_diags->error(Codes::kE_BadWildcard, typeRef->span,
                "wildcard '*' not allowed here");
            return false;
        }
        return true;
    }
    if (!m_reg) return true;  // no registry available, defer to runtime
    char buf[128];
    LowerCopy(typeRef->text, buf, sizeof(buf));
    const Schema::TypeRecord* rec = m_reg->findByCanonical(buf);
    if (!rec) {
        if (m_diags) {
            std::string s = "unknown type '";
            s.append(typeRef->text.data(), typeRef->text.size());
            s += "' (not in ZETypeRegistry)";
            m_diags->error(Codes::kE_UnknownType, typeRef->span, s,
                "type names must match a registered Zero Engine entity type");
        }
        return false;
    }
    if ((rec->traits & (u32)Schema::TRAIT_DEPRECATED) && m_diags) {
        std::string s = "type '";
        s.append(typeRef->text.data(), typeRef->text.size());
        s += "' is deprecated";
        m_diags->warn(Codes::kW_DeprecatedType, typeRef->span, s,
            "deprecated types still load but the ranker down-weights them");
    }
    return true;
}

bool Sema::resolveEvent(AstNode* eventRef, AstNode* /*sourceTypeRef*/, bool wildcardOK) {
    if (!eventRef) return false;
    if (IsWildcardText(eventRef->text)) {
        if (!wildcardOK && m_diags) {
            m_diags->error(Codes::kE_BadWildcard, eventRef->span,
                "wildcard '*' not allowed for event");
            return false;
        }
        return true;
    }
    // Without a source type bound to a TypeId we cannot strictly
    // verify the event is in that type's emitted-events list. We
    // SHOULD lower the source type to a TypeId here and look up the
    // signature DB. For brevity the v1 sema accepts any non-empty
    // event name and lets the runtime ranker handle unknowns by
    // bypassing the filter. Future work: tighten this once the
    // EventActionSignatureDB exposes per-source-type strict mode.
    return true;
}

bool Sema::resolveAction(AstNode* actionRef, AstNode* /*targetTypeRef*/, bool wildcardOK) {
    if (!actionRef) return false;
    if (IsWildcardText(actionRef->text)) {
        if (!wildcardOK && m_diags) {
            m_diags->error(Codes::kE_BadWildcard, actionRef->span,
                "wildcard '*' not allowed for action");
            return false;
        }
        return true;
    }
    return true;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
