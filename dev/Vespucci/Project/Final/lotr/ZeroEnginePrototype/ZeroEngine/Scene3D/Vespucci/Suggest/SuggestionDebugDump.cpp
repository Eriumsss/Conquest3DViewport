// SuggestionDebugDump.cpp
// =============================================================================
// DUMP A SUGGESTIONLIST AS HUMAN-READABLE TEXT FOR THE BRAIN OVERLAY
// AND THE 'WHY THE FUCK DID YOU SUGGEST THIS' DEBUG MENU.
// =============================================================================
// Written by: Eriumsss

#include "SuggestionTypes.h"

#include "../Core/FileIO.h"
#include "../Core/Logging.h"

#include <cstdio>
#include <string>

namespace Vespucci {
namespace Suggest {

namespace {
    void AppendChip(std::string& out, const ReasonChip& c) {
        out += "    - ";
        out.append(c.text.data(), c.text.size());
        char buf[32]; std::snprintf(buf, sizeof(buf), " (+%.3f)\n", c.contribution);
        out += buf;
    }
} // namespace

std::string DumpListAsHumanReadable(const SuggestionList& list) {
    std::string out;
    char hdr[256];
    std::snprintf(hdr, sizeof(hdr),
        "SuggestionList (count=%d, pool=%d, hardFiltered=%d, suppressed=%d, rules=%d, "
        "%.2fms, snap=%u)\n",
        list.count,
        list.candidatePoolSize,
        list.candidatesAfterHardFilter,
        list.suppressedByConfidence,
        list.rulesEvaluated,
        list.rankerMs,
        list.snapshotVersionUsed.raw);
    out += hdr;

    for (i32 i = 0; i < list.count; ++i) {
        const Suggestion& s = list.suggestions[i];
        char line[256];
        std::snprintf(line, sizeof(line),
            "  [%d] target=%d(0x%08X) score=%.3f conf=%.3f%s\n",
            i, s.target.raw, (unsigned)s.targetGuid.raw,
            s.score, s.confidence,
            s.faulted ? " FAULTED" : "");
        out += line;
        for (i32 r = 0; r < s.reasonCount; ++r) AppendChip(out, s.reasons[r]);
    }
    return out;
}

bool DumpListToFileOrPissOnDisk(const SuggestionList& list, const char* path) {
    if (!path || !*path) return false;
    std::string body = DumpListAsHumanReadable(list);
    if (!Core::WriteAtomic(path, body.data(), (i64)body.size())) {
        Core::Logging::Error("SuggestionDebugDump: write failed for '%s'", path);
        return false;
    }
    return true;
}

} // namespace Suggest
} // namespace Vespucci
