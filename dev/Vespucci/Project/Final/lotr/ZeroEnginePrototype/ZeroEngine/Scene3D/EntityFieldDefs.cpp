// EntityFieldDefs.cpp — A Fucking Encyclopedia of Every Entity Field Pandemic Ever Made
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Field offsets and default values ripped from lotrcparser output and
// cross-referenced with the .exe's type registry at 0x00A2B800. Every
// goddamn field — its name CRC, its type CRC, its byte offset, its
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
//  Type table
// ============================================================

static const uint32_t TYPE_STATIC_OBJECT = 0x408E062Eu;

static EntityTypeDefaults s_typeTable[] = {
    { 0x408E062Eu, NULL, sizeof(s_staticObjectFields)/sizeof(s_staticObjectFields[0]) },
    // Add more types here as needed:
    // { TYPE_SCALED_OBJECT, ..., ... },
    // { TYPE_SPAWN_POINT, ..., ... },
};

static bool s_tableInited = false;

static void InitTypeTable() {
    if (s_tableInited) return;
    InitStaticObjectDefaults();
    s_typeTable[0].fields = s_staticObjectFieldsMut;
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
