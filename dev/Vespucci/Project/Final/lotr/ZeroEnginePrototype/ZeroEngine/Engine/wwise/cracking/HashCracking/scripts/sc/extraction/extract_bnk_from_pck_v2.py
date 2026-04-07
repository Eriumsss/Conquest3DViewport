#!/usr/bin/env python3
"""
Extract BNK files from sound.pck (Version 2 - using BNK structure knowledge)
"""
import struct
import os
import mmap

def find_bnk_files(data):
    """Find all BNK files by BKHD signature"""
    print("Searching for BNK files (BKHD signature)...")
    
    positions = []
    offset = 0
    
    while True:
        pos = data.find(b'BKHD', offset)
        if pos == -1:
            break
        
        positions.append(pos)
        offset = pos + 4
    
    print(f"Found {len(positions)} BKHD signatures")
    
    return positions

def get_bnk_size(data, start_pos):
    """Calculate BNK file size by parsing sections"""
    try:
        # Parse BKHD header
        bkhd_size_le = struct.unpack('<I', data[start_pos+4:start_pos+8])[0]
        bkhd_size_be = struct.unpack('>I', data[start_pos+4:start_pos+8])[0]
        
        # Determine endianness
        if bkhd_size_le < 10000:
            endian = '<'
            bkhd_size = bkhd_size_le
        else:
            endian = '>'
            bkhd_size = bkhd_size_be
        
        # Start after BKHD
        offset = start_pos + 8 + bkhd_size
        last_section_end = offset
        
        # Parse all sections
        while offset < len(data) - 8:
            section_sig = data[offset:offset+4]
            
            # Check if valid section signature
            if not all(32 <= b <= 126 for b in section_sig):
                break
            
            section_size = struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]
            
            # Sanity check
            if section_size > 100*1024*1024 or section_size < 0:  # Max 100MB per section
                break
            
            last_section_end = offset + 8 + section_size
            offset = last_section_end
        
        total_size = last_section_end - start_pos
        return total_size, endian
        
    except Exception as e:
        return None, None

print("="*80)
print("EXTRACTING BNK FILES FROM sound.pck")
print("="*80)

print("\n[1/3] Opening sound.pck with memory mapping...")

with open('sound.pck', 'rb') as f:
    with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        file_size = len(data)
        print(f"  File size: {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")
        
        print("\n[2/3] Finding BNK files...")
        bnk_positions = find_bnk_files(data)
        
        print("\n[3/3] Extracting BNK files...")
        
        # Create output directory
        os.makedirs('extracted_bnk', exist_ok=True)
        
        extracted = 0
        skipped = 0
        
        for idx, start_pos in enumerate(bnk_positions):
            if idx % 10 == 0:
                print(f"  Processing {idx+1}/{len(bnk_positions)}...")
            
            # Get BNK size
            bnk_size, endian = get_bnk_size(data, start_pos)
            
            if bnk_size is None or bnk_size < 100 or bnk_size > 50*1024*1024:
                skipped += 1
                continue
            
            # Extract BNK
            bnk_data = data[start_pos:start_pos+bnk_size]
            
            # Get bank ID for filename
            try:
                if endian == '<':
                    bank_id = struct.unpack('<I', bnk_data[12:16])[0]
                else:
                    bank_id = struct.unpack('>I', bnk_data[12:16])[0]
            except:
                bank_id = idx
            
            endian_str = 'be' if endian == '>' else 'le'
            output_path = f'extracted_bnk/bank_{extracted:03d}_id_{bank_id:08X}_{endian_str}.bnk'
            
            with open(output_path, 'wb') as f:
                f.write(bnk_data)
            
            extracted += 1

print(f"\n{'='*80}")
print("EXTRACTION COMPLETE")
print('='*80)
print(f"✓ Found {len(bnk_positions)} BKHD signatures")
print(f"✓ Extracted {extracted} valid BNK files")
print(f"✓ Skipped {skipped} invalid entries")
print(f"✓ Output directory: extracted_bnk/")

# Calculate total size
total_size = sum(os.path.getsize(f'extracted_bnk/{f}') for f in os.listdir('extracted_bnk') if f.endswith('.bnk'))
print(f"✓ Total extracted size: {total_size:,} bytes ({total_size/1024/1024:.2f} MB)")

