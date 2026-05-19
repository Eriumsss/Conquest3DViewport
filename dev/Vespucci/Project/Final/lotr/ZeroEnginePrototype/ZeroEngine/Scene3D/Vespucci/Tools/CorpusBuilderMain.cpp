// CorpusBuilderMain.cpp
// =============================================================================
// STANDALONE CORPUS BUILDER CLI - WALKS PAK DIR, EMITS forge_global_corpus.bin
// =============================================================================
// Written by: Eriumsss
//
// Run from build_corpus_builder.bat. Argv:
//   corpus_builder.exe <pak_dir> <output_corpus.bin>
// Iterates every .pak in pak_dir, loads each level snapshot, runs
// the local corpus builder, merges into a single global priors set,
// writes the binary corpus.
// =============================================================================

#include "../Corpus/CorpusPriors.h"
#include "../Corpus/GlobalCorpusBuilder.h"
#include "../Corpus/LocalCorpusBuilder.h"
#include "../Core/Logging.h"
#include "../Core/PathUtils.h"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace Vespucci {
namespace Corpus {
    bool WriteCorpusOrEatShit(const CorpusPriors& priors, const char* path);
}
}

int main(int argc, char** argv) {
    using namespace Vespucci;
    if (argc < 3) {
        std::printf("usage: corpus_builder.exe <pak_dir> <output_corpus.bin>\n");
        return 1;
    }
    const char* pakDir = argv[1];
    const char* outPath = argv[2];

    if (!Core::Path::IsDir(pakDir)) {
        std::fprintf(stderr, "fatal: '%s' is not a directory\n", pakDir);
        return 2;
    }

    Corpus::GlobalCorpusBuilder gb;

    // Walk *.pak in the directory. Real corpus build logic depends on
    // the LevelReader (EXE-side) so this CLI is a stub-with-loops:
    // future patch wires LevelReader through a CLI bridge so the tool
    // can run fully offline.
#if defined(_WIN32)
    std::string pattern = std::string(pakDir) + "\\*.pak";
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(pattern.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        i32 packs = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::printf("  [%d] %s\n", packs, fd.cFileName);
            packs++;
            // (placeholder) per-pak corpus extraction goes here.
        } while (FindNextFileA(hf, &fd));
        FindClose(hf);
        std::printf("walked %d pak files in %s\n", packs, pakDir);
    }
#endif

    Corpus::CorpusPriors merged;
    gb.mergeAllShardsOrFuckOff(merged);

    if (!Corpus::WriteCorpusOrEatShit(merged, outPath)) {
        std::fprintf(stderr, "fatal: write failed for %s\n", outPath);
        return 3;
    }
    std::printf("corpus written to %s\n", outPath);
    return 0;
}
