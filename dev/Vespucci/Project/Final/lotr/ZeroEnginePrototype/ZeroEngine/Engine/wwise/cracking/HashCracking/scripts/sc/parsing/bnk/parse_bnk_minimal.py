#!/usr/bin/env python3
import struct

# Open first BNK file
with open('extracted_bnk/bank_000_id_0165A2BF_le.bnk', 'rb') as f:
    data = f.read()

# Parse BKHD
bank_id = struct.unpack('<I', data[12:16])[0]

# Find STID
pos = data.find(b'STID')

result = []
result.append(f"File: bank_000_id_0165A2BF_le.bnk")
result.append(f"Size: {len(data)} bytes")
result.append(f"Bank ID: 0x{bank_id:08X}")
result.append(f"STID found at: 0x{pos:08X}" if pos != -1 else "STID: NOT FOUND")

if pos != -1:
    section_size = struct.unpack('<I', data[pos+4:pos+8])[0]
    num_entries = struct.unpack('<I', data[pos+12:pos+16])[0]
    
    result.append(f"STID section size: {section_size}")
    result.append(f"STID entries: {num_entries}")
    
    # Parse entries
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
            result.append(f"  Entry {i}: ID=0x{entry_id:08X}, Name='{name}'")
        except:
            result.append(f"  Entry {i}: ERROR decoding")

# Write to file
with open('parse_result.txt', 'w') as f:
    f.write('\n'.join(result))

