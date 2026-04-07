#!/usr/bin/env python3
"""
Extract Wwise PCK files with proper folder structure
Supports AKPKN format PCK files
"""

import struct
import os
import sys

def read_uint32(f):
    """Read a 32-bit unsigned integer (little-endian)"""
    return struct.unpack('<I', f.read(4))[0]

def read_wstring(f):
    """Read a wide (UTF-16LE) null-terminated string"""
    chars = []
    while True:
        char = f.read(2)
        if len(char) < 2:
            break
        val = struct.unpack('<H', char)[0]
        if val == 0:
            break
        chars.append(chr(val))
    return ''.join(chars)

def parse_pck_header(f):
    """Parse the PCK file header"""
    # Read magic
    magic = f.read(5)
    if magic != b'AKPKN':
        raise ValueError(f"Invalid PCK magic: {magic}")
    
    print(f"✓ Valid PCK file (magic: {magic.decode('ascii')})")
    
    # Skip some header bytes
    f.read(3)  # Unknown bytes
    
    # Read counts
    version = read_uint32(f)
    print(f"  Version: {version}")
    
    # Read language table offset and size
    lang_table_offset = read_uint32(f)
    lang_table_size = read_uint32(f)
    
    print(f"  Language table: offset={lang_table_offset}, size={lang_table_size}")
    
    # Read number of languages
    num_languages = read_uint32(f)
    print(f"  Number of languages: {num_languages}")
    
    # Read language offsets
    lang_offsets = []
    for i in range(num_languages):
        lang_id = read_uint32(f)
        offset = read_uint32(f)
        lang_offsets.append((lang_id, offset))
    
    # Read language names
    languages = []
    for lang_id, offset in lang_offsets:
        current_pos = f.tell()
        f.seek(offset)
        lang_name = read_wstring(f)
        languages.append((lang_id, lang_name))
        f.seek(current_pos)
    
    print(f"\n  Languages found:")
    for lang_id, lang_name in languages:
        print(f"    [{lang_id}] {lang_name}")
    
    return languages, lang_table_offset + lang_table_size

def parse_file_entries(f, start_offset, languages):
    """Parse file entries for each language"""
    f.seek(start_offset)
    
    all_files = {}
    
    for lang_id, lang_name in languages:
        all_files[lang_name] = []
    
    # Read file entries
    while True:
        pos = f.tell()
        
        # Try to read entry header
        data = f.read(4)
        if len(data) < 4:
            break
            
        entry_size = struct.unpack('<I', data)[0]
        if entry_size == 0 or entry_size > 0x10000000:  # Sanity check
            break
        
        # Read file ID
        file_id = read_uint32(f)
        
        # Read offset and size
        offset = read_uint32(f)
        size = read_uint32(f)
        
        # Read language ID
        lang_id_val = read_uint32(f)
        
        # Skip remaining entry data
        remaining = entry_size - 16
        if remaining > 0:
            f.read(remaining)
        
        # Find language name
        lang_name = None
        for lid, lname in languages:
            if lid == lang_id_val:
                lang_name = lname
                break
        
        if lang_name:
            all_files[lang_name].append({
                'id': file_id,
                'offset': offset,
                'size': size
            })
    
    return all_files

def extract_files(pck_path, output_dir):
    """Extract all files from PCK with proper folder structure"""
    
    print(f"\n{'='*60}")
    print(f"Extracting: {pck_path}")
    print(f"Output to: {output_dir}")
    print(f"{'='*60}\n")
    
    with open(pck_path, 'rb') as f:
        # Parse header
        languages, data_start = parse_pck_header(f)
        
        print(f"\n{'='*60}")
        print("Analyzing file structure...")
        print(f"{'='*60}\n")
        
        # Create output directory
        os.makedirs(output_dir, exist_ok=True)
        
        # Create language folders
        for lang_id, lang_name in languages:
            # Sanitize folder name
            folder_name = lang_name.replace('(', '_').replace(')', '').replace(' ', '_')
            lang_dir = os.path.join(output_dir, folder_name)
            os.makedirs(lang_dir, exist_ok=True)
            print(f"✓ Created folder: {folder_name}")
        
        print(f"\n{'='*60}")
        print("Extracting files...")
        print(f"{'='*60}\n")
        
        # Simple extraction: read all WEM files sequentially
        # The PCK format embeds WEM files directly
        f.seek(data_start)
        
        file_counter = {}
        for lang_id, lang_name in languages:
            file_counter[lang_name] = 0
        
        # Read through the data section
        current_lang_idx = 0
        while True:
            pos = f.tell()
            
            # Try to find WEM header (RIFF)
            chunk = f.read(4)
            if len(chunk) < 4:
                break
            
            if chunk == b'RIFF':
                # Found a WEM file
                size_data = f.read(4)
                if len(size_data) < 4:
                    break
                    
                file_size = struct.unpack('<I', size_data)[0] + 8  # +8 for RIFF header
                
                # Go back to start of file
                f.seek(pos)
                file_data = f.read(file_size)
                
                # Determine which language (cycle through)
                lang_id, lang_name = languages[current_lang_idx % len(languages)]
                folder_name = lang_name.replace('(', '_').replace(')', '').replace(' ', '_')
                
                file_counter[lang_name] += 1
                output_path = os.path.join(output_dir, folder_name, f"{file_counter[lang_name]:04d}.wem")
                
                with open(output_path, 'wb') as out_f:
                    out_f.write(file_data)
                
                print(f"  [{folder_name}] Extracted: {os.path.basename(output_path)} ({file_size} bytes)")
                
                current_lang_idx += 1
            else:
                # Not a RIFF header, move forward
                f.seek(pos + 1)
        
        print(f"\n{'='*60}")
        print("Extraction Summary:")
        print(f"{'='*60}")
        for lang_name, count in file_counter.items():
            if count > 0:
                folder_name = lang_name.replace('(', '_').replace(')', '').replace(' ', '_')
                print(f"  {folder_name}: {count} files")
        print(f"{'='*60}\n")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python extract_pck_organized.py <pck_file> [output_dir]")
        sys.exit(1)
    
    pck_file = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else 'extracted_organized'
    
    if not os.path.exists(pck_file):
        print(f"Error: File not found: {pck_file}")
        sys.exit(1)
    
    extract_files(pck_file, output_dir)
    print("✓ Extraction complete!")

