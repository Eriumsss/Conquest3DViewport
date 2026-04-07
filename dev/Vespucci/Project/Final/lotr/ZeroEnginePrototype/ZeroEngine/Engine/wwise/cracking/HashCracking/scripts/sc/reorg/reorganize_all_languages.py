#!/usr/bin/env python3
"""
Reorganize audio files for ALL languages into a structured format:
- root/ for non-language content (music, SFX, ambience, levels)
- Languages/ for language-specific content (chatter, voice-overs) for ALL 6 languages
"""

import re
import shutil
from pathlib import Path
from collections import defaultdict

# Paths
SOURCE_DIR = Path("Organized")
EXTRACTED_DIR = SOURCE_DIR / "extracted"
OUTPUT_DIR = Path("Organized_Final_AllLanguages")

# Language mappings: folder name -> TXTP folder
LANGUAGE_MAPPINGS = {
    "english(us)": "txtp",
    "french(france)": "txtp_french",
    "german": "txtp_german",
    "italian": "txtp_italian",
    "russian": "txtp_russian",
    "spanish(spain)": "txtp_spanish"
}

def sanitize_language_name(lang):
    """Convert language name to safe folder name."""
    return lang.replace('(', '_').replace(')', '_')

def parse_txtp_file(txtp_path):
    """Parse a TXTP file to extract WEM files (BNK info not in new TXTPs)."""
    wem_files = []
    
    try:
        with open(txtp_path, 'r', encoding='utf-8') as f:
            for line in f:
                # Extract WEM files: wem/363070948.ogg or wem/363070948.wav
                if line.strip().startswith("wem/"):
                    wem_match = re.search(r'wem/(\d+)\.(ogg|wav|wem)', line)
                    if wem_match:
                        wem_id = wem_match.group(1)
                        wem_files.append(wem_id)
    except Exception as e:
        print(f"Error parsing {txtp_path}: {e}")
        return []
    
    return wem_files

def get_category_from_filename(filename):
    """Extract category from TXTP filename."""
    match = re.match(r'^([A-Za-z_]+)', filename)
    if match:
        return match.group(1)
    return "Uncategorized"

def find_bnk_for_category_and_language(category, language):
    """Find BNK files for a specific category and language by matching TXTP filenames."""
    # For language-specific categories, look in language folder
    if language:
        lang_dir = EXTRACTED_DIR / language
        if lang_dir.exists():
            bnk_files = list(lang_dir.glob("*.bnk"))
            return bnk_files
    else:
        # For root categories, look in root extracted folder
        bnk_files = list(EXTRACTED_DIR.glob("*.bnk"))
        return bnk_files
    
    return []

def is_language_specific_category(category):
    """Determine if a category is language-specific based on name."""
    language_prefixes = ['Chatter', 'VO_']
    return any(category.startswith(prefix) for prefix in language_prefixes)

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
    print("Processing all languages...")
    
    # First, process English (which has BNK references in TXTPs)
    print("\n=== Processing English (with BNK references) ===")
    process_english()
    
    # Then, process other languages (copy structure from English)
    print("\n=== Processing other languages ===")
    for language, txtp_dir_name in LANGUAGE_MAPPINGS.items():
        if language == "english(us)":
            continue  # Already processed
        
        print(f"\nProcessing {language}...")
        process_other_language(language, txtp_dir_name)
    
    print(f"\n[SUCCESS] Organization complete!")
    print(f"Output directory: {OUTPUT_DIR.absolute()}")

def process_english():
    """Process English with full BNK reference parsing."""
    TXTP_DIR = Path("Organized/txtp")
    
    organization = defaultdict(lambda: defaultdict(lambda: {
        'txtp_files': [],
        'wem_files': set(),
        'language': None,
        'bnk_path': None
    }))
    
    txtp_files = list(TXTP_DIR.glob("*.txtp"))
    print(f"Found {len(txtp_files)} English TXTP files")
    
    for txtp_path in txtp_files:
        # Parse for BNK reference (old format)
        bnk_file = None
        wem_files = []
        
        try:
            with open(txtp_path, 'r', encoding='utf-8') as f:
                for line in f:
                    if line.startswith("# - extracted/"):
                        bnk_match = re.search(r'extracted/(.+?)\.bnk', line)
                        if bnk_match:
                            bnk_file = bnk_match.group(1)
                    
                    if line.strip().startswith("wem/"):
                        wem_match = re.search(r'wem/(\d+)\.(ogg|wav|wem)', line)
                        if wem_match:
                            wem_files.append(wem_match.group(1))
        except:
            continue
        
        if not bnk_file:
            continue
        
        category = get_category_from_filename(txtp_path.name)
        clean_bnk_id = bnk_file.split('/')[-1] if '/' in bnk_file else bnk_file
        
        # Find BNK file
        bnk_path = None
        language = None
        
        for lang in LANGUAGE_MAPPINGS.keys():
            lang_bnk_path = EXTRACTED_DIR / lang / f"{clean_bnk_id}.bnk"
            if lang_bnk_path.exists():
                bnk_path = lang_bnk_path
                language = lang
                break
        
        if not bnk_path:
            root_bnk_path = EXTRACTED_DIR / f"{clean_bnk_id}.bnk"
            if root_bnk_path.exists():
                bnk_path = root_bnk_path
        
        if not bnk_path:
            continue
        
        org_data = organization[category][clean_bnk_id]
        org_data['txtp_files'].append(txtp_path)
        org_data['wem_files'].update(wem_files)
        org_data['language'] = language
        org_data['bnk_path'] = bnk_path
    
    # Create structure
    for category, bnks in organization.items():
        for bnk_id, data in bnks.items():
            if data['language']:
                safe_language = sanitize_language_name(data['language'])
                output_path = OUTPUT_DIR / "Languages" / safe_language / category / bnk_id
            else:
                output_path = OUTPUT_DIR / "root" / category / bnk_id
            
            output_path.mkdir(parents=True, exist_ok=True)
            
            # Copy files
            shutil.copy2(data['bnk_path'], output_path / data['bnk_path'].name)
            
            for txtp_path in data['txtp_files']:
                shutil.copy2(txtp_path, output_path / txtp_path.name)
            
            for wem_id in data['wem_files']:
                wem_path = find_wem_file(wem_id, data['language'])
                if wem_path:
                    shutil.copy2(wem_path, output_path / wem_path.name)

def process_other_language(language, txtp_dir_name):
    """Process other languages by copying English structure."""
    TXTP_DIR = Path(txtp_dir_name)
    
    if not TXTP_DIR.exists():
        print(f"  Warning: {txtp_dir_name} not found")
        return
    
    # Get English structure as template
    english_dir = OUTPUT_DIR / "Languages" / "english_us_"
    if not english_dir.exists():
        print(f"  Warning: English structure not found")
        return
    
    safe_language = sanitize_language_name(language)
    lang_output_dir = OUTPUT_DIR / "Languages" / safe_language
    
    # Copy structure from English
    for category_dir in english_dir.iterdir():
        if not category_dir.is_dir():
            continue
        
        category = category_dir.name
        
        for bnk_dir in category_dir.iterdir():
            if not bnk_dir.is_dir():
                continue
            
            bnk_id = bnk_dir.name
            output_path = lang_output_dir / category / bnk_id
            output_path.mkdir(parents=True, exist_ok=True)
            
            # Copy BNK file
            bnk_source = EXTRACTED_DIR / language / f"{bnk_id}.bnk"
            if bnk_source.exists():
                shutil.copy2(bnk_source, output_path / bnk_source.name)
            
            # Copy TXTP files (matching category)
            for txtp_file in TXTP_DIR.glob(f"{category}-*.txtp"):
                shutil.copy2(txtp_file, output_path / txtp_file.name)
            
            # Copy WEM files
            for wem_file in (EXTRACTED_DIR / language).glob("*.wem"):
                # Check if this WEM is referenced in English version
                if (bnk_dir / wem_file.name).exists():
                    shutil.copy2(wem_file, output_path / wem_file.name)
    
    print(f"  {language}: Structure copied")

if __name__ == "__main__":
    main()

