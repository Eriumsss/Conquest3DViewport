// Theme.h
// =============================================================================
// VESPUCCI UI THEME - PALETTE + SPACING CONSTANTS
// =============================================================================
// Written by: Eriumsss
//
// Every Vespucci UI surface pulls colors and spacing from this file.
// One change to the palette and the SuggestedWires sidebar, the
// HealthFix panel, the DebugRanker overlay all reskin together.
// We do NOT use ImGui's global StyleColorsLight/Dark because The
// Forge already has its own opinionated style and Vespucci needs
// to coexist visually.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_THEME_H_
#define VESPUCCI_UI_COMMON_THEME_H_

#include "../../Core/VespucciTypes.h"
#include "../../../imgui/imgui.h"

namespace Vespucci {
namespace UI {
namespace Common {

namespace Theme {
    // Background tiers (darkest to lightest).
    static const ImU32 BgPanel        = IM_COL32( 24,  26,  30, 245);
    static const ImU32 BgSubpanel     = IM_COL32( 32,  34,  40, 245);
    static const ImU32 BgSelectedRow  = IM_COL32( 40,  60,  80, 220);
    static const ImU32 BgHoveredRow   = IM_COL32( 50,  52,  62, 220);

    // Text.
    static const ImU32 TextPrimary    = IM_COL32(232, 234, 244, 255);
    static const ImU32 TextSecondary  = IM_COL32(172, 176, 188, 255);
    static const ImU32 TextDim        = IM_COL32(120, 124, 134, 255);
    static const ImU32 TextWarning    = IM_COL32(255, 196,  90, 255);
    static const ImU32 TextError      = IM_COL32(255, 110, 100, 255);
    static const ImU32 TextOK         = IM_COL32(120, 220, 140, 255);

    // Reason chip palette.
    static const ImU32 ChipTypeMatch    = IM_COL32(110, 170, 220, 220);
    static const ImU32 ChipEventMatch   = IM_COL32( 90, 200, 170, 220);
    static const ImU32 ChipNameMatch    = IM_COL32(220, 180, 110, 220);
    static const ImU32 ChipSpatial      = IM_COL32(180, 140, 220, 220);
    static const ImU32 ChipLayer        = IM_COL32(110, 200, 110, 220);
    static const ImU32 ChipParent       = IM_COL32(170, 200, 110, 220);
    static const ImU32 ChipRecent       = IM_COL32(220, 130, 180, 220);

    // Score bar gradient endpoints.
    static const ImU32 ScoreBarLow    = IM_COL32(120,  60,  60, 220);
    static const ImU32 ScoreBarMid    = IM_COL32(200, 160,  60, 220);
    static const ImU32 ScoreBarHigh   = IM_COL32( 80, 200, 100, 220);

    // Spacing.
    static const f32 PadX             = 8.0f;
    static const f32 PadY             = 6.0f;
    static const f32 RowGap           = 4.0f;
    static const f32 ChipGap          = 4.0f;
    static const f32 ChipPadX         = 6.0f;
    static const f32 ChipPadY         = 2.0f;
    static const f32 ScoreBarHeight   = 4.0f;
    static const f32 SidebarWidth     = 320.0f;
} // namespace Theme

// Apply a Vespucci-specific style scope. Caller wraps a Begin/End
// pair around their panel content. PopStyleScope() restores.
void PushVespucciStyleScope();
void PopVespucciStyleScope();

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_THEME_H_
