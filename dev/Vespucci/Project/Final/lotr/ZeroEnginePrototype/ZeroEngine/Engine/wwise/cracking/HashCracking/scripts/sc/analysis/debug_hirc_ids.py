#!/usr/bin/env python3
"""Check if HIRC object IDs match DIDX WEM IDs"""
import struct
import glob

def find_chunk(data, tag):
    i = 0
    while i + 8 <= len(data):
        if data[i:i+4] == tag:
            size = struct.unpack_from('<I', data, i+4)[0]
            return (i, size, i+8, i+8+size)
        i += 1
    return None

def parse_didx(data):
    c = find_chunk(data, b"DIDX")
    if not c:
        return {}
    _, size, start, end = c
    didx = {}
    p = start
    while p + 12 <= end:
        wem_id, off, sz = struct.unpack_from("<III", data, p)
        didx[wem_id] = {"offset": off, "size": sz}
        p += 12
    return didx

def parse_hirc(data):
    c = find_chunk(data, b"HIRC")
    if not c:
        return []
    _, size, start, end = c
    p = start
    obj_count = struct.unpack_from("<I", data, p)[0]
    p += 4
    out = []
    for _ in range(obj_count):
        if p + 9 > end:
            break
        obj_type = struct.unpack_from("<B", data, p)[0]
        p += 1
        p += 3
        obj_size = struct.unpack_from("<I", data, p)[0]
        p += 4
        if p + obj_size > end:
            break
        obj_id = struct.unpack_from("<I", data, p)[0]
        payload = data[p+4 : p+obj_size]
        out.append({"type": obj_type, "id": obj_id, "payload": payload})
        p += obj_size
    return out

# Get first BNK with DIDX
bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
bnk_path = None
for path in bnk_files:
    with open(path, "rb") as f:
        if find_chunk(f.read(), b"DIDX"):
            bnk_path = path
            break

with open(bnk_path, "rb") as f:
    data = f.read()

print(f"Analyzing: {bnk_path}\n")

didx = parse_didx(data)
hirc = parse_hirc(data)

print(f"DIDX WEM IDs: {len(didx)}")
wem_ids = set(didx.keys())
print(f"  Sample: {[f'0x{x:08X}' for x in list(wem_ids)[:5]]}")

print(f"\nHIRC object IDs by type:")
for obj_type in [1, 2, 3, 4, 5, 7, 14]:
    objs = [o for o in hirc if o["type"] == obj_type]
    if objs:
        obj_ids = [o["id"] for o in objs]
        print(f"  Type {obj_type:2d}: {len(objs):3d} objects")
        print(f"    Sample: {[f'0x{x:08X}' for x in obj_ids[:3]]}")
        
        # Check if any match DIDX
        matches = [x for x in obj_ids if x in wem_ids]
        if matches:
            print(f"    ✓ {len(matches)} match DIDX WEM IDs!")
            print(f"      Matches: {[f'0x{x:08X}' for x in matches[:5]]}")

# Check if any HIRC object ID is in DIDX
all_hirc_ids = set(o["id"] for o in hirc)
matching_ids = all_hirc_ids & wem_ids
print(f"\nTotal HIRC object IDs: {len(all_hirc_ids)}")
print(f"Total DIDX WEM IDs: {len(wem_ids)}")
print(f"Matching IDs: {len(matching_ids)}")
if matching_ids:
    print(f"  Matches: {[f'0x{x:08X}' for x in list(matching_ids)[:10]]}")

