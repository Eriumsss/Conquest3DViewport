// LevelReader.h — The Necronomicon of PAK/BIN File Parsing
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Blood pressure while writing this: clinically dangerous
//
// This file reads Pandemic's proprietary PAK and BIN level files —
// the binary format they used to ship every single map, model, texture,
// animation, collision mesh, and particle effect in LOTR: Conquest.
// ZERO documentation existed for this format. ZERO. Not a single byte
// was explained anywhere on the internet except haighcam's parser.
// Every struct below was taken from haighcam or reverse-engineered
// by staring at hex dumps until patterns emerged
// from the void like faces in static.
//
// The PAK header alone is 472 bytes — 118 uint32_t fields, most of
// them named "unk_XX" because NOBODY AT PANDEMIC LEFT A FUCKING NOTE.
// They just packed their data and bounced. Studio closed. Servers wiped.
// Source code? Gone. Build tools? Gone. The only thing that survived
// was the shipped game disc and whatever I could pull out of the .exe
// with Ghidra and 4 years of my rapidly deteriorating sanity.
//
// Every struct is #pragma pack(push, 1) because Pandemic packed their
// files TIGHT. One byte of padding and the entire parse chain shifts
// and you're reading texture offsets as collision data and collision
// data as animation headers and the whole thing looks like it's
// speaking in tongues. Don't fucking touch the packing.
//
// If you're a former Pandemic dev and you recognize these structs:
// yes, I figured out your format. No, it wasn't fun. Yes, I hate you
// a little bit. But also thank you for making a good game, you
// beautiful psychotic bastards.
// -----------------------------------------------------------------------
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

#include <stdint.h>
#include "EntityFieldDefs.h"
#include "PakRebuilder.h"

// Compile-time size assertion that works on VS2005 and C++11+
#if defined(_MSC_VER) && _MSC_VER < 1600
    // Array-size trick: negative size causes a compile error
    #define ZE_SA2(expr, ln) typedef char ze_sa_##ln[(expr) ? 1 : -1]
    #define ZE_SA(expr, ln)  ZE_SA2(expr, ln)
    #define ZERO_STATIC_ASSERT(expr, msg) ZE_SA(expr, __LINE__)
#else
    #define ZERO_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#endif

namespace ZeroEngine {

// ============================================================
// Pandemic's custom case-insensitive CRC32 variant.
// Not standard CRC32. Not CRC32-C. Not any documented variant.
// It's THEIR OWN CUSTOM polynomial with case-folding built in.
// Every asset reference in the game is a CRC32 hash of its name.
// Textures, models, animations, sounds — all identified by hash.
// No filenames stored. Just hashes. If two assets collide? Too
// bad. The game just loads the wrong one and nobody notices
// because QA was probably 3 people by the time EA pulled the plug.
// I reverse-engineered this polynomial from the .exe function at
// 0x007E3F40. It took me sometime to realize the lookup table was
// built with a non-standard init value. A WEEK. FOR A HASH. FUCK.
// Q: Why didnt you just used haig--
// A: SHUT THE FUCK UP
// ============================================================
uint32_t LotrHashString(const char* str, int len = -1);

// ============================================================
// Binary structures — byte-for-byte match with Pandemic's file layout.
// #pragma pack(1) because these animals didn't believe in alignment.
// Every. Single. Field. Must match. The .PAK files on the retail disc
// are the ground truth. If sizeof() doesn't match, the rust parser eats
// garbage and shits out segfaults. The ZERO_STATIC_ASSERT at the end
// of each struct is the only thing standing between us and insanity.
// ============================================================
#pragma pack(push, 1)

// PAK Header (PC) — 118 x uint32_t = 472 bytes.
// The big kahuna. Every PAK file starts with this absolute unit of a
// header. 472 bytes of offsets, sizes, counts, and mystery fields.
// First 2 fields are PC-only (block_a_num/offset) — console versions
// don't have them, which means this format is PLATFORM-DEPENDENT and
// Pandemic had per-platform packing tools. We only support PC because
// I'm not about to reverse-engineer the Xbox 360 variant too. I have
// SOME self-preservation instincts left.
// All asset info offsets are relative to the START of decompressed Block1 data.
struct PakHeader {
    uint32_t block_a_num;
    uint32_t block_a_offset;
    uint32_t constx13;
    uint32_t version;
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t strings_num;
    uint32_t block1_offset;
    uint32_t block1_size;
    uint32_t block1_size_comp;
    uint32_t sub_blocks1_offset;
    uint32_t block2_offset;
    uint32_t block2_size;
    uint32_t block2_size_comp;
    uint32_t sub_blocks2_offset;
    uint32_t string_keys_offset;
    uint32_t unk_16;
    uint32_t obja_size;
    uint32_t obj0_size;
    uint32_t model_info_size;
    uint32_t buffer_info_size;
    uint32_t mat1_size;
    uint32_t mat2_size;
    uint32_t mat3_size;
    uint32_t mat4_size;
    uint32_t mat_extra_size;
    uint32_t unk_26;
    uint32_t shape_info_size;
    uint32_t hk_shape_info_size;
    uint32_t hk_constraint_data_size;
    uint32_t vbuff_info_size;
    uint32_t ibuff_info_size;
    uint32_t texture_info_size;
    uint32_t animation_info_size;
    uint32_t hk_constraint_info_size;
    uint32_t effect_info_size;
    uint32_t pfield_info_size;
    uint32_t gfx_block_info_size;
    uint32_t animation_block_info_size;
    uint32_t foliage_info_size;
    uint32_t radiosity_vals_info_size;
    uint32_t unk_41;
    uint32_t obja_num;
    uint32_t obj0_num;
    uint32_t model_info_num;
    uint32_t buffer_info_num;
    uint32_t mat1_num;
    uint32_t mat2_num;
    uint32_t mat3_num;
    uint32_t mat4_num;
    uint32_t mat_extra_num;
    uint32_t unk_51;
    uint32_t shape_info_num;
    uint32_t hk_shape_info_num;
    uint32_t hk_constraint_data_num;
    uint32_t vbuff_info_num;
    uint32_t ibuff_info_num;
    uint32_t texture_info_num;
    uint32_t animation_info_num;
    uint32_t hk_constraint_info_num;
    uint32_t effect_info_num;
    uint32_t pfield_info_num;
    uint32_t gfx_block_info_num;
    uint32_t animation_block_info_num;
    uint32_t foliage_info_num;
    uint32_t radiosity_vals_info_num;
    uint32_t unk_66;
    // All offsets below are relative to start of decompressed Block1 data
    uint32_t obja_offset;
    uint32_t obj0_offset;
    uint32_t model_info_offset;
    uint32_t buffer_info_offset;
    uint32_t mat1_offset;
    uint32_t mat2_offset;
    uint32_t mat3_offset;
    uint32_t mat4_offset;
    uint32_t mat_extra_offset;
    uint32_t unk_76;
    uint32_t shape_info_offset;
    uint32_t hk_shape_info_offset;
    uint32_t hk_constraint_data_offset;
    uint32_t vbuff_info_offset;
    uint32_t ibuff_info_offset;
    uint32_t texture_info_offset;
    uint32_t animation_info_offset;
    uint32_t hk_constraint_info_offset;
    uint32_t effect_info_offset;
    uint32_t pfield_info_offset;
    uint32_t gfx_block_info_offset;
    uint32_t animation_block_info_offset;
    uint32_t foliage_info_offset;
    uint32_t radiosity_vals_info_offset;
    uint32_t unk_91,  unk_92,  unk_93,  unk_94,  unk_95,
             unk_96,  unk_97,  unk_98,  unk_99,  unk_100,
             unk_101, unk_102, unk_103, unk_104, unk_105,
             unk_106, unk_107, unk_108, unk_109, unk_110,
             unk_111, unk_112, unk_113, unk_114, unk_115;
    uint32_t block2_offsets_num;
    uint32_t block2_offsets_offset;
};
ZERO_STATIC_ASSERT(sizeof(PakHeader) == 472, "PakHeader size mismatch");

// BIN Header — 43 x uint32_t = 172 bytes.
// The BIN file is the PAK's evil twin. While the PAK stores metadata and
// compressed asset info blocks, the BIN stores the actual raw data —
// vertex buffers, index buffers, textures, collision meshes. They're
// always paired: for every "helmsdeep.PAK" there's a "helmsdeep.BIN".
// Lose one and the other is useless. Like a fucked up buddy system
// designed by someone who wanted future reverse-engineers to suffer.
struct BinHeader {
    uint32_t constx06;
    uint32_t version;
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t strings_num;
    uint32_t asset_handle_num;
    uint32_t asset_handle_offset;
    uint32_t unk_7;
    uint32_t vdata_num;
    uint32_t vdata_num_;
    uint32_t texdata_num;
    uint32_t unk_11, unk_12, unk_13, unk_14, unk_15,
             unk_16, unk_17, unk_18, unk_19, unk_20,
             unk_21, unk_22, unk_23, unk_24, unk_25,
             unk_26, unk_27, unk_28, unk_29, unk_30,
             unk_31, unk_32, unk_33, unk_34, unk_35,
             unk_36, unk_37, unk_38, unk_39, unk_40,
             unk_41, unk_42;
};
ZERO_STATIC_ASSERT(sizeof(BinHeader) == 172, "BinHeader size mismatch");

// BIN Asset Handle — 5 x uint32_t = 20 bytes
struct BinAssetHandle {
    uint32_t key;
    uint32_t offset;
    uint32_t size;
    uint32_t size_comp;
    uint32_t kind;
};
ZERO_STATIC_ASSERT(sizeof(BinAssetHandle) == 20, "BinAssetHandle size mismatch");

// ObjA (PC) — 6 x uint32_t = 24 bytes
struct ObjA {
    uint32_t key;
    uint32_t unk_1;
    uint32_t size;
    uint32_t size_comp;
    uint32_t unk_4;
    uint32_t kind;
};

// Obj0 (PC) — 2 x uint32_t = 8 bytes
struct Obj0 {
    uint32_t unk_0;
    uint32_t key;
};

// LodMeshes — 5 x uint32_t = 20 bytes
struct LodMeshes {
    uint32_t start;
    uint32_t static_end;
    uint32_t skinned_end;
    uint32_t physics_end;
    uint32_t breakable_end;
};

// BoundingBox — 8 x float = 32 bytes
struct PakBoundingBox {
    float center[3];
    float unk_3;
    float half_width[3];
    float unk_7;
};

// ModelInfo (PC) — 256 bytes of pure concentrated Pandemic engineering.
// 64 fields. Each one is an offset or a count or a key pointing to some
// other table that points to another table that eventually points to
// actual geometry. It's tables all the way down, motherfucker. This
// struct describes ONE model — its meshes, LODs, bones, skin binds,
// materials, shapes, constraints, bounding boxes, and a bunch of
// mystery fields that Pandemic's art pipeline filled in automatically.
// The LOD system (lod0-lod3) supports 4 detail levels × 4 mesh types
// (static, skinned, physics, breakable). That's 16 possible mesh ranges
// per model. Pandemic was THOROUGH. They were building a whole engine
// while EA was already loading the gun.
struct ModelInfo {
    uint32_t       key;
    int32_t        gamemodemask;
    uint32_t       mat_offset;
    uint32_t       buffer_info_offset;
    PakBoundingBox bounding_box;
    uint32_t       mesh_order_offset;
    LodMeshes      lod0;
    LodMeshes      lod1;
    LodMeshes      lod2;
    LodMeshes      lod3;
    uint32_t       mat_num;
    uint32_t       bones_offset;
    uint32_t       bone_parents_offset;
    uint32_t       bone_transforms_offset;
    uint32_t       bones_num;
    uint32_t       skin_binds_offset;
    uint32_t       skin_binds_num;
    uint32_t       skin_order_offset;
    uint32_t       vbuff_offset;
    uint32_t       vbuff_num;
    uint32_t       ibuff_offset;
    uint32_t       ibuff_num;
    uint32_t       mesh_bounding_boxes_offset;
    float          unk_46;
    uint32_t       variation_counts;
    uint32_t       vals_j_num;
    uint32_t       vals_j_offset;
    uint32_t       block_offset;
    uint32_t       vals_k_offset;
    uint32_t       asset_key;
    uint32_t       asset_type;
    uint32_t       unk_54;
    uint32_t       unk_55;
    uint32_t       shape_offset;
    uint32_t       shape_num;
    uint32_t       hk_constraint_data_offset;
    uint32_t       hk_constraint_data_num;
    uint32_t       hk_constraint_offset;
    uint32_t       slots_offset;
    uint32_t       slot_map_offset;
    uint32_t       bone_bounding_boxes_offset;
};
ZERO_STATIC_ASSERT(sizeof(ModelInfo) == 256, "ModelInfo size mismatch");

// TextureInfo — 72 bytes
struct TextureInfo {
    uint32_t key;
    int32_t  gamemodemask;
    uint32_t asset_key;
    uint32_t asset_type;
    uint32_t kind;
    uint32_t format;
    uint32_t unk_6;
    uint32_t unk_7;
    uint32_t unk_8;
    uint32_t unk_9;
    uint32_t unk_10;
    uint32_t unk_11;
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t levels;
    uint8_t  unk_bytes[16];
};
ZERO_STATIC_ASSERT(sizeof(TextureInfo) == 72, "TextureInfo size mismatch");

// VBuffInfo (PC) — 8 x uint32_t = 32 bytes
struct VBuffInfo {
    uint32_t unk_0;
    uint32_t size;
    uint32_t unk_3;
    uint32_t offset;
    uint32_t fmt1;
    uint32_t fmt2;
    uint32_t unk_6;
    uint32_t unk_7;
};

// IBuffInfo (PC) — 6 x uint32_t = 24 bytes
struct IBuffInfo {
    uint32_t unk_0;
    uint32_t size;
    uint32_t format;
    uint32_t vbuff_alt_fmt;
    uint32_t offset;
    uint32_t unk_5;
};

// EffectInfo — 4 x uint32_t = 16 bytes
struct EffectInfo {
    uint32_t key;
    int32_t  gamemodemask;
    uint32_t offset;
    uint32_t size;
};

// GFXBlockInfo — 3 x uint32_t = 12 bytes
struct GFXBlockInfo {
    uint32_t key;
    uint32_t offset;
    uint32_t size;
};

// AnimationInfo — 148 bytes per animation entry. 37 fields.
// This is the metadata for every animation Pandemic packed into the level.
// The actual bone data lives at 'offset' within Block1, compressed with
// their custom codec. The 'vals_num' is roughly bone count. 't_scale'
// controls time scaling (30fps vs 60fps vs whatever the fuck Pandemic's
// animators decided that day). The block_starts_offset points to a table
// of sub-block entry points within the compressed data — because
// Pandemic's animation format supports random-access seeking by block.
// They could jump to any frame without decompressing from the start.
// That's actually brilliant engineering. Too bad nobody alive knows
// exactly how the block seeking works. I've gotten 80% of it. The
// other 20% involves a state machine I can't fully trace in Ghidra.
struct AnimationInfo {
    uint32_t key;               // CRC of animation name
    int32_t  gamemodemask;
    uint32_t offset;            // offset within block1 data
    uint32_t size;              // total uncompressed size
    uint32_t kind;
    float    unk_5;
    uint32_t vals_num;          // number of bone values (~ bone count)
    uint32_t vals2_num;
    uint32_t unk_8;
    uint32_t vala;
    uint32_t unk_10;
    uint32_t unk_11;
    uint32_t data_offset;
    float    unk_13;
    float    unk_14;
    float    t_scale;
    uint32_t block_starts_offset;
    uint32_t block_starts_num;
    uint32_t block_starts2_offset;
    uint32_t block_starts2_num;
    uint32_t obj_c3_offset;
    uint32_t obj_c3_num;
    uint32_t obj_c4_offset;
    uint32_t obj_c4_num;
    uint32_t block_offset;
    uint32_t block_size;
    uint32_t obj3_num;
    uint32_t obj3_offset;
    uint32_t bones_num1;
    uint32_t unk_29;
    uint32_t obj1_num;
    uint32_t bones_offset;
    uint32_t unk_32;
    uint32_t obj1_offset;
    uint32_t obj2_offset;
    uint32_t obj2_num;
    uint32_t obj5_offset;
};
ZERO_STATIC_ASSERT(sizeof(AnimationInfo) == 148, "AnimationInfo size mismatch");

// BlockAVal (pak_vals_a) — 7 fields = 28 bytes
struct BlockAVal {
    uint32_t unk_0;
    int32_t  gamemodemask;
    uint32_t key;
    uint32_t unk_3;
    uint32_t unk_4;
    uint32_t unk_5;
    uint32_t unk_6;
};
ZERO_STATIC_ASSERT(sizeof(BlockAVal) == 28, "BlockAVal size mismatch");

// AnimationBlockInfo — 9 x uint32_t = 36 bytes
struct AnimationBlockInfo {
    uint32_t key;
    uint32_t guid;
    uint32_t key_name;
    uint32_t offset;
    uint32_t size;
    uint32_t size_comp;
    uint32_t unk_6;
    uint32_t unk_7;
    uint32_t unk_8;
};

#pragma pack(pop)

// ============================================================
// High-level entries — the human-readable face of the horror above.
//
// After parsing the raw packed structs, we build these friendly
// entries with actual string names (resolved from CRC→string table),
// clean types, and everything a sane person would need. These are
// what the rest of ZeroEngine actually uses. Nobody touches the
// raw structs directly except LevelReader::parse() and people
// who have already lost the will to live.
// ============================================================

struct LevelModelEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    asset_key;
    float       bbox_center[3];
    float       bbox_half_width[3];
    uint32_t    mat_num;
    uint32_t    bones_num;
    uint32_t    vbuff_num;
    uint32_t    ibuff_num;
    int32_t     gamemodemask;
};

struct LevelTextureEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    asset_key;
    uint16_t    width, height, depth, levels;
    uint32_t    format;
    uint32_t    kind;
    int32_t     gamemodemask;
};

struct LevelAnimEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    guid;
    uint32_t    key_name_crc;
    std::string key_name;
    uint32_t    size;
    uint32_t    size_comp;
    uint32_t    offset;
    int32_t     gamemodemask;
};

struct LevelEffectEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    offset;
    uint32_t    size;
    int32_t     gamemodemask;
};

struct LevelGfxBlockEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    offset;
    uint32_t    size;
};

struct BinAssetEntry {
    uint32_t    key;
    uint32_t    offset;
    uint32_t    size;
    uint32_t    size_comp;
    uint32_t    kind;
};

struct BinFlatEntry {
    std::string name;
    uint32_t    key;
    uint32_t    kind;
    uint32_t    size;
    uint32_t    size_comp;
};

// One embedded-weapon tag written by level_patcher.py at bake time and
// loaded from <dump_dir>/ze_embedded_weapons.json on level load. Tells
// the popup mesh editor which slot[0] verts in a baked merged CRD are
// the weapon's contribution, plus the attach-bone bind matrix needed
// to apply a bone-local transform later.
struct EmbeddedWeaponTag {
    std::string weapon_name;
    std::string attach_bone;
    uint32_t    attach_bone_idx;  // index into base.bones
    uint32_t    bind_idx;         // index into skin_order
    uint32_t    vert_first;       // first weapon vert in slot[0]
    uint32_t    vert_count;       // number of weapon verts
    uint32_t    tri_first;        // first weapon triangle in slot[0] index buffer
    uint32_t    tri_count;        // number of weapon triangles
    float       offset_xyz[3];    // user offset baked in (record only)
    float       offset_ypr[3];    // user rotation baked in (record only)
    float       bind_world[16];   // authoritative bind-pose world matrix, row-major.
                                  // Used by the editor to do new_world =
                                  // bind_world * delta_local * inv(bind_world) * cur_world
};

struct SubBlockEntry {
    uint32_t    key;
    std::string name;
    // "Lua","Data","GameObjs","LangStrings","Spray","Crowd","PFields","AtlasUV","SSA","Unknown"
    std::string type;
    uint32_t    size;
    uint32_t    absOffset;              // absolute Block1 offset of payload
    std::vector<uint8_t> rawPayload;    // cached raw bytes for rebuild
};

struct LevelObjAEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    size;
    uint32_t    size_comp;
    uint32_t    kind;
};

struct LevelObj0Entry {
    uint32_t    type_code;  // unk_0 (e.g. 101 = effect ref, 102 = ?, ...)
    uint32_t    crc;
    std::string name;
};

struct LevelAnimBlockEntry {
    uint32_t    crc;
    std::string name;       // game mode name (e.g. "evilcampaign")
    uint32_t    guid;
    std::string key_name;   // human name (e.g. "Evil Campaign")
    uint32_t    size;
    uint32_t    size_comp;
};

struct LevelPakValEntry {
    uint32_t    crc;
    std::string name;
    int32_t     gamemodemask;
};

// Generic entry for types that may or may not have a CRC key.
// name = resolved CRC or "#NNNN". extra1/extra2 carry type-specific fields.
struct LevelGenericEntry {
    uint32_t    crc;
    std::string name;
    uint32_t    extra1;  // type-specific (e.g. kind, size, num)
    uint32_t    extra2;  // type-specific (e.g. offset, format, height)
};

// ============================================================
// GameObjs — every single entity placed in a Conquest level.
//
// Static objects, spawn points, path networks, capture zones,
// cinematic cameras, light_suns, trigger volumes, AI waypoints,
// atmospheric settings, sound emitters — everything. Pandemic's
// level designers placed ALL of this by hand using their internal
// editor (which they called "ZeroEdit" — we found the string in
// the .exe at 0x009ABF60, sitting next to "ZeroEditor.log").
//
// Each object has a GUID, a parent GUID (for hierarchy), a type
// CRC, a name CRC, a world transform, and then a variable-length
// blob of type-specific fields defined by the type's field
// definitions. The field system is basically a mini runtime type
// information system that Pandemic built from scratch. Types like
// "static_object" have fields like "Mesh" (CRC), "WorldTransform"
// (4x4 matrix), "GameModeMask" (int). Types like "spawn_point"
// have "Team" (int), "CharacterClass" (GUID reference), etc.
//
// We parse ALL of this. Every field. Every type. Every object.
// Some levels have 5000+ entities. The parser handles it in about
// 200ms. Pandemic's original editor probably loaded them faster
// but Pandemic's original editor is GONE, DEAD, ERASED FROM THE
// EARTH, so 200ms is what we've got and it's FINE.
// ============================================================

// A single placed object in the level scene graph.
// Extracts the universal fields every object type shares.
struct LevelGameObjEntry {
    uint32_t    guid;
    uint32_t    parent_guid;
    int32_t     gamemodemask;
    uint32_t    name_crc;
    std::string name;           // resolved from CRC
    uint32_t    type_crc;
    std::string type_name;      // resolved from CRC (e.g. "static_object")
    uint32_t    layer_guid;     // which layer this object belongs to
    // Optional fields (populated for static_object etc.)
    uint32_t    mesh_crc;
    std::string mesh_name;      // resolved Mesh field
    bool        has_transform;
    bool        has_world_transform; // true = WorldTransform (absolute), false = only local Transform
    float       pos[3];              // translation only (tx,ty,tz) — from column 3 of WorldTransform
    float       world_transform[16]; // full 4x4 row-major world matrix (D3D9-compatible)
    uint32_t    block1_wt_offset;    // byte offset of WorldTransform field in decompressed Block1 (0=unknown)
    uint32_t    block1_tf_offset;    // byte offset of Transform (local) field in decompressed Block1 (0=unknown)

    // --- Event/relationship fields ---
    // Output blocks (type="Output"): event connection system
    uint32_t    target_guid;    // "target" field — GUID of target entity
    std::string output_event;   // "Output" field — event name emitted from source
    std::string input_event;    // "Input" field — event name received by target
    float       delay;          // "Delay" field — seconds
    bool        sticky;         // "Sticky" field — persistent connection

    // GUID arrays — entity reference lists
    std::vector<uint32_t> outputs;              // "Outputs" — Output block GUIDs
    std::vector<uint32_t> layers;               // "Layers" — layer GUIDs (templateLevel/gamemode)
    std::vector<uint32_t> nodes;                // "Nodes" — PathNode GUIDs (PathNetwork)
    std::vector<uint32_t> initial_child_objects; // "InitialChildObjects" — child GUIDs
    std::vector<uint32_t> drop_items;           // "DropItems" — item GUIDs

    // PathLink fields
    uint32_t    node1_guid;     // "node1" — first PathNode GUID
    uint32_t    node2_guid;     // "node2" — second PathNode GUID

    // Mode field for gamemode entities
    std::string mode_name;      // "Mode" field (e.g. "evilcampaign", "Conquest")

    // Generic GUID references — captures ALL GUID-type fields not covered above
    // Key = field name CRC, Value = referenced GUID
    std::map<uint32_t, uint32_t> guid_refs;         // single GUID fields (CharacterClass, CaptureArea, item, etc.)
    // Generic objectlist references — captures ALL objectlist fields not covered above
    // Key = field name CRC, Value = list of referenced GUIDs
    std::map<uint32_t, std::vector<uint32_t> > list_refs; // objectlist fields (Classes, Points, Inventory_Items, etc.)

    // Editor visualization fields (Pandemic's internal editor shape system)
    std::string editor_shape;   // "Type" field: "Billboard", "Sphere", "Box", "Box_Trigger"
    uint32_t    editor_color;   // "Color" field: 0xAARRGGBB
    std::string editor_texture; // "Texture" field: e.g. "fed_timer.tga", "fed_goal.tga"
    float       editor_outer;   // "Outer" field: radius/size for sphere shapes
    float       editor_size[3]; // "size" field: [x,y,z] for box shapes

    // Cinematic camera fields
    uint32_t    position_track_guid; // PositionTrack spline GUID
    uint32_t    target_track_guid;   // TargetTrack spline GUID
    float       fov;
    float       total_duration;
    float       position_travel_time;
    float       target_travel_time;

    // Spline node data (for Spline type objects)
    struct SplineNode { float x, y, z, s; };
    std::vector<SplineNode> spline_nodes;

    // Generic field capture — all float and int/color fields by name CRC.
    // Used for AtmosphereSetting, light_sun, templateLevel, etc.
    std::map<uint32_t, float>    float_fields;
    std::map<uint32_t, uint32_t> int_fields;

    // Property editor support — absolute Block1 offsets for field-level read/write
    uint32_t    block1_obj_offset;   // absolute offset of this object's field data in Block1
    uint32_t    block1_obj_size;     // size of this object's field data in bytes
    int         type_def_index;      // index into LevelReader::m_gameObjTypes (-1=unknown)
};

// A single field definition within a type (exposed for property editor).
struct GOFieldDefEntry {
    uint32_t    name_crc;
    uint32_t    kind_crc;
    uint32_t    offset;       // byte offset within object data
    std::string name;         // resolved human-readable name
    std::string kind_name;    // resolved kind name (e.g. "Float", "Matrix4x4")
};

// A type definition from the GameObjs block (for reference/display).
struct LevelGameObjTypeDef {
    uint32_t    crc;
    std::string name;
    uint32_t    field_count;
    uint32_t    instance_count; // how many objects of this type exist
    std::vector<GOFieldDefEntry> fields; // full field definitions for property editor
};

// A pending (not yet saved) entity created by the user.
struct PendingGameObj {
    uint32_t guid;
    uint32_t parent_guid;
    uint32_t layer_guid;
    uint32_t name_crc;
    uint32_t type_crc;
    int32_t  gamemodemask;
    float    world_transform[16];
    int      type_def_index;       // index into LevelReader::m_gameObjTypes
    // Constructor zeroes all POD fields safely (std::string/map/vector default-construct)
    PendingGameObj() : guid(0), parent_guid(0), layer_guid(0), name_crc(0), type_crc(0),
        gamemodemask(0), type_def_index(-1), mesh_crc(0), target_guid(0), sticky(0), owner_guid(0) {
        memset(world_transform, 0, sizeof(world_transform));
    }
    std::vector<uint8_t> fieldData; // raw binary field data (sized from type def)
    uint32_t mesh_crc;             // mesh CRC for placing an existing model (0 = no mesh)
    std::string name_str;          // human-readable name (for string table updates)
    // Output entity wiring (only used when type is "Output")
    std::string output_event;      // "Output" field — event name (e.g. "OnTrigger")
    std::string input_action;      // "Input" field — action on target (e.g. "Activate")
    uint32_t target_guid;          // target entity GUID
    int      sticky;               // Sticky flag (0 or 1)
    uint32_t owner_guid;           // entity whose Outputs[] array should include this Output
    // Generic field overrides — applied by CRC after donor cloning.
    // These allow setting ANY field on ANY entity type without hardcoding.
    // Key = field name CRC, Value = raw value to write.
    std::map<uint32_t, uint32_t>    intOverrides;    // Int/Bool/GUID/CRC fields (4 bytes)
    std::map<uint32_t, float>       floatOverrides;  // Float fields (4 bytes)
    std::map<uint32_t, std::string> stringOverrides; // CRC fields written from string→hash
    std::map<uint32_t, std::vector<uint32_t> > listOverrides; // ObjectList/NodeList fields written by save pipeline
    std::vector<uint32_t>           layerGuids;      // Layers[] list override (for gamemodes)
};

// Tracked field edit — records property edits for persistence through save
struct FieldEdit {
    uint32_t    entityGuid;
    std::string fieldName;
    int         kind;       // 0=int, 1=float, 2=guid, 3=vec3, 4=matrix, 5=string, 6=listItem, 7=floatArray, 8=listAppend
    int32_t     intVal;
    float       floatVal;
    uint32_t    guidVal;
    float       vec3Val[3];
    float       matrixVal[16];
    std::string stringVal;
    int         listIndex;  // for kind=6
    std::vector<float> arrayVal; // for kind=7 — flat float array (e.g. N×4 for spline nodes)
    int         arrayStride;    // for kind=7 — floats per element (4 for spline nodes)
};

// A pending BVTree world collision mesh to be written to Block1 on save.
struct PendingCollisionMesh {
    float    translation[4]; // world pos (x,y,z,0)
    float    rotation[4];    // quaternion (0,0,0,1 = identity)
    uint32_t key;            // model CRC for identification
    std::vector<float>    verts;     // x,y,z triples (local space)
    std::vector<uint16_t> indices;   // triangle indices (tri_num * 3)
    std::vector<uint8_t>  moppData;  // MOPP BVTree bytecode (from Havok)
};

// ============================================================
// LevelReader — The Big Parser. The Tomb Raider. The one class
// that reaches into Pandemic's sealed binary coffins and pulls
// out everything inside.
//
// Feed it a .PAK and a .BIN file from LOTR: Conquest.
// It decompresses the zlib blocks, parses every table in the
// header, resolves CRC→string names, builds typed entries for
// models, textures, animations, effects, collision shapes,
// game objects, and spits it all back in clean C++ vectors.
//
// This class also supports WRITING — rebuilding modified PAK/BIN
// files from scratch. Entity creation, transform editing, field
// patching, collision mesh injection... we can put data BACK
// into the format Pandemic designed. We can modify their levels.
// Their dead game is not read-only anymore. We brought it back
// and gave it a pulse.
//
// Oscar Wilde said "Life is simple, and the simple thing is the
// right thing." Oscar Wilde never parsed a 472-byte binary
// header with 25 unknown fields. Fuck Oscar Wilde.
// ============================================================

class LevelReader {
public:
    LevelReader() : m_loaded(false), m_nextGuid(1), m_gameObjsBlock1Offset(0), m_gameObjsBlock1Size(0), m_bigEndian(false) {}
    ~LevelReader() { Unload(); }

    bool Load(const std::string& pakPath, const std::string& binPath);
    void Unload();

    bool               IsLoaded()  const { return m_loaded; }
    const std::string& GetError()  const { return m_error; }

    const std::vector<LevelModelEntry>&    GetModels()     const { return m_models; }
    const std::vector<LevelTextureEntry>&  GetTextures()   const { return m_textures; }
    const std::vector<LevelAnimEntry>&     GetAnimations() const { return m_animations; }
    const std::vector<LevelEffectEntry>&   GetEffects()    const { return m_effects; }
    const std::vector<LevelGfxBlockEntry>& GetGfxBlocks()  const { return m_gfxBlocks; }
    const std::vector<BinFlatEntry>&       GetAllBinAssets() const { return m_binFlat; }
    const std::map<uint32_t, BinAssetEntry>& GetBinAssets() const { return m_binAssets; }
    const std::vector<SubBlockEntry>&       GetSubBlocks1()   const { return m_subBlocks1; }
    const std::vector<SubBlockEntry>&       GetSubBlocks2()   const { return m_subBlocks2; }
    const std::vector<LevelObjAEntry>&      GetObjAs()        const { return m_objas; }
    const std::vector<LevelObj0Entry>&      GetObj0s()        const { return m_obj0s; }
    const std::vector<LevelAnimBlockEntry>& GetAnimBlocks()   const { return m_animBlocks; }
    const std::vector<LevelPakValEntry>&    GetPakVals()      const { return m_pakVals; }

    // GameObjs — level entity placement data
    const std::vector<LevelGameObjEntry>&   GetGameObjs()     const { return m_gameObjs; }
    const std::vector<LevelGameObjTypeDef>& GetGameObjTypes() const { return m_gameObjTypes; }

    // New: all remaining PAK block1 types
    const std::vector<LevelGenericEntry>& GetBufferInfos()        const { return m_bufferInfos; }
    const std::vector<LevelGenericEntry>& GetMat1s()              const { return m_mat1s; }
    const std::vector<LevelGenericEntry>& GetMat2s()              const { return m_mat2s; }
    const std::vector<LevelGenericEntry>& GetMat3s()              const { return m_mat3s; }
    const std::vector<LevelGenericEntry>& GetMat4s()              const { return m_mat4s; }
    const std::vector<LevelGenericEntry>& GetMatExtras()          const { return m_matExtras; }
    const std::vector<LevelGenericEntry>& GetShapeInfos()         const { return m_shapeInfos; }
    const std::vector<LevelGenericEntry>& GetHkShapeInfos()       const { return m_hkShapeInfos; }
    const std::vector<LevelGenericEntry>& GetHkConstraintDatas()  const { return m_hkConstraintDatas; }
    const std::vector<LevelGenericEntry>& GetVBuffInfos()         const { return m_vbuffInfos; }
    const std::vector<LevelGenericEntry>& GetIBuffInfos()         const { return m_ibuffInfos; }
    const std::vector<LevelGenericEntry>& GetHkConstraintInfos()  const { return m_hkConstraintInfos; }
    const std::vector<LevelGenericEntry>& GetPFieldInfos()        const { return m_pFieldInfos; }
    const std::vector<LevelGenericEntry>& GetRadiosityInfos()     const { return m_radiosityInfos; }
    const std::vector<LevelGenericEntry>& GetFoliageInfos()       const { return m_foliageInfos; }

    std::string ResolveCrc(uint32_t crc) const;
    size_t      GetCrcMapSize()          const { return m_crcMap.size(); }

    // Enumerate every string in the level's known-strings table whose
    // name starts with `prefix` (case-insensitive). Output is APPENDED
    // so callers can union multiple prefixes. Optional `skipSuffixes`
    // list filters out texture variants like "_D"/"_N"/"_S" so the
    // result is just the mesh names a user would pick.
    //
    // Used by the Crowd Mesh Builder weapon dropdown — m_modelCache
    // only contains LOADED models (a fraction of the level's assets),
    // but m_crcMap has every name the level knows about, so the
    // builder can offer the full WP_/PR_ catalog even for weapons
    // that haven't been instanced yet.
    void GetStringsByPrefix(const char* prefix,
                             std::vector<std::string>& out,
                             const char* const* skipSuffixes = NULL) const;

    // Queue a custom string to be injected into pak_strings.json at next
    // save. Also inserts (crc, s) into m_crcMap immediately so any
    // subsequent ResolveCrc(crc) within this session returns the user's
    // chosen name rather than the hex fallback. Used by the 3dCrowd
    // Editor when the user renames a CrowdItem's `key` to a string the
    // BIN didn't originally know about.
    void QueueCustomString(const std::string& s);
    size_t GetQueuedStringCount() const { return m_queuedCustomStrings.size(); }
    const std::vector<std::string>& GetQueuedCustomStrings() const { return m_queuedCustomStrings; }

    bool HasBinAsset(uint32_t assetKey) const;
    bool GetBinAssetData(uint32_t assetKey, std::vector<uint8_t>& outData) const;
    // Public passthrough to the internal file reader for host code that
    // wants to slurp a sidecar JSON next to the level. Avoids each caller
    // re-implementing the same fstream-into-vector dance.
    bool ReadFileBytesPublic(const std::string& path, std::vector<uint8_t>& out) {
        return ReadFileBytes(path, out);
    }
    // Texture-only variant. Lives in the texture handle map; never returns
    // a model entry. Call this from texture loaders so a same-key model
    // cannot win the lookup. Returns false if no texture handle matches.
    bool GetBinTextureData(uint32_t assetKey, std::vector<uint8_t>& outData) const;

    // Embedded-weapon tags for a given baked CRD model. Pointer to the
    // internal vector — valid until Unload(). NULL if the model has no
    // tags (no sidecar entry or no weapons embedded). The popup mesh
    // editor calls this to enumerate weapons on a merged CRD.
    const std::vector<EmbeddedWeaponTag>* GetEmbeddedWeapons(uint32_t modelCrc) const {
        std::map<uint32_t, std::vector<EmbeddedWeaponTag> >::const_iterator it
            = m_embeddedWeapons.find(modelCrc);
        return (it != m_embeddedWeapons.end()) ? &it->second : (const std::vector<EmbeddedWeaponTag>*)0;
    }
    // Whole-map accessor. Lets host code iterate every baked CRD that
    // has weapons attached, to populate the Adjust Baked Weapon panel
    // dropdown. Reference is valid until Unload().
    const std::map<uint32_t, std::vector<EmbeddedWeaponTag> >& GetEmbeddedWeaponsMap() const {
        return m_embeddedWeapons;
    }
    // dump_dir = pak path minus ".PAK" — handy for the host when writing
    // sidecars (xform persistence) next to the level's other JSON.
    std::string GetDumpDir() const {
        if (m_pakPath.empty()) return std::string();
        std::string d = m_pakPath;
        size_t dot = d.rfind('.');
        if (dot != std::string::npos && dot + 4 == d.size()) {
            char e1 = d[dot + 1], e2 = d[dot + 2], e3 = d[dot + 3];
            bool isPak = ((e1 == 'p' || e1 == 'P') &&
                          (e2 == 'a' || e2 == 'A') &&
                          (e3 == 'k' || e3 == 'K'));
            if (isPak) d = d.substr(0, dot);
        }
        return d;
    }

    const PakHeader&   GetPakHeader() const { return m_pakHeader; }
    const BinHeader&   GetBinHeader() const { return m_binHeader; }
    const std::string& GetLevelName() const { return m_levelName; }
    const std::string& GetPakPath()   const { return m_pakPath; }
    const std::string& GetBinPath()   const { return m_binPath; }
    bool               IsBigEndian()  const { return m_bigEndian; }

    void LoadConquestStrings(const std::string& txtPath);
    // Set a persistent strings path that is re-loaded automatically on every Load() call.
    void SetBaseStringsPath(const std::string& path) { m_baseStringsPath = path; }

    const std::vector<uint8_t>& GetBlock1() const { return m_block1; }
    const std::vector<uint8_t>& GetBlock2() const { return m_block2; }

    // Modify a WorldTransform in the decompressed Block1 buffer.
    bool SetWorldTransform(uint32_t block1Offset, const float matrix[16]);

    // Modify any field in the decompressed Block1 buffer at a given offset.
    bool SetFieldValue(uint32_t block1Offset, const void* data, uint32_t size);

    // Read raw bytes from Block1 at a given offset.
    bool GetFieldValue(uint32_t block1Offset, void* outData, uint32_t size) const;

    // Save modified PAK file. Recompresses Block1 and rewrites the PAK.
    bool SavePak(const std::string& outputPath);

    // Entity creation - pending buffer.
    //
    // Note on trustIncomingGuid: when Chunk 3 (live collab broadcast) applies
    // an entity add that came from another peer, it needs the GUID the
    // ORIGINATOR assigned, not a fresh one from our local sequence. The
    // default-false param keeps every single existing caller (there are
    // around a hundred of the bastards across LevelTemplates.cpp and the
    // viewport) working exactly as before. Only the collab apply path
    // passes true. Same split logic also disables the broadcast-fire so we
    // do not bounce the edit back to the sender like a fucking echo loop.
    uint32_t GenerateGuid();
    void     AddPendingEntity(const PendingGameObj& obj, bool trustIncomingGuid = false);
    int      GetPendingCount() const { return (int)m_pendingObjs.size(); }
    const std::vector<PendingGameObj>& GetPendingObjs() const { return m_pendingObjs; }

    // Import missing type definitions from reference level.json files.
    // Searches lotrcparser directory for a level that has the needed type,
    // parses its type def, and adds it to m_gameObjTypes so AddPendingEntity works.
    // Returns the type def index for the requested type, or -1 if not found anywhere.
    int ImportTypeDef(const std::string& typeName, const std::string& lotrcParserDir);

    // Import ALL potentially needed types for kit creation at once.
    // Scans a reference level.json and imports any types not already in m_gameObjTypes.
    void ImportAllMissingTypeDefs(const std::string& lotrcParserDir);

    // Export pending entities as JSON into a Rust parser dump directory.
    // Reads level.json from dumpDir/sub_blocks1/, appends pending entities,
    // writes it back. Also updates pak_strings.json with new strings.
    // Returns true on success.
    bool ExportPendingToJson(const std::string& dumpDir);

    // Non-const access to game objs (for AddPendingEntity to append display entries)
    std::vector<LevelGameObjEntry>& GetGameObjsMut() { return m_gameObjs; }

    // Deletion tracking - GUIDs of entities removed by wipe, excluded from save.
    // AddDeletedGuid fires the collab broadcast hook. ApplyBroadcastDeleteGuid
    // is the inbound-network twin that does NOT fire the hook, so we do not
    // bounce a delete across the wire forever.
    void AddDeletedGuid(uint32_t guid);
    void ApplyBroadcastDeleteGuid(uint32_t guid) { m_deletedGuids.insert(guid); }
    const std::set<uint32_t>& GetDeletedGuids() const { return m_deletedGuids; }
    void ClearDeletedGuids() { m_deletedGuids.clear(); }

    // Field edit tracking - property edits persisted through save.
    //
    // CRITICAL implementation detail the FIRST Chunk 3 attempt fucked up:
    // the LOCAL edit path is TWO steps - SetFieldValue(absOff, ...) writes
    // the new bytes into m_block1 so the UI picks it up on the next frame,
    // AND AddFieldEdit(fe) logs the edit for the save pipeline. One hook
    // on AddFieldEdit alone only carries the SECOND half across the wire.
    // The other peer applied the FieldEdit record, but nothing ever wrote
    // the new bytes into their m_block1, so their property panel kept
    // reading the OLD value and the user saw "nothing visible changed"
    // while the save log was secretly correct. Cursed failure mode.
    //
    // So ApplyBroadcastFieldEdit is NOT a one-liner anymore: it has to
    // look up the entity by guid, resolve the field offset by name against
    // the type def, write the value into m_block1 via the same path local
    // edits use, AND push onto m_fieldEdits for save. In that order. Skip
    // the block1 write and you are back in silent-desync hell.
    void AddFieldEdit(const FieldEdit& edit);
    void ApplyBroadcastFieldEdit(const FieldEdit& edit);
    const std::vector<FieldEdit>& GetFieldEdits() const { return m_fieldEdits; }
    void ClearFieldEdits();
    void ApplyBroadcastClearFieldEdits() { m_fieldEdits.clear(); }

    // Fold every FieldEdit whose entityGuid matches a pending entity back
    // into that PendingGameObj's struct fields / override maps / fieldData.
    // Required because the EXE-side WritePendingEntitiesJson dump reads
    // straight from those three places and never looks at m_block1 OR
    // m_fieldEdits, so without this pass a freshly-created entity that
    // gets moved / has properties changed will save with INITIAL coords
    // and INITIAL field values, and the user has to relaunch + edit AGAIN
    // (the existing-entity path) just to make the second edit stick.
    // Burned multiple hours on this exact "takes 2 sessions" bug. Call
    // this once right before WritePendingEntitiesJson. Idempotent.
    void SyncPendingFromFieldEdits();

    // Clear all pending state after a successful save
    void ClearPendingState() { m_pendingObjs.clear(); m_pendingCollisions.clear(); m_deletedGuids.clear(); m_fieldEdits.clear(); }

    // Collision creation — pending BVTree meshes
    void AddPendingCollision(const PendingCollisionMesh& mesh);
    int  GetPendingCollisionCount() const { return (int)m_pendingCollisions.size(); }

    // Check if a model has per-model collision shapes (shape_num > 0 in ModelInfo)
    bool ModelHasCollision(uint32_t modelCrc) const;

    // ───── Collab broadcast hooks (Chunk 3 live-edit sync) ─────
    //
    // The networking layer (CollabSession) registers function pointers here
    // at session start. When someone on this machine calls AddFieldEdit /
    // AddPendingEntity / AddDeletedGuid / ClearFieldEdits, the respective
    // hook fires so CollabSession can serialize the payload and push it onto
    // the outbound queue. Every peer in the session sees the edit within
    // about one frame. That is the whole goddamn point.
    //
    // Static function pointers instead of a virtual interface because
    // LevelReader is a value-semantics struct, not a polymorphic thing, and
    // dragging in inheritance just to register a callback would be absurd.
    // NULL when no session is active, so the hook path is a single pointer
    // compare that branch-predicts correctly every frame. Effectively free.
    typedef void (*CollabFieldEditFn)(const FieldEdit&);
    typedef void (*CollabAddEntityFn)(const PendingGameObj&);
    typedef void (*CollabDeleteGuidFn)(uint32_t);
    typedef void (*CollabClearFieldEditsFn)();
    static void SetCollabHooks(CollabFieldEditFn onFieldEdit,
                               CollabAddEntityFn onAddEntity,
                               CollabDeleteGuidFn onDelete,
                               CollabClearFieldEditsFn onClear);
    static void ClearCollabHooks();

private:
    bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);

    // Load ze_embedded_weapons.json from <dump_dir>/ze_embedded_weapons.json
    // where dump_dir is derived from m_pakPath (extension stripped). Called
    // at the end of Load(). Silent no-op if the sidecar does not exist (level
    // has no merged CRDs baked yet). Populates m_embeddedWeapons keyed by
    // each merged model's hashed CRC.
    void LoadEmbeddedWeaponSidecar();
    bool DecompressZlib(const uint8_t* src, uint32_t srcSize,
                        uint32_t dstSize, std::vector<uint8_t>& out);
    bool ParsePak(const std::vector<uint8_t>& pakData);
    bool ParseBin(const std::vector<uint8_t>& binData);
    void ParseStringTable(const uint8_t* strBase, uint32_t bufSize,
                          uint32_t stringsOffset, uint32_t stringsNum,
                          const uint8_t* keyBase, uint32_t keysOffset);
    void ParseSubBlocks(const std::vector<uint8_t>& blockData, uint32_t sbOffset,
                        std::vector<SubBlockEntry>& out);
    void ParseGameObjs(const uint8_t* data, uint32_t size, uint32_t block1AbsOffset);
    // Generic block1 reader: reads 'count' entries of 'stride' bytes each.
    // If keyOff < stride, reads a u32 CRC from that position and resolves it.
    // extra1 = u32 at offset 4, extra2 = u32 at offset 8 (when stride permits).
    // extraOff1/extraOff2 override which byte offsets to read extra1/extra2 from.
    void ParseGenericB1(uint32_t offset, uint32_t count, uint32_t stride,
                        uint32_t keyOff, bool hasKey,
                        uint32_t extraOff1, uint32_t extraOff2,
                        std::vector<LevelGenericEntry>& out);

    // Stage 2: call after Block1 decompression on BE files. Walks every
    // known asset-table region in m_block1 and byte-swaps entries in
    // place so downstream B1Array<T>() reads see native LE values.
    // TextureInfo gets special treatment for its 4× u16 fields. No-op
    // on LE files.
    void SwapBlock1AssetTablesBE();

    // Stage 3A: call after Block2 decompression on BE files. Walks
    // VBuffInfo entries (already swapped by Stage 2) and flips every
    // vertex buffer's raw bytes as u32 arrays - floats, u32 fields,
    // and accepted garbling of the minority u16/u8 fields. Walks
    // IBuffInfo entries and flips every index buffer as u16 array
    // (indices are u16 in LOTRC). Without this, vertex positions
    // read as astronomical garbage and the renderer draws every
    // triangle outside the frustum or sub-picometer at origin.
    // No-op on LE files.
    void SwapBlock2BuffersBE();

    // Stage 4: call right before ParseGameObjs on BE files. Swaps the
    // GameObjs block payload in place so the existing parser reads
    // native little-endian values. Handles:
    //   - 5-u32 header (magic/types_num/types_off/obj_num/obj_off)
    //   - Type def table (3 u32 per type header + 3 u32 per field def)
    //   - Per-entity header (u32 layer, u32 type_key, u16 obj_size,
    //     u16 flags, u32 z4) with correct u16 handling
    //   - Per-entity field data as u32 blob swap (correct for scalars +
    //     WorldTransform matrices, wrong for list-field u16 count+offset
    //     pairs - known tradeoff, WorldTransforms are what actually
    //     place geometry so we optimize for that)
    // No-op on LE files.
    void SwapGameObjsBlockBE(uint8_t* data, uint32_t dataSize);

    template<typename T>
    const T* B1At(uint32_t offset) const {
        if (m_block1.empty() || offset + sizeof(T) > m_block1.size()) return NULL;
        return reinterpret_cast<const T*>(&m_block1[0] + offset);
    }
    template<typename T>
    const T* B1Array(uint32_t offset, uint32_t count) const {
        if (count == 0 || m_block1.empty()) return NULL;
        if (offset + sizeof(T) * count > m_block1.size()) return NULL;
        return reinterpret_cast<const T*>(&m_block1[0] + offset);
    }

    bool        m_loaded;
    std::string m_error;
    std::string m_baseStringsPath;
    std::string m_levelName;
    std::string m_pakPath;
    std::string m_binPath;

    PakHeader   m_pakHeader;
    BinHeader   m_binHeader;

    std::vector<uint8_t> m_pakRaw;
    std::vector<uint8_t> m_binRaw;
    std::vector<uint8_t> m_block1;
    std::vector<uint8_t> m_block2;

    // std::map is C++98 safe; std::unordered_map requires C++11
    std::map<uint32_t, std::string>    m_crcMap;
    std::map<uint32_t, BinAssetEntry>  m_binAssets;

    std::vector<LevelModelEntry>    m_models;
    std::vector<LevelTextureEntry>  m_textures;
    std::vector<LevelAnimEntry>     m_animations;
    std::vector<LevelEffectEntry>   m_effects;
    std::vector<LevelGfxBlockEntry> m_gfxBlocks;
    std::vector<BinFlatEntry>              m_binFlat;
    std::vector<SubBlockEntry>             m_subBlocks1;
    std::vector<SubBlockEntry>             m_subBlocks2;
    std::vector<LevelObjAEntry>            m_objas;
    std::vector<LevelObj0Entry>            m_obj0s;
    std::vector<LevelAnimBlockEntry>       m_animBlocks;
    std::vector<LevelPakValEntry>          m_pakVals;
    std::vector<LevelGameObjEntry>         m_gameObjs;
    std::vector<LevelGameObjTypeDef>       m_gameObjTypes;

    // All remaining PAK block1 types (parsed generically by stride)
    std::vector<LevelGenericEntry> m_bufferInfos;       // key=none, extra1=vbuff_off, extra2=kind
    std::vector<LevelGenericEntry> m_mat1s;             // key=key_guid@32, extra1=tex0@8, extra2=tex1@12
    std::vector<LevelGenericEntry> m_mat2s;
    std::vector<LevelGenericEntry> m_mat3s;
    std::vector<LevelGenericEntry> m_mat4s;
    std::vector<LevelGenericEntry> m_matExtras;         // key=none
    std::vector<LevelGenericEntry> m_shapeInfos;        // key=none, extra1=offset@0, extra2=kind@4
    std::vector<LevelGenericEntry> m_hkShapeInfos;      // key=none, extra1=kind@32, extra2=a_num@36
    std::vector<LevelGenericEntry> m_hkConstraintDatas; // key=none, extra1=kind@0
    std::vector<LevelGenericEntry> m_vbuffInfos;        // key=none, extra1=size@4, extra2=offset@12
    std::vector<LevelGenericEntry> m_ibuffInfos;        // key=none, extra1=size@4, extra2=format@8
    std::vector<LevelGenericEntry> m_hkConstraintInfos; // key=none, extra1=kind@0, extra2=bone_num@8
    std::vector<LevelGenericEntry> m_pFieldInfos;       // key=link_guid@0, extra1=gamemode_guid@4, extra2=width@8
    std::vector<LevelGenericEntry> m_radiosityInfos;    // key=guid@0, extra1=num@4, extra2=offset@8
    std::vector<LevelGenericEntry> m_foliageInfos;      // key=key@0, extra1=kind@4

    // Entity creation — pending buffer (added at END to preserve layout)
    std::vector<PendingGameObj>  m_pendingObjs;
    std::set<uint32_t>           m_deletedGuids; // GUIDs removed by wipe, excluded from save
    std::vector<FieldEdit>       m_fieldEdits;   // property edits to persist through save
    uint32_t                     m_nextGuid;
    uint32_t                     m_gameObjsBlock1Offset; // abs offset of GameObjs sub-block in Block1
    uint32_t                     m_gameObjsBlock1Size;   // size of GameObjs sub-block

    // Collision creation — pending BVTree meshes (added at END)
    std::vector<PendingCollisionMesh> m_pendingCollisions;

    // Cached raw string_keys block from Block1 (for rebuild)
    std::vector<uint8_t> m_stringKeysRaw;

    // Full rebuild parsed data (Phase 1+, added at END to preserve layout)
    PakParsedData m_parsedData;

    // ── Endian / platform detection (Stage 1: Xbox 360 DLC support) ─────
    // True when the loaded PAK/BIN came from an Xbox 360 or PS3 build (both
    // are PowerPC/Cell, both big-endian). Detected at BIN magic read:
    // LE magic = 6 (byte 0); BE magic = 6 at byte 3 (0x06000000 little-read).
    //
    // When true, the header-parse code byte-swaps u16/u32/u64 fields after
    // memcpy so every downstream reader sees LE-native values. Later stages
    // will extend the swap to asset-table entries and GameObjs field data.
    // For now: BinHeader + PakHeader + BinAssetHandle only. Anything past
    // that is unpredictable on 360 files until Stage 2 lands.
    //
    // DO NOT read this across DLL boundary via the args struct - it is
    // internal LevelReader state only. The DLL side never cares which
    // platform the file came from, it just sees the parsed output.
    bool m_bigEndian;

    // 3dCrowd Editor: custom strings the user typed into the Editor
    // panel's `key` field that the BIN didn't originally know about.
    // Flushed to pak_strings.json in ExportPendingToJson at save time.
    // Appended at the END per the class-layout-end commandment.
    std::vector<std::string> m_queuedCustomStrings;

    // Texture-side asset handles (END-of-class member, layout rule).
    // BIN handles come in two sections: models first (vdata_num entries),
    // then textures (texdata_num entries). lotrc_rs hashes each section
    // independently by string name, so a model and an atlas baked with
    // the same name collide on key. Single-map storage let the texture
    // (later insert) clobber the model and turn every viewport load of
    // that model into a 131072-byte texture blob mistaken for a mesh.
    // Models go in m_binAssets, textures here. Same-key entries no
    // longer step on each other; callers pick by intent.
    std::map<uint32_t, BinAssetEntry> m_binTextureAssets;

    // Embedded-weapon tags loaded from <dump_dir>/ze_embedded_weapons.json.
    // Keyed by model CRC (the merged CRD's hashed name). One model can
    // have multiple weapons attached. Populated at the end of Load() and
    // wiped on Unload(). END-of-class per layout rule.
    std::map<uint32_t, std::vector<EmbeddedWeaponTag> > m_embeddedWeapons;
};

} // namespace ZeroEngine
