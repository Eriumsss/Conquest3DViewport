#!/usr/bin/env python3
"""Analyze Action payloads to find WEM references"""
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

wem_ids = set(didx.keys())
print(f"DIDX WEM IDs: {len(wem_ids)}")
print(f"Sample WEM IDs: {[f'0x{x:08X}' for x in list(wem_ids)[:5]]}\n")

# Analyze Actions
actions = [o for o in hirc if o["type"] == 3]
print(f"Actions: {len(actions)}\n")

for i, action in enumerate(actions[:5]):
    print(f"Action {i} (id=0x{action['id']:08X}):")
    print(f"  Payload size: {len(action['payload'])} bytes")
    print(f"  Payload hex: {action['payload'].hex()}")
    
    # Extract all u32s
    u32s = []
    for j in range(0, len(action['payload']), 4):
        if j + 4 <= len(action['payload']):
            val = struct.unpack_from("<I", action['payload'], j)[0]
            u32s.append(val)
    
    print(f"  U32 values: {[f'0x{x:08X}' for x in u32s]}")
    
    # Check which match WEM IDs
    matches = [x for x in u32s if x in wem_ids]
    if matches:
        print(f"  ✓ WEM matches: {[f'0x{x:08X}' for x in matches]}")
    else:
        print(f"  ✗ No WEM matches")
    
    print()

# Try to find ANY u32 in ANY action that matches a WEM ID
print("="*80)
print("Searching all actions for WEM ID matches...")
found_matches = 0
for action in actions:
    u32s = []
    for j in range(0, len(action['payload']), 4):
        if j + 4 <= len(action['payload']):
            val = struct.unpack_from("<I", action['payload'], j)[0]
            u32s.append(val)
    
    matches = [x for x in u32s if x in wem_ids]
    if matches:
        found_matches += 1
        print(f"Action 0x{action['id']:08X}: {len(matches)} WEM matches")
        for m in matches:
            print(f"  0x{m:08X}")

print(f"\nTotal actions with WEM matches: {found_matches}/{len(actions)}")

