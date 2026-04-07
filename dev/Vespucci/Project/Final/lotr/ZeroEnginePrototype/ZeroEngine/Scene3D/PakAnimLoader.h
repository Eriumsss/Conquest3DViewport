// PakAnimLoader.h — Raw Binary Animation Decoder (The Hard Way)
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Loads Conquest animation data DIRECTLY from PAK binary Block1 — no
// JSON intermediate, no file I/O, straight from the decompressed memory
// buffer into JsonAnimClip structs. This is the fast path for loading
// all 11,726 animations from a level's PAK file without writing 11,726
// temp JSON files to disk like a fucking animal.
//
// Handles EVERY compression scheme Pandemic ever used:
// ThreeComp40, ThreeComp48, ThreeComp24, Polar32, Straight16,
// Uncompressed, HkaSplineSkeletalAnimation. Each one packs quaternions
// differently. Each one was a separate week of my life in Ghidra. The
// Polar32 format uses spherical coordinates for quaternion storage.
// WHO THE FUCK STORES QUATERNIONS IN SPHERICAL COORDINATES? Pandemic.
// Pandemic does. Did. Whatever. They're dead.
//
// "Lost time is never found again." — Benjamin Franklin
// The months I spent decoding these formats are gone forever. But the
// decoder works. Every animation in the game loads correctly now.
// All 11,726 of them. Ben would be... probably confused, honestly.
// -----------------------------------------------------------------------
//
// Data flow:
//   PAK Block1 → AnimationInfo (148 bytes) → ParsePakAnimation()
//     → reads bone CRCs, events, HkaSplineSkeletalAnimation blocks
//     → decodes ThreeComp40/48/24/Polar32/Straight16/Uncompressed rotations
//     → decodes translation/scale spline data
//     → outputs JsonAnimClip ready for playback
//
// Binary format reference:
//   - Rust parser: lotrc-rust/lotrc/src/pak/animation.rs
//   - Havok header: hk550/.../hkaSplineSkeletalAnimation.h
//   - Disassembly: FUN_004B0340 (hkaSplineSkeletalAnimationCtor.cpp)
//
// All offsets in AnimationInfo are RELATIVE to the start of Block1.
// block_offset is the start of the compressed animation data block.
// block_starts_offset is an array of uint32_t offsets within that block.

#pragma once

#include "Scene3DRendererInternal.h"
#include "LevelReader.h"
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// Rotation quantization types — matches Havok's RotationQuantization enum
// and Rust parser's RotationQuantization enum
// ---------------------------------------------------------------------------
enum PakRotQuant {
    PAK_ROT_POLAR32       = 0,  // 4 bytes per rotation
    PAK_ROT_THREECOMP40   = 1,  // 5 bytes per rotation (game's primary format)
    PAK_ROT_THREECOMP48   = 2,  // 6 bytes per rotation
    PAK_ROT_THREECOMP24   = 3,  // 3 bytes per rotation
    PAK_ROT_STRAIGHT16    = 4,  // 2 bytes per rotation
    PAK_ROT_UNCOMPRESSED  = 5   // 16 bytes per rotation (4 floats)
};

// Scalar quantization types
enum PakScalarQuant {
    PAK_SCALAR_BITS8  = 0,  // 1 byte per scalar
    PAK_SCALAR_BITS16 = 1   // 2 bytes per scalar
};

// ---------------------------------------------------------------------------
// Alignment helpers for reading Havok data (from hkaSplineSkeletalAnimation.h)
// ---------------------------------------------------------------------------
inline int PakRotBytes(PakRotQuant type) {
    switch (type) {
        case PAK_ROT_POLAR32:       return 4;
        case PAK_ROT_THREECOMP40:   return 5;
        case PAK_ROT_THREECOMP48:   return 6;
        case PAK_ROT_THREECOMP24:   return 3;
        case PAK_ROT_STRAIGHT16:    return 2;
        case PAK_ROT_UNCOMPRESSED:  return 16;
        default: return 0;
    }
}

inline int PakRotAlign(PakRotQuant type) {
    switch (type) {
        case PAK_ROT_POLAR32:       return 4;
        case PAK_ROT_THREECOMP40:   return 1; // byte-aligned
        case PAK_ROT_THREECOMP48:   return 2;
        case PAK_ROT_THREECOMP24:   return 1;
        case PAK_ROT_STRAIGHT16:    return 2;
        case PAK_ROT_UNCOMPRESSED:  return 16; // SSE-aligned
        default: return 1;
    }
}

inline int PakScalarBytes(PakScalarQuant type) {
    return (type == PAK_SCALAR_BITS16) ? 2 : 1;
}

// ---------------------------------------------------------------------------
// Unpack quantization type byte
// From hkaSplineSkeletalAnimation.h: packQuantizationTypes()
// Byte layout: bits[1:0]=translation, bits[4:2]=rotation, bits[6:5]=scale
// ---------------------------------------------------------------------------
inline void UnpackQuantTypes(uint8_t packed,
                             PakScalarQuant& transQuant,
                             PakRotQuant& rotQuant,
                             PakScalarQuant& scaleQuant)
{
    transQuant = (PakScalarQuant)(packed & 0x03);
    rotQuant   = (PakRotQuant)((packed >> 2) & 0x07);
    scaleQuant = (PakScalarQuant)((packed >> 5) & 0x03);
}

// Unpack mask + float quantization byte
// From hkaSplineSkeletalAnimation.h: packMaskAndQuantizationType()
inline void UnpackMaskAndFloatQuant(uint8_t packed,
                                    uint8_t& mask,
                                    PakScalarQuant& floatQuant)
{
    mask       = packed & 0x0F;
    floatQuant = (PakScalarQuant)((packed >> 4) & 0x03);
}

// ---------------------------------------------------------------------------
// Animation event (Obj3 in Rust parser) — 44 bytes per event
// ---------------------------------------------------------------------------
struct PakAnimEvent {
    float    time;          // +0x00: event time in seconds
    uint32_t eventCrc;      // +0x04: CRC of event name
    uint32_t dat[9];        // +0x08: event parameters (36 bytes)
};

// ---------------------------------------------------------------------------
// Obj5 header — secondary animation data (usually empty)
// ---------------------------------------------------------------------------
struct PakObj5Header {
    uint32_t obj_a_num;
    uint32_t obj_a_offset;
    uint32_t obj_b_num;
    uint32_t obj_b_offset;
};

// ---------------------------------------------------------------------------
// Main API
// ---------------------------------------------------------------------------

// Parse a single animation from PAK Block1 binary data.
// animInfo: the AnimationInfo struct from the PAK header
// block1:   pointer to decompressed Block1 data
// block1Size: size of Block1
// outClip: output JsonAnimClip (caller allocates)
// Returns true on success.
bool ParsePakAnimation(
    const ZeroEngine::AnimationInfo& animInfo,
    const unsigned char* block1,
    size_t block1Size,
    JsonAnimClip& outClip
);

// Parse all animations from a LevelReader and populate a clip cache.
// Returns number of clips successfully parsed.
int LoadAllPakAnimations(
    const ZeroEngine::LevelReader& reader,
    std::vector<JsonAnimClip>& outClips,
    std::vector<std::string>& outNames
);

// Look up a single animation by CRC from a LevelReader.
// Returns true and fills outClip on success.
bool LoadPakAnimationByCrc(
    const ZeroEngine::LevelReader& reader,
    uint32_t animCrc,
    JsonAnimClip& outClip
);

// Look up a single animation by name from a LevelReader.
bool LoadPakAnimationByName(
    const ZeroEngine::LevelReader& reader,
    const char* animName,
    JsonAnimClip& outClip
);
