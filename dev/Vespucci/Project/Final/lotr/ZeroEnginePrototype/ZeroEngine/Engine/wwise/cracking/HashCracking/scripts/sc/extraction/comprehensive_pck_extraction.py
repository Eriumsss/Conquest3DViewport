#!/usr/bin/env python3
"""
Comprehensive sound.pck Extraction & Classification System
Byte-accurate indexing, WEM extraction, event-based classification, deduplication
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
from datetime import datetime
from collections import defaultdict

# Configuration
PCK_FILE = "Audio/sound.pck"
EXTRACTED_BNK_DIR = "Audio/extracted_bnk"
OUTPUT_BASE = "Audio/Final_Organized"
REPORTS_DIR = "Audio/Reports"
LOGS_DIR = "Audio/Logs"
VGMSTREAM_CLI = "Audio/vgmstream-win64/vgmstream-cli.exe"
EVENT_MAPPING_CSV = "Audio/Analysis/event_wem_mapping.csv"
WWISE_TABLE = "Audio/WWiseIDTable.audio.json"

# Create output directories
for d in [OUTPUT_BASE, REPORTS_DIR, LOGS_DIR]:
    os.makedirs(d, exist_ok=True)

# Setup logging
log_file = os.path.join(LOGS_DIR, "run.log")
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler(log_file),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

class PCKIndexer:
    """Index all BNKs in sound.pck"""
    
    def __init__(self, pck_path):
        self.pck_path = pck_path
        self.bnks = []
        
    def scan_for_bnks(self):
        """Scan PCK for BKHD signatures"""
        logger.info(f"Scanning {self.pck_path} for BNK files...")
        
        with open(self.pck_path, 'rb') as f:
            data = f.read()
        
        pck_size = len(data)
        logger.info(f"PCK file size: {pck_size:,} bytes ({pck_size/1024/1024:.1f} MB)")
        
        # Find all BKHD signatures
        bkhd_sig = b'BKHD'
        offset = 0
        bnk_count = 0
        
        while True:
            pos = data.find(bkhd_sig, offset)
            if pos == -1:
                break
            
            # Try to parse BNK at this position
            try:
                bnk_data = self._parse_bnk_header(data, pos)
                if bnk_data:
                    self.bnks.append(bnk_data)
                    bnk_count += 1
                    logger.info(f"  Found BNK #{bnk_count}: offset=0x{pos:08x}, size={bnk_data['size']:,}")
            except:
                pass
            
            offset = pos + 1
        
        logger.info(f"Total BNKs found: {bnk_count}")
        return self.bnks
    
    def _parse_bnk_header(self, data, bkhd_pos):
        """Parse BNK header at given position"""
        if bkhd_pos + 16 > len(data):
            return None
        
        # BKHD structure: magic(4) + size(4) + version(4) + bank_id(4)
        magic = data[bkhd_pos:bkhd_pos+4]
        if magic != b'BKHD':
            return None
        
        bkhd_size = struct.unpack('<I', data[bkhd_pos+4:bkhd_pos+8])[0]
        bank_id = struct.unpack('<I', data[bkhd_pos+12:bkhd_pos+16])[0]
        
        # Estimate total BNK size by finding next section or end
        bnk_start = bkhd_pos
        bnk_size = self._estimate_bnk_size(data, bkhd_pos)
        
        return {
            'pck_offset': bnk_start,
            'size': bnk_size,
            'bank_id': bank_id,
            'bkhd_size': bkhd_size
        }
    
    def _estimate_bnk_size(self, data, bkhd_pos):
        """Estimate BNK size by scanning for next BKHD or end"""
        next_bkhd = data.find(b'BKHD', bkhd_pos + 4)
        if next_bkhd == -1:
            return len(data) - bkhd_pos
        return next_bkhd - bkhd_pos

class BNKParser:
    """Parse individual BNK files"""

    def __init__(self, bnk_data, pck_data):
        self.bnk_data = bnk_data
        self.pck_data = pck_data
        self.pck_offset = bnk_data['pck_offset']
        self.bnk_size = bnk_data['size']

    def parse(self):
        """Parse BNK and extract metadata"""
        result = {
            'pck_offset': self.pck_offset,
            'size': self.bnk_size,
            'bank_id': self.bnk_data['bank_id'],
            'stid': None,
            'hirc_count': 0,
            'didx_entries': [],
            'data_offset': None,
            'data_size': 0,
            'total_wem_bytes': 0
        }

        # Parse sections
        offset = self.pck_offset + 8  # Skip BKHD magic+size
        end = self.pck_offset + self.bnk_size

        while offset < end - 8:
            if offset + 8 > len(self.pck_data):
                break

            section_sig = self.pck_data[offset:offset+4]
            if not all(32 <= b <= 126 for b in section_sig):
                break

            try:
                section_size = struct.unpack('<I', self.pck_data[offset+4:offset+8])[0]
            except:
                break

            section_name = section_sig.decode('ascii', errors='ignore')

            if section_name == 'STID':
                result['stid'] = self._parse_stid(offset + 8, section_size)
            elif section_name == 'HIRC':
                result['hirc_count'] = self._count_hirc_objects(offset + 8, section_size)
            elif section_name == 'DIDX':
                entries = self._parse_didx(offset + 8, section_size)
                result['didx_entries'] = entries
                result['total_wem_bytes'] = sum(e['size'] for e in entries)
            elif section_name == 'DATA':
                result['data_offset'] = offset + 8
                result['data_size'] = section_size

            offset += 8 + section_size

        return result

    def _parse_stid(self, offset, size):
        """Parse STID section for bank name"""
        try:
            # STID format: skip 12 bytes, then read string length + string
            if offset + 13 > len(self.pck_data):
                return None
            str_len = self.pck_data[offset + 12]
            if str_len > 0 and offset + 13 + str_len <= len(self.pck_data):
                name = self.pck_data[offset+13:offset+13+str_len].decode('utf-8', errors='ignore')
                return name
        except:
            pass
        return None

    def _count_hirc_objects(self, offset, size):
        """Count HIRC objects"""
        count = 0
        pos = offset
        end = offset + size

        while pos + 12 <= end:
            try:
                obj_size = struct.unpack('<I', self.pck_data[pos+1:pos+5])[0]
                pos += 4 + obj_size
                count += 1
            except:
                break

        return count

    def _parse_didx(self, offset, size):
        """Parse DIDX section for WEM entries"""
        entries = []
        pos = offset
        end = offset + size

        while pos + 12 <= end:
            try:
                wem_id = struct.unpack('<I', self.pck_data[pos:pos+4])[0]
                wem_offset = struct.unpack('<I', self.pck_data[pos+4:pos+8])[0]
                wem_size = struct.unpack('<I', self.pck_data[pos+8:pos+12])[0]

                entries.append({
                    'wem_id': wem_id,
                    'offset': wem_offset,
                    'size': wem_size
                })
            except:
                break
            pos += 12

        return entries

def main():
    logger.info("=" * 80)
    logger.info("COMPREHENSIVE PCK EXTRACTION & CLASSIFICATION")
    logger.info("=" * 80)

    # Step 1: Index PCK
    indexer = PCKIndexer(PCK_FILE)
    bnks = indexer.scan_for_bnks()

    if not bnks:
        logger.error("No BNKs found in PCK!")
        return

    # Step 2: Parse each BNK
    logger.info(f"\nParsing {len(bnks)} BNKs...")

    with open(PCK_FILE, 'rb') as f:
        pck_data = f.read()

    bnk_map = []
    audio_bnks = 0
    total_wems = 0
    total_wem_bytes = 0

    for idx, bnk_info in enumerate(bnks):
        parser = BNKParser(bnk_info, pck_data)
        parsed = parser.parse()
        bnk_map.append(parsed)

        if parsed['didx_entries']:
            audio_bnks += 1
            total_wems += len(parsed['didx_entries'])
            total_wem_bytes += parsed['total_wem_bytes']
            if idx % 50 == 0:
                logger.info(f"  [{idx}/{len(bnks)}] BNK @ 0x{parsed['pck_offset']:08x}: {len(parsed['didx_entries'])} WEMs, "
                           f"STID={parsed['stid']}, {parsed['total_wem_bytes']:,} bytes")

    logger.info(f"\nParsing complete: {audio_bnks} audio BNKs, {total_wems} total WEMs, {total_wem_bytes:,} bytes")

    # Step 3: Write PCK_INDEX.csv
    pck_index_file = os.path.join(REPORTS_DIR, "PCK_INDEX.csv")
    with open(pck_index_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['BNK_Index', 'PCK_Offset', 'BNK_Size', 'Bank_ID', 'STID_Name', 'WEM_Count', 'Total_WEM_Bytes', 'Has_Data'])
        writer.writeheader()
        for idx, bnk in enumerate(bnk_map):
            writer.writerow({
                'BNK_Index': idx,
                'PCK_Offset': f"0x{bnk['pck_offset']:08x}",
                'BNK_Size': bnk['size'],
                'Bank_ID': f"0x{bnk['bank_id']:08x}",
                'STID_Name': bnk['stid'] or 'N/A',
                'WEM_Count': len(bnk['didx_entries']),
                'Total_WEM_Bytes': bnk['total_wem_bytes'],
                'Has_Data': 'Yes' if bnk['data_offset'] else 'No'
            })

    logger.info(f"PCK_INDEX.csv written: {pck_index_file}")

    # Step 4: Write BNK_MAP.csv
    bnk_map_file = os.path.join(REPORTS_DIR, "BNK_MAP.csv")
    with open(bnk_map_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['BNK_Index', 'PCK_Offset', 'STID_Name', 'WEM_Count', 'Total_WEM_Bytes', 'Data_Size', 'HIRC_Count'])
        writer.writeheader()
        for idx, bnk in enumerate(bnk_map):
            writer.writerow({
                'BNK_Index': idx,
                'PCK_Offset': f"0x{bnk['pck_offset']:08x}",
                'STID_Name': bnk['stid'] or 'N/A',
                'WEM_Count': len(bnk['didx_entries']),
                'Total_WEM_Bytes': bnk['total_wem_bytes'],
                'Data_Size': bnk['data_size'],
                'HIRC_Count': bnk['hirc_count']
            })

    logger.info(f"BNK_MAP.csv written: {bnk_map_file}")

    logger.info("\n" + "=" * 80)
    logger.info("PHASE 1 COMPLETE: PCK Indexing")
    logger.info(f"Total BNKs: {len(bnk_map)}, Audio BNKs: {audio_bnks}, Total WEMs: {total_wems}, Total Bytes: {total_wem_bytes:,}")
    logger.info("=" * 80)

if __name__ == "__main__":
    main()

