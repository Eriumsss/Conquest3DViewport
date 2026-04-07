// Scene3DCamera.cpp — Your Eyes Inside This Nightmare
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Camera system: orbit, pan, dolly, fly-through, bookmarks, ortho views,
// Maya/Blender/Unreal navigation presets, auto-framing, MOPP collision
// building, and the entire fucking physics playtest character controller.
//
// Yes, physics playtest lives in the CAMERA file. Why? Because the camera
// and the physics character controller share movement input, and by the
// time I realized they should be separate files, 2000 lines of physics
// code were already here and moving them would break the .obj layout.
// So the camera file also contains hkpWorld creation, hkpCharacterProxy
// setup, MOPP BVTree building from level collision meshes, fixed-step
// physics simulation, and gravity. THE CAMERA FILE HAS GRAVITY. This
// is what 4 years of organic code growth looks like. This is what
// happens when you never refactor because refactoring means recompiling
// the stolen Havok SDK from scratch and that takes 45 fucking minutes.
//
// "Man is condemned to be free." — Sartre. I am condemned to maintain
// a camera file that also does collision detection. I am free to
// refactor it. I choose not to. Sartre would understand.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "GameModelLoader.h"
#include "LevelScene.h"
#include "ZeroMath.h"
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Animation/Rig/hkaPose.h>
#include <Common/Base/Math/Vector/hkVector4Util.h>
#include <Common/Base/Math/hkMath.h>
#include <Graphics/Common/Window/hkgWindowDefines.h>

// Havok Physics — character controller
#include <Physics/Dynamics/World/hkpWorld.h>
#include <Physics/Dynamics/Entity/hkpRigidBody.h>
#include <Physics/Collide/Shape/Convex/Capsule/hkpCapsuleShape.h>
#include <Physics/Collide/Shape/Convex/Box/hkpBoxShape.h>
#include <Physics/Dynamics/Phantom/hkpSimpleShapePhantom.h>
#include <Physics/Collide/Dispatch/hkpAgentRegisterUtil.h>
#include <Physics/Collide/Filter/Group/hkpGroupFilter.h>
#include <Physics/Utilities/CharacterControl/CharacterProxy/hkpCharacterProxy.h>
#include <Physics/Utilities/CharacterControl/CharacterProxy/hkpCharacterProxyCinfo.h>
#include <Physics/Utilities/CharacterControl/StateMachine/hkpCharacterState.h>
#include <Physics/Utilities/CharacterControl/StateMachine/hkpCharacterStateManager.h>
#include <Physics/Utilities/CharacterControl/StateMachine/hkpCharacterContext.h>
#include <Physics/Utilities/CharacterControl/StateMachine/hkpDefaultCharacterStates.h>

// Level collision — MOPP mesh shape
#include <Physics/Collide/Shape/Compound/Collection/ExtendedMeshShape/hkpExtendedMeshShape.h>
#include <Physics/Collide/Shape/Compound/Tree/Mopp/hkpMoppBvTreeShape.h>
#include <Physics/Collide/Shape/Compound/Tree/Mopp/hkpMoppUtility.h>
#include <Physics/Collide/Shape/Compound/Tree/Mopp/hkpMoppCompilerInput.h>
#include <Physics/Internal/Collide/Mopp/Code/hkpMoppCode.h>

#include "LevelScene.h"

#include <windows.h>
#include <commctrl.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include <algorithm>

// JSON animation clip loader (defined in Scene3DAnimation.cpp)
JsonAnimClip* LoadJsonAnimClip(const char* path,
                                const hkaSkeleton* skeleton,
                                int decodeMode,
                                int type2PackingMode);

// ---------------------------------------------------------------------------
// Camera-only static helper
// ---------------------------------------------------------------------------
static float GetCameraSpeedMultiplier()
{
    const bool slow = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool fast = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (slow && fast) return 1.0f; // cancel each other
    if (fast) return 2.5f;
    if (slow) return 0.35f;
    return 1.0f;
}

// ---------------------------------------------------------------------------
// Fly camera — toggle, mouse look, update
// ---------------------------------------------------------------------------
void Scene3DRenderer::toggleFlyCamera()
{
    m_flyCameraActive = !m_flyCameraActive;
    if (m_flyCameraActive)
    {
        // Initialize fly position from current orbit camera position
        hkVector4 pos = getCameraPosition();
        m_flyCamPos[0] = pos(0);
        m_flyCamPos[1] = pos(1);
        m_flyCamPos[2] = pos(2);
        m_flyCamYaw   = m_cameraYaw;
        m_flyCamPitch = m_cameraPitch;
    }
    else
    {
        // Shut down physics if active
        if (m_physicsActive)
            shutdownPhysicsPlaytest();

        // When exiting fly mode, update orbit camera to match fly position
        float fwdX = -cosf(m_flyCamPitch) * sinf(m_flyCamYaw);
        float fwdY = -sinf(m_flyCamPitch);
        float fwdZ = -cosf(m_flyCamPitch) * cosf(m_flyCamYaw);
        float dist = m_cameraDistance;
        m_cameraTarget.set(m_flyCamPos[0] + fwdX * dist,
                           m_flyCamPos[1] + fwdY * dist,
                           m_flyCamPos[2] + fwdZ * dist);
        m_cameraYaw   = m_flyCamYaw;
        m_cameraPitch = m_flyCamPitch;
    }
}

void Scene3DRenderer::flyCameraMouseLook(int dx, int dy)
{
    if (!m_flyCameraActive) return;
    float sensitivity = 0.003f;
    m_flyCamYaw   += dx * sensitivity;
    m_flyCamPitch += dy * sensitivity;
    // Clamp pitch to avoid gimbal lock
    if (m_flyCamPitch >  1.5f) m_flyCamPitch =  1.5f;
    if (m_flyCamPitch < -1.5f) m_flyCamPitch = -1.5f;
}

// ---------------------------------------------------------------------------
// Player animation: sample a JSON clip into a Havok pose (standalone helper)
// ---------------------------------------------------------------------------
static void SampleJsonClipIntoPose(const JsonAnimClip* clip, const hkaSkeleton* skeleton,
                                    float timeSeconds, hkaPose& pose)
{
    if (!clip || !skeleton)
    {
        pose.setToReferencePose();
        pose.syncModelSpace();
        return;
    }

    pose.setToReferencePose();

    float frameTime = (clip->frameTime > 0.0f) ? clip->frameTime : (1.0f / 30.0f);
    float duration = clip->duration;
    if (duration <= 0.0f)
    {
        int fc = (clip->frameCount > 0) ? clip->frameCount : 1;
        duration = frameTime * (float)(fc > 1 ? (fc - 1) : 1);
    }

    // Loop the animation
    float t = timeSeconds;
    if (duration > 0.0f)
    {
        t = fmodf(t, duration);
        if (t < 0.0f) t += duration;
    }

    float frame = (frameTime > 0.0f) ? (t / frameTime) : 0.0f;

    hkQsTransform* local = pose.writeAccessPoseLocalSpace().begin();

    // Sample rotation tracks
    for (int i = 0; i < (int)clip->tracks.size(); i++)
    {
        const JsonTrack& track = clip->tracks[i];
        if (track.boneIndex < 0 || track.boneIndex >= skeleton->m_numBones)
            continue;

        const int keyCount = (int)track.frames.size();
        if (keyCount == 0 || (int)track.rotations.size() != keyCount)
            continue;

        hkQuaternion q;
        q.setIdentity();

        if (keyCount == 1)
        {
            const JsonTrack::Quat4& qa = track.rotations[0];
            q.set(qa.x, qa.y, qa.z, qa.w);
        }
        else if (frame <= (float)track.frames[0])
        {
            const JsonTrack::Quat4& qa = track.rotations[0];
            q.set(qa.x, qa.y, qa.z, qa.w);
        }
        else if (frame >= (float)track.frames[keyCount - 1])
        {
            const JsonTrack::Quat4& qa = track.rotations[keyCount - 1];
            q.set(qa.x, qa.y, qa.z, qa.w);
        }
        else
        {
            // Binary search for interpolation segment
            std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
            int k = (int)(it - track.frames.begin()) - 1;
            if (k >= 0 && k < keyCount - 1)
            {
                int f0 = track.frames[k];
                int f1 = track.frames[k + 1];
                float span = (float)(f1 - f0);
                float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                if (alpha < 0.0f) alpha = 0.0f;
                if (alpha > 1.0f) alpha = 1.0f;

                hkQuaternion q0 = MakeQuaternion(track.rotations[k]);
                hkQuaternion q1 = MakeQuaternion(track.rotations[k + 1]);
                q = QuatSlerpShortest(q0, q1, alpha);
            }
        }

        local[track.boneIndex].setRotation(q);
    }

    // Sample translation tracks
    for (int i = 0; i < (int)clip->translationTracks.size(); i++)
    {
        const JsonTranslationTrack& track = clip->translationTracks[i];
        if (track.boneIndex < 0 || track.boneIndex >= skeleton->m_numBones)
            continue;

        const int keyCount = (int)track.frames.size();
        if (keyCount == 0 || (int)track.translations.size() != keyCount)
            continue;

        JsonVec3 tv = track.translations[0];
        if (keyCount > 1 && frame > (float)track.frames[0])
        {
            if (frame >= (float)track.frames[keyCount - 1])
            {
                tv = track.translations[keyCount - 1];
            }
            else
            {
                std::vector<int>::const_iterator it = std::upper_bound(track.frames.begin(), track.frames.end(), frame, FloatLessInt());
                int k = (int)(it - track.frames.begin()) - 1;
                if (k >= 0 && k < keyCount - 1)
                {
                    int f0 = track.frames[k];
                    int f1 = track.frames[k + 1];
                    float span = (float)(f1 - f0);
                    float alpha = (span > 0.0f) ? ((frame - (float)f0) / span) : 0.0f;
                    if (alpha < 0.0f) alpha = 0.0f;
                    if (alpha > 1.0f) alpha = 1.0f;
                    // Catmull-Rom for smooth camera translation
                    // Camera clips always loop
                    tv = CatmullRomVec3(track.frames, track.translations, keyCount, k, alpha, true);
                }
            }
        }

        hkVector4 trans; trans.set(tv.x, tv.y, tv.z);
        local[track.boneIndex].setTranslation(trans);
    }

    pose.syncModelSpace();
}

// Blend two pre-sampled poses: out = lerp(poseA, poseB, alpha)
static void BlendPoses(const hkaSkeleton* skeleton,
                        const JsonAnimClip* clipA, float timeA,
                        const JsonAnimClip* clipB, float timeB,
                        float alpha, hkaPose& outPose)
{
    if (alpha <= 0.001f)
    {
        SampleJsonClipIntoPose(clipA, skeleton, timeA, outPose);
        return;
    }
    if (alpha >= 0.999f)
    {
        SampleJsonClipIntoPose(clipB, skeleton, timeB, outPose);
        return;
    }

    // Sample both into temporary poses, then blend
    hkaPose poseA(skeleton);
    hkaPose poseB(skeleton);
    SampleJsonClipIntoPose(clipA, skeleton, timeA, poseA);
    SampleJsonClipIntoPose(clipB, skeleton, timeB, poseB);

    outPose.setToReferencePose();
    hkQsTransform* outLocal = outPose.writeAccessPoseLocalSpace().begin();
    const hkQsTransform* localA = poseA.getPoseLocalSpace().begin();
    const hkQsTransform* localB = poseB.getPoseLocalSpace().begin();

    for (int i = 0; i < skeleton->m_numBones; i++)
    {
        hkQsTransform blended;
        blended.setInterpolate4(localA[i], localB[i], alpha);
        outLocal[i] = blended;
    }

    outPose.syncModelSpace();
}

// Get the clip duration (looping)
static float GetClipDuration(const JsonAnimClip* clip)
{
    if (!clip) return 1.0f;
    float duration = clip->duration;
    if (duration <= 0.0f)
    {
        float frameTime = (clip->frameTime > 0.0f) ? clip->frameTime : (1.0f / 30.0f);
        int fc = (clip->frameCount > 0) ? clip->frameCount : 1;
        duration = frameTime * (float)(fc > 1 ? (fc - 1) : 1);
    }
    return (duration > 0.0f) ? duration : 1.0f;
}

// ---------------------------------------------------------------------------
// Physics playtest — Havok character controller with gravity
// ---------------------------------------------------------------------------
static const float PHYS_TIMESTEP    = 1.0f / 60.0f;
static const float PHYS_GRAVITY     = -40.0f;   // game units/s^2
static const float PHYS_CAPSULE_H   = 2.5f;     // half-height of capsule axis
static const float PHYS_CAPSULE_R   = 1.5f;     // capsule radius
static const float PHYS_EYE_OFFSET  = 2.0f;     // camera Y offset above capsule center
static const float PHYS_WALK_SPEED  = 25.0f;    // walk speed (units/s)
static const float PHYS_JUMP_VEL   = 18.0f;     // jump initial velocity

bool Scene3DRenderer::initPhysicsPlaytest()
{
    if (m_physicsActive) return true;
    if (!m_flyCameraActive) return false;

    // ── Create physics world ──
    hkpWorldCinfo worldInfo;
    worldInfo.m_gravity.set(0.0f, PHYS_GRAVITY, 0.0f);
    worldInfo.setBroadPhaseWorldSize(100000.0f);
    worldInfo.m_collisionTolerance = 0.1f;

    hkpWorld* world = new hkpWorld(worldInfo);
    world->lock();
    hkpAgentRegisterUtil::registerAllAgents(world->getCollisionDispatcher());

    // ── Level collision geometry (MOPP mesh shape from loaded triangles) ──
    float groundY = 0.0f;
    if (m_levelScene && m_levelScene->isLoaded() && m_levelScene->hasBounds())
    {
        float bMin[3], bMax[3];
        m_levelScene->getBounds(bMin, bMax);
        groundY = bMin[1];
    }
    m_physGroundY = groundY;

    bool hasLevelCollision = false;
    if (m_levelScene && m_levelScene->isLoaded())
    {
        std::vector<float> triPos;
        std::vector<int>   triIdx;
        m_levelScene->getCollisionTriangles(triPos, triIdx);

        int numVerts = (int)(triPos.size() / 3);
        int numIdx   = (int)triIdx.size();
        int numTris  = numIdx / 3;

        if (numVerts > 0 && numTris > 0)
        {
            char logBuf[256];
            sprintf_s(logBuf, "Building collision: %d verts, %d tris...", numVerts, numTris);
            RendererLog(logBuf);

            // ── Progress window (same style as bank loader) ──
            WNDCLASSEX moppWc = {0};
            moppWc.cbSize        = sizeof(WNDCLASSEX);
            moppWc.lpfnWndProc   = DefWindowProc;
            moppWc.hInstance     = GetModuleHandle(NULL);
            moppWc.lpszClassName = "MoppBuildProgressClass";
            moppWc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            RegisterClassEx(&moppWc);

            HWND moppDlg = CreateWindowEx(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                "MoppBuildProgressClass",
                "Building Collision",
                WS_POPUP | WS_BORDER | WS_CAPTION,
                (GetSystemMetrics(SM_CXSCREEN) - 420) / 2,
                (GetSystemMetrics(SM_CYSCREEN) - 130) / 2,
                420, 130,
                NULL, NULL, GetModuleHandle(NULL), NULL);

            InitCommonControls();
            HWND moppBar = CreateWindowEx(
                0, PROGRESS_CLASS, NULL,
                WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
                20, 20, 380, 22,
                moppDlg, NULL, GetModuleHandle(NULL), NULL);
            SendMessage(moppBar, PBM_SETMARQUEE, TRUE, 40);

            char statusBuf[256];
            sprintf_s(statusBuf, "Building collision: %d verts, %d tris...", numVerts, numTris);
            HWND moppStatus = CreateWindowEx(
                0, "STATIC", statusBuf,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                20, 55, 380, 40,
                moppDlg, NULL, GetModuleHandle(NULL), NULL);

            ShowWindow(moppDlg, SW_SHOW);
            UpdateWindow(moppDlg);
            { MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }

            // Allocate 16-byte aligned vertex array (4 floats per vert for Havok alignment)
            float* verts = (float*)_aligned_malloc(numVerts * 4 * sizeof(float), 16);
            for (int i = 0; i < numVerts; ++i)
            {
                verts[i * 4 + 0] = triPos[i * 3 + 0];
                verts[i * 4 + 1] = triPos[i * 3 + 1];
                verts[i * 4 + 2] = triPos[i * 3 + 2];
                verts[i * 4 + 3] = 0.0f;
            }

            // Update progress: mesh shape
            SetWindowText(moppStatus, "Building extended mesh shape...");
            { MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }

            // Allocate index array (16-bit for Havok)
            hkUint16* indices = (hkUint16*)malloc(numIdx * sizeof(hkUint16));
            for (int i = 0; i < numIdx; ++i)
                indices[i] = (hkUint16)(triIdx[i] & 0xFFFF);

            // Build hkpExtendedMeshShape
            hkpExtendedMeshShape* meshShape = new hkpExtendedMeshShape();

            hkpExtendedMeshShape::TrianglesSubpart part;
            part.m_vertexBase        = verts;
            part.m_vertexStriding    = 4 * sizeof(float);  // 16 bytes per vertex
            part.m_numVertices       = numVerts;
            part.m_indexBase         = indices;
            part.m_indexStriding     = 3 * sizeof(hkUint16);  // 3 indices per triangle
            part.m_numTriangleShapes = numTris;
            part.m_stridingType      = hkpExtendedMeshShape::INDICES_INT16;
            meshShape->addTrianglesSubpart(part);

            // Update progress: MOPP BVH
            SetWindowText(moppStatus, "Computing MOPP BVH (this may take a few seconds)...");
            { MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }

            // Build MOPP code (BVH acceleration) — this is the slow part
            hkpMoppCompilerInput mci;
            hkpMoppCode* moppCode = hkpMoppUtility::buildCode(meshShape, mci);

            hkpMoppBvTreeShape* moppShape = new hkpMoppBvTreeShape(meshShape, moppCode);
            moppCode->removeReference();
            meshShape->removeReference();

            // Update progress: adding to world
            SetWindowText(moppStatus, "Adding collision body to physics world...");
            { MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }

            // Add as fixed rigid body
            hkpRigidBodyCinfo levelCi;
            levelCi.m_shape      = moppShape;
            levelCi.m_motionType = hkpMotion::MOTION_FIXED;
            levelCi.m_position.setZero4();
            hkpRigidBody* levelBody = new hkpRigidBody(levelCi);
            world->addEntity(levelBody);
            levelBody->removeReference();
            moppShape->removeReference();

            // Store pointers for cleanup
            m_physLevelVerts    = verts;
            m_physLevelIndices  = indices;
            m_physLevelVertCount = numVerts;
            m_physLevelIdxCount  = numIdx;
            hasLevelCollision = true;

            sprintf_s(logBuf, "Level collision built: %d tris, MOPP ready", numTris);
            RendererLog(logBuf);

            // Close progress window
            SetWindowText(moppStatus, "Done!");
            { MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); } }
            DestroyWindow(moppDlg);
        }
    }

    // Fallback: flat ground plane if no level geometry
    if (!hasLevelCollision)
    {
        hkVector4 groundHalf(50000.0f, 1.0f, 50000.0f);
        hkpBoxShape* groundShape = new hkpBoxShape(groundHalf, 0.0f);
        hkpRigidBodyCinfo groundCi;
        groundCi.m_shape = groundShape;
        groundCi.m_motionType = hkpMotion::MOTION_FIXED;
        groundCi.m_position.set(0.0f, groundY - 1.0f, 0.0f);
        hkpRigidBody* groundBody = new hkpRigidBody(groundCi);
        world->addEntity(groundBody);
        groundBody->removeReference();
        groundShape->removeReference();
    }

    // ── Character capsule (Y-up) ──
    hkVector4 vertA(0.0f,  PHYS_CAPSULE_H, 0.0f);
    hkVector4 vertB(0.0f, -PHYS_CAPSULE_H, 0.0f);
    hkpCapsuleShape* capsule = new hkpCapsuleShape(vertA, vertB, PHYS_CAPSULE_R);

    hkpSimpleShapePhantom* phantom = new hkpSimpleShapePhantom(
        capsule, hkTransform::getIdentity(),
        hkpGroupFilter::calcFilterInfo(0, 2));
    world->addPhantom(phantom);

    // ── Character proxy ──
    hkpCharacterProxyCinfo cpci;
    cpci.m_position.set(m_flyCamPos[0],
                        m_flyCamPos[1] - PHYS_EYE_OFFSET,
                        m_flyCamPos[2]);
    cpci.m_staticFriction  = 0.0f;
    cpci.m_dynamicFriction = 1.0f;
    cpci.m_up.set(0.0f, 1.0f, 0.0f);
    cpci.m_maxSlope        = HK_REAL_PI / 3.0f;
    cpci.m_shapePhantom    = phantom;
    cpci.m_userPlanes      = 4;
    hkpCharacterProxy* proxy = new hkpCharacterProxy(cpci);

    // ── State machine (walk / in-air / jump) ──
    hkpCharacterStateManager* mgr = new hkpCharacterStateManager();

    hkpCharacterState* s;
    s = new hkpCharacterStateOnGround(); mgr->registerState(s, HK_CHARACTER_ON_GROUND); s->removeReference();
    s = new hkpCharacterStateInAir();    mgr->registerState(s, HK_CHARACTER_IN_AIR);    s->removeReference();
    s = new hkpCharacterStateJumping();  mgr->registerState(s, HK_CHARACTER_JUMPING);   s->removeReference();

    hkpCharacterContext* ctx = new hkpCharacterContext(mgr, HK_CHARACTER_ON_GROUND);
    mgr->removeReference();

    phantom->removeReference();  // world + proxy hold refs
    capsule->removeReference();  // phantom holds ref

    world->unlock();

    // Store
    m_physWorld     = (void*)world;
    m_physCharProxy = (void*)proxy;
    m_physCharCtx   = (void*)ctx;
    m_physGroundY   = groundY;
    m_physAccum     = 0.0f;
    m_playerYaw     = m_flyCamYaw;
    m_physicsActive = true;

    // Load player character model (Gondor Soldier) if not already loaded
    if (!m_playerModel)
    {
        loadPlayerModel(
            "../GameFiles/jmodels/CRD_CH_gdr_swd_all_01.json",
            "../GameFiles/models/CRD_CH_gdr_swd_all_01.glb",
            "../GameFiles/textures/");
    }

    // ── Load player animation clips (idle / walk / run) ──
    if (m_playerModel && m_playerModel->skeleton)
    {
        const hkaSkeleton* skel = m_playerModel->skeleton;
        const int decodeMode = 0;  // havok mode (default)
        const int packMode   = 0;  // interleaved (default)

        if (!m_playerClipIdle)
            m_playerClipIdle = LoadJsonAnimClip("../GameFiles/animations/RH6_Swd_Idle.json", skel, decodeMode, packMode);
        if (!m_playerClipWalk)
            m_playerClipWalk = LoadJsonAnimClip("../GameFiles/animations/RH6_swd_loc_fwd_walk_N.json", skel, decodeMode, packMode);
        if (!m_playerClipRun)
            m_playerClipRun  = LoadJsonAnimClip("../GameFiles/animations/RH6_swd_loc_fwd_run_N.json", skel, decodeMode, packMode);

        char clipMsg[256];
        sprintf_s(clipMsg, "Player anim clips: idle=%s walk=%s run=%s",
                  m_playerClipIdle ? "OK" : "FAIL",
                  m_playerClipWalk ? "OK" : "FAIL",
                  m_playerClipRun  ? "OK" : "FAIL");
        RendererLog(clipMsg);
    }

    // Reset animation state
    m_playerAnimState = PLAYER_ANIM_IDLE;
    m_playerAnimTime  = 0.0f;
    m_playerAnimBlend = 0.0f;
    m_playerAnimBlendTime = 0.0f;
    m_playerAnimPrevState = PLAYER_ANIM_IDLE;
    m_playerAnimPrevTime  = 0.0f;
    m_playerSpeed = 0.0f;

    char msg[128];
    sprintf_s(msg, "Physics playtest ON  (ground Y=%.1f)", groundY);
    RendererLog(msg);
    return true;
}

void Scene3DRenderer::shutdownPhysicsPlaytest()
{
    if (!m_physicsActive) return;

    hkpCharacterProxy*  proxy = (hkpCharacterProxy*)m_physCharProxy;
    hkpCharacterContext* ctx   = (hkpCharacterContext*)m_physCharCtx;
    hkpWorld*            world = (hkpWorld*)m_physWorld;

    if (proxy) proxy->removeReference();
    if (ctx)   ctx->removeReference();
    if (world) {
        world->markForWrite();
        world->removeReference();
    }

    m_physCharProxy = NULL;
    m_physCharCtx   = NULL;
    m_physWorld     = NULL;
    m_physicsActive = false;

    // Free level collision data
    if (m_physLevelVerts)   { _aligned_free(m_physLevelVerts);  m_physLevelVerts = NULL; }
    if (m_physLevelIndices) { free(m_physLevelIndices);         m_physLevelIndices = NULL; }
    m_physLevelVertCount = 0;
    m_physLevelIdxCount  = 0;

    if (m_playerAnimChar) { delete m_playerAnimChar; m_playerAnimChar = NULL; }
    if (m_playerModel)    { m_playerModel->release(); delete m_playerModel; m_playerModel = NULL; }

    // Free player animation clips
    if (m_playerClipIdle) { delete m_playerClipIdle; m_playerClipIdle = NULL; }
    if (m_playerClipWalk) { delete m_playerClipWalk; m_playerClipWalk = NULL; }
    if (m_playerClipRun)  { delete m_playerClipRun;  m_playerClipRun  = NULL; }

    RendererLog("Physics playtest OFF");
}

void Scene3DRenderer::togglePhysicsPlaytest()
{
    if (m_physicsActive)
        shutdownPhysicsPlaytest();
    else
        initPhysicsPlaytest();
}

// Havok physics character step — mirrors CharacterControllerDemo::stepDemo()
static void StepPhysicsCharacter(hkpWorld* world, hkpCharacterProxy* proxy,
                                  hkpCharacterContext* ctx,
                                  float yaw, float /*pitch*/,
                                  float* outPos, float* outPlayerYaw,
                                  float deltaTime)
{
    if (deltaTime <= 0.0f) return;

    // --- Character update (under world lock) ---
    world->lock();

    // Y-up world
    hkVector4 up; up.set(0.0f, 1.0f, 0.0f);

    // Forward direction from camera yaw (horizontal plane only)
    hkVector4 forward;
    forward.set(-sinf(yaw), 0.0f, -cosf(yaw));

    // WASD → inputLR / inputUD  (range -1..+1)
    hkReal inputLR = 0.0f;   // A/D
    hkReal inputUD = 0.0f;   // W/S
    if (GetAsyncKeyState('W') & 0x8000) inputUD += 1.0f;
    if (GetAsyncKeyState('S') & 0x8000) inputUD -= 1.0f;
    if (GetAsyncKeyState('A') & 0x8000) inputLR -= 1.0f;
    if (GetAsyncKeyState('D') & 0x8000) inputLR += 1.0f;

    hkBool wantJump = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;

    // Update player facing direction when moving
    if (inputLR != 0.0f || inputUD != 0.0f)
    {
        // Movement direction in world space from camera yaw + WASD
        float moveX = -sinf(yaw) * inputUD + cosf(yaw) * inputLR;
        float moveZ = -cosf(yaw) * inputUD - sinf(yaw) * inputLR;
        float moveYaw = atan2f(-moveX, -moveZ);
        // Smooth towards movement direction
        float diff = moveYaw - *outPlayerYaw;
        while (diff >  3.14159f) diff -= 6.28318f;
        while (diff < -3.14159f) diff += 6.28318f;
        *outPlayerYaw += diff * 0.2f;  // smooth turn
    }

    // Build character input  (same fields as the demo)
    hkpCharacterInput  input;
    hkpCharacterOutput output;

    input.m_inputLR = inputLR;
    input.m_inputUD = inputUD;
    input.m_wantJump = wantJump;
    input.m_atLadder = false;

    input.m_up      = up;
    input.m_forward = forward;

    input.m_stepInfo.m_deltaTime    = deltaTime;
    input.m_stepInfo.m_invDeltaTime = 1.0f / deltaTime;
    input.m_characterGravity.set(0.0f, PHYS_GRAVITY, 0.0f);

    input.m_velocity = proxy->getLinearVelocity();
    input.m_position = proxy->getPosition();

    // Ground support check
    hkVector4 down; down.setNeg4(up);
    hkpSurfaceInfo ground;
    proxy->checkSupport(down, ground);
    input.m_isSupported    = (ground.m_supportedState == hkpSurfaceInfo::SUPPORTED);
    input.m_surfaceNormal  = ground.m_surfaceNormal;
    input.m_surfaceVelocity = ground.m_surfaceVelocity;

    // State machine → produces output velocity
    ctx->update(input, output);

    // Feed output into character proxy
    proxy->setLinearVelocity(output.m_velocity);

    hkStepInfo si;
    si.m_deltaTime    = deltaTime;
    si.m_invDeltaTime = 1.0f / deltaTime;
    proxy->integrate(si, world->getGravity());

    world->unlock();

    // Step the world (must be outside lock)
    world->stepDeltaTime(deltaTime);

    // Read back proxy position
    world->lock();
    hkVector4 proxyPos = proxy->getPosition();
    outPos[0] = proxyPos(0);
    outPos[1] = proxyPos(1) + PHYS_EYE_OFFSET;   // eye height above capsule center
    outPos[2] = proxyPos(2);
    world->unlock();
}

// ---------------------------------------------------------------------------
// Fly camera frame update (WASD + Space/C movement)
// ---------------------------------------------------------------------------
static void UpdateFlyCameraMovement(float* pos, float yaw, float pitch,
                                    float speed, float deltaTime)
{
    if (deltaTime <= 0.0f) return;

    float mul = GetCameraSpeedMultiplier();
    float move = speed * deltaTime * mul;

    // Full 3D forward vector (W/S move along look direction)
    float fwdX = -cosf(pitch) * sinf(yaw);
    float fwdY = -sinf(pitch);
    float fwdZ = -cosf(pitch) * cosf(yaw);

    // Right vector (horizontal only)
    float rtX =  cosf(yaw);
    float rtZ = -sinf(yaw);

    if (GetAsyncKeyState('W') & 0x8000) { pos[0] += fwdX * move; pos[1] += fwdY * move; pos[2] += fwdZ * move; }
    if (GetAsyncKeyState('S') & 0x8000) { pos[0] -= fwdX * move; pos[1] -= fwdY * move; pos[2] -= fwdZ * move; }
    if (GetAsyncKeyState('A') & 0x8000) { pos[0] -= rtX  * move; pos[2] -= rtZ  * move; }
    if (GetAsyncKeyState('D') & 0x8000) { pos[0] += rtX  * move; pos[2] += rtZ  * move; }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) { pos[1] += move; }
    if (GetAsyncKeyState('C') & 0x8000)      { pos[1] -= move; }
}

// ---------------------------------------------------------------------------
// Camera update from spherical coordinates (orbit) or fly camera
// ---------------------------------------------------------------------------
void Scene3DRenderer::updateCameraFromInput(float deltaTime)
{
    if (!m_camera) {
        return;
    }

    // ── Fly camera mode ──
    if (m_flyCameraActive)
    {
        if (m_physicsActive && m_physWorld && m_physCharProxy && m_physCharCtx)
        {
            // Physics-driven movement (fixed timestep accumulator)
            m_physAccum += deltaTime;
            while (m_physAccum >= PHYS_TIMESTEP)
            {
                StepPhysicsCharacter(
                    (hkpWorld*)m_physWorld,
                    (hkpCharacterProxy*)m_physCharProxy,
                    (hkpCharacterContext*)m_physCharCtx,
                    m_flyCamYaw, m_flyCamPitch,
                    m_flyCamPos, &m_playerYaw,
                    PHYS_TIMESTEP);
                m_physAccum -= PHYS_TIMESTEP;
            }

            // ── Update player animation state (idle/walk/run) ──
            {
                // Compute horizontal speed from WASD input
                float inputLR = 0.0f, inputUD = 0.0f;
                if (GetAsyncKeyState('W') & 0x8000) inputUD += 1.0f;
                if (GetAsyncKeyState('S') & 0x8000) inputUD -= 1.0f;
                if (GetAsyncKeyState('A') & 0x8000) inputLR -= 1.0f;
                if (GetAsyncKeyState('D') & 0x8000) inputLR += 1.0f;
                float inputMag = sqrtf(inputLR * inputLR + inputUD * inputUD);
                if (inputMag > 1.0f) inputMag = 1.0f;

                // Determine if running (Shift held)
                bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

                // Smoothly track speed for state transitions
                float targetSpeed = inputMag * (shiftHeld ? 1.0f : 0.5f);
                m_playerSpeed += (targetSpeed - m_playerSpeed) * (1.0f - expf(-10.0f * deltaTime));

                // State selection based on speed
                PlayerAnimState newState = PLAYER_ANIM_IDLE;
                if (m_playerSpeed > 0.6f)
                    newState = PLAYER_ANIM_RUN;
                else if (m_playerSpeed > 0.05f)
                    newState = PLAYER_ANIM_WALK;

                // Trigger crossfade on state change
                if (newState != m_playerAnimState)
                {
                    m_playerAnimPrevState = m_playerAnimState;
                    m_playerAnimPrevTime  = m_playerAnimTime;
                    m_playerAnimBlendTime = 0.0f;
                    m_playerAnimBlendDur  = 0.2f;  // 200ms crossfade
                    m_playerAnimBlend     = 0.0f;
                    m_playerAnimState     = newState;
                    m_playerAnimTime      = 0.0f;
                }

                // Advance animation time
                m_playerAnimTime += deltaTime;

                // Update crossfade blend
                if (m_playerAnimBlendTime < m_playerAnimBlendDur)
                {
                    m_playerAnimBlendTime += deltaTime;
                    m_playerAnimBlend = m_playerAnimBlendTime / m_playerAnimBlendDur;
                    if (m_playerAnimBlend > 1.0f) m_playerAnimBlend = 1.0f;
                    m_playerAnimPrevTime += deltaTime;
                }
                else
                {
                    m_playerAnimBlend = 1.0f;
                }
            }

            // Third-person camera: behind and above character
            float charX = m_flyCamPos[0];
            float charY = m_flyCamPos[1] - PHYS_EYE_OFFSET;  // feet position
            float charZ = m_flyCamPos[2];

            // Camera orbits behind character based on camera yaw/pitch
            float camBackX =  sinf(m_flyCamYaw) * m_tpCamDistance;
            float camBackZ =  cosf(m_flyCamYaw) * m_tpCamDistance;
            float camUpY   =  m_tpCamHeight;

            float posArray[3]    = { charX + camBackX,
                                     charY + camUpY,
                                     charZ + camBackZ };
            float targetArray[3] = { charX, charY + 2.0f, charZ };  // look at chest height
            float upArray[3]     = { 0.0f, 1.0f, 0.0f };

            m_camera->setFrom(posArray);
            m_camera->setTo(targetArray);
            m_camera->setUp(upArray);
            m_camera->setProjectionMode(HKG_CAMERA_PERSPECTIVE);
            m_camera->computeModelView();
            m_camera->setNear(0.5f);
            m_camera->setFar(100000.0f);
            m_camera->computeProjection();

            setGammaEnabled(m_gammaEnabled);
            setAnisotropy(m_maxAnisotropy);
            setMipBias(m_mipBias);
            return;
        }
        else
        {
            UpdateFlyCameraMovement(m_flyCamPos, m_flyCamYaw, m_flyCamPitch,
                                    m_flyCamSpeed, deltaTime);
        }

        // First-person fly camera
        float fwdX = -cosf(m_flyCamPitch) * sinf(m_flyCamYaw);
        float fwdY = -sinf(m_flyCamPitch);
        float fwdZ = -cosf(m_flyCamPitch) * cosf(m_flyCamYaw);

        float posArray[3]    = { m_flyCamPos[0], m_flyCamPos[1], m_flyCamPos[2] };
        float targetArray[3] = { m_flyCamPos[0] + fwdX,
                                 m_flyCamPos[1] + fwdY,
                                 m_flyCamPos[2] + fwdZ };
        float upArray[3]     = { 0.0f, 1.0f, 0.0f };

        m_camera->setFrom(posArray);
        m_camera->setTo(targetArray);
        m_camera->setUp(upArray);
        m_camera->setProjectionMode(HKG_CAMERA_PERSPECTIVE);
        m_camera->computeModelView();

        // Fixed near/far for level-scale viewing
        m_camera->setNear(0.5f);
        m_camera->setFar(100000.0f);
        m_camera->computeProjection();

        setGammaEnabled(m_gammaEnabled);
        setAnisotropy(m_maxAnisotropy);
        setMipBias(m_mipBias);
        return;
    }

    // ── Orbit camera mode (original) ──
    if (m_horizonLock)
    {
        m_cameraRoll = 0.0f;
    }

    // WASD fly-through: move camera target in horizontal view-forward/right
    if (deltaTime > 0.0f)
    {
        float speed = m_cameraDistance * 1.5f * deltaTime * GetCameraSpeedMultiplier();
        float fwdX = -sinf(m_cameraYaw);
        float fwdZ = -cosf(m_cameraYaw);
        float rtX  =  cosf(m_cameraYaw);
        float rtZ  = -sinf(m_cameraYaw);
        if (GetAsyncKeyState('W') & 0x8000) { m_cameraTarget(0) += fwdX * speed; m_cameraTarget(2) += fwdZ * speed; }
        if (GetAsyncKeyState('S') & 0x8000) { m_cameraTarget(0) -= fwdX * speed; m_cameraTarget(2) -= fwdZ * speed; }
        if (GetAsyncKeyState('A') & 0x8000) { m_cameraTarget(0) -= rtX  * speed; m_cameraTarget(2) -= rtZ  * speed; }
        if (GetAsyncKeyState('D') & 0x8000) { m_cameraTarget(0) += rtX  * speed; m_cameraTarget(2) += rtZ  * speed; }
    }

    // Calculate camera position from spherical coordinates (yaw/pitch)
    float x = m_cameraDistance * cosf(m_cameraPitch) * sinf(m_cameraYaw);
    float y = m_cameraDistance * sinf(m_cameraPitch);
    float z = m_cameraDistance * cosf(m_cameraPitch) * cosf(m_cameraYaw);

    hkVector4 position;
    position.set(x, y, z);
    position.add4(m_cameraTarget);

    // Forward vector
    hkVector4 fwd;
    fwd.set(m_cameraTarget(0) - position(0),
            m_cameraTarget(1) - position(1),
            m_cameraTarget(2) - position(2));
    fwd.normalize3();

    // Right vector
    hkVector4 upWorld; upWorld.set(0.0f, 1.0f, 0.0f);
    hkVector4 right; right.setCross(fwd, upWorld);
    if (right.length3() < 1e-4f) {
        right.set(1.0f, 0.0f, 0.0f);
    } else {
        right.normalize3();
    }

    // Apply roll around forward axis if unlocked
    hkVector4 upVec = upWorld;
    if (!m_horizonLock && fabsf(m_cameraRoll) > 1e-5f)
    {
        float c = cosf(m_cameraRoll);
        float s = sinf(m_cameraRoll);
        hkVector4 upRot;
        upRot.setMul4(c, upVec);
        hkVector4 rightScaled;
        rightScaled.setMul4(s, right);
        upRot.add4(rightScaled);
        upVec = upRot;
        upVec.normalize3();
    }

    // Convert hkVector4 to float arrays for camera (correct Havok v5.5.0 API)
    float posArray[3] = { position(0), position(1), position(2) };
    float targetArray[3] = { m_cameraTarget(0), m_cameraTarget(1), m_cameraTarget(2) };
    float upArray[3] = { upVec(0), upVec(1), upVec(2) };

    m_camera->setFrom(posArray);
    m_camera->setTo(targetArray);
    m_camera->setUp(upArray);
    m_camera->setProjectionMode(m_cameraOrtho ? HKG_CAMERA_ORTHOGRAPHIC : HKG_CAMERA_PERSPECTIVE);
    m_camera->computeModelView();

    // Dynamic near/far planes: scale with distance so close-up still works.
    // Near = distance * 0.001 clamped to [0.001, 10]
    // Far  = distance * 5000 clamped to [5000, 500000]
    {
        float dn = m_cameraDistance * 0.001f;
        if (dn < 0.001f) dn = 0.001f;
        if (dn > 10.0f)  dn = 10.0f;
        float df = m_cameraDistance * 5000.0f;
        if (df < 5000.0f)   df = 5000.0f;
        if (df > 500000.0f) df = 500000.0f;
        m_camera->setNear(dn);
        m_camera->setFar(df);
        m_camera->computeProjection();
    }

    // Apply gamma/anisotropy at camera update to handle device resets.
    setGammaEnabled(m_gammaEnabled);
    setAnisotropy(m_maxAnisotropy);
    setMipBias(m_mipBias);
}

hkVector4 Scene3DRenderer::getCameraPosition() const
{
    float x = m_cameraDistance * cosf(m_cameraPitch) * sinf(m_cameraYaw);
    float y = m_cameraDistance * sinf(m_cameraPitch);
    float z = m_cameraDistance * cosf(m_cameraPitch) * cosf(m_cameraYaw);

    hkVector4 position;
    position.set(x, y, z);
    position.add4(m_cameraTarget);
    return position;
}

// ---------------------------------------------------------------------------
// Direct camera setters
// ---------------------------------------------------------------------------
void Scene3DRenderer::setCameraPosition(const hkVector4& position)
{
    if (m_camera) {
        float posArray[3] = { position(0), position(1), position(2) };
        m_camera->setFrom(posArray);
        m_camera->computeModelView();
    }
}

void Scene3DRenderer::setCameraTarget(const hkVector4& target)
{
    m_cameraTarget = target;
    if (m_camera) {
        float targetArray[3] = { target(0), target(1), target(2) };
        m_camera->setTo(targetArray);
        m_camera->computeModelView();
    }
}

void Scene3DRenderer::setCameraUp(const hkVector4& up)
{
    if (m_camera) {
        float upArray[3] = { up(0), up(1), up(2) };
        m_camera->setUp(upArray);
        m_camera->computeModelView();
    }
}

void Scene3DRenderer::setCameraFOV(float fovDegrees)
{
    if (m_camera) {
        m_camera->setFOV(fovDegrees);
        m_camera->computeProjection();
    }
    m_cameraFovDegrees = fovDegrees;
}

void Scene3DRenderer::setCameraNearFar(float nearPlane, float farPlane)
{
    if (m_camera) {
        m_camera->setNear(nearPlane);
        m_camera->setFar(farPlane);
    }
}



// ---------------------------------------------------------------------------
// Mouse / wheel input
// ---------------------------------------------------------------------------
void Scene3DRenderer::onMouseMove(int x, int y, bool leftButton, bool rightButton)
{
    int dx = x - m_lastMouseX;
    int dy = y - m_lastMouseY;

    if (m_flyCameraActive)
    {
        // In fly mode, right-click drag = mouse look
        if (rightButton) {
            flyCameraMouseLook(dx, dy);
        }
    }
    else
    {
        if (leftButton) {
            orbitCamera((float)dx, (float)dy);
        }
        if (rightButton) {
            dollyCamera((float)dy * 4.0f);
        }
    }

    m_lastMouseX = x;
    m_lastMouseY = y;
}

void Scene3DRenderer::onMouseWheel(int delta)
{
    dollyCamera((float)(-delta));
}

// ---------------------------------------------------------------------------
// Orbit / pan / dolly
// ---------------------------------------------------------------------------
void Scene3DRenderer::orbitCamera(float dx, float dy)
{
    float mul = GetCameraSpeedMultiplier();
    m_cameraYaw += dx * 0.01f * mul;
    m_cameraPitch += dy * 0.01f * mul;

    const float maxPitch = 1.5f;
    if (m_cameraPitch > maxPitch) m_cameraPitch = maxPitch;
    if (m_cameraPitch < -maxPitch) m_cameraPitch = -maxPitch;
}

void Scene3DRenderer::panCamera(float dx, float dy)
{
    float mul = GetCameraSpeedMultiplier();
    // Pan target in view plane, scaled by camera distance.
    float dist = m_cameraDistance;
    if (dist < 0.001f) dist = 0.001f;
    float scale = dist * 0.0025f;
    scale *= mul;

    float cx = cosf(m_cameraPitch) * sinf(m_cameraYaw);
    float cy = sinf(m_cameraPitch);
    float cz = cosf(m_cameraPitch) * cosf(m_cameraYaw);
    float fx = -cx;
    float fy = -cy;
    float fz = -cz;

    float upx = 0.0f, upy = 1.0f, upz = 0.0f;

    float rx, ry, rz;
    ZCross3f(rx, ry, rz, fx, fy, fz, upx, upy, upz);
    float rlen = ZLength3f(rx, ry, rz);
    if (rlen < 1e-5f)
    {
        rx = 1.0f; ry = 0.0f; rz = 0.0f;
        rlen = 1.0f;
    }
    rx /= rlen; ry /= rlen; rz /= rlen;

    float cux, cuy, cuz;
    ZCross3f(cux, cuy, cuz, rx, ry, rz, fx, fy, fz);

    m_cameraTarget(0) += (-dx * rx + dy * cux) * scale;
    m_cameraTarget(1) += (-dx * ry + dy * cuy) * scale;
    m_cameraTarget(2) += (-dx * rz + dy * cuz) * scale;
}

void Scene3DRenderer::dollyCamera(float delta)
{
    float mul = GetCameraSpeedMultiplier();
    float scale = m_cameraDistance * 0.001f;
    if (scale < 0.01f) scale = 0.01f;
    m_cameraDistance += delta * scale * mul;
    if (m_cameraDistance < 0.05f) m_cameraDistance = 0.05f;
    if (m_cameraDistance > 50000.0f) m_cameraDistance = 50000.0f;
}

// ---------------------------------------------------------------------------
// Focus / home / navigation presets
// ---------------------------------------------------------------------------
void Scene3DRenderer::focusSelection()
{
    // Focus selected bone if any, otherwise frame whole model.
    if (m_gameModel && m_gameModel->skeleton && m_selectedBoneIndex >= 0 &&
        m_selectedBoneIndex < m_gameModel->skeleton->m_numBones)
    {
        hkaPose pose(m_gameModel->skeleton);
        pose.setToReferencePose();
        pose.syncModelSpace();
        hkVector4 pos = pose.getBoneModelSpace(m_selectedBoneIndex).getTranslation();
        m_cameraTarget = pos;
        float span = 2.0f;
        m_cameraDistance = span;
        if (m_cameraDistance < 1.5f) m_cameraDistance = 1.5f;
        updateCameraFromInput(0.0f);
        return;
    }

    // Mesh part focus could be implemented later; reuse model auto-frame for now.
    autoFrameLoadedModel();
    updateCameraFromInput(0.0f);
}

void Scene3DRenderer::homeCamera()
{
    // Reset to current nav preset values and clear roll/ortho.
    setOrthoEnabled(false);
    m_horizonLock = true;
    m_cameraRoll = 0.0f;
    setNavigationMode(m_navMode);
}

void Scene3DRenderer::setNavigationMode(int mode)
{
    if (mode < 1 || mode > 3) return;
    m_navMode = mode;

    switch (mode)
    {
    case 1: // Maya-style
        m_cameraYaw   = 0.785f;   // 45 deg
        m_cameraPitch = 0.35f;    // ~20 deg
        m_cameraDistance = 6.0f;
        setCameraFOV(60.0f);
        break;
    case 2: // Blender-style
        m_cameraYaw   = 0.9f;
        m_cameraPitch = 0.6f;
        m_cameraDistance = 5.0f;
        setCameraFOV(50.0f);
        break;
    case 3: // Unreal-ish
        m_cameraYaw   = -1.05f;
        m_cameraPitch = 0.35f;
        m_cameraDistance = 7.5f;
        setCameraFOV(55.0f);
        break;
    default:
        break;
    }
    m_cameraRoll = 0.0f;
    setOrthoEnabled(false);
    updateCameraFromInput(0.0f);
}

const char* Scene3DRenderer::getNavigationModeName() const
{
    return m_navModeLabels[m_navMode];
}

bool Scene3DRenderer::loadCameraIni(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n') continue;
        char key[64], value[128];
        if (sscanf(line, "%63[^=]=%127s", key, value) != 2) continue;

        if (_stricmp(key, "nav1_label") == 0) { strncpy_s(m_navModeLabels[1], value, _TRUNCATE); }
        else if (_stricmp(key, "nav2_label") == 0) { strncpy_s(m_navModeLabels[2], value, _TRUNCATE); }
        else if (_stricmp(key, "nav3_label") == 0) { strncpy_s(m_navModeLabels[3], value, _TRUNCATE); }
        else if (_stricmp(key, "nav_default") == 0)
        {
            int v = atoi(value);
            if (v >= 1 && v <= 3) m_navMode = v;
        }
        else if (_stricmp(key, "nav1_yaw") == 0) { m_cameraYaw = (float)atof(value); }
        else if (_stricmp(key, "nav1_pitch") == 0) { m_cameraPitch = (float)atof(value); }
        else if (_stricmp(key, "nav1_dist") == 0) { m_cameraDistance = (float)atof(value); }
    }
    fclose(f);
    // Apply the (possibly) updated nav mode
    setNavigationMode(m_navMode);
    return true;
}

// ---------------------------------------------------------------------------
// Ortho views
// ---------------------------------------------------------------------------
void Scene3DRenderer::setOrthoEnabled(bool ortho)
{
    m_cameraOrtho = ortho;
    if (m_camera)
    {
        m_camera->setProjectionMode(ortho ? HKG_CAMERA_ORTHOGRAPHIC : HKG_CAMERA_PERSPECTIVE);
        m_camera->computeProjection();
    }
}

void Scene3DRenderer::applyOrthoView(int viewId, bool invert)
{
    // Blender-style numpad: 1=front, 3=side, 7=top. Ctrl reverses handled in caller.
    setOrthoEnabled(true);
    m_cameraRoll = 0.0f;
    m_horizonLock = true;
    switch (viewId)
    {
    case 1: // Front
        m_cameraYaw = 0.0f;
        m_cameraPitch = 0.0f;
        break;
    case 2: // Side
        m_cameraYaw = -HK_REAL_PI * 0.5f;
        m_cameraPitch = 0.0f;
        break;
    case 3: // Top
        m_cameraYaw = 0.0f;
        m_cameraPitch = -HK_REAL_PI * 0.5f + 0.0001f;
        break;
    default:
        break;
    }
    if (invert)
    {
        if (viewId == 3)
        {
            m_cameraPitch = HK_REAL_PI * 0.5f - 0.0001f; // bottom view
        }
        else
        {
            m_cameraYaw += HK_REAL_PI; // back/right
        }
    }
    updateCameraFromInput(0.0f);
}

// ---------------------------------------------------------------------------
// Bookmarks
// ---------------------------------------------------------------------------
void Scene3DRenderer::saveCameraBookmark(int slot)
{
    if (slot < 1 || slot > 5) return;
    CameraBookmark& b = m_camBookmarks[slot - 1];
    b.target = m_cameraTarget;
    b.yaw = m_cameraYaw;
    b.pitch = m_cameraPitch;
    b.roll = m_cameraRoll;
    b.distance = m_cameraDistance;
    b.valid = true;
    char msg[128];
    sprintf_s(msg, "Saved camera bookmark %d", slot);
    RendererLog(msg);
}

bool Scene3DRenderer::loadCameraBookmark(int slot)
{
    if (slot < 1 || slot > 5) return false;
    const CameraBookmark& b = m_camBookmarks[slot - 1];
    if (!b.valid) return false;
    m_cameraTarget = b.target;
    m_cameraYaw = b.yaw;
    m_cameraPitch = b.pitch;
    m_cameraRoll = b.roll;
    m_cameraDistance = b.distance;
    setOrthoEnabled(false);
    updateCameraFromInput(0.0f);
    char msg[128];
    sprintf_s(msg, "Loaded camera bookmark %d", slot);
    RendererLog(msg);
    return true;
}

// ---------------------------------------------------------------------------
// Horizon lock / roll
// ---------------------------------------------------------------------------
void Scene3DRenderer::toggleHorizonLock()
{
    m_horizonLock = !m_horizonLock;
    if (m_horizonLock) m_cameraRoll = 0.0f;
    updateCameraFromInput(0.0f);
}

void Scene3DRenderer::rollCamera(float deltaRadians)
{
    if (m_horizonLock) return;
    m_cameraRoll += deltaRadians;
    const float pi = 3.14159265f;
    if (m_cameraRoll > pi) m_cameraRoll -= 2.0f * pi;
    if (m_cameraRoll < -pi) m_cameraRoll += 2.0f * pi;
    updateCameraFromInput(0.0f);
}

// ---------------------------------------------------------------------------
// Auto-frame loaded model (bounds + skinning)
// ---------------------------------------------------------------------------
void Scene3DRenderer::autoFrameLoadedModel()
{
    if (!m_gameModel)
    {
        return;
    }

    const bool hasSkeleton = (m_gameModel->skeleton != HK_NULL);
    int boneCount = hasSkeleton ? m_gameModel->skeleton->m_numBones : 0;
    std::vector<float> boneMatrices;
    if (hasSkeleton && boneCount > 0)
    {
        hkaPose refPose(m_gameModel->skeleton);
        refPose.setToReferencePose();
        refPose.syncModelSpace();
        boneMatrices.resize(boneCount * 16);
        for (int b = 0; b < boneCount; ++b)
        {
            refPose.getBoneModelSpace(b).get4x4ColumnMajor(&boneMatrices[b * 16]);
        }
    }

    bool hasBounds = false;
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;

    for (int i = 0; i < m_gameModel->meshPartCount; ++i)
    {
        GameModel::MeshPart& part = m_gameModel->meshParts[i];
        if (!part.vertices || part.vertexCount <= 0)
        {
            continue;
        }

        const int skinBoneCount = (part.inverseBindMatrixCount < boneCount) ? part.inverseBindMatrixCount : boneCount;
        bool canSkin = (boneCount > 0 &&
                        part.skinWeights &&
                        part.inverseBindMatrices &&
                        part.inverseBindMatrixCount > 0 &&
                        skinBoneCount > 0);

        std::vector<float> skinMatrices;
        if (canSkin)
        {
            skinMatrices.resize(skinBoneCount * 16);
            for (int b = 0; b < skinBoneCount; ++b)
            {
                MultiplyMatrix4(&boneMatrices[b * 16],
                                part.inverseBindMatrices + b * 16,
                                &skinMatrices[b * 16]);
            }
        }

        for (int v = 0; v < part.vertexCount; ++v)
        {
            float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
            float x = base[0];
            float y = base[1];
            float z = base[2];

            float sx = x;
            float sy = y;
            float sz = z;

            if (canSkin)
            {
                sx = sy = sz = 0.0f;
                float weightSum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    float w = part.skinWeights[v].boneWeights[k];
                    int boneIdx = part.skinWeights[v].boneIndices[k];
                    if (w <= 0.0f || boneIdx < 0 || boneIdx >= skinBoneCount)
                        continue;

                    const float* m = &skinMatrices[boneIdx * 16];
                    sx += w * (m[0] * x + m[4] * y + m[8] * z + m[12]);
                    sy += w * (m[1] * x + m[5] * y + m[9] * z + m[13]);
                    sz += w * (m[2] * x + m[6] * y + m[10] * z + m[14]);
                    weightSum += w;
                }
                if (weightSum < 0.999f)
                {
                    float inv = 1.0f - weightSum;
                    sx += x * inv;
                    sy += y * inv;
                    sz += z * inv;
                }
            }

            sy += m_modelBaseOffsetY;

            if (!hasBounds)
            {
                minX = maxX = sx;
                minY = maxY = sy;
                minZ = maxZ = sz;
                hasBounds = true;
            }
            else
            {
                if (sx < minX) minX = sx;
                if (sy < minY) minY = sy;
                if (sz < minZ) minZ = sz;
                if (sx > maxX) maxX = sx;
                if (sy > maxY) maxY = sy;
                if (sz > maxZ) maxZ = sz;
            }
        }
    }

    if (!hasBounds)
    {
        return;
    }

    float centerX = (minX + maxX) * 0.5f;
    float centerY = (minY + maxY) * 0.5f;
    float centerZ = (minZ + maxZ) * 0.5f;
    float spanX = maxX - minX;
    float spanY = maxY - minY;
    float spanZ = maxZ - minZ;
    float span = spanX;
    if (spanY > span) span = spanY;
    if (spanZ > span) span = spanZ;
    if (span < 0.5f) span = 0.5f;

    m_cameraTarget.set(centerX, centerY, centerZ);
    m_cameraDistance = span * 2.4f;
    if (m_cameraDistance < 3.0f) m_cameraDistance = 3.0f;
    if (m_cameraDistance > 400.0f) m_cameraDistance = 400.0f;
}

void Scene3DRenderer::focusCameraOnModel()
{
    // Reuse the auto-frame logic to center and size the camera on current model.
    autoFrameLoadedModel();
}

void Scene3DRenderer::setCameraPreset(int presetId)
{
    setNavigationMode(presetId);
}

// ---------------------------------------------------------------------------
// Player animation pose building (called from renderPlayerCharacter)
// ---------------------------------------------------------------------------
void Scene3DRenderer::buildPlayerAnimationPose(hkaPose& outPose)
{
    if (!m_playerModel || !m_playerModel->skeleton)
    {
        outPose.setToReferencePose();
        outPose.syncModelSpace();
        return;
    }

    const hkaSkeleton* skel = m_playerModel->skeleton;

    // Pick the clip for the current state
    const JsonAnimClip* currentClip = NULL;
    switch (m_playerAnimState)
    {
    case PLAYER_ANIM_WALK: currentClip = m_playerClipWalk; break;
    case PLAYER_ANIM_RUN:  currentClip = m_playerClipRun;  break;
    default:               currentClip = m_playerClipIdle;  break;
    }

    // Pick the clip for the previous state (crossfade source)
    const JsonAnimClip* prevClip = NULL;
    switch (m_playerAnimPrevState)
    {
    case PLAYER_ANIM_WALK: prevClip = m_playerClipWalk; break;
    case PLAYER_ANIM_RUN:  prevClip = m_playerClipRun;  break;
    default:               prevClip = m_playerClipIdle;  break;
    }

    // If no clips loaded, fall back to reference pose
    if (!currentClip)
    {
        outPose.setToReferencePose();
        outPose.syncModelSpace();
        return;
    }

    // If we're in a crossfade, blend between previous and current
    if (m_playerAnimBlend < 0.999f && prevClip)
    {
        BlendPoses(skel, prevClip, m_playerAnimPrevTime,
                   currentClip, m_playerAnimTime,
                   m_playerAnimBlend, outPose);
    }
    else
    {
        SampleJsonClipIntoPose(currentClip, skel, m_playerAnimTime, outPose);
    }
}

// ============================================================
//  MOPP BVTree builder — generates Havok MOPP bytecode from triangle mesh
//  Called by Model Viewer's "Build Collision" to produce game-compatible
//  collision acceleration data.
// ============================================================

bool BuildMoppFromMesh(const float* vertXYZ, int numVerts,
                       const unsigned short* indices, int numTris,
                       unsigned char** outMoppBytes, int* outMoppSize)
{
    *outMoppBytes = NULL;
    *outMoppSize = 0;
    if (numVerts < 3 || numTris < 1) return false;

    // Allocate 16-byte aligned vertex array (4 floats per vert for Havok)
    float* verts = (float*)_aligned_malloc(numVerts * 4 * sizeof(float), 16);
    if (!verts) return false;
    for (int i = 0; i < numVerts; ++i) {
        verts[i * 4 + 0] = vertXYZ[i * 3 + 0];
        verts[i * 4 + 1] = vertXYZ[i * 3 + 1];
        verts[i * 4 + 2] = vertXYZ[i * 3 + 2];
        verts[i * 4 + 3] = 0.0f;
    }

    // Copy indices to hkUint16 array
    hkUint16* idx = (hkUint16*)malloc(numTris * 3 * sizeof(hkUint16));
    if (!idx) { _aligned_free(verts); return false; }
    for (int i = 0; i < numTris * 3; ++i)
        idx[i] = (hkUint16)(indices[i] & 0xFFFF);

    // Build mesh shape
    hkpExtendedMeshShape* meshShape = new hkpExtendedMeshShape();
    hkpExtendedMeshShape::TrianglesSubpart part;
    part.m_vertexBase        = verts;
    part.m_vertexStriding    = 4 * sizeof(float);
    part.m_numVertices       = numVerts;
    part.m_indexBase         = idx;
    part.m_indexStriding     = 3 * sizeof(hkUint16);
    part.m_numTriangleShapes = numTris;
    part.m_stridingType      = hkpExtendedMeshShape::INDICES_INT16;
    meshShape->addTrianglesSubpart(part);

    // Build MOPP
    hkpMoppCompilerInput mci;
    hkpMoppCode* moppCode = hkpMoppUtility::buildCode(meshShape, mci);

    if (moppCode) {
        int sz = moppCode->m_data.getSize();
        if (sz > 0) {
            *outMoppBytes = (unsigned char*)malloc(sz);
            if (*outMoppBytes) {
                memcpy(*outMoppBytes, moppCode->m_data.begin(), sz);
                *outMoppSize = sz;
            }
        }
        moppCode->removeReference();
    }

    meshShape->removeReference();
    free(idx);
    _aligned_free(verts);

    return (*outMoppSize > 0);
}
