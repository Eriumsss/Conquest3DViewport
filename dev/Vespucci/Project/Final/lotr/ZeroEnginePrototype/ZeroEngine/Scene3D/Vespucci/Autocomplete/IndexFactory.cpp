// IndexFactory.cpp
// =============================================================================
// PICK A PREFIX INDEX BY CORPUS SIZE - SORTED-VEC FOR SMALL, MARISA
// FOR HUGE STATIC, CEDAR FOR LIVE-EDIT, TRIE AS THE BENCHMARK PEER
// =============================================================================
// Written by: Eriumsss

#include "IPrefixIndex.h"

#include "../Core/Logging.h"

namespace Vespucci {
namespace Autocomplete {

IPrefixIndex* MakeSortedVecIndex();
IPrefixIndex* MakeTrieIndex();
IPrefixIndex* MakeMarisaIndex();
IPrefixIndex* MakeCedarIndex();

IPrefixIndex* MakePrefixIndexForCorpus(i32 entryCount, IPrefixIndex::Kind preferred) {
    IPrefixIndex::Kind pick;
    if (preferred != IPrefixIndex::KIND_None) {
        pick = preferred;
    } else if (entryCount < 200) {
        pick = IPrefixIndex::KIND_SortedVec;
    } else if (entryCount < 5000) {
        pick = IPrefixIndex::KIND_Cedar;
    } else {
        pick = IPrefixIndex::KIND_MARISA;
    }

    IPrefixIndex* result = 0;
    switch (pick) {
        case IPrefixIndex::KIND_SortedVec: result = MakeSortedVecIndex(); break;
        case IPrefixIndex::KIND_Trie:      result = MakeTrieIndex();      break;
        case IPrefixIndex::KIND_MARISA:    result = MakeMarisaIndex();    break;
        case IPrefixIndex::KIND_Cedar:     result = MakeCedarIndex();     break;
        default:                            result = MakeSortedVecIndex(); break;
    }
    Core::Logging::Info("PrefixIndexFactory: picked %s for entryCount=%d (preferred=%d)",
        result->name(), entryCount, (int)preferred);
    return result;
}

void DestroyPrefixIndex(IPrefixIndex* idx) { delete idx; }

} // namespace Autocomplete
} // namespace Vespucci
