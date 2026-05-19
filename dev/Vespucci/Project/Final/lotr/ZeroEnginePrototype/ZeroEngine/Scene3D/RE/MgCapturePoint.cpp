// ================================================================
// MgCapturePoint.cpp - Capture Point State Machine
// Reconstructed from ConquestLLC.exe disassembly at FUN_008ebff6
// See MgCapturePoint.h for full documentation
// ================================================================

#include "MgCapturePoint.h"
#include "MgEventSystem.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// CapturePoint State Machine (FUN_008ebff6, 4293 bytes)
//
// Called every frame for each active capture point.
// State transitions:
//   Neutral -> Contested (players enter zone)
//   Contested -> Capturing (capture progress ticking)
//   Capturing -> Captured (progress reaches 1.0)
//   Captured -> Contested (enemy enters and neutralizes)
//
// Capture rate formula (from 0x008ec047):
//   rate = captureSpeed * numPlayers * difficultyMult * frameDelta * globalScale
//
// Timing values (from level.json cross-map analysis):
//   CaptureTime: 25.0s (1 player, 0->100%)
//   DecaptureTime: 35.0s (1 player, strip enemy ownership)
//   RevertDelay: 5.0s (grace period before bar reverts)
//   CaptureLevel: 0.5 (bar starts at 50% after neutralization)
//   PerCapperModifier: 0.3 (+30% speed per extra attacker)
//   PerDefenderModifier: 0.3 (+30% slow per defender)
//   ModifierLimit: 5 (max 5 extra players counted)
// ------------------------------------------------------------------
void MgCapturePoint_Update(
    void* cpEntity,
    float deltaTime,
    int attackerCount,
    int defenderCount,
    int attackerTeam
) {
    if (!cpEntity || deltaTime <= 0.0f) return;
    uint8_t* ent = (uint8_t*)cpEntity;

    float* progress     = (float*)(ent + MG_CP_OFF_CAPTURE_PROGRESS);
    int*   owningTeam   = (int*)(ent + MG_CP_OFF_OWNING_TEAM);
    int*   capturingTeam= (int*)(ent + MG_CP_OFF_CAPTURING_TEAM);
    float* captureSpeed = (float*)(ent + MG_CP_OFF_CAPTURE_SPEED);
    float* decapSpeed   = (float*)(ent + MG_CP_OFF_DECAPTURE_SPEED);

    // No attackers: progress may revert (after RevertDelay)
    if (attackerCount <= 0) {
        // Revert toward 0 or toward owning team's full capture
        if (*capturingTeam != 0 && *capturingTeam != *owningTeam) {
            *progress -= (*decapSpeed) * deltaTime;
            if (*progress <= 0.0f) {
                *progress = 0.0f;
                *capturingTeam = 0;
            }
        }
        return;
    }

    // Attackers present: calculate effective rate
    // rate = baseSpeed * (1 + min(extraAttackers, 5) * 0.3)
    //       * (1 - min(defenders, 5) * 0.3)
    int extraAttackers = (attackerCount > 1) ? (attackerCount - 1) : 0;
    if (extraAttackers > 5) extraAttackers = 5;
    if (defenderCount > 5) defenderCount = 5;

    float attackerMod = 1.0f + (float)extraAttackers * 0.3f;
    float defenderMod = 1.0f - (float)defenderCount * 0.3f;
    if (defenderMod < 0.1f) defenderMod = 0.1f;  // Floor at 10%

    float effectiveSpeed;

    if (*owningTeam == 0 || *owningTeam == attackerTeam) {
        // Capturing neutral or already-owned point
        effectiveSpeed = (*captureSpeed) * attackerMod * defenderMod;
        *capturingTeam = attackerTeam;

        *progress += effectiveSpeed * deltaTime;
        if (*progress >= 1.0f) {
            *progress = 1.0f;
            *owningTeam = attackerTeam;
            // Fire events (in real game):
            //   OnCapture at [entity+0x61C]
            //   OnRedCapture at [+0x620] or OnBlueCapture at [+0x624]
            //   OnPlayerCapture at [+0x634]
            //   Post Wwise sound event via FUN_00855712
        }
    } else {
        // Decapturing enemy point
        effectiveSpeed = (*decapSpeed) * attackerMod * defenderMod;
        *capturingTeam = attackerTeam;

        *progress -= effectiveSpeed * deltaTime;
        if (*progress <= 0.0f) {
            // Point neutralized — set to CaptureLevel (0.5)
            *progress = 0.5f;
            *owningTeam = 0;
            // Fire OnNeutralize event at [entity+0x628]
        }
    }
}

// ------------------------------------------------------------------
// CapturePoint::SetTeam (FUN_008e88bd)
//
// Forces the capture point to a specific team:
//   1. Set owning team at [entity+0x650]
//   2. Set progress to 1.0 (fully captured)
//   3. Fire team-specific output event
//
// Used by level scripts (e.g., initial CP ownership at match start).
// ------------------------------------------------------------------
void MgCapturePoint_SetTeam(void* cpEntity, int team) {
    if (!cpEntity) return;
    uint8_t* ent = (uint8_t*)cpEntity;

    *(int*)(ent + MG_CP_OFF_OWNING_TEAM) = team;
    *(int*)(ent + MG_CP_OFF_CAPTURING_TEAM) = team;
    *(float*)(ent + MG_CP_OFF_CAPTURE_PROGRESS) = (team != 0) ? 1.0f : 0.0f;
}

#ifdef __cplusplus
} // extern "C"
#endif
