#!/usr/bin/env python3
"""
Extract and attempt to decompress data around XML signatures in sound.pck
"""
import os
import zlib
import struct
import re

def find_xml_offsets(filename):
    """Find all offsets containing 'xml' (case-insensitive)"""
    offsets = []
    with open(filename, 'rb') as f:
        data = f.read()
        # Search for xml in various cases
        for match in re.finditer(b'(?i)xml', data):
            offsets.append(match.start())
    return offsets

def try_decompress(data, method_name, decompress_func):
    """Try to decompress data with given method"""
    try:
        result = decompress_func(data)
        if result and len(result) > 10:
            # Check if result contains readable text
            try:
                text = result.decode('utf-8', errors='ignore')
                if '<?xml' in text or '<' in text:
                    return (True, result, text)
            except:
                pass
            return (True, result, None)
        return (False, None, None)
    except Exception as e:
        return (False, None, None)

def extract_around_offset(filename, offset, before=1000, after=5000):
    """Extract data around an offset"""
    with open(filename, 'rb') as f:
        f.seek(max(0, offset - before))
        return f.read(before + after)

def main():
    pck_file = 'sound.pck'
    output_dir = 'extracted_xml_attempts'
    os.makedirs(output_dir, exist_ok=True)
    
    print("[+] Finding XML offsets...")
    offsets = find_xml_offsets(pck_file)
    print(f"[+] Found {len(offsets)} XML signatures")
    
    successful_extractions = []
    
    for i, offset in enumerate(offsets[:50]):  # Process first 50
        print(f"\n[*] Processing offset {i+1}/{min(50, len(offsets))}: 0x{offset:08X} ({offset})")
        
        # Extract data around the offset
        data = extract_around_offset(pck_file, offset, before=500, after=10000)
        
        # Try different decompression methods
        methods = [
            ('zlib', lambda d: zlib.decompress(d)),
            ('zlib_raw', lambda d: zlib.decompress(d, -zlib.MAX_WBITS)),
            ('gzip', lambda d: zlib.decompress(d, 16 + zlib.MAX_WBITS)),
        ]
        
        for method_name, decompress_func in methods:
            # Try decompressing from various starting points
            for start_offset in [0, 100, 200, 300, 400, 500]:
                if start_offset >= len(data):
                    continue
                    
                chunk = data[start_offset:]
                success, decompressed, text = try_decompress(chunk, method_name, decompress_func)
                
                if success and text and ('<?xml' in text or '<' in text[:100]):
                    print(f"  [✓] SUCCESS with {method_name} at start_offset={start_offset}")
                    print(f"      Decompressed size: {len(decompressed)} bytes")
                    print(f"      Preview: {text[:200]}")
                    
                    # Save the decompressed data
                    out_file = os.path.join(output_dir, f'xml_{offset:08X}_{method_name}.xml')
                    with open(out_file, 'wb') as f:
                        f.write(decompressed)
                    
                    successful_extractions.append({
                        'offset': offset,
                        'method': method_name,
                        'file': out_file,
                        'size': len(decompressed)
                    })
                    break
            else:
                continue
            break
    
    # Summary
    print("\n" + "="*80)
    print("EXTRACTION SUMMARY")
    print("="*80)
    print(f"Total XML signatures found: {len(offsets)}")
    print(f"Processed: {min(50, len(offsets))}")
    print(f"Successfully decompressed: {len(successful_extractions)}")
    
    if successful_extractions:
        print("\nSuccessful extractions:")
        for item in successful_extractions:
            print(f"  - Offset 0x{item['offset']:08X}: {item['file']} ({item['size']} bytes)")
    else:
        print("\n[!] No XML content could be decompressed.")
        print("[!] The 'xml' strings are likely random binary data, not compressed XML.")
    
    # Try alternative: look for actual XML headers
    print("\n" + "="*80)
    print("SEARCHING FOR ACTUAL XML HEADERS")
    print("="*80)
    
    with open(pck_file, 'rb') as f:
        data = f.read()
        
    # Search for <?xml
    xml_headers = []
    for match in re.finditer(b'<\?xml', data):
        xml_headers.append(match.start())
    
    print(f"Found {len(xml_headers)} actual '<?xml' headers")
    
    if xml_headers:
        for offset in xml_headers[:10]:
            print(f"\n[*] Found <?xml at offset 0x{offset:08X}")
            chunk = data[offset:offset+500]
            try:
                text = chunk.decode('utf-8', errors='ignore')
                print(f"    Preview: {text[:200]}")
            except:
                print(f"    (Binary data)")

if __name__ == '__main__':
    main()

