#!/usr/bin/env python3
"""
LOTR Conquest Audio Event Correlation Analyzer
Parses captured_audio_names.txt and builds co-occurrence matrix.
"""

import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Tuple

@dataclass
class AudioEvent:
    timestamp_ms: int
    txtp_name: str
    event_name: str

def parse_log(filepath: str) -> List[AudioEvent]:
    """Parse captured_audio_names.txt EVENT LOG section."""
    events = []
    in_event_section = False
    
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if 'EVENT LOG' in line:
                in_event_section = True
                continue
            if not in_event_section or not line or line.startswith('#'):
                continue
            
            # Parse: timestamp_ms | TXTP_name | event_name
            match = re.match(r'(\d+)ms\s*\|\s*([^\|]+)\s*\|\s*(.*)', line)
            if match:
                events.append(AudioEvent(
                    timestamp_ms=int(match.group(1)),
                    txtp_name=match.group(2).strip(),
                    event_name=match.group(3).strip()
                ))
    return events

def find_correlations(events: List[AudioEvent], window_ms: int = 50) -> Dict[str, Dict[str, int]]:
    """Find events that fire within window_ms of each other."""
    correlations = defaultdict(lambda: defaultdict(int))
    
    for i, event_a in enumerate(events):
        for j in range(i + 1, len(events)):
            event_b = events[j]
            delta = event_b.timestamp_ms - event_a.timestamp_ms
            if delta > window_ms:
                break  # Events are sorted by time
            if event_a.txtp_name != event_b.txtp_name:
                correlations[event_a.txtp_name][event_b.txtp_name] += 1
                correlations[event_b.txtp_name][event_a.txtp_name] += 1
    
    return correlations

def calculate_confidence(correlations: Dict, event_counts: Dict[str, int]) -> List[Tuple]:
    """Calculate confidence scores for correlations."""
    results = []
    for event_a, related in correlations.items():
        for event_b, count in related.items():
            total_a = event_counts.get(event_a, count)
            confidence = (count / total_a) * 100 if total_a > 0 else 0
            results.append((event_a, event_b, count, total_a, confidence))
    return sorted(results, key=lambda x: (-x[4], -x[2]))  # Sort by confidence, then count

def main():
    filepath = sys.argv[1] if len(sys.argv) > 1 else 'captured_audio_names.txt'
    window_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    
    print(f"Parsing {filepath}...")
    events = parse_log(filepath)
    print(f"Found {len(events)} events")
    
    # Count event occurrences
    event_counts = defaultdict(int)
    for e in events:
        event_counts[e.txtp_name] += 1
    
    print(f"\nFinding correlations (window={window_ms}ms)...")
    correlations = find_correlations(events, window_ms)
    
    print("\n" + "="*80)
    print("HIGH CONFIDENCE CORRELATIONS (>= 90%)")
    print("="*80)
    print(f"{'Event A':<30} {'Event B':<30} {'Count':>6} {'Total':>6} {'Conf%':>6}")
    print("-"*80)
    
    results = calculate_confidence(correlations, event_counts)
    for event_a, event_b, count, total, conf in results:
        if conf >= 90 and count >= 3:
            print(f"{event_a:<30} {event_b:<30} {count:>6} {total:>6} {conf:>5.1f}%")
    
    # Filter for Hero events
    print("\n" + "="*80)
    print("HERO EVENT CORRELATIONS")
    print("="*80)
    for event_a, event_b, count, total, conf in results:
        if 'Hero' in event_a and conf >= 50 and count >= 2:
            print(f"{event_a:<30} {event_b:<30} {count:>6} {total:>6} {conf:>5.1f}%")

if __name__ == '__main__':
    main()

