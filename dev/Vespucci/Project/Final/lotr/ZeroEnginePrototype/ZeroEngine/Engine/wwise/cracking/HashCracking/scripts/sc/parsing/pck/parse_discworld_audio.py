#!/usr/bin/env python3
"""
Parse Discworld audio files to find event name mappings
This should help us understand the ID.bin structure and decode hex keys
"""
import struct
import json
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[4]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

def parse_stid_section(data, offset):
    """Parse STID section to get string names"""
    try:
        sig = data[offset:offset+4]
        if sig != b'STID':
            return []
        
        section_size = struct.unpack('<I', data[offset+4:offset+8])[0]
        unknown = struct.unpack('<I', data[offset+8:offset+12])[0]
        num_entries = struct.unpack('<I', data[offset+12:offset+16])[0]
        
        entries = []
        entry_offset = offset + 16
        
        for i in range(num_entries):
            if entry_offset >= offset + 8 + section_size:
                break
            
            entry_id = struct.unpack('<I', data[entry_offset:entry_offset+4])[0]
            entry_offset += 4
            
            # Read null-terminated string
            name_bytes = b''
            while entry_offset < len(data) and data[entry_offset] != 0:
                name_bytes += bytes([data[entry_offset]])
                entry_offset += 1
            
            entry_offset += 1  # Skip null terminator
            
            try:
                name = name_bytes.decode('ascii')
                entries.append({'id': entry_id, 'name': name})
            except:
                pass
        
        return entries
    except:
        return []

def parse_bnk_file(filename):
    """Parse a .bnk file for STID sections"""
    print(f"\n[*] Parsing {filename}...")
    
    with open(filename, 'rb') as f:
        data = f.read()
    
    print(f"  File size: {len(data)} bytes")
    
    # Find all STID sections
    all_names = []
    offset = 0
    
    while True:
        pos = data.find(b'STID', offset)
        if pos == -1:
            break
        
        entries = parse_stid_section(data, pos)
        all_names.extend(entries)
        offset = pos + 4
    
    return all_names

def parse_id_bin(filename):
    """Parse CoriSoundID.bin to understand structure"""
    print(f"\n[*] Parsing {filename}...")
    
    with open(filename, 'rb') as f:
        data = f.read()
    
    print(f"  File size: {len(data)} bytes")
    
    # Try to find patterns
    # Look for readable strings
    strings = []
    current_str = b''
    
    for i, byte in enumerate(data):
        if 32 <= byte <= 126:  # Printable ASCII
            current_str += bytes([byte])
        elif byte == 0 and len(current_str) >= 4:
            try:
                s = current_str.decode('ascii')
                strings.append({'offset': i - len(current_str), 'string': s})
            except:
                pass
            current_str = b''
        else:
            current_str = b''
    
    return strings

def main():
    print("="*80)
    print("PARSING DISCWORLD AUDIO FILES")
    print("="*80)
    
    # Parse CoriSoundID.audio.json
    print("\n[1/4] Loading CoriSoundID.audio.json...")
    with open('Discworld/audio/CoriSoundID.audio.json', 'r') as f:
        cori_data = json.load(f)
    
    obj1s = cori_data.get('obj1s', [])
    print(f"  obj1s entries: {len(obj1s)}")
    
    # Count hex vs readable keys
    hex_keys = [e for e in obj1s if e['key'].startswith('0x')]
    readable_keys = [e for e in obj1s if not e['key'].startswith('0x')]
    print(f"  Hex keys: {len(hex_keys)}")
    print(f"  Readable keys: {len(readable_keys)}")
    
    # Parse CoriSoundID.bin
    print("\n[2/4] Parsing CoriSoundID.bin...")
    bin_strings = parse_id_bin('Discworld/audio/CoriSoundID.bin')
    print(f"  Found {len(bin_strings)} strings in .bin file")
    
    # Parse VO_Cori.bnk
    print("\n[3/4] Parsing VO_Cori.bnk...")
    bnk_names = parse_bnk_file('Discworld/audio/English(US)/VO_Cori.bnk')
    print(f"  Found {len(bnk_names)} STID entries")
    
    # Parse Level_Cori.bnk
    print("\n[4/4] Parsing Level_Cori.bnk...")
    level_names = parse_bnk_file('Discworld/audio/Level_Cori.bnk')
    print(f"  Found {len(level_names)} STID entries")
    
    # Combine all names
    all_names = []
    all_names.extend([e['name'] for e in bnk_names])
    all_names.extend([e['name'] for e in level_names])
    all_names.extend([s['string'] for s in bin_strings])
    
    print(f"\n[*] Total unique names: {len(set(all_names))}")
    
    # Test names against hex keys
    print("\n[*] Testing names against hex keys...")
    
    hex_lookup = {}
    for entry in obj1s:
        if entry['key'].startswith('0x'):
            hex_val = int(entry['key'], 16)
            hex_lookup[hex_val] = entry
    
    matches = []
    
    for name in set(all_names):
        # Test FNV-1 hash
        h = fnv1_hash(name)
        if h in hex_lookup:
            matches.append({
                'name': name,
                'hash': h,
                'hex_key': hex_lookup[h]['key'],
                'val': hex_lookup[h]['val'],
                'variation': 'original'
            })
        
        # Test lowercase
        h_lower = fnv1_hash(name.lower())
        if h_lower in hex_lookup:
            matches.append({
                'name': name,
                'hash': h_lower,
                'hex_key': hex_lookup[h_lower]['key'],
                'val': hex_lookup[h_lower]['val'],
                'variation': 'lowercase'
            })
    
    print(f"  Found {len(matches)} matches!")
    
    # Generate report
    print("\n[*] Generating report...")
    
    with open('discworld_audio_analysis.txt', 'w', encoding='utf-8') as f:
        f.write("DISCWORLD AUDIO ANALYSIS\n")
        f.write("="*80 + "\n\n")
        f.write(f"CoriSoundID.audio.json entries: {len(obj1s)}\n")
        f.write(f"  Hex keys: {len(hex_keys)}\n")
        f.write(f"  Readable keys: {len(readable_keys)}\n\n")
        f.write(f"Strings from CoriSoundID.bin: {len(bin_strings)}\n")
        f.write(f"STID names from VO_Cori.bnk: {len(bnk_names)}\n")
        f.write(f"STID names from Level_Cori.bnk: {len(level_names)}\n")
        f.write(f"Total unique names: {len(set(all_names))}\n")
        f.write(f"Hex key matches: {len(matches)}\n\n")
        f.write("="*80 + "\n\n")
        
        if matches:
            f.write("HEX KEY MATCHES (DECODED!)\n")
            f.write("-"*80 + "\n\n")
            for m in matches:
                f.write(f"Name: {m['name']} ({m['variation']})\n")
                f.write(f"  Hash: 0x{m['hash']:08X}\n")
                f.write(f"  Hex Key: {m['hex_key']}\n")
                f.write(f"  Val: {m['val']}\n\n")
        
        f.write("\n" + "="*80 + "\n\n")
        f.write("ALL STRINGS FROM CoriSoundID.bin\n")
        f.write("-"*80 + "\n\n")
        for s in bin_strings:
            f.write(f"Offset 0x{s['offset']:08X}: {s['string']}\n")
        
        f.write("\n" + "="*80 + "\n\n")
        f.write("ALL STID NAMES FROM VO_Cori.bnk\n")
        f.write("-"*80 + "\n\n")
        for e in bnk_names:
            f.write(f"ID {e['id']} (0x{e['id']:08X}): {e['name']}\n")
        
        f.write("\n" + "="*80 + "\n\n")
        f.write("ALL STID NAMES FROM Level_Cori.bnk\n")
        f.write("-"*80 + "\n\n")
        for e in level_names:
            f.write(f"ID {e['id']} (0x{e['id']:08X}): {e['name']}\n")
    
    print("\n" + "="*80)
    print("RESULTS")
    print("="*80)
    print(f"✓ Parsed Discworld audio files")
    print(f"✓ Found {len(set(all_names))} unique names")
    print(f"✓ Decoded {len(matches)} hex keys!")
    
    if matches:
        print(f"\n🎉 SUCCESS! Found hex key mappings in Discworld files!")
        print("\nExample decoded names:")
        for m in matches[:10]:
            print(f"  {m['name']} → {m['hex_key']}")
    
    print(f"\n✓ Report saved to discworld_audio_analysis.txt")
    
    # Now apply this to main WWiseIDTable
    print("\n" + "="*80)
    print("APPLYING TO MAIN WWiseIDTable.audio.json")
    print("="*80)
    
    print("\n[*] Loading main WWiseIDTable...")
    with open('WWiseIDTable.audio.json', 'r') as f:
        main_data = json.load(f)
    
    main_entries = main_data.get('obj1s', [])
    print(f"  Loaded {len(main_entries)} entries")
    
    # Test all Discworld names against main table
    main_hex_lookup = {}
    for entry in main_entries:
        if entry['key'].startswith('0x'):
            hex_val = int(entry['key'], 16)
            main_hex_lookup[hex_val] = entry
    
    main_matches = []
    
    for name in set(all_names):
        h = fnv1_hash(name)
        if h in main_hex_lookup:
            main_matches.append({'name': name, 'hex_key': main_hex_lookup[h]['key']})
        
        h_lower = fnv1_hash(name.lower())
        if h_lower in main_hex_lookup:
            main_matches.append({'name': name + ' (lower)', 'hex_key': main_hex_lookup[h_lower]['key']})
    
    print(f"  Matches in main table: {len(main_matches)}")
    
    if main_matches:
        print(f"\n✓ Found {len(main_matches)} hex keys in main WWiseIDTable!")
        print("\nExample:")
        for m in main_matches[:5]:
            print(f"  {m['name']} → {m['hex_key']}")

if __name__ == '__main__':
    main()
