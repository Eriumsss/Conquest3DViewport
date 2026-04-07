#!/usr/bin/env python3
"""
Fixed PCK Parser - Extracts audio files from sound.pck
Correctly handles offset + base_offset to find RIFF headers
"""

import struct
import os
from pathlib import Path

class PCKParserFixed:
    def __init__(self, pck_path):
        self.pck_path = pck_path
        self.base_offset = 1116192  # Confirmed base offset
        
    def extract_all(self, output_dir='extracted_fixed'):
        """Extract all audio files"""
        os.makedirs(output_dir, exist_ok=True)
        
        # Create language directories
        lang_names = {
            0: 'sfx',
            1: 'french',
            2: 'german',
            3: 'english',
            4: 'italian',
            5: 'russian',
            6: 'spanish'
        }
        
        for lang_id, lang_name in lang_names.items():
            os.makedirs(f'{output_dir}/{lang_name}', exist_ok=True)
        
        with open(self.pck_path, 'rb') as f:
            f.seek(7378)
            
            # Read all entries
            entries = []
            
            # First entry (28 bytes)
            entry0 = f.read(28)
            wwise_id = struct.unpack('<I', entry0[0:4])[0]
            offset = struct.unpack('<I', entry0[8:12])[0]
            size = struct.unpack('<I', entry0[16:20])[0]
            lang = struct.unpack('<I', entry0[20:24])[0]
            entries.append((wwise_id, offset, size, lang))
            
            # Remaining entries (24 bytes each)
            for i in range(1, 26800):
                entry = f.read(24)
                wwise_id = struct.unpack('<I', entry[0:4])[0]
                offset = struct.unpack('<I', entry[8:12])[0]
                size = struct.unpack('<I', entry[16:20])[0]
                lang = struct.unpack('<I', entry[20:24])[0]
                entries.append((wwise_id, offset, size, lang))
            
            print(f"Total entries: {len(entries)}")
            print()
            
            # Extract each entry
            extracted_count = 0
            for idx, (wwise_id, offset, size, lang) in enumerate(entries):
                if idx % 1000 == 0:
                    print(f"Processing entry {idx}/{len(entries)}...")
                
                # Calculate absolute position
                abs_pos = self.base_offset + offset
                
                # Search for RIFF header within 1MB
                f.seek(abs_pos)
                search_data = f.read(1000000)
                
                riff_idx = search_data.find(b'RIFF')
                if riff_idx == -1:
                    print(f"  WARNING: No RIFF found for entry {idx} (ID: 0x{wwise_id:08x})")
                    continue
                
                # Read RIFF file
                riff_pos = abs_pos + riff_idx
                f.seek(riff_pos)
                
                # Read RIFF header to get file size
                riff_header = f.read(8)
                if riff_header[:4] != b'RIFF':
                    print(f"  ERROR: Invalid RIFF at {riff_pos}")
                    continue
                
                riff_size = struct.unpack('<I', riff_header[4:8])[0] + 8
                
                # Read full RIFF file
                f.seek(riff_pos)
                riff_data = f.read(riff_size)
                
                # Save file
                lang_name = lang_names.get(lang, f'lang{lang}')
                output_path = f'{output_dir}/{lang_name}/id_{wwise_id:08x}.wem'
                
                with open(output_path, 'wb') as out_f:
                    out_f.write(riff_data)
                
                extracted_count += 1
            
            print()
            print(f"✓ Extracted {extracted_count} files")

if __name__ == '__main__':
    parser = PCKParserFixed('sound.pck')
    parser.extract_all()

