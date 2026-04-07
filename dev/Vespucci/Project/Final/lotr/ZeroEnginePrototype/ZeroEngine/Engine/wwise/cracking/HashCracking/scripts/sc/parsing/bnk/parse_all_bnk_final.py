#!/usr/bin/env python3
import struct
import os
import glob
from collections import defaultdict

def parse_bnk_file(filepath):
    """Parse a single BNK file and extract STID names"""
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        
        # Get bank ID
        bank_id = struct.unpack('<I', data[12:16])[0]
        
        # Find STID
        pos = data.find(b'STID')
        if pos == -1:
            return {'bank_id': bank_id, 'names': [], 'size': len(data)}
        
        section_size = struct.unpack('<I', data[pos+4:pos+8])[0]
        num_entries = struct.unpack('<I', data[pos+12:pos+16])[0]
        
        names = []
        entry_offset = pos + 16
        
        for i in range(num_entries):
            if entry_offset >= pos + 8 + section_size:
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
                names.append(name)
            except:
                pass
        
        return {'bank_id': bank_id, 'names': names, 'size': len(data)}
    except Exception as e:
        return None

# Parse all BNK files
bnk_files = sorted(glob.glob('extracted_bnk/bank_*.bnk'))

results = []
unique_names = set()
names_by_bank_id = defaultdict(list)

for filepath in bnk_files:
    result = parse_bnk_file(filepath)
    if result:
        results.append(result)
        for name in result['names']:
            unique_names.add(name)
            names_by_bank_id[result['bank_id']].append(name)

# Write comprehensive report
with open('bnk_names_complete_report.txt', 'w') as f:
    f.write("="*100 + "\n")
    f.write("COMPLETE BNK EXTRACTION REPORT\n")
    f.write("="*100 + "\n\n")
    
    f.write(f"Total BNK files parsed: {len(results)}\n")
    f.write(f"Unique STID names found: {len(unique_names)}\n")
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

print("Done!")

