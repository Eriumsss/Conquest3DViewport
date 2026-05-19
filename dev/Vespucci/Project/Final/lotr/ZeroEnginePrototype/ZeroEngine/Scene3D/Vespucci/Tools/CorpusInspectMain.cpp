// CorpusInspectMain.cpp
// =============================================================================
// LOAD A CORPUS FILE, PRINT STATS + TOP TRIPLES
// =============================================================================
// Written by: Eriumsss

#include "../Corpus/CorpusPriors.h"

#include "../Schema/ZETypeRegistry.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Vespucci {
namespace Corpus {
    bool ReadCorpusOrEatShit(CorpusPriors& outPriors, const char* path);
}
}

int main(int argc, char** argv) {
    using namespace Vespucci;
    if (argc < 2) {
        std::printf("usage: corpus_inspect.exe <corpus.bin>\n");
        return 1;
    }
    Schema::ZETypeRegistry reg;
    reg.init();
    Corpus::CorpusPriors p;
    if (!Corpus::ReadCorpusOrEatShit(p, argv[1])) {
        std::fprintf(stderr, "fatal: cannot load corpus '%s'\n", argv[1]);
        return 2;
    }
    Corpus::CorpusPriors::Stats s = p.stats();
    std::printf("Corpus: %s\n", argv[1]);
    std::printf("  triples       = %d\n", s.tripleEntries);
    std::printf("  src-pairs     = %d\n", s.srcPairEntries);
    std::printf("  event-pairs   = %d\n", s.eventPairEntries);
    std::printf("  total observations = %llu\n",
        (unsigned long long)s.totalObservations);
    return 0;
}
