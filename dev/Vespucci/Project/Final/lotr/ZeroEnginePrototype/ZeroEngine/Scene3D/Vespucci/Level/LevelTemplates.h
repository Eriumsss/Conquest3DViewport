// ================================================================
// LevelTemplates.h - Pre-built Level Entity Templates
//
// Creates the minimum set of entities for a playable level.
// Based on exhaustive analysis of all 16 level.json files:
//   - 22 minimum entities for bare playable Conquest
//   - Correct GUID wiring between all entities
//   - Hardcoded GUID references for shared template layers
//   - Mode-specific scoring/timing constants
//
// Written by: Eriumsss (2026-04-09)
// ================================================================

#pragma once

namespace ZeroEngine { class LevelReader; }

// Create a minimum playable Conquest level (~30 entities).
// Places entities around (centerX, centerZ) with reasonable spacing.
// Returns number of entities created.
int CreateConquestTemplate(ZeroEngine::LevelReader& reader, float centerX, float centerZ);

// Create a minimum playable TDM level (~20 entities).
int CreateTDMTemplate(ZeroEngine::LevelReader& reader, float centerX, float centerZ);

// Create a minimum playable CTR level (~25 entities).
int CreateCTRTemplate(ZeroEngine::LevelReader& reader, float centerX, float centerZ);
