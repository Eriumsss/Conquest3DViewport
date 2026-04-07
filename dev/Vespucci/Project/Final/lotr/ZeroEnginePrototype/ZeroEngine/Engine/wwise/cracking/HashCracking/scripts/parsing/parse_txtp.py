#!/usr/bin/env python3
"""
Parse BNK files, TXTP files, and optional overrides.csv to generate event_mapping.h.

Sources (priority):
  1. BNK files (v34) - authoritative for event_id -> bankName
  2. TXTP files - initial names (e.g., "BaseCombat-0705")
  3. overrides.csv - manual name fixes (authoritative for names)

Output: event_mapping.h with struct { DWORD id; const char* bank; const char* name; }
"""

import os
import re
import csv
import json
import struct
import argparse
from pathlib import Path
from datetime import datetime
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple, Set


@dataclass
class EventEntry:
    id: int
    bank: Optional[str]
    name: Optional[str]
    source: str  # "override" | "txtp" | "bank-only"


@dataclass
class GenerationSummary:
    total_events: int
    from_txtp: int
    from_override: int
    bank_only: int
    failed_bnks: List[str]
    orphan_overrides: List[int]
    duplicate_overrides: List[int]


# =============================================================================
# BNK Parsing (Wwise v34)
# =============================================================================

def find_chunk(data: bytes, magic: bytes) -> Optional[int]:
    """Find offset of a chunk by its 4-byte magic."""
    pos = 0
    while pos < len(data) - 8:
        if data[pos:pos+4] == magic:
            return pos
        # Read chunk size and skip to next chunk
        if pos + 8 <= len(data):
            chunk_size = struct.unpack_from('<I', data, pos + 4)[0]
            pos += 8 + chunk_size
        else:
            break
    return None


def parse_stid(data: bytes, offset: int) -> Optional[str]:
    """
    Parse STID chunk to extract bank name.
    Layout:
      0: 'STID' (4 bytes)
      4: chunk_size (u32)
      8: ui_type (u32, should be 1)
     12: ui_count (u32)
     16+: entries (bank_id u32, str_len u8, name chars)
    """
    try:
        chunk_size = struct.unpack_from('<I', data, offset + 4)[0]
        if chunk_size < 8:
            return None
        count = struct.unpack_from('<I', data, offset + 12)[0]
        if count < 1:
            return None
        # Read first entry
        entry_start = offset + 16
        # bank_id = struct.unpack_from('<I', data, entry_start)[0]
        str_len = data[entry_start + 4]
        name = data[entry_start + 5 : entry_start + 5 + str_len].decode('utf-8', errors='ignore')
        return name
    except (struct.error, IndexError):
        return None


def parse_hirc_events(data: bytes, offset: int) -> List[int]:
    """
    Walk HIRC chunk and extract all CAkEvent (type 4) object IDs.
    Layout (Wwise v34):
      0: 'HIRC' (4 bytes)
      4: chunk_size (u32)
      8: num_objects (u32)
     12+: objects[]

    Each object:
      0: type (u32) - type 4 = CAkEvent
      4: object_size (u32) - size of data after this field
      8: object_id (u32)
     12+: type-specific data

    Total object size = 4 (type) + 4 (size) + object_size
    """
    event_ids = []
    try:
        chunk_size = struct.unpack_from('<I', data, offset + 4)[0]
        num_objects = struct.unpack_from('<I', data, offset + 8)[0]

        pos = offset + 12
        end = offset + 8 + chunk_size

        for _ in range(num_objects):
            if pos + 12 > end or pos + 12 > len(data):
                break
            obj_type = struct.unpack_from('<I', data, pos)[0]
            obj_size = struct.unpack_from('<I', data, pos + 4)[0]
            obj_id = struct.unpack_from('<I', data, pos + 8)[0]

            if obj_type == 4:  # CAkEvent
                event_ids.append(obj_id)

            # Skip: type(4) + size_field(4) + data(obj_size) = 8 + obj_size
            pos += 8 + obj_size
    except (struct.error, IndexError):
        pass

    return event_ids


def parse_bnk(bnk_path: Path) -> Tuple[Optional[str], List[int]]:
    """
    Parse a BNK file to extract bank name and event IDs.
    Returns (bank_name, [event_ids]).
    If no STID, uses filename as bank name.
    """
    try:
        with open(bnk_path, 'rb') as f:
            data = f.read()
    except IOError:
        return None, []

    # Try to find bank name from STID
    stid_offset = find_chunk(data, b'STID')
    if stid_offset is not None:
        bank_name = parse_stid(data, stid_offset)
    else:
        bank_name = None

    # Fallback to filename
    if not bank_name:
        bank_name = bnk_path.stem

    # Find and parse HIRC for events
    hirc_offset = find_chunk(data, b'HIRC')
    if hirc_offset is not None:
        event_ids = parse_hirc_events(data, hirc_offset)
    else:
        event_ids = []

    return bank_name, event_ids


def scan_bnks(bnk_dir: Path) -> Tuple[Dict[int, str], List[str]]:
    """
    Scan all .bnk files for CAkEvent IDs and bank names.
    Only scans root directory (skip language subfolders for now).

    Returns:
        - Dict[event_id, bank_name]
        - List of failed BNK filenames
    """
    bnk_events: Dict[int, str] = {}
    failed_bnks: List[str] = []

    for bnk_path in bnk_dir.glob('*.bnk'):
        bank_name, event_ids = parse_bnk(bnk_path)
        if bank_name is None:
            failed_bnks.append(bnk_path.name)
            continue

        for eid in event_ids:
            if eid not in bnk_events:
                bnk_events[eid] = bank_name

    return bnk_events, failed_bnks


def load_extracted_events_json(json_path: Path) -> Dict[int, str]:
    """
    Load event ID -> bank name mappings from extracted_events.json.
    This is generated by extract_events_from_xml.py from the organized BNK XMLs.

    Returns:
        - Dict[event_id, bank_name]
    """
    if not json_path.exists():
        return {}

    try:
        with open(json_path, 'r', encoding='utf-8') as f:
            data = json.load(f)

        events = {}
        for event_id_str, info in data.get('events', {}).items():
            event_id = int(event_id_str)
            bank_name = info.get('bank', 'Unknown')
            events[event_id] = bank_name

        return events
    except (json.JSONDecodeError, IOError, KeyError) as e:
        print(f"Warning: Failed to load {json_path}: {e}")
        return {}


# =============================================================================
# TXTP Parsing
# =============================================================================

def parse_txtp_file(filepath: Path) -> Optional[Tuple[int, str]]:
    """
    Parse a single TXTP file and extract (event_id, short_name).
    Filename pattern: BankName-NNNN-event*.txtp
    """
    filename = filepath.name

    # Extract bank name and event index
    match = re.match(r'^([^-]+)-(\d+)-event', filename)
    if not match:
        return None

    bank_name = match.group(1)
    event_index = int(match.group(2))
    short_name = f"{bank_name}-{event_index:04d}"

    # Read file and find CAkEvent line
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except IOError:
        return None

    event_match = re.search(r'CAkEvent\[\d+\]\s+(\d+)', content)
    if not event_match:
        return None

    event_id = int(event_match.group(1))
    return event_id, short_name


def scan_txtps(txtp_dir: Path) -> Dict[int, str]:
    """
    Parse TXTP files for event ID -> initial name.
    Returns Dict[event_id, txtp_name].
    """
    txtp_names: Dict[int, str] = {}

    for txtp_path in txtp_dir.glob('*.txtp'):
        result = parse_txtp_file(txtp_path)
        if result:
            event_id, name = result
            if event_id not in txtp_names:
                txtp_names[event_id] = name

    return txtp_names


# =============================================================================
# Override CSV Loading
# =============================================================================

def load_overrides(path: Path) -> Tuple[Dict[int, str], List[int]]:
    """
    Load manual name overrides from CSV.
    Format: id_hex,name (with header row)
    Example: 0x4BF68CF3,Play_SwordSwing

    Returns:
        - Dict[event_id, name]
        - List of duplicate IDs (for warning)
    """
    overrides: Dict[int, str] = {}
    duplicates: List[int] = []
    seen_ids: Set[int] = set()

    if not path.exists():
        return overrides, duplicates

    try:
        with open(path, 'r', encoding='utf-8') as f:
            reader = csv.reader(f)
            # Skip header
            next(reader, None)

            for row in reader:
                if len(row) < 2:
                    continue
                id_str, name = row[0].strip(), row[1].strip()
                if not id_str or not name:
                    continue

                # Parse hex ID (0xDEADBEEF format)
                try:
                    if id_str.lower().startswith('0x'):
                        event_id = int(id_str, 16)
                    else:
                        event_id = int(id_str)
                except ValueError:
                    continue

                if event_id in seen_ids:
                    duplicates.append(event_id)
                seen_ids.add(event_id)

                # Last row wins
                overrides[event_id] = name
    except IOError:
        pass

    return overrides, duplicates


# =============================================================================
# Merge Logic
# =============================================================================

def merge_mappings(
    bnk_events: Dict[int, str],
    txtp_names: Dict[int, str],
    overrides: Dict[int, str],
) -> Tuple[List[EventEntry], List[int]]:
    """
    Merge all sources into final event list.

    Rules:
        - Only emit entries for IDs present in bnk_events
        - bank = bnk_events[id]
        - name = overrides.get(id) or txtp_names.get(id) or None
        - source = "override" if in overrides else "txtp" if in txtp else "bank-only"

    Returns:
        - List[EventEntry] sorted by ID
        - List of orphan override IDs (not in any BNK)
    """
    entries: List[EventEntry] = []
    orphan_overrides: List[int] = []

    # Check for orphan overrides
    for eid in overrides:
        if eid not in bnk_events:
            orphan_overrides.append(eid)

    # Build entries for all BNK events
    for eid, bank in bnk_events.items():
        if eid in overrides:
            name = overrides[eid]
            source = "override"
        elif eid in txtp_names:
            name = txtp_names[eid]
            source = "txtp"
        else:
            name = None
            source = "bank-only"

        entries.append(EventEntry(id=eid, bank=bank, name=name, source=source))

    # Sort by ID
    entries.sort(key=lambda e: e.id)

    return entries, orphan_overrides


# =============================================================================
# C++ Header Generation
# =============================================================================

def write_header(
    entries: List[EventEntry],
    output_path: Path,
    summary: GenerationSummary,
    dry_run: bool = False
) -> None:
    """Emit event_mapping.h with the new struct format."""

    content = f"""// Auto-generated by parse_txtp.py - DO NOT EDIT
// Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
// Total: {summary.total_events} events (TXTP: {summary.from_txtp}, Override: {summary.from_override}, Bank-only: {summary.bank_only})

#pragma once

#include <Windows.h>
#include <unordered_map>
#include <string>
#include <cstdio>

// Event mapping entry with separate bank and name fields
struct EventMappingEntry {{
    DWORD id;
    const char* bank;  // Bank name (always present)
    const char* name;  // Event name (nullptr if unresolved)
}};

static const EventMappingEntry g_EventMappingData[] = {{
"""

    for e in entries:
        bank_lit = f'"{e.bank}"' if e.bank else 'nullptr'
        name_lit = f'"{e.name}"' if e.name else 'nullptr'
        content += f'    {{0x{e.id:08X}U, {bank_lit}, {name_lit}}}, // {e.source}\n'

    content += f"""}};

static const size_t g_EventMappingCount = {len(entries)};

// Helper function to build the lookup map (call once at init)
// Generates display labels: "EventName" or "BankName::0xHEXID"
inline void BuildEventMappingTable(std::unordered_map<DWORD, std::string>& outMap) {{
    outMap.clear();
    outMap.reserve(g_EventMappingCount);

    for (size_t i = 0; i < g_EventMappingCount; i++) {{
        const auto& e = g_EventMappingData[i];
        std::string label;

        if (e.name && e.name[0]) {{
            // Resolved: use event name
            label = e.name;
        }} else if (e.bank && e.bank[0]) {{
            // Unresolved but known bank: BankName::0xHEXID
            char buf[64];
            sprintf_s(buf, "%s::0x%08X", e.bank, e.id);
            label = buf;
        }} else {{
            // Unknown: bare hex
            char buf[16];
            sprintf_s(buf, "0x%08X", e.id);
            label = buf;
        }}

        outMap[e.id] = std::move(label);
    }}
}}

// Extended helper: also builds event->bank mapping for filtering by loaded banks
inline void BuildEventMappingTableWithBanks(
    std::unordered_map<DWORD, std::string>& outMap,
    std::unordered_map<DWORD, std::string>& outEventSourceBank) {{
    outMap.clear();
    outMap.reserve(g_EventMappingCount);
    outEventSourceBank.clear();
    outEventSourceBank.reserve(g_EventMappingCount);

    for (size_t i = 0; i < g_EventMappingCount; i++) {{
        const auto& e = g_EventMappingData[i];
        std::string label;

        if (e.name && e.name[0]) {{
            label = e.name;
        }} else if (e.bank && e.bank[0]) {{
            char buf[64];
            sprintf_s(buf, "%s::0x%08X", e.bank, e.id);
            label = buf;
        }} else {{
            char buf[16];
            sprintf_s(buf, "0x%08X", e.id);
            label = buf;
        }}

        outMap[e.id] = std::move(label);

        // Track source bank for this event
        if (e.bank && e.bank[0]) {{
            outEventSourceBank[e.id] = e.bank;
        }}
    }}
}}
"""

    if dry_run:
        print(f"[DRY RUN] Would write {len(entries)} entries to {output_path}")
        print(f"[DRY RUN] First 10 entries:")
        for e in entries[:10]:
            bank_lit = f'"{e.bank}"' if e.bank else 'nullptr'
            name_lit = f'"{e.name}"' if e.name else 'nullptr'
            print(f"    {{0x{e.id:08X}U, {bank_lit}, {name_lit}}}, // {e.source}")
    else:
        with open(output_path, 'w', encoding='utf-8') as f:
            f.write(content)


# =============================================================================
# Summary Output
# =============================================================================

def build_summary(
    entries: List[EventEntry],
    failed_bnks: List[str],
    orphan_overrides: List[int],
    duplicate_overrides: List[int],
) -> GenerationSummary:
    """Build generation summary from results."""
    from_txtp = sum(1 for e in entries if e.source == "txtp")
    from_override = sum(1 for e in entries if e.source == "override")
    bank_only = sum(1 for e in entries if e.source == "bank-only")

    return GenerationSummary(
        total_events=len(entries),
        from_txtp=from_txtp,
        from_override=from_override,
        bank_only=bank_only,
        failed_bnks=failed_bnks,
        orphan_overrides=orphan_overrides,
        duplicate_overrides=duplicate_overrides,
    )


def print_summary(summary: GenerationSummary) -> None:
    """Print human-readable summary to stdout."""
    print("\n=== Generation Summary ===")
    print(f"Total events:     {summary.total_events}")
    print(f"  From TXTP:      {summary.from_txtp}")
    print(f"  From override:  {summary.from_override}")
    print(f"  Bank-only:      {summary.bank_only}")

    if summary.failed_bnks or summary.orphan_overrides or summary.duplicate_overrides:
        print("\nWarnings:")
        if summary.failed_bnks:
            print(f"  Failed BNKs ({len(summary.failed_bnks)}): {', '.join(summary.failed_bnks)}")
        if summary.orphan_overrides:
            orphans = ', '.join(f'0x{eid:08X}' for eid in summary.orphan_overrides)
            print(f"  Orphan overrides ({len(summary.orphan_overrides)}): {orphans}")
        if summary.duplicate_overrides:
            dups = ', '.join(f'0x{eid:08X}' for eid in summary.duplicate_overrides)
            print(f"  Duplicate overrides ({len(summary.duplicate_overrides)}): {dups}")
    else:
        print("\nNo warnings.")


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description='Generate event_mapping.h from BNK, TXTP, and override sources.')
    parser.add_argument('--dry-run', action='store_true', help='Show what would be written without modifying files')
    parser.add_argument('--bnk-dir', type=Path, default=None, help='Directory containing .bnk files')
    parser.add_argument('--txtp-dir', type=Path, default=None, help='Directory containing .txtp files')
    parser.add_argument('--overrides', type=Path, default=None, help='Path to overrides.csv')
    parser.add_argument('--output', type=Path, default=None, help='Output path for event_mapping.h')
    parser.add_argument('--extracted-json', type=Path, default=None, help='Path to extracted_events.json (from XML parsing)')
    args = parser.parse_args()

    # Default paths relative to script location
    script_dir = Path(__file__).parent
    bnk_dir = args.bnk_dir or (script_dir / 'extracted')
    txtp_dir = args.txtp_dir or (script_dir / 'txtp')
    overrides_path = args.overrides or (script_dir / 'overrides.csv')
    extracted_json_path = args.extracted_json or (script_dir / 'extracted_events.json')
    output_path = args.output or (script_dir.parent / 'DebugOverlay' / 'src' / 'event_mapping.h')

    # Check for extracted_events.json first (preferred source)
    xml_events = {}
    if extracted_json_path.exists():
        print(f"Loading events from XML extraction: {extracted_json_path}")
        xml_events = load_extracted_events_json(extracted_json_path)
        print(f"Found {len(xml_events)} events from XML extraction")

    # Validate directories (only if no XML events)
    bnk_events = {}
    failed_bnks = []
    if not xml_events:
        if not bnk_dir.exists():
            print(f"Error: BNK directory not found: {bnk_dir}")
            return 1
        print(f"Scanning BNKs from: {bnk_dir}")
        bnk_events, failed_bnks = scan_bnks(bnk_dir)
        print(f"Found {len(bnk_events)} unique event IDs in BNKs")
    else:
        # Use XML events as primary source
        bnk_events = xml_events

    # Scan TXTPs for event names (optional)
    txtp_names = {}
    if txtp_dir.exists():
        print(f"Scanning TXTPs from: {txtp_dir}")
        txtp_names = scan_txtps(txtp_dir)
        print(f"Found {len(txtp_names)} event names from TXTPs")
    else:
        print(f"TXTP directory not found (optional): {txtp_dir}")

    # Load overrides
    if overrides_path.exists():
        print(f"Loading overrides from: {overrides_path}")
    else:
        print(f"No overrides file found (optional): {overrides_path}")

    overrides, dup_overrides = load_overrides(overrides_path)
    if overrides:
        print(f"Loaded {len(overrides)} name overrides")

    # 4. Merge
    entries, orphan_overrides = merge_mappings(bnk_events, txtp_names, overrides)

    # 5. Build summary
    summary = build_summary(entries, failed_bnks, orphan_overrides, dup_overrides)

    # 6. Write header
    write_header(entries, output_path, summary, dry_run=args.dry_run)

    if not args.dry_run:
        print(f"\nOutput written to: {output_path}")

    # 7. Print summary
    print_summary(summary)

    return 0


if __name__ == '__main__':
    exit(main())

