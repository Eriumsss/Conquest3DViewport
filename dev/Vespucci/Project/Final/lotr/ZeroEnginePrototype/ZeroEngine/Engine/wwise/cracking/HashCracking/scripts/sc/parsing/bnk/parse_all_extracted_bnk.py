#!/usr/bin/env python3
"""
Parse all 296 extracted BNK files and extract STID names
"""
import struct
import os
import glob
from collections import defaultdict

def parse_stid_section(data, offset):
    """Parse STID section (little-endian PC format)"""
    try:
        sig = data[offset:offset+4]
        if sig != b'STID':
            return []
        
        section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
        unknown = struct.unpack('<I', data[offset+8:offset+12])[0]
        num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
        
        entries = []
        entry_offset = offset + 16
        
        for i in range(num_entries):
            if entry_offset >= offset + 8 + section_size:
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
                entries.append({'id': entry_id, 'name': name})
            except:
                pass
        
        return entries
    except Exception as e:
        return []

def parse_hirc_section(data, offset):
    """Count HIRC objects"""
    try:
        sig = data[offset:offset+4]
        if sig != b'HIRC':
            return 0
        
        num_objects = struct.unpack('<I', data[offset+8:offset+12])[0]
        return num_objects
    except:
        return 0

def parse_bnk_file(filepath):
    """Parse a single BNK file"""
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        
        # Parse BKHD to get bank ID
        if data[0:4] != b'BKHD':
            return None
        
        bank_id = struct.unpack('<I', data[12:16])[0]
        
        # Find all STID sections
        all_names = []
        offset = 0
        while True:
            pos = data.find(b'STID', offset)
            if pos == -1:
                break
            entries = parse_stid_section(data, pos)
            all_names.extend(entries)
            offset = pos + 4
        
        # Count HIRC objects
        hirc_count = 0
        offset = 0
        while True:
            pos = data.find(b'HIRC', offset)
            if pos == -1:
                break
            hirc_count += parse_hirc_section(data, pos)
            offset = pos + 4
        
        return {
            'filepath': filepath,
            'bank_id': bank_id,
            'size': len(data),
            'stid_names': all_names,
            'hirc_count': hirc_count
        }
    except Exception as e:
        print(f"Error parsing {filepath}: {e}")
        return None

print("="*100)
print("PARSING ALL 296 EXTRACTED BNK FILES")
print("="*100)

# Find all extracted BNK files
bnk_files = sorted(glob.glob('extracted_bnk/bank_*.bnk'))
print(f"\nFound {len(bnk_files)} BNK files\n")

results = []
unique_names = set()
names_by_bank_id = defaultdict(list)

for idx, bnk_file in enumerate(bnk_files):
    if idx % 50 == 0:
        print(f"Processing {idx+1}/{len(bnk_files)}...")
    
    result = parse_bnk_file(bnk_file)
    if result:
        results.append(result)
        for name_entry in result['stid_names']:
            unique_names.add(name_entry['name'])
            names_by_bank_id[result['bank_id']].append(name_entry['name'])

print(f"\n{'='*100}")
print("RESULTS")
print('='*100)

print(f"\n✓ Successfully parsed: {len(results)} BNK files")
print(f"✓ Unique STID names found: {len(unique_names)}")
print(f"✓ Banks with names: {len(names_by_bank_id)}")

print(f"\n{'='*100}")
print("UNIQUE STID NAMES")
print('='*100)

for name in sorted(unique_names):
    print(f"  {name}")

print(f"\n{'='*100}")
print("BANKS WITH THEIR NAMES")
print('='*100)

for bank_id in sorted(names_by_bank_id.keys()):
    names = names_by_bank_id[bank_id]
    print(f"\nBank ID: 0x{bank_id:08X}")
    for name in sorted(set(names)):
        print(f"  - {name}")

# Save detailed report
with open('extracted_bnk_names_report.txt', 'w') as f:
    f.write("="*100 + "\n")
    f.write("EXTRACTED BNK FILES - STID NAMES REPORT\n")
    f.write("="*100 + "\n\n")
    
    f.write(f"Total BNK files parsed: {len(results)}\n")
    f.write(f"Unique STID names: {len(unique_names)}\n")
    f.write(f"Banks with names: {len(names_by_bank_id)}\n\n")
    
    f.write("="*100 + "\n")
    f.write("ALL UNIQUE NAMES\n")
    f.write("="*100 + "\n\n")
    
    for name in sorted(unique_names):
        f.write(f"{name}\n")
    
    f.write("\n" + "="*100 + "\n")
    f.write("BANKS AND THEIR NAMES\n")
    f.write("="*100 + "\n\n")
    
    for bank_id in sorted(names_by_bank_id.keys()):
        names = names_by_bank_id[bank_id]
        f.write(f"Bank ID: 0x{bank_id:08X}\n")
        for name in sorted(set(names)):
            f.write(f"  - {name}\n")
        f.write("\n")

print(f"\n✓ Report saved to: extracted_bnk_names_report.txt")

