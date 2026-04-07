#!/usr/bin/env python3
"""
Extract and organize Wwise audio from LOTR: Conquest
- Extracts English audio only
- Maps IDs to event names using WWiseIDTable.audio.json
- Organizes into folders by type (music, sfx, voice, etc.)
"""

import struct
import os
import sys
import json
import subprocess
from pathlib import Path
from collections import defaultdict

class WwiseExtractor:
    def __init__(self, pck_path, id_table_path, output_dir):
        self.pck_path = pck_path
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.name_to_id = {}
        self.build_id_mappings()
        
    def load_id_table(self, path):
        """Load the Wwise ID table JSON"""
        print(f"📖 Loading ID table from {path}...")
        with open(path, 'r') as f:
            data = json.load(f)
        
        total = sum(len(data.get(key, [])) for key in data.keys())
        print(f"   Found {total} ID mappings")
        return data
    
    def build_id_mappings(self):
        """Build bidirectional ID to name mappings"""
        print("🔗 Building ID mappings...")

        # Process obj1s (simple list of dicts)
        if 'obj1s' in self.id_table:
            for entry in self.id_table['obj1s']:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']

                    # If key is a string (not hex), it's an event name
                    if not key.startswith('0x'):
                        self.name_to_id[key] = val
                        if val not in self.id_to_name:
                            self.id_to_name[val] = key
                    else:
                        # Hex key - convert to int
                        try:
                            hex_id = int(key, 16)
                            if hex_id not in self.id_to_name:
                                self.id_to_name[hex_id] = f"id_{hex_id:08x}"
                        except:
                            pass

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
                                self.name_to_id[key] = val
                                if val not in self.id_to_name:
                                    self.id_to_name[val] = key

        # Process other object types (similar to obj1s)
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

        print(f"   Mapped {len(self.name_to_id)} event names")
        print(f"   Total IDs: {len(self.id_to_name)}")
    
    def categorize_event(self, event_name):
        """Categorize an event by its name into folder structure"""
        name_lower = event_name.lower()
        
        # Voice/dialogue
        if any(x in name_lower for x in ['vo', 'voice', 'dialogue', 'taunt', 'cheer', 'vocal', 'kill', 'death', 'pain', 'effort']):
            if 'kill' in name_lower:
                return 'voice/combat/kills'
            elif 'taunt' in name_lower or 'cheer' in name_lower:
                return 'voice/combat/taunts'
            elif 'death' in name_lower or 'pain' in name_lower:
                return 'voice/combat/pain'
            else:
                return 'voice/dialogue'
        
        # Music
        if any(x in name_lower for x in ['music', 'theme', 'ambient_music', 'score', 'soundtrack']):
            if 'battle' in name_lower or 'combat' in name_lower:
                return 'music/battle'
            elif 'menu' in name_lower:
                return 'music/menu'
            else:
                return 'music/ambient'
        
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
        if any(x in name_lower for x in ['ui', 'menu', 'button', 'click', 'select', 'hover', 'transition']):
            return 'sfx/ui'
        
        # Environment
        if any(x in name_lower for x in ['ambient', 'wind', 'water', 'fire', 'rain', 'thunder', 'environment']):
            return 'ambient/environment'
        
        # Creatures
        if any(x in name_lower for x in ['creature', 'monster', 'beast', 'orc', 'troll', 'dragon']):
            return 'voice/creatures'
        
        # Default to uncategorized
        return 'uncategorized'
    
    def extract_wem_files(self):
        """Extract WEM files from PCK using QuickBMS"""
        print(f"\n{'='*60}")
        print("🔧 Extracting WEM files from PCK...")
        print(f"{'='*60}\n")
        
        # Create temp directory for extraction
        temp_dir = self.output_dir / 'temp_extraction'
        temp_dir.mkdir(parents=True, exist_ok=True)
        
        # Use QuickBMS to extract
        quickbms = Path('Augment/Wwise-Unpacker-1.0.3/Wwise-Unpacker-1.0.3/Tools/quickbms.exe')
        wavescan = Path('Augment/Wwise-Unpacker-1.0.3/Wwise-Unpacker-1.0.3/Tools/wavescan.bms')
        
        if not quickbms.exists():
            print(f"❌ QuickBMS not found at {quickbms}")
            return None
        
        print(f"   Using QuickBMS: {quickbms}")
        print(f"   Script: {wavescan}")
        print(f"   Output: {temp_dir}")
        
        # Run QuickBMS with -o flag to overwrite and auto-rename
        try:
            result = subprocess.run(
                [str(quickbms), '-o', '-Y', str(wavescan), str(self.pck_path), str(temp_dir)],
                capture_output=True,
                text=True,
                timeout=120
            )
            
            if result.returncode != 0:
                print(f"⚠️  QuickBMS returned code {result.returncode}")
            
            # Count extracted files
            extracted_files = list(temp_dir.glob('*.wav')) + list(temp_dir.glob('*.wem'))
            print(f"✓ Extracted {len(extracted_files)} files")
            
            return temp_dir
            
        except subprocess.TimeoutExpired:
            print("❌ QuickBMS timed out")
            return None
        except Exception as e:
            print(f"❌ Error running QuickBMS: {e}")
            return None
    
    def organize_files(self, temp_dir):
        """Organize extracted files by category with proper names"""
        print(f"\n{'='*60}")
        print("📁 Organizing files by category...")
        print(f"{'='*60}\n")
        
        if not temp_dir or not temp_dir.exists():
            print("❌ No temp directory found")
            return
        
        # Get all extracted files
        files = list(temp_dir.glob('*.wav')) + list(temp_dir.glob('*.wem'))
        print(f"   Processing {len(files)} files...")
        
        stats = defaultdict(int)
        organized_count = 0
        
        for file_path in files:
            # Try to extract ID from filename
            # Format might be: sound_123.wav or 12345678.wem
            filename = file_path.stem
            
            # Try different ID extraction methods
            file_id = None
            event_name = None
            
            # Method 1: Direct numeric ID
            if filename.isdigit():
                file_id = int(filename)
            # Method 2: sound_N format
            elif filename.startswith('sound_'):
                try:
                    num = int(filename.split('_')[1])
                    # This is just a sequence number, not the actual ID
                    # We'll use it as-is for now
                    event_name = f"sound_{num:04d}"
                except:
                    pass
            
            # Look up event name from ID
            if file_id and file_id in self.id_to_name:
                event_name = self.id_to_name[file_id]
            
            # If we have an event name, categorize it
            if event_name:
                category = self.categorize_event(event_name)
                stats[category] += 1
            else:
                # No name found, use uncategorized
                category = 'uncategorized'
                event_name = filename
                stats[category] += 1
            
            # Create category directory
            category_dir = self.output_dir / category
            category_dir.mkdir(parents=True, exist_ok=True)
            
            # Sanitize event name for filename
            safe_name = event_name.replace('/', '_').replace('\\', '_').replace(':', '_')
            
            # Copy file to organized location
            dest_path = category_dir / f"{safe_name}{file_path.suffix}"
            
            # Handle duplicates
            counter = 1
            while dest_path.exists():
                dest_path = category_dir / f"{safe_name}_{counter}{file_path.suffix}"
                counter += 1
            
            # Copy the file
            import shutil
            shutil.copy2(file_path, dest_path)
            organized_count += 1
        
        print(f"\n✓ Organized {organized_count} files")
        print(f"\n📊 Category Statistics:")
        print(f"{'='*60}")
        for category, count in sorted(stats.items(), key=lambda x: x[1], reverse=True):
            print(f"   {category:40s} {count:4d} files")
        print(f"{'='*60}")
    
    def run(self, use_existing=None):
        """Run the full extraction and organization process"""
        print(f"\n{'='*70}")
        print("🎮 LOTR: Conquest - Wwise Audio Extractor")
        print(f"{'='*70}\n")

        # Use existing extracted files if provided
        if use_existing and Path(use_existing).exists():
            print(f"📂 Using existing extracted files from: {use_existing}")
            temp_dir = Path(use_existing)
        else:
            # Extract WEM files
            temp_dir = self.extract_wem_files()

        if temp_dir:
            # Organize files
            self.organize_files(temp_dir)

            print(f"\n✅ Extraction complete!")
            print(f"📂 Output directory: {self.output_dir.absolute()}")
        else:
            print(f"\n❌ Extraction failed")

def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_organized.py <pck_file|existing_dir> [output_dir]")
        print("\nExamples:")
        print("  python extract_organized.py sound.pck extracted_organized")
        print("  python extract_organized.py extracted_bnk extracted_organized")
        sys.exit(1)

    input_path = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_organized'
    id_table = 'WWiseIDTable.audio.json'

    # Check if input is a directory (already extracted) or PCK file
    use_existing = None
    if os.path.isdir(input_path):
        use_existing = input_path
        pck_file = 'sound.pck'  # Dummy, won't be used
    else:
        pck_file = input_path
        if not os.path.exists(pck_file):
            print(f"❌ Error: PCK file not found: {pck_file}")
            sys.exit(1)

    if not os.path.exists(id_table):
        print(f"❌ Error: ID table not found: {id_table}")
        sys.exit(1)

    extractor = WwiseExtractor(pck_file, id_table, output_dir)
    extractor.run(use_existing=use_existing)

if __name__ == '__main__':
    main()

