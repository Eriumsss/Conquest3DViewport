// PakRebuilder.cpp — Reassembling the Dead. From Scratch. Every Goddamn Time.
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Full Block1 rebuild implementation. Takes every parsed info array,
// every variable-length data blob, every sub-block payload, and writes
// them ALL back into a fresh binary buffer with recomputed offsets.
// Then zlib-compresses the result and updates the PAK header's 118
// fields to match the new layout. This is not a patch. This is a
// COMPLETE RECONSTRUCTION of Pandemic's binary format, from the first
// byte to the last. Every time. No shortcuts. No in-place hacks.
//
// This is how we modify Conquest's levels — change a transform, add
// an entity, inject collision — then rebuild the entire PAK. It's
// slow, it's overkill, but it's CORRECT. And correct is all that
// matters when you're writing into a format where one wrong offset
// makes the game loader crash without explanation.
// -----------------------------------------------------------------------

#include "PakRebuilder.h"
#include "LevelReader.h"  // PakHeader, FoliageInfo, etc.

#include <cstring>
#include <cstdio>
#include <stdarg.h>

namespace ZeroEngine {

// ============================================================
//  Logging (shared log file with LevelReader)
// ============================================================
static FILE* s_rebuildLog = NULL;
static void RLog(const char* fmt, ...) {
    if (!s_rebuildLog) s_rebuildLog = fopen("level_reader.log", "a");
    if (!s_rebuildLog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_rebuildLog, fmt, ap);
    va_end(ap);
    fflush(s_rebuildLog);
}

// ============================================================
//  Helpers
// ============================================================

static inline uint32_t ReadU32(const uint8_t* p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}

static inline int32_t ReadI32(const uint8_t* p) {
    int32_t v; memcpy(&v, p, 4); return v;
}

static inline void WriteU32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
}

static inline uint32_t Align(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

// ============================================================
//  ParseBlock1Data — extract variable-length data from raw Block1
// ============================================================

bool ParseBlock1Data(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut)
{
    out.Clear();

    // --- RadiosityValsInfo: num u32s at offset ---
    {
        uint32_t base   = hdr.radiosity_vals_info_offset;
        uint32_t num    = hdr.radiosity_vals_info_num;
        uint32_t stride = hdr.radiosity_vals_info_size;
        out.radiosityVals.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t infoOff = base + i * stride;
            if (infoOff + stride > block1Size) break;

            // RadiosityValsInfo layout: guid(4) num(4) offset(4)
            uint32_t rNum    = ReadU32(block1 + infoOff + 4);
            uint32_t rOffset = ReadU32(block1 + infoOff + 8);

            if (rNum > 0 && rOffset + rNum * 4 <= block1Size) {
                out.radiosityVals[i].resize(rNum);
                memcpy(&out.radiosityVals[i][0], block1 + rOffset, rNum * 4);
            }
        }
    }

    // --- FoliageInfo: (ub_w - lb_w) * (ub_h - lb_h) × 8 bytes at offset ---
    {
        uint32_t base   = hdr.foliage_info_offset;
        uint32_t num    = hdr.foliage_info_num;
        uint32_t stride = hdr.foliage_info_size;
        out.foliageData.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t infoOff = base + i * stride;
            if (infoOff + stride > block1Size) break;

            // FoliageInfo layout: key(4) kind(4) lb_w(4) lb_h(4) ub_w(4) ub_h(4) scale(4) offset(4) ...
            int32_t lb_w = ReadI32(block1 + infoOff + 8);
            int32_t lb_h = ReadI32(block1 + infoOff + 12);
            int32_t ub_w = ReadI32(block1 + infoOff + 16);
            int32_t ub_h = ReadI32(block1 + infoOff + 20);
            uint32_t fOffset = ReadU32(block1 + infoOff + 28);

            int32_t n = (ub_w - lb_w) * (ub_h - lb_h);
            if (n < 0) n = 0;
            uint32_t dataSize = (uint32_t)n * 8; // FoliageVal = 8 bytes (u16,u16,i16,i16)

            if (dataSize > 0 && fOffset + dataSize <= block1Size) {
                out.foliageData[i].assign(block1 + fOffset, block1 + fOffset + dataSize);
            }
        }
    }

    // --- GFXBlockInfo: info.size raw bytes at info.offset ---
    {
        uint32_t base = hdr.gfx_block_info_offset;
        uint32_t num  = hdr.gfx_block_info_num;
        // GFXBlockInfo = 12 bytes: key(4) offset(4) size(4)
        out.gfxBlockData.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t infoOff = base + i * 12;
            if (infoOff + 12 > block1Size) break;

            uint32_t gOffset = ReadU32(block1 + infoOff + 4);
            uint32_t gSize   = ReadU32(block1 + infoOff + 8);

            if (gSize > 0 && gOffset + gSize <= block1Size) {
                out.gfxBlockData[i].assign(block1 + gOffset, block1 + gOffset + gSize);
            }
        }
    }

    // --- EffectInfo: info.size raw bytes at info.offset ---
    {
        uint32_t base = hdr.effect_info_offset;
        uint32_t num  = hdr.effect_info_num;
        // EffectInfo = 16 bytes: key(4) gamemodemask(4) offset(4) size(4)
        out.effectData.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t infoOff = base + i * 16;
            if (infoOff + 16 > block1Size) break;

            uint32_t eOffset = ReadU32(block1 + infoOff + 8);
            uint32_t eSize   = ReadU32(block1 + infoOff + 12);

            if (eSize > 0 && eOffset + eSize <= block1Size) {
                out.effectData[i].assign(block1 + eOffset, block1 + eOffset + eSize);
            }
        }
    }

    // PFieldInfo: no variable data — nothing to extract

    return true;
}

// ============================================================
//  RebuildBlock1Phase1 — write Phase 1 data at computed positions
// ============================================================

bool RebuildBlock1Phase1(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos,
    std::string&          errOut)
{
    // Two modes:
    //   dataWritePos == 0 → "in-place" mode: write at original info.offset positions
    //                       (incremental port — proves pipeline, byte-identical output)
    //   dataWritePos >  0 → "fresh layout" mode: write sequentially at cursor
    //                       (full rebuild — used when all phases are ported)
    bool freshLayout = (dataWritePos > 0);

    // --- RadiosityVals: write u32 arrays ---
    {
        uint32_t base   = hdr.radiosity_vals_info_offset;
        uint32_t num    = hdr.radiosity_vals_info_num;
        uint32_t stride = hdr.radiosity_vals_info_size;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.radiosityVals.size(); ++i) {
            const std::vector<uint32_t>& vals = parsed.radiosityVals[i];
            if (vals.empty()) continue;

            uint32_t dataSize = (uint32_t)vals.size() * 4;
            uint32_t infoOff = base + i * stride;
            if (infoOff + 12 > (uint32_t)newBlock1.size()) continue;

            uint32_t writeAt = freshLayout ? dataWritePos : ReadU32(&newBlock1[infoOff + 8]);
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &vals[0], dataSize);
            WriteU32(&newBlock1[infoOff + 8], writeAt);
            if (freshLayout) dataWritePos = writeAt + dataSize;
        }
    }

    // --- Foliage: write raw bytes ---
    {
        uint32_t base   = hdr.foliage_info_offset;
        uint32_t num    = hdr.foliage_info_num;
        uint32_t stride = hdr.foliage_info_size;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.foliageData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.foliageData[i];
            if (data.empty()) continue;

            uint32_t dataSize = (uint32_t)data.size();
            uint32_t infoOff = base + i * stride;
            if (infoOff + 32 > (uint32_t)newBlock1.size()) continue;

            uint32_t writeAt = freshLayout ? dataWritePos : ReadU32(&newBlock1[infoOff + 28]);
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);
            WriteU32(&newBlock1[infoOff + 28], writeAt);
            if (freshLayout) dataWritePos = writeAt + dataSize;
        }
    }

    // --- GFXBlock: write raw bytes ---
    {
        uint32_t base = hdr.gfx_block_info_offset;
        uint32_t num  = hdr.gfx_block_info_num;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.gfxBlockData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.gfxBlockData[i];
            if (data.empty()) continue;

            uint32_t dataSize = (uint32_t)data.size();
            uint32_t infoOff = base + i * 12;
            if (infoOff + 12 > (uint32_t)newBlock1.size()) continue;

            uint32_t writeAt = freshLayout ? dataWritePos : ReadU32(&newBlock1[infoOff + 4]);
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);
            WriteU32(&newBlock1[infoOff + 4], writeAt);
            WriteU32(&newBlock1[infoOff + 8], dataSize);
            if (freshLayout) dataWritePos = writeAt + dataSize;
        }
    }

    // --- Effect: write raw bytes ---
    {
        uint32_t base = hdr.effect_info_offset;
        uint32_t num  = hdr.effect_info_num;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.effectData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.effectData[i];
            if (data.empty()) continue;

            uint32_t dataSize = (uint32_t)data.size();
            uint32_t infoOff = base + i * 16;
            if (infoOff + 16 > (uint32_t)newBlock1.size()) continue;

            uint32_t writeAt = freshLayout ? dataWritePos : ReadU32(&newBlock1[infoOff + 8]);
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);
            WriteU32(&newBlock1[infoOff + 8], writeAt);
            WriteU32(&newBlock1[infoOff + 12], dataSize);
            if (freshLayout) dataWritePos = writeAt + dataSize;
        }
    }

    // PFieldInfo: no variable data — nothing to write

    return true;
}

// ============================================================
//  Helper: extract raw info array as byte blob
// ============================================================

static void ExtractRawArray(const uint8_t* block1, uint32_t block1Size,
                            uint32_t offset, uint32_t num, uint32_t stride,
                            std::vector<uint8_t>& out) {
    uint32_t total = num * stride;
    if (num > 0 && stride > 0 && offset + total <= block1Size) {
        out.assign(block1 + offset, block1 + offset + total);
    } else {
        out.clear();
    }
}

// ============================================================
//  Phase 2: Parse buffer infrastructure
// ============================================================

bool ParseBlock1Phase2(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const uint8_t*     block2,
    uint32_t           block2Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut)
{
    // --- VBuffInfo: extract raw array + build offset→index map ---
    {
        uint32_t base   = hdr.vbuff_info_offset;
        uint32_t num    = hdr.vbuff_info_num;
        uint32_t stride = hdr.vbuff_info_size;
        uint32_t totalBytes = num * stride;

        if (base + totalBytes <= block1Size) {
            out.vbuffInfoRaw.assign(block1 + base, block1 + base + totalBytes);
        }

        out.vbuffInfoMap.clear();
        for (uint32_t i = 0; i < num; ++i) {
            out.vbuffInfoMap[base + stride * i] = i;
        }
    }

    // --- IBuffInfo: extract raw array + build offset→index map ---
    {
        uint32_t base   = hdr.ibuff_info_offset;
        uint32_t num    = hdr.ibuff_info_num;
        uint32_t stride = hdr.ibuff_info_size;
        uint32_t totalBytes = num * stride;

        if (base + totalBytes <= block1Size) {
            out.ibuffInfoRaw.assign(block1 + base, block1 + base + totalBytes);
        }

        out.ibuffInfoMap.clear();
        for (uint32_t i = 0; i < num; ++i) {
            out.ibuffInfoMap[base + stride * i] = i;
        }
    }

    // --- block2_offsets: parse from Block2 ---
    {
        uint32_t off = hdr.block2_offsets_offset;
        uint32_t num = hdr.block2_offsets_num;
        out.block2Offsets.clear();

        if (block2 && num > 0 && off + num * 4 <= block2Size) {
            out.block2Offsets.resize(num);
            memcpy(&out.block2Offsets[0], block2 + off, num * 4);
        }
    }

    return true;
}

// ============================================================
//  Phase 2: Rebuild — remap block2_offsets cross-references
// ============================================================

bool RebuildBlock1Phase2(
    std::vector<uint8_t>& newBlock1,
    const PakHeader&      oldHdr,
    const PakHeader&      newHdr,
    const PakParsedData&  parsed,
    std::string&          errOut)
{
    // Build obj_map: old Block1 offset → new Block1 offset
    // For each VBuffInfo/IBuffInfo entry, map its old position to its new position.
    // This is the exact equivalent of the Rust parser's obj_map construction.
    std::map<uint32_t, uint32_t> objMap;

    // IBuffInfo entries
    for (std::map<uint32_t, uint32_t>::const_iterator it = parsed.ibuffInfoMap.begin();
         it != parsed.ibuffInfoMap.end(); ++it) {
        uint32_t oldOff = it->first;
        uint32_t idx    = it->second;
        uint32_t newOff = newHdr.ibuff_info_offset + newHdr.ibuff_info_size * idx;
        objMap[oldOff] = newOff;
    }

    // VBuffInfo entries
    for (std::map<uint32_t, uint32_t>::const_iterator it = parsed.vbuffInfoMap.begin();
         it != parsed.vbuffInfoMap.end(); ++it) {
        uint32_t oldOff = it->first;
        uint32_t idx    = it->second;
        uint32_t newOff = newHdr.vbuff_info_offset + newHdr.vbuff_info_size * idx;
        objMap[oldOff] = newOff;
    }

    // For each block2_offset location in Block1, read the u32 value,
    // look it up in objMap, and write the new value.
    for (uint32_t bi = 0; bi < (uint32_t)parsed.block2Offsets.size(); ++bi) {
        uint32_t loc = parsed.block2Offsets[bi];
        if (loc + 4 > (uint32_t)newBlock1.size()) continue;

        uint32_t oldVal = ReadU32(&newBlock1[loc]);
        std::map<uint32_t, uint32_t>::const_iterator it = objMap.find(oldVal);
        if (it != objMap.end()) {
            WriteU32(&newBlock1[loc], it->second);
        }
    }

    return true;
}

// ============================================================
//  Phase 3: Parse Shape data + all raw info arrays
// ============================================================

bool ParseBlock1Phase3(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut)
{
    // --- Shape variable-length data (kind=0 only) ---
    // Shape layout at ShapeInfo.offset:
    //   header: 4 × u32 (16 bytes) — num, unk1, unk2, unk3
    //   vals: num × u32
    //   data: (vals.last() + 2) bytes
    // We store the entire blob as raw bytes.
    {
        uint32_t base   = hdr.shape_info_offset;
        uint32_t num    = hdr.shape_info_num;
        uint32_t stride = hdr.shape_info_size;
        out.shapeData.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t infoOff = base + i * stride;
            if (infoOff + stride > block1Size) break;

            // ShapeInfo: offset at byte 0, kind at byte 4
            uint32_t sOffset = ReadU32(block1 + infoOff + 0);
            uint32_t sKind   = ReadU32(block1 + infoOff + 4);

            if (sKind != 0 || sOffset == 0) continue;

            // Read header to get num
            if (sOffset + 16 > block1Size) continue;
            uint32_t headerNum = ReadU32(block1 + sOffset);

            // Compute total size: header(16) + vals(headerNum*4) + data(vals[last]+2)
            uint32_t valsEnd = sOffset + 16 + headerNum * 4;
            if (valsEnd > block1Size || headerNum == 0) continue;

            uint32_t lastVal = ReadU32(block1 + valsEnd - 4);
            uint32_t dataSize = lastVal + 2;
            uint32_t totalSize = 16 + headerNum * 4 + dataSize;

            if (sOffset + totalSize > block1Size) continue;

            out.shapeData[i].assign(block1 + sOffset, block1 + sOffset + totalSize);
        }
    }

    // --- Extract ALL raw info arrays ---
    #define EXTRACT(field, offField, numField, szField) \
        ExtractRawArray(block1, block1Size, hdr.offField, hdr.numField, hdr.szField, out.field)

    EXTRACT(rawObjA,              obja_offset,              obja_num,              obja_size);
    EXTRACT(rawObj0,              obj0_offset,              obj0_num,              obj0_size);
    EXTRACT(rawModelInfo,         model_info_offset,        model_info_num,        model_info_size);
    EXTRACT(rawBufferInfo,        buffer_info_offset,       buffer_info_num,       buffer_info_size);
    EXTRACT(rawMat1,              mat1_offset,              mat1_num,              mat1_size);
    EXTRACT(rawMat2,              mat2_offset,              mat2_num,              mat2_size);
    EXTRACT(rawMat3,              mat3_offset,              mat3_num,              mat3_size);
    EXTRACT(rawMat4,              mat4_offset,              mat4_num,              mat4_size);
    EXTRACT(rawMatExtra,          mat_extra_offset,         mat_extra_num,         mat_extra_size);
    EXTRACT(rawShapeInfo,         shape_info_offset,        shape_info_num,        shape_info_size);
    EXTRACT(rawHkShapeInfo,       hk_shape_info_offset,     hk_shape_info_num,     hk_shape_info_size);
    EXTRACT(rawHkConstraintData,  hk_constraint_data_offset,hk_constraint_data_num,hk_constraint_data_size);
    EXTRACT(rawTextureInfo,       texture_info_offset,      texture_info_num,      texture_info_size);
    EXTRACT(rawAnimationInfo,     animation_info_offset,    animation_info_num,    animation_info_size);
    EXTRACT(rawHkConstraintInfo,  hk_constraint_info_offset,hk_constraint_info_num,hk_constraint_info_size);
    EXTRACT(rawEffectInfo,        effect_info_offset,       effect_info_num,       effect_info_size);
    EXTRACT(rawPFieldInfo,        pfield_info_offset,       pfield_info_num,       pfield_info_size);
    EXTRACT(rawGFXBlockInfo,      gfx_block_info_offset,    gfx_block_info_num,    gfx_block_info_size);
    EXTRACT(rawAnimationBlockInfo,animation_block_info_offset,animation_block_info_num,animation_block_info_size);
    EXTRACT(rawFoliageInfo,       foliage_info_offset,      foliage_info_num,      foliage_info_size);
    EXTRACT(rawRadiosityValsInfo, radiosity_vals_info_offset,radiosity_vals_info_num,radiosity_vals_info_size);

    #undef EXTRACT

    return true;
}

// ============================================================
//  Phase 3: Rebuild — write Shape data
// ============================================================

bool RebuildBlock1Phase3(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos,
    std::string&          errOut)
{
    bool freshLayout = (dataWritePos > 0);

    // --- Shape: write raw bytes at info.offset ---
    {
        uint32_t base   = hdr.shape_info_offset;
        uint32_t num    = hdr.shape_info_num;
        uint32_t stride = hdr.shape_info_size;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.shapeData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.shapeData[i];
            if (data.empty()) continue;

            uint32_t dataSize = (uint32_t)data.size();
            uint32_t infoOff = base + i * stride;
            if (infoOff + 8 > (uint32_t)newBlock1.size()) continue;

            uint32_t writeAt = freshLayout ? dataWritePos : ReadU32(&newBlock1[infoOff + 0]);
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);
            WriteU32(&newBlock1[infoOff + 0], writeAt); // update ShapeInfo.offset
            if (freshLayout) dataWritePos = writeAt + dataSize;
        }
    }

    return true;
}

// ============================================================
//  Phase 4: Parse Model, HkShape, HkConstraint variable-length data
// ============================================================

// Helper: extend range [lo, hi) to include [offset, offset+size)
static inline void ExtendRange(uint32_t& lo, uint32_t& hi,
                                uint32_t offset, uint32_t size) {
    if (size == 0 || offset == 0) return;
    if (offset < lo) lo = offset;
    uint32_t end = offset + size;
    if (end > hi) hi = end;
}

bool ParseBlock1Phase4(
    const uint8_t*     block1,
    uint32_t           block1Size,
    const PakHeader&   hdr,
    PakParsedData&     out,
    std::string&       errOut)
{
    // --- HkShape: extract per-entry collision data ---
    {
        uint32_t base = hdr.hk_shape_info_offset;
        uint32_t num  = hdr.hk_shape_info_num;
        out.hkShapeData.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * 80;
            if (e + 80 > block1Size) break;

            uint32_t kind = ReadU32(block1 + e + 32);

            if (kind == 5) {
                // convex: a_data (a_num × 16 bytes) at a_offset, b_data (b_num × 12 + padding) at b_offset
                uint32_t aNum = ReadU32(block1 + e + 40);
                uint32_t aOff = ReadU32(block1 + e + 44);
                uint32_t bNum = ReadU32(block1 + e + 48);
                uint32_t bOff = ReadU32(block1 + e + 52);

                // Compute b_data size with padding (align to 16)
                uint32_t bRawSize = bNum * 12;
                uint32_t bPadSize = bRawSize;
                while ((bOff + bPadSize) % 16 != 0) bPadSize += 12; // extra entries for alignment

                uint32_t lo = 0xFFFFFFFFu, hi = 0;
                ExtendRange(lo, hi, aOff, aNum * 16);
                ExtendRange(lo, hi, bOff, bPadSize);
                if (lo < hi && hi <= block1Size) {
                    out.hkShapeData[i].assign(block1 + lo, block1 + hi);
                }
            } else if (kind == 6) {
                // BVTree: c(MOPP) at c_offset, d(verts) at d_offset, e(indices) at e_offset
                uint32_t cNum = ReadU32(block1 + e + 56); // tree_size
                uint32_t cOff = ReadU32(block1 + e + 60); // tree_offset
                uint32_t dNum = ReadU32(block1 + e + 64); // num_verts
                uint32_t dOff = ReadU32(block1 + e + 68); // verts_offset
                uint32_t eNum = ReadU32(block1 + e + 72); // num_tris
                uint32_t eOff = ReadU32(block1 + e + 76); // indices_offset

                uint32_t lo = 0xFFFFFFFFu, hi = 0;
                ExtendRange(lo, hi, cOff, cNum);
                ExtendRange(lo, hi, dOff, dNum * 12);
                ExtendRange(lo, hi, eOff, eNum * 6);
                if (lo < hi && hi <= block1Size) {
                    out.hkShapeData[i].assign(block1 + lo, block1 + hi);
                }
            }
            // kind <= 4: no variable data
        }
    }

    // --- HkConstraint: extract per-entry data ---
    // Helper macro for extracting flat arrays from Block1
    #define EXTRACT_FLAT(dst, off, sz) \
        if ((off) != 0 && (sz) > 0 && (off)+(sz) <= block1Size) \
            (dst).assign(block1 + (off), block1 + (off) + (sz))

    // HkConstraintInfo: parse each sub-field individually (like Model sub-data).
    // No blobs, no deltas — each field written at fresh position on rebuild.
    //
    // HkConstraintInfo fields (Block1 offsets):
    //   byte 4:  bone_parents_offset,  byte 8:  bone_parents_num
    //   byte 12: bone_names_offset,    byte 16: bone_names_num
    //   byte 20: bone_transforms_offset, byte 24: bone_transforms_num
    //   byte 40: bones_offset (→ MODEL bones, NOT constraint data)
    //   byte 44: bones_num (u16) + bone_order_num (u16)
    //   byte 48: bone_order_offset
    //   byte 60: vals2_num,            byte 64: vals2_offset
    //
    // Bone names layout at bone_names_offset:
    //   [N u32 offsets] [N × (string_ptr, 0) pairs] ... then strings elsewhere
    //   We extract the raw strings individually and rebuild the table on write.
    {
        uint32_t base   = hdr.hk_constraint_info_offset;
        uint32_t num    = hdr.hk_constraint_info_num;
        uint32_t stride = hdr.hk_constraint_info_size;
        out.hkConstraintSub.resize(num);

        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * stride;
            if (e + stride > block1Size) break;

            PakParsedData::HkConstraintSubData& cd = out.hkConstraintSub[i];

            uint32_t bpOff = ReadU32(block1 + e + 4);
            uint32_t bpNum = ReadU32(block1 + e + 8);
            uint32_t bnOff = ReadU32(block1 + e + 12);
            uint32_t bnNum = ReadU32(block1 + e + 16);
            uint32_t btOff = ReadU32(block1 + e + 20);
            uint32_t btNum = ReadU32(block1 + e + 24);
            uint16_t borNum = (uint16_t)(ReadU32(block1 + e + 44) >> 16);
            uint32_t borOff = ReadU32(block1 + e + 48);
            uint32_t v2Num = ReadU32(block1 + e + 60);
            uint32_t v2Off = ReadU32(block1 + e + 64);

            cd.boneNamesNum = bnNum;

            // Extract flat arrays
            EXTRACT_FLAT(cd.boneParents,    bpOff, bpNum * 2);
            EXTRACT_FLAT(cd.boneTransforms, btOff, btNum * 64);
            EXTRACT_FLAT(cd.boneOrder,      borOff, borNum * 8);
            if (v2Off != 0 && v2Num != 0) {
                EXTRACT_FLAT(cd.vals2, v2Off, v2Num * 42 * 4);
            }

            // Extract bone name STRINGS (not the table — we rebuild the table on write)
            // Table layout: [N u32 offsets] [N × (string_ptr, hash) pairs]
            // Each string_ptr → null-terminated string in Block1
            if (bnNum > 0 && bnOff + bnNum * 12 <= block1Size) {
                uint32_t pairsStart = bnOff + bnNum * 4;
                for (uint32_t j = 0; j < bnNum; ++j) {
                    uint32_t pairOff = pairsStart + j * 8;
                    if (pairOff + 8 > block1Size) break;
                    uint32_t strPtr = ReadU32(block1 + pairOff);
                    uint32_t strHash = ReadU32(block1 + pairOff + 4);
                    if (strPtr == 0 || strPtr >= block1Size) {
                        // Empty entry — add a null byte
                        cd.nameStrings.push_back(0);
                        // pad to 4
                        while (cd.nameStrings.size() % 4 != 0) cd.nameStrings.push_back(0);
                        continue;
                    }
                    // Copy string bytes including null terminator
                    uint32_t strEnd = strPtr;
                    while (strEnd < block1Size && block1[strEnd] != 0) strEnd++;
                    uint32_t strLen = strEnd - strPtr; // not including null
                    cd.nameStrings.insert(cd.nameStrings.end(),
                        block1 + strPtr, block1 + strPtr + strLen);
                    cd.nameStrings.push_back(0); // null terminator
                    // pad to 4-byte align
                    while (cd.nameStrings.size() % 4 != 0) cd.nameStrings.push_back(0);
                }
            }
        }
    }

    // --- Model: parse each sub-field individually (like Rust Model::from_bytes) ---
    #define EXTRACT(dst, off, sz) \
        if ((off) != 0 && (sz) > 0 && (off)+(sz) <= block1Size) \
            (dst).assign(block1 + (off), block1 + (off) + (sz))

    {
        uint32_t miBase = hdr.model_info_offset;
        uint32_t miNum  = hdr.model_info_num;
        out.modelData.resize(miNum);

        for (uint32_t i = 0; i < miNum; ++i) {
            uint32_t mi = miBase + i * 256;
            if (mi + 256 > block1Size) break;

            PakParsedData::ModelSubData& md = out.modelData[i];

            uint32_t bonesNum = ReadU32(block1 + mi + 148);
            uint32_t matNum   = ReadU32(block1 + mi + 132);
            uint32_t skinNum  = ReadU32(block1 + mi + 156);
            uint32_t vbNum    = ReadU32(block1 + mi + 168);
            uint32_t ibNum    = ReadU32(block1 + mi + 176);
            uint32_t breakEnd = ReadU32(block1 + mi + 52 + 60 + 16); // lod3.breakable_end
            uint32_t maxBp    = (bonesNum > 4) ? bonesNum : 4;

            // Simple flat arrays
            EXTRACT(md.boneParents,    ReadU32(block1+mi+140), maxBp * 4);
            EXTRACT(md.boneKeys,       ReadU32(block1+mi+136), bonesNum * 4);
            EXTRACT(md.boneTransforms, ReadU32(block1+mi+144), bonesNum * 64);
            EXTRACT(md.boneBBs,        ReadU32(block1+mi+252), bonesNum * 32);
            EXTRACT(md.mats,           ReadU32(block1+mi+8),   matNum * 4);
            EXTRACT(md.meshOrder,      ReadU32(block1+mi+48),  breakEnd * 4);
            EXTRACT(md.meshBBs,        ReadU32(block1+mi+180), breakEnd * 32);
            EXTRACT(md.vbuffRefs,      ReadU32(block1+mi+164), vbNum * 4);
            EXTRACT(md.ibuffRefs,      ReadU32(block1+mi+172), ibNum * 4);
            EXTRACT(md.skinBinds,      ReadU32(block1+mi+152), skinNum * 64);

            // vals_j
            uint32_t vjNum = ReadU32(block1 + mi + 192);
            uint32_t vjOff = ReadU32(block1 + mi + 196);
            if (vjNum > 0) {
                EXTRACT(md.valsJ, vjOff, vjNum * 4);
            }

            // vals_k (4 bytes header + 35 × 4 = 144 bytes total)
            uint32_t vkOff = ReadU32(block1 + mi + 204);
            if (vkOff != 0) {
                EXTRACT(md.valsK, vkOff, 4 + 35 * 4);
            }

            // skin_order
            uint32_t soOff = ReadU32(block1 + mi + 160);
            if (soOff != 0 && skinNum > 0) {
                EXTRACT(md.skinOrder, soOff, skinNum * 4);
            }

            // slots (zero-terminated key pairs) + slot_map
            uint32_t slOff = ReadU32(block1 + mi + 244);
            uint32_t smOff = ReadU32(block1 + mi + 248);
            if (slOff != 0 && slOff + 8 <= block1Size) {
                uint32_t sc = 0;
                while (slOff + sc * 8 + 4 <= block1Size &&
                       ReadU32(block1 + slOff + sc * 8) != 0) sc++;
                sc++; // include terminator
                EXTRACT(md.slots, slOff, sc * 8);
                if (sc > 0 && smOff != 0) {
                    uint32_t lastKey = ReadU32(block1 + slOff + (sc * 2 - 1) * 4);
                    if (lastKey > 0) {
                        EXTRACT(md.slotMap, smOff, lastKey * 4);
                    }
                }
            }

            // Physics blocks: parse INDIVIDUALLY (not as one blob)
            uint32_t blkOff = ReadU32(block1 + mi + 200);
            if (blkOff != 0 && blkOff + 4 <= block1Size) {
                uint32_t physStart = ReadU32(block1 + mi + 52 + 8);  // lod0.skinned_end
                uint32_t physEnd   = ReadU32(block1 + mi + 52 + 12); // lod0.physics_end
                uint32_t n = physEnd - physStart;
                md.blockHeader = ReadU32(block1 + blkOff);
                if (n > 0 && blkOff + 4 + (n + 1) * 4 <= block1Size) {
                    md.blockOffsets.resize(n + 1);
                    for (uint32_t j = 0; j <= n; ++j)
                        md.blockOffsets[j] = ReadU32(block1 + blkOff + 4 + j * 4);
                    // Extract each block individually
                    for (uint32_t j = 0; j < n; ++j) {
                        uint32_t bStart = md.blockOffsets[j] + blkOff;
                        uint32_t bEnd   = md.blockOffsets[j + 1] + blkOff;
                        uint32_t bSize  = bEnd - bStart;
                        if (bStart + bSize <= block1Size) {
                            md.blocks.push_back(std::vector<uint8_t>(
                                block1 + bStart, block1 + bStart + bSize));
                        }
                    }
                }
            }

            // Padding value (special case: model with no mesh data)
            uint32_t moOff = ReadU32(block1 + mi + 48);
            uint32_t vbOff2 = ReadU32(block1 + mi + 164);
            uint32_t ibOff2 = ReadU32(block1 + mi + 172);
            uint32_t mbOff = ReadU32(block1 + mi + 180);
            if (moOff == vbOff2 && moOff == ibOff2 && moOff == mbOff && moOff != 0) {
                EXTRACT(md.paddingVal, moOff, 16); // 4 u32s
            }
        }
    }
    #undef EXTRACT

    return true;
}

// ============================================================
//  Phase 4: Rebuild — write Model, HkShape, HkConstraint data
// ============================================================

bool RebuildBlock1Phase4(
    std::vector<uint8_t>& newBlock1,
    PakHeader&            hdr,
    const PakParsedData&  parsed,
    uint32_t&             dataWritePos,
    std::string&          errOut)
{
    bool freshLayout = (dataWritePos > 0);

    // --- HkShape: write collision data ---
    {
        uint32_t base = hdr.hk_shape_info_offset;
        uint32_t num  = hdr.hk_shape_info_num;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.hkShapeData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.hkShapeData[i];
            if (data.empty()) continue;

            uint32_t e = base + i * 80;
            if (e + 80 > (uint32_t)newBlock1.size()) continue;

            uint32_t kind = ReadU32(&newBlock1[e + 32]);

            // Compute original start from the entry's offset fields
            uint32_t origStart = 0xFFFFFFFFu;
            if (kind == 5) {
                uint32_t a = ReadU32(&newBlock1[e + 44]);
                uint32_t b = ReadU32(&newBlock1[e + 52]);
                origStart = (a < b) ? a : b;
            } else if (kind == 6) {
                uint32_t c = ReadU32(&newBlock1[e + 60]);
                uint32_t d = ReadU32(&newBlock1[e + 68]);
                uint32_t ev = ReadU32(&newBlock1[e + 76]);
                origStart = c; if (d < origStart) origStart = d; if (ev < origStart) origStart = ev;
            }

            uint32_t writeAt = freshLayout ? dataWritePos : origStart;
            uint32_t dataSize = (uint32_t)data.size();
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);

            // Update offset fields: delta = writeAt - origStart
            if (freshLayout && origStart != 0xFFFFFFFFu) {
                int32_t delta = (int32_t)writeAt - (int32_t)origStart;
                if (kind == 5) {
                    uint32_t a = ReadU32(&newBlock1[e + 44]) + delta;
                    uint32_t b = ReadU32(&newBlock1[e + 52]) + delta;
                    WriteU32(&newBlock1[e + 44], a);
                    WriteU32(&newBlock1[e + 52], b);
                } else if (kind == 6) {
                    uint32_t c = ReadU32(&newBlock1[e + 60]) + delta;
                    uint32_t d = ReadU32(&newBlock1[e + 68]) + delta;
                    uint32_t ev = ReadU32(&newBlock1[e + 76]) + delta;
                    WriteU32(&newBlock1[e + 60], c);
                    WriteU32(&newBlock1[e + 68], d);
                    WriteU32(&newBlock1[e + 76], ev);
                }
                dataWritePos = writeAt + dataSize;
            }
        }
    }

    // --- HkConstraint: write constraint data ---
    {
        uint32_t base   = hdr.hk_constraint_info_offset;
        uint32_t num    = hdr.hk_constraint_info_num;
        uint32_t stride = hdr.hk_constraint_info_size;

        for (uint32_t i = 0; i < num && i < (uint32_t)parsed.hkConstraintData.size(); ++i) {
            const std::vector<uint8_t>& data = parsed.hkConstraintData[i];
            if (data.empty()) continue;

            uint32_t e = base + i * stride;
            if (e + stride > (uint32_t)newBlock1.size()) continue;

            // Find original min offset from the 6 offset fields
            uint32_t origStart = 0xFFFFFFFFu;
            uint32_t offsets[6] = {
                ReadU32(&newBlock1[e + 4]),  ReadU32(&newBlock1[e + 12]),
                ReadU32(&newBlock1[e + 20]), ReadU32(&newBlock1[e + 40]),
                ReadU32(&newBlock1[e + 48]), ReadU32(&newBlock1[e + 64])
            };
            for (int k = 0; k < 6; ++k)
                if (offsets[k] != 0 && offsets[k] < origStart) origStart = offsets[k];

            uint32_t writeAt = freshLayout ? dataWritePos : origStart;
            uint32_t dataSize = (uint32_t)data.size();
            if (writeAt + dataSize > (uint32_t)newBlock1.size())
                newBlock1.resize(writeAt + dataSize);

            memcpy(&newBlock1[writeAt], &data[0], dataSize);

            if (freshLayout && origStart != 0xFFFFFFFFu) {
                int32_t delta = (int32_t)writeAt - (int32_t)origStart;
                static const uint32_t flds[] = {4, 12, 20, 40, 48, 64};
                for (int k = 0; k < 6; ++k) {
                    uint32_t v = ReadU32(&newBlock1[e + flds[k]]);
                    if (v != 0) WriteU32(&newBlock1[e + flds[k]], v + delta);
                }
                dataWritePos = writeAt + dataSize;
            }
        }
    }

    return true;
}

// ============================================================
//  Phase 5: FULL Block1 rebuild from scratch
// ============================================================

// Helper: append raw bytes to buffer, return start offset
static uint32_t AppendBytes(std::vector<uint8_t>& buf,
                            const uint8_t* src, uint32_t size) {
    uint32_t pos = (uint32_t)buf.size();
    buf.insert(buf.end(), src, src + size);
    return pos;
}

// Helper: append raw vector to buffer, return start offset
static uint32_t AppendVec(std::vector<uint8_t>& buf,
                          const std::vector<uint8_t>& src) {
    if (src.empty()) return (uint32_t)buf.size();
    return AppendBytes(buf, &src[0], (uint32_t)src.size());
}

// Helper: align buffer size to boundary
static void AlignBuffer(std::vector<uint8_t>& buf, uint32_t align) {
    uint32_t aligned = ((uint32_t)buf.size() + align - 1) & ~(align - 1);
    buf.resize(aligned, 0);
}

// Helper: copy sub-array from origBlock1 to newBlock1, update offset field in info entry.
// Returns new offset written. Reads origOffset from the info entry in newBlock1 at (infoOff + fieldByte).
static uint32_t CopySubArray(std::vector<uint8_t>& newBlock1,
                             const std::vector<uint8_t>& origBlock1,
                             uint32_t infoOff, uint32_t fieldByte,
                             uint32_t dataSize) {
    if (dataSize == 0) return 0;
    uint32_t origOffset = ReadU32(&newBlock1[infoOff + fieldByte]);
    if (origOffset == 0 || origOffset == 0xFFFFFFFFu) return origOffset;
    if (origOffset + dataSize > (uint32_t)origBlock1.size()) return origOffset;

    uint32_t newOffset = (uint32_t)newBlock1.size();
    newBlock1.insert(newBlock1.end(),
                     origBlock1.begin() + origOffset,
                     origBlock1.begin() + origOffset + dataSize);
    WriteU32(&newBlock1[infoOff + fieldByte], newOffset);
    return newOffset;
}

// ============================================================
//  BuildBlock2Offsets: compute block2_offsets from new Block1 layout.
//  block2_offsets = list of Block1 byte positions that contain pointer fields.
//  The game loader uses these at runtime to patch file offsets → memory addresses.
//  Must match the Rust parser's level_alt.rs:642-760 exactly.
// ============================================================
void BuildBlock2Offsets(
    const std::vector<uint8_t>& block1,
    const PakHeader&            hdr,
    std::vector<uint32_t>&      out)
{
    out.clear();

    // --- ModelInfo pointer fields ---
    for (uint32_t i = 0; i < hdr.model_info_num; ++i) {
        uint32_t mi = hdr.model_info_offset + i * 256;
        if (mi + 256 > (uint32_t)block1.size()) break;

        // 10 unconditional
        out.push_back(mi + 8);   // mat_offset
        out.push_back(mi + 12);  // buffer_info_offset
        out.push_back(mi + 48);  // mesh_order_offset
        out.push_back(mi + 140); // bone_parents_offset
        out.push_back(mi + 144); // bone_transforms_offset
        out.push_back(mi + 152); // skin_binds_offset
        out.push_back(mi + 164); // vbuff_offset
        out.push_back(mi + 172); // ibuff_offset
        out.push_back(mi + 180); // mesh_bbs_offset
        out.push_back(mi + 252); // bone_bbs_offset

        // conditional (only if non-zero)
        if (ReadU32(&block1[mi + 136]) != 0) out.push_back(mi + 136); // bones_offset
        if (ReadU32(&block1[mi + 160]) != 0) out.push_back(mi + 160); // skin_order_offset
        if (ReadU32(&block1[mi + 196]) != 0) out.push_back(mi + 196); // vals_j_offset
        if (ReadU32(&block1[mi + 200]) != 0) out.push_back(mi + 200); // block_offset (physics)
        if (ReadU32(&block1[mi + 204]) != 0) out.push_back(mi + 204); // vals_k_offset
        if (ReadU32(&block1[mi + 224]) != 0) out.push_back(mi + 224); // shape_offset
        if (ReadU32(&block1[mi + 232]) != 0) out.push_back(mi + 232); // hk_constraint_data_offset
        if (ReadU32(&block1[mi + 240]) != 0) out.push_back(mi + 240); // hk_constraint_offset
        if (ReadU32(&block1[mi + 244]) != 0) out.push_back(mi + 244); // slots_offset
        if (ReadU32(&block1[mi + 248]) != 0) out.push_back(mi + 248); // slot_map_offset
    }

    // --- Model sub-data arrays: each entry in mats/vbuffs/ibuffs is a pointer ---
    for (uint32_t i = 0; i < hdr.model_info_num; ++i) {
        uint32_t mi = hdr.model_info_offset + i * 256;
        if (mi + 256 > (uint32_t)block1.size()) break;
        uint32_t matOff = ReadU32(&block1[mi + 8]);
        uint32_t matNum = ReadU32(&block1[mi + 132]);
        for (uint32_t j = 0; j < matNum; ++j) {
            uint32_t a = matOff + j * 4;
            if (a + 4 <= (uint32_t)block1.size()) out.push_back(a);
        }
        uint32_t vbOff = ReadU32(&block1[mi + 164]);
        uint32_t vbNum = ReadU32(&block1[mi + 168]);
        for (uint32_t j = 0; j < vbNum; ++j) {
            uint32_t a = vbOff + j * 4;
            if (a + 4 <= (uint32_t)block1.size()) out.push_back(a);
        }
        uint32_t ibOff = ReadU32(&block1[mi + 172]);
        uint32_t ibNum = ReadU32(&block1[mi + 176]);
        for (uint32_t j = 0; j < ibNum; ++j) {
            uint32_t a = ibOff + j * 4;
            if (a + 4 <= (uint32_t)block1.size()) out.push_back(a);
        }
    }

    // --- BufferInfo pointer fields ---
    for (uint32_t i = 0; i < hdr.buffer_info_num; ++i) {
        uint32_t bi = hdr.buffer_info_offset + i * hdr.buffer_info_size;
        if (bi + hdr.buffer_info_size > (uint32_t)block1.size()) break;

        out.push_back(bi + 0);   // vbuff_info_offset (unconditional)
        out.push_back(bi + 260); // ibuff_info_offset (unconditional)
        if (ReadU32(&block1[bi + 4]) != 0) out.push_back(bi + 4);  // vbuff_info_offset_2
        if (ReadU32(&block1[bi + 8]) != 0) out.push_back(bi + 8);  // vbuff_info_offset_3
    }

    // --- Mat1-4 mat_extra_offset (byte 344, conditional) ---
    struct MatArr { uint32_t off, num, sz; };
    MatArr mats[4] = {
        {hdr.mat1_offset, hdr.mat1_num, hdr.mat1_size},
        {hdr.mat2_offset, hdr.mat2_num, hdr.mat2_size},
        {hdr.mat3_offset, hdr.mat3_num, hdr.mat3_size},
        {hdr.mat4_offset, hdr.mat4_num, hdr.mat4_size}
    };
    for (int m = 0; m < 4; ++m) {
        for (uint32_t i = 0; i < mats[m].num; ++i) {
            uint32_t a = mats[m].off + i * mats[m].sz + 344;
            if (a + 4 > (uint32_t)block1.size()) break;
            if (ReadU32(&block1[a]) != 0) out.push_back(a);
        }
    }

    // --- ShapeInfo byte 112 (conditional) ---
    for (uint32_t i = 0; i < hdr.shape_info_num; ++i) {
        uint32_t si = hdr.shape_info_offset + i * hdr.shape_info_size;
        if (si + 116 > (uint32_t)block1.size()) break;
        if (ReadU32(&block1[si + 112]) != 0) out.push_back(si + 112);
    }

    // --- HkShapeInfo: kind-dependent offset fields ---
    for (uint32_t i = 0; i < hdr.hk_shape_info_num; ++i) {
        uint32_t e = hdr.hk_shape_info_offset + i * hdr.hk_shape_info_size;
        if (e + 80 > (uint32_t)block1.size()) break;
        uint32_t kind = ReadU32(&block1[e + 32]);
        if (kind == 5) { // ConvexVertices
            out.push_back(e + 44);
            out.push_back(e + 52);
        } else if (kind == 6) { // BVTreeMesh
            out.push_back(e + 60);
            out.push_back(e + 68);
            out.push_back(e + 76);
        }
    }

    // --- HkConstraintInfo: 5 unconditional + 1 conditional ---
    for (uint32_t i = 0; i < hdr.hk_constraint_info_num; ++i) {
        uint32_t ci = hdr.hk_constraint_info_offset + i * hdr.hk_constraint_info_size;
        if (ci + hdr.hk_constraint_info_size > (uint32_t)block1.size()) break;
        out.push_back(ci + 4);
        out.push_back(ci + 12);
        out.push_back(ci + 20);
        out.push_back(ci + 40);
        out.push_back(ci + 48);
        if (ReadU32(&block1[ci + 64]) != 0) out.push_back(ci + 64); // vals2_offset
    }

    // --- EffectInfo byte 8 ---
    for (uint32_t i = 0; i < hdr.effect_info_num; ++i) {
        out.push_back(hdr.effect_info_offset + i * 16 + 8);
    }

    // --- GFXBlockInfo byte 4 ---
    for (uint32_t i = 0; i < hdr.gfx_block_info_num; ++i) {
        out.push_back(hdr.gfx_block_info_offset + i * 12 + 4);
    }

    // --- RadiosityValsInfo byte 8 ---
    for (uint32_t i = 0; i < hdr.radiosity_vals_info_num; ++i) {
        out.push_back(hdr.radiosity_vals_info_offset + i * hdr.radiosity_vals_info_size + 8);
    }

    // --- FoliageInfo byte 28 ---
    for (uint32_t i = 0; i < hdr.foliage_info_num; ++i) {
        out.push_back(hdr.foliage_info_offset + i * hdr.foliage_info_size + 28);
    }

    // --- HkConstraintInfo: internal bone name pointers ---
    // bone_names_offset → N u32 offsets + N × (string_ptr, hash) pairs
    // Each u32 offset and each string_ptr is a Block1 pointer
    for (uint32_t i = 0; i < hdr.hk_constraint_info_num; ++i) {
        uint32_t ci = hdr.hk_constraint_info_offset + i * hdr.hk_constraint_info_size;
        if (ci + hdr.hk_constraint_info_size > (uint32_t)block1.size()) break;
        uint32_t bnOff = ReadU32(&block1[ci + 12]); // bone_names_offset
        uint32_t bnNum = ReadU32(&block1[ci + 16]); // bone_names_num
        if (bnNum > 0 && bnOff + bnNum * 12 <= (uint32_t)block1.size()) {
            // N u32 offsets
            for (uint32_t j = 0; j < bnNum; ++j)
                out.push_back(bnOff + j * 4);
            // N string_ptr values in pairs (at bnOff + 4*N + j*8)
            uint32_t pairsStart = bnOff + bnNum * 4;
            for (uint32_t j = 0; j < bnNum; ++j)
                out.push_back(pairsStart + j * 8);
        }
    }

    RLog("  [Block2Offsets] Built %u entries\n", (unsigned)out.size());
}

// ============================================================
//  RebuildBlock1Full — Clean implementation based on game loading model:
//
//  Block1 is ONE monolithic buffer. The game does ptr = &block1[offset].
//  All offsets are Block1-relative indices. No runtime fixup loop.
//  Data accessed via MOVAPS must be at 16-byte aligned Block1 offsets.
//
//  Pipeline:
//    Step 1: Write all 23 info arrays sequentially (16-byte aligned)
//    Step 2: Write all variable-length data at fresh positions
//            (model sub-data, shapes, HkShapes, HkConstraints, effects, etc.)
//    Step 3: Remap info-to-info cross-references
//            (ModelInfo→BufferInfo, BufferInfo→VBuffInfo, ShapeInfo→HkShapeInfo, etc.)
//    Step 4: Append sub_blocks + string_keys
//
//  Every offset field is set at write time. No deltas, no blobs, no patching.
// ============================================================
bool RebuildBlock1Full(
    const std::vector<uint8_t>& origBlock1,
    const PakHeader&            origHdr,
    const PakParsedData&        parsed,
    std::vector<uint8_t>&       newBlock1,
    PakHeader&                  newHdr,
    const std::vector<uint8_t>& goPayload,
    uint32_t                    goKey,
    const std::vector<SubBlockEntry>& subBlocks1,
    std::string&                errOut)
{
    newHdr = origHdr;
    newBlock1.clear();
    newBlock1.reserve(origHdr.sub_blocks1_offset + 1024 * 1024);

    // ================================================================
    //  Step 1: Write all 23 info arrays sequentially (16-byte aligned)
    //  Step 2: Write all variable-length data at fresh positions
    //  Step 3: Remap info-to-info cross-references
    //  Step 4: Append sub_blocks + string_keys
    // ================================================================

    // ============================================================
    //  STEP 1: Pack all 23 info arrays sequentially, 16-byte aligned.
    //  Each array's header offset is set to its position in newBlock1.
    //  The raw bytes still contain OLD offset values — fixed in Step 2+3.
    // ============================================================
    RLog("  [Rebuild] Step 1: packing info arrays\n");

    #define WRITE_ARRAY(rawField, offField, numField, szField) \
    { \
        newHdr.offField = (uint32_t)newBlock1.size(); \
        if (!parsed.rawField.empty()) { \
            newHdr.numField = (uint32_t)parsed.rawField.size() / origHdr.szField; \
            newBlock1.insert(newBlock1.end(), parsed.rawField.begin(), parsed.rawField.end()); \
        } \
        AlignBuffer(newBlock1, 16); \
    }
    WRITE_ARRAY(rawObjA, obja_offset, obja_num, obja_size);
    WRITE_ARRAY(rawObj0, obj0_offset, obj0_num, obj0_size);
    WRITE_ARRAY(rawModelInfo, model_info_offset, model_info_num, model_info_size);
    WRITE_ARRAY(rawBufferInfo, buffer_info_offset, buffer_info_num, buffer_info_size);
    WRITE_ARRAY(rawMat1, mat1_offset, mat1_num, mat1_size);
    WRITE_ARRAY(rawMat2, mat2_offset, mat2_num, mat2_size);
    WRITE_ARRAY(rawMat3, mat3_offset, mat3_num, mat3_size);
    WRITE_ARRAY(rawMat4, mat4_offset, mat4_num, mat4_size);
    WRITE_ARRAY(rawMatExtra, mat_extra_offset, mat_extra_num, mat_extra_size);
    WRITE_ARRAY(rawShapeInfo, shape_info_offset, shape_info_num, shape_info_size);
    WRITE_ARRAY(rawHkShapeInfo, hk_shape_info_offset, hk_shape_info_num, hk_shape_info_size);
    WRITE_ARRAY(rawHkConstraintData, hk_constraint_data_offset, hk_constraint_data_num, hk_constraint_data_size);
    { newHdr.vbuff_info_offset = (uint32_t)newBlock1.size();
      if (!parsed.vbuffInfoRaw.empty()) { newHdr.vbuff_info_num = (uint32_t)parsed.vbuffInfoRaw.size() / origHdr.vbuff_info_size;
        newBlock1.insert(newBlock1.end(), parsed.vbuffInfoRaw.begin(), parsed.vbuffInfoRaw.end()); }
      AlignBuffer(newBlock1, 16); }
    { newHdr.ibuff_info_offset = (uint32_t)newBlock1.size();
      if (!parsed.ibuffInfoRaw.empty()) { newHdr.ibuff_info_num = (uint32_t)parsed.ibuffInfoRaw.size() / origHdr.ibuff_info_size;
        newBlock1.insert(newBlock1.end(), parsed.ibuffInfoRaw.begin(), parsed.ibuffInfoRaw.end()); }
      AlignBuffer(newBlock1, 16); }
    WRITE_ARRAY(rawTextureInfo, texture_info_offset, texture_info_num, texture_info_size);
    WRITE_ARRAY(rawAnimationInfo, animation_info_offset, animation_info_num, animation_info_size);
    WRITE_ARRAY(rawHkConstraintInfo, hk_constraint_info_offset, hk_constraint_info_num, hk_constraint_info_size);
    WRITE_ARRAY(rawEffectInfo, effect_info_offset, effect_info_num, effect_info_size);
    WRITE_ARRAY(rawPFieldInfo, pfield_info_offset, pfield_info_num, pfield_info_size);
    WRITE_ARRAY(rawGFXBlockInfo, gfx_block_info_offset, gfx_block_info_num, gfx_block_info_size);
    WRITE_ARRAY(rawAnimationBlockInfo, animation_block_info_offset, animation_block_info_num, animation_block_info_size);
    WRITE_ARRAY(rawFoliageInfo, foliage_info_offset, foliage_info_num, foliage_info_size);
    WRITE_ARRAY(rawRadiosityValsInfo, radiosity_vals_info_offset, radiosity_vals_info_num, radiosity_vals_info_size);
    #undef WRITE_ARRAY

    uint32_t infoEnd = (uint32_t)newBlock1.size();
    RLog("  [Rebuild] Step 1 done: info arrays = %u bytes\n", infoEnd);

    // ============================================================
    //  STEP 2: Copy entire data region verbatim from origBlock1.
    //  The data region = everything between info arrays end and sub_blocks1.
    //  A uniform delta is applied to ALL offset fields in Step 3.
    //  This approach is PROVEN by Python roundtrip test.
    // ============================================================
    RLog("  [Rebuild] Step 2: copying data region verbatim\n");

    // Compute original data region: from end of original info arrays to sub_blocks1_offset
    uint32_t origDataStart = origHdr.sub_blocks1_offset; // will be minimized below
    {
        // Find the actual end of info arrays in the original Block1
        // (smallest offset past all info array ends)
        struct ArrInfo { uint32_t off, num, sz; };
        ArrInfo arrs[] = {
            {origHdr.obja_offset, origHdr.obja_num, origHdr.obja_size},
            {origHdr.obj0_offset, origHdr.obj0_num, origHdr.obj0_size},
            {origHdr.model_info_offset, origHdr.model_info_num, origHdr.model_info_size},
            {origHdr.buffer_info_offset, origHdr.buffer_info_num, origHdr.buffer_info_size},
            {origHdr.mat1_offset, origHdr.mat1_num, origHdr.mat1_size},
            {origHdr.mat2_offset, origHdr.mat2_num, origHdr.mat2_size},
            {origHdr.mat3_offset, origHdr.mat3_num, origHdr.mat3_size},
            {origHdr.mat4_offset, origHdr.mat4_num, origHdr.mat4_size},
            {origHdr.mat_extra_offset, origHdr.mat_extra_num, origHdr.mat_extra_size},
            {origHdr.shape_info_offset, origHdr.shape_info_num, origHdr.shape_info_size},
            {origHdr.hk_shape_info_offset, origHdr.hk_shape_info_num, origHdr.hk_shape_info_size},
            {origHdr.hk_constraint_data_offset, origHdr.hk_constraint_data_num, origHdr.hk_constraint_data_size},
            {origHdr.vbuff_info_offset, origHdr.vbuff_info_num, origHdr.vbuff_info_size},
            {origHdr.ibuff_info_offset, origHdr.ibuff_info_num, origHdr.ibuff_info_size},
            {origHdr.texture_info_offset, origHdr.texture_info_num, origHdr.texture_info_size},
            {origHdr.animation_info_offset, origHdr.animation_info_num, origHdr.animation_info_size},
            {origHdr.hk_constraint_info_offset, origHdr.hk_constraint_info_num, origHdr.hk_constraint_info_size},
            {origHdr.effect_info_offset, origHdr.effect_info_num, origHdr.effect_info_size},
            {origHdr.pfield_info_offset, origHdr.pfield_info_num, origHdr.pfield_info_size},
            {origHdr.gfx_block_info_offset, origHdr.gfx_block_info_num, origHdr.gfx_block_info_size},
            {origHdr.animation_block_info_offset, origHdr.animation_block_info_num, origHdr.animation_block_info_size},
            {origHdr.foliage_info_offset, origHdr.foliage_info_num, origHdr.foliage_info_size},
            {origHdr.radiosity_vals_info_offset, origHdr.radiosity_vals_info_num, origHdr.radiosity_vals_info_size},
        };
        uint32_t maxEnd = 0;
        for (int k = 0; k < 23; ++k) {
            if (arrs[k].num > 0) {
                uint32_t end = arrs[k].off + arrs[k].num * arrs[k].sz;
                if (end > maxEnd) maxEnd = end;
            }
        }
        origDataStart = (maxEnd + 15) & ~15u; // 16-byte align
    }
    uint32_t origDataEnd = origHdr.sub_blocks1_offset;

    // Pad so dataDelta is a multiple of 16.
    // This preserves ALL original alignment — data that was 16-byte aligned
    // stays 16-byte aligned. No MOVAPS crashes.
    {
        uint32_t curMod = (uint32_t)newBlock1.size() % 16;
        uint32_t origMod = origDataStart % 16;
        uint32_t pad = (origMod >= curMod) ? (origMod - curMod) : (16 - curMod + origMod);
        if (pad < 16) newBlock1.resize(newBlock1.size() + pad, 0);
    }

    // Copy data region verbatim
    int32_t dataDelta = (int32_t)newBlock1.size() - (int32_t)origDataStart;
    if (origDataStart < origDataEnd && origDataEnd <= (uint32_t)origBlock1.size()) {
        newBlock1.insert(newBlock1.end(),
            origBlock1.begin() + origDataStart,
            origBlock1.begin() + origDataEnd);
    }
    RLog("  [Rebuild] Data region: orig [%u..%u] (%u bytes), delta=%+d\n",
         origDataStart, origDataEnd, origDataEnd - origDataStart, dataDelta);

    // ---- REMOVED: per-field Step 2a-2d code ----
    // The entire data region is copied verbatim. All offset fixups happen in Step 3.

    // ---- OLD Step 2a-2d removed — data copied verbatim above ----
    #if 0 // BEGIN REMOVED PER-FIELD CODE
    // Write in Rust parser order with proper alignment.
    // WRITE_FIELD: align to 16, write data at cursor, set offset.
    // The game uses MOVAPS (SSE) on many data types — all must be 16-byte aligned.
    #define WRITE_FIELD(field, miOff, fieldByte) \
    { \
        AlignBuffer(newBlock1, 16); \
        uint32_t pos = (uint32_t)newBlock1.size(); \
        if (!md.field.empty()) \
            newBlock1.insert(newBlock1.end(), md.field.begin(), md.field.end()); \
        WriteU32(&newBlock1[miOff + fieldByte], pos); \
    }
    // WRITE_FIELD_COND: conditional — set to 0 if empty.
    #define WRITE_FIELD_COND(field, miOff, fieldByte) \
    { \
        if (!md.field.empty()) { \
            WriteU32(&newBlock1[miOff + fieldByte], (uint32_t)newBlock1.size()); \
            newBlock1.insert(newBlock1.end(), md.field.begin(), md.field.end()); \
        } else { \
            WriteU32(&newBlock1[miOff + fieldByte], 0); \
        } \
    }
    {
        uint32_t miBase = newHdr.model_info_offset;
        uint32_t miNum  = newHdr.model_info_num;
        for (uint32_t i = 0; i < miNum && i < (uint32_t)parsed.modelData.size(); ++i) {
            uint32_t mi = miBase + i * 256;
            if (mi + 256 > (uint32_t)newBlock1.size()) break;
            const PakParsedData::ModelSubData& md = parsed.modelData[i];

            // Rust order from pak_alt/model.rs dump():
            // All WRITE_FIELDs auto-align to 16 before writing.
            WRITE_FIELD(boneKeys,       mi, 136);  // 1. bones (Key2)
            WRITE_FIELD(boneBBs,        mi, 252);  // 2. bone_bounding_boxes
            WRITE_FIELD(valsJ,          mi, 196);  // 3. vals_j
            // 4. (hk_constraint — handled in Step 2c)
            WRITE_FIELD(skinBinds,      mi, 152);  // 5. skin_binds
            WRITE_FIELD_COND(skinOrder, mi, 160);  // 6. skin_order (conditional)
            WRITE_FIELD(boneParents,    mi, 140);  // 7. bone_parents
            WRITE_FIELD(boneTransforms, mi, 144);  // 8. bone_transforms
            WRITE_FIELD(mats,           mi, 8);    // 9. mats
            // 10. (shapes — handled in Step 2b)
            WRITE_FIELD(meshOrder,      mi, 48);   // 11. mesh_order
            WRITE_FIELD(meshBBs,        mi, 180);  // 12. mesh_bounding_boxes
            WRITE_FIELD(vbuffRefs,      mi, 164);  // 13. vbuff_refs
            WRITE_FIELD(ibuffRefs,      mi, 172);  // 14. ibuff_refs
            // 15. vals_k (conditional)
            if (!md.valsK.empty()) {
                AlignBuffer(newBlock1, 16);
                WriteU32(&newBlock1[mi + 204], (uint32_t)newBlock1.size());
                newBlock1.insert(newBlock1.end(), md.valsK.begin(), md.valsK.end());
            } else { WriteU32(&newBlock1[mi + 204], 0); }
            // 16. slots + slot_map (conditional)
            if (!md.slots.empty()) {
                WriteU32(&newBlock1[mi + 244], (uint32_t)newBlock1.size());
                newBlock1.insert(newBlock1.end(), md.slots.begin(), md.slots.end());
                WriteU32(&newBlock1[mi + 248], (uint32_t)newBlock1.size());
                newBlock1.insert(newBlock1.end(), md.slotMap.begin(), md.slotMap.end());
            } else {
                WriteU32(&newBlock1[mi + 244], 0);
                WriteU32(&newBlock1[mi + 248], 0);
            }
            // 17. Physics blocks (conditional, align before)
            if (!md.blockOffsets.empty() && !md.blocks.empty()) {
                AlignBuffer(newBlock1, 16);
                WriteU32(&newBlock1[mi + 200], (uint32_t)newBlock1.size());
                uint32_t hdrVal = md.blockHeader;
                newBlock1.insert(newBlock1.end(), (uint8_t*)&hdrVal, (uint8_t*)&hdrVal + 4);
                uint32_t n = (uint32_t)md.blocks.size();
                uint32_t dataStart = 4 + (n + 1) * 4;
                std::vector<uint32_t> newOffsets(n + 1);
                uint32_t cursor = dataStart;
                for (uint32_t j = 0; j < n; ++j) { newOffsets[j] = cursor; cursor += (uint32_t)md.blocks[j].size(); }
                newOffsets[n] = cursor;
                newBlock1.insert(newBlock1.end(), (uint8_t*)&newOffsets[0], (uint8_t*)&newOffsets[0] + (n+1)*4);
                for (uint32_t j = 0; j < n; ++j)
                    newBlock1.insert(newBlock1.end(), md.blocks[j].begin(), md.blocks[j].end());
            } else { WriteU32(&newBlock1[mi + 200], 0); }
            // 18. Padding value (no-mesh-data model)
            if (!md.paddingVal.empty()) {
                uint32_t pos = (uint32_t)newBlock1.size();
                newBlock1.insert(newBlock1.end(), md.paddingVal.begin(), md.paddingVal.end());
                WriteU32(&newBlock1[mi + 48], pos);
                WriteU32(&newBlock1[mi + 164], pos);
                WriteU32(&newBlock1[mi + 172], pos);
                WriteU32(&newBlock1[mi + 180], pos);
            }
            // Align between models
            AlignBuffer(newBlock1, 16);
        }
    }
    #undef WRITE_FIELD
    #undef WRITE_FIELD_COND

    // ---- Step 2b: Shape + HkShape data ----
    // Shape data (kind=0): write at fresh position, set ShapeInfo.offset
    for (uint32_t i = 0; i < newHdr.shape_info_num && i < (uint32_t)parsed.shapeData.size(); ++i) {
        if (parsed.shapeData[i].empty()) continue;
        uint32_t si = newHdr.shape_info_offset + i * newHdr.shape_info_size;
        if (si + 4 > (uint32_t)newBlock1.size()) break;
        AlignBuffer(newBlock1, 16);
        WriteU32(&newBlock1[si + 0], (uint32_t)newBlock1.size());
        newBlock1.insert(newBlock1.end(), parsed.shapeData[i].begin(), parsed.shapeData[i].end());
    }
    // HkShape data: copy from origBlock1 at old offsets, write at fresh position.
    // CopySubArray reads old offset from info, copies from origBlock1, writes at end, updates offset.
    {
        uint32_t hkBase = newHdr.hk_shape_info_offset;
        uint32_t hkNum  = newHdr.hk_shape_info_num;
        uint32_t origHkNum = origHdr.hk_shape_info_num;
        for (uint32_t i = 0; i < hkNum; ++i) {
            uint32_t e = hkBase + i * 80;
            if (e + 80 > (uint32_t)newBlock1.size()) break;
            uint32_t kind = ReadU32(&newBlock1[e + 32]);
            if (kind == 5) { // ConvexVertices: planes + verts
                CopySubArray(newBlock1, origBlock1, e, 44, ReadU32(&newBlock1[e+40]) * 16);
                uint32_t bNum = ReadU32(&newBlock1[e+48]);
                uint32_t bOff = ReadU32(&newBlock1[e+52]);
                if (bNum > 0 && bOff < (uint32_t)origBlock1.size()) {
                    uint32_t bRaw = bNum * 12;
                    while ((bOff + bRaw) % 16 != 0) bRaw += 12;
                    CopySubArray(newBlock1, origBlock1, e, 52, bRaw);
                }
            } else if (kind == 6 && i < origHkNum) { // BVTreeMesh (existing)
                CopySubArray(newBlock1, origBlock1, e, 60, ReadU32(&newBlock1[e+56]));
                CopySubArray(newBlock1, origBlock1, e, 68, ReadU32(&newBlock1[e+64]) * 12);
                CopySubArray(newBlock1, origBlock1, e, 76, ReadU32(&newBlock1[e+72]) * 6);
            } else if (kind == 6 && i >= origHkNum && i < (uint32_t)parsed.hkShapeData.size()) {
                // NEW BVTreeMesh — data from parsed.hkShapeData blob
                const std::vector<uint8_t>& blob = parsed.hkShapeData[i];
                uint32_t cSz = ReadU32(&newBlock1[e+56]);
                uint32_t dSz = ReadU32(&newBlock1[e+64]) * 12;
                uint32_t eSz = ReadU32(&newBlock1[e+72]) * 6;
                uint32_t cur = 0;
                if (cSz > 0 && cur+cSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+60], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+cSz); cur += cSz; }
                if (dSz > 0 && cur+dSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+68], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+dSz); cur += dSz; }
                if (eSz > 0 && cur+eSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+76], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+eSz); }
            }
        }
    }

    // ---- Step 2c: HkConstraint data (per-field, no blob/delta) ----
    for (uint32_t i = 0; i < newHdr.hk_constraint_info_num && i < (uint32_t)parsed.hkConstraintSub.size(); ++i) {
        const PakParsedData::HkConstraintSubData& cd = parsed.hkConstraintSub[i];
        uint32_t e = newHdr.hk_constraint_info_offset + i * newHdr.hk_constraint_info_size;
        if (e + newHdr.hk_constraint_info_size > (uint32_t)newBlock1.size()) break;
        uint32_t bnNum = cd.boneNamesNum;

        // 1. Name table: reserve 12*N, fill after strings
        AlignBuffer(newBlock1, 16);
        uint32_t nameTablePos = (uint32_t)newBlock1.size();
        WriteU32(&newBlock1[e + 12], nameTablePos);
        newBlock1.resize(newBlock1.size() + bnNum * 12, 0);
        AlignBuffer(newBlock1, 16);
        // 2. Bone transforms (16-byte aligned for MOVAPS)
        WriteU32(&newBlock1[e + 20], (uint32_t)newBlock1.size());
        if (!cd.boneTransforms.empty())
            newBlock1.insert(newBlock1.end(), cd.boneTransforms.begin(), cd.boneTransforms.end());
        // 3. Bone order (Key2 = 8 bytes)
        WriteU32(&newBlock1[e + 48], (uint32_t)newBlock1.size());
        if (!cd.boneOrder.empty())
            newBlock1.insert(newBlock1.end(), cd.boneOrder.begin(), cd.boneOrder.end());
        // 4. Bone parents (i16 array)
        AlignBuffer(newBlock1, 16);
        WriteU32(&newBlock1[e + 4], (uint32_t)newBlock1.size());
        if (!cd.boneParents.empty())
            newBlock1.insert(newBlock1.end(), cd.boneParents.begin(), cd.boneParents.end());
        AlignBuffer(newBlock1, 4);
        // 5. Strings: write and fill name table
        {
            uint32_t strCursor = 0;
            for (uint32_t j = 0; j < bnNum; ++j) {
                uint32_t strStart = strCursor;
                while (strCursor < (uint32_t)cd.nameStrings.size() && cd.nameStrings[strCursor] != 0)
                    strCursor++;
                strCursor++; // skip null
                while (strCursor % 4 != 0 && strCursor < (uint32_t)cd.nameStrings.size())
                    strCursor++;
                uint32_t strPos = (uint32_t)newBlock1.size();
                if (strStart < (uint32_t)cd.nameStrings.size())
                    newBlock1.insert(newBlock1.end(),
                        cd.nameStrings.begin() + strStart, cd.nameStrings.begin() + strCursor);
                // Fill table: offset[j] → pair address, pair = (string_ptr, 0)
                uint32_t pairAddr = nameTablePos + bnNum * 4 + j * 8;
                WriteU32(&newBlock1[nameTablePos + j * 4], pairAddr);
                WriteU32(&newBlock1[nameTablePos + bnNum * 4 + j * 8], strPos);
                WriteU32(&newBlock1[nameTablePos + bnNum * 4 + j * 8 + 4], 0);
            }
        }
        // 6. Vals2 (conditional)
        if (!cd.vals2.empty()) {
            WriteU32(&newBlock1[e + 64], (uint32_t)newBlock1.size());
            newBlock1.insert(newBlock1.end(), cd.vals2.begin(), cd.vals2.end());
        } else { WriteU32(&newBlock1[e + 64], 0); }
        // byte 40 (bones_offset) → set in Step 3
    }

    // ---- Step 2d: Effect, GFXBlock, Foliage, RadiosityVals ----
    for (uint32_t i = 0; i < newHdr.effect_info_num && i < (uint32_t)parsed.effectData.size(); ++i) {
        if (parsed.effectData[i].empty()) continue;
        uint32_t io = newHdr.effect_info_offset + i * 16;
        if (io + 16 > (uint32_t)newBlock1.size()) break;
        WriteU32(&newBlock1[io+8], (uint32_t)newBlock1.size());
        WriteU32(&newBlock1[io+12], (uint32_t)parsed.effectData[i].size());
        newBlock1.insert(newBlock1.end(), parsed.effectData[i].begin(), parsed.effectData[i].end());
    }
    for (uint32_t i = 0; i < newHdr.gfx_block_info_num && i < (uint32_t)parsed.gfxBlockData.size(); ++i) {
        if (parsed.gfxBlockData[i].empty()) continue;
        uint32_t io = newHdr.gfx_block_info_offset + i * 12;
        if (io + 12 > (uint32_t)newBlock1.size()) break;
        WriteU32(&newBlock1[io+4], (uint32_t)newBlock1.size());
        WriteU32(&newBlock1[io+8], (uint32_t)parsed.gfxBlockData[i].size());
        newBlock1.insert(newBlock1.end(), parsed.gfxBlockData[i].begin(), parsed.gfxBlockData[i].end());
    }
    for (uint32_t i = 0; i < newHdr.foliage_info_num && i < (uint32_t)parsed.foliageData.size(); ++i) {
        if (parsed.foliageData[i].empty()) continue;
        uint32_t io = newHdr.foliage_info_offset + i * newHdr.foliage_info_size;
        if (io + 32 > (uint32_t)newBlock1.size()) break;
        WriteU32(&newBlock1[io+28], (uint32_t)newBlock1.size());
        newBlock1.insert(newBlock1.end(), parsed.foliageData[i].begin(), parsed.foliageData[i].end());
    }
    for (uint32_t i = 0; i < newHdr.radiosity_vals_info_num && i < (uint32_t)parsed.radiosityVals.size(); ++i) {
        if (parsed.radiosityVals[i].empty()) continue;
        uint32_t io = newHdr.radiosity_vals_info_offset + i * newHdr.radiosity_vals_info_size;
        if (io + 12 > (uint32_t)newBlock1.size()) break;
        WriteU32(&newBlock1[io+8], (uint32_t)newBlock1.size());
        uint32_t sz = (uint32_t)parsed.radiosityVals[i].size() * 4;
        newBlock1.insert(newBlock1.end(), (const uint8_t*)&parsed.radiosityVals[i][0],
                         (const uint8_t*)&parsed.radiosityVals[i][0] + sz);
    }

    #endif // END REMOVED PER-FIELD CODE

    // ---- Step 2e: Write NEW HkShapeInfo collision data (appended entries only) ----
    // Existing entries (i < origHkNum) have their data in the verbatim-copied region.
    // New entries (i >= origHkNum) need their data blobs written at the end of newBlock1.
    {
        uint32_t hkBase = newHdr.hk_shape_info_offset;
        uint32_t hkNum  = newHdr.hk_shape_info_num;
        uint32_t origHkNum2 = origHdr.hk_shape_info_num;
        for (uint32_t i = origHkNum2; i < hkNum; ++i) {
            uint32_t e = hkBase + i * 80;
            if (e + 80 > (uint32_t)newBlock1.size()) break;
            uint32_t kind = ReadU32(&newBlock1[e + 32]);
            if (kind == 6 && i < (uint32_t)parsed.hkShapeData.size()) {
                const std::vector<uint8_t>& blob = parsed.hkShapeData[i];
                if (blob.empty()) continue;
                uint32_t cSz = ReadU32(&newBlock1[e+56]);        // tree_size (MOPP)
                uint32_t dSz = ReadU32(&newBlock1[e+64]) * 12;   // numVerts * 12
                uint32_t eSz = ReadU32(&newBlock1[e+72]) * 6;    // numTris * 6
                uint32_t cur = 0;
                AlignBuffer(newBlock1, 16);
                if (cSz > 0 && cur+cSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+60], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+cSz);
                    cur += cSz;
                }
                AlignBuffer(newBlock1, 16);
                if (dSz > 0 && cur+dSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+68], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+dSz);
                    cur += dSz;
                }
                AlignBuffer(newBlock1, 4);
                if (eSz > 0 && cur+eSz <= (uint32_t)blob.size()) {
                    WriteU32(&newBlock1[e+76], (uint32_t)newBlock1.size());
                    newBlock1.insert(newBlock1.end(), blob.begin()+cur, blob.begin()+cur+eSz);
                }
                RLog("  [Rebuild] NEW HkShape[%u]: MOPP=%u verts=%u(%u bytes) tris=%u(%u bytes) blob=%u\n",
                     i, cSz, ReadU32(&newBlock1[e+64]), dSz, ReadU32(&newBlock1[e+72]), eSz,
                     (unsigned)blob.size());
            }
        }
    }

    RLog("  [Rebuild] Step 2 done: data region = %u bytes\n", (unsigned)newBlock1.size());

    // ============================================================
    //  STEP 3: Remap info-to-info cross-references.
    //  Info arrays (Step 1) contain OLD offsets to other info arrays.
    //  Data region (Step 2) was copied verbatim with dataDelta shift.
    //  Apply info-to-info deltas AND dataDelta to all pointer fields.
    // ============================================================
    RLog("  [Rebuild] Step 3: remapping cross-references (dataDelta=%+d)\n", dataDelta);
    {
        int32_t biDelta  = (int32_t)newHdr.buffer_info_offset  - (int32_t)origHdr.buffer_info_offset;
        int32_t siDelta  = (int32_t)newHdr.shape_info_offset   - (int32_t)origHdr.shape_info_offset;
        int32_t hkiDelta = (int32_t)newHdr.hk_shape_info_offset - (int32_t)origHdr.hk_shape_info_offset;
        int32_t hcdDelta = (int32_t)newHdr.hk_constraint_data_offset - (int32_t)origHdr.hk_constraint_data_offset;
        int32_t hciDelta = (int32_t)newHdr.hk_constraint_info_offset - (int32_t)origHdr.hk_constraint_info_offset;
        int32_t vbDelta  = (int32_t)newHdr.vbuff_info_offset   - (int32_t)origHdr.vbuff_info_offset;
        int32_t ibDelta  = (int32_t)newHdr.ibuff_info_offset   - (int32_t)origHdr.ibuff_info_offset;
        int32_t meDelta  = (int32_t)newHdr.mat_extra_offset    - (int32_t)origHdr.mat_extra_offset;

        // Helper: apply dataDelta to a field if its value is in the data region
        #define FIX_DATA(off) { \
            uint32_t _v = ReadU32(&newBlock1[off]); \
            if (_v != 0 && _v != 0xFFFFFFFFu && _v >= origDataStart && _v < origDataEnd) \
                WriteU32(&newBlock1[off], _v + dataDelta); \
        }

        // ModelInfo: info-to-info refs + data-region offset fields
        for (uint32_t i = 0; i < newHdr.model_info_num; ++i) {
            uint32_t mi = newHdr.model_info_offset + i * 256;
            if (mi + 256 > (uint32_t)newBlock1.size()) break;
            uint32_t v;
            // Info-to-info cross-refs (apply info array deltas)
            v = ReadU32(&newBlock1[mi+12]);  if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[mi+12],  v + biDelta);
            v = ReadU32(&newBlock1[mi+224]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[mi+224], v + siDelta);
            v = ReadU32(&newBlock1[mi+232]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[mi+232], v + hcdDelta);
            v = ReadU32(&newBlock1[mi+240]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[mi+240], v + hciDelta);
            // Data-region offset fields (apply dataDelta)
            static const uint32_t miDataFields[] = {8,48,136,140,144,152,160,164,172,180,196,200,204,244,248,252};
            for (int k = 0; k < 16; ++k) FIX_DATA(mi + miDataFields[k]);
        }
        // BufferInfo → VBuffInfo(0,4,8), IBuffInfo(260)
        for (uint32_t i = 0; i < newHdr.buffer_info_num; ++i) {
            uint32_t bi = newHdr.buffer_info_offset + i * newHdr.buffer_info_size;
            if (bi + newHdr.buffer_info_size > (uint32_t)newBlock1.size()) break;
            uint32_t v;
            v = ReadU32(&newBlock1[bi+0]);   if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[bi+0],   v + vbDelta);
            v = ReadU32(&newBlock1[bi+4]);   if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[bi+4],   v + vbDelta);
            v = ReadU32(&newBlock1[bi+8]);   if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[bi+8],   v + vbDelta);
            v = ReadU32(&newBlock1[bi+260]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[bi+260], v + ibDelta);
        }
        // ShapeInfo → HkShapeInfo(112)
        for (uint32_t i = 0; i < newHdr.shape_info_num; ++i) {
            uint32_t si = newHdr.shape_info_offset + i * newHdr.shape_info_size;
            if (si + 116 > (uint32_t)newBlock1.size()) break;
            uint32_t v = ReadU32(&newBlock1[si+112]);
            if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[si+112], v + hkiDelta);
        }
        // Model vbuff/ibuff ref arrays → VBuffInfo/IBuffInfo
        for (uint32_t i = 0; i < newHdr.model_info_num; ++i) {
            uint32_t mi = newHdr.model_info_offset + i * 256;
            if (mi + 256 > (uint32_t)newBlock1.size()) break;
            uint32_t vbOff = ReadU32(&newBlock1[mi+164]), vbNum = ReadU32(&newBlock1[mi+168]);
            uint32_t ibOff = ReadU32(&newBlock1[mi+172]), ibNum = ReadU32(&newBlock1[mi+176]);
            for (uint32_t j = 0; j < vbNum; ++j) {
                uint32_t a = vbOff + j*4; if (a+4 > (uint32_t)newBlock1.size()) break;
                uint32_t v = ReadU32(&newBlock1[a]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[a], v + vbDelta); }
            for (uint32_t j = 0; j < ibNum; ++j) {
                uint32_t a = ibOff + j*4; if (a+4 > (uint32_t)newBlock1.size()) break;
                uint32_t v = ReadU32(&newBlock1[a]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[a], v + ibDelta); }
        }
        // Model mat ref arrays → Mat1/Mat2/Mat3/Mat4
        {
            int32_t m1Delta = (int32_t)newHdr.mat1_offset - (int32_t)origHdr.mat1_offset;
            int32_t m2Delta = (int32_t)newHdr.mat2_offset - (int32_t)origHdr.mat2_offset;
            int32_t m3Delta = (int32_t)newHdr.mat3_offset - (int32_t)origHdr.mat3_offset;
            int32_t m4Delta = (int32_t)newHdr.mat4_offset - (int32_t)origHdr.mat4_offset;
            uint32_t om1End = origHdr.mat1_offset + origHdr.mat1_num * origHdr.mat1_size;
            uint32_t om2End = origHdr.mat2_offset + origHdr.mat2_num * origHdr.mat2_size;
            uint32_t om3End = origHdr.mat3_offset + origHdr.mat3_num * origHdr.mat3_size;
            uint32_t om4End = origHdr.mat4_offset + origHdr.mat4_num * origHdr.mat4_size;

            for (uint32_t i = 0; i < newHdr.model_info_num; ++i) {
                uint32_t mi = newHdr.model_info_offset + i * 256;
                if (mi + 256 > (uint32_t)newBlock1.size()) break;
                uint32_t matOff = ReadU32(&newBlock1[mi+8]);
                uint32_t matNum = ReadU32(&newBlock1[mi+132]);
                for (uint32_t j = 0; j < matNum; ++j) {
                    uint32_t a = matOff + j*4;
                    if (a+4 > (uint32_t)newBlock1.size()) break;
                    uint32_t v = ReadU32(&newBlock1[a]);
                    if (v == 0 || v == 0xFFFFFFFFu) continue;
                    int32_t d = 0;
                    if      (v >= origHdr.mat1_offset && v < om1End) d = m1Delta;
                    else if (v >= origHdr.mat2_offset && v < om2End) d = m2Delta;
                    else if (v >= origHdr.mat3_offset && v < om3End) d = m3Delta;
                    else if (v >= origHdr.mat4_offset && v < om4End) d = m4Delta;
                    if (d != 0) WriteU32(&newBlock1[a], v + d);
                }
            }
        }
        // Mat mat_extra_offset (byte 344)
        if (meDelta != 0) {
            struct MD { uint32_t o,n,s; };
            MD ms[4] = {{newHdr.mat1_offset,newHdr.mat1_num,newHdr.mat1_size},
                        {newHdr.mat2_offset,newHdr.mat2_num,newHdr.mat2_size},
                        {newHdr.mat3_offset,newHdr.mat3_num,newHdr.mat3_size},
                        {newHdr.mat4_offset,newHdr.mat4_num,newHdr.mat4_size}};
            for (int m = 0; m < 4; ++m)
                for (uint32_t j = 0; j < ms[m].n; ++j) {
                    uint32_t a = ms[m].o + j*ms[m].s + 344;
                    if (a+4 > (uint32_t)newBlock1.size()) break;
                    uint32_t v = ReadU32(&newBlock1[a]); if (v && v != 0xFFFFFFFFu) WriteU32(&newBlock1[a], v + meDelta);
                }
        }
        // ShapeInfo byte 0 (shape data offset — in data region)
        for (uint32_t i = 0; i < newHdr.shape_info_num; ++i) {
            FIX_DATA(newHdr.shape_info_offset + i * newHdr.shape_info_size + 0);
        }
        // HkConstraintInfo: data-region fields + internal bone name pointers
        for (uint32_t i = 0; i < newHdr.hk_constraint_info_num; ++i) {
            uint32_t e = newHdr.hk_constraint_info_offset + i * newHdr.hk_constraint_info_size;
            if (e + newHdr.hk_constraint_info_size > (uint32_t)newBlock1.size()) break;
            { static const int hcf[] = {4,12,20,40,48,64};
              for (int k = 0; k < 6; ++k) FIX_DATA(e + hcf[k]); }
            // Internal bone name pointers
            uint32_t bnOff = ReadU32(&newBlock1[e+12]); // already fixed above
            uint32_t bnNum = ReadU32(&newBlock1[e+16]);
            if (bnNum > 0 && bnOff + bnNum * 12 <= (uint32_t)newBlock1.size()) {
                for (uint32_t j = 0; j < bnNum; ++j) FIX_DATA(bnOff + j * 4);
                uint32_t ps = bnOff + bnNum * 4;
                for (uint32_t j = 0; j < bnNum; ++j) FIX_DATA(ps + j * 8);
            }
        }
        // HkShapeInfo internal data offsets (kind-dependent)
        for (uint32_t i = 0; i < newHdr.hk_shape_info_num; ++i) {
            uint32_t e = newHdr.hk_shape_info_offset + i * newHdr.hk_shape_info_size;
            if (e + 80 > (uint32_t)newBlock1.size()) break;
            uint32_t kind = ReadU32(&newBlock1[e + 32]);
            if (kind == 5) { FIX_DATA(e+44); FIX_DATA(e+52); }
            else if (kind == 6) { FIX_DATA(e+60); FIX_DATA(e+68); FIX_DATA(e+76); }
        }
        // EffectInfo byte 8
        for (uint32_t i = 0; i < newHdr.effect_info_num; ++i)
            FIX_DATA(newHdr.effect_info_offset + i * 16 + 8);
        // GFXBlockInfo byte 4
        for (uint32_t i = 0; i < newHdr.gfx_block_info_num; ++i)
            FIX_DATA(newHdr.gfx_block_info_offset + i * 12 + 4);
        // FoliageInfo byte 28
        for (uint32_t i = 0; i < newHdr.foliage_info_num; ++i)
            FIX_DATA(newHdr.foliage_info_offset + i * newHdr.foliage_info_size + 28);
        // RadiosityValsInfo byte 8
        for (uint32_t i = 0; i < newHdr.radiosity_vals_info_num; ++i)
            FIX_DATA(newHdr.radiosity_vals_info_offset + i * newHdr.radiosity_vals_info_size + 8);
        // PFieldInfo byte 16
        for (uint32_t i = 0; i < newHdr.pfield_info_num; ++i)
            FIX_DATA(newHdr.pfield_info_offset + i * newHdr.pfield_info_size + 16);

        #undef FIX_DATA
        RLog("  [Rebuild] Step 3 done: deltas BI=%+d SI=%+d HKI=%+d VB=%+d IB=%+d dataDelta=%+d\n",
             biDelta, siDelta, hkiDelta, vbDelta, ibDelta, dataDelta);
    }

    // ============================================================
    //  STEP 4: Append sub_blocks + string_keys.
    //  These go at the end of Block1 after all data.
    // ============================================================
    RLog("  [Rebuild] Step 4: sub_blocks + string_keys\n");

    // Sub-blocks
    AlignBuffer(newBlock1, 16);
    newHdr.sub_blocks1_offset = (uint32_t)newBlock1.size();
    {
        uint32_t sbCount = (uint32_t)subBlocks1.size();
        uint32_t tableSize = 16 + sbCount * 12;
        std::vector<uint32_t> pSizes(sbCount);
        std::vector<uint32_t> pOffsets(sbCount);
        uint32_t cursor = tableSize;
        for (uint32_t i = 0; i < sbCount; ++i) {
            if (subBlocks1[i].key == goKey && !goPayload.empty())
                pSizes[i] = (uint32_t)goPayload.size();
            else
                pSizes[i] = subBlocks1[i].size;
            pOffsets[i] = cursor;
            cursor += pSizes[i];
        }
        uint32_t zero = 0;
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&sbCount, (uint8_t*)&sbCount + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);
        for (uint32_t i = 0; i < sbCount; ++i) {
            uint8_t entry[12];
            memcpy(entry + 0, &subBlocks1[i].key, 4);
            memcpy(entry + 4, &pOffsets[i], 4);
            memcpy(entry + 8, &pSizes[i], 4);
            newBlock1.insert(newBlock1.end(), entry, entry + 12);
        }
        for (uint32_t i = 0; i < sbCount; ++i) {
            if (subBlocks1[i].key == goKey && !goPayload.empty()) {
                newBlock1.insert(newBlock1.end(), goPayload.begin(), goPayload.end());
            } else {
                uint32_t pOff = subBlocks1[i].absOffset;
                uint32_t pSz  = subBlocks1[i].size;
                if (pOff + pSz <= (uint32_t)origBlock1.size())
                    newBlock1.insert(newBlock1.end(), origBlock1.begin() + pOff, origBlock1.begin() + pOff + pSz);
            }
        }
    }

    // String keys
    AlignBuffer(newBlock1, 32);
    newHdr.string_keys_offset = (uint32_t)newBlock1.size();
    {
        uint32_t oldSkOff = origHdr.string_keys_offset;
        if (oldSkOff > 0 && oldSkOff + 16 <= (uint32_t)origBlock1.size()) {
            uint16_t numA;
            memcpy(&numA, &origBlock1[oldSkOff], 2);
            uint32_t skBytes = 16 + (uint32_t)numA * 12;
            if (oldSkOff + skBytes <= (uint32_t)origBlock1.size())
                newBlock1.insert(newBlock1.end(), origBlock1.begin() + oldSkOff, origBlock1.begin() + oldSkOff + skBytes);
        }
    }

    newHdr.block1_size = (uint32_t)newBlock1.size();
    RLog("  [Rebuild] DONE: newBlock1=%u bytes, sub_blocks1=%u, string_keys=%u\n",
         (unsigned)newBlock1.size(), newHdr.sub_blocks1_offset, newHdr.string_keys_offset);
    return true;
}

} // namespace ZeroEngine
