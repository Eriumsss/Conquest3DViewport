// StringPool.h
// =============================================================================
// STRINGPOOL — INTERN ONCE, COMPARE BY POINTER FOREVER
// =============================================================================
// Written by: Eriumsss
//
// Vespucci will see the same event names ("OnTrigger", "OnDeath",
// "OnCapture") thousands of times across the corpus. Storing every
// occurrence as a separate std::string would waste memory and burn
// us on every comparison. Pool them once, compare pointers afterward.
//
// Lifetime: pool entries live as long as the pool. The corpus snapshot
// owns one pool; when the snapshot rolls, the old pool is dropped
// after the new one is built. StringRefs into the pool are stable for
// the snapshot version they were minted in, NOT across snapshot
// boundaries. The atomic-swap rebuild pattern handles this correctly.
//
// Implementation: open-addressing hash set keyed on (xxhash, length,
// content). Storage is a bump arena that grows in 64KB pages. Lookup
// is O(1) average. Insertion never realloc-moves an existing entry,
// so pointer stability is guaranteed for the pool's lifetime.
//
// Thread-safety: SINGLE-THREADED. Vespucci snapshot rebuild is
// single-threaded by design. If we ever go multi-threaded the pool
// becomes a flat-spinlock guarded shared structure or each builder
// thread gets its own pool and we merge. Don't pretend that path is
// free in this header.
// =============================================================================

#ifndef VESPUCCI_CORE_STRINGPOOL_H_
#define VESPUCCI_CORE_STRINGPOOL_H_

#include "VespucciTypes.h"
#include "StringRef.h"

namespace Vespucci {
namespace Core {

class Arena; // forward decl - StringPool uses Arena for backing storage

class StringPool {
public:
    StringPool();
    ~StringPool();

    // Intern a string. Returns a StringRef whose data() is stable
    // for the pool's lifetime. Repeated calls with identical content
    // return the same pointer.
    StringRef intern(const char* s);
    StringRef intern(const char* s, usize n);
    StringRef intern(const StringRef& s);

    // Lookup without insertion. Returns empty StringRef if not present.
    StringRef find(const char* s) const;
    StringRef find(const char* s, usize n) const;

    // Stats for telemetry / debug overlay.
    i32 entryCount()  const;
    i32 byteCount()   const; // total bytes interned (with NUL)
    i32 collisions()  const; // hash bucket collisions seen this pool

    // Reset all entries. Frees the backing arena. Use when a snapshot
    // dies and the new one starts fresh. Cheaper than per-entry frees.
    void reset();

private:
    // Open-addressing entry. We store the hash inline so that lookups
    // don't have to re-hash on every probe step.
    struct Entry {
        u32         hash;
        i32         length;
        const char* str;     // points into the arena
    };

    // Find the bucket for `s`. Returns the index where the entry
    // either lives or would live (m_buckets[idx].str == nullptr in
    // the second case). Records collision count via m_collisions.
    i32 probe(const char* s, usize n, u32 h) const;

    void grow();

    Entry* m_buckets;
    i32    m_bucketCount;   // power of two
    i32    m_bucketMask;    // m_bucketCount - 1
    i32    m_count;         // number of occupied buckets
    i32    m_byteCount;
    mutable i32 m_collisions;
    Arena* m_arena;
};

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_STRINGPOOL_H_
