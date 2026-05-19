// GoldenCorpus.cpp
// =============================================================================
// PIN A KNOWN-GOOD CORPUS BLOB AS THE BASELINE FOR REGRESSION TESTS
// =============================================================================
// Written by: Eriumsss

#include "../Core/FileIO.h"
#include "../Core/Logging.h"
#include "../Corpus/CorpusPriors.h"

namespace Vespucci {
namespace QA {

} // namespace QA
namespace Corpus {
    extern bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
} // namespace Corpus
namespace QA {

bool LoadGoldenCorpusOrChokeOnVersion(const char* path, Corpus::CorpusPriors& out) {
    if (!Corpus::ReadCorpusOrEatShit(out, path)) {
        Core::Logging::Error("Golden corpus load failed for '%s'", path);
        return false;
    }
    Corpus::CorpusPriors::Stats s = out.stats();
    Core::Logging::Info("Golden corpus loaded: %d triples, %llu observations",
        s.tripleEntries, (unsigned long long)s.totalObservations);
    return true;
}

bool DiffCorpusAgainstGolden(const Corpus::CorpusPriors& current,
                              const Corpus::CorpusPriors& golden,
                              i32& outAdded, i32& outRemoved, i32& outChanged)
{
    outAdded = outRemoved = outChanged = 0;
    const Corpus::PriorTables& cur = current.tables();
    const Corpus::PriorTables& gld = golden.tables();
    for (std::unordered_map<Corpus::TripleKey, u32, Corpus::TripleKeyHash>::const_iterator it =
             cur.srcEventToTarget.begin(); it != cur.srcEventToTarget.end(); ++it)
    {
        std::unordered_map<Corpus::TripleKey, u32, Corpus::TripleKeyHash>::const_iterator
            git = gld.srcEventToTarget.find(it->first);
        if (git == gld.srcEventToTarget.end())  outAdded++;
        else if (git->second != it->second)     outChanged++;
    }
    for (std::unordered_map<Corpus::TripleKey, u32, Corpus::TripleKeyHash>::const_iterator it =
             gld.srcEventToTarget.begin(); it != gld.srcEventToTarget.end(); ++it)
    {
        if (cur.srcEventToTarget.find(it->first) == cur.srcEventToTarget.end()) outRemoved++;
    }
    Core::Logging::Info("Corpus diff: +%d new, -%d removed, ~%d changed",
        outAdded, outRemoved, outChanged);
    return outAdded == 0 && outRemoved == 0 && outChanged == 0;
}

} // namespace QA
} // namespace Vespucci
