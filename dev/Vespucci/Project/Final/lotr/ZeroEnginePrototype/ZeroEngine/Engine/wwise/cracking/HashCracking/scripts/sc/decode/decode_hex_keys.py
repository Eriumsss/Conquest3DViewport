#!/usr/bin/env python3
"""
Decode hex keys by testing common hash algorithms against known STID names
"""
import json
import struct
import zlib
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]  # .../scripts
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data.get('obj1s', [])

def load_stid_names(filename):
    """Load STID names from JSON"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data

def hex_to_int(hex_str):
    """Convert '0x3E7CED61' to integer"""
    return int(hex_str, 16)

# Common hash algorithms used by Wwise
def fnv1_32(data: bytes) -> int:
    """FNV-1 32-bit hash (SDK GetIDFromString-compatible)."""
    return fnv1_hash(data.decode('latin-1'))

def fnv1a_32(data: bytes) -> int:
    """FNV-1a 32-bit hash (kept for comparison)."""
    hash_val = 0x811C9DC5
    for byte in data:
        hash_val = (hash_val ^ byte) & 0xFFFFFFFF
        hash_val = (hash_val * 0x01000193) & 0xFFFFFFFF
    return hash_val

def crc32_hash(data):
    """CRC32 hash"""
    return zlib.crc32(data) & 0xFFFFFFFF

def djb2_hash(data):
    """DJB2 hash"""
    hash_val = 5381
    for byte in data:
        hash_val = ((hash_val * 33) + byte) & 0xFFFFFFFF
    return hash_val

def sdbm_hash(data):
    """SDBM hash"""
    hash_val = 0
    for byte in data:
        hash_val = (byte + (hash_val << 6) + (hash_val << 16) - hash_val) & 0xFFFFFFFF
    return hash_val

def test_hash_algorithms(name_str):
    """Test all hash algorithms on a string"""
    name_bytes = name_str.encode('ascii')
    name_bytes_lower = name_str.lower().encode('ascii')
    
    results = {
        'fnv1': fnv1_32(name_bytes),
        'fnv1a': fnv1a_32(name_bytes),
        'crc32': crc32_hash(name_bytes),
        'djb2': djb2_hash(name_bytes),
        'sdbm': sdbm_hash(name_bytes),
        'fnv1_lower': fnv1_32(name_bytes_lower),
        'fnv1a_lower': fnv1a_32(name_bytes_lower),
        'crc32_lower': crc32_hash(name_bytes_lower),
    }
    
    return results

def main():
    print("="*80)
    print("DECODING HEX KEYS - Finding Original String Names")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/5] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    print(f"  Hex key entries: {len(hex_entries)}")
    
    # Create lookup by hex value
    hex_to_entry = {}
    for entry in hex_entries:
        hex_val = hex_to_int(entry['key'])
        if hex_val not in hex_to_entry:
            hex_to_entry[hex_val] = []
        hex_to_entry[hex_val].append(entry)
    
    # Load STID names
    print("\n[2/5] Loading STID names...")
    stid_data = load_stid_names('stid_analysis.json')
    stid_names = list(set([entry['name'] for entry in stid_data]))
    print(f"  STID names: {len(stid_names)}")
    
    # Test hash algorithms on STID names
    print("\n[3/5] Testing hash algorithms on STID names...")
    matches = []
    
    for name in stid_names:
        hashes = test_hash_algorithms(name)
        
        for algo, hash_val in hashes.items():
            if hash_val in hex_to_entry:
                for entry in hex_to_entry[hash_val]:
                    matches.append({
                        'hex_key': entry['key'],
                        'val': entry['val'],
                        'original_name': name,
                        'algorithm': algo,
                        'hash_value': hash_val
                    })
    
    print(f"  Found {len(matches)} matches!")
    
    # Test on readable keys too (to confirm algorithm)
    print("\n[4/5] Testing on readable keys to confirm algorithm...")
    readable_entries = [e for e in entries if not e['key'].startswith('0x')]
    
    readable_matches = []
    for entry in readable_entries:
        name = entry['key']
        hashes = test_hash_algorithms(name)
        
        # Check if any hash matches the val
        for algo, hash_val in hashes.items():
            if hash_val == entry['val']:
                readable_matches.append({
                    'name': name,
                    'val': entry['val'],
                    'algorithm': algo,
                    'hash_value': hash_val
                })
    
    print(f"  Readable key matches: {len(readable_matches)}")
    
    if readable_matches:
        print("\n  ✓ ALGORITHM CONFIRMED!")
        algo_counts = {}
        for m in readable_matches:
            algo = m['algorithm']
            algo_counts[algo] = algo_counts.get(algo, 0) + 1
        
        print("  Algorithm distribution:")
        for algo, count in sorted(algo_counts.items(), key=lambda x: x[1], reverse=True):
            print(f"    {algo}: {count} matches")
    
    # Generate report
    print("\n[5/5] Generating report...")
    
    with open('hex_key_decoded.txt', 'w', encoding='utf-8') as f:
        f.write("HEX KEY DECODING REPORT\n")
        f.write("="*80 + "\n\n")
        
        if readable_matches:
            f.write("ALGORITHM CONFIRMATION (Readable Keys)\n")
            f.write("-"*80 + "\n")
            f.write("These readable keys confirm which hash algorithm is used:\n\n")
            
            for m in readable_matches[:20]:
                f.write(f"Name: {m['name']}\n")
                f.write(f"  Val: {m['val']}\n")
                f.write(f"  Algorithm: {m['algorithm']}\n")
                f.write(f"  Hash: 0x{m['hash_value']:08X}\n\n")
        
        f.write("\n" + "="*80 + "\n\n")
        f.write("DECODED HEX KEYS (STID Names)\n")
        f.write("-"*80 + "\n")
        f.write(f"Total matches: {len(matches)}\n\n")
        
        for m in sorted(matches, key=lambda x: x['original_name']):
            f.write(f"Hex Key: {m['hex_key']}\n")
            f.write(f"  Original Name: {m['original_name']}\n")
            f.write(f"  Val: {m['val']}\n")
            f.write(f"  Algorithm: {m['algorithm']}\n\n")
    
    print("[+] Report saved to hex_key_decoded.txt")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Tested {len(stid_names)} STID names against {len(hex_entries)} hex keys")
    print(f"✓ Found {len(matches)} matches using hash algorithms")
    print(f"✓ Confirmed algorithm using {len(readable_matches)} readable keys")
    
    if matches:
        print(f"\n✓ Successfully decoded {len(matches)} hex keys!")
        print(f"  Remaining undecoded: {len(hex_entries) - len(matches)}")
    else:
        print("\n⚠ No matches found - need to try different approach")
        print("  Possible reasons:")
        print("  - Different hash algorithm")
        print("  - String preprocessing (case, prefix, suffix)")
        print("  - Hex keys reference different strings not in STID")

if __name__ == '__main__':
    main()
