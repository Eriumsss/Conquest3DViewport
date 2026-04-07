#!/usr/bin/env python3
"""Understand the exact byte layout"""
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
        hirc_start = i + 8
        break
    i += 1

p = hirc_start

# Read object count
obj_count_bytes = data[p:p+4]
print(f"Object count bytes: {' '.join(f'{b:02X}' for b in obj_count_bytes)}")
obj_count_le = struct.unpack_from("<I", data, p)[0]
obj_count_be = struct.unpack_from(">I", data, p)[0]
print(f"  LE: {obj_count_le}")
print(f"  BE: {obj_count_be}")
p += 4

# First object
print(f"\nFirst object bytes:")
obj_bytes = data[p:p+20]
print(f"  {' '.join(f'{b:02X}' for b in obj_bytes)}")

# Parse manually
print(f"\nManual parsing:")
print(f"  Byte 0 (type): 0x{data[p]:02X} = {data[p]}")
print(f"  Bytes 1-4 (size): {' '.join(f'{b:02X}' for b in data[p+1:p+5])}")

# The issue: I'm reading bytes 1-4 as size, but maybe it's structured differently
# Let me check if it's: type(1) + padding(3) + size(4) + id(4)
print(f"\nAlternative: type(1) + padding(3) + size(4) + id(4)")
alt_type = data[p]
alt_size = struct.unpack_from("<I", data, p+4)[0]
alt_id = struct.unpack_from("<I", data, p+8)[0]
print(f"  type: 0x{alt_type:02X}")
print(f"  size: 0x{alt_size:08X}")
print(f"  id: 0x{alt_id:08X}")

# Or maybe: type(1) + size(4) + id(4) but size is at offset 1
print(f"\nCorrect: type(1) + size(4) + id(4)")
corr_type = data[p]
corr_size = struct.unpack_from("<I", data, p+1)[0]
corr_id = struct.unpack_from("<I", data, p+5)[0]
print(f"  type: 0x{corr_type:02X}")
print(f"  size: 0x{corr_size:08X}")
print(f"  id: 0x{corr_id:08X}")

# Wait, the bytes are: 0E 00 00 00 5E 00 00 00 2B 41 A7 29
# If type=0x0E (1 byte), then next 4 bytes are: 00 00 00 5E
# That's 0x5E000000 in LE, or 0x0000005E in BE
# So the size should be read as BE!

print(f"\nWith BIG-ENDIAN size:")
be_type = data[p]
be_size = struct.unpack_from(">I", data, p+1)[0]
be_id = struct.unpack_from(">I", data, p+5)[0]
print(f"  type: 0x{be_type:02X}")
print(f"  size: 0x{be_size:08X}")
print(f"  id: 0x{be_id:08X}")

# Hmm, that gives id=0x2B41A729. Let me check if the structure is different
# Maybe it's: type(1) + size(4) + id(4) but we need to skip padding?

print(f"\nLet me check the actual structure by looking at multiple objects:")
p = hirc_start + 4
for i in range(5):
    print(f"\nObject {i} @ offset 0x{p:08X}:")
    print(f"  Bytes: {' '.join(f'{b:02X}' for b in data[p:p+16])}")
    
    # Try: type(1) + size(4) + id(4)
    t = data[p]
    s = struct.unpack_from(">I", data, p+1)[0]  # BE
    id_val = struct.unpack_from(">I", data, p+5)[0]  # BE
    print(f"  Parsed (BE): type=0x{t:02X}, size=0x{s:08X}, id=0x{id_val:08X}")
    
    if p + 9 + s <= hirc_start + 10447:
        p += 9 + s
    else:
        print(f"  Would overflow, stopping")
        break

