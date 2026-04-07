from pathlib import Path
import sys

# Make shared hash helpers importable
SCRIPT_ROOT = Path(__file__).resolve().parents[1]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.append(str(SCRIPT_ROOT))
from wwise_hash import fnv1_hash, hash_none_test  # type: ignore

# Two naming conventions to compare:
names = [
    "VO_TRN_KeepFighting_remind_02",     # From level.json (canonical)
    "play_vo_trn_keepfighting_remind_02", # From event_mapping.h (play_ prefix)
]

print("=" * 60)
print("HASH COMPARISON: level.json vs event_mapping.h")
print("=" * 60)
for name in names:
    h = fnv1_hash(name)
    print(f"  {name}")
    print(f"    Hash: 0x{h:08X} (dec: {h})")
    print()

# The hash from event_mapping.h
print("=" * 60)
print("VALIDATION AGAINST event_mapping.h:")
print("=" * 60)
print("  In event_mapping.h line 2550:")
print("    {0xEA28F201U, \"VO_Trng\", \"play_vo_trn_keepfighting_remind_02\"}")
print()
expected = 0xEA28F201
actual = fnv1_hash("play_vo_trn_keepfighting_remind_02")
print(f"  Expected: 0x{expected:08X}")
print(f"  Computed: 0x{actual:08X}")
print(f"  Match:    {expected == actual}")
print()

# Quick sanity check against SDK constant HashName(\"none\") == 0x2CB8CAAB
print("HashName(\"none\") SDK check:", hash_none_test())
print()

# Check if TXTP file uses CAkEvent ID or hash
print("=" * 60)
print("TXTP File Analysis (VO_Trng-0146-event):")
print("=" * 60)
print("  CAkEvent[146] 3337683776")
print(f"  3337683776 = 0x{3337683776:08X}")
print()
print("  This is the EVENT ID (index within BNK), not a string hash.")
print("  The string hash is used by GetIDFromString/PostEvent.")
