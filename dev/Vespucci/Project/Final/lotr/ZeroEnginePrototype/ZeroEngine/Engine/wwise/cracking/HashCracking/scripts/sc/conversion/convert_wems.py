#!/usr/bin/env python3
"""
WEM to WAV Conversion Pipeline
Converts extracted WEM files to WAV format using ffmpeg or vgmstream
"""
import os
import subprocess
import sys
from pathlib import Path

WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
REPORT_DIR = "Audio/Reports"

os.makedirs(WAV_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM TO WAV CONVERSION PIPELINE")
print("="*100)

# Check WEM directory
if not os.path.exists(WEM_DIR):
    print(f"[ERROR] {WEM_DIR} not found. Run extract_wems.py first.")
    sys.exit(1)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])
print(f"\n[1] Found {len(wem_files):,} WEM files to convert")

# Detect available tools
print(f"\n[2] Detecting conversion tools...")
ffmpeg_available = False
vgmstream_available = False

try:
    result = subprocess.run(['ffmpeg', '-version'], capture_output=True, timeout=5)
    if result.returncode == 0:
        ffmpeg_available = True
        print(f"  ✅ ffmpeg available")
except:
    print(f"  ❌ ffmpeg not available")

try:
    result = subprocess.run(['vgmstream-cli', '-h'], capture_output=True, timeout=5)
    if result.returncode == 0:
        vgmstream_available = True
        print(f"  ✅ vgmstream-cli available")
except:
    print(f"  ❌ vgmstream-cli not available")

if not ffmpeg_available and not vgmstream_available:
    print(f"\n[ERROR] Neither ffmpeg nor vgmstream-cli available!")
    print(f"Install ffmpeg: https://ffmpeg.org/download.html")
    print(f"Or install vgmstream: https://github.com/vgmstream/vgmstream")
    sys.exit(1)

# Convert WEMs
print(f"\n[3] Converting WEMs to WAV...")
converted = 0
failed = 0
conversion_log = []

for i, wem_file in enumerate(wem_files):
    if (i + 1) % 100 == 0:
        print(f"  [{i+1}/{len(wem_files)}] Converted: {converted}, Failed: {failed}")
    
    wem_path = os.path.join(WEM_DIR, wem_file)
    wav_file = wem_file.replace('.wem', '.wav')
    wav_path = os.path.join(WAV_DIR, wav_file)
    
    try:
        # Try ffmpeg first
        if ffmpeg_available:
            result = subprocess.run(
                ['ffmpeg', '-y', '-i', wem_path, '-acodec', 'pcm_s16le', '-ar', '44100', wav_path],
                capture_output=True,
                timeout=30
            )
            if result.returncode == 0 and os.path.exists(wav_path):
                converted += 1
                conversion_log.append(f"[OK] {wem_file} -> {wav_file} (ffmpeg)")
                continue
        
        # Fallback to vgmstream
        if vgmstream_available:
            result = subprocess.run(
                ['vgmstream-cli', wem_path, '-o', wav_path],
                capture_output=True,
                timeout=30
            )
            if result.returncode == 0 and os.path.exists(wav_path):
                converted += 1
                conversion_log.append(f"[OK] {wem_file} -> {wav_file} (vgmstream)")
                continue
        
        # Both failed
        conversion_log.append(f"[FAIL] {wem_file}: Both ffmpeg and vgmstream failed")
        failed += 1
        
    except subprocess.TimeoutExpired:
        conversion_log.append(f"[TIMEOUT] {wem_file}: Conversion timeout")
        failed += 1
    except Exception as e:
        conversion_log.append(f"[ERROR] {wem_file}: {str(e)}")
        failed += 1

print(f"\n[4] Conversion Summary")
print(f"  ✅ Converted: {converted:,}")
print(f"  ❌ Failed: {failed:,}")
print(f"  Total: {len(wem_files):,}")
print(f"  Success rate: {converted/len(wem_files)*100:.1f}%")

# Write conversion log
log_file = os.path.join(REPORT_DIR, "wem_conversion_summary.log")
with open(log_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM TO WAV CONVERSION SUMMARY\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total WEM files: {len(wem_files):,}\n")
    f.write(f"Converted: {converted:,}\n")
    f.write(f"Failed: {failed:,}\n")
    f.write(f"Success rate: {converted/len(wem_files)*100:.1f}%\n\n")
    f.write("="*100 + "\n")
    f.write("CONVERSION LOG\n")
    f.write("="*100 + "\n")
    for line in conversion_log[-100:]:  # Last 100 entries
        f.write(line + "\n")

print(f"\n✅ Conversion log: {log_file}")
print(f"✅ Converted WAVs: {WAV_DIR}")
print(f"\nNext: Run create_wem_index.py to create final report")

