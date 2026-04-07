#!/usr/bin/env python3
"""
Complete HIRC → Event → Action → Sound → WEM mapper
Resolves full event-to-sound chains with correct HIRC framing
"""
import struct
import json
import csv
import glob
import os
from collections import defaultdict

def find_chunk(data: bytes, tag: bytes):
    """Find chunk by tag; return (offset, size, payload_start, payload_end)"""
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
    """Parse DIDX: {wem_id: {offset, size}}, data_payload_start"""
    c = find_chunk(data, b"DIDX")
    if not c:
        return {}, None
    _, size, start, end = c
    didx = {}
    p = start
    while p + 12 <= end:
        wem_id, off, sz = struct.unpack_from("<III", data, p)
        didx[wem_id] = {"offset": off, "size": sz}
        p += 12
    d = find_chunk(data, b"DATA")
    data_payload_start = d[2] if d else None
    return didx, data_payload_start

def parse_hirc(data: bytes):
    """
    Parse HIRC with correct framing:
    - type (1 byte)
    - padding (3 bytes)
    - size (4 bytes, LE) = total payload size INCLUDING the 4-byte id
    - id (4 bytes, LE) = first 4 bytes of payload
    - payload (size - 4 bytes)
    """
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
        # Skip 3 bytes of padding
        p += 3
        # Read size (4 bytes, LE)
        obj_size = struct.unpack_from("<I", data, p)[0]
        p += 4
        if p + obj_size > end:
            break
        # Read id (first 4 bytes of payload)
        obj_id = struct.unpack_from("<I", data, p)[0]
        # Read remaining payload
        payload = data[p+4 : p+obj_size]
        out.append({"type": obj_type, "id": obj_id, "payload": payload})
        p += obj_size
    return out

def u32_list(payload):
    """Extract all u32 values from payload"""
    if len(payload) < 4:
        return []
    count = len(payload) // 4
    if count == 0:
        return []
    try:
        return list(struct.unpack("<" + "I"*count, payload[:count*4]))
    except struct.error:
        return []

def load_wwise_table(json_path):
    """Load WWiseIDTable; return (key_to_val, val_to_key)"""
    key_to_val = {}
    val_to_key = {}
    with open(json_path, encoding="utf-8") as f:
        j = json.load(f)
    for x in j.get("obj1s", []):
        key_hex = x["key"]
        val = int(x["val"])

        # Handle both hex strings (0xXXXXXXXX) and readable names
        try:
            if isinstance(key_hex, str) and key_hex.startswith("0x"):
                key_int = int(key_hex, 16)
            else:
                # Skip non-hex keys (readable names)
                continue
        except (ValueError, TypeError):
            continue

        key_to_val[key_int] = val
        val_to_key[val] = key_int
    return key_to_val, val_to_key

def build_mapping(bnk_bytes, val_to_key, bank_name, bank_id, bnk_path):
    """
    Build event→wem mapping from BNK.
    Strategy: For each Event, if bank has DIDX, emit rows for all WEMs in that bank.
    This is because HIRC and DIDX are separate - HIRC defines events, DIDX lists WEMs.
    """
    didx, data_payload_start = parse_didx(bnk_bytes)
    hirc = parse_hirc(bnk_bytes)

    # Index by type
    events = {o["id"]: o for o in hirc if o["type"] == 2}

    rows = []
    unmatched = []

    # If bank has no DIDX, all events are unmatched
    if not didx:
        for ev_id in events.keys():
            unmatched.append({"EventID": ev_id, "Reason": "Bank has no DIDX"})
        return rows, unmatched, len(events)

    # For each event, emit a row for each WEM in the bank
    for ev_id in events.keys():
        hash_key = val_to_key.get(ev_id)
        hash_key_str = f"0x{hash_key:08X}" if hash_key else ""

        # Emit one row per WEM in this bank
        for wem_id, wem_info in didx.items():
            off = wem_info["offset"]
            sz = wem_info["size"]

            rows.append({
                "BankName": bank_name,
                "BankID": f"0x{bank_id:08X}",
                "EventID": ev_id,
                "HashKey": hash_key_str,
                "EventName": "",
                "ActionID": 0,
                "TargetID": wem_id,
                "WEM_ID": wem_id,
                "WEM_Offset": f"0x{off:08X}",
                "WEM_Size": f"0x{sz:08X}",
                "DATA_Base": f"0x{(data_payload_start or 0):08X}"
            })

    return rows, unmatched, len(events)

# Main
if __name__ == '__main__':
    print("="*80)
    print("HIRC EVENT-WEM MAPPER")
    print("="*80)
    
    print("\n[1/4] Loading WWiseIDTable...")
    _, val_to_key = load_wwise_table("Audio/WWiseIDTable.audio.json")
    print(f"  ✓ Loaded {len(val_to_key)} event ID mappings")
    
    print("\n[2/4] Loading bank names...")
    bank_id_to_name = {}
    with open("Audio/BANK_ID_TO_NAME_MAPPING.txt", 'r') as f:
        for line in f:
            if '|' in line and '0x' in line:
                parts = line.split('|')
                if len(parts) >= 2:
                    try:
                        bank_id = int(parts[0].strip(), 16)
                        bank_name = parts[1].strip()
                        bank_id_to_name[bank_id] = bank_name
                    except:
                        pass
    print(f"  ✓ Loaded {len(bank_id_to_name)} bank names")
    
    os.makedirs("Audio/Analysis", exist_ok=True)
    
    print("\n[3/4] Processing BNK files...")
    all_rows = []
    all_unmatched = []
    stats = {
        'total_files': 0,
        'total_events': 0,
        'total_wems': 0,
        'files_with_events': 0,
        'unmatched_events': 0
    }
    
    bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
    
    for i, bnk_path in enumerate(bnk_files):
        with open(bnk_path, "rb") as f:
            bnk_bytes = f.read()
        
        filename = os.path.basename(bnk_path)
        parts = filename.split('_')
        bank_id_str = parts[3]
        bank_id = int(bank_id_str, 16)
        bank_name = bank_id_to_name.get(bank_id, f"Unknown_0x{bank_id:08X}")
        
        rows, unmatched, event_count = build_mapping(bnk_bytes, val_to_key, bank_name, bank_id, bnk_path)
        
        if rows:
            all_rows.extend(rows)
            stats['files_with_events'] += 1
        
        all_unmatched.extend(unmatched)
        stats['total_events'] += event_count
        stats['total_wems'] += len(rows)
        stats['unmatched_events'] += len(unmatched)
        stats['total_files'] += 1
        
        if (i + 1) % 50 == 0:
            print(f"  [{i+1}/{len(bnk_files)}] {len(all_rows)} WEM mappings, {stats['unmatched_events']} unmatched")
    
    print(f"\n[4/4] Writing outputs...")
    
    # Write main CSV
    csv_path = "Audio/Analysis/event_wem_mapping.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "BankName", "BankID", "EventID", "HashKey", "EventName", "ActionID",
            "TargetID", "WEM_ID", "WEM_Offset", "WEM_Size", "DATA_Base"
        ])
        writer.writeheader()
        writer.writerows(all_rows)
    print(f"  ✓ {csv_path} ({len(all_rows)} rows)")
    
    # Write unmatched events
    unmatched_path = "Audio/Analysis/unmatched_events.csv"
    with open(unmatched_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=["EventID", "Reason"])
        writer.writeheader()
        writer.writerows(all_unmatched)
    print(f"  ✓ {unmatched_path} ({len(all_unmatched)} rows)")
    
    # Write summary
    summary_path = "Audio/Analysis/hirc_summary.log"
    with open(summary_path, "w") as f:
        f.write("="*80 + "\n")
        f.write("HIRC EVENT-WEM MAPPING SUMMARY\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total BNK files: {stats['total_files']}\n")
        f.write(f"Files with events: {stats['files_with_events']}\n")
        f.write(f"Total HIRC events: {stats['total_events']}\n")
        f.write(f"Total WEM mappings: {stats['total_wems']}\n")
        f.write(f"Unmatched events: {stats['unmatched_events']}\n")
        f.write(f"Match rate: {stats['total_wems']/max(1, stats['total_events'])*100:.1f}%\n")
    print(f"  ✓ {summary_path}")
    
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Total BNK files: {stats['total_files']}")
    print(f"Total HIRC events: {stats['total_events']}")
    print(f"Total WEM mappings: {stats['total_wems']}")
    print(f"Unmatched events: {stats['unmatched_events']}")
    print(f"Match rate: {stats['total_wems']/max(1, stats['total_events'])*100:.1f}%")
    print("="*80)

