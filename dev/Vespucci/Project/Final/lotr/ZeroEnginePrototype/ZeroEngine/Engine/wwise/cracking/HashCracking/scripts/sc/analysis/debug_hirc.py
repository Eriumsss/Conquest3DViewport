#!/usr/bin/env python3
"""Debug HIRC parsing on a single BNK file"""
import struct
import glob

def find_chunk(data: bytes, tag: bytes):
    """Find chunk by tag"""
    i = 0
    end = len(data)
    while i + 8 <= end:
        if data[i:i+4] == tag:
            size = struct.unpack_from("<I", data, i+4)[0]
            payload_start = i + 8
            payload_end = payload_start + size
            return (i, size, payload_start, payload_end)
        i += 1
    return None

# Get first BNK file
bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
bnk_path = bnk_files[0]

print(f"Analyzing: {bnk_path}")

with open(bnk_path, "rb") as f:
    data = f.read()

print(f"File size: {len(data)} bytes")

# Find all chunks
for tag in [b"BKHD", b"DIDX", b"HIRC", b"DATA", b"STID"]:
    c = find_chunk(data, tag)
    if c:
        offset, size, start, end = c
        print(f"\n{tag.decode()}: offset=0x{offset:08X}, size=0x{size:08X}, payload=[0x{start:08X}, 0x{end:08X})")
    else:
        print(f"\n{tag.decode()}: NOT FOUND")

# Deep dive into HIRC
print("\n" + "="*80)
print("HIRC DEEP DIVE")
print("="*80)

c = find_chunk(data, b"HIRC")
if c:
    offset, size, start, end = c
    p = start
    
    # Read object count
    obj_count = struct.unpack_from("<I", data, p)[0]
    print(f"Object count: {obj_count}")
    p += 4
    
    # Read first 10 objects - try both endianness
    print("\nTrying LITTLE-ENDIAN:")
    p_test = start + 4
    for i in range(min(3, obj_count)):
        if p_test + 5 > end:
            break
        obj_type = struct.unpack_from("<B", data, p_test)[0]
        obj_size_le = struct.unpack_from("<I", data, p_test+1)[0]
        print(f"  Object {i}: type={obj_type}, size_le=0x{obj_size_le:08X}")
        p_test += 5 + obj_size_le

    print("\nTrying BIG-ENDIAN:")
    p_test = start + 4
    for i in range(min(3, obj_count)):
        if p_test + 5 > end:
            break
        obj_type = struct.unpack_from("<B", data, p_test)[0]
        obj_size_be = struct.unpack_from(">I", data, p_test+1)[0]
        print(f"  Object {i}: type={obj_type}, size_be=0x{obj_size_be:08X}")
        if p_test + 5 + obj_size_be <= end:
            p_test += 5 + obj_size_be
        else:
            break

    # Now parse with correct endianness
    print("\nParsing with BIG-ENDIAN (appears correct):")
    p = start + 4
    for i in range(min(10, obj_count)):
        if p + 5 > end:
            print(f"  Object {i}: TRUNCATED")
            break

        obj_type = struct.unpack_from("<B", data, p)[0]
        p += 1
        obj_size = struct.unpack_from(">I", data, p)[0]  # BIG-ENDIAN
        p += 4

        if p + obj_size > end:
            print(f"  Object {i}: SIZE OVERFLOW (size=0x{obj_size:08X}, remaining=0x{end-p:08X})")
            break

        obj_id = struct.unpack_from(">I", data, p)[0]  # BIG-ENDIAN
        payload = data[p+4 : p+obj_size]

        print(f"  Object {i}: type={obj_type}, size=0x{obj_size:08X}, id=0x{obj_id:08X}, payload_len={len(payload)}")

        p += obj_size

# Check DIDX
print("\n" + "="*80)
print("DIDX DEEP DIVE")
print("="*80)

c = find_chunk(data, b"DIDX")
if c:
    offset, size, start, end = c
    p = start
    
    entry_count = (end - start) // 12
    print(f"DIDX entries: {entry_count}")
    
    for i in range(min(5, entry_count)):
        wem_id, off, sz = struct.unpack_from("<III", data, p)
        print(f"  Entry {i}: wem_id=0x{wem_id:08X}, offset=0x{off:08X}, size=0x{sz:08X}")
        p += 12

