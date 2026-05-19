// RankerSelector.cpp
// =============================================================================
// PICK A RANKER STRATEGY AND RUN IT. ROUTES TARGET / AUTOCOMPLETE /
// REPAIR QUERIES TO THE RIGHT IRANKER IMPLEMENTATION.
// =============================================================================
// Written by: Eriumsss

#include "../IRanker.h"

#include "../../Corpus/CorpusPriors.h"
#include "../../Schema/EventActionSignatureDB.h"
#include "../../Core/Logging.h"

namespace Vespucci {
namespace Suggest {

// Forward decls from sibling .cpps.
IRanker* MakeFrequencyRanker(const Corpus::CorpusPriors* priors);
IRanker* MakeTypeDirectedRanker(const Corpus::CorpusPriors* priors,
                                 const Schema::EventActionSignatureDB* sigDB);
IRanker* MakeHybridRanker(const Corpus::CorpusPriors* priors);
IRanker* MakeMLStubRanker(const Corpus::CorpusPriors* priors);

class RankerSelector {
public:
    RankerSelector(const Corpus::CorpusPriors* priors,
                   const Schema::EventActionSignatureDB* sigDB)
        : m_priors(priors),
          m_sigDB(sigDB),
          m_freq(0), m_type(0), m_hybrid(0), m_ml(0),
          m_active(IRanker::KIND_Hybrid)
    {}

    ~RankerSelector() {
        delete m_freq; delete m_type; delete m_hybrid; delete m_ml;
    }

    void prebuildAll() {
        if (!m_freq)   m_freq   = MakeFrequencyRanker(m_priors);
        if (!m_type)   m_type   = MakeTypeDirectedRanker(m_priors, m_sigDB);
        if (!m_hybrid) m_hybrid = MakeHybridRanker(m_priors);
        if (!m_ml)     m_ml     = MakeMLStubRanker(m_priors);
    }

    void setActiveOrChokeOnUnknown(IRanker::Kind k) {
        switch (k) {
            case IRanker::KIND_Frequency:
            case IRanker::KIND_TypeDirected:
            case IRanker::KIND_Hybrid:
            case IRanker::KIND_MLStub:
                m_active = k; break;
            default:
                Core::Logging::Warn("RankerSelector: unknown kind %d, sticking with Hybrid", (int)k);
                m_active = IRanker::KIND_Hybrid; break;
        }
    }

    IRanker* active() {
        prebuildAll();
        switch (m_active) {
            case IRanker::KIND_Frequency:    return m_freq;
            case IRanker::KIND_TypeDirected: return m_type;
            case IRanker::KIND_MLStub:       return m_ml;
            case IRanker::KIND_Hybrid:
            default:                         return m_hybrid;
        }
    }

    IRanker::Kind activeKind() const { return m_active; }

private:
    const Corpus::CorpusPriors*           m_priors;
    const Schema::EventActionSignatureDB* m_sigDB;
    IRanker* m_freq;
    IRanker* m_type;
    IRanker* m_hybrid;
    IRanker* m_ml;
    IRanker::Kind m_active;
};

namespace {
    static RankerSelector* s_global = 0;
}

RankerSelector* GlobalSelector()             { return s_global; }
void            SetGlobalSelector(RankerSelector* s) { s_global = s; }

RankerSelector* MakeSelector(const Corpus::CorpusPriors* priors,
                              const Schema::EventActionSignatureDB* sigDB)
{
    return new RankerSelector(priors, sigDB);
}

void DestroySelector(RankerSelector* s) { delete s; }

// Convenience accessors for consumers that don't need the full RankerSelector
// class (which is defined inside this .cpp). Sidebar / Health-Fix call these
// instead of holding a RankerSelector*.
IRanker* ActiveRanker() {
    if (!s_global) return 0;
    return s_global->active();
}

void SetActiveRankerKind(IRanker::Kind k) {
    if (!s_global) return;
    s_global->setActiveOrChokeOnUnknown(k);
}

IRanker::Kind ActiveRankerKind() {
    return s_global ? s_global->activeKind() : IRanker::KIND_None;
}

} // namespace Suggest
} // namespace Vespucci
