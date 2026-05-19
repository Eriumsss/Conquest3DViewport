// ============================================================================
// MocapExporter.h -- Forge Fake Conquest Animation Files from Webcam Data
// ============================================================================
//
// Takes retargeted mocap frames (SMPL -> 62-bone game skeleton) and writes
// them out as native Conquest animation JSON that LoadJsonAnimClip() can
// parse. This is the final step: real human motion -> game animation file.
//
// Rotations get packed into Havok v5.5.0 ThreeComp40 format -- 5 bytes per
// quaternion, 40 bits total, the same encoding Pandemic used for every
// animation in the shipped game. We reverse-engineered that format by
// staring at hex dumps until our eyes bled, then found the packing
// algorithm buried in the stolen Havok SDK. EncodeThreeComp40Strict()
// is our baby and she is PRECISE.
//
// The bone list (62 entries from "" to "Bone_RFootBone3") was extracted
// from the game's animation binaries. Every name must match EXACTLY or
// the animation system silently drops the track. Ask me how I know.
//
// VS2005 compatible (C++03). Havok 5.5.0 quaternion encoding.
// ============================================================================

#ifndef MOCAP_EXPORTER_H
#define MOCAP_EXPORTER_H

#include "MocapRetargeter.h"
#include "MocapBridge.h"
#include <vector>

class MocapExporter
{
public:
    // Export retargeted frames to native Conquest JSON format.
    // This produces a file that LoadJsonAnimClip() can parse directly.
    // Returns true on success.
    static bool ExportConquestJSON(
        const char* outputPath,
        const char* animName,
        const std::vector<RetargetedFrame>& frames,
        const MocapBridge& bridge,   // for root motion data
        float fps);
};

#endif // MOCAP_EXPORTER_H
