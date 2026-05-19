// Hash.cpp
// =============================================================================
// XXH32 + FNV1a impls. Hand-rolled because pulling in the goddamn xxhash
// vendor header would mean another submodule, another version pin, another
// reason for the build to break at 11pm when somebody bumps the cmake.
// =============================================================================
// Written by: Eriumsss
//
// XXH32 reference: cyan4973/xxhash, public domain. We don't link the
// vendor library because it's fucking thirty thousand lines of macros
// and template porn for a function we use in three places. The hot
// loop here is hand-unrolled to four lanes, same as the upstream
// implementation, same multipliers, same rotates. Validated against
// upstream on the LOTR:C entity-name corpus: 100% match.
//
// FNV-1a is the no-brain fallback. We could have just shipped FNV
// for everything and moved on, but FNV's collision behavior on
// systematically-generated keys (those incrementing entity IDs we
// keep complaining about) sucks. XXH32 fixes that for the price of
// 4x more imuls per byte. We pay the price on user-facing strings
// where the names matter, skip it on internal IDs.
// =============================================================================

#include "Hash.h"

#include <cstring>

namespace Vespucci {
namespace Core {

// ── FNV-1a 32-bit ─────────────────────────────────────────────────────
// Hot path. Don't even think about touching it.

u32 FNV1aHash32(const void* bytes, usize n) {
    static const u32 kFnvOffset = 0x811c9dc5u;
    static const u32 kFnvPrime  = 0x01000193u;
    const u8* p = (const u8*)bytes;
    u32 h = kFnvOffset;
    for (usize i = 0; i < n; ++i) {
        h ^= (u32)p[i];
        h *= kFnvPrime;
    }
    return h;
}

// ── XXH32 ─────────────────────────────────────────────────────────────
// Hand-rolled to match upstream. Constants and rotates are exact.
// The four-lane main loop is the whole reason we use this thing.

namespace {
    static const u32 kXXH32_PRIME1 = 0x9E3779B1u;
    static const u32 kXXH32_PRIME2 = 0x85EBCA77u;
    static const u32 kXXH32_PRIME3 = 0xC2B2AE3Du;
    static const u32 kXXH32_PRIME4 = 0x27D4EB2Fu;
    static const u32 kXXH32_PRIME5 = 0x165667B1u;

    inline u32 RotL32(u32 x, i32 r) { return (x << r) | (x >> (32 - r)); }

    inline u32 ReadU32LE(const u8* p) {
        // Force unaligned reads through memcpy, the compiler folds
        // it to a single MOV on x86 and saves us a portability rant.
        u32 v;
        std::memcpy(&v, p, 4);
        return v;
    }

    inline u32 RoundXXH(u32 acc, u32 input) {
        acc += input * kXXH32_PRIME2;
        acc  = RotL32(acc, 13);
        acc *= kXXH32_PRIME1;
        return acc;
    }
} // namespace

u32 XxHash32(const void* bytes, usize n, u32 seed) {
    const u8* p   = (const u8*)bytes;
    const u8* end = p + n;
    u32 h32;

    if (n >= 16) {
        const u8* limit = end - 16;
        u32 v1 = seed + kXXH32_PRIME1 + kXXH32_PRIME2;
        u32 v2 = seed + kXXH32_PRIME2;
        u32 v3 = seed + 0;
        u32 v4 = seed - kXXH32_PRIME1;

        do {
            v1 = RoundXXH(v1, ReadU32LE(p));      p += 4;
            v2 = RoundXXH(v2, ReadU32LE(p));      p += 4;
            v3 = RoundXXH(v3, ReadU32LE(p));      p += 4;
            v4 = RoundXXH(v4, ReadU32LE(p));      p += 4;
        } while (p <= limit);

        h32 = RotL32(v1, 1) + RotL32(v2, 7) + RotL32(v3, 12) + RotL32(v4, 18);
    } else {
        h32 = seed + kXXH32_PRIME5;
    }

    h32 += (u32)n;

    while (p + 4 <= end) {
        h32 += ReadU32LE(p) * kXXH32_PRIME3;
        h32  = RotL32(h32, 17) * kXXH32_PRIME4;
        p += 4;
    }
    while (p < end) {
        h32 += (u32)(*p) * kXXH32_PRIME5;
        h32  = RotL32(h32, 11) * kXXH32_PRIME1;
        ++p;
    }

    h32 ^= h32 >> 15;
    h32 *= kXXH32_PRIME2;
    h32 ^= h32 >> 13;
    h32 *= kXXH32_PRIME3;
    h32 ^= h32 >> 16;
    return h32;
}

u32 XxHash32CStr(const char* s, u32 seed) {
    if (!s) return seed; // null is null, don't crash, don't strlen
    return XxHash32(s, std::strlen(s), seed);
}

// ── Compound key hashes ───────────────────────────────────────────────
// These get hammered in the corpus inner loop. Worth their own helpers
// to avoid throwing four-line key-construction blocks at every call site.

u32 TypeAndNameHash(TypeId t, const char* name) {
    // Salt 0xA17B7E5C is just a session-stable random constant. It
    // does not need to mean anything beyond "different from the
    // SourceEventTarget salt below so the two histograms have
    // independent collision profiles".
    u32 nh = XxHash32CStr(name, 0xA17B7E5Cu);
    return CombineHash(SplitMix32(t.raw), nh);
}

u32 SourceEventTargetHash(TypeId src, const char* eventName, TypeId tgt) {
    u32 eh = XxHash32CStr(eventName, 0xC0FFEE13u);
    u32 h  = SplitMix32(src.raw);
    h = CombineHash(h, eh);
    h = CombineHash(h, tgt.raw);
    return h;
}

} // namespace Core
} // namespace Vespucci
