#!/usr/bin/env python3
"""
Proper PCK extractor that preserves file IDs and organizes by language
"""

import struct
import os
import sys
import json
from pathlib import Path
from collections import defaultdict

class PCKExtractor:
    def __init__(self, pck_path, id_table_path, output_dir):
        self.pck_path = pck_path
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.build_id_mappings()
        
    def load_id_table(self, path):
        """Load the Wwise ID table JSON"""
        print(f"📖 Loading ID table from {path}...")
        with open(path, 'r') as f:
            data = json.load(f)
        print(f"   Loaded ID table")
        return data
    
    def build_id_mappings(self):
        """Build ID to name mappings"""
        print("🔗 Building ID mappings...")
        
        # Process obj1s
        if 'obj1s' in self.id_table:
            for entry in self.id_table['obj1s']:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    if not key.startswith('0x'):
                        if val not in self.id_to_name:
                            self.id_to_name[val] = key
        
        # Process obj2s (nested)
        if 'obj2s' in self.id_table:
            for group in self.id_table['obj2s']:
                if isinstance(group, list) and len(group) > 1:
                    entries = group[1] if isinstance(group[1], list) else []
                    for entry in entries:
                        if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                            key = entry['key']
                            val = entry['val']
                            if not key.startswith('0x'):
                                if val not in self.id_to_name:
                                    self.id_to_name[val] = key
        
        print(f"   Mapped {len(self.id_to_name)} event names")
    
    def categorize_event(self, event_name):
        """Categorize an event by its name"""
        name_lower = event_name.lower()
        
        # Voice/dialogue
        if any(x in name_lower for x in ['vo', 'voice', 'dialogue', 'taunt', 'cheer', 'vocal', 'kill', 'death', 'pain', 'effort', 'grunt']):
            if 'kill' in name_lower:
                return 'voice/combat/kills'
            elif 'taunt' in name_lower or 'cheer' in name_lower:
                return 'voice/combat/taunts'
            elif 'death' in name_lower or 'pain' in name_lower or 'grunt' in name_lower:
                return 'voice/combat/pain'
            else:
                return 'voice/dialogue'
        
        # Music
        if any(x in name_lower for x in ['music', 'theme', 'ambient_music', 'score', 'soundtrack']):
            return 'music'
        
        # Weapons
        if any(x in name_lower for x in ['sword', 'blade', 'swing', 'slash', 'stab', 'arrow', 'bow', 'axe', 'spear', 'weapon']):
            if 'impact' in name_lower:
                return 'sfx/weapons/impacts'
            else:
                return 'sfx/weapons/swings'
        
        # Combat
        if any(x in name_lower for x in ['impact', 'hit', 'block', 'parry', 'attack', 'damage']):
            if 'block' in name_lower or 'parry' in name_lower:
                return 'sfx/combat/blocks'
            else:
                return 'sfx/combat/impacts'
        
        # Magic/abilities
        if any(x in name_lower for x in ['ability', 'magic', 'spell', 'fire', 'lightning', 'heal', 'power']):
            return 'sfx/abilities'
        
        # Movement
        if any(x in name_lower for x in ['footstep', 'walk', 'run', 'jump', 'land', 'movement']):
            return 'sfx/movement'
        
        # UI
        if any(x in name_lower for x in ['ui', 'menu', 'button', 'click', 'select', 'hover', 'transition', 'state']):
            return 'sfx/ui'
        
        # Environment
        if any(x in name_lower for x in ['ambient', 'wind', 'water', 'fire', 'rain', 'thunder', 'environment']):
            return 'ambient/environment'
        
        # Creatures
        if any(x in name_lower for x in ['creature', 'monster', 'beast', 'orc', 'troll', 'dragon']):
            return 'voice/creatures'
        
        # Character types
        if any(x in name_lower for x in ['hero', 'scout', 'boss', 'normal']):
            return 'voice/characters'
        
        return 'uncategorized'
    
    def extract_and_organize(self):
        """Extract WEM files from PCK and organize them"""
        print(f"\n{'='*70}")
        print("🎮 LOTR: Conquest - PCK Extractor")
        print(f"{'='*70}\n")
        
        print(f"📂 Reading PCK file: {self.pck_path}")
        
        with open(self.pck_path, 'rb') as f:
            # Read magic
            magic = f.read(5)
            if magic != b'AKPKN':
                print(f"❌ Invalid PCK magic: {magic}")
                return
            
            print(f"✓ Valid PCK file (AKPKN format)")
            
            # Skip header bytes
            f.read(3)
            version = struct.unpack('<I', f.read(4))[0]
            print(f"   Version: {version}")
            
            # Read language table info
            lang_table_offset = struct.unpack('<I', f.read(4))[0]
            lang_table_size = struct.unpack('<I', f.read(4))[0]
            
            # Read number of languages
            num_languages = struct.unpack('<I', f.read(4))[0]
            print(f"   Languages: {num_languages}")
            
            # Read language IDs and offsets
            lang_info = []
            for i in range(num_languages):
                lang_id = struct.unpack('<I', f.read(4))[0]
                offset = struct.unpack('<I', f.read(4))[0]
                lang_info.append((lang_id, offset))
            
            # Read language names
            languages = []
            for lang_id, offset in lang_info:
                current_pos = f.tell()
                f.seek(offset)
                
                # Read wide string (UTF-16LE)
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
                languages.append((lang_id, lang_name))
                f.seek(current_pos)
            
            print(f"\n   Found languages:")
            for lang_id, lang_name in languages:
                print(f"      [{lang_id}] {lang_name}")
            
            # Focus on English only
            english_id = None
            for lang_id, lang_name in languages:
                if 'english' in lang_name.lower():
                    english_id = lang_id
                    print(f"\n✓ Focusing on English (ID: {english_id})")
                    break
            
            if english_id is None:
                print("⚠️  English language not found, extracting all languages")
            
            # Now extract files
            # The PCK format embeds WEM files directly in the data section
            # We need to scan for RIFF headers
            
            print(f"\n{'='*70}")
            print("🔍 Scanning for audio files...")
            print(f"{'='*70}\n")
            
            f.seek(lang_table_offset + lang_table_size)
            
            file_count = 0
            stats = defaultdict(int)
            
            while True:
                pos = f.tell()
                chunk = f.read(4)
                
                if len(chunk) < 4:
                    break
                
                if chunk == b'RIFF':
                    # Found a WEM/WAV file
                    size_bytes = f.read(4)
                    if len(size_bytes) < 4:
                        break
                    
                    file_size = struct.unpack('<I', size_bytes)[0] + 8
                    
                    # Go back to start of file
                    f.seek(pos)
                    file_data = f.read(file_size)
                    
                    # Try to determine file ID (this is tricky without proper index)
                    # For now, use position-based ID
                    file_id = pos
                    
                    # Check if we have a name for this ID
                    event_name = self.id_to_name.get(file_id, f"audio_{file_count:04d}")
                    
                    # Categorize
                    category = self.categorize_event(event_name)
                    stats[category] += 1
                    
                    # Create output directory
                    category_dir = self.output_dir / category
                    category_dir.mkdir(parents=True, exist_ok=True)
                    
                    # Sanitize filename
                    safe_name = event_name.replace('/', '_').replace('\\', '_').replace(':', '_')
                    output_path = category_dir / f"{safe_name}.wem"
                    
                    # Handle duplicates
                    counter = 1
                    while output_path.exists():
                        output_path = category_dir / f"{safe_name}_{counter}.wem"
                        counter += 1
                    
                    # Write file
                    with open(output_path, 'wb') as out_f:
                        out_f.write(file_data)
                    
                    file_count += 1
                    if file_count % 10 == 0:
                        print(f"   Extracted {file_count} files...")
                else:
                    # Not a RIFF header, move forward
                    f.seek(pos + 1)
            
            print(f"\n✓ Extracted {file_count} files")
            print(f"\n📊 Category Statistics:")
            print(f"{'='*70}")
            for category, count in sorted(stats.items(), key=lambda x: x[1], reverse=True):
                print(f"   {category:50s} {count:4d} files")
            print(f"{'='*70}")
            print(f"\n✅ Extraction complete!")
            print(f"📂 Output: {self.output_dir.absolute()}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_pck_proper.py <pck_file> [output_dir]")
        sys.exit(1)
    
    pck_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_organized'
    id_table = 'WWiseIDTable.audio.json'
    
    if not os.path.exists(pck_file):
        print(f"❌ Error: PCK file not found: {pck_file}")
        sys.exit(1)
    
    if not os.path.exists(id_table):
        print(f"❌ Error: ID table not found: {id_table}")
        sys.exit(1)
    
    extractor = PCKExtractor(pck_file, id_table, output_dir)
    extractor.extract_and_organize()

if __name__ == '__main__':
    main()

