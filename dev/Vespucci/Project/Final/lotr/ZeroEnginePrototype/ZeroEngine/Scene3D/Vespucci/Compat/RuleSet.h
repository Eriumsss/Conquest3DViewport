// RuleSet.h
// =============================================================================
// RULESET — COLLECTION OF LOWERED COMPAT RULES INDEXED FOR FAST LOOKUP
// =============================================================================
// Written by: Eriumsss
//
// Designers author .compat files, the DSL parser+sema+codegen pipeline
// turns each into an IRProgram, and we drop them into a RuleSet. The
// RuleSet keeps two indexes:
//   - flat list, in source order (priority follows definition order)
//   - reverse map by (sourceTypePattern, eventPattern) so the matrix
//     evaluator can prune the candidate rules before VM execution
//
// Loading is incremental: AddProgram(p) appends + reindexes one rule.
// Resetting drops everything (used when a designer reloads the
// `.compat` directory).
// =============================================================================

#ifndef VESPUCCI_COMPAT_RULESET_H_
#define VESPUCCI_COMPAT_RULESET_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "DSL/Ir.h"

#include <string>
#include <vector>

namespace Vespucci {
namespace Compat {

class RuleSet {
public:
    RuleSet();

    // Append one lowered program. Returns the rule index (0-based).
    i32 addProgram(const DSL::IRProgram& prog, const std::string& sourceFile);

    // Number of rules.
    i32 size() const;

    // Access raw IRPrograms for the matrix evaluator.
    const DSL::IRProgram* programAt(i32 i) const;

    // Source file for the i'th rule (used by debug overlay).
    const char* sourceFileAt(i32 i) const;

    // Drop everything.
    void clear();

    // Stats.
    struct Stats {
        i32 totalRules;
        i32 distinctSources;
        i32 totalOpcodes;
    };
    Stats stats() const;

private:
    struct Entry {
        DSL::IRProgram prog;
        std::string    sourceFile;
    };
    std::vector<Entry> m_entries;
};

} // namespace Compat
} // namespace Vespucci

#endif // VESPUCCI_COMPAT_RULESET_H_
