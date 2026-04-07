#!/usr/bin/env python3
"""
Fast string extraction from sound.pck
"""
import mmap
import re

print("="*80)
print("FAST STRING EXTRACTION FROM sound.pck")
print("="*80)

print("\n[1/2] Opening sound.pck...")

with open('sound.pck', 'rb') as f:
    with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        file_size = len(data)
        print(f"  File size: {file_size:,} bytes ({file_size/1024/1024:.2f} MB)")
        
        print("\n[2/2] Extracting null-terminated ASCII strings (min length 5)...")
        
        # Extract all null-terminated strings
        strings = set()
        current_str = b''
        
        chunk_size = 10 * 1024 * 1024  # 10 MB chunks
        num_chunks = (file_size + chunk_size - 1) // chunk_size
        
        for chunk_idx in range(num_chunks):
            if chunk_idx % 10 == 0:
                print(f"  Processing chunk {chunk_idx+1}/{num_chunks} ({chunk_idx*chunk_size/1024/1024:.0f} MB)...")
            
            start = chunk_idx * chunk_size
            end = min(start + chunk_size, file_size)
            chunk = data[start:end]
            
            for byte in chunk:
                if 32 <= byte <= 126:  # Printable ASCII
                    current_str += bytes([byte])
                elif byte == 0 and len(current_str) >= 5:  # Null terminator
                    try:
                        s = current_str.decode('ascii')
                        # Filter out likely garbage
                        if not re.search(r'[^\w\s_\-\.\(\)]', s):
                            strings.add(s)
                    except:
                        pass
                    current_str = b''
                else:
                    current_str = b''
        
        print(f"\n  Total unique strings: {len(strings)}")

# Save to file
with open('sound_pck_strings.txt', 'w', encoding='utf-8') as f:
    f.write("STRINGS EXTRACTED FROM sound.pck\n")
    f.write("="*80 + "\n\n")
    f.write(f"Total unique strings: {len(strings)}\n\n")
    f.write("="*80 + "\n\n")
    
    for s in sorted(strings):
        f.write(f"{s}\n")

print(f"\n✓ Saved to sound_pck_strings.txt")

# Show sample
print(f"\nSample strings (first 30):")
for s in sorted(strings)[:30]:
    print(f"  {s}")

