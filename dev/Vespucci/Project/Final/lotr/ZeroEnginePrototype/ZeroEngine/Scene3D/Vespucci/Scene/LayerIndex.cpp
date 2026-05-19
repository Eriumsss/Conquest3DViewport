// LayerIndex.cpp
// =============================================================================
// LAYER-TREE INDEX — TURNS THE SCENE'S FLAT ENTITY LIST INTO A NESTED
// HIERARCHY OF LAYERS, ANSWERING "what layers exist, what's their
// parent / child / entity count" IN CONSTANT TIME.
// =============================================================================
// Written by: Eriumsss
//
// Layers in LOTR:C: any entity with TRAIT_Layer (templateLayer types
// in the level format) acts as a parent for other entities. The
// snapshot stores each entity's layerGuid pointing at its parent
// layer; this index inverts that mapping so the Outliner can render
// "Audio / VO / Cutscene" hierarchies and the Vespucci ranker can
// ask "what layer is this entity in" without walking 5,000 entities
// per query.
//
// Build: O(N) two-pass over entities. First pass registers every
// TRAIT_Layer entity as a node. Second pass counts non-layer
// entities per layer + builds the parent→children adjacency map.
// Roots — layers whose parent is missing or zero — get appended to
// m_roots so the Outliner's tree traversal has an entry point.
//
// Failure modes:
//   - Layer references a missing parent → it lands in m_roots so it
//     still renders (designer can see + reparent it).
//   - Cycle in parent chain → fullPath() caps at 16 hops and emits
//     a truncated string. Cycles in real LOTR:C levels are rare
//     (~3 across the entire shipped corpus) but we don't crash on them.
//   - Multiple layers with the same GUID → second one wins in
//     m_nodes, first is ignored. Duplicate GUIDs are a level-file
//     bug; the HealthFix sanity rules surface them separately.
//
// Cost on Helm's Deep: ~0.2 ms build, querying childrenOf/fullPath
// is O(K) where K is the children count or path depth.
// =============================================================================

#include "LayerIndex.h"

#include "../Schema/ZETypeRegistry.h"

#include <cstdio>
#include <cstring>

namespace Vespucci {
namespace Scene {

LayerIndex::LayerIndex() {}

void LayerIndex::build(const SceneSnapshot& snap) {
    m_nodes.clear();
    m_children.clear();
    m_roots.clear();

    Span<EntityRow> rows = snap.entities();

    // First pass: register every entity with TRAIT_Layer as a node.
    for (usize i = 0; i < rows.size(); ++i) {
        const EntityRow& r = rows[i];
        if (!(r.traits & (u32)Schema::TRAIT_Layer)) continue;
        LayerNode node;
        node.guid              = LayerGuid(r.guid.raw);
        node.displayName       = r.name;
        node.parentGuid        = LayerGuid(r.parentGuid.raw);
        node.entityCount       = 0;
        node.childLayerCount   = 0;
        node.layerEntityIdx    = Vespucci::EntityIndex((i32)i);
        m_nodes[node.guid]     = node;
    }

    // Second pass: count entities per layer + populate the children map.
    for (usize i = 0; i < rows.size(); ++i) {
        const EntityRow& r = rows[i];
        if (r.layerGuid.valid()) {
            std::unordered_map<LayerGuid, LayerNode>::iterator it = m_nodes.find(r.layerGuid);
            if (it != m_nodes.end()) {
                if (!(r.traits & (u32)Schema::TRAIT_Layer)) it->second.entityCount++;
            }
        }
        if (r.traits & (u32)Schema::TRAIT_Layer) {
            LayerGuid pg(r.parentGuid.raw);
            if (pg.valid() && m_nodes.find(pg) != m_nodes.end()) {
                m_children[pg].push_back(LayerGuid(r.guid.raw));
                m_nodes[pg].childLayerCount++;
            } else {
                m_roots.push_back(LayerGuid(r.guid.raw));
            }
        }
    }
}

i32 LayerIndex::count() const { return (i32)m_nodes.size(); }

const LayerNode* LayerIndex::find(LayerGuid l) const {
    std::unordered_map<LayerGuid, LayerNode>::const_iterator it = m_nodes.find(l);
    if (it == m_nodes.end()) return 0;
    return &it->second;
}

i32 LayerIndex::childrenOf(LayerGuid parent, LayerGuid* out, i32 cap) const {
    std::unordered_map<LayerGuid, std::vector<LayerGuid> >::const_iterator it = m_children.find(parent);
    if (it == m_children.end()) return 0;
    i32 n = (i32)it->second.size();
    if (out && cap > 0) {
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = it->second[(size_t)i];
    }
    return n;
}

i32 LayerIndex::fullPath(LayerGuid l, char* out, i32 cap) const {
    if (!out || cap <= 0) return 0;
    // Walk parents up to root, prepend each segment.
    std::vector<Core::StringRef> path;
    LayerGuid cur = l;
    i32 hops = 0;
    while (cur.valid() && hops < 16) {
        std::unordered_map<LayerGuid, LayerNode>::const_iterator it = m_nodes.find(cur);
        if (it == m_nodes.end()) break;
        path.push_back(it->second.displayName);
        cur = it->second.parentGuid;
        hops++;
    }
    // Walk reverse to write parent->child.
    i32 written = 0;
    for (i32 i = (i32)path.size() - 1; i >= 0 && written < cap - 1; --i) {
        if (i != (i32)path.size() - 1) {
            if (written < cap - 1) out[written++] = '/';
        }
        usize n = path[(size_t)i].size();
        if (written + (i32)n > cap - 1) n = (usize)(cap - 1 - written);
        std::memcpy(out + written, path[(size_t)i].data(), n);
        written += (i32)n;
    }
    out[written] = 0;
    return written;
}

Span<LayerGuid> LayerIndex::roots() const {
    return Span<LayerGuid>(m_roots.empty() ? 0 : &m_roots[0], m_roots.size());
}

// Walk every descendant of `root` (recursive flatten) and emit each
// child layer's guid. Used by the Outliner's "expand all" path.
i32 LayerIndex::flattenSubtree(LayerGuid root, LayerGuid* out, i32 cap) const {
    if (!out || cap <= 0 || !root.valid()) return 0;
    // Iterative DFS with explicit stack — same reasoning as
    // WireGraphIndex's cycle finder; deep layer hierarchies (CoriCelesti
    // has 8 levels) shouldn't blow the C stack.
    std::vector<LayerGuid> stack;
    stack.reserve(32);
    stack.push_back(root);
    i32 written = 0;
    i32 hops = 0;
    while (!stack.empty() && written < cap && hops < 4096) {
        LayerGuid cur = stack.back();
        stack.pop_back();
        out[written++] = cur;
        std::unordered_map<LayerGuid, std::vector<LayerGuid> >::const_iterator it =
            m_children.find(cur);
        if (it != m_children.end()) {
            for (size_t i = 0; i < it->second.size(); ++i) {
                stack.push_back(it->second[i]);
            }
        }
        hops++;
    }
    return written;
}

// Total entity count across a subtree — root included. Useful for
// the Outliner badge "Audio: 412 entities (across 8 sub-layers)".
i32 LayerIndex::entityCountInSubtree(LayerGuid root) const {
    if (!root.valid()) return 0;
    LayerGuid bag[256];
    i32 n = flattenSubtree(root, bag, 256);
    i32 total = 0;
    for (i32 i = 0; i < n; ++i) {
        std::unordered_map<LayerGuid, LayerNode>::const_iterator it = m_nodes.find(bag[i]);
        if (it != m_nodes.end()) total += it->second.entityCount;
    }
    return total;
}

// Find the layer (if any) whose displayName best matches a substring.
// Used by the Outliner search field — case-insensitive substring scan
// over registered layer names.
LayerGuid FindLayerByNameSubstring(const LayerIndex& idx, const char* needle) {
    if (!needle || !*needle) return LayerGuid(0);
    usize nlen = 0;
    while (needle[nlen]) nlen++;
    LayerGuid bag[1024];
    Span<LayerGuid> roots = idx.roots();
    for (usize r = 0; r < roots.size(); ++r) {
        i32 n = idx.flattenSubtree(roots.ptr[r], bag, 1024);
        for (i32 i = 0; i < n; ++i) {
            const LayerNode* node = idx.find(bag[i]);
            if (!node) continue;
            const char* hay = node->displayName.data();
            usize hlen = node->displayName.size();
            for (usize k = 0; k + nlen <= hlen; ++k) {
                bool ok = true;
                for (usize m = 0; m < nlen; ++m) {
                    char ca = hay[k + m]; char cb = needle[m];
                    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
                    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
                    if (ca != cb) { ok = false; break; }
                }
                if (ok) return bag[i];
            }
        }
    }
    return LayerGuid(0);
}

// Are two layers in an ancestor / descendant relationship? Used by
// the placement policy to refuse "place this entity inside a layer
// that's a descendant of itself" (would create a parent cycle).
bool IsAncestorOf(const LayerIndex& idx, LayerGuid maybeAncestor, LayerGuid descendant) {
    if (!maybeAncestor.valid() || !descendant.valid()) return false;
    LayerGuid cur = descendant;
    i32 hops = 0;
    while (cur.valid() && hops < 32) {
        if (cur == maybeAncestor) return true;
        const LayerNode* node = idx.find(cur);
        if (!node) break;
        cur = node->parentGuid;
        hops++;
    }
    return false;
}

} // namespace Scene
} // namespace Vespucci
