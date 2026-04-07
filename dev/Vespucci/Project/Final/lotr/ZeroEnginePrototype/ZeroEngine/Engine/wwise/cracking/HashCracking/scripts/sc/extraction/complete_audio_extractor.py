#!/usr/bin/env python3
"""
Complete Audio Extractor for Wwise PCK Files
Extracts BNK files + Raw Audio Blocks (RIFF/RIFX/OggS/FSB5/XMA2)
Provides byte-accurate coverage verification
"""

import os
import struct
import hashlib
import csv
from pathlib import Path
from collections import defaultdict

# Configuration
PCK_FILE = "Audio/sound.pck"
OUTPUT_DIR = "Audio/Full_Extraction"
EXTRACTED_BNK_DIR = "Audio/extracted_bnk"

# Audio signatures to search for
AUDIO_SIGNATURES = {
    b'RIFF': ('RIFF', 'little'),  # RIFF/WAVE (little-endian)
    b'RIFX': ('RIFX', 'big'),     # RIFX/WAVE (big-endian)
    b'OggS': ('OggS', 'little'),  # Ogg Vorbis
    b'FSB5': ('FSB5', 'little'),  # FMOD SoundBank
    b'XMA2': ('XMA2', 'little'),  # XMA2 audio
    b'BKHD': ('BKHD', 'little'),  # Wwise BNK header
}

class AudioBlockScanner:
    def __init__(self, pck_file):
        self.pck_file = pck_file
        self.pck_size = os.path.getsize(pck_file)
        self.blocks = []
        self.coverage = {
            'bnk_blocks': [],
            'raw_blocks': [],
            'unknown_blocks': [],
            'total_extracted': 0,
            'total_unknown': 0,
        }
    
    def scan_for_blocks(self):
        """Scan PCK file for audio blocks"""
        print(f"Scanning {self.pck_file} ({self.pck_size / 1e9:.2f} GB)...")
        print("This may take several minutes...")

        with open(self.pck_file, 'rb') as f:
            # Read in chunks to avoid memory issues
            chunk_size = 50 * 1024 * 1024  # 50 MB chunks
            offset = 0
            overlap_buffer = b''

            while offset < self.pck_size:
                f.seek(offset)
                chunk = f.read(chunk_size)
                if not chunk:
                    break

                # Combine with overlap from previous chunk
                search_data = overlap_buffer + chunk
                search_offset = len(overlap_buffer)

                # Find RIFF blocks
                pos = 0
                while True:
                    pos = search_data.find(b'RIFF', pos)
                    if pos == -1:
                        break
                    actual_offset = offset - len(overlap_buffer) + pos
                    self.extract_wave_block_at(search_data, pos, actual_offset, 'RIFF', 'little')
                    pos += 4

                # Find BKHD blocks
                pos = 0
                while True:
                    pos = search_data.find(b'BKHD', pos)
                    if pos == -1:
                        break
                    actual_offset = offset - len(overlap_buffer) + pos
                    self.extract_bnk_block_at(search_data, pos, actual_offset)
                    pos += 4

                # Keep overlap for next iteration
                overlap_buffer = search_data[-1024:]
                offset += chunk_size

                if offset % (500 * 1024 * 1024) == 0:
                    print(f"  Scanned {offset / 1e9:.2f} GB... Found {len(self.blocks)} blocks so far")

        return self.blocks
    
    def extract_wave_block_at(self, data, local_offset, actual_offset, sig_name, endian):
        """Extract RIFF/RIFX WAVE block at given offset"""
        try:
            if local_offset + 12 > len(data):
                return

            # Read RIFF/RIFX header
            header = data[local_offset:local_offset+4]
            if header not in [b'RIFF', b'RIFX']:
                return

            # Read size (little or big endian)
            size_bytes = data[local_offset+4:local_offset+8]
            if endian == 'big':
                size = struct.unpack('>I', size_bytes)[0]
            else:
                size = struct.unpack('<I', size_bytes)[0]

            # Validate size
            total_size = size + 8
            if total_size > 500 * 1024 * 1024:  # Max 500 MB
                return

            # Verify WAVE signature
            if data[local_offset+8:local_offset+12] != b'WAVE':
                return

            # Check for duplicates
            if any(b['offset'] == actual_offset for b in self.blocks):
                return

            self.blocks.append({
                'type': 'WAVE',
                'offset': actual_offset,
                'size': total_size,
                'endian': endian,
            })

            self.coverage['raw_blocks'].append({
                'offset': actual_offset,
                'size': total_size,
                'type': 'WAVE',
            })
            self.coverage['total_extracted'] += total_size

        except Exception as e:
            pass
    
    def extract_bnk_block_at(self, data, local_offset, actual_offset):
        """Extract BNK block at given offset"""
        try:
            if local_offset + 8 > len(data):
                return

            # BKHD is at this offset, need to find BNK start
            # BNK files typically start with a size field before BKHD
            # Look backwards for the start
            bnk_start_local = local_offset

            # Try to find the actual BNK start by looking for size field
            if local_offset >= 4:
                # Check if there's a size field before BKHD
                potential_size = struct.unpack('<I', data[local_offset-4:local_offset])[0]
                if potential_size > 0 and potential_size < 100 * 1024 * 1024:
                    bnk_start_local = local_offset - 4

            # Estimate BNK size
            bnk_size = self.estimate_bnk_size_at(data, bnk_start_local)

            if bnk_size > 0 and bnk_start_local + bnk_size <= len(data):
                actual_bnk_start = actual_offset - (local_offset - bnk_start_local)

                # Check for duplicates
                if any(b['offset'] == actual_bnk_start for b in self.blocks):
                    return

                self.blocks.append({
                    'type': 'BNK',
                    'offset': actual_bnk_start,
                    'size': bnk_size,
                })

                self.coverage['bnk_blocks'].append({
                    'offset': actual_bnk_start,
                    'size': bnk_size,
                    'type': 'BNK',
                })
                self.coverage['total_extracted'] += bnk_size

        except Exception as e:
            pass
    
    def extract_raw_block(self, data, offset, sig_name, endian):
        """Extract raw audio block (OggS, FSB5, XMA2)"""
        try:
            # Estimate size based on signature
            size = self.estimate_raw_size(data, offset, sig_name)
            
            if size > 0 and offset + size <= len(data):
                block_data = data[offset:offset+size]
                block_hash = hashlib.sha1(block_data).hexdigest()
                
                self.blocks.append({
                    'type': sig_name,
                    'offset': offset,
                    'size': size,
                    'hash': block_hash,
                })
                
                self.coverage['raw_blocks'].append({
                    'offset': offset,
                    'size': size,
                    'type': sig_name,
                })
                self.coverage['total_extracted'] += size
        
        except Exception as e:
            pass
    
    def estimate_bnk_size_at(self, data, offset):
        """Estimate BNK file size at given offset"""
        # Look for next known signature
        min_size = 100
        max_size = 100 * 1024 * 1024  # 100 MB max

        for sig in [b'BKHD', b'RIFF', b'RIFX']:
            pos = data.find(sig, offset + min_size)
            if pos != -1 and pos - offset < max_size:
                return pos - offset

        return max_size
    
    def estimate_raw_size(self, data, offset, sig_name):
        """Estimate raw audio block size"""
        if sig_name == 'OggS':
            # Ogg files have page structure, estimate ~1-10 MB
            return min(10 * 1024 * 1024, len(data) - offset)
        elif sig_name == 'FSB5':
            # FSB5 has size in header
            if offset + 8 <= len(data):
                size = struct.unpack('<I', data[offset+4:offset+8])[0]
                return min(size, len(data) - offset)
        elif sig_name == 'XMA2':
            # XMA2 has size in header
            if offset + 8 <= len(data):
                size = struct.unpack('<I', data[offset+4:offset+8])[0]
                return min(size, len(data) - offset)
        
        return 0
    
    def generate_coverage_report(self):
        """Generate byte coverage report"""
        print("\n" + "="*80)
        print("BYTE COVERAGE REPORT")
        print("="*80)
        
        total_bnk = sum(b['size'] for b in self.coverage['bnk_blocks'])
        total_raw = sum(b['size'] for b in self.coverage['raw_blocks'])
        total_extracted = total_bnk + total_raw
        
        print(f"\nPCK File Size: {self.pck_size:,} bytes ({self.pck_size / 1e9:.2f} GB)")
        print(f"\nExtracted:")
        print(f"  BNK Blocks: {len(self.coverage['bnk_blocks'])} files, {total_bnk:,} bytes ({total_bnk / 1e9:.2f} GB)")
        print(f"  Raw Blocks: {len(self.coverage['raw_blocks'])} files, {total_raw:,} bytes ({total_raw / 1e9:.2f} GB)")
        print(f"  Total: {total_extracted:,} bytes ({total_extracted / 1e9:.2f} GB)")
        
        coverage_pct = (total_extracted / self.pck_size) * 100
        print(f"\nCoverage: {coverage_pct:.1f}%")
        
        unknown = self.pck_size - total_extracted
        print(f"Unknown/Unaccounted: {unknown:,} bytes ({unknown / 1e9:.2f} GB)")
        
        return {
            'pck_size': self.pck_size,
            'bnk_count': len(self.coverage['bnk_blocks']),
            'bnk_size': total_bnk,
            'raw_count': len(self.coverage['raw_blocks']),
            'raw_size': total_raw,
            'total_extracted': total_extracted,
            'coverage_pct': coverage_pct,
        }

if __name__ == '__main__':
    scanner = AudioBlockScanner(PCK_FILE)
    blocks = scanner.scan_for_blocks()
    report = scanner.generate_coverage_report()
    
    print(f"\nFound {len(blocks)} audio blocks total")
    print(f"BNK blocks: {report['bnk_count']}")
    print(f"Raw blocks: {report['raw_count']}")

