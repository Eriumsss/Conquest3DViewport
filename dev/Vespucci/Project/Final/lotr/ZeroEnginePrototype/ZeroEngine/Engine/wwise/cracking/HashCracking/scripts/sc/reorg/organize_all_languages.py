#!/usr/bin/env python3
"""
Organize audio files for ALL languages by copying the English structure.
"""

from pathlib import Path
import shutil

# Paths
OUTPUT_DIR = Path("Organized_AllLanguages")
LANGUAGES = ["english(us)", "french(france)", "german", "italian", "russian", "spanish(spain)"]

def main():
    print("Organizing audio files for all languages...")
    
    # Use the existing Organized_v4 as template
    template_dir = Path("Organized_v4")
    if not template_dir.exists():
        print("ERROR: Organized_v4 template not found!")
        return
    
    # Create output directory
    OUTPUT_DIR.mkdir(exist_ok=True)
    
    # Copy non-language categories (at root level)
    print("\nCopying non-language categories...")
    for item in template_dir.iterdir():
        if item.name == "Languages":
            continue
        
        dest = OUTPUT_DIR / item.name
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(item, dest)
        print(f"  Copied: {item.name}")
    
    # Process each language
    print("\nProcessing languages...")
    languages_dir = OUTPUT_DIR / "Languages"
    languages_dir.mkdir(exist_ok=True)
    
    for lang in LANGUAGES:
        print(f"\n  Processing: {lang}")
        safe_lang = lang.replace('(', '_').replace(')', '_')
        lang_output = languages_dir / safe_lang
        
        # Copy the English structure as template
        english_template = template_dir / "Languages" / "english_us_"
        if not english_template.exists():
            print(f"    ERROR: English template not found!")
            continue
        
        # Copy structure
        if lang_output.exists():
            shutil.rmtree(lang_output)
        shutil.copytree(english_template, lang_output)
        
        # Now replace all BNK files with the correct language version
        extracted_lang = Path("extracted") / lang
        if not extracted_lang.exists():
            print(f"    WARNING: {lang} folder not found in extracted/")
            continue
        
        # Find all BNK files in the copied structure
        bnk_count = 0
        for bnk_file in lang_output.rglob("*.bnk"):
            bnk_id = bnk_file.stem
            source_bnk = extracted_lang / f"{bnk_id}.bnk"
            
            if source_bnk.exists():
                # Replace with language-specific BNK
                shutil.copy2(source_bnk, bnk_file)
                bnk_count += 1
            else:
                print(f"    WARNING: {bnk_id}.bnk not found in {lang}")
        
        print(f"    Replaced {bnk_count} BNK files with {lang} versions")
    
    print(f"\n[SUCCESS] All languages organized!")
    print(f"Output directory: {OUTPUT_DIR.absolute()}")

if __name__ == "__main__":
    main()

