// DidYouMean.cpp
// =============================================================================
// Bridge to the RepairRanker. Pulls candidates from the snapshot's
// same-layer + same-type buckets, ranks via the doc-locked structural-
// dominates-string formula, returns top-K.
// =============================================================================
// Written by: Eriumsss

#include "DidYouMean.h"

#include "../Core/Logging.h"
#include "../Core/SmallVec.h"
#include "../Suggest/Features.h"
#include "../Suggest/SuggestionTypes.h"

#include <algorithm>

namespace Vespucci {
namespace Repair {

i32 GenerateDidYouMean(const Scene::SceneSnapshot& snap,
                        const BrokenRef& broken,
                        RepairCandidate* outCandidates,
                        i32 maxOut)
{
    if (!outCandidates || maxOut <= 0) return 0;

    // Resolve the OWNER of the broken wire so we know the type
    // signature we are repairing.
    Vespucci::EntityIndex ownerIdx = snap.indexForGuid(broken.ownerGuid);
    if (!ownerIdx.valid()) return 0;
    const Scene::EntityRow* owner = snap.entityAt(ownerIdx);
    if (!owner) return 0;

    // Build candidate set: every entity with same type as the original
    // target's NAME prefix would suggest, plus same-layer entities.
    // Without the original target's type (because it is a ghost) we
    // fall back to all spatial neighbors + same-layer set.
    Core::SmallVec<Vespucci::EntityIndex, 256> work;
    Span<Scene::EntityRow> rows = snap.entities();
    for (i32 i = 0; i < (i32)rows.size() && work.size() < 256; ++i) {
        const Scene::EntityRow& r = rows[(usize)i];
        if (r.guid == broken.ownerGuid) continue;
        if (r.layerGuid == owner->layerGuid) {
            work.push_back(Vespucci::EntityIndex(i));
        }
    }
    if (work.size() == 0) return 0;

    // Score each candidate via the structural-dominates-string formula.
    struct Scored { Vespucci::EntityIndex idx; f32 score; const Scene::EntityRow* row; };
    std::vector<Scored> scored;
    scored.reserve((size_t)work.size());
    for (i32 i = 0; i < work.size(); ++i) {
        const Scene::EntityRow* tgt = snap.entityAt(work[i]);
        if (!tgt) continue;
        Suggest::FeatureScores fs;
        std::memset(&fs, 0, sizeof(fs));
        fs.sameLayer = (owner->layerGuid == tgt->layerGuid) ? 1.0f : 0.0f;
        fs.sameParent = (owner->parentGuid == tgt->parentGuid) ? 1.0f : 0.0f;
        fs.spatialProximity = std::exp(-Suggest::Vec3DistOrZeroIfDeadGuids(
            owner->position, tgt->position) / 50.0f);
        fs.nameSimilarity = Suggest::NameSimilarityHammerOutOfTwoStrings(
            owner->name, tgt->name);

        // Structural dominates: gate the string contribution behind
        // a structural-floor.
        f32 structural = 0.7f * fs.sameLayer + 0.4f * fs.sameParent + 0.3f * fs.spatialProximity;
        f32 stringContribution = (structural > 0.1f) ? fs.nameSimilarity * 0.3f : 0.0f;
        Scored sc; sc.idx = work[i]; sc.row = tgt;
        sc.score = structural + stringContribution;
        scored.push_back(sc);
    }
    std::sort(scored.begin(), scored.end(),
        [](const Scored& a, const Scored& b) { return a.score > b.score; });

    i32 take = (i32)scored.size();
    if (take > maxOut) take = maxOut;
    for (i32 i = 0; i < take; ++i) {
        outCandidates[i].idx    = scored[(size_t)i].idx;
        outCandidates[i].guid   = scored[(size_t)i].row->guid;
        outCandidates[i].score  = scored[(size_t)i].score;
        outCandidates[i].reason = Core::StringRef("same layer + best structural fit");
    }
    return take;
}

} // namespace Repair
} // namespace Vespucci
