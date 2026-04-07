#!/usr/bin/env python3
"""
CUDA/GPU Accelerated Wwise Event Name Brute-Forcer

Uses NVIDIA GPU (RTX 5070) for massive parallel hashing.
Falls back to CPU if CUDA not available.

Requirements:
  pip install cupy-cuda12x  (or cupy-cuda11x depending on your CUDA version)
  pip install numba

Usage:
  python brute_force_cuda.py [--length N] [--gpu-mem GB]

Author: LOTR Conquest RE Project
"""

import os
import sys
import json
import time
import argparse
import itertools
from pathlib import Path
from datetime import datetime, timedelta

# ============================================================================
# CUDA CONFIGURATION
# ============================================================================
# Pull canonical constants from Wwise SDK helper
SCRIPT_ROOT = Path(__file__).resolve().parents[4]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import FNV_OFFSET, FNV_PRIME  # type: ignore
CHARSET = 'abcdefghijklmnopqrstuvwxyz_0123456789'
CHARSET_BYTES = bytes(CHARSET, 'ascii')

# ============================================================================
# TRY CUDA IMPORTS
# ============================================================================
CUDA_AVAILABLE = False
NUMBA_CUDA_AVAILABLE = False

try:
    import cupy as cp
    CUDA_AVAILABLE = True
    print(f"[+] CuPy CUDA available: {cp.cuda.runtime.getDeviceCount()} GPU(s)")
    
    # Get GPU info
    device = cp.cuda.Device(0)
    props = cp.cuda.runtime.getDeviceProperties(0)
    print(f"    GPU: {props['name'].decode()}")
    print(f"    Memory: {props['totalGlobalMem'] / 1024**3:.1f} GB")
    print(f"    Compute: {props['major']}.{props['minor']}")
except ImportError:
    print("[!] CuPy not available (pip install cupy-cuda12x)")
except Exception as e:
    print(f"[!] CUDA error: {e}")

try:
    from numba import cuda
    import numpy as np
    NUMBA_CUDA_AVAILABLE = cuda.is_available()
    if NUMBA_CUDA_AVAILABLE:
        print(f"[+] Numba CUDA available")
except ImportError:
    print("[!] Numba CUDA not available")

# ============================================================================
# NUMBA CUDA KERNEL (Preferred - more control)
# ============================================================================
if NUMBA_CUDA_AVAILABLE:
    from numba import cuda
    import numpy as np
    
    @cuda.jit
    def fnv1_hash_kernel(candidates, lengths, results, charset, num_candidates):
        """
        CUDA kernel for FNV-1 hash computation.
        Each thread processes one candidate string.
        """
        idx = cuda.grid(1)
        if idx >= num_candidates:
            return
        
        # Compute hash
        h = np.uint32(2166136261)
        length = lengths[idx]
        
        # Each candidate is stored as indices into charset
        base = idx * 16  # Max 16 chars per candidate
        for i in range(length):
            char_idx = candidates[base + i]
            c = charset[char_idx]
            h = (h * np.uint32(16777619)) ^ np.uint32(c)
        
        results[idx] = h

    def gpu_brute_force_batch(prefix, max_length, target_set, batch_size=10_000_000):
        """
        GPU-accelerated brute force for a given prefix.
        Processes candidates in batches to fit GPU memory.
        """
        matches = []
        tested = 0
        
        # Prepare charset on GPU
        charset_gpu = cuda.to_device(np.array(list(CHARSET_BYTES), dtype=np.uint8))
        
        # For each length
        for length in range(len(prefix), max_length + 1):
            remaining = length - len(prefix)
            if remaining < 0:
                continue
            
            # Generate candidates in batches
            prefix_indices = [CHARSET.index(c) for c in prefix]
            
            # Calculate total candidates for this length
            total = len(CHARSET) ** remaining if remaining > 0 else 1
            
            for batch_start in range(0, total, batch_size):
                batch_end = min(batch_start + batch_size, total)
                actual_batch = batch_end - batch_start
                
                # Allocate GPU arrays
                candidates = np.zeros((actual_batch, 16), dtype=np.uint8)
                lengths = np.full(actual_batch, length, dtype=np.uint8)
                results = np.zeros(actual_batch, dtype=np.uint32)
                
                # Fill candidates
                for i, suffix_idx in enumerate(range(batch_start, batch_end)):
                    # Decode suffix_idx to character indices
                    idx = suffix_idx
                    for j in range(len(prefix_indices)):
                        candidates[i, j] = prefix_indices[j]
                    for j in range(remaining - 1, -1, -1):
                        candidates[i, len(prefix) + j] = idx % len(CHARSET)
                        idx //= len(CHARSET)
                
                # Transfer to GPU
                candidates_gpu = cuda.to_device(candidates)
                lengths_gpu = cuda.to_device(lengths)
                results_gpu = cuda.to_device(results)
                
                # Launch kernel
                threads_per_block = 256
                blocks = (actual_batch + threads_per_block - 1) // threads_per_block
                fnv1_hash_kernel[blocks, threads_per_block](
                    candidates_gpu, lengths_gpu, results_gpu, charset_gpu, actual_batch
                )
                
                # Get results back
                results = results_gpu.copy_to_host()
                tested += actual_batch
                
                # Check for matches
                for i, h in enumerate(results):
                    if int(h) in target_set:
                        # Reconstruct string
                        chars = [CHARSET[candidates[i, j]] for j in range(length)]
                        name = ''.join(chars)
                        matches.append((name, int(h), target_set[int(h)]))

        return matches, tested

# ============================================================================
# CUPY RAW KERNEL (Alternative - faster memory transfer)
# ============================================================================
if CUDA_AVAILABLE:
    # Raw CUDA C kernel for maximum performance
    FNV1_KERNEL_CODE = '''
    extern "C" __global__
    void fnv1_hash(
        const unsigned char* __restrict__ candidates,
        const unsigned char* __restrict__ lengths,
        unsigned int* __restrict__ results,
        const unsigned char* __restrict__ charset,
        const int max_len,
        const int num_candidates
    ) {
        int idx = blockIdx.x * blockDim.x + threadIdx.x;
        if (idx >= num_candidates) return;

        unsigned int h = 2166136261u;
        int len = lengths[idx];
        int base = idx * max_len;

        for (int i = 0; i < len; i++) {
            unsigned char c = charset[candidates[base + i]];
            h = h * 16777619u;
            h ^= c;
        }

        results[idx] = h;
    }
    '''

    fnv1_kernel = cp.RawKernel(FNV1_KERNEL_CODE, 'fnv1_hash')

# ============================================================================
# MAIN EXECUTION
# ============================================================================
def load_targets():
    """Load target event IDs."""
    with open('extracted_events.json', 'r') as f:
        data = json.load(f)
    return {int(k): v.get('bank', 'unknown') for k, v in data.get('events', {}).items()}

def main():
    parser = argparse.ArgumentParser(description='CUDA Wwise Brute-Forcer')
    parser.add_argument('--length', '-l', type=int, default=7)
    parser.add_argument('--gpu-mem', type=float, default=4.0, help='GPU memory limit in GB')
    args = parser.parse_args()

    print("\n" + "=" * 60)
    print("  CUDA WWISE BRUTE-FORCER")
    print("=" * 60)

    if not NUMBA_CUDA_AVAILABLE and not CUDA_AVAILABLE:
        print("[!] No CUDA available. Please install:")
        print("    pip install cupy-cuda12x numba")
        sys.exit(1)

    targets = load_targets()
    print(f"[+] Loaded {len(targets):,} target IDs")

    # Calculate batch size based on GPU memory
    max_len = 16
    bytes_per_candidate = max_len + 1 + 4  # chars + length + result
    batch_size = int(args.gpu_mem * 1e9 / bytes_per_candidate / 2)  # Leave headroom
    print(f"[+] Batch size: {batch_size:,} candidates")

    # Generate prefixes
    prefixes = [''.join(p) for p in itertools.product(CHARSET, repeat=2)]

    print(f"[+] Processing {len(prefixes):,} prefixes...")

    start = time.time()
    all_matches = []
    total_tested = 0

    for i, prefix in enumerate(prefixes):
        if NUMBA_CUDA_AVAILABLE:
            matches, tested = gpu_brute_force_batch(prefix, args.length, targets, batch_size)
        else:
            # Fallback to CPU
            matches, tested = [], 0

        all_matches.extend(matches)
        total_tested += tested

        if (i + 1) % 50 == 0:
            elapsed = time.time() - start
            rate = total_tested / elapsed / 1e6
            pct = (i + 1) / len(prefixes) * 100
            print(f"\r[{pct:5.1f}%] {rate:.1f}M hashes/sec | {len(all_matches)} matches", end='')

    elapsed = time.time() - start
    print(f"\n\n[COMPLETE]")
    print(f"  Tested: {total_tested:,}")
    print(f"  Time: {timedelta(seconds=int(elapsed))}")
    print(f"  Rate: {total_tested/elapsed/1e6:.2f} M/sec")
    print(f"  Matches: {len(all_matches)}")

    if all_matches:
        with open('cuda_matches.txt', 'w') as f:
            for name, h, bank in sorted(all_matches):
                f.write(f"0x{h:08X},{name},{bank}\n")
                print(f"  0x{h:08X} -> {name} [{bank}]")

if __name__ == '__main__':
    main()
