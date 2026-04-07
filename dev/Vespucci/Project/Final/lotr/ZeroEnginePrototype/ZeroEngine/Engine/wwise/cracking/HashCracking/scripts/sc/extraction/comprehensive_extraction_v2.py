#!/usr/bin/env python3
"""
Comprehensive Extraction v2: Use extracted BNKs directly
Parse DIDX/DATA, extract WEMs, classify by STID + events, deduplicate
"""

import os
import sys
import struct
import csv
import json
import hashlib
import subprocess
import logging
from pathlib import Path
from collections import defaultdict
import re

# Configuration
EXTRACTED_BNK_DIR = "Audio/extracted_bnk"
OUTPUT_BASE = "Audio/Final_Organized"
REPORTS_DIR = "Audio/Reports"
LOGS_DIR = "Audio/Logs"
VGMSTREAM_CLI = "Audio/vgmstream-win64/vgmstream-cli.exe"
FFPROBE = "ffprobe"
EVENT_MAPPING_CSV = "Audio/Analysis/event_wem_mapping.csv"
WWISE_TABLE = "Audio/WWiseIDTable.audio.json"

# Create directories
for d in [OUTPUT_BASE, REPORTS_DIR, LOGS_DIR]:
    os.makedirs(d, exist_ok=True)

# Setup logging
log_file = os.path.join(LOGS_DIR, "run.log")
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler(log_file, encoding='utf-8'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class BNKParser:
    """Parse BNK files"""
    
    def __init__(self, bnk_path):
        self.bnk_path = bnk_path
        self.data = None
        
    def load(self):
        """Load BNK file"""
        with open(self.bnk_path, 'rb') as f:
            self.data = f.read()
        return len(self.data)
    
    def parse(self):
        """Parse BNK structure"""
        result = {
            'path': self.bnk_path,
            'name': os.path.basename(self.bnk_path),
            'size': len(self.data),
            'stid': None,
            'didx_entries': [],
            'data_offset': None,
            'data_size': 0,
            'hirc_count': 0
        }
        
        offset = 0
        while offset < len(self.data) - 8:
            sig = self.data[offset:offset+4]
            if not all(32 <= b <= 126 for b in sig):
                break
            
            try:
                size = struct.unpack('<I', self.data[offset+4:offset+8])[0]
            except:
                break
            
            sec_name = sig.decode('ascii', errors='ignore')
            sec_data_offset = offset + 8
            
            if sec_name == 'STID':
                result['stid'] = self._parse_stid(sec_data_offset, size)
            elif sec_name == 'DIDX':
                result['didx_entries'] = self._parse_didx(sec_data_offset, size)
            elif sec_name == 'DATA':
                result['data_offset'] = sec_data_offset
                result['data_size'] = size
            elif sec_name == 'HIRC':
                result['hirc_count'] = self._count_hirc(sec_data_offset, size)
            
            offset += 8 + size
        
        return result
    
    def _parse_stid(self, offset, size):
        """Parse STID for bank name"""
        try:
            if offset + 13 > len(self.data):
                return None
            str_len = self.data[offset + 12]
            if str_len > 0 and offset + 13 + str_len <= len(self.data):
                name = self.data[offset+13:offset+13+str_len].decode('utf-8', errors='ignore')
                return name
        except:
            pass
        return None
    
    def _parse_didx(self, offset, size):
        """Parse DIDX entries"""
        entries = []
        pos = offset
        end = offset + size
        
        while pos + 12 <= end:
            try:
                wem_id = struct.unpack('<I', self.data[pos:pos+4])[0]
                wem_offset = struct.unpack('<I', self.data[pos+4:pos+8])[0]
                wem_size = struct.unpack('<I', self.data[pos+8:pos+12])[0]
                
                entries.append({
                    'wem_id': wem_id,
                    'offset': wem_offset,
                    'size': wem_size
                })
            except:
                break
            pos += 12
        
        return entries
    
    def _count_hirc(self, offset, size):
        """Count HIRC objects"""
        count = 0
        pos = offset
        end = offset + size
        
        while pos + 5 <= end:
            try:
                obj_size = struct.unpack('<I', self.data[pos+1:pos+5])[0]
                pos += 5 + obj_size
                count += 1
            except:
                break
        
        return count
    
    def extract_wem(self, wem_id, wem_offset, wem_size):
        """Extract WEM data"""
        if not self.data or self.data_offset is None:
            return None
        
        # Calculate absolute offset in BNK
        abs_offset = self.data_offset + wem_offset
        
        if abs_offset + wem_size > len(self.data):
            return None
        
        return self.data[abs_offset:abs_offset + wem_size]

def main():
    logger.info("=" * 80)
    logger.info("COMPREHENSIVE EXTRACTION v2: Using Extracted BNKs")
    logger.info("=" * 80)
    
    # Find all BNK files
    bnk_files = sorted([f for f in os.listdir(EXTRACTED_BNK_DIR) if f.endswith('.bnk')])
    logger.info(f"Found {len(bnk_files)} BNK files")
    
    # Parse all BNKs
    bnk_map = []
    audio_bnks = 0
    total_wems = 0
    total_wem_bytes = 0
    
    for idx, bnk_file in enumerate(bnk_files):
        bnk_path = os.path.join(EXTRACTED_BNK_DIR, bnk_file)
        parser = BNKParser(bnk_path)
        parser.load()
        parsed = parser.parse()
        bnk_map.append(parsed)
        
        if parsed['didx_entries']:
            audio_bnks += 1
            total_wems += len(parsed['didx_entries'])
            total_wem_bytes += sum(e['size'] for e in parsed['didx_entries'])
            
            if idx % 50 == 0 or len(parsed['didx_entries']) > 0:
                logger.info(f"[{idx+1}/{len(bnk_files)}] {bnk_file}: {len(parsed['didx_entries'])} WEMs, "
                           f"STID={parsed['stid']}, {sum(e['size'] for e in parsed['didx_entries']):,} bytes")
    
    logger.info(f"\nParsing complete:")
    logger.info(f"  Total BNKs: {len(bnk_map)}")
    logger.info(f"  Audio BNKs: {audio_bnks}")
    logger.info(f"  Total WEMs: {total_wems}")
    logger.info(f"  Total WEM bytes: {total_wem_bytes:,}")
    
    # Write BNK_MAP.csv
    bnk_map_file = os.path.join(REPORTS_DIR, "BNK_MAP.csv")
    with open(bnk_map_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['BNK_Index', 'BNK_Name', 'STID_Name', 'WEM_Count', 'Total_WEM_Bytes', 'Data_Size', 'HIRC_Count'])
        writer.writeheader()
        for idx, bnk in enumerate(bnk_map):
            writer.writerow({
                'BNK_Index': idx,
                'BNK_Name': bnk['name'],
                'STID_Name': bnk['stid'] or 'N/A',
                'WEM_Count': len(bnk['didx_entries']),
                'Total_WEM_Bytes': sum(e['size'] for e in bnk['didx_entries']),
                'Data_Size': bnk['data_size'],
                'HIRC_Count': bnk['hirc_count']
            })
    
    logger.info(f"BNK_MAP.csv written: {bnk_map_file}")
    
    logger.info("\n" + "=" * 80)
    logger.info("PHASE 1 COMPLETE")
    logger.info("=" * 80)

if __name__ == "__main__":
    main()

