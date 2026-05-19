// ColorRules.cpp
// =============================================================================
// PICK COLOR FOR A GHOST MARKER BASED ON CONFIDENCE + EVENT FAMILY
// =============================================================================
// Written by: Eriumsss
//
// Top-1 candidate gets a stronger green-ish tint; ties get a yellow-
// orange ambiguity hue; below-confidence-floor candidates fade to
// gray. The renderer uses these to communicate ranking AT-A-GLANCE
// without forcing the designer to read score numbers.
// =============================================================================

#include "../../Core/VespucciTypes.h"
#include "../../Core/StringRef.h"

namespace Vespucci {
namespace UI {
namespace GhostOverlay {

// 32-bit ABGR / RGBA-like packed color. Renderer shifts as needed.
struct GhostColor { unsigned int rgba; };

GhostColor PickColorForRank(i32 rankIdx, f32 confidence, f32 marginRatio) {
    // High-confidence top-1 with strong margin: vibrant green.
    if (rankIdx == 0 && confidence >= 0.8f && marginRatio >= 0.15f) {
        GhostColor c; c.rgba = 0xFF60E080u; return c; // RGBA green
    }
    // High-confidence but ambiguous (close margin): yellow.
    if (rankIdx == 0 && confidence >= 0.6f && marginRatio < 0.1f) {
        GhostColor c; c.rgba = 0xFFE0C040u; return c; // RGBA yellow
    }
    // Top-1 medium-confidence: cyan.
    if (rankIdx == 0) {
        GhostColor c; c.rgba = 0xFF60C0E0u; return c; // RGBA cyan
    }
    // Secondary candidates: dimmer cyan.
    if (rankIdx == 1) {
        GhostColor c; c.rgba = 0xC080A0C0u; return c;
    }
    if (rankIdx == 2) {
        GhostColor c; c.rgba = 0xA070809Au; return c;
    }
    // Anything past top-3: nearly transparent gray.
    GhostColor c; c.rgba = 0x60707070u; return c;
}

GhostColor PickConnectorColorByEvent(const Core::StringRef& evt) {
    // Quick lookup table seeded from the doc's event family palette
    // (matches the F4 wire renderer's color-by-event convention).
    auto Has = [&](const char* needle) {
        usize n = 0; while (needle[n]) ++n;
        if (n > evt.size()) return false;
        for (usize i = 0; i + n <= evt.size(); ++i) {
            bool ok = true;
            for (usize k = 0; k < n; ++k) {
                char a = evt.data()[i + k]; char b = needle[k];
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };
    if (Has("capture")     || Has("neutralize"))   { GhostColor c; c.rgba = 0xFFFFD040u; return c; }
    if (Has("death")       || Has("die"))           { GhostColor c; c.rgba = 0xFFFF4040u; return c; }
    if (Has("trigger"))                             { GhostColor c; c.rgba = 0xFF40DDFFu; return c; }
    if (Has("enter")       || Has("exit"))          { GhostColor c; c.rgba = 0xFFFFEE40u; return c; }
    if (Has("spawn")       || Has("reinforcement")) { GhostColor c; c.rgba = 0xFFFF40FFu; return c; }
    if (Has("complete")    || Has("threshold"))     { GhostColor c; c.rgba = 0xFF40FF60u; return c; }
    GhostColor c; c.rgba = 0xFFCCCCCCu; return c;
}

} // namespace GhostOverlay
} // namespace UI
} // namespace Vespucci
