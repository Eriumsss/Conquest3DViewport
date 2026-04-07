#!/usr/bin/env python3
"""
Match WWiseIDTable hex IDs to actual strings in sound.pck
"""
import json
import struct
import re
from collections import defaultdict

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)

    # Get the obj1s array
    entries = data.get('obj1s', [])

    # Separate hex IDs from readable names
    hex_ids = {}
    readable_names = {}

    for entry in entries:
        key = entry['key']
        val = entry['val']

        if key.startswith('0x'):
            # Hex ID
            hex_ids[key] = val
        else:
            # Readable name
            readable_names[key] = val

    return hex_ids, readable_names

def extract_strings_from_pck(filename, min_length=4):
    """Extract all ASCII and UTF-16LE strings from PCK file"""
    with open(filename, 'rb') as f:
        data = f.read()
    
    strings_with_offsets = []
    
    # Extract ASCII strings
    ascii_pattern = rb'[ -~]{' + str(min_length).encode() + rb',}'
    for match in re.finditer(ascii_pattern, data):
        offset = match.start()
        string = match.group().decode('ascii')
        strings_with_offsets.append((offset, string, 'ascii'))
    
    # Extract UTF-16LE strings
    utf16_pattern = rb'(?:[ -~]\x00){' + str(min_length).encode() + rb',}'
    for match in re.finditer(utf16_pattern, data):
        offset = match.start()
        try:
            string = match.group().decode('utf-16le')
            strings_with_offsets.append((offset, string, 'utf16le'))
        except:
            pass
    
    return strings_with_offsets

def find_string_by_id(pck_data, wwise_id):
    """Try to find a string associated with a Wwise ID"""
    # Convert ID to different byte representations
    id_bytes_le = struct.pack('<I', wwise_id)  # Little-endian 32-bit
    id_bytes_be = struct.pack('>I', wwise_id)  # Big-endian 32-bit
    
    results = []
    
    # Search for the ID in the file
    for byte_repr in [id_bytes_le, id_bytes_be]:
        offset = 0
        while True:
            pos = pck_data.find(byte_repr, offset)
            if pos == -1:
                break
            results.append(pos)
            offset = pos + 1
    
    return results

def main():
    print("[+] Loading WWiseIDTable.audio.json...")
    hex_ids, readable_names = load_wwise_table('WWiseIDTable.audio.json')
    
    print(f"[+] Found {len(hex_ids)} hex IDs")
    print(f"[+] Found {len(readable_names)} readable names")
    
    print("\n[+] Extracting strings from sound.pck...")
    strings_with_offsets = extract_strings_from_pck('sound.pck', min_length=6)
    print(f"[+] Extracted {len(strings_with_offsets)} strings")
    
    # Create a lookup by string content
    string_lookup = defaultdict(list)
    for offset, string, encoding in strings_with_offsets:
        string_lookup[string.lower()].append((offset, encoding))
    
    print("\n" + "="*80)
    print("MATCHING READABLE NAMES TO STRINGS IN PCK")
    print("="*80)
    
    matched = 0
    for name, wwise_id in sorted(readable_names.items())[:100]:  # First 100
        # Check if this name appears in the extracted strings
        if name.lower() in string_lookup:
            offsets = string_lookup[name.lower()]
            print(f"\n✓ '{name}' (ID: {wwise_id})")
            for offset, encoding in offsets[:3]:  # Show first 3 matches
                print(f"    Found at offset 0x{offset:08X} ({encoding})")
            matched += 1
    
    print(f"\n[+] Matched {matched} readable names to strings in PCK")
    
    # Now check hex IDs
    print("\n" + "="*80)
    print("ANALYZING HEX IDs")
    print("="*80)
    
    with open('sound.pck', 'rb') as f:
        pck_data = f.read()
    
    print("\nSample hex IDs and their locations in PCK:")
    for hex_id, wwise_id in list(hex_ids.items())[:20]:  # First 20 hex IDs
        positions = find_string_by_id(pck_data, wwise_id)
        if positions:
            print(f"\n{hex_id} -> ID {wwise_id}")
            print(f"  Found at {len(positions)} location(s):")
            for pos in positions[:3]:
                # Show context around the ID
                context_start = max(0, pos - 20)
                context_end = min(len(pck_data), pos + 24)
                context = pck_data[context_start:context_end]
                
                # Try to find readable strings nearby
                nearby_ascii = re.findall(rb'[ -~]{4,}', context)
                nearby_utf16 = re.findall(rb'(?:[ -~]\x00){4,}', context)
                
                print(f"    0x{pos:08X}")
                if nearby_ascii:
                    print(f"      Nearby ASCII: {[s.decode('ascii') for s in nearby_ascii[:2]]}")
                if nearby_utf16:
                    try:
                        print(f"      Nearby UTF16: {[s.decode('utf-16le') for s in nearby_utf16[:2]]}")
                    except:
                        pass
    
    # Export results
    print("\n[+] Exporting matched strings...")
    with open('wwise_string_matches.txt', 'w', encoding='utf-8') as f:
        f.write("WWISE ID TABLE STRING MATCHES\n")
        f.write("="*80 + "\n\n")
        
        f.write("READABLE NAMES FOUND IN PCK:\n")
        f.write("-"*80 + "\n")
        for name, wwise_id in sorted(readable_names.items()):
            if name.lower() in string_lookup:
                offsets = string_lookup[name.lower()]
                f.write(f"\n'{name}' (Wwise ID: {wwise_id})\n")
                for offset, encoding in offsets:
                    f.write(f"  0x{offset:08X} ({encoding})\n")
        
        f.write("\n\n" + "="*80 + "\n")
        f.write("ALL EXTRACTED STRINGS (SAMPLE):\n")
        f.write("-"*80 + "\n")
        for offset, string, encoding in strings_with_offsets[:500]:
            f.write(f"0x{offset:08X}: {string} ({encoding})\n")
    
    print("[+] Results saved to wwise_string_matches.txt")

if __name__ == '__main__':
    main()

