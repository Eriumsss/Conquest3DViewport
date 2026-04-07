#!/usr/bin/env python3
"""
Analyze the context around XML signatures to understand what they are
"""
import re
import struct

def find_xml_offsets(filename):
    """Find all offsets containing 'xml' (case-insensitive)"""
    offsets = []
    with open(filename, 'rb') as f:
        data = f.read()
        for match in re.finditer(b'(?i)xml', data):
            offsets.append(match.start())
    return offsets

def analyze_context(filename, offset, window=200):
    """Analyze data around an offset"""
    with open(filename, 'rb') as f:
        f.seek(max(0, offset - window))
        before = f.read(window)
        f.seek(offset)
        at_offset = f.read(20)
        after = f.read(window)
    
    return before, at_offset, after

def check_for_patterns(data):
    """Check for known file signatures and patterns"""
    patterns = {
        'RIFF': b'RIFF',
        'vorbis': b'vorbis',
        'OggS': b'OggS',
        'BKHD': b'BKHD',
        'HIRC': b'HIRC',
        'DATA': b'DATA',
        'STID': b'STID',
        'null_bytes': b'\x00\x00\x00\x00',
        'xml_open': b'<?xml',
        'xml_tag': b'<xml',
        'angle_bracket': b'<',
    }
    
    found = {}
    for name, pattern in patterns.items():
        if pattern in data:
            found[name] = data.find(pattern)
    
    return found

def main():
    pck_file = 'sound.pck'
    
    print("[+] Finding XML offsets...")
    offsets = find_xml_offsets(pck_file)
    print(f"[+] Found {len(offsets)} XML signatures\n")
    
    # Analyze first 20 in detail
    print("="*80)
    print("DETAILED ANALYSIS OF FIRST 20 OFFSETS")
    print("="*80)
    
    for i, offset in enumerate(offsets[:20]):
        print(f"\n[{i+1}] Offset: 0x{offset:08X} ({offset})")
        
        before, at_offset, after = analyze_context(pck_file, offset, window=100)
        
        # Show hex dump around the offset
        print(f"  Before (last 32 bytes):")
        print(f"    {before[-32:].hex(' ')}")
        
        print(f"  At offset (20 bytes):")
        print(f"    {at_offset.hex(' ')}")
        print(f"    ASCII: {at_offset.decode('ascii', errors='replace')}")
        
        print(f"  After (first 32 bytes):")
        print(f"    {after[:32].hex(' ')}")
        
        # Check for patterns
        combined = before + at_offset + after
        patterns = check_for_patterns(combined)
        if patterns:
            print(f"  Patterns found: {', '.join(patterns.keys())}")
        
        # Check if it's part of a string
        try:
            # Try to find a longer readable string
            readable = b''
            for byte in at_offset + after[:100]:
                if 32 <= byte <= 126:  # Printable ASCII
                    readable += bytes([byte])
                else:
                    if len(readable) > 3:
                        break
                    readable = b''
            
            if len(readable) > 10:
                print(f"  Extended string: {readable.decode('ascii', errors='replace')}")
        except:
            pass
    
    # Statistical analysis
    print("\n" + "="*80)
    print("STATISTICAL ANALYSIS")
    print("="*80)
    
    # Check spacing between offsets
    if len(offsets) > 1:
        spacings = [offsets[i+1] - offsets[i] for i in range(len(offsets)-1)]
        avg_spacing = sum(spacings) / len(spacings)
        min_spacing = min(spacings)
        max_spacing = max(spacings)
        
        print(f"Average spacing: {avg_spacing:.0f} bytes")
        print(f"Min spacing: {min_spacing} bytes")
        print(f"Max spacing: {max_spacing} bytes")
        
        # Check for regular patterns
        common_spacings = {}
        for spacing in spacings:
            rounded = round(spacing / 1000) * 1000  # Round to nearest 1000
            common_spacings[rounded] = common_spacings.get(rounded, 0) + 1
        
        print(f"\nMost common spacing ranges:")
        for spacing, count in sorted(common_spacings.items(), key=lambda x: x[1], reverse=True)[:5]:
            print(f"  ~{spacing:,} bytes: {count} occurrences")
    
    # Check case distribution
    with open(pck_file, 'rb') as f:
        data = f.read()
    
    case_dist = {'xml': 0, 'XML': 0, 'Xml': 0, 'xML': 0, 'XmL': 0, 'xMl': 0, 'XMl': 0, 'xmL': 0}
    for variant in case_dist.keys():
        case_dist[variant] = len(re.findall(variant.encode(), data))
    
    print(f"\nCase distribution:")
    for variant, count in sorted(case_dist.items(), key=lambda x: x[1], reverse=True):
        if count > 0:
            print(f"  '{variant}': {count} occurrences")

if __name__ == '__main__':
    main()

