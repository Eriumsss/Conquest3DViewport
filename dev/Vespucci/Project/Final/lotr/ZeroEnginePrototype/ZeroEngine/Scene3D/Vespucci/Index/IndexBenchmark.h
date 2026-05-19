// IndexBenchmark.h
// =============================================================================
// INDEX BENCHMARK — RUN ALL FOUR PEERS, PICK THE FASTEST FOR THIS SCENE
// =============================================================================
// Written by: Eriumsss
//
// Runs each peer's build + a synthetic query workload (radius queries
// at random points + AABB queries + kNN queries) and prints the times.
// IndexFactory uses the result to pick a default for the loaded level.
// We do NOT run the benchmark in shipping editor sessions; only when
// the user opens the Brain debug tab and clicks "Benchmark spatial".
// =============================================================================

#ifndef VESPUCCI_INDEX_INDEXBENCHMARK_H_
#define VESPUCCI_INDEX_INDEXBENCHMARK_H_

#include "../Core/VespucciTypes.h"
#include "../Scene/SceneSnapshot.h"
#include "ISpatialIndex.h"

namespace Vespucci {
namespace Index {

struct PeerResult {
    ISpatialIndex::Kind kind;
    const char*         name;
    f32                 buildMs;
    f32                 queryMs_radius;
    f32                 queryMs_aabb;
    f32                 queryMs_knn;
    u64                 memoryBytes;
};

struct BenchmarkResult {
    PeerResult peers[4];
    i32        peerCount;
    ISpatialIndex::Kind winner;     // overall fastest combined
};

void RunIndexBenchmark(const Scene::SceneSnapshot& snap, BenchmarkResult& out);

} // namespace Index
} // namespace Vespucci

#endif // VESPUCCI_INDEX_INDEXBENCHMARK_H_
