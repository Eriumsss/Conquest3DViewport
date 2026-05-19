// ReasonChip.cpp
// =============================================================================
// Render rounded-pill reason chips with text + tinted bg + confidence-
// modulated alpha. Cheap on draw - one rect, one text, one optional
// confidence underline.
// =============================================================================
// Written by: Eriumsss

#include "ReasonChip.h"
#include "Theme.h"

#include <cstring>

namespace Vespucci {
namespace UI {
namespace Common {

namespace {
    ImU32 PickChipColorByText(const Core::StringRef& text) {
        // Heuristic mapping from chip text to palette. Substring match
        // because reason text is short and predictable.
        const char* p = text.data();
        usize n = text.size();
        auto Has = [&](const char* needle) {
            usize m = std::strlen(needle);
            if (m > n) return false;
            for (usize i = 0; i + m <= n; ++i) {
                bool match = true;
                for (usize k = 0; k < m; ++k) {
                    char a = p[i + k]; char b = needle[k];
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                    if (a != b) { match = false; break; }
                }
                if (match) return true;
            }
            return false;
        };
        if (Has("type"))    return Theme::ChipTypeMatch;
        if (Has("event"))   return Theme::ChipEventMatch;
        if (Has("name"))    return Theme::ChipNameMatch;
        if (Has("layer"))   return Theme::ChipLayer;
        if (Has("parent"))  return Theme::ChipParent;
        if (Has("nearby"))  return Theme::ChipSpatial;
        if (Has("recent"))  return Theme::ChipRecent;
        return Theme::ChipTypeMatch;
    }
} // namespace

ImVec2 DrawReasonChip(const Suggest::ReasonChip& chip) {
    const char* txt = chip.text.data();
    i32 txtLen = (i32)chip.text.size();

    ImVec2 textSize = ImGui::CalcTextSize(txt, txt + txtLen);
    ImVec2 chipSize(textSize.x + 2 * Theme::ChipPadX, textSize.y + 2 * Theme::ChipPadY);

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bg = PickChipColorByText(chip.text);
    // Modulate alpha by contribution.
    f32 a = 0.55f + 0.45f * chip.contribution;
    if (a > 1.0f) a = 1.0f;
    ImU32 bgWithAlpha = (bg & 0x00FFFFFFu) | ((u32)(a * 255.0f) << 24);

    dl->AddRectFilled(cursor,
        ImVec2(cursor.x + chipSize.x, cursor.y + chipSize.y),
        bgWithAlpha, 6.0f);
    dl->AddText(ImVec2(cursor.x + Theme::ChipPadX, cursor.y + Theme::ChipPadY),
        Theme::TextPrimary, txt, txt + txtLen);

    // Reserve space in the layout.
    ImGui::Dummy(chipSize);
    return chipSize;
}

void DrawChipRowWithWrap(const Suggest::ReasonChip* chips, i32 count, f32 maxWidth) {
    if (!chips || count <= 0) return;
    f32 lineLen = 0.0f;
    for (i32 i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, Theme::ChipGap);
        ImVec2 textSize = ImGui::CalcTextSize(chips[i].text.data(),
            chips[i].text.data() + chips[i].text.size());
        f32 chipW = textSize.x + 2 * Theme::ChipPadX + Theme::ChipGap;
        if (lineLen + chipW > maxWidth && i > 0) {
            ImGui::NewLine();
            lineLen = 0.0f;
        }
        DrawReasonChip(chips[i]);
        lineLen += chipW;
    }
}

} // namespace Common
} // namespace UI
} // namespace Vespucci
