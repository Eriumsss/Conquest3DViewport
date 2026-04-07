#!/usr/bin/env python3
"""
Parse STID sections from sound.pck and match with WWiseIDTable.audio.json
"""
import json
import struct
import re

def find_stid_offsets(filename):
    """Find all STID section offsets"""
    offsets = []
    with open(filename, 'rb') as f:
        data = f.read()
        for match in re.finditer(b'STID', data):
            offsets.append(match.start())
    return offsets

def parse_stid_section(data, offset):
    """Parse a single STID section"""
    try:
        # STID header structure:
        # 4 bytes: "STID"
        # 4 bytes: section size (little-endian)
        # 4 bytes: unknown/version
        # 4 bytes: number of entries
        
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
            
            # Read ID (4 bytes, little-endian)
            string_id = struct.unpack('<I', data[pos:pos+4])[0]
            pos += 4
            
            # Read null-terminated string
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
    except Exception as e:
        return None

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    
    entries = data.get('obj1s', [])
    
    # Create lookup by val
    val_to_keys = {}
    for entry in entries:
        val = entry['val']
        key = entry['key']
        if val not in val_to_keys:
            val_to_keys[val] = []
        val_to_keys[val].append(key)
    
    return val_to_keys

def main():
    pck_file = 'sound.pck'
    wwise_table = 'WWiseIDTable.audio.json'
    
    print("[+] Loading WWiseIDTable...")
    val_to_keys = load_wwise_table(wwise_table)
    print(f"[+] Loaded {len(val_to_keys)} unique IDs from WWiseIDTable")
    
    print("\n[+] Finding STID sections in sound.pck...")
    with open(pck_file, 'rb') as f:
        pck_data = f.read()
    
    stid_offsets = find_stid_offsets(pck_file)
    print(f"[+] Found {len(stid_offsets)} STID sections")
    
    print("\n" + "="*80)
    print("PARSING STID SECTIONS")
    print("="*80)
    
    all_stid_entries = []
    matched_count = 0
    
    for i, offset in enumerate(stid_offsets[:50]):  # Parse first 50
        stid = parse_stid_section(pck_data, offset)
        if not stid:
            continue
        
        print(f"\n[STID #{i+1}] Offset: 0x{offset:08X}")
        print(f"  Size: {stid['size']} bytes")
        print(f"  Entries: {stid['num_entries']}")
        
        for entry in stid['entries']:
            string_id = entry['id']
            string_name = entry['name']
            
            # Check if this ID exists in WWiseIDTable
            if string_id in val_to_keys:
                keys = val_to_keys[string_id]
                print(f"    ✓ ID {string_id} = '{string_name}'")
                print(f"      Matches WWiseIDTable keys: {keys}")
                matched_count += 1
            else:
                print(f"    • ID {string_id} = '{string_name}' (not in WWiseIDTable)")
            
            all_stid_entries.append({
                'stid_offset': offset,
                'id': string_id,
                'name': string_name,
                'wwise_keys': val_to_keys.get(string_id, [])
            })
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Total STID sections found: {len(stid_offsets)}")
    print(f"Total STID entries parsed: {len(all_stid_entries)}")
    print(f"Entries matched to WWiseIDTable: {matched_count}")
    
    # Export results
    print("\n[+] Exporting results...")
    with open('stid_analysis.json', 'w') as f:
        json.dump(all_stid_entries, f, indent=2)
    
    with open('stid_analysis.txt', 'w') as f:
        f.write("STID SECTION ANALYSIS\n")
        f.write("="*80 + "\n\n")
        
        for entry in all_stid_entries:
            f.write(f"ID: {entry['id']}\n")
            f.write(f"Name: {entry['name']}\n")
            f.write(f"STID Offset: 0x{entry['stid_offset']:08X}\n")
            if entry['wwise_keys']:
                f.write(f"WWise Keys: {', '.join(entry['wwise_keys'])}\n")
            f.write("\n")
    
    print("[+] Results saved to stid_analysis.json and stid_analysis.txt")
    
    # Show some interesting matches
    print("\n" + "="*80)
    print("INTERESTING MATCHES")
    print("="*80)
    
    interesting = [e for e in all_stid_entries if e['wwise_keys'] and not e['wwise_keys'][0].startswith('0x')]
    for entry in interesting[:20]:
        print(f"  {entry['name']} (ID: {entry['id']})")
        print(f"    → WWise keys: {', '.join(entry['wwise_keys'])}")

if __name__ == '__main__':
    main()

