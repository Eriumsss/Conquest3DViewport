// CorpusPriors.cpp
// =============================================================================
// Stupid-Backoff over five levels. The hot-path lookup is fucking
// fast — three hash-map probes max before falling through to defaults.
// Bumping is two hash-map writes per observation. Serialization is
// trivial: dump the tables as flat (key, count) records prefixed by a
// magic + schema version.
// =============================================================================
// Written by: Eriumsss

#include "CorpusPriors.h"

#include "../Core/Hash.h"
#include "../Core/Logging.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Vespucci {
namespace Corpus {

namespace {
    static CorpusPriors* s_global = 0;

    static const u32 kSerializeMagic   = 0x56455350u; // "VESP"
    static const u32 kSerializeVersion = 1;
} // namespace

CorpusPriors* GlobalPriors()                 { return s_global; }
void          SetGlobalPriors(CorpusPriors* p) { s_global = p; }

usize TripleKeyHash::operator()(const TripleKey& k) const {
    u32 h = Core::SplitMix32(k.sourceType.raw);
    h = Core::CombineHash(h, k.eventHash);
    h = Core::CombineHash(h, k.targetType.raw);
    return (usize)h;
}
usize PairKeyHash::operator()(const PairKey& k) const {
    u32 h = Core::SplitMix32(k.a.raw);
    h = Core::CombineHash(h, k.b.raw);
    h = Core::CombineHash(h, k.extra);
    return (usize)h;
}

CorpusPriors::CorpusPriors() : m_lastLevelFired(-1) {}

void CorpusPriors::clear() {
    m_t.srcEventToTarget.clear();
    m_t.srcToTarget.clear();
    m_t.eventToTarget.clear();
    m_t.srcEventTotal.clear();
    m_t.srcTotal.clear();
    m_t.eventTotal.clear();
    m_lastLevelFired = -1;
}

void CorpusPriors::bumpObservation(TypeId src, const Core::StringRef& evt, TypeId tgt) {
    if (!src.valid() || !tgt.valid() || evt.size() == 0) return;
    u32 evtHash = Core::XxHash32(evt.data(), evt.size(), 0xC0FFEE13u);

    TripleKey tk; tk.sourceType = src; tk.eventHash = evtHash; tk.targetType = tgt;
    m_t.srcEventToTarget[tk]++;

    TripleKey ttk = tk; ttk.targetType = TypeId(0);
    m_t.srcEventTotal[ttk]++;

    PairKey p1; p1.a = src; p1.b = tgt; p1.extra = 0;
    m_t.srcToTarget[p1]++;
    m_t.srcTotal[src]++;

    PairKey p2; p2.a = TypeId(0); p2.b = tgt; p2.extra = evtHash;
    m_t.eventToTarget[p2]++;
    m_t.eventTotal[evtHash]++;
}

logprob_t CorpusPriors::logProb(TypeId src, const Core::StringRef& evt, TypeId tgt) const {
    m_lastLevelFired = -1;
    if (!src.valid() || !tgt.valid() || evt.size() == 0) {
        m_lastLevelFired = 5;
        return -10.0f;
    }
    u32 evtHash = Core::XxHash32(evt.data(), evt.size(), 0xC0FFEE13u);

    // Level 1: P(t | s, e)
    TripleKey tk; tk.sourceType = src; tk.eventHash = evtHash; tk.targetType = tgt;
    std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator it1 =
        m_t.srcEventToTarget.find(tk);
    TripleKey ttk = tk; ttk.targetType = TypeId(0);
    std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator total1 =
        m_t.srcEventTotal.find(ttk);
    if (it1 != m_t.srcEventToTarget.end() && total1 != m_t.srcEventTotal.end() &&
        total1->second >= kSampleFloor)
    {
        m_lastLevelFired = 1;
        return std::log((f32)it1->second / (f32)total1->second);
    }

    // Level 2: P(t | s)
    PairKey p1; p1.a = src; p1.b = tgt; p1.extra = 0;
    std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it2 =
        m_t.srcToTarget.find(p1);
    std::unordered_map<TypeId, u32>::const_iterator total2 = m_t.srcTotal.find(src);
    if (it2 != m_t.srcToTarget.end() && total2 != m_t.srcTotal.end() &&
        total2->second >= kSampleFloor)
    {
        m_lastLevelFired = 2;
        return std::log(kBackoffDiscount * (f32)it2->second / (f32)total2->second);
    }

    // Level 3: P(t | e)
    PairKey p2; p2.a = TypeId(0); p2.b = tgt; p2.extra = evtHash;
    std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it3 =
        m_t.eventToTarget.find(p2);
    std::unordered_map<u32, u32>::const_iterator total3 = m_t.eventTotal.find(evtHash);
    if (it3 != m_t.eventToTarget.end() && total3 != m_t.eventTotal.end() &&
        total3->second >= kSampleFloor)
    {
        m_lastLevelFired = 3;
        return std::log(kBackoffDiscount * kBackoffDiscount *
                        (f32)it3->second / (f32)total3->second);
    }

    // Level 4: global cross-map fallback (a constant prior baked into
    // the corpus on load — we don't separate global vs local in this
    // file because GlobalCorpusBuilder folds global counts INTO the
    // same tables with extra weighting).
    m_lastLevelFired = 4;
    return -8.0f;
}

CorpusPriors::Stats CorpusPriors::stats() const {
    Stats s;
    s.tripleEntries    = (i32)m_t.srcEventToTarget.size();
    s.srcPairEntries   = (i32)m_t.srcToTarget.size();
    s.eventPairEntries = (i32)m_t.eventToTarget.size();
    s.totalObservations = 0;
    for (std::unordered_map<TypeId, u32>::const_iterator it = m_t.srcTotal.begin();
         it != m_t.srcTotal.end(); ++it) s.totalObservations += it->second;
    return s;
}

void* CorpusPriors::serialize(usize& outBytes) const {
    // Format: magic + version + countTriple + countPair + countPair2 +
    // counts of marginal totals; then flat arrays.
    usize headerBytes = sizeof(u32) * 8;
    usize tBytes = m_t.srcEventToTarget.size() * (sizeof(u32) * 4); // src,evtHash,tgt,count
    usize p1Bytes = m_t.srcToTarget.size() * (sizeof(u32) * 4);
    usize p2Bytes = m_t.eventToTarget.size() * (sizeof(u32) * 4);
    usize teBytes = m_t.srcEventTotal.size() * (sizeof(u32) * 3);
    usize stBytes = m_t.srcTotal.size() * (sizeof(u32) * 2);
    usize etBytes = m_t.eventTotal.size() * (sizeof(u32) * 2);
    usize total = headerBytes + tBytes + p1Bytes + p2Bytes + teBytes + stBytes + etBytes;
    u32* buf = (u32*)std::malloc(total);
    if (!buf) { outBytes = 0; return 0; }
    u32* p = buf;
    *p++ = kSerializeMagic;
    *p++ = kSerializeVersion;
    *p++ = (u32)m_t.srcEventToTarget.size();
    *p++ = (u32)m_t.srcToTarget.size();
    *p++ = (u32)m_t.eventToTarget.size();
    *p++ = (u32)m_t.srcEventTotal.size();
    *p++ = (u32)m_t.srcTotal.size();
    *p++ = (u32)m_t.eventTotal.size();
    for (std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator it = m_t.srcEventToTarget.begin();
         it != m_t.srcEventToTarget.end(); ++it) {
        *p++ = it->first.sourceType.raw;
        *p++ = it->first.eventHash;
        *p++ = it->first.targetType.raw;
        *p++ = it->second;
    }
    for (std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it = m_t.srcToTarget.begin();
         it != m_t.srcToTarget.end(); ++it) {
        *p++ = it->first.a.raw;
        *p++ = it->first.b.raw;
        *p++ = it->first.extra;
        *p++ = it->second;
    }
    for (std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it = m_t.eventToTarget.begin();
         it != m_t.eventToTarget.end(); ++it) {
        *p++ = it->first.a.raw;
        *p++ = it->first.b.raw;
        *p++ = it->first.extra;
        *p++ = it->second;
    }
    for (std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator it = m_t.srcEventTotal.begin();
         it != m_t.srcEventTotal.end(); ++it) {
        *p++ = it->first.sourceType.raw;
        *p++ = it->first.eventHash;
        *p++ = it->second;
    }
    for (std::unordered_map<TypeId, u32>::const_iterator it = m_t.srcTotal.begin();
         it != m_t.srcTotal.end(); ++it) {
        *p++ = it->first.raw;
        *p++ = it->second;
    }
    for (std::unordered_map<u32, u32>::const_iterator it = m_t.eventTotal.begin();
         it != m_t.eventTotal.end(); ++it) {
        *p++ = it->first;
        *p++ = it->second;
    }
    outBytes = total;
    return buf;
}

bool CorpusPriors::deserialize(const void* bytes, usize n) {
    if (!bytes || n < sizeof(u32) * 8) return false;
    const u32* p = (const u32*)bytes;
    if (*p++ != kSerializeMagic) return false;
    u32 version = *p++;
    if (version != kSerializeVersion) {
        Core::Logging::Warn("CorpusPriors: deserialize version mismatch (have %u, expected %u)",
            version, kSerializeVersion);
        return false;
    }
    clear();
    u32 cTrip = *p++; u32 cP1 = *p++; u32 cP2 = *p++;
    u32 cTE   = *p++; u32 cST = *p++; u32 cET = *p++;
    for (u32 i = 0; i < cTrip; ++i) {
        TripleKey k;
        k.sourceType = TypeId(*p++);
        k.eventHash  = *p++;
        k.targetType = TypeId(*p++);
        u32 v = *p++;
        m_t.srcEventToTarget[k] = v;
    }
    for (u32 i = 0; i < cP1; ++i) {
        PairKey k;
        k.a = TypeId(*p++); k.b = TypeId(*p++); k.extra = *p++;
        u32 v = *p++;
        m_t.srcToTarget[k] = v;
    }
    for (u32 i = 0; i < cP2; ++i) {
        PairKey k;
        k.a = TypeId(*p++); k.b = TypeId(*p++); k.extra = *p++;
        u32 v = *p++;
        m_t.eventToTarget[k] = v;
    }
    for (u32 i = 0; i < cTE; ++i) {
        TripleKey k;
        k.sourceType = TypeId(*p++);
        k.eventHash  = *p++;
        k.targetType = TypeId(0);
        u32 v = *p++;
        m_t.srcEventTotal[k] = v;
    }
    for (u32 i = 0; i < cST; ++i) {
        TypeId t = TypeId(*p++); u32 v = *p++;
        m_t.srcTotal[t] = v;
    }
    for (u32 i = 0; i < cET; ++i) {
        u32 t = *p++; u32 v = *p++;
        m_t.eventTotal[t] = v;
    }
    return true;
}

void CorpusPriors::mergeFrom(const PriorTables& o) {
    for (std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator it = o.srcEventToTarget.begin();
         it != o.srcEventToTarget.end(); ++it) m_t.srcEventToTarget[it->first] += it->second;
    for (std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it = o.srcToTarget.begin();
         it != o.srcToTarget.end(); ++it) m_t.srcToTarget[it->first] += it->second;
    for (std::unordered_map<PairKey, u32, PairKeyHash>::const_iterator it = o.eventToTarget.begin();
         it != o.eventToTarget.end(); ++it) m_t.eventToTarget[it->first] += it->second;
    for (std::unordered_map<TripleKey, u32, TripleKeyHash>::const_iterator it = o.srcEventTotal.begin();
         it != o.srcEventTotal.end(); ++it) m_t.srcEventTotal[it->first] += it->second;
    for (std::unordered_map<TypeId, u32>::const_iterator it = o.srcTotal.begin();
         it != o.srcTotal.end(); ++it) m_t.srcTotal[it->first] += it->second;
    for (std::unordered_map<u32, u32>::const_iterator it = o.eventTotal.begin();
         it != o.eventTotal.end(); ++it) m_t.eventTotal[it->first] += it->second;
}

} // namespace Corpus
} // namespace Vespucci
