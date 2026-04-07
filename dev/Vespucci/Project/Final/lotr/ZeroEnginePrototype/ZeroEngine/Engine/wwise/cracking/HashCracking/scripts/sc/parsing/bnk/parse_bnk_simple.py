#!/usr/bin/env python3
"""
Simple BNK parser to extract STID names
"""
import struct

def parse_stid_section(data, offset, big_endian=False):
    """Parse STID section (Xbox uses big-endian, PC uses little-endian)"""
    try:
        sig = data[offset:offset+4]
        if sig != b'STID':
            return []

        endian = '>' if big_endian else '<'

        section_size = struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]
        num_entries = struct.unpack(f'{endian}I', data[offset+12:offset+16])[0]

        entries = []
        entry_offset = offset + 16

        for i in range(num_entries):
            if entry_offset >= offset + 8 + section_size:
                break

            entry_id = struct.unpack(f'{endian}I', data[entry_offset:entry_offset+4])[0]
            entry_offset += 4

            # Read null-terminated string
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
    except Exception as e:
        print(f"    Error: {e}")
        return []

def parse_bnk(filename, big_endian=False):
    """Parse BNK file"""
    print(f"\nParsing {filename}...")

    with open(filename, 'rb') as f:
        data = f.read()

    print(f"  Size: {len(data)} bytes")
    print(f"  Endianness: {'Big-endian (Xbox)' if big_endian else 'Little-endian (PC)'}")

    # Find STID sections
    all_names = []
    offset = 0

    while True:
        pos = data.find(b'STID', offset)
        if pos == -1:
            break

        entries = parse_stid_section(data, pos, big_endian)
        all_names.extend(entries)
        offset = pos + 4

    print(f"  Found {len(all_names)} STID entries")

    return all_names

# Parse Discworld BNK files (Xbox = big-endian)
print("="*80)
print("PARSING DISCWORLD BNK FILES (XBOX FORMAT)")
print("="*80)

vo_names = parse_bnk('Discworld/audio/English(US)/VO_Cori.bnk', big_endian=True)
level_names = parse_bnk('Discworld/audio/Level_Cori.bnk', big_endian=True)

all_names = vo_names + level_names

print(f"\nTotal names: {len(all_names)}")
print("\nAll names found:")
for e in all_names:
    print(f"  ID {e['id']:10d} (0x{e['id']:08X}): {e['name']}")

# Save to file
with open('discworld_bnk_names.txt', 'w') as f:
    f.write("DISCWORLD BNK NAMES\n")
    f.write("="*80 + "\n\n")
    for e in all_names:
        f.write(f"ID {e['id']:10d} (0x{e['id']:08X}): {e['name']}\n")

print(f"\n✓ Saved to discworld_bnk_names.txt")

