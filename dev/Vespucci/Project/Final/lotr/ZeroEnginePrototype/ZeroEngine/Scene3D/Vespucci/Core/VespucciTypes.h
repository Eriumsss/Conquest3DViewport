// VespucciTypes.h
// =============================================================================
// VESPUCCI SMART WIRING — common typedefs, IDs, and small POD types
// =============================================================================
// Written by: Eriumsss
//
// This is the bottom of the dependency stack. Every other file in Vespucci/
// includes this header transitively, so it MUST stay light: no heavy STL
// pulls, no Vespucci/* dependencies, no global statics. Just typedefs,
// strong-typed integer wrappers, and a few tiny POD value types that show
// up in dozens of public APIs (score_t, EntityIndex, snapshot version).
//
// Why strong typedefs and not plain uint32_t everywhere: a Guid and an
// EntityIndex are both 32-bit but they live in different universes and
// cannot mix. The compiler is the only honest enforcer; comments rot.
// We pay one constructor call and gain a forever-broken-build-on-mistake.
//
// Header rule: do NOT pull in <vector>, <string>, <map>, <unordered_map>
// from here. Anything that big lives in its own header (StringPool.h,
// SmallVec.h, etc). This file should preprocess in under 1 ms.
// =============================================================================

#ifndef VESPUCCI_CORE_VESPUCCITYPES_H_
#define VESPUCCI_CORE_VESPUCCITYPES_H_

#include <cstddef>
#include <cstdint>

namespace Vespucci {

// ── Primitive width aliases ──────────────────────────────────────────
// Mirror MSVC stdint.h on the names everyone here actually types. Yes,
// the std versions are fine; this just keeps Vespucci sources from
// looking like a typewriter exhibit when scrolled fast.
typedef std::int8_t   i8;
typedef std::int16_t  i16;
typedef std::int32_t  i32;
typedef std::int64_t  i64;
typedef std::uint8_t  u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::uint64_t u64;
typedef float         f32;
typedef double        f64;
typedef std::size_t   usize;
typedef std::ptrdiff_t isize;

// ── Strong-typed identifier wrappers ─────────────────────────────────
// These are the only IDs that flow through Vespucci's public APIs.
// Every accidental swap (entity-index used where guid is expected, or
// wire-handle used where output-handle is expected) becomes a compile
// error instead of a "why is the suggestion list empty for two days"
// bug. The cost is a thin wrapper struct; the benefit is invariant
// preservation across 100k LOC of code.
//
// Convention: each wrapper carries the SAME raw width as the underlying
// concept in ImGuiGlueFrameArgs (u32 for Guid, i32 for EntityIndex)
// because they ride those channels back to the host. Do not change
// widths casually.

// Game object GUID — the 32-bit hash that identifies an entity in the
// loaded level. Sourced from gameObjGuids[] on the args struct, lives
// in chainAdd*/chainDelete*/hostRequest* request fields.
struct Guid {
    u32 raw;

    Guid() : raw(0) {}
    explicit Guid(u32 v) : raw(v) {}

    bool valid() const { return raw != 0; }
    bool operator==(Guid o) const { return raw == o.raw; }
    bool operator!=(Guid o) const { return raw != o.raw; }
    bool operator<(Guid o)  const { return raw < o.raw; }
};

// Index into a snapshot's dense entity array. Stable for the lifetime
// of one snapshot version; resolved through EntityIndex.h. Use this
// inside Vespucci internals; convert to/from Guid at API boundaries.
struct EntityIndex {
    i32 raw;

    EntityIndex() : raw(-1) {}
    explicit EntityIndex(i32 v) : raw(v) {}

    bool valid() const { return raw >= 0; }
    bool operator==(EntityIndex o) const { return raw == o.raw; }
    bool operator!=(EntityIndex o) const { return raw != o.raw; }
    bool operator<(EntityIndex o)  const { return raw < o.raw; }
};

// Index into a snapshot's wire array. Same lifetime rules as EntityIndex.
struct WireIndex {
    i32 raw;

    WireIndex() : raw(-1) {}
    explicit WireIndex(i32 v) : raw(v) {}

    bool valid() const { return raw >= 0; }
    bool operator==(WireIndex o) const { return raw == o.raw; }
    bool operator!=(WireIndex o) const { return raw != o.raw; }
    bool operator<(WireIndex o)  const { return raw < o.raw; }
};

// Layer GUID — same width as Guid but kept as a separate type so the
// compiler stops you from putting an entity guid where a layer guid is
// wanted (every CapturePoint accidentally ending up in the Audio/VO
// layer is the kind of bug this prevents).
struct LayerGuid {
    u32 raw;

    LayerGuid() : raw(0) {}
    explicit LayerGuid(u32 v) : raw(v) {}

    bool valid() const { return raw != 0; }
    bool operator==(LayerGuid o) const { return raw == o.raw; }
    bool operator!=(LayerGuid o) const { return raw != o.raw; }
    bool operator<(LayerGuid o)  const { return raw < o.raw; }
};

// Type-id — a 32-bit hash of a type name (e.g. "CapturePoint"). Stored
// in Schema/ZETypeRegistry; not the same as the type-def-index from
// LevelReader (that one is a small dense int we don't expose here).
struct TypeId {
    u32 raw;

    TypeId() : raw(0) {}
    explicit TypeId(u32 v) : raw(v) {}

    bool valid() const { return raw != 0; }
    bool operator==(TypeId o) const { return raw == o.raw; }
    bool operator!=(TypeId o) const { return raw != o.raw; }
    bool operator<(TypeId o)  const { return raw < o.raw; }
};

// Snapshot version — monotonically increasing per-level mutation
// counter. Used by SuggestionCache to invalidate memoized results
// without comparing the entire snapshot. Wraps after 2^32 mutations
// which would take 100 years of continuous editing at 1 mutation/sec
// so we are not adding overflow handling.
struct SnapshotVersion {
    u32 raw;

    SnapshotVersion() : raw(0) {}
    explicit SnapshotVersion(u32 v) : raw(v) {}

    bool operator==(SnapshotVersion o) const { return raw == o.raw; }
    bool operator!=(SnapshotVersion o) const { return raw != o.raw; }
    SnapshotVersion next() const { return SnapshotVersion(raw + 1u); }
};

// ── Score & probability types ─────────────────────────────────────────
// All ranker scores live in this scalar. Centralizing the typedef means
// if we ever swap to f64 for tighter arithmetic, one edit covers the
// whole codebase. The naming intent: score_t is the additive raw score
// before normalization; conf_t is the gated 0..1 confidence post-gate.
typedef f32 score_t;
typedef f32 conf_t;

// Log-prior for backoff calculations. Use log space to avoid catastrophic
// underflow when multiplying many small probabilities. Stupid-Backoff,
// Kneser-Ney and Laplace all live in this domain.
typedef f32 logprob_t;

// ── Small POD bundles ─────────────────────────────────────────────────
// These are tiny enough to live in the type header. Anything bigger
// gets its own header.

// 3D position in world space. Mirrors gameObjPosX/Y/Z layout from the
// args struct so we can memcpy-build snapshot positions in bulk.
struct Vec3 {
    f32 x, y, z;

    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(f32 ix, f32 iy, f32 iz) : x(ix), y(iy), z(iz) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(f32 s) const          { return Vec3(x * s, y * s, z * s); }
    f32  dot(const Vec3& o) const        { return x * o.x + y * o.y + z * o.z; }
    f32  lengthSq() const                { return x * x + y * y + z * z; }
};

// AABB used by spatial indexes and by SnapshotDiff for "what changed
// in this region" queries.
struct Aabb {
    Vec3 mn;
    Vec3 mx;

    Aabb() {}
    Aabb(const Vec3& a, const Vec3& b) : mn(a), mx(b) {}

    bool contains(const Vec3& p) const {
        return p.x >= mn.x && p.x <= mx.x &&
               p.y >= mn.y && p.y <= mx.y &&
               p.z >= mn.z && p.z <= mx.z;
    }
    bool overlaps(const Aabb& o) const {
        if (mx.x < o.mn.x || mn.x > o.mx.x) return false;
        if (mx.y < o.mn.y || mn.y > o.mx.y) return false;
        if (mx.z < o.mn.z || mn.z > o.mx.z) return false;
        return true;
    }
    Vec3 center() const {
        return Vec3((mn.x + mx.x) * 0.5f,
                    (mn.y + mx.y) * 0.5f,
                    (mn.z + mx.z) * 0.5f);
    }
};

// Span pair used everywhere for variable-size views into shared arrays.
// Non-owning. The owning array's lifetime must outlive the span — every
// caller in Vespucci already gets spans off the immutable snapshot, so
// the lifetime story is "valid until the snapshot swaps".
template <typename T>
struct Span {
    const T* ptr;
    usize    n;

    Span() : ptr(0), n(0) {}
    Span(const T* p, usize c) : ptr(p), n(c) {}

    const T& operator[](usize i) const { return ptr[i]; }
    const T* begin() const             { return ptr; }
    const T* end()   const             { return ptr + n; }
    bool     empty() const             { return n == 0; }
    usize    size()  const             { return n; }
};

// ── Compile-time constants ─────────────────────────────────────────────
// Tunables that the rest of the codebase reads as constants. Anything
// that the user might want to retune lives in Suggest/weights.json
// instead — these are structural, not policy.

namespace Limits {
    // Max candidates the ranker considers per query. Beyond this we
    // shed by hard-filter aggressiveness. 256 covers the 99.9th
    // percentile observed in the LOTR:C corpus.
    static const i32 kMaxCandidatesPerQuery = 256;

    // Max suggestions shown in any UI surface. Inline = 3, sidebar = 5.
    // Doc-locked from both design docs.
    static const i32 kMaxInlineSuggestions  = 3;
    static const i32 kMaxSidebarSuggestions = 5;

    // Reason chip cap per suggestion. Beyond 3 reasons the explanation
    // becomes noise; either consolidate or drop the weakest.
    static const i32 kMaxReasonsPerSuggestion = 3;

    // Snapshot rebuild budget — anything past this and we log a perf
    // warning so the HotPathProfiler picks it up.
    static const f32 kSnapshotRebuildBudgetMs = 0.5f;

    // Inline UI surface latency budget. Doc-locked from the AAA spec.
    static const f32 kInlineLatencyBudgetMs = 5.0f;
    static const f32 kPopupLatencyBudgetMs  = 10.0f;
    static const f32 kSidebarLatencyBudgetMs = 30.0f;
} // namespace Limits

} // namespace Vespucci

// Hash specializations for the strong-typed wrappers so they can be
// keys in std::unordered_map without writing a new hasher every time.
// Lives in std:: per the standard convention; do NOT relocate.
#include <functional>
namespace std {
    template<> struct hash<Vespucci::Guid> {
        std::size_t operator()(Vespucci::Guid g) const {
            // Knuth multiplicative hash mixed with a 32-bit splitmix
            // step. Plenty of entropy for editor-scale ID counts.
            Vespucci::u32 x = g.raw;
            x = (x ^ (x >> 16)) * 0x7feb352du;
            x = (x ^ (x >> 15)) * 0x846ca68bu;
            x =  x ^ (x >> 16);
            return static_cast<std::size_t>(x);
        }
    };
    template<> struct hash<Vespucci::EntityIndex> {
        std::size_t operator()(Vespucci::EntityIndex e) const {
            return static_cast<std::size_t>(e.raw);
        }
    };
    template<> struct hash<Vespucci::WireIndex> {
        std::size_t operator()(Vespucci::WireIndex w) const {
            return static_cast<std::size_t>(w.raw);
        }
    };
    template<> struct hash<Vespucci::LayerGuid> {
        std::size_t operator()(Vespucci::LayerGuid l) const {
            Vespucci::u32 x = l.raw;
            x = (x ^ (x >> 16)) * 0x7feb352du;
            x = (x ^ (x >> 15)) * 0x846ca68bu;
            x =  x ^ (x >> 16);
            return static_cast<std::size_t>(x);
        }
    };
    template<> struct hash<Vespucci::TypeId> {
        std::size_t operator()(Vespucci::TypeId t) const {
            Vespucci::u32 x = t.raw;
            x = (x ^ (x >> 16)) * 0x7feb352du;
            x = (x ^ (x >> 15)) * 0x846ca68bu;
            x =  x ^ (x >> 16);
            return static_cast<std::size_t>(x);
        }
    };
} // namespace std

#endif // VESPUCCI_CORE_VESPUCCITYPES_H_
