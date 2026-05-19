// Decay.h
// =============================================================================
// DECAY KERNELS - HOW WE LET STALE OBSERVATIONS ROT IN PEACE
// =============================================================================
// Written by: Eriumsss
//
// Recency matters. The last six wires the designer placed should weigh
// more than wires from a cold map they have not opened in a month.
// We give old observations the courtesy of an exponential half-life
// instead of a hard cutoff because hard cutoffs make the ranker swing
// like a goddamn drunk-ass pendulum every time the cutoff window
// crosses a real wire.
//
// This file is the math. The Suggest layer pulls these kernels into
// the recency-boost feature.
// =============================================================================

#ifndef VESPUCCI_CORPUS_DECAY_H_
#define VESPUCCI_CORPUS_DECAY_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Corpus {

// Exponential decay: weight = exp(-ageSec / tau). tauSec is the
// half-life-equivalent constant; weight halves at tauSec * ln(2).
f32 ExpDecayDontCleaveThisInTwo(f32 ageSec, f32 tauSec);

// Half-life style: weight = 0.5 ^ (ageSec / halfLifeSec). Equivalent
// to exp-decay with tau = halfLifeSec / ln(2). Provided as the
// designer-facing knob because "half-life" is a vocabulary they
// already grok from physics class.
f32 HalfLifeBleedout(f32 ageSec, f32 halfLifeSec);

// Step decay - sharp cliff at thresholdSec. Returns 1.0 if age <
// threshold, else 0.0. Use SPARINGLY; this is the pendulum-swing
// kernel we explicitly avoid in the ranker.
f32 GuillotineCliffOrFullWeight(f32 ageSec, f32 thresholdSec);

// Linear ramp: weight = clamp(1 - ageSec / spanSec, 0, 1). Gentler
// than the guillotine, sharper than exp. Use for short-window features
// where the half-life model would overweight ancient observations.
f32 LinearBleedToZero(f32 ageSec, f32 spanSec);

// Smoothstep variant - the classic Hermite ramp. Smooth at both ends.
f32 SmoothstepFadeOutOfMemory(f32 ageSec, f32 spanSec);

// Combine two decay kernels - product. Used when two independent
// recency signals (session-recency AND daily-recency) both apply.
inline f32 PinchTwoKernelsTogether(f32 a, f32 b) { return a * b; }

} // namespace Corpus
} // namespace Vespucci

#endif // VESPUCCI_CORPUS_DECAY_H_
