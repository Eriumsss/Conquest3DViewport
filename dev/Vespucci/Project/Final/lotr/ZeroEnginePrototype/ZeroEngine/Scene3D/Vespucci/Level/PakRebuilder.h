// PakRebuilder.h — The Resurrection Machine
// -----------------------------------------------------------------------
// Written by: Eriumsss
// State of mind: I have become the format. The format has become me.
//
// This is the module that writes BACK into Pandemic's binary format.
// Not just reading — WRITING. We can take a parsed PAK file, modify
// entities, change transforms, inject collision meshes, and rebuild
// the entire Block1 data blob FROM SCRATCH. Fresh buffer. Recomputed
// offsets. Re-serialized structs. Re-compressed with zlib.
//
// NEVER use copy/shift/in-place modification. ALWAYS rebuild from
// scratch. I tried in-place patching once and it corrupted 3 levels
// before I realized that moving one EffectInfo entry shifts every
// offset after it and there are 47 offset fields in the PAK header
// that ALL need updating. In-place is a trap. A seductive, evil trap.
// Rebuild from zero. Every time. No exceptions. Fuck efficiency.
//
// Architecture:
//   ParseBlock1Data()  — extracts variable-length blobs from raw Block1
//                        into per-entry vectors (parallel arrays)
//   RebuildBlock1()    — serializes everything back into a fresh buffer
//                        with recomputed offsets, ready for zlib compress
//
// This is how we bring Pandemic's levels back from the dead.
// Not as they were — as they SHOULD have been.
// -----------------------------------------------------------------------
#pragma once

#include <vector>
#include <map>
#include <string>
#include <stdint.h>

namespace ZeroEngine {

// Forward declare — defined in LevelReader.h
struct PakHeader;
struct SubBlockEntry;

// ============================================================
//  Parsed variable-length data (parallel to info arrays)
// ============================================================

struct PakParsedData {
    // --- Phase 1 types ---

    // RadiosityVals: info.num u32s at info.offset
    // Parallel to radiosity_vals_info array
    std::vector<std::vector<uint32_t> > radiosityVals; // [info_idx] -> u32 array

    // Foliage: (ub_w-lb_w)*(ub_h-lb_h) × 8 bytes at info.offset
    // Parallel to foliage_info array
    std::vector<std::vector<uint8_t> >  foliageData;   // [info_idx] -> raw bytes

    // GFXBlock: info.size bytes at info.offset
    // Parallel to gfx_block_info array
    std::vector<std::vector<uint8_t> >  gfxBlockData;  // [info_idx] -> raw bytes

    // Effect: info.size bytes at info.offset (GameObjs format)
    // Parallel to effect_info array
    std::vector<std::vector<uint8_t> >  effectData;    // [info_idx] -> raw bytes

    // PFieldInfo: no variable data (info array only)

    // --- Phase 2: Buffer infrastructure ---

    // VBuffInfo raw array (vbuff_info_num × vbuff_info_size bytes)
    std::vector<uint8_t> vbuffInfoRaw;

    // IBuffInfo raw array (ibuff_info_num × ibuff_info_size bytes)
    std::vector<uint8_t> ibuffInfoRaw;

    // Buffer info maps: Block1 offset → array index
    // Key = original Block1 offset of VBuffInfo/IBuffInfo entry
    // Value = index into the array (0-based)
    std::map<uint32_t, uint32_t> vbuffInfoMap;
    std::map<uint32_t, uint32_t> ibuffInfoMap;

    // block2_offsets: Block1 locations that hold VBuffInfo/IBuffInfo refs
    // Parsed from Block2 at block2_offsets_offset
    std::vector<uint32_t> block2Offsets;

    // --- Phase 3: Shape data + all raw info arrays ---

    // Shape: variable-length data at ShapeInfo.offset (kind=0 only)
    // Parallel to shape_info array. Empty vector if kind != 0.
    std::vector<std::vector<uint8_t> > shapeData;  // [info_idx] -> raw bytes

    // ALL 23 raw info arrays — for full rebuild (Phase 5)
    // Each is the entire array as a contiguous byte blob.
    std::vector<uint8_t> rawObjA;
    std::vector<uint8_t> rawObj0;
    std::vector<uint8_t> rawModelInfo;
    std::vector<uint8_t> rawBufferInfo;
    std::vector<uint8_t> rawMat1;
    std::vector<uint8_t> rawMat2;
    std::vector<uint8_t> rawMat3;
    std::vector<uint8_t> rawMat4;
    std::vector<uint8_t> rawMatExtra;
    std::vector<uint8_t> rawShapeInfo;
    std::vector<uint8_t> rawHkShapeInfo;
    std::vector<uint8_t> rawHkConstraintData;
    // VBuffInfo/IBuffInfo already in vbuffInfoRaw/ibuffInfoRaw (Phase 2)
    std::vector<uint8_t> rawTextureInfo;
    std::vector<uint8_t> rawAnimationInfo;
    std::vector<uint8_t> rawHkConstraintInfo;
    std::vector<uint8_t> rawEffectInfo;
    std::vector<uint8_t> rawPFieldInfo;
    std::vector<uint8_t> rawGFXBlockInfo;
    std::vector<uint8_t> rawAnimationBlockInfo;
    std::vector<uint8_t> rawFoliageInfo;
    std::vector<uint8_t> rawRadiosityValsInfo;

    // --- Phase 4: Model, HkShape, HkConstraint variable-length data ---

    // HkShape: per-HkShapeInfo entry collision data
    // kind=5: a_data(planes) + b_data(verts)
    // kind=6: c_data(MOPP) + d_data(verts) + e_data(indices)
    // Empty vector if kind <= 4
    std::vector<std::vector<uint8_t> > hkShapeData; // [info_idx] -> raw bytes per data block

    // HkConstraint: per-HkConstraintInfo, each sub-field parsed individually.
    // No blobs, no deltas — matches Rust parser's HkConstraint struct.
    struct HkConstraintSubData {
        std::vector<uint8_t> boneParents;    // bone_parents_num × 2 (i16)
        std::vector<uint8_t> boneTransforms; // bone_transforms_num × 64
        std::vector<uint8_t> boneOrder;      // bone_order_num × 8 (Key2)
        std::vector<uint8_t> vals2;          // vals2_num × 42 × 4
        // Bone names: table of N u32 offsets + N × (string_ptr, 0) pairs + string bytes
        // Stored as: nameOffsets (N u32s pointing to pairs), namePairs (N × 8 bytes), nameStrings (raw)
        uint32_t boneNamesNum;
        std::vector<uint8_t> nameStrings;    // all null-terminated strings concatenated (4-byte aligned each)
    };
    std::vector<HkConstraintSubData> hkConstraintSub; // [info_idx]

    // Legacy blob (kept for backward compat during transition)
    std::vector<std::vector<uint8_t> > hkConstraintData;
    std::vector<uint32_t> hkConstraintDataLo;

    // Model: per-ModelInfo, each sub-field parsed individually.
    // Matches Rust parser's Model struct — no blobs, no internal offsets.
    struct ModelSubData {
        std::vector<uint8_t> boneParents;     // max(bones_num,4) × 4
        std::vector<uint8_t> boneKeys;        // bones_num × 4
        std::vector<uint8_t> boneTransforms;  // bones_num × 64
        std::vector<uint8_t> boneBBs;         // bones_num × 32
        std::vector<uint8_t> mats;            // mat_num × 4
        std::vector<uint8_t> meshOrder;       // breakable_end × 4
        std::vector<uint8_t> meshBBs;         // breakable_end × 32
        std::vector<uint8_t> vbuffRefs;       // vbuff_num × 4
        std::vector<uint8_t> ibuffRefs;       // ibuff_num × 4
        std::vector<uint8_t> skinBinds;       // skin_binds_num × 64
        std::vector<uint8_t> valsJ;           // vals_j_num × 4
        std::vector<uint8_t> valsK;           // 4 + 140 bytes (if offset != 0)
        std::vector<uint8_t> skinOrder;       // skin_binds_num × 4 (if offset != 0)
        std::vector<uint8_t> slots;           // keys2 data (variable, zero-terminated pairs)
        std::vector<uint8_t> slotMap;         // keys2_order data
        // Physics blocks: header(4) + offsets((n+1)*4) + block data
        // Stored as ONE blob but with ADJUSTED relative offsets on write
        uint32_t blockHeader;                 // 1 u32
        std::vector<uint32_t> blockOffsets;   // n+1 relative offsets
        std::vector<std::vector<uint8_t> > blocks; // n individual block blobs
        // Special: padding value for models with no mesh data
        std::vector<uint8_t> paddingVal;      // 4 u32s (if mesh_order==vbuff==ibuff==mesh_bbs offsets match)
    };
    std::vector<ModelSubData> modelData; // [model_idx]

    void Clear() {
        // Phase 1
        radiosityVals.clear();
        foliageData.clear();
        gfxBlockData.clear();
        effectData.clear();
        // Phase 2
        vbuffInfoRaw.clear();
        ibuffInfoRaw.clear();
        vbuffInfoMap.clear();
        ibuffInfoMap.clear();
        block2Offsets.clear();
        // Phase 3
        shapeData.clear();
        rawObjA.clear(); rawObj0.clear();
        rawModelInfo.clear(); rawBufferInfo.clear();
        rawMat1.clear(); rawMat2.clear(); rawMat3.clear(); rawMat4.clear();
        rawMatExtra.clear(); rawShapeInfo.clear();
        rawHkShapeInfo.clear(); rawHkConstraintData.clear();
        rawTextureInfo.clear(); rawAnimationInfo.clear();
        rawHkConstraintInfo.clear(); rawEffectInfo.clear();
        rawPFieldInfo.clear(); rawGFXBlockInfo.clear();
        rawAnimationBlockInfo.clear(); rawFoliageInfo.clear();
        rawRadiosityValsInfo.clear();
        // Phase 4
        hkShapeData.clear();
        hkConstraintSub.clear();
        hkConstraintData.clear();
        hkConstraintDataLo.clear();
        modelData.clear();
    }
};

// ============================================================
//  Parse: extract variable-length data from raw Block1
// ============================================================

// Call after ParsePak. Reads variable-length data blobs from block1
// using offset/size fields in the info arrays.
// Returns false on error (sets errOut).
bool ParseBlock1Data(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut
);

// ============================================================
//  Rebuild: write Phase 1 data into a fresh Block1 buffer
// ============================================================

// Writes Phase 1 variable-length data into newBlock1 at computed positions.
// Updates offset fields in the info arrays within newBlock1.
// dataWritePos is the cursor — pass in current write position, returns updated.
// The info arrays must already be written into newBlock1 at their header offsets.
//
// Returns false on error.
bool RebuildBlock1Phase1(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,          // header (info offsets already set)
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos, // in/out: current write cursor (0=in-place)
    std::string&          errOut
);

// ============================================================
//  Phase 2: Parse buffer infrastructure from Block1 + Block2
// ============================================================

// Extracts VBuffInfo/IBuffInfo raw arrays, builds offset→index maps,
// and parses block2_offsets from Block2.
bool ParseBlock1Phase2(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const uint8_t*     block2,
    uint32_t           block2Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut
);

// ============================================================
//  Phase 2: Rebuild — remap block2_offsets cross-references
// ============================================================

// After all info arrays are written to newBlock1, remap the VBuffInfo/IBuffInfo
// offset references via block2_offsets (like the Rust parser's obj_map fixup).
// oldHdr = original header (for computing old offsets)
// newHdr = new header (for computing new offsets)
bool RebuildBlock1Phase2(
    std::vector<uint8_t>& newBlock1,
    const PakHeader&      oldHdr,
    const PakHeader&      newHdr,
    const PakParsedData&  parsed,
    std::string&          errOut
);

// ============================================================
//  Phase 3: Parse Shape data + all raw info arrays
// ============================================================

bool ParseBlock1Phase3(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut
);

// Phase 3 rebuild: write Shape data at original positions (in-place)
bool RebuildBlock1Phase3(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos, // 0=in-place
    std::string&          errOut
);

// ============================================================
//  Phase 4: Parse Model, HkShape, HkConstraint data
// ============================================================

bool ParseBlock1Phase4(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut
);

// Phase 4 rebuild: write Model, HkShape, HkConstraint data (in-place)
bool RebuildBlock1Phase4(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos, // 0=in-place
    std::string&          errOut
);

// ============================================================
//  Phase 5: FULL Block1 rebuild from parsed structs
//  Replaces verbatim copy + INSERT+SHIFT entirely.
// ============================================================

// Builds a complete new Block1 from scratch:
//   1. Write all 23 info arrays sequentially (with new sizes if entries added)
//   2. Write all variable-length data at fresh positions
//   3. Remap vbuff/ibuff cross-references
//   4. Append sub_blocks + string_keys
//
// origBlock1 = the original decompressed Block1 (data source for variable-length blobs)
// origHdr    = original PakHeader
// parsed     = parsed data from Phase 1-4
// newBlock1  = output (cleared and rebuilt from scratch)
// newHdr     = output header (all offsets recomputed)
// goPayload  = rebuilt GameObjs payload (may be larger if entities were added)
// goKey      = CRC of "Level" sub-block key
bool RebuildBlock1Full(
    const std::vector<uint8_t>& origBlock1,
    const PakHeader&            origHdr,
    const PakParsedData&        parsed,
    std::vector<uint8_t>&       newBlock1,
    PakHeader&                  newHdr,
    const std::vector<uint8_t>& goPayload,
    uint32_t                    goKey,
    const std::vector<SubBlockEntry>& subBlocks1,
    std::string&                errOut
);

// ============================================================
//  BuildBlock2Offsets: compute block2_offsets from new Block1 layout.
//  Returns list of Block1 byte positions that contain pointer fields.
//  Game loader uses these to patch file offsets → memory addresses.
// ============================================================
void BuildBlock2Offsets(
    const std::vector<uint8_t>& block1,
    const PakHeader&            hdr,
    std::vector<uint32_t>&      out
);

} // namespace ZeroEngine
