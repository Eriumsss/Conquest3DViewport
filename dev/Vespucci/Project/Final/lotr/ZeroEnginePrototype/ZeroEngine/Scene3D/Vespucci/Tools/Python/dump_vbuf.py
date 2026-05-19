"""
Standalone diagnostic: reads any level PAK, decompresses Block1/Block2,
then dumps the VBuffInfo for the first model's first buffer + first vertex XYZ.

Usage:
    python dump_vbuf.py <level.PAK>
"""
import struct, zlib, sys

if len(sys.argv) < 2:
    print("Usage: python dump_vbuf.py <level.PAK>")
    sys.exit(1)

PAK = sys.argv[1]

with open(PAK, 'rb') as f:
    data = f.read()

def u32(buf, off): return struct.unpack_from('<I', buf, off)[0]
def f32(buf, off): return struct.unpack_from('<f', buf, off)[0]

# PakHeader fields (each u32, starting at byte 0)
# block_a_num=0, block_a_offset=4, constx13=8, version=12,
# strings_offset=16, strings_size=20, strings_num=24,
# block1_offset=28, block1_size=32, block1_size_comp=36,
# sub_blocks1_offset=40,
# block2_offset=44, block2_size=48, block2_size_comp=52
b1_off  = u32(data, 28)
b1_sz   = u32(data, 32)
b1_szc  = u32(data, 36)
b2_off  = u32(data, 44)
b2_sz   = u32(data, 48)
b2_szc  = u32(data, 52)

print(f"Block1: file_off={b1_off}  size={b1_sz}  comp_size={b1_szc}")
print(f"Block2: file_off={b2_off}  size={b2_sz}  comp_size={b2_szc}")

# Decompress (or copy raw)
if b1_szc != b1_sz:
    b1 = zlib.decompress(data[b1_off:b1_off+b1_szc])
else:
    b1 = data[b1_off:b1_off+b1_sz]

if b2_szc != b2_sz:
    b2 = zlib.decompress(data[b2_off:b2_off+b2_szc])
else:
    b2 = data[b2_off:b2_off+b2_sz]

print(f"Block1 decompressed: {len(b1)}  Block2 decompressed: {len(b2)}")

# From debug log:
#   hdr.buffer_info_size=356
#   mdl.buffer_info_offset=514896  (ModelInfo[157] for BKG_BL_MountainSideEast_01)
#   BufferInfo[0]:  vbOff0=1128192  vbOff2=0  vStride0=32  ibIdx=1171104  i_num=3138
VB_INFO_OFF = 1128192  # absolute Block1 byte offset of VBuffInfo

# VBuffInfo layout (8 x u32 = 32 bytes):
vb_unk0   = u32(b1, VB_INFO_OFF +  0)
vb_size   = u32(b1, VB_INFO_OFF +  4)
vb_unk3   = u32(b1, VB_INFO_OFF +  8)
vb_offset = u32(b1, VB_INFO_OFF + 12)  # <- Block2 byte offset for vertex data
vb_fmt1   = u32(b1, VB_INFO_OFF + 16)
vb_fmt2   = u32(b1, VB_INFO_OFF + 20)

print(f"\nVBuffInfo @ Block1[{VB_INFO_OFF}]:")
print(f"  unk0={vb_unk0}  size={vb_size}  unk3={vb_unk3}")
print(f"  offset={vb_offset} (Block2 byte offset)  in_range={vb_offset + vb_size <= len(b2)}")
print(f"  fmt1=0x{vb_fmt1:08X}  fmt2=0x{vb_fmt2:08X}")

STRIDE = 32
if vb_offset + 12 <= len(b2):
    print("\nFirst 8 vertices (stride=32, pos at byte 0):")
    for i in range(min(8, vb_size // STRIDE)):
        base = vb_offset + i * STRIDE
        x = f32(b2, base + 0)
        y = f32(b2, base + 4)
        z = f32(b2, base + 8)
        print(f"  v[{i}] = ({x:.4f}, {y:.4f}, {z:.4f})")
else:
    print(f"\nERROR: vb_offset={vb_offset} OUT OF RANGE (b2 size={len(b2)})")
    # Try: what if vbOff0 is NOT a Block1 byte offset but an INDEX into VBuffInfo array?
    # hdr.vbuff_info_offset = 1091648
    vbuff_arr_start = 1091648
    alt_off = vbuff_arr_start + VB_INFO_OFF  # relative interpretation
    print(f"\nAlternative: treating vbOff0 as relative offset from vbuff array start")
    print(f"  alt Block1 offset = {alt_off}  in_range={alt_off + 32 <= len(b1)}")
    if alt_off + 32 <= len(b1):
        vb2_offset = u32(b1, alt_off + 12)
        vb2_size   = u32(b1, alt_off + 4)
        print(f"  -> b2offset={vb2_offset}  size={vb2_size}  in_range={vb2_offset+vb2_size<=len(b2)}")
        if vb2_offset + 12 <= len(b2):
            for i in range(min(4, vb2_size // STRIDE)):
                base = vb2_offset + i * STRIDE
                x = f32(b2, base)
                y = f32(b2, base+4)
                z = f32(b2, base+8)
                print(f"  v[{i}] = ({x:.4f}, {y:.4f}, {z:.4f})")

# Also dump IBuffInfo for cross-check
IB_INFO_OFF = 1171104  # ibIdx from debug log
ib_unk0   = u32(b1, IB_INFO_OFF +  0)
ib_size   = u32(b1, IB_INFO_OFF +  4)
ib_fmt    = u32(b1, IB_INFO_OFF +  8)
ib_altfmt = u32(b1, IB_INFO_OFF + 12)
ib_offset = u32(b1, IB_INFO_OFF + 16)
print(f"\nIBuffInfo @ Block1[{IB_INFO_OFF}]:")
print(f"  unk0={ib_unk0}  size={ib_size}  format={ib_fmt}(0=U16)")
print(f"  offset={ib_offset}  in_range={ib_offset+ib_size<=len(b2)}")
if ib_offset + 6 <= len(b2):
    i0,i1,i2 = struct.unpack_from('<HHH', b2, ib_offset)
    print(f"  first triangle indices: {i0},{i1},{i2}")
