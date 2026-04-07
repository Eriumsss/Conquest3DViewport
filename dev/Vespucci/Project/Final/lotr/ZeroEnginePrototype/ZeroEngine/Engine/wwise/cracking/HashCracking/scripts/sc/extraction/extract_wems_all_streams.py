#!/usr/bin/env python3
"""
Extract all WEM streams from BNK files using vgmstream-cli
"""
import os
import subprocess
import re
from pathlib import Path

BNK_DIR = "Audio/extracted_bnk"
OUT_DIR = "Audio/Extracted_WAVs_Proper"
VGMSTREAM = "Audio/vgmstream-win64/vgmstream-cli.exe"
REPORT_DIR = "Audio/Reports"

os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("EXTRACT ALL WEM STREAMS FROM BNK FILES")
print("="*100)

# Get list of BNK files
bnk_files = sorted([f for f in os.listdir(BNK_DIR) if f.endswith('.bnk')])
print(f"\n[1] Found {len(bnk_files)} BNK files")

# Extract WEMs from each BNK
print(f"\n[2] Extracting WEMs...")

extracted = 0
failed = 0
extraction_log = []

for i, bnk_file in enumerate(bnk_files):
    if (i + 1) % 50 == 0:
        print(f"  [{i+1}/{len(bnk_files)}] Extracted: {extracted}, Failed: {failed}")
    
    bnk_path = os.path.join(BNK_DIR, bnk_file)
    
    try:
        # Get metadata to find number of streams
        result = subprocess.run(
            [VGMSTREAM, bnk_path, "-m"],
            capture_output=True,
            timeout=10,
            text=True
        )
        
        if result.returncode != 0:
            extraction_log.append(f"[SKIP] {bnk_file}: vgmstream failed")
            continue
        
        # Parse output to find stream count
        output = result.stdout
        stream_count_match = re.search(r'stream count:\s*(\d+)', output)
        
        if not stream_count_match:
            extraction_log.append(f"[SKIP] {bnk_file}: No stream count found")
            continue
        
        stream_count = int(stream_count_match.group(1))
        
        # Extract each stream
        for stream_idx in range(stream_count):
            try:
                # Get stream name
                result = subprocess.run(
                    [VGMSTREAM, bnk_path, "-s", str(stream_idx), "-m"],
                    capture_output=True,
                    timeout=10,
                    text=True
                )
                
                stream_name_match = re.search(r'stream name:\s*(\S+)', result.stdout)
                if not stream_name_match:
                    continue
                
                stream_name = stream_name_match.group(1)
                wav_path = os.path.join(OUT_DIR, f"{stream_name}.wav")
                
                # Skip if already extracted
                if os.path.exists(wav_path):
                    continue
                
                # Extract stream
                result = subprocess.run(
                    [VGMSTREAM, bnk_path, "-s", str(stream_idx), "-o", wav_path],
                    capture_output=True,
                    timeout=30
                )
                
                if result.returncode == 0 and os.path.exists(wav_path):
                    size = os.path.getsize(wav_path)
                    extracted += 1
                    extraction_log.append(f"[OK] {stream_name}: {size} bytes")
                else:
                    failed += 1
                    extraction_log.append(f"[FAIL] {stream_name}: Extraction failed")
            
            except Exception as e:
                failed += 1
                extraction_log.append(f"[ERROR] Stream {stream_idx}: {str(e)}")
    
    except Exception as e:
        failed += 1
        extraction_log.append(f"[ERROR] {bnk_file}: {str(e)}")

print(f"\n[3] Extraction Summary")
print(f"  Extracted: {extracted:,}")
print(f"  Failed: {failed:,}")
print(f"  Total BNK files: {len(bnk_files):,}")

# Write log
log_file = os.path.join(REPORT_DIR, "wem_extraction_all_streams.log")
with open(log_file, 'w') as f:
    f.write("WEM EXTRACTION FROM BNK FILES (ALL STREAMS)\n")
    f.write("="*100 + "\n\n")
    for line in extraction_log[-100:]:
        f.write(line + "\n")

print(f"\nExtraction log: {log_file}")
print(f"Extracted WAVs: {OUT_DIR}")

# Verify
wav_files = [f for f in os.listdir(OUT_DIR) if f.endswith('.wav')]
print(f"\n[4] Verification")
print(f"  WAV files created: {len(wav_files)}")

