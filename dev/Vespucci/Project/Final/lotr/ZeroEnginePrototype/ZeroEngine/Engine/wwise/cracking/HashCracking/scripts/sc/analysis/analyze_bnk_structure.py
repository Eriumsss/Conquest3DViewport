#!/usr/bin/env python3
"""
Analyze BNK file structure to understand format
"""
import struct

def analyze_bnk(filepath):
    """Analyze a BNK file structure"""
    print(f"\n{'='*80}")
    print(f"Analyzing: {filepath}")
    print('='*80)
    
    with open(filepath, 'rb') as f:
        data = f.read()
    
    print(f"File size: {len(data):,} bytes")
    
    # Show first 64 bytes
    print(f"\nFirst 64 bytes (hex):")
    print(data[0:64].hex())
    
    # Check for RIFX signature (big-endian) or BKHD (little-endian)
    sig = data[0:4]
    print(f"\nSignature: {sig}")
    
    if sig == b'RIFX':
        print("  Format: RIFX (big-endian, Xbox)")
        endian = '>'
        
        # RIFX format:
        # 4 bytes: 'RIFX'
        # 4 bytes: file size - 8
        # 4 bytes: 'WAVE' or bank type
        
        chunk_size = struct.unpack('>I', data[4:8])[0]
        bank_type = data[8:12]
        
        print(f"  Chunk size: {chunk_size}")
        print(f"  Bank type: {bank_type}")
        
        offset = 12
        
    elif sig == b'BKHD':
        print("  Format: BKHD (little-endian, PC)")
        endian = '<'
        offset = 0
    else:
        print(f"  Unknown format!")
        return
    
    # Parse sections
    print(f"\nSections found:")
    sections = []
    
    while offset < len(data) - 8:
        section_sig = data[offset:offset+4]
        
        # Check if it's a valid section signature
        if not all(32 <= b <= 126 for b in section_sig):
            offset += 1
            continue
        
        try:
            section_size = struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]
            
            # Sanity check
            if section_size > len(data) or section_size < 0:
                offset += 1
                continue
            
            section_name = section_sig.decode('ascii', errors='ignore')
            sections.append({
                'name': section_name,
                'offset': offset,
                'size': section_size
            })
            
            print(f"  {section_name:8s} @ 0x{offset:08X}, size: {section_size:,} bytes")
            
            # Move to next section
            offset += 8 + section_size
            
        except:
            offset += 1
    
    print(f"\nTotal sections: {len(sections)}")
    
    return {
        'filepath': filepath,
        'size': len(data),
        'signature': sig,
        'endian': endian,
        'sections': sections
    }

# Analyze multiple BNK files
print("="*80)
print("BNK FILE STRUCTURE ANALYSIS")
print("="*80)

bnk_files = [
    'Augment/Level_Cori.bnk',
    'Augment/English(US)/VO_Cori.bnk',
    'Discworld/audio/Level_Cori.bnk',
]

results = []

for bnk_file in bnk_files:
    try:
        result = analyze_bnk(bnk_file)
        results.append(result)
    except Exception as e:
        print(f"\nError analyzing {bnk_file}: {e}")

# Summary
print(f"\n{'='*80}")
print("SUMMARY")
print('='*80)

for r in results:
    print(f"\n{r['filepath']}:")
    print(f"  Size: {r['size']:,} bytes")
    print(f"  Signature: {r['signature']}")
    print(f"  Sections: {len(r['sections'])}")
    print(f"  Section types: {', '.join(set(s['name'] for s in r['sections']))}")

