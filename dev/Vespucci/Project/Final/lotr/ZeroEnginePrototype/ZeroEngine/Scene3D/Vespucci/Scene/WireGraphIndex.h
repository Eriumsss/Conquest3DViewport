// WireGraphIndex.h
// =============================================================================
// WIRE GRAPH INDEX — CSR-STYLE ADJACENCY OFF THE SNAPSHOT
// =============================================================================
// Written by: Eriumsss
//
// SceneSnapshot's WireRow array is dense and frame-stable. This index
// turns it into per-entity outgoing/incoming adjacency lists so the
// candidate generator can answer "what does CapturePoint_03 fire into?"
// and "what fires into ScoreManager?" in constant time per query.
//
// CSR (compressed-sparse-row) format: one offset+count per entity,
// one flat list of WireIndex values. Memory is N + W; build is O(W).
// Read is O(1) to find the row, O(K) to walk the K wires there.
// =============================================================================

#ifndef VESPUCCI_SCENE_WIREGRAPHINDEX_H_
#define VESPUCCI_SCENE_WIREGRAPHINDEX_H_

#include "../Core/VespucciTypes.h"
#include "SceneSnapshot.h"

#include <vector>

namespace Vespucci {
namespace Scene {

class WireGraphIndex {
public:
    WireGraphIndex();

    void build(const SceneSnapshot& snap);

    // Outgoing wires from `e`. Returns count; outIndices fills up to cap.
    i32 outgoing(Vespucci::EntityIndex e, WireIndex* outIndices, i32 cap) const;

    // Incoming wires to `e`. Same shape.
    i32 incoming(Vespucci::EntityIndex e, WireIndex* outIndices, i32 cap) const;

    // In-degree / out-degree counts for the overload penalty.
    i32 inDegree(Vespucci::EntityIndex e)  const;
    i32 outDegree(Vespucci::EntityIndex e) const;

    // Walk every output entity's pair-counts. Used by the corpus.
    Span<WireRow> wires() const;

    // Stats — populated by build(); read by the Brain debug overlay.
    struct Stats {
        i32 entityCount;
        i32 wireCount;
        i32 maxOutDegree;
        i32 maxInDegree;
        i32 zeroDegreeEntities;
    };
    Stats stats() const;

    // Top-K entities by total degree (in + out). Returns count written.
    i32 topByDegree(Vespucci::EntityIndex* outEntities,
                     i32* outDegrees, i32 cap) const;

    // Visitor signature for iterateWires. Returns true to keep going.
    typedef bool (*WireVisitor)(i32 wireIdx, const WireRow& w, void* userData);
    i32 iterateWires(WireVisitor visitor, void* userData) const;

    // Detect cycles in the wire graph. Writes the entity index that
    // closed each detected cycle into outFirstEntityOfCycle. Used by
    // HealthFix's cycle-detection sub-pass.
    i32 findCyclesUpTo(i32* outFirstEntityOfCycle, i32 cap) const;

private:
    // Outgoing CSR
    std::vector<i32>       m_outOffset;
    std::vector<i32>       m_outCount;
    std::vector<WireIndex> m_outFlat;

    // Incoming CSR
    std::vector<i32>       m_inOffset;
    std::vector<i32>       m_inCount;
    std::vector<WireIndex> m_inFlat;

    Span<WireRow>          m_wires;
};

} // namespace Scene
} // namespace Vespucci

#endif // VESPUCCI_SCENE_WIREGRAPHINDEX_H_
