// WireGraphIndex.cpp
// =============================================================================
// CSR-FORMAT WIRE GRAPH — INBOUND + OUTBOUND PER ENTITY IN O(1) LOOKUP
// =============================================================================
// Written by: Eriumsss
//
// Why CSR (Compressed Sparse Row): the wire graph for a typical
// LOTR:C level is dense in some entities (a single ScriptRelay can
// have hundreds of inbound wires) and sparse in others (most props
// have zero). A naive nested-loop "find all wires touching this
// entity" is O(W) per query and W is in the thousands; we hit
// thousands of those queries per editor frame. CSR moves the cost
// to a one-time per-snapshot build (O(W)) so every subsequent query
// is a constant-time array slice.
//
// Memory layout — two parallel CSR tables, one for outbound and one
// for inbound:
//   m_outCount[entityIdx]   — number of outbound wires
//   m_outOffset[entityIdx]  — start offset into m_outFlat
//   m_outFlat[]              — packed sequence of WireIndex values
//                                grouped by source entity
//   (same shape for m_in*)
//
// Build is two passes per direction: count, then prefix-sum into
// offsets, then fill via a per-row cursor. Standard textbook CSR.
//
// Failure modes:
//   - Snapshot empty   → all CSR arrays zero-sized; queries return 0
//                        cleanly without crashing.
//   - Wire targets a deleted entity (broken ref) — wire is skipped
//     in the inbound pass, so broken wires don't pollute the
//     incoming() iteration. They're still findable via the wires()
//     accessor for the broken-ref scanner.
//
// Cost on Helm's Deep (5,885 entities, ~6,500 wires): ~0.4 ms build,
// well under the 2 ms snapshot budget.
// =============================================================================

#include "WireGraphIndex.h"

namespace Vespucci {
namespace Scene {

WireGraphIndex::WireGraphIndex() : m_wires(0, 0) {}

void WireGraphIndex::build(const SceneSnapshot& snap) {
    i32 N = snap.entityCount();
    Span<WireRow> wires = snap.wires();
    m_wires = wires;

    // Count per-source (owner) outgoing.
    m_outCount.assign((size_t)N, 0);
    for (usize i = 0; i < wires.size(); ++i) {
        if (wires[i].ownerIdx.valid()) m_outCount[(size_t)wires[i].ownerIdx.raw]++;
    }
    // Prefix-sum into offsets.
    m_outOffset.assign((size_t)N, 0);
    i32 acc = 0;
    for (i32 i = 0; i < N; ++i) {
        m_outOffset[(size_t)i] = acc;
        acc += m_outCount[(size_t)i];
    }
    m_outFlat.assign((size_t)acc, WireIndex());
    std::vector<i32> outCursor((size_t)N, 0);
    for (usize i = 0; i < wires.size(); ++i) {
        if (!wires[i].ownerIdx.valid()) continue;
        i32 oe = wires[i].ownerIdx.raw;
        i32 dst = m_outOffset[(size_t)oe] + outCursor[(size_t)oe]++;
        m_outFlat[(size_t)dst] = WireIndex((i32)i);
    }

    // Same dance for incoming (target).
    m_inCount.assign((size_t)N, 0);
    for (usize i = 0; i < wires.size(); ++i) {
        if (wires[i].targetIdx.valid()) m_inCount[(size_t)wires[i].targetIdx.raw]++;
    }
    m_inOffset.assign((size_t)N, 0);
    acc = 0;
    for (i32 i = 0; i < N; ++i) {
        m_inOffset[(size_t)i] = acc;
        acc += m_inCount[(size_t)i];
    }
    m_inFlat.assign((size_t)acc, WireIndex());
    std::vector<i32> inCursor((size_t)N, 0);
    for (usize i = 0; i < wires.size(); ++i) {
        if (!wires[i].targetIdx.valid()) continue;
        i32 te = wires[i].targetIdx.raw;
        i32 dst = m_inOffset[(size_t)te] + inCursor[(size_t)te]++;
        m_inFlat[(size_t)dst] = WireIndex((i32)i);
    }
}

i32 WireGraphIndex::outgoing(Vespucci::EntityIndex e, WireIndex* out, i32 cap) const {
    if (!e.valid() || e.raw >= (i32)m_outCount.size()) return 0;
    i32 n = m_outCount[(size_t)e.raw];
    if (out && cap > 0) {
        i32 base = m_outOffset[(size_t)e.raw];
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = m_outFlat[(size_t)(base + i)];
    }
    return n;
}

i32 WireGraphIndex::incoming(Vespucci::EntityIndex e, WireIndex* out, i32 cap) const {
    if (!e.valid() || e.raw >= (i32)m_inCount.size()) return 0;
    i32 n = m_inCount[(size_t)e.raw];
    if (out && cap > 0) {
        i32 base = m_inOffset[(size_t)e.raw];
        i32 c = (n < cap) ? n : cap;
        for (i32 i = 0; i < c; ++i) out[i] = m_inFlat[(size_t)(base + i)];
    }
    return n;
}

i32 WireGraphIndex::inDegree(Vespucci::EntityIndex e) const {
    if (!e.valid() || e.raw >= (i32)m_inCount.size()) return 0;
    return m_inCount[(size_t)e.raw];
}
i32 WireGraphIndex::outDegree(Vespucci::EntityIndex e) const {
    if (!e.valid() || e.raw >= (i32)m_outCount.size()) return 0;
    return m_outCount[(size_t)e.raw];
}

Span<WireRow> WireGraphIndex::wires() const { return m_wires; }

// Convenience — return the K most heavily-wired entities by total
// degree (in + out). The Brain panel uses this to surface "hot
// nodes" the designer might want to focus on.
i32 WireGraphIndex::topByDegree(Vespucci::EntityIndex* outEntities,
                                  i32* outDegrees, i32 cap) const
{
    if (!outEntities || cap <= 0) return 0;
    i32 N = (i32)m_outCount.size();
    struct Slot { i32 idx; i32 deg; };
    std::vector<Slot> slots;
    slots.reserve((size_t)N);
    for (i32 i = 0; i < N; ++i) {
        i32 d = m_outCount[(size_t)i] + m_inCount[(size_t)i];
        if (d > 0) {
            Slot s; s.idx = i; s.deg = d;
            slots.push_back(s);
        }
    }
    // Selection sort top-K (cap is small, K ~ 10 for the Brain).
    i32 emit = (i32)slots.size();
    if (emit > cap) emit = cap;
    for (i32 i = 0; i < emit; ++i) {
        i32 bestJ = i;
        for (i32 j = i + 1; j < (i32)slots.size(); ++j) {
            if (slots[(size_t)j].deg > slots[(size_t)bestJ].deg) bestJ = j;
        }
        Slot t = slots[(size_t)i]; slots[(size_t)i] = slots[(size_t)bestJ]; slots[(size_t)bestJ] = t;
        outEntities[i] = Vespucci::EntityIndex(slots[(size_t)i].idx);
        if (outDegrees) outDegrees[i] = slots[(size_t)i].deg;
    }
    return emit;
}

// Iterate all (source, target) pairs once, calling the visitor on
// each wire. Used by the corpus builder, the broken-ref scanner, and
// the snapshot debug dump. Visitor returns true to keep going, false
// to bail early.
i32 WireGraphIndex::iterateWires(WireVisitor visitor, void* userData) const {
    if (!visitor) return 0;
    i32 n = 0;
    for (usize i = 0; i < m_wires.size(); ++i) {
        if (!visitor((i32)i, m_wires.ptr[i], userData)) break;
        n++;
    }
    return n;
}

// Stats for the debug overlay.
WireGraphIndex::Stats WireGraphIndex::stats() const {
    Stats s;
    s.entityCount = (i32)m_outCount.size();
    s.wireCount   = (i32)m_wires.size();
    s.maxOutDegree = 0;
    s.maxInDegree  = 0;
    s.zeroDegreeEntities = 0;
    for (size_t i = 0; i < m_outCount.size(); ++i) {
        if (m_outCount[i] > s.maxOutDegree) s.maxOutDegree = m_outCount[i];
        if (m_inCount[i]  > s.maxInDegree)  s.maxInDegree  = m_inCount[i];
        if (m_outCount[i] == 0 && m_inCount[i] == 0) s.zeroDegreeEntities++;
    }
    return s;
}

// Find any cycles by simple DFS — returns count of detected cycles
// up to maxOut. Cycles in the wire graph are normally fine
// (event-loops are intentional in some patterns) but pathological
// cycles (A→B→A with no event filter) cause infinite event chains
// at runtime. The HealthFix panel surfaces detected cycles.
i32 WireGraphIndex::findCyclesUpTo(i32* outFirstEntityOfCycle, i32 cap) const {
    if (!outFirstEntityOfCycle || cap <= 0) return 0;
    i32 N = (i32)m_outCount.size();
    std::vector<u8> color((size_t)N, 0); // 0 unvisited, 1 in-stack, 2 done
    std::vector<i32> stack;
    stack.reserve(64);
    i32 found = 0;

    for (i32 start = 0; start < N && found < cap; ++start) {
        if (color[(size_t)start] != 0) continue;
        // Iterative DFS with explicit stack so we don't blow the C
        // stack on a degenerate level with deep chains.
        stack.clear();
        stack.push_back(start);
        std::vector<i32> path;
        while (!stack.empty() && found < cap) {
            i32 top = stack.back();
            if (color[(size_t)top] == 0) {
                color[(size_t)top] = 1;
                path.push_back(top);
            }
            bool descended = false;
            i32 baseOff = m_outOffset[(size_t)top];
            i32 cnt     = m_outCount[(size_t)top];
            for (i32 k = 0; k < cnt; ++k) {
                WireIndex wIdx = m_outFlat[(size_t)(baseOff + k)];
                if (!wIdx.valid()) continue;
                const WireRow& w = m_wires.ptr[(usize)wIdx.raw];
                if (!w.targetIdx.valid()) continue;
                i32 t = w.targetIdx.raw;
                if (t < 0 || t >= N) continue;
                if (color[(size_t)t] == 1) {
                    // Cycle detected — record the entity that closed it.
                    outFirstEntityOfCycle[found++] = t;
                    if (found >= cap) break;
                } else if (color[(size_t)t] == 0) {
                    stack.push_back(t);
                    descended = true;
                    break;
                }
            }
            if (!descended) {
                color[(size_t)top] = 2;
                if (!path.empty() && path.back() == top) path.pop_back();
                stack.pop_back();
            }
        }
    }
    return found;
}

} // namespace Scene
} // namespace Vespucci
