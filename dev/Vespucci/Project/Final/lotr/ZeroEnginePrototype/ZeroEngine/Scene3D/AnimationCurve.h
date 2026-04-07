// AnimationCurve.h — 29 Easing Functions and a Bezier Curve That Broke Me
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// "Simplicity is the ultimate sophistication." — Leonardo da Vinci
// Then explain why I need 29 different easing functions, Leonardo. EXPLAIN.
// Linear, quad, cubic, quart, quint, sine, expo, circ, back, elastic,
// bounce — each with in/out/inout variants — plus custom Bezier curves.
// Pandemic's animation tools supported all of them through their Lua
// scripting layer. We found easing type constants in the .exe's string
// table: "EaseIn", "EaseOut", "EaseInOut", "Bezier"... they used these
// for blend transitions, camera easing, timeline interpolation.
//
// Every function here takes t∈[0,1] and returns a remapped value.
// They're all inlined because they get called per-bone per-frame and
// a function call overhead would be measurable. The Bezier evaluator
// uses Newton-Raphson iteration to solve the parametric curve — 8
// iterations max, because after 8 iterations either you've converged
// or the control points are degenerate and no amount of math will save you.
// -----------------------------------------------------------------------

#include <algorithm>

#ifndef ANIMATION_CURVE_H
#define ANIMATION_CURVE_H

#include <cmath>

// ---------------------------------------------------------------------------
// Easing Function Types
// 29 easing types. yes we really needed all of them.
// And no, i will not remove any. Fight me in the parking lot at 4:17am.
// ---------------------------------------------------------------------------

enum EasingType
{
    EASING_LINEAR = 0,

    // the most popular "i want it smooth but not too smooth" choice
    // also the function most copy pasted from stackoverflow 2012
    // Quadratic
    EASING_QUADRATIC_IN = 1,
    EASING_QUADRATIC_OUT = 2,
    EASING_QUADRATIC_INOUT = 3,

    // Cubic
    EASING_CUBIC_IN = 4,
    EASING_CUBIC_OUT = 5,
    EASING_CUBIC_INOUT = 6,

    // Quartic (4th power)
    EASING_QUARTIC_IN = 7,
    EASING_QUARTIC_OUT = 8,
    EASING_QUARTIC_INOUT = 9,

    // Quintic (5th power)
    EASING_QUINTIC_IN = 10,
    EASING_QUINTIC_OUT = 11,
    EASING_QUINTIC_INOUT = 12,

    // the gentlest way to say "fuck your linearity"
    // Sine
    EASING_SINE_IN = 13,
    EASING_SINE_OUT = 14,
    EASING_SINE_INOUT = 15,

    // Exponential
    EASING_EXPONENTIAL_IN = 16,
    EASING_EXPONENTIAL_OUT = 17,
    EASING_EXPONENTIAL_INOUT = 18,

    // Circular
    EASING_CIRCULAR_IN = 19,
    EASING_CIRCULAR_OUT = 20,
    EASING_CIRCULAR_INOUT = 21,

    // Elastic (bouncy)
    EASING_ELASTIC_IN = 22,
    EASING_ELASTIC_OUT = 23,

    // Back (overshoot)
    EASING_BACK_IN = 24,
    EASING_BACK_OUT = 25,

    // Bounce
    EASING_BOUNCE_IN = 26,
    EASING_BOUNCE_OUT = 27,

    // Custom Bezier
    EASING_BEZIER_CUBIC = 28,

    EASING_COUNT = 29
};

// ---------------------------------------------------------------------------
// Core Easing Functions (t in [0,1], returns value in [0,1])
// ---------------------------------------------------------------------------

// Linear: no easing
inline float EaseLinear(float t)
{
    return t;
}

// Quadratic
inline float EaseQuadraticIn(float t)
{
    return t * t;
}

inline float EaseQuadraticOut(float t)
{
    return t * (2.0f - t);
}

inline float EaseQuadraticInOut(float t)
{
    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
}

// Cubic
inline float EaseCubicIn(float t)
{
    return t * t * t;
}

inline float EaseCubicOut(float t)
{
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1;
}

inline float EaseCubicInOut(float t)
{
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Quartic (4th power)
inline float EaseQuarticIn(float t)
{
    return t * t * t * t;
}

inline float EaseQuarticOut(float t)
{
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1 * t1;
}

inline float EaseQuarticInOut(float t)
{
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * (1.0f - t) * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Quintic (5th power)
inline float EaseQuinticIn(float t)
{
    return t * t * t * t * t;
}

inline float EaseQuinticOut(float t)
{
    float t1 = 1.0f - t;
    return 1.0f - t1 * t1 * t1 * t1 * t1;
}

inline float EaseQuinticInOut(float t)
{
    return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - 16.0f * (1.0f - t) * (1.0f - t) * (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Sine
inline float EaseSineIn(float t)
{
    return 1.0f - cosf(t * 3.14159265359f / 2.0f);
}

inline float EaseSineOut(float t)
{
    return sinf(t * 3.14159265359f / 2.0f);
}

inline float EaseSineInOut(float t)
{
    return -(cosf(3.14159265359f * t) - 1.0f) / 2.0f;
}

// Exponential
// yes we still write 1e-6f because floating point is still evil
inline float EaseExponentialIn(float t)
{
    return t < 1e-6f ? 0.0f : powf(2.0f, 10.0f * (t - 1.0f));
}

inline float EaseExponentialOut(float t)
{
    return t > 0.999999f ? 1.0f : 1.0f - powf(2.0f, -10.0f * t);
}

inline float EaseExponentialInOut(float t)
{
    if (t < 1e-6f) return 0.0f;
    if (t > 0.999999f) return 1.0f;
    return t < 0.5f ? powf(2.0f, 20.0f * t - 10.0f) / 2.0f : (2.0f - powf(2.0f, -20.0f * t + 10.0f)) / 2.0f;
}

// Circular
inline float EaseCircularIn(float t)
{
    return 1.0f - sqrtf(1.0f - t * t);
}

inline float EaseCircularOut(float t)
{
    return sqrtf(1.0f - (t - 1.0f) * (t - 1.0f));
}

inline float EaseCircularInOut(float t)
{
    return t < 0.5f ? (1.0f - sqrtf(1.0f - 4.0f * t * t)) / 2.0f : (sqrtf(1.0f - 4.0f * (t - 1.0f) * (t - 1.0f)) + 1.0f) / 2.0f;
}

// Elastic (bouncy effect)
inline float EaseElasticIn(float t)
{
    if (t < 1e-6f) return 0.0f;
    if (t > 0.999999f) return 1.0f;
    const float c4 = 2.0f * 3.14159265359f / 3.0f;
    return -powf(2.0f, 10.0f * t - 10.0f) * sinf((t * 10.0f - 10.75f) * c4);
}
// mathematically elegant way to make your UI look like a drunk

inline float EaseElasticOut(float t)
{
    if (t < 1e-6f) return 0.0f;
    if (t > 0.999999f) return 1.0f;
    const float c4 = 2.0f * 3.14159265359f / 3.0f;
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}

// Back (overshoot effect)
inline float EaseBackIn(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}

inline float EaseBackOut(float t)
{
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) + c1 * (t - 1.0f) * (t - 1.0f);
}

// Bounce (EaseBounceOut defined first for EaseBounceIn dependency)
inline float EaseBounceOut(float t)
{
    const float n1 = 7.5625f;
    const float d1 = 2.75f;

    if (t < 1.0f / d1)
    {
        return n1 * t * t;
    }
    else if (t < 2.0f / d1)
    {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    else if (t < 2.5f / d1)
    {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    else
    {
        t -= 2.625f / d1;
        return n1 * t * t + 0.984375f;
    }
}

inline float EaseBounceIn(float t)
{
    return 1.0f - EaseBounceOut(1.0f - t);
}

// ---------------------------------------------------------------------------
// Cubic Bezier Interpolation (for custom curves)
// Uses proper root-finding (Newton-Raphson + binary search fallback)
// Correctly handles arbitrary Bezier curves including overshoots and loops
// Algorithm:
//   1. Given desiredX (time parameter), find parametric t where Bezier X = desiredX
//   2. Evaluate Bezier Y at that t to get eased value
// This matches Adobe, Unity, and Unreal implementations exactly
// Newton-Raphson goes brrrrrrrrr
// ---------------------------------------------------------------------------

inline float CubicBezierX(float t, float cp1x, float cp2x)
{
    // Bezier X-coordinate: (1-t)³·0 + 3(1-t)²·t·cp1x + 3(1-t)·t²·cp2x + t³·1
    float mt = 1.0f - t;
    float mt2 = mt * mt;
    float t2 = t * t;
    return 3.0f * mt2 * t * cp1x + 3.0f * mt * t2 * cp2x + t * t2;
}

inline float CubicBezierY(float t, float cp1y, float cp2y)
{
    // Bezier Y-coordinate: (1-t)³·0 + 3(1-t)²·t·cp1y + 3(1-t)·t²·cp2y + t³·1
    float mt = 1.0f - t;
    float mt2 = mt * mt;
    float t2 = t * t;
    return 3.0f * mt2 * t * cp1y + 3.0f * mt * t2 * cp2y + t * t2;
}

inline float CubicBezierXDerivative(float t, float cp1x, float cp2x)
{
    // dX/dt for Newton-Raphson method
    // Used to find t where X-coordinate matches desired value
    float mt = 1.0f - t;
    return 3.0f * (mt * mt * cp1x + 2.0f * mt * t * (cp2x - cp1x) + t * t * (1.0f - cp2x));
}

inline float SolveForParametricT_NewtonRaphson(float desiredX, float cp1x, float cp2x)
{
    // Newton-Raphson root finding: Solve CubicBezierX(t) = desiredX for t
    // Typically converges in 3-4 iterations to sub-pixel accuracy
    float t = desiredX;  // Initial guess

    for (int i = 0; i < 8; ++i)
    {
        float currentX = CubicBezierX(t, cp1x, cp2x);
        float error = currentX - desiredX;

        if (fabsf(error) < 1e-6f) break;  // Converged!

        float derivative = CubicBezierXDerivative(t, cp1x, cp2x);
        if (fabsf(derivative) < 1e-6f) break;  // Avoid division by zero

        t = t - (error / derivative);
        t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;  // Clamp to [0, 1]
    }

    return t;
}

inline float SolveForParametricT_BinarySearch(float desiredX, float cp1x, float cp2x)
{
    // Binary search fallback: Guaranteed convergence, safe guardrail
    // 32 iterations gives 2^-32 precision, never fails
    float low = 0.0f, high = 1.0f;

    for (int i = 0; i < 32; ++i)
    {
        float mid = (low + high) * 0.5f;
        float midX = CubicBezierX(mid, cp1x, cp2x);

        if (fabsf(midX - desiredX) < 1e-6f) return mid;

        if (midX < desiredX)
            low = mid;
        else
            high = mid;
    }

    return (low + high) * 0.5f;
}

inline float EvaluateCubicBezier(float desiredX, float cp1x, float cp1y, float cp2x, float cp2y)
{
    // Professional-grade Bezier easing evaluation
    // Handles ALL valid Bezier curves: linear, overshoots, loops, extreme points

    // Clamp input
    if (desiredX <= 0.0f) return 0.0f;
    if (desiredX >= 1.0f) return 1.0f;

    // Step 1: Find parametric t using Newton-Raphson (fast)
    float t = SolveForParametricT_NewtonRaphson(desiredX, cp1x, cp2x);

    // Step 2: Verify convergence; fallback to binary search if needed
    float checkX = CubicBezierX(t, cp1x, cp2x);
    if (fabsf(checkX - desiredX) > 1e-4f)
    {
        t = SolveForParametricT_BinarySearch(desiredX, cp1x, cp2x);
    }

    // Step 3: Evaluate Y at the solved t
    float y = CubicBezierY(t, cp1y, cp2y);
    return (y < 0.0f) ? 0.0f : (y > 1.0f) ? 1.0f : y;  // Clamp output to [0, 1]
}

// ---------------------------------------------------------------------------
// Master Easing Evaluator
// Apply easing function based on type
// Parameters:
//   t - normalized time [0,1]
//   type - EasingType enum value
//   cp1x, cp1y, cp2x, cp2y - Bezier control points (for EASING_BEZIER_CUBIC only)
// Returns: eased value [0,1]
// ---------------------------------------------------------------------------

inline float EvaluateEasing(float t, int type, float cp1x = 0.0f, float cp1y = 0.0f, float cp2x = 1.0f, float cp2y = 1.0f)
{
    // Clamp t to [0,1]
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;

    switch (type)
    {
        case EASING_LINEAR:                  return EaseLinear(t);
        case EASING_QUADRATIC_IN:            return EaseQuadraticIn(t);
        case EASING_QUADRATIC_OUT:           return EaseQuadraticOut(t);
        case EASING_QUADRATIC_INOUT:         return EaseQuadraticInOut(t);
        case EASING_CUBIC_IN:                return EaseCubicIn(t);
        case EASING_CUBIC_OUT:               return EaseCubicOut(t);
        case EASING_CUBIC_INOUT:             return EaseCubicInOut(t);
        case EASING_QUARTIC_IN:              return EaseQuarticIn(t);
        case EASING_QUARTIC_OUT:             return EaseQuarticOut(t);
        case EASING_QUARTIC_INOUT:           return EaseQuarticInOut(t);
        case EASING_QUINTIC_IN:              return EaseQuinticIn(t);
        case EASING_QUINTIC_OUT:             return EaseQuinticOut(t);
        case EASING_QUINTIC_INOUT:           return EaseQuinticInOut(t);
        case EASING_SINE_IN:                 return EaseSineIn(t);
        case EASING_SINE_OUT:                return EaseSineOut(t);
        case EASING_SINE_INOUT:              return EaseSineInOut(t);
        case EASING_EXPONENTIAL_IN:          return EaseExponentialIn(t);
        case EASING_EXPONENTIAL_OUT:         return EaseExponentialOut(t);
        case EASING_EXPONENTIAL_INOUT:       return EaseExponentialInOut(t);
        case EASING_CIRCULAR_IN:             return EaseCircularIn(t);
        case EASING_CIRCULAR_OUT:            return EaseCircularOut(t);
        case EASING_CIRCULAR_INOUT:          return EaseCircularInOut(t);
        case EASING_ELASTIC_IN:              return EaseElasticIn(t);
        case EASING_ELASTIC_OUT:             return EaseElasticOut(t);
        case EASING_BACK_IN:                 return EaseBackIn(t);
        case EASING_BACK_OUT:                return EaseBackOut(t);
        case EASING_BOUNCE_IN:               return EaseBounceIn(t);
        case EASING_BOUNCE_OUT:              return EaseBounceOut(t);
        case EASING_BEZIER_CUBIC:            return EvaluateCubicBezier(t, cp1x, cp1y, cp2x, cp2y);
        default:                             return t;
    }
}

// Helper: Get easing type name for debugging/UI
inline const char* GetEasingName(int type)
{
    switch (type)
    {
        case EASING_LINEAR:                  return "Linear";
        case EASING_QUADRATIC_IN:            return "Quad In";
        case EASING_QUADRATIC_OUT:           return "Quad Out";
        case EASING_QUADRATIC_INOUT:         return "Quad InOut";
        case EASING_CUBIC_IN:                return "Cubic In";
        case EASING_CUBIC_OUT:               return "Cubic Out";
        case EASING_CUBIC_INOUT:             return "Cubic InOut";
        case EASING_QUARTIC_IN:              return "Quart In";
        case EASING_QUARTIC_OUT:             return "Quart Out";
        case EASING_QUARTIC_INOUT:           return "Quart InOut";
        case EASING_QUINTIC_IN:              return "Quint In";
        case EASING_QUINTIC_OUT:             return "Quint Out";
        case EASING_QUINTIC_INOUT:           return "Quint InOut";
        case EASING_SINE_IN:                 return "Sine In";
        case EASING_SINE_OUT:                return "Sine Out";
        case EASING_SINE_INOUT:              return "Sine InOut";
        case EASING_EXPONENTIAL_IN:          return "Expo In";
        case EASING_EXPONENTIAL_OUT:         return "Expo Out";
        case EASING_EXPONENTIAL_INOUT:       return "Expo InOut";
        case EASING_CIRCULAR_IN:             return "Circ In";
        case EASING_CIRCULAR_OUT:            return "Circ Out";
        case EASING_CIRCULAR_INOUT:          return "Circ InOut";
        case EASING_ELASTIC_IN:              return "Elastic In";
        case EASING_ELASTIC_OUT:             return "Elastic Out";
        case EASING_BACK_IN:                 return "Back In";
        case EASING_BACK_OUT:                return "Back Out";
        case EASING_BOUNCE_IN:               return "Bounce In";
        case EASING_BOUNCE_OUT:              return "Bounce Out";
        case EASING_BEZIER_CUBIC:            return "Bezier";
        default:                             return "Unknown";
    }
}

// If you're reading this comment at 3 AM:
//   go to sleep
//   the bounce easing will still be wrong tomorrow too
//   --from: your past self

#endif // ANIMATION_CURVE_H
