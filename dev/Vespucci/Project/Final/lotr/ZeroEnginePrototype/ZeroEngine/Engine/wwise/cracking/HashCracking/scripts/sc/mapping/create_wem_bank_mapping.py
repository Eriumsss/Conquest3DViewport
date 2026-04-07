#!/usr/bin/env python3
"""
Create WEM ID to Bank Name mapping by scanning extracted BNK files
"""

import os
import struct
import csv
import re
from pathlib import Path
from collections import defaultdict

INPUT_BNK_DIR = "Audio/extracted_bnk"
OUTPUT_FILE = "Audio/Reports/WEM_BANK_MAPPING.csv"

DIDX_SIGNATURE = b'DIDX'
DATA_SIGNATURE = b'DATA'
STID_SIGNATURE = b'STID'

def sanitize_name(name):
    """Sanitize filename"""
    if not name:
        return "Unknown"
    name = name.replace('\x00', '').strip()
    name = re.sub(r'[<>:"/\\|?*]', '_', name)
    name = name.strip('. ')
    return name if name else "Unknown"

def extract_stid(bnk_data):
    """Extract STID name from BNK"""
    try:
        pos = bnk_data.find(STID_SIGNATURE)
        if pos == -1:
            return 'Unknown'

        size_pos = pos + 4
        if size_pos + 4 > len(bnk_data):
            return 'Unknown'

        size = struct.unpack('<I', bnk_data[size_pos:size_pos+4])[0]
        data_start = size_pos + 4

        if data_start + size > len(bnk_data):
            return 'Unknown'

        stid_data = bnk_data[data_start:data_start+size]

        if len(stid_data) >= 5:
            name_len = stid_data[4]
            # Sanity check: name length should be reasonable (< 256)
            if name_len > 0 and name_len < 256 and 5 + name_len <= len(stid_data):
                name_bytes = stid_data[5:5+name_len]
                # Only keep printable ASCII characters
                name = ''.join(chr(b) for b in name_bytes if 32 <= b < 127)
                if name and len(name) > 2:  # Must be at least 3 chars
                    return sanitize_name(name)
    except:
        pass

    return 'Unknown'

def extract_wem_ids(bnk_data):
    """Extract WEM IDs from DIDX section"""
    wem_ids = []
    try:
        didx_pos = bnk_data.find(DIDX_SIGNATURE)
        if didx_pos == -1:
            return wem_ids
        
        didx_size_pos = didx_pos + 4
        didx_size = struct.unpack('<I', bnk_data[didx_size_pos:didx_size_pos+4])[0]
        didx_data_start = didx_size_pos + 4
        
        pos = didx_data_start
        end = didx_data_start + didx_size
        
        while pos + 12 <= end:
            try:
                wem_id = struct.unpack('<I', bnk_data[pos:pos+4])[0]
                wem_ids.append(f"{wem_id:08X}")
            except:
                pass
            pos += 12
    except:
        pass
    
    return wem_ids

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

def main():
    print("Creating WEM ID to Bank Name mapping...")
    
    if not os.path.exists(INPUT_BNK_DIR):
        print(f"Error: {INPUT_BNK_DIR} not found")
        return
    
    wem_mapping = {}
    bnk_files = sorted(Path(INPUT_BNK_DIR).glob("*.bnk"))
    
    print(f"Scanning {len(bnk_files)} BNK files...")
    
    for i, bnk_file in enumerate(bnk_files):
        try:
            with open(bnk_file, 'rb') as f:
                bnk_data = f.read()
            
            # Extract bank name
            bank_name = extract_stid(bnk_data)
            
            # Extract WEM IDs
            wem_ids = extract_wem_ids(bnk_data)
            
            # Map each WEM to this bank
            for wem_id in wem_ids:
                wem_mapping[wem_id] = bank_name
            
            if (i + 1) % 50 == 0:
                print(f"  Processed {i+1}/{len(bnk_files)} BNKs, mapped {len(wem_mapping)} WEMs")
        except Exception as e:
            print(f"Error processing {bnk_file}: {e}")
    
    # Write mapping to CSV
    os.makedirs(os.path.dirname(OUTPUT_FILE), exist_ok=True)
    
    with open(OUTPUT_FILE, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['WEM_ID', 'Bank_Name', 'Category'])
        writer.writeheader()
        
        for wem_id in sorted(wem_mapping.keys()):
            bank_name = wem_mapping[wem_id]
            category = classify_by_name(bank_name)
            writer.writerow({
                'WEM_ID': wem_id,
                'Bank_Name': bank_name,
                'Category': category
            })
    
    print(f"\nMapping complete!")
    print(f"  Total WEMs mapped: {len(wem_mapping)}")
    print(f"  Output file: {OUTPUT_FILE}")
    
    # Print category breakdown
    categories = defaultdict(int)
    for wem_id, bank_name in wem_mapping.items():
        category = classify_by_name(bank_name)
        categories[category] += 1
    
    print(f"\nCategory breakdown:")
    for cat in sorted(categories.keys()):
        print(f"  {cat}: {categories[cat]}")

if __name__ == "__main__":
    main()

