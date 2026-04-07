#!/usr/bin/env python3
"""
Analyze vals that are shared between hex keys and readable keys
This might reveal the pattern
"""
import json
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

def hex_to_int(hex_str):
    """Convert '0x3E7CED61' to integer"""
    return int(hex_str, 16)

def fnv1_32(data_bytes: bytes) -> int:
    """SDK-accurate FNV-1 on ASCII (lowercasing inside helper)."""
    # Decode with latin-1 to preserve byte values then hash
    return fnv1_hash(data_bytes.decode('latin-1'))

def main():
    print("="*80)
    print("ANALYZING SHARED VALS BETWEEN HEX AND READABLE KEYS")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/3] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    readable_entries = [e for e in entries if not e['key'].startswith('0x')]
    
    print(f"  Hex entries: {len(hex_entries)}")
    print(f"  Readable entries: {len(readable_entries)}")
    
    # Group by val
    print("\n[2/3] Grouping by val...")
    val_to_keys = {}
    
    for entry in entries:
        val = entry['val']
        if val not in val_to_keys:
            val_to_keys[val] = {'hex': [], 'readable': []}
        
        if entry['key'].startswith('0x'):
            val_to_keys[val]['hex'].append(entry['key'])
        else:
            val_to_keys[val]['readable'].append(entry['key'])
    
    # Find vals with both hex and readable keys
    shared_vals = []
    for val, keys in val_to_keys.items():
        if keys['hex'] and keys['readable']:
            shared_vals.append((val, keys))
    
    print(f"  Vals with both hex and readable keys: {len(shared_vals)}")
    
    # Analyze the pattern
    print("\n[3/3] Analyzing pattern...")
    
    with open('shared_vals_analysis.txt', 'w', encoding='utf-8') as f:
        f.write("SHARED VALS ANALYSIS\n")
        f.write("="*80 + "\n\n")
        f.write("These vals have BOTH hex keys AND readable keys.\n")
        f.write("This might reveal what the hex keys represent.\n\n")
        f.write("="*80 + "\n\n")
        
        for val, keys in sorted(shared_vals, key=lambda x: x[1]['readable'][0]):
            f.write(f"Val: {val}\n")
            f.write(f"  Readable keys:\n")
            for key in keys['readable']:
                # Calculate FNV-1 hash
                hash_orig = fnv1_32(key.encode('ascii', errors='ignore'))
                hash_lower = fnv1_32(key.lower().encode('ascii', errors='ignore'))
                
                f.write(f"    {key}\n")
                f.write(f"      FNV-1: 0x{hash_orig:08X} (matches val: {hash_orig == val})\n")
                f.write(f"      FNV-1 (lower): 0x{hash_lower:08X} (matches val: {hash_lower == val})\n")
            
            f.write(f"  Hex keys:\n")
            for key in keys['hex']:
                hex_val = hex_to_int(key)
                f.write(f"    {key} (int: {hex_val})\n")
                
                # Check if hex key itself matches the val
                if hex_val == val:
                    f.write(f"      ✓ HEX KEY EQUALS VAL!\n")
            
            f.write("\n" + "-"*80 + "\n\n")
    
    print("[+] Report saved to shared_vals_analysis.txt")
    
    # Check if any hex keys equal their vals
    hex_equals_val = []
    for entry in hex_entries:
        hex_val = hex_to_int(entry['key'])
        if hex_val == entry['val']:
            hex_equals_val.append(entry)
    
    print(f"\n  Hex keys where hex == val: {len(hex_equals_val)}")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Found {len(shared_vals)} vals with both hex and readable keys")
    print(f"✓ Found {len(hex_equals_val)} hex keys where hex value == val")
    
    if len(hex_equals_val) > 0:
        print("\n✓ KEY INSIGHT: Some hex keys ARE the val itself!")
        print("  This means:")
        print("  - Hex keys are NOT hashes of string names")
        print("  - Hex keys are ALTERNATIVE IDENTIFIERS for the same event")
        print("  - Game code can reference events by:")
        print("    1. Readable name (hashed to val)")
        print("    2. Direct val (as hex key)")
        print("    3. Val itself")
    
    print(f"\nConclusion:")
    print(f"  The 4,615 hex keys are likely:")
    print(f"  - Direct event IDs (not hashed from strings)")
    print(f"  - Alternative ways to reference the same audio events")
    print(f"  - May not have 'original string names' at all")

if __name__ == '__main__':
    main()
