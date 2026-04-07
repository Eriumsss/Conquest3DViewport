#!/usr/bin/env python3
"""
Final comprehensive attempt to decode hex keys
Try all possible string sources and transformations
"""
import json
import re
import glob
import struct
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

def fnv1_32(data: bytes) -> int:
    """FNV-1 32-bit hash (SDK accurate)."""
    return fnv1_hash(data.decode('latin-1'))

def test_all_variations(name_str, target_hash):
    """Test all possible variations of a string"""
    variations = [
        name_str,
        name_str.lower(),
        name_str.upper(),
        name_str.replace('_', ''),
        name_str.replace('_', ' '),
        name_str.replace(' ', '_'),
        name_str.replace('-', '_'),
        'Play_' + name_str,
        'play_' + name_str,
        'Stop_' + name_str,
        'stop_' + name_str,
        'Set_' + name_str,
        'set_' + name_str,
    ]
    
    for var in variations:
        h = fnv1_32(var.encode('ascii', errors='ignore'))
        if h == target_hash:
            return var
    
    return None

def main():
    print("="*80)
    print("FINAL COMPREHENSIVE DECODE ATTEMPT")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/5] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    readable_entries = [e for e in entries if not e['key'].startswith('0x')]
    
    print(f"  Hex entries: {len(hex_entries)}")
    print(f"  Readable entries: {len(readable_entries)}")
    
    # Create lookup
    hex_lookup = {}
    for entry in hex_entries:
        hex_val = hex_to_int(entry['key'])
        hex_lookup[hex_val] = entry
    
    # Collect ALL possible string names
    print("\n[2/5] Collecting all possible string names...")
    all_names = set()
    
    # From executable
    with open('../ConquestLLC.exe', 'rb') as f:
        exe_data = f.read()
    
    exe_strings = re.findall(b'[a-zA-Z_][a-zA-Z0-9_]{3,50}', exe_data)
    all_names.update([s.decode('ascii', errors='ignore') for s in exe_strings])
    
    # From sound.pck
    with open('sound.pck', 'rb') as f:
        pck_data = f.read()
    
    pck_strings = re.findall(b'[a-zA-Z_][a-zA-Z0-9_]{3,50}', pck_data)
    all_names.update([s.decode('ascii', errors='ignore') for s in pck_strings])
    
    # From .bnk files
    bnk_files = glob.glob('../AddOn/**/*.bnk', recursive=True)
    for bnk_file in bnk_files:
        try:
            with open(bnk_file, 'rb') as f:
                bnk_data = f.read()
            bnk_strings = re.findall(b'[a-zA-Z_][a-zA-Z0-9_]{3,50}', bnk_data)
            all_names.update([s.decode('ascii', errors='ignore') for s in bnk_strings])
        except:
            pass
    
    print(f"  Collected {len(all_names)} unique strings")
    
    # Test all names
    print("\n[3/5] Testing all names with variations...")
    matches = []
    
    for i, name in enumerate(all_names):
        if (i + 1) % 10000 == 0:
            print(f"    Tested {i+1}/{len(all_names)}...")
        
        # Test direct hash
        h = fnv1_32(name.encode('ascii', errors='ignore'))
        if h in hex_lookup:
            matches.append({
                'hex_key': hex_lookup[h]['key'],
                'val': hex_lookup[h]['val'],
                'original_name': name,
                'variation': 'original'
            })
        
        # Test lowercase
        h_lower = fnv1_32(name.lower().encode('ascii', errors='ignore'))
        if h_lower in hex_lookup:
            matches.append({
                'hex_key': hex_lookup[h_lower]['key'],
                'val': hex_lookup[h_lower]['val'],
                'original_name': name,
                'variation': 'lowercase'
            })
    
    print(f"  Found {len(matches)} matches!")
    
    # Also check if hex keys are just the vals themselves
    print("\n[4/5] Checking if hex keys reference vals...")
    val_to_entries = {}
    for entry in entries:
        val = entry['val']
        if val not in val_to_entries:
            val_to_entries[val] = []
        val_to_entries[val].append(entry)
    
    hex_as_val_refs = []
    for entry in hex_entries:
        hex_val = hex_to_int(entry['key'])
        if hex_val in val_to_entries:
            hex_as_val_refs.append({
                'hex_key': entry['key'],
                'hex_as_int': hex_val,
                'points_to_val': hex_val,
                'referenced_entries': val_to_entries[hex_val]
            })
    
    print(f"  Hex keys that reference other vals: {len(hex_as_val_refs)}")
    
    # Generate report
    print("\n[5/5] Generating final report...")
    
    with open('FINAL_HEX_DECODE_REPORT.txt', 'w', encoding='utf-8') as f:
        f.write("FINAL HEX KEY DECODE REPORT\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total hex keys: {len(hex_entries)}\n")
        f.write(f"Decoded from strings: {len(matches)}\n")
        f.write(f"Hex keys referencing other vals: {len(hex_as_val_refs)}\n")
        f.write(f"Remaining undecoded: {len(hex_entries) - len(matches) - len(hex_as_val_refs)}\n\n")
        f.write("="*80 + "\n\n")
        
        if matches:
            f.write("DECODED FROM STRING NAMES\n")
            f.write("-"*80 + "\n\n")
            
            by_name = {}
            for m in matches:
                name = m['original_name']
                if name not in by_name:
                    by_name[name] = []
                by_name[name].append(m)
            
            for name in sorted(by_name.keys()):
                entries_list = by_name[name]
                f.write(f"Original Name: {name}\n")
                for m in entries_list:
                    f.write(f"  {m['hex_key']} → val: {m['val']} ({m['variation']})\n")
                f.write("\n")
        
        if hex_as_val_refs:
            f.write("\n" + "="*80 + "\n\n")
            f.write("HEX KEYS THAT REFERENCE OTHER VALS\n")
            f.write("-"*80 + "\n")
            f.write("These hex keys contain val IDs that point to other entries:\n\n")
            
            for ref in hex_as_val_refs[:50]:
                f.write(f"Hex Key: {ref['hex_key']} (int: {ref['hex_as_int']})\n")
                f.write(f"  References val: {ref['points_to_val']}\n")
                f.write(f"  Which has keys:\n")
                for e in ref['referenced_entries']:
                    f.write(f"    - {e['key']}\n")
                f.write("\n")
    
    print("[+] Report saved to FINAL_HEX_DECODE_REPORT.txt")
    
    # Summary
    print("\n" + "="*80)
    print("FINAL SUMMARY")
    print("="*80)
    print(f"✓ Tested {len(all_names)} unique strings from all sources")
    print(f"✓ Decoded {len(matches)} hex keys from string names")
    print(f"✓ Found {len(hex_as_val_refs)} hex keys that reference other vals")
    print(f"✓ Remaining undecoded: {len(hex_entries) - len(matches) - len(hex_as_val_refs)}")
    
    if matches:
        print(f"\n✓ Successfully decoded {len(set([m['original_name'] for m in matches]))} unique names!")
        print("\nExample decoded names:")
        for name in sorted(set([m['original_name'] for m in matches]))[:30]:
            print(f"  - {name}")
    
    print("\n" + "="*80)
    print("CONCLUSION")
    print("="*80)
    
    total_decoded = len(matches) + len(hex_as_val_refs)
    if total_decoded > 100:
        print(f"✓ SUCCESS! Decoded {total_decoded} hex keys!")
    elif total_decoded > 0:
        print(f"⚠ Partial success: Decoded {total_decoded} hex keys")
        print("  Most hex keys remain undecoded")
    else:
        print("✗ Unable to decode hex keys")
        print("  Possible reasons:")
        print("  - Hex keys are runtime-generated")
        print("  - Original names not stored in any accessible file")
        print("  - Different hash algorithm or encoding")
        print("  - Hex keys are arbitrary IDs without string names")

if __name__ == '__main__':
    main()
