// EntityFieldDefs.h — Every Fucking Field Type in Pandemic's Entity System
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Default field definitions for EVERY entity type in LOTR: Conquest.
// static_object, spawn_point, path_node, capture_area, cinematic_camera,
// light_sun, trigger_volume, atmospheric_setting — each one has a
// specific set of typed fields (Float, Int, CRC, GUID, Matrix4x4,
// ObjectList, Color, String) with specific byte offsets in the binary.
//
// This entire type system was reverse-engineered from the GameObjs
// sub-block inside Block1 of the PAK files. Pandemic built a runtime
// type information system from scratch — their own little reflection
// engine buried inside a game engine. Type definitions, field layouts,
// default values, all packed in binary. No XML. No JSON. No mercy.
//
// Used by AddPendingEntity() to produce binary data that matches
// EXACTLY what the Rust lotrc parser expects. One byte wrong and the
// entity either doesn't appear in the level or appears at coordinates
// that would make a cartographer weep.
//
// "An unexamined life is not worth living." — Socrates
// An unexamined binary format is not worth parsing. I examined this
// one until my eyes fucking bled. Every field. Every offset. Every
// default value. Socrates would be proud. And also confused.
// -----------------------------------------------------------------------
#pragma once

#ifndef ENTITY_FIELD_DEFS_H
#define ENTITY_FIELD_DEFS_H

#include <stddef.h>

#ifdef _MSC_VER
typedef unsigned __int32 uint32_t;
typedef signed   __int32 int32_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int8  uint8_t;
#else
#include <stdint.h>
#endif

namespace ZeroEngine {

// ============================================================
//  Field kind CRCs (match Rust BaseTypes keys in types.rs)
// ============================================================
#define KIND_CRC_VAL           0x776C796Eu
#define KIND_GUID_VAL          0xCF98052Fu
#define KIND_COLOR_VAL         0xC908780Fu
#define KIND_VECTOR2_VAL       0x88FD83F0u
#define KIND_VECTOR3_VAL       0x8C3C9E47u
#define KIND_VECTOR4_VAL       0x927BCE42u
#define KIND_MATRIX4X4_VAL     0xB968BDDBu
#define KIND_FLOAT_VAL         0xB7F76AA0u
#define KIND_INT_VAL           0xF77336C5u
#define KIND_BOOL_VAL          0xBC2074C5u
#define KIND_BYTE_VAL          0x6F42203Au
#define KIND_STRING_VAL        0xCEBC116Au
#define KIND_STRINGLIST_VAL    0xF82B7A05u
#define KIND_OBJECTLIST_VAL    0x3350CA1Fu
#define KIND_NODELIST_VAL      0x0D0A0AC1u
#define KIND_INTLIST_VAL       0x8FEEF1E8u
#define KIND_CRCLIST_VAL       0x19E9EB23u
#define KIND_WEIGHTLIST_VAL    0x5A26F15Bu
#define KIND_MATRIXLIST_VAL    0x003BDE3Cu

// ============================================================
//  Field kind enum (for fast switching in C++)
// ============================================================
enum FieldKind {
    FK_CRC = 0,
    FK_GUID,
    FK_COLOR,
    FK_INT,
    FK_FLOAT,
    FK_BOOL,
    FK_BYTE,
    FK_VECTOR2,
    FK_VECTOR3,
    FK_VECTOR4,
    FK_MATRIX4X4,
    FK_STRING,
    FK_OBJECTLIST,
    FK_NODELIST,
    FK_INTLIST,
    FK_CRCLIST,
    FK_STRINGLIST,
    FK_WEIGHTLIST,
    FK_MATRIXLIST
};

// Returns the fixed-region byte size at the field's offset.
// For List/String types this is 4 (the List header: num u16 + offset u16).
// For scalar types this is the full value size.
uint32_t FieldKindFixedSize(FieldKind kind);

// Returns true if the kind is a List/String type (has tail data appended past fixed region).
bool FieldKindIsList(FieldKind kind);

// Maps a kind CRC (from PAK type defs) to our FieldKind enum. Returns FK_INT on unknown.
FieldKind FieldKindFromCrc(uint32_t kind_crc);

// ============================================================
//  Default value for a single field in a type definition
// ============================================================
struct EntityFieldDefault {
    uint32_t name_crc;    // CRC of field name (e.g. "GUID", "Mesh")
    uint32_t kind_crc;    // CRC of field kind (e.g. "Float", "Bool")
    FieldKind kind;       // enum for fast switching
    uint32_t offset;      // byte offset within object data
    union DefaultVal {
        uint32_t u32_val;
        int32_t  i32_val;
        float    f32_val;
        uint8_t  u8_val;
    } def;                // default scalar value (lists: ignored, always empty)
};

// ============================================================
//  Complete type definition with all field defaults
// ============================================================
struct EntityTypeDefaults {
    uint32_t type_crc;
    const EntityFieldDefault* fields;
    uint32_t field_count;
};

// Look up defaults for a type CRC. Returns NULL if type is not in our table.
const EntityTypeDefaults* GetEntityTypeDefaults(uint32_t type_crc);

// How many types are registered in the defaults table.
uint32_t GetEntityTypeDefaultsCount();

} // namespace ZeroEngine

#endif // ENTITY_FIELD_DEFS_H
