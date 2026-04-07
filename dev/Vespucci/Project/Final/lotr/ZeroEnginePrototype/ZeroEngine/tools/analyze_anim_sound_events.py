#!/usr/bin/env python3
"""
Batch analyzer for animation JSON sound events.
Scans all animation JSONs under GameFiles/srcjson/*/animations/
and extracts SoundEvent / SoundCue patterns, timing, parameter usage.
"""

import re
import os
import sys
from collections import Counter
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent / "GameFiles" / "srcjson"

# Precompiled regex patterns for speed
RE_KEY = re.compile(r'"key"\s*:\s*"([^"]*)"')
RE_UNK5 = re.compile(r'"unk_5"\s*:\s*([0-9.eE+-]+)')
RE_EVENT_TYPES = re.compile(r'"event"\s*:\s*"([^"]*)"')
RE_EVENT_BLOCK = re.compile(
    r'\{\s*"event"\s*:\s*"(SoundEvent|SoundCue)"\s*,\s*"t"\s*:\s*([0-9.eE+-]+)\s*,'
    r'\s*"vals"\s*:\s*\[(.*?)\]\s*\}', re.DOTALL)
RE_CRC_VAL = re.compile(r'"CRC"\s*:\s*"([^"]*)"')
RE_INT_VAL = re.compile(r'"Int"\s*:\s*(-?[0-9]+)')
RE_FLOAT_VAL = re.compile(r'"Float"\s*:\s*([0-9.eE+-]+)')

def find_animation_jsons(base):
    """Yield all animation JSON file paths."""
    for root, dirs, files in os.walk(base):
        if os.path.basename(root) == "animations":
            for f in files:
                if f.endswith(".json"):
                    yield os.path.join(root, f)

def extract_events_regex(filepath):
    """Pure-regex extraction. No json.load() — orders of magnitude faster.
    Returns (anim_key, duration, sound_events, all_event_types)."""
    with open(filepath, "r", encoding="utf-8") as fh:
        raw = fh.read()

    # Extract anim key from info section (near top of file)
    m = RE_KEY.search(raw[:1000])
    anim_key = m.group(1) if m else os.path.basename(filepath)

    # Extract duration
    m = RE_UNK5.search(raw[:1000])
    duration = float(m.group(1)) if m else 0.0

    # Extract all event type strings (fast)
    all_types = RE_EVENT_TYPES.findall(raw)

    # Quick bail if no sound events
    if '"SoundEvent"' not in raw and '"SoundCue"' not in raw:
        return anim_key, duration, [], all_types

    # Extract full SoundEvent/SoundCue blocks with vals
    sound_events = []
    for m in RE_EVENT_BLOCK.finditer(raw):
        etype = m.group(1)
        t = float(m.group(2))
        vals_block = m.group(3)
        # Parse individual val entries
        classified = []
        # Walk through each { ... } in the vals array
        for vm in re.finditer(r'\{([^}]+)\}', vals_block):
            inner = vm.group(1)
            cm = RE_CRC_VAL.search(inner)
            if cm:
                classified.append(("CRC", cm.group(1)))
                continue
            im = RE_INT_VAL.search(inner)
            if im:
                classified.append(("Int", int(im.group(1))))
                continue
            fm = RE_FLOAT_VAL.search(inner)
            if fm:
                classified.append(("Float", float(fm.group(1))))
                continue
            classified.append(("Unknown", inner.strip()))
        sound_events.append({"event": etype, "t": t, "classified": classified})

    return anim_key, duration, sound_events, all_types

def main():
    # Accumulators
    total_files = 0
    files_with_sound = 0
    event_type_counts = Counter()         # event type -> count
    sound_event_names = Counter()         # SoundEvent CRC[0] -> count
    sound_cue_names = Counter()           # SoundCue CRC[0] -> count
    all_event_types = Counter()           # all unique event type strings
    nonzero_int_examples = []             # examples where Int != 0
    sound_event_val_signatures = Counter()  # signature pattern
    sound_cue_val_signatures = Counter()
    timing_data = {"SoundEvent": [], "SoundCue": []}
    sound_cue_second_crc = Counter()      # SoundCue vals[1] CRC values
    sound_cue_third_crc = Counter()       # SoundCue vals[2] CRC values
    sound_event_second_crc = Counter()    # SoundEvent vals[1] CRC values
    files_per_level = Counter()
    total_sound_events = 0
    total_sound_cues = 0

    all_files = list(find_animation_jsons(BASE_DIR))
    print(f"Found {len(all_files)} animation JSON files. Scanning...", file=sys.stderr)
    for idx, fpath in enumerate(all_files):
        if idx % 200 == 0:
            print(f"  Processing {idx}/{len(all_files)}...", file=sys.stderr)
        total_files += 1
        level = Path(fpath).parts[-3]  # e.g. "Training"
        files_per_level[level] += 1

        try:
            anim_key, duration, sound_evts, event_types_list = extract_events_regex(fpath)
        except Exception as e:
            print(f"  ERROR reading {fpath}: {e}", file=sys.stderr)
            continue

        # Count all event types even from non-sound files
        for et in event_types_list:
            all_event_types[et] += 1

        if not sound_evts:
            continue

        files_with_sound += 1
        for sev in sound_evts:
            etype = sev["event"]
            t = sev["t"]
            classified = sev["classified"]
            sig = tuple(c[0] for c in classified)
            crc_vals = [c[1] for c in classified if c[0] == "CRC"]
            int_vals = [c[1] for c in classified if c[0] == "Int"]

            if etype == "SoundEvent":
                total_sound_events += 1
                sound_event_val_signatures[sig] += 1
                if crc_vals:
                    sound_event_names[crc_vals[0]] += 1
                if len(crc_vals) > 1:
                    sound_event_second_crc[crc_vals[1]] += 1
                timing_data["SoundEvent"].append(t)
                for i, iv in enumerate(int_vals):
                    if iv != 0:
                        nonzero_int_examples.append({
                            "file": anim_key, "event": etype,
                            "t": t, "int_idx": i, "int_val": iv,
                            "all_vals": classified
                        })

            elif etype == "SoundCue":
                total_sound_cues += 1
                sound_cue_val_signatures[sig] += 1
                if crc_vals:
                    sound_cue_names[crc_vals[0]] += 1
                if len(crc_vals) > 1:
                    sound_cue_second_crc[crc_vals[1]] += 1
                if len(crc_vals) > 2:
                    sound_cue_third_crc[crc_vals[2]] += 1
                timing_data["SoundCue"].append(t)
                for i, iv in enumerate(int_vals):
                    if iv != 0:
                        nonzero_int_examples.append({
                            "file": anim_key, "event": etype,
                            "t": t, "int_idx": i, "int_val": iv,
                            "all_vals": classified
                        })

    # ---- Report ----
    print("=" * 80)
    print("ANIMATION SOUND EVENT ANALYSIS REPORT")
    print("=" * 80)
    print(f"\nTotal animation files scanned: {total_files}")
    print(f"Files containing sound events: {files_with_sound}")
    print(f"Total SoundEvent instances:    {total_sound_events}")
    print(f"Total SoundCue instances:      {total_sound_cues}")

    print(f"\n--- Files per level ---")
    for lvl, cnt in sorted(files_per_level.items(), key=lambda x: -x[1]):
        print(f"  {lvl:25s} {cnt:5d} files")

    print(f"\n--- ALL event types (not just sound) ---")
    for et, cnt in all_event_types.most_common():
        print(f"  {et:30s}  {cnt:5d}")

    print(f"\n--- SoundEvent val signature patterns ---")
    for sig, cnt in sound_event_val_signatures.most_common(10):
        print(f"  {sig}  x{cnt}")

    print(f"\n--- SoundCue val signature patterns ---")
    for sig, cnt in sound_cue_val_signatures.most_common(10):
        print(f"  {sig}  x{cnt}")

    print(f"\n--- SoundEvent names (CRC[0]) top 30 ---")
    for name, cnt in sound_event_names.most_common(30):
        print(f"  {name:30s}  {cnt:5d}")

    print(f"\n--- SoundEvent second CRC values ---")
    for name, cnt in sound_event_second_crc.most_common(20):
        label = name if name else "(empty)"
        print(f"  {label:30s}  {cnt:5d}")

    print(f"\n--- SoundCue names (CRC[0]) top 50 ---")
    for name, cnt in sound_cue_names.most_common(50):
        print(f"  {name:30s}  {cnt:5d}")

    print(f"\n--- SoundCue second CRC values ---")
    for name, cnt in sound_cue_second_crc.most_common(20):
        label = name if name else "(empty)"
        print(f"  {label:30s}  {cnt:5d}")

    print(f"\n--- SoundCue third CRC values ---")
    for name, cnt in sound_cue_third_crc.most_common(20):
        label = name if name else "(empty)"
        print(f"  {label:30s}  {cnt:5d}")

    print(f"\n--- Non-zero Int parameter examples (first 30) ---")
    if nonzero_int_examples:
        for ex in nonzero_int_examples[:30]:
            print(f"  File: {ex['file']}")
            print(f"    Event: {ex['event']}  t={ex['t']:.4f}")
            print(f"    Int index: {ex['int_idx']}  value: {ex['int_val']}")
            print(f"    All vals: {ex['all_vals']}")
            print()
    else:
        print("  NONE - all Int parameters are 0 across all files!")

    print(f"\n--- Timing statistics ---")
    for stype in ("SoundEvent", "SoundCue"):
        times = timing_data[stype]
        if times:
            print(f"  {stype}:")
            print(f"    count:  {len(times)}")
            print(f"    min t:  {min(times):.6f}")
            print(f"    max t:  {max(times):.6f}")
            print(f"    mean t: {sum(times)/len(times):.6f}")
        else:
            print(f"  {stype}: no instances")

    print("\n" + "=" * 80)
    print("END OF REPORT")
    print("=" * 80)


if __name__ == "__main__":
    main()

