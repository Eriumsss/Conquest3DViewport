// Fast FNV-1 hash brute force in C
// Compile: cl /O2 brute_fnv1.c /Fe:brute_fnv1.exe
// Or: gcc -O3 -march=native brute_fnv1.c -o brute_fnv1.exe

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "fnv1_hash.h"

#define CHARSET_SIZE 37
#define NUM_TARGETS 21

static const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789_";

static const uint32_t TARGETS[NUM_TARGETS] = {
    0xDD7978E6, 0xDCD9D5DD, 0xDF91450F, 0xD1E41CDA,
    0xA6D835D7, 0xFF74FDE5, 0xEF688F80, 0x94BDA720,
    0xE234322F, 0x783CDC38, 0xB53A0D23, 0xD6454E24,
    0x8DCE21D5, 0x79D92FB7, 0x0CCA70A9, 0x4C480561,
    0x84405926, 0x5BBF9654, 0x2EB326D8, 0xD9A5464C, 0x214CA366
};

static inline int check_target(uint32_t h) {
    for (int i = 0; i < NUM_TARGETS; i++) {
        if (TARGETS[i] == h) return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int len = 7;  // Default to 7 chars
    if (argc > 1) len = atoi(argv[1]);
    
    printf("Brute forcing %d-char patterns...\n", len);
    printf("Charset size: %d, Total patterns: ", CHARSET_SIZE);
    
    uint64_t total = 1;
    for (int i = 0; i < len; i++) total *= CHARSET_SIZE;
    printf("%llu\n", total);
    
    char pattern[32] = {0};
    int indices[32] = {0};
    
    // Initialize pattern
    for (int i = 0; i < len; i++) {
        pattern[i] = CHARSET[0];
    }
    
    clock_t start = clock();
    uint64_t count = 0;
    int found = 0;
    
    while (1) {
        uint32_t h = wwise_hash_len(pattern, len);
        if (check_target(h)) {
            printf("MATCH: 0x%08X = %s\n", h, pattern);
            found++;
        }
        count++;
        
        // Progress every 100M
        if (count % 100000000 == 0) {
            double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
            double rate = count / elapsed / 1e6;
            double pct = 100.0 * count / total;
            printf("Progress: %.1f%% (%.2fM/s) found=%d\n", pct, rate, found);
            fflush(stdout);
        }
        
        // Increment pattern (like counting in base 37)
        int pos = len - 1;
        while (pos >= 0) {
            indices[pos]++;
            if (indices[pos] < CHARSET_SIZE) {
                pattern[pos] = CHARSET[indices[pos]];
                break;
            }
            indices[pos] = 0;
            pattern[pos] = CHARSET[0];
            pos--;
        }
        if (pos < 0) break;  // All done
    }
    
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("\nCompleted %llu patterns in %.1fs (%.2fM/s)\n", count, elapsed, count/elapsed/1e6);
    printf("Found: %d/%d\n", found, NUM_TARGETS);
    
    return 0;
}
