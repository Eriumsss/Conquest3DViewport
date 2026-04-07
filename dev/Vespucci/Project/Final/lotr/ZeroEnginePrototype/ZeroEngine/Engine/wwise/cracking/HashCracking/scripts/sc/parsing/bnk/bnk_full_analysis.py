#!/usr/bin/env python3
"""
Complete BNK Analysis - Parse STID, HIRC, DIDX sections and map to WWiseIDTable
"""
import struct
import json
import glob
import os
from collections import defaultdict

def parse_hirc_section(data, offset, endian):
    """Parse HIRC section - contains event hierarchy"""
    try:
        hirc_offset = offset + 8
        num_objects = struct.unpack(f'{endian}I', data[hirc_offset:hirc_offset+4])[0]
        
        objects = []
        obj_offset = hirc_offset + 4
        
        for i in range(num_objects):
            if obj_offset >= len(data) - 5:
                break
            
            obj_type = data[obj_offset]
            obj_id = struct.unpack(f'{endian}I', data[obj_offset+1:obj_offset+5])[0]
            obj_size = struct.unpack(f'{endian}I', data[obj_offset+5:obj_offset+9])[0]
            
            objects.append({
                'type': obj_type,
                'id': obj_id,
                'size': obj_size,
                'offset': obj_offset
            })
            
            obj_offset += 9 + obj_size
        
        return objects
    except Exception as e:
        return []

def parse_didx_section(data, offset, endian):
    """Parse DIDX section - maps event IDs to WEM data offsets/sizes"""
    try:
        didx_offset = offset + 8
        num_entries = struct.unpack(f'{endian}I', data[didx_offset:didx_offset+4])[0]
        
        entries = {}
        entry_offset = didx_offset + 4
        
        for i in range(num_entries):
            if entry_offset >= len(data) - 12:
                break
            
            wem_id = struct.unpack(f'{endian}I', data[entry_offset:entry_offset+4])[0]
            wem_offset = struct.unpack(f'{endian}I', data[entry_offset+4:entry_offset+8])[0]
            wem_size = struct.unpack(f'{endian}I', data[entry_offset+8:entry_offset+12])[0]
            
            entries[wem_id] = {
                'offset': wem_offset,
                'size': wem_size
            }
            
            entry_offset += 12
        
        return entries
    except Exception as e:
        return {}

def parse_stid_section(data, offset, endian):
    """Parse STID section - contains bank organization names"""
    try:
        stid_offset = offset + 8
        unknown = struct.unpack(f'{endian}I', data[stid_offset:stid_offset+4])[0]
        num_entries = struct.unpack(f'{endian}I', data[stid_offset+4:stid_offset+8])[0]
        
        entries = []
        entry_offset = stid_offset + 8
        
        for i in range(num_entries):
            if entry_offset >= offset + 8 + struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]:
                break
            
            entry_id = struct.unpack(f'{endian}I', data[entry_offset:entry_offset+4])[0]
            entry_offset += 4
            
            name_bytes = b''
            while entry_offset < len(data) and data[entry_offset] != 0:
                name_bytes += bytes([data[entry_offset]])
                entry_offset += 1
            
            entry_offset += 1
            
            try:
                name = name_bytes.decode('ascii')
                entries.append({'id': entry_id, 'name': name})
            except:
                pass
        
        return entries
    except Exception as e:
        return []

def parse_bnk_file(filepath):
    """Parse a complete BNK file"""
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
        
        # Check BKHD signature
        if data[0:4] != b'BKHD':
            return None
        
        # Determine endianness
        bkhd_size_le = struct.unpack('<I', data[4:8])[0]
        bkhd_size_be = struct.unpack('>I', data[4:8])[0]
        
        if bkhd_size_le < 10000:
            endian = '<'
            bkhd_size = bkhd_size_le
        else:
            endian = '>'
            bkhd_size = bkhd_size_be
        
        # Get bank ID
        bank_id = struct.unpack(f'{endian}I', data[12:16])[0]
        
        # Parse sections
        sections = {}
        offset = 8 + bkhd_size
        
        while offset < len(data) - 8:
            section_sig = data[offset:offset+4]
            
            if not all(32 <= b <= 126 for b in section_sig):
                break
            
            section_size = struct.unpack(f'{endian}I', data[offset+4:offset+8])[0]
            
            if section_size > len(data) - offset or section_size < 0:
                break
            
            section_name = section_sig.decode('ascii')
            sections[section_name] = {
                'offset': offset,
                'size': section_size,
                'data_offset': offset + 8
            }
            
            offset += 8 + section_size
        
        # Parse specific sections
        result = {
            'filepath': filepath,
            'bank_id': bank_id,
            'endian': endian,
            'stid': [],
            'hirc': [],
            'didx': {}
        }
        
        if 'STID' in sections:
            result['stid'] = parse_stid_section(data, sections['STID']['offset'], endian)
        
        if 'HIRC' in sections:
            result['hirc'] = parse_hirc_section(data, sections['HIRC']['offset'], endian)
        
        if 'DIDX' in sections:
            result['didx'] = parse_didx_section(data, sections['DIDX']['offset'], endian)
        
        return result
    
    except Exception as e:
        print(f"[FAIL] {filepath}: {e}")
        return None

# Main execution
if __name__ == '__main__':
    print("Starting BNK analysis...")
    
    # Parse all BNK files
    bnk_files = sorted(glob.glob('extracted_bnk/bank_*.bnk'))
    print(f"Found {len(bnk_files)} BNK files")
    
    all_data = []
    for i, filepath in enumerate(bnk_files):
        result = parse_bnk_file(filepath)
        if result:
            all_data.append(result)
            if (i + 1) % 50 == 0:
                print(f"  Processed {i + 1}/{len(bnk_files)}")
    
    print(f"Successfully parsed {len(all_data)} BNK files")
    
    # Save intermediate results
    with open('bnk_analysis_intermediate.json', 'w') as f:
        json.dump(all_data, f, indent=2, default=str)
    
    print("Saved intermediate results to bnk_analysis_intermediate.json")

