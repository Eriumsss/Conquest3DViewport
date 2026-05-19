// RingBuffer.h
// =============================================================================
// RINGBUFFER — FIXED-CAPACITY CIRCULAR BUFFER FOR EVENT LOGS AND HISTORIES
// =============================================================================
// Written by: Eriumsss
//
// Telemetry, the assert ring, the heat ring, the suggestion-history
// pane in the sidebar, and the autocomplete-recent-keys cache all
// have the same shape: fixed capacity, push at the head, drop the
// oldest when full, occasional read-from-tail walks. SmallVec doesn't
// fit because we don't want to grow; std::deque doesn't fit because
// it allocates per push and walks pointer chains. So we get this.
//
// Two modes:
//   - "drop oldest" (default): when full, push overwrites the oldest
//     entry. Used by event logs and assertion rings where we want
//     the last N regardless of how many came before.
//   - "drop newest" (constructor flag): when full, push silently
//     fails. Used by per-frame heat ring where we cap intake.
//
// Single-threaded by design. Multi-thread paths get their own ring
// per thread plus a merge pass; this file does not pretend to know
// about that.
// =============================================================================

#ifndef VESPUCCI_CORE_RINGBUFFER_H_
#define VESPUCCI_CORE_RINGBUFFER_H_

#include "VespucciTypes.h"
#include "VespucciAssert.h"

#include <cstring>
#include <new>

namespace Vespucci {
namespace Core {

template <typename T, i32 Cap>
class RingBuffer {
public:
    enum DropPolicy {
        DROP_OLDEST = 0,
        DROP_NEWEST = 1
    };

    RingBuffer(DropPolicy policy = DROP_OLDEST)
        : m_head(0), m_size(0), m_policy(policy) {}

    ~RingBuffer() { clear(); }

    // Push a value. Returns true if it was stored. Drop-oldest always
    // returns true (we evict an entry to make room). Drop-newest
    // returns false when full.
    bool push(const T& v) {
        if (m_size < Cap) {
            new (&m_buf[m_head]) T(v);
            m_head = (m_head + 1) % Cap;
            m_size++;
            return true;
        }
        if (m_policy == DROP_NEWEST) return false;
        // Drop oldest: head currently points at the oldest slot.
        m_buf[m_head].~T();
        new (&m_buf[m_head]) T(v);
        m_head = (m_head + 1) % Cap;
        return true;
    }

    // Pop oldest. Returns true if popped.
    bool pop_oldest(T& out) {
        if (m_size == 0) return false;
        i32 oldestIdx = (m_head - m_size + Cap) % Cap;
        out = m_buf[oldestIdx];
        m_buf[oldestIdx].~T();
        m_size--;
        return true;
    }

    // Read access ordered oldest-to-newest. Pointer/index stability
    // holds until the next push. cap must be >= size().
    i32 snapshot(T* out, i32 cap) const {
        i32 n = (m_size < cap) ? m_size : cap;
        i32 idx = (m_head - m_size + Cap) % Cap;
        for (i32 i = 0; i < n; ++i) {
            out[i] = m_buf[idx];
            idx = (idx + 1) % Cap;
        }
        return n;
    }

    // Index access: 0 = oldest, size()-1 = newest.
    const T& at(i32 i) const {
        VESPUCCI_CHECK(i >= 0 && i < m_size);
        return m_buf[(m_head - m_size + i + Cap) % Cap];
    }
    T& at(i32 i) {
        VESPUCCI_CHECK(i >= 0 && i < m_size);
        return m_buf[(m_head - m_size + i + Cap) % Cap];
    }

    // Newest entry shortcut. Use cautiously - if size==0 this asserts.
    const T& newest() const {
        VESPUCCI_CHECK(m_size > 0);
        return m_buf[(m_head + Cap - 1) % Cap];
    }
    const T& oldest() const {
        VESPUCCI_CHECK(m_size > 0);
        return m_buf[(m_head - m_size + Cap) % Cap];
    }

    void clear() {
        for (i32 i = 0; i < m_size; ++i) {
            i32 idx = (m_head - m_size + i + Cap) % Cap;
            m_buf[idx].~T();
        }
        m_size = 0;
        m_head = 0;
    }

    i32  size()     const { return m_size; }
    i32  capacity() const { return Cap; }
    bool empty()    const { return m_size == 0; }
    bool full()     const { return m_size == Cap; }

private:
    T   m_buf[Cap];
    i32 m_head;
    i32 m_size;
    DropPolicy m_policy;
};

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_RINGBUFFER_H_
