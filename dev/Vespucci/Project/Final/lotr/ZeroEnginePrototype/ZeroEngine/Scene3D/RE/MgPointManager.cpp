// ================================================================
// MgPointManager.cpp - Scoring and Victory System
// Reconstructed from ConquestLLC.exe disassembly at FUN_007f0cc3
// See MgPointManager.h for full documentation
// ================================================================

#include "MgPointManager.h"
#include "MgEventSystem.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// PointManager_AddScore
//
// Core scoring function. Adds points to a team's score and fires
// milestone events as thresholds are crossed.
//
// The real game stores score at [entity+0x190], victory at [entity+0x194].
// Default victory score is 9999 (0x270F) if not set by the gamemode.
//
// Milestone events fired (from FUN_007f0cc3 output slot registration):
//   OnPointChange: every time score changes
//   On50PointsToGo: when remaining <= 50
//   On10PointsToGo: when remaining <= 10
//   On5PointsToGo: when remaining <= 5
//   On1PointToGo: when remaining <= 1
//   On75Percent: when score >= 75% of victory
//   On50Percent: when score >= 50% of victory
//   On25Percent: when score >= 25% of victory
//   OnVictory: when score >= victory points
//
// Victory chain:
//   OnVictory -> logic_endgame.Team1Won (5s delay)
//   OnVictory -> HUDMovie Victory/Defeat display
//   OnVictory -> VictoryMode relay -> MetaReward
//     Winner: DamageAdj=32.0, SpeedAdj=1.1
//     Loser: DamageAdj=0.01, SpeedAdj=0.9
// ------------------------------------------------------------------

typedef struct {
    int currentScore;    // at +0x190
    int victoryScore;    // at +0x194
    int team;            // at +0x198
    int active;          // at +0x19C
    // Milestone tracking (to fire each event only once)
    int fired50ToGo;
    int fired10ToGo;
    int fired5ToGo;
    int fired1ToGo;
    int fired75Pct;
    int fired50Pct;
    int fired25Pct;
    int firedVictory;
} MgPointManagerState;

void MgPointManager_AddScore(MgPointManagerState* pm, int points) {
    if (!pm || !pm->active) return;

    int oldScore = pm->currentScore;
    pm->currentScore += points;

    // Clamp to 0 (no negative scores)
    if (pm->currentScore < 0) pm->currentScore = 0;

    // Don't exceed victory (score is capped at victory points)
    if (pm->currentScore > pm->victoryScore) {
        pm->currentScore = pm->victoryScore;
    }

    if (pm->currentScore == oldScore) return;

    // Fire OnPointChange (every change)
    // MgEvent_FireOutput(pm->onPointChangeSlot);

    int remaining = pm->victoryScore - pm->currentScore;

    // Countdown milestones (fire once when threshold crossed)
    if (remaining <= 50 && !pm->fired50ToGo) {
        pm->fired50ToGo = 1;
        // MgEvent_FireOutput(pm->on50PointsToGoSlot);
    }
    if (remaining <= 10 && !pm->fired10ToGo) {
        pm->fired10ToGo = 1;
        // MgEvent_FireOutput(pm->on10PointsToGoSlot);
    }
    if (remaining <= 5 && !pm->fired5ToGo) {
        pm->fired5ToGo = 1;
        // MgEvent_FireOutput(pm->on5PointsToGoSlot);
    }
    if (remaining <= 1 && !pm->fired1ToGo) {
        pm->fired1ToGo = 1;
        // MgEvent_FireOutput(pm->on1PointToGoSlot);
    }

    // Percentage milestones
    int pct25 = pm->victoryScore / 4;
    int pct50 = pm->victoryScore / 2;
    int pct75 = (pm->victoryScore * 3) / 4;

    if (pm->currentScore >= pct25 && !pm->fired25Pct) {
        pm->fired25Pct = 1;
        // MgEvent_FireOutput(pm->on25PercentSlot);
    }
    if (pm->currentScore >= pct50 && !pm->fired50Pct) {
        pm->fired50Pct = 1;
        // MgEvent_FireOutput(pm->on50PercentSlot);
    }
    if (pm->currentScore >= pct75 && !pm->fired75Pct) {
        pm->fired75Pct = 1;
        // MgEvent_FireOutput(pm->on75PercentSlot);
    }

    // Victory!
    if (pm->currentScore >= pm->victoryScore && !pm->firedVictory) {
        pm->firedVictory = 1;
        // MgEvent_FireOutput(pm->onVictorySlot);
    }
}

// ------------------------------------------------------------------
// PostDeathToScore (FUN_007137cb)
//
// Called from Creature::OnDeath to update team scores:
//   Conquest mode: killing team gets MG_SCORE_CONQUEST_KILL_VALUE (5)
//   TDM mode: killing team gets MG_SCORE_TDM_KILL_VALUE (1)
//                suicide: team gets MG_SCORE_TDM_SUICIDE_PENALTY (-1)
//   CTF mode: no score from kills (score from captures only)
// ------------------------------------------------------------------
void MgPointManager_PostDeath(MgPointManagerState* killerTeamPM, MgPointManagerState* victimTeamPM, int isSuicide, int gameMode) {
    if (!killerTeamPM) return;

    // gameMode: 0=Conquest, 1=TDM, 2=CTF (approximate mapping)
    switch (gameMode) {
        case 0:  // Conquest
            if (!isSuicide) {
                MgPointManager_AddScore(killerTeamPM, MG_SCORE_CONQUEST_KILL_VALUE);
            }
            break;
        case 1:  // TDM
            if (isSuicide && victimTeamPM) {
                MgPointManager_AddScore(victimTeamPM, MG_SCORE_TDM_SUICIDE_PENALTY);
            } else {
                MgPointManager_AddScore(killerTeamPM, MG_SCORE_TDM_KILL_VALUE);
            }
            break;
        case 2:  // CTF — no kill scoring
            break;
    }
}

// ------------------------------------------------------------------
// Conquest Trickle (per-frame score from held capture points)
//
// Each held CP adds MG_SCORE_CONQUEST_TRICKLE_RATE (3) pts/sec.
// The gamemode also has CQ_Team_TrickleRate (10 pts/sec) applied
// separately as a base gamemode trickle.
//
// Called every frame with deltaTime:
//   accum += trickleRate * numHeldCPs * deltaTime
//   if accum >= 1.0: add (int)accum points, accum -= (int)accum
// ------------------------------------------------------------------
void MgPointManager_ConquestTrickle(MgPointManagerState* pm, int numHeldCPs, float deltaTime) {
    if (!pm || !pm->active || numHeldCPs <= 0) return;

    // Accumulator would be stored per-PointManager in the real game
    // For this stub, we calculate the integer points to add
    static float s_accum = 0.0f;  // Simplified — real game has per-entity accum
    s_accum += (float)(MG_SCORE_CONQUEST_TRICKLE_RATE * numHeldCPs) * deltaTime;

    if (s_accum >= 1.0f) {
        int pts = (int)s_accum;
        s_accum -= (float)pts;
        MgPointManager_AddScore(pm, pts);
    }
}

// ------------------------------------------------------------------
// PointManager Init (helper to set defaults)
// ------------------------------------------------------------------
void MgPointManager_Init(MgPointManagerState* pm, int team, int victoryScore) {
    if (!pm) return;
    memset(pm, 0, sizeof(MgPointManagerState));
    pm->team = team;
    pm->victoryScore = (victoryScore > 0) ? victoryScore : 9999;  // Default 0x270F
    pm->active = 1;
}

#ifdef __cplusplus
} // extern "C"
#endif
