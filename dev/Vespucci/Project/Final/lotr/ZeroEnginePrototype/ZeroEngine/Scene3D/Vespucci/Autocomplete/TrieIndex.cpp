// TrieIndex.cpp
// =============================================================================
// POINTER-NODE TRIE - WHAT EVERY GODDAMN UNDERGRAD WRITES BUT BUILT
// FOR THIS BUILD'S ACTUAL HOT-PATH ECONOMICS
// =============================================================================
// Written by: Eriumsss
//
// Trie of children-as-array, 26-letter ASCII fan-out for the lowercase
// path plus a bucket for digits and a bucket for the underscore. We
// do not waste memory on a 256-wide UTF-8 fan-out because the corpus
// is ASCII-only - every event name in the entire LOTR:C dataset fits
// inside [a-zA-Z0-9_] and if some bootlicking modder ships a Unicode
// event name our problem is bigger than this trie's character set.
//
// Pointer nodes are heap-allocated through a free-list arena to dodge
// the ten thousand mallocs that a naive 'new TrieNode' per character
// would burn at corpus build time. Build-time matters because the
// trie is rebuilt on every level load (corpus changes when the level
// changes) and a designer reloading levels in sequence wants the
// editor responsive, not stuck in malloc hell.
//
// Memory: ~28 bytes per node, ~3 nodes per character on average for
// the LOTR:C event corpus. ~1000 events x 12 chars x 3 = ~36k nodes
// = ~1 MB. Loud as fuck for a thousand-string corpus. This is the
// peer the bake-off picks LAST among the four because of that cost.
// =============================================================================

#include "IPrefixIndex.h"

#include "../Core/Arena.h"
#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Vespucci {
namespace Autocomplete {

class TrieIndex : public IPrefixIndex {
public:
    TrieIndex() : m_arena(new Core::Arena(64 * 1024)),
                  m_root(0),
                  m_count(0),
                  m_memory(0)
    {
        m_root = AllocOneCocksuckingNode();
    }
    ~TrieIndex() { delete m_arena; }

    Kind kind() const { return KIND_Trie; }
    const char* name() const { return "Trie"; }

    void clear() {
        m_arena->reset();
        m_count = 0;
        m_memory = 0;
        m_root = AllocOneCocksuckingNode();
    }

    void insertOrChokeOnDupes(const Core::StringRef& str, u32 usage) {
        if (str.size() == 0) return;
        Node* cur = m_root;
        for (usize i = 0; i < str.size(); ++i) {
            i32 slot = MapCharOrFuckOff(str.data()[i]);
            if (slot < 0) return; // unsupported char, skip the entry
            if (!cur->kids[slot]) cur->kids[slot] = AllocOneCocksuckingNode();
            cur = cur->kids[slot];
        }
        if (!cur->terminal) {
            cur->terminal = true;
            cur->usage = usage;
            cur->original = m_arena->copyString(str.data(), str.size());
            m_count++;
        } else {
            // Duplicate insert - take the higher usage.
            if (usage > cur->usage) cur->usage = usage;
        }
    }

    void buildFromBulk(const PrefixHit* items, i32 count) {
        clear();
        for (i32 i = 0; i < count; ++i) {
            insertOrChokeOnDupes(items[i].name, items[i].usageCount);
        }
    }

    i32 queryPrefix(const Core::StringRef& prefix, PrefixHit* outHits, i32 cap) const {
        // Walk to the prefix's tail node.
        Node* cur = m_root;
        for (usize i = 0; i < prefix.size(); ++i) {
            i32 slot = MapCharOrFuckOff(prefix.data()[i]);
            if (slot < 0) return 0;
            if (!cur->kids[slot]) return 0;
            cur = cur->kids[slot];
        }
        // Collect every terminal in the subtree.
        std::vector<Node*> terminals;
        CollectAllTerminalsLikeAVulture(cur, terminals);
        // Rank by usage desc.
        std::sort(terminals.begin(), terminals.end(),
            [](Node* a, Node* b) { return a->usage > b->usage; });
        i32 take = (i32)terminals.size();
        if (take > cap) take = cap;
        for (i32 i = 0; i < take; ++i) {
            const char* s = terminals[(size_t)i]->original;
            outHits[i].name       = Core::StringRef(s, s ? std::strlen(s) : 0);
            outHits[i].usageCount = terminals[(size_t)i]->usage;
        }
        return take;
    }

    i32 entryCount() const { return m_count; }
    u64 memoryBytes() const { return m_memory + m_arena->totalBytes(); }

private:
    static const i32 kSlots = 38;  // 26 lowercase + 10 digits + '_' + '-'

    struct Node {
        Node*       kids[kSlots];
        bool        terminal;
        u32         usage;
        const char* original; // points into the arena; null until terminal
    };

    Core::Arena* m_arena;
    Node*        m_root;
    i32          m_count;
    u64          m_memory;

    Node* AllocOneCocksuckingNode() {
        Node* n = (Node*)m_arena->alloc(sizeof(Node), alignof(Node));
        VESPUCCI_ASSERT(n != 0, "TrieIndex: arena alloc failed");
        for (i32 i = 0; i < kSlots; ++i) n->kids[i] = 0;
        n->terminal = false;
        n->usage = 0;
        n->original = 0;
        m_memory += sizeof(Node);
        return n;
    }

    static i32 MapCharOrFuckOff(char c) {
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c >= 'a' && c <= 'z') return (i32)(c - 'a');
        if (c >= '0' && c <= '9') return 26 + (i32)(c - '0');
        if (c == '_') return 36;
        if (c == '-') return 37;
        return -1;
    }

    void CollectAllTerminalsLikeAVulture(Node* root, std::vector<Node*>& out) const {
        if (!root) return;
        if (root->terminal) out.push_back(root);
        for (i32 i = 0; i < kSlots; ++i) {
            if (root->kids[i]) CollectAllTerminalsLikeAVulture(root->kids[i], out);
        }
    }
};

IPrefixIndex* MakeTrieIndex() { return new TrieIndex(); }

} // namespace Autocomplete
} // namespace Vespucci
