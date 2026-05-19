#!/usr/bin/env python3
# =========================================================================
#  crowd_proxy_test.py — Half 1 verification for the CrowdProxy architecture
#
#  THE FUCKING POINT
#  -----------------
#  Pandemic shipped a level format where types[] declares per-level schemas
#  and entities can override the C++ runtime class via a Factory field. They
#  use it themselves: Helm's Deep has 99 entities with type "Lobber" that
#  all carry Factory="lobber_object". The schema name and the runtime
#  behavior class are independent. We are abusing this same mechanism to
#  ship a new editor-visible type "CrowdProxy" that the engine treats as
#  a stock logic_relay at runtime — so designers can wire crowd-affecting
#  events in The Forge without the engine knowing or caring about the
#  crowd-target metadata we tack onto the entity.
#
#  This script verifies the DATA SIDE only. The runtime crowd-mutation
#  injection DLL is a follow-up. After this script runs and the user repacks
#  the PAK and loads it, the CrowdProxy entity should fire its Outputs[] as
#  a normal logic_relay when triggered, with zero crashes. The crowd doesn't
#  actually destroy yet because nobody is reading the Vesp* fields at
#  runtime. That's Phase 2 of this whole thing.
#
#  HOW TO USE
#  ----------
#    1) Dump a level:
#       lotrc_rs.exe <path-to-PAK> -o <dump-dir>
#    2) Run this script:
#       python crowd_proxy_test.py <dump-dir>
#    3) Repack:
#       lotrc_rs.exe <dump-dir> -o <out-dir>
#    4) Copy the repacked PAK back over the level in the game's Levels folder
#       (BACK UP THE ORIGINAL FIRST or take the trade and lose your save).
#    5) Launch the game, load the level, find the test trigger, walk into it,
#       watch what happens. The injected logic_relay should fire its outputs.
#       The game should NOT crash. The crowd of course will not disappear
#       because no runtime is hooked yet — that's Phase 2.
#
#  EXIT CODES
#  ----------
#    0 = success, level.json mutated and ready for repack
#    1 = something went wrong, error printed, no mutations performed
# =========================================================================

from __future__ import print_function
import sys
import os
import json
import shutil
import copy


# CrowdProxy schema offsets — picked to be self-consistent and aligned.
# All fields 4-byte aligned, vector3 needs 4-byte alignment, no matrix4x4
# so we don't need 16-byte alignment anywhere here. Schema is intentionally
# slim — this entity is a logic_relay with metadata, not a renderable thing.
CROWD_PROXY_SCHEMA = {
    "name": "CrowdProxy",
    "fields": [
        # ── Required base every PBL entity has ──
        {"name": "GUID",         "type": "GUID",        "offset": 0},
        {"name": "ParentGUID",   "type": "GUID",        "offset": 4},
        {"name": "GameModeMask", "type": "int",         "offset": 8},
        {"name": "Name",         "type": "string",      "offset": 12},
        # ── THE OVERRIDE — engine resolves "logic_relay" in the C++ type
        #    registry BST at 0x00A3E89C. Without this field, the engine
        #    falls back to using the type name "CrowdProxy" which is not
        #    in the registry, the lookup misses, and the entity gets
        #    silently dropped at runtime (FUN_007e76d4 LAB_007e78a3). ──
        {"name": "Factory",      "type": "string",      "offset": 16},
        # ── Standard logic_relay knobs the engine will actually read ──
        {"name": "EnableEvents", "type": "bool",        "offset": 20},
        {"name": "Outputs",      "type": "objectlist",  "offset": 24},
        # ── Vespucci editor-only crowd-target metadata. C++ logic_relay
        #    has no hardcoded field handlers for these names, so its
        #    ReadFields() walks past them silently. The data lives in the
        #    binary blob until our injection DLL hooks the event path and
        #    reads them on Trigger. ──
        {"name": "VespCrowdAction",       "type": "string",  "offset": 28},
        {"name": "VespCrowdTargetItem",   "type": "int",     "offset": 32},
        {"name": "VespCrowdTargetMode",   "type": "string",  "offset": 36},
        {"name": "VespCrowdTargetValIdx", "type": "int",     "offset": 40},
        {"name": "VespCrowdTargetRadius", "type": "float",   "offset": 44},
        {"name": "VespCrowdTargetCenter", "type": "vector3", "offset": 48},
        # vector3 = 12 bytes → next field at offset 60
        {"name": "VespCrowdValIdxList",   "type": "intlist", "offset": 60},
    ],
}


def find_level_json(dump_dir):
    """Return the path to level.json inside a lotrc_rs dump directory.
    Tries sub_blocks1 first (every shipped level), falls back to sub_blocks0
    just in case a future level uses the alternate sub-block index."""
    for sub in ("sub_blocks1", "sub_blocks0"):
        candidate = os.path.join(dump_dir, sub, "level.json")
        if os.path.exists(candidate):
            return candidate
    # Also try the top of the dump (some dumps don't wrap in a sub-dir)
    candidate = os.path.join(dump_dir, "level.json")
    if os.path.exists(candidate):
        return candidate
    return None


def fresh_guid(level):
    """Allocate a GUID one above the max existing GUID. Brute and simple,
    but the engine sorts entities by GUID at parse time so there's no
    semantic meaning to the number — just uniqueness."""
    used = set()
    for obj in level.get("objs", []):
        g = obj.get("fields", {}).get("GUID", 0)
        if isinstance(g, int):
            used.add(g)
    return (max(used) + 1) if used else 99000001


def find_existing_trigger(level):
    """Return the first trigger_box or trigger_radius entity that has an
    OnEnter Output slot we can hook into. We prefer trigger_box for the
    test because the player walking through it is the easiest way to fire
    an event. Returns the entity dict or None."""
    candidates = []
    for obj in level.get("objs", []):
        t = obj.get("type", "")
        if t in ("trigger_box", "trigger_radius"):
            candidates.append(obj)
    # Prefer triggers that aren't already wired to a million things — those
    # are harder to isolate. Pick the one with the smallest Outputs list.
    if not candidates:
        return None
    def output_count(o):
        outs = o.get("fields", {}).get("Outputs", [])
        return len(outs) if isinstance(outs, list) else 0
    candidates.sort(key=output_count)
    return candidates[0]


def find_existing_crowd_archetype(level):
    """Return (crowdItemIdx, crowdValIdx) for an arbitrary crowd target. The
    runtime DLL isn't built yet so this is just for the metadata fields —
    they don't have to be accurate during Half 1. Returns (0, 0) if we
    can't determine real values."""
    # 3dCrowd lives in sub_blocks2; level.json (sub_blocks1) doesn't carry
    # it directly. For Half 1 we hardcode (0, 0) as a placeholder.
    return (0, 0)


def main(argv):
    if len(argv) != 2:
        print("Usage: crowd_proxy_test.py <dumped_level_dir>")
        print("  (the directory created by `lotrc_rs.exe -d <pak>`)")
        return 1

    dump_dir = argv[1]
    if not os.path.isdir(dump_dir):
        print("ERROR: %r is not a directory" % dump_dir)
        return 1

    level_path = find_level_json(dump_dir)
    if not level_path:
        print("ERROR: no level.json found anywhere in %r" % dump_dir)
        print("  Expected one of: sub_blocks1/level.json, sub_blocks0/level.json")
        return 1

    print("[1/6] Reading: %s" % level_path)
    with open(level_path, "r", encoding="utf-8") as f:
        level = json.load(f)

    # Backup before we touch anything. If the user re-runs the script we
    # don't overwrite the original backup — first run wins for safety.
    backup_path = level_path + ".bak"
    if not os.path.exists(backup_path):
        shutil.copy2(level_path, backup_path)
        print("[2/6] Backed up original: %s" % backup_path)
    else:
        print("[2/6] Backup already exists at %s (not overwriting)" % backup_path)

    # ── Inject CrowdProxy into types[] ────────────────────────────────────
    types_arr = level.setdefault("types", [])
    existing = [t for t in types_arr if t.get("name") == "CrowdProxy"]
    if existing:
        print("[3/6] CrowdProxy type already in types[]. Skipping schema injection.")
    else:
        types_arr.append(copy.deepcopy(CROWD_PROXY_SCHEMA))
        print("[3/6] Injected CrowdProxy schema (%d fields) into types[]"
              % len(CROWD_PROXY_SCHEMA["fields"]))

    # ── Build the test CrowdProxy entity ──────────────────────────────────
    proxy_guid = fresh_guid(level)
    crowd_item, crowd_val = find_existing_crowd_archetype(level)

    proxy_entity = {
        "type": "CrowdProxy",
        "layer": 0,
        "fields": {
            "GUID": proxy_guid,
            "ParentGUID": 0,
            "GameModeMask": -1,  # appears in every gamemode
            "Name": "VespCrowdProxy_Test",
            "Factory": "logic_relay",
            "EnableEvents": True,
            "Outputs": [],
            # Metadata fields — runtime DLL will read these once it exists.
            # Half 1 doesn't care what's here, it just verifies the engine
            # doesn't choke on the field NAMES being unrecognized.
            "VespCrowdAction": "destroy",
            "VespCrowdTargetItem": crowd_item,
            "VespCrowdTargetMode": "in_radius",
            "VespCrowdTargetValIdx": crowd_val,
            "VespCrowdTargetRadius": 15.0,
            "VespCrowdTargetCenter": [0.0, 0.0, 0.0],
            "VespCrowdValIdxList": [],
        },
    }
    level.setdefault("objs", []).append(proxy_entity)
    print("[4/6] Added CrowdProxy entity (GUID=%d) targeting crowd item %d val %d"
          % (proxy_guid, crowd_item, crowd_val))

    # ── Wire an existing trigger to fire the proxy ────────────────────────
    # We hook the first trigger we find with the smallest Outputs[] to keep
    # the test isolated. The wire is a brand-new Output entity that sits in
    # the trigger's Outputs list.
    trigger = find_existing_trigger(level)
    if trigger is None:
        print("[5/6] WARNING: no trigger_box or trigger_radius found.")
        print("       The CrowdProxy entity exists but nothing fires it.")
        print("       Either pick a level with a trigger, or wire it by hand")
        print("       in Vespucci's Forge after loading the repacked PAK.")
    else:
        # Build a new Output entity that the trigger references in its Outputs[]
        output_guid = fresh_guid(level) + 1  # one above the proxy
        trigger_name = trigger.get("fields", {}).get("Name", "<unnamed>")
        trigger_guid = trigger.get("fields", {}).get("GUID", 0)
        output_entity = {
            "type": "Output",
            "layer": trigger.get("layer", 0),
            "fields": {
                "GUID": output_guid,
                "ParentGUID": 0,
                "GameModeMask": trigger.get("fields", {}).get("GameModeMask", -1),
                "Name": "VespCrowdProxy_TestWire",
                "Output": "OnEnter",
                "target": proxy_guid,
                "Input": "Trigger",
                "delay": 0.0,
                "Sticky": True,
                "Parameter": "",
            },
        }
        level["objs"].append(output_entity)
        # Add the output's GUID to the trigger's Outputs[] list
        outs = trigger["fields"].setdefault("Outputs", [])
        outs.append(output_guid)
        print("[5/6] Wired trigger %r (GUID=%d) .OnEnter -> proxy .Trigger"
              % (trigger_name, trigger_guid))

    # ── Write level.json back ─────────────────────────────────────────────
    with open(level_path, "w", encoding="utf-8") as f:
        json.dump(level, f, indent=2)
    print("[6/6] Wrote mutated level.json")

    # ── Next steps banner ─────────────────────────────────────────────────
    print()
    print("=" * 70)
    print("  HALF 1 SETUP COMPLETE. NEXT STEPS:")
    print("=" * 70)
    print()
    print("  1) Repack:")
    print("       lotrc_rs.exe \"%s\" -o <out_dir>" % dump_dir)
    print()
    print("  2) Back up the original PAK in your game's Levels folder.")
    print("     Copy the repacked PAK/BIN over the originals.")
    print()
    print("  3) Launch the game, load the level. Find the trigger we hooked:")
    if trigger is not None:
        tf = trigger.get("fields", {})
        wt = tf.get("WorldTransform", [])
        if isinstance(wt, list) and len(wt) >= 15:
            print("       Trigger position (approx): (%.1f, %.1f, %.1f)"
                  % (wt[12], wt[13], wt[14]))
        print("       Trigger name: %s" % tf.get("Name", "<unnamed>"))
        print("       Trigger GUID: %d" % tf.get("GUID", 0))
    print()
    print("  4) Walk into the trigger. Watch the console / debug log.")
    print()
    print("  WHAT SHOULD HAPPEN:")
    print("    - Game does NOT crash on level load.  ← biggest signal")
    print("    - CrowdProxy entity is created at runtime (Factory=logic_relay).")
    print("    - On trigger fire, the proxy's .Trigger receives the event.")
    print("    - Crowd does NOT disappear (no injection DLL yet — Phase 2).")
    print()
    print("  IF IT CRASHES ON LOAD:")
    print("    - Restore: copy %s back over level.json" % backup_path)
    print("    - Possibilities: schema offsets misaligned, field type wrong,")
    print("      field name CRC collides with a real logic_relay field, the")
    print("      type-registry BST behaves differently than disassembly says.")
    print("    - Tell me what you see and we figure out which.")
    print()
    print("  IF IT LOADS BUT THE PROXY DOESN'T FIRE:")
    print("    - Less catastrophic — means the schema parsed but the Factory")
    print("      override didn't latch. Worth checking the binary blob with a")
    print("      hex viewer to see if Factory string actually made it in.")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
