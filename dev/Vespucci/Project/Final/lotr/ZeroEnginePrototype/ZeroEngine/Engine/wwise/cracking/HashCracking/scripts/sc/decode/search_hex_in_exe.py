#!/usr/bin/env python3
"""
Search for hex key values directly in the executable
Maybe there's a lookup table or the hex values are stored with their names
"""
import json
import struct
import re

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data.get('obj1s', [])

def hex_to_int(hex_str):
    """Convert '0x3E7CED61' to integer"""
    return int(hex_str, 16)

def extract_string_at_offset(data, offset, max_len=100):
    """Extract null-terminated ASCII string at offset"""
    end = offset
    while end < len(data) and end < offset + max_len:
        if data[end] == 0:
            break
        if data[end] < 32 or data[end] > 126:
            break
        end += 1
    
    if end > offset:
        return data[offset:end].decode('ascii', errors='ignore')
    return None

def search_for_hex_value(data, hex_val):
    """Search for a hex value in the executable (little-endian)"""
    search_bytes = struct.pack('<I', hex_val)
    positions = []
    offset = 0
    while True:
        pos = data.find(search_bytes, offset)
        if pos == -1:
            break
        positions.append(pos)
        offset = pos + 1
    return positions

def main():
    print("="*80)
    print("SEARCHING FOR HEX VALUES IN EXECUTABLE")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/3] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    print(f"  Hex key entries: {len(hex_entries)}")
    
    # Load executable
    print("\n[2/3] Loading executable...")
    with open('../ConquestLLC.exe', 'rb') as f:
        exe_data = f.read()
    print(f"  Size: {len(exe_data):,} bytes")
    
    # Search for sample hex values
    print("\n[3/3] Searching for hex values in executable...")
    sample_size = 100
    results = []
    
    for i, entry in enumerate(hex_entries[:sample_size]):
        if (i + 1) % 20 == 0:
            print(f"    Checking {i+1}/{sample_size}...")
        
        hex_key = entry['key']
        hex_val = hex_to_int(hex_key)
        val = entry['val']
        
        # Search for the hex value itself
        positions = search_for_hex_value(exe_data, hex_val)
        
        if positions:
            # Extract strings near the first occurrence
            strings_before = []
            strings_after = []
            
            pos = positions[0]
            
            # Look for strings before
            for offset in range(max(0, pos - 200), pos, 4):
                s = extract_string_at_offset(exe_data, offset, 50)
                if s and len(s) > 3:
                    strings_before.append((offset, s))
            
            # Look for strings after
            for offset in range(pos + 4, min(len(exe_data), pos + 200), 4):
                s = extract_string_at_offset(exe_data, offset, 50)
                if s and len(s) > 3:
                    strings_after.append((offset, s))
            
            results.append({
                'hex_key': hex_key,
                'hex_val': hex_val,
                'val': val,
                'positions': positions,
                'strings_before': strings_before[-5:],  # Last 5
                'strings_after': strings_after[:5]      # First 5
            })
    
    print(f"  Found {len(results)} hex values in executable")
    
    # Generate report
    print("\n" + "="*80)
    print("GENERATING REPORT")
    print("="*80)
    
    with open('hex_in_exe_report.txt', 'w', encoding='utf-8') as f:
        f.write("HEX VALUES IN EXECUTABLE REPORT\n")
        f.write("="*80 + "\n\n")
        f.write(f"Searched: {sample_size} hex keys\n")
        f.write(f"Found: {len(results)} in executable\n\n")
        f.write("="*80 + "\n\n")
        
        for r in results:
            f.write(f"Hex Key: {r['hex_key']}\n")
            f.write(f"  Hex Value: 0x{r['hex_val']:08X}\n")
            f.write(f"  Val: {r['val']}\n")
            f.write(f"  Found at {len(r['positions'])} location(s):\n")
            for pos in r['positions'][:3]:
                f.write(f"    0x{pos:08X}\n")
            
            if r['strings_before']:
                f.write(f"\n  Strings BEFORE first occurrence:\n")
                for offset, s in r['strings_before']:
                    f.write(f"    [0x{offset:08X}] {s}\n")
            
            if r['strings_after']:
                f.write(f"\n  Strings AFTER first occurrence:\n")
                for offset, s in r['strings_after']:
                    f.write(f"    [0x{offset:08X}] {s}\n")
            
            f.write("\n" + "-"*80 + "\n\n")
    
    print("[+] Report saved to hex_in_exe_report.txt")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Searched {sample_size} hex keys")
    print(f"✓ Found {len(results)} hex values in executable")
    print(f"✓ Extracted context strings around each occurrence")
    
    if results:
        print("\nKey insight:")
        print("  The hex values ARE present in the executable!")
        print("  Check the report to see what strings are nearby.")
    else:
        print("\n⚠ Hex values not found in executable")
        print("  They may be:")
        print("  - Generated at runtime")
        print("  - Stored in a different format")
        print("  - In a different file (DLL, config, etc.)")

if __name__ == '__main__':
    main()

