#!/usr/bin/env python3
"""
Master Orchestration Script
Runs all phases: PCK indexing → WEM extraction → Classification → Deduplication → Coverage
"""

import os
import sys
import logging
import subprocess
import time
from datetime import datetime
from pathlib import Path

# Configuration
LOGS_DIR = "Audio/Logs"
REPORTS_DIR = "Audio/Reports"
OUTPUT_BASE = "Audio/Final_Organized"

# Create directories
for d in [LOGS_DIR, REPORTS_DIR, OUTPUT_BASE]:
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

def run_phase(phase_num, script_name, description):
    """Run a phase script"""
    logger.info("\n" + "=" * 80)
    logger.info(f"PHASE {phase_num}: {description}")
    logger.info("=" * 80)
    
    try:
        result = subprocess.run(
            [sys.executable, script_name],
            cwd=os.getcwd(),
            capture_output=False,
            timeout=3600
        )
        
        if result.returncode == 0:
            logger.info(f"✓ Phase {phase_num} completed successfully")
            return True
        else:
            logger.error(f"✗ Phase {phase_num} failed with code {result.returncode}")
            return False
    except subprocess.TimeoutExpired:
        logger.error(f"✗ Phase {phase_num} timed out")
        return False
    except Exception as e:
        logger.error(f"✗ Phase {phase_num} error: {e}")
        return False

def main():
    start_time = time.time()
    
    logger.info("=" * 80)
    logger.info("COMPREHENSIVE SOUND.PCK EXTRACTION & CLASSIFICATION")
    logger.info("=" * 80)
    logger.info(f"Start time: {datetime.now()}")
    logger.info(f"Log file: {log_file}")
    
    phases = [
        (1, "Audio/comprehensive_pck_extraction.py", "PCK Indexing & BNK Discovery"),
        (2, "Audio/wem_extraction_classifier.py", "WEM Extraction & Event-Based Classification"),
        (3, "Audio/dedup_and_coverage.py", "Deduplication & Coverage Analysis"),
    ]
    
    results = []
    for phase_num, script, description in phases:
        success = run_phase(phase_num, script, description)
        results.append((phase_num, description, success))
        
        if not success:
            logger.error(f"Stopping at phase {phase_num}")
            break
    
    # Summary
    logger.info("\n" + "=" * 80)
    logger.info("EXECUTION SUMMARY")
    logger.info("=" * 80)
    
    for phase_num, description, success in results:
        status = "✓ PASS" if success else "✗ FAIL"
        logger.info(f"Phase {phase_num}: {description} ... {status}")
    
    elapsed = time.time() - start_time
    logger.info(f"\nTotal time: {elapsed:.1f} seconds ({elapsed/60:.1f} minutes)")
    logger.info(f"End time: {datetime.now()}")
    
    # Check outputs
    logger.info("\n" + "=" * 80)
    logger.info("OUTPUT FILES")
    logger.info("=" * 80)
    
    expected_files = [
        os.path.join(REPORTS_DIR, "PCK_INDEX.csv"),
        os.path.join(REPORTS_DIR, "BNK_MAP.csv"),
        os.path.join(REPORTS_DIR, "CLASSIFICATION.csv"),
        os.path.join(REPORTS_DIR, "DEDUP_REMOVED.csv"),
        os.path.join(REPORTS_DIR, "COVERAGE_REPORT.md"),
    ]
    
    for file_path in expected_files:
        if os.path.exists(file_path):
            size = os.path.getsize(file_path)
            logger.info(f"✓ {file_path} ({size:,} bytes)")
        else:
            logger.warning(f"✗ {file_path} (not found)")
    
    logger.info("\n" + "=" * 80)
    logger.info("EXTRACTION COMPLETE")
    logger.info("=" * 80)

if __name__ == "__main__":
    main()

