#!/usr/bin/env python3
"""Debug HIRC mapping on a single BNK"""
import struct
import glob

def find_chunk(data: bytes, tag: bytes):
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

def parse_didx(data: bytes):
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

def parse_hirc(data: bytes):
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
        p += 3  # padding
        obj_size = struct.unpack_from("<I", data, p)[0]
        p += 4
        if p + obj_size > end:
            break
        obj_id = struct.unpack_from("<I", data, p)[0]
        payload = data[p+4 : p+obj_size]
        out.append({"type": obj_type, "id": obj_id, "payload": payload})
        p += obj_size
    return out

def u32_list(payload):
    if len(payload) < 4:
        return []
    count = len(payload) // 4
    if count == 0:
        return []
    try:
        return list(struct.unpack("<" + "I"*count, payload[:count*4]))
    except struct.error:
        return []

# Get first BNK with DIDX
bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
bnk_path = None
for path in bnk_files:
    with open(path, "rb") as f:
        if find_chunk(f.read(), b"DIDX"):
            bnk_path = path
            break

if not bnk_path:
    print("No BNK files with DIDX found!")
    exit(1)

with open(bnk_path, "rb") as f:
    data = f.read()

print(f"Analyzing: {bnk_path}")

didx = parse_didx(data)
hirc = parse_hirc(data)

print(f"\nDIDX entries: {len(didx)}")
if didx:
    for wem_id, info in list(didx.items())[:5]:
        print(f"  0x{wem_id:08X}: offset=0x{info['offset']:08X}, size=0x{info['size']:08X}")

# Index by type
events = {o["id"]: o for o in hirc if o["type"] == 2}
actions = {o["id"]: o for o in hirc if o["type"] == 3}
sounds = {o["id"]: o for o in hirc if o["type"] == 1}

print(f"\nHIRC objects:")
print(f"  Events (type 2): {len(events)}")
print(f"  Actions (type 3): {len(actions)}")
print(f"  Sounds (type 1): {len(sounds)}")
print(f"  Other types: {len(hirc) - len(events) - len(actions) - len(sounds)}")

# Show type distribution
type_counts = {}
for o in hirc:
    t = o["type"]
    type_counts[t] = type_counts.get(t, 0) + 1

print(f"\nType distribution:")
for t in sorted(type_counts.keys()):
    print(f"  Type {t:2d}: {type_counts[t]:5d}")

# Try to find mappings
print(f"\n" + "="*80)
print("MAPPING ANALYSIS")
print("="*80)

if events:
    ev_id, ev_obj = list(events.items())[0]
    print(f"\nFirst Event (id=0x{ev_id:08X}):")
    print(f"  Payload size: {len(ev_obj['payload'])} bytes")
    print(f"  Payload hex: {ev_obj['payload'][:32].hex()}")
    
    u32s = u32_list(ev_obj["payload"])
    print(f"  U32 values: {[f'0x{x:08X}' for x in u32s[:10]]}")
    
    # Check which are actions
    action_refs = [x for x in u32s if x in actions]
    print(f"  Action references: {[f'0x{x:08X}' for x in action_refs]}")

if actions:
    act_id, act_obj = list(actions.items())[0]
    print(f"\nFirst Action (id=0x{act_id:08X}):")
    print(f"  Payload size: {len(act_obj['payload'])} bytes")
    print(f"  Payload hex: {act_obj['payload'][:32].hex()}")
    
    u32s = u32_list(act_obj["payload"])
    print(f"  U32 values: {[f'0x{x:08X}' for x in u32s[:10]]}")
    
    # Check which are sounds or WEM IDs
    sound_refs = [x for x in u32s if x in sounds]
    wem_refs = [x for x in u32s if x in didx]
    print(f"  Sound references: {[f'0x{x:08X}' for x in sound_refs]}")
    print(f"  WEM references: {[f'0x{x:08X}' for x in wem_refs]}")

if sounds:
    snd_id, snd_obj = list(sounds.items())[0]
    print(f"\nFirst Sound (id=0x{snd_id:08X}):")
    print(f"  Payload size: {len(snd_obj['payload'])} bytes")
    print(f"  Payload hex: {snd_obj['payload'][:32].hex()}")
    
    u32s = u32_list(snd_obj["payload"])
    print(f"  U32 values: {[f'0x{x:08X}' for x in u32s[:10]]}")
    
    # Check which are WEM IDs
    wem_refs = [x for x in u32s if x in didx]
    print(f"  WEM references: {[f'0x{x:08X}' for x in wem_refs]}")

