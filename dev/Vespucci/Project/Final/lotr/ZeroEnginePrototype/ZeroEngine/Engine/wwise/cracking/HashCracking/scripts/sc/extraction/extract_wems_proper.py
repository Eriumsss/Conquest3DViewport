#!/usr/bin/env python3
"""
Proper WEM Extraction from sound.pck
Finds BNK files first, then extracts audio data from their DATA sections
"""
import os
import csv
import struct
from collections import defaultdict

PCK_PATH = "Audio/sound.pck"
CSV_PATH = "Audio/Analysis/event_wem_mapping.csv"
OUT_DIR = "Audio/Extracted_WEMs_Proper"
REPORT_DIR = "Audio/Reports"

os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("PROPER WEM EXTRACTION FROM sound.pck")
print("="*100)

# Step 1: Find all BNK files in sound.pck
print("\n[1] Scanning sound.pck for BNK files...")

pck_size = os.path.getsize(PCK_PATH)
bnk_locations = {}  # bank_id -> offset_in_pck

with open(PCK_PATH, 'rb') as f:
    data = f.read()

# Search for BKHD signatures (BNK file headers)
bkhd_sig = b'BKHD'
offset = 0
bnk_count = 0

while True:
    offset = data.find(bkhd_sig, offset)
    if offset == -1:
        break
    
    # Try to read bank ID from BKHD section
    try:
        # BKHD structure: signature(4) + size(4) + version(1) + flags(1) + bank_id(4)
        if offset + 12 <= len(data):
            bank_id_bytes = data[offset+8:offset+12]
            bank_id = struct.unpack('<I', bank_id_bytes)[0]
            bnk_count += 1
            print(f"  Found BNK at offset 0x{offset:08x}, Bank ID: 0x{bank_id:08x}")
            bnk_locations[bank_id] = offset
    except:
        pass
    
    offset += 1

print(f"  Total BNK files found: {bnk_count}")

# Step 2: Read CSV and map WEMs to BNK files
print(f"\n[2] Reading CSV...")

wem_entries = {}  # wem_id -> (bank_id, offset_in_data, size)

with open(CSV_PATH, 'r', encoding='utf-8') as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            wem_id = row['WEM_ID']
            bank_id = int(row['BankID'], 16)
            offset = int(row['WEM_Offset'], 16)
            size = int(row['WEM_Size'], 16)
            data_base = int(row['DATA_Base'], 16)
            
            if wem_id not in wem_entries:
                wem_entries[wem_id] = (bank_id, data_base, offset, size)
        except Exception as e:
            pass

print(f"  Total unique WEM IDs: {len(wem_entries)}")

# Step 3: Extract WEMs
print(f"\n[3] Extracting WEMs...")

extracted = 0
failed = 0
extraction_log = []

with open(PCK_PATH, 'rb') as pck:
    for i, (wem_id, (bank_id, data_base, offset, size)) in enumerate(wem_entries.items()):
        if (i + 1) % 100 == 0:
            print(f"  [{i+1}/{len(wem_entries)}] Extracted: {extracted}, Failed: {failed}")
        
        try:
            # Find BNK location
            if bank_id not in bnk_locations:
                extraction_log.append(f"[SKIP] {wem_id}: Bank 0x{bank_id:08x} not found")
                failed += 1
                continue
            
            bnk_offset = bnk_locations[bank_id]
            
            # Calculate absolute offset: BNK offset + DATA section offset + WEM offset
            abs_offset = bnk_offset + data_base + offset
            
            # Validate bounds
            if abs_offset < 0 or abs_offset + size > pck_size:
                extraction_log.append(f"[SKIP] {wem_id}: Out of bounds")
                failed += 1
                continue
            
            # Extract data
            pck.seek(abs_offset)
            audio_data = pck.read(size)
            
            if len(audio_data) != size:
                extraction_log.append(f"[SIZE_MISMATCH] {wem_id}: Expected {size}, got {len(audio_data)}")
                failed += 1
                continue
            
            # Save file
            out_file = os.path.join(OUT_DIR, f"{wem_id}.wem")
            with open(out_file, 'wb') as f:
                f.write(audio_data)
            
            extracted += 1
            extraction_log.append(f"[OK] {wem_id}: {size} bytes")
            
        except Exception as e:
            extraction_log.append(f"[ERROR] {wem_id}: {str(e)}")
            failed += 1

print(f"\n[4] Extraction Summary")
print(f"  Extracted: {extracted:,}")
print(f"  Failed: {failed:,}")
print(f"  Total: {len(wem_entries):,}")
print(f"  Success rate: {(extracted/len(wem_entries)*100):.1f}%")

# Write log
log_file = os.path.join(REPORT_DIR, "wem_extraction_proper.log")
with open(log_file, 'w') as f:
    f.write("WEM EXTRACTION LOG\n")
    f.write("="*100 + "\n\n")
    for line in extraction_log[-100:]:
        f.write(line + "\n")

print(f"\n✅ Extraction log: {log_file}")
print(f"✅ Extracted WEMs: {OUT_DIR}")

