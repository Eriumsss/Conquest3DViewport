// Pinning.cpp
// =============================================================================
// PINNED SUGGESTIONS - DESIGNERS MARK A SUGGESTION TO KEEP IT VISIBLE
// EVEN AS NEW QUERIES ROLL THE LIST. THE PINNED SET IS PER-SOURCE-GUID.
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Core/Hash.h"
#include "../../../imgui/imgui.h"

#include <unordered_map>
#include <unordered_set>

namespace Vespucci {
namespace UI {
namespace SuggestedWires {

namespace {
    // Per-source map of pinned target GUIDs.
    static std::unordered_map<u32, std::unordered_set<u32> > s_pinnedBySource;
} // namespace

void TogglePinnedTarget(Guid sourceGuid, Guid targetGuid) {
    if (!sourceGuid.valid() || !targetGuid.valid()) return;
    std::unordered_set<u32>& set = s_pinnedBySource[sourceGuid.raw];
    if (set.count(targetGuid.raw)) set.erase(targetGuid.raw);
    else                            set.insert(targetGuid.raw);
}

bool IsTargetPinned(Guid sourceGuid, Guid targetGuid) {
    std::unordered_map<u32, std::unordered_set<u32> >::const_iterator it =
        s_pinnedBySource.find(sourceGuid.raw);
    if (it == s_pinnedBySource.end()) return false;
    return it->second.count(targetGuid.raw) != 0;
}

void RenderPinnedSection(PanelState& s) {
    std::unordered_map<u32, std::unordered_set<u32> >::const_iterator it =
        s_pinnedBySource.find(s.focusedSourceGuid.raw);
    if (it == s_pinnedBySource.end() || it->second.empty()) {
        ImGui::TextDisabled("(no pinned suggestions for this entity)");
        return;
    }
    for (std::unordered_set<u32>::const_iterator t = it->second.begin();
         t != it->second.end(); ++t)
    {
        ImGui::Text("0x%08X", (unsigned)*t);
    }
}

void ClearPinnedForSource(Guid sourceGuid) {
    s_pinnedBySource.erase(sourceGuid.raw);
}

} // namespace SuggestedWires
} // namespace UI
} // namespace Vespucci
