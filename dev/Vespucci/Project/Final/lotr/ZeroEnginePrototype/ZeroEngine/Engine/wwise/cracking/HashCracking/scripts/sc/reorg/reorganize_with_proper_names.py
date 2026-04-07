#!/usr/bin/env python3
"""
Reorganize extracted files with proper bank names from bnk_full_mapping.csv
"""

import os
import csv
import shutil
from pathlib import Path
from collections import defaultdict

MAPPING_FILE = "Audio/Analysis/bnk_full_mapping.csv"
RAW_BNK_DIR = "Audio/Raw_BNK"
RAW_WEM_DIR = "Audio/Raw_WEM"
DECODED_WAV_DIR = "Audio/Decoded_WAVs"

def load_bank_mapping():
    """Load bank name mapping from CSV"""
    mapping = {}
    if not os.path.exists(MAPPING_FILE):
        print(f"Mapping file not found: {MAPPING_FILE}")
        return mapping
    
    with open(MAPPING_FILE, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            bank_name = row.get('BankName', '').strip()
            file_path = row.get('FilePath', '').strip()
            if bank_name and file_path:
                # Extract bank filename
                bnk_file = os.path.basename(file_path)
                mapping[bnk_file] = bank_name
    
    print(f"Loaded {len(mapping)} bank name mappings")
    return mapping

def classify_by_name(name):
    """Classify audio by bank name"""
    name_lower = name.lower()
    
    if any(x in name_lower for x in ['level', 'map', 'moria', 'gondor', 'rohan', 'isengard', 'shire', 'rivendell', 'pelennor', 'mount']):
        return 'Level'
    elif any(x in name_lower for x in ['hero', 'player', 'aragorn', 'gandalf', 'gimli', 'legolas', 'boromir', 'eowyn', 'theoden', 'lurtz', 'mouth', 'wormtongue', 'nazgul', 'witch', 'balrog', 'sauron']):
        return 'Hero'
    elif any(x in name_lower for x in ['creature', 'orc', 'troll', 'dragon', 'warg', 'ent', 'balrog', 'eagle']):
        return 'Creature'
    elif any(x in name_lower for x in ['ambient', 'env', 'environment', 'wind', 'water', 'ambience', 'vo_']):
        return 'Environment'
    elif any(x in name_lower for x in ['ui', 'menu', 'button', 'interface']):
        return 'UI'
    elif any(x in name_lower for x in ['sfx', 'effect', 'weapon', 'impact', 'footstep', 'catapult']):
        return 'SFX'
    elif any(x in name_lower for x in ['music', 'bgm', 'theme', 'score']):
        return 'Music'
    elif any(x in name_lower for x in ['chatter', 'vo_']):
        return 'Chatter'
    else:
        return 'Misc'

def reorganize_directory(src_dir, dst_dir, mapping):
    """Reorganize directory with proper names"""
    if not os.path.exists(src_dir):
        print(f"Source directory not found: {src_dir}")
        return
    
    file_count = 0
    for root, dirs, files in os.walk(src_dir):
        for file in files:
            src_file = os.path.join(root, file)
            
            # Try to find proper bank name
            bank_name = "Unknown"
            for bnk_file, proper_name in mapping.items():
                if bnk_file in src_file:
                    bank_name = proper_name
                    break
            
            # Classify
            category = classify_by_name(bank_name)
            
            # Create destination path
            dst_path = os.path.join(dst_dir, category, bank_name)
            os.makedirs(dst_path, exist_ok=True)
            
            dst_file = os.path.join(dst_path, file)
            
            # Copy file
            try:
                shutil.copy2(src_file, dst_file)
                file_count += 1
                if file_count % 100 == 0:
                    print(f"  Reorganized {file_count} files...")
            except Exception as e:
                print(f"Error copying {src_file}: {e}")
    
    print(f"Reorganized {file_count} files in {src_dir}")

def main():
    print("Loading bank name mappings...")
    mapping = load_bank_mapping()
    
    if not mapping:
        print("No mappings found, skipping reorganization")
        return
    
    print("\nReorganizing Raw_BNK...")
    reorganize_directory(RAW_BNK_DIR, RAW_BNK_DIR + "_organized", mapping)
    
    print("\nReorganizing Raw_WEM...")
    reorganize_directory(RAW_WEM_DIR, RAW_WEM_DIR + "_organized", mapping)
    
    print("\nReorganizing Decoded_WAVs...")
    reorganize_directory(DECODED_WAV_DIR, DECODED_WAV_DIR + "_organized", mapping)
    
    print("\n✅ Reorganization complete!")
    print("New directories created:")
    print(f"  - {RAW_BNK_DIR}_organized/")
    print(f"  - {RAW_WEM_DIR}_organized/")
    print(f"  - {DECODED_WAV_DIR}_organized/")

if __name__ == "__main__":
    main()

