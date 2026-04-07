#!/usr/bin/env python3
"""
Extract .bnk files from sound.pck
"""
import struct
import os

def parse_pck_header(data):
    """Parse PCK file header"""
    # Check signature
    sig = data[0:4]
    print(f"Signature: {sig}")
    
    # Try to find file table
    # Wwise PCK format typically has:
    # - 4 bytes: signature
    # - 4 bytes: header size
    # - 4 bytes: version
    # - File table with entries
    
    offset = 0
    header_size = struct.unpack('<I', data[4:8])[0]
    version = struct.unpack('<I', data[8:12])[0]
    
    print(f"Header size: {header_size}")
    print(f"Version: {version}")
    
    return header_size

def find_all_bnk_files(data):
    """Find all embedded BNK files by signature"""
    print("\nSearching for BNK files by signature...")
    
    # BNK files start with BKHD (Bank Header) or RIFX
    signatures = [b'BKHD', b'RIFX']
    
    bnk_positions = []
    
    for sig in signatures:
        offset = 0
        while True:
            pos = data.find(sig, offset)
            if pos == -1:
                break
            
            # Verify this looks like a real BNK file
            # Check if there are other Wwise sections nearby
            has_sections = False
            for check_sig in [b'DIDX', b'DATA', b'HIRC', b'STID']:
                if data.find(check_sig, pos, pos + 1024) != -1:
                    has_sections = True
                    break
            
            if has_sections or sig == b'RIFX':
                bnk_positions.append(pos)
            
            offset = pos + 4
    
    # Remove duplicates and sort
    bnk_positions = sorted(set(bnk_positions))
    
    print(f"Found {len(bnk_positions)} potential BNK files")
    
    return bnk_positions

def extract_bnk_file(data, start_pos, end_pos, output_path):
    """Extract a single BNK file"""
    bnk_data = data[start_pos:end_pos]
    
    with open(output_path, 'wb') as f:
        f.write(bnk_data)
    
    return len(bnk_data)

print("="*80)
print("EXTRACTING BNK FILES FROM sound.pck")
print("="*80)

print("\n[1/4] Opening sound.pck...")

with open('sound.pck', 'rb') as f:
    data = f.read()

file_size = len(data)
print(f"  File size: {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")

print("\n[2/4] Analyzing PCK header...")

# Show first 64 bytes
print(f"\nFirst 64 bytes (hex):")
print(data[0:64].hex())

print(f"\nFirst 64 bytes (ASCII):")
for i in range(64):
    b = data[i]
    if 32 <= b <= 126:
        print(chr(b), end='')
    else:
        print('.', end='')
print()

# Try to parse header
try:
    header_size = parse_pck_header(data)
except Exception as e:
    print(f"Could not parse header: {e}")
    header_size = 0

print("\n[3/4] Finding embedded BNK files...")

bnk_positions = find_all_bnk_files(data)

if not bnk_positions:
    print("No BNK files found!")
    exit(1)

print("\n[4/4] Extracting BNK files...")

# Create output directory
os.makedirs('extracted_bnk', exist_ok=True)

# Extract each BNK file
for idx, start_pos in enumerate(bnk_positions):
    # Determine end position (start of next BNK or end of file)
    if idx < len(bnk_positions) - 1:
        end_pos = bnk_positions[idx + 1]
    else:
        end_pos = file_size
    
    # Extract
    output_path = f'extracted_bnk/bank_{idx:03d}_offset_{start_pos:08X}.bnk'
    size = extract_bnk_file(data, start_pos, end_pos, output_path)
    
    print(f"  [{idx+1}/{len(bnk_positions)}] Extracted {size:,} bytes to {output_path}")

print("\n" + "="*80)
print("EXTRACTION COMPLETE")
print("="*80)
print(f"✓ Extracted {len(bnk_positions)} BNK files to extracted_bnk/")
print(f"✓ Total size: {sum(os.path.getsize(f'extracted_bnk/bank_{i:03d}_offset_{bnk_positions[i]:08X}.bnk') for i in range(len(bnk_positions))):,} bytes")

