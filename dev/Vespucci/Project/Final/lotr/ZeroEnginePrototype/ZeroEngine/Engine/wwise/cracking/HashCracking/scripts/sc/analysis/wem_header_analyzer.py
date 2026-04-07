#!/usr/bin/env python3
"""
WEM Header Analyzer
Analyzes WEM file headers to detect codec types and validate structure
"""
import os
import struct
from collections import defaultdict

WEM_DIR = "Audio/Extracted_WEMs"
REPORT_DIR = "Audio/Reports"

os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM HEADER ANALYZER")
print("="*100)

if not os.path.exists(WEM_DIR):
    print(f"[ERROR] {WEM_DIR} not found")
    exit(1)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])
print(f"\n[1] Analyzing {len(wem_files):,} WEM files...")

codec_stats = defaultdict(int)
wem_analysis = []
invalid_wems = []

for i, wem_file in enumerate(wem_files):
    if (i + 1) % 200 == 0:
        print(f"  [{i+1}/{len(wem_files)}] Analyzed...")
    
    wem_id = wem_file.replace('.wem', '')
    wem_path = os.path.join(WEM_DIR, wem_file)
    
    try:
        with open(wem_path, 'rb') as f:
            data = f.read(256)  # Read first 256 bytes
        
        file_size = os.path.getsize(wem_path)
        
        if len(data) < 4:
            codec_stats["Empty"] += 1
            invalid_wems.append((wem_id, "Empty file", file_size))
            continue
        
        # Check for known headers
        header = data[:4]
        
        if header == b'RIFF':
            # WAV file
            codec_stats["WAV_RIFF"] += 1
            wem_analysis.append((wem_id, "WAV_RIFF", file_size, "Valid"))
        
        elif header == b'OggS':
            # OGG Vorbis
            codec_stats["OGG_Vorbis"] += 1
            wem_analysis.append((wem_id, "OGG_Vorbis", file_size, "Valid"))
        
        elif header == b'RIFF' or data[8:12] == b'WAVE':
            # WAVE format
            codec_stats["WAVE"] += 1
            wem_analysis.append((wem_id, "WAVE", file_size, "Valid"))
        
        elif header[:2] == b'\xff\xfb' or header[:2] == b'\xff\xfa':
            # MP3 frame sync
            codec_stats["MP3"] += 1
            wem_analysis.append((wem_id, "MP3", file_size, "Valid"))
        
        elif header == b'ID3\x04' or header == b'ID3\x03':
            # ID3 tag (MP3 metadata)
            codec_stats["ID3_MP3"] += 1
            wem_analysis.append((wem_id, "ID3_MP3", file_size, "Valid"))
        
        elif header == b'fLaC':
            # FLAC
            codec_stats["FLAC"] += 1
            wem_analysis.append((wem_id, "FLAC", file_size, "Valid"))
        
        elif header == b'OpusHead'[:4]:
            # Opus
            codec_stats["Opus"] += 1
            wem_analysis.append((wem_id, "Opus", file_size, "Valid"))
        
        else:
            # Unknown/WEM format
            # Try to detect Wwise WEM signature
            if b'RIFF' in data[:20]:
                codec_stats["WEM_RIFF_Embedded"] += 1
                wem_analysis.append((wem_id, "WEM_RIFF_Embedded", file_size, "Needs_Conversion"))
            elif b'OggS' in data[:20]:
                codec_stats["WEM_OGG_Embedded"] += 1
                wem_analysis.append((wem_id, "WEM_OGG_Embedded", file_size, "Needs_Conversion"))
            else:
                codec_stats["WEM_Unknown"] += 1
                wem_analysis.append((wem_id, "WEM_Unknown", file_size, "Needs_Conversion"))
    
    except Exception as e:
        codec_stats["Error"] += 1
        invalid_wems.append((wem_id, str(e), 0))

print(f"\n[2] Analysis Complete")
print(f"  Total files: {len(wem_files):,}")
print(f"  Valid audio: {sum(1 for _, _, _, status in wem_analysis if status == 'Valid'):,}")
print(f"  Need conversion: {sum(1 for _, _, _, status in wem_analysis if status == 'Needs_Conversion'):,}")
print(f"  Errors: {len(invalid_wems):,}")

print(f"\n[3] Codec Distribution")
for codec, count in sorted(codec_stats.items(), key=lambda x: x[1], reverse=True):
    pct = count / len(wem_files) * 100
    print(f"  {codec}: {count:,} ({pct:.1f}%)")

# Write analysis report
report_file = os.path.join(REPORT_DIR, "wem_header_analysis.txt")
with open(report_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM HEADER ANALYSIS REPORT\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total WEM files: {len(wem_files):,}\n")
    f.write(f"Valid audio files: {sum(1 for _, _, _, status in wem_analysis if status == 'Valid'):,}\n")
    f.write(f"Need conversion: {sum(1 for _, _, _, status in wem_analysis if status == 'Needs_Conversion'):,}\n")
    f.write(f"Errors: {len(invalid_wems):,}\n\n")
    
    f.write("="*100 + "\n")
    f.write("CODEC DISTRIBUTION\n")
    f.write("="*100 + "\n")
    for codec, count in sorted(codec_stats.items(), key=lambda x: x[1], reverse=True):
        pct = count / len(wem_files) * 100
        f.write(f"{codec}: {count:,} ({pct:.1f}%)\n")
    
    f.write("\n" + "="*100 + "\n")
    f.write("SAMPLE ANALYSIS (First 50 files)\n")
    f.write("="*100 + "\n")
    f.write("WEM_ID,Codec,Size,Status\n")
    for wem_id, codec, size, status in wem_analysis[:50]:
        f.write(f"{wem_id},{codec},{size},{status}\n")

# Write invalid files
invalid_file = os.path.join(REPORT_DIR, "wem_invalid_analysis.csv")
with open(invalid_file, 'w') as f:
    f.write("WEM_ID,Error,Size\n")
    for wem_id, error, size in invalid_wems:
        f.write(f"{wem_id},{error},{size}\n")

print(f"\n✅ Analysis report: {report_file}")
print(f"✅ Invalid files: {invalid_file}")

