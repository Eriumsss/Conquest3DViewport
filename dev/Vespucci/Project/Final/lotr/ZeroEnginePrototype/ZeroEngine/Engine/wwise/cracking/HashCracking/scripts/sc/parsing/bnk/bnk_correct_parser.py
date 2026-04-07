#!/usr/bin/env python3
"""
Correct BNK parser with proper HIRC and DIDX framing
Based on Wwise BNK format specification
"""
import struct
import json
import csv
import glob
import os
from collections import defaultdict

def find_chunk(data: bytes, tag: bytes):
    """Find a chunk by tag and return (offset, size, payload_start, payload_end)"""
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
    """
    Parse DIDX section.
    Return: (didx_dict, data_payload_start)
    didx_dict: {wem_id: {'offset': off, 'size': sz}}
    data_payload_start: absolute file offset where DATA payload begins
    """
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
    
    # Find DATA chunk start (for resolving absolute positions)
    d = find_chunk(data, b"DATA")
    data_payload_start = d[2] if d else None
    
    return didx, data_payload_start

def parse_hirc(data: bytes):
    """
    Parse HIRC section with correct framing.
    Return: list of dicts {'type': t, 'id': obj_id, 'payload': payload_bytes}
    
    Correct framing:
    - Read object_type (1 byte)
    - Read object_size (4 bytes) = total payload size INCLUDING the 4-byte id
    - Read object_id (4 bytes) = first 4 bytes of payload
    - Read remaining payload (object_size - 4 bytes)
    """
    c = find_chunk(data, b"HIRC")
    if not c:
        return []
    
    _, size, start, end = c
    p = start
    
    # Read object count
    obj_count = struct.unpack_from("<I", data, p)[0]
    p += 4
    
    out = []
    for _ in range(obj_count):
        if p + 5 > end:
            break
        
        # Read object type (1 byte)
        obj_type = struct.unpack_from("<B", data, p)[0]
        p += 1
        
        # Read object size (4 bytes) - this is the TOTAL payload size including id
        obj_size = struct.unpack_from("<I", data, p)[0]
        p += 4
        
        if p + obj_size > end:
            break
        
        # Read object id (first 4 bytes of payload)
        obj_id = struct.unpack_from("<I", data, p)[0]
        
        # Read remaining payload (obj_size - 4 bytes)
        payload = data[p+4 : p+obj_size]
        
        out.append({
            "type": obj_type,
            "id": obj_id,
            "payload": payload
        })
        
        p += obj_size
    
    return out

def load_wwise_table(json_path):
    """
    Load WWiseIDTable and create bidirectional maps.
    Return: (key_to_val, val_to_key)
    - key_to_val: {hash_int: event_id}
    - val_to_key: {event_id: hash_int}
    """
    key_to_val = {}
    val_to_key = {}
    
    with open(json_path, encoding="utf-8") as f:
        j = json.load(f)
    
    for x in j.get("obj1s", []):
        key_hex = x["key"]
        val = int(x["val"])
        key_int = int(key_hex, 16)
        
        key_to_val[key_int] = val
        val_to_key[val] = key_int
    
    return key_to_val, val_to_key

def u32_list(payload):
    """Extract all u32 values from payload"""
    return list(struct.unpack("<" + "I"*(len(payload)//4), payload))

def build_mapping(bnk_bytes, val_to_key, bank_name, bank_id):
    """
    Build event-to-WEM mapping from a single BNK file.
    Return: list of row dicts
    """
    # Parse DIDX and HIRC
    didx, data_payload_start = parse_didx(bnk_bytes)
    hirc = parse_hirc(bnk_bytes)
    
    # Index HIRC objects by type
    events = [o for o in hirc if o["type"] == 2]   # CAkEvent
    actions = [o for o in hirc if o["type"] == 3]  # CAkAction
    sounds = [o for o in hirc if o["type"] == 1]   # CAkSound
    
    # Build quick lookup maps
    action_by_id = {o["id"]: o for o in actions}
    sound_by_id = {o["id"]: o for o in sounds}
    
    # Build rows
    rows = []
    
    for ev in events:
        ev_id = ev["id"]
        hash_key = val_to_key.get(ev_id)
        hash_key_str = f"0x{hash_key:08X}" if hash_key is not None else ""
        
        # Heuristic: scan event payload for action IDs
        cand_actions = [x for x in u32_list(ev["payload"]) if x in action_by_id]
        
        for aid in cand_actions:
            act = action_by_id[aid]
            
            # Scan action payload for target IDs (sounds or WEM IDs)
            targets = [x for x in u32_list(act["payload"]) if (x in sound_by_id or x in didx)]
            
            for tid in targets:
                wem_id = None
                
                # If tid is directly in DIDX, it's a WEM ID
                if tid in didx:
                    wem_id = tid
                else:
                    # If tid is a sound object, scan its payload for WEM IDs
                    if tid in sound_by_id:
                        srcs = [x for x in u32_list(sound_by_id[tid]["payload"]) if x in didx]
                        if srcs:
                            wem_id = srcs[0]  # Take first WEM source
                
                if wem_id is not None:
                    off = didx[wem_id]["offset"]
                    sz = didx[wem_id]["size"]
                    
                    rows.append({
                        "BankName": bank_name,
                        "BankID": f"0x{bank_id:08X}",
                        "EventID": ev_id,
                        "HashKey": hash_key_str,
                        "ActionID": aid,
                        "TargetID": tid,
                        "WEM_ID": wem_id,
                        "WEM_Offset": f"0x{off:08X}",
                        "WEM_Size": f"0x{sz:08X}",
                        "DATA_Base": f"0x{(data_payload_start or 0):08X}"
                    })
    
    return rows

# Main execution
if __name__ == '__main__':
    print("Loading WWiseIDTable...")
    _, val_to_key = load_wwise_table("Audio/WWiseIDTable.audio.json")
    print(f"Loaded {len(val_to_key)} event ID mappings")
    
    # Load bank names
    bank_id_to_name = {}
    with open("Audio/BANK_ID_TO_NAME_MAPPING.txt", 'r') as f:
        for line in f:
            if '|' in line and '0x' in line:
                parts = line.split('|')
                if len(parts) >= 2:
                    bank_id_str = parts[0].strip()
                    bank_name = parts[1].strip()
                    try:
                        bank_id = int(bank_id_str, 16)
                        bank_id_to_name[bank_id] = bank_name
                    except:
                        pass
    
    print(f"Loaded {len(bank_id_to_name)} bank names")
    
    # Process all BNK files
    print("\nProcessing BNK files...")
    os.makedirs("Audio/Analysis", exist_ok=True)
    
    all_rows = []
    stats = {
        'total_files': 0,
        'total_events': 0,
        'total_wems': 0,
        'files_with_events': 0
    }
    
    bnk_files = sorted(glob.glob("Audio/extracted_bnk/*.bnk"))
    
    for i, bnk_path in enumerate(bnk_files):
        with open(bnk_path, "rb") as f:
            bnk_bytes = f.read()
        
        # Extract bank ID from filename
        filename = os.path.basename(bnk_path)
        # Format: bank_NNN_id_XXXXXXXX_le.bnk
        parts = filename.split('_')
        bank_id_str = parts[3]
        bank_id = int(bank_id_str, 16)
        bank_name = bank_id_to_name.get(bank_id, f"Unknown_0x{bank_id:08X}")
        
        # Build mapping
        rows = build_mapping(bnk_bytes, val_to_key, bank_name, bank_id)
        
        if rows:
            all_rows.extend(rows)
            stats['files_with_events'] += 1
            stats['total_events'] += len(rows)
        
        stats['total_files'] += 1
        
        if (i + 1) % 50 == 0:
            print(f"  Processed {i + 1}/{len(bnk_files)} files, {len(all_rows)} events found")
    
    # Write CSV
    print(f"\nWriting {len(all_rows)} rows to CSV...")
    csv_path = "Audio/Analysis/bnk_full_mapping.csv"
    
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "BankName", "BankID", "EventID", "HashKey", "ActionID", 
            "TargetID", "WEM_ID", "WEM_Offset", "WEM_Size", "DATA_Base"
        ])
        writer.writeheader()
        writer.writerows(all_rows)
    
    print(f"✓ Saved to {csv_path}")
    
    # Write summary
    summary_path = "Audio/Analysis/mapping_summary.log"
    with open(summary_path, "w") as f:
        f.write("="*80 + "\n")
        f.write("BNK FULL MAPPING ANALYSIS - CORRECT PARSER\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total BNK files processed: {stats['total_files']}\n")
        f.write(f"Files with events: {stats['files_with_events']}\n")
        f.write(f"Total events found: {stats['total_events']}\n")
        f.write(f"Total WEM mappings: {len(all_rows)}\n")
        f.write(f"Match rate: {stats['total_events']/max(1, stats['total_files'])*100:.1f}% files with events\n")
    
    print(f"✓ Saved to {summary_path}")
    
    # Print summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Total BNK files: {stats['total_files']}")
    print(f"Files with events: {stats['files_with_events']}")
    print(f"Total events: {stats['total_events']}")
    print(f"Total WEM mappings: {len(all_rows)}")

