#!/usr/bin/env python3
"""
Organize already extracted audio files using heuristics and ID mappings
"""

import os
import sys
import json
import shutil
from pathlib import Path
from collections import defaultdict

class AudioOrganizer:
    def __init__(self, input_dir, id_table_path, output_dir):
        self.input_dir = Path(input_dir)
        self.output_dir = Path(output_dir)
        self.id_table = self.load_id_table(id_table_path)
        self.id_to_name = {}
        self.build_id_mappings()
        
    def load_id_table(self, path):
        """Load the Wwise ID table JSON"""
        print(f"📖 Loading ID table from {path}...")
        with open(path, 'r') as f:
            data = json.load(f)
        return data
    
    def build_id_mappings(self):
        """Build ID to name mappings"""
        print("🔗 Building ID mappings...")
        
        # Process obj1s - these have the most useful event names
        if 'obj1s' in self.id_table:
            for entry in self.id_table['obj1s']:
                if isinstance(entry, dict) and 'key' in entry and 'val' in entry:
                    key = entry['key']
                    val = entry['val']
                    
                    # String keys are event names
                    if not key.startswith('0x'):
                        if val not in self.id_to_name:
                            self.id_to_name[val] = key
                    else:
                        # Also store hex IDs
                        try:
                            hex_id = int(key, 16)
                            if hex_id not in self.id_to_name:
                                self.id_to_name[hex_id] = f"id_{hex_id:08x}"
                        except:
                            pass
        
        # Process obj2s (nested structure with character types)
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
        
        print(f"   Mapped {len(self.id_to_name)} IDs to names")
        
        # Print some example mappings
        print(f"\n   Sample event names:")
        count = 0
        for val, name in self.id_to_name.items():
            if not name.startswith('id_'):
                print(f"      {name}")
                count += 1
                if count >= 10:
                    break
    
    def categorize_by_name(self, event_name):
        """Categorize an event by its name"""
        name_lower = event_name.lower()
        
        # Voice/dialogue
        if any(x in name_lower for x in ['vo', 'voice', 'dialogue', 'taunt', 'cheer', 'vocal', 'kill', 'death', 'pain', 'effort', 'grunt', 'yell', 'scream']):
            if 'kill' in name_lower:
                return 'voice/combat/kills'
            elif 'taunt' in name_lower or 'cheer' in name_lower:
                return 'voice/combat/taunts'
            elif 'death' in name_lower or 'pain' in name_lower or 'grunt' in name_lower:
                return 'voice/combat/pain'
            else:
                return 'voice/dialogue'
        
        # Music
        if any(x in name_lower for x in ['music', 'theme', 'ambient_music', 'score', 'soundtrack', 'bgm']):
            return 'music'
        
        # Weapons
        if any(x in name_lower for x in ['sword', 'blade', 'swing', 'slash', 'stab', 'arrow', 'bow', 'axe', 'spear', 'weapon', 'ranged_attack']):
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
            elif 'wood' in name_lower or 'metal' in name_lower or 'stone' in name_lower:
                return 'sfx/combat/impacts_material'
            else:
                return 'sfx/combat/impacts'
        
        # Magic/abilities
        if any(x in name_lower for x in ['ability', 'magic', 'spell', 'firewall', 'lightning', 'heal', 'power']):
            return 'sfx/abilities'
        
        # Movement
        if any(x in name_lower for x in ['footstep', 'walk', 'run', 'jump', 'land', 'movement']):
            return 'sfx/movement'
        
        # UI
        if any(x in name_lower for x in ['ui', 'menu', 'button', 'click', 'select', 'hover', 'transition', 'state', 'cp_transition']):
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
    
    def analyze_file_content(self, file_path):
        """Analyze file content to guess its type"""
        try:
            size = file_path.stat().st_size
            
            # Read first few bytes to check format
            with open(file_path, 'rb') as f:
                header = f.read(100)
            
            # Check for Vorbis encoding (voice/music)
            if b'vorb' in header:
                # Vorbis files are typically voice or music
                # Larger files are more likely music
                if size > 50000:
                    return 'music_or_voice_large'
                elif size > 20000:
                    return 'voice_or_sfx_medium'
                else:
                    return 'sfx_small'
            
            # Check for PCM/WAV
            if header.startswith(b'RIFF') and b'WAVE' in header:
                if size > 50000:
                    return 'music_or_voice_large'
                elif size > 15000:
                    return 'voice_or_sfx_medium'
                else:
                    return 'sfx_small'
            
            return 'unknown'
        except:
            return 'unknown'
    
    def organize(self):
        """Organize the extracted files"""
        print(f"\n{'='*70}")
        print("🎮 LOTR: Conquest - Audio Organizer")
        print(f"{'='*70}\n")
        
        print(f"📂 Input: {self.input_dir}")
        print(f"📂 Output: {self.output_dir}")
        
        # Get all audio files
        audio_files = list(self.input_dir.glob('*.wav')) + list(self.input_dir.glob('*.wem'))
        print(f"\n   Found {len(audio_files)} audio files")
        
        if not audio_files:
            print("❌ No audio files found!")
            return
        
        print(f"\n{'='*70}")
        print("📁 Organizing files...")
        print(f"{'='*70}\n")
        
        stats = defaultdict(int)
        organized_count = 0
        
        for file_path in audio_files:
            # Try to get event name from ID table
            # The files are named sound_N.wav where N is just a sequence number
            # We don't have the actual Wwise IDs, so we'll use heuristics
            
            filename = file_path.stem
            
            # Analyze file content
            content_type = self.analyze_file_content(file_path)
            
            # Determine category based on content analysis
            if content_type == 'music_or_voice_large':
                # Large files - could be music or long voice clips
                # Put in a special folder for manual sorting
                category = 'large_files/music_or_voice'
            elif content_type == 'voice_or_sfx_medium':
                category = 'medium_files/voice_or_sfx'
            elif content_type == 'sfx_small':
                category = 'small_files/sfx'
            else:
                category = 'uncategorized'
            
            stats[category] += 1
            
            # Create category directory
            category_dir = self.output_dir / category
            category_dir.mkdir(parents=True, exist_ok=True)
            
            # Copy file
            dest_path = category_dir / file_path.name
            
            # Handle duplicates
            counter = 1
            while dest_path.exists():
                dest_path = category_dir / f"{file_path.stem}_{counter}{file_path.suffix}"
                counter += 1
            
            shutil.copy2(file_path, dest_path)
            organized_count += 1
            
            if organized_count % 20 == 0:
                print(f"   Processed {organized_count}/{len(audio_files)} files...")
        
        print(f"\n✓ Organized {organized_count} files")
        print(f"\n📊 Category Statistics:")
        print(f"{'='*70}")
        for category, count in sorted(stats.items(), key=lambda x: x[1], reverse=True):
            print(f"   {category:50s} {count:4d} files")
        print(f"{'='*70}")
        
        print(f"\n💡 Note: Files are organized by size/type since we don't have")
        print(f"   the original Wwise IDs. You may need to manually categorize")
        print(f"   files in the 'large_files' and 'medium_files' folders.")
        
        print(f"\n✅ Organization complete!")
        print(f"📂 Output: {self.output_dir.absolute()}")

def main():
    if len(sys.argv) < 2:
        print("Usage: python organize_extracted.py <input_dir> [output_dir]")
        print("\nExample:")
        print("  python organize_extracted.py extracted_bnk extracted_organized")
        sys.exit(1)
    
    input_dir = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_organized'
    id_table = 'WWiseIDTable.audio.json'
    
    if not os.path.exists(input_dir):
        print(f"❌ Error: Input directory not found: {input_dir}")
        sys.exit(1)
    
    if not os.path.exists(id_table):
        print(f"⚠️  Warning: ID table not found: {id_table}")
        print(f"   Proceeding with content-based organization only")
    
    organizer = AudioOrganizer(input_dir, id_table, output_dir)
    organizer.organize()

if __name__ == '__main__':
    main()

