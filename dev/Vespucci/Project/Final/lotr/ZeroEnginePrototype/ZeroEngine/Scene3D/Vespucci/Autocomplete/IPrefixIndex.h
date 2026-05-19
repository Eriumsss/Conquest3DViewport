// IPrefixIndex.h
// =============================================================================
// PREFIX-INDEX INTERFACE - FOUR PEERS COMPETE FOR THE AUTOCOMPLETE THRONE
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked four-way bake-off: SortedVec (binary search), Trie (pointer
// nodes), MARISA (LOUDS-style succinct), Cedar (double-array trie).
// Same interface, different memory/build/query trade-offs. Factory
// picks one at runtime based on corpus size.
//
// All four answer the same question: "given a prefix string and a max
// hit count, return up to N strings that start with this prefix, plus
// their per-string usage counts."
// =============================================================================

#ifndef VESPUCCI_AUTOCOMPLETE_IPREFIXINDEX_H_
#define VESPUCCI_AUTOCOMPLETE_IPREFIXINDEX_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"

namespace Vespucci {
namespace Autocomplete {

struct PrefixHit {
    Core::StringRef name;
    u32             usageCount;
};

class IPrefixIndex {
public:
    enum Kind {
        KIND_SortedVec = 0,
        KIND_Trie      = 1,
        KIND_MARISA    = 2,
        KIND_Cedar     = 3,
        KIND_None      = 4
    };
    virtual ~IPrefixIndex() {}

    virtual Kind        kind() const = 0;
    virtual const char* name() const = 0;

    virtual void clear() = 0;
    virtual void insertOrChokeOnDupes(const Core::StringRef& str, u32 usage) = 0;

    // Build from a list of (string, usage) pairs in one shot. Implementations
    // may be much faster doing bulk-build vs N inserts.
    virtual void buildFromBulk(const PrefixHit* items, i32 count) = 0;

    // Query: top-K strings starting with `prefix` (case-insensitive ASCII),
    // ranked by usageCount descending. Returns count written.
    virtual i32 queryPrefix(const Core::StringRef& prefix,
                             PrefixHit* outHits, i32 cap) const = 0;

    virtual i32 entryCount() const = 0;
    virtual u64 memoryBytes() const = 0;
};

IPrefixIndex* MakePrefixIndexForCorpus(i32 entryCount, IPrefixIndex::Kind preferredOrNone);
void          DestroyPrefixIndex(IPrefixIndex* idx);

} // namespace Autocomplete
} // namespace Vespucci

#endif // VESPUCCI_AUTOCOMPLETE_IPREFIXINDEX_H_
