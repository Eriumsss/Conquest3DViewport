#!/usr/bin/env python3
"""
BNK to XML Mapping Audit Script for LOTR Conquest

Audits all BNK files in extracted/ directory and checks for:
1. Existing XML mappings
2. Bank names from STID chunk
3. Event counts from HIRC chunk (type=4 CAkEvent objects)
4. Cross-reference with event_mapping.json

Usage: python audit_bnk_xml.py
"""

import os
import struct
import json
from pathlib import Path
from collections import defaultdict

EXTRACTED_DIR = Path(__file__).parent / "extracted"
EVENT_MAPPING_JSON = Path(__file__).parent / "event_mapping.json"

def read_bnk_header(bnk_path: Path) -> dict:
    """Read BNK header to extract bank ID, version, and bank name from STID."""
    result = {
        'bank_id': None,
        'version': None,
        'bank_name': None,
        'event_count': 0,
        'event_ids': [],
        'chunks': [],
        'size': 0,
    }
    
    try:
        result['size'] = bnk_path.stat().st_size
        with open(bnk_path, 'rb') as f:
            # Parse chunks
            while True:
                chunk_tag = f.read(4)
                if len(chunk_tag) < 4:
                    break
                    
                chunk_size = struct.unpack('<I', f.read(4))[0]
                chunk_start = f.tell()
                tag_str = chunk_tag.decode('ascii', errors='replace')
                result['chunks'].append((tag_str, chunk_size))
                
                if chunk_tag == b'BKHD':
                    # Bank Header: version (4), bank_id (4), language_id (4), feedback (4)
                    version = struct.unpack('<I', f.read(4))[0]
                    bank_id = struct.unpack('<I', f.read(4))[0]
                    result['version'] = version
                    result['bank_id'] = bank_id
                    
                elif chunk_tag == b'STID':
                    # String Table ID - bank names
                    # u32: unk, u32: count
                    f.read(4)  # skip unknown
                    count = struct.unpack('<I', f.read(4))[0]
                    for _ in range(count):
                        sid = struct.unpack('<I', f.read(4))[0]
                        str_len = struct.unpack('B', f.read(1))[0]
                        name = f.read(str_len).decode('utf-8', errors='replace')
                        if sid == result['bank_id']:
                            result['bank_name'] = name
                            
                elif chunk_tag == b'HIRC':
                    # Hierarchy - count event objects (type=4)
                    num_items = struct.unpack('<I', f.read(4))[0]
                    for _ in range(num_items):
                        hirc_type = struct.unpack('B', f.read(1))[0]
                        section_size = struct.unpack('<I', f.read(4))[0]
                        item_id = struct.unpack('<I', f.read(4))[0]
                        
                        if hirc_type == 4:  # CAkEvent
                            result['event_count'] += 1
                            result['event_ids'].append(item_id)
                        
                        # Skip rest of item data
                        f.seek(f.tell() + section_size - 4)
                
                # Seek to next chunk
                f.seek(chunk_start + chunk_size)
                
    except Exception as e:
        result['error'] = str(e)
    
    return result

def audit_bnk_files():
    """Audit all BNK files and their XML mappings."""
    results = {
        'root_bnks': [],
        'language_bnks': defaultdict(list),
        'xml_exists': [],
        'xml_missing': [],
        'bank_names': {},
        'summary': {}
    }
    
    # Find all BNK files
    for item in EXTRACTED_DIR.iterdir():
        if item.is_file() and item.suffix == '.bnk':
            bnk_info = read_bnk_header(item)
            bnk_info['path'] = str(item.relative_to(EXTRACTED_DIR))
            bnk_info['xml_path'] = str(item) + '.xml'
            bnk_info['has_xml'] = Path(bnk_info['xml_path']).exists()
            results['root_bnks'].append(bnk_info)
            
            if bnk_info['bank_name']:
                results['bank_names'][bnk_info['bank_id']] = bnk_info['bank_name']
            
            if bnk_info['has_xml']:
                results['xml_exists'].append(bnk_info['path'])
            else:
                results['xml_missing'].append(bnk_info['path'])
                
        elif item.is_dir():
            # Language folder
            lang = item.name
            for bnk in item.glob('*.bnk'):
                bnk_info = read_bnk_header(bnk)
                bnk_info['path'] = str(bnk.relative_to(EXTRACTED_DIR))
                results['language_bnks'][lang].append(bnk_info)
    
    # Summary
    results['summary'] = {
        'total_root_bnks': len(results['root_bnks']),
        'total_language_folders': len(results['language_bnks']),
        'total_language_bnks': sum(len(v) for v in results['language_bnks'].values()),
        'xml_exists_count': len(results['xml_exists']),
        'xml_missing_count': len(results['xml_missing']),
        'unique_bank_names': len(results['bank_names']),
    }
    
    return results

def main():
    print("=" * 70)
    print("BNK to XML Mapping Audit - LOTR Conquest")
    print("=" * 70)
    
    results = audit_bnk_files()
    
    # Print summary
    s = results['summary']
    print(f"\n[SUMMARY]")
    print(f"  Root BNK files:     {s['total_root_bnks']}")
    print(f"  Language folders:   {s['total_language_folders']}")
    print(f"  Language BNK files: {s['total_language_bnks']} (per language: ~{s['total_language_bnks']//max(1,s['total_language_folders'])})")
    print(f"  XML files exist:    {s['xml_exists_count']}")
    print(f"  XML files missing:  {s['xml_missing_count']}")
    print(f"  Unique bank names:  {s['unique_bank_names']}")
    
    # Print root BNKs with details
    print(f"\n[ROOT BNK FILES] ({len(results['root_bnks'])})")
    print("-" * 70)
    print(f"{'Bank ID':<12} {'Bank Name':<20} {'Events':<8} {'XML':<6} {'Size'}")
    print("-" * 70)
    
    for bnk in sorted(results['root_bnks'], key=lambda x: (x.get('bank_name') or '') or str(x.get('bank_id') or '')):
        bank_id = bnk.get('bank_id') or 'N/A'
        bank_name = (bnk.get('bank_name') or '(unnamed)')[:20]
        events = bnk.get('event_count') or 0
        has_xml = 'YES' if bnk.get('has_xml') else 'NO'
        size = bnk.get('size') or 0
        print(f"{bank_id:<12} {bank_name:<20} {events:<8} {has_xml:<6} {size:,}")
    
    # Print XML missing list
    if results['xml_missing']:
        print(f"\n[XML MISSING] ({len(results['xml_missing'])} files)")
        for path in results['xml_missing']:
            print(f"  - {path}")
    
    # Save full results to JSON
    output_path = Path(__file__).parent / "bnk_audit_results.json"
    with open(output_path, 'w') as f:
        # Convert to serializable format
        export = {
            'summary': results['summary'],
            'root_bnks': [{k: v for k, v in b.items() if k != 'event_ids'} for b in results['root_bnks']],
            'xml_exists': results['xml_exists'],
            'xml_missing': results['xml_missing'],
            'bank_names': {str(k): v for k, v in results['bank_names'].items()},
            'language_folders': list(results['language_bnks'].keys()),
        }
        json.dump(export, f, indent=2)
    print(f"\n[OUTPUT] Full results saved to: {output_path}")

if __name__ == '__main__':
    main()

