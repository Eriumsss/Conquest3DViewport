// AstPrinter.cpp
// =============================================================================
// AST -> source. Round-trip pretty-printer for parser fuzz tests and
// for the "show me the rule normalized" UI surface in CompatEditor.
// Output is canonical, not byte-identical to input — comments and
// whitespace are gone, identifiers stay verbatim.
// =============================================================================
// Written by: Eriumsss

#include "Ast.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    void Append(std::string& out, const char* s) { out += s; }
    void AppendStr(std::string& out, const Core::StringRef& s) {
        out.append(s.data(), s.size());
    }
    void AppendIndent(std::string& out, i32 d) {
        for (i32 i = 0; i < d; ++i) out += "  ";
    }
    void PrintExpr(std::string& out, const AstNode* e);

    void PrintCall(std::string& out, const AstNode* call) {
        if (!call) return;
        AppendStr(out, call->text);
        out.push_back('(');
        for (i32 i = 0; i < call->childCount; ++i) {
            if (i > 0) out += ", ";
            PrintExpr(out, call->children[i]);
        }
        out.push_back(')');
    }

    void PrintExpr(std::string& out, const AstNode* e) {
        if (!e) return;
        switch (e->kind) {
            case AST_BoolLit:
                out += e->boolVal ? "true" : "false";
                break;
            case AST_IntLit: {
                char buf[32]; std::snprintf(buf, sizeof(buf), "%lld", (long long)e->intVal);
                out += buf;
                break;
            }
            case AST_FloatLit: {
                char buf[64]; std::snprintf(buf, sizeof(buf), "%g", e->floatVal);
                out += buf;
                break;
            }
            case AST_StringLit:
                out.push_back('"');
                AppendStr(out, e->text);
                out.push_back('"');
                break;
            case AST_Ident:
                AppendStr(out, e->text);
                break;
            case AST_Call:
                PrintCall(out, e);
                break;
            case AST_BinaryOp:
                out.push_back('(');
                PrintExpr(out, e->lhs);
                out.push_back(' ');
                AppendStr(out, e->text);
                out.push_back(' ');
                PrintExpr(out, e->rhs);
                out.push_back(')');
                break;
            case AST_UnaryOp:
                AppendStr(out, e->text);
                PrintExpr(out, e->lhs);
                break;
            case AST_FieldAccess:
                PrintExpr(out, e->lhs);
                out.push_back('.');
                AppendStr(out, e->text);
                break;
            case AST_If:
                out += "if (";
                PrintExpr(out, e->cond);
                out += ") ";
                PrintExpr(out, e->thenB);
                if (e->elseB) {
                    out += " else ";
                    PrintExpr(out, e->elseB);
                }
                break;
            default: break;
        }
    }

    void PrintWireSpec(std::string& out, const AstNode* spec) {
        if (!spec || spec->childCount < 4) return;
        AppendStr(out, spec->children[0]->text);
        out.push_back('.');
        AppendStr(out, spec->children[1]->text);
        out += " -> ";
        AppendStr(out, spec->children[2]->text);
        out.push_back('.');
        AppendStr(out, spec->children[3]->text);
    }

    void PrintRuleStmt(std::string& out, const AstNode* s, i32 d) {
        if (!s) return;
        AppendIndent(out, d);
        switch (s->kind) {
            case AST_AllowStmt:
                out += "allow ";
                PrintWireSpec(out, s->lhs);
                if (s->thenB && s->thenB->kind == AST_ReasonStmt) {
                    out += " with reason \"";
                    AppendStr(out, s->thenB->text);
                    out.push_back('"');
                }
                break;
            case AST_DenyStmt:
                out += "deny ";
                PrintWireSpec(out, s->lhs);
                if (s->thenB && s->thenB->kind == AST_ReasonStmt) {
                    out += " with reason \"";
                    AppendStr(out, s->thenB->text);
                    out.push_back('"');
                }
                break;
            case AST_RequireStmt:
                out += "require ";
                PrintExpr(out, s->lhs);
                break;
            case AST_WeightStmt:
                out += "weight ";
                PrintExpr(out, s->lhs);
                break;
            case AST_ReasonStmt:
                out += "reason \"";
                AppendStr(out, s->text);
                out.push_back('"');
                break;
            default: break;
        }
        out.push_back('\n');
    }
} // namespace

void AstPrint(const AstNode* root, char* outBuf, usize outCap) {
    if (!root || !outBuf || outCap == 0) return;
    std::string out;
    out.reserve(256);

    if (root->kind == AST_Module) {
        for (i32 i = 0; i < root->childCount; ++i) {
            const AstNode* c = root->children[i];
            if (!c) continue;
            if (c->kind == AST_Rule) {
                out += "rule";
                if (c->text.size() > 0) {
                    out += " \"";
                    AppendStr(out, c->text);
                    out.push_back('"');
                }
                out += " {\n";
                for (i32 j = 0; j < c->childCount; ++j) PrintRuleStmt(out, c->children[j], 1);
                out += "}\n\n";
            } else {
                PrintRuleStmt(out, c, 0);
            }
        }
    } else {
        PrintRuleStmt(out, root, 0);
    }

    usize n = out.size();
    if (n + 1 > outCap) n = outCap - 1;
    std::memcpy(outBuf, out.data(), n);
    outBuf[n] = 0;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
