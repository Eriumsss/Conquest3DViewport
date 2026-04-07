#!/usr/bin/env python3
"""
Final reorganization using WEM_BANK_MAPPING.csv
"""

import os
import csv
import shutil
from pathlib import Path

import re

WEM_MAPPING_FILE = "Audio/Reports/WEM_BANK_MAPPING.csv"

# Input directories
RAW_BNK_SRC = "Audio/Raw_BNK"
RAW_WEM_SRC = "Audio/Raw_WEM"
DECODED_WAV_SRC = "Audio/Decoded_WAVs"

# Output directories
RAW_BNK_OUT = "Audio/Raw_BNK_Final"
RAW_WEM_OUT = "Audio/Raw_WEM_Final"
DECODED_WAV_OUT = "Audio/Decoded_WAVs_Final"

def sanitize_path(name):
    """Sanitize name for use in file paths"""
    if not name:
        return "Unknown"
    # Remove null bytes and control characters
    name = ''.join(c for c in name if ord(c) >= 32 or c in '\t\n\r')
    # Replace invalid path characters
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', '_', name)
    # Remove leading/trailing spaces and dots
    name = name.strip('. ')
    return name if name else "Unknown"

def load_wem_mapping():
    """Load WEM ID to Bank/Category mapping"""
    mapping = {}
    if not os.path.exists(WEM_MAPPING_FILE):
        print(f"Error: {WEM_MAPPING_FILE} not found")
        return mapping
    
    with open(WEM_MAPPING_FILE, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            wem_id = row.get('WEM_ID', '').strip()
            bank_name = row.get('Bank_Name', 'Unknown').strip()
            category = row.get('Category', 'Misc').strip()
            if wem_id:
                mapping[wem_id] = {
                    'bank': bank_name,
                    'category': category
                }
    
    print(f"Loaded {len(mapping)} WEM mappings")
    return mapping

def reorganize_wavs(mapping):
    """Reorganize WAV files"""
    print("\nReorganizing WAV files...")

    if not os.path.exists(DECODED_WAV_SRC):
        print(f"Source not found: {DECODED_WAV_SRC}")
        return 0

    os.makedirs(DECODED_WAV_OUT, exist_ok=True)

    count = 0
    for root, dirs, files in os.walk(DECODED_WAV_SRC):
        for file in files:
            if not file.endswith('.wav'):
                continue

            src_file = os.path.join(root, file)
            wem_id = file.replace('.wav', '').upper()

            if wem_id in mapping:
                info = mapping[wem_id]
                category = info['category']
                bank = sanitize_path(info['bank'])
            else:
                category = 'Misc'
                bank = 'Unknown'

            dst_dir = os.path.join(DECODED_WAV_OUT, category, bank)
            os.makedirs(dst_dir, exist_ok=True)

            dst_file = os.path.join(dst_dir, file)

            try:
                shutil.copy2(src_file, dst_file)
                count += 1
                if count % 200 == 0:
                    print(f"  {count} WAVs...")
            except Exception as e:
                print(f"Error: {e}")

    print(f"Reorganized {count} WAV files")
    return count

def reorganize_wems(mapping):
    """Reorganize WEM files"""
    print("\nReorganizing WEM files...")

    if not os.path.exists(RAW_WEM_SRC):
        print(f"Source not found: {RAW_WEM_SRC}")
        return 0

    os.makedirs(RAW_WEM_OUT, exist_ok=True)

    count = 0
    for root, dirs, files in os.walk(RAW_WEM_SRC):
        for file in files:
            if not file.endswith('.wem'):
                continue

            src_file = os.path.join(root, file)
            wem_id = file.replace('.wem', '').upper()

            if wem_id in mapping:
                info = mapping[wem_id]
                category = info['category']
                bank = sanitize_path(info['bank'])
            else:
                category = 'Misc'
                bank = 'Unknown'

            dst_dir = os.path.join(RAW_WEM_OUT, category, bank)
            os.makedirs(dst_dir, exist_ok=True)

            dst_file = os.path.join(dst_dir, file)

            try:
                shutil.copy2(src_file, dst_file)
                count += 1
                if count % 200 == 0:
                    print(f"  {count} WEMs...")
            except Exception as e:
                print(f"Error: {e}")

    print(f"Reorganized {count} WEM files")
    return count

def reorganize_bnks():
    """Reorganize BNK files"""
    print("\nReorganizing BNK files...")
    
    if not os.path.exists(RAW_BNK_SRC):
        print(f"Source not found: {RAW_BNK_SRC}")
        return 0
    
    os.makedirs(RAW_BNK_OUT, exist_ok=True)
    
    count = 0
    for root, dirs, files in os.walk(RAW_BNK_SRC):
        for file in files:
            if not file.endswith('.bnk'):
                continue
            
            src_file = os.path.join(root, file)
            
            # Just copy to Misc/Unknown
            dst_dir = os.path.join(RAW_BNK_OUT, 'Misc', 'Unknown')
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
    print("FINAL AUDIO REORGANIZATION")
    print("=" * 80)
    
    mapping = load_wem_mapping()
    
    if not mapping:
        print("ERROR: No mapping data!")
        return
    
    wav_count = reorganize_wavs(mapping)
    wem_count = reorganize_wems(mapping)
    bnk_count = reorganize_bnks()
    
    print("\n" + "=" * 80)
    print("REORGANIZATION COMPLETE")
    print("=" * 80)
    print(f"\nNew directories created:")
    print(f"  - {RAW_BNK_OUT}/ ({bnk_count} files)")
    print(f"  - {RAW_WEM_OUT}/ ({wem_count} files)")
    print(f"  - {DECODED_WAV_OUT}/ ({wav_count} files)")
    print(f"\nTotal: {bnk_count + wem_count + wav_count} files")
    
    # Show category breakdown
    print(f"\nCategory breakdown:")
    for cat_dir in sorted(os.listdir(DECODED_WAV_OUT)):
        cat_path = os.path.join(DECODED_WAV_OUT, cat_dir)
        if os.path.isdir(cat_path):
            count = sum(1 for _ in Path(cat_path).rglob("*.wav"))
            print(f"  {cat_dir}: {count} files")

if __name__ == "__main__":
    main()

