#!/usr/bin/env python3
"""
Extract all WEM files from BNK files using vgmstream-cli
"""
import os
import subprocess
import csv
from pathlib import Path

BNK_DIR = "Audio/extracted_bnk"
OUT_DIR = "Audio/Extracted_WAVs_Proper"
CSV_PATH = "Audio/Analysis/event_wem_mapping.csv"
VGMSTREAM = "Audio/vgmstream-win64/vgmstream-cli.exe"
REPORT_DIR = "Audio/Reports"

os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("EXTRACT ALL WEM FILES FROM BNK FILES")
print("="*100)

# Step 1: Read CSV to get WEM ID to Bank ID mapping
print("\n[1] Reading CSV mapping...")

wem_to_bank = {}  # wem_id -> bank_id
with open(CSV_PATH, 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        wem_id = row['WEM_ID']
        bank_id = row['BankID']
        if wem_id not in wem_to_bank:
            wem_to_bank[wem_id] = bank_id

print(f"  Total unique WEM IDs: {len(wem_to_bank)}")

# Step 2: Get list of BNK files
print("\n[2] Scanning BNK files...")

bnk_files = sorted([f for f in os.listdir(BNK_DIR) if f.endswith('.bnk')])
print(f"  Total BNK files: {len(bnk_files)}")

# Step 3: Extract WEMs from each BNK
print("\n[3] Extracting WEMs...")

extracted = 0
failed = 0
extraction_log = []

for i, bnk_file in enumerate(bnk_files):
    if (i + 1) % 50 == 0:
        print(f"  [{i+1}/{len(bnk_files)}] Extracted: {extracted}, Failed: {failed}")
    
    bnk_path = os.path.join(BNK_DIR, bnk_file)
    
    try:
        # Run vgmstream-cli to get info
        result = subprocess.run(
            [VGMSTREAM, bnk_path, "-m"],
            capture_output=True,
            timeout=10,
            text=True
        )
        
        if result.returncode != 0:
            extraction_log.append(f"[SKIP] {bnk_file}: vgmstream failed")
            continue
        
        # Parse output to find stream names (WEM IDs)
        output_lines = result.stdout.split('\n')
        stream_name = None
        
        for line in output_lines:
            if 'stream name:' in line.lower():
                stream_name = line.split(':')[1].strip()
                break
        
        if not stream_name:
            extraction_log.append(f"[SKIP] {bnk_file}: No stream name found")
            continue
        
        # Extract all streams from this BNK
        # Use wildcard to extract all streams
        result = subprocess.run(
            [VGMSTREAM, bnk_path, "-o", os.path.join(OUT_DIR, "?n.wav")],
            capture_output=True,
            timeout=30
        )

        if result.returncode == 0:
            # Count extracted files
            new_files = [f for f in os.listdir(OUT_DIR) if f.endswith('.wav')]
            extracted += len(new_files)
            extraction_log.append(f"[OK] {bnk_file}: Extracted {len(new_files)} streams")
        else:
            failed += 1
            extraction_log.append(f"[FAIL] {bnk_file}: Extraction failed")
    
    except Exception as e:
        failed += 1
        extraction_log.append(f"[ERROR] {bnk_file}: {str(e)}")

print(f"\n[4] Extraction Summary")
print(f"  Extracted: {extracted:,}")
print(f"  Failed: {failed:,}")
print(f"  Total BNK files: {len(bnk_files):,}")
print(f"  Success rate: {(extracted/len(bnk_files)*100):.1f}%")

# Write log
log_file = os.path.join(REPORT_DIR, "wem_extraction_from_bnk.log")
with open(log_file, 'w') as f:
    f.write("WEM EXTRACTION FROM BNK FILES\n")
    f.write("="*100 + "\n\n")
    for line in extraction_log:
        f.write(line + "\n")

print(f"\n✅ Extraction log: {log_file}")
print(f"✅ Extracted WAVs: {OUT_DIR}")

# Verify
wav_files = [f for f in os.listdir(OUT_DIR) if f.endswith('.wav')]
print(f"\n[5] Verification")
print(f"  WAV files created: {len(wav_files)}")
print(f"  Expected WEM IDs: {len(wem_to_bank)}")
print(f"  Coverage: {(len(wav_files)/len(wem_to_bank)*100):.1f}%")

