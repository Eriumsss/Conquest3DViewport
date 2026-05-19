// ================================================================
// LevelValidator.h - Level Integrity Validation
//
// Validates a loaded level against the rules derived from
// exhaustive level.json analysis of all 16 maps.
//
// Checks:
//   1. Mandatory entities exist per gamemode
//   2. Hardcoded GUID references resolve
//   3. Wiring rules (spawn chain, capture areas, path links)
//   4. Field requirements per type
//
// Written by: Eriumsss (2026-04-09)
// ================================================================

#pragma once

#include <vector>
#include <string>
#include <stdint.h>

namespace ZeroEngine { class LevelReader; }

// Severity levels
enum ValidationSeverity {
    VSEV_ERROR   = 0,  // Will crash the game
    VSEV_WARNING = 1,  // May cause issues
    VSEV_INFO    = 2   // Informational
};

struct ValidationIssue {
    ValidationSeverity severity;
    uint32_t           entityGuid;    // 0 if global issue
    std::string        entityName;
    std::string        entityType;
    std::string        message;
};

// Run all validation checks on a loaded level.
// Returns number of errors (severity == VSEV_ERROR).
int ValidateLevel(
    const ZeroEngine::LevelReader& reader,
    std::vector<ValidationIssue>& outIssues
);

// Individual check functions (called by ValidateLevel)
void ValidateMandatoryEntities(const ZeroEngine::LevelReader& reader, std::vector<ValidationIssue>& out);
void ValidateGUIDReferences(const ZeroEngine::LevelReader& reader, std::vector<ValidationIssue>& out);
void ValidateWiringRules(const ZeroEngine::LevelReader& reader, std::vector<ValidationIssue>& out);
void ValidateFieldRequirements(const ZeroEngine::LevelReader& reader, std::vector<ValidationIssue>& out);
