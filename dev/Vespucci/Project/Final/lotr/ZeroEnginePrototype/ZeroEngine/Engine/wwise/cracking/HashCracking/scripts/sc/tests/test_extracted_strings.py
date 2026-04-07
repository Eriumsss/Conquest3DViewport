#!/usr/bin/env python3
"""
Test extracted strings against hex keys
"""
import json
import sys
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

print("="*80)
print("TESTING EXTRACTED STRINGS AGAINST HEX KEYS")
print("="*80)

# Load strings
print("\n[1/3] Loading extracted strings...")
with open('sound_pck_strings.txt', 'r', encoding='utf-8') as f:
    lines = f.readlines()

# Skip header
strings = []
for line in lines:
    line = line.strip()
    if line and not line.startswith('=') and not line.startswith('STRINGS') and not line.startswith('Total'):
        strings.append(line)

print(f"  Loaded {len(strings)} strings")

# Filter for likely event names
filtered_strings = []
for s in strings:
    # Must be at least 3 chars
    if len(s) < 3:
        continue
    # Must start with letter or underscore
    if not (s[0].isalpha() or s[0] == '_'):
        continue
    # Must contain mostly alphanumeric + underscore
    if sum(c.isalnum() or c == '_' for c in s) / len(s) < 0.8:
        continue
    filtered_strings.append(s)

print(f"  Filtered to {len(filtered_strings)} likely event names")

# Load main WWiseIDTable
print("\n[2/3] Loading WWiseIDTable...")
with open('WWiseIDTable.audio.json', 'r') as f:
    main_data = json.load(f)

main_entries = main_data.get('obj1s', [])

# Create hex lookup
hex_lookup = {}
for entry in main_entries:
    if entry['key'].startswith('0x'):
        hex_val = int(entry['key'], 16)
        hex_lookup[hex_val] = entry

print(f"  {len(hex_lookup)} hex keys to decode")

# Test strings
print("\n[3/3] Testing strings...")
matches = []

for idx, name in enumerate(filtered_strings):
    if idx % 1000 == 0:
        print(f"  Testing {idx}/{len(filtered_strings)}...")
    
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

print(f"\n  Found {len(matches)} matches!")

# Save results
with open('DECODED_HEX_KEYS_FROM_SOUND_PCK.txt', 'w', encoding='utf-8') as f:
    f.write("DECODED HEX KEYS FROM sound.pck\n")
    f.write("="*80 + "\n\n")
    f.write(f"Strings extracted: {len(strings)}\n")
    f.write(f"Filtered strings: {len(filtered_strings)}\n")
    f.write(f"Hex keys in WWiseIDTable: {len(hex_lookup)}\n")
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

print("\n" + "="*80)
print("RESULTS")
print("="*80)
print(f"✓ Tested {len(filtered_strings)} filtered strings")
print(f"✓ Decoded {len(matches)} hex keys ({len(matches)/len(hex_lookup)*100:.2f}%)")

if matches:
    print(f"\n🎉 SUCCESS! Decoded hex keys:")
    for m in matches[:30]:
        print(f"  {m['name']} → {m['hex_key']}")
    if len(matches) > 30:
        print(f"  ... and {len(matches)-30} more!")

print(f"\n✓ Report saved to DECODED_HEX_KEYS_FROM_SOUND_PCK.txt")
