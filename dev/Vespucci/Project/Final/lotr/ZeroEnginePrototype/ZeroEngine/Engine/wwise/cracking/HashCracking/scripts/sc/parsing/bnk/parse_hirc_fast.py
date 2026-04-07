#!/usr/bin/env python3
"""
Fast HIRC parser - memory efficient version
"""
import struct
import json
import mmap
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[4]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

def parse_hirc_object(data, offset, max_offset):
    """Parse a single HIRC object"""
    try:
        if offset + 9 > max_offset:
            return None, offset
        
        # Read object type
        obj_type = data[offset]
        offset += 1
        
        # Read object size
        obj_size = struct.unpack('<I', data[offset:offset+4])[0]
        offset += 4
        
        # Read object ID
        obj_id = struct.unpack('<I', data[offset:offset+4])[0]
        offset += 4
        
        # Skip object data
        obj_data_end = offset + obj_size - 4
        if obj_data_end > max_offset:
            return None, offset
        
        # Extract strings from object data (quick scan)
        strings = []
        obj_data = data[offset:obj_data_end]
        
        # Quick string extraction
        current_str = bytearray()
        for byte in obj_data[:min(len(obj_data), 1000)]:  # Only scan first 1000 bytes
            if 32 <= byte <= 126:
                current_str.append(byte)
            elif byte == 0 and len(current_str) >= 4:
                try:
                    strings.append(current_str.decode('ascii'))
                except:
                    pass
                current_str = bytearray()
            else:
                current_str = bytearray()
        
        return {
            'type': obj_type,
            'id': obj_id,
            'size': obj_size,
            'strings': strings
        }, obj_data_end
    
    except Exception as e:
        return None, offset

def main():
    print("="*80)
    print("FAST HIRC PARSER")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/3] Loading WWiseIDTable...")
    with open('WWiseIDTable.audio.json', 'r') as f:
        wwise_data = json.load(f)
    
    entries = wwise_data.get('obj1s', [])
    
    # Create lookups
    val_to_keys = {}
    hex_to_val = {}
    
    for entry in entries:
        val = entry['val']
        if val not in val_to_keys:
            val_to_keys[val] = []
        val_to_keys[val].append(entry['key'])
        
        if entry['key'].startswith('0x'):
            hex_val = int(entry['key'], 16)
            hex_to_val[hex_val] = entry['val']
    
    print(f"  Loaded {len(entries)} entries, {len(val_to_keys)} unique vals")
    
    # Parse HIRC sections using memory mapping
    print("\n[2/3] Parsing HIRC sections from sound.pck...")
    
    all_objects = []
    all_strings = set()
    matches = []
    hirc_count = 0
    
    with open('sound.pck', 'rb') as f:
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            file_size = len(mm)
            print(f"  File size: {file_size:,} bytes")
            
            offset = 0
            while True:
                # Find next HIRC
                pos = mm.find(b'HIRC', offset)
                if pos == -1:
                    break
                
                hirc_count += 1
                if hirc_count % 10 == 0:
                    print(f"  Processing HIRC #{hirc_count}...")
                
                try:
                    # Read section header
                    section_size = struct.unpack('<I', mm[pos+4:pos+8])[0]
                    num_objects = struct.unpack('<I', mm[pos+8:pos+12])[0]
                    
                    # Parse objects
                    obj_offset = pos + 12
                    max_offset = min(pos + 8 + section_size, file_size)
                    
                    for i in range(min(num_objects, 1000)):  # Limit to 1000 objects per section
                        obj, obj_offset = parse_hirc_object(mm, obj_offset, max_offset)
                        if obj is None:
                            break
                        
                        all_objects.append(obj)
                        all_strings.update(obj['strings'])
                        
                        # Check for matches
                        if obj['id'] in val_to_keys:
                            matches.append({
                                'id': obj['id'],
                                'type': obj['type'],
                                'wwise_keys': val_to_keys[obj['id']],
                                'strings': obj['strings']
                            })
                        
                        if obj['id'] in hex_to_val:
                            matches.append({
                                'id': obj['id'],
                                'type': obj['type'],
                                'wwise_val': hex_to_val[obj['id']],
                                'strings': obj['strings']
                            })
                
                except Exception as e:
                    print(f"    Error at offset 0x{pos:08X}: {e}")
                
                offset = pos + 4
    
    print(f"\n  Found {hirc_count} HIRC sections")
    print(f"  Parsed {len(all_objects)} objects")
    print(f"  Extracted {len(all_strings)} unique strings")
    print(f"  Found {len(matches)} matches with WWiseIDTable")
    
    # Test strings against hex keys
    print("\n[3/3] Testing strings against hex keys...")
    
    string_matches = []
    for s in all_strings:
        if len(s) < 4 or len(s) > 100:
            continue
        
        # Test FNV-1 hash
        h = fnv1_hash(s)
        if h in hex_to_val:
            string_matches.append({
                'string': s,
                'hash': h,
                'val': hex_to_val[h]
            })
        
        # Test lowercase
        h_lower = fnv1_hash(s.lower())
        if h_lower in hex_to_val:
            string_matches.append({
                'string': s + ' (lowercase)',
                'hash': h_lower,
                'val': hex_to_val[h_lower]
            })
    
    print(f"  String hash matches: {len(string_matches)}")
    
    # Generate report
    print("\n[*] Generating report...")
    
    with open('hirc_fast_report.txt', 'w', encoding='utf-8') as f:
        f.write("FAST HIRC ANALYSIS REPORT\n")
        f.write("="*80 + "\n\n")
        f.write(f"HIRC sections found: {hirc_count}\n")
        f.write(f"Objects parsed: {len(all_objects)}\n")
        f.write(f"Unique strings: {len(all_strings)}\n")
        f.write(f"ID matches with WWiseIDTable: {len(matches)}\n")
        f.write(f"String hash matches: {len(string_matches)}\n\n")
        f.write("="*80 + "\n\n")
        
        if string_matches:
            f.write("STRING HASH MATCHES (DECODED HEX KEYS!)\n")
            f.write("-"*80 + "\n\n")
            for m in string_matches:
                f.write(f"String: {m['string']}\n")
                f.write(f"  Hash: 0x{m['hash']:08X}\n")
                f.write(f"  Val: {m['val']}\n")
                f.write(f"  WWise Keys: {', '.join(val_to_keys.get(m['val'], []))}\n")
                f.write("\n")
        
        f.write("\n" + "="*80 + "\n\n")
        f.write("ALL UNIQUE STRINGS FROM HIRC\n")
        f.write("-"*80 + "\n\n")
        for s in sorted(all_strings):
            if len(s) >= 4:
                f.write(f"{s}\n")
        
        if matches:
            f.write("\n" + "="*80 + "\n\n")
            f.write("OBJECT ID MATCHES\n")
            f.write("-"*80 + "\n\n")
            for m in matches[:100]:
                f.write(f"Object ID: {m['id']} (0x{m['id']:08X})\n")
                f.write(f"  Type: {m['type']}\n")
                if 'wwise_keys' in m:
                    f.write(f"  WWise Keys: {', '.join(m['wwise_keys'])}\n")
                if 'strings' in m and m['strings']:
                    f.write(f"  Strings: {', '.join(m['strings'][:5])}\n")
                f.write("\n")
    
    print("\n" + "="*80)
    print("RESULTS")
    print("="*80)
    print(f"✓ Parsed {hirc_count} HIRC sections")
    print(f"✓ Found {len(all_objects)} objects")
    print(f"✓ Extracted {len(all_strings)} strings")
    print(f"✓ ID matches: {len(matches)}")
    print(f"✓ STRING HASH MATCHES: {len(string_matches)}")
    
    if string_matches:
        print(f"\n🎉 SUCCESS! Decoded {len(string_matches)} hex keys from HIRC strings!")
        print("\nExample decoded strings:")
        for m in string_matches[:10]:
            print(f"  {m['string']} → 0x{m['hash']:08X}")
    
    print(f"\n✓ Report saved to hirc_fast_report.txt")

if __name__ == '__main__':
    main()
