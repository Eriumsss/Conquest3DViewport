#!/usr/bin/env python3
"""
Parse HIRC (Hierarchy) sections from sound.pck
HIRC sections contain event definitions with IDs and potentially names
"""
import struct
import json

def parse_hirc_section(data, offset):
    """
    Parse a HIRC section starting at offset
    
    HIRC structure:
    - 4 bytes: 'HIRC' signature
    - 4 bytes: section size (little-endian)
    - 4 bytes: number of objects (little-endian)
    - Objects array
    
    Each object:
    - 1 byte: object type
    - 4 bytes: object size (little-endian)
    - 4 bytes: object ID (little-endian)
    - Variable: object data
    """
    try:
        # Verify signature
        sig = data[offset:offset+4]
        if sig != b'HIRC':
            return None
        
        # Read section size
        section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
        
        # Read number of objects
        num_objects = struct.unpack('<I', data[offset+8:offset+12])[0]
        
        print(f"  Found HIRC at offset 0x{offset:08X}")
        print(f"    Section size: {section_size} bytes")
        print(f"    Number of objects: {num_objects}")
        
        objects = []
        obj_offset = offset + 12
        
        for i in range(num_objects):
            if obj_offset >= offset + 8 + section_size:
                break
            
            # Read object type
            obj_type = data[obj_offset]
            obj_offset += 1
            
            # Read object size
            obj_size = struct.unpack('<I', data[obj_offset:obj_offset+4])[0]
            obj_offset += 4
            
            # Read object ID
            obj_id = struct.unpack('<I', data[obj_offset:obj_offset+4])[0]
            obj_offset += 4
            
            # Read object data
            obj_data = data[obj_offset:obj_offset+obj_size-4]
            
            # Try to extract strings from object data
            strings = []
            try:
                # Look for null-terminated ASCII strings
                current_str = b''
                for byte in obj_data:
                    if 32 <= byte <= 126:  # Printable ASCII
                        current_str += bytes([byte])
                    elif byte == 0 and len(current_str) >= 4:
                        strings.append(current_str.decode('ascii', errors='ignore'))
                        current_str = b''
                    else:
                        current_str = b''
            except:
                pass
            
            objects.append({
                'type': obj_type,
                'id': obj_id,
                'size': obj_size,
                'strings': strings,
                'offset': obj_offset - 8
            })
            
            obj_offset += obj_size - 4
        
        return {
            'offset': offset,
            'size': section_size,
            'num_objects': num_objects,
            'objects': objects
        }
    
    except Exception as e:
        print(f"    Error parsing HIRC at 0x{offset:08X}: {e}")
        return None

def find_all_hirc_sections(data):
    """Find all HIRC sections in data"""
    print("\n[*] Searching for HIRC sections...")

    all_sections = []
    offset = 0
    count = 0

    while True:
        pos = data.find(b'HIRC', offset)
        if pos == -1:
            break

        count += 1
        if count % 10 == 0:
            print(f"  Found {count} HIRC sections so far...")

        section = parse_hirc_section(data, pos)
        if section:
            all_sections.append(section)

        offset = pos + 4  # Skip past this HIRC signature

    return all_sections

def main():
    print("="*80)
    print("PARSING HIRC SECTIONS FROM sound.pck")
    print("="*80)
    
    # Load sound.pck
    print("\n[1/4] Loading sound.pck...")
    with open('sound.pck', 'rb') as f:
        pck_data = f.read()
    
    print(f"  Loaded {len(pck_data):,} bytes")
    
    # Find all HIRC sections
    print("\n[2/4] Finding all HIRC sections...")
    hirc_sections = find_all_hirc_sections(pck_data)
    
    print(f"\n  Found {len(hirc_sections)} HIRC sections")
    
    # Load WWiseIDTable for comparison
    print("\n[3/4] Loading WWiseIDTable...")
    with open('WWiseIDTable.audio.json', 'r') as f:
        wwise_data = json.load(f)
    
    entries = wwise_data.get('obj1s', [])
    
    # Create lookup by val
    val_to_entries = {}
    for entry in entries:
        val = entry['val']
        if val not in val_to_entries:
            val_to_entries[val] = []
        val_to_entries[val].append(entry['key'])
    
    # Create lookup by hex key
    hex_to_val = {}
    for entry in entries:
        if entry['key'].startswith('0x'):
            hex_val = int(entry['key'], 16)
            hex_to_val[hex_val] = entry['val']
    
    print(f"  Loaded {len(entries)} WWiseIDTable entries")
    print(f"  {len(val_to_entries)} unique vals")
    print(f"  {len(hex_to_val)} hex keys")
    
    # Analyze HIRC objects
    print("\n[4/4] Analyzing HIRC objects...")
    
    matches_by_id = []
    matches_by_string = []
    all_event_objects = []
    all_strings = set()
    
    for section in hirc_sections:
        for obj in section['objects']:
            # Object type 3 = Event, type 2 = Sound, type 4 = Action
            if obj['type'] in [2, 3, 4]:
                all_event_objects.append(obj)
            
            # Check if object ID matches any val
            if obj['id'] in val_to_entries:
                matches_by_id.append({
                    'object_id': obj['id'],
                    'object_type': obj['type'],
                    'wwise_keys': val_to_entries[obj['id']],
                    'strings': obj['strings']
                })
            
            # Check if object ID matches any hex key
            if obj['id'] in hex_to_val:
                matches_by_id.append({
                    'object_id': obj['id'],
                    'object_type': obj['type'],
                    'wwise_val': hex_to_val[obj['id']],
                    'strings': obj['strings']
                })
            
            # Collect all strings
            all_strings.update(obj['strings'])
    
    print(f"  Total objects: {sum(len(s['objects']) for s in hirc_sections)}")
    print(f"  Event-related objects (type 2,3,4): {len(all_event_objects)}")
    print(f"  Unique strings found: {len(all_strings)}")
    print(f"  Matches by ID: {len(matches_by_id)}")
    
    # Generate report
    print("\n[*] Generating report...")
    
    with open('hirc_analysis_report.txt', 'w', encoding='utf-8') as f:
        f.write("HIRC SECTION ANALYSIS REPORT\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total HIRC sections found: {len(hirc_sections)}\n")
        f.write(f"Total objects: {sum(len(s['objects']) for s in hirc_sections)}\n")
        f.write(f"Event-related objects: {len(all_event_objects)}\n")
        f.write(f"Unique strings: {len(all_strings)}\n")
        f.write(f"Matches with WWiseIDTable: {len(matches_by_id)}\n\n")
        f.write("="*80 + "\n\n")
        
        # Write all unique strings
        f.write("ALL UNIQUE STRINGS FROM HIRC OBJECTS\n")
        f.write("-"*80 + "\n\n")
        for s in sorted(all_strings):
            if len(s) >= 4:  # Only meaningful strings
                f.write(f"{s}\n")
        
        f.write("\n" + "="*80 + "\n\n")
        
        # Write matches
        if matches_by_id:
            f.write("HIRC OBJECTS MATCHING WWiseIDTable\n")
            f.write("-"*80 + "\n\n")
            for match in matches_by_id:
                f.write(f"Object ID: {match['object_id']} (0x{match['object_id']:08X})\n")
                f.write(f"  Object Type: {match['object_type']}\n")
                if 'wwise_keys' in match:
                    f.write(f"  WWise Keys: {', '.join(match['wwise_keys'])}\n")
                if 'wwise_val' in match:
                    f.write(f"  WWise Val: {match['wwise_val']}\n")
                if match['strings']:
                    f.write(f"  Strings: {', '.join(match['strings'])}\n")
                f.write("\n")
        
        f.write("\n" + "="*80 + "\n\n")
        
        # Write detailed section info
        f.write("DETAILED HIRC SECTION INFORMATION\n")
        f.write("-"*80 + "\n\n")
        for i, section in enumerate(hirc_sections):
            f.write(f"Section {i+1}:\n")
            f.write(f"  Offset: 0x{section['offset']:08X}\n")
            f.write(f"  Size: {section['size']} bytes\n")
            f.write(f"  Objects: {section['num_objects']}\n")
            
            # Show first 10 objects
            for j, obj in enumerate(section['objects'][:10]):
                f.write(f"    Object {j+1}:\n")
                f.write(f"      Type: {obj['type']}\n")
                f.write(f"      ID: {obj['id']} (0x{obj['id']:08X})\n")
                if obj['strings']:
                    f.write(f"      Strings: {', '.join(obj['strings'][:5])}\n")
            
            if len(section['objects']) > 10:
                f.write(f"    ... and {len(section['objects']) - 10} more objects\n")
            
            f.write("\n")
    
    # Save JSON
    print("[*] Saving JSON data...")
    
    json_data = {
        'total_sections': len(hirc_sections),
        'total_objects': sum(len(s['objects']) for s in hirc_sections),
        'unique_strings': sorted(list(all_strings)),
        'matches': matches_by_id,
        'sections': [{
            'offset': s['offset'],
            'size': s['size'],
            'num_objects': s['num_objects'],
            'objects': [{
                'type': o['type'],
                'id': o['id'],
                'strings': o['strings']
            } for o in s['objects']]
        } for s in hirc_sections]
    }
    
    with open('hirc_analysis.json', 'w', encoding='utf-8') as f:
        json.dump(json_data, f, indent=2)
    
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Found {len(hirc_sections)} HIRC sections")
    print(f"✓ Parsed {sum(len(s['objects']) for s in hirc_sections)} objects")
    print(f"✓ Extracted {len(all_strings)} unique strings")
    print(f"✓ Found {len(matches_by_id)} matches with WWiseIDTable")
    print(f"\n✓ Reports saved:")
    print(f"  - hirc_analysis_report.txt")
    print(f"  - hirc_analysis.json")

if __name__ == '__main__':
    main()

