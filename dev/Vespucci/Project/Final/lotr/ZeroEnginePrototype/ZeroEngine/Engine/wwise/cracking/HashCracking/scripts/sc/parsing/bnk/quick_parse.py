#!/usr/bin/env python3
import struct
import os

def parse_stid(data, offset):
    """Parse STID section"""
    if data[offset:offset+4] != b'STID':
        return []
    
    section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
    num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
    
    entries = []
    entry_offset = offset + 16
    
    for i in range(num_entries):
        if entry_offset >= offset + 8 + section_size:
            break
        
        entry_id = struct.unpack('<I', data[entry_offset:entry_offset+4])[0]
        entry_offset += 4
        
        name_bytes = b''
        while entry_offset < len(data) and data[entry_offset] != 0:
            name_bytes += bytes([data[entry_offset]])
            entry_offset += 1
        
        entry_offset += 1
        
        try:
            name = name_bytes.decode('ascii')
            entries.append(name)
        except:
            pass
    
    return entries

# Parse first 10 files
output = []
output.append("="*80)
output.append("QUICK PARSE OF EXTRACTED BNK FILES")
output.append("="*80)

for i in range(10):
    filepath = f'extracted_bnk/bank_{i:03d}_id_*.bnk'
    
    # Find the actual file
    import glob
    files = glob.glob(filepath)
    if not files:
        continue
    
    filepath = files[0]
    
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        
        bank_id = struct.unpack('<I', data[12:16])[0]
        
        # Find STID
        pos = data.find(b'STID')
        if pos != -1:
            names = parse_stid(data, pos)
            output.append(f"\nBank {i}: 0x{bank_id:08X}")
            output.append(f"  File: {os.path.basename(filepath)}")
            output.append(f"  Size: {len(data)} bytes")
            if names:
                output.append(f"  Names: {', '.join(names)}")
            else:
                output.append(f"  Names: (none)")
        else:
            output.append(f"\nBank {i}: 0x{bank_id:08X} - NO STID FOUND")
    except Exception as e:
        output.append(f"\nBank {i}: ERROR - {e}")

# Write output
with open('quick_parse_output.txt', 'w') as f:
    f.write('\n'.join(output))

print("Done! Output written to quick_parse_output.txt")

