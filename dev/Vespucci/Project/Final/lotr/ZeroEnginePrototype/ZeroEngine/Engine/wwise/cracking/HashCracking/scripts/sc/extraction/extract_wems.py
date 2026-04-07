#!/usr/bin/env python3
"""
WEM Extraction Pipeline
Extracts all WEM files from sound.pck using offsets from event_wem_mapping.csv
"""
import os
import csv
import sys
from pathlib import Path
from collections import defaultdict

PCK_PATH = "Audio/sound.pck"
CSV_PATH = "Audio/Analysis/event_wem_mapping.csv"
OUT_DIR = "Audio/Extracted_WEMs"
REPORT_DIR = "Audio/Reports"

os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM EXTRACTION PIPELINE")
print("="*100)

# Check files exist
if not os.path.exists(PCK_PATH):
    print(f"[ERROR] {PCK_PATH} not found")
    sys.exit(1)

if not os.path.exists(CSV_PATH):
    print(f"[ERROR] {CSV_PATH} not found")
    sys.exit(1)

pck_size = os.path.getsize(PCK_PATH)
print(f"\n[1] Initialization")
print(f"  PCK file: {PCK_PATH} ({pck_size:,} bytes)")
print(f"  CSV file: {CSV_PATH}")
print(f"  Output directory: {OUT_DIR}")

# Read CSV and deduplicate WEMs
print(f"\n[2] Reading CSV and deduplicating...")
wem_entries = {}  # wem_id -> (data_base, offset, size, bank_name, event_id)
event_map = defaultdict(list)  # wem_id -> [(bank_name, event_id), ...]

with open(CSV_PATH, 'r', encoding='utf-8') as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            wem_id = row['WEM_ID']
            data_base = int(row['DATA_Base'], 16)
            offset = int(row['WEM_Offset'], 16)
            size = int(row['WEM_Size'], 16)
            bank_name = row['BankName']
            event_id = row['EventID']
            
            # Store first occurrence
            if wem_id not in wem_entries:
                wem_entries[wem_id] = (data_base, offset, size, bank_name, event_id)
            
            # Track all events for this WEM
            event_map[wem_id].append((bank_name, event_id))
        except Exception as e:
            print(f"  [WARN] CSV parse error: {e}")

print(f"  Total CSV rows: {len(event_map)}")
print(f"  Unique WEM IDs: {len(wem_entries)}")

# Extract WEMs
print(f"\n[3] Extracting WEMs from sound.pck...")
extracted = 0
failed = 0
skipped = 0
extraction_log = []

with open(PCK_PATH, 'rb') as pck:
    for i, (wem_id, (data_base, offset, size, bank_name, event_id)) in enumerate(wem_entries.items()):
        if (i + 1) % 1000 == 0:
            print(f"  [{i+1}/{len(wem_entries)}] Extracted: {extracted}, Failed: {failed}")
        
        try:
            # Calculate absolute offset
            abs_offset = data_base + offset
            
            # Validate bounds
            if abs_offset < 0 or abs_offset + size > pck_size:
                extraction_log.append(f"[SKIP] {wem_id}: Out of bounds (offset={abs_offset}, size={size}, pck_size={pck_size})")
                skipped += 1
                continue
            
            # Extract data
            pck.seek(abs_offset)
            data = pck.read(size)
            
            if len(data) != size:
                extraction_log.append(f"[SIZE_MISMATCH] {wem_id}: Expected {size}, got {len(data)}")
                failed += 1
                continue
            
            # Save file
            out_file = os.path.join(OUT_DIR, f"{wem_id}.wem")
            with open(out_file, 'wb') as f:
                f.write(data)
            
            extracted += 1
            extraction_log.append(f"[OK] {wem_id}: {size} bytes")
            
        except Exception as e:
            extraction_log.append(f"[FAIL] {wem_id}: {str(e)}")
            failed += 1

print(f"\n[4] Extraction Summary")
print(f"  ✅ Extracted: {extracted:,}")
print(f"  ❌ Failed: {failed:,}")
print(f"  ⏭️  Skipped: {skipped:,}")
print(f"  Total: {len(wem_entries):,}")

# Write extraction log
log_file = os.path.join(REPORT_DIR, "wem_extraction_summary.log")
with open(log_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM EXTRACTION SUMMARY\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total WEM IDs: {len(wem_entries):,}\n")
    f.write(f"Extracted: {extracted:,}\n")
    f.write(f"Failed: {failed:,}\n")
    f.write(f"Skipped: {skipped:,}\n")
    f.write(f"Success rate: {extracted/len(wem_entries)*100:.1f}%\n\n")
    f.write("="*100 + "\n")
    f.write("EXTRACTION LOG\n")
    f.write("="*100 + "\n")
    for line in extraction_log[-100:]:  # Last 100 entries
        f.write(line + "\n")

print(f"\n✅ Extraction log: {log_file}")
print(f"✅ Extracted WEMs: {OUT_DIR}")
print(f"\nNext: Run convert_wems.py to convert to WAV format")

