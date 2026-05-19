// Arena.cpp
// =============================================================================
// Bump allocator. Pages chain into a singly-linked list. Reset keeps the
// first page so the next snapshot rebuild does not pay malloc twice for
// the same goddamn 64KB block.
// =============================================================================
// Written by: Eriumsss

#include "Arena.h"
#include "VespucciAssert.h"

#include <cstdlib>
#include <cstring>

namespace Vespucci {
namespace Core {

namespace {
    static const usize kDefaultPageBytes = 64u * 1024u;
    static const usize kMinPageBytes     = 4u  * 1024u;

    inline usize AlignUp(usize value, usize align) {
        return (value + (align - 1)) & ~(align - 1);
    }
} // namespace

Arena::Arena()
    : m_head(0),
      m_firstPageBytes(kDefaultPageBytes),
      m_pageCount(0)
{
    // Pre-allocate the first page so the very first alloc call does
    // not stall on a malloc. Snapshots are built on the hot path.
    m_head = allocPage(m_firstPageBytes);
    if (m_head) m_pageCount = 1;
}

Arena::Arena(usize firstPageBytes)
    : m_head(0),
      m_firstPageBytes(firstPageBytes < kMinPageBytes ? kMinPageBytes : firstPageBytes),
      m_pageCount(0)
{
    m_head = allocPage(m_firstPageBytes);
    if (m_head) m_pageCount = 1;
}

Arena::~Arena() {
    freeAllPages();
}

void* Arena::alloc(usize n, usize align) {
    if (n == 0) return 0;
    if (align == 0) align = 1;
    // Power-of-two check — bad alignment is a programmer bug, scream.
    VESPUCCI_CHECK((align & (align - 1)) == 0);

    Page* p = m_head;
    if (p) {
        usize aligned = AlignUp(p->used, align);
        if (aligned + n <= p->size) {
            void* result = p->data + aligned;
            p->used = aligned + n;
            return result;
        }
    }

    // Need a new page. Pick max(default, requested + slack).
    usize wantBytes = n + align - 1;
    usize newPageBytes = (wantBytes > m_firstPageBytes) ? wantBytes : m_firstPageBytes;
    Page* fresh = allocPage(newPageBytes);
    if (!fresh) return 0; // OOM, caller decides
    fresh->next = m_head;
    m_head = fresh;
    m_pageCount++;

    usize aligned = AlignUp(fresh->used, align);
    void* result = fresh->data + aligned;
    fresh->used = aligned + n;
    return result;
}

char* Arena::copyString(const char* s, usize n) {
    if (!s) return 0;
    char* dst = (char*)alloc(n + 1, 1);
    if (!dst) return 0;
    if (n > 0) std::memcpy(dst, s, n);
    dst[n] = 0;
    return dst;
}

void Arena::reset() {
    if (!m_head) return;
    // Free everything except the head, then zero the head's used.
    Page* p = m_head->next;
    while (p) {
        Page* next = p->next;
        std::free(p->data);
        std::free(p);
        p = next;
    }
    m_head->next = 0;
    m_head->used = 0;
    m_pageCount  = 1;
}

usize Arena::totalBytes() const {
    usize sum = 0;
    for (Page* p = m_head; p; p = p->next) sum += p->used;
    return sum;
}

usize Arena::totalCapacity() const {
    usize sum = 0;
    for (Page* p = m_head; p; p = p->next) sum += p->size;
    return sum;
}

i32 Arena::pageCount() const { return m_pageCount; }

Arena::Page* Arena::allocPage(usize bytes) {
    Page* page = (Page*)std::malloc(sizeof(Page));
    if (!page) return 0;
    page->next = 0;
    page->size = bytes;
    page->used = 0;
    page->data = (u8*)std::malloc(bytes);
    if (!page->data) {
        std::free(page);
        return 0;
    }
    return page;
}

void Arena::freeAllPages() {
    Page* p = m_head;
    while (p) {
        Page* next = p->next;
        std::free(p->data);
        std::free(p);
        p = next;
    }
    m_head = 0;
    m_pageCount = 0;
}

} // namespace Core
} // namespace Vespucci
