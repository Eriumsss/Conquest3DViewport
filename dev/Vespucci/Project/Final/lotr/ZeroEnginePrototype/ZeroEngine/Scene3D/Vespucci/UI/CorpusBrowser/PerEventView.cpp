// PerEventView.cpp
// =============================================================================
// PER-EVENT CORPUS DRILLDOWN - 'WHEN OnCapture FIRES, WHAT TYPES
// USUALLY RECEIVE IT?' RANKED HISTOGRAM RENDERED AS A TABLE
// =============================================================================
// Written by: Eriumsss

#include "Panel.h"

#include "../../Corpus/CorpusPriors.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../Core/Hash.h"
#include "../../../imgui/imgui.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Vespucci {
namespace UI {
namespace CorpusBrowser {

namespace {
    struct EventTargetRow {
        Core::StringRef targetTypeName;
        u32             count;
    };

    void GatherEventTargetsRanked(const char* eventName,
                                    std::vector<EventTargetRow>& rows)
    {
        rows.clear();
        Corpus::CorpusPriors* pr = Corpus::GlobalPriors();
        Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
        if (!pr || !reg || !eventName || !eventName[0]) return;

        u32 evtHash = Core::XxHash32(eventName, std::strlen(eventName), 0xC0FFEE13u);
        const Corpus::PriorTables& t = pr->tables();
        for (std::unordered_map<Corpus::PairKey, u32, Corpus::PairKeyHash>::const_iterator it =
                 t.eventToTarget.begin(); it != t.eventToTarget.end(); ++it)
        {
            if (it->first.extra != evtHash) continue;
            const Schema::TypeRecord* rec = reg->findById(it->first.b);
            if (!rec) continue;
            EventTargetRow row;
            row.targetTypeName = rec->name;
            row.count          = it->second;
            rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(),
            [](const EventTargetRow& a, const EventTargetRow& b) { return a.count > b.count; });
    }
} // namespace

void RenderPerEventView(PanelState& s) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cb_evtf", "event name (e.g. OnCapture)",
                              s.eventFilter, sizeof(s.eventFilter));

    if (s.eventFilter[0] == 0) {
        ImGui::TextDisabled("(type an event name to see ranked target types)");
        return;
    }

    // Lower the filter for canonical lookup against the corpus.
    char canon[64];
    usize n = std::strlen(s.eventFilter);
    if (n + 1 > sizeof(canon)) n = sizeof(canon) - 1;
    for (usize i = 0; i < n; ++i) {
        char c = s.eventFilter[i];
        canon[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    canon[n] = 0;

    std::vector<EventTargetRow> rows;
    GatherEventTargetsRanked(canon, rows);

    if (rows.empty()) {
        ImGui::TextDisabled("(no observations of '%s' in the corpus)", s.eventFilter);
        return;
    }

    if (ImGui::BeginTable("##cb_event_table", 2,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Target type");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < rows.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(rows[i].targetTypeName.data());
            ImGui::TableNextColumn();
            ImGui::Text("%u", (unsigned)rows[i].count);
        }
        ImGui::EndTable();
    }
}

} // namespace CorpusBrowser
} // namespace UI
} // namespace Vespucci
