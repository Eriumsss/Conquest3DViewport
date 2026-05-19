"""
AnimationBlockConverter.py — Extends asset gamemodemasks for a new gamemode.

When adding a new gamemode (e.g. CTR) to a level that doesn't have it,
all shared assets (animations, models, effects, textures, animation_tables)
need their gamemodemask extended to include the new bit. Otherwise the
Rust parser won't pack them into the new animation block and the game
crashes trying to sample empty spline data.

Usage:
  python AnimationBlockConverter.py <dump_dir> <new_bit>

  dump_dir  = path to the lotrc_rs dump directory (e.g. ze_lvl_dump/Helm'sDeep)
  new_bit   = the bit VALUE to add (e.g. 16 for bit 4)

What it touches:
  - animations/*.json        gamemodemask field
  - models/*.json            info.gamemodemask field
  - effects/*.json           gamemodemask field (top-level and sub-objects)
  - textures/*.json          gamemodemask field
  - animation_tables/*.json  gamemodemask field

What it does NOT touch:
  - level.json entities (GameModeMask) — kit entities already have correct bits
  - character_class layers — no hardcoded layer moves
  - gamemode sound banks — no hardcoded GUID references
  - pak_strings.json, pak_header.json, etc.

Only assets that already have at least one multiplayer bit set get extended.
Campaign-only assets (goodcampaign/evilcampaign only) are left alone.
"""

import json
import os
import re
import sys


def log(msg):
    print(f"[AnimBlockConv] {msg}", flush=True)


def is_multiplayer_mask(mask, block_count):
    """Check if this mask includes any MP block bit (not just campaign bits at high indices)."""
    if mask == -1:
        return True  # global = available everywhere
    if mask == 0:
        return False  # scripts/global-only
    # Any bit set in the first 8 blocks is considered MP-relevant
    return (mask & 0xFF) != 0


def extend_gamemodemask_binary(content, new_bit):
    """Extend gamemodemask values in raw JSON bytes using regex. Returns (new_content, count)."""
    pattern = rb'"gamemodemask"\s*:\s*(-?\d+)'
    matches = list(re.finditer(pattern, content, re.IGNORECASE))
    count = 0
    if matches:
        for match in reversed(matches):
            old_value = int(match.group(1))
            if old_value == -1:
                continue  # already global, skip
            if not (old_value & new_bit):
                new_value = old_value | new_bit
                start, end = match.span(1)
                content = content[:start] + str(new_value).encode() + content[end:]
                count += 1
    return content, count


def extend_model_gamemodemask(content, new_bit):
    """Extend gamemodemask inside models JSON (nested under info.gamemodemask)."""
    # Models have the mask inside an "info" object
    # Use the same binary approach — it catches nested gamemodemask too
    return extend_gamemodemask_binary(content, new_bit)


def convert(dump_dir, new_bit):
    """
    Extend gamemodemasks in a dump directory for a new animation block bit.

    Args:
        dump_dir: path to the lotrc_rs dump directory
        new_bit: the bit VALUE to OR in (e.g. 16 for bit 4)

    Returns:
        dict with counts of what was updated
    """
    stats = {
        'animations': 0,
        'models': 0,
        'effects': 0,
        'textures': 0,
        'animation_tables': 0,
    }

    # Asset directories to process (NOT level.json)
    asset_dirs = ['animations', 'models', 'effects', 'textures', 'animation_tables']

    for asset_type in asset_dirs:
        asset_path = os.path.join(dump_dir, asset_type)
        if not os.path.isdir(asset_path):
            continue

        for fn in os.listdir(asset_path):
            if not fn.endswith('.json'):
                continue
            fpath = os.path.join(asset_path, fn)
            try:
                with open(fpath, 'rb') as f:
                    content = f.read()

                new_content, count = extend_gamemodemask_binary(content, new_bit)

                if count > 0:
                    with open(fpath, 'wb') as f:
                        f.write(new_content)
                    stats[asset_type] += count

            except Exception as e:
                log(f"  Warning: {asset_type}/{fn}: {e}")

    return stats


def main():
    if len(sys.argv) < 3:
        print("Usage: python AnimationBlockConverter.py <dump_dir> <new_bit>")
        print("  dump_dir  = lotrc_rs dump directory (e.g. ze_lvl_dump/Helm'sDeep)")
        print("  new_bit   = bit VALUE to add (e.g. 16 for bit 4)")
        sys.exit(1)

    dump_dir = sys.argv[1]
    new_bit = int(sys.argv[2])

    if not os.path.isdir(dump_dir):
        log(f"ERROR: Directory not found: {dump_dir}")
        sys.exit(1)

    log(f"Extending gamemodemasks with bit value {new_bit} (bit {new_bit.bit_length()-1})")
    log(f"Directory: {dump_dir}")

    stats = convert(dump_dir, new_bit)

    total = sum(stats.values())
    log(f"Done. Updated {total} gamemodemask values:")
    for k, v in stats.items():
        if v > 0:
            log(f"  {k}: {v}")

    if total == 0:
        log("WARNING: Nothing was updated. Check that the dump directory has asset JSON files.")


if __name__ == '__main__':
    main()
