// ================================================================
// MgCapturePoint.h - Capture Point State Machine
// Reconstructed from ConquestLLC.exe disassembly at FUN_008ebff6 (4293 bytes)
//
// The CapturePoint runs a state machine each frame:
//   Neutral -> Contested (players in zone) -> Capturing (progress ticking) -> Captured
//
// Capture rate formula (from 0x008ec047):
//   rate = captureSpeed * numPlayers * difficultyMultiplier * frameDelta * globalScale
//
// When capture completes (progress reaches maximum at 0x008ec4e3):
//   1. Call FUN_008e88bd with team value
//   2. Fire OnCapture output (at [entity+0x61C])
//   3. Fire team-specific output (OnRedCapture at +0x620 or OnBlueCapture at +0x624)
//   4. Optionally fire OnPlayerCapture at +0x634
//   5. Post Wwise sound event via FUN_00855712
//
// Sources:
//   FUN_008ebff6 — CapturePoint update (4293 bytes, full state machine)
//   FUN_008ebcda — InitCapturePointOutputs (registers 10 output slots)
//   FUN_008e88bd — CapturePoint::SetTeam
// ================================================================

#pragma once
#include <stdint.h>
#include "MgEventSystem.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// CapturePoint Memory Layout (from disassembly)
// ------------------------------------------------------------------
#define MG_CP_OFF_CAPTURE_PROGRESS  0x64C  // float: 0.0 to 1.0 capture progress
#define MG_CP_OFF_OWNING_TEAM       0x650  // int: 0=neutral, 1=team1, 2=team2
#define MG_CP_OFF_CAPTURING_TEAM    0x654  // int: team currently capturing
#define MG_CP_OFF_CAPTURE_SPEED     0x658  // float: base capture speed per second
#define MG_CP_OFF_DECAPTURE_SPEED   0x65C  // float: base decapture speed per second

// See MgEventSystem.h for output slot offsets (CP_OUTPUT_*)

// ------------------------------------------------------------------
// CapturePoint Timing Values (from level.json analysis)
// ------------------------------------------------------------------
// CaptureTime: 25.0s (time for bar 0->100% with 1 player)
// DecaptureTime: 35.0s (time to strip enemy ownership)
// RevertDelay: 5.0s (grace period before bar reverts)
// CaptureLevel: 0.5 (bar starts at 50% after neutralization)
// PerCapperModifier: 0.3 (30% speed per extra attacker)
// PerDefenderModifier: 0.3 (30% slow per defender)
// ModifierLimit: 5 (max 5 extra players count)

// ------------------------------------------------------------------
// Function Addresses
// ------------------------------------------------------------------
#define MG_ADDR_CapturePointUpdate      0x008EBFF6  // 4293 bytes
#define MG_ADDR_CapturePointInitOutputs 0x008EBCDA
#define MG_ADDR_CapturePointSetTeam     0x008E88BD
#define MG_ADDR_PostWwiseEvent          0x00855712  // Sound event posting

#ifdef __cplusplus
} // extern "C"
#endif