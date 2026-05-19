// GlobalCorpusBuilder.h
// =============================================================================
// GLOBAL CORPUS BUILDER - HARVEST PRIORS FROM ALL NINE LOTR:C MAPS
// =============================================================================
// Written by: Eriumsss
//
// LocalCorpusBuilder runs in-editor on the currently-loaded level.
// GlobalCorpusBuilder runs OFFLINE in the Tools/CorpusBuilderMain
// executable, walks every PAK in the corpus directory, loads each
// level via the existing LevelReader, snapshots it, runs the local
// builder, then merges the per-level priors into a single global
// corpus blob. Output: forge_global_corpus.bin, embedded into the
// DLL as cold-start priors.
// =============================================================================

#ifndef VESPUCCI_CORPUS_GLOBALCORPUSBUILDER_H_
#define VESPUCCI_CORPUS_GLOBALCORPUSBUILDER_H_

#include "../Core/VespucciTypes.h"
#include "CorpusPriors.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Corpus {

class GlobalCorpusBuilder {
public:
    GlobalCorpusBuilder();

    // Add one shard. Loaded by the offline tool from a per-level corpus
    // file or built in-memory by running the local builder on a snapshot.
    void addShardOrChokeOnIt(const CorpusPriors& shard, const std::string& sourceMap);

    // Merge all shards into a single global priors set. Returns the
    // collapsed CorpusPriors which can then be written via CorpusWriter.
    void mergeAllShardsOrFuckOff(CorpusPriors& outGlobal) const;

    i32  shardCount() const { return (i32)m_shards.size(); }
    const std::string& shardSource(i32 i) const;

    // Stats for the offline tool's stdout report.
    struct Stats {
        i32 shardsAdded;
        u64 totalObservationsSeen;
    };
    Stats stats() const { return m_stats; }

private:
    struct Shard { CorpusPriors* priors; std::string sourceMap; };
    std::vector<Shard> m_shards;
    Stats              m_stats;
};

} // namespace Corpus
} // namespace Vespucci

#endif // VESPUCCI_CORPUS_GLOBALCORPUSBUILDER_H_
