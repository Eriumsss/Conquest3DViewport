#!/usr/bin/env python3
"""
Parse all .bnk files to extract STID names and test against hex keys
"""
import json
import struct
import os
import glob
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[4]  # .../scripts
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

def parse_stid_section(data, offset):
    """Parse a single STID section"""
    try:
        # Read header
        signature = data[offset:offset+4]
        if signature != b'STID':
            return None
        
        size = struct.unpack('<I', data[offset+4:offset+8])[0]
        unknown = struct.unpack('<I', data[offset+8:offset+12])[0]
        num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
        
        # Read entries
        entries = []
        entry_offset = offset + 16
        
        for i in range(num_entries):
            if entry_offset >= len(data):
                break
            
            # Read ID (4 bytes, little-endian)
            stid_id = struct.unpack('<I', data[entry_offset:entry_offset+4])[0]
            entry_offset += 4
            
            # Read null-terminated string
            name_start = entry_offset
            while entry_offset < len(data) and data[entry_offset] != 0:
                entry_offset += 1
            
            name = data[name_start:entry_offset].decode('ascii', errors='ignore')
            entry_offset += 1  # Skip null terminator
            
            entries.append({
                'id': stid_id,
                'name': name
            })
        
        return entries
    except:
        return None

def find_all_stid_sections(data):
    """Find all STID sections in data"""
    all_entries = []
    offset = 0
    
    while True:
        pos = data.find(b'STID', offset)
        if pos == -1:
            break
        
        entries = parse_stid_section(data, pos)
        if entries:
            all_entries.extend(entries)
        
        offset = pos + 1
    
    return all_entries

def test_string(name_str, hex_lookup):
    """Test if a string matches any hex key using FNV-1"""
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
    print("PARSING ALL .BNK FILES FOR EVENT NAMES")
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
    
    # Find all .bnk files
    print("\n[2/4] Finding .bnk files...")
    bnk_files = []
    
    # Check AddOn folder
    addon_bnks = glob.glob('../AddOn/**/*.bnk', recursive=True)
    bnk_files.extend(addon_bnks)
    
    # Check Audio folder
    audio_bnks = glob.glob('*.bnk')
    bnk_files.extend(audio_bnks)
    
    print(f"  Found {len(bnk_files)} .bnk files")
    
    # Parse all .bnk files
    print("\n[3/4] Parsing .bnk files for STID sections...")
    all_stid_names = []
    
    for bnk_file in bnk_files:
        try:
            with open(bnk_file, 'rb') as f:
                data = f.read()
            
            entries = find_all_stid_sections(data)
            if entries:
                print(f"  {os.path.basename(bnk_file)}: {len(entries)} STID entries")
                all_stid_names.extend([e['name'] for e in entries])
        except Exception as e:
            print(f"  Error reading {bnk_file}: {e}")
    
    # Remove duplicates
    unique_names = list(set(all_stid_names))
    print(f"\n  Total unique STID names: {len(unique_names)}")
    
    # Test all names against hex keys
    print("\n[4/4] Testing STID names against hex keys...")
    matches = []
    
    for name in unique_names:
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
    print("\n" + "="*80)
    print("GENERATING REPORT")
    print("="*80)
    
    with open('bnk_decoded_hex_keys.txt', 'w', encoding='utf-8') as f:
        f.write("DECODED HEX KEYS FROM .BNK FILES\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total .bnk files parsed: {len(bnk_files)}\n")
        f.write(f"Total unique STID names: {len(unique_names)}\n")
        f.write(f"Hex keys decoded: {len(matches)}\n")
        f.write(f"Remaining undecoded: {len(hex_entries) - len(matches)}\n\n")
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
            f.write(f"  Hash (FNV-1): 0x{entries_list[0]['hash_value']:08X}\n")
            f.write(f"  Hex Keys:\n")
            for m in entries_list:
                f.write(f"    {m['hex_key']} → val: {m['val']}\n")
            f.write("\n")
        
        # Also list all STID names found
        f.write("\n" + "="*80 + "\n\n")
        f.write("ALL STID NAMES FOUND IN .BNK FILES\n")
        f.write("-"*80 + "\n\n")
        for name in sorted(unique_names):
            f.write(f"{name}\n")
    
    print("[+] Report saved to bnk_decoded_hex_keys.txt")
    
    # Create JSON mapping
    if matches:
        with open('hex_to_name_mapping_final.json', 'w', encoding='utf-8') as f:
            mapping = {}
            for m in matches:
                mapping[m['hex_key']] = {
                    'name': m['original_name'],
                    'val': m['val'],
                    'case': m['case']
                }
            json.dump(mapping, f, indent=2)
        print("[+] Mapping saved to hex_to_name_mapping_final.json")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Parsed {len(bnk_files)} .bnk files")
    print(f"✓ Extracted {len(unique_names)} unique STID names")
    print(f"✓ Successfully decoded {len(matches)} hex keys!")
    print(f"✓ Remaining undecoded: {len(hex_entries) - len(matches)}")
    
    if matches:
        print(f"\n✓ Decoded {len(set([m['original_name'] for m in matches]))} unique event names")
        
        # Show some examples
        print("\nExample decoded names:")
        for name in sorted(set([m['original_name'] for m in matches]))[:30]:
            print(f"  - {name}")

if __name__ == '__main__':
    main()
