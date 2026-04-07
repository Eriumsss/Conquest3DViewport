#!/usr/bin/env python3
"""
Organize Wwise audio files by category and BNK file.
Structure: Organized/Category/BNKName/files
"""

import os
import re
import shutil
from pathlib import Path
from collections import defaultdict

# Paths
TXTP_DIR = Path("txtp_all")
WEM_DIR = Path("wem")
EXTRACTED_DIR = Path("extracted")
OUTPUT_DIR = Path("Organized_AllLanguages")

# Language folders
LANGUAGES = ["english(us)", "french(france)", "german", "italian", "russian", "spanish(spain)"]

def parse_txtp_file(txtp_path):
    """Parse a TXTP file to extract BNK source and WEM files."""
    bnk_file = None
    wem_files = []
    
    with open(txtp_path, 'r', encoding='utf-8') as f:
        for line in f:
            # Extract BNK file: # - extracted/85412153.bnk
            if line.startswith("# - extracted/"):
                bnk_match = re.search(r'extracted/(.+?)\.bnk', line)
                if bnk_match:
                    bnk_file = bnk_match.group(1)
            
            # Extract WEM files: wem/363070948.ogg or wem/363070948.wav
            if line.startswith("wem/"):
                wem_match = re.search(r'wem/(\d+)\.(ogg|wav|wem)', line)
                if wem_match:
                    wem_id = wem_match.group(1)
                    wem_files.append(wem_id)
    
    return bnk_file, wem_files

def get_category_from_filename(filename):
    """Extract category from TXTP filename."""
    # Examples: Ambience-0086-event.txtp, BaseCombat-0705-event.txtp
    match = re.match(r'^([A-Za-z_]+)', filename)
    if match:
        return match.group(1)
    return "Uncategorized"

def find_bnk_path(bnk_id):
    """Find the full path to a BNK file (could be in root or language folder)."""
    # bnk_id might be "language/id" or just "id"
    # Extract just the numeric ID
    clean_id = bnk_id.split('/')[-1] if '/' in bnk_id else bnk_id

    # Check root extracted folder
    root_path = EXTRACTED_DIR / f"{clean_id}.bnk"
    if root_path.exists():
        return root_path, None

    # Check language folders
    for lang in LANGUAGES:
        lang_path = EXTRACTED_DIR / lang / f"{clean_id}.bnk"
        try:
            if lang_path.exists():
                return lang_path, lang
        except OSError:
            # Handle path issues on Windows
            continue

    return None, None

def organize_files():
    """Main organization function."""
    print("Starting audio file organization...")
    
    # Create output directory
    OUTPUT_DIR.mkdir(exist_ok=True)
    
    # Data structure: {category: {bnk_id: {txtp_files: [], wem_files: set(), language: None}}}
    organization = defaultdict(lambda: defaultdict(lambda: {
        'txtp_files': [],
        'wem_files': set(),
        'language': None,
        'bnk_path': None
    }))
    
    # Parse all TXTP files
    print(f"Parsing TXTP files from {TXTP_DIR}...")
    txtp_files = list(TXTP_DIR.glob("*.txtp"))
    print(f"Found {len(txtp_files)} TXTP files")
    
    for txtp_path in txtp_files:
        bnk_id, wem_ids = parse_txtp_file(txtp_path)
        
        if not bnk_id:
            print(f"Warning: Could not find BNK reference in {txtp_path.name}")
            continue
        
        # Get category from filename
        category = get_category_from_filename(txtp_path.name)
        
        # Find BNK file location
        bnk_path, language = find_bnk_path(bnk_id)
        if not bnk_path:
            print(f"Warning: Could not find BNK file {bnk_id}.bnk")
            continue
        
        # Store information
        org_data = organization[category][bnk_id]
        org_data['txtp_files'].append(txtp_path)
        org_data['wem_files'].update(wem_ids)
        org_data['language'] = language
        org_data['bnk_path'] = bnk_path
    
    # Create organized structure
    print("\nCreating organized folder structure...")
    total_categories = len(organization)
    total_bnks = sum(len(bnks) for bnks in organization.values())
    print(f"Found {total_categories} categories with {total_bnks} unique BNK files")
    
    for category, bnks in organization.items():
        print(f"\nProcessing category: {category} ({len(bnks)} BNK files)")
        
        for bnk_id, data in bnks.items():
            # Determine output path
            # Remove language from bnk_id if it's there (format: "language/id")
            clean_bnk_id = bnk_id.split('/')[-1] if '/' in bnk_id else bnk_id

            if data['language']:
                # Language-specific: Organized/Languages/english_us/Category/BNKid/
                # Sanitize language name for Windows paths
                safe_language = data['language'].replace('(', '_').replace(')', '_')
                output_path = OUTPUT_DIR / "Languages" / safe_language / category / clean_bnk_id
            else:
                # Non-language: Organized/Category/BNKid/
                output_path = OUTPUT_DIR / category / clean_bnk_id

            try:
                output_path.mkdir(parents=True, exist_ok=True)
            except OSError as e:
                print(f"  ERROR creating directory: {e}")
                print(f"    Attempted path: {output_path}")
                continue

            # Copy BNK file
            bnk_dest = output_path / f"{clean_bnk_id}.bnk"
            try:
                if not bnk_dest.exists() and data['bnk_path']:
                    shutil.copy2(str(data['bnk_path']), str(bnk_dest))
            except Exception as e:
                print(f"  Error copying BNK {bnk_id}: Path issue")
                print(f"    Source: {data['bnk_path']}")
                print(f"    Dest: {bnk_dest}")
                continue
            
            # Copy TXTP files
            for txtp_path in data['txtp_files']:
                try:
                    txtp_dest = output_path / txtp_path.name
                    if not txtp_dest.exists():
                        shutil.copy2(str(txtp_path), str(txtp_dest))
                except Exception as e:
                    print(f"  Error copying TXTP {txtp_path.name}: {e}")

            # Copy WEM files
            for wem_id in data['wem_files']:
                try:
                    wem_path = WEM_DIR / f"{wem_id}.wem"
                    if wem_path.exists():
                        wem_dest = output_path / f"{wem_id}.wem"
                        if not wem_dest.exists():
                            shutil.copy2(str(wem_path), str(wem_dest))
                except Exception as e:
                    print(f"  Error copying WEM {wem_id}: {e}")
            
            print(f"  {bnk_id}: {len(data['txtp_files'])} TXTPs, {len(data['wem_files'])} WEMs")

    print("\n[SUCCESS] Organization complete!")
    print(f"Output directory: {OUTPUT_DIR.absolute()}")

if __name__ == "__main__":
    organize_files()

