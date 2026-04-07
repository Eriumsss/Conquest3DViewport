#!/usr/bin/env python3
"""
Simple WEM to WAV Conversion using Python libraries
Attempts to convert WEM files using available Python audio libraries
"""
import os
import sys
from pathlib import Path

WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
REPORT_DIR = "Audio/Reports"

os.makedirs(WAV_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM TO WAV CONVERSION (PYTHON-BASED)")
print("="*100)

# Check WEM directory
if not os.path.exists(WEM_DIR):
    print(f"[ERROR] {WEM_DIR} not found. Run extract_wems.py first.")
    sys.exit(1)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])
print(f"\n[1] Found {len(wem_files):,} WEM files to convert")

# Try to import audio libraries
print(f"\n[2] Detecting Python audio libraries...")
scipy_available = False
pydub_available = False

try:
    from scipy.io import wavfile
    import numpy as np
    scipy_available = True
    print(f"  ✅ scipy available")
except:
    print(f"  ❌ scipy not available")

try:
    from pydub import AudioSegment
    pydub_available = True
    print(f"  ✅ pydub available")
except:
    print(f"  ❌ pydub not available")

# If no libraries available, just copy WEM files as-is
if not scipy_available and not pydub_available:
    print(f"\n[3] No audio libraries available. Copying WEM files as WAV (no conversion)...")
    converted = 0
    for i, wem_file in enumerate(wem_files):
        if (i + 1) % 100 == 0:
            print(f"  [{i+1}/{len(wem_files)}] Copied: {converted}")
        
        try:
            wem_path = os.path.join(WEM_DIR, wem_file)
            wav_file = wem_file.replace('.wem', '.wav')
            wav_path = os.path.join(WAV_DIR, wav_file)
            
            # Copy file
            with open(wem_path, 'rb') as src:
                with open(wav_path, 'wb') as dst:
                    dst.write(src.read())
            
            converted += 1
        except Exception as e:
            print(f"  [ERROR] {wem_file}: {str(e)}")
    
    print(f"\n[4] Copy Summary")
    print(f"  ✅ Copied: {converted:,}")
    print(f"  Total: {len(wem_files):,}")
    print(f"  Success rate: {converted/len(wem_files)*100:.1f}%")
    
    # Write log
    log_file = os.path.join(REPORT_DIR, "wem_conversion_summary.log")
    with open(log_file, 'w') as f:
        f.write("="*100 + "\n")
        f.write("WEM CONVERSION SUMMARY (COPY MODE - NO CONVERSION TOOLS AVAILABLE)\n")
        f.write("="*100 + "\n\n")
        f.write(f"Total WEM files: {len(wem_files):,}\n")
        f.write(f"Copied: {converted:,}\n")
        f.write(f"Success rate: {converted/len(wem_files)*100:.1f}%\n\n")
        f.write("NOTE: WEM files were copied as-is without conversion.\n")
        f.write("To convert to WAV, install ffmpeg or vgmstream-cli.\n")
    
    print(f"\n✅ Conversion log: {log_file}")
    print(f"✅ Output files: {WAV_DIR}")
    print(f"\nNote: WEM files were copied without conversion.")
    print(f"To convert to WAV, install ffmpeg: https://ffmpeg.org/download.html")

else:
    print(f"\n[3] Converting WEM files...")
    # Conversion logic would go here
    print(f"  [TODO] Implement conversion with available libraries")

