#!/usr/bin/env python3
"""
Create BNK mapping using canonical Wwise FNV-1 hash (AkFNVHash / GetIDFromString).
"""
import json
import csv
import os
import sys
from collections import defaultdict
from pathlib import Path

SCRIPT_ROOT = Path(__file__).resolve().parents[4]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash  # type: ignore

# Load data
print("Loading data...")
with open('bnk_analysis_intermediate.json', 'r') as f:
    bnk_data = json.load(f)

with open('WWiseIDTable.audio.json', 'r') as f:
    wwise_table = json.load(f)

# Build bank name mapping
bank_id_to_name = {}
with open('BANK_ID_TO_NAME_MAPPING.txt', 'r') as f:
    for line in f:
        if '|' in line and '0x' in line:
            parts = line.split('|')
            if len(parts) >= 2:
                bank_id_str = parts[0].strip()
                bank_name = parts[1].strip()
                try:
                    bank_id = int(bank_id_str, 16)
                    bank_id_to_name[bank_id] = bank_name
                except:
                    pass

# Build hash to event ID mapping
hash_to_event_id = {}
for entry in wwise_table['obj1s']:
    hash_key = entry['key']
    event_id = entry['val']
    hash_to_event_id[hash_key] = event_id

# Create output directory
os.makedirs('Analysis', exist_ok=True)

# Build mapping rows
print("Building mapping rows...")
rows = []
stats = {
    'total_banks': len(bnk_data),
    'total_rows': 0,
    'matched_hashes': 0,
    'unmatched_hashes': 0,
    'wwise_entries_used': 0
}

# Group BNKs by bank ID
bnks_by_id = defaultdict(list)
for bnk in bnk_data:
    bnks_by_id[bnk['bank_id']].append(bnk)

# First, add all bank-level mappings
for bank_id in sorted(bnks_by_id.keys()):
    bank_name = bank_id_to_name.get(bank_id, f"Unknown_0x{bank_id:08X}")
    bnks = bnks_by_id[bank_id]

    # Get STID names from first BNK with this ID
    stid_names = {}
    for bnk in bnks:
        for entry in bnk['stid']:
            stid_names[entry['id']] = entry['name']

    # For each STID name, try to find it in WWiseIDTable
    for stid_id, stid_name in stid_names.items():
        # Calculate FNV-1a hash of the name
        hash_val = fnv1_hash(stid_name)
        hash_key = f"0x{hash_val:08X}"

        # Look up in WWiseIDTable
        if hash_key in hash_to_event_id:
            event_id = hash_to_event_id[hash_key]
            matched = True
            stats['matched_hashes'] += 1
        else:
            event_id = stid_id
            matched = False
            stats['unmatched_hashes'] += 1

        row = {
            'BankName': bank_name,
            'BankID': f"0x{bank_id:08X}",
            'STID_ID': f"0x{stid_id:08X}",
            'EventName': stid_name,
            'CalculatedHash': hash_key,
            'EventID': event_id,
            'Matched': 'YES' if matched else 'NO',
            'FilePath': bnks[0]['filepath']
        }

        rows.append(row)
        stats['total_rows'] += 1

# Add all WWiseIDTable entries for reference
print("Adding WWiseIDTable entries...")
for entry in wwise_table['obj1s']:
    hash_key = entry['key']
    event_id = entry['val']

    row = {
        'BankName': 'WWiseIDTable',
        'BankID': 'N/A',
        'STID_ID': 'N/A',
        'EventName': f'Event_0x{event_id:08X}',
        'CalculatedHash': hash_key,
        'EventID': event_id,
        'Matched': 'WWISE_ENTRY',
        'FilePath': 'WWiseIDTable.audio.json'
    }

    rows.append(row)
    stats['wwise_entries_used'] += 1

# Write CSV
print(f"Writing {len(rows)} rows to CSV...")
csv_path = 'Analysis/bnk_full_mapping.csv'
with open(csv_path, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=[
        'BankName', 'BankID', 'STID_ID', 'EventName', 'CalculatedHash', 
        'EventID', 'Matched', 'FilePath'
    ])
    writer.writeheader()
    writer.writerows(rows)

print(f"✓ Saved to {csv_path}")

# Write summary
print("\nWriting summary report...")
summary_path = 'Analysis/mapping_summary.log'
with open(summary_path, 'w') as f:
    f.write("="*80 + "\n")
    f.write("BNK FULL MAPPING ANALYSIS SUMMARY\n")
    f.write("="*80 + "\n\n")
    
    f.write(f"Total BNK files processed: {stats['total_banks']}\n")
    f.write(f"Total BNK mapping rows: {stats['total_rows']}\n")
    f.write(f"Total WWiseIDTable entries: {stats['wwise_entries_used']}\n")
    f.write(f"Total CSV rows: {len(rows)}\n")
    f.write(f"Hashes matched with WWiseIDTable: {stats['matched_hashes']}\n")
    f.write(f"Hashes unmatched: {stats['unmatched_hashes']}\n")
    f.write(f"Match rate: {stats['matched_hashes']/max(1, stats['total_rows'])*100:.1f}%\n")
    f.write(f"\nOutput file: {csv_path}\n")
    f.write(f"\nMethod: FNV-1a hash matching of STID bank names + WWiseIDTable entries\n")

print(f"✓ Saved to {summary_path}")

# Print stats
print("\n" + "="*80)
print("SUMMARY")
print("="*80)
print(f"Total BNK files: {stats['total_banks']}")
print(f"Total BNK mapping rows: {stats['total_rows']}")
print(f"Total WWiseIDTable entries: {stats['wwise_entries_used']}")
print(f"Total CSV rows: {len(rows)}")
print(f"Matched: {stats['matched_hashes']}")
print(f"Unmatched: {stats['unmatched_hashes']}")
print(f"Match rate: {stats['matched_hashes']/max(1, stats['total_rows'])*100:.1f}%")
