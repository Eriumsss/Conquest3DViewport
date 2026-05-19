// LayerIndex.h
// =============================================================================
// LAYER INDEX — TREE OF TEMPLATELAYER ENTITIES + ROLLED-UP STATS
// =============================================================================
// Written by: Eriumsss
//
// Layers in LOTR:C are themselves entities (TRAIT_Layer). They form a
// tree: templateLevel -> templateLayer -> templateGroup -> ...
// The ranker uses layer membership as a feature, the autocomplete
// shows layer paths in the wizard, and the suggested-layer placement
// policy walks this tree to find the most-common layer per type.
// =============================================================================

#ifndef VESPUCCI_SCENE_LAYERINDEX_H_
#define VESPUCCI_SCENE_LAYERINDEX_H_

#include "../Core/VespucciTypes.h"
#include "SceneSnapshot.h"

#include <unordered_map>
#include <vector>

namespace Vespucci {
namespace Scene {

struct LayerNode {
    LayerGuid       guid;
    Core::StringRef displayName;
    LayerGuid       parentGuid;     // 0 = root
    i32             entityCount;     // how many non-layer entities live in this layer
    i32             childLayerCount; // sub-layers
    Vespucci::EntityIndex layerEntityIdx; // index into snapshot for the layer itself, or invalid
};

class LayerIndex {
public:
    LayerIndex();

    void build(const SceneSnapshot& snap);

    i32                count() const;
    const LayerNode*   find(LayerGuid l) const;

    // Walk children of a given layer.
    i32 childrenOf(LayerGuid parent, LayerGuid* outGuids, i32 cap) const;

    // Resolve full layer path "Art / Structures / HelmsDeep_Walls" for
    // the UI. Returns characters written (not including null).
    i32 fullPath(LayerGuid l, char* out, i32 cap) const;

    // Roots (layers with parentGuid == 0).
    Span<LayerGuid>    roots() const;

    // Flatten a subtree into a flat list of layer GUIDs (DFS, root
    // included). Used by Outliner expand-all + the search path.
    i32 flattenSubtree(LayerGuid root, LayerGuid* out, i32 cap) const;

    // Total entity count across a subtree (rolls up children).
    i32 entityCountInSubtree(LayerGuid root) const;

private:
    std::unordered_map<LayerGuid, LayerNode>          m_nodes;
    std::unordered_map<LayerGuid, std::vector<LayerGuid> > m_children;
    std::vector<LayerGuid>                             m_roots;
};

} // namespace Scene
} // namespace Vespucci

#endif // VESPUCCI_SCENE_LAYERINDEX_H_
