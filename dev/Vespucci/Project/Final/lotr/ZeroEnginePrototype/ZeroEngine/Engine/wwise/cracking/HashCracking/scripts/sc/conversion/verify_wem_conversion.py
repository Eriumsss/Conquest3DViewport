#!/usr/bin/env python3
"""
WEM Conversion Verification
Verifies all converted files and generates comprehensive report
"""
import os
import csv
from collections import defaultdict

WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
OGG_DIR = "Audio/Extracted_OGGs"
REPORT_DIR = "Audio/Reports"

os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM CONVERSION VERIFICATION")
print("="*100)

# Check directories
print(f"\n[1] Checking output directories...")
wem_files = set(f.replace('.wem', '') for f in os.listdir(WEM_DIR) if f.endswith('.wem'))
wav_files = set(f.replace('.wav', '') for f in os.listdir(WAV_DIR) if f.endswith('.wav'))
ogg_files = set(f.replace('.ogg', '') for f in os.listdir(OGG_DIR) if f.endswith('.ogg'))

print(f"  WEM files: {len(wem_files):,}")
print(f"  WAV files: {len(wav_files):,}")
print(f"  OGG files: {len(ogg_files):,}")

# Verify headers
print(f"\n[2] Verifying file headers...")

def get_header(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4)
    except:
        return b''

def get_size(path):
    try:
        return os.path.getsize(path)
    except:
        return 0

valid_wavs = 0
valid_oggs = 0
invalid_wavs = []
invalid_oggs = []

for wem_id in wem_files:
    wav_path = os.path.join(WAV_DIR, f"{wem_id}.wav")
    if os.path.exists(wav_path):
        header = get_header(wav_path)
        size = get_size(wav_path)
        
        if header == b'RIFF' and size > 512:
            valid_wavs += 1
        else:
            invalid_wavs.append((wem_id, header, size))
    
    ogg_path = os.path.join(OGG_DIR, f"{wem_id}.ogg")
    if os.path.exists(ogg_path):
        header = get_header(ogg_path)
        size = get_size(ogg_path)
        
        if header == b'OggS' and size > 512:
            valid_oggs += 1
        else:
            invalid_oggs.append((wem_id, header, size))

print(f"  Valid WAV files: {valid_wavs:,}")
print(f"  Valid OGG files: {valid_oggs:,}")
print(f"  Invalid WAV files: {len(invalid_wavs):,}")
print(f"  Invalid OGG files: {len(invalid_oggs):,}")

# Calculate statistics
print(f"\n[3] Calculating statistics...")

total_wem_size = sum(get_size(os.path.join(WEM_DIR, f"{wem_id}.wem")) for wem_id in wem_files)
total_wav_size = sum(get_size(os.path.join(WAV_DIR, f"{wem_id}.wav")) for wem_id in wav_files)
total_ogg_size = sum(get_size(os.path.join(OGG_DIR, f"{wem_id}.ogg")) for wem_id in ogg_files)

print(f"  Total WEM size: {total_wem_size:,} bytes ({total_wem_size/(1024*1024):.1f} MB)")
print(f"  Total WAV size: {total_wav_size:,} bytes ({total_wav_size/(1024*1024):.1f} MB)")
print(f"  Total OGG size: {total_ogg_size:,} bytes ({total_ogg_size/(1024*1024):.1f} MB)")

# Generate comprehensive report
print(f"\n[4] Generating reports...")

# Main verification report
report_file = os.path.join(REPORT_DIR, "VERIFICATION_REPORT.txt")
with open(report_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM CONVERSION VERIFICATION REPORT\n")
    f.write("="*100 + "\n\n")
    
    f.write("SUMMARY\n")
    f.write("-"*100 + "\n")
    f.write(f"Total WEM files: {len(wem_files):,}\n")
    f.write(f"WAV files created: {len(wav_files):,}\n")
    f.write(f"OGG files created: {len(ogg_files):,}\n")
    f.write(f"Valid WAV files: {valid_wavs:,}\n")
    f.write(f"Valid OGG files: {valid_oggs:,}\n\n")
    
    f.write("FILE SIZES\n")
    f.write("-"*100 + "\n")
    f.write(f"Total WEM data: {total_wem_size:,} bytes ({total_wem_size/(1024*1024):.1f} MB)\n")
    f.write(f"Total WAV data: {total_wav_size:,} bytes ({total_wav_size/(1024*1024):.1f} MB)\n")
    f.write(f"Total OGG data: {total_ogg_size:,} bytes ({total_ogg_size/(1024*1024):.1f} MB)\n\n")
    
    f.write("HEADER VALIDATION\n")
    f.write("-"*100 + "\n")
    f.write(f"WAV files with RIFF header: {valid_wavs:,}\n")
    f.write(f"OGG files with OggS header: {valid_oggs:,}\n")
    f.write(f"Invalid WAV files: {len(invalid_wavs):,}\n")
    f.write(f"Invalid OGG files: {len(invalid_oggs):,}\n\n")
    
    f.write("SUCCESS CRITERIA\n")
    f.write("-"*100 + "\n")
    wav_success = valid_wavs / len(wem_files) * 100 if wem_files else 0
    ogg_success = valid_oggs / len(wem_files) * 100 if wem_files else 0
    f.write(f"WAV conversion success rate: {wav_success:.1f}%\n")
    f.write(f"OGG conversion success rate: {ogg_success:.1f}%\n")
    f.write(f"Overall success rate: {max(wav_success, ogg_success):.1f}%\n\n")
    
    f.write("NEXT STEPS\n")
    f.write("-"*100 + "\n")
    f.write("1. Install vgmstream-cli from: https://github.com/vgmstream/vgmstream/releases\n")
    f.write("2. Re-run wem_reconversion_pipeline.py to convert WEM files to proper WAV format\n")
    f.write("3. Verify converted files are playable in Windows Media Player\n")
    f.write("4. Use converted audio for game modding\n")

# Invalid files report
if invalid_wavs or invalid_oggs:
    invalid_file = os.path.join(REPORT_DIR, "INVALID_CONVERSIONS.csv")
    with open(invalid_file, 'w') as f:
        f.write("WEM_ID,Type,Header,Size\n")
        for wem_id, header, size in invalid_wavs:
            f.write(f"{wem_id},WAV,{header.hex()},{size}\n")
        for wem_id, header, size in invalid_oggs:
            f.write(f"{wem_id},OGG,{header.hex()},{size}\n")

# Create index with conversion status
print(f"\n[5] Creating conversion index...")
index_file = os.path.join(REPORT_DIR, "CONVERSION_INDEX.csv")
with open(index_file, 'w') as f:
    f.write("WEM_ID,WEM_Size,WAV_Exists,WAV_Valid,WAV_Size,OGG_Exists,OGG_Valid,OGG_Size,Status\n")
    
    for wem_id in sorted(wem_files):
        wem_path = os.path.join(WEM_DIR, f"{wem_id}.wem")
        wav_path = os.path.join(WAV_DIR, f"{wem_id}.wav")
        ogg_path = os.path.join(OGG_DIR, f"{wem_id}.ogg")
        
        wem_size = get_size(wem_path)
        wav_exists = os.path.exists(wav_path)
        wav_size = get_size(wav_path) if wav_exists else 0
        wav_valid = wav_exists and get_header(wav_path) == b'RIFF' and wav_size > 512
        
        ogg_exists = os.path.exists(ogg_path)
        ogg_size = get_size(ogg_path) if ogg_exists else 0
        ogg_valid = ogg_exists and get_header(ogg_path) == b'OggS' and ogg_size > 512
        
        status = "OK" if (wav_valid or ogg_valid) else "NEEDS_CONVERSION"
        
        f.write(f"{wem_id},{wem_size},{wav_exists},{wav_valid},{wav_size},{ogg_exists},{ogg_valid},{ogg_size},{status}\n")

print(f"\n[6] Summary")
print(f"  ✅ Verification report: {report_file}")
print(f"  ✅ Conversion index: {index_file}")
if invalid_wavs or invalid_oggs:
    print(f"  ⚠️  Invalid conversions: {invalid_file}")

print(f"\n" + "="*100)
print(f"VERIFICATION COMPLETE")
print(f"="*100)

