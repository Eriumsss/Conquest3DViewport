#!/usr/bin/env python3
"""
level_patcher.py — Full save pipeline: dump PAK → patch level.json → recompile PAK.

Usage:
  python level_patcher.py <original_pak> <entities_json> <lotrc_exe> <output_pak> [--strings str1 str2 ...]

This script:
  1. Dumps the original PAK via lotrc_rs to a temp directory
  2. Reads the entities_json and appends them to level.json
  2b. Imports missing type definitions from reference levels
  3. Scans ALL assets for strings (using CoreScripts approach) and updates pak_strings.json
  4. Recompiles via lotrc_rs to produce the output PAK
  5. Cleans up temp directories
"""

import json
import sys
import os
import math
import struct
import subprocess
import shutil


def log(msg):
    print(f"[level_patcher] {msg}", flush=True)
    # Also write to progress file for C++ to read
    try:
        with open("level_patcher_progress.txt", "a") as f:
            f.write(msg + "\n")
    except:
        pass


# ── String scanning (adapted from CoreScripts/update_debug_strings.py) ──
# Scans level.json + effects for all crc/crclist field values that need
# to be in pak_strings.json for the Rust parser to resolve them.

def gameobjs_crcs(objs, types):
    """Yield all string values from crc and crclist fields in game objects."""
    type_map = {}
    for ty in types:
        type_map[ty['name']] = {f['name']: f['type'] for f in ty.get('fields', [])}
    for obj in objs:
        ty = type_map.get(obj.get('type', ''), {})
        for name, v in obj.get('fields', {}).items():
            t = ty.get(name, '')
            if t == 'crc':
                if isinstance(v, str):
                    yield v
            elif t == 'crclist':
                if isinstance(v, list):
                    for v_ in v:
                        if isinstance(v_, str):
                            yield v_


def scan_all_strings(dump_dir, level_data):
    """
    Scan the entire dump directory for strings that need to be in pak_strings.json.
    Adapted from CoreScripts/update_debug_strings.py — scans level.json, effects,
    animations, textures, models.
    """
    found = set()

    # 1. Scan level.json crc/crclist fields
    for s in gameobjs_crcs(level_data.get('objs', []), level_data.get('types', [])):
        if s and not s.startswith('0x'):
            found.add(s)

    # 2. Scan entity names and type names from level objects
    for obj in level_data.get('objs', []):
        for field_name in obj.get('fields', {}).keys():
            if isinstance(field_name, str) and not field_name.startswith('0x'):
                found.add(field_name)
        obj_type = obj.get('type', '')
        if obj_type and not obj_type.startswith('0x'):
            found.add(obj_type)

    # 3. Scan type definition field names
    for ty in level_data.get('types', []):
        name = ty.get('name', '')
        if name and not name.startswith('0x'):
            found.add(name)
        for field in ty.get('fields', []):
            fname = field.get('name', '')
            if fname and not fname.startswith('0x'):
                found.add(fname)

    # 4. Scan effects for crc fields
    for dirpath, dirnames, filenames in os.walk(dump_dir):
        rel = os.path.relpath(dirpath, dump_dir).replace('\\', '/')
        for fn in filenames:
            fpath = os.path.join(dirpath, fn)
            if not fn.endswith('.json'):
                continue
            # Effects have their own objs/types
            if '/effects/' in rel.lower() + '/' or rel.lower().startswith('effects'):
                try:
                    with open(fpath, 'r', encoding='utf-8') as f:
                        edata = json.load(f)
                    if 'objs' in edata and 'types' in edata:
                        for s in gameobjs_crcs(edata['objs'], edata['types']):
                            if s and not s.startswith('0x'):
                                found.add(s)
                except Exception:
                    pass
            # Animations — extract key name
            elif '/animations/' in rel.lower() + '/' or rel.lower().startswith('animations'):
                try:
                    raw = open(fpath, 'rb').read()
                    if b'key": "' in raw:
                        name = raw.split(b'key": "')[1].split(b'"')[0].decode()
                        if name and not name.startswith('0x'):
                            found.add(name)
                except Exception:
                    pass
            # Models — extract key name
            elif '/models/' in rel.lower() + '/' or rel.lower().startswith('models'):
                try:
                    raw = open(fpath, 'rb').read()
                    if b'key": "' in raw:
                        name = raw.split(b'key": "')[1].split(b'"')[0].decode()
                        if name and not name.startswith('0x'):
                            found.add(name)
                except Exception:
                    pass

    return found


# ──────────────────────────────────────────────────────────────────────────
# Crowd Mesh Builder — Python merger
#
# Reads ze_merged_models.json (emitted by Vespucci's "Crowd Mesh Builder"
# panel) and bakes new merged character+weapon models into the dump dir
# before lotrc_rs -c picks them up.
#
# ── lotrc_rs model JSON shape (from inspecting WP_elf_ancn_bow_01.json
#    and CRD_CH_urk_spr_all_01.json) ────────────────────────────────────
#
# Top-level keys:
#   info                — header (mat_offset, bones_num, lod0..3, vbuff_num, etc.)
#   bone_parents        — list of parent indices (int, -1 for root)
#   bones               — list of bone NAMES (strings, parallel to bone_parents)
#   bone_transforms     — list of 4x4 mats in column form
#                         [{x:{x,y,z,w}, y:{x,y,z,w}, z:{x,y,z,w}, w:{x,y,z,w}}, …]
#                         w.{x,y,z} is the translation, columns x/y/z are the
#                         rotation+scale basis. Stored in MODEL/world bind pose.
#   bone_bounding_boxes — parallel to bones
#   mat_order           — list of material indices (ints into mats[])
#   mesh_order          — list of u32s. Bit 31 = visual, Bit 30 = collision/shadow.
#                         Low 30 bits = INDEX into buffer_infos[].
#   mesh_bounding_boxes — parallel to mesh_order
#   skin_binds, skin_order, vals_j, vals_k, val_k_header — runtime tables
#   slots, slot_map     — named attach-slot map (TrailTop, AttachBackUpper, …)
#   block_header, block_offsets, blocks — embedded sub-block data
#   mats                — list of material defs ({Mat1:{base:{…tex0/tex1/…}}, …})
#   mat_extras          — parallel to mats (often nulls)
#   buffer_infos        — list of draw-call records. Each entry has:
#                           vbuff_info_offset    — INDEX into vertex_data[]
#                           vbuff_info_offset_2  — alt INDEX (0xFFFFFFFF = unused)
#                           vbuff_info_offset_3  — alt INDEX (0xFFFFFFFF = unused)
#                           v_size, v_size_2, v_size_3 — per-stream stride
#                           vbuff_size, vbuff_size_2, vbuff_size_3 — per-stream size
#                           ibuff_info_offset    — INDEX into index_data[]
#                           i_num                — index count
#                           tri_num              — triangle count
#                           variation_id, variation
#   hk_constraint       — havok constraint sub-block (bone-name list, transforms,
#                         vals2). Parallel bone palette inside; if we grow bones[]
#                         we should grow this too for consistency, but the runtime
#                         tolerates a mismatch in v1 (Phase 5b TODO).
#   hk_constraint_datas, shapes — physics tables
#   vertex_data         — list of vertex BUFFERS (one per stream). Each entry:
#                           info: { size, offset, fmt1, fmt2, … }
#                           data: dict keyed by ATTRIBUTE NAME:
#                             "Position":    { val: { "Vector3":  [[xs…],[ys…],[zs…]] } }
#                             "Normal":      { val: { "Unorm4x8": [u32s] } }
#                             "Tangent":     { val: { "Unorm4x8": [u32s] } }
#                             "BlendWeight": { val: { "Unorm4x8": [u32s] } }
#                             "BlendIndices":{ val: { "Unorm4x8": [u32s] } }
#                             "TextureCoord(0)": { val: { "Vector2": [[us…],[vs…]] } }
#                             "Color(0)":    { val: { "Unorm4x8": [u32s] } }
#                         fmt1 flags (from LevelScene.cpp:2143+):
#                           0x001 = Position, 0x002 = Normal,
#                           0x040 = Tangent,  0x100 = Color0, 0x200 = Color1,
#                           0x400 = BlendWeight, 0x800 = BlendIndices,
#                           ((fmt1>>2)&0xF) = UV count
#                         Important: vertex attributes are stored Structure-of-Arrays
#                         (one parallel list per component) — NOT interleaved bytes.
#                         We never touch raw vertex stride at this JSON level.
#   index_data          — list of index BUFFERS, parallel to vertex_data entries.
#                         Each entry is either {"U16":{vals:[…]}} or {"U32":{vals:[…]}}.
#                         Indices are LOCAL to each buffer (start at 0 per entry),
#                         so we don't rebase them when appending.
#
# ── Merge strategy ────────────────────────────────────────────────────
# Because buffer_infos.vbuff_info_offset is an INDEX (not byte offset), and
# vertex_data + index_data are parallel arrays of self-contained streams, the
# merge is straightforward:
#
#   1. Bone palette: extend base.bones with any weapon bones not already there,
#      extending bone_parents / bone_transforms / bone_bounding_boxes in lock-step.
#      Build bone_idx_remap[weapon_idx] = base_idx.
#
#   2. Vertex transform: compute attach_world = base.bone_transforms[attach_bone] *
#      offset(XYZ, YPR). Pre-transform every position/normal/tangent component
#      in EACH of the weapon's vertex_data entries by this matrix (positions get
#      full 4x4, normals/tangents get rotation-only since they're directions).
#      Decode Unorm4x8 normals/tangents into 3 floats, transform, re-encode.
#
#   3. Bone remap: for any vertex_data entry that has a BlendIndices attribute,
#      remap each of the 4 packed bytes through bone_idx_remap[].
#
#   4. Append the weapon's vertex_data entries (now transformed) to base.vertex_data.
#      Remember the offset shift (= len(base.vertex_data) before append).
#
#   5. Same for weapon's index_data → base.index_data, tracking ibuff_offset shift.
#
#   6. For each of the weapon's buffer_infos: clone, add vbuff_offset_shift to
#      its vbuff_info_offset (and _offset_2 / _offset_3 if not 0xFFFFFFFF),
#      add ibuff_offset_shift to its ibuff_info_offset, append.
#
#   7. For each of weapon's mats: append to base.mats. mat_extras grows in lock-step.
#      Track mats_shift = len(base.mats) before append.
#
#   8. For each of weapon's mat_order: append shifted by mats_shift.
#
#   9. For each of weapon's mesh_order entries: mask off flags (top 2 bits),
#      shift the low-30-bit buffer index by buffer_infos_shift, OR the flags
#      back in, append. mesh_bounding_boxes grows in lock-step.
#
#   10. Bump base.lod0.skinned_end / physics_end / breakable_end by the count
#       of weapon mesh_order entries added (only LOD0 of weapon merged in v1).
#       Bump base.info.bones_num, vbuff_num, ibuff_num, mat_num to match the
#       new sizes.
#
#   11. Save as <new_name>.json. Don't overwrite base.
#
# ── Caveats / TODOs ──────────────────────────────────────────────────
#  - hk_constraint isn't merged. The weapon's physics constraints are dropped;
#    crowd character constraints (built from base) remain. Phase 5b can fix this.
#  - Skin binds (skin_binds, skin_order, vals_j) aren't merged. Weapon bones we
#    appended will lack inverse-bind entries; if weapon vertices reference them
#    via BlendIndices this could go wrong. v1 strategy: only handle weapons whose
#    bones already exist by name in base (or fall back to bone 0).
#  - Only LOD0 of weapon is merged; LOD1/2/3 of weapon ignored.
#  - Material textures aren't deduped — appended verbatim. The new mesh's
#    appended slots will use the WEAPON's textures (correct for a separate
#    draw call).
# ──────────────────────────────────────────────────────────────────────────

# ── Matrix helpers (column-major 4x4, matching lotrc_rs JSON layout) ──
# Internal representation: list of 16 floats in row-major order for math
# convenience: m[row*4+col].
# JSON layout is COLUMN form: {x:col0, y:col1, z:col2, w:col3} where each
# column is {x:elem0, y:elem1, z:elem2, w:elem3}.
# w-column's {x,y,z} is the translation; the .w of each column is 0 except
# w.w which is 1 (affine).

def _mat_from_json(m):
    """Convert {x:{x,y,z,w}, y:{…}, z:{…}, w:{…}} (col-form) to row-major 16-list."""
    cols = [m['x'], m['y'], m['z'], m['w']]
    out = [0.0] * 16
    for c in range(4):
        col = cols[c]
        out[0*4+c] = col['x']
        out[1*4+c] = col['y']
        out[2*4+c] = col['z']
        out[3*4+c] = col['w']
    return out


def _mat_to_json(m):
    """Inverse of _mat_from_json — row-major 16-list back to col-form dict."""
    out = {'x': {}, 'y': {}, 'z': {}, 'w': {}}
    for c, k in enumerate(['x', 'y', 'z', 'w']):
        out[k]['x'] = m[0*4+c]
        out[k]['y'] = m[1*4+c]
        out[k]['z'] = m[2*4+c]
        out[k]['w'] = m[3*4+c]
    return out


def _mat_identity():
    return [1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0]


def _mat_mul(a, b):
    """4x4 * 4x4 = 4x4, row-major."""
    out = [0.0] * 16
    for r in range(4):
        for c in range(4):
            s = 0.0
            for k in range(4):
                s += a[r*4+k] * b[k*4+c]
            out[r*4+c] = s
    return out


def _mat_inverse(m):
    """Inverse of a 4x4 row-major matrix via cofactor expansion.
    Handles rotation+scale+translation (general affine). Returns
    identity on singular input so callers don't explode mid-bake.

    Skinning needs this for the skin_bind table: each bone's
    inv_bind matrix is the inverse of its bind-pose world matrix.
    Pandemic's content tools computed these offline. We rebuild
    them here when grafting a new bone into skin_order.
    """
    # Cofactor expansion. Generated from sympy, hand-edited for
    # row-major layout. 16-element flat list, m[r*4+c].
    m00, m01, m02, m03 = m[0],  m[1],  m[2],  m[3]
    m10, m11, m12, m13 = m[4],  m[5],  m[6],  m[7]
    m20, m21, m22, m23 = m[8],  m[9],  m[10], m[11]
    m30, m31, m32, m33 = m[12], m[13], m[14], m[15]

    inv = [0.0] * 16
    inv[0]  =  m11*m22*m33 - m11*m23*m32 - m21*m12*m33 + m21*m13*m32 + m31*m12*m23 - m31*m13*m22
    inv[4]  = -m10*m22*m33 + m10*m23*m32 + m20*m12*m33 - m20*m13*m32 - m30*m12*m23 + m30*m13*m22
    inv[8]  =  m10*m21*m33 - m10*m23*m31 - m20*m11*m33 + m20*m13*m31 + m30*m11*m23 - m30*m13*m21
    inv[12] = -m10*m21*m32 + m10*m22*m31 + m20*m11*m32 - m20*m12*m31 - m30*m11*m22 + m30*m12*m21
    inv[1]  = -m01*m22*m33 + m01*m23*m32 + m21*m02*m33 - m21*m03*m32 - m31*m02*m23 + m31*m03*m22
    inv[5]  =  m00*m22*m33 - m00*m23*m32 - m20*m02*m33 + m20*m03*m32 + m30*m02*m23 - m30*m03*m22
    inv[9]  = -m00*m21*m33 + m00*m23*m31 + m20*m01*m33 - m20*m03*m31 - m30*m01*m23 + m30*m03*m21
    inv[13] =  m00*m21*m32 - m00*m22*m31 - m20*m01*m32 + m20*m02*m31 + m30*m01*m22 - m30*m02*m21
    inv[2]  =  m01*m12*m33 - m01*m13*m32 - m11*m02*m33 + m11*m03*m32 + m31*m02*m13 - m31*m03*m12
    inv[6]  = -m00*m12*m33 + m00*m13*m32 + m10*m02*m33 - m10*m03*m32 - m30*m02*m13 + m30*m03*m12
    inv[10] =  m00*m11*m33 - m00*m13*m31 - m10*m01*m33 + m10*m03*m31 + m30*m01*m13 - m30*m03*m11
    inv[14] = -m00*m11*m32 + m00*m12*m31 + m10*m01*m32 - m10*m02*m31 - m30*m01*m12 + m30*m02*m11
    inv[3]  = -m01*m12*m23 + m01*m13*m22 + m11*m02*m23 - m11*m03*m22 - m21*m02*m13 + m21*m03*m12
    inv[7]  =  m00*m12*m23 - m00*m13*m22 - m10*m02*m23 + m10*m03*m22 + m20*m02*m13 - m20*m03*m12
    inv[11] = -m00*m11*m23 + m00*m13*m21 + m10*m01*m23 - m10*m03*m21 - m20*m01*m13 + m20*m03*m11
    inv[15] =  m00*m11*m22 - m00*m12*m21 - m10*m01*m22 + m10*m02*m21 + m20*m01*m12 - m20*m02*m11

    det = m00*inv[0] + m01*inv[4] + m02*inv[8] + m03*inv[12]
    if abs(det) < 1e-12:
        log(f"[atlas_bake] _mat_inverse: matrix is singular (det={det}), "
            f"returning identity. Skin attachment will be approximate.")
        return _mat_identity()

    inv_det = 1.0 / det
    for i in range(16):
        inv[i] *= inv_det
    return inv


def _mat_from_ypr_xyz(xyz, ypr):
    """
    Build a transform matrix: T(xyz) * Ry(yaw) * Rx(pitch) * Rz(roll).
    YPR convention matches what most rigs in this engine use (Y-up world,
    yaw around Y, pitch around X, roll around Z). All angles in radians.
    Returns row-major 16-list.
    """
    cx, sx = math.cos(ypr[1]), math.sin(ypr[1])  # pitch (X)
    cy, sy = math.cos(ypr[0]), math.sin(ypr[0])  # yaw (Y)
    cz, sz = math.cos(ypr[2]), math.sin(ypr[2])  # roll (Z)

    # Ry * Rx * Rz, row-major (composed left-to-right, applied right-to-left)
    # Ry = [[cy,0,sy],[0,1,0],[-sy,0,cy]]
    # Rx = [[1,0,0],[0,cx,-sx],[0,sx,cx]]
    # Rz = [[cz,-sz,0],[sz,cz,0],[0,0,1]]
    # M = Ry * Rx * Rz
    m00 = cy*cz + sy*sx*sz
    m01 = -cy*sz + sy*sx*cz
    m02 = sy*cx
    m10 = cx*sz
    m11 = cx*cz
    m12 = -sx
    m20 = -sy*cz + cy*sx*sz
    m21 = sy*sz + cy*sx*cz
    m22 = cy*cx

    return [
        m00, m01, m02, xyz[0],
        m10, m11, m12, xyz[1],
        m20, m21, m22, xyz[2],
        0.0, 0.0, 0.0, 1.0,
    ]


def _mat_transform_point(m, p):
    """Apply 4x4 to a point (translation included). p is (x,y,z)."""
    x, y, z = p
    return (
        m[0]*x + m[1]*y + m[2]*z + m[3],
        m[4]*x + m[5]*y + m[6]*z + m[7],
        m[8]*x + m[9]*y + m[10]*z + m[11],
    )


def _mat_transform_dir(m, d):
    """Apply rotation part of 4x4 to a direction. d is (x,y,z)."""
    x, y, z = d
    return (
        m[0]*x + m[1]*y + m[2]*z,
        m[4]*x + m[5]*y + m[6]*z,
        m[8]*x + m[9]*y + m[10]*z,
    )


# ── Unorm4x8 packed normal/tangent helpers ─────────────────────────
# The engine packs 3 normalized direction components (x,y,z in [-1,1]) plus
# a 1-byte tail (often handedness or padding) into one u32:
#   byte 0: x (i8 mapped from [-1,1] → [0,255] as Unorm: v = byte/255 * 2 - 1)
#   byte 1: y
#   byte 2: z
#   byte 3: w/tail
# Implementations vary slightly on bit ordering; we preserve the tail byte
# and only re-encode bytes 0..2 from the rotated direction. If empirical
# testing shows the order is swapped, flip the byte indices here in one place.

def _unorm_unpack_xyz(u):
    """u32 → (x,y,z) floats in [-1,1], plus tail byte. Lowest byte = X."""
    bx = (u      ) & 0xFF
    by = (u >>  8) & 0xFF
    bz = (u >> 16) & 0xFF
    tail = (u >> 24) & 0xFF
    fx = (bx / 255.0) * 2.0 - 1.0
    fy = (by / 255.0) * 2.0 - 1.0
    fz = (bz / 255.0) * 2.0 - 1.0
    return fx, fy, fz, tail


def _unorm_pack_xyz(x, y, z, tail):
    """(x,y,z floats in [-1,1], tail byte) → u32. Inverse of _unorm_unpack_xyz."""
    def q(v):
        v = max(-1.0, min(1.0, v))
        return int(round((v + 1.0) * 0.5 * 255.0)) & 0xFF
    return q(x) | (q(y) << 8) | (q(z) << 16) | ((tail & 0xFF) << 24)


def _transform_vertex_data_entry(vd_entry, attach_mat):
    """
    Transform position/normal/tangent components of one vertex_data entry
    in-place by attach_mat (positions: full 4x4; normals/tangents:
    rotation-only). Leaves UVs and bone indices untouched. The caller
    handles bone-index remap separately.
    """
    data = vd_entry.get('data', {})

    # Position — Vector3, SoA (3 parallel arrays)
    pos = data.get('Position')
    if pos is not None:
        v3 = pos.get('val', {}).get('Vector3')
        if v3 and len(v3) >= 3 and len(v3[0]) == len(v3[1]) == len(v3[2]):
            xs, ys, zs = v3[0], v3[1], v3[2]
            for i in range(len(xs)):
                tx, ty, tz = _mat_transform_point(attach_mat, (xs[i], ys[i], zs[i]))
                xs[i], ys[i], zs[i] = tx, ty, tz

    # Normal — Unorm4x8 packed
    nrm = data.get('Normal')
    if nrm is not None:
        u32s = nrm.get('val', {}).get('Unorm4x8')
        if u32s is not None:
            for i, u in enumerate(u32s):
                fx, fy, fz, tail = _unorm_unpack_xyz(u)
                rx, ry, rz = _mat_transform_dir(attach_mat, (fx, fy, fz))
                # Renormalize (rotation should preserve length, but be safe)
                ln = math.sqrt(rx*rx + ry*ry + rz*rz)
                if ln > 1e-8:
                    rx /= ln; ry /= ln; rz /= ln
                u32s[i] = _unorm_pack_xyz(rx, ry, rz, tail)

    # Tangent — Unorm4x8 packed
    tan = data.get('Tangent')
    if tan is not None:
        u32s = tan.get('val', {}).get('Unorm4x8')
        if u32s is not None:
            for i, u in enumerate(u32s):
                fx, fy, fz, tail = _unorm_unpack_xyz(u)
                rx, ry, rz = _mat_transform_dir(attach_mat, (fx, fy, fz))
                ln = math.sqrt(rx*rx + ry*ry + rz*rz)
                if ln > 1e-8:
                    rx /= ln; ry /= ln; rz /= ln
                u32s[i] = _unorm_pack_xyz(rx, ry, rz, tail)


def _remap_bone_indices(vd_entry, bone_idx_remap):
    """Walk a vertex_data entry's BlendIndices (Unorm4x8 u32s) and remap each
    of the 4 packed bytes via bone_idx_remap[weapon_idx] = base_idx.
    Any weapon-bone-idx not in the remap (out of range) is left at 0."""
    data = vd_entry.get('data', {})
    bi = data.get('BlendIndices')
    if bi is None:
        return
    u32s = bi.get('val', {}).get('Unorm4x8')
    if u32s is None:
        return
    for i, u in enumerate(u32s):
        b0 = (u      ) & 0xFF
        b1 = (u >>  8) & 0xFF
        b2 = (u >> 16) & 0xFF
        b3 = (u >> 24) & 0xFF
        nb0 = bone_idx_remap.get(b0, 0)
        nb1 = bone_idx_remap.get(b1, 0)
        nb2 = bone_idx_remap.get(b2, 0)
        nb3 = bone_idx_remap.get(b3, 0)
        u32s[i] = (nb0 & 0xFF) | ((nb1 & 0xFF) << 8) | ((nb2 & 0xFF) << 16) | ((nb3 & 0xFF) << 24)


def _zero_bbox():
    return {
        'center': {'x': 0.0, 'y': 0.0, 'z': 0.0},
        'unk_3': 0.0,
        'half_width': {'x': 0.0, 'y': 0.0, 'z': 0.0},
        'unk_7': 0.0,
    }


def _shift_buffer_info_offsets(bi, vbuff_shift, ibuff_shift):
    """Shift the buffer-info's vertex/index INDICES (not byte offsets — these
    fields are array-indices into vertex_data[] / index_data[]).
    A value of 0xFFFFFFFF or 0 in the alt fields means 'unused'. Convention
    from inspecting real files: 0xFFFFFFFF = unused (clearly sentinel); 0 in
    alt slots happens too on some meshes (the bow's vbuff_info_offset is 0
    AND its vbuff_info_offset_2 is 1, meaning slot 0 is valid). So only the
    0xFFFFFFFF sentinel really marks "unused" — never confuse it with 0."""
    SENTINEL = 0xFFFFFFFF

    def shift(v, shift_amt):
        if v == SENTINEL:
            return v
        return v + shift_amt

    bi['vbuff_info_offset']   = shift(bi.get('vbuff_info_offset',   0), vbuff_shift)
    bi['vbuff_info_offset_2'] = shift(bi.get('vbuff_info_offset_2', SENTINEL), vbuff_shift)
    bi['vbuff_info_offset_3'] = shift(bi.get('vbuff_info_offset_3', SENTINEL), vbuff_shift)
    bi['ibuff_info_offset']   = shift(bi.get('ibuff_info_offset',   0), ibuff_shift)


# ── Vertex-format conversion: static → skinned ────────────────────────
# A weapon mesh authored as STATIC (no BlendIndices/BlendWeight streams,
# fmt1 lacks bits 0x400/0x800) cannot be drawn through the crowd
# renderer's skinning pipeline. To make a static weapon ride along with
# a hand bone during animation, we inject the two missing streams,
# pointing every vertex at the attach bone with full weight.

# fmt1 stream bits (from LevelScene.cpp:2143+ decoder, mirrored in the
# comment block above apply_merged_models):
#   0x001 = Position
#   0x002 = Normal
#   0x040 = Tangent
#   0x100 = Color0
#   0x200 = Color1
#   0x400 = BlendWeight
#   0x800 = BlendIndices
#   ((fmt1>>2)&0xF) = UV-channel count
_FMT1_BLEND_WEIGHT  = 0x400
_FMT1_BLEND_INDICES = 0x800
_FMT1_SKIN_BITS = _FMT1_BLEND_WEIGHT | _FMT1_BLEND_INDICES


def _vertex_count(vd_entry):
    """How many vertices does this entry hold? Tries every stream in
    priority order; first that has a length wins."""
    data = vd_entry.get('data', {})
    pos = data.get('Position')
    if pos is not None:
        v3 = pos.get('val', {}).get('Vector3')
        if v3 and isinstance(v3, list) and v3:
            return len(v3[0])
    # Fall through — try any Unorm4x8 stream
    for stream_name, stream in data.items():
        if not isinstance(stream, dict):
            continue
        val = stream.get('val', {})
        u = val.get('Unorm4x8')
        if isinstance(u, list):
            return len(u)
    return 0


def _inject_skin_streams(vd_entry, attach_bone_idx):
    """If vd_entry's fmt1 lacks BlendWeight/BlendIndices, inject them so
    every vertex is rigid-skinned to attach_bone_idx with weight 1.0.

    Also updates vd_entry['info']['fmt1'] to set bits 0x400|0x800 and
    vd_entry['info']['size'] to reflect the +8 bytes per vertex
    (4 bytes BlendWeight + 4 bytes BlendIndices).

    Returns True if injection happened, False if streams already present.
    Required for static weapons (fmt1=327 etc.) to render under the
    skinning shader path that the crowd renderer always uses for slots
    in [static_end..skinned_end]. Without this the engine reads garbage
    bone indices for the new vertices and either nukes the draw call
    or skins to bone 0 (root) producing visible chaos.
    """
    info = vd_entry.setdefault('info', {})
    fmt1 = int(info.get('fmt1', 0))
    data = vd_entry.setdefault('data', {})

    if (fmt1 & _FMT1_SKIN_BITS) == _FMT1_SKIN_BITS:
        # Already skinned — caller is responsible for remapping indices.
        return False

    n = _vertex_count(vd_entry)
    if n <= 0:
        # Empty / unparseable entry — nothing to do.
        return False

    # BlendIndices: pack 4 bytes per vertex = (attach_bone, 0, 0, 0).
    # Engine reads byte 0 (lowest) and weights it by BlendWeight byte 0.
    bi_val = attach_bone_idx & 0xFF
    bi_list = [bi_val for _ in range(n)]
    # BlendWeight: 255 (full weight) on slot 0, zero on the others.
    bw_list = [0xFF for _ in range(n)]

    data['BlendIndices'] = {'val': {'Unorm4x8': bi_list}}
    data['BlendWeight']  = {'val': {'Unorm4x8': bw_list}}

    # Update fmt1
    info['fmt1'] = fmt1 | _FMT1_SKIN_BITS

    # Update size: +8 bytes per vertex. The 'size' field is the total
    # vertex-buffer byte length consumed by this entry, and the engine
    # walks it as size/v_size for vertex count. We don't know v_size
    # here — buffer_info carries it — so we just bump 'size' by 8*n.
    old_size = int(info.get('size', 0))
    info['size'] = old_size + 8 * n
    return True


def _bump_buffer_info_skin_vsize(bi, vert_count):
    """Buffer-info carries v_size (vertex stride in bytes) and vbuff_size
    (total bytes). After injecting BlendIndices+BlendWeight streams the
    stride grows by 8 bytes/vertex and the total buffer size grows by
    8*vert_count. Only mutate when the buffer_info actually addresses a
    static vbuff (v_size < 40). v_size==0 in the _2/_3 slots stays 0."""
    v_size = int(bi.get('v_size', 0))
    if v_size > 0:
        bi['v_size'] = v_size + 8
    vb = int(bi.get('vbuff_size', 0))
    if vb > 0:
        bi['vbuff_size'] = vb + 8 * vert_count


# =====================================================================
# ATLAS BAKE: PIXEL-LEVEL SURGERY ON THE TEXTURE ALONGSIDE THE GEOMETRY
# =====================================================================
# The strip and embed flows up there fuck with vertex data. Cool.
# The actual ATLAS sat there untouched, which means:
#   - strip leaves the original pixels in the freed UV patch (sword
#     pixels in a sword-stripped CRD's atlas, hello mipmap bleed)
#   - embed pastes geometry onto pixels that don't match (bow verts
#     sampling sword leather, hello brown-stick bow)
# Havok's content tools did this RIGHT before EA fed them to the
# woodchipper in 2009 along with the engineers who wrote them.
# We rebuild it here, in Python, using Pillow, in the year of our
# lord 2026, because nobody else is going to. ride or die.
#
# Pillow 12.2 IO contract:
#   READ:  decodes DXT1/3/5 DDS to RGBA flawlessly. eat that.
#   WRITE: uncompressed RGBA DDS only. fourCC = 0x00000041. The game
#          runtime accepts both formats. We pay ~4x file size to
#          avoid bundling texconv.exe in v1. v2 can shell out.
# =====================================================================

def _load_dds_rgba(path):
    """Crack open a DDS file and hand back a Pillow RGBA image.
    Pillow's DDS plugin handles DXT1/3/5 silently, decoding into a
    plain RGBA byte buffer in memory. Caller treats it like any
    other Pillow image: crop, paste, resize, save.

    Returns None and logs a death rattle if the file doesn't exist
    or Pillow refuses to decode. Caller should bail gracefully."""
    try:
        from PIL import Image
    except ImportError:
        log(f"[atlas_bake] CRITICAL: Pillow not installed. "
            f"pip install Pillow or atlas bake is fucked.")
        return None
    if not os.path.isfile(path):
        log(f"[atlas_bake] _load_dds_rgba: file gone or never existed: {path}")
        return None
    try:
        img = Image.open(path)
        img.load()
        if img.mode != 'RGBA':
            img = img.convert('RGBA')
        return img
    except Exception as e:
        log(f"[atlas_bake] _load_dds_rgba: Pillow choked on {path}: {e}")
        return None


def _save_dds_rgba_uncompressed(img, path):
    """Slam a Pillow RGBA image to disk as an uncompressed DDS.
    LAST RESORT FALLBACK ONLY. lotrc_rs's repack reads the sidecar
    JSON to determine target format (DXT1, etc) and tries to re-
    encode our uncompressed RGBA into DXT, mangling the bytes into
    scanline garbage in the process. Don't use this unless texconv
    is missing.

    Returns True on success, False on disk failure."""
    try:
        from PIL import Image as _Image  # noqa: F401
    except ImportError:
        log(f"[atlas_bake] CRITICAL: Pillow vanished mid-execution somehow.")
        return False
    if img.mode != 'RGBA':
        img = img.convert('RGBA')
    try:
        parent = os.path.dirname(path)
        if parent and not os.path.isdir(parent):
            os.makedirs(parent, exist_ok=True)
        img.save(path)
        return True
    except Exception as e:
        log(f"[atlas_bake] _save_dds_rgba_uncompressed: failed writing {path}: {e}. "
            f"Disk full, file locked, or some other goblin holding the path open.")
        return False


# Cached path lookup for texconv. None = not yet probed, False = absent,
# str = found at this path. Probing is done lazily on first save.
_TEXCONV_PATH = None


def _find_texconv():
    """Hunt for Microsoft's texconv.exe. First try PATH, then the
    known DirectX SDK March 2008 install location, then a few other
    spots people drop it. Cached after first call. Returns the path
    string or None if it doesn't exist on this system."""
    global _TEXCONV_PATH
    if _TEXCONV_PATH is not None:
        return _TEXCONV_PATH if _TEXCONV_PATH else None

    import shutil
    candidate = shutil.which('texconv') or shutil.which('texconv.exe')
    if candidate and os.path.isfile(candidate):
        _TEXCONV_PATH = candidate
        log(f"[atlas_bake] texconv.exe located at: {candidate}")
        return candidate

    # Known fallback locations.
    for guess in (
        r'C:\Program Files (x86)\Microsoft DirectX SDK (March 2008)\Utilities\Bin\x86\texconv.exe',
        r'C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Utilities\bin\x86\texconv.exe',
        r'C:\Program Files\Microsoft DirectX SDK (March 2008)\Utilities\Bin\x86\texconv.exe',
    ):
        if os.path.isfile(guess):
            _TEXCONV_PATH = guess
            log(f"[atlas_bake] texconv.exe located at fallback path: {guess}")
            return guess

    _TEXCONV_PATH = False
    log(f"[atlas_bake] WARN: texconv.exe not found. Atlas writes will fall back "
        f"to uncompressed RGBA, which lotrc_rs mangles into DXT scanline garbage. "
        f"Install DirectX SDK or put texconv.exe on PATH.")
    return None


def _save_dds_via_texconv(img, output_path, target_format='DXT1', mip_count=10):
    """The PROPER DDS writer. Saves Pillow RGBA to a temp PNG, then
    shells out to Microsoft's texconv.exe to convert to the target
    DXT format with proper mipmap chain. The resulting DDS matches
    the byte layout the game engine expects, so lotrc_rs doesn't
    try to "fix" the file during repack (which is what corrupted
    the previous uncompressed RGBA writes).

    target_format: 'DXT1' (no/1-bit alpha), 'DXT3' (4-bit alpha),
    'DXT5' (8-bit alpha). The bow + elf atlases verified as DXT1.

    mip_count: number of mip levels. The shipping atlases use 10
    (full chain for 512x512: 512, 256, ..., 1). Use 0 for "let
    texconv decide" (defaults to full chain).

    Returns True on success, False on any failure (texconv missing,
    Pillow PNG write failure, subprocess error, output file absent).
    Falls back to uncompressed RGBA write on texconv-missing so the
    bake at least produces SOMETHING, even if mangled by lotrc_rs.
    """
    texconv = _find_texconv()
    if texconv is None:
        log(f"[atlas_bake] _save_dds_via_texconv: no texconv, falling back "
            f"to uncompressed RGBA (lotrc_rs WILL mangle this).")
        return _save_dds_rgba_uncompressed(img, output_path)

    try:
        from PIL import Image as _Image  # noqa: F401
    except ImportError:
        log(f"[atlas_bake] CRITICAL: Pillow vanished mid-execution.")
        return False

    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    # Make sure the destination directory exists.
    out_dir = os.path.dirname(output_path)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    # Write to a temp PNG. Use a name derived from the output so
    # multiple concurrent bakes don't clobber each other.
    out_name_noext = os.path.splitext(os.path.basename(output_path))[0]
    import tempfile
    tmp_dir = tempfile.mkdtemp(prefix='atlas_bake_')
    tmp_png = os.path.join(tmp_dir, out_name_noext + '.png')
    try:
        img.save(tmp_png)
    except Exception as e:
        log(f"[atlas_bake] _save_dds_via_texconv: PNG write failed: {e}")
        try: shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception: pass
        return False

    # Shell out to texconv: PNG -> DDS with format + mipmaps.
    # -mf BOX is the best general-purpose mip filter for color content.
    cmd = [
        texconv, '-nologo',
        '-f', target_format,
        '-m', str(mip_count) if mip_count > 0 else '0',
        '-mf', 'BOX',
        '-o', tmp_dir,
        tmp_png,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except Exception as e:
        log(f"[atlas_bake] _save_dds_via_texconv: subprocess failed: {e}")
        try: shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception: pass
        return False

    if r.returncode != 0:
        log(f"[atlas_bake] _save_dds_via_texconv: texconv rc={r.returncode}, "
            f"stderr: {r.stderr[:300]!r}")
        try: shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception: pass
        return False

    # texconv writes <out_dir>/<input_basename>.dds
    produced = os.path.join(tmp_dir, out_name_noext + '.dds')
    if not os.path.isfile(produced):
        log(f"[atlas_bake] _save_dds_via_texconv: texconv claimed success but "
            f"no output at {produced}")
        try: shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception: pass
        return False

    # Move into place.
    try:
        if os.path.exists(output_path):
            os.remove(output_path)
        shutil.move(produced, output_path)
    except Exception as e:
        log(f"[atlas_bake] _save_dds_via_texconv: failed moving "
            f"{produced} -> {output_path}: {e}")
        try: shutil.rmtree(tmp_dir, ignore_errors=True)
        except Exception: pass
        return False

    # Clean up temp dir (best effort).
    try: shutil.rmtree(tmp_dir, ignore_errors=True)
    except Exception: pass

    log(f"[atlas_bake] _save_dds_via_texconv: wrote {target_format} "
        f"with {mip_count} mips to {output_path} "
        f"({os.path.getsize(output_path)} bytes)")
    return True


def _detect_source_dds_format(dds_path):
    """Parse the source DDS header to find its fourCC. Used to
    match output format to source. Returns 'DXT1', 'DXT3', 'DXT5',
    or 'DXT1' fallback if unrecognizable."""
    try:
        with open(dds_path, 'rb') as f:
            data = f.read(128)
        if data[:4] != b'DDS ' or len(data) < 88:
            return 'DXT1'
        fourcc = data[84:88]
        if fourcc == b'DXT1': return 'DXT1'
        if fourcc == b'DXT3': return 'DXT3'
        if fourcc == b'DXT5': return 'DXT5'
        # Uncompressed or unknown: fall back to DXT1 for color, fine
        # for the standard PBR slots we touch.
        return 'DXT1'
    except Exception:
        return 'DXT1'


def _detect_source_dds_miplevels(dds_path):
    """Pull the mipmap count out of the source DDS header. Used to
    match output mips to source. Returns 10 fallback if unparseable."""
    try:
        with open(dds_path, 'rb') as f:
            data = f.read(32)
        if data[:4] != b'DDS ' or len(data) < 32:
            return 10
        return struct.unpack('<I', data[28:32])[0] or 10
    except Exception:
        return 10


def _resolve_diffuse_texture_name(model_json):
    """Read the diffuse texture name out of mats[0].Mat1.base.tex0.
    That's the atlas name the model samples for color. Returns the
    bare name like 'CRD_CH_elf_ancn_swd_all' (the _D suffix gets
    appended by the engine when looking up the DDS file).

    Walks defensively. Returns '' if the model has no mats, no
    Mat1, no base, or no tex0. Caller decides whether to bail or
    fall back."""
    if not isinstance(model_json, dict):
        return ''
    mats = model_json.get('mats', [])
    if not isinstance(mats, list) or not mats:
        return ''
    m0 = mats[0]
    if not isinstance(m0, dict):
        return ''
    # Materials can be wrapped in Mat1 / Mat2 / Mat3 / Mat4. We
    # only handle Mat1 (basic diffuse) here per the plan's scope.
    for mat_key in ('Mat1', 'Mat2', 'Mat3', 'Mat4'):
        wrapper = m0.get(mat_key)
        if isinstance(wrapper, dict):
            base = wrapper.get('base')
            if isinstance(base, dict):
                tex0 = base.get('tex0')
                if isinstance(tex0, str) and tex0:
                    return tex0
    return ''


# Atlas-op queue for the coalesce pass in Phase D. Strip and embed bakes
# both append entries here. _bake_pending_atlas_ops drains it after both
# apply_* phases finish. Kept module-level so the cross-phase plumbing
# stays simple. Initialized empty per run via main()'s explicit reset.
_pending_atlas_ops = []


def _uv_rect_to_pixels(uv_rect, atlas_w, atlas_h):
    """Convert a normalized UV rect (u0,v0,u1,v1 in 0..1) to pixel
    coordinates (x0,y0,x1,y1) for a given atlas size. Clamps to
    image bounds and ensures min/max ordering so callers don't have
    to validate before crop/paste. Empty rects (zero area) get
    flagged: returns None instead of a degenerate box."""
    u0, v0, u1, v1 = uv_rect
    if u1 < u0: u0, u1 = u1, u0
    if v1 < v0: v0, v1 = v1, v0
    x0 = max(0, min(atlas_w, int(round(u0 * atlas_w))))
    y0 = max(0, min(atlas_h, int(round(v0 * atlas_h))))
    x1 = max(0, min(atlas_w, int(round(u1 * atlas_w))))
    y1 = max(0, min(atlas_h, int(round(v1 * atlas_h))))
    if x1 <= x0 or y1 <= y0:
        return None
    return (x0, y0, x1, y1)


def _inpaint_rect_average(img, rect, ring_width=4):
    """The Havok-girl inpaint trick, cheap version. Sample a
    ring_width-pixel band JUST OUTSIDE the strip rect, take the
    mean RGBA, paint the rect with that color. Result: mipmaps
    averaging this region produce a smooth blend with neighbors
    instead of fringing a black hole into the elf's leather.

    `img` is a Pillow RGBA Image, mutated in place.
    `rect` is (x0,y0,x1,y1) in pixel coords.

    No-op if the rect is degenerate or the ring sample is empty
    (rect glued to the atlas corner with no outside pixels to
    sample). Caller should log the fallback case."""
    try:
        from PIL import Image, ImageDraw, ImageStat
    except ImportError:
        log(f"[atlas_bake] _inpaint_rect_average: Pillow gone, can't inpaint.")
        return False
    x0, y0, x1, y1 = rect
    if x1 <= x0 or y1 <= y0:
        return False

    w, h = img.size
    # Outer ring bounding box, clamped to image.
    ox0 = max(0, x0 - ring_width)
    oy0 = max(0, y0 - ring_width)
    ox1 = min(w, x1 + ring_width)
    oy1 = min(h, y1 + ring_width)
    if ox1 <= ox0 or oy1 <= oy0:
        return False

    # Carve out the ring: outer rect minus inner rect. Walk the four
    # strips around the inner box and sample each. Average them all
    # together for the mean.
    samples = []
    # Top strip
    if oy0 < y0:
        samples.append(img.crop((ox0, oy0, ox1, y0)))
    # Bottom strip
    if y1 < oy1:
        samples.append(img.crop((ox0, y1, ox1, oy1)))
    # Left strip (interior height only)
    if ox0 < x0:
        samples.append(img.crop((ox0, y0, x0, y1)))
    # Right strip
    if x1 < ox1:
        samples.append(img.crop((x1, y0, ox1, y1)))

    if not samples:
        log(f"[atlas_bake] _inpaint_rect_average: ring is empty "
            f"(strip rect butts up against atlas edge). Using mid-grey fallback.")
        fill_color = (128, 128, 128, 255)
    else:
        # Composite the strips into one image stack and average via ImageStat.
        # Easier: walk all pixels manually, weighted by sample area.
        total_pixels = 0
        sum_r = sum_g = sum_b = sum_a = 0
        for s in samples:
            stat = ImageStat.Stat(s)
            sw, sh = s.size
            n = sw * sh
            r, g, b = stat.mean[0], stat.mean[1], stat.mean[2]
            a = stat.mean[3] if len(stat.mean) > 3 else 255
            sum_r += r * n
            sum_g += g * n
            sum_b += b * n
            sum_a += a * n
            total_pixels += n
        if total_pixels == 0:
            fill_color = (128, 128, 128, 255)
        else:
            fill_color = (
                int(round(sum_r / total_pixels)),
                int(round(sum_g / total_pixels)),
                int(round(sum_b / total_pixels)),
                int(round(sum_a / total_pixels)),
            )

    # Paint the rect.
    draw = ImageDraw.Draw(img)
    # PIL rectangle is inclusive of bottom/right pixel by default; use
    # x1-1, y1-1 to stay within the rect.
    draw.rectangle((x0, y0, x1 - 1, y1 - 1), fill=fill_color)
    log(f"[atlas_bake] _inpaint_rect_average: ring avg={fill_color}, "
        f"painted rect=({x0},{y0})-({x1},{y1})")
    return True


def _register_new_model_in_objas(dump_dir, new_model_name, source_model_name):
    """Slam a new MODEL into objas.json. THIS WAS MISSING and it cost
    hours: without an objas entry for a new model, lotrc_rs falls back
    to a default ~128KB BIN-asset allocation. Merged CRDs (~267KB) get
    truncated past the vertex buffer, the index buffer ends up out of
    bounds, the loader bails on `ibuff OVERFLOW`, Vespucci shows nothing.

    Clone the source model's objas entry (correct `kind`, `unk_4`),
    swap the name, set `size` to a generous value (1 MB) so lotrc_rs
    pre-allocates enough room. `lotrc_rs -c` rewrites `size`/`size_comp`
    to the real byte counts during repack; we just need the row to
    exist with enough headroom.

    Idempotent: skip if an entry with the new name already exists.
    Logs all decisions so the user can see this firing in save logs.
    """
    if not new_model_name or new_model_name == source_model_name:
        log(f"[objas_register] nothing to register "
            f"(new={new_model_name!r}, source={source_model_name!r})")
        return False

    obj_path = os.path.join(dump_dir, 'objas.json')
    if not os.path.isfile(obj_path):
        log(f"[objas_register] {obj_path} not found, skipping")
        return False

    try:
        with open(obj_path, 'r', encoding='utf-8') as f:
            oa = json.load(f)
    except Exception as e:
        log(f"[objas_register] failed to load objas.json: {e}")
        return False
    if not isinstance(oa, list):
        log(f"[objas_register] objas.json root is {type(oa).__name__}, expected list")
        return False

    if any(isinstance(e, dict) and e.get('key') == new_model_name for e in oa):
        log(f"[objas_register] '{new_model_name}' already in objas, skip")
        return True

    src_entry = next((e for e in oa if isinstance(e, dict)
                      and e.get('key') == source_model_name), None)
    if src_entry is None:
        # Source might not be in objas (the user's stripped model isn't).
        # Hunt for a structurally similar CRD entry to clone instead.
        # The kind value distinguishes asset types. Any CRD_CH_*_01 model
        # works as a template since they share the same kind enum.
        candidates = ('CRD_CH_elf_ancn_swd_all_01', 'CRD_CH_urk_spr_all_01',
                      'CRD_CH_urk_spr_dead_01', 'CRD_CH_orc_swd_all_01')
        for cand in candidates:
            src_entry = next((e for e in oa if isinstance(e, dict)
                              and e.get('key') == cand), None)
            if src_entry:
                log(f"[objas_register] source '{source_model_name}' not in "
                    f"objas, using '{cand}' as template instead")
                break

    if src_entry is None:
        log(f"[objas_register] FAIL: no template CRD found in objas. "
            f"Manual fix required: add an entry for '{new_model_name}'.")
        return False

    import copy
    new_entry = copy.deepcopy(src_entry)
    new_entry['key'] = new_model_name
    # Use a generous size so lotrc_rs allocates enough BIN space for
    # merged content. Real size gets rewritten on -c repack.
    new_entry['size'] = 1048576       # 1 MB
    new_entry['size_comp'] = 0        # uncompressed; lotrc_rs recompresses
    oa.append(new_entry)
    try:
        with open(obj_path, 'w', encoding='utf-8') as f:
            json.dump(oa, f, indent=1)
        log(f"[objas_register] +objas entry for '{new_model_name}' "
            f"(cloned from '{src_entry.get('key')}', "
            f"size=1MB allocated, kind={new_entry.get('kind')})")
        return True
    except Exception as e:
        log(f"[objas_register] failed writing objas.json: {e}")
        return False


def _register_new_texture(dump_dir, new_tex_name, source_tex_name):
    """Slam a brand-new texture name into every string table that
    needs to know about it: pak_strings.json, bin_strings.json,
    string_keys.json, and objas.json. Clones the source texture's
    objas entry verbatim except for the `key` field (and `size` /
    `size_comp` which lotrc_rs will recompute at -c time anyway,
    we just need an entry to EXIST so the texture is packaged).

    Also writes the texture sidecar JSON next to the new DDS, cloned
    from the source's sidecar.

    Idempotent. If the name is already registered we skip writes
    and log it. Returns True on full registration, False if any
    table refused to update (textures/ sidecar missing, objas
    malformed, etc).
    """
    if not new_tex_name or new_tex_name == source_tex_name:
        log(f"[atlas_bake] _register_new_texture: nothing to register "
            f"(new={new_tex_name!r}, source={source_tex_name!r})")
        return False

    # ---- 1. pak_strings.json / bin_strings.json / string_keys.json ----
    for sfn in ('pak_strings.json', 'bin_strings.json', 'string_keys.json'):
        sp = os.path.join(dump_dir, sfn)
        if not os.path.isfile(sp):
            log(f"[atlas_bake] _register_new_texture: {sfn} missing, skipping")
            continue
        try:
            with open(sp, 'r', encoding='utf-8') as f:
                arr = json.load(f)
            if not isinstance(arr, list):
                log(f"[atlas_bake] _register_new_texture: {sfn} root not list, skipping")
                continue
            if new_tex_name not in arr:
                arr.append(new_tex_name)
                with open(sp, 'w', encoding='utf-8') as f:
                    json.dump(arr, f, indent=1)
                log(f"[atlas_bake] _register_new_texture: +'{new_tex_name}' in {sfn}")
        except Exception as e:
            log(f"[atlas_bake] _register_new_texture: failed updating {sfn}: {e}")

    # ---- 2. objas.json: SKIP. ----
    # Verified during exploration: textures are NOT registered in objas.
    # lotrc_rs picks DDS files up from the textures/ directory by file
    # presence, using the texture sidecar JSON (written below) for the
    # per-texture metadata. Cloning the model entry into objas would
    # add a spurious row with the wrong `kind` value and could mislead
    # the repacker into treating the texture as a model. Don't.

    # ---- 3. textures/<new_tex>_D.json sidecar ----
    # Clone the source's sidecar JSON if available.
    src_sidecar = os.path.join(dump_dir, 'textures', source_tex_name + '.json')
    new_sidecar = os.path.join(dump_dir, 'textures', new_tex_name + '.json')
    if os.path.isfile(src_sidecar) and not os.path.isfile(new_sidecar):
        try:
            with open(src_sidecar, 'r', encoding='utf-8') as f:
                sc = json.load(f)
            if isinstance(sc, dict):
                sc['key'] = new_tex_name
                sc['asset_key'] = new_tex_name
                with open(new_sidecar, 'w', encoding='utf-8') as f:
                    json.dump(sc, f, indent=1)
                log(f"[atlas_bake] _register_new_texture: wrote sidecar {new_sidecar}")
        except Exception as e:
            log(f"[atlas_bake] _register_new_texture: failed writing sidecar: {e}")
    elif not os.path.isfile(src_sidecar):
        log(f"[atlas_bake] _register_new_texture: no source sidecar at "
            f"{src_sidecar}, skipping sidecar write. Texture may load with "
            f"engine defaults.")
    return True


def _bake_atlas_for_strip(dump_dir, source_model_name, new_model_name,
                           model_json, uv_rect):
    """The full strip-side atlas surgery. Pulls the source model's
    diffuse atlas, inpaints the strip patch with neighborhood-average
    color, writes the new DDS as <new_model>_D.dds, mutates
    model_json's mats[0] tex0 to point at the new name, and registers
    everything in the string tables.

    Returns True if the bake succeeded end-to-end, False if anything
    bailed (no source atlas, no UV rect, disk failure). On False the
    stripped model keeps its parent's atlas reference — geometry strip
    still works, just no atlas reconstruction.

    uv_rect is normalized (u0,v0,u1,v1) in 0..1. Atlas pixel coords
    are derived inside.
    """
    if uv_rect is None:
        log(f"[atlas_bake] strip '{new_model_name}': no UV rect "
            f"(bone-based strip), inheriting parent atlas. No bake.")
        return False

    src_tex_name = _resolve_diffuse_texture_name(model_json)
    if not src_tex_name:
        log(f"[atlas_bake] strip '{new_model_name}': source model has "
            f"no Mat1.base.tex0, can't resolve atlas. No bake.")
        return False

    new_tex_name = new_model_name  # bare name. _D suffix is appended for the DDS file.
    src_dds_path = os.path.join(dump_dir, 'textures', src_tex_name + '.dds')
    new_dds_path = os.path.join(dump_dir, 'textures', new_tex_name + '.dds')

    img = _load_dds_rgba(src_dds_path)
    if img is None:
        log(f"[atlas_bake] strip '{new_model_name}': source atlas {src_dds_path} "
            f"failed to load. No bake.")
        return False

    w, h = img.size
    rect_px = _uv_rect_to_pixels(uv_rect, w, h)
    if rect_px is None:
        log(f"[atlas_bake] strip '{new_model_name}': UV rect is degenerate "
            f"(zero area). No bake.")
        return False

    log(f"[atlas_bake] STRIP '{source_model_name}' -> '{new_model_name}': "
        f"atlas {w}x{h}, patch px=({rect_px[0]},{rect_px[1]})-({rect_px[2]},{rect_px[3]})")

    if not _inpaint_rect_average(img, rect_px, ring_width=4):
        log(f"[atlas_bake] strip '{new_model_name}': inpaint failed, "
            f"continuing with unmodified atlas write.")

    # Match source format + mip count so lotrc_rs reads the file
    # without trying to re-encode (which corrupts uncompressed RGBA
    # into scanline garbage). texconv writes proper DXT with mips.
    src_fmt  = _detect_source_dds_format(src_dds_path)
    src_mips = _detect_source_dds_miplevels(src_dds_path)
    if not _save_dds_via_texconv(img, new_dds_path,
                                  target_format=src_fmt, mip_count=src_mips):
        log(f"[atlas_bake] strip '{new_model_name}': DDS write failed. No bake.")
        return False

    # Mutate model JSON in place so the caller's json.dump picks up
    # the new tex0 reference.
    mats = model_json.get('mats', [])
    if mats and isinstance(mats[0], dict):
        for mat_key in ('Mat1', 'Mat2', 'Mat3', 'Mat4'):
            wrapper = mats[0].get(mat_key)
            if isinstance(wrapper, dict):
                base = wrapper.get('base')
                if isinstance(base, dict) and base.get('tex0') == src_tex_name:
                    base['tex0'] = new_tex_name
                    log(f"[atlas_bake] strip '{new_model_name}': "
                        f"rewrote mats[0].{mat_key}.base.tex0 -> '{new_tex_name}'")
                    break

    _register_new_texture(dump_dir, new_tex_name, src_tex_name)
    log(f"[atlas_bake] STRIP DONE: '{new_model_name}' has its own atlas now. Eat it.")
    return True


def _weapon_uv_bbox(weapon_model_json):
    """Walk a weapon model's vertex_data[0].data.TextureCoord(0)
    Vector2 stream and compute the min/max U and V. Returns a tuple
    (u_min, v_min, u_max, v_max) in 0..1 atlas space, or None if
    the stream is missing or empty. That bbox is the rectangle in
    the weapon's atlas that actually contains weapon pixels; we
    crop that region for the embed paste."""
    vd = weapon_model_json.get('vertex_data', [])
    if not vd or not isinstance(vd, list):
        return None
    data = vd[0].get('data', {}) if isinstance(vd[0], dict) else {}
    uv_entry = data.get('TextureCoord(0)')
    if not isinstance(uv_entry, dict):
        return None
    vec2 = uv_entry.get('val', {}).get('Vector2')
    if not isinstance(vec2, list) or len(vec2) < 2:
        return None
    us, vs = vec2[0], vec2[1]
    if not us or not vs:
        return None
    return (min(us), min(vs), max(us), max(vs))


def _paste_with_feather(base_img, paste_img, dst_rect_px, feather_px=2):
    """Paste paste_img onto base_img at dst_rect_px=(x0,y0,x1,y1)
    with the outer feather_px pixels alpha-blended against the
    base, so the rectangle boundary doesn't show as a hard seam.

    paste_img is resized to fit dst_rect_px exactly via LANCZOS.
    The feather mask is an 8-bit L image, full white inside, alpha-
    ramped from 0 to 255 over the outer feather_px ring. Pillow's
    Image.paste(img, box, mask) does the rest.

    base_img is mutated in place. Returns True on success."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        log(f"[atlas_bake] _paste_with_feather: Pillow gone, can't paste.")
        return False

    x0, y0, x1, y1 = dst_rect_px
    dst_w = x1 - x0
    dst_h = y1 - y0
    if dst_w <= 0 or dst_h <= 0:
        log(f"[atlas_bake] _paste_with_feather: dst rect is dead, no paste.")
        return False

    # Resize the patch to fit the destination rect on the base atlas.
    if paste_img.mode != 'RGBA':
        paste_img = paste_img.convert('RGBA')
    resized = paste_img.resize((dst_w, dst_h), Image.LANCZOS)

    # Build feather mask. Mode 'L' = 8-bit single channel.
    # Strategy: start full 255, then for each pixel within feather_px
    # of the edge, ramp down based on distance to nearest edge.
    f = max(0, int(feather_px))
    if f == 0 or 2 * f >= dst_w or 2 * f >= dst_h:
        # Feather wider than patch: skip feather, paste opaque.
        mask = Image.new('L', (dst_w, dst_h), 255)
    else:
        mask = Image.new('L', (dst_w, dst_h), 0)
        draw = ImageDraw.Draw(mask)
        # Inner solid area = full alpha.
        draw.rectangle((f, f, dst_w - f - 1, dst_h - f - 1), fill=255)
        # Walk the four outer rings, each level gets a higher alpha
        # closer to the inner edge. Cheap linear ramp.
        for i in range(f):
            alpha = int(round(255.0 * (i + 1) / (f + 1)))
            # Top edge
            draw.rectangle((i, i, dst_w - i - 1, i), fill=alpha)
            # Bottom edge
            draw.rectangle((i, dst_h - i - 1, dst_w - i - 1, dst_h - i - 1), fill=alpha)
            # Left edge
            draw.rectangle((i, i, i, dst_h - i - 1), fill=alpha)
            # Right edge
            draw.rectangle((dst_w - i - 1, i, dst_w - i - 1, dst_h - i - 1), fill=alpha)

    base_img.paste(resized, (x0, y0), mask)
    log(f"[atlas_bake] _paste_with_feather: pasted {dst_w}x{dst_h} at "
        f"({x0},{y0}) with {f}px feather")
    return True


def _bake_atlas_for_embed(dump_dir, base_model_name, new_model_name,
                           merged_model_json, weapon_mesh_name, embed_uv_rect):
    """Full embed-side atlas surgery. Load base + weapon atlases,
    crop the weapon atlas to its actual UV footprint, resize that
    crop to the embed_uv_rect dimensions on the base atlas, paste
    with a 2-pixel alpha feather, write the result as
    <new_model>_D.dds, rewrite merged_model_json's tex0, register
    the new texture.

    Returns True on full success, False if anything bailed. On
    False the merged model keeps the base's atlas (bow geometry
    samples whatever pixels happen to live at embed_uv_rect on
    the base atlas, almost certainly the wrong color).

    The merged model JSON is mutated in place (mats[0].tex0).
    Caller must have NOT YET written it to disk so the rewrite
    actually lands."""
    if embed_uv_rect is None:
        log(f"[atlas_bake] embed '{new_model_name}': no embed_uv_rect, no bake.")
        return False

    # Resolve weapon model JSON to find its texture + UV bbox.
    weapon_json_path = os.path.join(dump_dir, 'models', weapon_mesh_name + '.json')
    if not os.path.isfile(weapon_json_path):
        log(f"[atlas_bake] embed '{new_model_name}': weapon JSON not found at "
            f"{weapon_json_path}. Can't crop weapon atlas. No bake.")
        return False
    try:
        with open(weapon_json_path, 'r', encoding='utf-8') as f:
            wp_json = json.load(f)
    except Exception as e:
        log(f"[atlas_bake] embed '{new_model_name}': weapon JSON refused "
            f"to parse: {e}. No bake.")
        return False

    wp_tex_name = _resolve_diffuse_texture_name(wp_json)
    if not wp_tex_name:
        log(f"[atlas_bake] embed '{new_model_name}': weapon '{weapon_mesh_name}' "
            f"has no Mat1.base.tex0, can't resolve weapon atlas. No bake.")
        return False

    wp_uv_bbox = _weapon_uv_bbox(wp_json)
    if wp_uv_bbox is None:
        log(f"[atlas_bake] embed '{new_model_name}': weapon has no UV stream, "
            f"can't compute crop bbox. No bake.")
        return False

    base_tex_name = _resolve_diffuse_texture_name(merged_model_json)
    if not base_tex_name:
        log(f"[atlas_bake] embed '{new_model_name}': base/merged model has no "
            f"Mat1.base.tex0. No bake.")
        return False

    base_dds_path = os.path.join(dump_dir, 'textures', base_tex_name + '.dds')
    wp_dds_path   = os.path.join(dump_dir, 'textures', wp_tex_name   + '.dds')
    base_img = _load_dds_rgba(base_dds_path)
    wp_img   = _load_dds_rgba(wp_dds_path)
    if base_img is None or wp_img is None:
        log(f"[atlas_bake] embed '{new_model_name}': atlas load failed "
            f"(base={base_img is not None}, weapon={wp_img is not None}). No bake.")
        return False

    base_w, base_h = base_img.size
    wp_w, wp_h     = wp_img.size

    # Crop the weapon atlas to the weapon's actual UV footprint.
    wp_crop_px = _uv_rect_to_pixels(wp_uv_bbox, wp_w, wp_h)
    if wp_crop_px is None:
        log(f"[atlas_bake] embed '{new_model_name}': weapon UV bbox is "
            f"degenerate. No bake.")
        return False
    wp_patch = wp_img.crop(wp_crop_px)

    # Destination rect on the base atlas.
    dst_rect_px = _uv_rect_to_pixels(embed_uv_rect, base_w, base_h)
    if dst_rect_px is None:
        log(f"[atlas_bake] embed '{new_model_name}': embed_uv_rect degenerate. No bake.")
        return False

    log(f"[atlas_bake] EMBED weapon '{wp_tex_name}' -> '{new_model_name}': "
        f"crop ({wp_patch.size[0]}x{wp_patch.size[1]}) from wp atlas, "
        f"paste at ({dst_rect_px[0]},{dst_rect_px[1]})-"
        f"({dst_rect_px[2]},{dst_rect_px[3]}) on base atlas {base_w}x{base_h}")

    if not _paste_with_feather(base_img, wp_patch, dst_rect_px, feather_px=2):
        log(f"[atlas_bake] embed '{new_model_name}': paste failed.")
        return False

    new_tex_name = new_model_name
    new_dds_path = os.path.join(dump_dir, 'textures', new_tex_name + '.dds')
    # Match source (base) format + mip count via texconv so lotrc_rs
    # doesn't re-encode (which corrupts uncompressed RGBA).
    src_fmt  = _detect_source_dds_format(base_dds_path)
    src_mips = _detect_source_dds_miplevels(base_dds_path)
    if not _save_dds_via_texconv(base_img, new_dds_path,
                                  target_format=src_fmt, mip_count=src_mips):
        log(f"[atlas_bake] embed '{new_model_name}': DDS write failed.")
        return False

    # Rewrite the merged model's tex0 to the new atlas.
    mats = merged_model_json.get('mats', [])
    if mats and isinstance(mats[0], dict):
        for mat_key in ('Mat1', 'Mat2', 'Mat3', 'Mat4'):
            wrapper = mats[0].get(mat_key)
            if isinstance(wrapper, dict):
                base = wrapper.get('base')
                if isinstance(base, dict) and base.get('tex0') == base_tex_name:
                    base['tex0'] = new_tex_name
                    log(f"[atlas_bake] embed '{new_model_name}': "
                        f"rewrote mats[0].{mat_key}.base.tex0 -> '{new_tex_name}'")
                    break

    _register_new_texture(dump_dir, new_tex_name, base_tex_name)
    log(f"[atlas_bake] EMBED DONE: '{new_model_name}' has actual weapon "
        f"pixels at the patch. The bow is a bow now. Eat it.")
    return True


def auto_find_weapon_uv_box(model_json):
    """Spatial-outlier heuristic for "find the baked weapon" in a CRD/CH model.

    A held weapon (sword, bow, spear) shares the hand's bones but extends in
    3D space far beyond the body's natural Z extent. The function:
      1. Looks at vertex_data[0]'s Position stream (the body+weapon mesh)
      2. Computes the body's Z range using verts NOT at hand height
      3. Finds verts at hand height (Y in [0.85, 1.20]) AND Z past body's
         Z extent (forward-extending)
      4. Returns the (u_min, v_min, u_max, v_max) bbox of those verts' UVs

    Returns None if model has no UV stream or no outliers detected. Caller
    can fall back to manual UV bbox entry in that case.

    Logs extensively at every decision point — caller can pipe stdout to
    capture a one-shot trace."""
    log("[auto_find] entry")
    if not model_json.get('vertex_data') or not model_json['vertex_data']:
        log("[auto_find] FAIL: no vertex_data in model")
        return None
    vd = model_json['vertex_data'][0]
    data = vd.get('data', {})
    if 'Position' not in data or 'TextureCoord(0)' not in data:
        log(f"[auto_find] FAIL: slot 0 missing Position or TextureCoord(0). "
            f"streams present: {list(data.keys())}")
        return None
    pos = data['Position']['val']['Vector3']
    uv  = data['TextureCoord(0)']['val']['Vector2']
    if not pos or len(pos) < 3 or not pos[0]:
        log("[auto_find] FAIL: Position stream malformed or empty")
        return None
    n = len(pos[0])
    log(f"[auto_find] slot 0 vert count: {n}")
    if n < 10:
        log("[auto_find] FAIL: too few verts to analyze")
        return None

    NON_HAND_Y_MAX = 0.85
    body_zs = [pos[2][i] for i in range(n) if pos[1][i] < NON_HAND_Y_MAX]
    log(f"[auto_find] body Z samples (Y<{NON_HAND_Y_MAX}): {len(body_zs)} verts")
    if len(body_zs) < 10:
        log("[auto_find] FAIL: too few body-height verts")
        return None
    body_zs_sorted = sorted(body_zs)
    body_z_lo = body_zs_sorted[len(body_zs_sorted) * 5 // 100]
    body_z_hi = body_zs_sorted[len(body_zs_sorted) * 95 // 100]
    z_threshold_fwd = body_z_hi + 0.05
    z_threshold_bwd = body_z_lo - 0.05
    log(f"[auto_find] body Z range (5th-95th pct): [{body_z_lo:.4f}, {body_z_hi:.4f}]; "
        f"thresholds fwd>{z_threshold_fwd:.4f} bwd<{z_threshold_bwd:.4f}")

    HAND_Y_LO, HAND_Y_HI = 0.85, 1.20
    weapon_idxs = []
    for i in range(n):
        if HAND_Y_LO <= pos[1][i] <= HAND_Y_HI:
            if pos[2][i] > z_threshold_fwd or pos[2][i] < z_threshold_bwd:
                weapon_idxs.append(i)
    log(f"[auto_find] hand-height Y[{HAND_Y_LO},{HAND_Y_HI}] outliers: {len(weapon_idxs)} verts")
    if not weapon_idxs:
        log("[auto_find] FAIL: no spatial outliers — model has no baked weapon "
            "OR weapon is at unusual Y range")
        return None

    us = [uv[0][i] for i in weapon_idxs if i < len(uv[0])]
    vs = [uv[1][i] for i in weapon_idxs if i < len(uv[1])]
    if not us or not vs:
        log("[auto_find] FAIL: weapon indices map to no valid UV entries")
        return None
    PAD = 0.01
    box = (max(0.0, min(us) - PAD), max(0.0, min(vs) - PAD),
           min(1.0, max(us) + PAD), min(1.0, max(vs) + PAD))
    log(f"[auto_find] OK: weapon UV bbox U[{box[0]:.4f},{box[2]:.4f}] V[{box[1]:.4f},{box[3]:.4f}]")
    return box


# =====================================================================
# Embed-into-slot-0 path. The whole reason this branch exists: the legacy
# new-slot merger is correct, exhaustive, and unusable. It synthesizes a
# new mat, a new mesh_order entry, a new buffer_info, copies the weapon
# atlas. The crowd shader path in this engine builds its draw list off
# whatever was in slot[0] at load time and silently ignores the extras.
# So we shove the bow's geometry into slot[0] directly. Same draw call,
# same material, same atlas. UVs get remapped into a user-picked rect
# inside the existing texture, which means we sample whatever pixels
# the user pointed at. Visual quality is whatever you pay for: pick a
# brown patch and the bow comes out brown. Pick a transparent patch
# and the bow disappears. Caveat coder.
# =====================================================================

def _append_crowd_spawn(dump_dir, new_name, base_name, spawn_pos, spawn_count,
                        attach_bone, weapons):
    """
    ============================================================
    AUTO-SPAWN: DRAG THE BAKED MESH OUT OF THE FUCKING VOID
    ============================================================

    The bake step writes a perfect merged-mesh JSON into models/
    and then walks away whistling, leaving the user staring at an
    empty viewport going "where in the hell did my mesh go?" The
    answer was always "nowhere, dipshit, you have to author a
    CrowdItem by hand in 3dCrowd Editor". That answer was dogshit.

    This function pins the mesh to the wall. We crack open the
    level's `sub_blocks2/3dcrowd.json`, slam a new crowd entry
    onto the end with `key_main = new_name`, drop `spawn_count`
    instances around `spawn_pos`, hand it a fistful of RH6_Swd_*
    idles, and slam the door. On the next repack and reload the
    fucker is standing in the world, breathing, swinging, exactly
    where the user's camera was pointing at bake time.

    No EA bullshit needed. No Pandemic Studios docs we'll never
    see because EA fed the team to the woodchipper. Just the
    JSON shape lotrc_rs already understands, hammered straight
    into the dump dir.

    Notes:
      - Idempotent. If an entry with this key_main is already
        in the list, we sit our ass back down and do nothing.
      - key_right grabs the first weapon's mesh name as a hint;
        per the crowd DLL findings the runtime mostly eats it
        anyway, but a polite kit author still fills it in.
      - Instances lay along +X 1.5m apart. Same y/z. The user can
        re-pose them later from the Crowd Editor if they care.
      - Animations: any RH6_* works on any human-rank rig per
        the language-rule animations memo, so we ship RH6_Swd_*
        defaults and let anyone who hates that swap them.
      - If the level has no 3dCrowd block at all, we log and
        bail. Adding a fresh block to a clean level needs more
        plumbing than a one-shot helper.
    """
    crowd_path = os.path.join(dump_dir, 'sub_blocks2', '3dcrowd.json')
    if not os.path.isfile(crowd_path):
        log(f"  [auto_spawn] BAIL '{new_name}': no 3dcrowd.json at {crowd_path}. "
            f"This level was born without a crowd block and we ain't growing one "
            f"from a one-shot helper. JSON's still in models/, dig it up yourself.")
        return False

    try:
        with open(crowd_path, 'r', encoding='utf-8') as f:
            crowd = json.load(f)
    except Exception as e:
        log(f"  [auto_spawn] FUCK: 3dcrowd.json refuses to parse: {e}. "
            f"File's corrupted or someone's been editing it with notepad. Either way, no spawn.")
        return False

    if not isinstance(crowd, list):
        log(f"  [auto_spawn] WARN: 3dcrowd.json root is {type(crowd).__name__}, "
            f"expected a list. Schema mismatch. Bailing before we make it worse.")
        return False

    # Idempotency check. Already got an entry? Sit down, shut up, do nothing.
    for item in crowd:
        if (isinstance(item, dict)
            and isinstance(item.get('header'), dict)
            and item['header'].get('key_main') == new_name):
            log(f"  [auto_spawn] '{new_name}': already has a crowd entry "
                f"breathing in this level. Not double-spawning, you can't "
                f"clone a corpse twice.")
            return True

    # Pick key_right from the weapons list (the user's chosen weapon).
    # Most crowds either reference it or leave it blank. We embedded the
    # weapon geometry into the mesh already, so key_right is mostly
    # cosmetic at this point (the engine's runtime attach for key_right
    # is a stub, per project_crowd_dll_findings).
    key_right = ''
    if weapons and isinstance(weapons, list):
        w0 = weapons[0]
        if isinstance(w0, dict):
            key_right = w0.get('mesh', '')

    # Spawn jitter: lay instances along +X from spawn_pos, 1.5m apart.
    # Keeps them visible and individually selectable without overlap.
    spawn_count = max(1, int(spawn_count) if spawn_count else 3)
    instances = []
    sx, sy, sz = float(spawn_pos[0]), float(spawn_pos[1]), float(spawn_pos[2])
    for i in range(spawn_count):
        instances.append({
            "position": { "x": sx + i * 1.5, "y": sy, "z": sz },
            "rotation": 0.0,
            "lod": 10000.0
        })

    # Use RH6_Swd_* animations as a safe default. Any RH6_* works on
    # any human-rank rig; sword set is the most common base.
    animations = [
        "RH6_Swd_Idle",
        "RH6_Swd_Idle_Engaged",
        "RH6_swd_loc_idle",
        "RH6_Swd_Idle",
        "RH6_Swd_Idle_Engaged",
        "RH6_Swd_Idle",
        "RH6_Swd_Idle",
        "RH6_Swd_Idle"
    ]

    new_item = {
        "header": {
            "key":           new_name + "_mesh",
            "key_main":      new_name,
            "key_right":     key_right,
            "key_left":      "",
            "unk_4":         0.2,
            "animation_num": len(animations),
            "instance_num":  len(instances)
        },
        "animations": animations,
        "instances": instances
    }
    crowd.append(new_item)

    try:
        with open(crowd_path, 'w', encoding='utf-8') as f:
            json.dump(crowd, f, indent=1)
        log(f"  [auto_spawn] '{new_name}': PLANTED. {len(instances)} instance(s) "
            f"standing 1.5m apart from ({sx:.2f},{sy:.2f},{sz:.2f}), "
            f"key_right='{key_right}'. Reload the level and watch them "
            f"breathe. Eat it.")
        return True
    except Exception as e:
        log(f"  [auto_spawn] FUCK: writing 3dcrowd.json blew up: {e}. "
            f"Crowd entry never made it to disk. File system, disk space, "
            f"or some asshole holding the file open in another process.")
        return False


def _cascade_bone_world(bone_parents, bone_transforms, bone_idx):
    """Bone transforms in this engine are PARENT-LOCAL (kid frame, not
    model frame). Walk the chain root-ward, accumulate the product, and
    spit out the absolute model-space bind-pose matrix for bone_idx.
    Used by the embed path to compute where the LHand actually lives in
    the elf's rest pose so we can yank bow vertices to that spot."""
    if bone_idx < 0 or bone_idx >= len(bone_transforms):
        return _mat_identity()
    chain = []
    cur = bone_idx
    seen = set()
    while 0 <= cur < len(bone_transforms) and cur not in seen:
        seen.add(cur)
        chain.append(_mat_from_json(bone_transforms[cur]))
        parent = bone_parents[cur] if cur < len(bone_parents) else -1
        if parent == cur:
            break
        cur = parent
    # chain: [bone_local, parent_local, grandparent_local, ..., root_local].
    # world = root_local * ... * grandparent_local * parent_local * bone_local
    chain.reverse()
    world = _mat_identity()
    for m in chain:
        world = _mat_mul(world, m)
    return world


def _index_buffer_values(idx_entry):
    """Pull the vals list out of an index_data entry regardless of width.
    Returns (vals_list, width_key) where width_key is 'U16' or 'U32'.
    width_key is None if the entry shape is something we don't recognise."""
    if not isinstance(idx_entry, dict):
        return [], None
    for key in ('U32', 'U16'):
        if key in idx_entry and isinstance(idx_entry[key], dict):
            return idx_entry[key].get('vals', []), key
    return [], None


def _embed_weapon_into_slot0(base, base_name, weapons, embed_uv_rect, models_dir):
    """
    Hammer weapon vertices into base.vertex_data[0] / index_data[0] using
    the base mesh's existing mat slot. The runtime draws them in the same
    pass as the body, sampling the same atlas, weighted to the user's
    chosen attach bone with full rigid weight. Side door into the crowd
    pipeline that the legacy slot-creation path can't get through.

    Implementation order per weapon:
      1. Resolve attach bone name -> bone index in base.bones.
      2. Resolve bone index -> bind index in skin_order (add identity
         skin_bind if missing, with a loud warning since that's the
         broken case).
      3. Compute the attach-bone bind-pose WORLD matrix by cascading up
         bone_parents. Compose with the user's XYZ/YPR offset.
      4. Load weapon JSON, read slot[0] streams.
      5. Transform weapon Position by attach world. Rotate Normal/Tangent
         by the rotation part. Remap UVs into embed_uv_rect. Fill
         BlendIndices/BlendWeight with (bind_idx, 0xFF) for rigid attach.
         Color(0) = white.
      6. Append everything to base.slot[0] streams. Append weapon
         triangles, vertex-indexed-shifted, to base.index_data[0].

    After all weapons baked: bump buffer_infos[0].vbuff_size/i_num/tri_num
    and vertex_data[0].info.size so the engine reads the new totals.

    Throws on truly broken input (no base slot[0], no Position stream).
    Per-weapon errors get logged and skipped, not raised.
    """
    log(f"[embed] STARTING THE MEAT GRINDER on base='{base_name}', "
        f"{len(weapons)} weapon(s) about to be hammered straight into slot[0]. "
        f"UV theft zone: U[{embed_uv_rect[0]:.4f},{embed_uv_rect[2]:.4f}] "
        f"V[{embed_uv_rect[1]:.4f},{embed_uv_rect[3]:.4f}]. "
        f"No new mat, no new texture, no asset_type dance with the stolen Havok shader.")

    # Per-weapon tags recorded as we embed. Returned to the caller so it
    # can write ze_embedded_weapons.json next to objas.json — the popup
    # mesh editor reads that sidecar to know which verts in slot[0] are
    # the weapon's (for the "select all weapon verts" button + the
    # bone-local gizmo). Each entry remembers vert range, tri range,
    # the attach bone + its bind matrix (so the editor can transform
    # selected verts in bone-local space), and the user offset that was
    # baked in (so the editor can show "current offset" as a starting
    # point instead of zero).
    weapon_tags = []

    vertex_data  = base.get('vertex_data', [])
    index_data   = base.get('index_data',  [])
    buffer_infos = base.get('buffer_infos', [])
    if not vertex_data or not index_data or not buffer_infos:
        raise RuntimeError(
            "base mesh is missing slot[0] (vertex_data/index_data/buffer_infos empty)")

    base_vd = vertex_data[0]
    base_id = index_data[0]
    base_bi = buffer_infos[0]
    base_data = base_vd.setdefault('data', {})

    if 'Position' not in base_data:
        raise RuntimeError("base slot[0] has no Position stream")

    base_pos_v3 = base_data['Position']['val']['Vector3']
    if not base_pos_v3 or len(base_pos_v3) < 3:
        raise RuntimeError("base slot[0] Position stream is malformed")

    base_vert_count_before = len(base_pos_v3[0])
    log(f"[embed] base slot[0] starting vert count: {base_vert_count_before}")

    # Which streams does the base carry? Anything we add has to match.
    # Critical: hunt for EVERY Color(N) variant the base uses (some CRDs
    # use Color(0), some use Color(1), some have both). Missing a color
    # stream extension makes the shader read past the array and cull
    # most of the embed - "flying single triangle" bug discovered when
    # the elf used Color(1) but our embed only knew about Color(0).
    has_normal = 'Normal'        in base_data
    has_tangent= 'Tangent'       in base_data
    color_streams = [k for k in base_data.keys() if k.startswith('Color(')]
    has_uv0    = 'TextureCoord(0)' in base_data
    has_bi     = 'BlendIndices'  in base_data
    has_bw     = 'BlendWeight'   in base_data
    log(f"[embed] base slot[0] streams: pos=Y normal={has_normal} "
        f"tangent={has_tangent} colors={color_streams} uv0={has_uv0} "
        f"blendidx={has_bi} blendwt={has_bw}")

    bones           = base.get('bones', [])
    bone_parents    = base.get('bone_parents', [])
    bone_transforms = base.get('bone_transforms', [])
    skin_order      = base.get('skin_order', [])
    skin_binds      = base.get('skin_binds', [])

    # Normalise the embed UV rect. User might have dragged backwards or
    # left it zeroed. Empty rect collapses to a single texel near (0,0).
    u0, v0, u1, v1 = (embed_uv_rect[0], embed_uv_rect[1],
                      embed_uv_rect[2], embed_uv_rect[3])
    if u1 < u0: u0, u1 = u1, u0
    if v1 < v0: v0, v1 = v1, v0
    if abs(u1 - u0) < 1e-6 or abs(v1 - v0) < 1e-6:
        log(f"[embed] WARN: embed_uv_rect is a goddamn ZERO-AREA rect "
            f"({u0:.4f},{v0:.4f}-{u1:.4f},{v1:.4f}). Either the user forgot "
            f"to drag a box or hit BAKE blind. Falling back to a tiny patch "
            f"at (0,0)-(0.05,0.05) so the bake doesn't divide by zero and "
            f"set itself on fire. New weapon's gonna look like ass. Your fault.")
        u0, v0, u1, v1 = 0.0, 0.0, 0.05, 0.05

    for w_idx, weapon in enumerate(weapons):
        wmesh       = weapon.get('mesh', '')
        attach_bone = weapon.get('attach_bone', '')
        offset_xyz  = weapon.get('offset_xyz', [0.0, 0.0, 0.0])
        offset_ypr  = weapon.get('offset_ypr', [0.0, 0.0, 0.0])

        if not wmesh:
            log(f"[embed] weapon[{w_idx}]: SKIP, no mesh name in spec")
            continue

        wpath = os.path.join(models_dir, wmesh + '.json')
        if not os.path.isfile(wpath):
            log(f"[embed] weapon[{w_idx}] '{wmesh}': SKIP, weapon JSON not found at {wpath}")
            continue

        try:
            with open(wpath, 'r', encoding='utf-8') as f:
                wp = json.load(f)
        except Exception as e:
            log(f"[embed] weapon[{w_idx}] '{wmesh}': SKIP, failed to load JSON: {e}")
            continue

        # ----- Resolve attach bone in base.bones -----
        if attach_bone and attach_bone in bones:
            ab_idx = bones.index(attach_bone)
        else:
            log(f"[embed] weapon[{w_idx}] '{wmesh}': WARN, attach_bone "
                f"'{attach_bone}' not found in base.bones; falling back to root (0)")
            ab_idx = 0

        # ----- Resolve skin bind index for ab_idx -----
        # Engine reads BlendIndices byte n, uses it to index skin_order[n]
        # to get the bone palette index. So the byte we pack is the
        # POSITION of ab_idx within skin_order. If ab_idx isn't there,
        # we extend skin_order + skin_binds with an identity bind. That
        # makes the weapon hang at the bone's CURRENT world position
        # (no inverse-bind correction), which is wrong in motion but
        # acceptable for crowd-distance render and recoverable later
        # via the offset sliders.
        # ----- Build attach matrix: bind-pose-world * user-offset -----
        # CRITICAL: For bones already in skin_order, use the AUTHORITATIVE
        # bind matrix (inverse of Pandemic's stored skin_binds entry), not
        # our parent-cascade computation. The two can differ by 10+ degrees
        # of rotation and significant translation because the engine's
        # actual bind-pose convention has subtleties our naive cascade
        # misses (e.g. bind-pose adjustments baked into the shipped
        # inverse-bind matrices). If we pre-transform verts with the
        # cascade bind but the runtime applies Pandemic's inverse, the
        # math doesn't cancel and the weapon renders rotated/offset,
        # often clipping through the body to where most triangles get
        # backface-culled or frustum-clipped.
        # For NEW bones (not in skin_order), fall back to cascade since
        # there's no authoritative reference to consult.
        bind_idx = None
        for bi_i, bone_pal_idx in enumerate(skin_order):
            if bone_pal_idx == ab_idx:
                bind_idx = bi_i
                break

        if bind_idx is not None:
            # Use Pandemic's stored inverse-bind, invert it to get the
            # authoritative bind matrix.
            stored_inv = _mat_from_json(skin_binds[bind_idx])
            bind_world = _mat_inverse(stored_inv)
            log(f"[embed] weapon[{w_idx}]: bone {ab_idx} in skin_order at "
                f"bind_idx={bind_idx}. Using authoritative bind from "
                f"inverse(skin_binds[{bind_idx}]). "
                f"t=({bind_world[3]:.3f},{bind_world[7]:.3f},{bind_world[11]:.3f})")
        else:
            # New bone. Compute via cascade, write inverse to skin_binds.
            # Cascade may be off but we use it for BOTH pre-transform and
            # inv_bind, so they cancel at runtime (current * inv(cascade)
            # * cascade * local = current * local).
            bind_world = _cascade_bone_world(bone_parents, bone_transforms, ab_idx)
            inv_bind_mat = _mat_inverse(bind_world)
            skin_binds.append(_mat_to_json(inv_bind_mat))
            skin_order.append(ab_idx)
            bind_idx = len(skin_order) - 1
            log(f"[embed] weapon[{w_idx}]: bone {ab_idx} NOT in skin_order. "
                f"Appended new skin_bind = inverse(cascade_bind) at "
                f"bind_idx={bind_idx}. Cascade t=({bind_world[3]:.3f},"
                f"{bind_world[7]:.3f},{bind_world[11]:.3f}). May be approximate "
                f"vs Pandemic conventions; tune offsets if needed.")
            base['skin_binds'] = skin_binds
            base['skin_order'] = skin_order

        offset_mat = _mat_from_ypr_xyz(offset_xyz, offset_ypr)
        attach_mat = _mat_mul(bind_world, offset_mat)
        log(f"[embed] weapon[{w_idx}] '{wmesh}': attach_bone='{attach_bone}' "
            f"bone_idx={ab_idx} bind_idx={bind_idx} "
            f"bind_world.t=({bind_world[3]:.3f},{bind_world[7]:.3f},{bind_world[11]:.3f}) "
            f"offset_xyz=({offset_xyz[0]:.3f},{offset_xyz[1]:.3f},{offset_xyz[2]:.3f})")

        # ----- Pull weapon attributes across ALL slots -----
        # Old code assumed everything was in slot[0]. That's wrong for
        # weapons like WP_elf_ancn_bow_01 that split attributes across
        # multiple D3D9 vertex streams: slot[0] holds UV+Tangent (453
        # entries), slot[1] holds Position+Normal (906 entries with
        # only the first 453 indexed). We have to walk every slot to
        # find each named stream, then take the smallest matching vert
        # count as the embed size.
        wp_vd = wp.get('vertex_data', [])
        wp_id = wp.get('index_data',  [])
        if not wp_vd or not wp_id:
            log(f"[embed] weapon[{w_idx}] '{wmesh}': SKIP, weapon JSON has "
                f"no vertex_data or index_data at all. Dead asset.")
            continue

        def _find_stream(name, value_type):
            """Walk every vertex_data slot looking for `name` with the
            requested value_type ('Vector3', 'Vector2', or 'Unorm4x8').
            Returns (slot_idx, value_list, n_verts) or (None, None, 0)."""
            for _si, _slot in enumerate(wp_vd):
                if not isinstance(_slot, dict): continue
                _data = _slot.get('data', {})
                if name not in _data: continue
                _val = _data[name].get('val', {})
                _v = _val.get(value_type)
                if _v is None: continue
                if value_type in ('Vector2', 'Vector3'):
                    if isinstance(_v, list) and _v and _v[0]:
                        return (_si, _v, len(_v[0]))
                else:  # Unorm4x8 or other scalar
                    if isinstance(_v, list):
                        return (_si, _v, len(_v))
            return (None, None, 0)

        pos_slot, wp_pos_v3, n_pos = _find_stream('Position', 'Vector3')
        if wp_pos_v3 is None:
            log(f"[embed] weapon[{w_idx}] '{wmesh}': SKIP, NO Position stream "
                f"anywhere in vertex_data slots. Asset has nothing to draw.")
            continue

        uv_slot,  wp_uv_v2,  n_uv  = _find_stream('TextureCoord(0)', 'Vector2')
        nrm_slot, wp_nrm_u32, n_nrm = _find_stream('Normal', 'Unorm4x8')
        tan_slot, wp_tan_u32, n_tan = _find_stream('Tangent', 'Unorm4x8')

        # Use the SMALLEST attribute count as the embed size. Many weapons
        # over-allocate one slot (e.g. bow's Position slot has 906 but only
        # 453 are indexed; the other 453 are LOD/padding garbage that we
        # don't want to copy). Capping at min() keeps us inside the
        # visible-mesh range.
        n_w = n_pos
        if n_uv > 0:  n_w = min(n_w, n_uv)
        if n_nrm > 0: n_w = min(n_w, n_nrm)

        # Also bound by the max vertex INDEX actually referenced by the
        # weapon's index buffer. If the index buffer max is below the
        # stream length, those extra verts are unreferenced garbage we
        # should skip (per the bow LOD investigation). Fall back to n_w
        # if the index buffer is missing.
        wp_idx_first, _idx_width_key = _index_buffer_values(wp_id[0]) if wp_id else ([], None)
        if wp_idx_first:
            max_idx = max(wp_idx_first)
            n_w = min(n_w, max_idx + 1)

        log(f"[embed] weapon[{w_idx}] '{wmesh}': streams found "
            f"Position@slot[{pos_slot}]={n_pos}, UV@slot[{uv_slot}]={n_uv}, "
            f"Normal@slot[{nrm_slot}]={n_nrm}, Tangent@slot[{tan_slot}]={n_tan}; "
            f"embedding {n_w} verts (capped by min stream + max indexed)")

        # ----- Transform positions -----
        new_xs = [0.0] * n_w
        new_ys = [0.0] * n_w
        new_zs = [0.0] * n_w
        for i in range(n_w):
            wx = wp_pos_v3[0][i] if i < len(wp_pos_v3[0]) else 0.0
            wy = wp_pos_v3[1][i] if i < len(wp_pos_v3[1]) else 0.0
            wz = wp_pos_v3[2][i] if i < len(wp_pos_v3[2]) else 0.0
            tx, ty, tz = _mat_transform_point(attach_mat, (wx, wy, wz))
            new_xs[i] = tx
            new_ys[i] = ty
            new_zs[i] = tz

        # ----- Rebuild Normal (rotate weapon's normals if present) -----
        new_normals = None
        if has_normal:
            new_normals = []
            wp_norm = wp_nrm_u32 if wp_nrm_u32 is not None else []
            for i in range(n_w):
                # Default: up (0,1,0) if weapon has no normal stream.
                u_in = wp_norm[i] if i < len(wp_norm) else _unorm_pack_xyz(0.0, 1.0, 0.0, 0)
                fx, fy, fz, tail = _unorm_unpack_xyz(u_in)
                rx, ry, rz = _mat_transform_dir(attach_mat, (fx, fy, fz))
                ln = math.sqrt(rx*rx + ry*ry + rz*rz)
                if ln > 1e-8:
                    rx /= ln; ry /= ln; rz /= ln
                new_normals.append(_unorm_pack_xyz(rx, ry, rz, tail))

        # ----- Rebuild Tangent (zeroed; tangent space isn't critical at crowd distance) -----
        new_tangents = None
        if has_tangent:
            # Pack (0,0,0) with tail 0 = 0x00808080. The crowd shader's
            # tangent reads collapse to identity rotation when tangent is
            # zero, which is fine for unlit-ish crowd render.
            zero_tan = _unorm_pack_xyz(0.0, 0.0, 0.0, 0)
            new_tangents = [zero_tan] * n_w

        # ----- Rebuild Color streams: opaque white -----
        # Build one new_colors list and reuse across every Color(N) the
        # base mesh declares. CRDs vary on whether they ship Color(0)
        # or Color(1) (or both); missing any of them broke the shader's
        # per-vertex color read and culled most embedded geometry.
        new_colors = [0xFFFFFFFF] * n_w if color_streams else None

        # ----- Remap UVs into embed_uv_rect -----
        new_us = []
        new_vs = []
        if has_uv0:
            if wp_uv_v2 is not None and isinstance(wp_uv_v2, list) and len(wp_uv_v2) >= 2:
                wp_us = wp_uv_v2[0] if wp_uv_v2 else []
                wp_vs = wp_uv_v2[1] if len(wp_uv_v2) >= 2 else []
                if wp_us and wp_vs:
                    umin = min(wp_us)
                    umax = max(wp_us)
                    vmin = min(wp_vs)
                    vmax = max(wp_vs)
                    u_range = max(1e-6, umax - umin)
                    v_range = max(1e-6, vmax - vmin)
                    for i in range(n_w):
                        wu = wp_us[i] if i < len(wp_us) else umin
                        wv = wp_vs[i] if i < len(wp_vs) else vmin
                        rem_u = u0 + (wu - umin) / u_range * (u1 - u0)
                        rem_v = v0 + (wv - vmin) / v_range * (v1 - v0)
                        new_us.append(rem_u)
                        new_vs.append(rem_v)
                    log(f"[embed] weapon[{w_idx}]: UV remap from "
                        f"U[{umin:.3f},{umax:.3f}] V[{vmin:.3f},{vmax:.3f}] "
                        f"to U[{u0:.3f},{u1:.3f}] V[{v0:.3f},{v1:.3f}]")
                else:
                    # Weapon has UV stream but it's empty. Fall through to centre.
                    pass
            if not new_us:
                # No source UVs or empty. Slam every vertex to the rect's centre.
                cu = (u0 + u1) * 0.5
                cv = (v0 + v1) * 0.5
                new_us = [cu] * n_w
                new_vs = [cv] * n_w
                log(f"[embed] weapon[{w_idx}]: no source UVs, all verts at "
                    f"({cu:.3f},{cv:.3f})")

        # ----- BlendIndices + BlendWeight: full rigid weight on bind_idx -----
        # This engine's BlendWeight convention puts the primary skin weight
        # in BYTE 2, not byte 0. Verified empirically: 1046 elf body rigid
        # verts all use (0, 0, 255, 0) = 0x00FF0000. NEVER byte 0.
        # BlendIndices must put the bone palette idx at the SAME byte
        # position as its paired weight, so byte 2 too. Earlier code put
        # the bind in byte 0 — the in-game renderer didn't care (crowds
        # render verts at their pre-baked positions, no skinning eval),
        # but Vespucci's bone-weight selection pairs byte K of indices
        # with byte K of weights and saw our bow verts as palette[0]
        # instead of palette[bind_idx]. User couldn't pick the bow by
        # bone name. Move both to byte 2 so the pairing matches
        # Pandemic's rigid pattern and bone-weight selection works.
        new_bi_vals = None
        new_bw_vals = None
        if has_bi:
            bi_packed = (bind_idx & 0xFF) << 16  # bone idx at BYTE 2 (matches weight at byte 2)
            new_bi_vals = [bi_packed] * n_w
        if has_bw:
            # 0x00FF0000 = bytes (0, 0, 255, 0). Match the elf rigid pattern.
            new_bw_vals = [0x00FF0000] * n_w

        # ----- Snapshot the offset to use when shifting the weapon's index buffer -----
        # We grab len BEFORE appending positions, so the offset reflects the
        # base's current vertex count (which is also the index into the new
        # verts we're about to write).
        vert_offset_for_this_weapon = len(base_pos_v3[0])

        # ----- Append all the streams in lockstep -----
        base_pos_v3[0].extend(new_xs)
        base_pos_v3[1].extend(new_ys)
        base_pos_v3[2].extend(new_zs)

        if has_normal and new_normals is not None:
            base_data['Normal']['val']['Unorm4x8'].extend(new_normals)
        if has_tangent and new_tangents is not None:
            base_data['Tangent']['val']['Unorm4x8'].extend(new_tangents)
        if color_streams and new_colors is not None:
            # Extend EVERY color stream the base carries, not just
            # Color(0). The elf uses Color(1), and skipping it caused
            # the bow's verts to read past the array - shader culled
            # all but one triangle. Touch all of them.
            for c_key in color_streams:
                stream = base_data.get(c_key, {}).get('val', {}).get('Unorm4x8')
                if isinstance(stream, list):
                    stream.extend(new_colors)
        if has_uv0 and new_us:
            base_uv_v2 = base_data['TextureCoord(0)']['val']['Vector2']
            base_uv_v2[0].extend(new_us)
            base_uv_v2[1].extend(new_vs)
        if has_bi and new_bi_vals is not None:
            base_data['BlendIndices']['val']['Unorm4x8'].extend(new_bi_vals)
        if has_bw and new_bw_vals is not None:
            base_data['BlendWeight']['val']['Unorm4x8'].extend(new_bw_vals)

        # ----- Append weapon triangles, vertex-shifted -----
        wp_indices, _ = _index_buffer_values(wp_id[0])
        base_indices, base_idx_width_key = _index_buffer_values(base_id)
        if base_idx_width_key is None:
            log(f"[embed] weapon[{w_idx}] '{wmesh}': WARN, base index_data[0] "
                f"format unknown, skipping triangle append "
                f"(weapon verts present but not drawn)")
            continue

        # Snapshot index buffer length BEFORE appending so the tag can
        # record which triangles belong to this weapon. The popup picker
        # uses tri_first / tri_count to highlight + transform just this
        # weapon's contribution.
        base_tri_count_before = len(base_indices) // 3

        appended_tris = 0
        skipped_degenerate = 0
        # Whole-triangles only; trailing leftover indices are ignored.
        for i in range(0, (len(wp_indices) // 3) * 3, 3):
            a = wp_indices[i+0]
            b = wp_indices[i+1]
            c = wp_indices[i+2]
            # Filter out any index that's out of the weapon's own vertex
            # count (corrupt or padding indices); cheap safety.
            if a >= n_w or b >= n_w or c >= n_w:
                skipped_degenerate += 1
                continue
            base_indices.append(a + vert_offset_for_this_weapon)
            base_indices.append(b + vert_offset_for_this_weapon)
            base_indices.append(c + vert_offset_for_this_weapon)
            appended_tris += 1
        log(f"[embed] weapon[{w_idx}]: appended {appended_tris} tris "
            f"(skipped {skipped_degenerate} degenerate), "
            f"vert offset = {vert_offset_for_this_weapon}")

        # Record the sidecar tag for this weapon. bind_world is the
        # authoritative bind-pose world matrix the embed used; the editor
        # transforms selected verts via bind_world * delta_local *
        # inv(bind_world) to apply a bone-local move in world space.
        weapon_tags.append({
            'weapon_name':      wmesh,
            'attach_bone':      attach_bone,
            'attach_bone_idx':  ab_idx,
            'bind_idx':         bind_idx,
            'vert_first':       vert_offset_for_this_weapon,
            'vert_count':       n_w,
            'tri_first':        base_tri_count_before,
            'tri_count':        appended_tris,
            'offset_xyz':       list(offset_xyz),
            'offset_ypr':       list(offset_ypr),
            'bind_world':       list(bind_world),
        })

    # ----- Update buffer_info[0] and vertex_data[0].info sizes -----
    final_vert_count = len(base_pos_v3[0])
    final_indices, final_width_key = _index_buffer_values(base_id)
    final_tri_count = len(final_indices) // 3

    v_size = int(base_bi.get('v_size', 0))
    if v_size > 0:
        base_bi['vbuff_size'] = final_vert_count * v_size
    base_bi['i_num']   = final_tri_count * 3
    base_bi['tri_num'] = final_tri_count

    vd_info = base_vd.setdefault('info', {})
    if 'size' in vd_info and v_size > 0:
        vd_info['size'] = final_vert_count * v_size

    # info.vbuff_num / ibuff_num / mat_num stay the same since we didn't
    # add new slots; only mesh contents grew.
    if 'info' in base and isinstance(base['info'], dict):
        # bones_num could have grown if a missing attach bone forced a
        # skin_order/skin_binds extension; sync the count.
        base['info']['skin_binds_num'] = len(base.get('skin_binds', []))

    delta = final_vert_count - base_vert_count_before
    log(f"[embed] DONE: slot[0] {base_vert_count_before} -> {final_vert_count} verts "
        f"(+{delta} grafted on), {final_tri_count} tris total. "
        f"vbuff_size={base_bi.get('vbuff_size')}, "
        f"i_num={base_bi.get('i_num')}, tri_num={base_bi.get('tri_num')}. "
        f"The new corpse is stitched. Repack, reload, watch it walk. Eat it.")
    return weapon_tags


def retro_tag_baked_weapon(dump_dir, merged_name, base_name, weapon_name,
                            attach_bone):
    """
    Retroactively register an already-baked merged CRD in ze_embedded_weapons.json.

    The Adjust Baked Weapon panel needs a sidecar entry to know which verts
    in slot[0] belong to the weapon. The M2 hook only writes that entry on
    FRESH bakes; merged CRDs baked before M2 landed have no entry and the
    panel can't see them. Re-baking from stripped works but loses any
    hand-tuned UV / atlas state.

    This helper reconstructs the sidecar entry by diffing the merged CRD
    against the stripped base it was derived from. The math:
      vert_first = base.slot[0].vertex_count
      vert_count = merged.slot[0].vertex_count - vert_first
      tri_first  = base.slot[0].triangle_count
      tri_count  = merged.slot[0].triangle_count - tri_first
      bind_idx   = merged.skin_order.index_of(attach_bone_idx)
      bind_world = inverse(merged.skin_binds[bind_idx])
    Saves into <dump_dir>/ze_embedded_weapons.json under merged_name. Idempotent.

    Inputs:
      dump_dir     — level dump root (contains models/ + sidecars)
      merged_name  — baked merged CRD name, e.g. 'CRD_CH_elf_ancn_bowtest_01'
      base_name    — stripped/source CRD the merge started from, e.g.
                     'CRD_CH_elf_ancn_stripped_01'
      weapon_name  — recorded for editor display only, e.g. 'WP_elf_ancn_bow_01'
      attach_bone  — bone name the weapon is rigid-weighted to, e.g. 'Bone_LHand'

    Raises RuntimeError on bad inputs (missing files, vert counts that don't
    indicate an embed, bone not in skin palette). Returns the populated tag dict.
    """
    models_dir = os.path.join(dump_dir, 'models')
    merged_path = os.path.join(models_dir, merged_name + '.json')
    base_path   = os.path.join(models_dir, base_name   + '.json')
    if not os.path.isfile(merged_path):
        raise RuntimeError(f"merged CRD not found at {merged_path}")
    if not os.path.isfile(base_path):
        raise RuntimeError(f"base CRD not found at {base_path}")

    with open(merged_path, 'r', encoding='utf-8') as f:
        merged = json.load(f)
    with open(base_path, 'r', encoding='utf-8') as f:
        base = json.load(f)

    def slot0_counts(m, label):
        vd = (m.get('vertex_data') or [None])[0]
        if not isinstance(vd, dict):
            raise RuntimeError(f"{label}: no vertex_data[0]")
        pos = vd.get('data', {}).get('Position', {}).get('val', {}).get('Vector3')
        if not pos or len(pos) < 3:
            raise RuntimeError(f"{label}: slot[0] has no Position stream")
        vert_count = len(pos[0])
        ind, _ = _index_buffer_values((m.get('index_data') or [None])[0])
        tri_count = len(ind) // 3 if ind else 0
        return vert_count, tri_count

    base_v, base_t   = slot0_counts(base,   f"base '{base_name}'")
    merg_v, merg_t   = slot0_counts(merged, f"merged '{merged_name}'")
    if merg_v <= base_v:
        raise RuntimeError(
            f"merged CRD has {merg_v} verts in slot[0], base has {base_v}; "
            f"no embedded weapon to tag (base may be wrong)")

    vert_first = base_v
    vert_count = merg_v - base_v
    tri_first  = base_t
    tri_count  = max(0, merg_t - base_t)

    # Resolve attach bone in the MERGED model's bone list (that's the one
    # we'll be reading at edit time).
    bones = merged.get('bones', []) or []
    if attach_bone not in bones:
        raise RuntimeError(
            f"attach bone '{attach_bone}' not in merged model's bones list")
    attach_bone_idx = bones.index(attach_bone)

    # Find bind_idx in skin_order; reject if the bone isn't in the palette
    # (the editor needs an authoritative bind matrix to do bone-local moves).
    skin_order = merged.get('skin_order', []) or []
    bind_idx = None
    for i, b in enumerate(skin_order):
        if b == attach_bone_idx:
            bind_idx = i
            break
    if bind_idx is None:
        raise RuntimeError(
            f"bone '{attach_bone}' (idx {attach_bone_idx}) is not in "
            f"merged model's skin_order; the embed used cascade fallback "
            f"and the authoritative bind isn't recoverable")

    skin_binds = merged.get('skin_binds', []) or []
    if bind_idx >= len(skin_binds):
        raise RuntimeError(
            f"bind_idx {bind_idx} out of range of skin_binds (len={len(skin_binds)})")
    inv_bind = _mat_from_json(skin_binds[bind_idx])
    bind_world = _mat_inverse(inv_bind)

    tag = {
        'weapon_name':      weapon_name,
        'attach_bone':      attach_bone,
        'attach_bone_idx':  attach_bone_idx,
        'bind_idx':         bind_idx,
        'vert_first':       vert_first,
        'vert_count':       vert_count,
        'tri_first':        tri_first,
        'tri_count':        tri_count,
        'offset_xyz':       [0.0, 0.0, 0.0],
        'offset_ypr':       [0.0, 0.0, 0.0],
        'bind_world':       list(bind_world),
    }

    # Reuse the same sidecar writer the fresh-bake path uses so the on-
    # disk schema stays in lockstep.
    _write_embedded_weapon_sidecar(dump_dir, merged_name, [tag])
    print(f"  retro-tagged '{merged_name}' weapon[0]: "
          f"verts=[{vert_first}..{vert_first+vert_count}) "
          f"tris=[{tri_first}..{tri_first+tri_count}) "
          f"bone='{attach_bone}'(idx={attach_bone_idx}, bind={bind_idx})")
    return tag


def apply_weapon_adjustment(dump_dir, model_name, weapon_idx,
                             delta_xyz, delta_ypr_deg,
                             also_rotate_normals=True):
    """
    Move an embedded weapon's verts inside an already-baked CRD JSON by a
    bone-local delta. The bow is welded into the elf's mesh skinned to
    Bone_LHand — you can't move it as an object, only re-transform its
    verts. This is the after-bake editing path that lets the user dial in
    bow position without re-running the full bake (atlas + UV remap +
    BlendWeight setup), which takes minutes; this takes milliseconds.

    Inputs:
      dump_dir       — directory containing models/ and ze_embedded_weapons.json
      model_name     — name of the baked merged CRD (e.g. 'CRD_CH_elf_ancn_bowtest_01')
      weapon_idx     — index into the model's weapons[] in the sidecar
      delta_xyz      — (dx,dy,dz) in bone-local space (meters, same units as the rig)
      delta_ypr_deg  — (yaw,pitch,roll) in DEGREES, bone-local rotation
      also_rotate_normals — if True, rotate the weapon's Normal stream too

    Math:
      current world pos p_w = bind_world * offset_baked * weapon_local
      to apply a bone-local delta D:
        p_w_new = bind_world * D * inv(bind_world) * p_w
      so the world-space transform M = bind_world * D * inv(bind_world).
      Apply M to position; apply M's rotation part to normals.

    Side-effect: rewrites <dump_dir>/models/<model_name>.json in place.
    User must run lotrc_rs -c after this to repack the PAK.

    Raises RuntimeError on bad inputs; returns the number of verts touched.
    """
    sidecar_path = os.path.join(dump_dir, 'ze_embedded_weapons.json')
    if not os.path.isfile(sidecar_path):
        raise RuntimeError(f"no sidecar at {sidecar_path}; bake first")
    with open(sidecar_path, 'r', encoding='utf-8') as f:
        sidecar = json.load(f)

    model_entry = (sidecar.get('models') or {}).get(model_name)
    if not model_entry:
        raise RuntimeError(f"no sidecar entry for model '{model_name}'")
    weapons = model_entry.get('weapons') or []
    if weapon_idx < 0 or weapon_idx >= len(weapons):
        raise RuntimeError(f"weapon_idx {weapon_idx} out of range "
                           f"(model has {len(weapons)} weapons)")

    tag = weapons[weapon_idx]
    bind_world = tag.get('bind_world')
    if not isinstance(bind_world, list) or len(bind_world) != 16:
        raise RuntimeError(f"weapon[{weapon_idx}] has no valid bind_world; "
                           f"sidecar predates the M2 tag schema, re-bake to regenerate")
    vert_first = int(tag['vert_first'])
    vert_count = int(tag['vert_count'])

    # Build bone-local delta matrix from XYZ + YPR (degrees).
    delta_ypr_rad = [math.radians(float(a)) for a in delta_ypr_deg]
    D = _mat_from_ypr_xyz([float(v) for v in delta_xyz], delta_ypr_rad)

    # M = bind_world * D * inv(bind_world)
    inv_bind = _mat_inverse(bind_world)
    M = _mat_mul(_mat_mul(bind_world, D), inv_bind)

    # Load model JSON
    model_path = os.path.join(dump_dir, 'models', model_name + '.json')
    if not os.path.isfile(model_path):
        raise RuntimeError(f"model JSON missing: {model_path}")
    with open(model_path, 'r', encoding='utf-8') as f:
        model = json.load(f)

    try:
        pos = model['vertex_data'][0]['data']['Position']['val']['Vector3']
    except (KeyError, IndexError):
        raise RuntimeError(f"{model_name}: slot[0] has no Position Vector3 stream")

    xs, ys, zs = pos[0], pos[1], pos[2]
    n_verts_stream = len(xs)
    if vert_first + vert_count > n_verts_stream:
        raise RuntimeError(f"tag vert range [{vert_first}..{vert_first+vert_count}) "
                           f"exceeds stream length {n_verts_stream} — sidecar stale")

    # Apply M to positions in the weapon's range.
    for i in range(vert_first, vert_first + vert_count):
        nx, ny, nz = _mat_transform_point(M, (xs[i], ys[i], zs[i]))
        xs[i] = nx
        ys[i] = ny
        zs[i] = nz

    # Rotate normals if requested. Normal is Unorm4x8 packed.
    if also_rotate_normals:
        try:
            n_stream = model['vertex_data'][0]['data']['Normal']['val']['Unorm4x8']
            if isinstance(n_stream, list) and len(n_stream) >= vert_first + vert_count:
                for i in range(vert_first, vert_first + vert_count):
                    fx, fy, fz, tail = _unorm_unpack_xyz(n_stream[i])
                    rx, ry, rz = _mat_transform_dir(M, (fx, fy, fz))
                    # renormalize
                    ln = math.sqrt(rx*rx + ry*ry + rz*rz)
                    if ln > 1e-8:
                        rx /= ln; ry /= ln; rz /= ln
                    n_stream[i] = _unorm_pack_xyz(rx, ry, rz, tail)
        except (KeyError, IndexError):
            # No Normal stream — silently skip; mesh just won't relight
            # quite right for the moved verts. Crowd distance hides it.
            pass

    with open(model_path, 'w', encoding='utf-8') as f:
        json.dump(model, f, indent=1)

    log(f"[adjust] {model_name} weapon[{weapon_idx}] '{tag.get('weapon_name','?')}': "
        f"moved {vert_count} verts by xyz=({delta_xyz[0]:+.3f},{delta_xyz[1]:+.3f},{delta_xyz[2]:+.3f}) "
        f"ypr=({delta_ypr_deg[0]:+.1f},{delta_ypr_deg[1]:+.1f},{delta_ypr_deg[2]:+.1f})deg "
        f"in bone-local space. Repack with lotrc_rs -c then reload.")
    return vert_count


def _write_embedded_weapon_sidecar(dump_dir, new_name, weapon_tags):
    """
    Merge per-model weapon tags into ze_embedded_weapons.json at the dump
    root. Vespucci reads this on level load to know which slot[0] verts
    in a baked CRD belong to an embedded weapon — the popup mesh editor
    uses it to enable the 'select all weapon verts' button and to apply
    bone-local transforms to just those verts.

    Sidecar shape:
      {
        "models": {
          "CRD_CH_elf_ancn_bowtest_01": {
            "weapons": [ <tag>, <tag>, ... ]
          },
          ...
        }
      }

    Idempotent: re-baking the same model replaces its entry, never
    duplicates. Other models in the file are preserved untouched.
    """
    if not weapon_tags:
        return
    sidecar_path = os.path.join(dump_dir, 'ze_embedded_weapons.json')
    data = {'models': {}}
    if os.path.isfile(sidecar_path):
        try:
            with open(sidecar_path, 'r', encoding='utf-8') as f:
                existing = json.load(f)
            if isinstance(existing, dict) and isinstance(existing.get('models'), dict):
                data = existing
        except Exception as e:
            log(f"  [tag] WARN existing {sidecar_path} unreadable ({e}); rewriting fresh")

    data['models'][new_name] = {'weapons': weapon_tags}

    try:
        with open(sidecar_path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=1)
        log(f"  [tag] wrote {len(weapon_tags)} weapon tag(s) for '{new_name}' "
            f"to {os.path.basename(sidecar_path)}")
    except Exception as e:
        log(f"  [tag] ERROR writing {sidecar_path}: {e}")


def apply_embedded_weapon_xforms(dump_dir, xforms_path):
    """
    Commit pending bone-local transforms to merged-CRD model JSONs.

    The Adjust Baked Weapon panel writes ze_embedded_weapon_xforms.json
    when the user clicks Apply. Each entry names a baked model + weapon
    index and carries the XYZ + YPR delta the user dialled in on the
    sliders. Vespucci's runtime already PREVIEWED the change by
    rewriting slot[0]'s VB live every frame; this function bakes the
    same math into the model JSON so the next lotrc_rs repack carries
    it forward.

    Pipeline:
      1. Load ze_embedded_weapons.json to find each weapon's vert range
         + bind_world matrix.
      2. Load ze_embedded_weapon_xforms.json for pending deltas.
      3. For each pending delta:
           - Open <dump_dir>/models/<model_name>.json
           - Compute world_delta = bind_world * delta_local * inv(bind_world)
           - Apply world_delta to positions [vert_first..vert_first+vert_count)
             in vertex_data[0].data.Position
           - Update the recorded user offset in ze_embedded_weapons.json so
             subsequent edits start from the *committed* baseline (else the
             user keeps adding the same delta on each save).
           - Save the model JSON.
      4. Delete ze_embedded_weapon_xforms.json (consumed; idempotent on
         next save with no new deltas).

    Schema (xforms file):
      {
        "xforms": [
          { "model": "CRD_CH_elf_ancn_bowtest_01",
            "weapon_idx": 0,
            "delta_xyz": [x, y, z],
            "delta_ypr": [yaw_deg, pit_deg, rol_deg]
          },
          ...
        ]
      }
    """
    if not os.path.isfile(xforms_path):
        return
    try:
        with open(xforms_path, 'r', encoding='utf-8') as f:
            x = json.load(f)
    except Exception as e:
        log(f"  apply_embedded_weapon_xforms: parse failed ({e})")
        return

    entries = x.get('xforms', []) if isinstance(x, dict) else []
    if not entries:
        return

    # Sidecar is OPTIONAL now. Bone-keyed entries don't need it — they
    # resolve everything (vert range, bind_world) from the model JSON
    # itself. Only legacy weapon-idx entries need the sidecar to look up
    # the recorded vert range. Load it if present, treat as empty if not.
    sidecar_path = os.path.join(dump_dir, 'ze_embedded_weapons.json')
    sidecar = {'models': {}}
    if os.path.isfile(sidecar_path):
        try:
            with open(sidecar_path, 'r', encoding='utf-8') as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                sidecar = loaded
        except Exception as e:
            log(f"  apply_embedded_weapon_xforms: sidecar parse failed, continuing without ({e})")

    models_dir = os.path.join(dump_dir, 'models')
    if not os.path.isdir(models_dir):
        log(f"  apply_embedded_weapon_xforms: missing {models_dir}")
        return

    def _bytes_at(stream, i):
        # Unorm4x8 entries may arrive as packed u32 OR as 4-byte arrays.
        # Both shapes appear across the various models lotrc_rs has dumped.
        v = stream[i] if i < len(stream) else 0
        if isinstance(v, int):
            return ((v >> 0) & 0xFF, (v >> 8) & 0xFF,
                    (v >> 16) & 0xFF, (v >> 24) & 0xFF)
        if isinstance(v, list) and len(v) >= 4:
            return v[0], v[1], v[2], v[3]
        return (0, 0, 0, 0)

    committed = 0
    for entry in entries:
        model_name = entry.get('model', '')
        # Three entry flavors supported:
        #   1. Vert-list (M6):     {"model": ..., "verts": [i, j, k, ...], "delta_xyz": [x, y, z]}
        #   2. Bone-keyed (M5):    {"model": ..., "bone": "Bone_LHand", ...}
        #   3. Weapon-idx legacy:  {"model": ..., "weapon_idx": N, ...}
        # The vert-list path is what the Move Triangles picker writes:
        # explicit slot[0] vert indices + a world-space translation, no
        # bone-local conjugation. The bone-keyed path works on ANY skinned
        # CRD. The weapon-idx path requires a sidecar tag.
        verts_list = entry.get('verts')
        bone_name  = entry.get('bone', '')
        weapon_idx = entry.get('weapon_idx', -1)
        dxyz = entry.get('delta_xyz', [0.0, 0.0, 0.0])
        dypr = entry.get('delta_ypr', [0.0, 0.0, 0.0])
        has_verts = isinstance(verts_list, list) and len(verts_list) > 0
        if not model_name or (not has_verts and not bone_name and weapon_idx < 0):
            log(f"  apply_embedded_weapon_xforms: malformed entry, skip: {entry}")
            continue

        model_path = os.path.join(models_dir, model_name + '.json')
        if not os.path.isfile(model_path):
            log(f"  apply_embedded_weapon_xforms: model JSON not found: {model_path}")
            continue
        try:
            with open(model_path, 'r', encoding='utf-8') as f:
                m = json.load(f)
        except Exception as e:
            log(f"  apply_embedded_weapon_xforms: failed to load {model_path}: {e}")
            continue

        vd0 = (m.get('vertex_data') or [None])[0]
        if not isinstance(vd0, dict):
            log(f"  apply_embedded_weapon_xforms: '{model_name}' has no slot[0]")
            continue
        data0 = vd0.get('data', {})
        pos = data0.get('Position', {}).get('val', {}).get('Vector3')
        if not pos or len(pos) < 3:
            log(f"  apply_embedded_weapon_xforms: '{model_name}' has no Position stream")
            continue

        # Resolve the vert index set + bind_world for this entry.
        bind_world = None
        indices = None
        if has_verts:
            # Move Triangles path: explicit slot[0] vert indices,
            # world-space translation only. No bone-local frame.
            n_pos = min(len(pos[0]), len(pos[1]), len(pos[2]))
            indices = [int(v) for v in verts_list if 0 <= int(v) < n_pos]
            if not indices:
                log(f"  apply_embedded_weapon_xforms: '{model_name}' verts list empty after bounds-check")
                continue
            bind_world = None  # sentinel — translation-only path below
        elif bone_name:
            bones = m.get('bones', []) or []
            if bone_name not in bones:
                log(f"  apply_embedded_weapon_xforms: '{model_name}' has no bone '{bone_name}'")
                continue
            bone_idx = bones.index(bone_name)
            skin_order = m.get('skin_order', []) or []
            skin_binds = m.get('skin_binds', []) or []
            palette_idx = None
            for i, b in enumerate(skin_order):
                if b == bone_idx:
                    palette_idx = i; break
            if palette_idx is None or palette_idx >= len(skin_binds):
                log(f"  apply_embedded_weapon_xforms: bone '{bone_name}' not in '{model_name}' skin palette")
                continue
            # skin_binds stores INVERSE bind; invert back to bind_world.
            inv_bind = _mat_from_json(skin_binds[palette_idx])
            bind_world = _mat_inverse(inv_bind)
            # Scan BlendIndices + BlendWeight to find verts weighted to bone.
            bi_stream = data0.get('BlendIndices', {}).get('val', {}).get('Unorm4x8', [])
            bw_stream = data0.get('BlendWeight',  {}).get('val', {}).get('Unorm4x8', [])
            if not bi_stream:
                log(f"  apply_embedded_weapon_xforms: '{model_name}' slot[0] has no BlendIndices")
                continue
            target = palette_idx & 0xFF
            # Pass 1: strict pair match. Matches Pandemic's shipped vert layout.
            indices = []
            for v_i in range(len(bi_stream)):
                b0, b1, b2, b3 = _bytes_at(bi_stream, v_i)
                w0, w1, w2, w3 = _bytes_at(bw_stream, v_i) if bw_stream else (255, 0, 0, 0)
                bs = (b0, b1, b2, b3)
                ws = (w0, w1, w2, w3)
                hit = False
                for k in range(4):
                    if bs[k] == target and ws[k] > 0:
                        hit = True; break
                if hit:
                    indices.append(v_i)
            # Pass 2 (fallback): loose match for M2-era embed bug where the
            # bone byte and the weight byte land in different slots. The
            # in-engine preview already does this fallback in
            # LevelScene::getVertsWeightedToBone; keeping the same logic
            # here so Apply commits the same verts the slider moved.
            if not indices and target != 0:
                for v_i in range(len(bi_stream)):
                    b0, b1, b2, b3 = _bytes_at(bi_stream, v_i)
                    w0, w1, w2, w3 = _bytes_at(bw_stream, v_i) if bw_stream else (255, 0, 0, 0)
                    if (w0 | w1 | w2 | w3) == 0:
                        continue
                    if target in (b0, b1, b2, b3):
                        indices.append(v_i)
            if not indices:
                log(f"  apply_embedded_weapon_xforms: bone '{bone_name}' on '{model_name}' selects 0 verts")
                continue
        else:
            # Legacy weapon-idx path
            sidecar_entry = sidecar.get('models', {}).get(model_name, {})
            weapons = sidecar_entry.get('weapons', [])
            if weapon_idx >= len(weapons):
                log(f"  apply_embedded_weapon_xforms: weapon_idx {weapon_idx} out of range for '{model_name}'")
                continue
            tag = weapons[weapon_idx]
            vert_first  = int(tag.get('vert_first', 0))
            vert_count  = int(tag.get('vert_count', 0))
            bind_world  = list(tag.get('bind_world', []))
            if vert_count <= 0 or len(bind_world) != 16:
                log(f"  apply_embedded_weapon_xforms: tag for '{model_name}'[{weapon_idx}] incomplete")
                continue
            end = vert_first + vert_count
            end = min(end, len(pos[0]), len(pos[1]), len(pos[2]))
            indices = list(range(vert_first, end))

        # Translation-only short-circuit for the vert-list (Move Triangles)
        # path: the panel slides in WORLD XYZ so we apply directly without
        # bone-local conjugation. Skips matrix multiplies entirely.
        if bind_world is None:
            dx, dy, dz = float(dxyz[0]), float(dxyz[1]), float(dxyz[2])
            moved = 0
            for i in indices:
                if i >= len(pos[0]) or i >= len(pos[1]) or i >= len(pos[2]):
                    continue
                pos[0][i] += dx
                pos[1][i] += dy
                pos[2][i] += dz
                moved += 1
            with open(model_path, 'w', encoding='utf-8') as f:
                json.dump(m, f, indent=1)
            committed += 1
            log(f"  apply_embedded_weapon_xforms: '{model_name}' [verts={len(indices)}] "
                f"translated {moved} verts by ({dx:+.3f}, {dy:+.3f}, {dz:+.3f})")
            continue

        # world_delta = bind_world * T(xyz)*R(ypr) * inv(bind_world)
        deg2rad = math.pi / 180.0
        delta_local = _mat_from_ypr_xyz(
            [dxyz[0], dxyz[1], dxyz[2]],
            [dypr[0] * deg2rad, dypr[1] * deg2rad, dypr[2] * deg2rad])
        inv_bind = _mat_inverse(bind_world)
        world_delta = _mat_mul(_mat_mul(bind_world, delta_local), inv_bind)

        moved = 0
        for i in indices:
            if i >= len(pos[0]) or i >= len(pos[1]) or i >= len(pos[2]):
                continue
            ox = pos[0][i]; oy = pos[1][i]; oz = pos[2][i]
            nx, ny, nz = _mat_transform_point(world_delta, (ox, oy, oz))
            pos[0][i] = nx
            pos[1][i] = ny
            pos[2][i] = nz
            moved += 1

        with open(model_path, 'w', encoding='utf-8') as f:
            json.dump(m, f, indent=1)

        # Sidecar offset bookkeeping (only meaningful for the weapon-idx
        # path; bone-keyed entries don't have a sidecar to update).
        if not bone_name and weapon_idx >= 0:
            sidecar_entry = sidecar.get('models', {}).get(model_name, {})
            weapons = sidecar_entry.get('weapons', [])
            if weapon_idx < len(weapons):
                tag = weapons[weapon_idx]
                cur_xyz = list(tag.get('offset_xyz', [0.0, 0.0, 0.0]))
                cur_ypr = list(tag.get('offset_ypr', [0.0, 0.0, 0.0]))
                for j in range(3):
                    cur_xyz[j] = float(cur_xyz[j]) + float(dxyz[j])
                    cur_ypr[j] = float(cur_ypr[j]) + float(dypr[j])
                tag['offset_xyz'] = cur_xyz
                tag['offset_ypr'] = cur_ypr

        committed += 1
        label = f"bone={bone_name}" if bone_name else f"weapon_idx={weapon_idx}"
        log(f"  apply_embedded_weapon_xforms: '{model_name}' [{label}] "
            f"moved {moved} verts (XYZ={dxyz}, YPR={dypr})")

    # Persist sidecar updates only if the file already existed (so we
    # don't create a stub when every entry was bone-keyed and the
    # sidecar wasn't touched).
    if os.path.isfile(sidecar_path):
        try:
            with open(sidecar_path, 'w', encoding='utf-8') as f:
                json.dump(sidecar, f, indent=1)
        except Exception as e:
            log(f"  apply_embedded_weapon_xforms: failed to rewrite sidecar: {e}")

    # Consume the xforms file so the next save without new edits is a no-op.
    try:
        os.remove(xforms_path)
        log(f"  apply_embedded_weapon_xforms: committed {committed} entries, consumed {os.path.basename(xforms_path)}")
    except Exception as e:
        log(f"  apply_embedded_weapon_xforms: could not remove xforms file: {e}")


def apply_strip_specs(dump_dir, spec_path):
    """
    Strip baked-in weapon geometry from CRD models per ze_strip_specs.json sidecar.

    Each spec entry names a source CRD model and a list of bones whose
    weighted vertices should be removed. The function reads the CRD JSON,
    identifies vertices weighted to any of those bones (via BlendIndices
    bytes → skin_order → bone name), drops them, then walks each index
    buffer and drops every triangle that referenced a dropped vertex.
    Remaining indices are rewritten through the old→new vertex map, vertex
    streams are repacked, and buffer_info sizes (vbuff_size, i_num, tri_num)
    are recomputed. Output is written as <dump_dir>/models/<new_name>.json
    (preserving the original CRD untouched). The new name is appended to
    pak_strings/bin_strings so the engine can resolve it.

    Spec entry shape:
      {
        "new_name": "CRD_CH_elf_ancn_stripped_01",
        "source":   "CRD_CH_elf_ancn_swd_all_01",
        "bones":    ["Bone_RHand_attach"]   // bones whose weighted vertices to strip
      }
    """
    if not os.path.isfile(spec_path):
        log(f"  --strip-weapons: spec file not found: {spec_path}")
        return

    try:
        with open(spec_path, 'r', encoding='utf-8') as f:
            spec = json.load(f)
    except Exception as e:
        log(f"  --strip-weapons: failed to parse spec: {e}")
        return

    if not isinstance(spec, list) or not spec:
        log(f"  --strip-weapons: spec is empty")
        return

    models_dir = os.path.join(dump_dir, 'models')
    if not os.path.isdir(models_dir):
        log(f"  --strip-weapons: models dir missing at {models_dir}")
        return

    log(f"  --strip-weapons: applying {len(spec)} strip entries")

    new_names_added = []

    for entry_idx, entry in enumerate(spec):
        new_name    = entry.get('new_name', '')
        source_name = entry.get('source', '')
        bones_strip = entry.get('bones', []) or []
        if not (new_name and source_name and bones_strip):
            log(f"  [{entry_idx}] SKIP: missing new_name / source / bones")
            continue

        src_path = os.path.join(models_dir, source_name + '.json')
        out_path = os.path.join(models_dir, new_name + '.json')
        if not os.path.isfile(src_path):
            log(f"  [{entry_idx}] SKIP '{new_name}': source not found at {src_path}")
            continue

        try:
            with open(src_path, 'r', encoding='utf-8') as f:
                model = json.load(f)
        except Exception as e:
            log(f"  [{entry_idx}] ERROR reading {src_path}: {e}")
            continue

        # ── Decode strip criterion: UV-box vs bone-based vs auto ─────────
        # The Mesh Surgery panel passes UV bounds encoded in the bones[] list
        # as "__UV_BOX__u_min,v_min,u_max,v_max" or "__AUTO__" to invoke the
        # auto-find heuristic at save time. The legacy Strip Baked Weapon
        # panel passes real bone names. Detect which mode this spec uses.
        uv_box = None
        if len(bones_strip) == 1 and bones_strip[0] == '__AUTO__':
            # Auto-detect: run the heuristic on the loaded source model.
            uv_box = auto_find_weapon_uv_box(model)
            if uv_box is None:
                log(f"  [{entry_idx}] SKIP '{new_name}': auto-find found no weapon "
                    f"verts (no UV outliers at hand height in source)")
                continue
            log(f"  [{entry_idx}] '{new_name}': AUTO-detected UV box "
                f"U[{uv_box[0]:.3f},{uv_box[2]:.3f}] V[{uv_box[1]:.3f},{uv_box[3]:.3f}]")
        elif len(bones_strip) == 1 and bones_strip[0].startswith('__UV_BOX__'):
            try:
                parts = bones_strip[0][len('__UV_BOX__'):].split(',')
                uv_box = (float(parts[0]), float(parts[1]),
                          float(parts[2]), float(parts[3]))
                log(f"  [{entry_idx}] '{new_name}': UV-box mode, "
                    f"U[{uv_box[0]:.3f},{uv_box[2]:.3f}] V[{uv_box[1]:.3f},{uv_box[3]:.3f}]")
            except (ValueError, IndexError):
                log(f"  [{entry_idx}] SKIP '{new_name}': malformed __UV_BOX__ spec")
                continue

        # ── Build the set of bind indices that map to a stripped bone ──
        # (Bone-based mode only — UV-box mode skips this block.)
        bones_list = model.get('bones', [])
        skin_order = model.get('skin_order', [])
        strip_bone_idxs = set()
        strip_bind_idxs = set()
        if uv_box is None:
            for bone_name in bones_strip:
                if bone_name in bones_list:
                    strip_bone_idxs.add(bones_list.index(bone_name))
                else:
                    log(f"  [{entry_idx}] WARN: bone '{bone_name}' not in model bones")
            if not strip_bone_idxs:
                log(f"  [{entry_idx}] SKIP '{new_name}': no valid strip bones")
                continue
            strip_bind_idxs = {bi for bi, b in enumerate(skin_order) if b in strip_bone_idxs}
            log(f"  [{entry_idx}] '{new_name}': stripping bone indices "
                f"{sorted(strip_bone_idxs)} via bind indices {sorted(strip_bind_idxs)}")

        total_dropped_verts = 0
        total_dropped_tris  = 0

        vertex_data = model.get('vertex_data', [])
        index_data  = model.get('index_data',  [])
        buffer_infos = model.get('buffer_infos', [])

        # The bow's bind-byte extraction helper. Returns the 4 bytes packed
        # into a Unorm4x8 entry. Streams may be stored as list of packed
        # u32 ints OR lists of 4 bytes; handle both.
        def bind_bytes_at(stream, i):
            v = stream[i]
            if isinstance(v, int):
                return ((v >> 0) & 0xFF, (v >> 8) & 0xFF,
                        (v >> 16) & 0xFF, (v >> 24) & 0xFF)
            if isinstance(v, list) and len(v) >= 4:
                return v[0], v[1], v[2], v[3]
            return (0, 0, 0, 0)

        # ── For each mesh slot: identify, filter, recompact ──
        for slot_idx in range(min(len(vertex_data), len(index_data), len(buffer_infos))):
            vd = vertex_data[slot_idx]
            id_ = index_data[slot_idx]
            bi = buffer_infos[slot_idx]

            data = vd.get('data', {})
            if 'BlendIndices' not in data:
                continue  # static mesh, no skin → can't be a skinned weapon
            bi_stream = data['BlendIndices']['val'].get('Unorm4x8', [])
            bw_stream = data.get('BlendWeight', {}).get('val', {}).get('Unorm4x8', [])
            n_verts = len(bi_stream)
            if n_verts == 0:
                continue

            # ── Mark each vertex: True = keep, False = drop ──
            # UV-box mode: strip if vertex's UV falls inside the box.
            # Bone mode: strip if any non-zero-weight bind is in strip set.
            keep = [True] * n_verts
            dropped = 0
            if uv_box is not None:
                uv_stream = data.get('TextureCoord(0)', {}).get('val', {}).get('Vector2')
                if not uv_stream:
                    continue  # no UVs in this slot, can't UV-strip
                umin, vmin, umax, vmax = uv_box
                for v_i in range(n_verts):
                    if v_i >= len(uv_stream[0]) or v_i >= len(uv_stream[1]):
                        continue
                    u, vv = uv_stream[0][v_i], uv_stream[1][v_i]
                    if umin <= u <= umax and vmin <= vv <= vmax:
                        keep[v_i] = False
                        dropped += 1
            else:
                for v_i in range(n_verts):
                    b0, b1, b2, b3 = bind_bytes_at(bi_stream, v_i)
                    if v_i < len(bw_stream):
                        w0, w1, w2, w3 = bind_bytes_at(bw_stream, v_i)
                    else:
                        w0 = w1 = w2 = w3 = 255
                    if ((b0 in strip_bind_idxs and w0 > 0) or
                        (b1 in strip_bind_idxs and w1 > 0) or
                        (b2 in strip_bind_idxs and w2 > 0) or
                        (b3 in strip_bind_idxs and w3 > 0)):
                        keep[v_i] = False
                        dropped += 1

            if dropped == 0:
                continue  # nothing in this slot referenced the strip bones

            # ── Build old→new vertex index map for the kept vertices ──
            old_to_new = {}
            new_idx = 0
            for v_i in range(n_verts):
                if keep[v_i]:
                    old_to_new[v_i] = new_idx
                    new_idx += 1
            new_vert_count = new_idx

            # ── Filter every vertex_data stream: scalar streams (BlendIndices,
            # BlendWeight, Normal, Color(1), Tangent — Unorm4x8) and
            # vector streams (Position Vector3, TextureCoord Vector2) ─────
            for stream_name, stream_obj in list(data.items()):
                if not isinstance(stream_obj, dict):
                    continue
                val = stream_obj.get('val', {})
                for type_key, type_data in list(val.items()):
                    if not isinstance(type_data, list) or not type_data:
                        continue
                    # Detect vector (list-of-component-lists) vs scalar
                    if isinstance(type_data[0], list):
                        # Vector stream: outer = N components, each is a list of n_verts
                        new_components = []
                        for comp in type_data:
                            new_components.append([comp[i] for i in range(n_verts)
                                                   if i < len(comp) and keep[i]])
                        val[type_key] = new_components
                    else:
                        # Scalar (one-per-vertex) stream
                        val[type_key] = [type_data[i] for i in range(min(n_verts, len(type_data)))
                                         if keep[i]]

            # ── Filter the index buffer: drop triangles referencing any
            # dropped vertex, rewrite kept triangles through old_to_new ───
            ib = id_.get('U32', {}).get('vals', []) if isinstance(id_, dict) else []
            if not ib:
                # Index data missing — try other formats lotrc_rs might emit
                continue
            new_indices = []
            tri_count_old = len(ib) // 3
            for tri_i in range(tri_count_old):
                a = ib[tri_i * 3 + 0]
                b = ib[tri_i * 3 + 1]
                c = ib[tri_i * 3 + 2]
                if (a < len(keep) and b < len(keep) and c < len(keep)
                    and keep[a] and keep[b] and keep[c]):
                    new_indices.append(old_to_new[a])
                    new_indices.append(old_to_new[b])
                    new_indices.append(old_to_new[c])
            id_['U32']['vals'] = new_indices
            tri_count_new = len(new_indices) // 3
            dropped_tris = tri_count_old - tri_count_new

            # ── Update buffer_info counts ──
            v_size = int(bi.get('v_size', 0))
            if v_size > 0:
                bi['vbuff_size']    = new_vert_count * v_size
            bi['i_num']   = len(new_indices)
            bi['tri_num'] = tri_count_new

            # Update size field on vertex_data info too
            vd_info = vd.get('info', {})
            if 'size' in vd_info and v_size > 0:
                vd_info['size'] = new_vert_count * v_size

            total_dropped_verts += dropped
            total_dropped_tris  += dropped_tris
            log(f"    slot[{slot_idx}]: {n_verts} -> {new_vert_count} verts, "
                f"{tri_count_old} -> {tri_count_new} tris")

        # ── Rename + write out ──
        model['info']['key']       = new_name
        model['info']['asset_key'] = new_name

        # ── Atlas reconstruction: bake a new DDS with the strip patch
        # inpainted to neighborhood-average color, mutate mats[0].tex0
        # to point at the new atlas, register the new texture name in
        # the string tables. Only runs when we know the UV rect (UV-box
        # mode or auto-find). Bone-based strips inherit the parent
        # atlas and skip this. _bake_atlas_for_strip mutates model in
        # place so the json.dump below picks up the new tex0.
        try:
            _bake_atlas_for_strip(dump_dir, source_name, new_name, model, uv_box)
        except Exception as e:
            log(f"  [{entry_idx}] WARN atlas bake threw: {e}. "
                f"Geometry strip still proceeds; model inherits parent atlas.")
            import traceback
            log(traceback.format_exc())

        try:
            with open(out_path, 'w', encoding='utf-8') as f:
                json.dump(model, f, indent=1)
            log(f"  [{entry_idx}] wrote {out_path} "
                f"(total dropped: {total_dropped_verts} verts, "
                f"{total_dropped_tris} tris)")
            new_names_added.append(new_name)
            # Also register the new model in objas.json so lotrc_rs allocates
            # enough BIN space for it. Missing entries make lotrc_rs default
            # to a tiny ~128KB allocation that truncates merged CRDs and
            # makes Vespucci's loader bail on ibuff overflow.
            try:
                _register_new_model_in_objas(dump_dir, new_name, source_name)
            except Exception as _e:
                log(f"  [{entry_idx}] WARN objas register threw: {_e}")
        except Exception as e:
            log(f"  [{entry_idx}] ERROR writing {out_path}: {e}")

    # Register new names in pak_strings/bin_strings so lotrc_rs -c resolves them
    if new_names_added:
        for sfn in ('pak_strings.json', 'bin_strings.json', 'string_keys.json'):
            sp = os.path.join(dump_dir, sfn)
            if not os.path.isfile(sp):
                continue
            try:
                with open(sp, 'r', encoding='utf-8') as f:
                    arr = json.load(f)
                existing = set(arr) if isinstance(arr, list) else set()
                added = 0
                for n in new_names_added:
                    if n not in existing:
                        arr.append(n)
                        added += 1
                if added:
                    with open(sp, 'w', encoding='utf-8') as f:
                        json.dump(arr, f, indent=1)
                    log(f"  {sfn}: +{added} new stripped-model names")
            except Exception as e:
                log(f"  WARN: failed to update {sfn}: {e}")


def apply_merged_models(dump_dir, spec_path):
    """
    Bake merged character+weapon meshes per ze_merged_models.json sidecar.

    For each entry in the spec, loads <dump_dir>/models/<base>.json, merges
    in each weapon's geometry (bone-remapped, position-transformed), and
    writes the result as <dump_dir>/models/<new_name>.json. Also appends
    <new_name> to <dump_dir>/pak_strings.json so lotrc_rs -c picks it up.

    See the comment block above this function for the full lotrc_rs model
    JSON schema reverse-engineered from real files (WP_elf_ancn_bow_01.json,
    CRD_CH_urk_spr_all_01.json) and the merge-algorithm rationale.
    """
    if not os.path.isfile(spec_path):
        log(f"  --merged-models: spec file not found: {spec_path}")
        return

    try:
        with open(spec_path, 'r', encoding='utf-8') as f:
            spec = json.load(f)
    except Exception as e:
        log(f"  --merged-models: failed to parse spec: {e}")
        return

    if not isinstance(spec, list) or not spec:
        log(f"  --merged-models: spec is empty, nothing to do")
        return

    models_dir = os.path.join(dump_dir, 'models')
    if not os.path.isdir(models_dir):
        log(f"  --merged-models: <dump_dir>/models not found at {models_dir}")
        return

    log(f"  --merged-models: applying {len(spec)} merge entries from {spec_path}")

    new_names_added = []

    for entry_idx, entry in enumerate(spec):
        new_name = entry.get('new_name', '')
        base_name = entry.get('base', '')
        weapons = entry.get('weapons', [])
        embed_mode = int(entry.get('embed_mode', 0))
        embed_uv_rect = entry.get('embed_uv_rect', [0.0, 0.0, 0.0, 0.0])
        # Auto-spawn payload from the sidecar. If the user ticked the
        # box on the panel we're going to bolt a 3dCrowd entry onto
        # the level so the bake doesn't vanish into models/. spawn_pos
        # comes from the camera focus point at bake time, count from
        # the panel slider.
        auto_spawn  = int(entry.get('auto_spawn', 0))
        spawn_count = int(entry.get('spawn_count', 3))
        spawn_pos   = entry.get('spawn_pos', [0.0, 0.0, 0.0])
        # First weapon's attach bone gets passed through to the spawn
        # helper as a hint. It only uses it for diag logging right now
        # so a missing one is harmless.
        primary_attach_bone = ''
        if weapons and isinstance(weapons, list) and isinstance(weapons[0], dict):
            primary_attach_bone = weapons[0].get('attach_bone', '')

        if not new_name or not base_name:
            log(f"  [{entry_idx}] SKIP: missing new_name or base in entry")
            continue

        base_path = os.path.join(models_dir, base_name + '.json')
        if not os.path.isfile(base_path):
            log(f"  [{entry_idx}] SKIP '{new_name}': base model not found: {base_path}")
            continue

        out_path = os.path.join(models_dir, new_name + '.json')
        if os.path.exists(out_path):
            log(f"  [{entry_idx}] WARN '{new_name}': output already exists, overwriting (idempotent)")

        log(f"  [{entry_idx}] Baking '{new_name}' from base='{base_name}' + {len(weapons)} weapon(s) "
            f"(embed_mode={embed_mode})")

        try:
            with open(base_path, 'r', encoding='utf-8') as f:
                base = json.load(f)
        except Exception as e:
            log(f"    ERROR: failed to load base JSON: {e}")
            continue

        # ── Embed path. Different game entirely from the new-slot route. ──
        # When embed_mode is 1 the bake skips the new-mesh-slot ceremony and
        # hammers the weapon verts straight into base.slot[0]. UVs map into
        # embed_uv_rect (a region of the base atlas the user picked, usually
        # the patch the stripped weapon used to occupy). Skin weights all
        # go to the attach bone via its skin_order bind index. Vertices and
        # tris get appended, buffer_info bumped, done. No second mat slot,
        # no second texture, no asset_type or fmt1 dance with the stolen
        # Havok crowd shader.
        if embed_mode == 1 and weapons:
            try:
                weapon_tags = _embed_weapon_into_slot0(
                    base, base_name, weapons, embed_uv_rect, models_dir)
                base['info']['key']       = new_name
                base['info']['asset_key'] = new_name

                # Record vert ranges for the popup mesh editor. Written
                # to the dump root, not the model JSON, so re-baking the
                # same model name simply overwrites its entry and
                # multiple merged models share one sidecar.
                try:
                    _write_embedded_weapon_sidecar(dump_dir, new_name, weapon_tags or [])
                except Exception as ee:
                    log(f"  [{entry_idx}] WARN embedded-weapon sidecar write threw: {ee}")

                # Atlas reconstruction. Crop the weapon's diffuse atlas
                # by its actual UV footprint, resize, paste onto a copy
                # of the base atlas at embed_uv_rect with 2px feather,
                # write a new DDS, rewrite mats[0].tex0. Skipped if
                # weapons[] is empty or the UV rect is dead. v1 only
                # handles weapons[0]; multi-weapon merges paste each
                # weapon onto its own slot of the embed rect (future).
                try:
                    w0 = weapons[0] if weapons else None
                    if isinstance(w0, dict):
                        _bake_atlas_for_embed(dump_dir, base_name, new_name,
                                              base, w0.get('mesh', ''),
                                              embed_uv_rect)
                except Exception as ea:
                    log(f"  [{entry_idx}] WARN atlas bake threw: {ea}. "
                        f"Geometry embed still proceeds; merged model "
                        f"inherits base atlas (bow will sample wrong pixels).")
                    import traceback
                    log(traceback.format_exc())

                with open(out_path, 'w', encoding='utf-8') as f:
                    json.dump(base, f, indent=1)
                log(f"  [{entry_idx}] Wrote (embed) {out_path}")
                new_names_added.append(new_name)
                # Register in objas so lotrc_rs allocates enough BIN
                # space. Without this the merged asset gets truncated
                # at the default ~128KB, ibuff overflows, viewport bails.
                try:
                    _register_new_model_in_objas(dump_dir, new_name, base_name)
                except Exception as _e:
                    log(f"  [{entry_idx}] WARN objas register threw: {_e}")
                # Drag the freshly-baked mesh out of the goddamn void
                # if the user ticked auto-spawn. Without this the JSON
                # rots in models/ unloved while the user reloads and
                # stares at empty viewport, which was the panel's worst
                # UX failure for months. Not happening anymore.
                if auto_spawn:
                    _append_crowd_spawn(dump_dir, new_name, base_name,
                                        spawn_pos, spawn_count,
                                        primary_attach_bone, weapons)
            except Exception as e:
                log(f"  [{entry_idx}] ERROR in embed path: {e}")
                import traceback
                log(traceback.format_exc())
            continue
        # Otherwise fall through to the legacy new-slot pipeline below.

        # Update top-level key/asset_key in 'info' so the merged model
        # identifies as <new_name>, not the original base. Also force
        # gamemodemask = -1 (any mode) so baked crowd meshes load in
        # every gamemode regardless of what the source character's mask
        # was (Haldir ships with mask=11 = Campaign+TDM+Conquest only,
        # which would gate the bake out of any future modes you add).
        # Matches the lesson learned with PR_gdr_Barricade_01 palisades.
        if 'info' in base and isinstance(base['info'], dict):
            base['info']['key'] = new_name
            base['info']['asset_key'] = new_name
            base['info']['gamemodemask'] = -1

        for w_idx, weapon in enumerate(weapons):
            wmesh = weapon.get('mesh', '')
            attach_bone = weapon.get('attach_bone', '')
            offset_xyz = weapon.get('offset_xyz', [0.0, 0.0, 0.0])
            offset_ypr = weapon.get('offset_ypr', [0.0, 0.0, 0.0])

            if not wmesh:
                log(f"    weapon[{w_idx}]: SKIP, no mesh name")
                continue

            wpath = os.path.join(models_dir, wmesh + '.json')
            if not os.path.isfile(wpath):
                log(f"    weapon[{w_idx}] '{wmesh}': SKIP, mesh JSON not found at {wpath}")
                continue

            try:
                with open(wpath, 'r', encoding='utf-8') as f:
                    wp = json.load(f)
            except Exception as e:
                log(f"    weapon[{w_idx}] '{wmesh}': SKIP, failed to load: {e}")
                continue

            # ── 1. Resolve attach bone in base ──────────────────────────
            base_bones = base.get('bones', [])
            base_bone_transforms = base.get('bone_transforms', [])
            base_bone_parents = base.get('bone_parents', [])
            base_bone_bboxes = base.get('bone_bounding_boxes', [])
            base_skin_binds = base.get('skin_binds', [])
            base_skin_order = base.get('skin_order', [])

            attach_idx = -1
            if attach_bone:
                for i, bn in enumerate(base_bones):
                    if bn == attach_bone:
                        attach_idx = i
                        break

            if attach_idx < 0:
                log(f"    weapon[{w_idx}] '{wmesh}': WARN, attach_bone '{attach_bone}' not in base bones; falling back to bone 0")
                attach_idx = 0

            if attach_idx >= len(base_bone_transforms):
                log(f"    weapon[{w_idx}] '{wmesh}': SKIP, attach bone index {attach_idx} out of range")
                continue

            # ── 2. Compute attach world transform ────────────────────────
            # NOTE: bone_transforms in this format are LOCAL (parent-relative).
            # attach_world here is actually attach-LOCAL — we only use it for
            # the static-vertex pre-transform below, where we want the bow's
            # vertices to land at attach-bone-local origin + offset. For
            # skinned vertices the attach happens via BlendIndices at draw
            # time, so attach_mat is just used as a "best-effort" rest-pose
            # placement for static fmt1 vertices that we don't otherwise
            # know how to position.
            attach_local_xform = _mat_from_json(base_bone_transforms[attach_idx])
            offset_local = _mat_from_json_offset(offset_xyz, offset_ypr)
            attach_mat = _mat_mul(attach_local_xform, offset_local)

            # ── 3. Build bone remap (weapon → base) ──────────────────────
            # Bones in the weapon that already exist by name in base (e.g.
            # the shield's Bone_Lumbar3 matches Haldir's) get remapped to
            # the base's index — no new bone is appended for them. Bones
            # that DON'T exist by name (e.g. WP_roh_shield_a_00) get
            # appended as CHILDREN of attach_bone, with their ORIGINAL
            # local transform preserved so skeletal hierarchy stays sane.
            wp_bones = wp.get('bones', [])
            wp_bone_parents = wp.get('bone_parents', [])
            wp_bone_transforms = wp.get('bone_transforms', [])
            wp_bone_bboxes = wp.get('bone_bounding_boxes', [])

            base_name_to_idx = {n: i for i, n in enumerate(base_bones)}
            bone_idx_remap = {}
            newly_appended_bone_indices = []  # base indices we just added

            for wi, wname in enumerate(wp_bones):
                if wname in base_name_to_idx:
                    bone_idx_remap[wi] = base_name_to_idx[wname]
                else:
                    new_idx = len(base_bones)
                    base_bones.append(wname)
                    base_name_to_idx[wname] = new_idx
                    newly_appended_bone_indices.append(new_idx)

                    # Parent: if weapon parent is in remap, use that; else
                    # fall back to attach bone in base.
                    wp_parent = wp_bone_parents[wi] if wi < len(wp_bone_parents) else -1
                    if wp_parent in bone_idx_remap:
                        new_parent = bone_idx_remap[wp_parent]
                    elif wp_parent == -1:
                        new_parent = attach_idx
                    else:
                        new_parent = attach_idx
                    base_bone_parents.append(new_parent)

                    # Transform: store the weapon's ORIGINAL LOCAL transform
                    # (parent-relative). bone_transforms in this format are
                    # local — composing with attach_mat to get a "world"
                    # matrix here was the previous bug. The new parent is
                    # already attach_idx, so the local transform stays
                    # local-relative-to-attach automatically.
                    if wi < len(wp_bone_transforms):
                        wp_local_json = wp_bone_transforms[wi]
                        base_bone_transforms.append(
                            json.loads(json.dumps(wp_local_json)))
                    else:
                        base_bone_transforms.append(_mat_to_json(_mat_identity()))

                    # Bounding box: clone if present, else zero
                    if wi < len(wp_bone_bboxes):
                        base_bone_bboxes.append(json.loads(json.dumps(wp_bone_bboxes[wi])))
                    else:
                        base_bone_bboxes.append(_zero_bbox())

                    bone_idx_remap[wi] = new_idx

            # ── 3b. Grow skin_binds / skin_order for newly-added bones ──
            # The engine pointer-walks skin_binds with skin_order. Each
            # entry in skin_order is a u32 bone-palette index; the matching
            # entry in skin_binds is the inverse-bind matrix that takes a
            # world-space vertex into that bone's local space. For newly-
            # appended bones (which are CHILDREN of attach_idx with the
            # weapon's local transform preserved), proper inverse bind
            # would require composing the chain of parent transforms and
            # inverting. We don't have numpy here and the chain may not
            # be needed in practice (the static→skinned pipeline below
            # weights every weapon vertex to attach_idx, not to the new
            # bones), so we append IDENTITY as a safe placeholder. The
            # entry exists so skin_binds_num matches expectations; the
            # matrix content is only read if a vertex actually points at
            # this bone via BlendIndices — which the injected-skin path
            # below avoids. TODO Phase 5b: real inverse-bind math.
            for new_b_idx in newly_appended_bone_indices:
                base_skin_binds.append(_mat_to_json(_mat_identity()))
                base_skin_order.append(new_b_idx)

            # Persist back (in case Python aliased these out of base dict)
            base['bones'] = base_bones
            base['bone_parents'] = base_bone_parents
            base['bone_transforms'] = base_bone_transforms
            base['bone_bounding_boxes'] = base_bone_bboxes
            base['skin_binds'] = base_skin_binds
            base['skin_order'] = base_skin_order

            # ── 4. Pre-compute shifts ────────────────────────────────────
            base_vertex_data = base.setdefault('vertex_data', [])
            base_index_data = base.setdefault('index_data', [])
            base_buffer_infos = base.setdefault('buffer_infos', [])
            base_mats = base.setdefault('mats', [])
            base_mat_extras = base.setdefault('mat_extras', [])
            base_mat_order = base.setdefault('mat_order', [])
            base_mesh_order = base.setdefault('mesh_order', [])
            base_mesh_bboxes = base.setdefault('mesh_bounding_boxes', [])

            vbuff_shift = len(base_vertex_data)
            ibuff_shift = len(base_index_data)
            buffer_info_shift = len(base_buffer_infos)
            mats_shift = len(base_mats)

            wp_vertex_data = wp.get('vertex_data', [])
            wp_index_data = wp.get('index_data', [])
            wp_buffer_infos = wp.get('buffer_infos', [])
            wp_mats = wp.get('mats', [])
            wp_mat_extras = wp.get('mat_extras', [])
            wp_mat_order = wp.get('mat_order', [])
            wp_mesh_order = wp.get('mesh_order', [])
            wp_mesh_bboxes = wp.get('mesh_bounding_boxes', [])
            wp_lod0 = wp.get('info', {}).get('lod0', {})

            # ── 4a. Restrict to weapon's LOD0 slot range ─────────────────
            # Weapon's mesh_order spans all 4 LODs; we only want LOD0.
            # If lod0 is malformed, fall back to all of mesh_order.
            try:
                lod0_start = int(wp_lod0.get('start', 0))
                lod0_end = int(wp_lod0.get('breakable_end', len(wp_mesh_order)))
                if lod0_end <= lod0_start or lod0_end > len(wp_mesh_order):
                    lod0_start, lod0_end = 0, len(wp_mesh_order)
            except Exception:
                lod0_start, lod0_end = 0, len(wp_mesh_order)

            # Figure out which buffer_infos LOD0 actually references so we
            # only carry those over. (Weapon buffer_infos array may include
            # entries used by higher LODs that we'd otherwise drag along.)
            lod0_buf_indices = set()
            for slot in range(lod0_start, lod0_end):
                mo = wp_mesh_order[slot]
                lod0_buf_indices.add(mo & 0x3FFFFFFF)

            # ── 5. Deep-copy + transform weapon vertex/index data ───────
            # We deep-copy every weapon vertex_data and index_data entry,
            # transforming positions/normals/tangents on the way and
            # remapping bone indices. Even entries not referenced by LOD0
            # get copied (to keep indices simple); the unreferenced ones
            # cost a few KB and won't be drawn.
            #
            # CRITICAL: If a weapon vertex_data entry is STATIC (fmt1 lacks
            # BlendIndices/BlendWeight bits, e.g. fmt1=327 for a rigid
            # shield), we MUST inject those streams. The crowd renderer
            # always pipes [static_end..skinned_end] slots through the
            # skinning vertex shader, which reads BlendIndices to pick
            # which bone matrix to apply. With no BlendIndices stream the
            # engine either reads garbage (random bones, deforming the
            # weapon into the void) or silently kills the draw call. By
            # pre-transforming positions/normals into attach-bone-local
            # space AND injecting BlendIndices=[attach_idx,0,0,0] /
            # BlendWeight=[255,0,0,0] for every vertex, the skinning
            # shader multiplies by attach_idx's bone matrix at draw time,
            # producing a rigid-attached look that follows the hand bone
            # through animation. Same trick the URK crowds use natively.
            wp_vd_vert_counts = []  # parallel to wp_vertex_data, used below
            wp_vd_was_static = []   # parallel; True if we injected streams
            new_vd_entries = []
            for vd in wp_vertex_data:
                vd_copy = json.loads(json.dumps(vd))

                # Pre-transform positions/normals/tangents from weapon-local
                # space into attach-bone-local space, so they land in the
                # right spot when the skinning shader multiplies by the
                # attach bone's world matrix at draw time. (For an already-
                # skinned weapon the positions are in skeleton-bind space
                # and this pre-transform is technically wrong — but our
                # current scope is static weapons, so we apply it
                # unconditionally and document the caveat.)
                _transform_vertex_data_entry(vd_copy, attach_mat)

                # Inject skin streams if missing. Returns True iff we
                # actually added BlendIndices/BlendWeight.
                injected = _inject_skin_streams(vd_copy, attach_idx)
                vert_n = _vertex_count(vd_copy)
                wp_vd_vert_counts.append(vert_n)
                wp_vd_was_static.append(injected)

                # If the weapon was ALREADY skinned, remap its existing
                # BlendIndices through bone_idx_remap (weapon→base palette).
                # After _inject_skin_streams the indices are already
                # pointing at base palette indices (we wrote attach_idx
                # directly), so this is a no-op for the static path but
                # important for any future skinned-weapon merges.
                if not injected:
                    _remap_bone_indices(vd_copy, bone_idx_remap)

                new_vd_entries.append(vd_copy)
                base_vertex_data.append(vd_copy)

            for ix in wp_index_data:
                base_index_data.append(json.loads(json.dumps(ix)))

            # ── 6. Append buffer_infos with shifted indices ─────────────
            # Track new buffer_info index for each weapon-side index so
            # the mesh_order remap below points at the right slot. If we
            # injected skin streams into any weapon vertex_data entry, we
            # must also grow that buffer_info's v_size and vbuff_size by
            # 8 bytes/vertex (BlendIndices=4 + BlendWeight=4).
            wp_to_new_bi_idx = {}
            for wbi_idx, wbi in enumerate(wp_buffer_infos):
                bi_copy = json.loads(json.dumps(wbi))
                _shift_buffer_info_offsets(bi_copy, vbuff_shift, ibuff_shift)

                # If this buffer_info's primary vbuff index points at a
                # vertex_data entry that just had skin streams injected,
                # bump its v_size + vbuff_size to match.
                # vbuff_info_offset (post-shift) is the absolute index;
                # subtract the shift to get back to the weapon-local one.
                wp_primary = int(wbi.get('vbuff_info_offset', 0))
                if 0 <= wp_primary < len(wp_vd_was_static) and wp_vd_was_static[wp_primary]:
                    _bump_buffer_info_skin_vsize(bi_copy, wp_vd_vert_counts[wp_primary])

                wp_to_new_bi_idx[wbi_idx] = len(base_buffer_infos)
                base_buffer_infos.append(bi_copy)

            # ── 7. Append mats + mat_extras ─────────────────────────────
            for m in wp_mats:
                base_mats.append(json.loads(json.dumps(m)))
            for me in wp_mat_extras:
                base_mat_extras.append(json.loads(json.dumps(me)) if me is not None else None)

            # ── 8. Build the new mesh_order entries (LOD0 only) ─────────
            # Don't insert into base_mesh_order yet — we need to know where
            # in the LOD0 SKINNED range to splice them.
            FLAGS_MASK = 0xC0000000
            INDEX_MASK = 0x3FFFFFFF
            new_mesh_order = []
            new_mesh_bboxes = []
            new_mat_order  = []
            for slot in range(lod0_start, lod0_end):
                mo = wp_mesh_order[slot]
                flags = mo & FLAGS_MASK
                old_idx = mo & INDEX_MASK
                new_idx = wp_to_new_bi_idx.get(old_idx, old_idx + buffer_info_shift)
                new_mesh_order.append((flags | (new_idx & INDEX_MASK)))

                if slot < len(wp_mesh_bboxes):
                    new_mesh_bboxes.append(json.loads(json.dumps(wp_mesh_bboxes[slot])))
                else:
                    new_mesh_bboxes.append(_zero_bbox())

                # mat_order is parallel to mesh_order; shift by mats_shift.
                if slot < len(wp_mat_order):
                    new_mat_order.append(int(wp_mat_order[slot]) + mats_shift)
                else:
                    new_mat_order.append(mats_shift)

            slots_added = len(new_mesh_order)

            # ── 9. Insert new slots at LOD0's SKINNED tail ───────────────
            # NOT at the tail of mesh_order. The mesh_order array is sliced
            # into ranges by lod0/lod1/lod2/lod3 — appending at the absolute
            # tail (after lod3.breakable_end) would leave the new slots
            # OUTSIDE every LOD range, and the engine would never iterate
            # them. Previous bug: mat_num was also being set to len(mats),
            # which is wrong (mat_num is per-slot, matches len(mat_order)),
            # so the engine truncated mat_order to 13 entries and Haldir's
            # torso slots (indices 13..31) ran off the end → torso
            # disappeared. Insert at lod0.skinned_end so the new static-
            # weapon slots sit at the tail of LOD0's drawable range, then
            # shift every later LOD edge / parallel array by slots_added.
            info = base.setdefault('info', {})
            lod0 = info.setdefault('lod0', {
                'start': 0, 'static_end': 0, 'skinned_end': 0,
                'physics_end': 0, 'breakable_end': 0,
            })

            insert_at = int(lod0.get('skinned_end', 0))
            insert_at = max(0, min(insert_at, len(base_mesh_order)))

            base_mesh_order  [insert_at:insert_at] = new_mesh_order
            base_mesh_bboxes [insert_at:insert_at] = new_mesh_bboxes
            base_mat_order   [insert_at:insert_at] = new_mat_order

            # Persist (in case Python aliased these out)
            base['mesh_order'] = base_mesh_order
            base['mesh_bounding_boxes'] = base_mesh_bboxes
            base['mat_order'] = base_mat_order

            # ── 10. Bump LOD ranges ──────────────────────────────────────
            # The new slots live in [insert_at, insert_at+slots_added).
            # Everything at index >= insert_at in any LOD range shifts by
            # slots_added. The insertion point is exactly lod0.skinned_end,
            # so lod0.static_end stays put (skin streams were injected, so
            # the new slots are SKINNED, not static), but skinned_end /
            # physics_end / breakable_end all bump up.
            for fk in ('skinned_end', 'physics_end', 'breakable_end'):
                if fk in lod0:
                    lod0[fk] = int(lod0[fk]) + slots_added

            # Every subsequent LOD edge sits past insert_at, so shift all
            # of lod1/lod2/lod3 by slots_added.
            for k in ('lod1', 'lod2', 'lod3'):
                ld = info.get(k)
                if ld is not None:
                    for fk in ('start', 'static_end', 'skinned_end',
                               'physics_end', 'breakable_end'):
                        if fk in ld:
                            ld[fk] = int(ld[fk]) + slots_added

            # ── 10b. Header counts ───────────────────────────────────────
            # CRITICAL fix: mat_num is the per-MESH-SLOT material count
            # (i.e. len(mat_order)), NOT the unique-material count
            # (len(mats)). Setting it to len(mats) was truncating the
            # engine's slot walk and killing torso geometry. Verified
            # against Haldir's source: bones_num=63, mat_num=32,
            # len(mats)=11, len(mat_order)=32.
            info['bones_num'] = len(base['bones'])
            info['vbuff_num'] = len(base_vertex_data)
            info['ibuff_num'] = len(base_index_data)
            info['mat_num']   = len(base_mat_order)
            info['skin_binds_num'] = len(base.get('skin_binds', []))

            log(f"    weapon[{w_idx}] '{wmesh}': merged "
                f"+{slots_added} mesh slots at lod0[{insert_at}], "
                f"+{len(wp_vertex_data)} vbufs, +{len(wp_index_data)} ibufs, "
                f"+{len(wp_mats)} mats, "
                f"+{len(newly_appended_bone_indices)} bones, "
                f"injected_skin={sum(1 for x in wp_vd_was_static if x)}/{len(wp_vd_was_static)}")

        # ── Save merged JSON ───────────────────────────────────────────
        try:
            with open(out_path, 'w', encoding='utf-8') as f:
                json.dump(base, f, indent=2)
            log(f"  [{entry_idx}] Wrote {out_path}")
            new_names_added.append(new_name)
            # Register in objas so lotrc_rs allocates enough BIN space.
            try:
                _register_new_model_in_objas(dump_dir, new_name, base_name)
            except Exception as _e:
                log(f"  [{entry_idx}] WARN objas register threw: {_e}")
            # Same "make the goddamn mesh visible" courtesy the embed
            # branch above does. Legacy new-slot path or embed, same
            # rule: bake without spawn = invisible mesh = angry user.
            if auto_spawn:
                _append_crowd_spawn(dump_dir, new_name, base_name,
                                    spawn_pos, spawn_count,
                                    primary_attach_bone, weapons)
        except Exception as e:
            log(f"  [{entry_idx}] ERROR: failed to write {out_path}: {e}")

    # ── Register new model names in pak_strings.json ────────────────────
    if new_names_added:
        strings_path = os.path.join(dump_dir, 'pak_strings.json')
        if os.path.isfile(strings_path):
            try:
                with open(strings_path, 'r', encoding='utf-8') as f:
                    strings = json.load(f)
                existing_cf = set(s.casefold() for s in strings if isinstance(s, str))
                added = 0
                for nn in new_names_added:
                    if nn.casefold() not in existing_cf:
                        strings.append(nn)
                        existing_cf.add(nn.casefold())
                        added += 1
                if added > 0:
                    with open(strings_path, 'w', encoding='utf-8') as f:
                        json.dump(strings, f, indent=2)
                    log(f"  --merged-models: added {added} new name(s) to pak_strings.json")
                else:
                    log(f"  --merged-models: all new names already in pak_strings.json")
            except Exception as e:
                log(f"  --merged-models: failed to update pak_strings.json: {e}")
        else:
            log(f"  --merged-models: pak_strings.json not found at {strings_path}; skipping registration")


def _mat_from_json_offset(xyz, ypr):
    """Build the local offset matrix the user dialed in on the panel:
    T(xyz) * Ry(yaw) * Rx(pitch) * Rz(roll). Wrapper for _mat_from_ypr_xyz
    that defends against missing/bad input lists."""
    try:
        xyz = [float(xyz[0]), float(xyz[1]), float(xyz[2])]
    except Exception:
        xyz = [0.0, 0.0, 0.0]
    try:
        ypr = [float(ypr[0]), float(ypr[1]), float(ypr[2])]
    except Exception:
        ypr = [0.0, 0.0, 0.0]
    return _mat_from_ypr_xyz(xyz, ypr)


def main():
    # ── Standalone adjust-weapon mode ─────────────────────────────────
    # Bypasses the full PAK pipeline. Just moves an embedded weapon's
    # verts inside a baked model JSON by a bone-local delta. User runs
    # lotrc_rs -c separately afterward to repack the PAK.
    #
    # Usage:
    #   level_patcher.py --adjust-weapon <dump_dir> <model_name> <weapon_idx>
    #                    <dx> <dy> <dz> <dyaw_deg> <dpitch_deg> <droll_deg>
    # ── Retro-tag a pre-existing merged CRD ────────────────────────────
    # Bridges the gap for merges baked before the M2 sidecar hook landed.
    # Reads the merged + base CRDs off disk, diffs slot[0] to recover
    # vert_first / vert_count / tri_first / tri_count, resolves the
    # attach bone in skin_order, inverts the stored inverse-bind to get
    # bind_world, writes a fresh entry into ze_embedded_weapons.json.
    # After this the Adjust Baked Weapon panel sees the model.
    #
    # Usage:
    #   level_patcher.py --retro-tag-weapon <dump_dir>
    #                    <merged_name> <base_name>
    #                    <weapon_name> <attach_bone>
    if '--retro-tag-weapon' in sys.argv:
        idx = sys.argv.index('--retro-tag-weapon')
        rest = sys.argv[idx + 1:]
        if len(rest) < 5:
            print("Usage: level_patcher.py --retro-tag-weapon <dump_dir> "
                  "<merged_name> <base_name> <weapon_name> <attach_bone>")
            sys.exit(1)
        try:
            retro_tag_baked_weapon(rest[0], rest[1], rest[2], rest[3], rest[4])
            print(f"OK: retro-tagged '{rest[1]}' in {rest[0]}")
            sys.exit(0)
        except Exception as e:
            print(f"ERROR: {e}")
            import traceback; traceback.print_exc()
            sys.exit(2)

    if '--adjust-weapon' in sys.argv:
        idx = sys.argv.index('--adjust-weapon')
        rest = sys.argv[idx + 1:]
        if len(rest) < 9:
            print("Usage: level_patcher.py --adjust-weapon <dump_dir> "
                  "<model_name> <weapon_idx> <dx> <dy> <dz> "
                  "<dyaw_deg> <dpitch_deg> <droll_deg>")
            sys.exit(1)
        dump_dir   = rest[0]
        model_name = rest[1]
        weapon_idx = int(rest[2])
        dxyz       = [float(rest[3]), float(rest[4]), float(rest[5])]
        dypr       = [float(rest[6]), float(rest[7]), float(rest[8])]
        try:
            n = apply_weapon_adjustment(dump_dir, model_name, weapon_idx, dxyz, dypr)
            print(f"OK: moved {n} verts")
            sys.exit(0)
        except Exception as e:
            print(f"ERROR: {e}")
            import traceback; traceback.print_exc()
            sys.exit(2)

    if len(sys.argv) < 5:
        print("Usage: level_patcher.py <original_pak> <entities_json> <lotrc_exe> <output_pak> [--strings str1 str2 ...]")
        sys.exit(1)

    # Clear progress file from previous run
    try:
        open("level_patcher_progress.txt", "w").close()
    except:
        pass

    original_pak = sys.argv[1]
    entities_json_path = sys.argv[2]
    lotrc_exe = sys.argv[3]
    output_pak = sys.argv[4]

    # Parse --strings (kept as fallback, but scan_all_strings is primary now)
    extra_strings = []
    if '--strings' in sys.argv:
        idx = sys.argv.index('--strings')
        # Collect strings until we hit another flag
        for s in sys.argv[idx + 1:]:
            if s.startswith('--'):
                break
            extra_strings.append(s)

    # Parse --deleted <path>
    deleted_guids_path = None
    if '--deleted' in sys.argv:
        idx = sys.argv.index('--deleted')
        if idx + 1 < len(sys.argv):
            deleted_guids_path = sys.argv[idx + 1]

    # Parse --edits <path>
    field_edits_path = None
    if '--edits' in sys.argv:
        idx = sys.argv.index('--edits')
        if idx + 1 < len(sys.argv):
            field_edits_path = sys.argv[idx + 1]

    # Parse --crowd <path> — full overwrite for sub_blocks2/3dcrowd.json.
    # The viewer dumps its mutated m_crowdItems here as the SAME JSON shape
    # lotrc_rs -d emits, so we just copy it over the post-dump file before
    # the -c repack reads it back. No merge logic: the viewer's vector is
    # the source of truth once the user has touched a crowd member.
    crowd_json_path = None
    if '--crowd' in sys.argv:
        idx = sys.argv.index('--crowd')
        if idx + 1 < len(sys.argv):
            crowd_json_path = sys.argv[idx + 1]

    # Parse --merged-models <path> — sidecar produced by Vespucci's "Crowd
    # Mesh Builder" panel. Each entry bakes a new merged character+weapon
    # model JSON into the dump dir's models/ folder before lotrc_rs -c
    # repacks. See apply_merged_models() above for the schema + algorithm.
    merged_models_path = None
    if '--merged-models' in sys.argv:
        idx = sys.argv.index('--merged-models')
        if idx + 1 < len(sys.argv):
            merged_models_path = sys.argv[idx + 1]

    # Parse --strip-weapons <path> — sidecar produced by Vespucci's "Strip
    # Baked Weapon" UI. Each entry strips vertices weighted to specified
    # bones from a CRD model and writes a new <crd>_stripped.json into the
    # dump dir's models/ folder. See apply_strip_specs() for the algorithm.
    strip_specs_path = None
    if '--strip-weapons' in sys.argv:
        idx = sys.argv.index('--strip-weapons')
        if idx + 1 < len(sys.argv):
            strip_specs_path = sys.argv[idx + 1]

    # Parse --xforms <path> — sidecar produced by the Move Triangles panel's
    # Apply button. Lives next to the PAK (Vespucci writes it via
    # LevelReader::GetDumpDir which is "pak path minus .PAK"). The patcher
    # unpacks the PAK to a TEMP dir, so it would never see the file unless
    # we hand it the path explicitly. apply_embedded_weapon_xforms() reads
    # vert-list / bone-keyed / weapon-idx entries and commits them to the
    # corresponding model JSONs in dump_level_dir before lotrc_rs -c repacks.
    xforms_path_arg = None
    if '--xforms' in sys.argv:
        idx = sys.argv.index('--xforms')
        if idx + 1 < len(sys.argv):
            xforms_path_arg = sys.argv[idx + 1]

    # Validate inputs
    if not os.path.exists(original_pak):
        log(f"ERROR: PAK not found: {original_pak}")
        sys.exit(1)
    if not os.path.exists(entities_json_path):
        log(f"ERROR: entities JSON not found: {entities_json_path}")
        sys.exit(1)
    if not os.path.exists(lotrc_exe):
        log(f"ERROR: lotrc_rs.exe not found: {lotrc_exe}")
        sys.exit(1)

    # Create temp directories next to the PAK
    pak_dir = os.path.dirname(os.path.abspath(original_pak))
    temp_dump = os.path.join(pak_dir, "ze_lvl_dump")
    temp_out = os.path.join(pak_dir, "ze_lvl_out")

    # Clean previous
    for d in [temp_dump, temp_out]:
        if os.path.exists(d):
            shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d, exist_ok=True)

    try:
        # ── Step 1: Dump original PAK ──
        log("Step 1: Dumping original PAK...")
        cmd = [lotrc_exe, original_pak, "-o", temp_dump]
        log(f"  Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            log(f"ERROR: lotrc_rs dump failed (exit {result.returncode})")
            log(f"  stderr: {result.stderr[:2000]}")
            sys.exit(1)
        log("  Dump OK")

        # Find the dump subdirectory (lotrc_rs creates <name>/ subfolder)
        dump_level_dir = None
        for entry in os.listdir(temp_dump):
            candidate = os.path.join(temp_dump, entry)
            if os.path.isdir(candidate) and os.path.exists(os.path.join(candidate, "pak_header.json")):
                dump_level_dir = candidate
                break
        if not dump_level_dir:
            if os.path.exists(os.path.join(temp_dump, "pak_header.json")):
                dump_level_dir = temp_dump
        if not dump_level_dir:
            log(f"ERROR: No pak_header.json found in dump output")
            log(f"  Contents of {temp_dump}: {os.listdir(temp_dump)}")
            sys.exit(1)
        log(f"  Dump dir: {dump_level_dir}")

        # ── Step 2: Find and patch level.json ──
        log("Step 2: Patching level.json...")
        level_path = None
        for sub in ['sub_blocks1', 'sub_blocks0']:
            candidate = os.path.join(dump_level_dir, sub, 'level.json')
            if os.path.exists(candidate):
                level_path = candidate
                break
        if not level_path:
            log("ERROR: level.json not found in dump")
            sys.exit(1)

        # Load entities to add
        with open(entities_json_path, 'r', encoding='utf-8') as f:
            new_entities = json.load(f)

        # Load level.json
        with open(level_path, 'r', encoding='utf-8') as f:
            data = json.load(f)

        # ── Step 2a: Remove deleted entities (from wipe) ──
        deleted_guids = set()
        if deleted_guids_path and os.path.exists(deleted_guids_path):
            with open(deleted_guids_path, 'r') as f:
                deleted_guids = set(json.load(f))
            if deleted_guids:
                before_del = len(data['objs'])
                data['objs'] = [o for o in data['objs'] if o.get('fields', {}).get('GUID', 0) not in deleted_guids]
                after_del = len(data['objs'])
                log(f"  Deleted {before_del - after_del} entities ({len(deleted_guids)} GUIDs requested)")

        objs_before = len(data['objs'])
        for ent in new_entities:
            data['objs'].append(ent)
        log(f"  objs: {objs_before} -> {len(data['objs'])} (+{len(new_entities)})")

        # ── Step 2b: Import missing type definitions from reference levels ──
        existing_types = set(t['name'] for t in data.get('types', []))
        needed_types = set(ent.get('type', '') for ent in new_entities) - existing_types
        needed_types.discard('')
        if needed_types:
            log(f"  Missing types: {needed_types}")
            lotrc_dir = os.path.dirname(os.path.abspath(lotrc_exe))
            ref_maps = [
                "BlackGates", "MinasTirith", "Helm'sDeep", "Weathertop",
                "Isengard", "Moria", "PelennorFields", "Mount_Doom",
                "Osgiliath", "MinasTirith_Top", "Rivendell", "Shire",
                "CoriCelesti", "Minas_Morgul", "Training",
            ]
            imported = 0
            for type_name in list(needed_types):
                for ref_map in ref_maps:
                    ref_level = os.path.join(lotrc_dir, ref_map, "sub_blocks1", "level.json")
                    if not os.path.exists(ref_level):
                        continue
                    try:
                        with open(ref_level, 'r', encoding='utf-8') as rf:
                            ref_data = json.load(rf)
                        for ref_type in ref_data.get('types', []):
                            if ref_type.get('name') == type_name:
                                data['types'].append(ref_type)
                                imported += 1
                                log(f"  Imported type '{type_name}' from {ref_map}")
                                needed_types.discard(type_name)
                                break
                    except Exception:
                        continue
                    if type_name not in needed_types:
                        break
            if imported > 0:
                log(f"  Imported {imported} missing type definitions")
            if needed_types:
                log(f"  WARNING: Could not find types: {needed_types}")

        # ── Step 2c: Apply field edits from properties panel ──
        if field_edits_path and os.path.exists(field_edits_path):
            with open(field_edits_path, 'r') as f:
                field_edits = json.load(f)
            if field_edits:
                # Build GUID→obj index map
                guid_to_idx = {}
                for i, obj in enumerate(data['objs']):
                    g = obj.get('fields', {}).get('GUID', 0)
                    if g:
                        guid_to_idx[g] = i

                # Build type→field→kind map for CRC/string resolution
                type_field_kinds = {}
                for ty in data.get('types', []):
                    fmap = {}
                    for fd in ty.get('fields', []):
                        fmap[fd['name'].lower()] = fd['type']
                    type_field_kinds[ty['name']] = fmap

                applied = 0
                for edit in field_edits:
                    guid = edit.get('guid', 0)
                    field = edit.get('field', '')
                    kind = edit.get('kind', -1)
                    value = edit.get('value')
                    idx = guid_to_idx.get(guid)
                    if idx is None or not field:
                        continue

                    obj = data['objs'][idx]
                    fields = obj.get('fields', {})

                    # Find the field (case-insensitive)
                    target_key = None
                    for k in fields:
                        if k.lower() == field.lower():
                            target_key = k
                            break
                    if target_key is None:
                        target_key = field  # add new field

                    if kind == 5:
                        # String/CRC — write as string directly
                        fields[target_key] = edit.get('value', '')
                    elif kind == 6:
                        # List item edit
                        li = edit.get('listIndex', 0)
                        if isinstance(fields.get(target_key), list) and li < len(fields[target_key]):
                            fields[target_key][li] = edit.get('value', 0)
                    elif kind == 0:
                        # Int — check if field is bool type
                        obj_type = obj.get('type', '')
                        fkind = type_field_kinds.get(obj_type, {}).get(field.lower(), '')
                        if fkind == 'bool':
                            fields[target_key] = bool(value)
                        else:
                            fields[target_key] = value
                    elif kind in (1,):
                        # Float
                        fields[target_key] = value
                    elif kind in (2,):
                        # GUID
                        fields[target_key] = value
                    elif kind == 3:
                        # Vec3
                        fields[target_key] = value
                    elif kind == 4:
                        # Matrix
                        fields[target_key] = value
                    elif kind == 7:
                        # Float array (spline nodes, etc.) — value is array of sub-arrays
                        fields[target_key] = value
                    elif kind == 8:
                        # List append — grow an objectlist by one. Value is a single GUID
                        # to append to the list. Used by the Chain editor when there's no
                        # zero slot available in the source's Outputs[] list.
                        lst = fields.get(target_key)
                        if isinstance(lst, list):
                            lst.append(value)
                        else:
                            # Initialize as list if field wasn't already one
                            fields[target_key] = [value]
                    else:
                        fields[target_key] = value

                    applied += 1

                log(f"  Applied {applied} field edits ({len(field_edits)} total)")

        # Write back
        with open(level_path, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2)

        # ── Step 3: Update pak_strings.json (CoreScripts full scan) ──
        log("Step 3: Scanning all assets for strings...")
        strings_path = os.path.join(dump_level_dir, 'pak_strings.json')
        if os.path.exists(strings_path):
            with open(strings_path, 'r', encoding='utf-8') as f:
                strings = json.load(f)
            existing = set(s.casefold() for s in strings)

            # Full asset scan (CoreScripts approach)
            all_strings = scan_all_strings(dump_level_dir, data)

            # Also include --strings from command line as fallback
            for s in extra_strings:
                if s:
                    all_strings.add(s)

            added = 0
            for s in sorted(all_strings):
                if s.casefold() not in existing:
                    strings.append(s)
                    existing.add(s.casefold())
                    added += 1

            if added > 0:
                with open(strings_path, 'w', encoding='utf-8') as f:
                    json.dump(strings, f, indent=2)
                log(f"  Added {added} new strings to pak_strings.json (scan found {len(all_strings)} total)")
            else:
                log(f"  All strings already present ({len(all_strings)} scanned)")

        # ── Step 3b: Add animation block entries for new gamemodes ──
        anim_blocks_path = os.path.join(dump_level_dir, 'animation_block_infos.json')
        if os.path.exists(anim_blocks_path):
            with open(anim_blocks_path, 'r', encoding='utf-8') as f:
                anim_blocks = json.load(f)

            # Find existing gamemode keys
            existing_keys = set(b.get('key', '') for b in anim_blocks)

            # Check new entities for gamemode types with a Mode field
            new_gamemodes = []
            for ent in new_entities:
                if ent.get('type') == 'gamemode':
                    mode_key = ent.get('fields', {}).get('Mode', '')
                    mode_name = ent.get('fields', {}).get('ModeName', '')
                    gm_guid = ent.get('fields', {}).get('GUID', 0)
                    if mode_key and mode_key not in existing_keys:
                        new_gamemodes.append((mode_key, mode_name, gm_guid))

            if new_gamemodes:
                # Clone the last existing block as template (for offset/size/compression fields)
                # The Rust parser will recalculate these, but they need valid structure
                template = anim_blocks[-1] if anim_blocks else {
                    "offset": 0, "size": 0, "size_comp": 0,
                    "unk_6": 0, "unk_7": 0, "unk_8": 0
                }
                new_bits = []
                for mode_key, mode_name, gm_guid in new_gamemodes:
                    new_block = {
                        "key": mode_key,
                        "guid": gm_guid,
                        "key_name": mode_name,
                        "offset": template.get("offset", 0),
                        "size": template.get("size", 0),
                        "size_comp": template.get("size_comp", 0),
                        "unk_6": 0,
                        "unk_7": 0,
                        "unk_8": 0
                    }
                    anim_blocks.append(new_block)
                    block_index = len(anim_blocks) - 1
                    bit_value = 1 << block_index
                    new_bits.append(bit_value)
                    log(f"  Added animation block: key={mode_key} key_name={mode_name} guid={gm_guid} (bit {block_index} = {bit_value})")

                with open(anim_blocks_path, 'w', encoding='utf-8') as f:
                    json.dump(anim_blocks, f, indent=2)

                # Extend asset gamemodemasks so animations/models/effects/textures
                # are packed into the new block (prevents empty spline crash)
                log("  Extending asset gamemodemasks for new block(s)...")
                # Import AnimationBlockConverter from CoreScripts
                lotrc_dir = os.path.dirname(os.path.abspath(lotrc_exe))
                converter_path = os.path.join(lotrc_dir, 'CoreScripts', 'AnimationBlockConverter.py')
                if os.path.exists(converter_path):
                    import importlib.util
                    spec = importlib.util.spec_from_file_location("AnimationBlockConverter", converter_path)
                    abc = importlib.util.module_from_spec(spec)
                    spec.loader.exec_module(abc)
                    for bit_val in new_bits:
                        stats = abc.convert(dump_level_dir, bit_val)
                        total = sum(stats.values())
                        log(f"  Extended {total} gamemodemask values for bit {bit_val}")
                        for k, v in stats.items():
                            if v > 0:
                                log(f"    {k}: {v}")
                else:
                    log(f"  WARNING: AnimationBlockConverter.py not found at {converter_path}")
                    log(f"  Assets will NOT have the new gamemode bit — game may crash!")

        # ── Step 3c: Overlay 3dCrowd edits from the viewer ──
        # The viewer's LevelScene::dumpCrowdAsJson() emits the same JSON
        # shape lotrc_rs -d produces. Just clobber the post-dump file with
        # the user's mutated version — no diff, no merge. Crowds have no
        # GUID identity so a per-instance diff would buy us nothing.
        if crowd_json_path and os.path.exists(crowd_json_path):
            target_crowd = None
            for sub in ['sub_blocks2', 'sub_blocks1', 'sub_blocks0']:
                cand = os.path.join(dump_level_dir, sub, '3dcrowd.json')
                if os.path.exists(cand):
                    target_crowd = cand
                    break
            if target_crowd:
                shutil.copy2(crowd_json_path, target_crowd)
                log(f"  Overlayed 3dcrowd.json from {crowd_json_path}")
            else:
                # The level had no crowd block originally. lotrc_rs won't
                # pick up a stray file we drop in — without a matching key
                # in pak_header / subblock index, it'll be ignored. Log
                # and move on. (Adding a NEW 3dCrowd block to a level that
                # didn't have one needs more plumbing than just dropping
                # this file — punt.)
                log(f"  WARNING: --crowd given but no 3dcrowd.json in dump (level had no crowd block, edits dropped)")

        # ── Step 3d: Bake merged character+weapon meshes ─────────────────
        # Crowd Mesh Builder hands us a list of {new_name, base, weapons[]}
        # entries. Each writes a brand-new models/<new_name>.json into the
        # dump dir and registers the name in pak_strings. The original base
        # JSON is left untouched. Runs AFTER 3dcrowd overlay so the new
        # mesh names are available the same save cycle the user picks them.
        # Strip-weapon step runs BEFORE merge so a stripped CRD can become
        # the merge base in the same save cycle (strip sword off elf CRD,
        # then merge bow onto the stripped version). If both sidecars are
        # present and reference the same source name, the merge step picks
        # up the freshly-written stripped JSON via filesystem read.
        if strip_specs_path and os.path.isfile(strip_specs_path):
            log("Step 3c: Stripping baked weapons from CRDs...")
            try:
                apply_strip_specs(dump_level_dir, strip_specs_path)
            except Exception as e:
                log(f"  ERROR in apply_strip_specs: {e}")
                import traceback
                log(traceback.format_exc())

        if merged_models_path and os.path.isfile(merged_models_path):
            log("Step 3d: Baking merged crowd meshes...")
            try:
                apply_merged_models(dump_level_dir, merged_models_path)
            except Exception as e:
                log(f"  ERROR in apply_merged_models: {e}")
                import traceback
                log(traceback.format_exc())

        # Step 3e: Adjust Baked Weapon / Move Triangles deltas.
        #
        # The Apply button writes ze_embedded_weapon_xforms.json next to
        # the PAK (LevelReader's "dump dir" = pak path minus .PAK), NOT
        # inside our temp unpack dir. So the host passes the absolute path
        # to that file via --xforms. We use that explicit path if given;
        # otherwise we still look inside dump_level_dir as a courtesy
        # for users running level_patcher.py by hand.
        xforms_path = None
        if xforms_path_arg and os.path.isfile(xforms_path_arg):
            xforms_path = xforms_path_arg
        else:
            cand = os.path.join(dump_level_dir, 'ze_embedded_weapon_xforms.json')
            if os.path.isfile(cand):
                xforms_path = cand
        if xforms_path:
            log(f"Step 3e: Committing Move Triangles / Adjust Baked Weapon deltas from {xforms_path}...")
            try:
                apply_embedded_weapon_xforms(dump_level_dir, xforms_path)
            except Exception as e:
                log(f"  ERROR in apply_embedded_weapon_xforms: {e}")
                import traceback
                log(traceback.format_exc())

        # ── Step 4: Compile back to PAK ──
        log("Step 4: Compiling back to PAK...")
        cmd = [lotrc_exe, dump_level_dir, "-o", temp_out]
        log(f"  Running: {' '.join(cmd)}")
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            log(f"ERROR: lotrc_rs compile failed (exit {result.returncode})")
            log(f"  stderr: {result.stderr[:2000]}")
            sys.exit(1)
        log("  Compile OK")

        # ── Step 5: Find output files and copy ──
        log("Step 5: Copying output files...")
        out_pak = None
        out_bin = None
        for root, dirs, files in os.walk(temp_out):
            for fn in files:
                if fn.upper().endswith('.PAK'):
                    out_pak = os.path.join(root, fn)
                elif fn.upper().endswith('.BIN'):
                    out_bin = os.path.join(root, fn)

        if out_pak:
            shutil.copy2(out_pak, output_pak)
            log(f"  Copied PAK to {output_pak}")
        else:
            log("ERROR: No .PAK file in compile output")
            sys.exit(1)

        if out_bin:
            # Derive BIN path from PAK path
            bin_path = output_pak.replace('.PAK', '.BIN').replace('.pak', '.bin')
            shutil.copy2(out_bin, bin_path)
            log(f"  Copied BIN to {bin_path}")

        log("DONE: Save complete!")

    finally:
        # Cleanup
        for d in [temp_dump, temp_out]:
            if os.path.exists(d):
                shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    main()
