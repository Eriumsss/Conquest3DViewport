#!/usr/bin/env python3
"""
Footstep Filter Script for LOTR Conquest Audio Logs

Removes footstep events from captured audio logs, EXCEPT for:
- Hero character footsteps (Lurtz, Sauron, Aragorn, etc.)
- Large creature footsteps (Troll, Ent, Balrog, Oliphaunt, etc.)

Usage:
    python filter_footsteps.py <input_log> [output_log]
    
If output_log is not specified, outputs to <input_log>_filtered.txt
"""

import sys
import re
from pathlib import Path

# Patterns that indicate a footstep event
FOOTSTEP_PATTERNS = [
    r'\bEffects-0717\b',           # Generic footstep event
    r'\bfootstep\b',               # Named footstep
    r'\btroll_footstep\b',         # Troll footstep (keep for analysis but mark)
]

# Hero/Large creature banks - KEEP their footsteps
HERO_LARGE_CREATURE_BANKS = [
    'HeroSauron', 'HeroAragorn', 'HeroGandalf', 'HeroLegolas', 'HeroGimli',
    'HeroFrodo', 'HeroSaruman', 'HeroNazgul', 'HeroEowyn', 'HeroElrond',
    'HeroIsildur', 'HeroBoromir', 'HeroFaramir', 'HeroArwen', 'HeroLurtz',
    'HeroWitchKing', 'HeroWormtongue', 'HeroMouth', 'HeroTheoden', 'HeroGothmog',
    'HeroTreebeard', 'HeroBalrog',
    # Large creatures
    'SFXTroll', 'SFXEnt', 'SFXBalrog', 'SFXOliphaunt', 'SFXEagle', 'SFXFellBeast',
]

def is_footstep_line(line: str) -> bool:
    """Check if a line contains a footstep event."""
    for pattern in FOOTSTEP_PATTERNS:
        if re.search(pattern, line, re.IGNORECASE):
            return True
    return False

def is_hero_or_large_creature(line: str) -> bool:
    """Check if the line is from a hero or large creature bank."""
    for bank in HERO_LARGE_CREATURE_BANKS:
        if bank in line:
            return True
    return False

def filter_log(input_path: str, output_path: str = None) -> dict:
    """
    Filter footstep events from the log file.
    
    Returns stats about filtering.
    """
    input_file = Path(input_path)
    if not input_file.exists():
        print(f"Error: Input file not found: {input_path}")
        sys.exit(1)
    
    if output_path is None:
        output_path = input_file.stem + "_filtered" + input_file.suffix
    
    stats = {
        'total_lines': 0,
        'footsteps_removed': 0,
        'footsteps_kept': 0,
        'other_events': 0,
    }
    
    filtered_lines = []
    
    with open(input_file, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            stats['total_lines'] += 1
            
            # Keep header/comment lines
            if line.startswith('#') or line.startswith('=') or not line.strip():
                filtered_lines.append(line)
                continue
            
            # Check if it's a footstep
            if is_footstep_line(line):
                # Keep hero/large creature footsteps
                if is_hero_or_large_creature(line):
                    filtered_lines.append(line)
                    stats['footsteps_kept'] += 1
                else:
                    stats['footsteps_removed'] += 1
            else:
                filtered_lines.append(line)
                stats['other_events'] += 1
    
    # Write filtered output
    with open(output_path, 'w', encoding='utf-8') as f:
        f.writelines(filtered_lines)
    
    return stats, output_path

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    
    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else None
    
    print(f"Filtering footsteps from: {input_path}")
    stats, out_file = filter_log(input_path, output_path)
    
    print(f"\n=== Filtering Results ===")
    print(f"Total lines:        {stats['total_lines']}")
    print(f"Footsteps removed:  {stats['footsteps_removed']}")
    print(f"Footsteps kept:     {stats['footsteps_kept']} (hero/large creature)")
    print(f"Other events:       {stats['other_events']}")
    print(f"\nFiltered output:    {out_file}")
    print(f"Reduction:          {stats['footsteps_removed'] / max(1, stats['total_lines']) * 100:.1f}%")

if __name__ == '__main__':
    main()

