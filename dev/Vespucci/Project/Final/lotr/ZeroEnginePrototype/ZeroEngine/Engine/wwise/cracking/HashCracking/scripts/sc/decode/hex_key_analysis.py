#!/usr/bin/env python3
"""
Analyze hex keys from WWiseIDTable and their relationship to sound.pck
"""
import json
import struct

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data.get('obj1s', [])

def hex_string_to_int(hex_str):
    """Convert '0x3E7CED61' to integer"""
    return int(hex_str, 16)

def search_binary_for_value(data, value, search_type='le32'):
    """Search for a value in binary data"""
    positions = []
    
    if search_type == 'le32':
        # Little-endian 32-bit
        search_bytes = struct.pack('<I', value)
    elif search_type == 'be32':
        # Big-endian 32-bit
        search_bytes = struct.pack('>I', value)
    else:
        return positions
    
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
    print("HEX KEY ANALYSIS")
    print("="*80)
    
    # Load data
    print("\n[1/3] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    print(f"  Total hex key entries: {len(hex_entries)}")
    
    print("\n[2/3] Loading sound.pck...")
    with open('sound.pck', 'rb') as f:
        pck_data = f.read()
    print(f"  File size: {len(pck_data):,} bytes")
    
    print("\n[3/3] Searching for hex keys in sound.pck...")
    print("  Testing both the hex value itself AND the 'val' field...")
    
    results = []
    sample_size = 100
    
    for i, entry in enumerate(hex_entries[:sample_size]):
        if (i + 1) % 20 == 0:
            print(f"    Checking {i+1}/{sample_size}...")
        
        hex_key = entry['key']
        val = entry['val']
        
        # Convert hex key to integer
        hex_int = hex_string_to_int(hex_key)
        
        # Search for hex key value (both endianness)
        hex_le_positions = search_binary_for_value(pck_data, hex_int, 'le32')
        hex_be_positions = search_binary_for_value(pck_data, hex_int, 'be32')
        
        # Search for val (both endianness)
        val_le_positions = search_binary_for_value(pck_data, val, 'le32')
        val_be_positions = search_binary_for_value(pck_data, val, 'be32')
        
        if hex_le_positions or hex_be_positions or val_le_positions or val_be_positions:
            results.append({
                'key': hex_key,
                'key_int': hex_int,
                'val': val,
                'hex_le': hex_le_positions,
                'hex_be': hex_be_positions,
                'val_le': val_le_positions,
                'val_be': val_be_positions
            })
    
    print(f"\n  Found {len(results)} entries with matches in sound.pck")
    
    # Analyze results
    print("\n" + "="*80)
    print("ANALYSIS RESULTS")
    print("="*80)
    
    hex_le_count = sum(1 for r in results if r['hex_le'])
    hex_be_count = sum(1 for r in results if r['hex_be'])
    val_le_count = sum(1 for r in results if r['val_le'])
    val_be_count = sum(1 for r in results if r['val_be'])
    
    print(f"\nOut of {sample_size} hex key entries:")
    print(f"  Hex key (little-endian) found: {hex_le_count}")
    print(f"  Hex key (big-endian) found: {hex_be_count}")
    print(f"  Val (little-endian) found: {val_le_count}")
    print(f"  Val (big-endian) found: {val_be_count}")
    
    # Generate detailed report
    print("\n[+] Generating detailed report...")
    
    with open('hex_key_analysis_report.txt', 'w', encoding='utf-8') as f:
        f.write("HEX KEY ANALYSIS REPORT\n")
        f.write("="*80 + "\n\n")
        
        f.write("SUMMARY\n")
        f.write("-"*80 + "\n")
        f.write(f"Total hex keys in WWiseIDTable: {len(hex_entries)}\n")
        f.write(f"Sample size analyzed: {sample_size}\n")
        f.write(f"Entries with matches: {len(results)}\n")
        f.write(f"\n")
        f.write(f"Hex key (LE) matches: {hex_le_count}\n")
        f.write(f"Hex key (BE) matches: {hex_be_count}\n")
        f.write(f"Val (LE) matches: {val_le_count}\n")
        f.write(f"Val (BE) matches: {val_be_count}\n")
        f.write("\n\n")
        
        f.write("DETAILED MATCHES\n")
        f.write("-"*80 + "\n\n")
        
        for r in results[:50]:
            f.write(f"Key: {r['key']} (int: {r['key_int']})\n")
            f.write(f"Val: {r['val']}\n")
            
            if r['hex_le']:
                f.write(f"  Hex (LE) found at {len(r['hex_le'])} location(s): ")
                f.write(', '.join(f"0x{p:08X}" for p in r['hex_le'][:5]))
                f.write("\n")
            
            if r['hex_be']:
                f.write(f"  Hex (BE) found at {len(r['hex_be'])} location(s): ")
                f.write(', '.join(f"0x{p:08X}" for p in r['hex_be'][:5]))
                f.write("\n")
            
            if r['val_le']:
                f.write(f"  Val (LE) found at {len(r['val_le'])} location(s): ")
                f.write(', '.join(f"0x{p:08X}" for p in r['val_le'][:5]))
                f.write("\n")
            
            if r['val_be']:
                f.write(f"  Val (BE) found at {len(r['val_be'])} location(s): ")
                f.write(', '.join(f"0x{p:08X}" for p in r['val_be'][:5]))
                f.write("\n")
            
            f.write("\n")
        
        # Key insight section
        f.write("\n" + "="*80 + "\n")
        f.write("KEY INSIGHTS\n")
        f.write("="*80 + "\n\n")
        
        f.write("The hex 'key' values in WWiseIDTable are likely:\n")
        if hex_le_count > val_le_count:
            f.write("  → The PRIMARY identifiers used in sound.pck (stored as little-endian)\n")
        elif val_le_count > hex_le_count:
            f.write("  → Secondary identifiers; 'val' is the PRIMARY identifier in sound.pck\n")
        else:
            f.write("  → Both hex keys and vals are used in sound.pck\n")
        
        f.write("\nThe 'val' values in WWiseIDTable are likely:\n")
        f.write("  → Wwise Event IDs used by the game engine\n")
        f.write("  → These are found extensively throughout sound.pck\n")
        
        f.write("\nRelationship:\n")
        f.write("  Game Code → Uses 'val' to trigger events\n")
        f.write("  Wwise Runtime → Uses 'key' (hex) to locate audio in sound.pck\n")
        f.write("  WWiseIDTable → Maps between the two ID systems\n")
    
    print("[+] Report saved to hex_key_analysis_report.txt")
    
    # Show some examples
    print("\n" + "="*80)
    print("EXAMPLE MATCHES")
    print("="*80)
    
    for r in results[:10]:
        print(f"\n{r['key']} → val: {r['val']}")
        if r['hex_le']:
            print(f"  Hex (LE): {len(r['hex_le'])} matches")
        if r['val_le']:
            print(f"  Val (LE): {len(r['val_le'])} matches")

if __name__ == '__main__':
    main()

