#!/usr/bin/env python3
"""
Working PCK Parser - Brute force approach
Finds RIFF files by scanning from offset positions
"""

import struct
import os
import json
from pathlib import Path
from collections import defaultdict

class WwisePCKParserWorking:
    def __init__(self, pck_path, id_table_path, output_dir):
        self.pck_path = pck_path
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.build_id_mappings()
        
        # Constants
        self.INDEX_START = 7378
        self.ENTRY_SIZE = 24
        
        # Pre-scan for all RIFF positions
        print("Pre-scanning for RIFF file positions...")
        self.riff_positions = self.scan_riff_positions()
        print(f"✓ Found {len(self.riff_positions)} RIFF files")
    
    def load_id_table(self, path):
        """Load WWise ID table"""
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    
    def build_id_mappings(self):
        """Build ID to event name mappings"""
        for obj_type in ['obj1s', 'obj3s', 'obj5s', 'obj6s', 'obj7s', 'extra']:
            if obj_type not in self.id_table:
                continue
            for entry in self.id_table[obj_type]:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    if not key.startswith('0x'):
                        self.id_to_name[val] = key
        
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
    
    def scan_riff_positions(self):
        """Scan entire PCK file for RIFF headers"""
        riff_positions = set()
        
        with open(self.pck_path, 'rb') as f:
            for chunk_start in range(0, 2090371072, 100000000):
                f.seek(chunk_start)
                chunk_size = min(100000000, 2090371072 - chunk_start)
                data = f.read(chunk_size)
                
                offset = 0
                while True:
                    idx = data.find(b'RIFF', offset)
                    if idx == -1:
                        break
                    riff_positions.add(chunk_start + idx)
                    offset = idx + 4
        
        return sorted(riff_positions)
    
    def parse_index(self):
        """Parse the file index"""
        entries = []
        
        with open(self.pck_path, 'rb') as f:
            f.seek(self.INDEX_START)
            
            # Entry 0 (28 bytes)
            entry0 = f.read(28)
            wwise_id = struct.unpack('<I', entry0[0:4])[0]
            offset = struct.unpack('<I', entry0[8:12])[0]
            size = struct.unpack('<I', entry0[16:20])[0]
            lang_id = struct.unpack('<I', entry0[20:24])[0]
            
            entries.append({
                'wwise_id': wwise_id,
                'offset': offset,
                'size': size,
                'lang_id': lang_id,
                'entry_num': 0
            })
            
            # Entries 1-26799 (24 bytes each)
            for i in range(1, 26800):
                entry = f.read(24)
                if len(entry) < 24:
                    break
                
                wwise_id = struct.unpack('<I', entry[0:4])[0]
                offset = struct.unpack('<I', entry[8:12])[0]
                size = struct.unpack('<I', entry[16:20])[0]
                lang_id = struct.unpack('<I', entry[20:24])[0]
                
                entries.append({
                    'wwise_id': wwise_id,
                    'offset': offset,
                    'size': size,
                    'lang_id': lang_id,
                    'entry_num': i
                })
        
        print(f'✓ Parsed {len(entries)} index entries')
        return entries
    
    def find_riff_for_entry(self, offset):
        """Find the RIFF file for an entry by scanning from offset"""
        # Find the nearest RIFF that's >= offset
        for riff_pos in self.riff_positions:
            if riff_pos >= offset:
                return riff_pos
        return None
    
    def extract_files(self, entries, target_lang_ids=[4, 0]):
        """Extract files with language filtering"""
        print(f'\n{"="*70}')
        print('EXTRACTING FILES')
        print(f'{"="*70}\n')
        
        lang_names = {0: 'sfx', 2: 'french', 3: 'german', 4: 'english', 5: 'italian', 6: 'russian', 7: 'spanish'}
        
        print(f'Target languages: {[lang_names.get(lid, f"id_{lid}") for lid in target_lang_ids]}')
        print()
        
        extracted = 0
        skipped = 0
        
        with open(self.pck_path, 'rb') as f:
            for idx, entry in enumerate(entries):
                if entry['lang_id'] not in target_lang_ids:
                    skipped += 1
                    continue
                
                # Find RIFF for this entry
                riff_pos = self.find_riff_for_entry(entry['offset'])
                
                if riff_pos is None:
                    print(f'  Warning: Entry {idx} (ID 0x{entry["wwise_id"]:08X}) - no RIFF found')
                    continue
                
                # Read RIFF file
                f.seek(riff_pos)
                magic = f.read(4)
                
                if magic != b'RIFF':
                    print(f'  Warning: Entry {idx} - RIFF magic mismatch')
                    continue
                
                riff_size = struct.unpack('<I', f.read(4))[0]
                total_size = riff_size + 8
                
                f.seek(riff_pos)
                file_data = f.read(total_size)
                
                # Get event name
                event_name = self.id_to_name.get(entry['wwise_id'], f'id_{entry["wwise_id"]:08x}')
                
                # Create output path
                lang_name = lang_names.get(entry['lang_id'], f'lang_{entry["lang_id"]}')
                output_dir = self.output_dir / lang_name
                output_dir.mkdir(parents=True, exist_ok=True)
                
                output_path = output_dir / f'{event_name}.wem'
                
                # Handle duplicates
                counter = 1
                while output_path.exists():
                    output_path = output_dir / f'{event_name}_{counter}.wem'
                    counter += 1
                
                # Write file
                with open(output_path, 'wb') as out_f:
                    out_f.write(file_data)
                
                extracted += 1
                
                if (extracted + skipped) % 1000 == 0:
                    print(f'  Processed {extracted + skipped} entries...')
        
        print(f'\n✓ Extracted {extracted} files')
        print(f'✓ Skipped {skipped} files (wrong language)')

if __name__ == '__main__':
    parser = WwisePCKParserWorking('sound.pck', 'WWiseIDTable.audio.json', 'extracted_english_working')
    entries = parser.parse_index()
    parser.extract_files(entries, target_lang_ids=[4, 0])  # English + SFX

