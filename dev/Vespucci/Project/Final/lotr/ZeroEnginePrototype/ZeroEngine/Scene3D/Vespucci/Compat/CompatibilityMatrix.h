// CompatibilityMatrix.h
// =============================================================================
// COMPATIBILITY MATRIX — THE GODDAMN HARD FILTER, KING OF THE PIPELINE
// =============================================================================
// Written by: Eriumsss
//
// This is the single most-called subsystem in the entire ranker. Every
// candidate the ranker considers gets one Check() call before it
// enters scoring. Two doc-locks fall on this file:
//   1. "Filter first, rank second." If the matrix says NO, the candidate
//      is dead. No score, no reason chip, no debug overlay row.
//   2. "Wrong suggestions are worse than missing suggestions." A false
//      positive in the matrix is the most expensive possible bug here.
//      We err on the side of saying YES with a low weight bonus and
//      let the ranker prune; we do NOT let bullshit wires into the
//      ranker's scoring path.
//
// Two-layer design:
//   - HARD layer: the EventActionSignatureDB. Source emits the event,
//     target accepts the action. If either fails, instant DENY.
//   - DSL layer: the RuleSet of designer-authored rules. Run every
//     rule's IRProgram against the (src, evt, tgt, act) query. First
//     DENY wins. Otherwise sum allow-weight bonuses, attach reasons.
//
// Designers can author rules that override the defaults — see
// rules/*.compat. The CompatEditor UI roundtrips rules through the
// DSL parser+printer for in-editor authoring.
// =============================================================================

#ifndef VESPUCCI_COMPAT_COMPATIBILITYMATRIX_H_
#define VESPUCCI_COMPAT_COMPATIBILITYMATRIX_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "DSL/Vm.h"

namespace Vespucci {
namespace Schema {
    class ZETypeRegistry;
    class EventActionSignatureDB;
}
namespace Compat {

class RuleSet;

enum CompatVerdict {
    COMPAT_Allow      = 0,
    COMPAT_Deny       = 1,
    COMPAT_Unknown    = 2     // no signature info — treat as ALLOW with low confidence
};

struct CompatResult {
    CompatVerdict   verdict;
    f32             weightBonus;       // additive score adjustment from rules
    Core::StringRef reason;             // reason chip text (may be empty)
    i32             rulesEvaluated;
    bool            hardLayerHit;       // true if the signature DB had hard-info on this query
};

class CompatibilityMatrix {
public:
    CompatibilityMatrix();
    ~CompatibilityMatrix();

    // Bind to dependencies. Lifetimes managed by callers; the matrix
    // holds raw pointers that MUST stay valid across every Check().
    void bind(const Schema::ZETypeRegistry* reg,
              const Schema::EventActionSignatureDB* sigDB,
              const RuleSet* rules);

    // The main hot-path query. NEVER allocates; every output field
    // is filled into outResult. Safe to call thousands of times per
    // editor frame.
    void check(const DSL::QueryContext& q, CompatResult& outResult) const;

    // Per-frame stats for the debug overlay.
    struct Stats {
        u64 callsThisFrame;
        u64 hardDenies;
        u64 hardAllows;
        u64 dslDenies;
        u64 dslAllows;
        u64 unknowns;
    };
    Stats stats() const;
    void  resetFrameStats();

private:
    const Schema::ZETypeRegistry*         m_reg;
    const Schema::EventActionSignatureDB* m_sigDB;
    const RuleSet*                        m_rules;

    // Per-thread VM. Single-threaded for now — one mutable VM is fine.
    mutable DSL::VM m_vm;

    // Stats counters. Mutable because Check() is logically const but
    // bumps counters; counters are reset each frame.
    mutable Stats m_stats;
};

// Process singleton accessor — wired in Vespucci::Init.
CompatibilityMatrix* GlobalMatrix();
void                 SetGlobalMatrix(CompatibilityMatrix* m);

} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_COMPATIBILITYMATRIX_H_
