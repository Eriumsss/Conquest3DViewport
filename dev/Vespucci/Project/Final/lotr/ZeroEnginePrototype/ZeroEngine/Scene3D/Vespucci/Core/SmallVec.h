// SmallVec.h
// =============================================================================
// SMALLVEC — STACK-BUFFERED VECTOR FOR HOT PATHS, NO MALLOC, NO MERCY
// =============================================================================
// Written by: Eriumsss
//
// Every ranker call produces a candidate set. Per the doc lock, that
// set caps at 256 entities (Limits::kMaxCandidatesPerQuery). We hit
// this code path on every drag-frame, every selection change, every
// sidebar refresh - if it allocates on the heap we are paying for
// thousands of malloc/free pairs per second of editor use, and that's
// before the autocomplete popup adds its own per-keystroke pool churn.
//
// Solution: SmallVec<T, N> keeps the first N elements on the stack
// (or wherever the SmallVec lives) and only spills to heap when
// pushed past N. We size N to the realistic max for each call site:
// 256 for candidate sets, 32 for reason chips, 16 for spatial query
// hits, 8 for the assert ring's per-frame batch.
//
// Why not std::vector<T> with a reserve? Two reasons:
//   1. std::vector ALWAYS heap-allocates on first push, even if you
//      reserve(N) on a fresh vector. The "small buffer optimization"
//      isn't part of the standard, MSVC's std::vector doesn't have
//      it, and we are NOT shipping a custom allocator everywhere.
//   2. std::vector<T> at MSVC debug-iterator-level wraps every []
//      access in a fucking bounds-check that tanks perf 10x in
//      Debug builds. SmallVec ships with VESPUCCI_CHECK guards that
//      can be tuned independently.
//
// API mirrors std::vector for the operations we actually use:
// push_back, pop_back, size, capacity, reserve, clear, [], data,
// begin/end. NOT a drop-in replacement: no insert(), no erase()
// other than pop_back(), no resize() to shrink. If you find yourself
// reaching for those, you're using SmallVec wrong.
//
// Trivially-destructible T only (or you must clear() before going out
// of scope, which is fine, just don't rely on auto cleanup of fancy
// types). Vespucci internal types are POD; this constraint is free.
// =============================================================================

#ifndef VESPUCCI_CORE_SMALLVEC_H_
#define VESPUCCI_CORE_SMALLVEC_H_

#include "VespucciTypes.h"
#include "VespucciAssert.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace Vespucci {
namespace Core {

template <typename T, i32 InlineN>
class SmallVec {
public:
    SmallVec() : m_data(reinterpret_cast<T*>(m_inline)),
                 m_size(0),
                 m_cap(InlineN),
                 m_heap(false) {}

    ~SmallVec() {
        if (m_heap) std::free(m_data);
    }

    // Copy constructor — full deep copy. Heap-promoted result if the
    // source was heap, stack result if source was stack.
    SmallVec(const SmallVec& other)
        : m_data(reinterpret_cast<T*>(m_inline)),
          m_size(0),
          m_cap(InlineN),
          m_heap(false)
    {
        reserve(other.m_size);
        for (i32 i = 0; i < other.m_size; ++i) {
            new (&m_data[i]) T(other.m_data[i]);
        }
        m_size = other.m_size;
    }

    SmallVec& operator=(const SmallVec& other) {
        if (this == &other) return *this;
        clear();
        reserve(other.m_size);
        for (i32 i = 0; i < other.m_size; ++i) {
            new (&m_data[i]) T(other.m_data[i]);
        }
        m_size = other.m_size;
        return *this;
    }

    // Element access.
    T&       operator[](i32 i)       { VESPUCCI_CHECK(i >= 0 && i < m_size); return m_data[i]; }
    const T& operator[](i32 i) const { VESPUCCI_CHECK(i >= 0 && i < m_size); return m_data[i]; }

    T*       data()       { return m_data; }
    const T* data() const { return m_data; }

    i32  size()     const { return m_size; }
    i32  capacity() const { return m_cap; }
    bool empty()    const { return m_size == 0; }

    T&       front()       { VESPUCCI_CHECK(m_size > 0); return m_data[0]; }
    const T& front() const { VESPUCCI_CHECK(m_size > 0); return m_data[0]; }
    T&       back()        { VESPUCCI_CHECK(m_size > 0); return m_data[m_size - 1]; }
    const T& back()  const { VESPUCCI_CHECK(m_size > 0); return m_data[m_size - 1]; }

    T*       begin()       { return m_data; }
    const T* begin() const { return m_data; }
    T*       end()         { return m_data + m_size; }
    const T* end()   const { return m_data + m_size; }

    // push_back — value semantics. Promotes to heap on overflow.
    void push_back(const T& v) {
        if (m_size >= m_cap) grow();
        new (&m_data[m_size]) T(v);
        m_size++;
    }

    // Emplace overload, single-arg only because we don't need
    // the variadic perfect-forwarding gymnastics for our use cases.
    template <typename U>
    void emplace_back(const U& v) {
        if (m_size >= m_cap) grow();
        new (&m_data[m_size]) T(v);
        m_size++;
    }

    // Default-construct one slot at the back. Useful when the caller
    // wants to fill in fields directly to avoid a temporary.
    T& emplace_back_default() {
        if (m_size >= m_cap) grow();
        T* slot = &m_data[m_size];
        new (slot) T();
        m_size++;
        return *slot;
    }

    void pop_back() {
        VESPUCCI_CHECK(m_size > 0);
        m_size--;
        m_data[m_size].~T();
    }

    void clear() {
        for (i32 i = 0; i < m_size; ++i) m_data[i].~T();
        m_size = 0;
    }

    // Reserve at least `n` capacity. Promotes to heap if needed.
    // Does NOT shrink. Does NOT change size.
    void reserve(i32 n) {
        if (n <= m_cap) return;
        promote(n);
    }

    // Resize to exactly `n`. New slots are default-constructed.
    // Truncates if smaller. Use cautiously - clear+pushback is
    // usually clearer at the call site.
    void resize(i32 n) {
        while (m_size > n) pop_back();
        if (n > m_cap) reserve(n);
        while (m_size < n) {
            new (&m_data[m_size]) T();
            m_size++;
        }
    }

    // True if storage has been promoted to heap. Useful for
    // perf-watching callers who care.
    bool isHeap() const { return m_heap; }

private:
    void grow() {
        i32 next = m_cap * 2;
        if (next < InlineN + 1) next = InlineN + 1;
        promote(next);
    }

    void promote(i32 newCap) {
        T* fresh = (T*)std::malloc(sizeof(T) * (size_t)newCap);
        VESPUCCI_ASSERT(fresh != 0, "SmallVec malloc failed at cap=%d", newCap);
        // Move-construct old elements. T is required trivially-
        // copyable in our use cases so memcpy would also work, but
        // the placement-new path is correct for any well-behaved T.
        for (i32 i = 0; i < m_size; ++i) {
            new (&fresh[i]) T(m_data[i]);
            m_data[i].~T();
        }
        if (m_heap) std::free(m_data);
        m_data = fresh;
        m_cap  = newCap;
        m_heap = true;
    }

    // Inline storage. Aligned to T's alignment via alignas. We use
    // a raw byte buffer + placement-new instead of T inline[N]
    // because we DO NOT want T's constructor to fire for unused
    // slots - that's a real cost when T is non-trivial and we
    // typically push fewer than InlineN slots.
    alignas(T) u8 m_inline[sizeof(T) * (size_t)InlineN];
    T*  m_data;
    i32 m_size;
    i32 m_cap;
    bool m_heap;
};

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_SMALLVEC_H_
