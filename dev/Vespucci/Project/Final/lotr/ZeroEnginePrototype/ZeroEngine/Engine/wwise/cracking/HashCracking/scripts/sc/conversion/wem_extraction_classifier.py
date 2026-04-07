#!/usr/bin/env python3
"""
WEM Extraction & Event-Based Classification
Extracts WEMs from BNKs, decodes to WAV, classifies by STID + event mapping
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
REPORTS_DIR = "Audio/Reports"
LOGS_DIR = "Audio/Logs"
OUTPUT_BASE = "Audio/Final_Organized"
VGMSTREAM_CLI = "Audio/vgmstream-win64/vgmstream-cli.exe"
FFPROBE = "ffprobe"
EVENT_MAPPING_CSV = "Audio/Analysis/event_wem_mapping.csv"
WWISE_TABLE = "Audio/WWiseIDTable.audio.json"
PCK_FILE = "Audio/sound.pck"

logger = logging.getLogger(__name__)

class EventMapper:
    """Load and manage event-to-WEM mappings"""
    
    def __init__(self):
        self.wem_to_events = defaultdict(list)
        self.event_to_wems = defaultdict(list)
        self.event_names = {}
        self.wwise_table = {}
        
    def load_mappings(self):
        """Load event_wem_mapping.csv and WWiseIDTable"""
        logger.info("Loading event mappings...")
        
        # Load event_wem_mapping.csv
        if os.path.exists(EVENT_MAPPING_CSV):
            with open(EVENT_MAPPING_CSV, 'r', encoding='utf-8') as f:
                reader = csv.DictReader(f)
                for row in reader:
                    try:
                        wem_id = int(row.get('WEM_ID', 0))
                        event_id = int(row.get('EventID', 0))
                        event_name = row.get('EventName', '')
                        
                        if wem_id > 0:
                            self.wem_to_events[wem_id].append(event_id)
                            self.event_to_wems[event_id].append(wem_id)
                            if event_name:
                                self.event_names[event_id] = event_name
                    except:
                        pass
            logger.info(f"  Loaded {len(self.wem_to_events)} WEM→Event mappings")
        
        # Load WWiseIDTable
        if os.path.exists(WWISE_TABLE):
            with open(WWISE_TABLE, 'r', encoding='utf-8') as f:
                self.wwise_table = json.load(f)
            logger.info(f"  Loaded {len(self.wwise_table)} WWise entries")
    
    def get_events_for_wem(self, wem_id):
        """Get event IDs for a WEM"""
        return self.wem_to_events.get(wem_id, [])
    
    def get_event_name(self, event_id):
        """Get event name"""
        return self.event_names.get(event_id, f"Event_{event_id}")

class Classifier:
    """Classify WAVs by STID + event context"""
    
    CATEGORY_PATTERNS = {
        'Level': r'(level|map|cori|ankh|mount|moria|rivendell|gondor|rohan|isengard)',
        'Hero': r'(hero|player|gandalf|aragorn|gimli|legolas|boromir|frodo)',
        'Creature': r'(creature|orc|goblin|troll|dragon|balrog|human|uruk)',
        'Environment': r'(ambient|env|environment|wind|water|fire|nature|weather)',
        'UI': r'(ui|menu|button|select|confirm|cancel|interface|hud)',
        'SFX': r'(sfx|effect|sound|impact|hit|attack|footstep|weapon)',
        'Music': r'(music|bgm|theme|score|ambient_music)',
    }
    
    def __init__(self, event_mapper):
        self.event_mapper = event_mapper
    
    def classify(self, stid_name, wem_id, event_ids):
        """Classify a WEM based on STID + events"""
        
        # Rule 1: Check STID name
        if stid_name:
            for category, pattern in self.CATEGORY_PATTERNS.items():
                if re.search(pattern, stid_name, re.IGNORECASE):
                    # Extract subcategory (e.g., "Level_Moria" → "Moria")
                    parts = stid_name.split('_', 1)
                    subcat = parts[1] if len(parts) > 1 else stid_name
                    return category, subcat
        
        # Rule 2: Check event names
        for event_id in event_ids:
            event_name = self.event_mapper.get_event_name(event_id)
            for category, pattern in self.CATEGORY_PATTERNS.items():
                if re.search(pattern, event_name, re.IGNORECASE):
                    return category, event_name
        
        # Default
        return 'Misc', 'Uncategorized'

class WEMExtractor:
    """Extract WEMs from BNKs and decode to WAV"""
    
    def __init__(self, pck_data):
        self.pck_data = pck_data
        self.extracted_wems = []
        self.failed_decodes = []
    
    def extract_wem(self, bnk_offset, data_offset, wem_offset, wem_size, wem_id):
        """Extract a single WEM from PCK"""
        try:
            # Calculate absolute offset in PCK
            abs_offset = bnk_offset + data_offset + wem_offset
            
            if abs_offset + wem_size > len(self.pck_data):
                logger.warning(f"  WEM {wem_id}: offset out of bounds")
                return None
            
            wem_data = self.pck_data[abs_offset:abs_offset + wem_size]
            
            # Verify WEM signature
            if not wem_data.startswith(b'RIFF'):
                logger.warning(f"  WEM {wem_id}: invalid RIFF signature")
                return None
            
            return {
                'wem_id': wem_id,
                'data': wem_data,
                'size': wem_size,
                'pck_offset': abs_offset,
                'hash': hashlib.sha1(wem_data).hexdigest()
            }
        except Exception as e:
            logger.error(f"  WEM {wem_id}: extraction failed: {e}")
            return None
    
    def decode_wem_to_wav(self, wem_data, output_path):
        """Decode WEM to WAV using vgmstream-cli"""
        try:
            # Write WEM to temp file
            temp_wem = output_path.replace('.wav', '.wem')
            with open(temp_wem, 'wb') as f:
                f.write(wem_data)
            
            # Decode
            result = subprocess.run(
                [VGMSTREAM_CLI, temp_wem, '-o', output_path],
                capture_output=True,
                timeout=30,
                text=True
            )
            
            # Clean up temp WEM
            if os.path.exists(temp_wem):
                os.remove(temp_wem)
            
            if result.returncode == 0 and os.path.exists(output_path):
                return True
            else:
                logger.warning(f"  Decode failed: {result.stderr}")
                return False
        except Exception as e:
            logger.error(f"  Decode error: {e}")
            return False
    
    def get_wav_duration(self, wav_path):
        """Get WAV duration using ffprobe"""
        try:
            result = subprocess.run(
                [FFPROBE, '-v', 'error', '-show_entries', 'format=duration',
                 '-of', 'default=nw=1:nk=1', wav_path],
                capture_output=True,
                timeout=10,
                text=True
            )
            if result.returncode == 0:
                return float(result.stdout.strip())
        except:
            pass
        return 0.0

def main():
    logger.info("=" * 80)
    logger.info("PHASE 2: WEM EXTRACTION & CLASSIFICATION")
    logger.info("=" * 80)
    
    # Load mappings
    mapper = EventMapper()
    mapper.load_mappings()
    
    classifier = Classifier(mapper)
    
    # Load PCK
    logger.info(f"Loading {PCK_FILE}...")
    with open(PCK_FILE, 'rb') as f:
        pck_data = f.read()
    
    extractor = WEMExtractor(pck_data)
    
    # Load PCK_INDEX.csv
    pck_index_file = os.path.join(REPORTS_DIR, "PCK_INDEX.csv")
    if not os.path.exists(pck_index_file):
        logger.error(f"PCK_INDEX.csv not found! Run comprehensive_pck_extraction.py first.")
        return
    
    logger.info(f"Processing BNKs from {pck_index_file}...")
    
    classification_rows = []
    dedup_map = {}  # hash → first occurrence
    
    with open(pck_index_file, 'r', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                pck_offset = int(row['PCK_Offset'], 16)
                bnk_size = int(row['BNK_Size'])
                stid_name = row['STID_Name']
                wem_count = int(row['WEM_Count'])
                
                if wem_count == 0:
                    continue
                
                logger.info(f"Processing BNK @ 0x{pck_offset:08x}: {stid_name} ({wem_count} WEMs)")
                
                # TODO: Parse DIDX/DATA from this BNK
                # For now, placeholder
                
            except Exception as e:
                logger.error(f"Error processing BNK: {e}")
    
    logger.info("\n" + "=" * 80)
    logger.info("PHASE 2 COMPLETE: WEM Extraction & Classification")
    logger.info("=" * 80)

if __name__ == "__main__":
    main()

