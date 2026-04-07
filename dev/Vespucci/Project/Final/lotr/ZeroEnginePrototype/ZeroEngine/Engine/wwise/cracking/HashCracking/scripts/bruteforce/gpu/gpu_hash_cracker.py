#!/usr/bin/env python3
"""
GPU-Accelerated FNV-1 Hash Cracker for Wwise Event Names
Uses CUDA via Numba for parallel hash computation on RTX 5070
"""

import numpy as np
from numba import cuda, uint32, uint8
import time
import csv
import json
import math
import sys
from pathlib import Path

# Canonical Wwise hash constants (SDK v2.1.2821)
SCRIPT_ROOT = Path(__file__).resolve().parents[4]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import FNV_OFFSET as _FNV_OFFSET, FNV_PRIME as _FNV_PRIME  # type: ignore

FNV_OFFSET = np.uint32(_FNV_OFFSET)
FNV_PRIME = np.uint32(_FNV_PRIME)

# Character set: a-z, 0-9, _
CHARSET = b'abcdefghijklmnopqrstuvwxyz0123456789_'
CHARSET_SIZE = 37

@cuda.jit
def fnv1_hash_kernel(results, targets, num_targets, length, offset, chars):
    """CUDA kernel to compute FNV-1 hashes for character combinations"""
    idx = cuda.grid(1)
    
    if idx >= offset + (CHARSET_SIZE ** length):
        return
    
    # Convert index to character combination
    pattern = cuda.local.array(16, dtype=uint8)
    temp_idx = idx
    
    for i in range(length - 1, -1, -1):
        pattern[i] = chars[temp_idx % CHARSET_SIZE]
        temp_idx //= CHARSET_SIZE
    
    # Compute FNV-1 hash
    h = uint32(2166136261)
    for i in range(length):
        h = (h * uint32(16777619)) & uint32(0xFFFFFFFF)
        h = h ^ uint32(pattern[i])
    
    # Check against targets
    for t in range(num_targets):
        if h == targets[t]:
            # Found a match - store the index
            results[t] = idx

def index_to_string(idx, length):
    """Convert numeric index to character string"""
    chars = []
    for _ in range(length):
        chars.append(CHARSET[idx % CHARSET_SIZE])
        idx //= CHARSET_SIZE
    return bytes(reversed(chars)).decode('ascii')

def load_targets():
    """Load remaining uncracked event hashes"""
    # Priority targets from stubborn banks
    targets = {
        0xDD7978E6: 'Creatures',
        0xDCD9D5DD: 'SFXSiegeTower',
        0xDF91450F: 'SFXOliphant',
        0xD1E41CDA: 'SFXBalrog',
        0xA6D835D7: 'HeroSaruman',
        0xFF74FDE5: 'HeroGimli',
        0xEF688F80: 'HeroMouth',
        0x94BDA720: 'Level_Isengard',
        0xE234322F: 'Ambience',
        0x783CDC38: 'Ambience',
        0xB53A0D23: 'SFXBallista',
        0xD6454E24: 'SFXBallista',
        0x8DCE21D5: 'SFXBatteringRam',
        0x79D92FB7: 'SFXBatteringRam',
        0x0CCA70A9: 'SFXCatapult',
        0x4C480561: 'SFXCatapult',
        0x84405926: 'HeroIsildur',
        0x5BBF9654: 'HeroIsildur',
        0x2EB326D8: 'HeroIsildur',
        0xD9A5464C: 'HeroLegolas',
        0x214CA366: 'HeroLegolas',
    }
    return targets

def gpu_brute_force(length=8, batch_size=100_000_000):
    """Run GPU-accelerated brute force for given pattern length"""
    print(f"\n{'='*60}")
    print(f"GPU Hash Cracker - {length}-char patterns")
    print(f"{'='*60}")
    
    targets = load_targets()
    target_hashes = np.array(list(targets.keys()), dtype=np.uint32)
    target_banks = list(targets.values())
    
    print(f"Targets: {len(targets)} hashes")
    print(f"Pattern space: {CHARSET_SIZE}^{length} = {CHARSET_SIZE**length:,} combinations")
    print(f"Batch size: {batch_size:,}")
    
    # Prepare GPU arrays
    d_targets = cuda.to_device(target_hashes)
    d_chars = cuda.to_device(np.frombuffer(CHARSET, dtype=np.uint8))
    
    total_patterns = CHARSET_SIZE ** length
    found = {}
    
    threads_per_block = 256
    start_time = time.time()
    
    for offset in range(0, total_patterns, batch_size):
        batch_end = min(offset + batch_size, total_patterns)
        batch_count = batch_end - offset
        
        blocks = math.ceil(batch_count / threads_per_block)
        
        # Results array (-1 means not found)
        results = np.full(len(targets), -1, dtype=np.int64)
        d_results = cuda.to_device(results)
        
        # Launch kernel
        fnv1_hash_kernel[blocks, threads_per_block](
            d_results, d_targets, len(targets), length, offset, d_chars
        )
        
        # Check results
        results = d_results.copy_to_host()
        for i, idx in enumerate(results):
            if idx >= 0 and target_hashes[i] not in found:
                pattern = index_to_string(idx, length)
                found[target_hashes[i]] = pattern
                print(f"CRACKED: 0x{target_hashes[i]:08X} = {pattern} ({target_banks[i]})")
        
        # Progress
        elapsed = time.time() - start_time
        rate = (offset + batch_count) / elapsed if elapsed > 0 else 0
        pct = 100 * (offset + batch_count) / total_patterns
        print(f"\rProgress: {pct:.1f}% ({rate/1e9:.2f}B/s) Found: {len(found)}", end='', flush=True)
    
    print(f"\n\nCompleted in {elapsed:.1f}s")
    print(f"Found: {len(found)}/{len(targets)}")
    return found

if __name__ == '__main__':
    # Test with 6-char first to verify GPU works
    print("Testing GPU with 6-char patterns...")
    found = gpu_brute_force(length=6, batch_size=10_000_000)
    
    if len(found) == 0:
        print("\nNo 6-char matches. Trying 7-char...")
        found = gpu_brute_force(length=7, batch_size=50_000_000)
