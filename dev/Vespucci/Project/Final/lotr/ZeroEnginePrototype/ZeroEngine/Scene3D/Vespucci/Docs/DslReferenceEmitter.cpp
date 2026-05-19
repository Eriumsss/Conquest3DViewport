// DslReferenceEmitter.cpp
// =============================================================================
// EMIT REFERENCE DOCS FOR THE COMPAT DSL - GRAMMAR + STDLIB FUNCTIONS
// =============================================================================
// Written by: Eriumsss

#include "../Core/FileIO.h"
#include "../Core/Logging.h"

#include <string>

namespace Vespucci {
namespace Compat { namespace DSL {
    struct StdlibMeta;
    i32 StdlibCatalogCount();
    const StdlibMeta* StdlibCatalogAt(i32 i);
}}
namespace Docs {

void EmitDslReference(const char* outPath) {
    std::string body;
    body += "# Vespucci Compat DSL — Reference\n\n";
    body += "## Grammar\n\n";
    body += "```\n";
    body += "module      ::= top_stmt*\n";
    body += "top_stmt    ::= rule | allow | deny\n";
    body += "rule        ::= 'rule' STRING? '{' stmt* '}'\n";
    body += "stmt        ::= allow | deny | require | weight | reason\n";
    body += "allow       ::= 'allow' wire_spec ('with' (reason | weight))?\n";
    body += "deny        ::= 'deny' wire_spec ('with' (reason | weight))?\n";
    body += "require     ::= 'require' expr\n";
    body += "weight      ::= 'weight' expr\n";
    body += "reason      ::= 'reason' STRING\n";
    body += "wire_spec   ::= type '.' event '->' type '.' action\n";
    body += "type        ::= IDENT | '*'\n";
    body += "event       ::= IDENT | '*'\n";
    body += "action      ::= IDENT | '*'\n";
    body += "expr        ::= or_expr ('||' or_expr)*\n";
    body += "...\n";
    body += "```\n\n";
    body += "## Stdlib functions\n\n";
    body += "| Name | Arity | Description |\n";
    body += "|---|---:|---|\n";
    // The StdlibCatalog isn't strongly typed at this layer; we
    // produce a placeholder list and let the offline tool patch
    // the real signatures into the table.
    body += "| sameLayer       | 0 | True if source/target share layer. |\n";
    body += "| sameParent      | 0 | True if source/target share parent. |\n";
    body += "| distLessThan    | 1 | True if world distance < N. |\n";
    body += "| spatial         | 0 | True if both source and target are spatial. |\n";
    body += "| deprecated      | 0 | True if either side is type-flagged deprecated. |\n";
    body += "| hasEventCount   | 1 | True if source emits at least N events. |\n";
    body += "| hasTrait        | 1 | True if source has the given trait. |\n";
    body += "| nameMatches     | 1-2 | Name regex/wildcard match. |\n";
    body += "| min/max/abs/floor/ceil | 1-2 | Numeric helpers. |\n";

    if (Core::WriteAtomic(outPath, body.data(), (i64)body.size())) {
        Core::Logging::Info("Docs: DSL reference written to %s", outPath);
    }
}

} // namespace Docs
} // namespace Vespucci
