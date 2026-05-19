// IndexBenchmark.cpp
// =============================================================================
// Build all four peers, run synthetic queries, pick a winner. Reports
// to the log so the developer can pin a default if benchmark says
// the auto-pick is wrong.
// =============================================================================
// Written by: Eriumsss

#include "IndexBenchmark.h"

#include "GridIndex.h"
#include "KdTreeIndex.h"
#include "RTreeIndex.h"
#include "BvhIndex.h"

#include "../Core/HeatTimer.h"
#include "../Core/Logging.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace Vespucci {
namespace Index {

namespace {
    // Synthetic query workload sized so the benchmark itself takes
    // under 50 ms on a typical level, even when all four peers run.
    static const i32 kRadiusQueries = 256;
    static const i32 kAabbQueries   = 256;
    static const i32 kKnnQueries    = 64;
    static const i32 kKnnK          = 5;

    f32 RunRadiusWorkload(const ISpatialIndex& idx, const Scene::SceneSnapshot& snap) {
        if (snap.entityCount() == 0) return 0.0f;
        std::vector<Vespucci::EntityIndex> out(64);
        u64 t0 = Core::NowTicks();
        for (i32 q = 0; q < kRadiusQueries; ++q) {
            i32 ei = q % snap.entityCount();
            const Scene::EntityRow* r = snap.entityAt(Vespucci::EntityIndex(ei));
            if (!r) continue;
            idx.queryRadius(r->position, 32.0f, out.data(), (i32)out.size());
        }
        u64 ticks = Core::NowTicks() - t0;
        return (f32)Core::TicksToNs(ticks) / 1000000.0f;
    }
    f32 RunAabbWorkload(const ISpatialIndex& idx, const Scene::SceneSnapshot& snap) {
        if (snap.entityCount() == 0) return 0.0f;
        std::vector<Vespucci::EntityIndex> out(64);
        u64 t0 = Core::NowTicks();
        for (i32 q = 0; q < kAabbQueries; ++q) {
            i32 ei = q % snap.entityCount();
            const Scene::EntityRow* r = snap.entityAt(Vespucci::EntityIndex(ei));
            if (!r) continue;
            Aabb b(Vec3(r->position.x - 16, r->position.y - 16, r->position.z - 16),
                   Vec3(r->position.x + 16, r->position.y + 16, r->position.z + 16));
            idx.queryAabb(b, out.data(), (i32)out.size());
        }
        u64 ticks = Core::NowTicks() - t0;
        return (f32)Core::TicksToNs(ticks) / 1000000.0f;
    }
    f32 RunKnnWorkload(const ISpatialIndex& idx, const Scene::SceneSnapshot& snap) {
        if (snap.entityCount() == 0) return 0.0f;
        std::vector<Vespucci::EntityIndex> out((size_t)kKnnK);
        u64 t0 = Core::NowTicks();
        for (i32 q = 0; q < kKnnQueries; ++q) {
            i32 ei = q % snap.entityCount();
            const Scene::EntityRow* r = snap.entityAt(Vespucci::EntityIndex(ei));
            if (!r) continue;
            idx.queryKNearest(r->position, kKnnK, out.data(), (i32)out.size());
        }
        u64 ticks = Core::NowTicks() - t0;
        return (f32)Core::TicksToNs(ticks) / 1000000.0f;
    }

    void RunOnePeer(ISpatialIndex& peer, const Scene::SceneSnapshot& snap, PeerResult& r) {
        peer.build(snap);
        BuildStats s = peer.lastBuildStats();
        r.kind            = peer.kind();
        r.name            = peer.name();
        r.buildMs         = s.buildMs;
        r.memoryBytes     = s.memoryBytes;
        r.queryMs_radius  = RunRadiusWorkload(peer, snap);
        r.queryMs_aabb    = RunAabbWorkload(peer, snap);
        r.queryMs_knn     = RunKnnWorkload(peer, snap);
        Core::Logging::Info("Index benchmark: %s build=%.2fms r=%.2fms a=%.2fms k=%.2fms mem=%llu",
            r.name, r.buildMs, r.queryMs_radius, r.queryMs_aabb, r.queryMs_knn,
            (unsigned long long)r.memoryBytes);
    }
} // namespace

void RunIndexBenchmark(const Scene::SceneSnapshot& snap, BenchmarkResult& out) {
    GridIndex   g;
    KdTreeIndex k;
    RTreeIndex  r;
    BvhIndex    b;
    out.peerCount = 4;
    RunOnePeer(g, snap, out.peers[0]);
    RunOnePeer(k, snap, out.peers[1]);
    RunOnePeer(r, snap, out.peers[2]);
    RunOnePeer(b, snap, out.peers[3]);

    // Combined cost: build cost is amortized across many queries, so
    // weight queries 4x build for the winner pick.
    f32 bestCost = 1e30f;
    out.winner = ISpatialIndex::KIND_Grid;
    for (i32 i = 0; i < out.peerCount; ++i) {
        const PeerResult& p = out.peers[i];
        f32 cost = p.buildMs + 4.0f * (p.queryMs_radius + p.queryMs_aabb + p.queryMs_knn);
        if (cost < bestCost) { bestCost = cost; out.winner = p.kind; }
    }
    const char* winnerName = "?";
    for (i32 i = 0; i < out.peerCount; ++i) {
        if (out.peers[i].kind == out.winner) winnerName = out.peers[i].name;
    }
    Core::Logging::Info("Index benchmark winner: %s (combined cost %.2fms)", winnerName, bestCost);
}

} // namespace Index
} // namespace Vespucci
