#!/usr/bin/env python3
"""
Extract all possible event names from executable and test against hex keys
"""
import json
import re
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
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

def test_string(name_str, hex_lookup):
    """Test if a string matches any hex key"""
    # Try original case
    hash_val = fnv1_hash(name_str)
    
    if hash_val in hex_lookup:
        return ('original', hash_val, hex_lookup[hash_val])
    
    # Try lowercase
    hash_val_lower = fnv1_hash(name_str.lower())
    
    if hash_val_lower in hex_lookup:
        return ('lowercase', hash_val_lower, hex_lookup[hash_val_lower])
    
    return None

def main():
    print("="*80)
    print("EXTRACTING ALL EVENT NAMES FROM EXECUTABLE")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/4] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    print(f"  Hex key entries: {len(hex_entries)}")
    
    # Create lookup by hex value
    hex_lookup = {}
    for entry in hex_entries:
        hex_val = hex_to_int(entry['key'])
        if hex_val not in hex_lookup:
            hex_lookup[hex_val] = []
        hex_lookup[hex_val].append(entry)
    
    # Load executable
    print("\n[2/4] Loading executable and extracting strings...")
    with open('../ConquestLLC.exe', 'rb') as f:
        exe_data = f.read()
    
    # Extract all ASCII strings (min length 4)
    all_strings = re.findall(b'[ -~]{4,}', exe_data)
    all_strings = [s.decode('ascii', errors='ignore') for s in all_strings]
    print(f"  Total strings: {len(all_strings)}")
    
    # Filter for likely event names (alphanumeric + underscore)
    event_like = []
    for s in all_strings:
        # Must contain letters and be reasonable length
        if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]{2,50}$', s):
            event_like.append(s)
    
    event_like = list(set(event_like))
    print(f"  Event-like strings: {len(event_like)}")
    
    # Test all strings
    print("\n[3/4] Testing strings against hex keys...")
    matches = []
    
    for i, name in enumerate(event_like):
        if (i + 1) % 1000 == 0:
            print(f"    Tested {i+1}/{len(event_like)}...")
        
        result = test_string(name, hex_lookup)
        if result:
            case_type, hash_val, entries_list = result
            for entry in entries_list:
                matches.append({
                    'hex_key': entry['key'],
                    'val': entry['val'],
                    'original_name': name,
                    'case': case_type,
                    'hash_value': hash_val
                })
    
    print(f"  Found {len(matches)} matches!")
    
    # Generate report
    print("\n[4/4] Generating report...")
    
    with open('decoded_hex_keys_full.txt', 'w', encoding='utf-8') as f:
        f.write("DECODED HEX KEYS - FULL REPORT\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total hex keys: {len(hex_entries)}\n")
        f.write(f"Decoded: {len(matches)}\n")
        f.write(f"Remaining: {len(hex_entries) - len(matches)}\n\n")
        f.write("="*80 + "\n\n")
        
        # Group by original name
        by_name = {}
        for m in matches:
            name = m['original_name']
            if name not in by_name:
                by_name[name] = []
            by_name[name].append(m)
        
        for name in sorted(by_name.keys()):
            entries_list = by_name[name]
            f.write(f"Original Name: {name}\n")
            f.write(f"  Case: {entries_list[0]['case']}\n")
            f.write(f"  Hash: 0x{entries_list[0]['hash_value']:08X}\n")
            f.write(f"  Hex Keys:\n")
            for m in entries_list:
                f.write(f"    {m['hex_key']} → val: {m['val']}\n")
            f.write("\n")
    
    print("[+] Report saved to decoded_hex_keys_full.txt")
    
    # Also create a simple mapping file
    with open('hex_to_name_mapping.json', 'w', encoding='utf-8') as f:
        mapping = {}
        for m in matches:
            mapping[m['hex_key']] = {
                'name': m['original_name'],
                'val': m['val'],
                'case': m['case']
            }
        json.dump(mapping, f, indent=2)
    
    print("[+] Mapping saved to hex_to_name_mapping.json")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Extracted {len(all_strings)} strings from executable")
    print(f"✓ Filtered to {len(event_like)} event-like strings")
    print(f"✓ Successfully decoded {len(matches)} hex keys!")
    print(f"✓ Remaining undecoded: {len(hex_entries) - len(matches)}")
    
    if matches:
        print(f"\n✓ Decoded {len(set([m['original_name'] for m in matches]))} unique event names")
        
        # Show some examples
        print("\nExample decoded names:")
        for name in sorted(set([m['original_name'] for m in matches]))[:20]:
            print(f"  - {name}")

if __name__ == '__main__':
    main()
