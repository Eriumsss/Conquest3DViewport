// CorpusPriors.h
// =============================================================================
// CORPUS PRIORS - THE FUCKING BACKOFF LADDER WE BUILT BECAUSE EA SHUT
// PANDEMIC DOWN BEFORE THEY COULD SHIP A REAL ONE
// =============================================================================
// Written by: Eriumsss
//
// Real talk: there is no official LOTR:C wiring corpus. Pandemic had
// internal authoring tools, internal docs, internal pattern libraries,
// and EA fed all of that into the goddamn wood-chipper in 2009 when
// they killed the studio. Every wire pattern in this priors table was
// reverse-engineered from binary .lvl files, cross-referenced against
// the level.json memory, and counted by hand the first time before we
// automated the corpus builder. The priors are us doing the work the
// dead studio's leftover spineless bastards at EA never had to.
//
// Per the doc-locked five-level Stupid-Backoff ladder:
//   P(targetType | sourceType, eventName)        - most specific
//     -> P(targetType | sourceType)               - marginalize event
//     -> P(targetType | eventName)                - marginalize source
//     -> global cross-map prior                   - cold-start
//     -> hardcoded type-signature default         - last fucking resort
//
// Each level holds a histogram. Lookup walks down the ladder and
// returns the first level with N >= kSampleFloor counts. One freak
// observation in the most-specific table does NOT get to outvote a
// well-supported prior down the ladder - that is the whole reason
// the ladder exists.
//
// We use Stupid-Backoff (Brants et al. 2007) and not Kneser-Ney
// because the corpus is under a million wires across all nine LOTR:C
// maps. KN's smoothing math earns its keep at a billion tokens; here
// it would be turbocharged overengineering and waste of CPU. Eat it.
// =============================================================================

#ifndef VESPUCCI_CORPUS_CORPUSPRIORS_H_
#define VESPUCCI_CORPUS_CORPUSPRIORS_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

#include <unordered_map>

namespace Vespucci {
namespace Corpus {

// Hash key for (sourceType, eventName, targetType). Uses Core::Hash::CombineHash.
struct TripleKey {
    TypeId sourceType;
    u32    eventHash;
    TypeId targetType;
    bool operator==(const TripleKey& o) const {
        return sourceType == o.sourceType &&
               eventHash  == o.eventHash &&
               targetType == o.targetType;
    }
};
struct TripleKeyHash { usize operator()(const TripleKey& k) const; };

struct PairKey {
    TypeId a;     // sourceType OR eventName-hash placeholder
    TypeId b;     // targetType
    u32    extra; // 0 or eventHash
    bool operator==(const PairKey& o) const {
        return a == o.a && b == o.b && extra == o.extra;
    }
};
struct PairKeyHash { usize operator()(const PairKey& k) const; };

// Per-level histograms.
struct PriorTables {
    // Level 1: P(targetType | sourceType, eventName)
    std::unordered_map<TripleKey, u32, TripleKeyHash> srcEventToTarget;
    // Level 2: P(targetType | sourceType)
    std::unordered_map<PairKey,   u32, PairKeyHash>   srcToTarget;
    // Level 3: P(targetType | eventName)
    std::unordered_map<PairKey,   u32, PairKeyHash>   eventToTarget;
    // Marginal totals for normalization
    std::unordered_map<TripleKey, u32, TripleKeyHash> srcEventTotal;
    std::unordered_map<TypeId,    u32>                srcTotal;
    std::unordered_map<u32,       u32>                eventTotal;
};

class CorpusPriors {
public:
    CorpusPriors();

    // Reset to empty.
    void clear();

    // Bump a (src, event, tgt) observation. Called by the corpus builders
    // for every wire row in every level.
    void bumpObservation(TypeId src, const Core::StringRef& eventCanonical, TypeId tgt);

    // Stupid-Backoff probability with the five-level ladder. Returns a
    // log-probability (negative value, 0 = certain, more-negative =
    // less likely) so the ranker can sum these with other features.
    logprob_t logProb(TypeId src, const Core::StringRef& eventCanonical, TypeId tgt) const;

    // Diagnostic: which level of the ladder fired for the most recent
    // logProb call. Cached for the debug overlay.
    i32 lastLevelFired() const { return m_lastLevelFired; }

    // Stats.
    struct Stats {
        i32 tripleEntries;
        i32 srcPairEntries;
        i32 eventPairEntries;
        u64 totalObservations;
    };
    Stats stats() const;

    // Persistence — serialize to a binary format consumed by CorpusReader.
    // Returned blob is heap-owned; caller frees with std::free.
    void* serialize(usize& outBytes) const;

    // Load from a serialized blob. Returns true if the magic matches.
    bool  deserialize(const void* bytes, usize n);

    // Fold another PriorTables into this one (used by the merger to
    // combine per-map shards into a global corpus).
    void  mergeFrom(const PriorTables& other);

    // Read access for the merger.
    const PriorTables& tables() const { return m_t; }

private:
    PriorTables m_t;
    mutable i32 m_lastLevelFired;

    // Stupid-Backoff discount factor. Brants et al. used 0.4. Same here.
    static constexpr f32 kBackoffDiscount = 0.4f;

    // Sample floor — smallest count we trust at a level before backing off.
    static const u32 kSampleFloor = 5;
};

// Global accessor — wired by Vespucci::Init.
CorpusPriors* GlobalPriors();
void          SetGlobalPriors(CorpusPriors*);

// Disk persistence — implementations live in CorpusReader.cpp and
// CorpusWriter.cpp. Decl'd here so consumers don't have to extern-them
// at every call site.
bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path);

} // namespace Corpus
} // namespace Vespucci

#endif // VESPUCCI_CORPUS_CORPUSPRIORS_H_
