// StringPool.cpp
// =============================================================================
// Open-addressing intern table backed by an Arena. Linear probe, hash-
// inline. Grows at 75% load factor by doubling the bucket count and
// rehashing in place. Lookup is O(1) average; the worst case happens
// when a designer types ten thousand identically-prefixed entity names
// and we still have to scan the cluster, which is fine, that's like
// 10us once and we don't care.
// =============================================================================
// Written by: Eriumsss

#include "StringPool.h"
#include "Arena.h"
#include "VespucciAssert.h"
#include "Hash.h"

#include <cstdlib>
#include <cstring>

namespace Vespucci {
namespace Core {

namespace {
    static const i32 kInitialBuckets = 256;
    static const i32 kGrowNumerator   = 3; // 75% load → grow
    static const i32 kGrowDenominator = 4;

    inline u32 PoolHash(const char* s, usize n) {
        // Pool-specific salt so the bucket distribution does not
        // alias other XxHash users (the corpus, the autocomplete trie).
        return XxHash32(s, n, 0x5C0095u);
    }
} // namespace

StringPool::StringPool()
    : m_buckets(0),
      m_bucketCount(0),
      m_bucketMask(0),
      m_count(0),
      m_byteCount(0),
      m_collisions(0),
      m_arena(0)
{
    m_arena = new Arena(64 * 1024);
    m_bucketCount = kInitialBuckets;
    m_bucketMask  = m_bucketCount - 1;
    usize bytes = sizeof(Entry) * (size_t)m_bucketCount;
    m_buckets = (Entry*)std::malloc(bytes);
    VESPUCCI_ASSERT(m_buckets != 0, "StringPool initial bucket malloc failed");
    std::memset(m_buckets, 0, bytes);
}

StringPool::~StringPool() {
    if (m_buckets) std::free(m_buckets);
    delete m_arena;
}

i32 StringPool::probe(const char* s, usize n, u32 h) const {
    i32 idx = (i32)(h & (u32)m_bucketMask);
    while (m_buckets[idx].str != 0) {
        const Entry& e = m_buckets[idx];
        if (e.hash == h && e.length == (i32)n &&
            std::memcmp(e.str, s, n) == 0) {
            return idx; // hit
        }
        idx = (idx + 1) & m_bucketMask;
        m_collisions++;
    }
    return idx; // miss — return where it would go
}

StringRef StringPool::intern(const char* s) {
    if (!s) return StringRef();
    return intern(s, std::strlen(s));
}

StringRef StringPool::intern(const StringRef& s) {
    return intern(s.data(), s.size());
}

StringRef StringPool::intern(const char* s, usize n) {
    if (!s || n == 0) return StringRef("", 0);

    // Grow if past 75%.
    if ((m_count + 1) * kGrowDenominator > m_bucketCount * kGrowNumerator) {
        grow();
    }

    u32 h = PoolHash(s, n);
    i32 idx = probe(s, n, h);
    Entry& e = m_buckets[idx];
    if (e.str != 0) {
        return StringRef(e.str, (usize)e.length);
    }

    // Insert: copy bytes into arena, store pointer.
    char* copy = m_arena->copyString(s, n);
    if (!copy) {
        // Arena OOM. Return a fresh non-interned StringRef so the
        // caller does not crash; the string is gone the moment the
        // source buffer dies, but at least we did not lie with a
        // dangling pointer.
        return StringRef(s, n);
    }
    e.hash   = h;
    e.length = (i32)n;
    e.str    = copy;
    m_count++;
    m_byteCount += (i32)(n + 1);
    return StringRef(copy, n);
}

StringRef StringPool::find(const char* s) const {
    if (!s) return StringRef();
    return find(s, std::strlen(s));
}

StringRef StringPool::find(const char* s, usize n) const {
    if (!s || n == 0 || m_count == 0) return StringRef();
    u32 h = PoolHash(s, n);
    i32 idx = probe(s, n, h);
    if (m_buckets[idx].str == 0) return StringRef();
    return StringRef(m_buckets[idx].str, (usize)m_buckets[idx].length);
}

void StringPool::grow() {
    i32 oldCount = m_bucketCount;
    Entry* oldBuckets = m_buckets;

    m_bucketCount = oldCount * 2;
    m_bucketMask  = m_bucketCount - 1;
    usize bytes = sizeof(Entry) * (size_t)m_bucketCount;
    m_buckets = (Entry*)std::malloc(bytes);
    VESPUCCI_ASSERT(m_buckets != 0, "StringPool grow malloc failed (newCount=%d)", m_bucketCount);
    std::memset(m_buckets, 0, bytes);

    // Re-probe each old entry into the new table. Hashes are stored
    // inline so no recomputation needed.
    for (i32 i = 0; i < oldCount; ++i) {
        Entry& src = oldBuckets[i];
        if (src.str == 0) continue;
        i32 idx = (i32)(src.hash & (u32)m_bucketMask);
        while (m_buckets[idx].str != 0) {
            idx = (idx + 1) & m_bucketMask;
        }
        m_buckets[idx] = src;
    }
    std::free(oldBuckets);
}

i32 StringPool::entryCount() const { return m_count; }
i32 StringPool::byteCount()  const { return m_byteCount; }
i32 StringPool::collisions() const { return m_collisions; }

void StringPool::reset() {
    if (!m_buckets || !m_arena) return;
    // Wipe buckets, reset arena. Bucket array stays sized.
    std::memset(m_buckets, 0, sizeof(Entry) * (size_t)m_bucketCount);
    m_arena->reset();
    m_count      = 0;
    m_byteCount  = 0;
    m_collisions = 0;
}

} // namespace Core
} // namespace Vespucci
