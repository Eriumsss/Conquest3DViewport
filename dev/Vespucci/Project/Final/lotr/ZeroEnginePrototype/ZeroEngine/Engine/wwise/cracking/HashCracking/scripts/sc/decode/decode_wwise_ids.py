#!/usr/bin/env python3
"""
Decode WWiseIDTable hex keys and vals by searching game executable
"""
import json
import struct
import re
from collections import defaultdict

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data.get('obj1s', [])

def hex_to_int(hex_str):
    """Convert '0x3E7CED61' to integer"""
    return int(hex_str, 16)

def search_binary_for_id(data, numeric_id):
    """Search for a numeric ID in binary data (little-endian 32-bit)"""
    id_bytes = struct.pack('<I', numeric_id)
    positions = []
    offset = 0
    while True:
        pos = data.find(id_bytes, offset)
        if pos == -1:
            break
        positions.append(pos)
        offset = pos + 1
    return positions

def extract_string_near_offset(data, offset, max_distance=200):
    """Extract ASCII strings near an offset"""
    start = max(0, offset - max_distance)
    end = min(len(data), offset + max_distance)
    chunk = data[start:end]
    
    # Find ASCII strings (min length 4)
    strings = re.findall(b'[ -~]{4,}', chunk)
    return [s.decode('ascii', errors='ignore') for s in strings]

def analyze_wwise_hash(key_str):
    """Analyze if hex key might be a hash"""
    if not key_str.startswith('0x'):
        return None
    
    hex_val = hex_to_int(key_str)
    
    # Check if it looks like a common hash
    analysis = {
        'hex': key_str,
        'int': hex_val,
        'likely_hash': True if hex_val > 0x10000000 else False
    }
    
    return analysis

def main():
    print("="*80)
    print("WWISE ID DECODER - Analyzing hex keys and vals")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/5] Loading WWiseIDTable.audio.json...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    print(f"  Total entries: {len(entries)}")
    
    # Separate hex keys and readable keys
    hex_entries = [e for e in entries if e['key'].startswith('0x')]
    readable_entries = [e for e in entries if not e['key'].startswith('0x')]
    
    print(f"  Hex key entries: {len(hex_entries)}")
    print(f"  Readable key entries: {len(readable_entries)}")
    
    # Analyze val distribution
    print("\n[2/5] Analyzing 'val' distribution...")
    val_to_keys = defaultdict(list)
    for entry in entries:
        val_to_keys[entry['val']].append(entry['key'])
    
    print(f"  Unique vals: {len(val_to_keys)}")
    print(f"  Vals with multiple keys: {sum(1 for v in val_to_keys.values() if len(v) > 1)}")
    
    # Show examples of vals with multiple keys
    print("\n  Examples of vals with multiple keys:")
    multi_key_vals = [(val, keys) for val, keys in val_to_keys.items() if len(keys) > 1]
    for val, keys in multi_key_vals[:5]:
        print(f"    Val {val}: {len(keys)} keys")
        for key in keys[:3]:
            print(f"      - {key}")
    
    # Load game executable
    print("\n[3/5] Loading game executable...")
    exe_files = ['../ConquestLLC.exe', '../Debug.exe']
    exe_data = None
    exe_name = None
    
    for exe_file in exe_files:
        try:
            with open(exe_file, 'rb') as f:
                exe_data = f.read()
            exe_name = exe_file
            print(f"  Loaded: {exe_file} ({len(exe_data):,} bytes)")
            break
        except:
            continue
    
    if not exe_data:
        print("  ERROR: Could not load game executable!")
        return
    
    # Search for readable keys in executable
    print("\n[4/5] Searching for readable keys in executable...")
    readable_in_exe = {}
    
    for entry in readable_entries:
        key = entry['key']
        key_bytes = key.encode('ascii')
        
        positions = []
        offset = 0
        while True:
            pos = exe_data.find(key_bytes, offset)
            if pos == -1:
                break
            positions.append(pos)
            offset = pos + 1
        
        if positions:
            readable_in_exe[key] = {
                'val': entry['val'],
                'positions': positions
            }
    
    print(f"  Found {len(readable_in_exe)}/{len(readable_entries)} readable keys in executable")
    
    # Search for vals in executable
    print("\n[5/5] Searching for vals in executable...")
    vals_in_exe = {}
    sample_size = 200
    
    for i, entry in enumerate(entries[:sample_size]):
        if (i + 1) % 50 == 0:
            print(f"    Checking {i+1}/{sample_size}...")
        
        val = entry['val']
        positions = search_binary_for_id(exe_data, val)
        
        if positions:
            vals_in_exe[val] = {
                'keys': val_to_keys[val],
                'positions': positions[:10]  # Limit to first 10
            }
    
    print(f"  Found {len(vals_in_exe)}/{sample_size} vals in executable")
    
    # Generate report
    print("\n" + "="*80)
    print("GENERATING REPORT")
    print("="*80)
    
    with open('wwise_id_decoder_report.txt', 'w', encoding='utf-8') as f:
        f.write("WWISE ID DECODER REPORT\n")
        f.write("="*80 + "\n\n")
        
        # Section 1: Overview
        f.write("1. OVERVIEW\n")
        f.write("-"*80 + "\n")
        f.write(f"WWiseIDTable entries: {len(entries)}\n")
        f.write(f"Hex key entries: {len(hex_entries)}\n")
        f.write(f"Readable key entries: {len(readable_entries)}\n")
        f.write(f"Unique vals: {len(val_to_keys)}\n")
        f.write(f"\n")
        f.write(f"Game executable: {exe_name}\n")
        f.write(f"Executable size: {len(exe_data):,} bytes\n")
        f.write(f"\n")
        f.write(f"Readable keys found in exe: {len(readable_in_exe)}/{len(readable_entries)}\n")
        f.write(f"Vals found in exe: {len(vals_in_exe)}/{sample_size} (sampled)\n")
        f.write("\n\n")
        
        # Section 2: All readable keys
        f.write("2. ALL READABLE KEYS (48 TOTAL)\n")
        f.write("-"*80 + "\n")
        for entry in sorted(readable_entries, key=lambda x: x['key']):
            key = entry['key']
            val = entry['val']
            in_exe = "✓ IN EXE" if key in readable_in_exe else "✗ NOT IN EXE"
            f.write(f"{key:30s} → val: {val:12d}  {in_exe}\n")
        f.write("\n\n")
        
        # Section 3: Vals with multiple keys
        f.write("3. VALS WITH MULTIPLE KEYS\n")
        f.write("-"*80 + "\n")
        f.write("These vals have multiple hex/readable keys pointing to them:\n\n")
        
        for val, keys in sorted(multi_key_vals, key=lambda x: len(x[1]), reverse=True)[:50]:
            f.write(f"Val: {val}\n")
            f.write(f"  Keys ({len(keys)} total):\n")
            for key in keys:
                f.write(f"    - {key}\n")
            f.write("\n")
        
        # Section 4: Readable keys in executable
        f.write("\n4. READABLE KEYS FOUND IN EXECUTABLE\n")
        f.write("-"*80 + "\n")
        
        for key, data in sorted(readable_in_exe.items()):
            f.write(f"{key} (val: {data['val']})\n")
            f.write(f"  Found at {len(data['positions'])} location(s) in {exe_name}:\n")
            for pos in data['positions'][:5]:
                f.write(f"    0x{pos:08X}\n")
            f.write("\n")
        
        # Section 5: Sample hex keys
        f.write("\n5. SAMPLE HEX KEYS (First 100)\n")
        f.write("-"*80 + "\n")
        
        for entry in hex_entries[:100]:
            key = entry['key']
            val = entry['val']
            hex_int = hex_to_int(key)
            f.write(f"{key} (int: {hex_int:12d}) → val: {val:12d}\n")
    
    print("[+] Report saved to wwise_id_decoder_report.txt")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Analyzed {len(entries)} WWiseIDTable entries")
    print(f"✓ Found {len(readable_in_exe)}/{len(readable_entries)} readable keys in {exe_name}")
    print(f"✓ Found {len(vals_in_exe)}/{sample_size} vals in {exe_name}")
    print(f"✓ Identified {len(multi_key_vals)} vals with multiple keys")
    
    print("\nKey Insights:")
    print(f"  • {len(val_to_keys)} unique vals serve {len(entries)} total entries")
    print(f"  • Average {len(entries)/len(val_to_keys):.2f} keys per val")
    print(f"  • Hex keys are likely hashed versions of string names")
    print(f"  • Readable keys are direct string references")

if __name__ == '__main__':
    main()

