// Pulse.cpp
// =============================================================================
// SUBTLE PULSE ANIMATION FOR GHOST MARKERS - DRAWS ATTENTION WITHOUT
// BEING THE OBNOXIOUS YOUTUBE-AD KIND OF FLASHING
// =============================================================================
// Written by: Eriumsss
//
// Smooth sinusoidal alpha modulation, ~1.2 Hz. Phase-shifted per
// candidate so the three ghosts don't all blink together (which
// would feel like a goddamn alarm and flunk the visual quiet bar
// the doc set for this surface).
// =============================================================================

#include "../../Core/VespucciTypes.h"

#include <cmath>

namespace Vespucci {
namespace UI {
namespace GhostOverlay {

f32 ComputePulseAlpha(f32 timeSec, i32 candidateIdx, f32 baseAlpha) {
    if (timeSec < 0.0f) timeSec = 0.0f;
    f32 phase = (f32)candidateIdx * 0.7f; // stagger the three ghosts
    f32 omega = 6.2831853f * 1.2f;        // ~1.2 Hz
    f32 mod = 0.5f + 0.5f * std::sin(omega * timeSec + phase);
    f32 alpha = baseAlpha * (0.7f + 0.3f * mod);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    return alpha;
}

f32 ComputeRingScale(f32 timeSec, i32 candidateIdx) {
    f32 phase = (f32)candidateIdx * 0.4f;
    f32 omega = 6.2831853f * 0.8f;
    return 1.0f + 0.15f * std::sin(omega * timeSec + phase);
}

} // namespace GhostOverlay
} // namespace UI
} // namespace Vespucci
