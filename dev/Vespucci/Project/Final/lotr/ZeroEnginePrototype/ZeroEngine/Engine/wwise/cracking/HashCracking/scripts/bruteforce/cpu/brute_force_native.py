#!/usr/bin/env python3
"""
Native C-accelerated Wwise brute-forcer.

Uses compiled C library for maximum hash performance.
Falls back to Numba/Python if C library not available.

Compile the C library first:
  Windows (MSVC): cl /O2 /LD fnv1_hash.c /Fe:fnv1_hash.dll
  Windows (MinGW): gcc -O3 -shared fnv1_hash.c -o fnv1_hash.dll
  Linux: gcc -O3 -march=native -shared -fPIC fnv1_hash.c -o fnv1_hash.so

Usage:
  python brute_force_native.py [--length N] [--workers N]
"""

import os
import sys
import ctypes
import json
import time
import argparse
import itertools
import multiprocessing as mp
from pathlib import Path
from datetime import timedelta

# Shared Wwise hash helpers (matches SDK AkFNVHash)
SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import FNV_OFFSET, FNV_PRIME, fnv1_hash as py_fnv1_hash  # type: ignore

# ============================================================================
# LOAD NATIVE LIBRARY
# ============================================================================
NATIVE_AVAILABLE = False
native_lib = None

def load_native_lib():
    global native_lib, NATIVE_AVAILABLE
    
    lib_name = 'fnv1_hash.dll' if sys.platform == 'win32' else 'fnv1_hash.so'
    lib_path = Path(__file__).parent / lib_name
    
    if not lib_path.exists():
        print(f"[!] Native library not found: {lib_path}")
        print(f"    Compile with: cl /O2 /LD fnv1_hash.c /Fe:fnv1_hash.dll")
        return False
    
    try:
        native_lib = ctypes.CDLL(str(lib_path))
        
        # Setup function signatures
        native_lib.wwise_hash.argtypes = [ctypes.c_char_p]
        native_lib.wwise_hash.restype = ctypes.c_uint32
        
        native_lib.wwise_hash_len.argtypes = [ctypes.c_char_p, ctypes.c_int]
        native_lib.wwise_hash_len.restype = ctypes.c_uint32
        
        native_lib.brute_force_prefix.argtypes = [
            ctypes.c_char_p,  # prefix
            ctypes.c_int,     # prefix_len
            ctypes.c_int,     # max_len
            ctypes.POINTER(ctypes.c_uint32),  # targets
            ctypes.c_int,     # target_count
            ctypes.POINTER(ctypes.c_uint32),  # found_hashes
            ctypes.c_int      # max_found
        ]
        native_lib.brute_force_prefix.restype = ctypes.c_int
        
        NATIVE_AVAILABLE = True
        print(f"[+] Loaded native library: {lib_path}")
        return True
        
    except Exception as e:
        print(f"[!] Failed to load native library: {e}")
        return False

# ============================================================================
# CONFIGURATION
# ============================================================================
CHARSET = 'abcdefghijklmnopqrstuvwxyz_0123456789'
# Shared data for workers
TARGET_IDS = None
TARGET_ARRAY = None

def wwise_hash_python(s):
    return py_fnv1_hash(s)

# ============================================================================
# WORKER FUNCTION
# ============================================================================
def init_worker(target_ids, target_array_bytes):
    global TARGET_IDS, TARGET_ARRAY
    TARGET_IDS = target_ids
    # Reconstruct ctypes array from bytes
    arr_type = ctypes.c_uint32 * len(target_ids)
    TARGET_ARRAY = arr_type.from_buffer_copy(target_array_bytes)

def process_prefix_native(args):
    """Process prefix using native C library."""
    prefix, max_length = args
    
    if not NATIVE_AVAILABLE:
        return process_prefix_python(args)
    
    # Allocate result buffer
    max_found = 1000
    found_buffer = (ctypes.c_uint32 * max_found)()
    
    # Call native function
    found_count = native_lib.brute_force_prefix(
        prefix.encode('ascii'),
        len(prefix),
        max_length,
        TARGET_ARRAY,
        len(TARGET_IDS),
        found_buffer,
        max_found
    )
    
    # Collect matches
    matches = []
    for i in range(found_count):
        h = found_buffer[i]
        if h in TARGET_IDS:
            # We need to reconstruct the string - native lib doesn't return it
            # For now, mark as found and we'll reverse-lookup
            matches.append((None, h, TARGET_IDS[h]))
    
    # Calculate tested count
    tested = sum(len(CHARSET)**(l - len(prefix)) 
                 for l in range(len(prefix), max_length + 1))
    
    return prefix, matches, tested

def process_prefix_python(args):
    """Fallback Python implementation."""
    prefix, max_length = args
    matches = []
    tested = 0
    
    for length in range(len(prefix), max_length + 1):
        remaining = length - len(prefix)
        if remaining == 0:
            h = wwise_hash_python(prefix)
            tested += 1
            if h in TARGET_IDS:
                matches.append((prefix, h, TARGET_IDS[h]))
        else:
            for suffix in itertools.product(CHARSET, repeat=remaining):
                candidate = prefix + ''.join(suffix)
                h = wwise_hash_python(candidate)
                tested += 1
                if h in TARGET_IDS:
                    matches.append((candidate, h, TARGET_IDS[h]))
    
    return prefix, matches, tested

# ============================================================================
# MAIN EXECUTION
# ============================================================================
def main():
    parser = argparse.ArgumentParser(description='Native C-accelerated brute-forcer')
    parser.add_argument('--length', '-l', type=int, default=7)
    parser.add_argument('--workers', '-w', type=int, default=None)
    parser.add_argument('--benchmark', '-b', action='store_true')
    args = parser.parse_args()

    print("=" * 60)
    print("  NATIVE C WWISE BRUTE-FORCER")
    print("=" * 60)

    # Load native library
    load_native_lib()

    if args.benchmark:
        run_benchmark()
        return

    # Load targets
    with open('extracted_events.json', 'r') as f:
        data = json.load(f)
    target_ids = {int(k): v.get('bank', '?') for k, v in data.get('events', {}).items()}
    print(f"[+] Loaded {len(target_ids):,} targets")

    # Create sorted array for binary search
    sorted_ids = sorted(target_ids.keys())
    target_array = (ctypes.c_uint32 * len(sorted_ids))(*sorted_ids)
    target_array_bytes = bytes(target_array)

    # Generate prefixes
    prefix_len = 3
    prefixes = [''.join(p) for p in itertools.product(CHARSET, repeat=prefix_len)]

    num_workers = args.workers or mp.cpu_count()
    work_items = [(p, args.length) for p in prefixes]

    print(f"[+] Workers: {num_workers}, Prefixes: {len(prefixes):,}")
    print(f"[+] Native: {'YES' if NATIVE_AVAILABLE else 'NO (Python fallback)'}")
    print()

    start = time.time()
    all_matches = []
    total_tested = 0

    with mp.Pool(num_workers, initializer=init_worker,
                 initargs=(target_ids, target_array_bytes)) as pool:
        for i, (prefix, matches, tested) in enumerate(
            pool.imap_unordered(process_prefix_native, work_items, chunksize=10)
        ):
            all_matches.extend(matches)
            total_tested += tested

            if (i + 1) % 100 == 0:
                elapsed = time.time() - start
                rate = total_tested / elapsed / 1e6
                pct = (i + 1) / len(prefixes) * 100
                print(f"\r[{pct:5.1f}%] {rate:.2f}M/s | {len(all_matches)} matches", end='')

    elapsed = time.time() - start
    print(f"\n\n[COMPLETE]")
    print(f"  Tested: {total_tested:,}")
    print(f"  Time: {timedelta(seconds=int(elapsed))}")
    print(f"  Rate: {total_tested/elapsed/1e6:.2f} M/sec")
    print(f"  Matches: {len(all_matches)}")

def run_benchmark():
    """Benchmark native vs Python hash."""
    print("\n[BENCHMARK]")

    test_strings = ['test', 'hello_world', 'play_music_combat']
    iterations = 100000

    # Python
    start = time.time()
    for _ in range(iterations):
        for s in test_strings:
            wwise_hash_python(s)
    py_time = time.time() - start
    py_rate = (iterations * len(test_strings)) / py_time
    print(f"Python: {py_rate/1e6:.2f} M/s")

    # Native
    if NATIVE_AVAILABLE:
        start = time.time()
        for _ in range(iterations):
            for s in test_strings:
                native_lib.wwise_hash(s.encode('ascii'))
        native_time = time.time() - start
        native_rate = (iterations * len(test_strings)) / native_time
        print(f"Native: {native_rate/1e6:.2f} M/s ({native_rate/py_rate:.1f}x faster)")

if __name__ == '__main__':
    mp.freeze_support()
    main()
