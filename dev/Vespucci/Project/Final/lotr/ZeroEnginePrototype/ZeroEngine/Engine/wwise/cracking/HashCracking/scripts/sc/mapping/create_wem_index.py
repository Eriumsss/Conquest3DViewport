#!/usr/bin/env python3
"""
WEM Index Creator
Creates comprehensive WEM_INDEX.csv linking WEMs to events and output files
"""
import os
import csv
import sys
from pathlib import Path
from collections import defaultdict
import wave

CSV_PATH = "Audio/Analysis/event_wem_mapping.csv"
WEM_DIR = "Audio/Extracted_WEMs"
WAV_DIR = "Audio/Extracted_WAVs"
REPORT_DIR = "Audio/Reports"

os.makedirs(REPORT_DIR, exist_ok=True)

print("="*100)
print("WEM INDEX CREATOR")
print("="*100)

# Check files exist
if not os.path.exists(CSV_PATH):
    print(f"[ERROR] {CSV_PATH} not found")
    sys.exit(1)

# Load extracted WEMs
print(f"\n[1] Loading extracted WEMs...")
extracted_wems = set()
if os.path.exists(WEM_DIR):
    extracted_wems = set(f.replace('.wem', '') for f in os.listdir(WEM_DIR) if f.endswith('.wem'))
print(f"  Found {len(extracted_wems):,} extracted WEM files")

# Load converted WAVs
print(f"\n[2] Loading converted WAVs...")
converted_wavs = set()
wav_durations = {}
if os.path.exists(WAV_DIR):
    for f in os.listdir(WAV_DIR):
        if f.endswith('.wav'):
            wav_id = f.replace('.wav', '')
            converted_wavs.add(wav_id)
            
            # Get duration
            try:
                wav_path = os.path.join(WAV_DIR, f)
                with wave.open(wav_path, 'rb') as wav_file:
                    frames = wav_file.getnframes()
                    rate = wav_file.getframerate()
                    duration = frames / rate
                    wav_durations[wav_id] = duration
            except:
                wav_durations[wav_id] = 0

print(f"  Found {len(converted_wavs):,} converted WAV files")

# Read CSV and create index
print(f"\n[3] Creating WEM index...")
index_rows = []
wem_to_events = defaultdict(list)

with open(CSV_PATH, 'r', encoding='utf-8') as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            wem_id = row['WEM_ID']
            bank_name = row['BankName']
            event_id = row['EventID']
            event_name = row.get('EventName', '')
            data_base = row['DATA_Base']
            offset = row['WEM_Offset']
            size = row['WEM_Size']
            
            # Track unique WEM entries
            if wem_id not in wem_to_events:
                extracted_file = f"{wem_id}.wem" if wem_id in extracted_wems else ""
                converted_file = f"{wem_id}.wav" if wem_id in converted_wavs else ""
                duration = wav_durations.get(wem_id, 0)
                
                status = "OK"
                if not extracted_file:
                    status = "NOT_EXTRACTED"
                elif not converted_file:
                    status = "NOT_CONVERTED"
                
                index_rows.append({
                    'WEM_ID': wem_id,
                    'BankName': bank_name,
                    'EventID': event_id,
                    'EventName': event_name,
                    'InputOffset': data_base + offset,
                    'InputSize': size,
                    'ExtractedFile': extracted_file,
                    'ConvertedFile': converted_file,
                    'Duration': f"{duration:.2f}s",
                    'Status': status
                })
                
                wem_to_events[wem_id].append((bank_name, event_id))
        except Exception as e:
            print(f"  [WARN] CSV parse error: {e}")

print(f"  Created {len(index_rows):,} index entries")

# Write WEM_INDEX.csv
index_file = os.path.join(REPORT_DIR, "WEM_INDEX.csv")
with open(index_file, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=['WEM_ID', 'BankName', 'EventID', 'EventName', 'InputOffset', 'InputSize', 'ExtractedFile', 'ConvertedFile', 'Duration', 'Status'])
    writer.writeheader()
    writer.writerows(index_rows)

print(f"  ✅ WEM_INDEX.csv: {index_file}")

# Calculate statistics
print(f"\n[4] Statistics")
extracted_count = sum(1 for r in index_rows if r['ExtractedFile'])
converted_count = sum(1 for r in index_rows if r['ConvertedFile'])
total_duration = sum(wav_durations.values())

print(f"  Total unique WEMs: {len(index_rows):,}")
print(f"  Extracted: {extracted_count:,} ({extracted_count/len(index_rows)*100:.1f}%)")
print(f"  Converted: {converted_count:,} ({converted_count/len(index_rows)*100:.1f}%)")
print(f"  Total audio duration: {total_duration/3600:.1f} hours")

# Write final report
report_file = os.path.join(REPORT_DIR, "EXTRACTION_REPORT.txt")
with open(report_file, 'w') as f:
    f.write("="*100 + "\n")
    f.write("WEM EXTRACTION AND CONVERSION REPORT\n")
    f.write("="*100 + "\n\n")
    f.write(f"Total unique WEM IDs: {len(index_rows):,}\n")
    f.write(f"Extracted: {extracted_count:,} ({extracted_count/len(index_rows)*100:.1f}%)\n")
    f.write(f"Converted to WAV: {converted_count:,} ({converted_count/len(index_rows)*100:.1f}%)\n")
    f.write(f"Total audio duration: {total_duration/3600:.1f} hours\n")
    f.write(f"Average WEM size: {sum(int(r['InputSize'], 16) for r in index_rows)/len(index_rows):,.0f} bytes\n\n")
    f.write("="*100 + "\n")
    f.write("OUTPUT FILES\n")
    f.write("="*100 + "\n")
    f.write(f"Extracted WEMs: {WEM_DIR}\n")
    f.write(f"Converted WAVs: {WAV_DIR}\n")
    f.write(f"Index file: {index_file}\n")
    f.write(f"Report file: {report_file}\n")

print(f"\n✅ Final report: {report_file}")
print(f"✅ All outputs in: {REPORT_DIR}")

