#!/usr/bin/env python3
"""
FINAL PCK Parser - Successfully reverse-engineered!
Extracts Wwise audio files with proper IDs, names, and language filtering
"""

import struct
import os
import sys
import json
from pathlib import Path
from collections import defaultdict
import re

class WwisePCKParserFinal:
    def __init__(self, pck_path, id_table_path, output_dir):
        self.pck_path = pck_path
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.build_id_mappings()
        
        # Constants discovered through reverse engineering
        self.BASE_OFFSET = 1116192  # 0x110820
        self.INDEX_START = 7378  # After language table
        self.ENTRY_SIZE = 24  # bytes per entry
        
    def load_id_table(self, path):
        """Load WWise ID table"""
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    
    def build_id_mappings(self):
        """Build ID to event name mappings"""
        # Process all object types
        for obj_type in ['obj1s', 'obj3s', 'obj5s', 'obj6s', 'obj7s', 'extra']:
            if obj_type not in self.id_table:
                continue
            
            for entry in self.id_table[obj_type]:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    if not key.startswith('0x'):
                        self.id_to_name[val] = key
        
        # Process obj2s (nested structure)
        if 'obj2s' in self.id_table:
            for group in self.id_table['obj2s']:
                if isinstance(group, list) and len(group) > 1:
                    entries = group[1] if isinstance(group[1], list) else []
                    for entry in entries:
                        if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                            key = entry['key']
                            val = entry['val']
                            if not key.startswith('0x'):
                                self.id_to_name[val] = key
        
        print(f'✓ Loaded {len(self.id_to_name)} event name mappings')
    
    def parse_index(self, f):
        """Parse the file index - FINAL CORRECTED STRUCTURE"""
        f.seek(self.INDEX_START)

        entries = []
        entry_num = 0

        # First entry is 28 bytes, rest are 24 bytes
        # All entries have 0x800 marker at different positions

        # Read until we hit the audio data section
        while f.tell() < self.BASE_OFFSET:
            if entry_num == 0:
                # First entry: 28 bytes
                # [ID (4)] [0x800 (4)] [Size (4)] [0 (4)] [? (4)] [Lang (4)] [? (4)]
                entry_data = f.read(28)
                if len(entry_data) < 28:
                    break

                wwise_id = struct.unpack('<I', entry_data[0:4])[0]
                marker = struct.unpack('<I', entry_data[4:8])[0]
                size = struct.unpack('<I', entry_data[8:12])[0]
                padding = struct.unpack('<I', entry_data[12:16])[0]
                unk = struct.unpack('<I', entry_data[16:20])[0]
                lang_id = struct.unpack('<I', entry_data[20:24])[0]

                if marker == 0x800:
                    entries.append({
                        'wwise_id': wwise_id,
                        'offset': 0x800,  # First file always at 0x800
                        'size': size,
                        'lang_id': lang_id
                    })
                    entry_num += 1
                else:
                    break
            else:
                # Subsequent entries: 24 bytes
                # [ID (4)] [0x800 (4)] [Offset (4)] [0 (4)] [Size (4)] [Lang (4)]
                entry_data = f.read(24)
                if len(entry_data) < 24:
                    break

                wwise_id = struct.unpack('<I', entry_data[0:4])[0]
                marker = struct.unpack('<I', entry_data[4:8])[0]
                offset = struct.unpack('<I', entry_data[8:12])[0]
                padding = struct.unpack('<I', entry_data[12:16])[0]
                size = struct.unpack('<I', entry_data[16:20])[0]
                lang_id = struct.unpack('<I', entry_data[20:24])[0]

                if marker == 0x800:
                    entries.append({
                        'wwise_id': wwise_id,
                        'offset': offset,
                        'size': size,
                        'lang_id': lang_id
                    })
                    entry_num += 1
                else:
                    # No more valid entries
                    break

        print(f'✓ Parsed {len(entries)} index entries')
        return entries
    
    def extract_files(self, entries, target_lang_ids=[4, 0]):
        """Extract files with language filtering"""
        print(f'\n{'='*70}')
        print('EXTRACTING FILES')
        print(f'{'='*70}\n')

        lang_names = {0: 'sfx', 2: 'french', 3: 'german', 4: 'english', 5: 'italian', 6: 'russian', 7: 'spanish'}

        print(f'Target languages: {[lang_names.get(lid, f"id_{lid}") for lid in target_lang_ids]}')
        print()

        stats = defaultdict(int)
        extracted = 0
        skipped = 0

        with open(self.pck_path, 'rb') as f:
            for idx, entry in enumerate(entries):
                wwise_id = entry['wwise_id']
                lang_id = entry['lang_id']
                offset = entry['offset']
                size = entry['size']

                # Skip if not target language
                if lang_id not in target_lang_ids:
                    skipped += 1
                    continue

                # Calculate actual offset
                actual_offset = offset + self.BASE_OFFSET

                # Read file data
                f.seek(actual_offset)

                # Verify RIFF header
                magic = f.read(4)
                if magic != b'RIFF':
                    print(f'  Warning: Entry {idx} (ID 0x{wwise_id:08X}) at offset {actual_offset} is not a RIFF file')
                    continue

                # Read RIFF size
                riff_size = struct.unpack('<I', f.read(4))[0]
                total_size = riff_size + 8

                # Read full file
                f.seek(actual_offset)
                file_data = f.read(total_size)

                # Get event name
                event_name = self.id_to_name.get(wwise_id, f'id_{wwise_id:08x}')

                # Categorize
                category = self.categorize_event(event_name)
                stats[category] += 1

                # Create output path
                lang_name = lang_names.get(lang_id, f'lang_{lang_id}')
                category_dir = self.output_dir / lang_name / category
                category_dir.mkdir(parents=True, exist_ok=True)

                # Sanitize filename
                safe_name = self.sanitize_filename(event_name)
                output_path = category_dir / f'{safe_name}.wem'

                # Handle duplicates
                counter = 1
                while output_path.exists():
                    output_path = category_dir / f'{safe_name}_{counter}.wem'
                    counter += 1

                # Write file
                with open(output_path, 'wb') as out_f:
                    out_f.write(file_data)

                extracted += 1

                if extracted % 100 == 0:
                    print(f'  Extracted {extracted} files...')

        print(f'\n✓ Extracted {extracted} files (skipped {skipped} from other languages)')
        print(f'\n📊 Statistics by category:')
        for cat, count in sorted(stats.items(), key=lambda x: x[1], reverse=True):
            print(f'  {cat:40s} {count:5d} files')
    
    def categorize_event(self, name):
        """Categorize event by name"""
        n = name.lower()
        
        if any(x in n for x in ['music', 'theme', 'bgm']):
            return 'music'
        if any(x in n for x in ['vo', 'voice', 'dialogue', 'vocal']):
            if 'kill' in n:
                return 'voice/combat/kills'
            elif 'taunt' in n or 'cheer' in n:
                return 'voice/combat/taunts'
            elif 'death' in n or 'pain' in n:
                return 'voice/combat/pain'
            return 'voice/dialogue'
        if any(x in n for x in ['sword', 'blade', 'swing', 'weapon']):
            return 'sfx/weapons'
        if any(x in n for x in ['impact', 'hit', 'block']):
            return 'sfx/combat'
        if any(x in n for x in ['ability', 'magic', 'spell']):
            return 'sfx/abilities'
        if any(x in n for x in ['ui', 'menu', 'button']):
            return 'sfx/ui'
        if any(x in n for x in ['ambient', 'environment']):
            return 'ambient'
        
        return 'uncategorized'
    
    def sanitize_filename(self, name):
        """Sanitize filename"""
        name = re.sub(r'[<>:"/\\|?*]', '_', name)
        name = re.sub(r'_+', '_', name)
        return name.strip('_')
    
    def extract(self):
        """Main extraction method"""
        print(f'\n{'='*70}')
        print('WWISE PCK PARSER - REVERSE ENGINEERED')
        print(f'{'='*70}\n')
        
        with open(self.pck_path, 'rb') as f:
            # Parse index
            entries = self.parse_index(f)
            
            # Extract files (English + SFX only)
            self.extract_files(entries, target_lang_ids=[4, 0])
        
        print(f'\n✅ Extraction complete!')
        print(f'📂 Output: {self.output_dir.absolute()}')

def main():
    if len(sys.argv) < 2:
        print('Usage: python pck_parser_final.py <pck_file> [output_dir]')
        sys.exit(1)
    
    pck_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_final'
    id_table = 'WWiseIDTable.audio.json'
    
    parser = WwisePCKParserFinal(pck_file, id_table, output_dir)
    parser.extract()

if __name__ == '__main__':
    main()

