// Counters.cpp
// =============================================================================
// HASH-KEYED MONOTONIC COUNTERS WITH PER-FRAME DELTA SAMPLING
// =============================================================================
// Written by: Eriumsss
//
// Single-threaded by design — Vespucci runs all of its work on the
// editor's main thread, and counters from background threads (we
// don't have any yet, but if we ever ship a background corpus
// builder) would need their own thread-local rings.
//
// Two flavors of read:
//   1. GetCounter(name) — current cumulative value.
//   2. GetCounterDelta(name) — delta since the last call. Used by
//      per-frame dashboards so they can render "ranker queries this
//      frame: 12" without saving state at every call site.
//
// Counter naming convention is "subsystem.event" or "subsystem.event.tag":
//   - "ranker.queries"
//   - "ranker.queries.suppressed"
//   - "ranker.queries.cached"
//   - "snapshot.builds"
//   - "apply.commits"
//   - "apply.undos"
//   - "corpus.observations"
//   - "compat.denies"
//   - "compat.allows"
//   - "healthfix.scans"
//   - "healthfix.heals"
//
// The dashboard renders these grouped by the dot-separated prefix so
// ranker-related counters cluster, snapshot-related counters cluster,
// etc. Designer trying to understand "did the ranker actually fire?"
// looks at the ranker.* group; "is the corpus growing?" looks at
// corpus.*. No magic — just a string-prefix grouping.
// =============================================================================

#include "Counters.h"

#include "../Core/Hash.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace Vespucci {
namespace Telemetry {

namespace {
    struct Entry {
        const char* name;
        u64         value;
        u64         lastSampledValue;   // for delta queries
        u64         resetEpoch;          // bumps on ResetAllCounters
    };
    static std::unordered_map<u32, Entry> s_counters;
    static u64 s_currentEpoch = 0;

    Entry& EnsureCounter(const char* name) {
        u32 h = Core::XxHash32CStr(name, 0xC04E7E45u);
        Entry& e = s_counters[h];
        if (!e.name) {
            e.name             = name;
            e.value            = 0;
            e.lastSampledValue = 0;
            e.resetEpoch       = s_currentEpoch;
        }
        return e;
    }

    // Compare counter names by their dot-prefix group THEN alphabetical
    // within group. Used by sorted-iteration helpers.
    bool DotPrefixLessThan(const char* a, const char* b) {
        const char* pa = std::strchr(a, '.');
        const char* pb = std::strchr(b, '.');
        usize la = pa ? (usize)(pa - a) : std::strlen(a);
        usize lb = pb ? (usize)(pb - b) : std::strlen(b);
        usize lcmp = la < lb ? la : lb;
        i32 r = std::strncmp(a, b, lcmp);
        if (r != 0) return r < 0;
        if (la != lb) return la < lb;
        return std::strcmp(a, b) < 0;
    }
} // namespace

void IncrementCounter(const char* name, u64 delta) {
    if (!name) return;
    EnsureCounter(name).value += delta;
}

void DecrementCounter(const char* name, u64 delta) {
    if (!name) return;
    Entry& e = EnsureCounter(name);
    if (e.value < delta) e.value = 0;
    else                 e.value -= delta;
}

void SetCounter(const char* name, u64 value) {
    if (!name) return;
    EnsureCounter(name).value = value;
}

u64 GetCounter(const char* name) {
    if (!name) return 0;
    u32 h = Core::XxHash32CStr(name, 0xC04E7E45u);
    std::unordered_map<u32, Entry>::const_iterator it = s_counters.find(h);
    if (it == s_counters.end()) return 0;
    return it->second.value;
}

// Delta since the last call. First call returns the current value.
// Used by per-frame dashboards.
u64 GetCounterDelta(const char* name) {
    if (!name) return 0;
    u32 h = Core::XxHash32CStr(name, 0xC04E7E45u);
    std::unordered_map<u32, Entry>::iterator it = s_counters.find(h);
    if (it == s_counters.end()) return 0;
    Entry& e = it->second;
    u64 d = (e.value >= e.lastSampledValue)
            ? (e.value - e.lastSampledValue)
            : 0;   // counter went backwards (Set was called) — show 0
    e.lastSampledValue = e.value;
    return d;
}

void ResetAllCounters() {
    s_counters.clear();
    s_currentEpoch++;
}

void ResetCounter(const char* name) {
    if (!name) return;
    u32 h = Core::XxHash32CStr(name, 0xC04E7E45u);
    std::unordered_map<u32, Entry>::iterator it = s_counters.find(h);
    if (it != s_counters.end()) {
        it->second.value = 0;
        it->second.lastSampledValue = 0;
    }
}

i32 CounterCount() { return (i32)s_counters.size(); }

const char* CounterNameAt(i32 i) {
    if (i < 0 || i >= (i32)s_counters.size()) return 0;
    std::unordered_map<u32, Entry>::const_iterator it = s_counters.begin();
    std::advance(it, i);
    return it->second.name;
}

u64 CounterValueAt(i32 i) {
    if (i < 0 || i >= (i32)s_counters.size()) return 0;
    std::unordered_map<u32, Entry>::const_iterator it = s_counters.begin();
    std::advance(it, i);
    return it->second.value;
}

// Group counters by their dot-prefix and emit a grouped iteration.
// Caller passes a callback that gets (prefix, name, value); useful for
// rendering the dashboard split by subsystem.
void IterateGrouped(void (*emit)(const char* prefix, const char* name,
                                   u64 value, void* user),
                    void* user)
{
    if (!emit) return;
    // Snapshot to vector for stable sort.
    std::vector<const Entry*> entries;
    entries.reserve(s_counters.size());
    for (std::unordered_map<u32, Entry>::const_iterator it = s_counters.begin();
         it != s_counters.end(); ++it)
    {
        entries.push_back(&it->second);
    }
    // Sort by dot-prefix then alphabetical.
    for (size_t i = 1; i < entries.size(); ++i) {
        for (size_t j = i; j > 0; --j) {
            if (DotPrefixLessThan(entries[j]->name, entries[j-1]->name)) {
                const Entry* t = entries[j]; entries[j] = entries[j-1]; entries[j-1] = t;
            } else break;
        }
    }
    char prefixBuf[64];
    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry* e = entries[i];
        const char* dot = std::strchr(e->name, '.');
        usize plen = dot ? (usize)(dot - e->name) : std::strlen(e->name);
        if (plen > sizeof(prefixBuf) - 1) plen = sizeof(prefixBuf) - 1;
        std::memcpy(prefixBuf, e->name, plen);
        prefixBuf[plen] = 0;
        emit(prefixBuf, e->name, e->value, user);
    }
}

// Convenience aggregations used by the dashboard.
u64 SumCountersInGroup(const char* prefix) {
    if (!prefix) return 0;
    usize plen = std::strlen(prefix);
    u64 total = 0;
    for (std::unordered_map<u32, Entry>::const_iterator it = s_counters.begin();
         it != s_counters.end(); ++it)
    {
        if (std::strncmp(it->second.name, prefix, plen) == 0 &&
            (it->second.name[plen] == 0 || it->second.name[plen] == '.'))
        {
            total += it->second.value;
        }
    }
    return total;
}

i32 CountersInGroup(const char* prefix) {
    if (!prefix) return 0;
    usize plen = std::strlen(prefix);
    i32 n = 0;
    for (std::unordered_map<u32, Entry>::const_iterator it = s_counters.begin();
         it != s_counters.end(); ++it)
    {
        if (std::strncmp(it->second.name, prefix, plen) == 0 &&
            (it->second.name[plen] == 0 || it->second.name[plen] == '.'))
        {
            n++;
        }
    }
    return n;
}

} // namespace Telemetry
} // namespace Vespucci
