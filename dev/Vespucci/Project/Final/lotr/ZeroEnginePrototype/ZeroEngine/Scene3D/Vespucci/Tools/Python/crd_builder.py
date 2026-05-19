#!/usr/bin/env python3
"""
crd_builder.py — Convert a hero character model JSON into a CRD-formatted
model the engine's crowd render path will accept.

Phase A (this file): structural re-encode. Takes CH_*.json, produces CRD_*.json
with single-stream URK-pattern vertex layout, CrowdModel asset_type, crowd-class
mat flags, empty slots/slot_map. Bones and skin_binds unchanged.

Usage:
    python crd_builder.py <input_char.json> <output_crd.json> [--name NEW_NAME]

Phase D (later, separate function): inject a weapon mesh as an additional slot
weighted to a hand bone, without adding new bones to the palette.
"""

import json
import sys
import os
import argparse

# URK CRD reference values. These are the structural constants that the shipping
# crowd render path expects. Every field we touch is set to match URK's pattern.
URK_FMT1                  = 0x80C07          # Pos + BlendWt + BlendIdx + Normal + UV
URK_VSIZE                 = 32               # bytes per vertex
CROWD_ASSET_TYPE          = 2936587374       # URK CRD asset_type hash
CROWD_MAT_FLAGS_BODY      = 0x0002000000420000
CROWD_MAT_FLAGS_PHYS      = 0x0002002000400000

# Elf CRD pattern — observed in CRD_CH_elf_ancn_swd_all_01. Same render path,
# different vertex format. Adds Color(1) stream (default white = 0xFFFFFFFF
# per vertex) and bumps v_size to 36. Mat flags carry the +0x40 low-byte
# suffix which likely toggles a shader feature (AO sample? extra UV?).
ELF_FMT1                  = 0x80E07          # URK_FMT1 + Color(1) bit (0x200)
ELF_VSIZE                 = 36               # 32 + 4 for Color(1) Unorm4x8
ELF_MAT_FLAGS_BODY        = 0x0002000000420040
ELF_MAT_FLAGS_PHYS        = 0x0002002000400040
ELF_COLOR_DEFAULT         = 0xFFFFFFFF       # white, full alpha

# fmt1 bit definitions (decoded from comparing URK / elf CRD / Haldir vertex_data):
FMT1_POSITION             = 0x00001
FMT1_NORMAL               = 0x00002
FMT1_TANGENT              = 0x00040    # dropped in CRD conversion
FMT1_COLOR                = 0x00200    # dropped in CRD conversion
FMT1_BLEND_WEIGHTS        = 0x00400
FMT1_BLEND_INDICES        = 0x00800
FMT1_COMPACT_MARKER       = 0x80000    # URK has it. Significance unclear; we set it.

FMT1_RENDERABLE_MASK      = FMT1_NORMAL | FMT1_TANGENT | FMT1_COLOR
FMT1_PHYSICS_BITS         = FMT1_POSITION | FMT1_BLEND_WEIGHTS | FMT1_BLEND_INDICES


def log(msg):
    print(f"[crd_builder] {msg}", flush=True)


def is_renderable_vertex_data(vd):
    """A vertex_data entry is 'renderable' if it has Normal+Tangent — i.e. the engine
    intends to light it. Physics-only entries (Pos+BlendWt+BlendIdx, maybe UV) skip
    lighting and don't need reformatting since they never go through the crowd shader."""
    fmt1 = int(vd.get('info', {}).get('fmt1', 0))
    return (fmt1 & FMT1_NORMAL) != 0


def convert_renderable_to_urk_stream(vd, pattern='urk', keep_full_streams=False):
    """Re-encode a full-format vertex_data entry to either URK or elf pattern.

    'urk' (default): drop Color + Tangent, fmt1=0x80C07, v_size=32.
    'elf':           drop Tangent only, keep/synthesize Color(1) at 0xFFFFFFFF,
                     fmt1=0x80E07, v_size=36.
    keep_full_streams=True: preserve all streams the source has (Color, Tangent
                     etc). For testing with character render path which needs
                     Tangent for normal-mapped lighting — without it the lighting
                     calc produces near-black/silver output."""
    data = vd['data']

    pos = data['Position']['val']['Vector3']
    n_verts = len(pos[0]) if pos and pos[0] else 0

    def get_stream_or_zero(name, stream_type, n):
        d = data.get(name)
        if d and 'val' in d and stream_type in d['val']:
            return d['val'][stream_type]
        if stream_type == 'Unorm4x8':
            return [0] * n
        if stream_type == 'Vector2':
            return [[0.0] * n, [0.0] * n]
        if stream_type == 'Vector3':
            return [[0.0] * n, [0.0] * n, [0.0] * n]
        return None

    new_data = {
        'Position':        {'val': {'Vector3':  pos}},
        'BlendWeight':     {'val': {'Unorm4x8': get_stream_or_zero('BlendWeight',  'Unorm4x8', n_verts)}},
        'BlendIndices':    {'val': {'Unorm4x8': get_stream_or_zero('BlendIndices', 'Unorm4x8', n_verts)}},
        'Normal':          {'val': {'Unorm4x8': get_stream_or_zero('Normal',       'Unorm4x8', n_verts)}},
        'TextureCoord(0)': {'val': {'Vector2':  get_stream_or_zero('TextureCoord(0)', 'Vector2', n_verts)}},
    }

    new_info = dict(vd['info'])

    if keep_full_streams:
        # Preserve everything the source has — Color, Tangent, anything else.
        for name in ('Color(1)', 'Tangent'):
            if name in data:
                new_data[name] = data[name]
        # Use the source's original fmt1 (preserves all stream bits) and recompute
        # size from the original data's size field.
        new_info['fmt1'] = vd['info']['fmt1']
        new_info['size'] = vd['info']['size']
    elif pattern == 'elf':
        existing_color = data.get('Color(1)', {}).get('val', {}).get('Unorm4x8')
        if existing_color and len(existing_color) == n_verts:
            color_stream = existing_color
        else:
            color_stream = [ELF_COLOR_DEFAULT] * n_verts
        new_data['Color(1)'] = {'val': {'Unorm4x8': color_stream}}
        new_info['fmt1'] = ELF_FMT1
        new_info['size'] = n_verts * ELF_VSIZE
    else:
        new_info['fmt1'] = URK_FMT1
        new_info['size'] = n_verts * URK_VSIZE

    return {'info': new_info, 'data': new_data}, n_verts


def update_buffer_info_to_single_stream(bi, new_v_size, new_vert_count):
    """Set buffer_info to URK single-stream pattern: only the primary stream is
    used, _2 and _3 marked unused (0xFFFFFFFF). v_size and vbuff_size updated."""
    bi['v_size']             = new_v_size
    bi['v_size_2']           = 0
    bi['v_size_3']           = 0
    bi['vbuff_size']         = new_v_size * new_vert_count
    bi['vbuff_size_2']       = 0
    bi['vbuff_size_3']       = 0
    # Keep primary vbuff_info_offset pointing at the same vertex_data entry;
    # mark the secondary/tertiary as UNUSED (-1 as u32).
    bi['vbuff_info_offset_2'] = 0xFFFFFFFF
    bi['vbuff_info_offset_3'] = 0xFFFFFFFF


def classify_mat_flag(old_flag, body_flag=CROWD_MAT_FLAGS_BODY,
                      phys_flag=CROWD_MAT_FLAGS_PHYS):
    """Decide whether a material is body-render or physics-only based on the
    physics bit (0x0000002000000000 = bit 49 in URK's physics flag).
    Returns the crowd-pattern replacement flag for the chosen target pattern."""
    is_phys = (old_flag & 0x0000002000000000) != 0
    return phys_flag if is_phys else body_flag


# ── Phase B: Skeleton alignment ─────────────────────────────────────────
# URK CRD ships with a specific 63-bone humanoid layout. The shipping crowd
# animations target bone names that the engine resolves to indices within
# THIS model's bone palette. But because some crowd-render paths and shaders
# may use cached/hardcoded indices keyed to URK's bone order, hero models like
# Haldir need their skeletons aligned to URK's layout exactly — same names in
# same slots — for animations to drive the right vertices.
#
# Two differences between Haldir and URK skeletons:
#   1. Haldir has 2 extra Mount_* bones at original indices 2 and 10. These
#      are riding-mount attach points used by hero gameplay code, not in CRD.
#   2. Haldir is MISSING URK's Bone_eye_L (orig URK [30]) and Bone_eye_R (orig
#      URK [31]) — small bones parented to Bone_Head, zero-bbox.
# Net difference: same 63 bone count but layout diverges between indices 2–31.
#
# Phase B removes the 2 Mount bones and inserts 2 placeholder eye bones at the
# URK positions, then renumbers bone_parents and skin_order references through
# the old→new index map.

MOUNT_BONE_NAMES = {'Mount_Bone_GlobalSRT', 'Mount_Bone_Lumbar3'}

# Templates for the eye bones (copied verbatim from URK CRD bone_transforms[30]
# and [31]). Transforms are 3x4 matrices stored column-major as x/y/z/w dicts.
# Parent is Bone_Head.
EYE_L_TRANSFORM = {
    'x': {'x': -0.0062141456, 'y':  0.99026805,   'z':  0.13903435,   'w': 0.0},
    'y': {'x':  0.9999805,    'y':  0.006070723,  'z':  0.001455622,  'w': 0.0},
    'z': {'x':  0.000597417,  'y':  0.13904068,   'z': -0.99028647,   'w': 0.0},
    'w': {'x':  0.081388,     'y':  0.032338,     'z': -0.116541,     'w': 1.0},
}
EYE_R_TRANSFORM = {
    'x': {'x':  0.0062141144, 'y':  0.99026805,   'z': -0.13903435,   'w': 0.0},
    'y': {'x':  0.9999805,    'y': -0.006070691,  'z':  0.001455622,  'w': 0.0},
    'z': {'x':  0.00059742143,'y': -0.13904068,   'z': -0.99028647,   'w': 0.0},
    'w': {'x':  0.083228,     'y': -0.033604,     'z': -0.11642,      'w': 1.0},
}
# Quaternion forms for hk_constraint.bone_transforms (URK's actual values).
# All sub-fields are homogeneous 4-vectors {x,y,z,w}. Translation w=1 (point),
# rotation w is the scalar part of the quaternion, scale w=0.
EYE_L_HK_TRANSFORM = {
    'rotation':    {'x': 0.70320725, 'y': 0.70756125, 'z': 0.049641054, 'w': -0.04891342},
    'translation': {'x': 0.081388,   'y': 0.032338,   'z': -0.116541,   'w': 1.0},
    'scale':       {'x': 1.0,        'y': 1.0,        'z': 1.0,         'w': 0.0},
}
EYE_R_HK_TRANSFORM = {
    'rotation':    {'x': 0.70756125, 'y': 0.70320725, 'z': -0.048913416,'w': 0.049641054},
    'translation': {'x': 0.083228,   'y': -0.033604,  'z': -0.11642,    'w': 1.0},
    'scale':       {'x': 1.0,        'y': 1.0,        'z': 1.0,         'w': 0.0},
}
ZERO_BBOX = {
    'center':     {'x': 0.0, 'y': 0.0, 'z': 0.0},
    'unk_3':       0.0,
    'half_width': {'x': 0.0, 'y': 0.0, 'z': 0.0},
    'unk_7':       0.0,
}


def align_skeleton_to_urk(char_json):
    """Drop Mount_* hero-only bones, insert placeholder eye bones at URK
    positions, and remap bone_parents + skin_order through the index shift."""
    bones      = char_json['bones']
    parents    = char_json['bone_parents']
    transforms = char_json['bone_transforms']
    bboxes     = char_json['bone_bounding_boxes']
    skin_order = char_json['skin_order']
    info       = char_json['info']

    # ── Step 1: identify bones to remove (Mount_*) ────────────────────
    to_remove = [i for i, b in enumerate(bones) if b in MOUNT_BONE_NAMES]
    log(f"  Mount bones to remove: {[(i, bones[i]) for i in to_remove]}")

    # Sanity: confirm they have no children and no skin_order references.
    for ri in to_remove:
        children = [i for i, p in enumerate(parents) if p == ri]
        if children:
            log(f"  ABORT Phase B: bone[{ri}] ({bones[ri]}) has children: {children}")
            return
        skin_refs = [bi for bi, bidx in enumerate(skin_order) if bidx == ri]
        if skin_refs:
            log(f"  ABORT Phase B: bone[{ri}] ({bones[ri]}) is referenced by skin_binds: {skin_refs}")
            return

    # ── Step 2: build old→new index map for the Mount-removal pass ────
    # Bones not removed keep their order; their new indices shift down by 1 for
    # each removed bone earlier in the list.
    old_to_post_mount = {}
    new_idx = 0
    keep_indices = []
    for old_idx in range(len(bones)):
        if old_idx in to_remove:
            continue
        old_to_post_mount[old_idx] = new_idx
        keep_indices.append(old_idx)
        new_idx += 1

    # ── Step 3: build the kept arrays after Mount removal ─────────────
    new_bones      = [bones[i]      for i in keep_indices]
    new_parents    = [parents[i]    for i in keep_indices]
    new_transforms = [transforms[i] for i in keep_indices]
    new_bboxes     = [bboxes[i]     for i in keep_indices]

    # Remap parent indices through old_to_post_mount. Mount bones can't be
    # parents (we verified above) so every parent index in the kept list maps.
    new_parents = [old_to_post_mount.get(p, p) for p in new_parents]

    # ── Step 4: find Bone_Head in the post-Mount layout, insert eye bones ─
    try:
        head_idx = new_bones.index('Bone_Head')
    except ValueError:
        log("  ABORT Phase B: Bone_Head not found after Mount removal")
        return

    # Insert position is right after Bone_Head.
    insert_at = head_idx + 1
    log(f"  Inserting Bone_eye_L/Bone_eye_R after Bone_Head (post-mount index {head_idx})")

    new_bones[insert_at:insert_at]      = ['Bone_eye_L', 'Bone_eye_R']
    new_parents[insert_at:insert_at]    = [head_idx, head_idx]
    new_transforms[insert_at:insert_at] = [EYE_L_TRANSFORM, EYE_R_TRANSFORM]
    new_bboxes[insert_at:insert_at]     = [dict(ZERO_BBOX), dict(ZERO_BBOX)]

    # ── Step 5: build final old→new map (both Mount-removal + eye-insert) ─
    # Any bone whose post-mount index >= insert_at gets bumped by 2.
    old_to_final = {}
    for old_idx, post_mount_idx in old_to_post_mount.items():
        final_idx = post_mount_idx if post_mount_idx < insert_at else post_mount_idx + 2
        old_to_final[old_idx] = final_idx

    # Re-walk parent indices: each parent currently references the post-mount
    # index. Bump everything past insert_at by 2 to account for eye insertion.
    new_parents = [
        p if p < insert_at else p + 2
        for p in new_parents
    ]
    # But the two NEW eye bones' parent values (head_idx) we set above are
    # already correct (head_idx is post-mount but eyes are inserted AFTER head
    # so head_idx is unchanged by the eye insertion).
    # Wait: head_idx itself didn't move. But our new_parents list now has the
    # eye bones at positions insert_at and insert_at+1 with parent = head_idx
    # — that's still < insert_at, so the +2 shift didn't touch them. Good.
    # Fix the case where eyes' parent got incorrectly bumped above:
    new_parents[insert_at]     = head_idx
    new_parents[insert_at + 1] = head_idx

    # ── Step 6: remap skin_order through old_to_final ──────────────────
    new_skin_order = [old_to_final.get(b, b) for b in skin_order]

    # ── Step 7: commit ─────────────────────────────────────────────────
    char_json['bones']               = new_bones
    char_json['bone_parents']        = new_parents
    char_json['bone_transforms']     = new_transforms
    char_json['bone_bounding_boxes'] = new_bboxes
    char_json['skin_order']          = new_skin_order
    info['bones_num']                = len(new_bones)
    # skin_binds_num doesn't change — same binds, just different bone indices.

    log(f"  bones: {len(bones)} -> {len(new_bones)} (Mount removed, eye inserted)")
    log(f"  bones_num field updated to {info['bones_num']}")

    # ── Step 8: keep hk_constraint's parallel arrays in sync ───────────
    # hk_constraint has its own bone_names/bone_parents/bone_transforms list
    # (each parallel to the main bones array). If we touch main bones but not
    # these, the constraint subsystem sees a stale skeleton — known to push
    # the engine into a corrupted bone-state fallback that breaks render too.
    hk = char_json.get('hk_constraint')
    if hk and 'bone_names' in hk:
        hk_names      = hk['bone_names']
        hk_parents    = hk.get('bone_parents', [])
        hk_transforms = hk.get('bone_transforms', [])

        # Same Mount removal + eye insertion shape, applied to these arrays.
        new_hk_names      = [hk_names[i]      for i in keep_indices if i < len(hk_names)]
        new_hk_parents    = [hk_parents[i]    for i in keep_indices if i < len(hk_parents)]
        new_hk_transforms = [hk_transforms[i] for i in keep_indices if i < len(hk_transforms)]

        # Remap parents through old_to_post_mount; then bump past insert_at by 2.
        new_hk_parents = [old_to_post_mount.get(p, p) for p in new_hk_parents]
        new_hk_parents = [p if p < insert_at else p + 2 for p in new_hk_parents]

        # Insert eye bones at the same position
        new_hk_names[insert_at:insert_at]      = ['Bone_eye_L', 'Bone_eye_R']
        new_hk_parents[insert_at:insert_at]    = [head_idx, head_idx]
        new_hk_transforms[insert_at:insert_at] = [dict(EYE_L_HK_TRANSFORM), dict(EYE_R_HK_TRANSFORM)]

        # Each hk_constraint transform also has a 'scale' field — add it
        # to the inserted eye entries since the dict structure expects it.
        for t in (new_hk_transforms[insert_at], new_hk_transforms[insert_at + 1]):
            if 'scale' not in t:
                t['scale'] = {'x': 1.0, 'y': 1.0, 'z': 1.0, 'w': 0.0}

        hk['bone_names']      = new_hk_names
        hk['bone_parents']    = new_hk_parents
        hk['bone_transforms'] = new_hk_transforms

        # Update info counts if they exist.
        hk_info = hk.get('info', {})
        if 'bone_names_num' in hk_info:
            hk_info['bone_names_num']      = len(new_hk_names)
        if 'bone_parents_num' in hk_info:
            hk_info['bone_parents_num']    = len(new_hk_parents)
        if 'bone_transforms_num' in hk_info:
            hk_info['bone_transforms_num'] = len(new_hk_transforms)

        log(f"  hk_constraint synced: bone_names {len(hk_names)} -> {len(new_hk_names)}, "
            f"parents {len(hk_parents)} -> {len(new_hk_parents)}, "
            f"transforms {len(hk_transforms)} -> {len(new_hk_transforms)}")

        # hk_constraint.info.unk_13 looks like a skeleton-template hash that
        # the Havok skeleton mapper uses to pick a per-skeleton retargeting
        # rule set. Each shipping CRD has its own value:
        #   URK CRD: 3432558849
        #   Elf CRD: 192167520
        # Since we aligned this skeleton to URK's layout, claim URK's template.
        URK_HK_UNK13 = 3432558849
        old_unk13 = hk_info.get('unk_13')
        if old_unk13 != URK_HK_UNK13:
            log(f"  hk_constraint.info.unk_13: {old_unk13} -> {URK_HK_UNK13} (URK skeleton-template)")
            hk_info['unk_13'] = URK_HK_UNK13


def convert_to_crd(char_json, new_name=None, align_skeleton=True, pattern='urk',
                   keep_hero_mats=False, urk_ref_json=None, rebake_binds=False):
    """Apply the full Phase A + B + C conversion in place. Returns the modified dict.

    pattern='urk' (default): URK CRD format (fmt1=0x80C07, v_size=32, no Color)
    pattern='elf':           Elf CRD format (fmt1=0x80E07, v_size=36, white Color)
    keep_hero_mats=True:     Skip the mat-flag rewrite. Materials stay at the
                             original 0x0008... character-class flags so the
                             engine routes draws through the hero render path
                             (full lighting, normal maps, specular). Test only.
    urk_ref_json:            URK CRD JSON dict. If provided, copy URK's
                             bone_transforms + bone_bounding_boxes onto this
                             model. Tests "engine assumes URK anatomy"
                             hypothesis. Skin_binds stay Haldir-baked unless
                             rebake_binds=True.
    rebake_binds=True:       Recompute every skin_bind matrix from the model's
                             current bone hierarchy (after Phase B and any URK
                             transform replacement). Use with urk_ref_json to
                             produce a mesh that sits cleanly on URK anatomy."""
    info = char_json['info']
    body_flag = ELF_MAT_FLAGS_BODY if pattern == 'elf' else CROWD_MAT_FLAGS_BODY
    phys_flag = ELF_MAT_FLAGS_PHYS if pattern == 'elf' else CROWD_MAT_FLAGS_PHYS

    # ── 1. Asset type + name + gamemodemask ─────────────────────────────
    old_type = info['asset_type']
    info['asset_type'] = CROWD_ASSET_TYPE
    log(f"  asset_type: {old_type} -> {CROWD_ASSET_TYPE} (CrowdModel)")

    # gamemodemask -1 = visible in every gamemode. Hero models default to a
    # specific mode (11 for Haldir hero); CRDs are mode-agnostic.
    if 'gamemodemask' in info and info['gamemodemask'] != -1:
        log(f"  gamemodemask: {info['gamemodemask']} -> -1 (all modes)")
        info['gamemodemask'] = -1

    if new_name:
        info['key']       = new_name
        info['asset_key'] = new_name
        log(f"  key/asset_key set to {new_name!r}")

    # ── 2. Empty slots / slot_map (CRD models don't use them) ──────────
    char_json['slots']    = []
    char_json['slot_map'] = []
    info['slots_offset']    = 0
    info['slot_map_offset'] = 0
    log(f"  slots/slot_map emptied")

    # ── 3. Re-encode each renderable vertex_data entry ─────────────────
    n_converted = 0
    n_physics   = 0
    for i, vd in enumerate(char_json['vertex_data']):
        if not is_renderable_vertex_data(vd):
            n_physics += 1
            continue

        new_vd, n_verts = convert_renderable_to_urk_stream(
            vd, pattern=pattern, keep_full_streams=keep_hero_mats)
        char_json['vertex_data'][i] = new_vd

        # Update the matching buffer_info. Layout is 1-1 indexed: buffer_info[i]
        # primary stream references vertex_data[i].
        if i < len(char_json['buffer_infos']):
            bi = char_json['buffer_infos'][i]
            if bi.get('vbuff_info_offset') != i:
                log(f"  WARN: buffer_info[{i}].vbuff_info_offset = "
                    f"{bi.get('vbuff_info_offset')}, expected {i}. Leaving primary "
                    f"offset alone but updating v_size.")
            if keep_hero_mats:
                # Use the new vertex_data's serialized size to compute v_size.
                vd_size = char_json['vertex_data'][i]['info']['size']
                v_size = vd_size // n_verts if n_verts else 0
            else:
                v_size = ELF_VSIZE if pattern == 'elf' else URK_VSIZE
            update_buffer_info_to_single_stream(bi, v_size, n_verts)

        n_converted += 1

    log(f"  vertex_data: {n_converted} renderable converted, "
        f"{n_physics} physics-only left alone")

    # ── 4. Rewrite mat flags to crowd-class pattern ────────────────────
    if keep_hero_mats:
        log(f"  mat flags: KEPT AS-IS (--keep-hero-mats) — character render path")
    else:
        for i, m in enumerate(char_json['mats']):
            base = m['Mat1']['base']
            old_flag = base['flags']
            new_flag = classify_mat_flag(old_flag, body_flag, phys_flag)
            if new_flag != old_flag:
                base['flags'] = new_flag

        log(f"  mat flags: {len(char_json['mats'])} materials -> {pattern}-pattern crowd-class")

    # ── 5. Phase B: skeleton alignment to URK layout ───────────────────
    if align_skeleton:
        log("  Phase B: aligning skeleton to URK bone order")
        align_skeleton_to_urk(char_json)

    # ── 6. Phase C: bind-index alignment to URK layout ─────────────────
    if align_skeleton:
        log("  Phase C: reordering skin_order to URK bind layout")
        align_bind_indices_to_urk(char_json)

    # ── 7. Optional: copy URK bone_transforms (test "URK anatomy" path) ─
    if urk_ref_json is not None:
        copy_urk_bone_anatomy(char_json, urk_ref_json)

    # ── 8. Optional: rebake skin_binds against current rest pose ───────
    if rebake_binds:
        rebake_all_skin_binds(char_json)

    return char_json


def copy_urk_bone_anatomy(char_json, urk_json):
    """Replace bone_transforms and bone_bounding_boxes with URK's, by bone NAME
    matching. Requires Phase B to have aligned bone names to URK first. The
    insertion-only bones (eye_L, eye_R) get URK's transforms automatically by
    name match."""
    ub = urk_json['bones']
    ut = urk_json['bone_transforms']
    ubb = urk_json['bone_bounding_boxes']
    hb = char_json['bones']

    urk_by_name = {name: i for i, name in enumerate(ub)}

    n_replaced = 0
    n_missing  = 0
    for i, name in enumerate(hb):
        if name in urk_by_name:
            urk_idx = urk_by_name[name]
            char_json['bone_transforms'][i]     = json.loads(json.dumps(ut[urk_idx]))
            char_json['bone_bounding_boxes'][i] = json.loads(json.dumps(ubb[urk_idx]))
            n_replaced += 1
        else:
            n_missing += 1
    log(f"  URK-anatomy: {n_replaced} bone_transforms replaced from URK reference, "
        f"{n_missing} bones not in URK")


def rebake_all_skin_binds(char_json):
    """Recompute every skin_bind matrix as inverse(rest_world(skin_order[i])).
    Use this after copy_urk_bone_anatomy to make Haldir's mesh sit cleanly
    on URK's anatomy — at rest pose, palette × inverse-rest = identity, so
    vertices land where they should given the new bone positions."""
    bones      = char_json['bones']
    parents    = char_json['bone_parents']
    transforms = char_json['bone_transforms']
    skin_order = char_json['skin_order']

    rebaked = 0
    failed  = 0
    cache   = {}
    new_binds = []
    for i, bone_idx in enumerate(skin_order):
        try:
            world = _compute_bone_world(bones, parents, transforms, bone_idx, cache)
            inv   = _mat_inv(world)
            new_binds.append(_rows_to_mat(inv))
            rebaked += 1
        except Exception as e:
            log(f"    WARN: rebake failed for bind {i} (bone {bones[bone_idx]}): {e}")
            new_binds.append(char_json['skin_binds'][i] if i < len(char_json['skin_binds']) else None)
            failed += 1
    char_json['skin_binds'] = new_binds
    log(f"  Rebake skin_binds: {rebaked} computed, {failed} failed")


# URK's skin_order layout — bind index -> bone name. The shipping crowd render
# path may cache or hardcode by this exact layout (e.g. "bind 13 = LHand").
# To make a hero-rig model render through that path we must reshuffle bind
# slots so the same bone names sit at the same indices.
URK_BIND_LAYOUT = [
    'Bone_Lumbar1','Bone_UpperBody','Bone_Lumbar2','Bone_Lumbar3','Bone_LShoulder',
    'Bone_Neck','Bone_RShoulder','Bone_LBicep','Bone_LForearm','Bone_LForearmRoll',
    'Bone_LHand','Bone_LIndex1','Bone_LMiddle1','Bone_LPinky1','Bone_LRing1',
    'Bone_LThumb1','Bone_LIndex2','Bone_LIndex3','Bone_LMiddle2','Bone_LMiddle3',
    'Bone_LPinky2','Bone_LPinky3','Bone_LRing2','Bone_LRing3','Bone_LThumb2',
    'Bone_LThumb3','Bone_Head','Bone_jaw','Bone_RBicep','Bone_RForearm',
    'Bone_RForearmRoll','Bone_RHand','Bone_RIndex1','Bone_RMiddle1','Bone_RPinky1',
    'Bone_RRing1','Bone_RThumb1','Bone_RIndex2','Bone_RIndex3','Bone_RMiddle2',
    'Bone_RMiddle3','Bone_RPinky2','Bone_RPinky3','Bone_RRing2','Bone_RRing3',
    'Bone_RThumb2','Bone_RThumb3','Bone_LThigh','Bone_RThigh','Bone_LShin',
    'Bone_LFootBone1','Bone_LFootBone2','Bone_LFootBone3','Bone_RShin','Bone_RFootBone1',
    'Bone_RFootBone2','Bone_RFootBone3','Bone_LHand_attach','Bone_RHand_attach',
]


# ── 4×4 matrix helpers for computing inverse-rest-pose bind matrices ──
# Matrices are stored in the model JSON as nested dicts: outer keys x/y/z/w
# are the 4 COLUMNS, inner keys x/y/z/w are the entries of that column.
# Column-major 4x4: M = [col_x | col_y | col_z | col_w], last row = (0,0,0,1).

def _mat_to_rows(m):
    """Read the JSON column-major dict into 16 floats arranged row-major."""
    cols = [m['x'], m['y'], m['z'], m['w']]
    # Build rows from columns
    return [
        [cols[0]['x'], cols[1]['x'], cols[2]['x'], cols[3]['x']],
        [cols[0]['y'], cols[1]['y'], cols[2]['y'], cols[3]['y']],
        [cols[0]['z'], cols[1]['z'], cols[2]['z'], cols[3]['z']],
        [cols[0]['w'], cols[1]['w'], cols[2]['w'], cols[3]['w']],
    ]


def _rows_to_mat(rows):
    """Inverse of _mat_to_rows: serialize 4 rows back to column-major dict."""
    return {
        'x': {'x': rows[0][0], 'y': rows[1][0], 'z': rows[2][0], 'w': rows[3][0]},
        'y': {'x': rows[0][1], 'y': rows[1][1], 'z': rows[2][1], 'w': rows[3][1]},
        'z': {'x': rows[0][2], 'y': rows[1][2], 'z': rows[2][2], 'w': rows[3][2]},
        'w': {'x': rows[0][3], 'y': rows[1][3], 'z': rows[2][3], 'w': rows[3][3]},
    }


def _mat_mul(a, b):
    """a × b for 4x4 row-major matrices."""
    return [
        [sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
        for i in range(4)
    ]


def _mat_inv(m):
    """Invert a 4x4 row-major matrix via Gauss-Jordan elimination."""
    n = 4
    a = [row[:] + ([1.0 if i == j else 0.0 for j in range(n)]) for i, row in enumerate(m)]
    for col in range(n):
        # Pivot: find row with max abs in this column
        pivot = max(range(col, n), key=lambda r: abs(a[r][col]))
        a[col], a[pivot] = a[pivot], a[col]
        if abs(a[col][col]) < 1e-12:
            raise ValueError("singular matrix")
        # Normalize pivot row
        pv = a[col][col]
        a[col] = [v / pv for v in a[col]]
        # Eliminate other rows
        for r in range(n):
            if r == col:
                continue
            factor = a[r][col]
            a[r] = [a[r][k] - factor * a[col][k] for k in range(2 * n)]
    return [row[n:] for row in a]


def _compute_bone_world(bones, parents, transforms, bone_idx, _cache=None):
    """Recursively cascade bone_transforms up the hierarchy to get world matrix."""
    if _cache is None:
        _cache = {}
    if bone_idx in _cache:
        return _cache[bone_idx]
    local = _mat_to_rows(transforms[bone_idx])
    p = parents[bone_idx]
    if p < 0 or p >= len(bones) or p == bone_idx:
        world = local  # root
    else:
        parent_world = _compute_bone_world(bones, parents, transforms, p, _cache)
        world = _mat_mul(parent_world, local)
    _cache[bone_idx] = world
    return world


def _proper_inverse_bind_matrix(char_json, bone_name):
    """Compute inverse(rest_world(bone)) — the proper bind matrix that, when
    multiplied by the bone's animated world matrix at rest pose, yields identity.
    Returns None if the bone isn't in the model."""
    bones = char_json['bones']
    if bone_name not in bones:
        return None
    idx = bones.index(bone_name)
    try:
        world = _compute_bone_world(
            bones, char_json['bone_parents'], char_json['bone_transforms'], idx)
        inv = _mat_inv(world)
        return _rows_to_mat(inv)
    except Exception as e:
        log(f"    WARN: failed to compute inverse-bind for {bone_name}: {e}")
        return None


def align_bind_indices_to_urk(char_json):
    """Reorder skin_order + skin_binds to match URK's bind layout exactly, and
    remap every BlendIndices value in vertex_data so each vertex still weights
    to the same logical bone — just through a URK-compatible bind slot now.

    For URK binds that don't exist in the source character (LHand_attach,
    RHand_attach are missing in Haldir), we append the bone-index in skin_order
    but copy an IDENTITY skin_bind matrix as a placeholder. Without proper
    inverse-rest-pose math those slots will be visually wrong if any vertex
    references them — but Haldir's mesh doesn't, so they sit unused."""
    bones      = char_json['bones']
    old_so     = char_json['skin_order']
    old_binds  = char_json['skin_binds']

    bone_name_to_idx = {b: i for i, b in enumerate(bones)}

    # Build old bind index -> bone name lookup.
    old_idx_to_name = [bones[bi] if 0 <= bi < len(bones) else None for bi in old_so]
    name_to_old_idx = {name: i for i, name in enumerate(old_idx_to_name) if name}

    # Now build new arrays in URK order.
    new_so    = []
    new_binds = []
    old_to_new_idx = {}  # old_bind_idx -> new_bind_idx

    # Identity bind matrix — column-major 4x4 stored as x/y/z/w each being a
    # {x,y,z,w} dict (matching the model's bind layout).
    identity_bind = {
        'x': {'x': 1.0, 'y': 0.0, 'z': 0.0, 'w': 0.0},
        'y': {'x': 0.0, 'y': 1.0, 'z': 0.0, 'w': 0.0},
        'z': {'x': 0.0, 'y': 0.0, 'z': 1.0, 'w': 0.0},
        'w': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 1.0},
    }
    # Detect the actual bind format from an existing entry to match shape.
    sample_bind = old_binds[0] if old_binds else identity_bind
    bind_keys = list(sample_bind.keys()) if isinstance(sample_bind, dict) else None

    appended = 0
    for new_i, bone_name in enumerate(URK_BIND_LAYOUT):
        bone_idx = bone_name_to_idx.get(bone_name)
        if bone_idx is None:
            log(f"    WARN: URK-required bone {bone_name!r} not in model, "
                f"skipping bind slot {new_i}")
            continue
        new_so.append(bone_idx)
        # Find existing bind matrix for this bone (via old skin_order).
        old_i = name_to_old_idx.get(bone_name)
        if old_i is not None:
            new_binds.append(old_binds[old_i])
            old_to_new_idx[old_i] = new_i
        else:
            # Bone has no existing bind (e.g. LHand_attach / RHand_attach in
            # Haldir). Compute the proper inverse-rest-pose matrix from the
            # bone hierarchy so any shader path that reads this slot sees a
            # valid bind that produces identity at rest pose.
            proper = _proper_inverse_bind_matrix(char_json, bone_name)
            if proper is not None:
                new_binds.append(proper)
                log(f"    computed proper inverse-bind for {bone_name} at slot {new_i}")
            else:
                new_binds.append(dict(identity_bind))
            appended += 1

    log(f"    skin_binds: {len(old_binds)} -> {len(new_binds)} "
        f"({appended} identity placeholders appended)")

    # Commit reordered arrays.
    char_json['skin_order'] = new_so
    char_json['skin_binds']  = new_binds
    char_json['info']['skin_binds_num'] = len(new_binds)

    # Remap every vertex BlendIndices value through old_to_new_idx. BlendIndices
    # are stored as Unorm4x8 packed bytes — each byte is a bind index. We need
    # to scan each vertex_data entry's BlendIndices list and rewrite the bytes.
    remapped_entries = 0
    for vd in char_json['vertex_data']:
        d = vd.get('data', {})
        if 'BlendIndices' not in d:
            continue
        bi_stream = d['BlendIndices']['val'].get('Unorm4x8')
        if not bi_stream:
            continue
        # Each entry is either a packed u32 int OR a list of 4 bytes; handle both.
        new_stream = []
        for v in bi_stream:
            if isinstance(v, int):
                b0 =  v        & 0xFF
                b1 = (v >> 8)  & 0xFF
                b2 = (v >> 16) & 0xFF
                b3 = (v >> 24) & 0xFF
                b0 = old_to_new_idx.get(b0, b0)
                b1 = old_to_new_idx.get(b1, b1)
                b2 = old_to_new_idx.get(b2, b2)
                b3 = old_to_new_idx.get(b3, b3)
                new_stream.append(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24))
            elif isinstance(v, list):
                new_stream.append([old_to_new_idx.get(x, x) for x in v])
            else:
                new_stream.append(v)
        d['BlendIndices']['val']['Unorm4x8'] = new_stream
        remapped_entries += 1

    log(f"    BlendIndices remapped across {remapped_entries} vertex_data entries")


def main():
    ap = argparse.ArgumentParser(description="Convert hero character JSON to CRD format")
    ap.add_argument('input',  help='Input CH_*.json path')
    ap.add_argument('output', help='Output CRD_*.json path')
    ap.add_argument('--name', help='New asset name (optional)')
    ap.add_argument('--elf',  action='store_true',
                    help='Target elf-CRD pattern (v_size=36, keep Color stream, '
                         'mat flag 0x40 suffix) instead of URK pattern')
    ap.add_argument('--keep-hero-mats', action='store_true',
                    help='Test mode: keep the original 0x0008... character-class '
                         'mat flags so the engine renders through the hero path '
                         '(normal maps, specular, full lighting) even when the '
                         'asset is in a crowd batch.')
    ap.add_argument('--urk-bones', action='store_true',
                    help='Variant 1 test: copy URK CRD bone_transforms and '
                         'bone_bounding_boxes onto this model (by bone name '
                         'match after Phase B). Tests whether the engine '
                         'expects URK anatomy. Mesh will stretch since '
                         'skin_binds are still Haldir-baked.')
    ap.add_argument('--rebake-binds', action='store_true',
                    help='Variant 2 test: also recompute skin_binds = '
                         'inverse(rest_world) using the current bone hierarchy. '
                         'Combine with --urk-bones to put Haldir mesh cleanly '
                         'on URK skeleton (will look chunky like URK).')
    ap.add_argument('--urk-ref', default=None,
                    help='Path to URK CRD JSON (default: looks for '
                         'CRD_CH_urk_spr_all_01.json in the input file dir).')
    args = ap.parse_args()

    if not os.path.isfile(args.input):
        log(f"ERROR: input not found: {args.input}")
        sys.exit(2)

    log(f"Reading {args.input}")
    with open(args.input, 'r', encoding='utf-8') as f:
        j = json.load(f)

    pattern = 'elf' if args.elf else 'urk'
    suffix = []
    if args.keep_hero_mats: suffix.append('hero-mats')
    if args.urk_bones:      suffix.append('urk-bones')
    if args.rebake_binds:   suffix.append('rebake-binds')
    log(f"Target pattern: {pattern}{' (' + ', '.join(suffix) + ')' if suffix else ''}")

    urk_json = None
    if args.urk_bones:
        urk_path = args.urk_ref or os.path.join(os.path.dirname(args.input),
                                                'CRD_CH_urk_spr_all_01.json')
        if not os.path.isfile(urk_path):
            log(f"ERROR: --urk-bones requires URK reference at {urk_path}")
            sys.exit(2)
        with open(urk_path, 'r', encoding='utf-8') as f:
            urk_json = json.load(f)
        log(f"Loaded URK reference from {urk_path}")

    convert_to_crd(j, new_name=args.name, pattern=pattern,
                   keep_hero_mats=args.keep_hero_mats,
                   urk_ref_json=urk_json,
                   rebake_binds=args.rebake_binds)

    log(f"Writing {args.output}")
    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(j, f, indent=1)

    log(f"Done. Output size: {os.path.getsize(args.output)} bytes")


if __name__ == '__main__':
    main()
