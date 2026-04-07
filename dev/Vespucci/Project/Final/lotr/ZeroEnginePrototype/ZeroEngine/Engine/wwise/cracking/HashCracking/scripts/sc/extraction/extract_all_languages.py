#!/usr/bin/env python3
"""
Master Orchestration Script - Extract all language audio files
Handles: English, French, German, Italian, Spanish
"""

import os
import sys
import logging
import subprocess
from pathlib import Path
from datetime import datetime

# Setup logging
LOGS_DIR = "Audio/Logs"
os.makedirs(LOGS_DIR, exist_ok=True)

log_file = os.path.join(LOGS_DIR, "multi_language_extraction.log")
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler(log_file, encoding='utf-8'),
        logging.StreamHandler()
    ]
)
logger = logging.getLogger(__name__)

# Define all audio sources
AUDIO_SOURCES = [
    {
        'name': 'English (Main)',
        'pck_path': 'Audio/sound.pck',
        'output_prefix': 'English'
    },
    {
        'name': 'Discworld - English',
        'pck_path': 'Audio/Discworld/audio/English(US)/sound.pck',
        'output_prefix': 'Discworld_English',
        'optional': True
    },
    {
        'name': 'Discworld - French',
        'pck_path': 'Audio/Discworld/audio/French(France)/sound.pck',
        'output_prefix': 'Discworld_French',
        'optional': True
    },
    {
        'name': 'Discworld - German',
        'pck_path': 'Audio/Discworld/audio/German/sound.pck',
        'output_prefix': 'Discworld_German',
        'optional': True
    },
    {
        'name': 'Discworld - Italian',
        'pck_path': 'Audio/Discworld/audio/Italian/sound.pck',
        'output_prefix': 'Discworld_Italian',
        'optional': True
    },
    {
        'name': 'Discworld - Spanish',
        'pck_path': 'Audio/Discworld/audio/Spanish(Spain)/sound.pck',
        'output_prefix': 'Discworld_Spanish',
        'optional': True
    }
]

def check_file_exists(path):
    """Check if file exists"""
    return os.path.exists(path)

def extract_pck(pck_path, output_prefix):
    """Extract a single PCK file"""
    logger.info(f"Extracting {pck_path}...")
    
    if not check_file_exists(pck_path):
        logger.warning(f"File not found: {pck_path}")
        return False
    
    try:
        # Run extraction script
        result = subprocess.run(
            [sys.executable, 'Audio/pck_bnk_wem_tri_extractor.py', pck_path],
            capture_output=True,
            timeout=3600,
            text=True
        )
        
        if result.returncode == 0:
            logger.info(f"Successfully extracted {pck_path}")
            return True
        else:
            logger.error(f"Extraction failed for {pck_path}")
            logger.error(result.stderr)
            return False
    except Exception as e:
        logger.error(f"Error extracting {pck_path}: {e}")
        return False

def main():
    logger.info("=" * 80)
    logger.info("MULTI-LANGUAGE AUDIO EXTRACTION")
    logger.info("=" * 80)
    
    results = {}
    
    for source in AUDIO_SOURCES:
        name = source['name']
        pck_path = source['pck_path']
        is_optional = source.get('optional', False)
        
        logger.info(f"\n{'='*80}")
        logger.info(f"Processing: {name}")
        logger.info(f"File: {pck_path}")
        logger.info(f"{'='*80}")
        
        if not check_file_exists(pck_path):
            if is_optional:
                logger.info(f"Skipping optional file: {pck_path}")
                results[name] = 'SKIPPED'
            else:
                logger.error(f"Required file not found: {pck_path}")
                results[name] = 'FAILED'
            continue
        
        success = extract_pck(pck_path, source['output_prefix'])
        results[name] = 'SUCCESS' if success else 'FAILED'
    
    # Summary
    logger.info("\n" + "=" * 80)
    logger.info("EXTRACTION SUMMARY")
    logger.info("=" * 80)
    
    for name, status in results.items():
        logger.info(f"{name:<40} {status}")
    
    success_count = sum(1 for s in results.values() if s == 'SUCCESS')
    logger.info(f"\nSuccessfully extracted: {success_count}/{len(results)}")
    
    logger.info("=" * 80)
    logger.info("MULTI-LANGUAGE EXTRACTION COMPLETE")
    logger.info("=" * 80)

if __name__ == "__main__":
    main()

