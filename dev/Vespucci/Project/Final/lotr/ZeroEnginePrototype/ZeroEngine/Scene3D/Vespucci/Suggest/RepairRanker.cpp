// RepairRanker.cpp
// =============================================================================
// 'DID YOU MEAN' RERANKER FOR BROKEN-REF HEALING. TYPE/LAYER FIRST,
// STRING SIMILARITY AS A TIEBREAKER. DOC-LOCKED HARD: STRING CANNOT
// WIN ALONE.
// =============================================================================
// Written by: Eriumsss

#include "SuggestionTypes.h"
#include "Features.h"
#include "../Core/Logging.h"
#include "../Scene/SceneSnapshot.h"

#include <algorithm>
#include <vector>

namespace Vespucci {
namespace Suggest {

struct RepairCandidate {
    Vespucci::EntityIndex idx;
    Guid                  guid;
    score_t               typeScore;
    score_t               layerScore;
    score_t               spatialScore;
    score_t               nameScore;
    score_t               total;
};

// Given the broken-ref source + a list of candidate replacements, pick
// the top-K that obey "type/layer/spatial dominate string". Doc-locked:
// the highest possible string-only score CANNOT outrank a candidate
// that has even a small type-or-layer signal. We enforce that by
// gating the string contribution behind a structural-floor.
i32 RankRepairCandidatesOrChokeOnAStringMatch(
    const Scene::SceneSnapshot& snap,
    Vespucci::EntityIndex brokenSourceIdx,
    Core::StringRef desiredEvent,
    const Vespucci::EntityIndex* candidates,
    i32 candCount,
    RepairCandidate* outRanked,
    i32 maxOut)
{
    (void)desiredEvent;
    if (!candidates || candCount <= 0 || maxOut <= 0) return 0;

    const Scene::EntityRow* src = snap.entityAt(brokenSourceIdx);
    if (!src) return 0;

    std::vector<RepairCandidate> work;
    work.reserve((size_t)candCount);
    for (i32 i = 0; i < candCount; ++i) {
        const Scene::EntityRow* tgt = snap.entityAt(candidates[i]);
        if (!tgt) continue;
        RepairCandidate c;
        c.idx = candidates[i];
        c.guid = tgt->guid;
        c.typeScore    = (src->typeId == tgt->typeId) ? 1.0f : 0.0f;
        c.layerScore   = (src->layerGuid.valid() && src->layerGuid == tgt->layerGuid) ? 1.0f : 0.0f;
        f32 dist       = Vec3DistOrZeroIfDeadGuids(src->position, tgt->position);
        c.spatialScore = std::exp(-dist / 50.0f);
        c.nameScore    = NameSimilarityHammerOutOfTwoStrings(src->name, tgt->name);

        // Doc-locked combination: structural sum dominates. String
        // contributes ONLY if structural is non-trivial (gate ratio).
        f32 structural = c.typeScore + 0.7f * c.layerScore + 0.4f * c.spatialScore;
        f32 stringContribution = (structural > 0.1f) ? c.nameScore * 0.3f : 0.0f;
        c.total = structural + stringContribution;
        work.push_back(c);
    }
    std::sort(work.begin(), work.end(),
        [](const RepairCandidate& a, const RepairCandidate& b) {
            return a.total > b.total;
        });
    i32 take = (i32)work.size();
    if (take > maxOut) take = maxOut;
    for (i32 i = 0; i < take; ++i) outRanked[i] = work[(size_t)i];
    return take;
}

} // namespace Suggest
} // namespace Vespucci
