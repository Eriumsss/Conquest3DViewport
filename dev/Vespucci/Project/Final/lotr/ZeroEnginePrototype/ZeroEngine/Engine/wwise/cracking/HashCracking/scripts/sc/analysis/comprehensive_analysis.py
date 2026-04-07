#!/usr/bin/env python3
"""
Comprehensive analysis of WWiseIDTable.audio.json and sound.pck
Find ALL connections between the two data sources
"""
import json
import struct
import re
from collections import defaultdict

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json and organize data"""
    with open(filename, 'r') as f:
        data = json.load(f)
    
    entries = data.get('obj1s', [])
    
    # Organize by different criteria
    all_entries = []
    hex_keys = []
    readable_keys = []
    val_to_entries = defaultdict(list)
    
    for entry in entries:
        key = entry['key']
        val = entry['val']
        
        all_entries.append(entry)
        val_to_entries[val].append(entry)
        
        if key.startswith('0x'):
            hex_keys.append(entry)
        else:
            readable_keys.append(entry)
    
    return {
        'all': all_entries,
        'hex_keys': hex_keys,
        'readable_keys': readable_keys,
        'val_to_entries': val_to_entries,
        'total': len(all_entries)
    }

def find_all_stid_offsets(data):
    """Find all STID section offsets"""
    offsets = []
    for match in re.finditer(b'STID', data):
        offsets.append(match.start())
    return offsets

def parse_stid_section(data, offset):
    """Parse a single STID section"""
    try:
        if offset + 16 > len(data):
            return None
        
        signature = data[offset:offset+4]
        if signature != b'STID':
            return None
        
        section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
        unknown = struct.unpack('<I', data[offset+8:offset+12])[0]
        num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
        
        entries = []
        pos = offset + 16
        
        for i in range(num_entries):
            if pos + 4 > len(data):
                break
            
            string_id = struct.unpack('<I', data[pos:pos+4])[0]
            pos += 4
            
            string_end = data.find(b'\x00', pos)
            if string_end == -1:
                break
            
            string_name = data[pos:string_end].decode('ascii', errors='ignore')
            pos = string_end + 1
            
            entries.append({
                'id': string_id,
                'name': string_name
            })
        
        return {
            'offset': offset,
            'size': section_size,
            'num_entries': num_entries,
            'entries': entries
        }
    except:
        return None

def search_binary_for_id(data, numeric_id):
    """Search for a numeric ID in binary data (little-endian 32-bit)"""
    id_bytes = struct.pack('<I', numeric_id)
    positions = []
    offset = 0
    while True:
        pos = data.find(id_bytes, offset)
        if pos == -1:
            break
        positions.append(pos)
        offset = pos + 1
    return positions

def search_binary_for_string(data, search_string):
    """Search for ASCII string in binary data"""
    search_bytes = search_string.encode('ascii')
    positions = []
    offset = 0
    while True:
        pos = data.find(search_bytes, offset)
        if pos == -1:
            break
        positions.append(pos)
        offset = pos + 1
    return positions

def main():
    print("="*80)
    print("COMPREHENSIVE WWISE ANALYSIS")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/6] Loading WWiseIDTable.audio.json...")
    wwise = load_wwise_table('WWiseIDTable.audio.json')
    print(f"  Total entries: {wwise['total']}")
    print(f"  Hex key entries: {len(wwise['hex_keys'])}")
    print(f"  Readable key entries: {len(wwise['readable_keys'])}")
    print(f"  Unique 'val' IDs: {len(wwise['val_to_entries'])}")
    
    # Load sound.pck
    print("\n[2/6] Loading sound.pck...")
    with open('sound.pck', 'rb') as f:
        pck_data = f.read()
    print(f"  File size: {len(pck_data):,} bytes")
    
    # Parse ALL STID sections
    print("\n[3/6] Parsing ALL STID sections...")
    stid_offsets = find_all_stid_offsets(pck_data)
    print(f"  Found {len(stid_offsets)} STID sections")
    
    all_stid_entries = []
    stid_ids = set()
    stid_names = set()
    
    for i, offset in enumerate(stid_offsets):
        if (i + 1) % 50 == 0:
            print(f"    Parsing STID {i+1}/{len(stid_offsets)}...")
        
        stid = parse_stid_section(pck_data, offset)
        if stid:
            for entry in stid['entries']:
                all_stid_entries.append(entry)
                stid_ids.add(entry['id'])
                stid_names.add(entry['name'])
    
    print(f"  Total STID entries: {len(all_stid_entries)}")
    print(f"  Unique STID IDs: {len(stid_ids)}")
    print(f"  Unique STID names: {len(stid_names)}")
    
    # Search for WWiseIDTable VAL IDs in sound.pck
    print("\n[4/6] Searching for WWiseIDTable 'val' IDs in sound.pck...")
    val_matches = {}
    sample_vals = list(wwise['val_to_entries'].keys())[:100]  # Sample first 100
    
    for i, val in enumerate(sample_vals):
        if (i + 1) % 20 == 0:
            print(f"    Checking val {i+1}/100...")
        positions = search_binary_for_id(pck_data, val)
        if positions:
            val_matches[val] = positions
    
    print(f"  Found {len(val_matches)} 'val' IDs present in sound.pck (out of 100 sampled)")
    
    # Search for readable key strings in sound.pck
    print("\n[5/6] Searching for readable key strings in sound.pck...")
    string_matches = {}
    
    for entry in wwise['readable_keys']:
        key = entry['key']
        positions = search_binary_for_string(pck_data, key)
        if positions:
            string_matches[key] = {
                'val': entry['val'],
                'positions': positions
            }
    
    print(f"  Found {len(string_matches)} readable keys in sound.pck (out of {len(wwise['readable_keys'])})")
    
    # Cross-reference analysis
    print("\n[6/6] Cross-referencing data...")
    
    # Check if STID IDs match WWise vals
    stid_in_wwise = stid_ids & set(wwise['val_to_entries'].keys())
    print(f"  STID IDs that match WWise 'val': {len(stid_in_wwise)}")
    
    # Generate report
    print("\n" + "="*80)
    print("GENERATING DETAILED REPORT")
    print("="*80)
    
    with open('comprehensive_analysis_report.txt', 'w', encoding='utf-8') as f:
        f.write("COMPREHENSIVE WWISE ANALYSIS REPORT\n")
        f.write("="*80 + "\n\n")
        
        # Section 1: Overview
        f.write("1. DATA OVERVIEW\n")
        f.write("-"*80 + "\n")
        f.write(f"WWiseIDTable total entries: {wwise['total']}\n")
        f.write(f"WWiseIDTable hex keys: {len(wwise['hex_keys'])}\n")
        f.write(f"WWiseIDTable readable keys: {len(wwise['readable_keys'])}\n")
        f.write(f"WWiseIDTable unique vals: {len(wwise['val_to_entries'])}\n")
        f.write(f"\n")
        f.write(f"sound.pck STID sections: {len(stid_offsets)}\n")
        f.write(f"sound.pck STID entries: {len(all_stid_entries)}\n")
        f.write(f"sound.pck unique STID IDs: {len(stid_ids)}\n")
        f.write(f"sound.pck unique STID names: {len(stid_names)}\n")
        f.write("\n\n")
        
        # Section 2: All STID Names
        f.write("2. ALL UNIQUE STID NAMES FOUND (COMPLETE LIST)\n")
        f.write("-"*80 + "\n")
        for name in sorted(stid_names):
            # Find all IDs for this name
            ids = [e['id'] for e in all_stid_entries if e['name'] == name]
            unique_ids = set(ids)
            f.write(f"{name}\n")
            f.write(f"  IDs: {', '.join(str(id) for id in sorted(unique_ids))}\n")
            f.write(f"  Occurrences: {len(ids)}\n")
        f.write("\n\n")
        
        # Section 3: Readable keys found in sound.pck
        f.write("3. READABLE KEYS FOUND IN sound.pck\n")
        f.write("-"*80 + "\n")
        for key, data in sorted(string_matches.items()):
            f.write(f"{key} (val: {data['val']})\n")
            f.write(f"  Found at {len(data['positions'])} location(s)\n")
            for pos in data['positions'][:5]:
                f.write(f"    0x{pos:08X}\n")
        f.write("\n\n")
        
        # Section 4: Val IDs found in sound.pck
        f.write("4. WWISE 'VAL' IDs FOUND IN sound.pck (SAMPLE)\n")
        f.write("-"*80 + "\n")
        for val, positions in sorted(val_matches.items())[:50]:
            entries = wwise['val_to_entries'][val]
            keys = [e['key'] for e in entries]
            f.write(f"Val: {val}\n")
            f.write(f"  Keys: {', '.join(keys[:5])}\n")
            f.write(f"  Found at {len(positions)} location(s): ")
            f.write(', '.join(f"0x{p:08X}" for p in positions[:3]))
            f.write("\n")
        f.write("\n\n")
        
        # Section 5: Cross-reference
        f.write("5. CROSS-REFERENCE ANALYSIS\n")
        f.write("-"*80 + "\n")
        f.write(f"STID IDs matching WWise vals: {len(stid_in_wwise)}\n")
        if stid_in_wwise:
            f.write("Matches:\n")
            for stid_id in sorted(stid_in_wwise):
                stid_entry = [e for e in all_stid_entries if e['id'] == stid_id][0]
                wwise_entries = wwise['val_to_entries'][stid_id]
                f.write(f"  STID ID {stid_id}: '{stid_entry['name']}'\n")
                f.write(f"    WWise keys: {[e['key'] for e in wwise_entries]}\n")
    
    print("[+] Report saved to comprehensive_analysis_report.txt")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Parsed all {len(stid_offsets)} STID sections")
    print(f"✓ Found {len(stid_names)} unique string names in STID")
    print(f"✓ Found {len(string_matches)}/{len(wwise['readable_keys'])} readable keys in sound.pck")
    print(f"✓ Found {len(val_matches)}/100 sampled 'val' IDs in sound.pck")
    print(f"✓ STID-WWise matches: {len(stid_in_wwise)}")

if __name__ == '__main__':
    main()

