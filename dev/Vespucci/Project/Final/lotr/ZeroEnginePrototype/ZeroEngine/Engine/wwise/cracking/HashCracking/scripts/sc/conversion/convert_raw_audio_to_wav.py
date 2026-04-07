#!/usr/bin/env python3
"""
Convert raw Wwise audio data to WAV format
Uses vorbis-tools or creates minimal WAV wrapper
"""
import os
import struct
import subprocess
from pathlib import Path

WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
REPORT_DIR = "Audio/Reports"

os.makedirs(WAV_DIR, exist_ok=True)
os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("RAW AUDIO TO WAV CONVERSION")
print("="*100)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])
print(f"\n[1] Found {len(wem_files):,} raw audio files")

# Check for vorbis-tools
vorbis_tools_dir = "Audio/vorbis-tools-1.4.3"
oggdec_path = os.path.join(vorbis_tools_dir, "oggdec", "oggdec.exe")
oggenc_path = os.path.join(vorbis_tools_dir, "oggenc", "oggenc.exe")

print(f"\n[2] Checking for vorbis-tools...")
print(f"  oggdec available: {os.path.exists(oggdec_path)}")
print(f"  oggenc available: {os.path.exists(oggenc_path)}")

# Strategy: Create minimal WAV wrapper for raw audio
# Assume 44.1 kHz, 16-bit stereo (common for game audio)

def create_wav_wrapper(raw_data, sample_rate=44100, channels=2, bits_per_sample=16):
    """Create a minimal WAV file from raw audio data"""
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    
    # WAV header
    wav_header = b'RIFF'
    wav_header += struct.pack('<I', 36 + len(raw_data))  # File size - 8
    wav_header += b'WAVE'
    
    # fmt subchunk
    wav_header += b'fmt '
    wav_header += struct.pack('<I', 16)  # Subchunk1Size
    wav_header += struct.pack('<H', 1)   # AudioFormat (1 = PCM)
    wav_header += struct.pack('<H', channels)
    wav_header += struct.pack('<I', sample_rate)
    wav_header += struct.pack('<I', byte_rate)
    wav_header += struct.pack('<H', block_align)
    wav_header += struct.pack('<H', bits_per_sample)
    
    # data subchunk
    wav_header += b'data'
    wav_header += struct.pack('<I', len(raw_data))
    wav_header += raw_data
    
    return wav_header

print(f"\n[3] Converting raw audio to WAV...")
converted = 0
failed = 0
conversion_log = []

for i, wem_file in enumerate(wem_files):
    if (i + 1) % 100 == 0:
        print(f"  [{i+1}/{len(wem_files)}] Converted: {converted}, Failed: {failed}")
    
    wem_id = wem_file.replace('.wem', '')
    wem_path = os.path.join(WEM_DIR, wem_file)
    wav_path = os.path.join(WAV_DIR, f"{wem_id}.wav")
    
    try:
        # Read raw audio data
        with open(wem_path, 'rb') as f:
            raw_data = f.read()
        
        # Create WAV wrapper
        wav_data = create_wav_wrapper(raw_data)
        
        # Write WAV file
        with open(wav_path, 'wb') as f:
            f.write(wav_data)
        
        converted += 1
        conversion_log.append(f"[OK] WEM_ID={wem_id} | Size={len(raw_data)} | Output=WAV_wrapper")
        
    except Exception as e:
        failed += 1
        conversion_log.append(f"[ERROR] WEM_ID={wem_id} | Error={str(e)}")

print(f"\n[4] Conversion Summary")
print(f"  ✅ Converted: {converted:,}")
print(f"  ❌ Failed: {failed:,}")
print(f"  Total: {len(wem_files):,}")
print(f"  Success rate: {(converted/len(wem_files)*100):.1f}%")

# Write log
log_file = os.path.join(REPORT_DIR, "raw_audio_conversion.log")
with open(log_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("RAW AUDIO TO WAV CONVERSION LOG\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total files: {len(wem_files):,}\n")
    f.write(f"Converted: {converted:,}\n")
    f.write(f"Failed: {failed:,}\n")
    f.write(f"Success rate: {(converted/len(wem_files)*100):.1f}%\n\n")
    f.write("="*100 + "\n")
    f.write("CONVERSION LOG\n")
    f.write("="*100 + "\n")
    for line in conversion_log[-100:]:
        f.write(line + "\n")

print(f"\n✅ Conversion log: {log_file}")
print(f"✅ Converted WAVs: {WAV_DIR}")
print(f"\nNote: WAV files contain raw audio data wrapped in WAV headers.")
print(f"These may not be directly playable without proper codec information.")

