// ViewportProj.cpp
// =============================================================================
// WORLD->SCREEN PROJECTION HELPERS FOR GHOST RENDERING
// =============================================================================
// Written by: Eriumsss
//
// The renderer-side ghost paint needs to project world-space candidate
// positions to screen pixels. Reuses the LevelScene's m_cachedVP that
// the F4 wire renderer already populates each frame (Phase 10a). This
// file is the bridge - we read m_cachedVP through an extern getter so
// Vespucci does not have to include LevelScene.h directly.
// =============================================================================

#include "../../Core/VespucciTypes.h"

namespace Vespucci {
namespace UI {
namespace GhostOverlay {

// Forward decl into LevelScene for the cached ViewProj. This pointer
// is set by ZeroEngine3DViewport between LevelScene::render() and the
// DLL's DrawFrame call. NULL until first frame after level load.
static const float* s_cachedVP = 0;
static const float* s_cachedCamPos = 0;

void BindViewportTransforms(const float* viewProj4x4, const float* camPos3) {
    s_cachedVP     = viewProj4x4;
    s_cachedCamPos = camPos3;
}

bool ProjectWorldToScreen(const Vec3& world, f32 screenW, f32 screenH,
                           f32& outScreenX, f32& outScreenY)
{
    if (!s_cachedVP) return false;
    const float* m = s_cachedVP;
    f32 cx = m[0]*world.x + m[4]*world.y + m[8]*world.z  + m[12];
    f32 cy = m[1]*world.x + m[5]*world.y + m[9]*world.z  + m[13];
    f32 cz = m[2]*world.x + m[6]*world.y + m[10]*world.z + m[14];
    f32 cw = m[3]*world.x + m[7]*world.y + m[11]*world.z + m[15];
    if (cw <= 0.0001f) return false; // behind camera
    f32 ndcX = cx / cw;
    f32 ndcY = cy / cw;
    outScreenX = (ndcX * 0.5f + 0.5f) * screenW;
    outScreenY = (1.0f - (ndcY * 0.5f + 0.5f)) * screenH;
    return true;
}

f32 DistanceToCameraOrZero(const Vec3& world) {
    if (!s_cachedCamPos) return 0.0f;
    f32 dx = world.x - s_cachedCamPos[0];
    f32 dy = world.y - s_cachedCamPos[1];
    f32 dz = world.z - s_cachedCamPos[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

} // namespace GhostOverlay
} // namespace UI
} // namespace Vespucci
