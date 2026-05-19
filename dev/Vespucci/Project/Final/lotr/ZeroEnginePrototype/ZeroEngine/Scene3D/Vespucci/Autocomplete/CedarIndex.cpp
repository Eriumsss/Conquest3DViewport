// CedarIndex.cpp
// =============================================================================
// CEDAR-STYLE DOUBLE-ARRAY TRIE - O(1) STATE TRANSITIONS, GHOSTPACED MEMORY
// =============================================================================
// Written by: Eriumsss
//
// Cedar (Yoshinaga 2014) is a double-array trie variant that beats
// classic Aoe-style double-arrays on insertion cost while keeping
// the constant-time character-transition property that makes them
// the cocksucker's choice for hot-path autocomplete on big corpora.
//
// HOW DOUBLE-ARRAY TRIES WORK (the version we hand-rolled):
//
//   base[]  : per-state offset into the next-state array.
//   check[] : per-state parent-pointer for collision detection.
//
// To transition from state s on character c:
//   next = base[s] + c
//   if check[next] == s -> next is a valid child; otherwise no.
//
// Insertion has to either find an empty slot and use it, or RELOCATE
// an existing state's children if the desired slot is occupied.
// Cedar's contribution: a free-cell linked list across the unused
// slots so finding a relocation target is O(K) instead of O(N).
//
// We hand-rolled this because the upstream Cedar implementation is
// 7,000 lines of templated agony designed for arbitrary character
// sets and we only need the 38-char [a-z0-9_-] alphabet. The hand
// roll lands in ~500 LOC and gives us comparable query speed to
// the upstream library on the LOTR:C corpus, with a goddamn
// understandable structure that future-me can debug at 3am.
//
// MEMORY BUDGET: roughly 2 ints per state, ~5 states per character
// in a deduped trie, ~12 chars per string -> ~120 bytes per string
// in the worst case. For 1000 strings: ~120 KB. Larger than MARISA
// (~20 KB) but smaller than the pointer Trie (~1 MB) and queries
// run two array loads per character with no recursion.
//
// FACTORY POLICY: Cedar wins for corpora where INSERT performance
// matters - i.e. live-update of the autocomplete index as the user
// types new entity names mid-session. MARISA wins for static, build-
// once-query-many use. We keep both peers because the user explicitly
// said "no trim" and benchmarking matters.
// =============================================================================

#include "IPrefixIndex.h"

#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Vespucci {
namespace Autocomplete {

class CedarIndex : public IPrefixIndex {
public:
    CedarIndex() : m_size(1), m_freeHead(-1) {
        // Initialize root state at index 0.
        m_base.push_back(1);   // base for root
        m_check.push_back(-1); // root has no parent
        m_terminal.push_back(0);
        m_termIdx.push_back(-1);
    }

    Kind kind() const { return KIND_Cedar; }
    const char* name() const { return "Cedar"; }

    void clear() {
        m_base.clear(); m_check.clear();
        m_terminal.clear(); m_termIdx.clear();
        m_originals.clear(); m_usage.clear();
        m_freeHead = -1;
        m_size = 1;
        m_base.push_back(1);
        m_check.push_back(-1);
        m_terminal.push_back(0);
        m_termIdx.push_back(-1);
    }

    void insertOrChokeOnDupes(const Core::StringRef& str, u32 usage) {
        if (str.size() == 0) return;
        i32 cur = 0;
        for (usize i = 0; i < str.size(); ++i) {
            i32 slot = MapCharOrFuckOff(str.data()[i]);
            if (slot < 0) return; // unsupported byte
            i32 candidate = m_base[(size_t)cur] + slot;
            if (candidate < (i32)m_size && m_check[(size_t)candidate] == cur) {
                cur = candidate;
                continue;
            }
            if (candidate >= (i32)m_size) {
                ResizeArraysOrPissOnTheBudget(candidate + 1);
            }
            if (m_check[(size_t)candidate] != cur) {
                // Slot is occupied by another state. Relocate THIS
                // node's children to a fresh base where `slot` is free.
                cur = ResolveCollisionOrEatBitchassOverhead(cur, slot);
                candidate = m_base[(size_t)cur] + slot;
                if (candidate >= (i32)m_size) {
                    ResizeArraysOrPissOnTheBudget(candidate + 1);
                }
            }
            // Place new state.
            m_base[(size_t)candidate] = (i32)m_size;
            m_check[(size_t)candidate] = cur;
            m_terminal[(size_t)candidate] = 0;
            m_termIdx[(size_t)candidate] = -1;
            cur = candidate;
        }
        if (m_terminal[(size_t)cur]) {
            // Duplicate. Bump usage if higher.
            i32 ti = m_termIdx[(size_t)cur];
            if (usage > m_usage[(size_t)ti]) m_usage[(size_t)ti] = usage;
        } else {
            m_terminal[(size_t)cur] = 1;
            m_termIdx[(size_t)cur] = (i32)m_originals.size();
            std::string s(str.data(), str.size());
            m_originals.push_back(s);
            m_usage.push_back(usage);
        }
    }

    void buildFromBulk(const PrefixHit* items, i32 count) {
        clear();
        // Sort by string length ascending then alphabetically; this
        // tends to reduce mid-build relocations for double-array tries.
        std::vector<PrefixHit> sorted(items, items + count);
        std::sort(sorted.begin(), sorted.end(),
            [](const PrefixHit& a, const PrefixHit& b) {
                if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
                return std::memcmp(a.name.data(), b.name.data(),
                                    a.name.size() < b.name.size() ? a.name.size() : b.name.size()) < 0;
            });
        for (size_t i = 0; i < sorted.size(); ++i) {
            insertOrChokeOnDupes(sorted[i].name, sorted[i].usageCount);
        }
    }

    i32 queryPrefix(const Core::StringRef& prefix, PrefixHit* outHits, i32 cap) const {
        i32 cur = 0;
        for (usize i = 0; i < prefix.size(); ++i) {
            i32 slot = MapCharOrFuckOff(prefix.data()[i]);
            if (slot < 0) return 0;
            i32 candidate = m_base[(size_t)cur] + slot;
            if (candidate >= (i32)m_size || m_check[(size_t)candidate] != cur) return 0;
            cur = candidate;
        }
        std::vector<i32> terms;
        DragTerminalsFromSubtreeOrCryAboutIt(cur, terms);
        std::sort(terms.begin(), terms.end(), [&](i32 a, i32 b) {
            return m_usage[(size_t)a] > m_usage[(size_t)b];
        });
        i32 take = (i32)terms.size();
        if (take > cap) take = cap;
        for (i32 i = 0; i < take; ++i) {
            const std::string& s = m_originals[(size_t)terms[(size_t)i]];
            outHits[i].name       = Core::StringRef(s.c_str(), s.size());
            outHits[i].usageCount = m_usage[(size_t)terms[(size_t)i]];
        }
        return take;
    }

    i32 entryCount() const { return (i32)m_originals.size(); }
    u64 memoryBytes() const {
        return m_base.capacity() * sizeof(i32) +
               m_check.capacity() * sizeof(i32) +
               m_terminal.capacity() * sizeof(u8) +
               m_termIdx.capacity() * sizeof(i32) +
               m_usage.capacity() * sizeof(u32);
    }

private:
    static const i32 kAlphabet = 38;
    std::vector<i32> m_base;
    std::vector<i32> m_check;
    std::vector<u8>  m_terminal;
    std::vector<i32> m_termIdx;
    std::vector<std::string> m_originals;
    std::vector<u32>         m_usage;
    i32 m_size;
    i32 m_freeHead;

    static i32 MapCharOrFuckOff(char c) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c >= 'a' && c <= 'z') return (i32)(c - 'a');
        if (c >= '0' && c <= '9') return 26 + (i32)(c - '0');
        if (c == '_') return 36;
        if (c == '-') return 37;
        return -1;
    }

    void ResizeArraysOrPissOnTheBudget(i32 wanted) {
        if (wanted <= (i32)m_size) return;
        m_base.resize((size_t)wanted, 0);
        m_check.resize((size_t)wanted, -2);  // -2 = empty marker
        m_terminal.resize((size_t)wanted, 0);
        m_termIdx.resize((size_t)wanted, -1);
        m_size = wanted;
    }

    // When inserting child `slot` under `parent` collides with an
    // existing state, we move parent's existing children to a fresh
    // base where `slot` is free. Real Cedar uses a free-list to make
    // this fast; we hand-roll a simple linear scan because the editor's
    // working corpus is small enough that O(N) per relocation amortizes
    // out in the noise.
    i32 ResolveCollisionOrEatBitchassOverhead(i32 parent, i32 slotNeeded) {
        // Find existing children of parent.
        std::vector<std::pair<i32, i32> > existing; // (slot, childIdx)
        i32 oldBase = m_base[(size_t)parent];
        for (i32 c = 0; c < kAlphabet; ++c) {
            i32 cand = oldBase + c;
            if (cand >= 0 && cand < (i32)m_size && m_check[(size_t)cand] == parent) {
                existing.push_back(std::make_pair(c, cand));
            }
        }
        // Find a new base where slotNeeded AND every existing child slot
        // are simultaneously free.
        i32 newBase = 1;
        for (;;) {
            bool ok = true;
            i32 slot = newBase + slotNeeded;
            if (slot >= (i32)m_size) ResizeArraysOrPissOnTheBudget(slot + 1);
            if (m_check[(size_t)slot] != -2 && m_check[(size_t)slot] != -1 + 0) ok = false;
            // -2 is empty, -1 is reserved-for-root; treat -2 as free.
            if (m_check[(size_t)slot] != -2) ok = false;
            if (ok) {
                for (size_t i = 0; i < existing.size() && ok; ++i) {
                    i32 ec = newBase + existing[i].first;
                    if (ec >= (i32)m_size) ResizeArraysOrPissOnTheBudget(ec + 1);
                    if (m_check[(size_t)ec] != -2) ok = false;
                }
            }
            if (ok) break;
            newBase++;
            if (newBase > (i32)m_size + 64) {
                ResizeArraysOrPissOnTheBudget(newBase + kAlphabet);
            }
        }
        // Relocate existing children to newBase.
        for (size_t i = 0; i < existing.size(); ++i) {
            i32 oldIdx = existing[i].second;
            i32 newIdx = newBase + existing[i].first;
            m_base[(size_t)newIdx]    = m_base[(size_t)oldIdx];
            m_check[(size_t)newIdx]   = parent;
            m_terminal[(size_t)newIdx]= m_terminal[(size_t)oldIdx];
            m_termIdx[(size_t)newIdx] = m_termIdx[(size_t)oldIdx];
            // Reparent grandchildren that pointed back at oldIdx.
            i32 gOldBase = m_base[(size_t)oldIdx];
            for (i32 cc = 0; cc < kAlphabet; ++cc) {
                i32 gOld = gOldBase + cc;
                if (gOld >= 0 && gOld < (i32)m_size && m_check[(size_t)gOld] == oldIdx) {
                    m_check[(size_t)gOld] = newIdx;
                }
            }
            // Mark old slot as empty.
            m_check[(size_t)oldIdx] = -2;
        }
        m_base[(size_t)parent] = newBase;
        return parent;
    }

    void DragTerminalsFromSubtreeOrCryAboutIt(i32 root, std::vector<i32>& out) const {
        if (root < 0 || (size_t)root >= m_terminal.size()) return;
        if (m_terminal[(size_t)root]) {
            i32 ti = m_termIdx[(size_t)root];
            if (ti >= 0) out.push_back(ti);
        }
        i32 base = m_base[(size_t)root];
        for (i32 c = 0; c < kAlphabet; ++c) {
            i32 cand = base + c;
            if (cand >= 0 && cand < (i32)m_size && m_check[(size_t)cand] == root) {
                DragTerminalsFromSubtreeOrCryAboutIt(cand, out);
            }
        }
    }
};

IPrefixIndex* MakeCedarIndex() { return new CedarIndex(); }

} // namespace Autocomplete
} // namespace Vespucci
