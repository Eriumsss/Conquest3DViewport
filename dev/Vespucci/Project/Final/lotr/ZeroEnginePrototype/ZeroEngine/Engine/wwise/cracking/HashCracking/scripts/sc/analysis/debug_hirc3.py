#!/usr/bin/env python3
"""Check if BNK is big-endian"""
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
        hirc_size_le = struct.unpack_from("<I", data, i+4)[0]
        hirc_size_be = struct.unpack_from(">I", data, i+4)[0]
        hirc_start = i + 8
        break
    i += 1

print(f"HIRC chunk size:")
print(f"  LE: 0x{hirc_size_le:08X} ({hirc_size_le})")
print(f"  BE: 0x{hirc_size_be:08X} ({hirc_size_be})")
print(f"  File size: {len(data)}")
print(f"  HIRC start: 0x{hirc_start:08X}")

# Check which makes sense
if hirc_start + hirc_size_le <= len(data):
    print(f"  ✓ LE size fits in file")
else:
    print(f"  ✗ LE size overflows")

if hirc_start + hirc_size_be <= len(data):
    print(f"  ✓ BE size fits in file")
else:
    print(f"  ✗ BE size overflows")

# Parse with LE (since filename says _le)
hirc_end = hirc_start + hirc_size_le
p = hirc_start

obj_count = struct.unpack_from("<I", data, p)[0]
print(f"\nObject count (LE): {obj_count}")
p += 4

print(f"\nParsing objects with LE size:")
for obj_idx in range(min(10, obj_count)):
    if p + 9 > hirc_end:
        print(f"  Object {obj_idx}: TRUNCATED")
        break
    
    obj_type = data[p]
    obj_size = struct.unpack_from("<I", data, p+1)[0]
    obj_id = struct.unpack_from("<I", data, p+5)[0]
    
    print(f"  Object {obj_idx}: type={obj_type:2d}, size=0x{obj_size:08X}, id=0x{obj_id:08X}", end="")
    
    if p + 9 + obj_size <= hirc_end:
        print(f" ✓")
        p += 9 + obj_size
    else:
        print(f" ✗ OVERFLOW")
        break

