// StdLib.cpp
// =============================================================================
// COMPAT-DSL STDLIB — RUNTIME HELPERS THE VM CAN CALL FROM RULE CODE
// =============================================================================
// Written by: Eriumsss
//
// Most of the stdlib is implemented inline in Vm.cpp's runStdlibCall.
// This file is the documentation + extension surface: when we add a
// new stdlib helper, we register the metadata here so the CompatEditor
// UI knows the name, doc, and arity to show in autocomplete. The
// actual runtime hook lives in Vm.cpp; this is the catalog.
// =============================================================================

#include "Ir.h"

#include "../../Core/Logging.h"
#include "../../Core/StringRef.h"

namespace Vespucci {
namespace Compat {
namespace DSL {

struct StdlibMeta {
    StdlibId   id;
    const char* name;
    i32         minArity;
    i32         maxArity;
    const char* doc;
};

static const StdlibMeta kStdlibMeta[] = {
    { SL_SAME_LAYER,
      "sameLayer", 0, 0,
      "True if source and target entities share the same layer GUID." },
    { SL_SAME_PARENT,
      "sameParent", 0, 0,
      "True if source and target share the same parent entity." },
    { SL_DIST_LESS_THAN,
      "distLessThan", 1, 1,
      "True if world-space distance between source and target is below the given threshold." },
    { SL_SPATIAL,
      "spatial", 0, 0,
      "True if both source and target are spatial (have a WorldTransform)." },
    { SL_DEPRECATED,
      "deprecated", 0, 0,
      "True if either source or target type is flagged deprecated." },
    { SL_HAS_EVENT_COUNT,
      "hasEventCount", 1, 1,
      "True if the source emits at least N distinct events. (Runtime hook deferred to Phase D.)" },
    { SL_HAS_TRAIT,
      "hasTrait", 1, 1,
      "True if the source has the given trait flag (e.g. \"Spatial\", \"Animated\")." },
    { SL_NAME_MATCHES,
      "nameMatches", 1, 2,
      "True if the entity's display name matches the pattern. Optional second arg \"source\" / \"target\" picks side." },
    { SL_MIN,            "min",   2, 2, "Numeric min of two args." },
    { SL_MAX,            "max",   2, 2, "Numeric max of two args." },
    { SL_ABS,            "abs",   1, 1, "Numeric absolute value." },
    { SL_FLOOR,          "floor", 1, 1, "Floor of a number." },
    { SL_CEIL,           "ceil",  1, 1, "Ceiling of a number." }
};

// Public: enumerate stdlib functions for the CompatEditor autocomplete.
i32 StdlibCatalogCount() {
    return (i32)(sizeof(kStdlibMeta) / sizeof(kStdlibMeta[0]));
}

const StdlibMeta* StdlibCatalogAt(i32 i) {
    if (i < 0 || i >= StdlibCatalogCount()) return 0;
    return &kStdlibMeta[i];
}

void DumpStdlibCatalogToLog() {
    Core::Logging::Info("Compat DSL stdlib catalog:");
    for (i32 i = 0; i < StdlibCatalogCount(); ++i) {
        const StdlibMeta* m = StdlibCatalogAt(i);
        Core::Logging::Info("  %s [%d-%d args] — %s", m->name,
            (int)m->minArity, (int)m->maxArity, m->doc);
    }
}

} // namespace DSL
} // namespace Compat
} // namespace Vespucci
