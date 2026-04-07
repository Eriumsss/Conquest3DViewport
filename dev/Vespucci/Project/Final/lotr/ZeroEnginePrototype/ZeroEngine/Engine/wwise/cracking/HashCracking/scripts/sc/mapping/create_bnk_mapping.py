#!/usr/bin/env python3
"""
Create unified BNK mapping CSV by combining STID, HIRC, DIDX, and WWiseIDTable data
"""
import json
import csv
import os
from collections import defaultdict

# Load intermediate BNK analysis
print("Loading BNK analysis data...")
with open('bnk_analysis_intermediate.json', 'r') as f:
    bnk_data = json.load(f)

# Load WWiseIDTable
print("Loading WWiseIDTable...")
with open('WWiseIDTable.audio.json', 'r') as f:
    wwise_table = json.load(f)

# Create mappings
bank_id_to_name = {}
event_id_to_bank = defaultdict(list)
hash_to_event_id = {}

# Load bank names from our previous extraction
print("Loading bank names...")
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

# Build hash to event ID mapping from WWiseIDTable
print("Building hash to event ID mapping...")
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
    'total_events': 0,
    'matched_events': 0,
    'unmatched_events': 0,
    'banks_with_events': 0
}

for bnk in bnk_data:
    bank_id = bnk['bank_id']
    bank_name = bank_id_to_name.get(bank_id, f"Unknown_0x{bank_id:08X}")
    filepath = bnk['filepath']
    
    # Get STID names
    stid_names = {entry['id']: entry['name'] for entry in bnk['stid']}
    
    # Get DIDX mappings
    didx_map = bnk['didx']
    
    # Get HIRC objects
    hirc_objects = bnk['hirc']
    
    if hirc_objects or didx_map:
        stats['banks_with_events'] += 1
    
    # Create rows for each HIRC object
    for obj in hirc_objects:
        obj_id = obj['id']
        obj_type = obj['type']
        
        # Try to find matching event in WWiseIDTable
        hash_key = f"0x{obj_id:08X}"
        event_id = hash_to_event_id.get(hash_key, obj_id)
        
        # Get WEM info if available
        wem_offset = ""
        wem_size = ""
        if obj_id in didx_map:
            wem_offset = f"0x{didx_map[obj_id]['offset']:08X}"
            wem_size = f"0x{didx_map[obj_id]['size']:08X}"
        
        # Get event name from STID if available
        event_name = stid_names.get(obj_id, f"Event_0x{obj_id:08X}")
        
        row = {
            'BankName': bank_name,
            'BankID': f"0x{bank_id:08X}",
            'EventID': event_id,
            'HashKey': hash_key,
            'EventName': event_name,
            'ObjectType': obj_type,
            'WEM_Offset': wem_offset,
            'WEM_Size': wem_size,
            'FilePath': filepath
        }
        
        rows.append(row)
        stats['total_events'] += 1
        
        if event_id != obj_id:
            stats['matched_events'] += 1
        else:
            stats['unmatched_events'] += 1

# Write CSV
print(f"Writing {len(rows)} rows to CSV...")
csv_path = 'Analysis/bnk_full_mapping.csv'
with open(csv_path, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=[
        'BankName', 'BankID', 'EventID', 'HashKey', 'EventName', 
        'ObjectType', 'WEM_Offset', 'WEM_Size', 'FilePath'
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
    f.write(f"Banks with events: {stats['banks_with_events']}\n")
    f.write(f"Total events found: {stats['total_events']}\n")
    f.write(f"Events matched with WWiseIDTable: {stats['matched_events']}\n")
    f.write(f"Events unmatched: {stats['unmatched_events']}\n")
    f.write(f"Match rate: {stats['matched_events']/max(1, stats['total_events'])*100:.1f}%\n")
    f.write(f"\nOutput file: {csv_path}\n")

print(f"✓ Saved to {summary_path}")

# Print stats
print("\n" + "="*80)
print("SUMMARY")
print("="*80)
print(f"Total BNK files: {stats['total_banks']}")
print(f"Banks with events: {stats['banks_with_events']}")
print(f"Total events: {stats['total_events']}")
print(f"Matched: {stats['matched_events']}")
print(f"Unmatched: {stats['unmatched_events']}")
print(f"Match rate: {stats['matched_events']/max(1, stats['total_events'])*100:.1f}%")

