#pragma once

// Apply memory patches for texture and vertex buffer limits
void ApplyMemoryPatches();

// Cleanup (close log file)
void CleanupMemoryPatcher();

// Danger-Aware Engine Layer: Called each frame from EndScene
// Updates danger state based on allocation failures and VEH catches
void DangerOnEndOfFrame();

