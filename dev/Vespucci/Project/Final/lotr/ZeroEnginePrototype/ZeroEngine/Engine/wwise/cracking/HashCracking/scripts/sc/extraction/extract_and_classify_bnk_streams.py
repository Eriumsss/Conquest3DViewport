#!/usr/bin/env python3
"""
BNK Stream Extraction and Classification Tool
Extracts all WEM streams from BNK files using vgmstream-cli
Classifies by type and organizes into category folders
"""

import os
import subprocess
import glob
import csv
import re
from pathlib import Path
from datetime import datetime

# Configuration
VGMSTREAM_CLI = r"Audio/vgmstream-win64/vgmstream-cli.exe"
BNK_INPUT_DIR = r"Audio/extracted_bnk"
OUTPUT_BASE_DIR = r"Audio/Sorted_Output"
LOG_FILE = os.path.join(OUTPUT_BASE_DIR, "extraction_log.txt")
CSV_FILE = os.path.join(OUTPUT_BASE_DIR, "category_summary.csv")
ERROR_LOG = os.path.join(OUTPUT_BASE_DIR, "errors.log")

# Classification patterns
CATEGORY_PATTERNS = {
    'Level': r'(level|map|cori|ankh|mount|moria|rivendell)',
    'Hero': r'(hero|player|gandalf|aragorn|gimli|legolas)',
    'Creature': r'(creature|orc|goblin|troll|dragon|balrog|human)',
    'Environment': r'(ambient|env|environment|wind|water|fire|nature)',
    'UI': r'(ui|menu|button|select|confirm|cancel|interface)',
    'SFX': r'(sfx|effect|sound|impact|hit|attack|footstep)',
    'Music': r'(music|bgm|theme|score|ambient_music)',
}

def classify_bnk(filename):
    """Classify BNK file based on filename patterns"""
    filename_lower = filename.lower()
    
    for category, pattern in CATEGORY_PATTERNS.items():
        if re.search(pattern, filename_lower):
            return category
    
    return 'Misc'

def verify_riff_header(filepath):
    """Verify file has valid RIFF header"""
    try:
        with open(filepath, 'rb') as f:
            header = f.read(4)
            return header == b'RIFF'
    except:
        return False

def extract_streams_from_bnk(bnk_path, output_dir):
    """Extract all streams from a BNK file using vgmstream-cli"""
    streams = []
    stream_idx = 0
    
    while True:
        output_file = os.path.join(output_dir, f"stream_{stream_idx:03d}.wav")
        
        try:
            result = subprocess.run(
                [VGMSTREAM_CLI, bnk_path, "-s", str(stream_idx), "-o", output_file],
                capture_output=True,
                timeout=30,
                text=True
            )
            
            if result.returncode != 0:
                break
            
            if os.path.exists(output_file) and verify_riff_header(output_file):
                file_size = os.path.getsize(output_file)
                streams.append({
                    'index': stream_idx,
                    'filename': f"stream_{stream_idx:03d}.wav",
                    'size': file_size,
                    'valid': True
                })
            else:
                if os.path.exists(output_file):
                    os.remove(output_file)
                break
            
            stream_idx += 1
            
        except subprocess.TimeoutExpired:
            break
        except Exception as e:
            break
    
    return streams

def main():
    print("=" * 80)
    print("BNK Stream Extraction and Classification Tool")
    print("=" * 80)
    print(f"Start time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print()
    
    # Create output directory
    os.makedirs(OUTPUT_BASE_DIR, exist_ok=True)
    
    # Verify vgmstream-cli exists
    if not os.path.exists(VGMSTREAM_CLI):
        print(f"ERROR: vgmstream-cli not found at {VGMSTREAM_CLI}")
        return
    
    print(f"✓ vgmstream-cli found: {VGMSTREAM_CLI}")
    print(f"✓ Input directory: {BNK_INPUT_DIR}")
    print(f"✓ Output directory: {OUTPUT_BASE_DIR}")
    print()
    
    # Get all BNK files
    bnk_files = sorted(glob.glob(os.path.join(BNK_INPUT_DIR, "*.bnk")))
    print(f"Found {len(bnk_files)} BNK files to process")
    print()
    
    # Initialize logs
    extraction_log = []
    csv_data = []
    error_log = []
    
    total_streams = 0
    successful_bnks = 0
    failed_bnks = 0
    
    # Process each BNK
    for idx, bnk_path in enumerate(bnk_files, 1):
        bnk_name = os.path.basename(bnk_path)
        category = classify_bnk(bnk_name)
        
        print(f"[{idx:3d}/{len(bnk_files)}] Processing: {bnk_name} ({category})")
        
        # Create category and BNK-specific output directory
        category_dir = os.path.join(OUTPUT_BASE_DIR, category, bnk_name.replace('.bnk', ''))
        os.makedirs(category_dir, exist_ok=True)
        
        try:
            # Extract streams
            streams = extract_streams_from_bnk(bnk_path, category_dir)
            
            if streams:
                successful_bnks += 1
                total_streams += len(streams)
                status = "OK"
                
                log_entry = f"[OK] {bnk_name} → {len(streams)} streams extracted to {category}/{bnk_name.replace('.bnk', '')}"
                extraction_log.append(log_entry)
                print(f"      ✓ {len(streams)} streams extracted")
                
            else:
                failed_bnks += 1
                status = "FAILED"
                log_entry = f"[FAIL] {bnk_name} → No streams extracted"
                extraction_log.append(log_entry)
                error_log.append(log_entry)
                print(f"      ✗ No streams extracted")
            
            # Add to CSV
            csv_data.append({
                'BNK_Name': bnk_name,
                'Category': category,
                'StreamCount': len(streams),
                'Status': status
            })
            
        except Exception as e:
            failed_bnks += 1
            error_msg = f"[ERROR] {bnk_name} → {str(e)}"
            extraction_log.append(error_msg)
            error_log.append(error_msg)
            print(f"      ✗ Error: {str(e)}")
            
            csv_data.append({
                'BNK_Name': bnk_name,
                'Category': category,
                'StreamCount': 0,
                'Status': 'ERROR'
            })
    
    print()
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"Total BNKs processed: {len(bnk_files)}")
    print(f"Successful: {successful_bnks}")
    print(f"Failed: {failed_bnks}")
    print(f"Total streams extracted: {total_streams}")
    success_rate = (successful_bnks / len(bnk_files) * 100) if bnk_files else 0
    print(f"Success rate: {success_rate:.1f}%")
    print()
    
    # Write extraction log
    with open(LOG_FILE, 'w', encoding='utf-8') as f:
        f.write("BNK Stream Extraction Log\n")
        f.write(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write("=" * 80 + "\n\n")
        for entry in extraction_log:
            f.write(entry + "\n")
        f.write("\n" + "=" * 80 + "\n")
        f.write(f"SUMMARY: {len(bnk_files)} BNKs | {total_streams} streams | {success_rate:.1f}% success\n")
    
    # Write CSV summary
    with open(CSV_FILE, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['BNK_Name', 'Category', 'StreamCount', 'Status'])
        writer.writeheader()
        writer.writerows(csv_data)
    
    # Write error log if there are errors
    if error_log:
        with open(ERROR_LOG, 'w', encoding='utf-8') as f:
            f.write("Extraction Errors\n")
            f.write(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write("=" * 80 + "\n\n")
            for entry in error_log:
                f.write(entry + "\n")
    
    print(f"✓ Extraction log: {LOG_FILE}")
    print(f"✓ CSV summary: {CSV_FILE}")
    if error_log:
        print(f"✓ Error log: {ERROR_LOG}")
    print()
    print(f"End time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 80)

if __name__ == "__main__":
    main()

