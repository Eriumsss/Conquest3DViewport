// ================================================================
// MgEntityFactory.h - Entity Creation Pipeline
// Reconstructed from ConquestLLC.exe disassembly at FUN_007e76d4
//
// The 10-step entity creation pipeline:
//   1. Read "Factory" string field from binary data (entity type name)
//   2. CRC the factory name, BST lookup in type registry (0x008a3bba)
//   3. Check type flags: bit1=server-only, bit3=disabled (skip if set)
//   4. Read "WorldTransform" (pbl::Matrix4x4) from binary data
//   5. Resolve parent MgHandle from "ParentGUID" field
//   6. Call virtual Create() via type descriptor vtable[0] -> allocate entity
//   7. Call virtual ReadFields() via vtable+0x64 -> read all field data
//   8. Init components via entity[+0xC4] array (0x0084c41c)
//   9. Read property table from entity[+0x94] (0x00851408)
//  10. Insert into world (dynamic 0x008c6b67 or static 0x008c6c56)
//      Fire OnCreated event via vtable[0x20]
//
// CreateOnLoad Processing (0x007e78b7):
//   Iterates all entity definitions in the level
//   Checks "CreateOnLoad" field CRC at 0x9D84EC — if false, SKIPS (dormant)
//   Checks "QualityCategory" against global quality setting at 0xD017F0
//   If both pass, calls FUN_007e76d4 to instantiate
//
// Sources:
//   FUN_007e76d4 — EntityCreate_FromData (483 bytes, main factory)
//   FUN_007e78b7 — CreateOnLoad processor (228 bytes)
//   FUN_007e7636 — EntityCreate_ByCRC (simpler path, CRC only)
//   FUN_008a3bba — PblTypeRegistry_Lookup (BST by CRC)
//   FUN_0084c41c — Entity_InitComponents (iterate entity[+0xC4] array)
//   FUN_00851408 — Entity_ReadProperties (property table at entity[+0x94])
//   FUN_004012ae — FieldOffset_Resolve (field index -> data offset)
// ================================================================

#pragma once
#include <stdint.h>
#include "MgPblTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// Type Descriptor (12 bytes, from static registration at startup)
//
// ~200+ Mg* types registered via C++ static initializers.
// Each descriptor stores parent pointer, name CRC, and flags.
//
// Flags byte:
//   bit 1: server-only (skip in client mode)
//   bit 2: static entity (use static entity list) vs dynamic
//   bit 3: disabled/deleted (always skip)
//   non-zero value = instantiable (has factory)
// ------------------------------------------------------------------
typedef struct {
    void*    parent;     // +0x00 parent type descriptor (0xE568F4 for root)
    uint32_t nameCRC;    // +0x04 CRC hash of type name
    uint32_t flags;      // +0x08 type flags
} MgTypeDescriptor;

// ------------------------------------------------------------------
// Type Registry BST (Binary Search Tree)
//
// Global root at 0x00A3E89C, count at 0x00A3E8A0
// Each BST node:
//   [+0x00] right child pointer
//   [+0x04] left child pointer
//   [+0x08] reserved
//   [+0x0C] key CRC (uint32)
//   [+0x10] value pointer (MgTypeDescriptor* or factory pointer)
//
// Lookup at 0x008a3bba: walk tree comparing CRC at [node+0xC]
// ------------------------------------------------------------------
#define MG_ADDR_TypeRegistryRoot  0x00A3E89C
#define MG_ADDR_TypeRegistryCount 0x00A3E8A0

// ------------------------------------------------------------------
// Field Definition Entry (12 bytes per field)
//
// Stored in type table within GameObjs binary:
//   [+0x00] field_name_CRC (uint32)
//   [+0x04] field_type_CRC (uint32)
//   [+0x08] field_data_offset (uint32) — byte offset within entity data
//
// Field lookup at 0x007e866b: iterates entries, compares name CRC
// ------------------------------------------------------------------
typedef struct {
    uint32_t nameCRC;    // +0x00 CRC of field name
    uint32_t typeCRC;    // +0x04 CRC of field type
    uint32_t dataOffset; // +0x08 byte offset within entity data blob
} MgFieldDef;

// ------------------------------------------------------------------
// Entity Component Array
//
// At [entity+0xC4], a list of component pointers.
// FUN_0084c41c iterates this array and calls each component's Init():
//   for (i = 0; i < count; i++)
//     component = entity->components[i]
//     success = component->vtable[2]()  // Init()
//     if (!success) return false
// ------------------------------------------------------------------
#define MG_ENTITY_COMPONENTS_OFFSET 0xC4

// ------------------------------------------------------------------
// Entity Property Table
//
// At [entity+0x94], stores per-entity property overrides.
// FUN_00851408 reads this table:
//   [+0x00] count (uint16)
//   [+0x02] header_size (uint16)
//   [+header_size+4] array of property CRCs
//
// Each property resolved via FUN_004012ae (field index -> data offset)
// ------------------------------------------------------------------
#define MG_ENTITY_PROPERTIES_OFFSET 0x94

// ------------------------------------------------------------------
// Identity Matrix Constant (at 0xE56950 in game memory)
// Used as default when entity has no WorldTransform field
// ------------------------------------------------------------------
static const float MG_IDENTITY_MATRIX[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

// ------------------------------------------------------------------
// Key String Addresses (from disassembly)
// These are .rdata string pointers used by the factory
// ------------------------------------------------------------------
#define MG_STR_Factory          0x9D8658  // "Factory"
#define MG_STR_MgTemplateString 0x9D0648  // "MgTemplateString"
#define MG_STR_WorldTransform   0x9D8520  // "WorldTransform"
#define MG_STR_CreateOnLoad     0x9D84EC  // "CreateOnLoad"
#define MG_STR_QualityCategory  0x9D84F8  // "QualityCategory" (approx)

// ------------------------------------------------------------------
// Function Addresses
// ------------------------------------------------------------------
#define MG_ADDR_EntityFactory       0x007E76D4  // EntityCreate_FromData
#define MG_ADDR_CreateOnLoadLoop    0x007E78B7  // Entity spawn loop
#define MG_ADDR_EntityCreateByCRC   0x007E7636  // Simpler factory (CRC only)
#define MG_ADDR_TypeLookup          0x008A3BBA  // BST lookup by CRC
#define MG_ADDR_InitComponents      0x0084C41C  // Init entity components
#define MG_ADDR_ReadProperties      0x00851408  // Read property table
#define MG_ADDR_FieldResolve        0x004012AE  // Field index -> offset
#define MG_ADDR_WorldInsertDynamic  0x008C6B67  // Insert dynamic entity
#define MG_ADDR_WorldInsertStatic   0x008C6C56  // Insert static entity

#ifdef __cplusplus
} // extern "C"
#endif