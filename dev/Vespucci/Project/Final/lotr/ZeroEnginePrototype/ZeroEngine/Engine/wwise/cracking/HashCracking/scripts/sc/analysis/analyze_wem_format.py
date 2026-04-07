#!/usr/bin/env python3
"""
Analyze WEM audio format to detect codec and structure
"""
import os
import struct

WEM_DIR = "Audio/Extracted_WEMs"

print("="*100)
print("WEM FORMAT ANALYSIS")
print("="*100)

wem_files = sorted([f for f in os.listdir(WEM_DIR) if f.endswith('.wem')])[:50]

print(f"\nAnalyzing {len(wem_files)} WEM files...\n")

# Signatures to look for
signatures = {
    b'\x01vorbis': 'Vorbis identification packet',
    b'\x03vorbis': 'Vorbis comment packet',
    b'\x05vorbis': 'Vorbis setup packet',
    b'OggS': 'OGG container',
    b'RIFF': 'WAV container',
    b'\xff\xfb': 'MP3 frame',
    b'\xff\xfa': 'MP3 frame',
}

vorbis_count = 0
ogg_count = 0
riff_count = 0
mp3_count = 0
unknown_count = 0

for wem_file in wem_files:
    path = os.path.join(WEM_DIR, wem_file)
    
    with open(path, 'rb') as f:
        data = f.read(512)
    
    found = False
    
    # Check for Vorbis packets
    if b'vorbis' in data:
        vorbis_count += 1
        print(f"{wem_file}: Vorbis packet found at offset {data.find(b'vorbis')}")
        found = True
    
    # Check for OGG
    if data[:4] == b'OggS':
        ogg_count += 1
        print(f"{wem_file}: OGG container")
        found = True
    
    # Check for RIFF
    if data[:4] == b'RIFF':
        riff_count += 1
        print(f"{wem_file}: RIFF/WAV container")
        found = True
    
    # Check for MP3
    if data[:2] in [b'\xff\xfb', b'\xff\xfa']:
        mp3_count += 1
        print(f"{wem_file}: MP3 frame")
        found = True
    
    if not found:
        unknown_count += 1
        # Show first bytes
        hex_str = data[:16].hex()
        print(f"{wem_file}: Unknown format - {hex_str}")

print(f"\n" + "="*100)
print("SUMMARY")
print("="*100)
print(f"Vorbis packets: {vorbis_count}")
print(f"OGG containers: {ogg_count}")
print(f"RIFF/WAV: {riff_count}")
print(f"MP3 frames: {mp3_count}")
print(f"Unknown: {unknown_count}")

print(f"\nConclusion:")
if vorbis_count > 0:
    print("  -> Raw Vorbis audio data (needs OGG wrapper)")
elif ogg_count > 0:
    print("  -> Already OGG format")
elif riff_count > 0:
    print("  -> Already WAV format")
elif mp3_count > 0:
    print("  -> MP3 format")
else:
    print("  -> Unknown format - may be Wwise proprietary or ADPCM")

# Analyze first file in detail
print(f"\n" + "="*100)
print("DETAILED ANALYSIS - First File")
print("="*100)

first_file = os.path.join(WEM_DIR, wem_files[0])
with open(first_file, 'rb') as f:
    data = f.read(256)

print(f"\nFile: {wem_files[0]}")
print(f"Size: {os.path.getsize(first_file)} bytes")
print(f"\nFirst 256 bytes (hex):")
for i in range(0, min(256, len(data)), 16):
    hex_str = ' '.join(f'{b:02x}' for b in data[i:i+16])
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[i:i+16])
    print(f"  {i:04x}: {hex_str:<48} {ascii_str}")

# Check for Vorbis setup
if b'vorbis' in data:
    offset = data.find(b'vorbis')
    print(f"\nVorbis packet found at offset: {offset}")
    print(f"Context: {data[max(0,offset-4):offset+20].hex()}")

