// ================================================================
// MgPointManager.h - Scoring and Victory System
// Reconstructed from ConquestLLC.exe disassembly at FUN_007f0cc3
//
// PointManager tracks score for a team and fires milestone events:
//   OnNotify1/2/3: custom score thresholds
//   OnVictory: when VictoryPoints reached
//   OnPointChange: every time score changes
//   On50/10/5/1PointsToGo: countdown milestones
//   On75/50/25Percent: percentage milestones
//
// Victory score at [entity+0x194] (default 9999/0x270F if not set)
//
// Scoring sources (from level.json analysis):
//   Conquest: trickle 3pts/sec/CP + kills 5pts each
//   TDM: kills 1pt each, suicides -1pt
//   CTF: ring captures 1pt each
//
// Victory chain:
//   PointManager.OnVictory -> logic_endgame.Team1Won (5s delay)
//   PointManager.OnVictory -> HUDMovie Victory/Defeat
//   PointManager.OnVictory -> VictoryMode relay -> MetaReward
//     Winner buff: DamageAdj=32.0 SpeedAdj=1.1
//     Loser debuff: DamageAdj=0.01 SpeedAdj=0.9
//
// Sources:
//   FUN_007f0cc3 — InitPointManagerOutputs
//   See MgEventSystem.h for output slot offset defines (PM_OUTPUT_*)
// ================================================================

#pragma once
#include <stdint.h>
#include "MgEventSystem.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// PointManager Memory Layout
// ------------------------------------------------------------------
#define MG_PM_OFF_CURRENT_SCORE   0x190  // int: current score (estimated)
#define MG_PM_OFF_VICTORY_SCORE   0x194  // int: points needed to win (default 9999)
#define MG_PM_OFF_TEAM            0x198  // int: which team (1 or 2)
#define MG_PM_OFF_ACTIVE          0x19C  // bool: is scoring active

// See MgEventSystem.h for PM_OUTPUT_* defines

// ------------------------------------------------------------------
// Scoring Constants (from level.json cross-map analysis)
// ------------------------------------------------------------------

// Conquest mode
#define MG_SCORE_CONQUEST_VP            500     // VictoryPoints
#define MG_SCORE_CONQUEST_KILL_VALUE    5       // points per kill
#define MG_SCORE_CONQUEST_TRICKLE_RATE  3       // points per second per held CP
#define MG_SCORE_CONQUEST_CQ_TRICKLE   10      // gamemode CQ_Team1/2_TrickleRate

// TDM mode
#define MG_SCORE_TDM_VP                50      // VictoryPoints
#define MG_SCORE_TDM_KILL_VALUE        1       // points per kill
#define MG_SCORE_TDM_SUICIDE_PENALTY   (-1)    // points lost on suicide

// CTF mode
#define MG_SCORE_CTF_VP                5       // VictoryPoints
#define MG_SCORE_CTF_CAPTURE_VALUE     1       // points per ring capture

// Victory Mode buffs (from MetaReward analysis)
#define MG_VICTORY_WINNER_DAMAGE_ADJ   32.0f   // 32x damage
#define MG_VICTORY_WINNER_SPEED_ADJ    1.1f    // +10% speed
#define MG_VICTORY_LOSER_DAMAGE_ADJ    0.01f   // 1% damage
#define MG_VICTORY_LOSER_SPEED_ADJ     0.9f    // -10% speed

// ------------------------------------------------------------------
// Function Addresses
// ------------------------------------------------------------------
#define MG_ADDR_PointManagerInitOutputs  0x007F0CC3
#define MG_ADDR_PostDeathToScore         0x007137CB

#ifdef __cplusplus
} // extern "C"
#endif