#!/usr/bin/env python3
"""
Multi-core CPU Hash Cracker with optimized hash computation
Uses all available CPU cores with ThreadPoolExecutor
"""

import numpy as np
from concurrent.futures import ProcessPoolExecutor, as_completed
import multiprocessing as mp
import time
import ctypes
import os
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

# Character set
CHARSET = 'abcdefghijklmnopqrstuvwxyz0123456789_'
CHARSET_SIZE = 37

# Priority targets
TARGETS = {
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

TARGET_SET = set(TARGETS.keys())

def index_to_string(idx, length):
    """Convert numeric index to character string"""
    chars = []
    for _ in range(length):
        chars.append(CHARSET[idx % CHARSET_SIZE])
        idx //= CHARSET_SIZE
    return ''.join(reversed(chars))

def test_range(args):
    """Test a range of indices for the given pattern length"""
    start_idx, end_idx, length = args
    found = []
    
    for idx in range(start_idx, end_idx):
        pattern = index_to_string(idx, length)
        h = fnv1_hash(pattern)
        if h in TARGET_SET:
            found.append((h, pattern))
    
    return found, end_idx - start_idx

def run_multicore_attack(length=6, num_workers=None):
    """Run multi-core brute force attack"""
    if num_workers is None:
        num_workers = mp.cpu_count()
    
    total_patterns = CHARSET_SIZE ** length
    chunk_size = total_patterns // (num_workers * 10)  # 10 chunks per worker
    chunk_size = max(chunk_size, 100000)  # Minimum chunk size
    
    print(f"\n{'='*60}")
    print(f"Multi-Core Hash Cracker - {length}-char patterns")
    print(f"{'='*60}")
    print(f"CPU cores: {num_workers}")
    print(f"Targets: {len(TARGETS)} hashes")
    print(f"Pattern space: {CHARSET_SIZE}^{length} = {total_patterns:,}")
    print(f"Chunk size: {chunk_size:,}")
    print()
    
    # Create work chunks
    chunks = []
    for start in range(0, total_patterns, chunk_size):
        end = min(start + chunk_size, total_patterns)
        chunks.append((start, end, length))
    
    print(f"Total chunks: {len(chunks)}")
    
    found = {}
    tested = 0
    start_time = time.time()
    
    with ProcessPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(test_range, chunk): chunk for chunk in chunks}
        
        for future in as_completed(futures):
            results, count = future.result()
            tested += count
            
            for h, pattern in results:
                if h not in found:
                    found[h] = pattern
                    bank = TARGETS.get(h, 'Unknown')
                    print(f"\nCRACKED: 0x{h:08X} = {pattern} ({bank})")
            
            # Progress update
            elapsed = time.time() - start_time
            rate = tested / elapsed if elapsed > 0 else 0
            pct = 100 * tested / total_patterns
            print(f"\rProgress: {pct:.1f}% ({rate/1e6:.2f}M/s) Tested: {tested:,}", end='', flush=True)
    
    elapsed = time.time() - start_time
    print(f"\n\nCompleted {total_patterns:,} patterns in {elapsed:.1f}s")
    print(f"Rate: {total_patterns/elapsed/1e6:.2f}M/s")
    print(f"Found: {len(found)}/{len(TARGETS)}")
    
    return found

if __name__ == '__main__':
    import sys
    
    length = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    workers = int(sys.argv[2]) if len(sys.argv) > 2 else None
    
    print(f"Starting {length}-char brute force with {workers or mp.cpu_count()} workers...")
    found = run_multicore_attack(length=length, num_workers=workers)
    
    # Save results
    if found:
        print("\nSaving results...")
        with open('wwiseRE/multicore_results.txt', 'a') as f:
            for h, pattern in found.items():
                f.write(f"0x{h:08X},{pattern},{TARGETS.get(h, 'Unknown')}\n")
