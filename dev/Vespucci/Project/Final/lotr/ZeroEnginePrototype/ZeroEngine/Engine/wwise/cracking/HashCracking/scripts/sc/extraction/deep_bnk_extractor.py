#!/usr/bin/env python3
"""
Deep Mathematical BNK Extractor
Properly parses Wwise BNK files and extracts WEM audio with correct IDs
"""

import os
import struct
from pathlib import Path

class DeepBNKExtractor:
    def __init__(self, pck_file, output_dir):
        self.pck_file = pck_file
        self.output_dir = output_dir
        self.pck_size = os.path.getsize(pck_file)
        
        # Statistics
        self.stats = {
            'bnk_files': 0,
            'wem_files': 0,
            'total_wem_size': 0,
            'bnk_with_audio': 0,
            'bnk_metadata_only': 0
        }
        
    def find_all_bnk_offsets(self):
        """Phase 1: Find all BNK file offsets in PCK"""
        print("Phase 1: Scanning for BNK files...")
        bnk_offsets = []
        
        with open(self.pck_file, 'rb') as f:
            chunk_size = 50 * 1024 * 1024
            offset = 0
            overlap_buffer = b''
            
            while offset < self.pck_size:
                f.seek(offset)
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                
                search_data = overlap_buffer + chunk
                pos = 0
                
                while True:
                    pos = search_data.find(b'BKHD', pos)
                    if pos == -1:
                        break
                    
                    actual_offset = offset - len(overlap_buffer) + pos
                    bnk_offsets.append(actual_offset)
                    pos += 4
                
                overlap_buffer = search_data[-1024:]
                offset += chunk_size
        
        print(f"  ✓ Found {len(bnk_offsets)} BNK files")
        return bnk_offsets
    
    def calculate_bnk_boundaries(self, bnk_offsets):
        """Phase 2: Calculate exact BNK file boundaries by parsing sections"""
        print("\nPhase 2: Calculating BNK boundaries...")
        bnk_files = []

        with open(self.pck_file, 'rb') as f:
            for i, offset in enumerate(bnk_offsets):
                f.seek(offset)

                # Read enough data to parse all sections
                max_read = min(50 * 1024 * 1024, self.pck_size - offset)
                data = f.read(max_read)

                # Calculate actual BNK size by parsing sections
                total_size = 0
                pos = 0

                while pos < len(data) - 8:
                    sig = data[pos:pos+4]
                    if len(sig) < 4:
                        break

                    try:
                        section_size = struct.unpack('<I', data[pos+4:pos+8])[0]
                    except:
                        break

                    # Check if this is a valid section signature
                    valid_sigs = [b'BKHD', b'DIDX', b'DATA', b'HIRC', b'STID', b'ENVS', b'FXPR', b'STMG']
                    if sig not in valid_sigs:
                        # End of BNK file
                        break

                    total_size = pos + 8 + section_size
                    pos = total_size

                    # Safety check
                    if total_size > 100 * 1024 * 1024:  # Max 100MB per BNK
                        break

                # If we didn't find any sections, use a default small size
                if total_size == 0:
                    total_size = 12288  # 12KB default

                bnk_files.append({
                    'index': i,
                    'offset': offset,
                    'size': total_size
                })

        print(f"  ✓ Calculated {len(bnk_files)} BNK boundaries")
        return bnk_files
    
    def parse_bnk_structure(self, bnk_data):
        """Parse BNK file structure and extract sections"""
        sections = {}
        offset = 0
        
        while offset < len(bnk_data) - 8:
            # Read section signature and size
            sig = bnk_data[offset:offset+4]
            if len(sig) < 4:
                break
            
            try:
                size = struct.unpack('<I', bnk_data[offset+4:offset+8])[0]
            except:
                break
            
            section_name = sig.decode('ascii', errors='ignore')
            section_data = bnk_data[offset+8:offset+8+size]
            
            sections[section_name] = {
                'offset': offset,
                'size': size,
                'data': section_data
            }
            
            offset += 8 + size
            
            # Stop if we've gone too far
            if offset > len(bnk_data):
                break
        
        return sections
    
    def extract_wems_from_bnk(self, bnk_index, bnk_data):
        """Extract WEM files from BNK using DIDX and DATA sections"""
        sections = self.parse_bnk_structure(bnk_data)
        
        # Check if BNK has audio data
        if 'DIDX' not in sections or 'DATA' not in sections:
            self.stats['bnk_metadata_only'] += 1
            return []
        
        self.stats['bnk_with_audio'] += 1
        
        # Parse DIDX (Data Index)
        didx_data = sections['DIDX']['data']
        data_section = sections['DATA']['data']
        
        wem_files = []
        
        # DIDX entries are 12 bytes each: ID (4), Offset (4), Size (4)
        num_entries = len(didx_data) // 12
        
        for i in range(num_entries):
            entry_offset = i * 12
            
            try:
                wem_id = struct.unpack('<I', didx_data[entry_offset:entry_offset+4])[0]
                wem_offset = struct.unpack('<I', didx_data[entry_offset+4:entry_offset+8])[0]
                wem_size = struct.unpack('<I', didx_data[entry_offset+8:entry_offset+12])[0]
                
                # Extract WEM data
                if wem_offset + wem_size <= len(data_section):
                    wem_data = data_section[wem_offset:wem_offset+wem_size]
                    
                    wem_files.append({
                        'id': wem_id,
                        'size': wem_size,
                        'data': wem_data,
                        'bnk_index': bnk_index
                    })
                    
                    self.stats['wem_files'] += 1
                    self.stats['total_wem_size'] += wem_size
            except:
                continue
        
        return wem_files
    
    def extract_all(self):
        """Main extraction pipeline"""
        print("="*80)
        print("DEEP MATHEMATICAL BNK EXTRACTION")
        print("="*80)
        print(f"\nSource: {self.pck_file}")
        print(f"Size: {self.pck_size / 1e9:.2f} GB")
        print(f"Output: {self.output_dir}\n")
        
        # Phase 1: Find BNK offsets
        bnk_offsets = self.find_all_bnk_offsets()
        
        # Phase 2: Calculate boundaries
        bnk_files = self.calculate_bnk_boundaries(bnk_offsets)
        
        # Phase 3: Extract WEM files from each BNK
        print("\nPhase 3: Extracting WEM files from BNK files...")
        
        # Create output directories
        wem_dir = os.path.join(self.output_dir, "WEM_Files")
        bnk_dir = os.path.join(self.output_dir, "BNK_Files")
        os.makedirs(wem_dir, exist_ok=True)
        os.makedirs(bnk_dir, exist_ok=True)
        
        with open(self.pck_file, 'rb') as f:
            for bnk in bnk_files:
                # Read BNK data
                f.seek(bnk['offset'])
                bnk_data = f.read(bnk['size'])
                
                # Save BNK file
                bnk_filename = f"bank_{bnk['index']:03d}.bnk"
                bnk_path = os.path.join(bnk_dir, bnk_filename)
                with open(bnk_path, 'wb') as out:
                    out.write(bnk_data)
                
                self.stats['bnk_files'] += 1
                
                # Extract WEM files
                wem_files = self.extract_wems_from_bnk(bnk['index'], bnk_data)
                
                for wem in wem_files:
                    wem_filename = f"{wem['id']:08X}.wem"
                    wem_path = os.path.join(wem_dir, wem_filename)
                    
                    with open(wem_path, 'wb') as out:
                        out.write(wem['data'])
                
                if (bnk['index'] + 1) % 50 == 0:
                    print(f"  Processed {bnk['index'] + 1}/{len(bnk_files)} BNK files...")
        
        print(f"  ✓ Processed all {len(bnk_files)} BNK files")
        
        # Print statistics
        self.print_statistics()
    
    def print_statistics(self):
        """Print extraction statistics"""
        print("\n" + "="*80)
        print("EXTRACTION STATISTICS")
        print("="*80)
        print(f"BNK files extracted: {self.stats['bnk_files']}")
        print(f"  - With audio data: {self.stats['bnk_with_audio']}")
        print(f"  - Metadata only: {self.stats['bnk_metadata_only']}")
        print(f"\nWEM files extracted: {self.stats['wem_files']}")
        print(f"Total WEM size: {self.stats['total_wem_size'] / 1e9:.2f} GB")
        print(f"Average WEM size: {self.stats['total_wem_size'] / self.stats['wem_files'] / 1e3:.2f} KB" if self.stats['wem_files'] > 0 else "N/A")
        print("="*80)

if __name__ == '__main__':
    extractor = DeepBNKExtractor(
        pck_file='Audio/sound.pck',
        output_dir='Audio/PROPER_EXTRACTION'
    )
    extractor.extract_all()

