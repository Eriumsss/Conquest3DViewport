// IRanker.h
// =============================================================================
// IRANKER — INTERFACE EVERY RANKING STRATEGY OBEYS OR DIES
// =============================================================================
// Written by: Eriumsss
//
// Four strategies implement this in Strategy/: Frequency, TypeDirected,
// Hybrid, MLStub. The selector picks one (or chains them) at runtime.
// The interface is intentionally narrow: candidates in, scored
// candidates out. The pipeline around it (compat filter, candidate
// gen, confidence gate, reason building) lives elsewhere.
// =============================================================================

#ifndef VESPUCCI_SUGGEST_IRANKER_H_
#define VESPUCCI_SUGGEST_IRANKER_H_

#include "SuggestionTypes.h"

namespace Vespucci {
namespace Scene { class SceneSnapshot; }
namespace Suggest {

class IRanker {
public:
    enum Kind {
        KIND_Frequency    = 0,
        KIND_TypeDirected = 1,
        KIND_Hybrid       = 2,
        KIND_MLStub       = 3,
        KIND_None         = 4
    };
    virtual ~IRanker() {}

    virtual Kind        kind() const = 0;
    virtual const char* name() const = 0;

    // Score one (source, event, candidate) triple. Returns the score
    // and writes up to maxReasons reason chips into outReasons.
    // Faulted output stays defensive - the ranker NEVER throws.
    virtual score_t scoreCandidate(const Scene::SceneSnapshot& snap,
                                    Vespucci::EntityIndex sourceIdx,
                                    const Core::StringRef& eventCanonical,
                                    Vespucci::EntityIndex candidateIdx,
                                    ReasonChip* outReasons,
                                    i32 maxReasons,
                                    i32& outReasonCount) const = 0;
};

// Lightweight convenience accessors over the RankerSelector singleton.
// Consumers don't need to hold a RankerSelector*; just call these.
IRanker*       ActiveRanker();
IRanker::Kind  ActiveRankerKind();
void           SetActiveRankerKind(IRanker::Kind k);

} // namespace Suggest
} // namespace Vespucci

#endif // VESPUCCI_SUGGEST_IRANKER_H_
