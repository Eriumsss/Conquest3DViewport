#!/usr/bin/env python3
"""
Parse HIRC sections from sound.pck to find event names
"""
import struct
import mmap

def find_all_sections(data, signature):
    """Find all occurrences of a section signature"""
    positions = []
    offset = 0
    while True:
        pos = data.find(signature, offset)
        if pos == -1:
            break
        positions.append(pos)
        offset = pos + 4
    return positions

def parse_hirc_section(data, offset):
    """Parse HIRC section and extract strings"""
    try:
        sig = data[offset:offset+4]
        if sig != b'HIRC':
            return []
        
        section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
        num_objects = struct.unpack('<I', data[offset+8:offset+12])[0]
        
        strings = []
        obj_offset = offset + 12
        
        # Parse each HIRC object
        for i in range(num_objects):
            if obj_offset >= offset + 8 + section_size:
                break
            
            obj_type = data[obj_offset]
            obj_size = struct.unpack('<I', data[obj_offset+1:obj_offset+5])[0]
            obj_id = struct.unpack('<I', data[obj_offset+5:obj_offset+9])[0]
            
            # Extract any ASCII strings from object data
            obj_data = data[obj_offset+9:obj_offset+5+obj_size]
            
            # Find null-terminated strings
            current_str = b''
            for byte in obj_data:
                if 32 <= byte <= 126:  # Printable ASCII
                    current_str += bytes([byte])
                elif byte == 0 and len(current_str) >= 4:  # Null terminator
                    try:
                        s = current_str.decode('ascii')
                        strings.append({
                            'obj_type': obj_type,
                            'obj_id': obj_id,
                            'string': s
                        })
                    except:
                        pass
                    current_str = b''
                else:
                    current_str = b''
            
            obj_offset += 5 + obj_size
        
        return strings
    except Exception as e:
        return []

print("="*80)
print("PARSING HIRC SECTIONS FROM sound.pck")
print("="*80)

print("\n[1/3] Opening sound.pck with memory mapping...")

with open('sound.pck', 'rb') as f:
    with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        file_size = len(data)
        print(f"  File size: {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")
        
        print("\n[2/3] Finding HIRC sections...")
        hirc_positions = find_all_sections(data, b'HIRC')
        print(f"  Found {len(hirc_positions)} HIRC sections")
        
        print("\n[3/3] Parsing HIRC sections...")
        all_strings = []
        
        for idx, pos in enumerate(hirc_positions):
            if idx % 10 == 0:
                print(f"  Processing section {idx+1}/{len(hirc_positions)}...")
            
            strings = parse_hirc_section(data, pos)
            all_strings.extend(strings)
        
        print(f"\n  Total strings extracted: {len(all_strings)}")

# Remove duplicates
unique_strings = {}
for s in all_strings:
    key = s['string']
    if key not in unique_strings:
        unique_strings[key] = s

print(f"  Unique strings: {len(unique_strings)}")

# Save to file
with open('hirc_strings.txt', 'w', encoding='utf-8') as f:
    f.write("STRINGS EXTRACTED FROM HIRC SECTIONS\n")
    f.write("="*80 + "\n\n")
    f.write(f"Total strings: {len(all_strings)}\n")
    f.write(f"Unique strings: {len(unique_strings)}\n\n")
    f.write("="*80 + "\n\n")
    
    for s in sorted(unique_strings.keys()):
        obj = unique_strings[s]
        f.write(f"Type {obj['obj_type']:3d} | ID 0x{obj['obj_id']:08X} | {s}\n")

print(f"\n✓ Saved to hirc_strings.txt")

# Show sample
print(f"\nSample strings (first 20):")
for s in sorted(unique_strings.keys())[:20]:
    print(f"  {s}")

