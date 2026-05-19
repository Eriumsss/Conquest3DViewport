// SchemaVersion.cpp
// =============================================================================
// Built-in version + the migration ledger. Bump CurrentSchemaVersion
// whenever Schema/ semantics change. Add a row to kMigrationSteps when
// a minor bump renames a canonical event/action.
// =============================================================================
// Written by: Eriumsss

#include "SchemaVersion.h"

#include <cstring>

namespace Vespucci {
namespace Schema {

// 1.0.0 = first shipped Vespucci Smart Wiring schema. Bump on changes.
static const SchemaVersionTriple kCurrent = { 1, 0, 0 };

// No migrations yet at 1.0.0. Add rows as we evolve.
static const MigrationStep kMigrationSteps[] = {
    // example, intentionally inert at 1.0.0:
    // { {1,0,0}, {1,1,0}, "ontriggered", "ontrigger", "Renamed verbose 'OnTriggered' to canonical 'OnTrigger'." },
    { {0,0,0}, {0,0,0}, "", "", "" }     // sentinel, stop here
};
static const i32 kMigrationStepsLive = 0;  // count of REAL steps above the sentinel

SchemaVersionTriple CurrentSchemaVersion() { return kCurrent; }

u32 PackSchemaVersion(SchemaVersionTriple v) {
    // 2 bytes major, 2 bytes minor, with patch packed into the high
    // byte of an unused word. We use a dense encoding so a casual
    // hex-edit of corpus headers reads naturally: 0x00010002 = 1.0.2.
    return ((u32)v.major << 16) | (((u32)v.patch & 0xFFu) << 8) | ((u32)v.minor & 0xFFu);
}

SchemaVersionTriple UnpackSchemaVersion(u32 packed) {
    SchemaVersionTriple v;
    v.major = (u16)((packed >> 16) & 0xFFFFu);
    v.patch = (u16)((packed >> 8)  & 0xFFu);
    v.minor = (u16)(packed         & 0xFFu);
    return v;
}

bool IsLoadable(SchemaVersionTriple recorded, SchemaVersionTriple current) {
    // Reject pre-1.0 unstamped artifacts unless caller explicitly
    // overrides with acceptUnstamped (not in this signature; callers
    // who need that override the check at their layer).
    if (recorded.major == 0 && recorded.minor == 0 && recorded.patch == 0) return false;
    if (recorded.major != current.major) return false;
    return true;
}

const MigrationStep* MigrationStepsTable(i32* outCount) {
    if (outCount) *outCount = kMigrationStepsLive;
    return kMigrationSteps;
}

const char* MigrateCanonicalName(const char* name,
                                  SchemaVersionTriple from,
                                  SchemaVersionTriple to)
{
    if (!name) return name;
    if (kMigrationStepsLive == 0) return name;
    // Walk the steps in order and apply any that match the from/to
    // span. Names are lowercase canonical, so plain strcmp is fine.
    const char* current = name;
    for (i32 i = 0; i < kMigrationStepsLive; ++i) {
        const MigrationStep& s = kMigrationSteps[i];
        // Step applies if (s.from <= from AND s.to <= to). We use
        // packed comparison for simplicity.
        u32 sf = PackSchemaVersion(s.from);
        u32 st = PackSchemaVersion(s.to);
        u32 ff = PackSchemaVersion(from);
        u32 tt = PackSchemaVersion(to);
        if (sf >= ff && st <= tt) {
            if (std::strcmp(current, s.oldCanonical) == 0) {
                current = s.newCanonical;
            }
        }
    }
    return current;
}

} // namespace Schema
} // namespace Vespucci
