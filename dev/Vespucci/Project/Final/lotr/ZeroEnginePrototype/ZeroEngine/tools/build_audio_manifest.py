#!/usr/bin/env python3
"""
Phase 0: Build Audio Asset Manifest
Scans GameFiles for all sound cue strings, computes FNV-1 hashes,
resolves to Wwise Event IDs, parses TXTP files for source WEM IDs,
and exports a complete manifest JSON.

Chain: CueString → FNV-1 hash → WWiseIDTable → EventID → Bank → TXTP → SourceIDs → WEM paths
"""

import json
import os
import re
import csv
import sys
from pathlib import Path
from collections import defaultdict

# Force unbuffered output so progress is visible
sys.stdout.reconfigure(line_buffering=True)

# ============================================================================
# Configuration
# ============================================================================
SCRIPT_DIR = Path(__file__).parent
ENGINE_ROOT = SCRIPT_DIR.parent  # ZeroEngine/
GAMEFILES_DIR = ENGINE_ROOT / "GameFiles"
WWISE_DIR = ENGINE_ROOT / "Engine" / "source" / "WwiseV28" / "ExtractedSounds"
DICT_DIR = ENGINE_ROOT / "Engine" / "wwise" / "cracking" / "HashCracking" / "Dictionary"

WWISEIDTABLE_PATH = ENGINE_ROOT / "Engine" / "wwise" / "cracking" / "Dictionary canditates" / "WWiseIDTable.audio.json"
OVERRIDES_PATH = DICT_DIR / "overrides.csv"
EVENT_MAPPING_PATH = DICT_DIR / "event_mapping.json"
TXTP_DIR = WWISE_DIR / "txtp"
ORGANIZED_DIR = WWISE_DIR / "Organized_Final"

OUTPUT_DIR = SCRIPT_DIR / "audio_manifest"
OUTPUT_MANIFEST = OUTPUT_DIR / "audio_manifest.json"
OUTPUT_REPORT = OUTPUT_DIR / "extraction_report.txt"

# ============================================================================
# FNV-1 Hash (32-bit, as used by the game engine)
# ============================================================================
FNV_OFFSET_BASIS = 0x811c9dc5
FNV_PRIME = 0x01000193
FNV_MASK = 0xFFFFFFFF

def fnv1_hash(s: str) -> int:
    """Compute FNV-1 hash (32-bit) for a string, case-insensitive (lowercase)."""
    h = FNV_OFFSET_BASIS
    for c in s.lower():
        h = ((h * FNV_PRIME) & FNV_MASK) ^ ord(c)
    return h

def fnv1a_hash(s: str) -> int:
    """Compute FNV-1a hash (32-bit) for a string, case-insensitive (lowercase)."""
    h = FNV_OFFSET_BASIS
    for c in s.lower():
        h = ((h ^ ord(c)) * FNV_PRIME) & FNV_MASK
    return h

# ============================================================================
# Step 1: Scan GameFiles for sound cue strings
# ============================================================================
class CueExtractor:
    def __init__(self):
        self.cues = defaultdict(set)  # cue_string -> set of source files
        self.effect_cues = defaultdict(set)
        self.lua_cues = defaultdict(set)
        self.csv_events = defaultdict(set)
        self.stats = defaultdict(int)

    def scan_all(self, gamefiles_dir: Path):
        """Scan all GameFiles subdirectories for sound cue strings."""
        print("[Step 1] Scanning GameFiles for sound cue strings...")

        # Collect all files first for progress tracking
        all_files = []
        for root, dirs, files in os.walk(gamefiles_dir):
            root_path = Path(root)
            for fname in files:
                fpath = root_path / fname
                ext = fname.lower().rsplit('.', 1)[-1] if '.' in fname else ''
                if ext in ('json', 'lua', 'csv'):
                    all_files.append((fpath, ext))

        total = len(all_files)
        print(f"  Found {total} scannable files (.json/.lua/.csv)")

        for idx, (fpath, ext) in enumerate(all_files):
            if (idx + 1) % 5000 == 0 or idx == 0:
                print(f"  Processing {idx + 1}/{total}...")

            try:
                if ext == 'json':
                    rel = fpath.relative_to(gamefiles_dir)
                    parts = str(rel).replace('\\', '/').split('/')
                    # Detect file type by directory name
                    if 'animations' in parts:
                        self._scan_animation_json(fpath)
                    elif 'effects' in parts:
                        self._scan_effect_json(fpath)
                    else:
                        self._scan_generic_json(fpath)
                elif ext == 'lua':
                    self._scan_lua(fpath)
                elif ext == 'csv':
                    self._scan_csv(fpath)
            except Exception as e:
                self.stats['errors'] += 1

        print(f"  Scan complete. Processed {total} files.")

    def _scan_animation_json(self, fpath: Path):
        """Extract sound cue strings from animation JSON events."""
        self.stats['anim_files'] += 1
        try:
            with open(fpath, 'r', encoding='utf-8') as f:
                data = json.load(f)
        except (json.JSONDecodeError, UnicodeDecodeError):
            return

        events = data.get('events', [])
        for ev in events:
            ev_type = ev.get('event', '')
            vals = ev.get('vals', [])
            # Extract CRC string values from event parameters
            for v in vals:
                if isinstance(v, dict):
                    crc_val = v.get('CRC', '')
                    if crc_val and crc_val.strip():
                        self.cues[crc_val].add(str(fpath))
                        self.stats['anim_cues'] += 1

    def _scan_effect_json(self, fpath: Path):
        """Extract sound cues from effect JSON files."""
        self.stats['effect_files'] += 1
        try:
            with open(fpath, 'r', encoding='utf-8') as f:
                text = f.read()
        except UnicodeDecodeError:
            return

        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return

        self._extract_effect_sounds_recursive(data, fpath)

    def _extract_effect_sounds_recursive(self, obj, fpath):
        """Recursively find effectSound objects and extract cue/event/StopSound."""
        if isinstance(obj, dict):
            # Check for effectSound-type objects
            obj_type = obj.get('type', '')
            if obj_type == 'effectSound' or obj.get('Name') == 'effectSound':
                for key in ('cue', 'event', 'StopSound'):
                    val = obj.get(key, '')
                    if val and isinstance(val, str) and val.strip():
                        self.effect_cues[val].add(str(fpath))
                        self.stats['effect_cues'] += 1
            # Also check fields array pattern (srcjson format)

    def _scan_generic_json(self, fpath: Path):
        """Scan any other JSON file for sound-related fields."""
        self.stats['other_json'] += 1
        try:
            with open(fpath, 'r', encoding='utf-8') as f:
                text = f.read()
        except UnicodeDecodeError:
            return

        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            return

        # Look for sound-related fields in any JSON structure
        self._find_sound_fields(data, fpath)

    def _find_sound_fields(self, obj, fpath):
        """Recursively find sound-related string fields in JSON."""
        if isinstance(obj, dict):
            for key, val in obj.items():
                key_lower = key.lower()
                if isinstance(val, str) and val.strip():
                    if any(kw in key_lower for kw in
                           ('sound', 'cue', 'music', 'rumble', 'soundenvironment')):
                        self.cues[val].add(str(fpath))
                        self.stats['generic_cues'] += 1
                elif isinstance(val, (dict, list)):
                    self._find_sound_fields(val, fpath)
        elif isinstance(obj, list):
            for item in obj:
                if isinstance(item, (dict, list)):
                    self._find_sound_fields(item, fpath)

    def _scan_lua(self, fpath: Path):
        """Extract sound cue strings from Lua source files."""
        self.stats['lua_files'] += 1
        try:
            with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
                text = f.read()
        except Exception:
            return

        # Pattern 1: PlaySound("cue") / StopSound("cue") / PushSoundState("cue")
        for m in re.finditer(r'(?:PlaySound|StopSound|PushSoundState)\s*\(\s*["\']([^"\']+)["\']', text):
            cue = m.group(1).strip()
            if cue:
                self.lua_cues[cue].add(str(fpath))
                self.stats['lua_func_cues'] += 1

        # Pattern 2: sound = "cue_name"
        for m in re.finditer(r'sound\s*=\s*["\']([^"\']+)["\']', text):
            cue = m.group(1).strip()
            if cue:
                self.lua_cues[cue].add(str(fpath))
                self.stats['lua_sound_eq'] += 1

        # Pattern 3: Act_PlaySound arguments in table constructors
        for m in re.finditer(r'"Act_PlaySound"\s*,\s*["\']?([a-zA-Z_][a-zA-Z0-9_]*)', text):
            cue = m.group(1).strip()
            if cue:
                self.lua_cues[cue].add(str(fpath))
                self.stats['lua_act_play'] += 1

        # Pattern 4: SoundOverride strings
        for m in re.finditer(r'(?:SoundOverride|SetSoundOverride|ClearSoundOverride)\s*\(\s*["\']([^"\']*)["\']', text):
            cue = m.group(1).strip()
            if cue:
                self.lua_cues[cue].add(str(fpath))
                self.stats['lua_override'] += 1

        # Pattern 5: Quoted strings that look like sound event names (Play_*, Stop_*, etc.)
        for m in re.finditer(r'["\']((Play|Stop|Set|Push|Pop)_[a-zA-Z0-9_]+)["\']', text):
            cue = m.group(1).strip()
            if cue:
                self.lua_cues[cue].add(str(fpath))
                self.stats['lua_play_prefix'] += 1

    def _scan_csv(self, fpath: Path):
        """Extract event names from CSV files like EventDefinitions.csv."""
        self.stats['csv_files'] += 1
        try:
            with open(fpath, 'r', encoding='utf-8', errors='replace') as f:
                reader = csv.reader(f)
                header = None
                for row in reader:
                    if not row:
                        continue
                    if header is None:
                        header = row
                        # Check if this looks like EventDefinitions
                        if row[0].strip().startswith('EventName'):
                            continue
                        header = None
                        # Still scan for sound-like strings
                    if header and row[0].strip():
                        self.csv_events[row[0].strip()].add(str(fpath))
                        self.stats['csv_events'] += 1
        except Exception:
            pass

    def get_all_unique_cues(self):
        """Return all unique cue strings with their sources."""
        all_cues = {}
        for cue, sources in self.cues.items():
            if cue not in all_cues:
                all_cues[cue] = {'sources': set(), 'origin': []}
            all_cues[cue]['sources'].update(sources)
            all_cues[cue]['origin'].append('animation')
        for cue, sources in self.effect_cues.items():
            if cue not in all_cues:
                all_cues[cue] = {'sources': set(), 'origin': []}
            all_cues[cue]['sources'].update(sources)
            all_cues[cue]['origin'].append('effect')
        for cue, sources in self.lua_cues.items():
            if cue not in all_cues:
                all_cues[cue] = {'sources': set(), 'origin': []}
            all_cues[cue]['sources'].update(sources)
            all_cues[cue]['origin'].append('lua')
        for cue, sources in self.csv_events.items():
            if cue not in all_cues:
                all_cues[cue] = {'sources': set(), 'origin': []}
            all_cues[cue]['sources'].update(sources)
            all_cues[cue]['origin'].append('csv')
        return all_cues


# ============================================================================
# Step 2: Load Reference Data
# ============================================================================
class ReferenceData:
    def __init__(self):
        self.hash_to_event_id = {}    # hex_key -> event_id (from obj1s)
        self.readable_to_event_id = {}  # readable_name -> event_id
        self.event_id_to_bank = {}    # event_id -> bank_name
        self.event_id_to_info = {}    # event_id -> {bank, idx, name, wems}
        self.override_names = {}      # hex_hash -> name (from overrides.csv)
        self.hash_dict_names = {}     # hash_int -> name (from hash_dictionary.cpp)

    def load_all(self):
        print("[Step 2] Loading reference data...")
        self._load_wwise_id_table()
        self._load_overrides()
        self._load_event_mapping()
        self._load_hash_dictionary()

    def _load_wwise_id_table(self):
        """Load WWiseIDTable.audio.json - obj1s maps hash keys to Event IDs."""
        if not WWISEIDTABLE_PATH.exists():
            print(f"  WARNING: {WWISEIDTABLE_PATH} not found")
            return
        with open(WWISEIDTABLE_PATH, 'r', encoding='utf-8') as f:
            data = json.load(f)

        for entry in data.get('obj1s', []):
            key = entry['key']
            val = entry['val']
            if key.startswith('0x'):
                self.hash_to_event_id[key.lower()] = val
            else:
                self.readable_to_event_id[key] = val
                self.hash_to_event_id[key.lower()] = val

        print(f"  WWiseIDTable: {len(self.hash_to_event_id)} hash entries, "
              f"{len(self.readable_to_event_id)} readable names")

    def _load_overrides(self):
        """Load overrides.csv - FNV-1 cracked hash names."""
        if not OVERRIDES_PATH.exists():
            print(f"  WARNING: {OVERRIDES_PATH} not found")
            return
        count = 0
        with open(OVERRIDES_PATH, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = line.split(',')
                if len(parts) >= 2:
                    hex_hash = parts[0].strip().lower()
                    name = parts[1].strip()
                    self.override_names[hex_hash] = name
                    count += 1
        print(f"  Overrides: {count} cracked hash names")

    def _load_event_mapping(self):
        """Load event_mapping.json - Event ID to bank name."""
        if not EVENT_MAPPING_PATH.exists():
            print(f"  WARNING: {EVENT_MAPPING_PATH} not found")
            return
        with open(EVENT_MAPPING_PATH, 'r', encoding='utf-8') as f:
            data = json.load(f)

        for eid_str, info in data.get('events', {}).items():
            eid = int(eid_str)
            self.event_id_to_bank[eid] = info.get('bank', 'Unknown')
            self.event_id_to_info[eid] = info

        print(f"  Event mapping: {len(self.event_id_to_bank)} events across "
              f"{len(set(self.event_id_to_bank.values()))} banks")

    def _load_hash_dictionary(self):
        """Load known hash→name pairs from hash_dictionary.cpp comments."""
        # These are the authoritative mappings from the game
        known = {
            1166589404: "ability", 2386519981: "swing",
            2782159082: "stop_ability", 3257506471: "impact",
            3289505165: "ranged_attack_release", 3317156538: "ranged_attack_charge",
            3703028245: "firewall", 3940739935: "impact_kill",
            4001580976: "Block", 2234217108: "VOKill",
            2046698775: "taunt", 2603361507: "attack_vocal",
            3326610637: "creature_death", 3478489869: "Human",
            4141430720: "cheer", 17721092: "Grab",
        }
        self.hash_dict_names = known
        print(f"  Hash dictionary: {len(known)} known event names")


# ============================================================================
# Step 3: Parse TXTP files for EventID → Source WEM IDs
# ============================================================================
class TXTPParser:
    """Parses TXTP files to extract event_id → source_ids mapping."""

    def __init__(self):
        # event_id (int) -> list of TXTPEntry dicts
        self.event_entries = defaultdict(list)
        # bank_event_idx -> event_id (from CAkEvent lines)
        self.idx_to_event_id = {}
        self.stats = {'files': 0, 'events': 0, 'sources': 0, 'errors': 0}

    def parse_all(self, txtp_dir: Path):
        """Parse all TXTP files in the directory."""
        print("[Step 3] Parsing TXTP files...")
        if not txtp_dir.exists():
            print(f"  WARNING: TXTP directory not found: {txtp_dir}")
            return

        txtp_files = list(txtp_dir.glob('*.txtp'))
        total = len(txtp_files)
        print(f"  Found {total} TXTP files")
        for idx, fpath in enumerate(txtp_files):
            if (idx + 1) % 500 == 0:
                print(f"  Parsing {idx + 1}/{total}...")
            try:
                self._parse_txtp(fpath)
            except Exception as e:
                self.stats['errors'] += 1

        print(f"  Parsed {self.stats['files']} TXTP files")
        print(f"  Found {self.stats['events']} unique events, "
              f"{self.stats['sources']} total source refs")

    def _parse_txtp(self, fpath: Path):
        """Parse a single TXTP file."""
        self.stats['files'] += 1
        fname = fpath.name

        # Extract bank name and event idx from filename
        # Format: BankName-NNNN-event [switches] {flags}.txtp
        fn_match = re.match(r'^(\w+)-(\d+)-event', fname)
        bank_name = fn_match.group(1) if fn_match else 'Unknown'
        event_idx = int(fn_match.group(2)) if fn_match else -1

        # Extract switch conditions from filename [group=value] pairs
        switches = {}
        for sw_match in re.finditer(r'\[(\d+)=(\d+)\]', fname):
            switches[int(sw_match.group(1))] = int(sw_match.group(2))

        # Read file content for comment-based metadata
        try:
            with open(fpath, 'r', encoding='utf-8') as f:
                text = f.read()
        except Exception:
            return

        event_id = None
        source_ids = []

        for line in text.splitlines():
            line = line.strip()

            # Extract event ID from: #   CAkEvent[NNN] EVENT_ID
            ev_match = re.match(r'^#\s+CAkEvent\[(\d+)\]\s+(\d+)', line)
            if ev_match:
                event_id = int(ev_match.group(2))

            # Extract source IDs from: #   Source SOURCE_ID
            src_match = re.match(r'^#\s+Source\s+(\d+)', line)
            if src_match:
                source_ids.append(int(src_match.group(1)))
                self.stats['sources'] += 1

        if event_id is not None:
            entry = {
                'bank': bank_name,
                'event_idx': event_idx,
                'switches': switches,
                'source_ids': source_ids,
                'txtp_file': fname,
            }
            self.event_entries[event_id].append(entry)
            self.stats['events'] = len(self.event_entries)

            # Also map by bank+idx for cross-reference
            key = f"{bank_name}-{event_idx}"
            self.idx_to_event_id[key] = event_id

    def get_all_source_ids_for_event(self, event_id):
        """Return deduplicated list of all source IDs across all switch variants."""
        sources = set()
        for entry in self.event_entries.get(event_id, []):
            sources.update(entry['source_ids'])
        return sorted(sources)

    def get_entries_for_event(self, event_id):
        """Return all TXTP entries (switch variants) for an event."""
        return self.event_entries.get(event_id, [])


# ============================================================================
# Step 4: Hash Resolution — map cue strings to Event IDs
# ============================================================================
class HashResolver:
    """Resolves cue strings to Wwise Event IDs via FNV-1 hashing."""

    def __init__(self, ref: ReferenceData):
        self.ref = ref
        self.stats = {'total': 0, 'resolved_hash': 0, 'resolved_readable': 0,
                      'resolved_override': 0, 'resolved_dict': 0, 'unresolved': 0}

    def resolve(self, cue_string: str):
        """
        Try to resolve a cue string to an Event ID.
        Returns (event_id, method) or (None, None).
        """
        self.stats['total'] += 1

        # Method 1: Direct readable name lookup in WWiseIDTable
        if cue_string in self.ref.readable_to_event_id:
            self.stats['resolved_readable'] += 1
            return self.ref.readable_to_event_id[cue_string], 'readable'

        # Case-insensitive readable lookup
        cue_lower = cue_string.lower()
        for name, eid in self.ref.readable_to_event_id.items():
            if name.lower() == cue_lower:
                self.stats['resolved_readable'] += 1
                return eid, 'readable_ci'

        # Method 2: FNV-1 hash lookup
        h = fnv1_hash(cue_string)
        hex_key = f"0x{h:08x}"
        if hex_key in self.ref.hash_to_event_id:
            self.stats['resolved_hash'] += 1
            return self.ref.hash_to_event_id[hex_key], 'fnv1'

        # Method 3: FNV-1a hash lookup (alternate)
        h_a = fnv1a_hash(cue_string)
        hex_key_a = f"0x{h_a:08x}"
        if hex_key_a in self.ref.hash_to_event_id:
            self.stats['resolved_hash'] += 1
            return self.ref.hash_to_event_id[hex_key_a], 'fnv1a'

        # Method 4: Check if FNV-1 hash matches a known event_id directly
        if h in self.ref.event_id_to_bank:
            self.stats['resolved_dict'] += 1
            return h, 'fnv1_direct'

        # Method 5: Override name reverse lookup — check if cue matches an override
        for hex_hash, name in self.ref.override_names.items():
            if name.lower() == cue_lower:
                if hex_hash in self.ref.hash_to_event_id:
                    self.stats['resolved_override'] += 1
                    return self.ref.hash_to_event_id[hex_hash], 'override'

        # Method 6: Hash dictionary known mappings
        for known_hash, known_name in self.ref.hash_dict_names.items():
            if known_name.lower() == cue_lower:
                if known_hash in self.ref.event_id_to_bank:
                    self.stats['resolved_dict'] += 1
                    return known_hash, 'hash_dict'

        self.stats['unresolved'] += 1
        return None, None


# ============================================================================
# Step 5: Build Manifest
# ============================================================================
def find_wem_file(source_id, organized_dir: Path, wem_dir: Path):
    """Find a WEM file by source ID in the organized or wem directories."""
    # Check Organized_Final subdirectories
    for subdir in organized_dir.iterdir() if organized_dir.exists() else []:
        if subdir.is_dir():
            candidate = subdir / f"{source_id}.wem"
            if candidate.exists():
                return str(candidate.relative_to(organized_dir.parent.parent))

    # Check wem/ directory
    for ext in ('wem', 'ogg'):
        candidate = wem_dir / f"{source_id}.{ext}"
        if candidate.exists():
            return str(candidate.relative_to(wem_dir.parent))

    return None


def build_manifest(extractor: CueExtractor, ref: ReferenceData,
                   txtp: TXTPParser, resolver: HashResolver):
    """Build the complete audio manifest JSON."""
    print("\n[Step 5] Building manifest...")

    all_cues = extractor.get_all_unique_cues()
    manifest = {
        'version': 1,
        'description': 'LOTR Conquest Audio Manifest - CueString to WEM mapping',
        'cues': {},
        'events_by_id': {},
        'unresolved_cues': [],
        'statistics': {},
    }

    resolved_count = 0
    with_sources = 0

    for cue_string, info in sorted(all_cues.items()):
        event_id, method = resolver.resolve(cue_string)

        entry = {
            'hash_fnv1': fnv1_hash(cue_string),
            'hash_fnv1_hex': f"0x{fnv1_hash(cue_string):08X}",
            'origin': list(set(info['origin'])),
            'source_files_count': len(info['sources']),
        }

        if event_id is not None:
            resolved_count += 1
            entry['event_id'] = event_id
            entry['resolve_method'] = method

            # Get bank info from event_mapping
            if event_id in ref.event_id_to_info:
                ev_info = ref.event_id_to_info[event_id]
                entry['bank'] = ev_info.get('bank', 'Unknown')
                entry['event_name'] = ev_info.get('name', '')

            # Get source IDs from TXTP parser
            source_ids = txtp.get_all_source_ids_for_event(event_id)
            if source_ids:
                with_sources += 1
                entry['source_ids'] = source_ids
                entry['source_count'] = len(source_ids)

                # Get all TXTP variant info
                variants = txtp.get_entries_for_event(event_id)
                entry['txtp_variants'] = len(variants)
        else:
            manifest['unresolved_cues'].append(cue_string)

        manifest['cues'][cue_string] = entry

    # Build reverse index: event_id -> cue_strings
    for cue_string, entry in manifest['cues'].items():
        eid = entry.get('event_id')
        if eid is not None:
            eid_str = str(eid)
            if eid_str not in manifest['events_by_id']:
                manifest['events_by_id'][eid_str] = {
                    'cue_strings': [],
                    'bank': entry.get('bank', ''),
                    'source_ids': entry.get('source_ids', []),
                }
            manifest['events_by_id'][eid_str]['cue_strings'].append(cue_string)

    manifest['statistics'] = {
        'total_cue_strings': len(all_cues),
        'resolved_to_event_id': resolved_count,
        'with_source_ids': with_sources,
        'unresolved': len(manifest['unresolved_cues']),
        'resolution_rate': f"{resolved_count / max(len(all_cues), 1) * 100:.1f}%",
        'resolver_stats': dict(resolver.stats),
        'txtp_stats': dict(txtp.stats),
        'extractor_stats': dict(extractor.stats),
    }

    print(f"  Total cue strings: {len(all_cues)}")
    print(f"  Resolved to Event ID: {resolved_count} "
          f"({resolved_count / max(len(all_cues), 1) * 100:.1f}%)")
    print(f"  With source WEM IDs: {with_sources}")
    print(f"  Unresolved: {len(manifest['unresolved_cues'])}")

    return manifest


# ============================================================================
# Step 6: Report Generation
# ============================================================================
def write_report(manifest, ref: ReferenceData, txtp: TXTPParser, report_path: Path):
    """Write a detailed extraction report."""
    print(f"\n[Step 6] Writing report to {report_path}...")

    stats = manifest['statistics']
    lines = []
    lines.append("=" * 72)
    lines.append("LOTR Conquest — Audio Asset Manifest Extraction Report")
    lines.append("=" * 72)
    lines.append("")

    lines.append("SUMMARY")
    lines.append("-" * 40)
    lines.append(f"Total unique cue strings extracted: {stats['total_cue_strings']}")
    lines.append(f"Resolved to Event ID:              {stats['resolved_to_event_id']}")
    lines.append(f"With source WEM IDs:               {stats['with_source_ids']}")
    lines.append(f"Unresolved:                        {stats['unresolved']}")
    lines.append(f"Resolution rate:                   {stats['resolution_rate']}")
    lines.append("")

    # Extractor stats
    ext_stats = stats.get('extractor_stats', {})
    lines.append("EXTRACTION STATS")
    lines.append("-" * 40)
    lines.append(f"Animation files scanned:  {ext_stats.get('anim_files', 0)}")
    lines.append(f"Animation cue refs:       {ext_stats.get('anim_cues', 0)}")
    lines.append(f"Effect files scanned:     {ext_stats.get('effect_files', 0)}")
    lines.append(f"Effect cue refs:          {ext_stats.get('effect_cues', 0)}")
    lines.append(f"Lua files scanned:        {ext_stats.get('lua_files', 0)}")
    lines.append(f"  PlaySound/StopSound:    {ext_stats.get('lua_func_cues', 0)}")
    lines.append(f"  sound= assignments:     {ext_stats.get('lua_sound_eq', 0)}")
    lines.append(f"  Act_PlaySound:          {ext_stats.get('lua_act_play', 0)}")
    lines.append(f"  SoundOverride:          {ext_stats.get('lua_override', 0)}")
    lines.append(f"  Play_/Stop_ prefixed:   {ext_stats.get('lua_play_prefix', 0)}")
    lines.append(f"CSV files scanned:        {ext_stats.get('csv_files', 0)}")
    lines.append(f"CSV event entries:        {ext_stats.get('csv_events', 0)}")
    lines.append(f"Other JSON files:         {ext_stats.get('other_json', 0)}")
    lines.append(f"Generic sound refs:       {ext_stats.get('generic_cues', 0)}")
    lines.append(f"Errors:                   {ext_stats.get('errors', 0)}")
    lines.append("")

    # TXTP stats
    txtp_stats = stats.get('txtp_stats', {})
    lines.append("TXTP PARSING STATS")
    lines.append("-" * 40)
    lines.append(f"TXTP files parsed:        {txtp_stats.get('files', 0)}")
    lines.append(f"Unique events found:      {txtp_stats.get('events', 0)}")
    lines.append(f"Total source refs:        {txtp_stats.get('sources', 0)}")
    lines.append(f"Parse errors:             {txtp_stats.get('errors', 0)}")
    lines.append("")

    # Resolver stats
    res_stats = stats.get('resolver_stats', {})
    lines.append("RESOLUTION METHOD BREAKDOWN")
    lines.append("-" * 40)
    lines.append(f"Readable name match:      {res_stats.get('resolved_readable', 0)}")
    lines.append(f"FNV-1 hash match:         {res_stats.get('resolved_hash', 0)}")
    lines.append(f"Override name match:      {res_stats.get('resolved_override', 0)}")
    lines.append(f"Hash dictionary match:    {res_stats.get('resolved_dict', 0)}")
    lines.append(f"Unresolved:               {res_stats.get('unresolved', 0)}")
    lines.append("")

    # List unresolved cues
    if manifest['unresolved_cues']:
        lines.append("UNRESOLVED CUE STRINGS")
        lines.append("-" * 40)
        for cue in sorted(manifest['unresolved_cues']):
            h = fnv1_hash(cue)
            lines.append(f"  {cue:40s}  FNV1=0x{h:08X}")
        lines.append("")

    # Top resolved cues by source count
    lines.append("TOP 30 RESOLVED CUES (by source file count)")
    lines.append("-" * 40)
    resolved = [(k, v) for k, v in manifest['cues'].items() if 'event_id' in v]
    resolved.sort(key=lambda x: x[1].get('source_files_count', 0), reverse=True)
    for cue, entry in resolved[:30]:
        eid = entry.get('event_id', '?')
        bank = entry.get('bank', '?')
        srcs = entry.get('source_count', 0)
        lines.append(f"  {cue:35s} EID={eid:<12} bank={bank:<20s} sources={srcs}")
    lines.append("")

    # Reference data coverage
    lines.append("REFERENCE DATA COVERAGE")
    lines.append("-" * 40)
    lines.append(f"WWiseIDTable entries:     {len(ref.hash_to_event_id)}")
    lines.append(f"Override entries:         {len(ref.override_names)}")
    lines.append(f"Event mapping entries:    {len(ref.event_id_to_bank)}")
    lines.append(f"TXTP unique events:       {len(txtp.event_entries)}")
    lines.append("")

    report_text = '\n'.join(lines)
    with open(report_path, 'w', encoding='utf-8') as f:
        f.write(report_text)

    # Print report safely (handle non-ASCII chars on Windows console)
    try:
        print(report_text)
    except UnicodeEncodeError:
        print(report_text.encode('ascii', errors='replace').decode('ascii'))
    print(f"\nReport saved to: {report_path}")


# ============================================================================
# Main
# ============================================================================
def main():
    print("=" * 72)
    print("Phase 0: Build Audio Asset Manifest")
    print("=" * 72)
    print(f"GameFiles:    {GAMEFILES_DIR}")
    print(f"TXTP Dir:     {TXTP_DIR}")
    print(f"Dict Dir:     {DICT_DIR}")
    print(f"Output Dir:   {OUTPUT_DIR}")
    print()

    # Validate directories
    if not GAMEFILES_DIR.exists():
        print(f"ERROR: GameFiles directory not found: {GAMEFILES_DIR}")
        sys.exit(1)

    # Create output directory
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    # Step 1: Scan GameFiles for cue strings
    extractor = CueExtractor()
    extractor.scan_all(GAMEFILES_DIR)
    all_cues = extractor.get_all_unique_cues()
    print(f"  => {len(all_cues)} unique cue strings extracted\n")

    # Step 2: Load reference data
    ref = ReferenceData()
    ref.load_all()
    print()

    # Step 3: Parse TXTP files
    txtp_parser = TXTPParser()
    txtp_parser.parse_all(TXTP_DIR)
    print()

    # Step 4 + 5: Resolve hashes and build manifest
    resolver = HashResolver(ref)
    manifest = build_manifest(extractor, ref, txtp_parser, resolver)

    # Save manifest JSON
    print(f"\nSaving manifest to {OUTPUT_MANIFEST}...")
    with open(OUTPUT_MANIFEST, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"  Manifest size: {OUTPUT_MANIFEST.stat().st_size / 1024:.1f} KB")

    # Step 6: Write report
    write_report(manifest, ref, txtp_parser, OUTPUT_REPORT)

    print("\n" + "=" * 72)
    print("Phase 0 complete.")
    print(f"Manifest: {OUTPUT_MANIFEST}")
    print(f"Report:   {OUTPUT_REPORT}")
    print("=" * 72)


if __name__ == '__main__':
    main()