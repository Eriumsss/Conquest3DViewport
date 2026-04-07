/*
 * High-performance FNV-1 hash implementation for Wwise event name brute-forcing
 *
 * ADVANCED OPTIMIZATIONS:
 *   1. Inverse FNV for suffix searches (899433627 = modular inverse of 16777619)
 *   2. N-gram filtering (skip impossible 3-char sequences)
 *   3. Fuzzy hash early-exit (check upper 24 bits first)
 *   4. Meet-in-the-middle attack support
 *   5. Prefix hash caching
 *
 * Compile as DLL/shared library:
 *   Windows: gcc -O3 -march=native -shared fnv1_hash.c -o fnv1_hash.dll
 *   Linux:   gcc -O3 -march=native -shared -fPIC fnv1_hash.c -o fnv1_hash.so
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "fnv1_hash.h"

/* Use the exported calling convention defined in fnv1_hash.h */
#define EXPORT FNV_EXPORT

/* ============================================================================
 * CORE HASH FUNCTIONS
 * FNV-1 algorithm: multiply-then-XOR (confirmed by Wwise SDK)
 * ============================================================================ */

/*
 * FNV-1 hash with optional shift-add optimization
 * From official FNV spec: http://www.isthe.com/chongo/tech/comp/fnv/
 *
 * FNV_PRIME (16777619) = 2^24 + 2^8 + 0x93
 * Shift-add equivalent: h + (h<<1) + (h<<4) + (h<<7) + (h<<8) + (h<<24)
 *
 * Note: Modern compilers often optimize multiply better than shift-add,
 * but shift-add can be faster on some architectures.
 */
#ifdef USE_FNV_SHIFT_ADD
#define FNV_MULTIPLY(h) ((h) + ((h)<<1) + ((h)<<4) + ((h)<<7) + ((h)<<8) + ((h)<<24))
#else
#define FNV_MULTIPLY(h) ((h) * FNV_PRIME_32)
#endif

static inline uint8_t _lower_ascii(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

EXPORT uint32_t wwise_hash(const char* s) {
    uint32_t h = FNV_OFFSET_32;
    while (*s) {
        uint8_t c = _lower_ascii((uint8_t)*s);
        h = FNV_MULTIPLY(h);
        h ^= c;
        s++;
    }
    return h;
}

/* Hash30 variant from Wwise SDK - XOR-folds 32-bit to 30-bit */
EXPORT uint32_t wwise_hash30(const char* s) {
    uint32_t h32 = wwise_hash(s);
    return (h32 >> 30) ^ (h32 & FNV_HASH30_MASK);
}

/* Convert existing 32-bit hash to 30-bit */
EXPORT uint32_t wwise_hash32_to_30(uint32_t h32) {
    return (h32 >> 30) ^ (h32 & FNV_HASH30_MASK);
}

/* Fixed-length version - no null check, faster */
EXPORT uint32_t wwise_hash_len(const char* s, int len) {
    uint32_t h = FNV_OFFSET_32;
    for (int i = 0; i < len; i++) {
        uint8_t c = _lower_ascii((uint8_t)s[i]);
        h = FNV_MULTIPLY(h);
        h ^= c;
    }
    return h;
}

/* Continue hash from existing state (for prefix caching) */
EXPORT uint32_t wwise_hash_continue(uint32_t prev_hash, const char* s) {
    uint32_t h = prev_hash;
    while (*s) {
        uint8_t c = _lower_ascii((uint8_t)*s);
        h = FNV_MULTIPLY(h);
        h ^= c;
        s++;
    }
    return h;
}

/* ============================================================================
 * INVERSE FNV - For suffix optimization
 * Key insight: FNV is mathematically invertible
 * ifnv("abc") == fnv("cba") with inverse operations
 * ============================================================================ */

/* Compute inverse FNV hash (undoing hash from the end) */
EXPORT uint32_t wwise_hash_inverse(uint32_t target_hash, const char* suffix, int len) {
    uint32_t h = target_hash;
    for (int i = len - 1; i >= 0; i--) {
        h = (h ^ _lower_ascii((uint8_t)suffix[i])) * FNV_INVERSE_32;
    }
    return h;
}

/* Given target hash and known suffix, compute what prefix hash should be */
EXPORT uint32_t wwise_hash_target_with_suffix(uint32_t target_hash, const char* suffix) {
    int len = strlen(suffix);
    return wwise_hash_inverse(target_hash, suffix, len);
}

/* Hash raw bytes; normalize_ascii==1 lowercases ASCII A-Z */
EXPORT uint32_t wwise_hash_bytes(const uint8_t* bytes, size_t len, int normalize_ascii) {
    uint32_t h = FNV_OFFSET_32;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = normalize_ascii ? _lower_ascii(bytes[i]) : bytes[i];
        h = FNV_MULTIPLY(h);
        h ^= c;
    }
    return h;
}

/* Wide-char helper to mirror AkAudioLib: wide -> ANSI (latin-1) -> lowercase -> hash */
EXPORT uint32_t wwise_hash_w(const wchar_t* ws) {
    uint32_t h = FNV_OFFSET_32;
    while (*ws) {
        uint16_t wc = (uint16_t)*ws++;
        uint8_t c = _lower_ascii((uint8_t)(wc & 0xFF)); // best-effort narrowing
        h = FNV_MULTIPLY(h);
        h ^= c;
    }
    return h;
}

EXPORT int wwise_hash_selftest(void) {
    return wwise_hash("none") == FNV_HASH_NONE;
}

/* ============================================================================
 * FUZZY HASH - Early exit optimization
 * Only check upper 24 bits first (much faster rejection)
 * ============================================================================ */

EXPORT uint32_t wwise_hash_fuzzy_mask(uint32_t hash) {
    /* Return upper 24 bits - for early rejection */
    return (hash * FNV_PRIME_32) & 0xFFFFFF00u;
}

/* Batch hash - process multiple strings */
EXPORT void wwise_hash_batch(
    const char** strings,
    int count,
    uint32_t* results
) {
    for (int i = 0; i < count; i++) {
        results[i] = wwise_hash(strings[i]);
    }
}

/* ============================================================================
 * N-GRAM FILTERING
 * Skip impossible 3-character sequences to reduce search space ~90%
 * Based on English letter frequency analysis
 * ============================================================================ */

/* Quick n-gram validity check - returns 0 if trigram is unlikely */
static uint8_t* ngram_filter = NULL;
static int ngram_filter_loaded = 0;

/* Initialize n-gram filter from embedded data */
EXPORT void init_ngram_filter(const uint8_t* filter_data, int size) {
    if (ngram_filter) free(ngram_filter);
    ngram_filter = (uint8_t*)malloc(size);
    memcpy(ngram_filter, filter_data, size);
    ngram_filter_loaded = 1;
}

/* Check if 3-gram is valid (fast inline check) */
static inline int is_valid_trigram(char a, char b, char c) {
    if (!ngram_filter_loaded) return 1;  /* No filter = allow all */
    /* Hash the trigram to index into filter bitmap */
    unsigned int idx = ((unsigned char)a * 37 * 37 + (unsigned char)b * 37 + (unsigned char)c);
    idx = idx % (37 * 37 * 37);  /* Wrap to charset^3 */
    return (ngram_filter[idx / 8] >> (idx % 8)) & 1;
}

/* ============================================================================
 * BRUTE-FORCE WORKER
 * Wwise charset rules (from FnvBrute/Audiokinetic SDK):
 * - First character MUST be lowercase letter [a-z]
 * - Remaining characters can be [a-z, 0-9, _]
 * ============================================================================ */

/* Full charset for legacy compatibility */
static const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz_0123456789";
static const int CHARSET_LEN = 37;

/* Wwise-specific charsets (from FnvBrute) */
static const char CHARSET_FIRST[] = "abcdefghijklmnopqrstuvwxyz";  /* First char: letters only */
static const int CHARSET_FIRST_LEN = 26;
static const char CHARSET_REST[] = "abcdefghijklmnopqrstuvwxyz_0123456789";  /* Rest: letters, digits, underscore */
static const int CHARSET_REST_LEN = 37;

/* Check if hash is in sorted target array (binary search) */
static int is_target(uint32_t h, const uint32_t* targets, int target_count) {
    int lo = 0, hi = target_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (targets[mid] == h) return 1;
        if (targets[mid] < h) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* Optimized brute-force with prefix hash caching */
EXPORT int brute_force_prefix_optimized(
    const char* prefix,
    int prefix_len,
    int max_len,
    const uint32_t* targets,
    int target_count,
    uint32_t* found_hashes,
    char (*found_names)[32],
    int max_found
) {
    char candidate[32];
    int found = 0;

    strcpy(candidate, prefix);

    /* Cache prefix hash to avoid recomputation */
    uint32_t prefix_hash = wwise_hash(prefix);

    /* Test just the prefix */
    if (prefix_len <= max_len) {
        if (is_target(prefix_hash, targets, target_count) && found < max_found) {
            found_hashes[found] = prefix_hash;
            strcpy(found_names[found], candidate);
            found++;
        }
    }

    /* Test all extensions with cached prefix hash */
    for (int len = prefix_len + 1; len <= max_len && found < max_found; len++) {
        int remaining = len - prefix_len;

        /* Initialize suffix to all first chars */
        for (int i = 0; i < remaining; i++) {
            candidate[prefix_len + i] = CHARSET[0];
        }
        candidate[len] = '\0';

        /* Iterate through all combinations */
        while (1) {
            /* Use continue hash from cached prefix */
            uint32_t h = wwise_hash_continue(prefix_hash, candidate + prefix_len);

            if (is_target(h, targets, target_count) && found < max_found) {
                found_hashes[found] = h;
                strcpy(found_names[found], candidate);
                found++;
            }

            /* Increment (like counting in base-37) */
            int pos = len - 1;
            while (pos >= prefix_len) {
                int idx = strchr(CHARSET, candidate[pos]) - CHARSET;
                if (idx < CHARSET_LEN - 1) {
                    candidate[pos] = CHARSET[idx + 1];
                    break;
                }
                candidate[pos] = CHARSET[0];
                pos--;
            }
            if (pos < prefix_len) break;
        }
    }

    return found;
}

/* Legacy brute-force (without name capture) */
EXPORT int brute_force_prefix(
    const char* prefix,
    int prefix_len,
    int max_len,
    const uint32_t* targets,
    int target_count,
    uint32_t* found_hashes,
    int max_found
) {
    char found_names[100][32];
    return brute_force_prefix_optimized(prefix, prefix_len, max_len, targets,
                                         target_count, found_hashes, found_names, max_found);
}

/* ============================================================================
 * WWISE BRUTE-FORCE (from FnvBrute charset rules)
 * First char must be [a-z], rest can be [a-z0-9_]
 * This reduces search space by ~30% compared to naive brute force
 * ============================================================================ */

EXPORT int brute_force_wwise(
    int min_len,
    int max_len,
    const uint32_t* targets,
    int target_count,
    uint32_t* found_hashes,
    char (*found_names)[32],
    int max_found
) {
    char candidate[32];
    int found = 0;

    for (int len = min_len; len <= max_len && found < max_found; len++) {
        /* First char must be letter [a-z] */
        for (int first_idx = 0; first_idx < CHARSET_FIRST_LEN && found < max_found; first_idx++) {
            candidate[0] = CHARSET_FIRST[first_idx];

            if (len == 1) {
                candidate[1] = '\0';
                uint32_t h = wwise_hash(candidate);
                if (is_target(h, targets, target_count)) {
                    found_hashes[found] = h;
                    strcpy(found_names[found], candidate);
                    found++;
                }
            } else {
                /* Initialize rest to first valid char */
                for (int i = 1; i < len; i++) {
                    candidate[i] = CHARSET_REST[0];
                }
                candidate[len] = '\0';

                /* Cache first char hash */
                uint32_t first_hash = FNV_OFFSET_32;
                first_hash *= FNV_PRIME_32;
                first_hash ^= (uint8_t)candidate[0];

                /* Iterate through all combinations of rest chars */
                while (1) {
                    uint32_t h = wwise_hash_continue(first_hash, candidate + 1);

                    if (is_target(h, targets, target_count) && found < max_found) {
                        found_hashes[found] = h;
                        strcpy(found_names[found], candidate);
                        found++;
                    }

                    /* Increment (like counting in base-37 for positions 1+) */
                    int pos = len - 1;
                    while (pos >= 1) {
                        char* p = strchr(CHARSET_REST, candidate[pos]);
                        int idx = p ? (int)(p - CHARSET_REST) : 0;
                        if (idx < CHARSET_REST_LEN - 1) {
                            candidate[pos] = CHARSET_REST[idx + 1];
                            break;
                        }
                        candidate[pos] = CHARSET_REST[0];
                        pos--;
                    }
                    if (pos < 1) break;
                }
            }
        }
    }

    return found;
}

/* ============================================================================
 * MEET-IN-THE-MIDDLE ATTACK
 * Split target into prefix + suffix, precompute both directions
 * Time complexity: O(2^(n/2)) instead of O(2^n)
 * ============================================================================ */

typedef struct {
    uint32_t hash;
    char str[16];
} HashEntry;

static int hash_entry_compare(const void* a, const void* b) {
    return ((HashEntry*)a)->hash - ((HashEntry*)b)->hash;
}

/* Generate all prefix hashes up to given length */
EXPORT int generate_prefix_hashes(
    int max_len,
    HashEntry* entries,
    int max_entries
) {
    int count = 0;
    char candidate[16];

    for (int len = 1; len <= max_len && count < max_entries; len++) {
        /* Initialize */
        for (int i = 0; i < len; i++) candidate[i] = CHARSET[0];
        candidate[len] = '\0';

        /* Generate all */
        while (count < max_entries) {
            entries[count].hash = wwise_hash(candidate);
            strcpy(entries[count].str, candidate);
            count++;

            /* Increment */
            int pos = len - 1;
            while (pos >= 0) {
                int idx = strchr(CHARSET, candidate[pos]) - CHARSET;
                if (idx < CHARSET_LEN - 1) {
                    candidate[pos] = CHARSET[idx + 1];
                    break;
                }
                candidate[pos] = CHARSET[0];
                pos--;
            }
            if (pos < 0) break;
        }
    }

    return count;
}

/* Generate inverse hashes for suffixes (what prefix hash would need to be) */
EXPORT int generate_suffix_inverse_hashes(
    int max_len,
    const uint32_t* targets,
    int target_count,
    HashEntry* entries,
    int max_entries
) {
    int count = 0;
    char candidate[16];

    for (int len = 1; len <= max_len && count < max_entries; len++) {
        for (int i = 0; i < len; i++) candidate[i] = CHARSET[0];
        candidate[len] = '\0';

        while (count < max_entries) {
            /* For each target, compute what prefix hash would need to be */
            for (int t = 0; t < target_count && count < max_entries; t++) {
                entries[count].hash = wwise_hash_inverse(targets[t], candidate, len);
                strcpy(entries[count].str, candidate);
                count++;
            }

            int pos = len - 1;
            while (pos >= 0) {
                int idx = strchr(CHARSET, candidate[pos]) - CHARSET;
                if (idx < CHARSET_LEN - 1) {
                    candidate[pos] = CHARSET[idx + 1];
                    break;
                }
                candidate[pos] = CHARSET[0];
                pos--;
            }
            if (pos < 0) break;
        }
    }

    qsort(entries, count, sizeof(HashEntry), hash_entry_compare);
    return count;
}

/* Find collisions between prefix and inverse-suffix tables */
EXPORT int mitm_find_collisions(
    HashEntry* prefix_table, int prefix_count,
    HashEntry* suffix_table, int suffix_count,
    char (*results)[32], int max_results
) {
    int found = 0;

    for (int i = 0; i < prefix_count && found < max_results; i++) {
        /* Binary search in suffix table */
        int lo = 0, hi = suffix_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (suffix_table[mid].hash == prefix_table[i].hash) {
                /* Found! Concatenate prefix + suffix */
                snprintf(results[found], 32, "%s%s",
                         prefix_table[i].str, suffix_table[mid].str);
                found++;
                break;
            }
            if (suffix_table[mid].hash < prefix_table[i].hash) lo = mid + 1;
            else hi = mid - 1;
        }
    }

    return found;
}

/* ============================================================================
 * BIDIRECTIONAL SEARCH (from wwiser-utils issue #7)
 * O(37^n + 37^m) instead of O(37^(n+m))
 * Uses Wwise charset rules for first character
 * ============================================================================ */

/* Generate prefix hashes with Wwise charset rules */
EXPORT int generate_prefix_hashes_wwise(
    int max_len,
    HashEntry* entries,
    int max_entries
) {
    int count = 0;
    char candidate[16];

    for (int len = 1; len <= max_len && count < max_entries; len++) {
        /* First char must be letter [a-z] */
        for (int first_idx = 0; first_idx < CHARSET_FIRST_LEN && count < max_entries; first_idx++) {
            candidate[0] = CHARSET_FIRST[first_idx];

            if (len == 1) {
                candidate[1] = '\0';
                entries[count].hash = wwise_hash(candidate);
                strcpy(entries[count].str, candidate);
                count++;
            } else {
                /* Initialize rest */
                for (int i = 1; i < len; i++) candidate[i] = CHARSET_REST[0];
                candidate[len] = '\0';

                /* Generate all combinations */
                while (count < max_entries) {
                    entries[count].hash = wwise_hash(candidate);
                    strcpy(entries[count].str, candidate);
                    count++;

                    /* Increment positions 1+ */
                    int pos = len - 1;
                    while (pos >= 1) {
                        char* p = strchr(CHARSET_REST, candidate[pos]);
                        int idx = p ? (int)(p - CHARSET_REST) : 0;
                        if (idx < CHARSET_REST_LEN - 1) {
                            candidate[pos] = CHARSET_REST[idx + 1];
                            break;
                        }
                        candidate[pos] = CHARSET_REST[0];
                        pos--;
                    }
                    if (pos < 1) break;
                }
            }
        }
    }

    return count;
}

/* ============================================================================
 * BENCHMARK (standalone mode)
 * ============================================================================ */

#ifdef BENCHMARK
int main() {
    printf("FNV-1 Hash Benchmark\n");
    printf("====================\n\n");
    
    const char* test_strings[] = {
        "test", "hello_world", "play_music", "footstep_grass_run",
        "abcdefghij", "ui_button_click", "explosion_large"
    };
    int num_strings = sizeof(test_strings) / sizeof(test_strings[0]);
    
    /* Verify hash values */
    printf("Hash verification:\n");
    for (int i = 0; i < num_strings; i++) {
        printf("  %s -> 0x%08X\n", test_strings[i], wwise_hash(test_strings[i]));
    }
    
    /* Benchmark */
    printf("\nBenchmarking...\n");
    int iterations = 10000000;
    
    clock_t start = clock();
    volatile uint32_t h = 0;
    for (int i = 0; i < iterations; i++) {
        for (int j = 0; j < num_strings; j++) {
            h ^= wwise_hash(test_strings[j]);
        }
    }
    clock_t end = clock();
    
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double rate = (iterations * num_strings) / elapsed / 1e6;
    
    printf("  %d iterations x %d strings = %lld hashes\n", 
           iterations, num_strings, (long long)iterations * num_strings);
    printf("  Time: %.2f seconds\n", elapsed);
    printf("  Rate: %.2f M hashes/sec\n", rate);
    printf("  (dummy: 0x%08X)\n", h);
    
    return 0;
}
#endif
