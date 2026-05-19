// Vm.h
// =============================================================================
// VM — RUNTIME EVALUATOR FOR LOWERED COMPAT IR PROGRAMS
// =============================================================================
// Written by: Eriumsss
//
// Stack-machine. One VM instance per evaluation; we do NOT thread
// VMs through the call hierarchy because the editor side is single
// threaded and the program is small enough that VM allocation cost
// is irrelevant (~32 stack slots, pre-reserved).
//
// The VM evaluates one IRProgram against one query context. The
// query context describes the candidate wire being judged:
// (sourceTypeId, eventName, targetTypeId, actionName, sourceGuid,
// targetGuid, sameLayer, sameParent, distance, ...). Stdlib calls
// read fields off the context; pure-arithmetic ops never touch it.
//
// Output: a VMResult that carries (allowed?, weight delta, reason).
// The compat matrix evaluates every rule's program in priority order
// and accumulates the result.
// =============================================================================

#ifndef VESPUCCI_COMPAT_DSL_VM_H_
#define VESPUCCI_COMPAT_DSL_VM_H_

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"
#include "Ir.h"

namespace Vespucci {
namespace Compat {
namespace DSL {

// Query context. Populated by the matrix evaluator before each VM
// run. Stdlib functions read these fields.
struct QueryContext {
    TypeId            sourceType;
    TypeId            targetType;
    Core::StringRef   eventName;     // canonical lowercase
    Core::StringRef   actionName;    // canonical lowercase
    Guid              sourceGuid;
    Guid              targetGuid;
    LayerGuid         sourceLayer;
    LayerGuid         targetLayer;
    Guid              sourceParent;
    Guid              targetParent;
    f32               distance;       // world-space, 0 if unknown
    bool              sourceSpatial;
    bool              targetSpatial;
    bool              sourceDeprecated;
    bool              targetDeprecated;
    Core::StringRef   sourceName;
    Core::StringRef   targetName;
};

enum VerdictKind {
    VERDICT_None  = 0,    // rule did not match this query
    VERDICT_Allow = 1,
    VERDICT_Deny  = 2
};

struct VMResult {
    VerdictKind     verdict;
    f32             weightBonus;
    Core::StringRef reason;     // points into the IR's stringPool
    i32             rulesEvaluated;
    bool            faulted;     // hit a runtime error (overflow, bad arg, etc.)
};

class VM {
public:
    VM();

    // Run one program against a query. Returns the result; never throws.
    VMResult run(const IRProgram& prog, const QueryContext& q);

    // Bulk run: evaluate every program in `progs`, accumulate result.
    // First DENY wins; otherwise highest-weight ALLOW.
    VMResult runAll(const IRProgram* progs, i32 count, const QueryContext& q);

private:
    // Stack — fixed cap, asserts on overflow.
    static const i32 kStackCap = 64;

    // We use a tagged-union value so int / float / bool / string all
    // ride the same stack slot.
    enum ValueKind {
        V_Int    = 0,
        V_Float  = 1,
        V_Bool   = 2,
        V_String = 3
    };
    struct Value {
        ValueKind kind;
        union {
            i64 i;
            f64 d;
            bool b;
        } v;
        Core::StringRef s;
    };

    Value m_stack[kStackCap];
    i32   m_sp;

    void  push(const Value& v);
    Value pop();
    Value top() const;

    bool  asBool(const Value& v) const;
    f64   asNumber(const Value& v) const;

    void  applyBinaryArith(IROp op);
    void  applyComparison(IROp op);
    void  applyLogical(IROp op);
    void  applyUnary(IROp op);
    Value runStdlibCall(StdlibId id, i32 argc, const QueryContext& q);

    bool m_faulted;
};

} // namespace DSL
} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_DSL_VM_H_
