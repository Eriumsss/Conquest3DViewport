// ================================================================
// MgEntityFactory.cpp - Entity Creation Pipeline
// Reconstructed from ConquestLLC.exe disassembly
// See MgEntityFactory.h for full documentation
// ================================================================

#include "MgEntityFactory.h"
#include "MgCRC.h"
#include "MgEventSystem.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// PblTypeRegistry_Lookup (0x008a3bba)
//
// Binary search tree walk: compare CRC at [node+0xC].
// Returns type descriptor pointer, or NULL if not found.
//
// BST node layout:
//   [+0x00] right child
//   [+0x04] left child
//   [+0x08] reserved
//   [+0x0C] key CRC (uint32)
//   [+0x10] value pointer (MgTypeDescriptor*)
// ------------------------------------------------------------------
MgTypeDescriptor* MgEntity_LookupType(uint32_t nameCRC) {
    // Stub — requires live BST root at 0x00A3E89C.
    // In viewer mode: return NULL (no live type registry).
    // In game-attached mode: walk BST from g_TypeRegistryRoot.
    (void)nameCRC;
    return NULL;
}

// ------------------------------------------------------------------
// EntityCreate_FromData (0x007e76d4, 483 bytes)
//
// 10-step entity creation pipeline:
//   1. Read "Factory" string field -> CRC hash it
//   2. BST lookup in type registry (0x008a3bba)
//   3. Check type flags: bit1=server-only, bit3=disabled
//   4. Read "WorldTransform" (pbl::Matrix4x4)
//   5. Resolve parent from "ParentGUID" field
//   6. Call Create() via type descriptor vtable[0]
//   7. Call ReadFields() via vtable+0x64
//   8. Init components via entity[+0xC4] array (0x0084c41c)
//   9. Read property table from entity[+0x94] (0x00851408)
//  10. Insert into world + fire OnCreated event
// ------------------------------------------------------------------
void* MgEntity_CreateFromData(
    const uint8_t* entityData,
    uint32_t dataSize,
    const MgFieldDef* fields,
    uint32_t fieldCount
) {
    if (!entityData || !fields || fieldCount == 0) return NULL;

    // Step 1: Find "Factory" field (CRC of "Factory" = hash of string at 0x9D8658)
    static uint32_t s_factoryCRC = 0;
    if (s_factoryCRC == 0) {
        s_factoryCRC = MgCRC_HashString_Runtime("Factory");
    }

    const MgFieldDef* factoryField = NULL;
    for (uint32_t i = 0; i < fieldCount; i++) {
        if (fields[i].nameCRC == s_factoryCRC) {
            factoryField = &fields[i];
            break;
        }
    }
    if (!factoryField) return NULL;  // No factory field = can't create

    // Step 2: Read factory string, hash it, BST lookup
    // In the real game: reads MgTemplateString from entity data at factoryField->dataOffset,
    // CRC hashes it, then calls 0x008a3bba to find the type descriptor.
    // Stub: we can't instantiate without the live type registry.

    // Step 3: Check flags (bit1=server-only, bit3=disabled)
    // MgTypeDescriptor* td = MgEntity_LookupType(factoryCRC);
    // if (!td) return NULL;
    // if (td->flags & 0x02) return NULL;  // server-only in client mode
    // if (td->flags & 0x08) return NULL;  // disabled

    // Steps 4-10: require vtable infrastructure
    // This is a documentation stub for the viewer.
    (void)dataSize;
    return NULL;
}

// ------------------------------------------------------------------
// CreateOnLoad Processor (0x007e78b7, 228 bytes)
//
// Iterates all entity definitions in the loaded level:
//   for each entity:
//     if entity has "CreateOnLoad" field:
//       read it; if false -> skip (entity stays dormant until event fires)
//     if entity has "QualityCategory" field:
//       read it; compare to global quality at [0xD017F0]
//       if category > globalQuality -> skip (LOD culled)
//     call EntityCreate_FromData for this entity
//
// CRC for "CreateOnLoad" is pre-computed from string at 0x9D84EC.
// ------------------------------------------------------------------
int MgEntity_ProcessCreateOnLoad(
    const uint8_t* gameObjsData,
    uint32_t numEntities,
    uint32_t entityOffset
) {
    // Stub — requires live entity table and type registry.
    // In the viewer, entities are parsed by LevelReader::ParseGameObjs()
    // and displayed in the scene graph without runtime instantiation.
    (void)gameObjsData;
    (void)numEntities;
    (void)entityOffset;
    return 0;
}

// ------------------------------------------------------------------
// Field Lookup (0x007e866b)
//
// Iterates field definition array, compares nameCRC.
// Returns pointer to field data within entity data blob.
// ------------------------------------------------------------------
const uint8_t* MgEntity_FindFieldData(
    const uint8_t* entityData,
    const MgFieldDef* fields,
    uint32_t fieldCount,
    uint32_t targetNameCRC
) {
    if (!entityData || !fields) return NULL;

    for (uint32_t i = 0; i < fieldCount; i++) {
        if (fields[i].nameCRC == targetNameCRC) {
            return entityData + fields[i].dataOffset;
        }
    }
    return NULL;
}

#ifdef __cplusplus
} // extern "C"
#endif
