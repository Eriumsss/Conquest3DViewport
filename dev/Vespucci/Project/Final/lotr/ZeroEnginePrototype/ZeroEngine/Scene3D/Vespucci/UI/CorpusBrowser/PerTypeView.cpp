// PerTypeView.cpp
// =============================================================================
// PER-TYPE CORPUS DRILLDOWN - LIST OF ENTITY TYPES, EXPANDABLE TO SHOW
// EVERY (event, target-type, count) TRIPLE OBSERVED FOR EACH TYPE
// =============================================================================
// Written by: Eriumsss
//
// The doc-locked transparency surface: a designer who wants to know
// 'what does my corpus think CapturePoints DO?' opens this tab,
// types 'capture' in the filter, and sees every event the corpus
// has observed CapturePoints firing into, ranked by frequency.
//
// We pull data straight from CorpusPriors via the global pointer.
// The histogram tables on CorpusPriors are public-friendly enough
// that we can iterate them without breaking encapsulation.
// =============================================================================

#include "Panel.h"

#include "../../Corpus/CorpusPriors.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Vespucci {
namespace UI {
namespace CorpusBrowser {

namespace {
    bool LowerSubstrMatch(const Core::StringRef& haystack, const char* needle) {
        if (!needle || !needle[0]) return true;
        usize nLen = std::strlen(needle);
        if (nLen > haystack.size()) return false;
        auto Low = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
        for (usize i = 0; i + nLen <= haystack.size(); ++i) {
            bool ok = true;
            for (usize k = 0; k < nLen; ++k) {
                if (Low(haystack.data()[i + k]) != Low(needle[k])) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }

    struct TypeRow {
        TypeId  id;
        Core::StringRef name;
        u32     totalObservations;
    };

    void GatherTypeRowsRanked(std::vector<TypeRow>& rows, const char* filter) {
        rows.clear();
        Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
        Corpus::CorpusPriors*    pr = Corpus::GlobalPriors();
        if (!reg) return;
        i32 n = reg->count();
        for (i32 i = 0; i < n; ++i) {
            const Schema::TypeRecord* rec = reg->at(i);
            if (!rec) continue;
            if (!LowerSubstrMatch(rec->name, filter)) continue;
            TypeRow tr;
            tr.id   = rec->id;
            tr.name = rec->name;
            tr.totalObservations = 0;
            if (pr) {
                std::unordered_map<TypeId, u32>::const_iterator it =
                    pr->tables().srcTotal.find(rec->id);
                if (it != pr->tables().srcTotal.end()) tr.totalObservations = it->second;
            }
            rows.push_back(tr);
        }
        std::sort(rows.begin(), rows.end(),
            [](const TypeRow& a, const TypeRow& b) {
                return a.totalObservations > b.totalObservations;
            });
    }
} // namespace

void RenderPerTypeView(PanelState& s) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cb_typef", "filter type name (substring)",
                              s.typeFilter, sizeof(s.typeFilter));

    std::vector<TypeRow> rows;
    GatherTypeRowsRanked(rows, s.typeFilter);

    if (ImGui::BeginTable("##cb_type_table", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(-1.0f, -1.0f)))
    {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Observations");
        ImGui::TableSetupColumn("TypeId");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < rows.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            bool isSelected = ((i32)i == s.selectedTypeIdx);
            if (ImGui::Selectable(rows[i].name.data(), isSelected,
                ImGuiSelectableFlags_SpanAllColumns))
            {
                s.selectedTypeIdx = (i32)i;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%u", (unsigned)rows[i].totalObservations);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("0x%08X", (unsigned)rows[i].id.raw);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

} // namespace CorpusBrowser
} // namespace UI
} // namespace Vespucci
