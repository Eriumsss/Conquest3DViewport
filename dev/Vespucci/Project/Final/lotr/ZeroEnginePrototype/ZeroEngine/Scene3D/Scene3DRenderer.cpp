// Scene3DRenderer.cpp — The Main Loop. The Sacred Ritual. The One Function.
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Implementation of Scene3DRenderer — initialize(), shutdown(), stepAndRender(),
// the whole lifecycle. This is where the D3D9 device gets created through
// Havok's hkgWindow, where the display world gets populated, where the
// camera gets updated, where the frame gets rendered. Every single pixel
// that appears on screen passes through code in this file.
//
// The initialization sequence mirrors Pandemic's boot process: create
// hkgSystem → create hkgWindow (which creates the D3D9 device internally)
// → create hkgDisplayWorld → create hkgCamera → setup lights → done.
// We know this because we traced the game's WinMain through 14 function
// calls until we hit the same Havok API sequence. Pandemic didn't
// deviate from Havok's demo framework. Neither do we. The stolen
// SDK's demo code is the Rosetta Stone for everything in this file.
//
// "Amor fati." — Nietzsche. Love your fate. My fate is to maintain a
// D3D9 renderer built on a stolen 2008 SDK for a dead game. I have
// accepted this. I have embraced it. I fucking LOVE this fate now.
// Stockholm syndrome? Maybe. But the renderer works. 60fps. Every frame.
// -----------------------------------------------------------------------

#include "Scene3DRenderer.h"
#include "Scene3DRendererInternal.h"
#include "Scene3DLoader.h"
#include "ZeroMath.h"
#include "HavokToDisplayConverter.h"
#include "AnimationSystem.h"
#include "GameModelLoader.h"
#include "Scene3DEffects.h"
#include "Scene3DEffectManager.h"
#include "MgPackedParticleShaders.h"
#include "RefractionPSBytecode.h"
#include "LevelScene.h"
#include "LevelReader.h"
#include <Common/Serialize/Util/hkLoader.h>
#include <Common/Serialize/Util/hkRootLevelContainer.h>
#include <Animation/Animation/hkaAnimationContainer.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Internal/Compression/hkaCompression.h>
#include <Common/Base/Math/Vector/hkVector4Util.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>
#include <Common/Base/Math/Matrix/hkRotation.h>
#include <Common/Base/Math/hkMath.h>
#include <Graphics/Common/Window/hkgWindowDefines.h>
#include <Graphics/Common/Geometry/hkgGeometry.h>
#include <Graphics/Common/Texture/hkgTexture.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

static void ResetD3D9StateAfterFx(IDirect3DDevice9* dev)
{
    if (!dev)
        return;

    // These are the most common "leaky" states that cause wireframe, white-out,
    // or broken shading across frames when FX rendering returns early.
    dev->SetVertexShader(NULL);
    dev->SetPixelShader(NULL);

    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Unbind common texture stages used by custom FX shaders.
    for (int s = 0; s < 4; ++s)
        dev->SetTexture(s, NULL);
}

static void D3DPerfMarker(bool enabled, const wchar_t* name, DWORD color = 0xFF88CCFF)
{
    if (!enabled || !name)
        return;
    D3DPERF_SetMarker(color, name);
}

static void D3DPerfBegin(bool enabled, const wchar_t* name, DWORD color = 0xFF44AA44)
{
    if (!enabled || !name)
        return;
    D3DPERF_BeginEvent(color, name);
}

static void D3DPerfEnd(bool enabled)
{
    if (!enabled)
        return;
    D3DPERF_EndEvent();
}

static bool ReadTextFileToBuffer(const char* path, std::vector<char>& out)
{
    out.clear();
    if (!path || !path[0])
        return false;

    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return false;
    }

    out.resize((size_t)size + 1);
    size_t got = fread(&out[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size)
    {
        out.clear();
        return false;
    }
    out[size] = '\0';
    return true;
}

static bool LoadShaderTextRelative(const char* relPath, std::vector<char>& out)
{
    const char* prefixes[] = { "", ".\\", "Scene3D\\", ".\\Scene3D\\", "..\\Scene3D\\" };
    char full[1024];
    for (int i = 0; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])); ++i)
    {
        sprintf_s(full, "%s%s", prefixes[i], relPath);
        if (ReadTextFileToBuffer(full, out))
            return true;
    }
    return false;
}

static void LogD3DXErrorBuffer(const char* prefix, ID3DXBuffer* errors)
{
    if (!prefix)
        prefix = "D3DX";
    if (errors && errors->GetBufferPointer())
    {
        const char* msg = (const char*)errors->GetBufferPointer();
        if (msg && msg[0])
        {
            char buf[512];
            sprintf_s(buf, "%s compile error: %s", prefix, msg);
            RendererLog(buf);
            return;
        }
    }
    RendererLog("D3DX compile error: (no message)");
}

static bool CompilePixelShaderFromFile(IDirect3DDevice9* device, const char* relPath,
                                       const char* entryPoint, const char* profile,
                                       IDirect3DPixelShader9** outShader)
{
    if (!device || !relPath || !outShader)
        return false;

    std::vector<char> src;
    if (!LoadShaderTextRelative(relPath, src))
        return false;

    ID3DXBuffer* shaderBuf = NULL;
    ID3DXBuffer* errorBuf = NULL;

    DWORD flags = D3DXSHADER_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags |= D3DXSHADER_DEBUG;
#endif

    HRESULT hr = D3DXCompileShader(&src[0], (UINT)(src.size() - 1),
                                   NULL, NULL,
                                   entryPoint ? entryPoint : "main",
                                   profile ? profile : "ps_2_0",
                                   flags, &shaderBuf, &errorBuf, NULL);

    if (FAILED(hr) || !shaderBuf)
    {
        LogD3DXErrorBuffer("D3DXCompileShader", errorBuf);
        if (errorBuf) errorBuf->Release();
        return false;
    }

    if (errorBuf) errorBuf->Release();

    IDirect3DPixelShader9* ps = NULL;
    hr = device->CreatePixelShader((const DWORD*)shaderBuf->GetBufferPointer(), &ps);
    shaderBuf->Release();

    if (FAILED(hr) || !ps)
        return false;

    *outShader = ps;
    return true;
}

static IDirect3DTexture9* CreateTextureFromFileExAInternal(IDirect3DDevice9* device, const char* path,
                                                           bool srgb, D3DCOLOR colorKey)
{
    if (!device || !path || !path[0])
        return NULL;

    DWORD filter = D3DX_DEFAULT;
    DWORD mipFilter = D3DX_DEFAULT;
    if (srgb)
    {
        filter |= D3DX_FILTER_SRGB;
        mipFilter |= D3DX_FILTER_SRGB;
    }

    IDirect3DTexture9* tex = NULL;
    HRESULT hr = D3DXCreateTextureFromFileExA(
        device,
        path,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        D3DX_DEFAULT,
        0,
        D3DFMT_UNKNOWN,
        D3DPOOL_MANAGED,
        filter,
        mipFilter,
        colorKey,
        NULL,
        NULL,
        &tex);

    if (FAILED(hr) || !tex)
        return NULL;
    return tex;
}

static bool LoadSurfaceFromMemoryInternal(IDirect3DSurface9* dst,
                                          const void* src,
                                          UINT srcPitch,
                                          D3DFORMAT srcFormat,
                                          UINT width,
                                          UINT height,
                                          D3DCOLOR colorKey)
{
    if (!dst || !src || width == 0 || height == 0)
        return false;

    RECT rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG)width;
    rect.bottom = (LONG)height;

    HRESULT hr = D3DXLoadSurfaceFromMemory(
        dst,
        NULL,
        NULL,
        src,
        srcFormat,
        srcPitch,
        NULL,
        &rect,
        D3DX_FILTER_NONE,
        colorKey);

    return SUCCEEDED(hr);
}

static bool PathIsDirA(const char* path)
{
    if (!path || !path[0])
        return false;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool PathIsFileA(const char* path)
{
    if (!path || !path[0])
        return false;
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool Scene3DRenderer::initD3D9AdapterInfo()
{
    if (m_d3d9)
    {
        m_d3d9->Release();
        m_d3d9 = NULL;
    }
    m_d3dAdapterValid = false;
    m_d3dAdapterIndex = D3DADAPTER_DEFAULT;
    m_d3dVendorId = 0;
    m_d3dAdapterDesc[0] = '\0';

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        RendererLog("Direct3DCreate9 failed (adapter probe)");
        return false;
    }

    UINT count = d3d->GetAdapterCount();
    HMONITOR target = m_hwnd ? MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
    UINT chosen = D3DADAPTER_DEFAULT;

    if (target && count > 0)
    {
        for (UINT i = 0; i < count; ++i)
        {
            if (d3d->GetAdapterMonitor(i) == target)
            {
                chosen = i;
                break;
            }
        }
    }

    D3DADAPTER_IDENTIFIER9 ident;
    ZeroMemory(&ident, sizeof(ident));
    if (SUCCEEDED(d3d->GetAdapterIdentifier(chosen, 0, &ident)))
    {
        m_d3dVendorId = ident.VendorId;
        strncpy_s(m_d3dAdapterDesc, ident.Description, _TRUNCATE);
    }

    {
        char msg[256];
        if (m_d3dAdapterDesc[0])
            sprintf_s(msg, "D3D9 adapter %u/%u: %s", chosen, (count > 0 ? count - 1 : 0), m_d3dAdapterDesc);
        else
            sprintf_s(msg, "D3D9 adapter %u/%u selected", chosen, (count > 0 ? count - 1 : 0));
        RendererLog(msg);
    }

    D3DDISPLAYMODE mode;
    if (SUCCEEDED(d3d->GetAdapterDisplayMode(chosen, &mode)))
    {
        char msg[256];
        sprintf_s(msg, "D3D9 display mode: %ux%u @ %u Hz (fmt=%u)",
                  mode.Width, mode.Height, mode.RefreshRate, (unsigned int)mode.Format);
        RendererLog(msg);
    }

    D3DCAPS9 caps;
    if (SUCCEEDED(d3d->GetDeviceCaps(chosen, D3DDEVTYPE_HAL, &caps)))
    {
        unsigned int vs = caps.VertexShaderVersion;
        unsigned int ps = caps.PixelShaderVersion;
        char msg[256];
        sprintf_s(msg, "D3D9 caps: VS %u.%u PS %u.%u",
                  (vs >> 8) & 0xFF, vs & 0xFF, (ps >> 8) & 0xFF, ps & 0xFF);
        RendererLog(msg);
    }

    m_d3d9 = d3d;
    m_d3dAdapterIndex = chosen;
    m_d3dAdapterValid = true;

    // Mirror game behavior: pick a packed shader root based on vendor ID.
    // 0x10DE = NVIDIA, 0x1002 = ATI/AMD, else generic.
    const char* desiredRoot = "GameFiles\\lotrcparser\\Shaders_PC_generic";
    const char* desiredBin = "Shaders_PC_generic.bin";
    if (m_d3dVendorId == 0x10DE)
    {
        desiredRoot = "GameFiles\\lotrcparser\\Shaders_PC_nvidia";
        desiredBin = "Shaders_PC_nvidia.bin";
    }
    else if (m_d3dVendorId == 0x1002)
    {
        desiredRoot = "GameFiles\\lotrcparser\\Shaders_PC_ati";
        desiredBin = "Shaders_PC_ati.bin";
    }

    if (PathIsDirA(desiredRoot))
    {
        strcpy_s(m_packedShaderRoot, desiredRoot);
    }
    else
    {
        // Fallback to nvidia folder if available (only one present in repo).
        if (PathIsDirA("GameFiles\\lotrcparser\\Shaders_PC_nvidia"))
            strcpy_s(m_packedShaderRoot, "GameFiles\\lotrcparser\\Shaders_PC_nvidia");
        else
            strcpy_s(m_packedShaderRoot, desiredRoot);
    }

    {
        char msg[256];
        sprintf_s(msg, "Packed shader root: %s", m_packedShaderRoot);
        RendererLog(msg);
    }

    // Probe packed shader bin file (if present) for visibility in logs.
    char binPath[512];
    sprintf_s(binPath, "GameFiles\\lotrcparser\\%s", desiredBin);
    if (!PathIsFileA(binPath))
        sprintf_s(binPath, "GameFiles\\%s", desiredBin);
    if (!PathIsFileA(binPath))
        sprintf_s(binPath, "%s", desiredBin);
    if (!PathIsFileA(binPath))
        sprintf_s(binPath, "Scene3D\\%s", desiredBin);
    if (PathIsFileA(binPath))
    {
        char msg[256];
        sprintf_s(msg, "Found packed shader bin: %s", binPath);
        RendererLog(msg);
    }
    else
    {
        char msg[256];
        sprintf_s(msg, "Packed shader bin not found: %s", desiredBin);
        RendererLog(msg);
    }
    return true;
}
#include <map>
#include <windows.h>
#include <algorithm>
#include <limits.h>

// Havok DX9-specific context type lives in hkgDx9.lib and exposes a getDevice() helper.
// The SDK headers we have in-tree don't declare it, so we forward-declare the minimum we need.
class hkgDisplayContextDX9 : public hkgDisplayContext
{
public:
    IDirect3DDevice9* getDevice();
};

// Hook: ImGui overlay pass (implemented in ZeroEngine3DViewport.cpp)
extern void RenderImGuiOverlayPass();
extern void ImGui_PreReset();
extern void ImGui_PostReset();

// Default JSON animation for rig testing
static const char* kDefaultJsonAnimPath = "../GameFiles/animations/RH6_swd_loc_idle.json";

// Constructor
Scene3DRenderer::Scene3DRenderer()
    : m_hwnd(NULL)
    , m_windowWidth(0)
    , m_windowHeight(0)
    , m_window(HK_NULL)
    , m_context(HK_NULL)
    , m_displayWorld(HK_NULL)
    , m_camera(HK_NULL)
    , m_lightManager(HK_NULL)
    , m_loader(HK_NULL)
    , m_converter(HK_NULL)
    , m_animatedCharacter(HK_NULL)
    , m_animationTime(0.0f)
    , m_jsonAnim(NULL)
    , m_jsonBlendAnim(NULL)
    , m_animGraph(NULL)
    , m_jsonAnimTime(0.0f)
    , m_lastEventTime(0.0f)
    , m_lastEventIndex(0)
    , m_useJsonAnim(false)
    , m_useJsonBlendAnim(false)
    , m_jsonDecodeMode(0)
    , m_type2PackingMode(0)
    , m_logType2(false)
    , m_lastType2LogFrame(-1)
    , m_rotAxisMode(0)
    , m_rotSignMask(0)
    , m_rotApplyMode(ROT_REF_DELTA)
    , m_applyPoseCorrection(false)
    , m_forceReferencePose(false)
    , m_jsonAnimPaused(false)
    , m_jsonBlendEnabled(false)
    , m_loopAnimation(true)
    , m_playbackSpeed(1.0f)
    , m_animFixedStepEnabled(false)
    , m_animFixedStep(1.0f / 30.0f)
    , m_animFixedAccumulator(0.0f)
    , m_animFixedMaxSteps(8)
    , m_animFixedStepsLast(0)
    , m_timeWarpEnabled(false)
    , m_timeWarpEasingType(EASING_LINEAR)
    , m_timeWarpCp1x(0.0f)
    , m_timeWarpCp1y(0.0f)
    , m_timeWarpCp2x(1.0f)
    , m_timeWarpCp2y(1.0f)
    , m_jsonBlendMode(JSON_BLEND_CROSSFADE)
    , m_jsonBlendRotMode(JSON_BLEND_ROT_SLERP)
    , m_rotInterpMode(ROT_INTERP_SLERP)
    , m_jsonBlendAlpha(0.0f)
    , m_jsonBlendLayerRootBone(-1)
    , m_jsonBlendAdditiveRefValid(false)
    , m_motionMatchEnabled(false)
    , m_motionMatchDatabaseValid(false)
    , m_motionMatchSearchInterval(0.15f)
    , m_motionMatchSearchTimer(0.0f)
    , m_motionMatchBlendDuration(0.12f)
    , m_motionMatchBlendTime(0.0f)
    , m_motionMatchBlendActive(false)
    , m_motionMatchRequestBlend(false)
    , m_motionMatchWeightVel(1.0f)
    , m_motionMatchWeightFacing(0.5f)
    , m_motionMatchWeightFeet(1.5f)
    , m_motionMatchWeightTraj(0.75f)
    , m_motionMatchTrajT1(0.2f)
    , m_motionMatchTrajT2(0.4f)
    , m_motionMatchTrajT3(0.6f)
    , m_motionMatchLastScore(0.0f)
    , m_motionMatchCurrentFrameIndex(-1)
    , m_motionMatchCurrentClipIndex(-1)
    , m_motionMatchTime(0.0f)
    , m_motionMatchPrevTime(0.0f)
    , m_motionMatchHasLastFeature(false)
    , m_motionMatchHasLastPose(false)
    , m_ikEnabled(true)
    , m_footIkEnabled(true)
    , m_lookAtEnabled(false)
    , m_aimEnabled(false)
    , m_lookAtBoneIndex(-1)
    , m_aimBoneIndex(-1)
    , m_lookAtWeight(1.0f)
    , m_aimWeight(1.0f)
    , m_physicalAnimEnabled(false)
    , m_ragdollEnabled(false)
    , m_ragdollBlend(0.0f)
    , m_physPosStiffness(120.0f)
    , m_physPosDamping(18.0f)
    , m_physRotStiffness(40.0f)
    , m_physRotDamping(6.0f)
    , m_lastAnimDeltaTime(1.0f / 60.0f)
    , m_rootMotionMode(ROOT_MOTION_FULL)
    , m_groundClampMode(GROUND_CLAMP_SNAP)
    , m_groundOffsetY(0.0f)
    , m_groundContactEps(0.03f)
    , m_groundReleaseHeight(0.35f)
    , m_groundSmoothFactor(0.35f)
    , m_groundFillEnabled(true)
    , m_groundTexture(NULL)
    , m_groundTextureD3D(NULL)
    , m_groundSize(60.0f)
    , m_groundTextureRepeat(8.0f)
    , m_groundHeight(0.0f)
    , m_groundModel(NULL)
    , m_groundModelVisible(true)
    , m_groundModelOffsetY(0.0f)
    , m_gammaEnabled(true)
    , m_maxAnisotropy(8)
    , m_mipBias(-0.5f)
    , m_d3dPerfMarkersEnabled(false)
    , m_rimLightEnabled(false)
    , m_cameraRoll(0.0f)
    , m_horizonLock(true)
    , m_cameraOrtho(false)
    , m_navMode(1)
    , m_debugMeshMinY(0.0f)
    , m_debugFootMinY(0.0f)
    , m_modelBaseOffsetY(0.0f)
    , m_modelBaseComputed(false)
    , m_selectedBoneIndex(-1)
    , m_editorRecording(false)
    , m_editorDragging(false)
    , m_editorDraggingTrans(false)
    , m_editorRotateAxisLock(AXIS_FREE)
    , m_editorTranslateAxisLock(AXIS_FREE)
    , m_gizmoMode(GIZMO_ROTATE)
    , m_gizmoSpace(GIZMO_LOCAL)
    , m_rotateSnapEnabled(true)
    , m_moveSnapEnabled(true)
    , m_rotateSnapDegrees(15.0f)
    , m_moveSnapUnits(0.05f)
    , m_editorInterpolationMode(INTERP_HOLD)
    , m_editorDefaultEasingType(EASING_LINEAR)
    , m_editorDefaultEasingCp1x(0.0f)
    , m_editorDefaultEasingCp1y(0.0f)
    , m_editorDefaultEasingCp2x(1.0f)
    , m_editorDefaultEasingCp2y(1.0f)
    , m_rotateSnapAccum(0.0f)
    , m_rotateSnapApplied(0.0f)
    , m_rotateSnapAxis(0)
    , m_cameraFovDegrees(60.0f)
    , m_gizmoCacheValid(false)
    , m_gizmoCacheScale(0.5f)
    , m_editorFrameTime(1.0f / 30.0f)
    , m_editorTimelineDuration(0.0f)
    , m_rootMotionPrevTime(0.0f)
    , m_rootMotionPrevValid(false)
    , m_rootMotionWarpEnabled(false)
    , m_rootMotionWarpMode(ROOT_WARP_NONE)
    , m_compressAuto(false)
    , m_compressQuantizeRot(false)
    , m_compressStripRot(false)
    , m_compressPosTol(0.0015f)
    , m_compressRotTolDeg(0.25f)
    , m_compressScaleTol(0.0010f)
    , m_compressRootTol(0.0020f)
    , m_compressStatsValid(false)
    , m_compressRotBefore(0)
    , m_compressRotAfter(0)
    , m_compressTransBefore(0)
    , m_compressTransAfter(0)
    , m_compressScaleBefore(0)
    , m_compressScaleAfter(0)
    , m_compressRootBefore(0)
    , m_compressRootAfter(0)
    , m_gameModel(NULL)
    , m_activeSkyboxIndex(-1)
    , m_skyboxEnabled(true)
    , m_skyRenderMode(SKY_BACKDROP)
    , m_cloudLayerEnabled(false)
    , m_cloudSkyboxIndex(-1)
    , m_d3dDevice(NULL)
    , m_d3d9(NULL)
    , m_d3dAdapterIndex(0)
    , m_d3dAdapterValid(false)
    , m_d3dVendorId(0)
    , m_strictRigCoverage(true)
    , m_scene3dEnabled(false)
    , m_imguiViewportActive(false)
    , m_imguiViewportW(0)
    , m_imguiViewportH(0)
    , m_imguiViewportTex(NULL)
    , m_imguiViewportSurf(NULL)
    , m_imguiViewportDepth(NULL)
    , m_imguiViewportTexW(0)
    , m_imguiViewportTexH(0)
    , m_refractionSceneTex(NULL)
    , m_refractionSceneSurf(NULL)
    , m_refractionSceneW(0)
    , m_refractionSceneH(0)
    , m_refractionSceneFmt(D3DFMT_UNKNOWN)
    , m_refractionPS(NULL)
    , m_refractionWhiteTex(NULL)
    , m_usePackedParticleShaders(true)
    , m_packedParticleShaders(NULL)
    , m_levelScene(NULL)
    , m_effectManager(NULL)
    , m_cameraDistance(10.0f)
    , m_cameraYaw(0.0f)
    , m_cameraPitch(0.3f)
    , m_lastMouseX(0)
    , m_lastMouseY(0)
    , m_initialized(false)
{
    // Fly camera state (at end of class, init in body to match declaration order)
    m_flyCameraActive = false;
    m_flyCamPos[0] = m_flyCamPos[1] = m_flyCamPos[2] = 0.0f;
    m_flyCamYaw = 0.0f;
    m_flyCamPitch = 0.0f;
    m_flyCamSpeed = 200.0f;
    m_physicsActive = false;
    m_physWorld = NULL;
    m_physCharProxy = NULL;
    m_physCharCtx = NULL;
    m_physGroundY = 0.0f;
    m_physAccum = 0.0f;
    m_physLevelVerts = NULL;
    m_physLevelIndices = NULL;
    m_physLevelVertCount = 0;
    m_physLevelIdxCount = 0;
    m_playerModel = NULL;
    m_playerAnimChar = NULL;
    m_playerYaw = 0.0f;
    m_tpCamDistance = 5.0f;
    m_tpCamHeight = 2.5f;
    m_playerAnimState = PLAYER_ANIM_IDLE;
    m_playerAnimTime = 0.0f;
    m_playerAnimBlend = 0.0f;
    m_playerAnimBlendTime = 0.0f;
    m_playerAnimBlendDur = 0.2f;
    m_playerAnimPrevState = PLAYER_ANIM_IDLE;
    m_playerAnimPrevTime = 0.0f;
    m_playerSpeed = 0.0f;
    m_playerClipIdle = NULL;
    m_playerClipWalk = NULL;
    m_playerClipRun = NULL;
    m_cameraTarget.setZero4();
    m_lookAtTarget.set(0.0f, 0.0f, 1.0f);
    m_aimTarget.set(0.0f, 0.0f, 1.0f);
    m_d3dAdapterDesc[0] = '\0';
    strcpy_s(m_packedShaderRoot, "GameFiles\\lotrcparser\\Shaders_PC_nvidia");
    m_ragdollGravity.set(0.0f, -9.81f, 0.0f);
    m_rootMotionOffset.setZero4();
    m_rootMotionPrevPos.setZero4();
    m_rootMotionWarpTarget.setZero4();
    m_gizmoCachePos.setZero4();
    m_gizmoCacheAxisX.set(1.0f, 0.0f, 0.0f);
    m_gizmoCacheAxisY.set(0.0f, 1.0f, 0.0f);
    m_gizmoCacheAxisZ.set(0.0f, 0.0f, 1.0f);
    m_moveSnapAccum[0] = m_moveSnapAccum[1] = m_moveSnapAccum[2] = 0.0f;
    m_moveSnapApplied[0] = m_moveSnapApplied[1] = m_moveSnapApplied[2] = 0.0f;
    m_moveSnapAxis = 0;
    m_jsonAnimPath[0] = '\0';
    m_jsonBlendAnimPath[0] = '\0';
    m_animGraphPath[0] = '\0';
    m_groundTexturePath[0] = '\0';
    m_groundJPath[0] = '\0';
    m_groundGlbPath[0] = '\0';
    m_groundTexDir[0] = '\0';
    for (int i = 0; i < 5; ++i) { m_poseLibrary[i].valid = false; m_poseLibrary[i].name[0] = '\0'; }
    m_rootMotionLock[0] = m_rootMotionLock[1] = m_rootMotionLock[2] = false;
    m_graphRngState = 0x12345678u;
    for (int i = 0; i < 64; ++i) { m_creatureVars[i] = 0.0f; }
    for (int i = 0; i < 32; ++i) { m_inputButtonState[i] = 0; m_inputButtonDown[i] = false; }
    strcpy_s(m_navModeLabels[1], sizeof(m_navModeLabels[1]), "Maya");
    strcpy_s(m_navModeLabels[2], sizeof(m_navModeLabels[2]), "Blender");
    strcpy_s(m_navModeLabels[3], sizeof(m_navModeLabels[3]), "Unreal");
    for (int i = 0; i < 5; ++i)
    {
        m_camBookmarks[i].valid = false;
        m_camBookmarks[i].target.setZero4();
        m_camBookmarks[i].yaw = 0.0f;
        m_camBookmarks[i].pitch = 0.0f;
        m_camBookmarks[i].roll = 0.0f;
        m_camBookmarks[i].distance = 6.0f;
    }
    m_undoStack.clear();
    m_redoStack.clear();
    m_showRootTrail = true;
    m_motionMatchTargetVel.setZero4();
    m_motionMatchTargetFacing.set(0.0f, 0.0f, 1.0f, 0.0f);
    memset(&m_motionMatchLastFeature, 0, sizeof(m_motionMatchLastFeature));
}

// Destructor
Scene3DRenderer::~Scene3DRenderer()
{
    shutdown();
}

// Debug log
static FILE* g_logFile = NULL;
void RendererLog(const char* msg) {
    if (!g_logFile) g_logFile = fopen("renderer.log", "w");
    if (g_logFile) { fprintf(g_logFile, "%s\n", msg); fflush(g_logFile); }
}

static bool ContainsNoCase(const char* haystack, const char* needle)
{
    if (!haystack || !needle || needle[0] == '\0')
        return false;
    size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p)
    {
        if (_strnicmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

static bool IsFootBoneName(const char* name)
{
    if (!name) return false;
    return ContainsNoCase(name, "Foot") ||
           ContainsNoCase(name, "Toe") ||
           ContainsNoCase(name, "Ankle") ||
           ContainsNoCase(name, "Paw") ||
           ContainsNoCase(name, "Hoof");
}

static bool IsLikelyFxTranslucentName(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    return ContainsNoCase(name, "fx") ||
           ContainsNoCase(name, "vfx") ||
           ContainsNoCase(name, "smoke") ||
           ContainsNoCase(name, "cloud") ||
           ContainsNoCase(name, "mist") ||
           ContainsNoCase(name, "fog") ||
           ContainsNoCase(name, "fire") ||
           ContainsNoCase(name, "flame") ||
           ContainsNoCase(name, "glow") ||
           ContainsNoCase(name, "aura") ||
           ContainsNoCase(name, "trail") ||
           ContainsNoCase(name, "sprite") ||
           ContainsNoCase(name, "particle") ||
           ContainsNoCase(name, "glass") ||
           ContainsNoCase(name, "water") ||
           ContainsNoCase(name, "window") ||
           ContainsNoCase(name, "ghost") ||
           ContainsNoCase(name, "beam") ||
           ContainsNoCase(name, "light");
}

static bool IsLikelyOpaqueCharacterName(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    return ContainsNoCase(name, "head") ||
           ContainsNoCase(name, "face") ||
           ContainsNoCase(name, "skin") ||
           ContainsNoCase(name, "body") ||
           ContainsNoCase(name, "cloth") ||
           ContainsNoCase(name, "cape") ||
           ContainsNoCase(name, "metal") ||
           ContainsNoCase(name, "armor") ||
           ContainsNoCase(name, "helmet") ||
           ContainsNoCase(name, "hand") ||
           ContainsNoCase(name, "arm") ||
           ContainsNoCase(name, "leg") ||
           ContainsNoCase(name, "foot") ||
           ContainsNoCase(name, "boot") ||
           ContainsNoCase(name, "belt") ||
           ContainsNoCase(name, "weapon") ||
           ContainsNoCase(name, "sword") ||
           ContainsNoCase(name, "shield") ||
           ContainsNoCase(name, "horse") ||
           ContainsNoCase(name, "rider");
}

static bool IsLikelyCutoutCharacterName(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    return ContainsNoCase(name, "hair") ||
           ContainsNoCase(name, "beard") ||
           ContainsNoCase(name, "lash") ||
           ContainsNoCase(name, "brow") ||
           ContainsNoCase(name, "fur") ||
           ContainsNoCase(name, "feather");
}

static bool ShouldRenderAsTransparentPart(const GameModel::MeshPart& part)
{
    if (!part.diffuseTexture || !part.diffuseTexture->isTransparent())
    {
        return false;
    }

    // Most game character/body textures are stored in alpha-capable DDS formats
    // but are meant for opaque rendering. Only route known FX-like materials to blend.
    if (IsLikelyFxTranslucentName(part.materialName) || IsLikelyFxTranslucentName(part.diffuseName))
    {
        return true;
    }

    // Character cutouts (hair/lashes/fur cards) need alpha blending.
    if (IsLikelyCutoutCharacterName(part.materialName) || IsLikelyCutoutCharacterName(part.diffuseName))
    {
        return true;
    }

    if (IsLikelyOpaqueCharacterName(part.materialName) || IsLikelyOpaqueCharacterName(part.diffuseName))
    {
        return false;
    }

    // Conservative default: keep opaque to avoid face/skin sorting artifacts.
    return false;
}

static void ExtractBaseNameNoExt(const char* path, char* out, int outSize)
{
    if (!out || outSize <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (!path || path[0] == '\0')
    {
        return;
    }

    const char* base = strrchr(path, '\\');
    if (!base) base = strrchr(path, '/');
    base = base ? (base + 1) : path;

    char tmp[256];
    strcpy_s(tmp, sizeof(tmp), base);
    char* dot = strrchr(tmp, '.');
    if (dot) *dot = '\0';
    strcpy_s(out, outSize, tmp);
}

static void ExtractRigFamilyToken(const char* name, char* out, int outSize)
{
    if (!out || outSize <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (!name || name[0] == '\0')
    {
        return;
    }

    char base[256];
    strcpy_s(base, sizeof(base), name);

    // CH_gdr_..., CH_bal_..., CH_hrs_...
    if (_strnicmp(base, "CH_", 3) == 0)
    {
        const char* p = base + 3;
        const char* us = strchr(p, '_');
        if (us)
        {
            int len = (int)(us - base);
            if (len >= outSize) len = outSize - 1;
            memcpy(out, base, len);
            out[len] = '\0';
            return;
        }
    }

    // RH6_..., RH3_...
    if ((_strnicmp(base, "RH", 2) == 0) && isdigit((unsigned char)base[2]))
    {
        const char* us = strchr(base, '_');
        int len = us ? (int)(us - base) : (int)strlen(base);
        if (len >= outSize) len = outSize - 1;
        memcpy(out, base, len);
        out[len] = '\0';
        return;
    }

    const char* us = strchr(base, '_');
    int len = us ? (int)(us - base) : (int)strlen(base);
    if (len >= outSize) len = outSize - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

// Initialize renderer
// Based on: hkDefaultPhysicsDemo constructor and Havok demo framework
bool Scene3DRenderer::initialize(HWND hwnd, int width, int height)
{
    RendererLog("=== Scene3DRenderer::initialize() ===");
    m_hwnd = hwnd;
    m_windowWidth = width;
    m_windowHeight = height;

    // Probe D3D9 adapter info (matches render dumps that select adapter by monitor)
    initD3D9AdapterInfo();

    // Step 1: Initialize Havok Graphics System
    // Use "d3d9" (same as demo) instead of "dx9"
    RendererLog("Calling hkgSystem::init(\"d3d9\")...");
    hkgSystem::init("d3d9");
    RendererLog("hkgSystem initialized");

    // Step 2: Create Havok Graphics Window
    // hkgWindow::create is a function pointer set by hkgSystem::init()
    // Check if it was properly initialized, otherwise fall back to direct call
    RendererLog("Creating hkgWindow...");

    if (hkgWindow::create != HK_NULL) {
        RendererLog("Using hkgWindow::create() factory method");
        m_window = hkgWindow::create();
    } else {
        RendererLog("WARNING: hkgWindow::create is NULL! Using direct hkgWindowDX9PC::createWindowDX9PC()");
        m_window = hkgWindowDX9PC::createWindowDX9PC();
    }

    if (!m_window) {
        RendererLog("ERROR: Window creation returned NULL!");
        return false;
    }
    RendererLog("hkgWindow created successfully");

    // Step 3: Initialize the window - use the application's existing window
    RendererLog("Initializing hkgWindow with application's HWND...");

    // Match the exact flags from the working demo (hkDemoFramework.cpp line 1227)
    HKG_WINDOW_CREATE_FLAG flags = HKG_WINDOW_PRIMARY_DEVICE | HKG_WINDOW_WINDOWED;

    // Match the exact buffers from the demo: COLOR | DEPTH32 (NO STENCIL!)
    HKG_WINDOW_BUFFERS buffers = HKG_WINDOW_BUF_COLOR | HKG_WINDOW_BUF_DEPTH32;

    RendererLog("Flags: PRIMARY_DEVICE | WINDOWED");
    RendererLog("Buffers: COLOR | DEPTH32 (no stencil)");

    // Pass the application's HWND so Havok uses our existing window
    bool initResult = m_window->initialize(flags, buffers, width, height, "ZeroEngine 3D Viewport", m_hwnd, HK_NULL);

    if (!initResult) {
        RendererLog("ERROR: hkgWindow::initialize() returned false!");
        RendererLog("This usually means:");
        RendererLog("  1. DirectX 9.0c runtime is not installed");
        RendererLog("  2. Graphics driver doesn't support DirectX 9");
        RendererLog("  3. d3d9.dll is missing or incompatible");
        RendererLog("");
        RendererLog("SOLUTION: Install DirectX End-User Runtime from:");
        RendererLog("  https://www.microsoft.com/en-us/download/details.aspx?id=35");
        m_window->removeReference();
        m_window = HK_NULL;
        return false;
    }
    RendererLog("hkgWindow initialized successfully!");

    // Step 4: Get the display context
    m_context = m_window->getContext();
    if (!m_context) {
        RendererLog("ERROR: Failed to get display context!");
        m_window->removeReference();
        m_window = HK_NULL;
        return false;
    }
    RendererLog("Got display context");
    // Improve texture sampling quality globally (used when textures request anisotropic filtering).
    m_context->setMaxAnisotropy((hkUint8)m_maxAnisotropy);

    // Capture the underlying D3D9 device for overlays (ImGui) and optional texture helpers.
    // Havok's public headers don't expose it, but hkgDx9 provides hkgDisplayContextDX9::getDevice().
    m_d3dDevice = NULL;
    {
        hkgDisplayContextDX9* dx9 = static_cast<hkgDisplayContextDX9*>(m_context);
        IDirect3DDevice9* dev = dx9 ? dx9->getDevice() : NULL;
        if (dev)
        {
            m_d3dDevice = dev;
            RendererLog("Captured IDirect3DDevice9 from Havok hkgDisplayContextDX9");
        }
        else
        {
            RendererLog("WARNING: Failed to capture IDirect3DDevice9 (overlay UI will be disabled)");
        }
    }

    // Step 5: Create display world
    m_displayWorld = hkgDisplayWorld::createDisplayWorld();
    if (!m_displayWorld) {
        RendererLog("ERROR: Failed to create display world!");
        m_window->removeReference();
        m_window = HK_NULL;
        return false;
    }
    RendererLog("Created display world");

    // Apply default lighting and render settings
    setupDefaultLighting();
    setGammaEnabled(m_gammaEnabled);
    setAnisotropy(m_maxAnisotropy);
    setMipBias(m_mipBias);

    // Step 6: Create camera
    m_camera = hkgCamera::create();
    if (!m_camera) {
        RendererLog("ERROR: Failed to create camera!");
        m_displayWorld->removeReference();
        m_displayWorld = HK_NULL;
        m_window->removeReference();
        m_window = HK_NULL;
        return false;
    }

    // Setup camera
    m_camera->setProjectionMode(HKG_CAMERA_PERSPECTIVE);
    m_camera->setFOV(60.0f);
    m_camera->setNear(1.0f);
    m_camera->setFar(100000.0f);
    m_camera->setAspect((float)width / (float)height);
    RendererLog("Created and configured camera");

    // Step 7: Create light manager
    m_lightManager = hkgLightManager::create();
    setupDefaultLighting();
    RendererLog("Created light manager");

    // Step 8: Create scene loader
    m_loader = new Scene3DLoader();

    // Step 9: Create converter
    m_converter = new HavokToDisplayConverter();
    m_converter->initialize(m_context, m_displayWorld);
    RendererLog("Created scene loader and converter");

    // Step 10: Create effect manager
    m_effectManager = new EffectManager();
    m_effectManager->initialize(m_context);
    RendererLog("Created effect manager");

    RendererLog("Renderer initialized successfully!");
    m_initialized = true;
    return true;
}

// Shutdown renderer
void Scene3DRenderer::shutdown()
{
    shutdownPhysicsPlaytest();
    releaseImGuiViewportResources();
    releaseRefractionResources();
    if (m_groundTextureD3D)
    {
        m_groundTextureD3D->Release();
        m_groundTextureD3D = NULL;
    }
    if (m_packedParticleShaders)
    {
        delete m_packedParticleShaders;
        m_packedParticleShaders = NULL;
    }

    if (m_jsonAnim)
    {
        delete m_jsonAnim;
        m_jsonAnim = NULL;
    }
    m_useJsonAnim = false;
    m_jsonAnimTime = 0.0f;
    clearAnimationGraph();
    clearMotionMatchDatabase();

    if (m_jsonBlendAnim)
    {
        delete m_jsonBlendAnim;
        m_jsonBlendAnim = NULL;
    }
    m_useJsonBlendAnim = false;
    m_jsonBlendEnabled = false;
    m_jsonBlendAlpha = 0.0f;
    m_jsonBlendAnimPath[0] = '\0';
    m_jsonBlendMask.clear();
    m_jsonBlendAdditiveRefLocal.clear();
    m_jsonBlendAdditiveRefValid = false;
    m_loopRegionEnabled = false;
    m_loopRegionIn = 0.0f;
    m_loopRegionOut = 0.0f;
    m_blendAutoNormalize = false; // off by default — exclusive layers need full weight
    m_blendWeightDamp = 4.0f;
    m_rootPathCount = 0;
    m_rootPathHead = 0;
    m_rootPathEnabled = false;
    m_poseSnapshotValid = false;
    m_poseSnapshotBlendActive = false;
    m_poseSnapshotBlendAlpha = 1.0f;
    // BlendLayer default constructor handles init
    m_jsonBlendTime = 0.0f;
    m_jsonBlendIndependentTime = true;
    m_jsonBlendLoopBlendClip = true;
    m_jsonBlendFadeDuration = 0.0f;
    m_jsonBlendFadeElapsed = 0.0f;
    m_jsonBlendFadingIn = false;
    m_jsonBlendFadeEasing = 0; // Linear

    if (m_animatedCharacter) {
        delete m_animatedCharacter;
        m_animatedCharacter = HK_NULL;
    }
    clearSkyboxes();
    if (m_gameModel)
    {
        m_gameModel->release();
        delete m_gameModel;
        m_gameModel = NULL;
    }

    if (m_converter) {
        delete m_converter;
        m_converter = HK_NULL;
    }

    if (m_loader) {
        delete m_loader;
        m_loader = HK_NULL;
    }

    if (m_levelScene) {
        delete m_levelScene;
        m_levelScene = NULL;
    }

    if (m_effectManager) {
        delete m_effectManager;
        m_effectManager = NULL;
    }

    if (m_lightManager) {
        m_lightManager->removeReference();
        m_lightManager = HK_NULL;
    }

    if (m_camera) {
        m_camera->removeReference();
        m_camera = HK_NULL;
    }

    if (m_d3d9)
    {
        m_d3d9->Release();
        m_d3d9 = NULL;
    }
    m_d3dAdapterValid = false;
    m_d3dAdapterIndex = D3DADAPTER_DEFAULT;
    m_d3dAdapterDesc[0] = '\0';

    if (m_displayWorld) {
        m_displayWorld->removeReference();
        m_displayWorld = HK_NULL;
    }

    clearGroundModel();

    if (m_groundTexture)
    {
        m_groundTexture->removeReference();
        m_groundTexture = NULL;
    }
    if (m_groundTextureD3D)
    {
        m_groundTextureD3D->Release();
        m_groundTextureD3D = NULL;
    }

    // Context is owned by window, don't release it separately
    m_context = HK_NULL;

    if (m_window) {
        m_window->removeReference();
        m_window = HK_NULL;
    }

    m_initialized = false;
}

void Scene3DRenderer::releaseImGuiViewportResources()
{
    if (m_imguiViewportDepth)
    {
        m_imguiViewportDepth->Release();
        m_imguiViewportDepth = NULL;
    }
    if (m_imguiViewportSurf)
    {
        m_imguiViewportSurf->Release();
        m_imguiViewportSurf = NULL;
    }
    if (m_imguiViewportTex)
    {
        m_imguiViewportTex->Release();
        m_imguiViewportTex = NULL;
    }
    m_imguiViewportTexW = 0;
    m_imguiViewportTexH = 0;
}

bool Scene3DRenderer::ensureImGuiViewportResources(int desiredW, int desiredH)
{
    if (!m_d3dDevice)
    {
        return false;
    }
    if (desiredW < 1) desiredW = 1;
    if (desiredH < 1) desiredH = 1;

    // Keep the existing RT if it's already large enough.
    if (m_imguiViewportTex && m_imguiViewportSurf && m_imguiViewportDepth &&
        desiredW <= m_imguiViewportTexW && desiredH <= m_imguiViewportTexH)
    {
        return true;
    }

    releaseImGuiViewportResources();

    HRESULT hr = m_d3dDevice->CreateTexture(
        (UINT)desiredW, (UINT)desiredH,
        1,
        D3DUSAGE_RENDERTARGET,
        D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT,
        &m_imguiViewportTex,
        NULL);
    if (FAILED(hr) || !m_imguiViewportTex)
    {
        RendererLog("ensureImGuiViewportResources: CreateTexture failed");
        releaseImGuiViewportResources();
        return false;
    }

    hr = m_imguiViewportTex->GetSurfaceLevel(0, &m_imguiViewportSurf);
    if (FAILED(hr) || !m_imguiViewportSurf)
    {
        RendererLog("ensureImGuiViewportResources: GetSurfaceLevel failed");
        releaseImGuiViewportResources();
        return false;
    }

    // Match the current depth buffer format where possible.
    // Note: render-target textures cannot be multisampled in D3D9, so the depth surface must use
    // D3DMULTISAMPLE_NONE to be compatible with the RT texture surface.
    D3DFORMAT depthFmt = D3DFMT_D24X8;
    IDirect3DSurface9* backDepth = NULL;
    if (SUCCEEDED(m_d3dDevice->GetDepthStencilSurface(&backDepth)) && backDepth)
    {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(backDepth->GetDesc(&desc)))
        {
            depthFmt = desc.Format;
        }
        backDepth->Release();
    }

    hr = m_d3dDevice->CreateDepthStencilSurface(
        (UINT)desiredW, (UINT)desiredH,
        depthFmt,
        D3DMULTISAMPLE_NONE,
        0,
        TRUE,
        &m_imguiViewportDepth,
        NULL);
    if (FAILED(hr) || !m_imguiViewportDepth)
    {
        // Fallback: common format
        hr = m_d3dDevice->CreateDepthStencilSurface(
            (UINT)desiredW, (UINT)desiredH,
            D3DFMT_D24X8,
            D3DMULTISAMPLE_NONE,
            0,
            TRUE,
            &m_imguiViewportDepth,
            NULL);
    }
    if (FAILED(hr) || !m_imguiViewportDepth)
    {
        RendererLog("ensureImGuiViewportResources: CreateDepthStencilSurface failed");
        releaseImGuiViewportResources();
        return false;
    }

    m_imguiViewportTexW = desiredW;
    m_imguiViewportTexH = desiredH;
    return true;
}

void Scene3DRenderer::releaseRefractionResources()
{
    if (m_refractionPS)
    {
        m_refractionPS->Release();
        m_refractionPS = NULL;
    }
    if (m_refractionWhiteTex)
    {
        m_refractionWhiteTex->Release();
        m_refractionWhiteTex = NULL;
    }
    if (m_refractionSceneSurf)
    {
        m_refractionSceneSurf->Release();
        m_refractionSceneSurf = NULL;
    }
    if (m_refractionSceneTex)
    {
        m_refractionSceneTex->Release();
        m_refractionSceneTex = NULL;
    }
    m_refractionSceneW = 0;
    m_refractionSceneH = 0;
    m_refractionSceneFmt = D3DFMT_UNKNOWN;
}

bool Scene3DRenderer::ensureRefractionSceneCopyResources(int desiredW, int desiredH, int desiredFmtRaw)
{
    if (!m_d3dDevice)
        return false;

    if (desiredW < 1) desiredW = 1;
    if (desiredH < 1) desiredH = 1;
    D3DFORMAT desiredFmt = (D3DFORMAT)desiredFmtRaw;
    if (desiredFmt == D3DFMT_UNKNOWN)
        desiredFmt = D3DFMT_A8R8G8B8;

    // Keep the existing RT if it's already the right size.
    if (m_refractionSceneTex && m_refractionSceneSurf &&
        desiredW == m_refractionSceneW && desiredH == m_refractionSceneH &&
        (int)desiredFmt == m_refractionSceneFmt)
    {
        return true;
    }

    if (m_refractionSceneSurf)
    {
        m_refractionSceneSurf->Release();
        m_refractionSceneSurf = NULL;
    }
    if (m_refractionSceneTex)
    {
        m_refractionSceneTex->Release();
        m_refractionSceneTex = NULL;
    }

    HRESULT hr = m_d3dDevice->CreateTexture(
        (UINT)desiredW, (UINT)desiredH,
        1,
        D3DUSAGE_RENDERTARGET,
        desiredFmt,
        D3DPOOL_DEFAULT,
        &m_refractionSceneTex,
        NULL);
    if (FAILED(hr) || !m_refractionSceneTex)
    {
        // Fallback: some drivers/backbuffers are X8R8G8B8; if that fails as an RT texture, try A8R8G8B8.
        if (desiredFmt != D3DFMT_A8R8G8B8)
        {
            hr = m_d3dDevice->CreateTexture(
                (UINT)desiredW, (UINT)desiredH,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &m_refractionSceneTex,
                NULL);
            if (SUCCEEDED(hr) && m_refractionSceneTex)
            {
                desiredFmt = D3DFMT_A8R8G8B8;
            }
        }
        if (FAILED(hr) || !m_refractionSceneTex)
        {
            RendererLog("ensureRefractionSceneCopyResources: CreateTexture failed");
            return false;
        }
    }

    hr = m_refractionSceneTex->GetSurfaceLevel(0, &m_refractionSceneSurf);
    if (FAILED(hr) || !m_refractionSceneSurf)
    {
        RendererLog("ensureRefractionSceneCopyResources: GetSurfaceLevel failed");
        if (m_refractionSceneTex) { m_refractionSceneTex->Release(); m_refractionSceneTex = NULL; }
        return false;
    }

    m_refractionSceneW = desiredW;
    m_refractionSceneH = desiredH;
    m_refractionSceneFmt = (int)desiredFmt;
    return true;
}

bool Scene3DRenderer::ensureRefractionWhiteTexture()
{
    if (!m_d3dDevice)
        return false;
    if (m_refractionWhiteTex)
        return true;

    static int s_refractionWhiteLogCount = 0;
    if (s_refractionWhiteLogCount < 10)
        RendererLog("ensureRefractionWhiteTexture: enter");

    IDirect3DTexture9* tex = NULL;
    if (s_refractionWhiteLogCount < 10)
        RendererLog("ensureRefractionWhiteTexture: CreateTexture");
    HRESULT hr = m_d3dDevice->CreateTexture(
        1, 1, 1,
        0,
        D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED,
        &tex,
        NULL);
    if (FAILED(hr) || !tex)
    {
        RendererLog("ensureRefractionWhiteTexture: CreateTexture failed");
        return false;
    }

    IDirect3DSurface9* surf = NULL;
    if (SUCCEEDED(tex->GetSurfaceLevel(0, &surf)) && surf)
    {
        DWORD white = 0xFFFFFFFF; // opaque white
        if (!LoadSurfaceFromMemoryInternal(surf, &white, sizeof(DWORD), D3DFMT_A8R8G8B8, 1, 1, 0))
        {
            D3DLOCKED_RECT lr;
            if (s_refractionWhiteLogCount < 10)
                RendererLog("ensureRefractionWhiteTexture: LockRect");
            if (SUCCEEDED(tex->LockRect(0, &lr, NULL, 0)))
            {
                *(DWORD*)lr.pBits = 0xFFFFFFFF; // opaque white
                tex->UnlockRect(0);
            }
        }
        surf->Release();
    }

    m_refractionWhiteTex = tex;
    if (s_refractionWhiteLogCount < 10)
    {
        RendererLog("ensureRefractionWhiteTexture: ok");
        s_refractionWhiteLogCount++;
    }
    return true;
}

bool Scene3DRenderer::ensureRefractionShader()
{
    if (!m_d3dDevice)
        return false;
    if (m_refractionPS)
        return true;

    // Note: We avoid runtime D3DX assembly here because D3DXAssembleShader has been observed
    // to hang on some systems/drivers during refraction FX. Use a precompiled ps_2_0 blob instead.
    HRESULT hr = m_d3dDevice->CreatePixelShader((const DWORD*)g_ps20_main, &m_refractionPS);
    if (FAILED(hr) || !m_refractionPS)
    {
        RendererLog("ensureRefractionShader: Precompiled CreatePixelShader failed, trying HLSL compile");
        IDirect3DPixelShader9* compiled = NULL;
        if (CompilePixelShaderFromFile(m_d3dDevice, "refraction_ps.hlsl", "main", "ps_2_0", &compiled))
        {
            m_refractionPS = compiled;
            RendererLog("ensureRefractionShader: Created refraction pixel shader (D3DXCompileShader)");
            return true;
        }
        RendererLog("ensureRefractionShader: D3DX compile failed");
        return false;
    }

    RendererLog("ensureRefractionShader: Created refraction pixel shader (precompiled)");
    return true;
}

bool Scene3DRenderer::prepareRefractionSceneCopy()
{
    if (!m_d3dDevice)
        return false;

    static int s_refractionLogCount = 0;
    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: enter");

    D3DVIEWPORT9 vp;
    if (FAILED(m_d3dDevice->GetViewport(&vp)))
        return false;

    IDirect3DSurface9* src = NULL;
    if (FAILED(m_d3dDevice->GetRenderTarget(0, &src)) || !src)
        return false;

    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: got RT0");

    D3DSURFACE_DESC srcDesc;
    if (FAILED(src->GetDesc(&srcDesc)))
    {
        src->Release();
        return false;
    }

    const int w = (int)vp.Width;
    const int h = (int)vp.Height;
    if (w < 1 || h < 1)
    {
        src->Release();
        return false;
    }

    // Try to match the current render target format to avoid driver issues on StretchRect.
    D3DFORMAT copyFmt = srcDesc.Format;
    if (copyFmt != D3DFMT_A8R8G8B8 && copyFmt != D3DFMT_X8R8G8B8)
        copyFmt = D3DFMT_A8R8G8B8;

    if (!ensureRefractionSceneCopyResources(w, h, (int)copyFmt))
    {
        src->Release();
        return false;
    }
    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: ensured RT texture");
    if (!ensureRefractionWhiteTexture())
    {
        src->Release();
        return false;
    }
    if (!ensureRefractionShader())
    {
        src->Release();
        return false;
    }
    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: shader/white ok");

    RECT srcRect;
    srcRect.left = (LONG)vp.X;
    srcRect.top = (LONG)vp.Y;
    srcRect.right = (LONG)(vp.X + vp.Width);
    srcRect.bottom = (LONG)(vp.Y + vp.Height);

    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: before StretchRect");

    HRESULT hr = m_d3dDevice->StretchRect(src, &srcRect, m_refractionSceneSurf, NULL, D3DTEXF_NONE);
    src->Release();

    if (FAILED(hr))
    {
        RendererLog("prepareRefractionSceneCopy: StretchRect failed");
        return false;
    }

    if (s_refractionLogCount < 10)
        RendererLog("prepareRefractionSceneCopy: after StretchRect");
    if (s_refractionLogCount < 10)
        s_refractionLogCount++;

    return true;
}

void Scene3DRenderer::setImGuiViewportActive(bool active)
{
    m_imguiViewportActive = active;
}

void Scene3DRenderer::setImGuiViewportSize(int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    m_imguiViewportW = width;
    m_imguiViewportH = height;
}

IDirect3DTexture9* Scene3DRenderer::getImGuiViewportTexture()
{
    // Allocate an RT at least as large as the current backbuffer.
    int desiredW = (m_windowWidth > 1) ? m_windowWidth : 1;
    int desiredH = (m_windowHeight > 1) ? m_windowHeight : 1;
    ensureImGuiViewportResources(desiredW, desiredH);
    return m_imguiViewportTex;
}

void Scene3DRenderer::getImGuiViewportTextureSize(int& outW, int& outH) const
{
    outW = m_imguiViewportTexW;
    outH = m_imguiViewportTexH;
}

// Load scene
bool Scene3DRenderer::loadScene(const char* hkxFilename)
{
    if (!m_initialized || !m_loader || !m_converter) {
        return false;
    }

    // Load scene (same as hkDefaultPhysicsDemo)
    if (!m_loader->loadHavokScene(hkxFilename)) {
        return false;
    }

    // Convert to display objects (same as all demos)
    m_converter->convertScene(m_loader->getScene());

    return true;
}

bool Scene3DRenderer::loadGameModel(const char* jmodelPath, const char* glbPath, const char* textureDir)
{
    if (!m_context || !jmodelPath || !glbPath)
    {
        return false;
    }

    // Keep current animation paths so we can reapply after reloading the model.
    char prevAnimPath[260] = {0};
    char prevGraphPath[512] = {0};
    if (m_jsonAnimPath[0] != '\0')
    {
        strcpy_s(prevAnimPath, m_jsonAnimPath);
    }
    if (m_animGraphPath[0] != '\0')
    {
        strcpy_s(prevGraphPath, m_animGraphPath);
    }

    GameModelLoader modelLoader;
    modelLoader.setDisplayContext(m_context);

    GameModel* newModel = modelLoader.loadModel(jmodelPath, glbPath, textureDir);
    if (!newModel || !newModel->skeleton)
    {
        if (newModel)
        {
            newModel->release();
            delete newModel;
        }
        return false;
    }

    AnimatedCharacter* newCharacter = new AnimatedCharacter();
    if (!newCharacter->setSkeleton(newModel->skeleton))
    {
        delete newCharacter;
        newModel->release();
        delete newModel;
        return false;
    }

    if (m_animatedCharacter)
    {
        delete m_animatedCharacter;
        m_animatedCharacter = HK_NULL;
    }
    if (m_gameModel)
    {
        m_gameModel->release();
        delete m_gameModel;
        m_gameModel = NULL;
    }

    m_animatedCharacter = newCharacter;
    m_gameModel = newModel;

    if (m_jsonAnim)
    {
        delete m_jsonAnim;
        m_jsonAnim = NULL;
    }
    m_useJsonAnim = false;
    m_jsonAnimTime = 0.0f;
    clearAnimationGraph();
    m_modelBaseComputed = false;
    m_modelBaseOffsetY = 0.0f;
    m_editorRotKeys.clear();
    m_editorPosKeysX.clear();
    m_editorPosKeysY.clear();
    m_editorPosKeysZ.clear();
    m_editorScaleKeysX.clear();
    m_editorScaleKeysY.clear();
    m_editorScaleKeysZ.clear();
    m_editorOverrideRot.clear();
    m_editorLastLocalPose.clear();
    m_editorOverrideTrans.clear();
    m_editorRecording = false;
    m_editorDragging = false;
    m_editorDraggingTrans = false;
    m_editorTimelineDuration = 0.0f;
    m_localVars.clear();
    computeModelBaseOffsetFromReference();
    autoFrameLoadedModel();

    if (prevAnimPath[0] != '\0')
    {
        loadJsonAnimation(prevAnimPath);
    }
    if (prevGraphPath[0] != '\0')
    {
        loadAnimationGraph(prevGraphPath);
    }

    // Lift the ground to the character's feet if we already have a clamp offset
    // so the textured ground shows under the player.
    m_groundHeight = 0.0f;
    if (m_groundClampMode != GROUND_CLAMP_OFF)
    {
        m_groundHeight = 0.0f; // keep plane at Y=0; clamp logic offsets model
    }

    rebuildDefaultIKChains();

    return true;
}

bool Scene3DRenderer::loadPlayerModel(const char* jmodelPath, const char* glbPath, const char* textureDir)
{
    if (!m_context || !jmodelPath || !glbPath) return false;

    // Release old player model
    if (m_playerAnimChar) { delete m_playerAnimChar; m_playerAnimChar = NULL; }
    if (m_playerModel) { m_playerModel->release(); delete m_playerModel; m_playerModel = NULL; }

    GameModelLoader loader;
    loader.setDisplayContext(m_context);
    GameModel* model = loader.loadModel(jmodelPath, glbPath, textureDir);
    if (!model) return false;

    AnimatedCharacter* anim = NULL;
    if (model->skeleton)
    {
        anim = new AnimatedCharacter();
        if (!anim->setSkeleton(model->skeleton))
        {
            delete anim;
            anim = NULL;
        }
    }

    m_playerModel = model;
    m_playerAnimChar = anim;

    char msg[256];
    sprintf_s(msg, "Player model loaded: %d parts, %d bones",
              model->meshPartCount,
              model->skeleton ? model->skeleton->m_numBones : 0);
    RendererLog(msg);
    return true;
}

void Scene3DRenderer::renderPlayerCharacter()
{
    if (!m_playerModel || !m_physicsActive || !m_context) return;

    // Character world position = fly cam position (eye), adjusted down for model feet
    float charX = m_flyCamPos[0];
    float charY = m_flyCamPos[1] - 2.0f;  // eye offset down to feet
    float charZ = m_flyCamPos[2];
    float yaw   = m_playerYaw;

    float cosY = cosf(yaw);
    float sinY = sinf(yaw);

    // Texture state
    if (m_d3dDevice)
    {
        for (int s = 0; s < 4; ++s)
        {
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, m_maxAnisotropy);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, *(DWORD*)&m_mipBias);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, m_gammaEnabled ? TRUE : FALSE);
        }
    }

    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(true);
    m_context->setBlendState(false);
    m_context->setCullfaceMode(HKG_CULLFACE_CCW);
    m_context->setCullFaceState(true);
    m_context->setLightingState(false);

    // Skin: build animated pose from JSON clips (idle/walk/run)
    int boneCount = 0;
    std::vector<float> boneMatrices;

    if (m_playerModel->skeleton)
    {
        boneCount = m_playerModel->skeleton->m_numBones;
        boneMatrices.resize(boneCount * 16);

        hkaPose tmpPose(m_playerModel->skeleton);
        buildPlayerAnimationPose(tmpPose);

        for (int b = 0; b < boneCount; b++)
            tmpPose.getBoneModelSpace(b).get4x4ColumnMajor(&boneMatrices[b * 16]);
    }

    const float lightDir[3] = { 0.35f, 0.75f, 0.56f };
    const float ambient = 0.42f;
    const float diffuse = 0.58f;

    for (int pass = 0; pass < 2; ++pass)
    {
        const bool transparentPass = (pass == 1);
        m_context->setDepthReadState(true);
        m_context->setDepthWriteState(!transparentPass);
        m_context->setBlendState(transparentPass);
        m_context->setCullFaceState(!transparentPass);

        for (int i = 0; i < m_playerModel->meshPartCount; i++)
        {
            GameModel::MeshPart& part = m_playerModel->meshParts[i];
            if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
                continue;

            const bool isAlpha = ShouldRenderAsTransparentPart(part);
            if (transparentPass != isAlpha) continue;

            const int skinBoneCount = (part.inverseBindMatrixCount < boneCount)
                                      ? part.inverseBindMatrixCount : boneCount;
            bool canSkin = (boneCount > 0 && part.skinWeights &&
                            part.inverseBindMatrices && part.skinnedPositions &&
                            part.inverseBindMatrixCount > 0 && skinBoneCount > 0);

            // CPU skinning
            if (canSkin)
            {
                std::vector<float> skinMats(boneCount * 16);
                for (int b = 0; b < skinBoneCount; b++)
                    MultiplyMatrix4(&boneMatrices[b * 16],
                                    part.inverseBindMatrices + b * 16,
                                    &skinMats[b * 16]);
                for (int b = skinBoneCount; b < boneCount; b++)
                    MultiplyMatrix4(&boneMatrices[b * 16],
                                    part.inverseBindMatrices,
                                    &skinMats[b * 16]);

                for (int v = 0; v < part.vertexCount; v++)
                {
                    float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
                    float x = base[0], y = base[1], z = base[2];
                    float sx = 0.0f, sy = 0.0f, sz = 0.0f, ws = 0.0f;
                    for (int k = 0; k < 4; k++)
                    {
                        float w = part.skinWeights[v].boneWeights[k];
                        int bi  = part.skinWeights[v].boneIndices[k];
                        if (w <= 0.0f || bi < 0 || bi >= boneCount) continue;
                        const float* m = &skinMats[bi * 16];
                        sx += w * (m[0]*x + m[4]*y + m[8]*z  + m[12]);
                        sy += w * (m[1]*x + m[5]*y + m[9]*z  + m[13]);
                        sz += w * (m[2]*x + m[6]*y + m[10]*z + m[14]);
                        ws += w;
                    }
                    if (ws < 0.999f) { float inv = 1.0f - ws; sx += x*inv; sy += y*inv; sz += z*inv; }
                    part.skinnedPositions[v*3+0] = sx;
                    part.skinnedPositions[v*3+1] = sy;
                    part.skinnedPositions[v*3+2] = sz;
                }
            }

            if (part.diffuseTexture)
            {
                m_context->setCurrentTexture(0, part.diffuseTexture, HKG_TEXTURE_MODULATE);
                m_context->setTexture2DState(true);
            }
            else
            {
                m_context->setTexture2DState(false);
            }

            const bool hasNormals = (part.vertexStride >= 32);

            m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);

            for (int j = 0; j + 2 < part.indexCount; j += 3)
            {
                int idx[3] = { part.indices[j], part.indices[j+1], part.indices[j+2] };
                if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0 ||
                    idx[0] >= part.vertexCount || idx[1] >= part.vertexCount || idx[2] >= part.vertexCount)
                    continue;

                for (int t = 0; t < 3; t++)
                {
                    float* vb = (float*)((unsigned char*)part.vertices + idx[t] * part.vertexStride);
                    const float* pos = canSkin ? (part.skinnedPositions + idx[t]*3) : vb;

                    // Rotate model-space position by yaw, then translate to world
                    float lx = pos[0], ly = pos[1], lz = pos[2];
                    float wx = cosY * lx - sinY * lz + charX;
                    float wy = ly + charY;
                    float wz = sinY * lx + cosY * lz + charZ;
                    float pp[3] = { wx, wy, wz };

                    float n[3];
                    if (hasNormals) { n[0] = vb[5]; n[1] = vb[6]; n[2] = vb[7]; }
                    else            { n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f; }

                    float il = n[0]*lightDir[0] + n[1]*lightDir[1] + n[2]*lightDir[2];
                    if (il < 0.0f) il = 0.0f; if (il > 1.0f) il = 1.0f;
                    float c[4] = { ambient + diffuse*il, ambient + diffuse*il, ambient + diffuse*il, 1.0f };

                    float uv[2] = { 0.0f, 0.0f };
                    if (part.vertexStride >= 20) { uv[0] = vb[3]; uv[1] = vb[4]; }

                    m_context->setCurrentColor4(c);
                    m_context->setCurrentNormal(n);
                    m_context->setCurrentTextureCoord(uv);
                    m_context->setCurrentPosition(pp);
                }
            }

            m_context->endGroup();
        }
    }

    m_context->flush();
    m_context->setBlendState(false);
    m_context->setDepthWriteState(true);
    m_context->setCullFaceState(false);
}

bool Scene3DRenderer::loadGroundTexture(const char* texturePath, float tileRepeat)
{
    if (!m_context || !texturePath || texturePath[0] == '\0')
    {
        return false;
    }

    if (m_groundTexture)
    {
        m_groundTexture->removeReference();
        m_groundTexture = NULL;
    }

    hkgTexture* tex = hkgTexture::create(m_context);
    if (!tex)
    {
        return false;
    }

    if (!tex->loadFromFile(texturePath))
    {
        tex->removeReference();
        return false;
    }

    tex->setAutoMipMaps(true);
    tex->setAutoCompression(false);
    tex->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    tex->setTextureWrapModeU(HKG_TEXTURE_WRAP);
    tex->setTextureWrapModeV(HKG_TEXTURE_WRAP);
    tex->realize(false);
    tex->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));

    m_groundTexture = tex;
    strncpy_s(m_groundTexturePath, texturePath, _TRUNCATE);
    if (tileRepeat > 0.1f)
        m_groundTextureRepeat = tileRepeat;

    if (m_d3dDevice)
    {
        m_groundTextureD3D = CreateTextureFromFileExAInternal(m_d3dDevice, texturePath, m_gammaEnabled, 0);
        if (!m_groundTextureD3D)
        {
            RendererLog("loadGroundTexture: D3DXCreateTextureFromFileExA failed");
        }
    }
    return true;
}

void Scene3DRenderer::setGroundSize(float meters)
{
    if (meters > 1.0f)
    {
        m_groundSize = meters; // half-extent in world units (used as +/- range)
    }
}

// Create a simple test scene (load animated character)
void Scene3DRenderer::createTestScene()
{
    if (!m_initialized || !m_displayWorld || !m_context) {
        return;
    }

    RendererLog("Setting up test scene with animated character...");

    // Position camera to view the character
    m_cameraDistance = 3.0f;
    m_cameraYaw = 0.785f;  // 45 degrees
    m_cameraPitch = 0.3f;   // 17 degrees
    m_cameraTarget.setZero4();

    // Load model + skeleton using GameModelLoader
    if (!m_gameModel)
    {
        RendererLog("Loading model...");
        if (!loadGameModel(
            "../GameFiles/jmodels/CRD_CH_gdr_swd_all_01.json",
            "../GameFiles/models/CRD_CH_gdr_swd_all_01.glb",
            "../GameFiles/textures/"
        )) {
            RendererLog("ERROR: Failed to load model!");
            return;
        }

        RendererLog("Model loaded successfully!");
    }
    else
    {
        RendererLog("Using preloaded model (startup selection).");
    }

    // Use a simple textured plane for ground so the character sits on visible terrain.
    clearGroundModel(); // ensure no occluder geo stays bound
    loadGroundTexture("../GameFiles/textures/PLF_GB_DirtRoad_01_D.dds", 8.0f);
    m_groundSize = 80.0f; // widen the playable patch

    static bool s_rigScanDone = false;
    if (!s_rigScanDone)
    {
        s_rigScanDone = true;
        ScanForMatchingRigs(m_gameModel->skeleton);
    }

    // Load JSON animation that matches current skeleton (only if not already loaded).
    if (m_jsonAnimPath[0] == '\0')
    {
        strcpy_s(m_jsonAnimPath, kDefaultJsonAnimPath);
    }
    if (!m_jsonAnim && loadJsonAnimation(m_jsonAnimPath))
    {
        RendererLog("JSON animation active. HKX demo animations will remain idle.");
    }

    RendererLog("Test scene ready with animated character!");
}

void Scene3DRenderer::stepAnimation(float deltaTime)
{
    m_lastAnimDeltaTime = deltaTime;
    if (m_motionMatchEnabled)
    {
        updateMotionMatching(deltaTime);
        return;
    }
    if (getAnimationGraphEnabled())
    {
        updateAnimationGraph(deltaTime);
    }
    else if (m_useJsonAnim)
    {
        if (!m_jsonAnimPaused)
        {
            m_jsonAnimTime += deltaTime * m_playbackSpeed;
            float duration = getJsonAnimationDuration();
            if (duration > 0.0f)
            {
                if (m_loopAnimation)
                {
                    m_jsonAnimTime = fmod(m_jsonAnimTime, duration);
                    if (m_jsonAnimTime < 0.0f) m_jsonAnimTime += duration;
                }
                else
                {
                    if (m_jsonAnimTime > duration)
                    {
                        m_jsonAnimTime = duration;
                    }
                }
            }
        }

        if (m_jsonAnim && !m_jsonAnimPaused)
        {
            processAnimEvents(m_lastEventTime, m_jsonAnimTime);
            m_lastEventTime = m_jsonAnimTime;
        }

        // A/B loop region enforcement
        if (!m_jsonAnimPaused && m_loopRegionEnabled && m_loopRegionOut > m_loopRegionIn)
        {
            if (m_jsonAnimTime >= m_loopRegionOut)
            {
                m_jsonAnimTime = m_loopRegionIn + fmod(m_jsonAnimTime - m_loopRegionIn, m_loopRegionOut - m_loopRegionIn);
            }
            else if (m_jsonAnimTime < m_loopRegionIn)
            {
                m_jsonAnimTime = m_loopRegionIn;
            }
        }

        // Advance blend clip time independently
        if (!m_jsonAnimPaused && m_jsonBlendEnabled && m_jsonBlendIndependentTime && m_jsonBlendAnim)
        {
            m_jsonBlendTime += deltaTime * m_playbackSpeed;
            // Compute blend clip duration inline (GetClipDuration is static in Scene3DAnimation.cpp)
            float blendDur = m_jsonBlendAnim->duration;
            if (blendDur <= 0.0f)
            {
                float ft = (m_jsonBlendAnim->frameTime > 0.0f) ? m_jsonBlendAnim->frameTime : (1.0f / 30.0f);
                int fc = (m_jsonBlendAnim->frameCount > 0) ? m_jsonBlendAnim->frameCount : 1;
                blendDur = ft * (float)(fc > 1 ? (fc - 1) : 1);
            }
            if (blendDur > 0.0f)
            {
                if (m_jsonBlendLoopBlendClip)
                {
                    m_jsonBlendTime = fmod(m_jsonBlendTime, blendDur);
                    if (m_jsonBlendTime < 0.0f) m_jsonBlendTime += blendDur;
                }
                else if (m_jsonBlendTime > blendDur)
                {
                    m_jsonBlendTime = blendDur;
                }
            }
        }

        // Advance multi-clip blend layer times + damped weights + transitions
        if (!m_jsonAnimPaused)
        {
            for (int li = 0; li < MAX_BLEND_LAYERS; ++li)
            {
                BlendLayer& layer = m_blendLayers[li];

                // Damped weight transition (smooth weight changes like Lua WeightDamp)
                if (m_blendWeightDamp > 0.0f && layer.weight != layer.targetWeight)
                {
                    float dampRate = m_blendWeightDamp * deltaTime;
                    float diff = layer.targetWeight - layer.weight;
                    if (fabsf(diff) < 0.001f)
                        layer.weight = layer.targetWeight;
                    else
                        layer.weight += diff * (1.0f - expf(-dampRate));
                }

                // Eased transition
                if (layer.inTransition && layer.transitionDuration > 0.0f)
                {
                    layer.transitionElapsed += deltaTime;
                    if (layer.transitionElapsed >= layer.transitionDuration)
                    {
                        layer.transitionElapsed = layer.transitionDuration;
                        layer.inTransition = false;
                    }
                }

                if (!layer.active || !layer.clip) continue;
                layer.time += deltaTime * m_playbackSpeed;
                float layerDur = layer.clip->duration;
                if (layerDur <= 0.0f)
                {
                    float ft = (layer.clip->frameTime > 0.0f) ? layer.clip->frameTime : (1.0f / 30.0f);
                    int fc = (layer.clip->frameCount > 0) ? layer.clip->frameCount : 1;
                    layerDur = ft * (float)(fc > 1 ? (fc - 1) : 1);
                }
                if (layerDur > 0.0f)
                {
                    if (layer.loop)
                    {
                        layer.time = fmod(layer.time, layerDur);
                        if (layer.time < 0.0f) layer.time += layerDur;
                    }
                    else if (layer.time > layerDur)
                    {
                        layer.time = layerDur;
                    }
                }
            }
        }

        // Advance auto-fade timer
        if (!m_jsonAnimPaused && m_jsonBlendFadeDuration > 0.0f)
        {
            m_jsonBlendFadeElapsed += deltaTime;
            if (m_jsonBlendFadeElapsed > m_jsonBlendFadeDuration)
                m_jsonBlendFadeElapsed = m_jsonBlendFadeDuration;
        }
    }
    else if (m_animatedCharacter)
    {
        m_animatedCharacter->update(deltaTime);
        m_animationTime += deltaTime;
    }
}

// Main rendering loop
// Based on: hkDemo::stepDemo() and render()
void Scene3DRenderer::stepAndRender(float deltaTime)
{
    if (!m_initialized || !m_window || !m_context) {
        return;
    }

    const bool perf = (m_d3dPerfMarkersEnabled && m_d3dDevice);
    D3DPerfBegin(perf, L"Scene3D Frame");

    static int s_fxStepTraceFrames = 0;
    static bool s_prevAuraActive = false;
    static int s_animFixedDropCooldown = 0;
    const bool auraActive = (m_effectManager && m_effectManager->hasActiveEffectNamed("FX_AB_Captain_power_aura"));
    if (auraActive && !s_prevAuraActive)
        s_fxStepTraceFrames = 30;
    s_prevAuraActive = auraActive;
    const bool fxTrace = (s_fxStepTraceFrames > 0);
    if (fxTrace)
    {
        char msg[128];
        sprintf_s(msg, "[FXSTEP] begin dt=%.4f", deltaTime);
        RendererLog(msg);
    }

    // Safety: if we have a loaded JSON clip and a skeleton, force json mode on.
    if (m_jsonAnim && m_gameModel && m_gameModel->skeleton)
    {
        m_useJsonAnim = true;
    }

    // Step 1: Clear buffers (same as all Havok demos)
    D3DPerfMarker(perf, L"Clear");
    m_window->clearBuffers();

    // Safety: ensure FX didn't leave "sticky" GPU state that corrupts the next frame.
    // This is especially important when custom FX paths early-return (packed shaders, refraction).
    ResetD3D9StateAfterFx(m_d3dDevice);

    if (!m_scene3dEnabled)
    {
        // Viewport disabled: override clear to match ImGui dark background so
        // any gap not covered by ImGui panels blends in rather than showing black.
        if (m_d3dDevice)
            m_d3dDevice->Clear(0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB(30, 30, 30), 1.0f, 0);
        RenderImGuiOverlayPass();
        m_window->swapBuffers();
        D3DPerfEnd(perf);
        return;
    }

    // Optional: render the 3D scene into an offscreen RT for the ImGui docking viewport.
    IDirect3DSurface9* savedRT = NULL;
    IDirect3DSurface9* savedDS = NULL;
    D3DVIEWPORT9 savedVP;
    bool restoreVP = false;
    bool usingImGuiRT = false;
    DWORD savedColorWrite = 0;
    bool restoreColorWrite = false;
    int vpW = m_windowWidth;
    int vpH = m_windowHeight;
    if (m_imguiViewportActive && m_d3dDevice)
    {
        if (m_imguiViewportW > 1) vpW = m_imguiViewportW;
        if (m_imguiViewportH > 1) vpH = m_imguiViewportH;
        if (vpW < 1) vpW = 1;
        if (vpH < 1) vpH = 1;

        int texW = (m_windowWidth > 1) ? m_windowWidth : 1;
        int texH = (m_windowHeight > 1) ? m_windowHeight : 1;
        if (ensureImGuiViewportResources(texW, texH) && m_imguiViewportSurf && m_imguiViewportDepth)
        {
            usingImGuiRT = true;
            if (vpW > m_imguiViewportTexW) vpW = m_imguiViewportTexW;
            if (vpH > m_imguiViewportTexH) vpH = m_imguiViewportTexH;

            m_d3dDevice->GetRenderTarget(0, &savedRT);
            m_d3dDevice->GetDepthStencilSurface(&savedDS);
            m_d3dDevice->GetViewport(&savedVP);
            restoreVP = true;

            m_d3dDevice->SetRenderTarget(0, m_imguiViewportSurf);
            m_d3dDevice->SetDepthStencilSurface(m_imguiViewportDepth);
            D3DVIEWPORT9 vp;
            vp.X = 0;
            vp.Y = 0;
            vp.Width = (DWORD)vpW;
            vp.Height = (DWORD)vpH;
            vp.MinZ = 0.0f;
            vp.MaxZ = 1.0f;
            m_d3dDevice->SetViewport(&vp);
            m_d3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               D3DCOLOR_ARGB(255, 20, 24, 30), 1.0f, 0);

            // Important: the scene often writes "garbage" alpha (or 0 alpha) from DDS textures.
            // When we present the viewport via ImGui::Image(), ImGui will alpha-blend the RT onto the
            // backbuffer, which can make models look gray/dithered. Keep the RT alpha fully opaque by
            // disabling alpha writes during scene rendering (alpha stays 255 from the clear).
            if (SUCCEEDED(m_d3dDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &savedColorWrite)))
            {
                restoreColorWrite = true;
                m_d3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE,
                                            D3DCOLORWRITEENABLE_RED |
                                            D3DCOLORWRITEENABLE_GREEN |
                                            D3DCOLORWRITEENABLE_BLUE);
            }

            if (m_camera)
            {
                m_camera->setAspect((float)vpW / (float)vpH);
                m_camera->computeProjection();
            }
        }
    }

    // Step 2: Update animation (fixed-step option for determinism)
    if (s_animFixedDropCooldown > 0)
        --s_animFixedDropCooldown;
    if (m_animFixedStepEnabled)
    {
        D3DPerfMarker(perf, L"AnimFixed");
        float step = m_animFixedStep;
        if (step < 1e-6f) step = 1e-6f;
        m_animFixedAccumulator += deltaTime;
        int steps = 0;
        int maxSteps = (m_animFixedMaxSteps > 0) ? m_animFixedMaxSteps : 1;
        while (m_animFixedAccumulator >= step && steps < maxSteps)
        {
            stepAnimation(step);
            m_animFixedAccumulator -= step;
            ++steps;
        }
        if (m_animFixedAccumulator >= step)
        {
            float dropped = m_animFixedAccumulator - step;
            m_animFixedAccumulator = fmod(m_animFixedAccumulator, step);
            if (s_animFixedDropCooldown <= 0 && dropped > 0.0f)
            {
                char msg[128];
                sprintf_s(msg, "[AnimFixed] Dropped %.2fms (exceeded max steps %d)", dropped * 1000.0f, maxSteps);
                RendererLog(msg);
                s_animFixedDropCooldown = 60;
            }
        }
        m_animFixedStepsLast = steps;
    }
    else
    {
        D3DPerfMarker(perf, L"Anim");
        m_animFixedStepsLast = 0;
        stepAnimation(deltaTime);
    }

    // Step 3: Update camera from input
    D3DPerfMarker(perf, L"Camera");
    updateCameraFromInput(deltaTime);

    // Step 4: Set camera as current (correct Havok v5.5.0 API)
    m_camera->setAsCurrent(m_context);

    // Step 5: Render skybox (separate from creature/model pipeline)
    D3DPerfMarker(perf, L"Skybox");
    renderSkyboxMesh();

    // Step 6: Render floor grid (skip when a level scene is loaded — level geometry replaces it)
    if (!(m_levelScene && m_levelScene->isLoaded()))
    {
        D3DPerfMarker(perf, L"Grid");
        renderFloorGrid();
    }

    // Step 6.5: Render full level scene (if loaded)
    if (m_levelScene && m_levelScene->isLoaded())
    {
        // Reset Havok context to known-good state BEFORE level render.
        // Without this, the skybox enabled/disabled toggle changes the
        // inherited Havok state, causing level meshes to render differently.
        if (m_context)
        {
            m_context->setDepthReadState(true);
            m_context->setDepthWriteState(true);
            m_context->setBlendState(false);
            m_context->setCullFaceState(false);
            m_context->setLightingState(false);
            m_context->setTexture2DState(true);
        }
        D3DPerfMarker(perf, L"LevelScene");
        m_levelScene->render();
        // Havok state may be dirty after D3D9 direct calls — reset safe defaults
        if (m_context)
        {
            m_context->setDepthReadState(true);
            m_context->setDepthWriteState(true);
            m_context->setBlendState(false);
            m_context->setCullFaceState(false);
            m_context->setLightingState(false);
        }
    }

    // Step 7: Build pose for skeleton/mesh
    bool fxUpdated = false;
    if (m_gameModel && m_gameModel->skeleton)
    {
        hkaPose pose(m_gameModel->skeleton);
        pose.setToReferencePose();

        if (m_motionMatchEnabled)
        {
            buildPoseFromMotionMatch(pose);
            // buildPoseFromMotionMatch calls syncModelSpace internally
        }
        else if (getAnimationGraphEnabled())
        {
            buildPoseFromAnimGraph(pose);
            // buildPoseFromAnimGraph calls syncModelSpace internally
        }
        else if (m_useJsonAnim && m_jsonAnim)
        {
            buildPoseFromJson(pose, m_jsonAnimTime);
            // buildPoseFromJson calls syncModelSpace internally
        }
        else if (m_animatedCharacter)
        {
            m_animatedCharacter->getPose(pose);
            pose.syncModelSpace();
        }
        else
        {
            pose.syncModelSpace();
        }

        // Update particle effects while the pose is available (bone-attached FX).
        if (m_effectManager)
        {
            D3DPerfMarker(perf, L"FXUpdate");
            m_effectManager->update(deltaTime, this, &pose);
            fxUpdated = true;
        }
        if (fxTrace) RendererLog("[FXSTEP] after fx update (pose)");

        // Step 8: Render animated skeleton
        D3DPerfMarker(perf, L"Skeleton");
        if (fxTrace) RendererLog("[FXSTEP] before skeleton");
        renderSkeletonFromPose(pose);
        if (fxTrace) RendererLog("[FXSTEP] after skeleton");

        // Step 9: Render game model mesh
        if (m_gameModel->meshPartCount > 0) {
            D3DPerfMarker(perf, L"Mesh");
            if (fxTrace) RendererLog("[FXSTEP] before mesh");
            renderMesh(&pose);
            if (fxTrace) RendererLog("[FXSTEP] after mesh");
        }
    }
    else if (m_gameModel && m_gameModel->meshPartCount > 0)
    {
        renderMesh(NULL);
    }

    // Step 9.5: Render player character (physics playtest third-person model)
    if (m_physicsActive && m_playerModel)
    {
        D3DPerfMarker(perf, L"PlayerChar");
        renderPlayerCharacter();
    }

    // Step 4: Render display world (same as all Havok demos)
    D3DPerfMarker(perf, L"DisplayWorld");
    if (fxTrace) RendererLog("[FXSTEP] before displayWorld->render");
    m_displayWorld->render(m_context,
                           true,   // enableFrustumCull
                           true,   // enableShadows
                           false); // inOverdraw
    if (fxTrace) RendererLog("[FXSTEP] after displayWorld->render");

    // Step 4.5: Update and render particle effects
    if (m_effectManager) {
        if (!fxUpdated)
        {
            m_effectManager->update(deltaTime, this, HK_NULL);
        }
        D3DPerfMarker(perf, L"FXRender");
        if (fxTrace) RendererLog("[FXSTEP] before fx render");
        m_effectManager->render(this);
        if (fxTrace) RendererLog("[FXSTEP] after fx render");
    }

    if (usingImGuiRT && m_d3dDevice)
    {
        if (restoreColorWrite)
        {
            m_d3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE, savedColorWrite);
        }
        if (savedRT)
        {
            m_d3dDevice->SetRenderTarget(0, savedRT);
            savedRT->Release();
            savedRT = NULL;
        }
        if (savedDS)
        {
            m_d3dDevice->SetDepthStencilSurface(savedDS);
            savedDS->Release();
            savedDS = NULL;
        }
        if (restoreVP)
        {
            m_d3dDevice->SetViewport(&savedVP);
        }
    }

    // Step 5: Swap buffers (present frame)
    D3DPerfMarker(perf, L"ImGui");
    RenderImGuiOverlayPass();
    D3DPerfMarker(perf, L"Present");
    m_window->swapBuffers();

    if (s_fxStepTraceFrames > 0)
        s_fxStepTraceFrames--;

    D3DPerfEnd(perf);
}

// Handle window resize
void Scene3DRenderer::resize(int width, int height)
{
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    m_windowWidth = width;
    m_windowHeight = height;

    // Keep Havok's backbuffer/context in sync with host HWND size to avoid
    // stretched/overlapping output after resize/fullscreen changes.
    if (m_window)
    {
        // D3D9 Reset requires ALL default-pool resources to be released and
        // nothing still bound to the device (SetRenderTarget/SetTexture/etc.
        // internally AddRef, blocking Reset even after we Release our pointers).
        ImGui_PreReset();
        releaseImGuiViewportResources();
        releaseRefractionResources();

        // Unbind everything from the device so D3D9 drops its internal refs.
        if (m_d3dDevice)
        {
            // Restore Havok's own back buffer as RT0 (can't set RT0 to NULL).
            IDirect3DSurface9* backBuf = NULL;
            if (SUCCEEDED(m_d3dDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf)) && backBuf)
            {
                m_d3dDevice->SetRenderTarget(0, backBuf);
                backBuf->Release();
            }
            m_d3dDevice->SetDepthStencilSurface(NULL);
            for (int i = 0; i < 8; ++i)
                m_d3dDevice->SetTexture(i, NULL);
            m_d3dDevice->SetVertexShader(NULL);
            m_d3dDevice->SetPixelShader(NULL);
            m_d3dDevice->SetStreamSource(0, NULL, 0, 0);
            m_d3dDevice->SetIndices(NULL);
        }

        bool ok = m_window->updateSize((unsigned int)width, (unsigned int)height);
        ImGui_PostReset();
        if (!ok)
        {
            char msg[128];
            sprintf(msg, "WARNING: hkgWindow::updateSize(%d,%d) failed", width, height);
            RendererLog(msg);
        }
    }

    if (m_camera) {
        m_camera->setAspect((float)width / (float)height);
        m_camera->computeProjection();
    }
}

// Setup default lighting
void Scene3DRenderer::setupDefaultLighting()
{
    if (!m_lightManager) return;
    while (m_lightManager->getNumLights() > 0)
    {
        int last = m_lightManager->getNumLights() - 1;
        hkgLight* old = m_lightManager->removeLight(last);
        if (old) old->removeReference();
    }

    // Key light
    hkgLight* key = hkgLight::create();
    if (key)
    {
        key->setType(HKG_LIGHT_DIRECTIONAL);
        float diff[3] = { 1.0f, 0.95f, 0.88f };
        float dir[3] = { 0.35f, -0.75f, 0.56f };
        key->setDiffuse(diff);
        key->setDirection(dir);
        m_lightManager->addLight(key);
        key->removeReference();
    }

    // Fill light
    hkgLight* fill = hkgLight::create();
    if (fill)
    {
        fill->setType(HKG_LIGHT_DIRECTIONAL);
        float diff[3] = { 0.45f, 0.50f, 0.60f };
        float dir[3] = { -0.6f, -0.4f, -0.4f };
        fill->setDiffuse(diff);
        fill->setDirection(dir);
        m_lightManager->addLight(fill);
        fill->removeReference();
    }

    // Back light
    hkgLight* back = hkgLight::create();
    if (back)
    {
        back->setType(HKG_LIGHT_DIRECTIONAL);
        float diff[3] = { 0.35f, 0.35f, 0.45f };
        float dir[3] = { 0.0f, -0.5f, -1.0f };
        back->setDiffuse(diff);
        back->setDirection(dir);
        m_lightManager->addLight(back);
        back->removeReference();
    }

    if (m_rimLightEnabled)
    {
        hkgLight* rim = hkgLight::create();
        if (rim)
        {
            rim->setType(HKG_LIGHT_DIRECTIONAL);
            float diff[3] = { 0.6f, 0.65f, 0.9f };
            float dir[3] = { 0.0f, 0.1f, 1.0f };
            rim->setDiffuse(diff);
            rim->setDirection(dir);
            m_lightManager->addLight(rim);
            rim->removeReference();
        }
    }

    // Ambient from skybox-ish: soft neutral ambient
    float ambient[3] = { 0.22f, 0.22f, 0.25f };
    m_lightManager->setSceneAmbient(ambient);
}

// Compute bounds helper was placed above.


void Scene3DRenderer::onKeyDown(int keyCode)
{
    if (keyCode == 'W')
    {
        setGizmoMode(GIZMO_TRANSLATE);
        RendererLog("Gizmo mode: MOVE");
        return;
    }
    if (keyCode == 'E')
    {
        setGizmoMode(GIZMO_ROTATE);
        RendererLog("Gizmo mode: ROTATE");
        return;
    }
    if (keyCode == 'Q')
    {
        toggleGizmoSpace();
        RendererLog((m_gizmoSpace == GIZMO_LOCAL) ? "Gizmo space: LOCAL" : "Gizmo space: WORLD");
        return;
    }
    if (keyCode == VK_HOME)
    {
        // Shift+Home: camera home. Plain Home handled in viewport for animation restart.
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        {
            homeCamera();
            RendererLog("Camera: Home/reset");
            return;
        }
    }
    if (keyCode == 'H')
    {
        toggleHorizonLock();
        RendererLog(m_horizonLock ? "Camera horizon lock: ON" : "Camera horizon lock: OFF");
        return;
    }
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && keyCode == 'Z')
    {
        undoPoseEdit();
        return;
    }
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && keyCode == 'Y')
    {
        redoPoseEdit();
        return;
    }
    if (keyCode == 'U')
    {
        setGammaEnabled(!m_gammaEnabled);
        char msg[128];
        sprintf_s(msg, "sRGB Gamma: %s", m_gammaEnabled ? "ON" : "OFF");
        RendererLog(msg);
        return;
    }
    if (keyCode == 'I')
    {
        toggleRimLight();
        RendererLog(m_rimLightEnabled ? "Rim light: ON" : "Rim light: OFF");
        return;
    }
    if (keyCode == VK_F9)
    {
        m_usePackedParticleShaders = !m_usePackedParticleShaders;
        RendererLog(m_usePackedParticleShaders ? "Packed particle shaders: ON" : "Packed particle shaders: OFF");
        return;
    }
    if (keyCode >= VK_F1 && keyCode <= VK_F5)
    {
        int slot = keyCode - VK_F1 + 1;
        bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (altDown && shiftDown)
        {
            savePoseSlot(slot, NULL);
            return;
        }
        if (altDown && ctrlDown)
        {
            applyPoseSlot(slot, true);
            return;
        }
        if (altDown)
        {
            applyPoseSlot(slot, false);
            return;
        }
    }
    if (keyCode == 'X' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
    {
        m_rootMotionLock[0] = !m_rootMotionLock[0];
        RendererLog(m_rootMotionLock[0] ? "Root motion X locked" : "Root motion X free");
        return;
    }
    if (keyCode == 'Y' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
    {
        m_rootMotionLock[1] = !m_rootMotionLock[1];
        RendererLog(m_rootMotionLock[1] ? "Root motion Y locked" : "Root motion Y free");
        return;
    }
    if (keyCode == 'Z' && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
    {
        m_rootMotionLock[2] = !m_rootMotionLock[2];
        RendererLog(m_rootMotionLock[2] ? "Root motion Z locked" : "Root motion Z free");
        return;
    }
    if (keyCode == VK_OEM_COMMA) // roll left
    {
        rollCamera(-0.05f);
        return;
    }
    if (keyCode == VK_OEM_PERIOD) // roll right
    {
        rollCamera(0.05f);
        return;
    }
    if (keyCode == 'T')
    {
        m_jsonDecodeMode = (m_jsonDecodeMode + 1) % 4;
        const char* modeLabel =
            (m_jsonDecodeMode == 1) ? "legacyA" :
            (m_jsonDecodeMode == 2) ? "legacyB" :
            (m_jsonDecodeMode == 3) ? "rotvec" : "havok";

        char msg[128];
        sprintf_s(msg, "JSON decode mode: %s", modeLabel);
        RendererLog(msg);

        // Reload JSON animation to update log stats and apply new mode
        if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
        {
            loadJsonAnimation(m_jsonAnimPath);
        }
        return;
    }
    if (keyCode == 'Y')
    {
        m_type2PackingMode = (m_type2PackingMode + 1) % 2;
        const char* modeLabel = (m_type2PackingMode == 1) ? "per-axis" : "interleaved";

        char msg[128];
        sprintf_s(msg, "Type2 packing mode: %s", modeLabel);
        RendererLog(msg);

        if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
        {
            loadJsonAnimation(m_jsonAnimPath);
        }
        return;
    }
    if (keyCode == 'L')
    {
        m_logType2 = !m_logType2;
        m_lastType2LogFrame = -1;
        RendererLog(m_logType2 ? "Type2 translation logging: ON" : "Type2 translation logging: OFF");
        return;
    }
    if (keyCode == 'R')
    {
        m_rotAxisMode = (m_rotAxisMode + 1) % 6;
        const char* modeLabel =
            (m_rotAxisMode == 1) ? "xzy" :
            (m_rotAxisMode == 2) ? "zyx" :
            (m_rotAxisMode == 3) ? "yxz" :
            (m_rotAxisMode == 4) ? "yzx" :
            (m_rotAxisMode == 5) ? "zxy" : "xyz";

        char msg[128];
        sprintf_s(msg, "Rotation axis mode: %s", modeLabel);
        RendererLog(msg);

        if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
        {
            loadJsonAnimation(m_jsonAnimPath);
        }
        return;
    }
    if (keyCode == 'F')
    {
        // Shift+F preserves legacy rotation-sign toggle; plain F focuses camera.
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        {
            m_rotSignMask = (m_rotSignMask + 1) % 8;
            char msg[128];
            sprintf_s(msg, "Rotation sign mask: %d (flipX=%s flipY=%s flipZ=%s)",
                      m_rotSignMask,
                      (m_rotSignMask & 1) ? "yes" : "no",
                      (m_rotSignMask & 2) ? "yes" : "no",
                      (m_rotSignMask & 4) ? "yes" : "no");
            RendererLog(msg);

            if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
            {
                loadJsonAnimation(m_jsonAnimPath);
            }
        }
        else
        {
            focusSelection();
            RendererLog("Camera focus on selection/model");
        }
        return;
    }
    if (keyCode == 'P')
    {
        m_forceReferencePose = !m_forceReferencePose;
        RendererLog(m_forceReferencePose ? "Reference pose lock: ON" : "Reference pose lock: OFF");
        return;
    }
    if (keyCode == 'C')
    {
        m_applyPoseCorrection = !m_applyPoseCorrection;
        RendererLog(m_applyPoseCorrection ? "Pose correction (frame0->ref) : ON" : "Pose correction (frame0->ref) : OFF");

        if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
        {
            loadJsonAnimation(m_jsonAnimPath);
        }
        return;
    }
    if (keyCode == 'M')
    {
        int next = ((int)m_rootMotionMode + 1) % 4;
        setRootMotionMode(next);
        RendererLog(
            (m_rootMotionMode == ROOT_MOTION_EXTRACT) ? "Root motion: EXTRACT (world offset)" :
            (m_rootMotionMode == ROOT_MOTION_OFF) ? "Root motion: OFF (locked to ref)" :
            (m_rootMotionMode == ROOT_MOTION_CLAMP_Y) ? "Root motion: clamp Y" :
                                      "Root motion: full");
        return;
    }
    if (keyCode == 'G')
    {
        m_groundClampMode = static_cast<GroundClampMode>((m_groundClampMode + 1) % 3);
        RendererLog(
            (m_groundClampMode == GROUND_CLAMP_SNAP) ? "Ground clamp: SNAP" :
            (m_groundClampMode == GROUND_CLAMP_BELOW) ? "Ground clamp: BELOW" :
                                       "Ground clamp: OFF");
        return;
    }
    if (keyCode == 'O' || keyCode == 'K')
    {
        m_rotApplyMode = static_cast<RotApplyMode>((m_rotApplyMode + 1) % 3);
        RendererLog(
            (m_rotApplyMode == ROT_DELTA_ONLY) ? "Rotation apply order: delta-only" :
            (m_rotApplyMode == ROT_DELTA_REF) ? "Rotation apply order: delta*ref" :
                                    "Rotation apply order: ref*delta");

        if (m_useJsonAnim && m_gameModel && m_jsonAnimPath[0] != '\0')
        {
            loadJsonAnimation(m_jsonAnimPath);
        }
        return;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// LevelScene integration
// ─────────────────────────────────────────────────────────────────────────────

bool Scene3DRenderer::loadLevelScene(const ZeroEngine::LevelReader& reader)
{
    if (!m_d3dDevice) return false;

    if (!m_levelScene)
        m_levelScene = new LevelScene();

    m_levelScene->setDevice(m_d3dDevice);
    bool ok = m_levelScene->load(reader);
    if (ok && m_levelScene->hasBounds())
    {
        float bMin[3], bMax[3];
        m_levelScene->getBounds(bMin, bMax);
        float cx = (bMin[0] + bMax[0]) * 0.5f;
        float cy = (bMin[1] + bMax[1]) * 0.5f;
        float cz = (bMin[2] + bMax[2]) * 0.5f;
        float ex = bMax[0] - bMin[0];
        float ey = bMax[1] - bMin[1];
        float ez = bMax[2] - bMin[2];
        float extent = ex > ey ? ex : ey;
        if (ez > extent) extent = ez;
        m_cameraTarget.set(cx, cy, cz, 0.0f);
        m_cameraDistance = extent * 0.7f;
        if (m_cameraDistance > 50000.0f) m_cameraDistance = 50000.0f;
        float farNeeded = extent * 3.0f;
        if (farNeeded > 100000.0f) m_camera->setFar(farNeeded);
    }
    return ok;
}

void Scene3DRenderer::unloadLevelScene()
{
    if (m_levelScene)
        m_levelScene->unload();
}

bool Scene3DRenderer::hasLevelScene() const
{
    return m_levelScene && m_levelScene->isLoaded();
}

int Scene3DRenderer::levelSceneInstanceCount() const
{
    return m_levelScene ? m_levelScene->instanceCount() : 0;
}

int Scene3DRenderer::levelSceneModelCount() const
{
    return m_levelScene ? m_levelScene->modelCount() : 0;
}

int Scene3DRenderer::levelSceneDrawCalls() const
{
    return m_levelScene ? m_levelScene->drawCallCount() : 0;
}


void Scene3DRenderer::renderFloorGrid()
{
    if (!m_context) return;

    // Basic render state
    m_context->setLightingState(false);
    m_context->setBlendState(false);

    const float gridSize = m_groundSize;          // extent in +X/-X, +Z/-Z
    const float gridStep = 1.0f;                   // meter spacing for lines
    const int gridLines = (int)(gridSize / gridStep);
    const float groundY = m_groundHeight;          // lift/sink ground if needed

    // If we have a loaded ground model, render it first and skip the flat quad.
    if (m_groundModel && m_groundModelVisible)
    {
        renderGroundModel();
    }
    else if (m_groundFillEnabled)
    {
        // --- Ground plane (textured if available) ---
        const float groundExtent = gridSize;
        float white[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float groundDark[]  = { 0.20f, 0.20f, 0.20f, 1.0f };
        float groundLight[] = { 0.32f, 0.32f, 0.32f, 1.0f };

        float v0[] = { -groundExtent, groundY, -groundExtent };
        float v1[] = {  groundExtent, groundY, -groundExtent };
        float v2[] = {  groundExtent, groundY,  groundExtent };
        float v3[] = { -groundExtent, groundY,  groundExtent };

        const float uvRepeat = m_groundTextureRepeat;
        float uv0[] = { 0.0f,           0.0f          };
        float uv1[] = { uvRepeat,       0.0f          };
        float uv2[] = { uvRepeat,       uvRepeat      };
        float uv3[] = { 0.0f,           uvRepeat      };

        if (m_groundTexture)
        {
            m_context->setCurrentTexture(0, m_groundTexture, HKG_TEXTURE_MODULATE);
            m_context->setTexture2DState(true);
            m_context->setCurrentColor4(white);
        }
        else
        {
            m_context->setTexture2DState(false);
        }

        m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);

        if (m_groundTexture)
        {
            m_context->setCurrentTextureCoord(uv0); m_context->setCurrentPosition(v0);
            m_context->setCurrentTextureCoord(uv1); m_context->setCurrentPosition(v1);
            m_context->setCurrentTextureCoord(uv2); m_context->setCurrentPosition(v2);

            m_context->setCurrentTextureCoord(uv0); m_context->setCurrentPosition(v0);
            m_context->setCurrentTextureCoord(uv2); m_context->setCurrentPosition(v2);
            m_context->setCurrentTextureCoord(uv3); m_context->setCurrentPosition(v3);
        }
        else
        {
            m_context->setCurrentColor4(groundDark);  m_context->setCurrentPosition(v0);
            m_context->setCurrentColor4(groundLight); m_context->setCurrentPosition(v1);
            m_context->setCurrentColor4(groundDark);  m_context->setCurrentPosition(v2);

            m_context->setCurrentColor4(groundDark);  m_context->setCurrentPosition(v0);
            m_context->setCurrentColor4(groundLight); m_context->setCurrentPosition(v2);
            m_context->setCurrentColor4(groundLight); m_context->setCurrentPosition(v3);
        }

        m_context->endGroup();
    }

    // --- Grid + axes overlay ---
    m_context->setTexture2DState(false);

    float gridColor[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    m_context->beginGroup(HKG_IMM_LINES);
    m_context->setCurrentColor4(gridColor);

    for (int i = -gridLines; i <= gridLines; i++)
    {
        float z = i * gridStep;
        float x1[] = { -gridSize, groundY, z };
        float x2[] = {  gridSize, groundY, z };
        m_context->setCurrentPosition(x1);
        m_context->setCurrentPosition(x2);
    }

    for (int i = -gridLines; i <= gridLines; i++)
    {
        float x = i * gridStep;
        float z1[] = { x, groundY, -gridSize };
        float z2[] = { x, groundY,  gridSize };
        m_context->setCurrentPosition(z1);
        m_context->setCurrentPosition(z2);
    }

    float redColor[] = { 1.0f, 0.0f, 0.0f, 1.0f };
    m_context->setCurrentColor4(redColor);
    float xAxis1[] = { -gridSize, groundY, 0.0f };
    float xAxis2[] = {  gridSize, groundY, 0.0f };
    m_context->setCurrentPosition(xAxis1);
    m_context->setCurrentPosition(xAxis2);

    float blueColor[] = { 0.0f, 0.0f, 1.0f, 1.0f };
    m_context->setCurrentColor4(blueColor);
    float zAxis1[] = { 0.0f, groundY, -gridSize };
    float zAxis2[] = { 0.0f, groundY,  gridSize };
    m_context->setCurrentPosition(zAxis1);
    m_context->setCurrentPosition(zAxis2);

    m_context->endGroup();
    m_context->flush();
}

void Scene3DRenderer::clearGroundModel()
{
    if (m_groundModel)
    {
        m_groundModel->release();
        delete m_groundModel;
        m_groundModel = NULL;
    }
}

static void ComputeModelBounds(const GameModel* model, float& outMinY, float& outMaxY, bool& hasBounds)
{
    hasBounds = false;
    outMinY = 0.0f;
    outMaxY = 0.0f;
    if (!model) return;
    for (int i = 0; i < model->meshPartCount; ++i)
    {
        const GameModel::MeshPart& p = model->meshParts[i];
        if (!p.vertices || p.vertexCount <= 0) continue;
        for (int v = 0; v < p.vertexCount; ++v)
        {
            const float* base = (const float*)((const unsigned char*)p.vertices + v * p.vertexStride);
            float y = base[1];
            if (!hasBounds || y < outMinY) { outMinY = y; hasBounds = true; }
            if (!hasBounds || y > outMaxY) { outMaxY = y; hasBounds = true; }
        }
    }
}

bool Scene3DRenderer::loadGroundModel(const char* jmodelPath, const char* glbPath, const char* textureDir, float extraYOffset)
{
    if (!m_context || !jmodelPath || !glbPath)
    {
        return false;
    }

    GameModelLoader loader;
    loader.setDisplayContext(m_context);
    GameModel* model = loader.loadModel(jmodelPath, glbPath, textureDir);
    if (!model)
    {
        RendererLog("Ground model load failed");
        return false;
    }

    clearGroundModel();
    m_groundModel = model;
    m_groundModelOffsetY = extraYOffset;
    strncpy_s(m_groundJPath, jmodelPath, _TRUNCATE);
    strncpy_s(m_groundGlbPath, glbPath, _TRUNCATE);
    strncpy_s(m_groundTexDir, textureDir ? textureDir : "", _TRUNCATE);

    float minY, maxY;
    bool hasBounds = false;
    ComputeModelBounds(model, minY, maxY, hasBounds);
    if (hasBounds)
    {
        // Place the lowest vertex on Y=0, plus extra offset.
        m_groundHeight = -minY + extraYOffset;
    }
    else
    {
        m_groundHeight = 0.0f;
    }
    RendererLog("Ground model loaded");
    return true;
}


void Scene3DRenderer::renderGroundModel()
{
    if (!m_context || !m_groundModel) return;

    m_context->setTexture2DState(true);
    m_context->setLightingState(true);
    m_context->setBlendState(false);
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(true);
    m_context->setCullFaceState(true);

    if (m_d3dDevice)
    {
        for (int s = 0; s < 2; ++s)
        {
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, m_maxAnisotropy);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, *(DWORD*)&m_mipBias);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, m_gammaEnabled ? TRUE : FALSE);
        }
    }

    const float ambient = 0.35f;
    const float diffuse = 0.65f;
    const float lightDir[3] = { 0.35f, 0.75f, 0.56f };

    for (int i = 0; i < m_groundModel->meshPartCount; ++i)
    {
        const GameModel::MeshPart& part = m_groundModel->meshParts[i];
        if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
            continue;

        if (part.diffuseTexture)
        {
            m_context->setCurrentTexture(0, part.diffuseTexture, HKG_TEXTURE_MODULATE);
            m_context->setTexture2DState(true);
        }
        else
        {
            m_context->setTexture2DState(false);
        }

        const bool hasNormals = (part.vertexStride >= 32);
        m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);

        for (int j = 0; j + 2 < part.indexCount; j += 3)
        {
            int idx0 = part.indices[j + 0];
            int idx1 = part.indices[j + 1];
            int idx2 = part.indices[j + 2];
            if (idx0 < 0 || idx1 < 0 || idx2 < 0 ||
                idx0 >= part.vertexCount || idx1 >= part.vertexCount || idx2 >= part.vertexCount)
            {
                continue;
            }

            const float* v0 = (const float*)((const unsigned char*)part.vertices + idx0 * part.vertexStride);
            const float* v1 = (const float*)((const unsigned char*)part.vertices + idx1 * part.vertexStride);
            const float* v2 = (const float*)((const unsigned char*)part.vertices + idx2 * part.vertexStride);

            float p0[3] = { v0[0], v0[1] + m_groundHeight + m_groundModelOffsetY, v0[2] };
            float p1[3] = { v1[0], v1[1] + m_groundHeight + m_groundModelOffsetY, v1[2] };
            float p2[3] = { v2[0], v2[1] + m_groundHeight + m_groundModelOffsetY, v2[2] };

            float n0[3], n1[3], n2[3];
            if (hasNormals)
            {
                n0[0] = v0[5]; n0[1] = v0[6]; n0[2] = v0[7];
                n1[0] = v1[5]; n1[1] = v1[6]; n1[2] = v1[7];
                n2[0] = v2[5]; n2[1] = v2[6]; n2[2] = v2[7];
            }
            else
            {
                float e1x = p1[0] - p0[0];
                float e1y = p1[1] - p0[1];
                float e1z = p1[2] - p0[2];
                float e2x = p2[0] - p0[0];
                float e2y = p2[1] - p0[1];
                float e2z = p2[2] - p0[2];
                float fnx, fny, fnz;
                ZCross3f(fnx, fny, fnz, e1x, e1y, e1z, e2x, e2y, e2z);
                float fl = ZLength3f(fnx, fny, fnz);
                if (fl > 1e-6f)
                {
                    ZNormalize3f(fnx, fny, fnz);
                }
                else
                {
                    fnx = 0.0f; fny = 1.0f; fnz = 0.0f;
                }
                n0[0] = n1[0] = n2[0] = fnx;
                n0[1] = n1[1] = n2[1] = fny;
                n0[2] = n1[2] = n2[2] = fnz;
            }

            float i0 = hkMath::clamp(ZDot3f(n0[0], n0[1], n0[2], lightDir[0], lightDir[1], lightDir[2]), 0.0f, 1.0f);
            float i1 = hkMath::clamp(ZDot3f(n1[0], n1[1], n1[2], lightDir[0], lightDir[1], lightDir[2]), 0.0f, 1.0f);
            float i2 = hkMath::clamp(ZDot3f(n2[0], n2[1], n2[2], lightDir[0], lightDir[1], lightDir[2]), 0.0f, 1.0f);
            float c0[4] = { ambient + diffuse * i0, ambient + diffuse * i0, ambient + diffuse * i0, 1.0f };
            float c1[4] = { ambient + diffuse * i1, ambient + diffuse * i1, ambient + diffuse * i1, 1.0f };
            float c2[4] = { ambient + diffuse * i2, ambient + diffuse * i2, ambient + diffuse * i2, 1.0f };

            float uv0[2] = { 0.0f, 0.0f };
            float uv1[2] = { 0.0f, 0.0f };
            float uv2[2] = { 0.0f, 0.0f };
            if (part.vertexStride >= 20 && part.hasTexcoord0)
            {
                uv0[0] = v0[3]; uv0[1] = v0[4];
                uv1[0] = v1[3]; uv1[1] = v1[4];
                uv2[0] = v2[3]; uv2[1] = v2[4];
            }

            m_context->setCurrentColor4(c0);
            m_context->setCurrentNormal(n0);
            m_context->setCurrentTextureCoord(uv0);
            m_context->setCurrentPosition(p0);

            m_context->setCurrentColor4(c1);
            m_context->setCurrentNormal(n1);
            m_context->setCurrentTextureCoord(uv1);
            m_context->setCurrentPosition(p1);

            m_context->setCurrentColor4(c2);
            m_context->setCurrentNormal(n2);
            m_context->setCurrentTextureCoord(uv2);
            m_context->setCurrentPosition(p2);
        }

        m_context->endGroup();
    }
}

static bool uvLooksHalfTexelLocal(float a0, float a1, unsigned int size)
{
    if (size == 0)
        return false;

    float p0 = a0 * (float)size;
    float p1 = a1 * (float)size;

    float frac0 = fabsf((p0 - floorf(p0)) - 0.5f);
    float frac1 = fabsf((p1 - floorf(p1)) - 0.5f);

    // Game atlases store pixel-center bounds (half-texel inset). Standalone textures often use edge UVs.
    return (frac0 < 0.01f && frac1 < 0.01f);
}

static void Normalize3(float& x, float& y, float& z)
{
    float l2 = x*x + y*y + z*z;
    if (l2 < 1e-12f)
    {
        x = 1.0f; y = 0.0f; z = 0.0f;
        return;
    }
    ZNormalize3f(x, y, z);
}

static void RotateAroundAxis(float ax, float ay, float az, float angle, float& vx, float& vy, float& vz)
{
    // Rodrigues' rotation formula (axis must be normalized).
    float cosA = cosf(angle);
    float sinA = sinf(angle);

    float dot = ZDot3f(ax, ay, az, vx, vy, vz);
    float cx, cy, cz;
    ZCross3f(cx, cy, cz, ax, ay, az, vx, vy, vz);

    float rx = vx * cosA + cx * sinA + ax * dot * (1.0f - cosA);
    float ry = vy * cosA + cy * sinA + ay * dot * (1.0f - cosA);
    float rz = vz * cosA + cz * sinA + az * dot * (1.0f - cosA);

    vx = rx; vy = ry; vz = rz;
}

// Render billboard particles (camera-facing quads)
void Scene3DRenderer::renderParticleEmitter(ParticleEmitter* emitter)
{
    if (!emitter || !m_context || !m_camera)
        return;

    // Ensure we don't inherit debug/wireframe/shader state from other passes.
    ResetD3D9StateAfterFx(m_d3dDevice);

    // No particles to render
    if (emitter->particles.getSize() == 0)
    {
        static int logCount = 0;
        if (logCount++ % 60 == 0)
        {
            char msg[256];
            sprintf_s(msg, "[renderParticleEmitter] Emitter '%s' has 0 particles", emitter->name);
            RendererLog(msg);
        }
        return;
    }

    // Whether we have a texture for this emitter
    const bool hasTexture = (emitter->texture != HK_NULL);

    // For now, skip untextured emitters. In the shipped game these are usually refraction-only
    // ("heat haze") particles that require a special shader; rendering them as solid-color quads
    // looks like small colored walls.
    if (!hasTexture)
    {
        static int missingTexLog = 0;
        if (missingTexLog++ % 120 == 0)
        {
            char msg[256];
            sprintf_s(msg, "[renderParticleEmitter] Skipping emitter '%s' (no texture)", emitter->name);
            RendererLog(msg);
        }
        return;
    }

    // Prefer the game's packed particle shaders when available. This matches alpha-cut and
    // billboarding math more closely than Havok's fixed-function immediate mode.
    if (m_usePackedParticleShaders && m_d3dDevice)
    {
        static int s_packedTryLog = 0;
        if (s_packedTryLog < 10)
        {
            char msg[256];
            sprintf_s(msg, "[PackedFX] try emitter '%s' (particles=%d, orient=%d, refract=%d, tex=%p)",
                emitter->name, emitter->particles.getSize(), (int)emitter->orientMode, emitter->refract ? 1 : 0, emitter->texture);
            RendererLog(msg);
            s_packedTryLog++;
        }

        if (!m_packedParticleShaders)
            m_packedParticleShaders = new MgPackedParticleShaders();

        if (m_packedParticleShaders)
        {
            m_packedParticleShaders->setShaderRoot(m_packedShaderRoot);
            hkVector4 camPosV = getCameraPosition();

            // Guard against state leakage: the packed path touches raw D3D state.
            IDirect3DStateBlock9* sb = NULL;
            if (SUCCEEDED(m_d3dDevice->CreateStateBlock(D3DSBT_ALL, &sb)))
            {
                const bool ok = m_packedParticleShaders->renderBillboard_A_Vd(
                    m_d3dDevice, m_context, m_camera, camPosV, m_cameraTarget, emitter);
                sb->Apply();
                sb->Release();
                if (ok)
                {
                    ResetD3D9StateAfterFx(m_d3dDevice);
                    m_context->setTexture2DState(true);
                    m_context->setBlendState(false);
                    m_context->setLightingState(true);
                    m_context->setDepthWriteState(true);
                    m_context->setCullFaceState(true);
                    return;
                }
            }
            else
            {
                const bool ok = m_packedParticleShaders->renderBillboard_A_Vd(
                    m_d3dDevice, m_context, m_camera, camPosV, m_cameraTarget, emitter);
                if (ok)
                {
                    ResetD3D9StateAfterFx(m_d3dDevice);
                    m_context->setTexture2DState(true);
                    m_context->setBlendState(false);
                    m_context->setLightingState(true);
                    m_context->setDepthWriteState(true);
                    m_context->setCullFaceState(true);
                    return;
                }
            }
        }
    }

    static int renderLogCount = 0;
    if (renderLogCount++ % 300 == 0)
    {
        char msg[512];
        sprintf_s(msg, "[renderParticleEmitter] Rendering emitter '%s' with %d particles (tex=%s, atlas=%s, texPtr=%p, uv=%.3f,%.3f,%.3f,%.3f)",
                  emitter->name, emitter->particles.getSize(),
                  "yes",
                  emitter->usesSharedAtlas ? "yes" : "no",
                  emitter->texture,
                  emitter->atlasUVRect.u0, emitter->atlasUVRect.v0,
                  emitter->atlasUVRect.u1, emitter->atlasUVRect.v1);
        RendererLog(msg);
    }

    // Get camera vectors for billboard calculation
    float camPos[3], camUp[3], camRight[3];
    m_camera->getFrom(camPos);
    m_camera->getUp(camUp);
    m_camera->getRight(camRight);

    // Set render state for particles
    m_context->setLightingState(false);
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(false); // Particles don't write depth
    m_context->setCullFaceState(false); // Billboards are double-sided

    // Reset D3D state that the display world render may have left dirty.
    // This prevents wireframe mode, stale shaders, and wrong sampler states
    // from corrupting particle rendering.
    if (m_d3dDevice)
    {
        m_d3dDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        m_d3dDevice->SetVertexShader(NULL);
        m_d3dDevice->SetPixelShader(NULL);
        m_d3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

        // Force vertex color usage for our immediate-mode quads (many FX textures are missing
        // in extracted sets; without this, fallback quads render as opaque white "walls").
        m_d3dDevice->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        m_d3dDevice->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
        m_d3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

        // Clean sampler state for stage 0 (particle atlas texture)
        m_d3dDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        // Disable higher texture stages that display world may have enabled
        m_d3dDevice->SetTexture(1, NULL);
        m_d3dDevice->SetTexture(2, NULL);
        m_d3dDevice->SetTexture(3, NULL);
    }

    const bool orientToVelocity = (emitter->orientMode == ParticleEmitter::ORIENT_Z_TO_MOVEMENT_DIRECTION);
    const bool useAlphaTest = (emitter->startAlphaTestMin > 0 || emitter->startAlphaTestMax > 0 ||
                               emitter->endAlphaTestMin > 0 || emitter->endAlphaTestMax > 0);

    if (useAlphaTest && m_d3dDevice)
    {
        m_d3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        m_d3dDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        m_d3dDevice->SetRenderState(D3DRS_ALPHAREF, 0);
    }

    // Set blend mode
    if (emitter->blendMode == ParticleEmitter::ADDITIVE)
    {
        m_context->setBlendState(true);
        if (m_d3dDevice)
        {
            // For additive particles, use SRCALPHA/ONE so texture alpha properly attenuates brightness.
            // (We use fixed-function MODULATE, so alpha comes from texture.a * vertex.a.)
            m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        }
    }
    else // NORMAL (alpha blend)
    {
        m_context->setBlendState(true);
        if (m_d3dDevice)
        {
            // Straight alpha blend: SRCALPHA / INVSRCALPHA.
            // This keeps the output energy bounded even when the texture has soft alpha edges.
            m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        }
    }

    // Set texture or disable texturing for fallback colored quads.
    // IMPORTANT: Do NOT use direct m_d3dDevice->SetTextureStageState() here.
    // Doing so desyncs Havok's internal state cache and corrupts subsequent
    // rendering passes (skybox, mesh, etc.) because Havok skips D3D calls
    // when its cache says "nothing changed."
    m_context->setTexture2DState(true);
    m_context->setCurrentTexture(0, emitter->texture, HKG_TEXTURE_MODULATE);

    // Pre-compute atlas UV rect for this emitter
    // If usesSharedAtlas is true, the UV rect comes from the atlas lookup table
    // Otherwise fall back to full 0-1 range (individual texture)
    float atlasU0 = emitter->atlasUVRect.u0;
    float atlasV0 = emitter->atlasUVRect.v0;
    float atlasU1 = emitter->atlasUVRect.u1;
    float atlasV1 = emitter->atlasUVRect.v1;

    // Precompute flipbook grid slicing (pixel-accurate) for this emitter.
    // Layout comes from EffectManager's computed uvColumns/uvRows.
    unsigned int texW = 0;
    unsigned int texH = 0;
    if (emitter->texture)
    {
        texW = emitter->texture->getWidth();
        texH = emitter->texture->getHeight();
    }

    int cols = emitter->uvColumns;
    int rows = emitter->uvRows;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    const bool halfU = uvLooksHalfTexelLocal(atlasU0, atlasU1, texW);
    const bool halfV = uvLooksHalfTexelLocal(atlasV0, atlasV1, texH);

    float x0 = halfU ? (atlasU0 * (float)texW - 0.5f) : (atlasU0 * (float)texW);
    float x1 = halfU ? (atlasU1 * (float)texW + 0.5f) : (atlasU1 * (float)texW);
    float y0 = halfV ? (atlasV0 * (float)texH - 0.5f) : (atlasV0 * (float)texH);
    float y1 = halfV ? (atlasV1 * (float)texH + 0.5f) : (atlasV1 * (float)texH);

    float rectW = x1 - x0;
    float rectH = y1 - y0;
    if (rectW <= 0.0f) rectW = 1.0f;
    if (rectH <= 0.0f) rectH = 1.0f;

    const float frameW = rectW / (float)cols;
    const float frameH = rectH / (float)rows;

    // Render each particle as a billboard quad
    m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);

    const int particleCount = emitter->particles.getSize();

    std::vector<int> sortedIdx;
    if (emitter->sortParticles && particleCount > 1)
    {
        sortedIdx.resize(particleCount);
        for (int i = 0; i < particleCount; i++)
            sortedIdx[i] = i;

        // Sort back-to-front along view direction for correct alpha blending.
        hkVector4 camPosV = getCameraPosition();
        hkVector4 camTargetV = m_cameraTarget;
        float fx = camTargetV(0) - camPosV(0);
        float fy = camTargetV(1) - camPosV(1);
        float fz = camTargetV(2) - camPosV(2);
        float fl2 = fx*fx + fy*fy + fz*fz;
        if (fl2 < 1e-6f) { fx = 0.0f; fy = 0.0f; fz = 1.0f; }
        else { ZNormalize3f(fx, fy, fz); }

        struct DepthCmp
        {
            ParticleEmitter* e;
            float cx, cy, cz;
            float fx, fy, fz;

            DepthCmp(ParticleEmitter* _e, float _cx, float _cy, float _cz, float _fx, float _fy, float _fz)
                : e(_e), cx(_cx), cy(_cy), cz(_cz), fx(_fx), fy(_fy), fz(_fz) {}

            bool operator()(int a, int b) const
            {
                const Particle& pa = e->particles[a];
                const Particle& pb = e->particles[b];
                float dax = pa.position(0) - cx;
                float day = pa.position(1) - cy;
                float daz = pa.position(2) - cz;
                float dbx = pb.position(0) - cx;
                float dby = pb.position(1) - cy;
                float dbz = pb.position(2) - cz;
                float da = dax*fx + day*fy + daz*fz;
                float db = dbx*fx + dby*fy + dbz*fz;
                return da > db;
            }
        };

        std::sort(sortedIdx.begin(), sortedIdx.end(),
                  DepthCmp(emitter, camPosV(0), camPosV(1), camPosV(2), fx, fy, fz));
    }

    int lastAlphaRef = -1;

    for (int drawIdx = 0; drawIdx < particleCount; drawIdx++)
    {
        const int i = sortedIdx.empty() ? drawIdx : sortedIdx[drawIdx];
        const Particle& p = emitter->particles[i];

        if (useAlphaTest && m_d3dDevice)
        {
            float t = 1.0f;
            if (p.maxLifetime > 0.000001f)
            {
                t = p.lifetime / p.maxLifetime;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
            }

            int aRef = (int)((float)p.alphaTestStart + ((float)p.alphaTestEnd - (float)p.alphaTestStart) * t + 0.5f);
            if (aRef < 0) aRef = 0;
            if (aRef > 255) aRef = 255;

            if (aRef != lastAlphaRef)
            {
                lastAlphaRef = aRef;
                m_d3dDevice->SetRenderState(D3DRS_ALPHAREF, (DWORD)aRef);
            }
        }

        // Calculate billboard quad vertices (camera-facing)
        float w = p.width;
        float h = p.height;

        // Pivot point: where the particle position lies within the quad.
        // (0,0) = bottom-left, (1,1) = top-right. Default is (0.5,0.5) center.
        float pivotX = p.pivotX;
        float pivotY = p.pivotY;
        if (pivotX < 0.0f) pivotX = 0.0f; else if (pivotX > 1.0f) pivotX = 1.0f;
        if (pivotY < 0.0f) pivotY = 0.0f; else if (pivotY > 1.0f) pivotY = 1.0f;

        float left   = -pivotX * w;
        float right  = (1.0f - pivotX) * w;
        float bottom = -pivotY * h;
        float top    = (1.0f - pivotY) * h;

        // Billboard center position
        float px = p.position(0);
        float py = p.position(1);
        float pz = p.position(2);

        // Calculate quad corners using camera right/up vectors, or velocity-aligned streaks.
        float rightX = camRight[0];
        float rightY = camRight[1];
        float rightZ = camRight[2];
        float upX = camUp[0];
        float upY = camUp[1];
        float upZ = camUp[2];

        const float rotX = p.rotation(0);
        const float rotY = p.rotation(1);
        const float rotZ = p.rotation(2);
        const bool hasEuler =
            (fabsf(rotX) > 1e-6f) ||
            (fabsf(rotY) > 1e-6f) ||
            (fabsf(rotZ) > 1e-6f);

        if (orientToVelocity)
        {
            // Long axis follows particle velocity (or emitter axis as fallback).
            float ax = p.velocity(0);
            float ay = p.velocity(1);
            float az = p.velocity(2);
            float al2 = ax*ax + ay*ay + az*az;
            if (al2 < 1e-6f)
            {
                ax = emitter->axisY(0);
                ay = emitter->axisY(1);
                az = emitter->axisY(2);
                al2 = ax*ax + ay*ay + az*az;
                if (al2 < 1e-6f) { ax = 0.0f; ay = 1.0f; az = 0.0f; }
            }
            if (al2 >= 1e-6f) { ZNormalize3f(ax, ay, az); }

            // Side vector facing the camera around the axis.
            float vdx = camPos[0] - px;
            float vdy = camPos[1] - py;
            float vdz = camPos[2] - pz;
            float vl2 = vdx*vdx + vdy*vdy + vdz*vdz;
            if (vl2 < 1e-6f) { vdx = 0.0f; vdy = 0.0f; vdz = 1.0f; }
            else { ZNormalize3f(vdx, vdy, vdz); }

            float sx, sy, sz;
            ZCross3f(sx, sy, sz, vdx, vdy, vdz, ax, ay, az);
            float sl2 = sx*sx + sy*sy + sz*sz;
            if (sl2 < 1e-6f)
            {
                // Degenerate: use camera up to build a side vector.
                ZCross3f(sx, sy, sz, camUp[0], camUp[1], camUp[2], ax, ay, az);
                sl2 = sx*sx + sy*sy + sz*sz;
            }
            if (sl2 < 1e-6f)
            {
                sx = camRight[0];
                sy = camRight[1];
                sz = camRight[2];
                sl2 = sx*sx + sy*sy + sz*sz;
            }
            if (sl2 >= 1e-6f) { ZNormalize3f(sx, sy, sz); }

            rightX = sx; rightY = sy; rightZ = sz;
            upX = ax; upY = ay; upZ = az;

            if (hasEuler)
            {
                // Build a local basis: right=side, up=movement axis, forward=normal.
                Normalize3(rightX, rightY, rightZ);
                Normalize3(upX, upY, upZ);
                float fx, fy, fz;
                ZCross3f(fx, fy, fz, upX, upY, upZ, rightX, rightY, rightZ);
                Normalize3(fx, fy, fz);

                // X: rotate around right axis
                if (fabsf(rotX) > 1e-6f)
                {
                    RotateAroundAxis(rightX, rightY, rightZ, rotX, upX, upY, upZ);
                    RotateAroundAxis(rightX, rightY, rightZ, rotX, fx, fy, fz);
                }

                // Y: rotate around movement axis (matches prior behavior)
                if (fabsf(rotY) > 1e-6f)
                {
                    RotateAroundAxis(upX, upY, upZ, rotY, rightX, rightY, rightZ);
                    RotateAroundAxis(upX, upY, upZ, rotY, fx, fy, fz);
                }

                // Z: rotate around forward axis
                if (fabsf(rotZ) > 1e-6f)
                {
                    RotateAroundAxis(fx, fy, fz, rotZ, rightX, rightY, rightZ);
                    RotateAroundAxis(fx, fy, fz, rotZ, upX, upY, upZ);
                }
            }
        }
        else if (hasEuler)
        {
            // Build a local billboard basis: right=camera right, up=camera up, forward=normal.
            // Note: forward is computed as cross(up,right) to preserve legacy in-plane spin direction.
            Normalize3(rightX, rightY, rightZ);
            Normalize3(upX, upY, upZ);
            float fx, fy, fz;
            ZCross3f(fx, fy, fz, upX, upY, upZ, rightX, rightY, rightZ);
            Normalize3(fx, fy, fz);

            // X: pitch around right axis (tilt sprites for ground-aligned quads, etc.)
            if (fabsf(rotX) > 1e-6f)
            {
                RotateAroundAxis(rightX, rightY, rightZ, rotX, upX, upY, upZ);
                RotateAroundAxis(rightX, rightY, rightZ, rotX, fx, fy, fz);
            }

            // Y: in-plane spin around forward axis (matches prior behavior)
            if (fabsf(rotY) > 1e-6f)
            {
                RotateAroundAxis(fx, fy, fz, rotY, rightX, rightY, rightZ);
                RotateAroundAxis(fx, fy, fz, rotY, upX, upY, upZ);
            }

            // Z: yaw around up axis
            if (fabsf(rotZ) > 1e-6f)
            {
                RotateAroundAxis(upX, upY, upZ, rotZ, rightX, rightY, rightZ);
                RotateAroundAxis(upX, upY, upZ, rotZ, fx, fy, fz);
            }
        }

        // Calculate 4 corners of the billboard quad
        float v0[3], v1[3], v2[3], v3[3];

        // Bottom-left
        v0[0] = px + rightX * left + upX * bottom;
        v0[1] = py + rightY * left + upY * bottom;
        v0[2] = pz + rightZ * left + upZ * bottom;

        // Bottom-right
        v1[0] = px + rightX * right + upX * bottom;
        v1[1] = py + rightY * right + upY * bottom;
        v1[2] = pz + rightZ * right + upZ * bottom;

        // Top-right
        v2[0] = px + rightX * right + upX * top;
        v2[1] = py + rightY * right + upY * top;
        v2[2] = pz + rightZ * right + upZ * top;

        // Top-left
        v3[0] = px + rightX * left + upX * top;
        v3[1] = py + rightY * left + upY * top;
        v3[2] = pz + rightZ * left + upZ * top;

        // Calculate texture coordinates
        float u0, v0_uv, u1, v1_uv;
        if (cols > 1 || rows > 1)
        {
            int capacity = cols * rows;
            int frameIndex = p.currentFrame;
            if (frameIndex < 0) frameIndex = 0;
            if (capacity > 0 && frameIndex >= capacity) frameIndex = capacity - 1;

            int col = (cols > 0) ? (frameIndex % cols) : 0;
            int row = (cols > 0) ? (frameIndex / cols) : 0;
            if (col < 0) col = 0;
            if (col >= cols) col = cols - 1;
            if (row < 0) row = 0;
            if (row >= rows) row = rows - 1;

            float fx0 = x0 + frameW * (float)col;
            float fx1 = fx0 + frameW;
            float fy0 = y0 + frameH * (float)row;
            float fy1 = fy0 + frameH;

            if (halfU && texW > 0)
            {
                u0 = (fx0 + 0.5f) / (float)texW;
                u1 = (fx1 - 0.5f) / (float)texW;
            }
            else if (texW > 0)
            {
                u0 = fx0 / (float)texW;
                u1 = fx1 / (float)texW;
            }
            else
            {
                u0 = atlasU0;
                u1 = atlasU1;
            }

            if (halfV && texH > 0)
            {
                v0_uv = (fy0 + 0.5f) / (float)texH;
                v1_uv = (fy1 - 0.5f) / (float)texH;
            }
            else if (texH > 0)
            {
                v0_uv = fy0 / (float)texH;
                v1_uv = fy1 / (float)texH;
            }
            else
            {
                v0_uv = atlasV0;
                v1_uv = atlasV1;
            }
        }
        else
        {
            u0 = atlasU0;
            v0_uv = atlasV0;
            u1 = atlasU1;
            v1_uv = atlasV1;
        }

        // Set particle color
        float color[4];
        color[0] = (float)p.color.r / 255.0f;
        color[1] = (float)p.color.g / 255.0f;
        color[2] = (float)p.color.b / 255.0f;
        color[3] = (float)p.color.a / 255.0f;

        // Emit 2 triangles (6 vertices) for the quad
        // Triangle 1: v0, v1, v2
        float uv_bl[2] = { u0, v1_uv };
        float uv_br[2] = { u1, v1_uv };
        float uv_tr[2] = { u1, v0_uv };
        float uv_tl[2] = { u0, v0_uv };

        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_bl);
        m_context->setCurrentPosition(v0);

        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_br);
        m_context->setCurrentPosition(v1);

        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_tr);
        m_context->setCurrentPosition(v2);

        // Triangle 2: v0, v2, v3
        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_bl);
        m_context->setCurrentPosition(v0);

        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_tr);
        m_context->setCurrentPosition(v2);

        m_context->setCurrentColor4(color);
        m_context->setCurrentTextureCoord(uv_tl);
        m_context->setCurrentPosition(v3);
    }

    m_context->endGroup();
    m_context->flush(); // Commit all buffered geometry before state restore

    // Restore render state — use Havok API only (no direct D3D calls for
    // texture stage state) to keep Havok's internal cache in sync.
    m_context->setTexture2DState(true);
    m_context->setBlendState(false);
    m_context->setLightingState(true);
    m_context->setDepthWriteState(true);
    m_context->setCullFaceState(true);
    if (m_d3dDevice)
    {
        // Blend factors are raw D3D state not tracked by Havok, safe to set.
        m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_d3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    }
}

struct RefractVertex
{
    float x, y, z;
    DWORD color;
    float u0, v0; // normal/noise UV
    float u1, v1; // screen UV
    float u2, v2; // mask UV
};

static const DWORD kRefractFVF = (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX3);

void Scene3DRenderer::renderRefractParticleEmitter(ParticleEmitter* emitter)
{
    if (!emitter || !m_context || !m_d3dDevice || !m_camera)
        return;

    // Safety: refraction uses custom D3D state + shaders.
    ResetD3D9StateAfterFx(m_d3dDevice);

    if (!emitter->refract)
    {
        renderParticleEmitter(emitter);
        return;
    }

    const int particleCount = emitter->particles.getSize();
    if (particleCount <= 0)
        return;

    // Need a refraction normal/noise texture to compute screen UV offsets.
    if (!emitter->refractionTexture)
    {
        // Fallback: if the author provided a visible mask texture but no refraction map could be resolved,
        // render as a regular particle emitter instead of showing a broken refract pass.
        if (emitter->texture)
            renderParticleEmitter(emitter);
        return;
    }

    // Ensure we have a captured scene and shader.
    if (!m_refractionSceneTex || !m_refractionSceneSurf || !m_refractionPS)
    {
        if (!prepareRefractionSceneCopy())
            return;
    }

    const int vpW = (m_refractionSceneW > 0) ? m_refractionSceneW : m_windowWidth;
    const int vpH = (m_refractionSceneH > 0) ? m_refractionSceneH : m_windowHeight;
    if (vpW < 1 || vpH < 1)
        return;

    // Camera vectors for billboard orientation.
    float camPos[3], camUp[3], camRight[3];
    m_camera->getFrom(camPos);
    m_camera->getUp(camUp);
    m_camera->getRight(camRight);

    const bool orientToVelocity = (emitter->orientMode == ParticleEmitter::ORIENT_Z_TO_MOVEMENT_DIRECTION);

    // Flipbook layout (shared between mask + refraction textures in most effects).
    int cols = emitter->uvColumns;
    int rows = emitter->uvRows;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    // Refraction normal/noise atlas sampling parameters.
    hkgTexture* normalTex = emitter->refractionTexture;
    const AtlasUVRect& nRect = emitter->refractionAtlasUVRect;
    unsigned int nTexW = normalTex ? normalTex->getWidth() : 0;
    unsigned int nTexH = normalTex ? normalTex->getHeight() : 0;
    const bool nHalfU = uvLooksHalfTexelLocal(nRect.u0, nRect.u1, nTexW);
    const bool nHalfV = uvLooksHalfTexelLocal(nRect.v0, nRect.v1, nTexH);
    float nX0 = nHalfU ? (nRect.u0 * (float)nTexW - 0.5f) : (nRect.u0 * (float)nTexW);
    float nX1 = nHalfU ? (nRect.u1 * (float)nTexW + 0.5f) : (nRect.u1 * (float)nTexW);
    float nY0 = nHalfV ? (nRect.v0 * (float)nTexH - 0.5f) : (nRect.v0 * (float)nTexH);
    float nY1 = nHalfV ? (nRect.v1 * (float)nTexH + 0.5f) : (nRect.v1 * (float)nTexH);
    float nRectW = nX1 - nX0;
    float nRectH = nY1 - nY0;
    if (nRectW <= 0.0f) nRectW = 1.0f;
    if (nRectH <= 0.0f) nRectH = 1.0f;
    const float nFrameW = nRectW / (float)cols;
    const float nFrameH = nRectH / (float)rows;

    // Optional mask texture sampling parameters.
    hkgTexture* maskTex = emitter->texture;
    const AtlasUVRect& mRect = emitter->atlasUVRect;
    unsigned int mTexW = maskTex ? maskTex->getWidth() : 0;
    unsigned int mTexH = maskTex ? maskTex->getHeight() : 0;
    const bool mHalfU = uvLooksHalfTexelLocal(mRect.u0, mRect.u1, mTexW);
    const bool mHalfV = uvLooksHalfTexelLocal(mRect.v0, mRect.v1, mTexH);
    float mX0 = mHalfU ? (mRect.u0 * (float)mTexW - 0.5f) : (mRect.u0 * (float)mTexW);
    float mX1 = mHalfU ? (mRect.u1 * (float)mTexW + 0.5f) : (mRect.u1 * (float)mTexW);
    float mY0 = mHalfV ? (mRect.v0 * (float)mTexH - 0.5f) : (mRect.v0 * (float)mTexH);
    float mY1 = mHalfV ? (mRect.v1 * (float)mTexH + 0.5f) : (mRect.v1 * (float)mTexH);
    float mRectW = mX1 - mX0;
    float mRectH = mY1 - mY0;
    if (mRectW <= 0.0f) mRectW = 1.0f;
    if (mRectH <= 0.0f) mRectH = 1.0f;
    const float mFrameW = (maskTex ? (mRectW / (float)cols) : 1.0f);
    const float mFrameH = (maskTex ? (mRectH / (float)rows) : 1.0f);

    // D3D state baseline.
    m_context->setLightingState(false);
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(false);
    m_context->setCullFaceState(false);
    m_context->setBlendState(true);

    // Bind textures for the pixel shader.
    m_context->setTexture2DState(true);
    m_context->setCurrentTexture(0, normalTex, HKG_TEXTURE_MODULATE);
    m_d3dDevice->SetTexture(1, m_refractionSceneTex);
    if (maskTex)
        m_context->setCurrentTexture(2, maskTex, HKG_TEXTURE_MODULATE);
    else
        m_d3dDevice->SetTexture(2, m_refractionWhiteTex);

    // Sampler defaults (clamp to avoid UV bleed/wrap at screen edges and atlas tiles).
    for (int s = 0; s < 3; ++s)
    {
        m_d3dDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        m_d3dDevice->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        m_d3dDevice->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        m_d3dDevice->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    // Fixed alpha blend for refraction: lerp between (distorted scene) and (original scene).
    m_d3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_d3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Pixel shader constants:
    // c0.xy = UV scale (RefractionScale interpreted as pixels)
    // c1.xy = (2,2), c2.xy = (1,1) for (n*2 - 1)
    float c0[4] = {
        (vpW > 0) ? (emitter->refractionScale / (float)vpW) : 0.0f,
        (vpH > 0) ? (-emitter->refractionScale / (float)vpH) : 0.0f,
        0.0f, 0.0f
    };
    float c1[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
    float c2[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_d3dDevice->SetPixelShader(m_refractionPS);
    m_d3dDevice->SetVertexShader(NULL);
    m_d3dDevice->SetPixelShaderConstantF(0, c0, 1);
    m_d3dDevice->SetPixelShaderConstantF(1, c1, 1);
    m_d3dDevice->SetPixelShaderConstantF(2, c2, 1);

    std::vector<RefractVertex> verts;
    verts.reserve(particleCount * 6);

    for (int i = 0; i < particleCount; ++i)
    {
        const Particle& p = emitter->particles[i];

        float w = p.width;
        float h = p.height;

        float pivotX = p.pivotX;
        float pivotY = p.pivotY;
        if (pivotX < 0.0f) pivotX = 0.0f; else if (pivotX > 1.0f) pivotX = 1.0f;
        if (pivotY < 0.0f) pivotY = 0.0f; else if (pivotY > 1.0f) pivotY = 1.0f;

        float left   = -pivotX * w;
        float right  = (1.0f - pivotX) * w;
        float bottom = -pivotY * h;
        float top    = (1.0f - pivotY) * h;

        float px = p.position(0);
        float py = p.position(1);
        float pz = p.position(2);

        float rightX = camRight[0];
        float rightY = camRight[1];
        float rightZ = camRight[2];
        float upX = camUp[0];
        float upY = camUp[1];
        float upZ = camUp[2];

        const float rotX = p.rotation(0);
        const float rotY = p.rotation(1);
        const float rotZ = p.rotation(2);
        const bool hasEuler =
            (fabsf(rotX) > 1e-6f) ||
            (fabsf(rotY) > 1e-6f) ||
            (fabsf(rotZ) > 1e-6f);

        if (orientToVelocity)
        {
            float ax = p.velocity(0);
            float ay = p.velocity(1);
            float az = p.velocity(2);
            float al2 = ax*ax + ay*ay + az*az;
            if (al2 < 1e-6f)
            {
                ax = emitter->axisY(0);
                ay = emitter->axisY(1);
                az = emitter->axisY(2);
                al2 = ax*ax + ay*ay + az*az;
                if (al2 < 1e-6f) { ax = 0.0f; ay = 1.0f; az = 0.0f; }
            }
            if (al2 >= 1e-6f) { ZNormalize3f(ax, ay, az); }

            float vdx = camPos[0] - px;
            float vdy = camPos[1] - py;
            float vdz = camPos[2] - pz;
            float vl2 = vdx*vdx + vdy*vdy + vdz*vdz;
            if (vl2 < 1e-6f) { vdx = 0.0f; vdy = 0.0f; vdz = 1.0f; }
            else { ZNormalize3f(vdx, vdy, vdz); }

            float sx, sy, sz;
            ZCross3f(sx, sy, sz, vdx, vdy, vdz, ax, ay, az);
            float sl2 = sx*sx + sy*sy + sz*sz;
            if (sl2 < 1e-6f)
            {
                ZCross3f(sx, sy, sz, camUp[0], camUp[1], camUp[2], ax, ay, az);
                sl2 = sx*sx + sy*sy + sz*sz;
            }
            if (sl2 < 1e-6f)
            {
                sx = camRight[0];
                sy = camRight[1];
                sz = camRight[2];
                sl2 = sx*sx + sy*sy + sz*sz;
            }
            if (sl2 >= 1e-6f) { ZNormalize3f(sx, sy, sz); }

            rightX = sx; rightY = sy; rightZ = sz;
            upX = ax; upY = ay; upZ = az;

            if (hasEuler)
            {
                Normalize3(rightX, rightY, rightZ);
                Normalize3(upX, upY, upZ);
                float fx, fy, fz;
                ZCross3f(fx, fy, fz, upX, upY, upZ, rightX, rightY, rightZ);
                Normalize3(fx, fy, fz);

                if (fabsf(rotX) > 1e-6f)
                {
                    RotateAroundAxis(rightX, rightY, rightZ, rotX, upX, upY, upZ);
                    RotateAroundAxis(rightX, rightY, rightZ, rotX, fx, fy, fz);
                }

                if (fabsf(rotY) > 1e-6f)
                {
                    RotateAroundAxis(upX, upY, upZ, rotY, rightX, rightY, rightZ);
                    RotateAroundAxis(upX, upY, upZ, rotY, fx, fy, fz);
                }

                if (fabsf(rotZ) > 1e-6f)
                {
                    RotateAroundAxis(fx, fy, fz, rotZ, rightX, rightY, rightZ);
                    RotateAroundAxis(fx, fy, fz, rotZ, upX, upY, upZ);
                }
            }
        }
        else if (hasEuler)
        {
            Normalize3(rightX, rightY, rightZ);
            Normalize3(upX, upY, upZ);
            float fx, fy, fz;
            ZCross3f(fx, fy, fz, upX, upY, upZ, rightX, rightY, rightZ);
            Normalize3(fx, fy, fz);

            if (fabsf(rotX) > 1e-6f)
            {
                RotateAroundAxis(rightX, rightY, rightZ, rotX, upX, upY, upZ);
                RotateAroundAxis(rightX, rightY, rightZ, rotX, fx, fy, fz);
            }

            if (fabsf(rotY) > 1e-6f)
            {
                RotateAroundAxis(fx, fy, fz, rotY, rightX, rightY, rightZ);
                RotateAroundAxis(fx, fy, fz, rotY, upX, upY, upZ);
            }

            if (fabsf(rotZ) > 1e-6f)
            {
                RotateAroundAxis(upX, upY, upZ, rotZ, rightX, rightY, rightZ);
                RotateAroundAxis(upX, upY, upZ, rotZ, fx, fy, fz);
            }
        }

        float v0p[3], v1p[3], v2p[3], v3p[3];
        v0p[0] = px + rightX * left + upX * bottom;
        v0p[1] = py + rightY * left + upY * bottom;
        v0p[2] = pz + rightZ * left + upZ * bottom;

        v1p[0] = px + rightX * right + upX * bottom;
        v1p[1] = py + rightY * right + upY * bottom;
        v1p[2] = pz + rightZ * right + upZ * bottom;

        v2p[0] = px + rightX * right + upX * top;
        v2p[1] = py + rightY * right + upY * top;
        v2p[2] = pz + rightZ * right + upZ * top;

        v3p[0] = px + rightX * left + upX * top;
        v3p[1] = py + rightY * left + upY * top;
        v3p[2] = pz + rightZ * left + upZ * top;

        // Compute flipbook UV for the current frame in the refraction atlas.
        float nu0 = nRect.u0, nv0 = nRect.v0, nu1 = nRect.u1, nv1 = nRect.v1;
        if (cols > 1 || rows > 1)
        {
            int capacity = cols * rows;
            int frameIndex = p.currentFrame;
            if (frameIndex < 0) frameIndex = 0;
            if (capacity > 0 && frameIndex >= capacity) frameIndex = capacity - 1;
            int col = (cols > 0) ? (frameIndex % cols) : 0;
            int row = (cols > 0) ? (frameIndex / cols) : 0;
            if (col < 0) col = 0;
            if (col >= cols) col = cols - 1;
            if (row < 0) row = 0;
            if (row >= rows) row = rows - 1;

            float fx0 = nX0 + nFrameW * (float)col;
            float fx1 = fx0 + nFrameW;
            float fy0 = nY0 + nFrameH * (float)row;
            float fy1 = fy0 + nFrameH;

            if (nHalfU && nTexW > 0)
            {
                nu0 = (fx0 + 0.5f) / (float)nTexW;
                nu1 = (fx1 - 0.5f) / (float)nTexW;
            }
            else if (nTexW > 0)
            {
                nu0 = fx0 / (float)nTexW;
                nu1 = fx1 / (float)nTexW;
            }

            if (nHalfV && nTexH > 0)
            {
                nv0 = (fy0 + 0.5f) / (float)nTexH;
                nv1 = (fy1 - 0.5f) / (float)nTexH;
            }
            else if (nTexH > 0)
            {
                nv0 = fy0 / (float)nTexH;
                nv1 = fy1 / (float)nTexH;
            }
        }

        // Compute flipbook UV for the mask (or default 0..1 if missing).
        float mu0 = 0.0f, mv0 = 0.0f, mu1 = 1.0f, mv1 = 1.0f;
        if (maskTex)
        {
            mu0 = mRect.u0; mv0 = mRect.v0; mu1 = mRect.u1; mv1 = mRect.v1;
            if (cols > 1 || rows > 1)
            {
                int capacity = cols * rows;
                int frameIndex = p.currentFrame;
                if (frameIndex < 0) frameIndex = 0;
                if (capacity > 0 && frameIndex >= capacity) frameIndex = capacity - 1;
                int col = (cols > 0) ? (frameIndex % cols) : 0;
                int row = (cols > 0) ? (frameIndex / cols) : 0;
                if (col < 0) col = 0;
                if (col >= cols) col = cols - 1;
                if (row < 0) row = 0;
                if (row >= rows) row = rows - 1;

                float fx0 = mX0 + mFrameW * (float)col;
                float fx1 = fx0 + mFrameW;
                float fy0 = mY0 + mFrameH * (float)row;
                float fy1 = fy0 + mFrameH;

                if (mHalfU && mTexW > 0)
                {
                    mu0 = (fx0 + 0.5f) / (float)mTexW;
                    mu1 = (fx1 - 0.5f) / (float)mTexW;
                }
                else if (mTexW > 0)
                {
                    mu0 = fx0 / (float)mTexW;
                    mu1 = fx1 / (float)mTexW;
                }

                if (mHalfV && mTexH > 0)
                {
                    mv0 = (fy0 + 0.5f) / (float)mTexH;
                    mv1 = (fy1 - 0.5f) / (float)mTexH;
                }
                else if (mTexH > 0)
                {
                    mv0 = fy0 / (float)mTexH;
                    mv1 = fy1 / (float)mTexH;
                }
            }
        }

        // Screen UV (projected per vertex).
        float sp[3];
        float su0, sv0, su1, sv1, su2, sv2, su3, sv3;
        m_camera->project(v0p[0], v0p[1], v0p[2], vpW, vpH, sp);
        su0 = (sp[0] + 0.5f) / (float)vpW;
        sv0 = (sp[1] + 0.5f) / (float)vpH;
        m_camera->project(v1p[0], v1p[1], v1p[2], vpW, vpH, sp);
        su1 = (sp[0] + 0.5f) / (float)vpW;
        sv1 = (sp[1] + 0.5f) / (float)vpH;
        m_camera->project(v2p[0], v2p[1], v2p[2], vpW, vpH, sp);
        su2 = (sp[0] + 0.5f) / (float)vpW;
        sv2 = (sp[1] + 0.5f) / (float)vpH;
        m_camera->project(v3p[0], v3p[1], v3p[2], vpW, vpH, sp);
        su3 = (sp[0] + 0.5f) / (float)vpW;
        sv3 = (sp[1] + 0.5f) / (float)vpH;

        DWORD c = D3DCOLOR_ARGB(p.color.a, p.color.r, p.color.g, p.color.b);

        // Corner UVs (note: AtlasUVRect stores top/bottom; match existing particle UV orientation).
        const float n_uv_bl_u = nu0, n_uv_bl_v = nv1;
        const float n_uv_br_u = nu1, n_uv_br_v = nv1;
        const float n_uv_tr_u = nu1, n_uv_tr_v = nv0;
        const float n_uv_tl_u = nu0, n_uv_tl_v = nv0;

        const float m_uv_bl_u = mu0, m_uv_bl_v = mv1;
        const float m_uv_br_u = mu1, m_uv_br_v = mv1;
        const float m_uv_tr_u = mu1, m_uv_tr_v = mv0;
        const float m_uv_tl_u = mu0, m_uv_tl_v = mv0;

        // Triangle 1: v0, v1, v2
        RefractVertex a;
        a.x = v0p[0]; a.y = v0p[1]; a.z = v0p[2]; a.color = c;
        a.u0 = n_uv_bl_u; a.v0 = n_uv_bl_v; a.u1 = su0; a.v1 = sv0; a.u2 = m_uv_bl_u; a.v2 = m_uv_bl_v;
        verts.push_back(a);

        a.x = v1p[0]; a.y = v1p[1]; a.z = v1p[2]; a.color = c;
        a.u0 = n_uv_br_u; a.v0 = n_uv_br_v; a.u1 = su1; a.v1 = sv1; a.u2 = m_uv_br_u; a.v2 = m_uv_br_v;
        verts.push_back(a);

        a.x = v2p[0]; a.y = v2p[1]; a.z = v2p[2]; a.color = c;
        a.u0 = n_uv_tr_u; a.v0 = n_uv_tr_v; a.u1 = su2; a.v1 = sv2; a.u2 = m_uv_tr_u; a.v2 = m_uv_tr_v;
        verts.push_back(a);

        // Triangle 2: v0, v2, v3
        a.x = v0p[0]; a.y = v0p[1]; a.z = v0p[2]; a.color = c;
        a.u0 = n_uv_bl_u; a.v0 = n_uv_bl_v; a.u1 = su0; a.v1 = sv0; a.u2 = m_uv_bl_u; a.v2 = m_uv_bl_v;
        verts.push_back(a);

        a.x = v2p[0]; a.y = v2p[1]; a.z = v2p[2]; a.color = c;
        a.u0 = n_uv_tr_u; a.v0 = n_uv_tr_v; a.u1 = su2; a.v1 = sv2; a.u2 = m_uv_tr_u; a.v2 = m_uv_tr_v;
        verts.push_back(a);

        a.x = v3p[0]; a.y = v3p[1]; a.z = v3p[2]; a.color = c;
        a.u0 = n_uv_tl_u; a.v0 = n_uv_tl_v; a.u1 = su3; a.v1 = sv3; a.u2 = m_uv_tl_u; a.v2 = m_uv_tl_v;
        verts.push_back(a);
    }

    if (!verts.empty())
    {
        m_d3dDevice->SetFVF(kRefractFVF);
        m_d3dDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), &verts[0], sizeof(RefractVertex));
    }

    // Restore baseline state for subsequent rendering.
    ResetD3D9StateAfterFx(m_d3dDevice);

    m_context->setTexture2DState(true);
    m_context->setBlendState(false);
    m_context->setLightingState(true);
    m_context->setDepthWriteState(true);
    m_context->setCullFaceState(true);
    m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

void Scene3DRenderer::renderMesh(const hkaPose* pose)
{
    if (!m_context || !m_gameModel) return;

    // Improve texture sampling defaults (studio-friendly).
    if (m_d3dDevice)
    {
        for (int s = 0; s < 4; ++s)
        {
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, m_maxAnisotropy);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, *(DWORD*)&m_mipBias);
            m_d3dDevice->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, m_gammaEnabled ? TRUE : FALSE);
        }
    }

    // Solid render state baseline.
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(true);
    m_context->setBlendState(false);
    m_context->setCullfaceMode(HKG_CULLFACE_CCW);
    m_context->setCullFaceState(true);
    m_context->setLightingState(false);

    int boneCount = 0;
    std::vector<float> boneMatrices;
    float footMinY = 0.0f;
    bool hasFootMin = false;
    float rootOffsetX = 0.0f;
    float rootOffsetY = 0.0f;
    float rootOffsetZ = 0.0f;
    if (m_rootMotionMode == ROOT_MOTION_EXTRACT)
    {
        rootOffsetX = m_rootMotionOffset(0);
        rootOffsetY = m_rootMotionOffset(1);
        rootOffsetZ = m_rootMotionOffset(2);
    }
    if (m_gameModel->skeleton)
    {
        if (pose)
        {
            boneCount = m_gameModel->skeleton->m_numBones;
            boneMatrices.resize(boneCount * 16);
            for (int b = 0; b < boneCount; b++)
            {
                pose->getBoneModelSpace(b).get4x4ColumnMajor(&boneMatrices[b * 16]);
            }
        }
        else if (m_animatedCharacter)
        {
            boneCount = m_gameModel->skeleton->m_numBones;
            boneMatrices.resize(boneCount * 16);

            hkaPose tmpPose(m_gameModel->skeleton);
            m_animatedCharacter->getPose(tmpPose);
            tmpPose.syncModelSpace();

            for (int b = 0; b < boneCount; b++)
            {
                tmpPose.getBoneModelSpace(b).get4x4ColumnMajor(&boneMatrices[b * 16]);
            }
        }
    }

    if (boneCount > 0 && !boneMatrices.empty())
    {
        for (int b = 0; b < boneCount; b++)
        {
            const hkaBone* bone = m_gameModel->skeleton->m_bones[b];
            const char* name = bone ? bone->m_name : NULL;
            if (!IsFootBoneName(name))
            {
                continue;
            }
            float y = boneMatrices[b * 16 + 13];
            if (!hasFootMin || y < footMinY)
            {
                footMinY = y;
                hasFootMin = true;
            }
        }
    }
    if (hasFootMin)
    {
        m_debugFootMinY = footMinY;
    }
    else
    {
        m_debugFootMinY = 0.0f;
    }

    // Pass 1: skin (if needed) and compute ground offset
    m_groundOffsetY = 0.0f;
    bool hasMin = false;
    float minY = 0.0f;

    for (int i = 0; i < m_gameModel->meshPartCount; i++)
    {
        GameModel::MeshPart& part = m_gameModel->meshParts[i];
        if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
        {
            continue;
        }

        const int skinBoneCount = (part.inverseBindMatrixCount < boneCount) ? part.inverseBindMatrixCount : boneCount;
        bool canSkin = (boneCount > 0 &&
                        part.skinWeights &&
                        part.inverseBindMatrices &&
                        part.skinnedPositions &&
                        part.inverseBindMatrixCount > 0 &&
                        skinBoneCount > 0);

        std::vector<float> skinMatrices;
        if (canSkin)
        {
            // FIX: Use full boneCount for matrix storage, not just skinBoneCount
            // This ensures all bones referenced by skinWeights are processed correctly
            skinMatrices.resize(boneCount * 16);
            for (int b = 0; b < skinBoneCount; b++)
            {
                MultiplyMatrix4(&boneMatrices[b * 16],
                                part.inverseBindMatrices + b * 16,
                                &skinMatrices[b * 16]);
            }
            // For bones beyond skinBoneCount, use identity-like bone matrices if boneCount > skinBoneCount
            for (int b = skinBoneCount; b < boneCount; b++)
            {
                MultiplyMatrix4(&boneMatrices[b * 16],
                                part.inverseBindMatrices,  // Use first inverse bind matrix as fallback
                                &skinMatrices[b * 16]);
            }

            for (int v = 0; v < part.vertexCount; v++)
            {
                float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
                float x = base[0];
                float y = base[1];
                float z = base[2];

                float sx = 0.0f;
                float sy = 0.0f;
                float sz = 0.0f;
                float weightSum = 0.0f;

                for (int k = 0; k < 4; k++)
                {
                    float w = part.skinWeights[v].boneWeights[k];
                    int boneIdx = part.skinWeights[v].boneIndices[k];
                    // FIX: Check against full boneCount instead of skinBoneCount
                    // so bones beyond the inverse bind matrix subset don't get discarded
                    if (w <= 0.0f || boneIdx < 0 || boneIdx >= boneCount)
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

                part.skinnedPositions[v * 3 + 0] = sx;
                part.skinnedPositions[v * 3 + 1] = sy;
                part.skinnedPositions[v * 3 + 2] = sz;
            }
        }

        if (m_groundClampMode != GROUND_CLAMP_OFF)
        {
            if (canSkin)
            {
                for (int v = 0; v < part.vertexCount; v++)
                {
                    float y = part.skinnedPositions[v * 3 + 1];
                    if (!hasMin || y < minY)
                    {
                        minY = y;
                        hasMin = true;
                    }
                }
            }
            else
            {
                for (int v = 0; v < part.vertexCount; v++)
                {
                    float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
                    float y = base[1];
                    if (!hasMin || y < minY)
                    {
                        minY = y;
                        hasMin = true;
                    }
                }
            }
        }
    }

    if (hasMin)
    {
        m_debugMeshMinY = minY;
    }
    else
    {
        m_debugMeshMinY = 0.0f;
    }

    if (m_groundClampMode != GROUND_CLAMP_OFF && hasMin)
    {
        float clampMinY = (hasFootMin ? footMinY : minY) + m_modelBaseOffsetY;
        float releaseY = (hasFootMin ? footMinY : minY) + m_modelBaseOffsetY;
        float desiredOffset = m_groundOffsetY;
        bool immediate = false;

        if (m_groundClampMode == GROUND_CLAMP_SNAP)
        {
            // Always snap lowest (foot) to Y=0
            desiredOffset = -clampMinY;
        }
        else if (m_groundClampMode == GROUND_CLAMP_BELOW)
        {
            if (clampMinY < 0.0f)
            {
                // Push up if below ground
                desiredOffset = -clampMinY;
                immediate = true;
            }
            else if (releaseY <= m_groundContactEps)
            {
                // Stick to ground when feet are very close
                desiredOffset = -clampMinY;
                immediate = true;
            }
            else if (releaseY > m_groundReleaseHeight)
            {
                // Allow jumps: release to zero when clearly airborne
                desiredOffset = 0.0f;
                immediate = true;
            }
            // else: keep current offset to avoid ground jitter
        }

        if (!immediate && m_groundSmoothFactor > 0.0f && m_groundSmoothFactor < 1.0f)
        {
            m_groundOffsetY = m_groundOffsetY + (desiredOffset - m_groundOffsetY) * m_groundSmoothFactor;
        }
        else
        {
            m_groundOffsetY = desiredOffset;
        }
    }

    // Pass 2: render opaque first, then transparent alpha surfaces.
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool transparentPass = (pass == 1);
        m_context->setDepthReadState(true);
        m_context->setDepthWriteState(!transparentPass);
        m_context->setBlendState(transparentPass);
        m_context->setCullFaceState(transparentPass ? false : true);

        for (int i = 0; i < m_gameModel->meshPartCount; i++)
        {
            GameModel::MeshPart& part = m_gameModel->meshParts[i];
            if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
            {
                continue;
            }

            // Avoid routing all alpha-channel textures through blending (many are opaque DXT formats).
            const bool isAlphaPart = ShouldRenderAsTransparentPart(part);
            if (transparentPass != isAlphaPart)
            {
                continue;
            }

            const int skinBoneCount = (part.inverseBindMatrixCount < boneCount) ? part.inverseBindMatrixCount : boneCount;
            bool canSkin = (boneCount > 0 &&
                            part.skinWeights &&
                            part.inverseBindMatrices &&
                            part.skinnedPositions &&
                            part.inverseBindMatrixCount > 0 &&
                            skinBoneCount > 0);

            // Bind texture (Havok texture pipeline)
            if (part.diffuseTexture)
            {
                m_context->setCurrentTexture(0, part.diffuseTexture, HKG_TEXTURE_MODULATE);
                m_context->setTexture2DState(true);
            }
            else
            {
                m_context->setTexture2DState(false);
            }

            const bool hasNormals = (part.vertexStride >= 32);
            const float lightDir[3] = { 0.35f, 0.75f, 0.56f }; // world-space key light
            const float ambient = 0.42f;
            const float diffuse = 0.58f;

            m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);

            for (int j = 0; j + 2 < part.indexCount; j += 3)
            {
                int idx0 = part.indices[j + 0];
                int idx1 = part.indices[j + 1];
                int idx2 = part.indices[j + 2];
                if (idx0 < 0 || idx1 < 0 || idx2 < 0 ||
                    idx0 >= part.vertexCount || idx1 >= part.vertexCount || idx2 >= part.vertexCount)
                {
                    continue;
                }

                float* v0 = (float*)((unsigned char*)part.vertices + idx0 * part.vertexStride);
                float* v1 = (float*)((unsigned char*)part.vertices + idx1 * part.vertexStride);
                float* v2 = (float*)((unsigned char*)part.vertices + idx2 * part.vertexStride);
                const float* p0 = canSkin ? (part.skinnedPositions + idx0 * 3) : v0;
                const float* p1 = canSkin ? (part.skinnedPositions + idx1 * 3) : v1;
                const float* p2 = canSkin ? (part.skinnedPositions + idx2 * 3) : v2;

                float n0[3], n1[3], n2[3];
                if (hasNormals)
                {
                    n0[0] = v0[5]; n0[1] = v0[6]; n0[2] = v0[7];
                    n1[0] = v1[5]; n1[1] = v1[6]; n1[2] = v1[7];
                    n2[0] = v2[5]; n2[1] = v2[6]; n2[2] = v2[7];
                }
                else
                {
                    float e1x = p1[0] - p0[0];
                    float e1y = p1[1] - p0[1];
                    float e1z = p1[2] - p0[2];
                    float e2x = p2[0] - p0[0];
                    float e2y = p2[1] - p0[1];
                    float e2z = p2[2] - p0[2];
                    float fnx, fny, fnz;
                    ZCross3f(fnx, fny, fnz, e1x, e1y, e1z, e2x, e2y, e2z);
                    float fl = ZLength3f(fnx, fny, fnz);
                    if (fl > 1e-6f)
                    {
                        ZNormalize3f(fnx, fny, fnz);
                    }
                    else
                    {
                        fnx = 0.0f; fny = 1.0f; fnz = 0.0f;
                    }
                    n0[0] = fnx; n0[1] = fny; n0[2] = fnz;
                    n1[0] = fnx; n1[1] = fny; n1[2] = fnz;
                    n2[0] = fnx; n2[1] = fny; n2[2] = fnz;
                }

                float i0 = ZDot3f(n0[0], n0[1], n0[2], lightDir[0], lightDir[1], lightDir[2]);
                float i1 = ZDot3f(n1[0], n1[1], n1[2], lightDir[0], lightDir[1], lightDir[2]);
                float i2 = ZDot3f(n2[0], n2[1], n2[2], lightDir[0], lightDir[1], lightDir[2]);
                if (i0 < 0.0f) i0 = 0.0f; if (i0 > 1.0f) i0 = 1.0f;
                if (i1 < 0.0f) i1 = 0.0f; if (i1 > 1.0f) i1 = 1.0f;
                if (i2 < 0.0f) i2 = 0.0f; if (i2 > 1.0f) i2 = 1.0f;
                float c0[4] = { ambient + diffuse * i0, ambient + diffuse * i0, ambient + diffuse * i0, 1.0f };
                float c1[4] = { ambient + diffuse * i1, ambient + diffuse * i1, ambient + diffuse * i1, 1.0f };
                float c2[4] = { ambient + diffuse * i2, ambient + diffuse * i2, ambient + diffuse * i2, 1.0f };

                if (part.vertexStride >= 20)
                {
                    float uv0[2] = { v0[3], v0[4] };
                    float uv1[2] = { v1[3], v1[4] };
                    float uv2[2] = { v2[3], v2[4] };

                    float yOffset = m_modelBaseOffsetY + m_groundOffsetY + rootOffsetY;
                    float pp0[3] = { p0[0] + rootOffsetX, p0[1] + yOffset, p0[2] + rootOffsetZ };
                    float pp1[3] = { p1[0] + rootOffsetX, p1[1] + yOffset, p1[2] + rootOffsetZ };
                    float pp2[3] = { p2[0] + rootOffsetX, p2[1] + yOffset, p2[2] + rootOffsetZ };

                    m_context->setCurrentColor4(c0);
                    m_context->setCurrentNormal(n0);
                    m_context->setCurrentTextureCoord(uv0);
                    m_context->setCurrentPosition(pp0);

                    m_context->setCurrentColor4(c1);
                    m_context->setCurrentNormal(n1);
                    m_context->setCurrentTextureCoord(uv1);
                    m_context->setCurrentPosition(pp1);

                    m_context->setCurrentColor4(c2);
                    m_context->setCurrentNormal(n2);
                    m_context->setCurrentTextureCoord(uv2);
                    m_context->setCurrentPosition(pp2);
                }
                else
                {
                    float uv[2] = { 0.0f, 0.0f };
                    m_context->setCurrentTextureCoord(uv);
                    float yOffset = m_modelBaseOffsetY + m_groundOffsetY + rootOffsetY;
                    float pp0[3] = { p0[0] + rootOffsetX, p0[1] + yOffset, p0[2] + rootOffsetZ };
                    float pp1[3] = { p1[0] + rootOffsetX, p1[1] + yOffset, p1[2] + rootOffsetZ };
                    float pp2[3] = { p2[0] + rootOffsetX, p2[1] + yOffset, p2[2] + rootOffsetZ };
                    m_context->setCurrentColor4(c0);
                    m_context->setCurrentNormal(n0);
                    m_context->setCurrentPosition(pp0);
                    m_context->setCurrentColor4(c1);
                    m_context->setCurrentNormal(n1);
                    m_context->setCurrentPosition(pp1);
                    m_context->setCurrentColor4(c2);
                    m_context->setCurrentNormal(n2);
                    m_context->setCurrentPosition(pp2);
                }
            }

            m_context->endGroup();
        }
    }

    m_context->flush();
    m_context->setBlendState(false);
    m_context->setDepthWriteState(true);
    m_context->setCullFaceState(false);
}



void Scene3DRenderer::computeModelBaseOffsetFromReference()
{
    if (!m_gameModel || !m_gameModel->skeleton)
    {
        return;
    }

    hkaPose refPose(m_gameModel->skeleton);
    refPose.setToReferencePose();
    refPose.syncModelSpace();

    int boneCount = m_gameModel->skeleton->m_numBones;
    std::vector<float> boneMatrices;
    boneMatrices.resize(boneCount * 16);
    for (int b = 0; b < boneCount; b++)
    {
        refPose.getBoneModelSpace(b).get4x4ColumnMajor(&boneMatrices[b * 16]);
    }

    bool hasFootMin = false;
    float footMinY = 0.0f;
    for (int b = 0; b < boneCount; b++)
    {
        const hkaBone* bone = m_gameModel->skeleton->m_bones[b];
        const char* name = bone ? bone->m_name : NULL;
        if (!IsFootBoneName(name))
        {
            continue;
        }

        float pos[3];
        refPose.getBoneModelSpace(b).getTranslation().store3(pos);
        float y = pos[1];
        if (!hasFootMin || y < footMinY)
        {
            footMinY = y;
            hasFootMin = true;
        }
    }

    bool hasMin = false;
    float minY = 0.0f;

    for (int i = 0; i < m_gameModel->meshPartCount; i++)
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
                        part.skinnedPositions &&
                        part.inverseBindMatrixCount > 0 &&
                        skinBoneCount > 0);

        if (canSkin)
        {
            std::vector<float> skinMatrices;
            skinMatrices.resize(skinBoneCount * 16);
            for (int b = 0; b < skinBoneCount; b++)
            {
                MultiplyMatrix4(&boneMatrices[b * 16],
                                part.inverseBindMatrices + b * 16,
                                &skinMatrices[b * 16]);
            }

            for (int v = 0; v < part.vertexCount; v++)
            {
                float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
                float x = base[0];
                float y = base[1];
                float z = base[2];

                float sy = 0.0f;
                float weightSum = 0.0f;

                for (int k = 0; k < 4; k++)
                {
                    float w = part.skinWeights[v].boneWeights[k];
                    int boneIdx = part.skinWeights[v].boneIndices[k];
                    if (w <= 0.0f || boneIdx < 0 || boneIdx >= skinBoneCount)
                        continue;

                    const float* m = &skinMatrices[boneIdx * 16];
                    sy += w * (m[1] * x + m[5] * y + m[9] * z + m[13]);
                    weightSum += w;
                }

                if (weightSum < 0.999f)
                {
                    float inv = 1.0f - weightSum;
                    sy += y * inv;
                }

                if (!hasMin || sy < minY)
                {
                    minY = sy;
                    hasMin = true;
                }
            }
        }
        else
        {
            for (int v = 0; v < part.vertexCount; v++)
            {
                float* base = (float*)((unsigned char*)part.vertices + v * part.vertexStride);
                float y = base[1];
                if (!hasMin || y < minY)
                {
                    minY = y;
                    hasMin = true;
                }
            }
        }
    }

    if (hasFootMin)
    {
        m_modelBaseOffsetY = -footMinY;
    }
    else if (hasMin)
    {
        m_modelBaseOffsetY = -minY;
    }
    else
    {
        m_modelBaseOffsetY = 0.0f;
    }

    m_modelBaseComputed = true;
}

// Particle effects testing methods
void Scene3DRenderer::loadTestEffect(const char* jsonPath, const char* textureDir)
{
    if (!m_effectManager) return;
    m_effectManager->loadEffectDefinition(jsonPath, textureDir);
}

void Scene3DRenderer::spawnTestEffect(const char* effectName, const hkVector4& position)
{
    if (!m_effectManager) return;

    // Heuristic: effects authored as "*_Camera" are meant to follow the camera (spawn volume around view).
    // The game attaches these to the camera rig; we emulate that by updating the effect position each frame.
    if (effectName && _stricmp(effectName, "") != 0)
    {
        const char* p = effectName;
        while (*p)
        {
            if (_strnicmp(p, "_camera", 7) == 0)
            {
                hkVector4 camPos = getCameraPosition();
                ActiveEffect* e = m_effectManager->spawnEffect(effectName, camPos);
                if (e)
                {
                    e->boneIndex = -2; // follow camera
                }
                return;
            }
            ++p;
        }
    }

    m_effectManager->spawnEffect(effectName, position);
}

void Scene3DRenderer::clearAllEffects()
{
    if (!m_effectManager) return;
    m_effectManager->clearAll();
}

// ============================================================================
// Animation Event Processing — implementations moved to Scene3DAnimation.cpp
// ============================================================================


