#!/usr/bin/env python3
"""
Comprehensive Tri-Output BNK Extractor
Extracts BNK, WEM, and WAV files with full byte coverage accounting
Uses pre-extracted BNKs from Audio/extracted_bnk/
"""

import os
import sys
import struct
import csv
import hashlib
import logging
import subprocess
import tempfile
import re
from pathlib import Path
from collections import defaultdict
from datetime import datetime

# Configuration
VGMSTREAM_CLI = "Audio/vgmstream-win64/vgmstream-cli.exe"
MIN_WAV_DURATION = 0.05  # seconds
RIFF_HEADER = b'RIFF'
BKHD_SIGNATURE = b'BKHD'
DIDX_SIGNATURE = b'DIDX'
DATA_SIGNATURE = b'DATA'
STID_SIGNATURE = b'STID'

# Input/Output directories
INPUT_BNK_DIR = "Audio/extracted_bnk"
OUTPUT_BASE = "Audio"
RAW_BNK_DIR = os.path.join(OUTPUT_BASE, "Raw_BNK")
RAW_WEM_DIR = os.path.join(OUTPUT_BASE, "Raw_WEM")
DECODED_WAV_DIR = os.path.join(OUTPUT_BASE, "Decoded_WAVs")
UNKNOWN_BLOCKS_DIR = os.path.join(OUTPUT_BASE, "Unknown_Blocks")
REPORTS_DIR = os.path.join(OUTPUT_BASE, "Reports")
LOGS_DIR = os.path.join(OUTPUT_BASE, "Logs")

# Setup logging
log_file = os.path.join(LOGS_DIR, "tri_extraction.log")
os.makedirs(LOGS_DIR, exist_ok=True)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler(log_file, encoding='utf-8'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

def sanitize_filename(name):
    """Sanitize filename by removing invalid characters"""
    if not name:
        return "Unknown"
    # Remove null bytes and other invalid characters
    name = name.replace('\x00', '').strip()
    # Replace invalid path characters
    name = re.sub(r'[<>:"/\\|?*]', '_', name)
    # Remove leading/trailing spaces and dots
    name = name.strip('. ')
    return name if name else "Unknown"

class TriExtractor:
    def __init__(self, input_dir=None):
        self.input_dir = input_dir or INPUT_BNK_DIR
        self.bnks = []
        self.wems = []
        self.wavs = []
        self.duplicates = defaultdict(list)
        self.coverage_map = []

    def scan_bnks(self):
        """Scan extracted_bnk directory for all BNK files"""
        logger.info(f"Scanning {self.input_dir} for BNK files...")

        if not os.path.exists(self.input_dir):
            logger.error(f"Input directory not found: {self.input_dir}")
            return []

        bnk_files = list(Path(self.input_dir).glob("*.bnk"))
        logger.info(f"Found {len(bnk_files)} BNK files")

        for bnk_file in bnk_files:
            try:
                bnk_size = os.path.getsize(bnk_file)
                bnk_data = open(bnk_file, 'rb').read()

                # Extract STID name
                stid_name = self._extract_stid(bnk_data)
                stid_name = sanitize_filename(stid_name)

                self.bnks.append({
                    'bnk_file': str(bnk_file),
                    'bnk_name': bnk_file.stem,
                    'bnk_size': bnk_size,
                    'stid_name': stid_name,
                    'bnk_data': bnk_data
                })
            except Exception as e:
                logger.error(f"Error scanning {bnk_file}: {e}")

        logger.info(f"Scanned {len(self.bnks)} BNKs")
        return self.bnks
    
    def carve_raw_bnks(self):
        """Copy raw BNK files to output structure"""
        logger.info("Organizing raw BNK files...")

        for i, bnk_info in enumerate(self.bnks):
            try:
                bnk_data = bnk_info['bnk_data']
                stid_name = bnk_info['stid_name']

                # Determine category
                category = self._classify_by_stid(stid_name)
                bnk_info['category'] = category

                # Create output path
                output_path = os.path.join(RAW_BNK_DIR, category, stid_name)
                os.makedirs(output_path, exist_ok=True)

                bnk_filename = f"{stid_name}.bnk"
                output_file = os.path.join(output_path, bnk_filename)

                with open(output_file, 'wb') as f:
                    f.write(bnk_data)

                bnk_info['raw_bnk_path'] = output_file

                if (i + 1) % 50 == 0:
                    logger.info(f"  Organized {i+1}/{len(self.bnks)} BNKs")
            except Exception as e:
                logger.error(f"Error organizing BNK {i}: {e}")

        logger.info(f"Organized {len(self.bnks)} raw BNK files")
    
    def _extract_stid(self, bnk_data):
        """Extract STID name from BNK"""
        try:
            pos = bnk_data.find(STID_SIGNATURE)
            if pos == -1:
                return 'Unknown'
            
            # STID format: signature(4) + size(4) + data
            size_pos = pos + 4
            if size_pos + 4 > len(bnk_data):
                return 'Unknown'
            
            size = struct.unpack('<I', bnk_data[size_pos:size_pos+4])[0]
            data_start = size_pos + 4
            
            if data_start + size > len(bnk_data):
                return 'Unknown'
            
            stid_data = bnk_data[data_start:data_start+size]
            
            # Parse STID entries (each: id(4) + name_len(1) + name)
            if len(stid_data) >= 5:
                name_len = stid_data[4]
                if name_len > 0 and 5 + name_len <= len(stid_data):
                    name = stid_data[5:5+name_len].decode('utf-8', errors='ignore')
                    return name.strip()
        except:
            pass
        
        return 'Unknown'
    
    def _classify_by_stid(self, stid_name):
        """Classify audio by STID name"""
        name_lower = stid_name.lower()
        
        if any(x in name_lower for x in ['level', 'map', 'moria', 'gondor', 'rohan', 'isengard']):
            return 'Level'
        elif any(x in name_lower for x in ['hero', 'player', 'aragorn', 'gandalf', 'gimli', 'legolas']):
            return 'Hero'
        elif any(x in name_lower for x in ['creature', 'orc', 'troll', 'dragon', 'warg', 'ent', 'balrog']):
            return 'Creature'
        elif any(x in name_lower for x in ['ambient', 'env', 'environment', 'wind', 'water']):
            return 'Environment'
        elif any(x in name_lower for x in ['ui', 'menu', 'button', 'interface']):
            return 'UI'
        elif any(x in name_lower for x in ['sfx', 'effect', 'weapon', 'impact', 'footstep']):
            return 'SFX'
        elif any(x in name_lower for x in ['music', 'bgm', 'theme', 'score']):
            return 'Music'
        else:
            return 'Misc_Unresolved'
    
    def extract_wems(self):
        """Extract WEM files from BNKs"""
        logger.info("Extracting WEM files from BNKs...")

        wem_count = 0
        for i, bnk_info in enumerate(self.bnks):
            try:
                bnk_data = bnk_info['bnk_data']

                # Find DIDX and DATA sections
                didx_pos = bnk_data.find(DIDX_SIGNATURE)
                data_pos = bnk_data.find(DATA_SIGNATURE)

                if didx_pos == -1 or data_pos == -1:
                    continue

                # Parse DIDX
                didx_size_pos = didx_pos + 4
                didx_size = struct.unpack('<I', bnk_data[didx_size_pos:didx_size_pos+4])[0]
                didx_data_start = didx_size_pos + 4

                # Parse DATA offset
                data_size_pos = data_pos + 4
                data_size = struct.unpack('<I', bnk_data[data_size_pos:data_size_pos+4])[0]
                data_start = data_size_pos + 4

                # Extract WEMs from DIDX entries
                pos = didx_data_start
                end = didx_data_start + didx_size

                while pos + 12 <= end:
                    try:
                        wem_id = struct.unpack('<I', bnk_data[pos:pos+4])[0]
                        wem_offset = struct.unpack('<I', bnk_data[pos+4:pos+8])[0]
                        wem_size = struct.unpack('<I', bnk_data[pos+8:pos+12])[0]

                        # Extract WEM data
                        wem_abs_offset = data_start + wem_offset
                        if wem_abs_offset + wem_size <= len(bnk_data):
                            wem_data = bnk_data[wem_abs_offset:wem_abs_offset+wem_size]

                            # Save raw WEM
                            category = bnk_info.get('category', 'Misc_Unresolved')
                            stid_name = bnk_info.get('stid_name', 'Unknown')

                            wem_dir = os.path.join(RAW_WEM_DIR, category, stid_name)
                            os.makedirs(wem_dir, exist_ok=True)

                            wem_file = os.path.join(wem_dir, f"{wem_id:08X}.wem")
                            with open(wem_file, 'wb') as f:
                                f.write(wem_data)

                            self.wems.append({
                                'wem_id': wem_id,
                                'bnk_name': stid_name,
                                'category': category,
                                'wem_file': wem_file,
                                'wem_size': wem_size
                            })

                            wem_count += 1
                    except:
                        pass

                    pos += 12

                if (i + 1) % 10 == 0:
                    logger.info(f"  Processed {i+1}/{len(self.bnks)} BNKs, extracted {wem_count} WEMs")
            except Exception as e:
                logger.error(f"Error extracting WEMs from BNK {i}: {e}")

        logger.info(f"Extracted {wem_count} WEM files")
    
    def decode_wavs(self):
        """Decode WEM files to WAV"""
        logger.info("Decoding WEM files to WAV...")
        
        if not os.path.exists(VGMSTREAM_CLI):
            logger.error(f"vgmstream-cli not found at {VGMSTREAM_CLI}")
            return
        
        decoded_count = 0
        for i, wem_info in enumerate(self.wems):
            try:
                wem_file = wem_info['wem_file']
                category = wem_info['category']
                stid_name = wem_info['bnk_name']
                wem_id = wem_info['wem_id']
                
                # Create output directory
                wav_dir = os.path.join(DECODED_WAV_DIR, category, stid_name)
                os.makedirs(wav_dir, exist_ok=True)
                
                wav_file = os.path.join(wav_dir, f"{wem_id:08X}.wav")
                
                # Decode using vgmstream-cli
                result = subprocess.run(
                    [VGMSTREAM_CLI, wem_file, '-o', wav_file],
                    capture_output=True,
                    timeout=30
                )
                
                if result.returncode == 0 and os.path.exists(wav_file):
                    # Verify RIFF header
                    with open(wav_file, 'rb') as f:
                        header = f.read(4)
                    
                    if header == RIFF_HEADER:
                        self.wavs.append({
                            'wem_id': wem_id,
                            'bnk_name': stid_name,
                            'category': category,
                            'wav_file': wav_file,
                            'status': 'OK'
                        })
                        decoded_count += 1
                    else:
                        logger.warning(f"Invalid RIFF header: {wav_file}")
                else:
                    logger.warning(f"Decode failed: {wem_file}")
                
                if (i + 1) % 100 == 0:
                    logger.info(f"  Decoded {decoded_count}/{i+1} WAVs")
            except Exception as e:
                logger.error(f"Error decoding WEM {i}: {e}")
        
        logger.info(f"Decoded {decoded_count} WAV files")
    
    def generate_reports(self):
        """Generate CSV reports"""
        logger.info("Generating reports...")

        os.makedirs(REPORTS_DIR, exist_ok=True)

        # BNK_MAP.csv
        bnk_map_file = os.path.join(REPORTS_DIR, "BNK_MAP.csv")
        with open(bnk_map_file, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=['BNK_Index', 'BNK_Name', 'STID_Name', 'Category', 'BNK_Size', 'WEM_Count', 'Raw_BNK_Path'])
            writer.writeheader()
            for i, bnk in enumerate(self.bnks):
                wem_count = sum(1 for w in self.wems if w['bnk_name'] == bnk['stid_name'])
                writer.writerow({
                    'BNK_Index': i,
                    'BNK_Name': bnk['bnk_name'],
                    'STID_Name': bnk['stid_name'],
                    'Category': bnk.get('category', 'Unknown'),
                    'BNK_Size': bnk['bnk_size'],
                    'WEM_Count': wem_count,
                    'Raw_BNK_Path': bnk.get('raw_bnk_path', '')
                })

        logger.info(f"Generated {bnk_map_file}")

        # CLASSIFICATION.csv
        class_file = os.path.join(REPORTS_DIR, "CLASSIFICATION.csv")
        with open(class_file, 'w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=['BNK_Name', 'WEM_ID', 'Category', 'WEM_Size', 'WEM_File', 'WAV_File', 'Status'])
            writer.writeheader()
            for wem in self.wems:
                wav_file = wem['wem_file'].replace('.wem', '.wav').replace('Raw_WEM', 'Decoded_WAVs')
                writer.writerow({
                    'BNK_Name': wem['bnk_name'],
                    'WEM_ID': f"{wem['wem_id']:08X}",
                    'Category': wem['category'],
                    'WEM_Size': wem['wem_size'],
                    'WEM_File': wem['wem_file'],
                    'WAV_File': wav_file,
                    'Status': 'OK' if os.path.exists(wav_file) else 'DECODE_FAILED'
                })

        logger.info(f"Generated {class_file}")
    
    def run(self):
        """Run full extraction pipeline"""
        logger.info("=" * 80)
        logger.info("TRI-OUTPUT BNK EXTRACTION PIPELINE")
        logger.info("=" * 80)

        self.scan_bnks()
        self.carve_raw_bnks()
        self.extract_wems()
        self.decode_wavs()
        self.generate_reports()

        logger.info("=" * 80)
        logger.info("EXTRACTION COMPLETE")
        logger.info(f"  BNKs: {len(self.bnks)}")
        logger.info(f"  WEMs: {len(self.wems)}")
        logger.info(f"  WAVs: {len(self.wavs)}")
        logger.info("=" * 80)

def main():
    input_dir = sys.argv[1] if len(sys.argv) > 1 else INPUT_BNK_DIR

    if not os.path.exists(input_dir):
        print(f"Error: {input_dir} not found")
        sys.exit(1)

    extractor = TriExtractor(input_dir)
    extractor.run()

if __name__ == "__main__":
    main()

