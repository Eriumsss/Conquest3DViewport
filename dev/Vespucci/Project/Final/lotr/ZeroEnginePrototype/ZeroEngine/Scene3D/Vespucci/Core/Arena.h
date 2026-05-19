// Arena.h
// =============================================================================
// ARENA — BUMP ALLOCATOR. ALLOCATE FAST, DEALLOCATE NEVER, RESET TOGETHER.
// =============================================================================
// Written by: Eriumsss
//
// Vespucci snapshots are built once per graph mutation and read
// thousands of times. The build phase wants fast allocation; the
// read phase doesn't allocate at all. Individual frees inside a
// snapshot's lifetime are pointless because the entire snapshot
// dies together when the next one swaps in.
//
// That's a textbook arena allocator. malloc a 64KB page, hand out
// bytes from it linearly, when full grab another page. Reset wipes
// every page in one stroke. Allocations are typically 5-20 ns each
// (one comparison + one pointer add) versus 100-500 ns for a real
// malloc. Multiplied by 10k allocations per snapshot, that's a
// difference between sub-millisecond rebuild and "the editor
// stutters every time you click a wire".
//
// Pages chain via a singly-linked list. Free() walks the list and
// frees each page; we cannot return individual allocations because
// we don't track them. That's the whole point.
//
// Alignment: 8-byte default, override per-allocation if needed.
// We round up the bump pointer; never assume malloc-level 16-byte
// SSE-friendly alignment without asking.
// =============================================================================

#ifndef VESPUCCI_CORE_ARENA_H_
#define VESPUCCI_CORE_ARENA_H_

#include "VespucciTypes.h"

namespace Vespucci {
namespace Core {

class Arena {
public:
    Arena();
    explicit Arena(usize firstPageBytes);
    ~Arena();

    // Allocate `n` bytes. Returns NULL on heap-out (we don't crash;
    // the caller checks). Alignment defaults to 8; override for SSE
    // (16) or cache-line (64) when the use case demands.
    void* alloc(usize n, usize align = 8);

    // Allocate and copy a string into the arena. Returns interned
    // pointer (StringPool calls this for its backing storage).
    char* copyString(const char* s, usize n);

    // Reset to empty. Frees all pages except the first (which we
    // keep for the next reset to amortize the page-malloc cost).
    void reset();

    // Stats for telemetry. Useful when the corpus build feels slow
    // and we want to know if it's allocator pressure or hashing.
    usize totalBytes()    const;     // sum of bytes used across all pages
    usize totalCapacity() const;     // sum of all page sizes
    i32   pageCount()     const;

private:
    struct Page {
        Page* next;
        u8*   data;
        usize size;
        usize used;
    };

    Page* allocPage(usize bytes);
    void  freeAllPages();

    Page* m_head;
    usize m_firstPageBytes;
    i32   m_pageCount;
};

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_ARENA_H_
