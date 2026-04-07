#!/usr/bin/env python3
"""
Generate XML for all BNK files in Organized_Final_AllLanguages

Uses wwiser to parse each BNK and output XML representation.
Scans both root/ and Languages/ subdirectories.

Usage: python generate_all_xml.py [--force]
  --force: Regenerate XML even if it already exists
"""

import subprocess
import sys
from pathlib import Path
from collections import defaultdict

BASE_DIR = Path(__file__).parent
ORGANIZED_DIR = BASE_DIR / "Organized_Final_AllLanguages"
WWISER_PATH = BASE_DIR / "Tools" / "wwiser-20250928" / "wwiser.py"

def find_all_bnk_files():
    """Find all BNK files in the organized directory structure."""
    bnk_files = []
    
    # Scan root folder
    root_dir = ORGANIZED_DIR / "root"
    if root_dir.exists():
        for bank_folder in root_dir.iterdir():
            if bank_folder.is_dir():
                for sub in bank_folder.iterdir():
                    if sub.is_dir():
                        for bnk in sub.glob("*.bnk"):
                            bnk_files.append({
                                'path': bnk,
                                'category': 'root',
                                'bank_name': bank_folder.name,
                                'bank_id': sub.name,
                            })
    
    # Scan Languages folder
    lang_dir = ORGANIZED_DIR / "Languages"
    if lang_dir.exists():
        for lang_folder in lang_dir.iterdir():
            if lang_folder.is_dir():
                for bank_folder in lang_folder.iterdir():
                    if bank_folder.is_dir():
                        for sub in bank_folder.iterdir():
                            if sub.is_dir():
                                for bnk in sub.glob("*.bnk"):
                                    bnk_files.append({
                                        'path': bnk,
                                        'category': 'language',
                                        'language': lang_folder.name,
                                        'bank_name': bank_folder.name,
                                        'bank_id': sub.name,
                                    })
    
    return bnk_files

def generate_xml(bnk_path: Path, force: bool = False) -> tuple:
    """Generate XML for a single BNK file using wwiser."""
    xml_path = bnk_path.with_suffix('.bnk.xml')
    
    if xml_path.exists() and not force:
        return ('skipped', xml_path)
    
    try:
        # Run wwiser with xsl dump type
        result = subprocess.run(
            [sys.executable, str(WWISER_PATH), '-d', 'xsl', str(bnk_path)],
            capture_output=True,
            text=True,
            timeout=60,
            cwd=str(bnk_path.parent)
        )
        
        # Check if XML was created
        if xml_path.exists():
            return ('success', xml_path)
        else:
            return ('failed', result.stderr or result.stdout)
            
    except subprocess.TimeoutExpired:
        return ('timeout', None)
    except Exception as e:
        return ('error', str(e))

def main():
    force = '--force' in sys.argv
    
    print("=" * 70)
    print("BNK to XML Generator - LOTR Conquest")
    print("=" * 70)
    print(f"Organized directory: {ORGANIZED_DIR}")
    print(f"wwiser path: {WWISER_PATH}")
    print(f"Force regenerate: {force}")
    print()
    
    # Find all BNK files
    bnk_files = find_all_bnk_files()
    print(f"Found {len(bnk_files)} BNK files")
    
    # Group by category
    by_category = defaultdict(list)
    for bnk in bnk_files:
        by_category[bnk['category']].append(bnk)
    
    print(f"  - Root banks: {len(by_category['root'])}")
    print(f"  - Language banks: {len(by_category['language'])}")
    print()
    
    # Process each BNK
    stats = {'success': 0, 'skipped': 0, 'failed': 0, 'error': 0, 'timeout': 0}
    results = []
    
    for i, bnk in enumerate(bnk_files, 1):
        bnk_path = bnk['path']
        print(f"[{i}/{len(bnk_files)}] {bnk['bank_name']}/{bnk['bank_id']}: ", end='', flush=True)
        
        status, detail = generate_xml(bnk_path, force)
        stats[status] += 1
        results.append({**bnk, 'status': status, 'detail': str(detail)})
        
        if status == 'success':
            print("✓ XML generated")
        elif status == 'skipped':
            print("○ XML exists (skipped)")
        else:
            print(f"✗ {status}: {detail}")
    
    # Summary
    print()
    print("=" * 70)
    print("[SUMMARY]")
    print(f"  Success:  {stats['success']}")
    print(f"  Skipped:  {stats['skipped']} (already exist)")
    print(f"  Failed:   {stats['failed']}")
    print(f"  Errors:   {stats['error']}")
    print(f"  Timeouts: {stats['timeout']}")
    print(f"  Total:    {sum(stats.values())}")
    
    # Save results
    import json
    output_path = BASE_DIR / "xml_generation_results.json"
    with open(output_path, 'w') as f:
        export = {
            'stats': stats,
            'results': [{k: str(v) if isinstance(v, Path) else v for k, v in r.items()} for r in results]
        }
        json.dump(export, f, indent=2)
    print(f"\nResults saved to: {output_path}")

if __name__ == '__main__':
    main()

