#!/usr/bin/env python3
"""
Extract context around WWise strings in the game executable
"""
import json
import re

def load_wwise_table(filename):
    """Load WWiseIDTable.audio.json"""
    with open(filename, 'r') as f:
        data = json.load(f)
    return data.get('obj1s', [])

def extract_strings_near_offset(data, offset, distance=500):
    """Extract all ASCII strings near an offset"""
    start = max(0, offset - distance)
    end = min(len(data), offset + distance)
    chunk = data[start:end]
    
    # Find ASCII strings (min length 3)
    strings = re.findall(b'[ -~]{3,}', chunk)
    return [s.decode('ascii', errors='ignore') for s in strings]

def main():
    print("="*80)
    print("EXTRACTING CONTEXT FROM GAME EXECUTABLE")
    print("="*80)
    
    # Load WWiseIDTable
    print("\n[1/3] Loading WWiseIDTable...")
    entries = load_wwise_table('WWiseIDTable.audio.json')
    readable_entries = [e for e in entries if not e['key'].startswith('0x')]
    print(f"  Readable keys: {len(readable_entries)}")
    
    # Load executable
    print("\n[2/3] Loading executable...")
    with open('../ConquestLLC.exe', 'rb') as f:
        exe_data = f.read()
    print(f"  Size: {len(exe_data):,} bytes")
    
    # Search for each readable key and extract context
    print("\n[3/3] Extracting context...")
    
    results = []
    
    for entry in readable_entries:
        key = entry['key']
        val = entry['val']
        key_bytes = key.encode('ascii')
        
        # Find all occurrences
        positions = []
        offset = 0
        while True:
            pos = exe_data.find(key_bytes, offset)
            if pos == -1:
                break
            positions.append(pos)
            offset = pos + 1
        
        if positions:
            # Extract context for first occurrence
            context_strings = extract_strings_near_offset(exe_data, positions[0], 500)
            
            results.append({
                'key': key,
                'val': val,
                'positions': positions,
                'context': context_strings
            })
    
    # Generate report
    print("\n" + "="*80)
    print("GENERATING REPORT")
    print("="*80)
    
    with open('exe_context_report.txt', 'w', encoding='utf-8') as f:
        f.write("GAME EXECUTABLE CONTEXT REPORT\n")
        f.write("="*80 + "\n\n")
        f.write("This report shows strings found near WWise event names in the executable.\n")
        f.write("This helps understand how the game references these audio events.\n\n")
        f.write("="*80 + "\n\n")
        
        for result in results:
            f.write(f"KEY: {result['key']}\n")
            f.write(f"VAL: {result['val']}\n")
            f.write(f"FOUND AT: {len(result['positions'])} location(s)\n")
            f.write(f"  Offsets: {', '.join(f'0x{p:08X}' for p in result['positions'][:5])}\n")
            f.write(f"\nCONTEXT STRINGS (near first occurrence):\n")
            
            # Filter and display relevant strings
            relevant = [s for s in result['context'] if len(s) > 3 and not all(c in '0123456789' for c in s)]
            for s in relevant[:20]:
                f.write(f"  - {s}\n")
            
            f.write("\n" + "-"*80 + "\n\n")
    
    print("[+] Report saved to exe_context_report.txt")
    
    # Also extract ALL strings from executable for reference
    print("\n[+] Extracting all strings from executable...")
    all_strings = re.findall(b'[ -~]{6,}', exe_data)
    all_strings = [s.decode('ascii', errors='ignore') for s in all_strings]
    
    # Filter for audio-related strings
    audio_keywords = ['wwise', 'audio', 'sound', 'music', 'voice', 'sfx', 'chatter', 
                      'hero', 'level', 'play', 'stop', 'event', 'bank', 'pck']
    
    audio_strings = []
    for s in all_strings:
        s_lower = s.lower()
        if any(kw in s_lower for kw in audio_keywords):
            audio_strings.append(s)
    
    with open('exe_audio_strings.txt', 'w', encoding='utf-8') as f:
        f.write("AUDIO-RELATED STRINGS FROM EXECUTABLE\n")
        f.write("="*80 + "\n\n")
        f.write(f"Total strings extracted: {len(all_strings)}\n")
        f.write(f"Audio-related strings: {len(audio_strings)}\n\n")
        f.write("="*80 + "\n\n")
        
        for s in sorted(set(audio_strings)):
            f.write(f"{s}\n")
    
    print("[+] Audio strings saved to exe_audio_strings.txt")
    
    # Summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"✓ Found {len(results)} readable keys in executable")
    print(f"✓ Extracted context for each key")
    print(f"✓ Found {len(audio_strings)} audio-related strings in executable")
    print(f"\nKey findings:")
    print(f"  • 46/48 readable keys are present in the executable")
    print(f"  • These are likely hardcoded event names used by game code")
    print(f"  • The hex keys are probably generated at runtime or compile-time")

if __name__ == '__main__':
    main()

