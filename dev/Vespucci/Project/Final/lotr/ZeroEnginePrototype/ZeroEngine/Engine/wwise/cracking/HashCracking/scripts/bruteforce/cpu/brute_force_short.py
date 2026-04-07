#!/usr/bin/env python3
"""
Brute-force short Wwise event names using multiprocessing.
Tests all combinations of 1-6 character strings against known event IDs.
"""

import json
import itertools
import multiprocessing as mp
from functools import partial
import time

# Wwise uses FNV-1 hash with lowercase
def wwise_hash(s):
    h = 2166136261
    for c in s.lower():
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= ord(c)
    return h

# Character set for brute force
CHARSET = 'abcdefghijklmnopqrstuvwxyz_0123456789'

def process_chunk(args):
    """Process a chunk of strings and return matches."""
    prefix, max_len, known_ids = args
    matches = []
    
    # Generate all strings starting with this prefix
    for length in range(len(prefix), max_len + 1):
        remaining = length - len(prefix)
        if remaining == 0:
            h = wwise_hash(prefix)
            if h in known_ids:
                matches.append((prefix, h, known_ids[h]))
        else:
            for suffix in itertools.product(CHARSET, repeat=remaining):
                candidate = prefix + ''.join(suffix)
                h = wwise_hash(candidate)
                if h in known_ids:
                    matches.append((candidate, h, known_ids[h]))
    
    return matches

def main():
    print("Loading known event IDs...")
    with open('extracted_events.json', 'r') as f:
        data = json.load(f)
    known_ids = {int(k): v['bank'] for k, v in data['events'].items()}
    print(f"Loaded {len(known_ids)} known event IDs")
    
    # Load existing matches to avoid duplicates
    existing = set()
    try:
        with open('dictionary_matches.txt', 'r') as f:
            for line in f:
                if line.startswith('0x'):
                    parts = line.strip().split(',')
                    if len(parts) >= 2:
                        existing.add(parts[1].lower())
    except FileNotFoundError:
        pass
    print(f"Already have {len(existing)} matches")
    
    max_len = 6
    num_workers = mp.cpu_count()
    print(f"\nBrute-forcing 1-{max_len} char strings using {num_workers} workers...")
    print(f"Character set: {CHARSET} ({len(CHARSET)} chars)")
    
    # Calculate total combinations
    total = sum(len(CHARSET)**i for i in range(1, max_len + 1))
    print(f"Total combinations to test: {total:,}")
    
    # Create work chunks - each worker gets a 2-char prefix
    chunks = []
    for c1 in CHARSET:
        for c2 in CHARSET:
            chunks.append((c1 + c2, max_len, known_ids))
    # Also add single-char prefixes
    for c in CHARSET:
        chunks.append((c, 1, known_ids))  # Just test single chars
    
    print(f"Created {len(chunks)} work chunks")
    
    start_time = time.time()
    all_matches = []
    
    with mp.Pool(num_workers) as pool:
        for i, matches in enumerate(pool.imap_unordered(process_chunk, chunks)):
            all_matches.extend(matches)
            if (i + 1) % 100 == 0:
                elapsed = time.time() - start_time
                pct = (i + 1) / len(chunks) * 100
                print(f"  Progress: {pct:.1f}% ({i+1}/{len(chunks)}) - {len(all_matches)} matches - {elapsed:.1f}s")
    
    elapsed = time.time() - start_time
    print(f"\nCompleted in {elapsed:.1f} seconds")
    
    # Filter out existing matches
    new_matches = [(name, h, bank) for name, h, bank in all_matches 
                   if name.lower() not in existing]
    
    print(f"\n*** NEW MATCHES FOUND: {len(new_matches)} ***\n")
    
    # Sort and display
    new_matches.sort(key=lambda x: (x[2], x[0]))
    for name, h, bank in new_matches:
        print(f"  0x{h:08X} -> {name:20} [{bank}]")
    
    # Append to dictionary_matches.txt
    if new_matches:
        with open('dictionary_matches.txt', 'a') as f:
            f.write(f"\n# Brute-force matches (1-{max_len} chars)\n")
            for name, h, bank in new_matches:
                f.write(f"0x{h:08X},{name},{bank}\n")
        print(f"\nAppended {len(new_matches)} new matches to dictionary_matches.txt")

if __name__ == '__main__':
    mp.freeze_support()  # Required for Windows
    main()

