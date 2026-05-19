// CompatibilityMatrix.cpp
// =============================================================================
// Hard signature filter + DSL rule evaluator. Hot path. Zero alloc on
// the success-AND-no-rules track; one VM run per non-faulted rule on
// the slow path.
// =============================================================================
// Written by: Eriumsss

#include "CompatibilityMatrix.h"
#include "RuleSet.h"

#include "../Schema/EventActionSignatureDB.h"
#include "../Schema/ZETypeRegistry.h"

#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

#include <cstring>

namespace Vespucci {
namespace Compat {

namespace {
    static CompatibilityMatrix* s_global = 0;

    void LowerCanonical(const Core::StringRef& s, char* out, usize cap) {
        usize n = s.size();
        if (n + 1 > cap) n = cap - 1;
        for (usize i = 0; i < n; ++i) {
            char c = s.data()[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            out[i] = c;
        }
        out[n] = 0;
    }
} // namespace

CompatibilityMatrix* GlobalMatrix()                  { return s_global; }
void                 SetGlobalMatrix(CompatibilityMatrix* m) { s_global = m; }

CompatibilityMatrix::CompatibilityMatrix()
    : m_reg(0), m_sigDB(0), m_rules(0)
{
    std::memset(&m_stats, 0, sizeof(m_stats));
}

CompatibilityMatrix::~CompatibilityMatrix() {}

void CompatibilityMatrix::bind(const Schema::ZETypeRegistry* reg,
                                const Schema::EventActionSignatureDB* sigDB,
                                const RuleSet* rules)
{
    m_reg   = reg;
    m_sigDB = sigDB;
    m_rules = rules;
}

void CompatibilityMatrix::check(const DSL::QueryContext& q, CompatResult& out) const {
    out.verdict        = COMPAT_Unknown;
    out.weightBonus    = 0.0f;
    out.reason         = Core::StringRef();
    out.rulesEvaluated = 0;
    out.hardLayerHit   = false;
    m_stats.callsThisFrame++;

    // ── Hard layer: signature DB ──────────────────────────────────────
    bool hardYes = false;
    bool hardNo  = false;
    if (m_sigDB) {
        // Convert event/action names to lowercase canonical for the
        // DB's lookup keys. The DB stores canonical lowercase.
        char evt[64]; LowerCanonical(q.eventName,  evt, sizeof(evt));
        char act[64]; LowerCanonical(q.actionName, act, sizeof(act));

        bool emits = m_sigDB->typeEmitsEvent(q.sourceType, evt);
        bool accepts = m_sigDB->typeAcceptsAction(q.targetType, act);
        if (emits || accepts) out.hardLayerHit = true;

        // If we have signature info on the source AND it does not list
        // the event, that is a DENY. Same for the target action.
        const Schema::TypeSignature* srcSig = m_sigDB->findSignature(q.sourceType);
        const Schema::TypeSignature* tgtSig = m_sigDB->findSignature(q.targetType);

        if (srcSig && srcSig->emittedCount > 0 && !emits) {
            hardNo = true;
            out.reason = Core::StringRef(
                "source type does not emit this event in any known LOTR:C wiring", 60);
        }
        if (tgtSig && tgtSig->acceptedCount > 0 && !accepts) {
            hardNo = true;
            if (out.reason.size() == 0) {
                out.reason = Core::StringRef(
                    "target type does not accept this action in any known LOTR:C wiring", 64);
            }
        }
        if (emits && accepts) hardYes = true;
    }

    if (hardNo) {
        out.verdict = COMPAT_Deny;
        m_stats.hardDenies++;
        return;
    }

    // ── DSL layer: run rules in priority order ────────────────────────
    DSL::VMResult agg;
    agg.verdict        = DSL::VERDICT_None;
    agg.weightBonus    = 0.0f;
    agg.reason         = Core::StringRef();
    agg.rulesEvaluated = 0;
    agg.faulted        = false;

    if (m_rules && m_rules->size() > 0) {
        i32 ruleCount = m_rules->size();
        for (i32 i = 0; i < ruleCount; ++i) {
            const DSL::IRProgram* p = m_rules->programAt(i);
            if (!p) continue;
            DSL::VMResult one = m_vm.run(*p, q);
            agg.rulesEvaluated++;
            if (one.faulted) {
                agg.faulted = true;
                agg.verdict = DSL::VERDICT_Deny;
                agg.reason  = one.reason;
                break;
            }
            if (one.verdict == DSL::VERDICT_Deny) {
                agg.verdict = DSL::VERDICT_Deny;
                agg.reason  = one.reason;
                break;
            }
            if (one.verdict == DSL::VERDICT_Allow) {
                agg.verdict = DSL::VERDICT_Allow;
                agg.weightBonus += one.weightBonus;
                if (agg.reason.size() == 0) agg.reason = one.reason;
            }
        }
    }
    out.rulesEvaluated = agg.rulesEvaluated;

    if (agg.verdict == DSL::VERDICT_Deny) {
        out.verdict = COMPAT_Deny;
        if (agg.reason.size() > 0) out.reason = agg.reason;
        m_stats.dslDenies++;
        return;
    }

    if (hardYes) {
        out.verdict     = COMPAT_Allow;
        out.weightBonus = agg.weightBonus;
        if (agg.reason.size() > 0) out.reason = agg.reason;
        m_stats.hardAllows++;
        return;
    }

    if (agg.verdict == DSL::VERDICT_Allow) {
        out.verdict     = COMPAT_Allow;
        out.weightBonus = agg.weightBonus;
        if (agg.reason.size() > 0) out.reason = agg.reason;
        m_stats.dslAllows++;
        return;
    }

    // No hard yes, no DSL verdict — call it Unknown. Ranker treats
    // these as low-confidence allow.
    out.verdict = COMPAT_Unknown;
    m_stats.unknowns++;
}

CompatibilityMatrix::Stats CompatibilityMatrix::stats() const { return m_stats; }
void CompatibilityMatrix::resetFrameStats() { std::memset(&m_stats, 0, sizeof(m_stats)); }

} // namespace Compat
} // namespace Vespucci
