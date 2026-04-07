#!/usr/bin/env python3
"""Verify bank name to filename mapping using FNV-1 hash."""
import os

def fnv1_hash(name):
    """FNV-1 32-bit hash, lowercase input (Wwise convention)."""
    hval = 0x811c9dc5
    for c in name.lower():
        hval = (hval * 0x01000193) & 0xFFFFFFFF
        hval ^= ord(c)
    return hval

# Extracted .bnk filenames
extracted_dir = r'ZeroEnginePrototype\ZeroEngine\Engine\source\WwiseV28\ExtractedSounds\extracted'
bnk_files = set()
for f in os.listdir(extracted_dir):
    if f.endswith('.bnk'):
        bnk_files.add(f.replace('.bnk', ''))

print(f"Found {len(bnk_files)} extracted .bnk files\n")

# Test bank names
bank_names = [
    'Init', 'Ambience', 'BaseCombat', 'Effects', 'Creatures',
    'HeroGandalf', 'HeroAragorn', 'HeroSauron', 'Music', 'UI',
    'VoiceOver', 'HeroFrodo', 'HeroGimli', 'HeroLegolas',
    'SFXTroll', 'SFXWarg', 'Level_Moria', 'Level_HelmsDeep',
]

print("Bank Name            | FNV-1 Hash   | Decimal       | File Exists?")
print("-" * 80)
matched = 0
for name in bank_names:
    h = fnv1_hash(name)
    dec_str = str(h)
    exists = dec_str in bnk_files
    if exists:
        matched += 1
    flag = "YES" if exists else "NO"
    print(f"{name:20s} | 0x{h:08X}   | {h:>13d} | {flag}")

print(f"\nMatched: {matched}/{len(bank_names)}")

# Check hex IDs from BANK_ID_TO_NAME_MAPPING against FNV-1 hashes
print("\n\nVerifying BANK_ID_TO_NAME_MAPPING hex IDs match FNV-1 hashes...")
mapping_entries = [
    ("Ambience", 0x05174939),
    ("HeroGandalf", 0xB9842D06),
    ("HeroAragorn", 0xD210FF9B),
    ("HeroSauron", 0xAE7FDABB),
    ("Music", 0xEDF036D6),
    ("UI", 0x5C770DB7),
    ("Effects", 0x73CB32C9),
    ("BaseCombat", 0xD0DEF16A),  # Guessed - may not be correct
]

for name, mapping_id in mapping_entries:
    computed = fnv1_hash(name)
    match = "MATCH" if computed == mapping_id else "MISMATCH"
    print(f"{name:20s} | Mapping: 0x{mapping_id:08X} | Computed: 0x{computed:08X} | {match}")

# Show all extracted .bnk file IDs
print("\n\nAll extracted .bnk files (sorted by name as integer):")
sorted_ids = sorted(bnk_files, key=lambda x: int(x))
for fid in sorted_ids:
    n = int(fid)
    print(f"  {n:>12d} (0x{n:08X})")

