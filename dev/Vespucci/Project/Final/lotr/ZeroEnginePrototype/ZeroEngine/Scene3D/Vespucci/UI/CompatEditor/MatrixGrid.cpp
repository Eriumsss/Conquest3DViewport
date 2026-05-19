// MatrixGrid.cpp
// =============================================================================
// VIEW THE COMPAT MATRIX AS A TYPE-VS-TYPE GRID OF ALLOW/DENY/UNKNOWN
// =============================================================================
// Written by: Eriumsss
//
// The grid asks the compat matrix one query per cell — "given this
// (sourceType, eventName, targetType, actionName), allow or deny?" —
// and colors the cell by verdict. Two big quality-of-life features:
//
//   1. The (eventName, actionName) probe pair is NOT hardcoded. We
//      scan the loaded level's wires and build a frequency-sorted list
//      of pairs that ACTUALLY occur in the corpus. Designer picks one
//      from a combo, the grid re-runs against that probe. No more
//      degenerate `OnTrigger -> Activate` blank wasteland.
//
//   2. Scrollable, resizable, sortable, with a frozen first column so
//      the source-name stays visible when scrolling right across 90+
//      columns. Hover any cell for a tooltip with the full type names
//      and the matrix's reason chip if any.
// =============================================================================

#include "Panel.h"

#include "../../Compat/CompatibilityMatrix.h"
#include "../../Schema/ZETypeRegistry.h"
#include "../../Compat/DSL/Vm.h"
#include "../../Scene/SceneSnapshot.h"
#include "../../../imgui/imgui.h"
#include "../Common/Theme.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

namespace Vespucci {
namespace UI {
namespace CompatEditor {

namespace {
    // Scan the snapshot once and build a frequency-sorted list of the
    // (event, action) pairs designers might want to inspect. We cap at
    // 64 because the combo box becomes a navigation problem above that.
    void RebuildProbePairCacheIfStale(PanelState& s,
                                       const Scene::SceneSnapshot& snap)
    {
        if (s.matrixProbeBuiltAt == snap.version() &&
            !s.matrixProbePairs.empty())
        {
            return;
        }

        // Tally (event||action) -> count via a hash map. Concatenate with
        // a NUL separator so "open|close" and "openclose|" cannot collide.
        struct PairTally { std::string evt; std::string act; i32 count; };
        std::unordered_map<std::string, i32> tally;
        Span<Scene::WireRow> wires = snap.wires();
        for (usize i = 0; i < wires.n; ++i) {
            const Scene::WireRow& w = wires.ptr[i];
            if (w.eventName.size() == 0 && w.actionName.size() == 0) continue;
            std::string key;
            key.reserve(w.eventName.size() + w.actionName.size() + 1);
            key.append(w.eventName.data(), w.eventName.size());
            key.push_back('\0');
            key.append(w.actionName.data(), w.actionName.size());
            tally[key]++;
        }

        std::vector<MatrixProbePair> fresh;
        fresh.reserve(tally.size());
        for (std::unordered_map<std::string, i32>::const_iterator it = tally.begin();
             it != tally.end(); ++it)
        {
            const std::string& k = it->first;
            size_t sep = k.find('\0');
            if (sep == std::string::npos) continue;
            MatrixProbePair p;
            p.eventName     = k.substr(0, sep);
            p.actionName    = k.substr(sep + 1);
            p.observedCount = it->second;
            fresh.push_back(p);
        }
        std::sort(fresh.begin(), fresh.end(),
            [](const MatrixProbePair& a, const MatrixProbePair& b) {
                return a.observedCount > b.observedCount;
            });
        if (fresh.size() > 64) fresh.resize(64);

        s.matrixProbePairs   = fresh;
        s.matrixProbeBuiltAt = snap.version();
        if (s.matrixProbeIdx >= (i32)fresh.size()) s.matrixProbeIdx = 0;
    }

    bool TypeNameMatchesFilter(const Schema::TypeRecord* rec, const char* filter) {
        if (!filter || !filter[0]) return true;
        if (!rec) return false;
        const char* name = rec->name.data();
        i32 nameLen = (i32)rec->name.size();
        // Case-insensitive substring scan.
        usize fn = std::strlen(filter);
        for (i32 i = 0; i + (i32)fn <= nameLen; ++i) {
            bool ok = true;
            for (usize k = 0; k < fn; ++k) {
                char a = name[i + (i32)k];
                char b = filter[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }
} // namespace

void RenderMatrixGridSnapshot(PanelState& s) {
    Schema::ZETypeRegistry* reg = Schema::GlobalRegistry();
    Compat::CompatibilityMatrix* m = Compat::GlobalMatrix();
    Scene::SceneSnapshot* snap = Scene::GlobalSnapshot();
    if (!reg || !m) {
        ImGui::TextDisabled("(no registry / matrix bound)");
        return;
    }
    i32 typeCount = reg->count();
    if (typeCount == 0) {
        ImGui::TextDisabled("(registry is empty)");
        return;
    }

    if (snap) RebuildProbePairCacheIfStale(s, *snap);

    // ── Probe-pair selector ──────────────────────────────────────────
    ImGui::Text("Probe pair (from level wires):");
    ImGui::SameLine();
    if (s.matrixProbePairs.empty()) {
        ImGui::TextDisabled("(no wires observed in this snapshot — load a level)");
    } else {
        const MatrixProbePair& current = s.matrixProbePairs[
            (size_t)((s.matrixProbeIdx >= 0 && s.matrixProbeIdx < (i32)s.matrixProbePairs.size())
                ? s.matrixProbeIdx : 0)];
        char preview[160];
        std::snprintf(preview, sizeof(preview), "%s -> %s   (%d wires)",
            current.eventName.c_str(), current.actionName.c_str(), current.observedCount);
        if (ImGui::BeginCombo("##cmp_probe", preview)) {
            for (i32 i = 0; i < (i32)s.matrixProbePairs.size(); ++i) {
                const MatrixProbePair& p = s.matrixProbePairs[(size_t)i];
                char label[200];
                std::snprintf(label, sizeof(label), "%s -> %s   (%d)",
                    p.eventName.c_str(), p.actionName.c_str(), p.observedCount);
                bool sel = (i == s.matrixProbeIdx);
                if (ImGui::Selectable(label, sel)) s.matrixProbeIdx = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // ── Filter inputs + cap slider ───────────────────────────────────
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##cmp_src_filter", "filter source rows", s.matrixSrcFilter, sizeof(s.matrixSrcFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##cmp_tgt_filter", "filter target columns", s.matrixTgtFilter, sizeof(s.matrixTgtFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (s.matrixTypeCap < 16) s.matrixTypeCap = 16;
    if (s.matrixTypeCap > typeCount) s.matrixTypeCap = typeCount;
    ImGui::SliderInt("##cmp_cap", &s.matrixTypeCap, 16, typeCount, "cap %d");

    // Resolve the chosen probe pair (or sane default).
    Core::StringRef probeEvent("ontrigger", 9);
    Core::StringRef probeAction("activate", 8);
    if (!s.matrixProbePairs.empty() &&
        s.matrixProbeIdx >= 0 && s.matrixProbeIdx < (i32)s.matrixProbePairs.size())
    {
        const MatrixProbePair& p = s.matrixProbePairs[(size_t)s.matrixProbeIdx];
        probeEvent  = Core::StringRef(p.eventName.c_str(),  (i32)p.eventName.size());
        probeAction = Core::StringRef(p.actionName.c_str(), (i32)p.actionName.size());
    }

    // ── Build the filtered row / column index lists ──────────────────
    std::vector<i32> rowIdx;
    std::vector<i32> colIdx;
    rowIdx.reserve((size_t)s.matrixTypeCap);
    colIdx.reserve((size_t)s.matrixTypeCap);
    for (i32 i = 0; i < typeCount && (i32)rowIdx.size() < s.matrixTypeCap; ++i) {
        if (TypeNameMatchesFilter(reg->at(i), s.matrixSrcFilter)) rowIdx.push_back(i);
    }
    for (i32 i = 0; i < typeCount && (i32)colIdx.size() < s.matrixTypeCap; ++i) {
        if (TypeNameMatchesFilter(reg->at(i), s.matrixTgtFilter)) colIdx.push_back(i);
    }

    if (rowIdx.empty() || colIdx.empty()) {
        ImGui::TextDisabled("(filter eliminated every row or column — broaden it)");
        return;
    }

    // ── The grid itself ──────────────────────────────────────────────
    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_SizingFixedFit;

    ImVec2 outerSize = ImVec2(0.0f, ImGui::GetContentRegionAvail().y - 4.0f);
    if (ImGui::BeginTable("##cmp_matrix", (i32)colIdx.size() + 1, flags, outerSize))
    {
        ImGui::TableSetupScrollFreeze(1, 1); // freeze first col + header row
        ImGui::TableSetupColumn("src \\ tgt", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        for (size_t j = 0; j < colIdx.size(); ++j) {
            const Schema::TypeRecord* t = reg->at(colIdx[j]);
            const char* nm = t ? t->name.data() : "?";
            ImGui::TableSetupColumn(nm, ImGuiTableColumnFlags_WidthFixed, 80.0f);
        }
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < rowIdx.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const Schema::TypeRecord* src = reg->at(rowIdx[i]);
            const char* srcName = src ? src->name.data() : "?";
            ImGui::TextUnformatted(srcName);

            for (size_t j = 0; j < colIdx.size(); ++j) {
                ImGui::TableNextColumn();
                const Schema::TypeRecord* tgt = reg->at(colIdx[j]);
                Compat::DSL::QueryContext q;
                std::memset(&q, 0, sizeof(q));
                q.sourceType = src ? src->id : TypeId(0);
                q.targetType = tgt ? tgt->id : TypeId(0);
                q.eventName  = probeEvent;
                q.actionName = probeAction;
                Compat::CompatResult r;
                m->check(q, r);

                ImVec4 col;
                const char* glyph;
                switch (r.verdict) {
                    case Compat::COMPAT_Allow:
                        col = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
                        glyph = "A";
                        break;
                    case Compat::COMPAT_Deny:
                        col = ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
                        glyph = "D";
                        break;
                    default:
                        col = ImVec4(0.55f, 0.55f, 0.60f, 1.0f);
                        glyph = "?";
                        break;
                }
                ImGui::TextColored(col, "%s", glyph);

                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%s -> %s", srcName, tgt ? tgt->name.data() : "?");
                    ImGui::TextColored(col, "verdict: %s",
                        r.verdict == Compat::COMPAT_Allow ? "ALLOW" :
                        r.verdict == Compat::COMPAT_Deny  ? "DENY"  : "UNKNOWN");
                    if (r.reason.size() > 0) {
                        ImGui::TextDisabled("reason: %.*s",
                            (int)r.reason.size(), r.reason.data());
                    }
                    ImGui::TextDisabled("event:  %.*s", (int)probeEvent.size(),  probeEvent.data());
                    ImGui::TextDisabled("action: %.*s", (int)probeAction.size(), probeAction.data());
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::EndTable();
    }
}

} // namespace CompatEditor
} // namespace UI
} // namespace Vespucci
