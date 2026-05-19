import struct
import zlib
import os
import sys
from collections import defaultdict

# check_fmt1.py - format-1 PAK section validator
#
# Usage:
#   python check_fmt1.py <level.PAK> <level.BIN> [output.txt]
#
# If output.txt is omitted, results go to fmt1_check.txt next to the script.
if len(sys.argv) < 3:
    print("Usage: python check_fmt1.py <level.PAK> <level.BIN> [output.txt]")
    sys.exit(1)

PAK_PATH = sys.argv[1]
BIN_PATH = sys.argv[2]
OUT_PATH = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "fmt1_check.txt")

def read_u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def load_block(file_data, block_offset, block_size, block_size_comp):
    raw = file_data[block_offset:block_offset + block_size_comp]
    if block_size_comp == block_size:
        return raw
    else:
        return zlib.decompress(raw)

# ── PAK ──────────────────────────────────────────────────────────────────────
with open(PAK_PATH, 'rb') as f:
    pak_data = f.read()

block1_offset      = read_u32(pak_data, 28)
block1_size        = read_u32(pak_data, 32)
block1_size_comp   = read_u32(pak_data, 36)
block2_offset      = read_u32(pak_data, 44)
block2_size        = read_u32(pak_data, 48)
block2_size_comp   = read_u32(pak_data, 52)
model_info_offset  = read_u32(pak_data, 72)
model_info_num     = read_u32(pak_data, 76)
buffer_info_size   = read_u32(pak_data, 80)
vbuff_info_offset  = read_u32(pak_data, 260)
ibuff_info_offset  = read_u32(pak_data, 264)

print(f"block1_offset={block1_offset:#010x}  size={block1_size}  comp={block1_size_comp}")
print(f"block2_offset={block2_offset:#010x}  size={block2_size}  comp={block2_size_comp}")
print(f"model_info_offset={model_info_offset:#010x}  num={model_info_num}")
print(f"buffer_info_size={buffer_info_size}")
print(f"vbuff_info_offset={vbuff_info_offset:#010x}  ibuff_info_offset={ibuff_info_offset:#010x}")

block1 = load_block(pak_data, block1_offset, block1_size, block1_size_comp)
print(f"Block1 decompressed: {len(block1)} bytes")

# ── BIN ───────────────────────────────────────────────────────────────────────
with open(BIN_PATH, 'rb') as f:
    bin_header = f.read(64)

asset_handle_num    = read_u32(bin_header, 20)
asset_handle_offset = read_u32(bin_header, 24)
vdata_num           = read_u32(bin_header, 32)

print(f"\nBIN: asset_handle_num={asset_handle_num}  asset_handle_offset={asset_handle_offset:#x}  vdata_num={vdata_num}")

# Read all asset handles
with open(BIN_PATH, 'rb') as f:
    f.seek(asset_handle_offset)
    handle_data = f.read(asset_handle_num * 20)

bin_assets = {}  # key -> (file_offset, size, size_comp, kind)
for i in range(asset_handle_num):
    base = i * 20
    key       = read_u32(handle_data, base + 0)
    offset    = read_u32(handle_data, base + 4)
    size      = read_u32(handle_data, base + 8)
    size_comp = read_u32(handle_data, base + 12)
    kind      = read_u32(handle_data, base + 16)
    bin_assets[key] = (offset, size, size_comp, kind)

print(f"Loaded {len(bin_assets)} BIN asset handles ({vdata_num} are mesh assets)")

def get_bin_asset(key):
    """Return decompressed bytes for a BIN asset by key."""
    if key not in bin_assets:
        return None
    offset, size, size_comp, kind = bin_assets[key]
    with open(BIN_PATH, 'rb') as f:
        f.seek(offset)
        raw = f.read(size_comp)
    if size_comp == size:
        return raw
    return zlib.decompress(raw)

# ── Helper: read VBuffInfo from block1 ───────────────────────────────────────
VBUFF_INFO_SIZE = 32
IBUFF_INFO_SIZE = 24
BUFFER_INFO_SIZE = 356
MODEL_INFO_SIZE = 256

def read_vbuff_info(offset_in_block1):
    """Returns (size, data_offset, fmt1, fmt2)"""
    base = offset_in_block1
    size        = read_u32(block1, base + 4)
    data_offset = read_u32(block1, base + 12)
    fmt1        = read_u32(block1, base + 16)
    fmt2        = read_u32(block1, base + 20)
    return size, data_offset, fmt1, fmt2

def read_ibuff_info(offset_in_block1):
    base = offset_in_block1
    size        = read_u32(block1, base + 4)
    fmt         = read_u32(block1, base + 8)
    data_offset = read_u32(block1, base + 16)
    return size, fmt, data_offset

def read_buffer_info(offset_in_block1):
    """Returns (vbuff_info_off, vbuff_info_off2, v_size, ibuff_info_off, i_num)"""
    base = offset_in_block1
    vbuff_info_off  = read_u32(block1, base + 0)
    vbuff_info_off2 = read_u32(block1, base + 4)
    v_size          = read_u32(block1, base + 128)
    ibuff_info_off  = read_u32(block1, base + 260)
    i_num           = read_u32(block1, base + 264)
    return vbuff_info_off, vbuff_info_off2, v_size, ibuff_info_off, i_num

# ── fmt1 decoder ─────────────────────────────────────────────────────────────
def decode_fmt1(fmt1):
    """
    Returns (expected_stride, normal_offset, uv0_offset, has_blend, component_list)
    Layout order (matches typical GPU vertex buffer packing):
      Position      (0x001) 12 bytes
      BlendWeight   (0x400)  4 bytes
      BlendIndices  (0x800)  4 bytes
      Normal        (0x002)  4 bytes
      Color0        (0x100)  4 bytes
      Color1        (0x200)  4 bytes
      UV[0..N-1]    each     8 bytes  (N = (fmt1>>2)&0xF)
      Tangent       (0x040)  4 bytes
      PSize         (0x080) 12 bytes
    """
    offset = 0
    normal_offset = -1
    uv0_offset = -1
    has_blend = False
    components = []

    if fmt1 & 0x001:
        components.append(('Position', offset, 12))
        offset += 12

    if fmt1 & 0x400:
        components.append(('BlendWeight', offset, 4))
        offset += 4
        has_blend = True

    if fmt1 & 0x800:
        components.append(('BlendIndices', offset, 4))
        offset += 4
        has_blend = True

    if fmt1 & 0x002:
        normal_offset = offset
        components.append(('Normal', offset, 4))
        offset += 4

    if fmt1 & 0x100:
        components.append(('Color0', offset, 4))
        offset += 4

    if fmt1 & 0x200:
        components.append(('Color1', offset, 4))
        offset += 4

    n_uv = (fmt1 >> 2) & 0xF
    for i in range(n_uv):
        if i == 0:
            uv0_offset = offset
        components.append((f'UV[{i}]', offset, 8))
        offset += 8

    if fmt1 & 0x040:
        components.append(('Tangent', offset, 4))
        offset += 4

    if fmt1 & 0x080:
        components.append(('PSize', offset, 12))
        offset += 12

    return offset, normal_offset, uv0_offset, has_blend, components

# ── Main scan ─────────────────────────────────────────────────────────────────
# unique_pairs: (stride, fmt1) -> count
unique_pairs = defaultdict(int)
errors = []

total_models = 0
total_slots = 0

for mi in range(model_info_num):
    mi_base = model_info_offset + mi * MODEL_INFO_SIZE
    if mi_base + MODEL_INFO_SIZE > len(block1):
        errors.append(f"ModelInfo[{mi}] out of bounds at {mi_base}")
        continue

    key              = read_u32(block1, mi_base + 0)
    buffer_info_off  = read_u32(block1, mi_base + 12)
    mesh_order_off   = read_u32(block1, mi_base + 48)

    # LOD0: 5 u32s at byte 52
    lod0_start        = read_u32(block1, mi_base + 52)
    lod0_static_end   = read_u32(block1, mi_base + 56)
    lod0_skinned_end  = read_u32(block1, mi_base + 60)
    lod0_physics_end  = read_u32(block1, mi_base + 64)
    lod0_break_end    = read_u32(block1, mi_base + 68)

    asset_key        = read_u32(block1, mi_base + 208)

    total_models += 1
    static_count = lod0_static_end - lod0_start
    if static_count <= 0:
        continue

    for slot in range(lod0_start, lod0_static_end):
        total_slots += 1

        # mesh_order_off points to an array of slot indices (u32 per slot)
        # The slot index selects which BufferInfo to use
        slot_bi_off = mesh_order_off + slot * 4
        if slot_bi_off + 4 > len(block1):
            errors.append(f"  ModelInfo[{mi}] slot {slot}: mesh_order out of bounds")
            continue

        bi_index = read_u32(block1, slot_bi_off)

        # BufferInfo array starts at buffer_info_off
        bi_off = buffer_info_off + bi_index * BUFFER_INFO_SIZE
        if bi_off + BUFFER_INFO_SIZE > len(block1):
            errors.append(f"  ModelInfo[{mi}] slot {slot}: BufferInfo[{bi_index}] out of bounds at {bi_off}")
            continue

        try:
            vbuff_info_off, _, v_size, ibuff_info_off, i_num = read_buffer_info(bi_off)
        except Exception as e:
            errors.append(f"  ModelInfo[{mi}] slot {slot}: read_buffer_info failed: {e}")
            continue

        if vbuff_info_off == 0 or vbuff_info_off >= len(block1):
            errors.append(f"  ModelInfo[{mi}] slot {slot}: invalid vbuff_info_off {vbuff_info_off:#x}")
            continue

        try:
            vb_size, vb_data_off, fmt1, fmt2 = read_vbuff_info(vbuff_info_off)
        except Exception as e:
            errors.append(f"  ModelInfo[{mi}] slot {slot}: read_vbuff_info failed: {e}")
            continue

        unique_pairs[(v_size, fmt1)] += 1

print(f"\nScanned {total_models} models, {total_slots} LOD0 static slots")
if errors:
    print(f"Errors encountered: {len(errors)}")
    for e in errors[:20]:
        print(" ", e)
    if len(errors) > 20:
        print(f"  ... and {len(errors)-20} more")

# ── Build report ──────────────────────────────────────────────────────────────
lines = []
lines.append("=" * 100)
lines.append(f"{'stride':>8} | {'fmt1':>10} | {'exp_stride':>10} | {'match':>5} | {'normal_off':>10} | {'uv0_off':>7} | {'has_blend':>9} | {'count':>6} | components")
lines.append("=" * 100)

for (stride, fmt1), count in sorted(unique_pairs.items(), key=lambda x: (-x[1], x[0][0])):
    exp_stride, norm_off, uv0_off, has_blend, comps = decode_fmt1(fmt1)
    match = "OK" if exp_stride == stride else f"MISMATCH"
    comp_str = "  ".join(f"{name}@{off}({sz}b)" for name, off, sz in comps)
    lines.append(
        f"{stride:>8} | {fmt1:#010x} | {exp_stride:>10} | {match:>5} | "
        f"{norm_off:>10} | {uv0_off:>7} | {str(has_blend):>9} | {count:>6} | {comp_str}"
    )

lines.append("=" * 100)
lines.append(f"Total unique (stride, fmt1) pairs: {len(unique_pairs)}")
lines.append(f"Total LOD0 static slots scanned: {total_slots}")

report = "\n".join(lines)
print("\n" + report)

with open(OUT_PATH, 'w', encoding='utf-8') as f:
    f.write(report + "\n")

print(f"\nOutput written to {OUT_PATH}")
