"""Scan all LOD0 static mesh slots and report unique (stride, fmt1) combos.
Uses confirmed field offsets from LevelReader.h and C++ debug output.

Usage:
    python scan_fmt1.py <level.PAK>
"""
import struct, zlib, sys

if len(sys.argv) < 2:
    print("Usage: python scan_fmt1.py <level.PAK>")
    sys.exit(1)

PAK = sys.argv[1]

with open(PAK, "rb") as f:
    data = f.read()

def u32(buf, off): return struct.unpack_from("<I", buf, off)[0]

# Raw PAK header (PakHeader struct starts at raw file offset 0)
b1_off  = u32(data, 28)
b1_sz   = u32(data, 32)
b1_szc  = u32(data, 36)
b1 = zlib.decompress(data[b1_off:b1_off+b1_szc]) if b1_szc != b1_sz else data[b1_off:b1_off+b1_sz]
print(f"Block1: {len(b1)} bytes")

# PakHeader is at the START of the raw PAK file (data), NOT in decompressed block1.
# All offsets in PakHeader are byte offsets INTO block1.
# Verified field positions:
#   byte  80 = buffer_info_size  (PakHeader field[20])
#   byte 176 = model_info_num    (PakHeader field[44])
#   byte 276 = model_info_offset (PakHeader field[69])
model_info_num    = u32(data, 176)
model_info_offset = u32(data, 276)
buffer_info_size  = u32(data, 80)

print(f"model_info_num={model_info_num}  model_info_offset={model_info_offset}  buffer_info_size={buffer_info_size}")
# Verify against known debug values
assert model_info_num == 249,    f"Unexpected model_info_num={model_info_num}"
assert model_info_offset == 46736, f"Unexpected model_info_offset={model_info_offset}"
assert buffer_info_size == 356,  f"Unexpected buffer_info_size={buffer_info_size}"
print("Assertions OK\n")

# ModelInfo layout (confirmed from struct + debug):
#   byte 12: buffer_info_offset
#   byte 48: mesh_order_offset
#   byte 52: lod0.start
#   byte 56: lod0.static_end
MODEL_SIZE = 256

# VBuffInfo layout (from LevelReader.h):
#   byte  4: size
#   byte 12: offset (BIN asset byte offset)
#   byte 16: fmt1
VBUFF_SIZE = 32

# BufferInfo layout (from LevelScene.cpp):
#   byte   0: vbuff_info_offset  (Block1 byte ptr → VBuffInfo)
#   byte   4: vbuff_info_offset2 (alt, 0=unused, 0xFFFFFFFF=sentinel)
#   byte 128: v_size  (stride)
#   byte 132: v_size2 (stride2)
#   byte 260: ibuff_info_offset
#   byte 264: i_num
BUF_SIZE = buffer_info_size  # 356

def decode_fmt1(fmt1):
    """Returns (stride, normal_off, uv0_off, components_list)"""
    off = 0; n_off = -1; uv_off = -1; comps = []
    if fmt1 & 0x001: comps.append(("Pos",    off, 12)); off += 12
    if fmt1 & 0x400: comps.append(("BW",     off,  4)); off += 4
    if fmt1 & 0x800: comps.append(("BI",     off,  4)); off += 4
    if fmt1 & 0x002: n_off = off; comps.append(("Nrm",  off,  4)); off += 4
    if fmt1 & 0x100: comps.append(("Col0",   off,  4)); off += 4
    if fmt1 & 0x200: comps.append(("Col1",   off,  4)); off += 4
    nuv = (fmt1 >> 2) & 0xF
    for i in range(nuv):
        if i == 0: uv_off = off
        comps.append((f"UV{i}", off, 8)); off += 8
    if fmt1 & 0x040: comps.append(("Tan",    off,  4)); off += 4
    if fmt1 & 0x080: comps.append(("PSz",    off, 12)); off += 12
    return off, n_off, uv_off, comps

combos = {}  # (stride, fmt1) -> count

for mi in range(model_info_num):
    m_base = model_info_offset + mi * MODEL_SIZE
    buf_info_off  = u32(b1, m_base + 12)
    mesh_order_off = u32(b1, m_base + 48)
    lod0_start     = u32(b1, m_base + 52)
    lod0_static_end= u32(b1, m_base + 56)

    for slot in range(lod0_start, lod0_static_end):
        mo_off = mesh_order_off + slot * 4
        if mo_off + 4 > len(b1): continue
        buf_idx = u32(b1, mo_off) & 0x3FFFFFFF
        bi_off  = buf_info_off + buf_idx * BUF_SIZE
        if bi_off + BUF_SIZE > len(b1): continue

        vb0_off  = u32(b1, bi_off + 0)
        vb2_off  = u32(b1, bi_off + 4)
        stride0  = u32(b1, bi_off + 128)
        stride2  = u32(b1, bi_off + 132)

        use2  = (vb2_off != 0 and vb2_off != 0xFFFFFFFF)
        vb_off  = vb2_off  if use2 else vb0_off
        stride  = stride2  if use2 else stride0

        if vb_off == 0 or vb_off + VBUFF_SIZE > len(b1): continue
        fmt1 = u32(b1, vb_off + 16)

        key = (stride, fmt1)
        combos[key] = combos.get(key, 0) + 1

print(f"{'stride':>8}  {'fmt1':>10}  {'calc_stride':>11}  {'nrm_off':>7}  {'uv0_off':>7}  {'count':>6}  layout")
print("-" * 100)
for (stride, fmt1), cnt in sorted(combos.items()):
    calc, n_off, uv_off, comps = decode_fmt1(fmt1)
    match = "OK" if calc == stride else "MISMATCH"
    layout = "  ".join(f"{n}@{o}" for n, o, _ in comps)
    print(f"{stride:>8}  {fmt1:#010x}  {calc:>11}  {n_off:>7}  {uv_off:>7}  {cnt:>6}  {layout}  [{match}]")
