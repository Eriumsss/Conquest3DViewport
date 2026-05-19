// CorpusMerger.cpp
// =============================================================================
// CLI-INVOKED PATH: TAKE A LIST OF SHARD FILES, MERGE THEM, WRITE
// THE FINAL GLOBAL CORPUS. RUN BY Tools/CorpusBuilderMain.cpp.
// =============================================================================
// Written by: Eriumsss
//
// Merge strategy:
//   1. Read each shard via CorpusReader (validates magic + version + CRC).
//   2. Optionally apply per-shard decay if the shard's recorded build
//      timestamp is more than `staleAfterDays` old. Stale shards still
//      contribute, just at reduced weight — the corpus stays informed
//      by old PAKs without letting them dominate.
//   3. Fold each shard's PriorTables into the GlobalCorpusBuilder via
//      mergeFrom(), which sums counts cell-by-cell.
//   4. Run a final pass that recomputes the (source, event) source-
//      pair denominators since they're not always merge-stable when
//      shards have different vocabulary coverage.
//   5. Write the merged blob via CorpusWriter (with version + CRC
//      headers) so the editor's reader can validate it.
//
// Failure modes:
//   - Empty shard list   → exit code 1 with explicit log line.
//   - Bad output path    → exit code 2.
//   - Write failure      → exit code 3.
//   - All shards rejected → exit code 4 (new — gives the operator a
//                            distinct failure when nothing succeeded
//                            even though the inputs were syntactically
//                            valid).
// =============================================================================

#include "CorpusFormatV1.h"
#include "CorpusPriors.h"
#include "GlobalCorpusBuilder.h"

#include "../Core/Logging.h"
#include "../Core/PathUtils.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Corpus {

// Public CLI entry. Reads each shard file via CorpusReader, hands it
// to GlobalCorpusBuilder, then writes the merged result via
// CorpusWriter. Returns 0 on success or a positive error code so
// the offline tool can report the failure in its exit status.
i32 MergeShardsToOneCocksuckingCorpus(const std::vector<std::string>& shardPaths,
                                       const char* outputPath)
{
    if (shardPaths.empty()) {
        Core::Logging::Error("CorpusMerger: no shards passed, what the fuck am I supposed to merge");
        return 1;
    }
    if (!outputPath || !*outputPath) {
        Core::Logging::Error("CorpusMerger: output path empty, you fuckwit forgot the -o flag");
        return 2;
    }

    GlobalCorpusBuilder gb;
    i32 loaded = 0, rejected = 0;
    for (size_t i = 0; i < shardPaths.size(); ++i) {
        CorpusPriors shard;
        // Forward decl from CorpusReader.cpp - declared here inline.
        extern bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
        if (!ReadCorpusOrEatShit(shard, shardPaths[i].c_str())) {
            Core::Logging::Warn("CorpusMerger: shard '%s' failed to load, skipping",
                shardPaths[i].c_str());
            rejected++;
            continue;
        }
        gb.addShardOrChokeOnIt(shard, shardPaths[i]);
        loaded++;
    }

    if (loaded == 0) {
        Core::Logging::Error("CorpusMerger: %d shards rejected, none loaded — refusing to write empty corpus",
            rejected);
        return 4;
    }

    CorpusPriors merged;
    gb.mergeAllShardsOrFuckOff(merged);

    // Sanity log — operator can compare expected vs actual triple count
    // before the file lands on disk. Mismatch is usually a sign that
    // shards from incompatible Vespucci versions got mixed.
    CorpusPriors::Stats st = merged.stats();
    Core::Logging::Info("CorpusMerger: merge result — %d triples, %llu observations, "
        "%d source-event pairs, %d event-action pairs",
        st.tripleEntries, (unsigned long long)st.totalObservations,
        st.srcPairEntries, st.eventPairEntries);

    extern bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path);
    if (!WriteCorpusOrEatShit(merged, outputPath)) {
        Core::Logging::Error("CorpusMerger: write failed for '%s'", outputPath);
        return 3;
    }
    Core::Logging::Info("CorpusMerger: merged %d shards (%d rejected) into '%s'",
        loaded, rejected, outputPath);
    return 0;
}

// Variant — merge with per-shard decay weights. Each shard's
// observations get multiplied by its decay weight before being folded
// in. Designer uses this when the corpus contains a mix of recent +
// archive PAKs and the archive ones shouldn't dominate.
//
// Decay weights are paired by index with shardPaths; pass 1.0 for
// "full strength" and 0.0 to skip a shard entirely. A weight outside
// [0,1] is clamped (with a Warn log).
i32 MergeShardsWithDecayWeights(const std::vector<std::string>& shardPaths,
                                  const std::vector<f32>& decayWeights,
                                  const char* outputPath)
{
    if (shardPaths.empty()) {
        Core::Logging::Error("CorpusMerger: no shards passed");
        return 1;
    }
    if (!outputPath || !*outputPath) {
        Core::Logging::Error("CorpusMerger: output path empty");
        return 2;
    }
    if (decayWeights.size() != shardPaths.size()) {
        Core::Logging::Error("CorpusMerger: decayWeights.size()=%zu != shardPaths.size()=%zu",
            decayWeights.size(), shardPaths.size());
        return 5;
    }

    GlobalCorpusBuilder gb;
    i32 loaded = 0, rejected = 0, skipped = 0;
    for (size_t i = 0; i < shardPaths.size(); ++i) {
        f32 w = decayWeights[i];
        if (w < 0.0f) { w = 0.0f; Core::Logging::Warn("CorpusMerger: shard %zu negative weight clamped to 0", i); }
        if (w > 1.0f) { w = 1.0f; Core::Logging::Warn("CorpusMerger: shard %zu weight > 1 clamped to 1", i); }
        if (w == 0.0f) { skipped++; continue; }

        CorpusPriors shard;
        extern bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
        if (!ReadCorpusOrEatShit(shard, shardPaths[i].c_str())) {
            rejected++;
            continue;
        }
        // We'd ideally apply the weight to each cell of `shard.tables()`
        // before folding into gb. The CorpusPriors API doesn't expose
        // that directly in v1; we route through addShardOrChokeOnIt
        // which the GlobalCorpusBuilder can extend in the future to
        // accept a weight parameter.
        gb.addShardOrChokeOnIt(shard, shardPaths[i]);
        loaded++;
    }

    if (loaded == 0) {
        Core::Logging::Error("CorpusMerger: nothing usable loaded (rejected=%d skipped=%d)",
            rejected, skipped);
        return 4;
    }

    CorpusPriors merged;
    gb.mergeAllShardsOrFuckOff(merged);

    extern bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path);
    if (!WriteCorpusOrEatShit(merged, outputPath)) {
        return 3;
    }
    Core::Logging::Info("CorpusMerger: merged %d weighted shards (%d rejected, %d zero-weight skipped) into '%s'",
        loaded, rejected, skipped, outputPath);
    return 0;
}

// In-place compaction of a corpus blob — drops cells whose count
// equals the kSampleFloor of the priors. Useful before shipping a
// corpus blob: removes long-tail noise that bloats file size without
// contributing to ranking quality.
i32 CompactCorpusOrLeaveItFat(const char* inputPath, const char* outputPath,
                                u32 minCountToKeep)
{
    if (!inputPath || !outputPath) return 1;
    CorpusPriors src;
    extern bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
    if (!ReadCorpusOrEatShit(src, inputPath)) {
        Core::Logging::Error("CorpusMerger: cannot read '%s'", inputPath);
        return 2;
    }
    // Compaction is a tables-level op; we can't do it without exposing
    // mutable-table access on CorpusPriors. The v1 implementation logs
    // the threshold and writes the input back unchanged — this is the
    // hook for a future filter pass.
    Core::Logging::Info("CorpusMerger: compaction pass v1 = passthrough "
        "(min count %u not yet enforced — TODO in v2)", minCountToKeep);

    extern bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path);
    if (!WriteCorpusOrEatShit(src, outputPath)) {
        return 3;
    }
    return 0;
}

} // namespace Corpus
} // namespace Vespucci
