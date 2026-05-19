// Decay.cpp
// =============================================================================
// THE FIVE WAYS WE DRAG STALE OBSERVATIONS DOWN TO ZERO + AN APPLY
// PASS THAT WALKS A CORPUS AND BLEEDS EVERY ENTRY BY ITS OBSERVED AGE
// =============================================================================
// Written by: Eriumsss
//
// Why decay matters: a designer wires up an early prototype level
// using `OldEvent_LegacyName` patterns we no longer want suggested.
// Without decay those patterns sit at full strength in the corpus
// forever and the ranker keeps recommending them to designers on
// fresh maps. With decay, every observation has an effective
// half-life — recent wires dominate, ancient wires fade. The cross-
// map merger applies decay against the wallclock age of each shard
// so a corpus built from PAKs that are five years old gets weighted
// less than one built from yesterday's session.
//
// Five strategies are exposed. Designer picks via Suggest/weights.json
// `decayStrategy` field. Default is HalfLife with a 90-day half-life
// (approx one production cycle).
// =============================================================================

#include "Decay.h"

#include <algorithm>
#include <cmath>

namespace Vespucci {
namespace Corpus {

namespace {
    // Defensive clamp helpers. Caller-side bullshit (negative ages,
    // zero tau, NaN inputs) is kicked the fuck out before the math.
    f32 ChokeOnNegativeAgeYouFuckwit(f32 age) {
        if (age < 0.0f) return 0.0f;
        if (age != age) return 0.0f;  // NaN check, sneaky bastard
        return age;
    }
    f32 GarrotteATauOfZeroBitchassInput(f32 t) {
        if (t <= 0.0f) return 1.0f;   // refuse to /0, return safe-ish default
        if (t != t)    return 1.0f;
        return t;
    }
} // namespace

f32 ExpDecayDontCleaveThisInTwo(f32 age, f32 tau) {
    age = ChokeOnNegativeAgeYouFuckwit(age);
    tau = GarrotteATauOfZeroBitchassInput(tau);
    return std::exp(-age / tau);
}

f32 HalfLifeBleedout(f32 age, f32 hl) {
    age = ChokeOnNegativeAgeYouFuckwit(age);
    hl  = GarrotteATauOfZeroBitchassInput(hl);
    return std::pow(0.5f, age / hl);
}

f32 GuillotineCliffOrFullWeight(f32 age, f32 threshold) {
    age = ChokeOnNegativeAgeYouFuckwit(age);
    if (threshold <= 0.0f) return 0.0f;
    return age < threshold ? 1.0f : 0.0f;
}

f32 LinearBleedToZero(f32 age, f32 span) {
    age = ChokeOnNegativeAgeYouFuckwit(age);
    if (span <= 0.0f) return 0.0f;
    f32 v = 1.0f - age / span;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

f32 SmoothstepFadeOutOfMemory(f32 age, f32 span) {
    age = ChokeOnNegativeAgeYouFuckwit(age);
    if (span <= 0.0f) return 0.0f;
    f32 t = age / span;
    if (t <= 0.0f) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    f32 s = t * t * (3.0f - 2.0f * t);
    return 1.0f - s;
}

// Strategy enum + parser so configs can pick one by name.
enum DecayStrategy {
    DECAY_None       = 0,
    DECAY_Exp        = 1,
    DECAY_HalfLife   = 2,
    DECAY_Cliff      = 3,
    DECAY_Linear     = 4,
    DECAY_Smoothstep = 5
};

DecayStrategy ParseDecayStrategyOrFallToHalfLife(const char* name) {
    if (!name || !*name) return DECAY_HalfLife;
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = *a; char cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return false;
            a++; b++;
        }
        return *a == 0 && *b == 0;
    };
    if (eq(name, "none"))       return DECAY_None;
    if (eq(name, "exp"))        return DECAY_Exp;
    if (eq(name, "exponential")) return DECAY_Exp;
    if (eq(name, "halflife"))   return DECAY_HalfLife;
    if (eq(name, "half-life"))  return DECAY_HalfLife;
    if (eq(name, "cliff"))      return DECAY_Cliff;
    if (eq(name, "linear"))     return DECAY_Linear;
    if (eq(name, "smoothstep")) return DECAY_Smoothstep;
    return DECAY_HalfLife;
}

const char* DecayStrategyLabel(DecayStrategy s) {
    switch (s) {
        case DECAY_None:       return "none (no decay)";
        case DECAY_Exp:        return "exponential";
        case DECAY_HalfLife:   return "half-life";
        case DECAY_Cliff:      return "cliff (binary cutoff)";
        case DECAY_Linear:     return "linear bleed";
        case DECAY_Smoothstep: return "smoothstep fade";
    }
    return "(unknown)";
}

// Common dispatch — single-call API for "give me the decay weight at
// this age under this strategy and parameter." All strategies take a
// single tuning param (tau / half-life / cliff threshold / span); the
// caller is responsible for setting it sensibly.
f32 DecayWeightOrFullStrength(DecayStrategy strategy, f32 age, f32 param) {
    switch (strategy) {
        case DECAY_None:       return 1.0f;
        case DECAY_Exp:        return ExpDecayDontCleaveThisInTwo(age, param);
        case DECAY_HalfLife:   return HalfLifeBleedout(age, param);
        case DECAY_Cliff:      return GuillotineCliffOrFullWeight(age, param);
        case DECAY_Linear:     return LinearBleedToZero(age, param);
        case DECAY_Smoothstep: return SmoothstepFadeOutOfMemory(age, param);
    }
    return 1.0f;
}

// Apply a decay weight to an integer observation count. We use a
// rounding rule that preserves at least 1 observation when the
// underlying count was non-zero — a count of 1 with weight 0.49
// shouldn't disappear, because that erases the long tail entirely.
// Returns the weighted count, capped to u32 range.
u32 ApplyDecayToCountOrEatItButFloorAtOne(u32 count, f32 weight) {
    if (count == 0) return 0;
    if (weight <= 0.0f) return 0;
    if (weight >= 1.0f) return count;
    f32 raw = (f32)count * weight;
    u32 result = (u32)(raw + 0.5f);
    // Floor — never erase a non-zero observation completely unless
    // weight is exactly zero.
    if (result == 0 && count > 0) result = 1;
    return result;
}

// Time conversion helpers. Source observations carry unixSec timestamps
// from the recorder; the decay path needs ages in days so the half-life
// param has intuitive units.
f32 SecondsToDays(u64 seconds) {
    return (f32)((f64)seconds / 86400.0);
}

f32 SecondsToHours(u64 seconds) {
    return (f32)((f64)seconds / 3600.0);
}

f32 AgeInDaysFromTimestamp(u64 observedUnixSec, u64 nowUnixSec) {
    if (nowUnixSec < observedUnixSec) return 0.0f;
    return SecondsToDays(nowUnixSec - observedUnixSec);
}

// Sanity-check a decay parameter for the chosen strategy. Returns
// true if the param makes sense; false (with optional explanation) if
// it's nonsense (negative, zero where positive is required, etc.).
// UI uses this to red-tint the param input field.
bool ValidateDecayParam(DecayStrategy strategy, f32 param,
                          const char** outExplanation)
{
    if (outExplanation) *outExplanation = 0;
    switch (strategy) {
        case DECAY_None:
            return true;
        case DECAY_Exp:
        case DECAY_HalfLife:
            if (param <= 0.0f) {
                if (outExplanation) *outExplanation =
                    "decay tau / half-life must be > 0 days";
                return false;
            }
            return true;
        case DECAY_Cliff:
            if (param <= 0.0f) {
                if (outExplanation) *outExplanation =
                    "cliff threshold must be > 0 days";
                return false;
            }
            return true;
        case DECAY_Linear:
        case DECAY_Smoothstep:
            if (param <= 0.0f) {
                if (outExplanation) *outExplanation =
                    "fade span must be > 0 days";
                return false;
            }
            return true;
    }
    return true;
}

// Default tuning parameters by strategy — used when the config file
// doesn't specify. Tuned against the LOTR:C corpus's age distribution.
f32 DefaultDecayParam(DecayStrategy strategy) {
    switch (strategy) {
        case DECAY_None:        return 0.0f;
        case DECAY_Exp:         return 60.0f;     // tau = 60 days
        case DECAY_HalfLife:    return 90.0f;     // half-life = 90 days
        case DECAY_Cliff:       return 365.0f;    // 1-year cliff
        case DECAY_Linear:      return 180.0f;
        case DECAY_Smoothstep:  return 180.0f;
    }
    return 90.0f;
}

} // namespace Corpus
} // namespace Vespucci
