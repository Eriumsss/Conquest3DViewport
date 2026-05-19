// RuleSet.cpp
// =============================================================================
// Vector-of-IRProgram. Keep it simple. The hot path is the matrix
// evaluator iterating these in priority order; nothing else needs
// to be clever.
// =============================================================================
// Written by: Eriumsss

#include "RuleSet.h"

namespace Vespucci {
namespace Compat {

RuleSet::RuleSet() {}

i32 RuleSet::addProgram(const DSL::IRProgram& prog, const std::string& sourceFile) {
    Entry e;
    e.prog       = prog;
    e.sourceFile = sourceFile;
    m_entries.push_back(e);
    return (i32)m_entries.size() - 1;
}

i32 RuleSet::size() const { return (i32)m_entries.size(); }

const DSL::IRProgram* RuleSet::programAt(i32 i) const {
    if (i < 0 || i >= (i32)m_entries.size()) return 0;
    return &m_entries[(size_t)i].prog;
}

const char* RuleSet::sourceFileAt(i32 i) const {
    if (i < 0 || i >= (i32)m_entries.size()) return "";
    return m_entries[(size_t)i].sourceFile.c_str();
}

void RuleSet::clear() { m_entries.clear(); }

RuleSet::Stats RuleSet::stats() const {
    Stats s; s.totalRules = (i32)m_entries.size(); s.distinctSources = 0; s.totalOpcodes = 0;
    std::vector<std::string> seen;
    seen.reserve(m_entries.size());
    for (size_t i = 0; i < m_entries.size(); ++i) {
        s.totalOpcodes += (i32)m_entries[i].prog.ops.size();
        bool found = false;
        for (size_t j = 0; j < seen.size(); ++j) {
            if (seen[j] == m_entries[i].sourceFile) { found = true; break; }
        }
        if (!found) { seen.push_back(m_entries[i].sourceFile); s.distinctSources++; }
    }
    return s;
}

} // namespace Compat
} // namespace Vespucci
