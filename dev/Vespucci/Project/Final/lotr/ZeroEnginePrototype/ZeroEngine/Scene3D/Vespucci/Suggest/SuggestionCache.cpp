// SuggestionCache.cpp
// =============================================================================
// PER-FRAME MEMOIZATION OF (source, event) -> SuggestionList. KEYED ON
// SNAPSHOT VERSION SO ANY GRAPH MUTATION INVALIDATES THE WHOLE CACHE.
// =============================================================================
// Written by: Eriumsss
//
// Why a cache: the SuggestedWires sidebar runs the full ranker pipeline
// every frame. With Hybrid + 256 candidates + 7 features + the corpus's
// log-prob backoff cascade, that's ~50 ms of work per frame on a busy
// level. Memoizing the result keyed on (source, event, snapshot
// version) drops that to a hash lookup most frames since the designer
// rarely changes selection or event filter on every paint.
//
// Cache invalidation:
//   - Snapshot version bump → the whole cache is dropped (one-line
//     branch in the Vespucci::Update path). Cheap.
//   - Manual NukeTheCache() → called on level unload from outside.
//   - Memory cap → we cap entries at kCacheMaxEntries so a designer
//     who clicks every entity in a 5,000-entity level doesn't blow up
//     the working set. Eviction is FIFO (we don't bother with LRU
//     because we drop the whole thing on snapshot change anyway).
//
// Cache hit ratio is exposed via Stats() for the Brain panel — if the
// hit ratio is < 30% the snapshot is bumping more often than the user
// is interacting and there's a perf cliff to address upstream.
// =============================================================================

#include "SuggestionTypes.h"

#include "../Core/Hash.h"
#include "../Core/Logging.h"

#include <cstring>
#include <unordered_map>

namespace Vespucci {
namespace Suggest {

namespace {
    struct CacheKey {
        u32 sourceGuid;
        u32 eventHash;
        bool operator==(const CacheKey& o) const {
            return sourceGuid == o.sourceGuid && eventHash == o.eventHash;
        }
    };
    struct CacheKeyHash {
        usize operator()(const CacheKey& k) const {
            return (usize)Core::CombineHash(Core::SplitMix32(k.sourceGuid), k.eventHash);
        }
    };

    static SnapshotVersion s_versionUsed(0);
    static std::unordered_map<CacheKey, SuggestionList, CacheKeyHash> s_cache;

    // Hard cap on entries to prevent the cache from outgrowing a
    // designer's working set. When we cross this we evict half the
    // table (cheap, simple) — entries are reproducible by re-running
    // the ranker.
    static const i32 kCacheMaxEntries = 512;
    static const i32 kCacheEvictTo    = 256;

    // Hit/miss telemetry.
    static u32 s_hitCount  = 0;
    static u32 s_missCount = 0;
    static u32 s_storeCount = 0;
    static u32 s_evictionCount = 0;
} // namespace

void DropTheCacheLikeItOweYouMoney(SnapshotVersion newVersion) {
    if (newVersion == s_versionUsed) return;
    s_cache.clear();
    s_versionUsed = newVersion;
    Core::Logging::Debug("SuggestionCache: dropped, snapshot moved to v%u", newVersion.raw);
}

bool TryFetchOrFuckOff(Guid src, const Core::StringRef& evt,
                        SnapshotVersion currentVersion,
                        SuggestionList& outList)
{
    if (currentVersion != s_versionUsed) {
        DropTheCacheLikeItOweYouMoney(currentVersion);
        s_missCount++;
        return false;
    }
    CacheKey k;
    k.sourceGuid = src.raw;
    k.eventHash  = Core::XxHash32(evt.data(), evt.size(), 0xC0FFEE13u);
    std::unordered_map<CacheKey, SuggestionList, CacheKeyHash>::const_iterator it =
        s_cache.find(k);
    if (it == s_cache.end()) {
        s_missCount++;
        return false;
    }
    outList = it->second;
    s_hitCount++;
    return true;
}

void StoreOrEvictTheStaleBitchassBefore(Guid src, const Core::StringRef& evt,
                                          SnapshotVersion currentVersion,
                                          const SuggestionList& list)
{
    if (currentVersion != s_versionUsed) {
        DropTheCacheLikeItOweYouMoney(currentVersion);
    }
    CacheKey k;
    k.sourceGuid = src.raw;
    k.eventHash  = Core::XxHash32(evt.data(), evt.size(), 0xC0FFEE13u);
    s_cache[k] = list;
    s_storeCount++;

    // Eviction — drop half the entries when we cross the cap. Iteration
    // order over unordered_map is implementation-defined which is fine
    // for our purposes; we don't care WHICH half gets dropped.
    if ((i32)s_cache.size() > kCacheMaxEntries) {
        i32 toDrop = (i32)s_cache.size() - kCacheEvictTo;
        std::unordered_map<CacheKey, SuggestionList, CacheKeyHash>::iterator it =
            s_cache.begin();
        while (toDrop > 0 && it != s_cache.end()) {
            it = s_cache.erase(it);
            toDrop--;
            s_evictionCount++;
        }
        Core::Logging::Debug("SuggestionCache: evicted %d entries (now %zu)",
            (i32)s_cache.size() - kCacheEvictTo + (kCacheMaxEntries - kCacheEvictTo),
            s_cache.size());
    }
}

void NukeTheCache() {
    s_cache.clear();
    s_versionUsed = SnapshotVersion(0);
    s_hitCount = 0;
    s_missCount = 0;
    s_storeCount = 0;
    s_evictionCount = 0;
}

i32 CacheSize() { return (i32)s_cache.size(); }

// Telemetry for the Brain panel.
struct CacheStats {
    i32 size;
    u32 hits;
    u32 misses;
    u32 stores;
    u32 evictions;
    f32 hitRatio;        // hits / (hits + misses), 0..1
};

CacheStats GetCacheStats() {
    CacheStats s;
    s.size       = (i32)s_cache.size();
    s.hits       = s_hitCount;
    s.misses     = s_missCount;
    s.stores     = s_storeCount;
    s.evictions  = s_evictionCount;
    u32 total    = s_hitCount + s_missCount;
    s.hitRatio   = (total > 0) ? ((f32)s_hitCount / (f32)total) : 0.0f;
    return s;
}

void ResetCacheCounters() {
    s_hitCount = 0;
    s_missCount = 0;
    s_storeCount = 0;
    s_evictionCount = 0;
}

} // namespace Suggest
} // namespace Vespucci
