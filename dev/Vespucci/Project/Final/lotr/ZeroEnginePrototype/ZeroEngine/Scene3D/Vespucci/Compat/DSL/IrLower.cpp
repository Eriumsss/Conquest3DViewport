// IrLower.cpp
// =============================================================================
// AST -> IRProgram. One rule = one program. Walks the AST top-down,
// emits opcodes into program.ops and constants into the pools.
// =============================================================================
// Written by: Eriumsss

#include "Ast.h"
#include "Ir.h"
#include "Diagnostics.h"

#include "../../Core/VespucciAssert.h"

#include <cstring>
#include <string>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    i32 InternFloat(IRProgram& p, f64 v) {
        for (i32 i = 0; i < (i32)p.floatPool.size(); ++i) {
            if (p.floatPool[i] == v) return i;
        }
        p.floatPool.push_back(v);
        return (i32)p.floatPool.size() - 1;
    }

    i32 InternString(IRProgram& p, const Core::StringRef& s) {
        std::string copy(s.data(), s.size());
        for (i32 i = 0; i < (i32)p.stringPool.size(); ++i) {
            if (p.stringPool[i] == copy) return i;
        }
        p.stringPool.push_back(copy);
        return (i32)p.stringPool.size() - 1;
    }

    void Emit0(IRProgram& p, IROp op) {
        p.ops.push_back(EncodeOpAndImm(op, 0));
    }
    void EmitImm(IRProgram& p, IROp op, u32 imm) {
        p.ops.push_back(EncodeOpAndImm(op, imm));
    }

    StdlibId LookupStdlib(const Core::StringRef& name) {
        char buf[64];
        usize n = name.size();
        if (n + 1 > sizeof(buf)) n = sizeof(buf) - 1;
        for (usize i = 0; i < n; ++i) {
            char c = name.data()[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            buf[i] = c;
        }
        buf[n] = 0;
        if (std::strcmp(buf, "samelayer")     == 0) return SL_SAME_LAYER;
        if (std::strcmp(buf, "sameparent")    == 0) return SL_SAME_PARENT;
        if (std::strcmp(buf, "distlessthan")  == 0) return SL_DIST_LESS_THAN;
        if (std::strcmp(buf, "spatial")       == 0) return SL_SPATIAL;
        if (std::strcmp(buf, "deprecated")    == 0) return SL_DEPRECATED;
        if (std::strcmp(buf, "haseventcount") == 0) return SL_HAS_EVENT_COUNT;
        if (std::strcmp(buf, "hastrait")      == 0) return SL_HAS_TRAIT;
        if (std::strcmp(buf, "namematches")   == 0) return SL_NAME_MATCHES;
        if (std::strcmp(buf, "min")           == 0) return SL_MIN;
        if (std::strcmp(buf, "max")           == 0) return SL_MAX;
        if (std::strcmp(buf, "abs")           == 0) return SL_ABS;
        if (std::strcmp(buf, "floor")         == 0) return SL_FLOOR;
        if (std::strcmp(buf, "ceil")          == 0) return SL_CEIL;
        return (StdlibId)0;
    }

    void LowerExpr(IRProgram& p, AstNode* e);

    void LowerCall(IRProgram& p, AstNode* call) {
        if (!call) return;
        // Args first, left-to-right.
        for (i32 i = 0; i < call->childCount; ++i) LowerExpr(p, call->children[i]);
        StdlibId id = LookupStdlib(call->text);
        // Encode (id, argc) into a single instruction's imm: high 16 = id, low 16 = argc.
        u32 imm = ((u32)id << 16) | ((u32)call->childCount & 0xFFFFu);
        EmitImm(p, OP_CALL, imm);
    }

    void LowerExpr(IRProgram& p, AstNode* e) {
        if (!e) return;
        switch (e->kind) {
            case AST_BoolLit:
                EmitImm(p, OP_PUSH_BOOL, e->boolVal ? 1u : 0u);
                break;
            case AST_IntLit:
                EmitImm(p, OP_PUSH_INT, (u32)(i32)e->intVal);
                break;
            case AST_FloatLit: {
                i32 idx = InternFloat(p, e->floatVal);
                EmitImm(p, OP_PUSH_FLOAT, (u32)idx);
                break;
            }
            case AST_StringLit: {
                i32 idx = InternString(p, e->text);
                EmitImm(p, OP_PUSH_STR, (u32)idx);
                break;
            }
            case AST_Ident: {
                // Bare ident => zero-arg stdlib call.
                StdlibId id = LookupStdlib(e->text);
                if (id != 0) {
                    u32 imm = ((u32)id << 16) | 0u;
                    EmitImm(p, OP_CALL, imm);
                } else {
                    // Unknown bare ident (sema should have caught it).
                    Emit0(p, OP_NOP);
                }
                break;
            }
            case AST_Call:
                LowerCall(p, e);
                break;
            case AST_BinaryOp: {
                LowerExpr(p, e->lhs);
                LowerExpr(p, e->rhs);
                const char* op = e->text.data();
                if (e->text.size() == 0) { Emit0(p, OP_NOP); break; }
                if (e->text.size() == 1) {
                    switch (op[0]) {
                        case '+': Emit0(p, OP_ADD); break;
                        case '-': Emit0(p, OP_SUB); break;
                        case '*': Emit0(p, OP_MUL); break;
                        case '/': Emit0(p, OP_DIV); break;
                        case '%': Emit0(p, OP_MOD); break;
                        case '<': Emit0(p, OP_LT);  break;
                        case '>': Emit0(p, OP_GT);  break;
                        default:  Emit0(p, OP_NOP); break;
                    }
                } else if (e->text.size() == 2 && op[0] == '<' && op[1] == '=') Emit0(p, OP_LE);
                else if   (e->text.size() == 2 && op[0] == '>' && op[1] == '=') Emit0(p, OP_GE);
                else if   (e->text.size() == 2 && op[0] == '=' && op[1] == '=') Emit0(p, OP_EQ);
                else if   (e->text.size() == 2 && op[0] == '!' && op[1] == '=') Emit0(p, OP_NEQ);
                else if   (e->text.size() == 2 && op[0] == 'o' && op[1] == 'r') Emit0(p, OP_OR);
                else if   (e->text.size() == 3 && op[0] == 'a' && op[1] == 'n' && op[2] == 'd') Emit0(p, OP_AND);
                else      Emit0(p, OP_NOP);
                break;
            }
            case AST_UnaryOp: {
                LowerExpr(p, e->lhs);
                const char* op = e->text.data();
                if (e->text.size() == 1 && op[0] == '!') Emit0(p, OP_NOT);
                else if (e->text.size() == 3 && op[0] == 'n' && op[1] == 'o' && op[2] == 't') Emit0(p, OP_NOT);
                else if (e->text.size() == 1 && op[0] == '-') Emit0(p, OP_NEG);
                else Emit0(p, OP_NOP);
                break;
            }
            case AST_If: {
                // condition; jump_if_false to else; then; jump end; else; end:
                LowerExpr(p, e->cond);
                i32 jumpFalseAt = (i32)p.ops.size();
                EmitImm(p, OP_JUMP_IF_F, 0u);
                LowerExpr(p, e->thenB);
                i32 jumpEndAt = (i32)p.ops.size();
                EmitImm(p, OP_JUMP, 0u);
                i32 elseStart = (i32)p.ops.size();
                if (e->elseB) LowerExpr(p, e->elseB);
                i32 endPos = (i32)p.ops.size();
                // Patch jumps.
                p.ops[jumpFalseAt] = EncodeOpAndImm(OP_JUMP_IF_F, (u32)(elseStart - jumpFalseAt - 1));
                p.ops[jumpEndAt]   = EncodeOpAndImm(OP_JUMP,      (u32)(endPos    - jumpEndAt    - 1));
                break;
            }
            default: break;
        }
    }

    void LowerWireSpec(IRProgram& p, AstNode* spec) {
        if (!spec || spec->childCount < 4) return;
        AstNode* src    = spec->children[0];
        AstNode* event  = spec->children[1];
        AstNode* tgt    = spec->children[2];
        AstNode* action = spec->children[3];
        // Const-pool the four name strings (or 0 for wildcards).
        i32 sIdx = InternString(p, src->text);
        i32 eIdx = InternString(p, event->text);
        i32 tIdx = InternString(p, tgt->text);
        i32 aIdx = InternString(p, action->text);
        // Pack four 8-bit pool indices. Pool > 255 entries per rule
        // is not realistic; we assert in case.
        VESPUCCI_ASSERT(sIdx < 64 && eIdx < 64 && tIdx < 64 && aIdx < 64,
            "wire-spec const-pool index overflow (rule too big)");
        u32 imm = ((u32)sIdx & 63u) | (((u32)eIdx & 63u) << 6) |
                  (((u32)tIdx & 63u) << 12) | (((u32)aIdx & 63u) << 18);
        EmitImm(p, OP_MATCH_WIRE, imm);
        p.hasSpec = true;
    }
} // namespace

bool LowerRuleToIR(AstNode* ruleAST, IRProgram& outProg) {
    outProg = IRProgram();
    outProg.hasSpec = false;
    outProg.reasonStringIdx = -1;
    outProg.weightBonus = 0.0f;
    if (!ruleAST) return false;

    if (ruleAST->kind == AST_Rule) {
        outProg.name = ruleAST->text;
        for (i32 i = 0; i < ruleAST->childCount; ++i) {
            AstNode* s = ruleAST->children[i];
            if (!s) continue;
            switch (s->kind) {
                case AST_AllowStmt:
                    LowerWireSpec(outProg, s->lhs);
                    if (s->thenB && s->thenB->kind == AST_ReasonStmt) {
                        outProg.reasonStringIdx = InternString(outProg, s->thenB->text);
                    }
                    if (s->elseB && s->elseB->kind == AST_WeightStmt) {
                        AstNode* w = s->elseB->lhs;
                        if (w && w->kind == AST_FloatLit) outProg.weightBonus = (f32)w->floatVal;
                        if (w && w->kind == AST_IntLit)   outProg.weightBonus = (f32)w->intVal;
                    }
                    Emit0(outProg, OP_RES_ALLOW);
                    break;
                case AST_DenyStmt:
                    LowerWireSpec(outProg, s->lhs);
                    if (s->thenB && s->thenB->kind == AST_ReasonStmt) {
                        outProg.reasonStringIdx = InternString(outProg, s->thenB->text);
                    }
                    Emit0(outProg, OP_RES_DENY);
                    break;
                case AST_RequireStmt:
                    LowerExpr(outProg, s->lhs);
                    EmitImm(outProg, OP_JUMP_IF_F, 1u); // skip next op if false
                    break;
                case AST_WeightStmt: {
                    LowerExpr(outProg, s->lhs);
                    Emit0(outProg, OP_RES_WEIGHT);
                    break;
                }
                case AST_ReasonStmt: {
                    i32 idx = InternString(outProg, s->text);
                    EmitImm(outProg, OP_RES_REASON, (u32)idx);
                    break;
                }
                default: break;
            }
        }
    } else if (ruleAST->kind == AST_AllowStmt || ruleAST->kind == AST_DenyStmt) {
        LowerWireSpec(outProg, ruleAST->lhs);
        if (ruleAST->thenB && ruleAST->thenB->kind == AST_ReasonStmt) {
            outProg.reasonStringIdx = InternString(outProg, ruleAST->thenB->text);
        }
        Emit0(outProg, ruleAST->kind == AST_AllowStmt ? OP_RES_ALLOW : OP_RES_DENY);
    }

    Emit0(outProg, OP_RETURN);
    return true;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
