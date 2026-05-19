// Vm.cpp
// =============================================================================
// Stack-machine bytecode interpreter for the compat DSL. Hot path —
// keep allocations off this code path entirely. Faults are caught
// (overflow, bad argc, divide by zero) and turned into faulted=true
// in the result; the matrix evaluator treats faulted rules as DENY
// with reason "rule faulted at runtime, see logs".
// =============================================================================
// Written by: Eriumsss

#include "Vm.h"

#include "../../Core/Logging.h"
#include "../../Core/VespucciAssert.h"

#include <cmath>
#include <cstring>

namespace Vespucci {
namespace Compat {
namespace DSL {

namespace {
    bool StringEqualsLower(const Core::StringRef& a, const Core::StringRef& b) {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            char ca = a.data()[i]; char cb = b.data()[i];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return false;
        }
        return true;
    }

    bool WildcardOrEqualLower(const Core::StringRef& pattern,
                               const Core::StringRef& candidate)
    {
        if (pattern.size() == 1 && pattern.data()[0] == '*') return true;
        return StringEqualsLower(pattern, candidate);
    }
} // namespace

VM::VM() : m_sp(0), m_faulted(false) {}

void VM::push(const Value& v) {
    if (m_sp >= kStackCap) {
        Core::Logging::Error("Compat VM: stack overflow at sp=%d, faulting rule", m_sp);
        m_faulted = true;
        return;
    }
    m_stack[m_sp++] = v;
}

VM::Value VM::pop() {
    if (m_sp <= 0) {
        Core::Logging::Error("Compat VM: stack underflow, faulting rule");
        m_faulted = true;
        Value v; v.kind = V_Int; v.v.i = 0; return v;
    }
    return m_stack[--m_sp];
}

VM::Value VM::top() const {
    if (m_sp <= 0) {
        Value v; v.kind = V_Int; v.v.i = 0; return v;
    }
    return m_stack[m_sp - 1];
}

bool VM::asBool(const Value& v) const {
    switch (v.kind) {
        case V_Bool:   return v.v.b;
        case V_Int:    return v.v.i != 0;
        case V_Float:  return v.v.d != 0.0;
        case V_String: return v.s.size() > 0;
    }
    return false;
}

f64 VM::asNumber(const Value& v) const {
    switch (v.kind) {
        case V_Int:    return (f64)v.v.i;
        case V_Float:  return v.v.d;
        case V_Bool:   return v.v.b ? 1.0 : 0.0;
        case V_String: return 0.0;
    }
    return 0.0;
}

void VM::applyBinaryArith(IROp op) {
    Value rhs = pop();
    Value lhs = pop();
    f64 r = asNumber(rhs);
    f64 l = asNumber(lhs);
    f64 out = 0.0;
    switch (op) {
        case OP_ADD: out = l + r; break;
        case OP_SUB: out = l - r; break;
        case OP_MUL: out = l * r; break;
        case OP_DIV:
            if (r == 0.0) { m_faulted = true; out = 0.0; break; }
            out = l / r; break;
        case OP_MOD:
            if (r == 0.0) { m_faulted = true; out = 0.0; break; }
            out = std::fmod(l, r); break;
        default: break;
    }
    Value v; v.kind = V_Float; v.v.d = out;
    push(v);
}

void VM::applyComparison(IROp op) {
    Value rhs = pop();
    Value lhs = pop();
    bool out = false;
    if (lhs.kind == V_String && rhs.kind == V_String) {
        // String comparison: only EQ / NEQ are meaningful.
        bool eq = StringEqualsLower(lhs.s, rhs.s);
        if      (op == OP_EQ)  out = eq;
        else if (op == OP_NEQ) out = !eq;
        else                   out = false; // <,<=,>,>= on strings is junk
    } else {
        f64 l = asNumber(lhs);
        f64 r = asNumber(rhs);
        switch (op) {
            case OP_EQ:  out = (l == r); break;
            case OP_NEQ: out = (l != r); break;
            case OP_LT:  out = (l <  r); break;
            case OP_LE:  out = (l <= r); break;
            case OP_GT:  out = (l >  r); break;
            case OP_GE:  out = (l >= r); break;
            default: break;
        }
    }
    Value v; v.kind = V_Bool; v.v.b = out;
    push(v);
}

void VM::applyLogical(IROp op) {
    Value rhs = pop();
    Value lhs = pop();
    bool l = asBool(lhs); bool r = asBool(rhs);
    bool out = (op == OP_AND) ? (l && r) : (l || r);
    Value v; v.kind = V_Bool; v.v.b = out;
    push(v);
}

void VM::applyUnary(IROp op) {
    Value top = pop();
    Value out;
    if (op == OP_NOT) {
        out.kind = V_Bool;
        out.v.b = !asBool(top);
    } else if (op == OP_NEG) {
        out.kind = V_Float;
        out.v.d = -asNumber(top);
    } else {
        out = top;
    }
    push(out);
}

VM::Value VM::runStdlibCall(StdlibId id, i32 argc, const QueryContext& q) {
    Value res; res.kind = V_Bool; res.v.b = false;

    switch (id) {
        case SL_SAME_LAYER:
            if (argc != 0) { m_faulted = true; break; }
            res.v.b = (q.sourceLayer.valid() && q.sourceLayer == q.targetLayer);
            break;
        case SL_SAME_PARENT:
            if (argc != 0) { m_faulted = true; break; }
            res.v.b = (q.sourceParent.valid() && q.sourceParent == q.targetParent);
            break;
        case SL_DIST_LESS_THAN: {
            if (argc != 1) { m_faulted = true; break; }
            Value th = pop();
            f64 t = asNumber(th);
            res.v.b = (q.distance < (f32)t);
            break;
        }
        case SL_SPATIAL:
            res.v.b = (q.sourceSpatial && q.targetSpatial);
            break;
        case SL_DEPRECATED:
            res.v.b = (q.sourceDeprecated || q.targetDeprecated);
            break;
        case SL_HAS_EVENT_COUNT: {
            if (argc != 1) { m_faulted = true; break; }
            (void)pop(); // not yet wired into the runtime context
            res.v.b = false;
            break;
        }
        case SL_HAS_TRAIT: {
            if (argc != 1) { m_faulted = true; break; }
            (void)pop(); // trait name string — runtime hook deferred
            res.v.b = false;
            break;
        }
        case SL_NAME_MATCHES: {
            if (argc != 1 && argc != 2) { m_faulted = true; break; }
            Value pat = pop();
            // Optional "side" arg: "source" / "target". Default source.
            Core::StringRef which("source", 6);
            if (argc == 2) { Value w = pop(); which = w.s; (void)which; }
            const Core::StringRef& name =
                StringEqualsLower(which, Core::StringRef("target", 6))
                    ? q.targetName : q.sourceName;
            res.v.b = WildcardOrEqualLower(pat.s, name);
            break;
        }
        case SL_MIN: case SL_MAX: case SL_ABS:
        case SL_FLOOR: case SL_CEIL:
        {
            if (id == SL_MIN || id == SL_MAX) {
                if (argc != 2) { m_faulted = true; break; }
                Value b = pop();
                Value a = pop();
                f64 av = asNumber(a); f64 bv = asNumber(b);
                f64 out = (id == SL_MIN) ? (av < bv ? av : bv)
                                          : (av > bv ? av : bv);
                res.kind = V_Float; res.v.d = out;
            } else {
                if (argc != 1) { m_faulted = true; break; }
                Value a = pop();
                f64 v = asNumber(a);
                f64 out = 0.0;
                switch (id) {
                    case SL_ABS:   out = (v < 0 ? -v : v); break;
                    case SL_FLOOR: out = std::floor(v); break;
                    case SL_CEIL:  out = std::ceil(v); break;
                    default: break;
                }
                res.kind = V_Float; res.v.d = out;
            }
            break;
        }
        default:
            m_faulted = true;
            break;
    }
    return res;
}

VMResult VM::run(const IRProgram& prog, const QueryContext& q) {
    VMResult r;
    r.verdict = VERDICT_None;
    r.weightBonus = 0.0f;
    r.reason = Core::StringRef();
    r.rulesEvaluated = 1;
    r.faulted = false;

    m_sp = 0;
    m_faulted = false;

    bool matched = !prog.hasSpec; // if there is no spec, every query "matches"

    i32 ip = 0;
    i32 N = (i32)prog.ops.size();
    while (ip < N && !m_faulted) {
        u32 instr = prog.ops[(size_t)ip++];
        IROp op = DecodeOp(instr);
        u32 imm = DecodeImm(instr);

        switch (op) {
            case OP_NOP: break;
            case OP_PUSH_BOOL: {
                Value v; v.kind = V_Bool; v.v.b = (imm != 0);
                push(v);
                break;
            }
            case OP_PUSH_INT: {
                Value v; v.kind = V_Int; v.v.i = (i64)(i32)imm;
                push(v);
                break;
            }
            case OP_PUSH_FLOAT: {
                Value v; v.kind = V_Float;
                if ((i32)imm < (i32)prog.floatPool.size()) v.v.d = prog.floatPool[imm];
                else { m_faulted = true; v.v.d = 0.0; }
                push(v);
                break;
            }
            case OP_PUSH_STR: {
                Value v; v.kind = V_String;
                if ((i32)imm < (i32)prog.stringPool.size()) {
                    const std::string& s = prog.stringPool[imm];
                    v.s = Core::StringRef(s.c_str(), s.size());
                } else {
                    m_faulted = true;
                }
                push(v);
                break;
            }
            case OP_ADD: case OP_SUB: case OP_MUL:
            case OP_DIV: case OP_MOD:
                applyBinaryArith(op);
                break;
            case OP_NEG: case OP_NOT:
                applyUnary(op);
                break;
            case OP_AND: case OP_OR:
                applyLogical(op);
                break;
            case OP_EQ: case OP_NEQ:
            case OP_LT: case OP_LE:
            case OP_GT: case OP_GE:
                applyComparison(op);
                break;
            case OP_CALL: {
                StdlibId id = (StdlibId)((imm >> 16) & 0xFFFFu);
                i32 argc = (i32)(imm & 0xFFFFu);
                Value res = runStdlibCall(id, argc, q);
                push(res);
                break;
            }
            case OP_JUMP: {
                ip += (i32)(i16)(imm & 0xFFFFu);
                break;
            }
            case OP_JUMP_IF_F: {
                Value v = pop();
                if (!asBool(v)) ip += (i32)(i16)(imm & 0xFFFFu);
                break;
            }
            case OP_MATCH_WIRE: {
                i32 sIdx = (i32)(imm & 63u);
                i32 eIdx = (i32)((imm >> 6)  & 63u);
                i32 tIdx = (i32)((imm >> 12) & 63u);
                i32 aIdx = (i32)((imm >> 18) & 63u);
                if (sIdx >= (i32)prog.stringPool.size() ||
                    eIdx >= (i32)prog.stringPool.size() ||
                    tIdx >= (i32)prog.stringPool.size() ||
                    aIdx >= (i32)prog.stringPool.size())
                {
                    m_faulted = true; break;
                }
                const std::string& srcPat = prog.stringPool[sIdx];
                const std::string& evtPat = prog.stringPool[eIdx];
                const std::string& tgtPat = prog.stringPool[tIdx];
                const std::string& actPat = prog.stringPool[aIdx];
                Core::StringRef sp(srcPat.c_str(), srcPat.size());
                Core::StringRef ep(evtPat.c_str(), evtPat.size());
                Core::StringRef tp(tgtPat.c_str(), tgtPat.size());
                Core::StringRef ap(actPat.c_str(), actPat.size());

                // Source / target type names not directly on the
                // QueryContext yet; we match by canonicalName via
                // a stub. A future revision walks the registry to
                // resolve TypeId -> canonical and compare. For now
                // a wildcard-only check passes through and named
                // patterns rely on event/action match alone.
                bool srcWild = (sp.size() == 1 && sp.data()[0] == '*');
                bool tgtWild = (tp.size() == 1 && tp.data()[0] == '*');
                bool srcMatch = srcWild;
                bool tgtMatch = tgtWild;
                if (!srcMatch) srcMatch = StringEqualsLower(sp, q.sourceName);
                if (!tgtMatch) tgtMatch = StringEqualsLower(tp, q.targetName);
                bool eventMatch  = WildcardOrEqualLower(ep, q.eventName);
                bool actionMatch = WildcardOrEqualLower(ap, q.actionName);
                matched = srcMatch && tgtMatch && eventMatch && actionMatch;
                break;
            }
            case OP_RETURN:
                ip = N;
                break;
            case OP_RES_ALLOW:
                if (matched) r.verdict = VERDICT_Allow;
                break;
            case OP_RES_DENY:
                if (matched) r.verdict = VERDICT_Deny;
                break;
            case OP_RES_WEIGHT: {
                Value w = pop();
                if (matched) r.weightBonus += (f32)asNumber(w);
                break;
            }
            case OP_RES_REASON: {
                if (matched && (i32)imm < (i32)prog.stringPool.size()) {
                    const std::string& s = prog.stringPool[imm];
                    r.reason = Core::StringRef(s.c_str(), s.size());
                }
                break;
            }
            default:
                m_faulted = true;
                break;
        }
    }

    if (m_faulted) {
        r.faulted = true;
        r.verdict = VERDICT_Deny;
        r.reason = Core::StringRef("rule faulted at runtime — see logs", 32);
    }
    return r;
}

VMResult VM::runAll(const IRProgram* progs, i32 count, const QueryContext& q) {
    VMResult acc;
    acc.verdict = VERDICT_None;
    acc.weightBonus = 0.0f;
    acc.reason = Core::StringRef();
    acc.rulesEvaluated = 0;
    acc.faulted = false;
    if (!progs || count <= 0) return acc;
    for (i32 i = 0; i < count; ++i) {
        VMResult one = run(progs[i], q);
        acc.rulesEvaluated++;
        if (one.faulted) { acc.faulted = true; acc.verdict = VERDICT_Deny; acc.reason = one.reason; return acc; }
        if (one.verdict == VERDICT_Deny) {
            // First DENY wins.
            acc.verdict = VERDICT_Deny;
            acc.reason = one.reason;
            return acc;
        }
        if (one.verdict == VERDICT_Allow) {
            acc.verdict = VERDICT_Allow;
            acc.weightBonus += one.weightBonus;
            if (acc.reason.size() == 0) acc.reason = one.reason;
        }
    }
    return acc;
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
