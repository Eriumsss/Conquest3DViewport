#!/usr/bin/env python3
"""
Reorganize extracted audio files using proper bank names from bnk_full_mapping.csv
Creates clean category-based structure: Level/Hero/Creature/Environment/SFX/UI/Music/Chatter
"""

import os
import csv
import shutil
from pathlib import Path
from collections import defaultdict

MAPPING_FILE = "Audio/Analysis/bnk_full_mapping.csv"
CLASSIFICATION_FILE = "Audio/Reports/CLASSIFICATION.csv"

# Input directories
RAW_BNK_SRC = "Audio/Raw_BNK"
RAW_WEM_SRC = "Audio/Raw_WEM"
DECODED_WAV_SRC = "Audio/Decoded_WAVs"

# Output directories
RAW_BNK_OUT = "Audio/Raw_BNK_Organized"
RAW_WEM_OUT = "Audio/Raw_WEM_Organized"
DECODED_WAV_OUT = "Audio/Decoded_WAVs_Organized"

def load_bank_mapping():
    """Load bank ID to name mapping"""
    mapping = {}
    if not os.path.exists(MAPPING_FILE):
        print(f"Warning: {MAPPING_FILE} not found")
        return mapping
    
    with open(MAPPING_FILE, 'r', encoding='utf-8', errors='ignore') as f:
        reader = csv.DictReader(f)
        for row in reader:
            bank_name = row.get('BankName', '').strip()
            if bank_name:
                mapping[bank_name] = bank_name
    
    print(f"Loaded {len(mapping)} bank names")
    return mapping

def load_classification():
    """Load WEM classification data"""
    classification = {}
    if not os.path.exists(CLASSIFICATION_FILE):
        print(f"Warning: {CLASSIFICATION_FILE} not found")
        return classification
    
    with open(CLASSIFICATION_FILE, 'r', encoding='utf-8', errors='ignore') as f:
        reader = csv.DictReader(f)
        for row in reader:
            wem_id = row.get('WEM_ID', '').strip()
            bnk_name = row.get('BNK_Name', '').strip()
            category = row.get('Category', 'Misc').strip()
            if wem_id:
                classification[wem_id] = {
                    'bnk_name': bnk_name,
                    'category': category
                }
    
    print(f"Loaded {len(classification)} WEM classifications")
    return classification

def classify_by_name(name):
    """Classify by bank name"""
    name_lower = name.lower()
    
    if any(x in name_lower for x in ['level', 'map', 'moria', 'gondor', 'rohan', 'isengard', 'shire', 'rivendell', 'pelennor', 'mount', 'osgiliath']):
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

def reorganize_wavs(classification):
    """Reorganize WAV files using classification data"""
    print("\nReorganizing WAV files...")
    
    if not os.path.exists(DECODED_WAV_SRC):
        print(f"Source directory not found: {DECODED_WAV_SRC}")
        return 0
    
    os.makedirs(DECODED_WAV_OUT, exist_ok=True)
    
    count = 0
    for root, dirs, files in os.walk(DECODED_WAV_SRC):
        for file in files:
            if not file.endswith('.wav'):
                continue
            
            src_file = os.path.join(root, file)
            
            # Extract WEM ID from filename
            wem_id = file.replace('.wav', '').upper()
            
            # Get classification
            if wem_id in classification:
                info = classification[wem_id]
                category = info['category']
                bnk_name = info['bnk_name']
            else:
                category = 'Misc'
                bnk_name = 'Unknown'
            
            # Create destination
            dst_dir = os.path.join(DECODED_WAV_OUT, category, bnk_name)
            os.makedirs(dst_dir, exist_ok=True)
            
            dst_file = os.path.join(dst_dir, file)
            
            try:
                shutil.copy2(src_file, dst_file)
                count += 1
                if count % 200 == 0:
                    print(f"  Reorganized {count} WAV files...")
            except Exception as e:
                print(f"Error: {e}")
    
    print(f"Reorganized {count} WAV files")
    return count

def reorganize_wems(classification):
    """Reorganize WEM files using classification data"""
    print("\nReorganizing WEM files...")
    
    if not os.path.exists(RAW_WEM_SRC):
        print(f"Source directory not found: {RAW_WEM_SRC}")
        return 0
    
    os.makedirs(RAW_WEM_OUT, exist_ok=True)
    
    count = 0
    for root, dirs, files in os.walk(RAW_WEM_SRC):
        for file in files:
            if not file.endswith('.wem'):
                continue
            
            src_file = os.path.join(root, file)
            
            # Extract WEM ID from filename
            wem_id = file.replace('.wem', '').upper()
            
            # Get classification
            if wem_id in classification:
                info = classification[wem_id]
                category = info['category']
                bnk_name = info['bnk_name']
            else:
                category = 'Misc'
                bnk_name = 'Unknown'
            
            # Create destination
            dst_dir = os.path.join(RAW_WEM_OUT, category, bnk_name)
            os.makedirs(dst_dir, exist_ok=True)
            
            dst_file = os.path.join(dst_dir, file)
            
            try:
                shutil.copy2(src_file, dst_file)
                count += 1
                if count % 200 == 0:
                    print(f"  Reorganized {count} WEM files...")
            except Exception as e:
                print(f"Error: {e}")
    
    print(f"Reorganized {count} WEM files")
    return count

def reorganize_bnks(bank_mapping):
    """Reorganize BNK files"""
    print("\nReorganizing BNK files...")
    
    if not os.path.exists(RAW_BNK_SRC):
        print(f"Source directory not found: {RAW_BNK_SRC}")
        return 0
    
    os.makedirs(RAW_BNK_OUT, exist_ok=True)
    
    count = 0
    for root, dirs, files in os.walk(RAW_BNK_SRC):
        for file in files:
            if not file.endswith('.bnk'):
                continue
            
            src_file = os.path.join(root, file)
            
            # Try to find bank name
            bnk_name = 'Unknown'
            for name in bank_mapping.keys():
                if name in file or name.lower() in file.lower():
                    bnk_name = name
                    break
            
            category = classify_by_name(bnk_name)
            
            # Create destination
            dst_dir = os.path.join(RAW_BNK_OUT, category, bnk_name)
            os.makedirs(dst_dir, exist_ok=True)
            
            dst_file = os.path.join(dst_dir, file)
            
            try:
                shutil.copy2(src_file, dst_file)
                count += 1
            except Exception as e:
                print(f"Error: {e}")
    
    print(f"Reorganized {count} BNK files")
    return count

def main():
    print("=" * 80)
    print("REORGANIZING AUDIO FILES BY PROPER CATEGORIES")
    print("=" * 80)
    
    bank_mapping = load_bank_mapping()
    classification = load_classification()
    
    if not classification:
        print("ERROR: No classification data found!")
        return
    
    wav_count = reorganize_wavs(classification)
    wem_count = reorganize_wems(classification)
    bnk_count = reorganize_bnks(bank_mapping)
    
    print("\n" + "=" * 80)
    print("REORGANIZATION COMPLETE")
    print("=" * 80)
    print(f"\nNew organized directories created:")
    print(f"  - {RAW_BNK_OUT}/ ({bnk_count} files)")
    print(f"  - {RAW_WEM_OUT}/ ({wem_count} files)")
    print(f"  - {DECODED_WAV_OUT}/ ({wav_count} files)")
    print(f"\nTotal: {bnk_count + wem_count + wav_count} files reorganized")
    print("\nYou can now delete the old directories:")
    print(f"  - {RAW_BNK_SRC}/")
    print(f"  - {RAW_WEM_SRC}/")
    print(f"  - {DECODED_WAV_SRC}/")

if __name__ == "__main__":
    main()

