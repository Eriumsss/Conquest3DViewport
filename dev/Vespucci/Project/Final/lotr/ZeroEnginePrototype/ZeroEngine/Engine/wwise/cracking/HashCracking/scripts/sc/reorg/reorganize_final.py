#!/usr/bin/env python3
"""
Reorganize audio files into a structured format:
- root/ for non-language content (music, SFX, ambience, levels)
- Languages/ for language-specific content (chatter, voice-overs)
"""

import re
import shutil
from pathlib import Path
from collections import defaultdict

# Paths
SOURCE_DIR = Path("Organized")
TXTP_DIR = SOURCE_DIR / "txtp"
EXTRACTED_DIR = SOURCE_DIR / "extracted"
OUTPUT_DIR = Path("Organized_Final")

# Language folders
LANGUAGES = ["english(us)", "french(france)", "german", "italian", "russian", "spanish(spain)"]

def sanitize_language_name(lang):
    """Convert language name to safe folder name."""
    return lang.replace('(', '_').replace(')', '_')

def parse_txtp_file(txtp_path):
    """Parse a TXTP file to extract BNK source and WEM files."""
    bnk_file = None
    wem_files = []
    
    try:
        with open(txtp_path, 'r', encoding='utf-8') as f:
            for line in f:
                # Extract BNK file: # - extracted/85412153.bnk or # - extracted/english(us)/871361464.bnk
                if line.startswith("# - extracted/"):
                    bnk_match = re.search(r'extracted/(.+?)\.bnk', line)
                    if bnk_match:
                        bnk_file = bnk_match.group(1)  # Returns "85412153" or "english(us)/871361464"
                
                # Extract WEM files: wem/363070948.ogg or wem/363070948.wav
                if line.strip().startswith("wem/"):
                    wem_match = re.search(r'wem/(\d+)\.(ogg|wav|wem)', line)
                    if wem_match:
                        wem_id = wem_match.group(1)
                        wem_files.append(wem_id)
    except Exception as e:
        print(f"Error parsing {txtp_path}: {e}")
        return None, []
    
    return bnk_file, wem_files

def get_category_from_filename(filename):
    """Extract category from TXTP filename."""
    # Examples: Ambience-0086-event.txtp -> "Ambience"
    #          BaseCombat-0705-event.txtp -> "BaseCombat"
    match = re.match(r'^([A-Za-z_]+)', filename)
    if match:
        return match.group(1)
    return "Uncategorized"

def find_bnk_and_language(bnk_id):
    """Find the BNK file and determine if it's language-specific."""
    # bnk_id might be "language/id" or just "id"
    # Extract just the numeric ID
    clean_id = bnk_id.split('/')[-1] if '/' in bnk_id else bnk_id
    
    # Check if it's in a language folder
    for lang in LANGUAGES:
        lang_bnk_path = EXTRACTED_DIR / lang / f"{clean_id}.bnk"
        if lang_bnk_path.exists():
            return lang_bnk_path, lang, clean_id
    
    # Check root extracted folder
    root_bnk_path = EXTRACTED_DIR / f"{clean_id}.bnk"
    if root_bnk_path.exists():
        return root_bnk_path, None, clean_id
    
    return None, None, clean_id

def find_wem_file(wem_id, language=None):
    """Find a WEM file in the appropriate location."""
    wem_filename = f"{wem_id}.wem"
    
    # If language-specific, check language folder first
    if language:
        lang_wem_path = EXTRACTED_DIR / language / wem_filename
        if lang_wem_path.exists():
            return lang_wem_path
    
    # Check root extracted folder
    root_wem_path = EXTRACTED_DIR / wem_filename
    if root_wem_path.exists():
        return root_wem_path
    
    return None

def main():
    print("Parsing TXTP files...")
    
    # Collect all TXTP files and organize by category and BNK
    organization = defaultdict(lambda: defaultdict(lambda: {
        'txtp_files': [],
        'wem_files': set(),
        'language': None,
        'bnk_path': None
    }))
    
    txtp_files = list(TXTP_DIR.glob("*.txtp"))
    print(f"Found {len(txtp_files)} TXTP files")
    
    for txtp_path in txtp_files:
        bnk_id, wem_ids = parse_txtp_file(txtp_path)
        
        if not bnk_id:
            print(f"Warning: Could not find BNK reference in {txtp_path.name}")
            continue
        
        # Get category from filename
        category = get_category_from_filename(txtp_path.name)
        
        # Find BNK file and determine language
        bnk_path, language, clean_bnk_id = find_bnk_and_language(bnk_id)
        
        if not bnk_path:
            print(f"Warning: Could not find BNK file for {bnk_id}")
            continue
        
        # Store information
        org_data = organization[category][clean_bnk_id]
        org_data['txtp_files'].append(txtp_path)
        org_data['wem_files'].update(wem_ids)
        org_data['language'] = language
        org_data['bnk_path'] = bnk_path
    
    print(f"\nCreating organized folder structure...")
    print(f"Found {len(organization)} categories")
    
    # Create organized structure
    total_bnks = 0
    total_txtps = 0
    total_wems = 0
    
    for category, bnks in organization.items():
        print(f"\nProcessing category: {category} ({len(bnks)} BNK files)")
        
        for bnk_id, data in bnks.items():
            if data['language']:
                # Language-specific: Organized_Final/Languages/english_us/Category/BNKid/
                safe_language = sanitize_language_name(data['language'])
                output_path = OUTPUT_DIR / "Languages" / safe_language / category / bnk_id
            else:
                # Non-language: Organized_Final/root/Category/BNKid/
                output_path = OUTPUT_DIR / "root" / category / bnk_id
            
            try:
                output_path.mkdir(parents=True, exist_ok=True)
            except OSError as e:
                print(f"  ERROR creating directory: {e}")
                continue
            
            # Copy BNK file
            bnk_dest = output_path / data['bnk_path'].name
            if not bnk_dest.exists():
                shutil.copy2(data['bnk_path'], bnk_dest)
                total_bnks += 1
            
            # Copy TXTP files
            for txtp_path in data['txtp_files']:
                txtp_dest = output_path / txtp_path.name
                if not txtp_dest.exists():
                    shutil.copy2(txtp_path, txtp_dest)
                    total_txtps += 1
            
            # Copy WEM files
            for wem_id in data['wem_files']:
                wem_path = find_wem_file(wem_id, data['language'])
                if wem_path:
                    wem_dest = output_path / wem_path.name
                    if not wem_dest.exists():
                        shutil.copy2(wem_path, wem_dest)
                        total_wems += 1
                else:
                    print(f"  Warning: Could not find WEM file {wem_id}.wem")
            
            lang_str = f"({data['language']})" if data['language'] else "(root)"
            print(f"  {lang_str} {bnk_id}: {len(data['txtp_files'])} TXTPs, {len(data['wem_files'])} WEMs")
    
    print(f"\n[SUCCESS] Organization complete!")
    print(f"Output directory: {OUTPUT_DIR.absolute()}")
    print(f"Total: {total_bnks} BNKs, {total_txtps} TXTPs, {total_wems} WEMs copied")

if __name__ == "__main__":
    main()

