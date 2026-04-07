#!/usr/bin/env python3
"""
Complete BNK file parser
"""
import struct

def parse_bnk_complete(filepath):
    """Parse complete BNK structure"""
    print(f"\n{'='*80}")
    print(f"Parsing: {filepath}")
    print('='*80)
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    print(f"File size: {len(data):,} bytes")
    
    # Parse BKHD header
    sig = data[0:4]
    if sig != b'BKHD':
        print(f"Not a BNK file! Signature: {sig}")
        return

    # Try both endianness
    bkhd_size_le = struct.unpack('<I', data[4:8])[0]
    bkhd_size_be = struct.unpack('>I', data[4:8])[0]

    # Determine endianness (reasonable BKHD size is < 10KB)
    if bkhd_size_le < 10000:
        endian = '<'
        bkhd_size = bkhd_size_le
        print(f"Format: Little-endian (PC)")
    else:
        endian = '>'
        bkhd_size = bkhd_size_be
        print(f"Format: Big-endian (Xbox)")

    print(f"BKHD section size: {bkhd_size}")

    # BKHD contains bank metadata
    bank_version = struct.unpack(f'{endian}I', data[8:12])[0]
    bank_id = struct.unpack(f'{endian}I', data[12:16])[0]

    print(f"Bank version: {bank_version}")
    print(f"Bank ID: {bank_id} (0x{bank_id:08X})")
    
    # Parse all sections after BKHD
    offset = 8 + bkhd_size  # Skip BKHD header + data

    print(f"\nSections:")
    sections = []

    while offset < len(data) - 8:
        section_sig = data[offset:offset+4]

        # Check if valid ASCII signature
        if not all(32 <= b <= 126 for b in section_sig):
            break

        section_size = struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]
        
        # Sanity check
        if section_size > len(data) - offset or section_size < 0:
            break
        
        section_name = section_sig.decode('ascii')
        sections.append({
            'name': section_name,
            'offset': offset,
            'size': section_size,
            'data_offset': offset + 8,
            'data_end': offset + 8 + section_size
        })
        
        print(f"  {section_name:8s} @ 0x{offset:08X}, size: {section_size:,} bytes")
        
        # For STID, show entries
        if section_name == 'STID':
            try:
                stid_offset = offset + 8
                stid_unknown = struct.unpack(f'{endian}I', data[stid_offset:stid_offset+4])[0]
                stid_count = struct.unpack(f'{endian}I', data[stid_offset+4:stid_offset+8])[0]
                print(f"    → {stid_count} string entries")
            except:
                pass

        # For HIRC, show object count
        if section_name == 'HIRC':
            try:
                hirc_offset = offset + 8
                hirc_count = struct.unpack(f'{endian}I', data[hirc_offset:hirc_offset+4])[0]
                print(f"    → {hirc_count} HIRC objects")
            except:
                pass
        
        # For DIDX, show file count
        if section_name == 'DIDX':
            try:
                didx_offset = offset + 8
                # DIDX entries are 12 bytes each: ID (4) + Offset (4) + Size (4)
                num_files = section_size // 12
                print(f"    → {num_files} audio files")
            except:
                pass
        
        offset += 8 + section_size
    
    print(f"\nTotal sections: {len(sections)}")
    
    return sections

# Parse BNK files
print("="*80)
print("COMPLETE BNK FILE PARSING")
print("="*80)

bnk_files = [
    'Augment/Level_Cori.bnk',
    'Augment/English(US)/VO_Cori.bnk',
    'extracted_bnk/bank_024_id_05174939_le.bnk',
    'extracted_bnk/bank_025_id_09C7E577_le.bnk',
]

for bnk_file in bnk_files:
    try:
        parse_bnk_complete(bnk_file)
    except Exception as e:
        print(f"\nError parsing {bnk_file}: {e}")
        import traceback
        traceback.print_exc()

