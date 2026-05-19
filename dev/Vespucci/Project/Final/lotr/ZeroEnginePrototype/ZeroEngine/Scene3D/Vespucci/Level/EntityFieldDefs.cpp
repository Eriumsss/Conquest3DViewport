// EntityFieldDefs.cpp — A Fucking Encyclopedia of Every Entity Field Pandemic Ever Made
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Field offsets and default values ripped from lotrcparser output and
// cross-referenced with the .exe's type registry at 0x00A2B800. Every
// goddamn field its name CRC, its type CRC, its byte offset, its
// default value. Hundreds of them. VS2005 compatible because of course.
// -----------------------------------------------------------------------

#include "EntityFieldDefs.h"
#include <string.h>  // memcpy

namespace ZeroEngine {

// ============================================================
//  Helper functions
// ============================================================

uint32_t FieldKindFixedSize(FieldKind kind) {
    switch (kind) {
    case FK_BYTE:       return 1;
    case FK_CRC:        return 4;
    case FK_GUID:       return 4;
    case FK_COLOR:      return 4;
    case FK_INT:        return 4;
    case FK_FLOAT:      return 4;
    case FK_BOOL:       return 4;
    case FK_VECTOR2:    return 8;
    case FK_VECTOR3:    return 12;
    case FK_VECTOR4:    return 16;
    case FK_MATRIX4X4:  return 64;
    // List/String types: 4-byte header (num:u16 + offset:u16) at the field offset
    case FK_STRING:     return 4;
    case FK_OBJECTLIST: return 4;
    case FK_NODELIST:   return 4;
    case FK_INTLIST:    return 4;
    case FK_CRCLIST:    return 4;
    case FK_STRINGLIST: return 4;
    case FK_WEIGHTLIST: return 4;
    case FK_MATRIXLIST: return 4;
    default:            return 4;
    }
}

bool FieldKindIsList(FieldKind kind) {
    switch (kind) {
    case FK_STRING:
    case FK_OBJECTLIST:
    case FK_NODELIST:
    case FK_INTLIST:
    case FK_CRCLIST:
    case FK_STRINGLIST:
    case FK_WEIGHTLIST:
    case FK_MATRIXLIST:
        return true;
    default:
        return false;
    }
}

FieldKind FieldKindFromCrc(uint32_t kind_crc) {
    if (kind_crc == KIND_CRC_VAL)        return FK_CRC;
    if (kind_crc == KIND_GUID_VAL)       return FK_GUID;
    if (kind_crc == KIND_COLOR_VAL)      return FK_COLOR;
    if (kind_crc == KIND_INT_VAL)        return FK_INT;
    if (kind_crc == KIND_FLOAT_VAL)      return FK_FLOAT;
    if (kind_crc == KIND_BOOL_VAL)       return FK_BOOL;
    if (kind_crc == KIND_BYTE_VAL)       return FK_BYTE;
    if (kind_crc == KIND_VECTOR2_VAL)    return FK_VECTOR2;
    if (kind_crc == KIND_VECTOR3_VAL)    return FK_VECTOR3;
    if (kind_crc == KIND_VECTOR4_VAL)    return FK_VECTOR4;
    if (kind_crc == KIND_MATRIX4X4_VAL)  return FK_MATRIX4X4;
    if (kind_crc == KIND_STRING_VAL)     return FK_STRING;
    if (kind_crc == KIND_OBJECTLIST_VAL) return FK_OBJECTLIST;
    if (kind_crc == KIND_NODELIST_VAL)   return FK_NODELIST;
    if (kind_crc == KIND_INTLIST_VAL)    return FK_INTLIST;
    if (kind_crc == KIND_CRCLIST_VAL)    return FK_CRCLIST;
    if (kind_crc == KIND_STRINGLIST_VAL) return FK_STRINGLIST;
    if (kind_crc == KIND_WEIGHTLIST_VAL) return FK_WEIGHTLIST;
    if (kind_crc == KIND_MATRIXLIST_VAL) return FK_MATRIXLIST;
    return FK_INT; // fallback
}

// ============================================================
//  Helper macro for building field entries
// ============================================================
// D_U32(val): default as uint32
// D_I32(val): default as int32
// D_F32(val): default as float
// D_ZERO:     default zero (for lists, guids, etc.)
// D_BOOL(v):  default bool (1 or 0 as uint32, byte 0 = val on LE)

#define D_ZERO {0}

static EntityFieldDefault::DefaultVal MakeU32(uint32_t v) {
    EntityFieldDefault::DefaultVal d; d.u32_val = v; return d;
}
static EntityFieldDefault::DefaultVal MakeI32(int32_t v) {
    EntityFieldDefault::DefaultVal d; d.i32_val = v; return d;
}
static EntityFieldDefault::DefaultVal MakeF32(float v) {
    EntityFieldDefault::DefaultVal d; d.f32_val = v; return d;
}

// ============================================================
//  static_object — 42 fields
//  Offsets from lotrcparser/Training/sub_blocks1/level.json types
// ============================================================

static const EntityFieldDefault s_staticObjectFields[] = {
    // offset  0: GUID (GUID)
    { 0xCF98052Fu, KIND_GUID_VAL,       FK_GUID,       0,   D_ZERO },
    // offset  4: ParentGUID (GUID)
    { 0xBB757061u, KIND_GUID_VAL,       FK_GUID,       4,   D_ZERO },
    // offset  8: GameModeMask (Int) — default -1 (all modes)
    { 0x141C4D91u, KIND_INT_VAL,        FK_INT,        8,   D_ZERO }, // filled as MakeI32(-1) below
    // offset 12: Name (CRC)
    { 0xB11241E0u, KIND_CRC_VAL,        FK_CRC,       12,   D_ZERO },
    // offset 16: WorldTransform (Matrix4x4) — identity, set at runtime
    { 0xD486DE80u, KIND_MATRIX4X4_VAL,  FK_MATRIX4X4, 16,   D_ZERO },
    // offset 80: CreateOnLoad (Bool) — default true
    { 0x8A871EEAu, KIND_BOOL_VAL,       FK_BOOL,      80,   D_ZERO }, // filled below
    // offset 84: IsNetworkable (Bool) — default false
    { 0xC73F284Au, KIND_BOOL_VAL,       FK_BOOL,      84,   D_ZERO },
    // offset 88: IsAlwaysInScope (Bool) — default false
    { 0x99F35C5Du, KIND_BOOL_VAL,       FK_BOOL,      88,   D_ZERO },
    // offset 92: EnableEvents (Bool) — default true
    { 0xBCFEC4BEu, KIND_BOOL_VAL,       FK_BOOL,      92,   D_ZERO }, // filled below
    // offset 96: Outputs (ObjectList) — default empty
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL,  FK_OBJECTLIST, 96,  D_ZERO },
    // offset 100: InitialChildObjects (ObjectList) — default empty
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL,  FK_OBJECTLIST, 100, D_ZERO },
    // offset 104: Mesh (CRC) — set at runtime
    { 0x9805A0A6u, KIND_CRC_VAL,        FK_CRC,       104,  D_ZERO },
    // offset 108: Mesh_CastShadow (Bool) — default true
    { 0xDF430CD0u, KIND_BOOL_VAL,       FK_BOOL,      108,  D_ZERO }, // filled below
    // offset 112: Mesh_ReceiveShadows (Bool) — default true
    { 0x67FC0ABEu, KIND_BOOL_VAL,       FK_BOOL,      112,  D_ZERO }, // filled below
    // offset 116: Mesh_ReceiveLights (Bool) — default true
    { 0xB38E74BEu, KIND_BOOL_VAL,       FK_BOOL,      116,  D_ZERO }, // filled below
    // offset 120: Mesh_LOD0 (Float) — default 5.0
    { 0x7096B82Au, KIND_FLOAT_VAL,      FK_FLOAT,     120,  D_ZERO }, // filled below
    // offset 124: Mesh_LOD1 (Float) — default 10.0
    { 0x7457A59Du, KIND_FLOAT_VAL,      FK_FLOAT,     124,  D_ZERO },
    // offset 128: Mesh_LOD2 (Float) — default 20.0
    { 0x79148344u, KIND_FLOAT_VAL,      FK_FLOAT,     128,  D_ZERO },
    // offset 132: Mesh_LOD3 (Float) — default 100000.0
    { 0x7DD59EF3u, KIND_FLOAT_VAL,      FK_FLOAT,     132,  D_ZERO },
    // offset 136: Mesh_LODMaterial (Float) — default 45.0
    { 0x1DD60D0Eu, KIND_FLOAT_VAL,      FK_FLOAT,     136,  D_ZERO },
    // offset 140: Mesh_MaxShadowDistance (Float) — default 100000.0
    { 0x78E65268u, KIND_FLOAT_VAL,      FK_FLOAT,     140,  D_ZERO },
    // offset 144: Mesh_DisableLODFading (Bool) — default false
    { 0xBA25B099u, KIND_BOOL_VAL,       FK_BOOL,      144,  D_ZERO },
    // offset 148: Mesh_IgnoreFarClip (Bool) — default false
    { 0x67EF1F78u, KIND_BOOL_VAL,       FK_BOOL,      148,  D_ZERO },
    // offset 152: Mesh_LocalOccluder (Bool) — default false
    { 0xD826283Eu, KIND_BOOL_VAL,       FK_BOOL,      152,  D_ZERO },
    // offset 156: Mesh_IntersectVolume (Bool) — default false
    { 0x6AD515BCu, KIND_BOOL_VAL,       FK_BOOL,      156,  D_ZERO },
    // offset 160: stitch (Bool) — default false
    { 0x90FABAE4u, KIND_BOOL_VAL,       FK_BOOL,      160,  D_ZERO },
    // offset 164: Variation (Int) — default -1
    { 0x0F4F4522u, KIND_INT_VAL,        FK_INT,       164,  D_ZERO }, // filled below
    // offset 168: Color (Color) — default 0xFFFFFFFF
    { 0xC908780Fu, KIND_COLOR_VAL,      FK_COLOR,     168,  D_ZERO }, // filled below
    // offset 172: (gap — 4 bytes padding between Color@168 and Transform@176)
    // offset 176: Transform (Matrix4x4) — copy of WorldTransform
    { 0x869492C8u, KIND_MATRIX4X4_VAL,  FK_MATRIX4X4, 176,  D_ZERO },
    // offset 240: QualityCategory (CRC) — default CRC("Always") = 0x5A4032E8
    { 0xC4C1F163u, KIND_CRC_VAL,        FK_CRC,       240,  D_ZERO }, // filled below
    // offset 244: mesh_occluder (Bool) — default false
    { 0xB4DB5543u, KIND_BOOL_VAL,       FK_BOOL,      244,  D_ZERO },
    // offset 248: static_mesh (Bool) — default false
    { 0x70A0C8BCu, KIND_BOOL_VAL,       FK_BOOL,      248,  D_ZERO },
    // offset 252: Radiosity (Bool) — default true
    { 0xDE6FE96Du, KIND_BOOL_VAL,       FK_BOOL,      252,  D_ZERO }, // filled below
    // offset 256: IgnoreCamera (Bool) — default false
    { 0x45BC5DE2u, KIND_BOOL_VAL,       FK_BOOL,      256,  D_ZERO },
    // offset 260: CollisionAlpha (Float) — default 0.0
    { 0xB4794D38u, KIND_FLOAT_VAL,      FK_FLOAT,     260,  D_ZERO },
    // offset 264: CollisionAlphaDistance (Float) — default 0.0
    { 0x2804BF69u, KIND_FLOAT_VAL,      FK_FLOAT,     264,  D_ZERO },
    // offset 272: HUDHealthIndicatorOffset (Vector3) — default 0,0,0
    { 0x5B2F1781u, KIND_VECTOR3_VAL,    FK_VECTOR3,   272,  D_ZERO },
    // offset 284: HUDHealthIndicatorSize (Int) — default 0
    { 0xCFB2B907u, KIND_INT_VAL,        FK_INT,       284,  D_ZERO },
    // offset 288: ForceHealthIndicatorOff (Bool) — default false
    { 0x407D5237u, KIND_BOOL_VAL,       FK_BOOL,      288,  D_ZERO },
    // offset 292: CollideWithProps (Bool) — default false
    { 0x145F6E3Bu, KIND_BOOL_VAL,       FK_BOOL,      292,  D_ZERO },
    // offset 296: CollideWithProjectiles (Bool) — default false
    { 0xF5101794u, KIND_BOOL_VAL,       FK_BOOL,      296,  D_ZERO },
    // offset 300: CollideWithCreatures (Bool) — default false
    { 0xB7B3BAC5u, KIND_BOOL_VAL,       FK_BOOL,      300,  D_ZERO },
};

// We need to fill non-zero defaults at init time since VS2005 can't do designated initializers.
// Use a simple init flag.
static bool s_staticObjectInited = false;
static EntityFieldDefault s_staticObjectFieldsMut[sizeof(s_staticObjectFields)/sizeof(s_staticObjectFields[0])];

static void InitStaticObjectDefaults() {
    if (s_staticObjectInited) return;
    memcpy(s_staticObjectFieldsMut, s_staticObjectFields, sizeof(s_staticObjectFields));
    int n = sizeof(s_staticObjectFields)/sizeof(s_staticObjectFields[0]);
    for (int i = 0; i < n; ++i) {
        EntityFieldDefault& f = s_staticObjectFieldsMut[i];
        switch (f.offset) {
        case   8: f.def = MakeI32(-1);       break; // GameModeMask
        case  80: f.def = MakeU32(1);        break; // CreateOnLoad = true
        case  92: f.def = MakeU32(1);        break; // EnableEvents = true
        case 108: f.def = MakeU32(1);        break; // Mesh_CastShadow = true
        case 112: f.def = MakeU32(1);        break; // Mesh_ReceiveShadows = true
        case 116: f.def = MakeU32(1);        break; // Mesh_ReceiveLights = true
        case 120: f.def = MakeF32(5.0f);     break; // Mesh_LOD0
        case 124: f.def = MakeF32(10.0f);    break; // Mesh_LOD1
        case 128: f.def = MakeF32(20.0f);    break; // Mesh_LOD2
        case 132: f.def = MakeF32(100000.0f);break; // Mesh_LOD3
        case 136: f.def = MakeF32(45.0f);    break; // Mesh_LODMaterial
        case 140: f.def = MakeF32(100000.0f);break; // Mesh_MaxShadowDistance
        case 164: f.def = MakeI32(-1);       break; // Variation
        case 168: f.def = MakeU32(0xFFFFFFFFu); break; // Color
        case 240: f.def = MakeU32(0x5A4032E8u); break; // QualityCategory = CRC("Always")
        case 252: f.def = MakeU32(1);        break; // Radiosity = true
        }
    }
    s_staticObjectInited = true;
}


// ============================================================
//  Level.json entity type field definitions (18 types)
//  Generated from level.json binary layout analysis
// ============================================================

// templateLevel -- 21 fields
static const EntityFieldDefault s_templateLevelFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0x0528F11Cu, KIND_GUID_VAL           , FK_GUID         ,   16, D_ZERO }, // Atmosphere
    { 0x50E80EF4u, KIND_GUID_VAL           , FK_GUID         ,   20, D_ZERO }, // Atmosphere_Low
    { 0xDE6FE96Du, KIND_CRC_VAL            , FK_CRC          ,   24, D_ZERO }, // Radiosity
    { 0x731CA3AAu, KIND_FLOAT_VAL          , FK_FLOAT        ,   28, D_ZERO }, // convergence
    { 0xADB13385u, KIND_FLOAT_VAL          , FK_FLOAT        ,   32, D_ZERO }, // multiplier
    { 0xE32782B3u, KIND_FLOAT_VAL          , FK_FLOAT        ,   36, D_ZERO }, // DrawDistance
    { 0x093413C1u, KIND_FLOAT_VAL          , FK_FLOAT        ,   40, D_ZERO }, // CullDistance
    { 0x554CAFADu, KIND_FLOAT_VAL          , FK_FLOAT        ,   44, D_ZERO }, // CullDistanceLowQuality
    { 0x2EA7EC3Eu, KIND_FLOAT_VAL          , FK_FLOAT        ,   48, D_ZERO }, // LodFactor
    { 0x994851F2u, KIND_COLOR_VAL          , FK_COLOR        ,   52, D_ZERO }, // ClearColor
    { 0x33A93C5Cu, KIND_GUID_VAL           , FK_GUID         ,   56, D_ZERO }, // Music
    { 0xEF005016u, KIND_GUID_VAL           , FK_GUID         ,   60, D_ZERO }, // RumbleDefinition
    { 0x78E49743u, KIND_GUID_VAL           , FK_GUID         ,   64, D_ZERO }, // SoundEnvironment
    { 0xBDAF028Du, KIND_GUID_VAL           , FK_GUID         ,   68, D_ZERO }, // HavokBroadphase
    { 0xD6A8B702u, KIND_STRING_VAL         , FK_STRING       ,   72, D_ZERO }, // target
    { 0x1A5CD0F3u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   76, D_ZERO }, // Layers
    { 0x98C472A7u, KIND_STRINGLIST_VAL     , FK_STRINGLIST   ,   80, D_ZERO }, // LevelSpecificBanks
};
static EntityFieldDefault s_templateLevelFieldsMut[sizeof(s_templateLevelFields)/sizeof(s_templateLevelFields[0])];
static bool s_templateLevelInited = false;

// templateLayer -- 4 fields
static const EntityFieldDefault s_templateLayerFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
};
static EntityFieldDefault s_templateLayerFieldsMut[sizeof(s_templateLayerFields)/sizeof(s_templateLayerFields[0])];
static bool s_templateLayerInited = false;

// spawn_point -- 22 fields
static const EntityFieldDefault s_spawn_pointFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  184, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
    { 0xEDF0E1CFu, KIND_INT_VAL            , FK_INT          ,  192, D_ZERO }, // Team
    { 0xC1A63FD5u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  196, D_ZERO }, // Nodes
    { 0x82B74BDEu, KIND_INT_VAL            , FK_INT          ,  200, D_ZERO }, // QueueSize
    { 0x466AEC18u, KIND_GUID_VAL           , FK_GUID         ,  204, D_ZERO }, // DemoCam
    { 0xFDF39D47u, KIND_INT_VAL            , FK_INT          ,  208, D_ZERO }, // PriorityTeam1
    { 0xF0B0BB9Eu, KIND_INT_VAL            , FK_INT          ,  212, D_ZERO }, // PriorityTeam2
};
static EntityFieldDefault s_spawn_pointFieldsMut[sizeof(s_spawn_pointFields)/sizeof(s_spawn_pointFields[0])];
static bool s_spawn_pointInited = false;

// spawn_node -- 10 fields
static const EntityFieldDefault s_spawn_nodeFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   80, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  144, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  148, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  152, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  156, D_ZERO }, // Texture
};
static EntityFieldDefault s_spawn_nodeFieldsMut[sizeof(s_spawn_nodeFields)/sizeof(s_spawn_nodeFields[0])];
static bool s_spawn_nodeInited = false;

// spawn_class -- 7 fields
static const EntityFieldDefault s_spawn_classFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xE6B9716Au, KIND_GUID_VAL           , FK_GUID         ,   16, D_ZERO }, // CharacterClass
    { 0xA445F705u, KIND_INT_VAL            , FK_INT          ,   20, D_ZERO }, // Min
    { 0x789A2295u, KIND_INT_VAL            , FK_INT          ,   24, D_ZERO }, // max
};
static EntityFieldDefault s_spawn_classFieldsMut[sizeof(s_spawn_classFields)/sizeof(s_spawn_classFields[0])];
static bool s_spawn_classInited = false;

// spawn_emitter -- 49 fields
static const EntityFieldDefault s_spawn_emitterFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x9805A0A6u, KIND_CRC_VAL            , FK_CRC          ,  104, D_ZERO }, // Mesh
    { 0xDF430CD0u, KIND_BOOL_VAL           , FK_BOOL         ,  108, D_ZERO }, // Mesh_CastShadow
    { 0x67FC0ABEu, KIND_BOOL_VAL           , FK_BOOL         ,  112, D_ZERO }, // Mesh_ReceiveShadows
    { 0xB38E74BEu, KIND_BOOL_VAL           , FK_BOOL         ,  116, D_ZERO }, // Mesh_ReceiveLights
    { 0x7096B82Au, KIND_FLOAT_VAL          , FK_FLOAT        ,  120, D_ZERO }, // Mesh_LOD0
    { 0x7457A59Du, KIND_FLOAT_VAL          , FK_FLOAT        ,  124, D_ZERO }, // Mesh_LOD1
    { 0x79148344u, KIND_FLOAT_VAL          , FK_FLOAT        ,  128, D_ZERO }, // Mesh_LOD2
    { 0x7DD59EF3u, KIND_FLOAT_VAL          , FK_FLOAT        ,  132, D_ZERO }, // Mesh_LOD3
    { 0x1DD60D0Eu, KIND_FLOAT_VAL          , FK_FLOAT        ,  136, D_ZERO }, // Mesh_LODMaterial
    { 0x78E65268u, KIND_FLOAT_VAL          , FK_FLOAT        ,  140, D_ZERO }, // Mesh_MaxShadowDistance
    { 0xBA25B099u, KIND_BOOL_VAL           , FK_BOOL         ,  144, D_ZERO }, // Mesh_DisableLODFading
    { 0x67EF1F78u, KIND_BOOL_VAL           , FK_BOOL         ,  148, D_ZERO }, // Mesh_IgnoreFarClip
    { 0xD826283Eu, KIND_BOOL_VAL           , FK_BOOL         ,  152, D_ZERO }, // Mesh_LocalOccluder
    { 0x6AD515BCu, KIND_BOOL_VAL           , FK_BOOL         ,  156, D_ZERO }, // Mesh_IntersectVolume
    { 0x90FABAE4u, KIND_BOOL_VAL           , FK_BOOL         ,  160, D_ZERO }, // stitch
    { 0x0F4F4522u, KIND_INT_VAL            , FK_INT          ,  164, D_ZERO }, // Variation
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  168, D_ZERO }, // Color
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  176, D_ZERO }, // Transform
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  240, D_ZERO }, // Type
    { 0x362FA3AAu, KIND_STRING_VAL         , FK_STRING       ,  244, D_ZERO }, // size
    { 0x230410EFu, KIND_STRING_VAL         , FK_STRING       ,  248, D_ZERO }, // Outer
    { 0xAD2841E3u, KIND_BOOL_VAL           , FK_BOOL         ,  252, D_ZERO }, // ActiveAtStart
    { 0x92058946u, KIND_BOOL_VAL           , FK_BOOL         ,  256, D_ZERO }, // IsHeroEmitter
    { 0x760898D7u, KIND_BOOL_VAL           , FK_BOOL         ,  260, D_ZERO }, // IsAIDriven
    { 0xFD13CE92u, KIND_CRC_VAL            , FK_CRC          ,  264, D_ZERO }, // EmitterID
    { 0x33CED45Du, KIND_INT_VAL            , FK_INT          ,  268, D_ZERO }, // Delay
    { 0x6EF8A502u, KIND_INT_VAL            , FK_INT          ,  272, D_ZERO }, // NumberOfDeadForRespawn
    { 0xCA6F4ABEu, KIND_BOOL_VAL           , FK_BOOL         ,  276, D_ZERO }, // OrDelayAndNumberOfDead
    { 0xEDF0E1CFu, KIND_INT_VAL            , FK_INT          ,  280, D_ZERO }, // Team
    { 0xB3E7FE20u, KIND_INT_VAL            , FK_INT          ,  284, D_ZERO }, // CreatureCount
    { 0xD65BED13u, KIND_INT_VAL            , FK_INT          ,  288, D_ZERO }, // Reinforcements
    { 0x0FFAC86Bu, KIND_FLOAT_VAL          , FK_FLOAT        ,  292, D_ZERO }, // SpawnPointDetectionRadius
    { 0x761FCAE2u, KIND_VECTOR3_VAL        , FK_VECTOR3      ,  304, D_ZERO }, // CarouselOffset
    { 0x37876E10u, KIND_FLOAT_VAL          , FK_FLOAT        ,  316, D_ZERO }, // CarouselRadius
    { 0xF4940CCFu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  320, D_ZERO }, // Points
    { 0x769A103Eu, KIND_GUID_VAL           , FK_GUID         ,  324, D_ZERO }, // GROUP
    { 0x3C96D3B5u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  328, D_ZERO }, // GroupList
    { 0x574060EAu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  332, D_ZERO }, // Classes
};
static EntityFieldDefault s_spawn_emitterFieldsMut[sizeof(s_spawn_emitterFields)/sizeof(s_spawn_emitterFields[0])];
static bool s_spawn_emitterInited = false;

// PlayerRespawner -- 24 fields
static const EntityFieldDefault s_PlayerRespawnerFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
    { 0x7B7992C6u, KIND_FLOAT_VAL          , FK_FLOAT        ,  192, D_ZERO }, // UpperLeftX
    { 0x72FBA9A8u, KIND_FLOAT_VAL          , FK_FLOAT        ,  196, D_ZERO }, // UpperLeftZ
    { 0x7F5CD2B2u, KIND_FLOAT_VAL          , FK_FLOAT        ,  200, D_ZERO }, // LowerRightX
    { 0x76DEE9DCu, KIND_FLOAT_VAL          , FK_FLOAT        ,  204, D_ZERO }, // LowerRightZ
    { 0x595FF06Au, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  208, D_ZERO }, // TeamEmitters
    { 0x8AE5F98Au, KIND_GUID_VAL           , FK_GUID         ,  212, D_ZERO }, // Team1PlayerEmitter
    { 0x8A3F3362u, KIND_GUID_VAL           , FK_GUID         ,  216, D_ZERO }, // Team1HeroEmitter
    { 0x42FDAAC9u, KIND_GUID_VAL           , FK_GUID         ,  220, D_ZERO }, // Team2PlayerEmitter
    { 0x82241668u, KIND_GUID_VAL           , FK_GUID         ,  224, D_ZERO }, // Team2HeroEmitter
};
static EntityFieldDefault s_PlayerRespawnerFieldsMut[sizeof(s_PlayerRespawnerFields)/sizeof(s_PlayerRespawnerFields[0])];
static bool s_PlayerRespawnerInited = false;

// PointManager -- 26 fields
static const EntityFieldDefault s_PointManagerFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
    { 0x47B1AF22u, KIND_INT_VAL            , FK_INT          ,  192, D_ZERO }, // VictoryPoints
    { 0xEDF0E1CFu, KIND_INT_VAL            , FK_INT          ,  196, D_ZERO }, // Team
    { 0xDE0CCA0Eu, KIND_INT_VAL            , FK_INT          ,  200, D_ZERO }, // Notify1
    { 0xD34FECD7u, KIND_INT_VAL            , FK_INT          ,  204, D_ZERO }, // Notify2
    { 0xD78EF160u, KIND_INT_VAL            , FK_INT          ,  208, D_ZERO }, // Notify3
    { 0x65E86346u, KIND_BOOL_VAL           , FK_BOOL         ,  212, D_ZERO }, // ACTIVE
    { 0x565EF300u, KIND_CRCLIST_VAL        , FK_CRCLIST      ,  216, D_ZERO }, // VO50PointsToGo
    { 0x49857F18u, KIND_CRCLIST_VAL        , FK_CRCLIST      ,  220, D_ZERO }, // VO10PointsToGo
    { 0xC0E3980Fu, KIND_CRCLIST_VAL        , FK_CRCLIST      ,  224, D_ZERO }, // VO5PointsToGo
    { 0xA8C8CF2Au, KIND_CRCLIST_VAL        , FK_CRCLIST      ,  228, D_ZERO }, // VO1PointToGo
    { 0xF6E165ADu, KIND_CRCLIST_VAL        , FK_CRCLIST      ,  232, D_ZERO }, // VOVictory
};
static EntityFieldDefault s_PointManagerFieldsMut[sizeof(s_PointManagerFields)/sizeof(s_PointManagerFields[0])];
static bool s_PointManagerInited = false;

// PathNetwork -- 14 fields
static const EntityFieldDefault s_PathNetworkFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   32, D_ZERO }, // WorldTransform
    { 0xC1A63FD5u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Nodes
    { 0xF1B955D1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // links
    { 0x0627E4ECu, KIND_INT_VAL            , FK_INT          ,  104, D_ZERO }, // SoldierSize
    { 0x1D5BE05Eu, KIND_INT_VAL            , FK_INT          ,  108, D_ZERO }, // GiantSize
    { 0x71323724u, KIND_INT_VAL            , FK_INT          ,  112, D_ZERO }, // MountSize
    { 0xF174C608u, KIND_INT_VAL            , FK_INT          ,  116, D_ZERO }, // SiegeSize
    { 0xAA392B49u, KIND_INT_VAL            , FK_INT          ,  120, D_ZERO }, // OliphantSize
    { 0x33DD3B30u, KIND_INT_VAL            , FK_INT          ,  124, D_ZERO }, // OliphantSmallMarginSize
    { 0x815DE847u, KIND_INT_VAL            , FK_INT          ,  128, D_ZERO }, // OliphantLargeMarginSize
};
static EntityFieldDefault s_PathNetworkFieldsMut[sizeof(s_PathNetworkFields)/sizeof(s_PathNetworkFields[0])];
static bool s_PathNetworkInited = false;

// PathNode -- 6 fields
static const EntityFieldDefault s_PathNodeFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   80, D_ZERO }, // Transform
};
static EntityFieldDefault s_PathNodeFieldsMut[sizeof(s_PathNodeFields)/sizeof(s_PathNodeFields[0])];
static bool s_PathNodeInited = false;

// PathLink -- 16 fields
static const EntityFieldDefault s_PathLinkFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0x8E4545D5u, KIND_WEIGHTLIST_VAL     , FK_WEIGHTLIST   ,   16, D_ZERO }, // PathWeights1
    { 0x8306630Cu, KIND_WEIGHTLIST_VAL     , FK_WEIGHTLIST   ,   20, D_ZERO }, // PathWeights2
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   32, D_ZERO }, // WorldTransform
    { 0x2B50BFF3u, KIND_BOOL_VAL           , FK_BOOL         ,  108, D_ZERO }, // backward
    { 0x2F6D7F50u, KIND_FLOAT_VAL          , FK_FLOAT        ,  128, D_ZERO }, // Width
    { 0x1AE59A6Bu, KIND_FLOAT_VAL          , FK_FLOAT        ,  132, D_ZERO }, // length1
    { 0x17A6BCB2u, KIND_FLOAT_VAL          , FK_FLOAT        ,  136, D_ZERO }, // length2
    { 0x239B3316u, KIND_FLOAT_VAL          , FK_FLOAT        ,  144, D_ZERO }, // top
    { 0x207B49D0u, KIND_BOOL_VAL           , FK_BOOL         ,  148, D_ZERO }, // use1
    { 0x2D386F09u, KIND_BOOL_VAL           , FK_BOOL         ,  152, D_ZERO }, // use2
    { 0x29F972BEu, KIND_BOOL_VAL           , FK_BOOL         ,  156, D_ZERO }, // use3
    { 0x37BE22BBu, KIND_BOOL_VAL           , FK_BOOL         ,  160, D_ZERO }, // use4
};
static EntityFieldDefault s_PathLinkFieldsMut[sizeof(s_PathLinkFields)/sizeof(s_PathLinkFields[0])];
static bool s_PathLinkInited = false;

// Output -- 10 fields
static const EntityFieldDefault s_OutputFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0xD6A8B702u, KIND_GUID_VAL           , FK_GUID         ,   84, D_ZERO }, // target
    { 0xF2D63F0Cu, KIND_CRC_VAL            , FK_CRC          ,   88, D_ZERO }, // Input
    { 0x33CED45Du, KIND_FLOAT_VAL          , FK_FLOAT        ,   92, D_ZERO }, // Delay
    { 0x4AE3B094u, KIND_BOOL_VAL           , FK_BOOL         ,   96, D_ZERO }, // Sticky
    { 0xFFB8F4D3u, KIND_STRING_VAL         , FK_STRING       ,  100, D_ZERO }, // Parameter
};
static EntityFieldDefault s_OutputFieldsMut[sizeof(s_OutputFields)/sizeof(s_OutputFields[0])];
static bool s_OutputInited = false;

// logic_relay -- 16 fields
static const EntityFieldDefault s_logic_relayFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  184, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
};
static EntityFieldDefault s_logic_relayFieldsMut[sizeof(s_logic_relayFields)/sizeof(s_logic_relayFields[0])];
static bool s_logic_relayInited = false;

// logic_endgame -- 16 fields
static const EntityFieldDefault s_logic_endgameFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  184, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
};
static EntityFieldDefault s_logic_endgameFieldsMut[sizeof(s_logic_endgameFields)/sizeof(s_logic_endgameFields[0])];
static bool s_logic_endgameInited = false;

// logic_gamestart -- 17 fields
static const EntityFieldDefault s_logic_gamestartFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  112, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  176, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  180, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  184, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  188, D_ZERO }, // Texture
    { 0x18C6AE80u, KIND_BOOL_VAL           , FK_BOOL         ,  244, D_ZERO }, // DisableAI
};
static EntityFieldDefault s_logic_gamestartFieldsMut[sizeof(s_logic_gamestartFields)/sizeof(s_logic_gamestartFields[0])];
static bool s_logic_gamestartInited = false;

// trigger_radius -- 29 fields
static const EntityFieldDefault s_trigger_radiusFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x8A871EEAu, KIND_BOOL_VAL           , FK_BOOL         ,   80, D_ZERO }, // CreateOnLoad
    { 0xC73F284Au, KIND_BOOL_VAL           , FK_BOOL         ,   84, D_ZERO }, // IsNetworkable
    { 0x99F35C5Du, KIND_BOOL_VAL           , FK_BOOL         ,   88, D_ZERO }, // IsAlwaysInScope
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x0E5FB5D2u, KIND_BOOL_VAL           , FK_BOOL         ,  104, D_ZERO }, // OnePlayerOnly
    { 0xFD90A09Du, KIND_BOOL_VAL           , FK_BOOL         ,  108, D_ZERO }, // UseDead
    { 0x2C48771Cu, KIND_BOOL_VAL           , FK_BOOL         ,  112, D_ZERO }, // AllPlayers
    { 0x284D7D31u, KIND_BOOL_VAL           , FK_BOOL         ,  116, D_ZERO }, // UseTeam
    { 0x7DDB7F84u, KIND_BOOL_VAL           , FK_BOOL         ,  120, D_ZERO }, // DetectStealthed
    { 0x910D9BB2u, KIND_BOOL_VAL           , FK_BOOL         ,  124, D_ZERO }, // OnlyStealthed
    { 0xEDF0E1CFu, KIND_INT_VAL            , FK_INT          ,  128, D_ZERO }, // Team
    { 0x3FAC0E6Bu, KIND_CRC_VAL            , FK_CRC          ,  132, D_ZERO }, // CreatureType
    { 0xE7D3F5FFu, KIND_GUID_VAL           , FK_GUID         ,  136, D_ZERO }, // SpecificCreature
    { 0x74C115FCu, KIND_GUID_VAL           , FK_GUID         ,  140, D_ZERO }, // SpecificObject
    { 0xED5B6407u, KIND_CRC_VAL            , FK_CRC          ,  144, D_ZERO }, // ObjectFilter
    { 0x15CA4255u, KIND_BOOL_VAL           , FK_BOOL         ,  148, D_ZERO }, // EnableDebugRendering
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,  160, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  224, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  228, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  232, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  236, D_ZERO }, // Texture
    { 0x362FA3AAu, KIND_FLOAT_VAL          , FK_FLOAT        ,  240, D_ZERO }, // size
};
static EntityFieldDefault s_trigger_radiusFieldsMut[sizeof(s_trigger_radiusFields)/sizeof(s_trigger_radiusFields[0])];
static bool s_trigger_radiusInited = false;

// AtmosphereSetting -- 22 fields
static const EntityFieldDefault s_AtmosphereSettingFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0xBCFEC4BEu, KIND_BOOL_VAL           , FK_BOOL         ,   92, D_ZERO }, // EnableEvents
    { 0x250BE3C1u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,   96, D_ZERO }, // Outputs
    { 0x68C1B52Eu, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  100, D_ZERO }, // InitialChildObjects
    { 0x08E104EAu, KIND_FLOAT_VAL          , FK_FLOAT        ,  108, D_ZERO }, // BlendWeight0
    { 0x0C20195Du, KIND_FLOAT_VAL          , FK_FLOAT        ,  112, D_ZERO }, // BlendWeight1
    { 0x01633F84u, KIND_FLOAT_VAL          , FK_FLOAT        ,  116, D_ZERO }, // BlendWeight2
    { 0x05A22233u, KIND_FLOAT_VAL          , FK_FLOAT        ,  120, D_ZERO }, // BlendWeight3
    { 0x3DACF599u, KIND_FLOAT_VAL          , FK_FLOAT        ,  124, D_ZERO }, // key
    { 0x4CDE8806u, KIND_FLOAT_VAL          , FK_FLOAT        ,  128, D_ZERO }, // AdaptationSpeed
    { 0x3081C87Bu, KIND_FLOAT_VAL          , FK_FLOAT        ,  132, D_ZERO }, // gamma
    { 0x37C1C237u, KIND_FLOAT_VAL          , FK_FLOAT        ,  136, D_ZERO }, // Whitepoint
    { 0x075A11D1u, KIND_FLOAT_VAL          , FK_FLOAT        ,  180, D_ZERO }, // InscatteringMultiplier
    { 0x1D9348F6u, KIND_FLOAT_VAL          , FK_FLOAT        ,  188, D_ZERO }, // BetaRayMultipler
    { 0x55983B18u, KIND_FLOAT_VAL          , FK_FLOAT        ,  200, D_ZERO }, // AtmosphereLimit
    { 0x32AA679Fu, KIND_FLOAT_VAL          , FK_FLOAT        ,  204, D_ZERO }, // AtmosphereForce
    { 0x1A7B62BEu, KIND_FLOAT_VAL          , FK_FLOAT        ,  212, D_ZERO }, // HazeForce
    { 0x1B6E48A2u, KIND_GUID_VAL           , FK_GUID         ,  232, D_ZERO }, // Wind
};
static EntityFieldDefault s_AtmosphereSettingFieldsMut[sizeof(s_AtmosphereSettingFields)/sizeof(s_AtmosphereSettingFields[0])];
static bool s_AtmosphereSettingInited = false;

// gamemode -- 49 fields
static const EntityFieldDefault s_gamemodeFields[] = {
    { 0xCF98052Fu, KIND_GUID_VAL           , FK_GUID         ,    0, D_ZERO }, // GUID
    { 0xBB757061u, KIND_GUID_VAL           , FK_GUID         ,    4, D_ZERO }, // ParentGUID
    { 0x141C4D91u, KIND_INT_VAL            , FK_INT          ,    8, D_ZERO }, // GameModeMask
    { 0xB11241E0u, KIND_CRC_VAL            , FK_CRC          ,   12, D_ZERO }, // Name
    { 0xD486DE80u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   16, D_ZERO }, // WorldTransform
    { 0x869492C8u, KIND_MATRIX4X4_VAL      , FK_MATRIX4X4    ,   80, D_ZERO }, // Transform
    { 0xC908780Fu, KIND_COLOR_VAL          , FK_COLOR        ,  144, D_ZERO }, // Color
    { 0x1D9D48ECu, KIND_STRING_VAL         , FK_STRING       ,  148, D_ZERO }, // Type
    { 0x230410EFu, KIND_FLOAT_VAL          , FK_FLOAT        ,  152, D_ZERO }, // Outer
    { 0x7951FC0Bu, KIND_STRING_VAL         , FK_STRING       ,  156, D_ZERO }, // Texture
    { 0x1A5CD0F3u, KIND_OBJECTLIST_VAL     , FK_OBJECTLIST   ,  160, D_ZERO }, // Layers
    { 0x8322FE2Au, KIND_CRC_VAL            , FK_CRC          ,  164, D_ZERO }, // Mode
    { 0x6428825Eu, KIND_STRING_VAL         , FK_STRING       ,  168, D_ZERO }, // ModeName
    { 0xD2FB7BA5u, KIND_GUID_VAL           , FK_GUID         ,  172, D_ZERO }, // MapNW
    { 0x9A4CA38Fu, KIND_GUID_VAL           , FK_GUID         ,  176, D_ZERO }, // MapSE
    { 0x33A93C5Cu, KIND_GUID_VAL           , FK_GUID         ,  180, D_ZERO }, // Music
    { 0x0528F11Cu, KIND_GUID_VAL           , FK_GUID         ,  184, D_ZERO }, // Atmosphere
    { 0x50E80EF4u, KIND_GUID_VAL           , FK_GUID         ,  188, D_ZERO }, // Atmosphere_Low
    { 0xA521E37Bu, KIND_STRING_VAL         , FK_STRING       ,  192, D_ZERO }, // Intro
    { 0xA83F20AFu, KIND_STRING_VAL         , FK_STRING       ,  196, D_ZERO }, // Outtro
    { 0x1DAC4465u, KIND_GUID_VAL           , FK_GUID         ,  200, D_ZERO }, // IntroCamera
    { 0x94CDBFC2u, KIND_STRINGLIST_VAL     , FK_STRINGLIST   ,  204, D_ZERO }, // ModeSpecificBanks
    { 0x05D9C2DDu, KIND_CRC_VAL            , FK_CRC          ,  208, D_ZERO }, // MatchStartMsg
    { 0x3F24AA64u, KIND_CRC_VAL            , FK_CRC          ,  212, D_ZERO }, // MatchStartVO
    { 0x6EC689EFu, KIND_INT_VAL            , FK_INT          ,  216, D_ZERO }, // MP_MinPlayerCount
    { 0x01716E86u, KIND_FLOAT_VAL          , FK_FLOAT        ,  220, D_ZERO }, // MP_WarmupTime
    { 0xD0C64BC2u, KIND_FLOAT_VAL          , FK_FLOAT        ,  224, D_ZERO }, // MP_SpawnDelay
    { 0x2DD7CF8Du, KIND_FLOAT_VAL          , FK_FLOAT        ,  228, D_ZERO }, // MP_SpawnWindow
    { 0xEB95375Fu, KIND_INT_VAL            , FK_INT          ,  232, D_ZERO }, // RB_VictoryPoints
    { 0x02BD9EABu, KIND_INT_VAL            , FK_INT          ,  236, D_ZERO }, // RB_PointsPerSecond
    { 0x4675F6D3u, KIND_INT_VAL            , FK_INT          ,  240, D_ZERO }, // RB_PointsPerKill
    { 0xC2236416u, KIND_CRC_VAL            , FK_CRC          ,  244, D_ZERO }, // RB_FrodoBirthEffect
    { 0x5BE76C57u, KIND_FLOAT_VAL          , FK_FLOAT        ,  248, D_ZERO }, // CQ_UpdateDelay
    { 0x3C2F6C93u, KIND_INT_VAL            , FK_INT          ,  252, D_ZERO }, // CQ_Team1_PerPointRate
    { 0xF4373FD0u, KIND_INT_VAL            , FK_INT          ,  256, D_ZERO }, // CQ_Team2_PerPointRate
    { 0x84B71FF7u, KIND_INT_VAL            , FK_INT          ,  260, D_ZERO }, // CQ_Team1_TrickleRate
    { 0xB99AF84Fu, KIND_INT_VAL            , FK_INT          ,  264, D_ZERO }, // CQ_Team2_TrickleRate
    { 0xBB339058u, KIND_CRC_VAL            , FK_CRC          ,  268, D_ZERO }, // Team1CPCompleteVO
    { 0x861E77E0u, KIND_CRC_VAL            , FK_CRC          ,  272, D_ZERO }, // Team2CPCompleteVO
    { 0x29A4F24Eu, KIND_BOOL_VAL           , FK_BOOL         ,  276, D_ZERO }, // ShowPoints
    { 0x7233CDBCu, KIND_BOOL_VAL           , FK_BOOL         ,  280, D_ZERO }, // AutoSelectSpawnPoint
    { 0x45A39FEAu, KIND_BOOL_VAL           , FK_BOOL         ,  284, D_ZERO }, // AutoSelectClass
    { 0x15CEAEC2u, KIND_BOOL_VAL           , FK_BOOL         ,  288, D_ZERO }, // UsesAI
    { 0x65AABB37u, KIND_INT_VAL            , FK_INT          ,  292, D_ZERO }, // MaxAISpawnTeam1
    { 0x68E99DEEu, KIND_INT_VAL            , FK_INT          ,  296, D_ZERO }, // MaxAISpawnTeam2
    { 0x3D3A7E8Cu, KIND_FLOAT_VAL          , FK_FLOAT        ,  300, D_ZERO }, // AITeam1Difficulty
    { 0x330EEB09u, KIND_FLOAT_VAL          , FK_FLOAT        ,  304, D_ZERO }, // AITeam2Difficulty
    { 0xC3AA6DF3u, KIND_FLOAT_VAL          , FK_FLOAT        ,  308, D_ZERO }, // AITeam1DifficultyIncrement
    { 0x9FADFD89u, KIND_FLOAT_VAL          , FK_FLOAT        ,  312, D_ZERO }, // AITeam2DifficultyIncrement
};
static EntityFieldDefault s_gamemodeFieldsMut[sizeof(s_gamemodeFields)/sizeof(s_gamemodeFields[0])];
static bool s_gamemodeInited = false;

// ============================================================
//  Type table
// ============================================================

static const uint32_t TYPE_STATIC_OBJECT = 0x408E062Eu;

static EntityTypeDefaults s_typeTable[] = {
    { 0x408E062Eu, NULL, sizeof(s_staticObjectFields)/sizeof(s_staticObjectFields[0]) }, // static_object
    { 0x1A4EE5CBu, NULL, sizeof(s_templateLevelFields)/sizeof(s_templateLevelFields[0]) }, // templateLevel
    { 0x1DEAF119u, NULL, sizeof(s_templateLayerFields)/sizeof(s_templateLayerFields[0]) }, // templateLayer
    { 0x6CEFA47Eu, NULL, sizeof(s_spawn_pointFields)/sizeof(s_spawn_pointFields[0]) }, // spawn_point
    { 0x58F03EEAu, NULL, sizeof(s_spawn_nodeFields)/sizeof(s_spawn_nodeFields[0]) }, // spawn_node
    { 0x49FBBC82u, NULL, sizeof(s_spawn_classFields)/sizeof(s_spawn_classFields[0]) }, // spawn_class
    { 0x03BA01EFu, NULL, sizeof(s_spawn_emitterFields)/sizeof(s_spawn_emitterFields[0]) }, // spawn_emitter
    { 0xDADAEE19u, NULL, sizeof(s_PlayerRespawnerFields)/sizeof(s_PlayerRespawnerFields[0]) }, // PlayerRespawner
    { 0x6F041B85u, NULL, sizeof(s_PointManagerFields)/sizeof(s_PointManagerFields[0]) }, // PointManager
    { 0xAD144559u, NULL, sizeof(s_PathNetworkFields)/sizeof(s_PathNetworkFields[0]) }, // PathNetwork
    { 0x34867852u, NULL, sizeof(s_PathNodeFields)/sizeof(s_PathNodeFields[0]) }, // PathNode
    { 0x9A7BC6CEu, NULL, sizeof(s_PathLinkFields)/sizeof(s_PathLinkFields[0]) }, // PathLink
    { 0xFFC8FCD8u, NULL, sizeof(s_OutputFields)/sizeof(s_OutputFields[0]) }, // Output
    { 0x9785621Bu, NULL, sizeof(s_logic_relayFields)/sizeof(s_logic_relayFields[0]) }, // logic_relay
    { 0xE91E3891u, NULL, sizeof(s_logic_endgameFields)/sizeof(s_logic_endgameFields[0]) }, // logic_endgame
    { 0x52671E5Eu, NULL, sizeof(s_logic_gamestartFields)/sizeof(s_logic_gamestartFields[0]) }, // logic_gamestart
    { 0x5B0BBE6Du, NULL, sizeof(s_trigger_radiusFields)/sizeof(s_trigger_radiusFields[0]) }, // trigger_radius
    { 0x459D0207u, NULL, sizeof(s_AtmosphereSettingFields)/sizeof(s_AtmosphereSettingFields[0]) }, // AtmosphereSetting
    { 0x23B429B1u, NULL, sizeof(s_gamemodeFields)/sizeof(s_gamemodeFields[0]) }, // gamemode
};

static bool s_tableInited = false;

static void InitTypeTable() {
    if (s_tableInited) return;
    InitStaticObjectDefaults();
    s_typeTable[0].fields = s_staticObjectFieldsMut;
    if (!s_templateLevelInited) { memcpy(s_templateLevelFieldsMut, s_templateLevelFields, sizeof(s_templateLevelFields)); s_templateLevelInited = true; }
    s_typeTable[1].fields = s_templateLevelFieldsMut;
    if (!s_templateLayerInited) { memcpy(s_templateLayerFieldsMut, s_templateLayerFields, sizeof(s_templateLayerFields)); s_templateLayerInited = true; }
    s_typeTable[2].fields = s_templateLayerFieldsMut;
    if (!s_spawn_pointInited) { memcpy(s_spawn_pointFieldsMut, s_spawn_pointFields, sizeof(s_spawn_pointFields)); s_spawn_pointInited = true; }
    s_typeTable[3].fields = s_spawn_pointFieldsMut;
    if (!s_spawn_nodeInited) { memcpy(s_spawn_nodeFieldsMut, s_spawn_nodeFields, sizeof(s_spawn_nodeFields)); s_spawn_nodeInited = true; }
    s_typeTable[4].fields = s_spawn_nodeFieldsMut;
    if (!s_spawn_classInited) { memcpy(s_spawn_classFieldsMut, s_spawn_classFields, sizeof(s_spawn_classFields)); s_spawn_classInited = true; }
    s_typeTable[5].fields = s_spawn_classFieldsMut;
    if (!s_spawn_emitterInited) { memcpy(s_spawn_emitterFieldsMut, s_spawn_emitterFields, sizeof(s_spawn_emitterFields)); s_spawn_emitterInited = true; }
    s_typeTable[6].fields = s_spawn_emitterFieldsMut;
    if (!s_PlayerRespawnerInited) { memcpy(s_PlayerRespawnerFieldsMut, s_PlayerRespawnerFields, sizeof(s_PlayerRespawnerFields)); s_PlayerRespawnerInited = true; }
    s_typeTable[7].fields = s_PlayerRespawnerFieldsMut;
    if (!s_PointManagerInited) { memcpy(s_PointManagerFieldsMut, s_PointManagerFields, sizeof(s_PointManagerFields)); s_PointManagerInited = true; }
    s_typeTable[8].fields = s_PointManagerFieldsMut;
    if (!s_PathNetworkInited) { memcpy(s_PathNetworkFieldsMut, s_PathNetworkFields, sizeof(s_PathNetworkFields)); s_PathNetworkInited = true; }
    s_typeTable[9].fields = s_PathNetworkFieldsMut;
    if (!s_PathNodeInited) { memcpy(s_PathNodeFieldsMut, s_PathNodeFields, sizeof(s_PathNodeFields)); s_PathNodeInited = true; }
    s_typeTable[10].fields = s_PathNodeFieldsMut;
    if (!s_PathLinkInited) { memcpy(s_PathLinkFieldsMut, s_PathLinkFields, sizeof(s_PathLinkFields)); s_PathLinkInited = true; }
    s_typeTable[11].fields = s_PathLinkFieldsMut;
    if (!s_OutputInited) { memcpy(s_OutputFieldsMut, s_OutputFields, sizeof(s_OutputFields)); s_OutputInited = true; }
    s_typeTable[12].fields = s_OutputFieldsMut;
    if (!s_logic_relayInited) { memcpy(s_logic_relayFieldsMut, s_logic_relayFields, sizeof(s_logic_relayFields)); s_logic_relayInited = true; }
    s_typeTable[13].fields = s_logic_relayFieldsMut;
    if (!s_logic_endgameInited) { memcpy(s_logic_endgameFieldsMut, s_logic_endgameFields, sizeof(s_logic_endgameFields)); s_logic_endgameInited = true; }
    s_typeTable[14].fields = s_logic_endgameFieldsMut;
    if (!s_logic_gamestartInited) { memcpy(s_logic_gamestartFieldsMut, s_logic_gamestartFields, sizeof(s_logic_gamestartFields)); s_logic_gamestartInited = true; }
    s_typeTable[15].fields = s_logic_gamestartFieldsMut;
    if (!s_trigger_radiusInited) { memcpy(s_trigger_radiusFieldsMut, s_trigger_radiusFields, sizeof(s_trigger_radiusFields)); s_trigger_radiusInited = true; }
    s_typeTable[16].fields = s_trigger_radiusFieldsMut;
    if (!s_AtmosphereSettingInited) { memcpy(s_AtmosphereSettingFieldsMut, s_AtmosphereSettingFields, sizeof(s_AtmosphereSettingFields)); s_AtmosphereSettingInited = true; }
    s_typeTable[17].fields = s_AtmosphereSettingFieldsMut;
    if (!s_gamemodeInited) { memcpy(s_gamemodeFieldsMut, s_gamemodeFields, sizeof(s_gamemodeFields)); s_gamemodeInited = true; }
    s_typeTable[18].fields = s_gamemodeFieldsMut;
    s_tableInited = true;
}

// ============================================================
//  Public API
// ============================================================

const EntityTypeDefaults* GetEntityTypeDefaults(uint32_t type_crc) {
    InitTypeTable();
    int n = sizeof(s_typeTable)/sizeof(s_typeTable[0]);
    for (int i = 0; i < n; ++i) {
        if (s_typeTable[i].type_crc == type_crc) return &s_typeTable[i];
    }
    return NULL;
}

uint32_t GetEntityTypeDefaultsCount() {
    return sizeof(s_typeTable)/sizeof(s_typeTable[0]);
}

} // namespace ZeroEngine
