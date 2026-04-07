#!/usr/bin/env python3
"""
Verify BNK to XML Mapping Completeness

Cross-references:
1. All BNK files in Organized_Final_AllLanguages
2. Generated XML files
3. event_mapping.json (2,695 events across 84 banks)
4. TXTP files (~2,900 expected)

Usage: python verify_bnk_xml_mapping.py
"""

import json
import struct
from pathlib import Path
from collections import defaultdict

BASE_DIR = Path(__file__).parent
ORGANIZED_DIR = BASE_DIR / "Organized_Final_AllLanguages"
EVENT_MAPPING_JSON = BASE_DIR / "event_mapping.json"
TXTP_DIR = BASE_DIR / "txtp"

def parse_xml_events(xml_path: Path) -> list:
    """Extract event IDs from XML file (CAkEvent type=4 in HIRC)."""
    events = []
    try:
        content = xml_path.read_text(encoding='utf-8', errors='replace')
        # Look for CAkEvent objects with their IDs
        import re
        # Pattern: <obj na="CAkEvent"...><fld...na="ulID" va="NNNN"
        for match in re.finditer(r'<obj na="CAkEvent"[^>]*>.*?<fld[^>]*na="ulID"[^>]*va="(\d+)"', content, re.DOTALL):
            events.append(int(match.group(1)))
    except Exception as e:
        pass
    return events

def parse_bnk_stid(bnk_path: Path) -> dict:
    """Parse BNK to get bank name from STID chunk."""
    result = {'bank_id': None, 'bank_name': None, 'version': None}
    try:
        with open(bnk_path, 'rb') as f:
            while True:
                tag = f.read(4)
                if len(tag) < 4:
                    break
                size = struct.unpack('<I', f.read(4))[0]
                start = f.tell()
                
                if tag == b'BKHD':
                    result['version'] = struct.unpack('<I', f.read(4))[0]
                    result['bank_id'] = struct.unpack('<I', f.read(4))[0]
                elif tag == b'STID':
                    f.read(4)  # skip unknown
                    count = struct.unpack('<I', f.read(4))[0]
                    for _ in range(count):
                        sid = struct.unpack('<I', f.read(4))[0]
                        str_len = struct.unpack('B', f.read(1))[0]
                        name = f.read(str_len).decode('utf-8', errors='replace')
                        if sid == result['bank_id']:
                            result['bank_name'] = name
                
                f.seek(start + size)
    except:
        pass
    return result

def find_all_bnk_xml_pairs():
    """Find all BNK files and their XML counterparts."""
    pairs = []

    for bnk in ORGANIZED_DIR.rglob("*.bnk"):
        xml = Path(str(bnk) + '.xml')  # Append .xml to full path
        folder_name = bnk.parent.parent.name  # Bank folder name

        pairs.append({
            'bnk_path': bnk,
            'xml_path': xml,
            'xml_exists': xml.exists(),
            'folder_name': folder_name,
            'bank_id': bnk.stem,
        })

    return pairs

def main():
    print("=" * 70)
    print("BNK to XML Mapping Verification - LOTR Conquest")
    print("=" * 70)
    
    # 1. Find all BNK/XML pairs
    pairs = find_all_bnk_xml_pairs()
    print(f"\n[1] BNK Files Found: {len(pairs)}")
    
    xml_exists = sum(1 for p in pairs if p['xml_exists'])
    xml_missing = sum(1 for p in pairs if not p['xml_exists'])
    print(f"    XML exists: {xml_exists}")
    print(f"    XML missing: {xml_missing}")
    
    # 2. Load event_mapping.json
    print(f"\n[2] Event Mapping JSON")
    if EVENT_MAPPING_JSON.exists():
        with open(EVENT_MAPPING_JSON) as f:
            mapping = json.load(f)
        print(f"    Events: {mapping.get('event_count', 'N/A')}")
        print(f"    Banks: {len(mapping.get('banks', []))}")
    else:
        print("    NOT FOUND")
        mapping = {}
    
    # 3. Count TXTP files
    print(f"\n[3] TXTP Files")
    if TXTP_DIR.exists():
        txtp_count = len(list(TXTP_DIR.glob("*.txtp")))
        print(f"    Count: {txtp_count}")
    else:
        print("    Directory not found")
    
    # 4. Unique banks by folder name
    print(f"\n[4] Unique Banks by Folder")
    banks_by_folder = defaultdict(list)
    for p in pairs:
        banks_by_folder[p['folder_name']].append(p['bank_id'])
    
    print(f"    Unique bank names: {len(banks_by_folder)}")
    
    # 5. Cross-reference banks
    json_banks = set(mapping.get('banks', []))
    folder_banks = set(banks_by_folder.keys())
    
    print(f"\n[5] Bank Cross-Reference")
    print(f"    In JSON: {len(json_banks)}")
    print(f"    In Folders: {len(folder_banks)}")
    
    in_both = json_banks & folder_banks
    json_only = json_banks - folder_banks
    folder_only = folder_banks - json_banks
    
    print(f"    Match: {len(in_both)}")
    if json_only:
        print(f"    JSON only: {sorted(json_only)}")
    if folder_only:
        print(f"    Folder only: {sorted(folder_only)}")
    
    # 6. Summary table
    print(f"\n{'=' * 70}")
    print("[COMPLETE BANK MAPPING]")
    print(f"{'=' * 70}")
    print(f"{'Bank Name':<25} {'Bank ID':<12} {'XML':<5} {'Events':<8}")
    print("-" * 70)
    
    # Get unique root banks (check path parts, not string)
    root_banks = {}
    for p in pairs:
        path_parts = p['bnk_path'].parts
        # Check if 'root' is in path and the Languages folder is not (exact match)
        is_root = 'root' in path_parts and 'Languages' not in path_parts
        if is_root:
            info = parse_bnk_stid(p['bnk_path'])
            root_banks[p['folder_name']] = {
                'bank_id': p['bank_id'],
                'xml': 'Y' if p['xml_exists'] else 'N',
                'bank_name_stid': info.get('bank_name', ''),
            }
    
    for name in sorted(root_banks.keys()):
        b = root_banks[name]
        events = sum(1 for e in mapping.get('events', {}).values() 
                    if e.get('bank') == name)
        print(f"{name:<25} {b['bank_id']:<12} {b['xml']:<5} {events:<8}")
    
    print(f"\n{'=' * 70}")
    print(f"TOTAL: {len(root_banks)} unique banks, {xml_exists} XML files generated")
    print(f"{'=' * 70}")

if __name__ == '__main__':
    main()

