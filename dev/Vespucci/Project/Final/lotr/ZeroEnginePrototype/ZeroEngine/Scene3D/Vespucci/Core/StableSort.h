// StableSort.h
// =============================================================================
// STABLESORT — TOP-K AND BRANCHLESS-INSERTION FOR THE CANDIDATE PIPELINE
// =============================================================================
// Written by: Eriumsss
//
// The ranker outputs a candidate set of up to 256 entries and we want
// the top 5. std::sort + truncate is O(N log N), pointless waste -
// we just want a partial sort. std::partial_sort exists but pulls in
// the whole <algorithm> machinery and the MSVC debug-iterator overhead
// makes it slower than a hand-written heap-based top-K in Debug
// builds where everything inside Vespucci has to stay tight.
//
// Three primitives:
//
//   TopK<T, K, Less>(in[], n, out[])
//     Streaming top-K using a min-heap of K. O(N log K). For K << N,
//     way faster than full sort. Used by Suggest/CandidateGenerator
//     to feed at most 5 to the UI.
//
//   StableInsertionSort(arr[], n, less)
//     Branchless-friendly insertion sort. Stable. Best when n < 32.
//     Used by reason-chip rerank and small candidate lists where
//     stability matters (we want consistent output for identical
//     scores, otherwise the sidebar flickers between snapshots).
//
//   PartialSort(arr[], n, k, less)
//     Heap-based partial sort. After return, arr[0..k-1] is the top-K
//     in sorted order; arr[k..n-1] is unspecified. Slightly slower
//     than TopK for streaming use but saves the output array when
//     the input is already a SmallVec.
//
// All three take a Less callable. We do NOT templatize on iterator -
// pointers and SmallVec::data() are good enough.
// =============================================================================

#ifndef VESPUCCI_CORE_STABLESORT_H_
#define VESPUCCI_CORE_STABLESORT_H_

#include "VespucciTypes.h"
#include "VespucciAssert.h"

namespace Vespucci {
namespace Core {

// Default-less: a < b. Provided so callers don't have to write the
// trivial lambda themselves.
template <typename T>
struct DefaultLess {
    bool operator()(const T& a, const T& b) const { return a < b; }
};

// Insertion sort. Stable. O(n^2). Use for n < 32.
template <typename T, typename Less>
void StableInsertionSort(T* arr, i32 n, Less less) {
    for (i32 i = 1; i < n; ++i) {
        T   key = arr[i];
        i32 j   = i - 1;
        while (j >= 0 && less(key, arr[j])) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

template <typename T>
void StableInsertionSort(T* arr, i32 n) {
    StableInsertionSort(arr, n, DefaultLess<T>());
}

// Min-heap helpers. Internal but exposed in the header because we
// inline the sift-down for better codegen.
namespace SortDetail {
    template <typename T, typename Less>
    inline void SiftDown(T* h, i32 size, i32 i, Less less) {
        i32 cur = i;
        for (;;) {
            i32 l = cur * 2 + 1;
            i32 r = l + 1;
            i32 smallest = cur;
            if (l < size && less(h[l], h[smallest])) smallest = l;
            if (r < size && less(h[r], h[smallest])) smallest = r;
            if (smallest == cur) break;
            T tmp = h[cur]; h[cur] = h[smallest]; h[smallest] = tmp;
            cur = smallest;
        }
    }

    template <typename T, typename Less>
    inline void HeapifyMin(T* h, i32 n, Less less) {
        for (i32 i = n / 2 - 1; i >= 0; --i) {
            SiftDown(h, n, i, less);
        }
    }
} // namespace SortDetail

// TopK: select the k LARGEST elements per `less` (i.e. top-K when
// less is "ascending"). Output ordering is descending. Returns the
// actual count written (min(n, k)).
template <typename T, typename Less>
i32 TopK(const T* in, i32 n, i32 k, T* out, Less less) {
    if (k <= 0 || n <= 0) return 0;
    if (k > n) k = n;

    // Fill the heap with the first k elements, treating the heap as
    // a min-heap on `less` (smallest at root). Then walk the rest:
    // if a candidate beats the root, replace and sift down.
    for (i32 i = 0; i < k; ++i) out[i] = in[i];
    SortDetail::HeapifyMin(out, k, less);

    for (i32 i = k; i < n; ++i) {
        if (less(out[0], in[i])) {
            out[0] = in[i];
            SortDetail::SiftDown(out, k, 0, less);
        }
    }

    // Heapsort the heap to produce descending order.
    for (i32 sz = k; sz > 1; --sz) {
        T tmp = out[0]; out[0] = out[sz - 1]; out[sz - 1] = tmp;
        SortDetail::SiftDown(out, sz - 1, 0, less);
    }
    return k;
}

template <typename T>
i32 TopK(const T* in, i32 n, i32 k, T* out) {
    return TopK(in, n, k, out, DefaultLess<T>());
}

// PartialSort: in-place. After return, arr[0..k-1] is the top-K sorted
// descending per `less`. arr[k..n-1] is unspecified. Useful when the
// input is already a buffer you're willing to scramble.
template <typename T, typename Less>
i32 PartialSort(T* arr, i32 n, i32 k, Less less) {
    if (k <= 0 || n <= 0) return 0;
    if (k > n) k = n;

    SortDetail::HeapifyMin(arr, k, less);
    for (i32 i = k; i < n; ++i) {
        if (less(arr[0], arr[i])) {
            T tmp = arr[0]; arr[0] = arr[i]; arr[i] = tmp;
            SortDetail::SiftDown(arr, k, 0, less);
        }
    }
    for (i32 sz = k; sz > 1; --sz) {
        T tmp = arr[0]; arr[0] = arr[sz - 1]; arr[sz - 1] = tmp;
        SortDetail::SiftDown(arr, sz - 1, 0, less);
    }
    return k;
}

template <typename T>
i32 PartialSort(T* arr, i32 n, i32 k) {
    return PartialSort(arr, n, k, DefaultLess<T>());
}

} // namespace Core
} // namespace Vespucci

#endif // VESPUCCI_CORE_STABLESORT_H_
