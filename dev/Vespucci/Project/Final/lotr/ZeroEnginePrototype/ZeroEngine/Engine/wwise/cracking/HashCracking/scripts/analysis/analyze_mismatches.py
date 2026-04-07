#!/usr/bin/env python3
"""
Analyze mismatched audio event mappings by finding co-occurrence patterns.

For events that appear in wrong contexts (e.g., Level_Shire in Helm's Deep),
this script finds what other events occur at the same timestamps to help
identify what the events actually are.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

def parse_log(filepath: str) -> list:
    """Parse log file into list of (timestamp_ms, event_name) tuples."""
    events = []
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            # Match: 12345ms | EventName | optional_semantic
            match = re.match(r'(\d+)ms\s*\|\s*([^\|]+)', line)
            if match:
                ts = int(match.group(1))
                event = match.group(2).strip()
                events.append((ts, event))
    return events

def find_cooccurrences(events: list, target_pattern: str, window_ms: int = 50) -> dict:
    """Find events that co-occur with target pattern within time window."""
    cooccurrences = defaultdict(int)
    target_events = [(ts, ev) for ts, ev in events if re.search(target_pattern, ev)]
    
    for target_ts, target_ev in target_events:
        # Find events within window
        for ts, ev in events:
            if abs(ts - target_ts) <= window_ms and ev != target_ev:
                # Normalize the event name for counting
                cooccurrences[ev] += 1
    
    return dict(sorted(cooccurrences.items(), key=lambda x: -x[1])[:30])

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_mismatches.py <log_file>")
        sys.exit(1)
    
    filepath = sys.argv[1]
    print(f"Analyzing: {filepath}\n")
    
    events = parse_log(filepath)
    print(f"Parsed {len(events)} events\n")
    
    # Analyze each suspect pattern
    suspects = [
        ("Level_Shire", "Level_Shire"),
        ("HeroLurtz", "HeroLurtz"),
        ("SFXTroll::0x4BF68CF3", r"SFXTroll::0x4BF68CF3"),
        ("SFXTroll::0xA5D460EA", r"SFXTroll::0xA5D460EA"),
    ]
    
    for name, pattern in suspects:
        print(f"=" * 60)
        print(f"CO-OCCURRENCES for: {name}")
        print(f"=" * 60)
        cooc = find_cooccurrences(events, pattern)
        if not cooc:
            print("  No matches found")
        else:
            for ev, count in list(cooc.items())[:15]:
                print(f"  {count:5d}x  {ev}")
        print()

if __name__ == '__main__':
    main()

