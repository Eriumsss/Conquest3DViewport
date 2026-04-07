#!/usr/bin/env python3
"""
Use Discworld prototype data to decode hex keys in main WWiseIDTable
"""
import struct
import json
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

def parse_stid_xbox(data, offset):
    """Parse STID section (Xbox big-endian format)"""
    try:
        sig = data[offset:offset+4]
        if sig != b'STID':
            return []
        
        section_size = struct.unpack('>I', data[offset+4:offset+8])[0]
        num_entries = struct.unpack('>I', data[offset+12:offset+16])[0]
        
        entries = []
        entry_offset = offset + 16
        
        for i in range(num_entries):
            if entry_offset >= offset + 8 + section_size:
                break
            
            entry_id = struct.unpack('>I', data[entry_offset:entry_offset+4])[0]
            entry_offset += 4
            
            name_bytes = b''
            while entry_offset < len(data) and data[entry_offset] != 0:
                name_bytes += bytes([data[entry_offset]])
                entry_offset += 1
            
            entry_offset += 1
            
            try:
                name = name_bytes.decode('ascii')
                entries.append({'id': entry_id, 'name': name})
            except:
                pass
        
        return entries
    except:
        return []

print("="*80)
print("DECODING HEX KEYS USING DISCWORLD PROTOTYPE DATA")
print("="*80)

# Step 1: Extract all names from Discworld files
print("\n[1/4] Extracting names from Discworld BNK files...")

all_names = set()

# Parse Level_Cori.bnk
with open('Discworld/audio/Level_Cori.bnk', 'rb') as f:
    data = f.read()

pos = data.find(b'STID')
if pos != -1:
    entries = parse_stid_xbox(data, pos)
    for e in entries:
        all_names.add(e['name'])
    print(f"  Level_Cori.bnk: {len(entries)} names")

# Parse VO_Cori.bnk
with open('Discworld/audio/English(US)/VO_Cori.bnk', 'rb') as f:
    data = f.read()

pos = data.find(b'STID')
if pos != -1:
    entries = parse_stid_xbox(data, pos)
    for e in entries:
        all_names.add(e['name'])
    print(f"  VO_Cori.bnk: {len(entries)} names")

# Add readable keys from CoriSoundID.audio.json
with open('Discworld/audio/CoriSoundID.audio.json', 'r') as f:
    cori_data = json.load(f)

for section in ['obj1s', 'obj2s', 'obj5s', 'obj6s', 'obj7s']:
    items = cori_data.get(section, [])
    if section == 'obj2s':
        # obj2s has nested structure
        for group in items:
            if isinstance(group, list) and len(group) > 1:
                for entry in group[1]:
                    if not entry['key'].startswith('0x'):
                        all_names.add(entry['key'])
    else:
        for entry in items:
            if not entry['key'].startswith('0x'):
                all_names.add(entry['key'])

print(f"\n  Total unique names: {len(all_names)}")

# Step 2: Load main WWiseIDTable
print("\n[2/4] Loading main WWiseIDTable.audio.json...")

with open('WWiseIDTable.audio.json', 'r') as f:
    main_data = json.load(f)

main_entries = main_data.get('obj1s', [])
print(f"  Loaded {len(main_entries)} entries")

# Create hex lookup
hex_lookup = {}
for entry in main_entries:
    if entry['key'].startswith('0x'):
        hex_val = int(entry['key'], 16)
        hex_lookup[hex_val] = entry

print(f"  {len(hex_lookup)} hex keys to decode")

# Step 3: Test all names against hex keys
print("\n[3/4] Testing names against hex keys...")

matches = []

for name in all_names:
    # Test original
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
            'name': name.lower(),
            'hash': h_lower,
            'hex_key': hex_lookup[h_lower]['key'],
            'val': hex_lookup[h_lower]['val'],
            'variation': 'lowercase'
        })

print(f"  Found {len(matches)} matches!")

# Step 4: Generate report
print("\n[4/4] Generating report...")

with open('DECODED_HEX_KEYS_FROM_DISCWORLD.txt', 'w', encoding='utf-8') as f:
    f.write("DECODED HEX KEYS FROM DISCWORLD PROTOTYPE\n")
    f.write("="*80 + "\n\n")
    f.write(f"Names extracted from Discworld: {len(all_names)}\n")
    f.write(f"Hex keys in main WWiseIDTable: {len(hex_lookup)}\n")
    f.write(f"Successfully decoded: {len(matches)}\n")
    f.write(f"Decode rate: {len(matches)/len(hex_lookup)*100:.2f}%\n\n")
    f.write("="*80 + "\n\n")
    
    if matches:
        f.write("DECODED HEX KEYS\n")
        f.write("-"*80 + "\n\n")
        for m in sorted(matches, key=lambda x: x['name']):
            f.write(f"Name: {m['name']} ({m['variation']})\n")
            f.write(f"  Hash: 0x{m['hash']:08X}\n")
            f.write(f"  Hex Key: {m['hex_key']}\n")
            f.write(f"  Val: {m['val']}\n\n")
    
    f.write("\n" + "="*80 + "\n\n")
    f.write("ALL NAMES FROM DISCWORLD\n")
    f.write("-"*80 + "\n\n")
    for name in sorted(all_names):
        f.write(f"{name}\n")

print("\n" + "="*80)
print("RESULTS")
print("="*80)
print(f"✓ Extracted {len(all_names)} names from Discworld")
print(f"✓ Decoded {len(matches)} hex keys ({len(matches)/len(hex_lookup)*100:.2f}%)")

if matches:
    print(f"\n🎉 SUCCESS! Decoded hex keys:")
    for m in matches[:20]:
        print(f"  {m['name']} → {m['hex_key']}")
    if len(matches) > 20:
        print(f"  ... and {len(matches)-20} more!")

print(f"\n✓ Report saved to DECODED_HEX_KEYS_FROM_DISCWORLD.txt")
