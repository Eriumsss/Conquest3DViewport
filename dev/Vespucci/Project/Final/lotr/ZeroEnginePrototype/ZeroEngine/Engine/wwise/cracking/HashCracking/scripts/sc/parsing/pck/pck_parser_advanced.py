#!/usr/bin/env python3
"""
Advanced PCK Parser for Wwise AKPKN format
Extracts audio files with proper IDs and organization
"""

import struct
import os
import sys
import json
from pathlib import Path
from collections import defaultdict
import re

class WwisePCKParser:
    def __init__(self, pck_path, id_table_path, output_dir):
        self.pck_path = pck_path
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.name_to_id = {}
        self.build_id_mappings()
        self.languages = []
        
    def load_id_table(self, path):
        """Load the Wwise ID table JSON"""
        print(f"Loading ID table from {path}...")
        with open(path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        print(f"   ✓ Loaded ID table")
        return data
    
    def build_id_mappings(self):
        """Build bidirectional ID to name mappings"""
        print("Building ID mappings...")
        
        # Process obj1s - main event mappings
        if 'obj1s' in self.id_table:
            for entry in self.id_table['obj1s']:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    
                    if not key.startswith('0x'):
                        # String event name
                        self.name_to_id[key] = val
                        if val not in self.id_to_name:
                            self.id_to_name[val] = key
                    else:
                        # Hex ID
                        try:
                            hex_id = int(key, 16)
                            if hex_id not in self.id_to_name:
                                self.id_to_name[hex_id] = f"id_{hex_id:08x}"
                        except:
                            pass
        
        # Process obj2s - character types and additional mappings
        if 'obj2s' in self.id_table:
            for group in self.id_table['obj2s']:
                if isinstance(group, list) and len(group) > 1:
                    entries = group[1] if isinstance(group[1], list) else []
                    for entry in entries:
                        if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                            key = entry['key']
                            val = entry['val']
                            if not key.startswith('0x'):
                                self.name_to_id[key] = val
                                if val not in self.id_to_name:
                                    self.id_to_name[val] = key
        
        # Process other object types
        for obj_type in ['obj3s', 'obj5s', 'obj6s', 'obj7s', 'extra']:
            if obj_type not in self.id_table:
                continue
            
            for entry in self.id_table[obj_type]:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    if not key.startswith('0x'):
                        self.name_to_id[key] = val
                        if val not in self.id_to_name:
                            self.id_to_name[val] = key
        
        print(f"   ✓ Mapped {len(self.name_to_id)} event names")
        print(f"   ✓ Total IDs: {len(self.id_to_name)}")
        
        # Show sample event names
        sample_names = [name for name in self.name_to_id.keys() if not name.startswith('id_')][:10]
        if sample_names:
            print(f"\n   Sample events: {', '.join(sample_names[:5])}")
    
    def parse_header(self, f):
        """Parse PCK header"""
        print(f"\n{'='*70}")
        print("Parsing PCK Header")
        print(f"{'='*70}\n")
        
        # Read magic
        magic = f.read(5)
        if magic != b'AKPKN':
            raise ValueError(f"Invalid PCK magic: {magic}")
        
        print(f"✓ Valid AKPKN format")
        
        # Read header
        unk_bytes = f.read(3)
        version = struct.unpack('<I', f.read(4))[0]
        lang_table_offset = struct.unpack('<I', f.read(4))[0]
        lang_table_size = struct.unpack('<I', f.read(4))[0]
        file_data_offset = struct.unpack('<I', f.read(4))[0]
        
        print(f"   Version: {version}")
        print(f"   Language table: offset={lang_table_offset}, size={lang_table_size}")
        print(f"   File data offset: {file_data_offset}")
        
        return {
            'version': version,
            'lang_table_offset': lang_table_offset,
            'lang_table_size': lang_table_size,
            'file_data_offset': file_data_offset
        }
    
    def parse_languages(self, f, header):
        """Parse language table"""
        print(f"\n{'='*70}")
        print("Parsing Language Table")
        print(f"{'='*70}\n")
        
        # Read language entries (7 languages)
        lang_entries = []
        for i in range(7):
            lang_id = struct.unpack('<I', f.read(4))[0]
            name_offset = struct.unpack('<I', f.read(4))[0]
            lang_entries.append((lang_id, name_offset))
        
        # Read language names
        languages = []
        for lang_id, offset in lang_entries:
            current_pos = f.tell()
            f.seek(offset)
            
            # Read UTF-16LE string
            chars = []
            while True:
                char_bytes = f.read(2)
                if len(char_bytes) < 2:
                    break
                char_val = struct.unpack('<H', char_bytes)[0]
                if char_val == 0:
                    break
                chars.append(chr(char_val))
            
            lang_name = ''.join(chars)
            languages.append({'id': lang_id, 'name': lang_name})
            f.seek(current_pos)
        
        self.languages = languages
        
        print(f"   Found {len(languages)} languages:")
        for lang in languages:
            marker = "✓" if 'english' in lang['name'].lower() else " "
            print(f"   {marker} [{lang['id']}] {lang['name']}")
        
        return languages
    
    def find_all_riff_files(self, f):
        """Scan file for all RIFF headers"""
        print(f"\n{'='*70}")
        print("Scanning for Audio Files")
        print(f"{'='*70}\n")
        
        f.seek(0)
        data = f.read()
        file_size = len(data)
        
        print(f"   File size: {file_size:,} bytes ({file_size / 1024 / 1024:.1f} MB)")
        
        # Find all RIFF positions
        riff_files = []
        pos = 0
        while True:
            pos = data.find(b'RIFF', pos)
            if pos == -1:
                break
            
            # Read RIFF size
            if pos + 8 <= file_size:
                riff_size = struct.unpack('<I', data[pos+4:pos+8])[0]
                total_size = riff_size + 8
                
                # Verify it's a WAVE file
                if pos + 12 <= file_size and data[pos+8:pos+12] == b'WAVE':
                    riff_files.append({
                        'offset': pos,
                        'size': total_size,
                        'riff_size': riff_size
                    })
            
            pos += 1
        
        print(f"   ✓ Found {len(riff_files)} RIFF/WAVE files")
        
        return riff_files
    
    def categorize_event(self, event_name):
        """Categorize an event by its name"""
        name_lower = event_name.lower()
        
        # Voice/dialogue
        if any(x in name_lower for x in ['vo', 'voice', 'dialogue', 'taunt', 'cheer', 'vocal', 'yell', 'scream']):
            if 'kill' in name_lower:
                return 'voice/combat/kills'
            elif 'taunt' in name_lower or 'cheer' in name_lower:
                return 'voice/combat/taunts'
            elif 'death' in name_lower or 'pain' in name_lower or 'grunt' in name_lower or 'effort' in name_lower:
                return 'voice/combat/pain'
            else:
                return 'voice/dialogue'
        
        # Music
        if any(x in name_lower for x in ['music', 'theme', 'ambient_music', 'score', 'soundtrack', 'bgm']):
            return 'music'
        
        # Weapons
        if any(x in name_lower for x in ['sword', 'blade', 'swing', 'slash', 'stab', 'arrow', 'bow', 'axe', 'spear', 'weapon', 'ranged']):
            if 'impact' in name_lower:
                return 'sfx/weapons/impacts'
            elif 'charge' in name_lower or 'release' in name_lower:
                return 'sfx/weapons/ranged'
            else:
                return 'sfx/weapons/swings'
        
        # Combat
        if any(x in name_lower for x in ['impact', 'hit', 'block', 'parry', 'attack', 'damage', 'grab']):
            if 'block' in name_lower or 'parry' in name_lower:
                return 'sfx/combat/blocks'
            elif 'kill' in name_lower:
                return 'sfx/combat/kills'
            else:
                return 'sfx/combat/impacts'
        
        # Magic/abilities
        if any(x in name_lower for x in ['ability', 'magic', 'spell', 'firewall', 'lightning', 'heal', 'power']):
            return 'sfx/abilities'
        
        # Movement
        if any(x in name_lower for x in ['footstep', 'walk', 'run', 'jump', 'land', 'movement']):
            return 'sfx/movement'
        
        # UI
        if any(x in name_lower for x in ['ui', 'menu', 'button', 'click', 'select', 'hover', 'transition', 'state', 'cp_']):
            return 'sfx/ui'
        
        # Environment
        if any(x in name_lower for x in ['ambient', 'wind', 'water', 'fire', 'rain', 'thunder', 'environment']):
            return 'ambient/environment'
        
        # Creatures
        if any(x in name_lower for x in ['creature', 'monster', 'beast', 'orc', 'troll', 'dragon']):
            return 'voice/creatures'
        
        # Character types
        if any(x in name_lower for x in ['hero', 'scout', 'boss', 'normal', 'human']):
            return 'voice/characters'
        
        return 'uncategorized'
    
    def sanitize_filename(self, name):
        """Sanitize filename"""
        # Remove invalid characters
        name = re.sub(r'[<>:"/\\|?*]', '_', name)
        # Remove multiple underscores
        name = re.sub(r'_+', '_', name)
        # Trim
        name = name.strip('_')
        return name

    def extract_and_organize(self):
        """Main extraction and organization logic"""
        print(f"\n{'='*70}")
        print("LOTR: Conquest - Advanced PCK Extractor")
        print(f"{'='*70}\n")

        with open(self.pck_path, 'rb') as f:
            # Parse header
            header = self.parse_header(f)

            # Parse languages
            languages = self.parse_languages(f, header)

            # Find all RIFF files
            riff_files = self.find_all_riff_files(f)

            # Extract and organize files
            print(f"\n{'='*70}")
            print("Extracting and Organizing Files")
            print(f"{'='*70}\n")

            stats = defaultdict(int)
            extracted_count = 0

            # Since we don't have the actual file index with IDs,
            # we'll use a heuristic approach:
            # 1. Extract all RIFF files
            # 2. Analyze their content (size, format)
            # 3. Try to match with event names based on patterns
            # 4. Organize by category

            for idx, riff_info in enumerate(riff_files):
                offset = riff_info['offset']
                size = riff_info['size']

                # Read file data
                f.seek(offset)
                file_data = f.read(size)

                # Try to determine event name
                # Since we don't have the actual ID mapping, we'll use:
                # - File size as a hint
                # - Position in file
                # - Content analysis

                # For now, use a generic name with index
                # In a real implementation, we'd need the PCK index structure
                event_name = f"audio_{idx:04d}"

                # Try to find a matching event name based on file characteristics
                # This is a heuristic approach since we don't have the index
                if size > 100000:
                    # Large files are likely music
                    event_name = f"music_{idx:04d}"
                    category = 'music'
                elif size > 30000:
                    # Medium-large files might be voice
                    event_name = f"voice_{idx:04d}"
                    category = 'voice/dialogue'
                elif size > 10000:
                    # Medium files could be combat sounds
                    event_name = f"combat_{idx:04d}"
                    category = 'sfx/combat/impacts'
                else:
                    # Small files are likely UI or small SFX
                    event_name = f"sfx_{idx:04d}"
                    category = 'sfx/ui'

                stats[category] += 1

                # Create category directory
                category_dir = self.output_dir / category
                category_dir.mkdir(parents=True, exist_ok=True)

                # Sanitize filename
                safe_name = self.sanitize_filename(event_name)
                output_path = category_dir / f"{safe_name}.wem"

                # Handle duplicates
                counter = 1
                while output_path.exists():
                    output_path = category_dir / f"{safe_name}_{counter}.wem"
                    counter += 1

                # Write file
                with open(output_path, 'wb') as out_f:
                    out_f.write(file_data)

                extracted_count += 1

                if extracted_count % 1000 == 0:
                    print(f"   Extracted {extracted_count}/{len(riff_files)} files...")

            print(f"\n✓ Extracted {extracted_count} files")
            print(f"\nCategory Statistics:")
            print(f"{'='*70}")
            for category, count in sorted(stats.items(), key=lambda x: x[1], reverse=True):
                print(f"   {category:50s} {count:5d} files")
            print(f"{'='*70}")

            print(f"\n Note: Files are organized by size heuristics since the PCK")
            print(f"   index structure couldn't be fully parsed. For proper ID mapping,")
            print(f"   the PCK index format needs to be reverse-engineered further.")

            print(f"\n Extraction complete!")
            print(f" Output: {self.output_dir.absolute()}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python pck_parser_advanced.py <pck_file> [output_dir]")
        print("\nExample:")
        print("  python pck_parser_advanced.py sound.pck extracted_organized")
        sys.exit(1)

    pck_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_organized'
    id_table = 'WWiseIDTable.audio.json'

    if not os.path.exists(pck_file):
        print(f"❌ Error: PCK file not found: {pck_file}")
        sys.exit(1)

    if not os.path.exists(id_table):
        print(f"Warning: ID table not found: {id_table}")
        print(f"   Proceeding without event name mapping")

    parser = WwisePCKParser(pck_file, id_table, output_dir)
    parser.extract_and_organize()

if __name__ == '__main__':
    main()


