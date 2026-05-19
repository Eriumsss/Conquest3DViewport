// ZETypeRegistry.cpp
// =============================================================================
// Hash-indexed type table. Records live in a flat vector backed by an
// arena; bucket array is open-addressing on TypeId (which is already a
// hashed name, no need to re-hash). Population happens in
// ZETypeRegistry_Builtins.cpp via init() -> RegisterAllBuiltins().
// =============================================================================
// Written by: Eriumsss

#include "ZETypeRegistry.h"

#include "../Core/Arena.h"
#include "../Core/Hash.h"
#include "../Core/Logging.h"
#include "../Core/StringPool.h"
#include "../Core/VespucciAssert.h"

#include <cstdlib>
#include <cstring>

namespace Vespucci {
namespace Schema {

// Forward decl from ZETypeRegistry_Builtins.cpp. We don't put it in
// the header because nothing else needs to call it - only init() does.
void RegisterAllBuiltins(ZETypeRegistry& reg);

// Forward decl of Pandemic CRC routine from Core/Crc32Tables.cpp.
namespace { extern "C" {} }
namespace Vespucci_Core_Crc { u32 PandemicCrcCStr(const char* s); }
// Actually it's in Vespucci::Core, not a separate namespace. Inline-link below.

namespace {
    static const i32 kInitialBuckets = 256;
    static ZETypeRegistry* s_global = 0;
} // namespace

ZETypeRegistry* GlobalRegistry()                  { return s_global; }
void            SetGlobalRegistry(ZETypeRegistry* r) { s_global = r; }

ZETypeRegistry::ZETypeRegistry()
    : m_buckets(0), m_bucketCount(0), m_bucketMask(0),
      m_records(0), m_recordCap(0), m_recordCount(0),
      m_arena(0), m_pool(0), m_collisions(0)
{
    m_arena = new Core::Arena(64 * 1024);
    m_pool  = new Core::StringPool();

    m_bucketCount = kInitialBuckets;
    m_bucketMask  = m_bucketCount - 1;
    usize bytes = sizeof(Bucket) * (size_t)m_bucketCount;
    m_buckets = (Bucket*)std::malloc(bytes);
    VESPUCCI_ASSERT(m_buckets != 0, "ZETypeRegistry initial bucket malloc failed");
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id        = TypeId(0);
        m_buckets[i].recordIdx = -1;
    }

    m_recordCap   = 256;
    m_records     = (TypeRecord*)std::malloc(sizeof(TypeRecord) * (size_t)m_recordCap);
    m_recordCount = 0;
}

ZETypeRegistry::~ZETypeRegistry() {
    if (m_buckets) std::free(m_buckets);
    if (m_records) std::free(m_records);
    delete (Core::Arena*)m_arena;
    delete (Core::StringPool*)m_pool;
}

bool ZETypeRegistry::init() {
    if (m_recordCount > 0) {
        // Already initialized. Idempotent path - log and bail.
        Core::Logging::Debug("ZETypeRegistry::init() called twice, ignoring");
        return true;
    }
    RegisterAllBuiltins(*this);
    Core::Logging::Info("ZETypeRegistry: %d types registered (%d buckets, %d collisions)",
        m_recordCount, m_bucketCount, m_collisions);
    return m_recordCount > 0;
}

i32 ZETypeRegistry::probe(TypeId id) const {
    // TypeId is already SplitMix-derived from the name's hash, so we
    // do NOT need to rehash here. Just mask and probe.
    i32 idx = (i32)(id.raw & (u32)m_bucketMask);
    while (m_buckets[idx].recordIdx != -1) {
        if (m_buckets[idx].id == id) return idx;
        idx = (idx + 1) & m_bucketMask;
        m_collisions++;
    }
    return idx;
}

void ZETypeRegistry::grow() {
    i32 oldCount = m_bucketCount;
    Bucket* old = m_buckets;

    m_bucketCount = oldCount * 2;
    m_bucketMask  = m_bucketCount - 1;
    m_buckets = (Bucket*)std::malloc(sizeof(Bucket) * (size_t)m_bucketCount);
    VESPUCCI_ASSERT(m_buckets != 0, "ZETypeRegistry grow malloc failed");
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id        = TypeId(0);
        m_buckets[i].recordIdx = -1;
    }

    for (i32 i = 0; i < oldCount; ++i) {
        if (old[i].recordIdx == -1) continue;
        i32 idx = (i32)(old[i].id.raw & (u32)m_bucketMask);
        while (m_buckets[idx].recordIdx != -1) {
            idx = (idx + 1) & m_bucketMask;
        }
        m_buckets[idx] = old[i];
    }
    std::free(old);
}

TypeId ZETypeRegistry::registerType(const char* displayName,
                                    const char* canonicalName,
                                    TypeId parentId,
                                    u32 traits,
                                    i32 crcMpeg,
                                    const char* description)
{
    if (!displayName || !canonicalName) {
        Core::Logging::Error("ZETypeRegistry::registerType called with null name");
        return TypeId(0);
    }

    // TypeId comes from canonical name (lowercase) so "CapturePoint"
    // and "capturepoint" hash identically. The displayName preserves
    // case for UI rendering.
    Core::StringPool* pool = (Core::StringPool*)m_pool;
    Core::StringRef canonical = pool->intern(canonicalName);
    Core::StringRef display   = pool->intern(displayName);
    Core::StringRef desc      = description ? pool->intern(description) : Core::StringRef("", 0);

    // Salt 0x7EE10000 (lowercase "zee"-ish, picked once and burned in).
    u32 hash = Core::XxHash32(canonical.data(), canonical.size(), 0x7EE10000u);
    TypeId id(hash);

    // Look up — return existing TypeId if already present (idempotent).
    i32 bucketIdx = probe(id);
    if (m_buckets[bucketIdx].recordIdx != -1) {
        return id;
    }

    // Grow the records array if needed.
    if (m_recordCount >= m_recordCap) {
        m_recordCap *= 2;
        m_records = (TypeRecord*)std::realloc(m_records,
                                              sizeof(TypeRecord) * (size_t)m_recordCap);
        VESPUCCI_ASSERT(m_records != 0, "ZETypeRegistry record realloc failed");
    }

    // Grow the bucket table if at 75% capacity.
    if ((m_recordCount + 1) * 4 > m_bucketCount * 3) {
        grow();
        bucketIdx = probe(id);
    }

    TypeRecord& rec    = m_records[m_recordCount];
    rec.id             = id;
    rec.name           = display;
    rec.canonicalName  = canonical;
    rec.parentId       = parentId;
    rec.traits         = traits;
    rec.crcMpeg        = crcMpeg;
    rec.description    = desc;

    m_buckets[bucketIdx].id        = id;
    m_buckets[bucketIdx].recordIdx = m_recordCount;
    m_recordCount++;
    return id;
}

const TypeRecord* ZETypeRegistry::findById(TypeId id) const {
    if (!id.valid()) return 0;
    i32 idx = probe(id);
    if (m_buckets[idx].recordIdx == -1) return 0;
    return &m_records[m_buckets[idx].recordIdx];
}

const TypeRecord* ZETypeRegistry::findByName(const char* displayName) const {
    if (!displayName || !*displayName) return 0;
    // Linear scan because we only ever go through this path on user
    // input where the display-name might differ in case from canonical.
    for (i32 i = 0; i < m_recordCount; ++i) {
        if (std::strcmp(m_records[i].name.data(), displayName) == 0) {
            return &m_records[i];
        }
    }
    // Fallback: try lowering and going through findByCanonical.
    char buf[128];
    usize n = std::strlen(displayName);
    if (n >= sizeof(buf)) return 0;
    for (usize i = 0; i < n; ++i) {
        char c = displayName[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        buf[i] = c;
    }
    buf[n] = 0;
    return findByCanonical(buf);
}

const TypeRecord* ZETypeRegistry::findByCanonical(const char* canonical) const {
    if (!canonical || !*canonical) return 0;
    // Same salt as registerType() — they MUST match or lookups fail silently.
    u32 hash = Core::XxHash32(canonical, std::strlen(canonical), 0x7EE10000u);
    return findById(TypeId(hash));
}

const TypeRecord* ZETypeRegistry::findByMpegCrc(i32 crc) const {
    for (i32 i = 0; i < m_recordCount; ++i) {
        if (m_records[i].crcMpeg == crc) return &m_records[i];
    }
    return 0;
}

i32                ZETypeRegistry::count() const { return m_recordCount; }
const TypeRecord*  ZETypeRegistry::at(i32 idx) const {
    if (idx < 0 || idx >= m_recordCount) return 0;
    return &m_records[idx];
}

bool ZETypeRegistry::hasTrait(TypeId id, TypeTrait t) const {
    const TypeRecord* r = findById(id);
    if (!r) return false;
    return (r->traits & (u32)t) != 0;
}

bool ZETypeRegistry::isSubtypeOf(TypeId descendant, TypeId ancestor) const {
    if (descendant == ancestor) return true;
    const TypeRecord* r = findById(descendant);
    i32 hops = 0;
    while (r && r->parentId.valid() && hops < 16) {
        if (r->parentId == ancestor) return true;
        r = findById(r->parentId);
        hops++;
    }
    return false;
}

void ZETypeRegistry::resetForTests() {
    for (i32 i = 0; i < m_bucketCount; ++i) {
        m_buckets[i].id        = TypeId(0);
        m_buckets[i].recordIdx = -1;
    }
    m_recordCount = 0;
    m_collisions  = 0;
    ((Core::Arena*)m_arena)->reset();
    ((Core::StringPool*)m_pool)->reset();
}

ZETypeRegistry::Stats ZETypeRegistry::stats() const {
    Stats s;
    s.typeCount      = m_recordCount;
    s.hashBuckets    = m_bucketCount;
    s.hashCollisions = m_collisions;
    return s;
}

} // namespace Schema
} // namespace Vespucci
