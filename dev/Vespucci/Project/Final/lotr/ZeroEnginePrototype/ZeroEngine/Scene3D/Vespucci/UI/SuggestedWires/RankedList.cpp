// RankedList.cpp
// =============================================================================
// RANKED LIST RENDERER + RANKER QUERY DRIVER
// =============================================================================
// Written by: Eriumsss
//
// Lives next to Panel.cpp. The Panel orchestrates; this file owns
// the actual ranker query + the Selectable-row rendering.
// =============================================================================

#include "Panel.h"

#include "../../Suggest/SuggestionTypes.h"
#include "../../Suggest/CandidateGenerator.h"
#include "../../Suggest/ConfidenceGate.h"
#include "../../Suggest/IRanker.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../Schema/EventActionSignatureDB.h"
#include "../../Core/HeatTimer.h"
#include "../../Core/Logging.h"
#include "../../Core/SmallVec.h"
#include "../Common/Theme.h"
#include "../Common/ReasonChip.h"
#include "../Common/ScoreBar.h"
#include "../DebugRanker/Overlay.h"
#include "../../../imgui/imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

// Forward decls from sibling cpps for the global accessors.
namespace Strategy = Vespucci::Suggest;

namespace {
    // Pick the source type's most-frequent emitted event as a sane default
    // when the user hasn't typed an event filter. Falls back to a corpus
    // staple ("ontrigger") if the type has no signatures registered.
    Core::StringRef PickDefaultEventFor(Vespucci::TypeId srcType) {
        Schema::EventActionSignatureDB* sigDB = Schema::GlobalSignatureDB();
        if (sigDB && srcType.valid()) {
            i32 evtCount = 0;
            const Schema::SignatureEntry* events = sigDB->eventsFor(srcType, &evtCount);
            if (events && evtCount > 0) {
                // eventsFor returns the array as inserted, not sorted by
                // frequency. Walk it and pick the highest-frequency entry.
                i32 bestIdx = 0;
                u32 bestFreq = events[0].frequency;
                for (i32 i = 1; i < evtCount; ++i) {
                    if (events[i].frequency > bestFreq) {
                        bestFreq = events[i].frequency;
                        bestIdx  = i;
                    }
                }
                return events[bestIdx].name;
            }
        }
        return Core::StringRef("ontrigger", 9);
    }
} // namespace

// Full ranker pipeline driver — replaces the V1 placeholder. Pulls the
// global candidate generator, ranker selector, and confidence gate; runs
// generate → score → sort → gate; publishes the result to the Brain
// overlay so its "Last query" row stops showing zeros.
i32 RunRankerForFocusOrAbort(Guid sourceGuid, const char* eventFilter,
                              Suggest::SuggestionList& outList)
{
    std::memset(&outList, 0, sizeof(outList));
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!snap || !sourceGuid.valid()) return 0;

    Vespucci::EntityIndex srcIdx = snap->indexForGuid(sourceGuid);
    if (!srcIdx.valid()) return 0;
    const Scene::EntityRow* src = snap->entityAt(srcIdx);
    if (!src) return 0;

    // Resolve the event the user wants suggestions for.
    Core::StringRef evt = (eventFilter && eventFilter[0])
                          ? Core::StringRef(eventFilter)
                          : PickDefaultEventFor(src->typeId);

    // Pull the global pipeline pieces — orchestrator wires these in
    // Vespucci::Init. Bail soft if any are missing (the panel will
    // render a "no suggestions" message instead of crashing).
    Suggest::CandidateGenerator* gen = Suggest::GlobalGenerator();
    Suggest::IRanker*            ranker = Suggest::ActiveRanker();
    if (!gen || !ranker) {
        Core::Logging::Warn("SuggestedWires: ranker pipeline not bound (gen=%p, ranker=%p)",
            (void*)gen, (void*)ranker);
        return 0;
    }

    u64 t0 = Core::NowTicks();

    // Candidate generation. Generator does its own compat hard-filter
    // pass internally so what comes out is already pre-filtered.
    Core::SmallVec<Vespucci::EntityIndex, 256> candidates;
    i32 nCand = gen->generate(srcIdx, evt, candidates,
                              Limits::kMaxCandidatesPerQuery);
    Suggest::CandidateGenerator::Stats genStats = gen->lastStats();

    // Score each surviving candidate via the active ranker. We allocate
    // an on-stack scoring buffer sized to the candidate cap.
    Suggest::Suggestion scored[Limits::kMaxCandidatesPerQuery];
    i32 scoredCount = 0;
    for (i32 i = 0; i < nCand && scoredCount < Limits::kMaxCandidatesPerQuery; ++i) {
        Vespucci::EntityIndex cIdx = candidates[i];
        const Scene::EntityRow* tgt = snap->entityAt(cIdx);
        if (!tgt) continue;

        Suggest::ReasonChip reasons[4];
        i32 reasonCount = 0;
        score_t score = ranker->scoreCandidate(*snap, srcIdx, evt,
                                                cIdx, reasons, 4, reasonCount);

        Suggest::Suggestion& sg = scored[scoredCount];
        sg.target           = cIdx;
        sg.targetGuid       = tgt->guid;
        sg.score            = score;
        sg.confidence       = score;
        sg.eventName        = evt;
        sg.actionName       = Core::StringRef("activate", 8);
        sg.suggestedDelay   = 0.0f;
        sg.suggestedSticky  = false;
        for (i32 r = 0; r < reasonCount && r < 4; ++r) sg.reasons[r] = reasons[r];
        sg.reasonCount      = reasonCount;
        sg.faulted          = false;
        scoredCount++;
    }

    // Sort descending by score so the best candidate floats to index 0.
    std::sort(scored, scored + scoredCount,
        [](const Suggest::Suggestion& a, const Suggest::Suggestion& b) {
            return a.score > b.score;
        });

    // Apply the three confidence gates. Returns count of survivors.
    i32 suppressed = 0;
    Suggest::GateConfig gate = Suggest::DefaultGateConfig();
    i32 surviving = Suggest::ApplyGatesOrSuppressTheBitchassWeakOnes(
        scored, scoredCount, gate, &suppressed);

    u64 elapsedNs = Core::TicksToNs(Core::NowTicks() - t0);

    // Fill the public output list.
    outList.count                     = surviving;
    outList.candidatePoolSize         = (i32)candidates.size();
    outList.candidatesAfterHardFilter = (i32)candidates.size() - genStats.hardFilterRejects;
    if (outList.candidatesAfterHardFilter < 0) outList.candidatesAfterHardFilter = 0;
    outList.suppressedByConfidence    = suppressed;
    outList.rulesEvaluated            = 0;
    outList.rankerMs                  = (f32)elapsedNs / 1000000.0f;
    outList.snapshotVersionUsed       = snap->version();
    for (i32 i = 0; i < surviving; ++i) outList.suggestions[i] = scored[i];

    // Hand the result off to the Brain overlay so its telemetry row
    // shows live numbers. No-op if no Brain state is registered.
    DebugRanker::PublishLastQuery(outList);

    return surviving;
}

void RenderFilterRow(PanelState& s) {
    ImGui::SetNextItemWidth(-80.0f);
    ImGui::InputTextWithHint("##sw_evt_filter", "event filter (e.g. OnCapture)",
                              s.eventFilter, sizeof(s.eventFilter));
    ImGui::SameLine();
    ImGui::Checkbox("Pinned only##sw_pin", &s.showPinnedOnly);
}

void RenderRankedList(PanelState& s, const Suggest::SuggestionList& list) {
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    for (i32 i = 0; i < list.count; ++i) {
        const Suggest::Suggestion& sg = list.suggestions[i];
        const Scene::EntityRow* tgt = snap ? snap->entityAt(sg.target) : 0;
        const char* tgtName = tgt ? tgt->name.data() : "(unknown)";
        i32 tgtNameLen = tgt ? (i32)tgt->name.size() : 9;
        char rowLabel[200];
        std::snprintf(rowLabel, sizeof(rowLabel), "%.*s   (%.2f)##sw_row_%d",
            tgtNameLen, tgtName, sg.score, i);
        bool selected = (i == s.selectedSuggestionIdx);
        if (ImGui::Selectable(rowLabel, selected)) {
            s.selectedSuggestionIdx = i;
        }
        // Score bar under the row.
        Common::DrawScoreBar(sg.score, 280.0f, 3.0f);
        // Reason chips.
        if (sg.reasonCount > 0) {
            Common::DrawChipRowWithWrap(sg.reasons, sg.reasonCount, 280.0f);
        }
    }
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
