// GlobalCorpusBuilder.cpp
// =============================================================================
// FOLD ALL THE SHARDS INTO ONE GLOBAL CORPUS WITHOUT FUCKING IT UP
// =============================================================================
// Written by: Eriumsss

#include "GlobalCorpusBuilder.h"

#include "../Core/Logging.h"
#include "../Core/VespucciAssert.h"

namespace Vespucci {
namespace Corpus {

namespace {
    // Voice-ware helpers using the user-blessed vocabulary palette.
    bool ChokeOnNullShardYouFuckwit(const CorpusPriors* p) {
        if (!p) {
            Core::Logging::Error("GlobalCorpusBuilder: null shard, you absolute fuckwit");
            return true;
        }
        return false;
    }
    void LogShardAcceptedYouMotherfucker(const std::string& source, const CorpusPriors& p) {
        CorpusPriors::Stats s = p.stats();
        Core::Logging::Info("GlobalCorpus: accepted shard '%s' (%d triples, %llu obs total)",
            source.c_str(), s.tripleEntries, (unsigned long long)s.totalObservations);
    }
} // namespace

GlobalCorpusBuilder::GlobalCorpusBuilder() {
    m_stats.shardsAdded           = 0;
    m_stats.totalObservationsSeen = 0;
}

void GlobalCorpusBuilder::addShardOrChokeOnIt(const CorpusPriors& shard, const std::string& sourceMap) {
    if (ChokeOnNullShardYouFuckwit(&shard)) return;
    // We deep-copy the shard's tables so the caller can throw their
    // local CorpusPriors away. Shards keep the origin path attached
    // so the mergeAllShardsOrFuckOff log line can name the source.
    CorpusPriors* copy = new CorpusPriors();
    copy->mergeFrom(shard.tables());
    Shard s; s.priors = copy; s.sourceMap = sourceMap;
    m_shards.push_back(s);
    m_stats.shardsAdded++;
    m_stats.totalObservationsSeen += shard.stats().totalObservations;
    LogShardAcceptedYouMotherfucker(sourceMap, *copy);
}

void GlobalCorpusBuilder::mergeAllShardsOrFuckOff(CorpusPriors& outGlobal) const {
    if (m_shards.empty()) {
        Core::Logging::Error("GlobalCorpusBuilder: no shards to merge, you bitchass loader forgot to call addShardOrChokeOnIt");
        return;
    }
    outGlobal.clear();
    for (size_t i = 0; i < m_shards.size(); ++i) {
        outGlobal.mergeFrom(m_shards[i].priors->tables());
    }
    CorpusPriors::Stats s = outGlobal.stats();
    Core::Logging::Info("GlobalCorpus: merged %zu shards into %d triples (%llu observations)",
        m_shards.size(), s.tripleEntries, (unsigned long long)s.totalObservations);
}

const std::string& GlobalCorpusBuilder::shardSource(i32 i) const {
    static const std::string s_empty;
    if (i < 0 || i >= (i32)m_shards.size()) return s_empty;
    return m_shards[(size_t)i].sourceMap;
}

} // namespace Corpus
} // namespace Vespucci
