#!/usr/bin/env python3
"""
Analyze WWiseIDTable.audio.json and extract all strings from sound.pck
that match the readable keys in the ID table.
"""
import json
import re
import os

def load_wwise_id_table(json_path):
    """Load the WWiseIDTable.audio.json file"""
    with open(json_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    return data

def extract_readable_keys(wwise_data):
    """Extract all non-hex keys (readable strings) from the ID table"""
    readable_keys = []
    hex_keys = []
    
    for obj in wwise_data.get('obj1s', []):
        key = obj.get('key', '')
        val = obj.get('val', 0)
        
        # Check if key starts with 0x (hex format)
        if key.startswith('0x'):
            hex_keys.append((key, val))
        else:
            readable_keys.append((key, val))
    
    return readable_keys, hex_keys

def search_in_extracted_strings(search_terms, ascii_file, utf16_file):
    """Search for terms in the extracted string files"""
    found_strings = {}
    
    # Search in ASCII strings
    if os.path.exists(ascii_file):
        print(f"[*] Searching in {ascii_file}...")
        with open(ascii_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                for term, val in search_terms:
                    if term.lower() in line.lower():
                        if term not in found_strings:
                            found_strings[term] = []
                        found_strings[term].append({
                            'line': line,
                            'file': 'ASCII',
                            'line_num': line_num,
                            'wwise_id': val
                        })
    
    # Search in UTF16 strings
    if os.path.exists(utf16_file):
        print(f"[*] Searching in {utf16_file}...")
        with open(utf16_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line_num, line in enumerate(f, 1):
                line = line.strip()
                for term, val in search_terms:
                    if term.lower() in line.lower():
                        if term not in found_strings:
                            found_strings[term] = []
                        found_strings[term].append({
                            'line': line,
                            'file': 'UTF16',
                            'line_num': line_num,
                            'wwise_id': val
                        })
    
    return found_strings

def extract_all_meaningful_strings(ascii_file, min_length=4):
    """Extract all strings that look meaningful (alphanumeric with underscores)"""
    meaningful_strings = set()
    pattern = re.compile(r'^0x[0-9A-F]+:\s+([a-zA-Z_][a-zA-Z0-9_]{' + str(min_length-1) + r',})$')
    
    if os.path.exists(ascii_file):
        print(f"[*] Extracting meaningful strings from {ascii_file}...")
        with open(ascii_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                match = pattern.match(line.strip())
                if match:
                    meaningful_strings.add(match.group(1))
    
    return sorted(meaningful_strings)

def main():
    # Paths
    wwise_json = 'WWiseIDTable.audio.json'
    ascii_strings = 'strings_extracted/sound_strings_ascii.txt'
    utf16_strings = 'strings_extracted/sound_strings_utf16.txt'
    output_report = 'wwise_string_analysis_report.txt'
    output_all_strings = 'all_meaningful_strings.txt'
    
    print("[+] Loading WWiseIDTable.audio.json...")
    wwise_data = load_wwise_id_table(wwise_json)
    
    print("[+] Extracting readable keys...")
    readable_keys, hex_keys = extract_readable_keys(wwise_data)
    
    print(f"[+] Found {len(readable_keys)} readable keys")
    print(f"[+] Found {len(hex_keys)} hex keys")
    
    print("\n[+] Readable keys from WWiseIDTable:")
    for key, val in readable_keys[:50]:  # Show first 50
        print(f"    {key} -> {val}")
    if len(readable_keys) > 50:
        print(f"    ... and {len(readable_keys) - 50} more")
    
    print("\n[+] Searching for readable keys in extracted strings...")
    found = search_in_extracted_strings(readable_keys, ascii_strings, utf16_strings)
    
    print(f"\n[+] Found {len(found)} matching terms")
    
    # Extract all meaningful strings
    print("\n[+] Extracting all meaningful strings from sound.pck...")
    all_meaningful = extract_all_meaningful_strings(ascii_strings, min_length=4)
    print(f"[+] Found {len(all_meaningful)} meaningful strings")
    
    # Write report
    print(f"\n[+] Writing report to {output_report}...")
    with open(output_report, 'w', encoding='utf-8') as f:
        f.write("=" * 80 + "\n")
        f.write("WWISE STRING ANALYSIS REPORT\n")
        f.write("=" * 80 + "\n\n")
        
        f.write(f"Total readable keys in WWiseIDTable: {len(readable_keys)}\n")
        f.write(f"Total hex keys in WWiseIDTable: {len(hex_keys)}\n")
        f.write(f"Total matches found in sound.pck: {len(found)}\n\n")
        
        f.write("=" * 80 + "\n")
        f.write("READABLE KEYS FROM WWiseIDTable.audio.json\n")
        f.write("=" * 80 + "\n\n")
        for key, val in readable_keys:
            f.write(f"{key:<40} -> Wwise ID: {val}\n")
        
        f.write("\n" + "=" * 80 + "\n")
        f.write("MATCHES FOUND IN sound.pck\n")
        f.write("=" * 80 + "\n\n")
        
        if found:
            for term in sorted(found.keys()):
                f.write(f"\n--- {term} ---\n")
                for match in found[term][:10]:  # Limit to 10 matches per term
                    f.write(f"  [{match['file']}] Line {match['line_num']}: {match['line']}\n")
                    f.write(f"  Wwise ID: {match['wwise_id']}\n")
                if len(found[term]) > 10:
                    f.write(f"  ... and {len(found[term]) - 10} more matches\n")
        else:
            f.write("No direct matches found.\n")
    
    # Write all meaningful strings
    print(f"[+] Writing all meaningful strings to {output_all_strings}...")
    with open(output_all_strings, 'w', encoding='utf-8') as f:
        f.write("=" * 80 + "\n")
        f.write("ALL MEANINGFUL STRINGS EXTRACTED FROM sound.pck\n")
        f.write("=" * 80 + "\n")
        f.write(f"Total: {len(all_meaningful)} strings\n")
        f.write("=" * 80 + "\n\n")
        for s in all_meaningful:
            f.write(s + "\n")
    
    print(f"\n[+] Done!")
    print(f"[+] Reports saved:")
    print(f"    - {output_report}")
    print(f"    - {output_all_strings}")

if __name__ == '__main__':
    main()

