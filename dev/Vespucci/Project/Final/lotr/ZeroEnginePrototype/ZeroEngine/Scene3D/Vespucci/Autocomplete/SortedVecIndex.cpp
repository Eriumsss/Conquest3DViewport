// SortedVecIndex.cpp
// =============================================================================
// SORTED VECTOR + BINARY SEARCH. THE BORING ONE THAT WINS ON SMALL N.
// =============================================================================
// Written by: Eriumsss
//
// Sort once on build, binary-search the prefix range on query. Memory
// is one std::string + one u32 per entry, no overhead. Query is O(log
// N + K) where K is hits returned. For LOTR:C corpus (~1000 entries
// for events + actions combined) this is the fastest peer.
// =============================================================================

#include "IPrefixIndex.h"

#include "../Core/Logging.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Vespucci {
namespace Autocomplete {

class SortedVecIndex : public IPrefixIndex {
public:
    SortedVecIndex() : m_sorted(false) {}

    Kind kind() const { return KIND_SortedVec; }
    const char* name() const { return "SortedVec"; }

    void clear() {
        m_data.clear();
        m_sorted = false;
    }

    void insertOrChokeOnDupes(const Core::StringRef& str, u32 usage) {
        Entry e;
        e.lowered.assign(str.data(), str.size());
        for (size_t i = 0; i < e.lowered.size(); ++i) {
            char c = e.lowered[i];
            if (c >= 'A' && c <= 'Z') e.lowered[i] = (char)(c + 32);
        }
        e.original.assign(str.data(), str.size());
        e.usage = usage;
        m_data.push_back(e);
        m_sorted = false;
    }

    void buildFromBulk(const PrefixHit* items, i32 count) {
        clear();
        m_data.reserve((size_t)count);
        for (i32 i = 0; i < count; ++i) insertOrChokeOnDupes(items[i].name, items[i].usageCount);
        SortDataOrChokeOnIt();
    }

    i32 queryPrefix(const Core::StringRef& prefix, PrefixHit* outHits, i32 cap) const {
        if (m_data.empty()) return 0;
        EnsureSortedOrLog();
        std::string prefL;
        prefL.assign(prefix.data(), prefix.size());
        for (size_t i = 0; i < prefL.size(); ++i) {
            char c = prefL[i];
            if (c >= 'A' && c <= 'Z') prefL[i] = (char)(c + 32);
        }
        // Find lower bound on prefix.
        Entry probe; probe.lowered = prefL;
        std::vector<Entry>::const_iterator lo =
            std::lower_bound(m_data.begin(), m_data.end(), probe,
                [](const Entry& a, const Entry& b) { return a.lowered < b.lowered; });

        // Collect prefix-matching candidates.
        std::vector<const Entry*> candidates;
        for (std::vector<Entry>::const_iterator it = lo; it != m_data.end(); ++it) {
            if (it->lowered.size() < prefL.size()) break;
            if (std::memcmp(it->lowered.data(), prefL.data(), prefL.size()) != 0) break;
            candidates.push_back(&*it);
        }
        // Rank by usage desc.
        std::sort(candidates.begin(), candidates.end(),
            [](const Entry* a, const Entry* b) { return a->usage > b->usage; });
        i32 take = (i32)candidates.size();
        if (take > cap) take = cap;
        for (i32 i = 0; i < take; ++i) {
            outHits[i].name       = Core::StringRef(candidates[(size_t)i]->original.c_str(),
                                                     candidates[(size_t)i]->original.size());
            outHits[i].usageCount = candidates[(size_t)i]->usage;
        }
        return take;
    }

    i32 entryCount() const { return (i32)m_data.size(); }
    u64 memoryBytes() const {
        u64 total = 0;
        for (size_t i = 0; i < m_data.size(); ++i) {
            total += sizeof(Entry) + m_data[i].lowered.capacity() + m_data[i].original.capacity();
        }
        return total;
    }

private:
    struct Entry {
        std::string lowered;
        std::string original;
        u32         usage;
    };
    std::vector<Entry> m_data;
    mutable bool       m_sorted;

    void SortDataOrChokeOnIt() {
        std::sort(m_data.begin(), m_data.end(),
            [](const Entry& a, const Entry& b) { return a.lowered < b.lowered; });
        m_sorted = true;
    }
    void EnsureSortedOrLog() const {
        if (m_sorted) return;
        Core::Logging::Warn("SortedVecIndex: query on unsorted data, sorting now (caller forgot buildFromBulk)");
        const_cast<SortedVecIndex*>(this)->SortDataOrChokeOnIt();
    }
};

IPrefixIndex* MakeSortedVecIndex() { return new SortedVecIndex(); }

} // namespace Autocomplete
} // namespace Vespucci
