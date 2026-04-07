#!/usr/bin/env python3
"""
Test parsing a single extracted BNK file
"""
import struct
import os

def parse_stid_section(data, offset):
    """Parse STID section (little-endian PC format)"""
    print(f"\n  Parsing STID at offset 0x{offset:08X}")
    
    sig = data[offset:offset+4]
    print(f"    Signature: {sig}")
    
    if sig != b'STID':
        return []
    
    section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
    unknown = struct.unpack('<I', data[offset+8:offset+12])[0]
    num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
    
    print(f"    Section size: {section_size}")
    print(f"    Num entries: {num_entries}")
    
    entries = []
    entry_offset = offset + 16
    
    for i in range(num_entries):
        if entry_offset >= offset + 8 + section_size:
            print(f"    Reached end of section at entry {i}")
            break
        
        # Read ID (4 bytes, little-endian)
        entry_id = struct.unpack('<I', data[entry_offset:entry_offset+4])[0]
        entry_offset += 4
        
        # Read null-terminated string
        name_bytes = b''
        while entry_offset < len(data) and data[entry_offset] != 0:
            name_bytes += bytes([data[entry_offset]])
            entry_offset += 1
        
        entry_offset += 1  # Skip null terminator
        
        try:
            name = name_bytes.decode('ascii')
            print(f"    Entry {i}: ID=0x{entry_id:08X}, Name='{name}'")
            entries.append({'id': entry_id, 'name': name})
        except Exception as e:
            print(f"    Entry {i}: Error decoding - {e}")
    
    return entries

# Test on a few files
test_files = [
    'extracted_bnk/bank_000_id_0165A2BF_le.bnk',
    'extracted_bnk/bank_024_id_05174939_le.bnk',
    'extracted_bnk/bank_025_id_09C7E577_le.bnk',
]

print("="*80)
print("TESTING BNK PARSING")
print("="*80)

for filepath in test_files:
    if not os.path.exists(filepath):
        print(f"\n✗ File not found: {filepath}")
        continue
    
    print(f"\n{'='*80}")
    print(f"Parsing: {filepath}")
    print('='*80)
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    print(f"File size: {len(data)} bytes")
    
    # Parse BKHD
    if data[0:4] != b'BKHD':
        print("✗ Not a BNK file!")
        continue
    
    bank_id = struct.unpack('<I', data[12:16])[0]
    print(f"Bank ID: 0x{bank_id:08X}")
    
    # Find STID sections
    print(f"\nSearching for STID sections...")
    offset = 0
    stid_count = 0
    
    while True:
        pos = data.find(b'STID', offset)
        if pos == -1:
            break
        
        print(f"\nFound STID at offset 0x{pos:08X}")
        entries = parse_stid_section(data, pos)
        print(f"  Extracted {len(entries)} entries")
        
        stid_count += 1
        offset = pos + 4
    
    print(f"\nTotal STID sections: {stid_count}")

