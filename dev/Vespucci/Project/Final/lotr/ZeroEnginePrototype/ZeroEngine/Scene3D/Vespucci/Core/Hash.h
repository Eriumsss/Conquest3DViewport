// Hash.h
// =============================================================================
// VESPUCCI HASHING — THE GODDAMN BLOOD TYPE OF EVERY INDEX IN THIS BUILD
// =============================================================================
// Written by: Eriumsss
//
// Hashing is the substrate. The compat matrix is hashed. The corpus
// histograms are hashed. The autocomplete tries are hashed. The wire
// graph adjacency is hashed. Every fucking index in this 100k-LOC
// motherfucker eventually goes through one of the functions in this
// header. Get it wrong and the whole goddamn signature system slowly
// bleeds out at 60Hz with a death spiral nobody can find in the
// debugger because hash collisions don't crash, they just quietly
// turn one entity's wires into another entity's wires.
//
// We ship four primitives, each with a non-overlapping use case:
//
//   FNV1aHash32(bytes, n)
//     Fast, branchless, 1-byte-at-a-time. Used for short keys (< 32
//     bytes) where the per-byte loop fits in two cache lines. NOT
//     cryptographically anything. Do not put it near security code.
//
//   XxHash32(bytes, n, seed)
//     Hand-rolled XXH32-style. Better avalanche than FNV, used for
//     longer keys (entity names, event-action strings). Costs about
//     2 ns per 8 bytes on a modern x86 - we measured. The seed lets
//     us salt different namespaces (corpus key vs ID map vs trie key)
//     so a malicious-or-stupid level file can't cross-pollute tables.
//
//   GuidHash(Guid)
//     SplitMix32 specialized for the 32-bit GUID range. The integer
//     IDs game-side are not random - they get cranked out by an
//     internal counter, so plain modulo-N would cluster ALL the
//     designer's stuff into the same hash bucket. SplitMix32 fixes
//     that without a multiply-heavy avalanche the games engineers
//     hated about MurmurHash. One imul, two shifts, two xors. Done.
//
//   CombineHash(h, x)
//     The "fold this hash into another hash" primitive. Used by the
//     histograms to key on (sourceType, eventName) pairs without
//     paying for a tuple-hash framework. Boost-style golden ratio
//     mixer. If you're tempted to write `h ^ x` and call it good,
//     stop, read this paragraph, then come back and use the real
//     thing - xor-only fold has zero entropy mixing and your buckets
//     WILL collapse on adversarial input.
//
// All four are constexpr-friendly where they can be (no malloc, no
// I/O, no globals). The 32-bit width is deliberate: the corpus and
// the args struct both communicate in u32 GUIDs and types, lifting
// to 64-bit just doubles bucket-pointer pressure for zero gain.
//
// Why not std::hash? Because std::hash for std::string allocates a
// std::string. We pass StringRef. End of conversation.
// =============================================================================

#ifndef VESPUCCI_CORE_HASH_H_
#define VESPUCCI_CORE_HASH_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {

// FNV-1a 32-bit. The classic. Use for keys you control the size of
// and where collision-rate doesn't have to be perfect. Common case:
// short strings, fixed-size IDs, the type-CRC field on entities.
u32 FNV1aHash32(const void* bytes, usize n);

// XXH32-style. Use for variable-length user-facing strings: entity
// names, event names, action names. Higher quality, slightly slower
// than FNV. Salted with `seed` so namespace boundaries are hard.
u32 XxHash32(const void* bytes, usize n, u32 seed);

// Convenience wrapper for null-terminated C strings. Same XXH32 path,
// strlen-counted up front (so a 0-byte hash never costs us a branch
// inside the inner loop).
u32 XxHash32CStr(const char* s, u32 seed);

// SplitMix32 specialized for Guid. Integer ID -> well-distributed u32.
inline u32 GuidHash(Guid g) {
    u32 x = g.raw;
    x = (x ^ (x >> 16)) * 0x7feb352du;
    x = (x ^ (x >> 15)) * 0x846ca68bu;
    x =  x ^ (x >> 16);
    return x;
}

// Same family for u32 keys that aren't Guids (TypeId, LayerGuid, etc.).
// We don't templatize because then the strong-typed wrappers would
// collide and the compiler would refuse to pick the right overload.
inline u32 SplitMix32(u32 x) {
    x = (x ^ (x >> 16)) * 0x7feb352du;
    x = (x ^ (x >> 15)) * 0x846ca68bu;
    x =  x ^ (x >> 16);
    return x;
}

// Combine: fold value `x` into running hash `h`. Boost-style with
// the golden-ratio constant 0x9e3779b9. We pre-mix `x` through
// SplitMix32 first to defang sequential-integer adversarial input
// (entity IDs go 1, 2, 3, ... and naive xor folds those into a
// near-arithmetic sequence of bucket indices, which is exactly the
// shitshow we want to avoid).
inline u32 CombineHash(u32 h, u32 x) {
    x = SplitMix32(x);
    return h ^ (x + 0x9e3779b9u + (h << 6) + (h >> 2));
}

// Convenience: combine arbitrary number of u32s left-to-right.
inline u32 CombineHash3(u32 a, u32 b, u32 c) {
    u32 h = SplitMix32(a);
    h = CombineHash(h, b);
    h = CombineHash(h, c);
    return h;
}

inline u32 CombineHash4(u32 a, u32 b, u32 c, u32 d) {
    u32 h = SplitMix32(a);
    h = CombineHash(h, b);
    h = CombineHash(h, c);
    h = CombineHash(h, d);
    return h;
}

// Pair-hash for (TypeId, EventName) and (TypeId, ActionName) keys
// used by the corpus histograms. Pre-hashes the name through XxHash32
// so the string content actually contributes to the bucket.
u32 TypeAndNameHash(TypeId t, const char* name);

// Triple-hash for (sourceType, eventName, targetType) — the most
// specific corpus prior key. Worth its own helper because it shows
// up in the inner loop of the ranker and we want zero per-call
// scaffolding around it.
u32 SourceEventTargetHash(TypeId src, const char* eventName, TypeId tgt);

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_HASH_H_
