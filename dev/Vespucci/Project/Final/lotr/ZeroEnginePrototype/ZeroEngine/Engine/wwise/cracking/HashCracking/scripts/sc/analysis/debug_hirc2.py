#!/usr/bin/env python3
"""Debug HIRC structure more carefully"""
import struct
import glob

bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
bnk_path = bnk_files[0]

with open(bnk_path, "rb") as f:
    data = f.read()

# Find HIRC
i = 0
while i + 8 <= len(data):
    if data[i:i+4] == b"HIRC":
        hirc_offset = i
        hirc_size = struct.unpack_from("<I", data, i+4)[0]
        hirc_start = i + 8
        hirc_end = hirc_start + hirc_size
        break
    i += 1

print(f"HIRC: offset=0x{hirc_offset:08X}, size=0x{hirc_size:08X}")
print(f"Payload: 0x{hirc_start:08X} - 0x{hirc_end:08X}")

# Dump raw bytes
print("\nRaw bytes at HIRC payload start:")
for i in range(0, 100, 16):
    hex_str = ' '.join(f'{b:02X}' for b in data[hirc_start+i:hirc_start+i+16])
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[hirc_start+i:hirc_start+i+16])
    print(f"0x{hirc_start+i:08X}: {hex_str:<48s} {ascii_str}")

# Parse object count
obj_count = struct.unpack_from("<I", data, hirc_start)[0]
print(f"\nObject count: {obj_count}")

# Try to understand the structure
p = hirc_start + 4
print(f"\nFirst 20 bytes after count:")
for i in range(20):
    print(f"  +{i:2d}: 0x{data[p+i]:02X}")

# Try parsing as: type(1) + size(4) + id(4) + payload
print("\n\nTrying: type(1) + size(4) + id(4) + payload")
p = hirc_start + 4
for obj_idx in range(min(5, obj_count)):
    if p + 9 > hirc_end:
        break
    
    obj_type = data[p]
    obj_size_le = struct.unpack_from("<I", data, p+1)[0]
    obj_size_be = struct.unpack_from(">I", data, p+1)[0]
    obj_id_le = struct.unpack_from("<I", data, p+5)[0]
    obj_id_be = struct.unpack_from(">I", data, p+5)[0]
    
    print(f"\nObject {obj_idx} @ 0x{p:08X}:")
    print(f"  type: 0x{obj_type:02X}")
    print(f"  size (LE): 0x{obj_size_le:08X}")
    print(f"  size (BE): 0x{obj_size_be:08X}")
    print(f"  id (LE): 0x{obj_id_le:08X}")
    print(f"  id (BE): 0x{obj_id_be:08X}")
    
    # Try to advance with LE size
    if p + 9 + obj_size_le <= hirc_end:
        print(f"  ✓ LE size works (next @ 0x{p+9+obj_size_le:08X})")
        p += 9 + obj_size_le
    else:
        print(f"  ✗ LE size overflow")
        break

