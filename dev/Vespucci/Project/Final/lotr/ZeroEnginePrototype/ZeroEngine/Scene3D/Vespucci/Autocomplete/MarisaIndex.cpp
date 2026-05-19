// MarisaIndex.cpp
// =============================================================================
// MARISA-STYLE LOUDS TRIE - SUCCINCT, CACHE-FRIENDLY, MIDDLE FINGER TO MEMORY
// =============================================================================
// Written by: Eriumsss
//
// MARISA (Matching Algorithm with Recursively Implemented StorAge) is
// the bitchass beautiful idea Yata et al. published in 2007: build a
// trie, encode its shape with LOUDS (Level-Order Unary Degree
// Sequence) bits so the parent/child topology takes 2N+1 bits instead
// of N pointers, and recursively store the EDGE LABELS as another
// trie because the same edge-label byte sequences repeat all over
// the place in any human-meaningful corpus.
//
// We do NOT pull the upstream MARISA library because it is a 30k LOC
// behemoth designed for indexing the entire goddamn web. Our corpus
// is one thousand event names. We hand-roll the LOUDS bitmap + a
// flat label array + a simple rank/select scheme, which is plenty
// for the editor's working scale and avoids dragging in a vendor
// dependency we would have to re-license.
//
// LOUDS BITMAP encoding (super-root sentinel + level-order traversal):
//   1     - super-root has 1 child
//   1...0 - root: K ones for K children, terminating zero
//   1...0 - first child of root: M ones for M children, terminating zero
//   ...   - and so on, level by level
//
// To find child #k of node N: rank the position of N in the LOUDS,
// hop to the (rank * 2)-ish position, walk k ones forward. We use a
// 256-bit-block popcount sample to make rank() O(1) average.
//
// MEMORY EXPECTATION: for a corpus of N strings averaging L characters
// each:
//   - LOUDS bits    : ~2 * N * L bits  -> ~N*L/4 bytes
//   - label bytes   : ~ N*L           bytes (deduped via the recursive
//                                            sub-trie; we approximate
//                                            with one flat byte per
//                                            edge in this hand-roll)
//   - usage payload : sizeof(u32) per terminal node
//
// For LOTR:C (~1000 strings, ~12 chars avg): ~3 KB LOUDS + ~12 KB
// labels + ~4 KB usage = under 20 KB total. The Trie peer's pointer
// nodes ate ~1 MB for the same input. THIS is the win.
//
// Trade-off: build is heavier (sort + LOUDS encode + rank precompute)
// and queries that walk many children have to do bit-rank hops. For
// our corpus size both are well under the autocomplete latency
// budget. The factory picks MARISA for corpora > 500 entries.
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

class MarisaIndex : public IPrefixIndex {
public:
    MarisaIndex() : m_built(false), m_terminalCount(0) {}

    Kind kind() const { return KIND_MARISA; }
    const char* name() const { return "MARISA"; }

    void clear() {
        m_pending.clear();
        m_louds.clear();
        m_labels.clear();
        m_terminals.clear();
        m_usage.clear();
        m_originals.clear();
        m_terminalIndexAtNode.clear();
        m_rankCache.clear();
        m_built = false;
        m_terminalCount = 0;
    }

    void insertOrChokeOnDupes(const Core::StringRef& str, u32 usage) {
        if (str.size() == 0) return;
        Pending p; p.s.assign(str.data(), str.size());
        p.usage = usage;
        m_pending.push_back(p);
        m_built = false;
    }

    void buildFromBulk(const PrefixHit* items, i32 count) {
        clear();
        m_pending.reserve((size_t)count);
        for (i32 i = 0; i < count; ++i) insertOrChokeOnDupes(items[i].name, items[i].usageCount);
        BuildLoudsOrChokeOnEmptyCorpus();
    }

    i32 queryPrefix(const Core::StringRef& prefix, PrefixHit* outHits, i32 cap) const {
        if (!m_built) const_cast<MarisaIndex*>(this)->BuildLoudsOrChokeOnEmptyCorpus();
        if (m_louds.empty()) return 0;

        // Walk the LOUDS structure following the prefix. Returns -1 if
        // the prefix walks off into nothing.
        i32 nodeIdx = WalkPrefixOrFuckOff(prefix);
        if (nodeIdx < 0) return 0;

        // Collect every terminal in the subtree rooted at nodeIdx.
        std::vector<i32> terms;
        DragEveryTerminalDownFromTheSubtree(nodeIdx, terms);

        // Rank by usage desc.
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

    i32 entryCount() const { return m_terminalCount; }
    u64 memoryBytes() const {
        return (m_louds.size() + 7) / 8 +
               m_labels.size() +
               m_usage.size() * sizeof(u32) +
               m_terminals.size() * sizeof(u8) +
               m_terminalIndexAtNode.size() * sizeof(i32) +
               m_rankCache.size() * sizeof(u32);
    }

private:
    struct Pending { std::string s; u32 usage; };
    std::vector<Pending> m_pending;

    // ── LOUDS-encoded structure ──────────────────────────────────────
    // m_louds: bit array. Each node contributes one block of K ones
    //   + a terminating zero, in level order.
    // m_labels: edge label bytes parallel to m_louds ones (ones-only
    //   positions). Length = number of nodes - 1 (root has no edge).
    // m_terminals: per-node terminal bit (1 = a string ends at this node).
    // m_terminalIndexAtNode: for nodes with terminal=1, the index into
    //   m_originals/m_usage. -1 if not a terminal.
    // m_rankCache: cumulative count of '1' bits at every kRankSampleStride.
    std::vector<u8>       m_louds;     // bit-packed; index by bit
    std::vector<u8>       m_labels;
    std::vector<u8>       m_terminals;
    std::vector<i32>      m_terminalIndexAtNode;
    std::vector<u32>      m_usage;
    std::vector<std::string> m_originals;
    std::vector<u32>      m_rankCache;

    bool m_built;
    i32  m_terminalCount;

    static const i32 kRankSampleStride = 256;

    inline bool LoudsBit(i32 idx) const {
        return ((m_louds[(size_t)(idx >> 3)] >> (idx & 7)) & 1u) != 0;
    }
    inline void SetLoudsBit(i32 idx, bool v) {
        if ((i32)m_louds.size() * 8 <= idx) {
            m_louds.resize((size_t)((idx >> 3) + 1), 0);
        }
        if (v) m_louds[(size_t)(idx >> 3)] |= (u8)(1u << (idx & 7));
        else   m_louds[(size_t)(idx >> 3)] &= (u8)~(1u << (idx & 7));
    }

    // Build phase. Sort the input strings, build a temporary pointer
    // trie, then linearize it level-order into the LOUDS encoding.
    void BuildLoudsOrChokeOnEmptyCorpus() {
        if (m_pending.empty()) {
            m_built = true;
            return;
        }
        // Step 1: sort lexicographically (lowercase) for deterministic build.
        std::sort(m_pending.begin(), m_pending.end(),
            [](const Pending& a, const Pending& b) {
                return CompareLowerLikeABastard(a.s, b.s);
            });

        // Step 2: build a temporary pointer trie using vectors so we
        // can free everything at end of build cheaply.
        struct TempNode {
            std::vector<std::pair<u8, i32> > kids; // (label, childIdx)
            bool                              terminal;
            i32                               termIdx; // -1 if not terminal
        };
        std::vector<TempNode> temp;
        temp.reserve(m_pending.size() * 8);
        TempNode root;
        root.terminal = false; root.termIdx = -1;
        temp.push_back(root);

        m_originals.reserve(m_pending.size());
        m_usage.reserve(m_pending.size());

        for (size_t i = 0; i < m_pending.size(); ++i) {
            const Pending& p = m_pending[i];
            i32 cur = 0;
            for (size_t c = 0; c < p.s.size(); ++c) {
                u8 lc = (u8)p.s[c];
                if (lc >= 'A' && lc <= 'Z') lc = (u8)(lc + 32);
                bool found = false;
                for (size_t k = 0; k < temp[(size_t)cur].kids.size(); ++k) {
                    if (temp[(size_t)cur].kids[k].first == lc) {
                        cur = temp[(size_t)cur].kids[k].second;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    TempNode kid;
                    kid.terminal = false; kid.termIdx = -1;
                    i32 kidIdx = (i32)temp.size();
                    temp.push_back(kid);
                    temp[(size_t)cur].kids.push_back(std::make_pair(lc, kidIdx));
                    cur = kidIdx;
                }
            }
            if (!temp[(size_t)cur].terminal) {
                temp[(size_t)cur].terminal = true;
                temp[(size_t)cur].termIdx  = (i32)m_originals.size();
                m_originals.push_back(p.s);
                m_usage.push_back(p.usage);
                m_terminalCount++;
            } else {
                // Duplicate. Bump usage.
                i32 ti = temp[(size_t)cur].termIdx;
                if (p.usage > m_usage[(size_t)ti]) m_usage[(size_t)ti] = p.usage;
            }
        }

        // Step 3: linearize level-order into LOUDS.
        std::vector<i32> levelQueue;
        levelQueue.push_back(0);
        m_louds.clear();
        m_labels.clear();
        m_terminals.assign(temp.size(), 0);
        m_terminalIndexAtNode.assign(temp.size(), -1);

        // LOUDS prefix: super-root dummy. One '1' for the real root,
        // then a terminating '0'.
        i32 bitPos = 0;
        SetLoudsBit(bitPos++, true);
        SetLoudsBit(bitPos++, false);

        // Process nodes level-by-level. Each node contributes K ones
        // (one per child) + one terminating zero. Sort children by
        // label so traversal order is deterministic.
        size_t qHead = 0;
        while (qHead < levelQueue.size()) {
            i32 nodeIdx = levelQueue[qHead++];
            TempNode& nd = temp[(size_t)nodeIdx];
            std::sort(nd.kids.begin(), nd.kids.end(),
                [](const std::pair<u8, i32>& a, const std::pair<u8, i32>& b) {
                    return a.first < b.first;
                });
            for (size_t k = 0; k < nd.kids.size(); ++k) {
                SetLoudsBit(bitPos++, true);
                m_labels.push_back(nd.kids[k].first);
                levelQueue.push_back(nd.kids[k].second);
            }
            SetLoudsBit(bitPos++, false);

            // Mark terminal info, indexed by the node's level-order
            // position (which equals its index in levelQueue).
            i32 levelIdx = (i32)(qHead - 1);
            m_terminals[(size_t)levelIdx] = nd.terminal ? 1u : 0u;
            m_terminalIndexAtNode[(size_t)levelIdx] = nd.termIdx;
        }

        // Build rank cache for O(1) bit-rank hops.
        BuildRankCacheOrChokeOnEmptyLouds(bitPos);
        m_built = true;
        Core::Logging::Info("MarisaIndex: %d strings, %d nodes, LOUDS=%d bits, %llu memory bytes",
            (int)m_pending.size(), (int)levelQueue.size(), bitPos,
            (unsigned long long)memoryBytes());
    }

    void BuildRankCacheOrChokeOnEmptyLouds(i32 totalBits) {
        m_rankCache.clear();
        if (totalBits <= 0) return;
        i32 sampleCount = (totalBits + kRankSampleStride - 1) / kRankSampleStride + 1;
        m_rankCache.assign((size_t)sampleCount, 0);
        u32 running = 0;
        for (i32 i = 0; i < totalBits; ++i) {
            if ((i % kRankSampleStride) == 0) {
                m_rankCache[(size_t)(i / kRankSampleStride)] = running;
            }
            if (LoudsBit(i)) running++;
        }
        m_rankCache[(size_t)(totalBits / kRankSampleStride)] = running;
    }

    // rank1 = number of '1' bits in [0, idx). O(1) average via cache.
    i32 Rank1OrEatShit(i32 idx) const {
        if (idx <= 0) return 0;
        i32 sample = idx / kRankSampleStride;
        if ((size_t)sample >= m_rankCache.size()) sample = (i32)m_rankCache.size() - 1;
        u32 base = m_rankCache[(size_t)sample];
        i32 from = sample * kRankSampleStride;
        for (i32 i = from; i < idx; ++i) {
            if (LoudsBit(i)) base++;
        }
        return (i32)base;
    }

    // select1 = position of the (k-th) '1' bit (k starting at 1).
    // Slow path implementation - linear scan from the rank cache.
    i32 Select1OrChokeOnSparseBits(i32 k) const {
        if (k <= 0) return -1;
        // Find the sample block where the k-th 1 lives.
        i32 totalSamples = (i32)m_rankCache.size();
        i32 lo = 0, hi = totalSamples - 1, picked = 0;
        while (lo <= hi) {
            i32 mid = (lo + hi) >> 1;
            if ((i32)m_rankCache[(size_t)mid] < k) { picked = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        i32 from = picked * kRankSampleStride;
        u32 running = m_rankCache[(size_t)picked];
        i32 totalBits = (i32)(m_louds.size() * 8);
        for (i32 i = from; i < totalBits; ++i) {
            if (LoudsBit(i)) {
                running++;
                if ((i32)running == k) return i;
            }
        }
        return -1;
    }

    // Walk the LOUDS-encoded trie from the root following the prefix.
    // Returns the level-order index of the node where the prefix ends,
    // or -1 if any character does not match.
    i32 WalkPrefixOrFuckOff(const Core::StringRef& prefix) const {
        // Root is level-order index 1 (super-root is index 0).
        i32 nodeIdx = 1;
        for (usize i = 0; i < prefix.size(); ++i) {
            char c = prefix.data()[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);

            // Find children of nodeIdx. Children's bit positions are
            // contiguous starting at SelectFirstChildBit(nodeIdx).
            i32 childBitStart = SelectFirstChildBit(nodeIdx);
            if (childBitStart < 0) return -1;
            // The label for the (j-th) child is at m_labels[childBitStart-1+j]
            // because labels parallel ones-positions in LOUDS, and the
            // first label corresponds to bit-position 1 (after super-root).
            i32 labelStart = childBitStart - 1; // labels are 0-indexed against LOUDS ones
            // Walk forward until we find label == c or hit a 0 bit.
            bool matched = false;
            for (i32 j = 0; ; ++j) {
                i32 bp = childBitStart + j;
                if (bp >= (i32)(m_louds.size() * 8)) return -1;
                if (!LoudsBit(bp)) return -1;
                if (m_labels[(size_t)(labelStart + j)] == (u8)c) {
                    // Child node level-order idx = rank1(bp+1)
                    nodeIdx = Rank1OrEatShit(bp + 1);
                    matched = true;
                    break;
                }
            }
            if (!matched) return -1;
        }
        return nodeIdx;
    }

    // Locate the first child-bit of `node`. Equivalent to:
    //   select0(node) + 1   where select0 finds the (node-th) '0'.
    // We approximate via linear scan from a likely position - slow
    // path but correct.
    i32 SelectFirstChildBit(i32 nodeIdx) const {
        // Walk the bit array counting zero markers. The N-th node
        // (level-order) follows the N-th '0' marker.
        i32 zerosNeeded = nodeIdx;
        i32 zeros = 0;
        i32 totalBits = (i32)(m_louds.size() * 8);
        for (i32 i = 0; i < totalBits; ++i) {
            if (!LoudsBit(i)) {
                zeros++;
                if (zeros == zerosNeeded) return i + 1;
            }
        }
        return -1;
    }

    void DragEveryTerminalDownFromTheSubtree(i32 nodeIdx, std::vector<i32>& outTerms) const {
        if (nodeIdx < 0 || (size_t)nodeIdx >= m_terminals.size()) return;
        if (m_terminals[(size_t)nodeIdx]) {
            i32 ti = m_terminalIndexAtNode[(size_t)nodeIdx];
            if (ti >= 0) outTerms.push_back(ti);
        }
        // Walk children.
        i32 childBitStart = SelectFirstChildBit(nodeIdx);
        if (childBitStart < 0) return;
        for (i32 j = 0; ; ++j) {
            i32 bp = childBitStart + j;
            if (bp >= (i32)(m_louds.size() * 8)) break;
            if (!LoudsBit(bp)) break;
            i32 childNode = Rank1OrEatShit(bp + 1);
            DragEveryTerminalDownFromTheSubtree(childNode, outTerms);
        }
    }

    static bool CompareLowerLikeABastard(const std::string& a, const std::string& b) {
        size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; ++i) {
            char ca = a[i]; char cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    }
};

IPrefixIndex* MakeMarisaIndex() { return new MarisaIndex(); }

} // namespace Autocomplete
} // namespace Vespucci
