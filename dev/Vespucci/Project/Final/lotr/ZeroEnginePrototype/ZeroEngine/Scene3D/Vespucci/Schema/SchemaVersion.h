// SchemaVersion.h
// =============================================================================
// SCHEMA VERSIONING — TRACK BREAKING CHANGES TO THE SIGNATURE DB AND COMPAT
// =============================================================================
// Written by: Eriumsss
//
// When we change a built-in trait, rename a canonical event, or shuffle
// the TypeId hash salt (we will not, but we said the same thing about
// the gizmo namespace and look how that went), every persisted artifact
// downstream of Schema breaks: the corpus shards, the golden tests, the
// weights JSON. SchemaVersion is the version stamp every persisted
// artifact carries so we can detect the mismatch and either auto-migrate
// or refuse to load with a clear error.
//
// Bump policy:
//   - PATCH: new types added, no rename. Old corpus loads fine.
//   - MINOR: rename canonical event/action. Old corpus loads with a
//            translation map (recorded in SchemaVersion.cpp).
//   - MAJOR: TypeId hash salt change, parent-chain rewrite, anything
//            that breaks bucket-locality. Old artifacts must be
//            rebuilt; the loader logs an Error and continues with
//            cold-start defaults.
//
// Versions are recorded as MAJOR.MINOR.PATCH integers. Zero is the
// pre-1.0.0 sentinel; on-disk artifacts that lack a version are
// treated as 0.0.0 and rejected unless `acceptUnstamped` is set.
// =============================================================================

#ifndef VESPUCCI_SCHEMA_SCHEMAVERSION_H_
#define VESPUCCI_SCHEMA_SCHEMAVERSION_H_

#include "../Core/VespucciTypes.h"

namespace Vespucci {
namespace Schema {

struct SchemaVersionTriple {
    u16 major;
    u16 minor;
    u16 patch;
};

// Current built-in version. Bump when registry / signature DB change.
SchemaVersionTriple CurrentSchemaVersion();

// Pack/unpack to a 32-bit field for easy serialization in corpus
// shards and replay headers.
u32 PackSchemaVersion(SchemaVersionTriple v);
SchemaVersionTriple UnpackSchemaVersion(u32 packed);

// Compatibility: return TRUE if `recorded` is loadable by `current`.
// Same major + recorded.minor <= current.minor: yes.
// Same major + recorded.minor > current.minor: yes (forward-compat,
// new fields ignored).
// Major mismatch: no.
bool IsLoadable(SchemaVersionTriple recorded, SchemaVersionTriple current);

// Migration step recorder. Each minor bump that requires a rename
// adds one entry below; the loader walks this table to translate
// old canonical names into current ones.
struct MigrationStep {
    SchemaVersionTriple from;
    SchemaVersionTriple to;
    const char*         oldCanonical;
    const char*         newCanonical;
    const char*         note;
};

const MigrationStep* MigrationStepsTable(i32* outCount);

// Apply migrations to a canonical name string. Returns the migrated
// name or the original if no migration applies.
const char* MigrateCanonicalName(const char* name,
                                  SchemaVersionTriple from,
                                  SchemaVersionTriple to);

} // namespace Schema
} // namespace Vespucci

#endif // VESPUCCI_SCHEMA_SCHEMAVERSION_H_
