// LevelScene.cpp — Building 3D Worlds From Binary Corpses
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Takes the parsed LevelReader data and turns it into actual renderable
// D3D9 geometry. For every mesh instance in the level: look up ModelInfo
// → find VBuffInfo/IBuffInfo → extract raw vertex/index bytes from Block2
// → decode the vertex format (position, normal, UV, tangent, bone weights)
// → create IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9 → load DDS
// textures from BIN asset data → apply the 4x4 WorldTransform from the
// GameObjs block → render. Thousands of instances. Per level. All of
// them placed by Pandemic designers who are now scattered across the
// industry (or retired, or who the fuck knows). Their work materializes
// on screen every time this code runs. It's not archaeology. It's
// necromancy. We're not studying the dead. We're making them move.
// -----------------------------------------------------------------------

#include "LevelScene.h"
#include "LevelReader.h"
#include "ZeroMath.h"
#include "GameShaderCache.h"
#include "SplineEditor.h"
#include "GpuFilterCaps.h"

#include <d3d9.h>
#include <d3dx9.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <utility>
#include <windows.h>

#define LEVEL_VERTEX_FVF  (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define LEVEL_VERTEX_SIZE (sizeof(LevelVertex))   // 32 bytes

// ── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_levelLog = NULL;
static bool s_renderLogOnce = false;

// Per-mesh-part material flags — stored here (not in header structs) to avoid
// sizeof changes that break other .obj files.
// Key = (model_ptr, part_index) -> flags: bit0=transparent, bit1=two-sided, bit2=water, bit3=lava, bit4=terrain
static std::map< std::pair<const void*,int>, uint32_t > g_partFlags;

static uint32_t GetPartFlags(const LevelModel* m, int pi) {
    std::map< std::pair<const void*,int>, uint32_t >::iterator it =
        g_partFlags.find(std::make_pair((const void*)m, pi));
    return (it != g_partFlags.end()) ? it->second : 0u;
}
static void LevelLog(const char* msg)
{
    if (!g_levelLog) g_levelLog = fopen("level_scene.log", "w");
    if (g_levelLog) { fprintf(g_levelLog, "%s\n", msg); fflush(g_levelLog); }
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
}
static void LevelLogf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    buf[sizeof(buf)-1] = '\0';
    va_end(ap);
    LevelLog(buf);
}

// =============================================================================
//  Normal decode helpers
// =============================================================================

static void UnpackNormal(uint32_t packed, float& nx, float& ny, float& nz)
{
    nx = ((packed        & 0xFF) / 127.5f) - 1.0f;
    ny = (((packed >>  8) & 0xFF) / 127.5f) - 1.0f;
    nz = (((packed >> 16) & 0xFF) / 127.5f) - 1.0f;
    ZNormalize3f(nx, ny, nz);  // FUN_004068d0: rsqrtss + Newton-Raphson
}

// =============================================================================
//  LevelModel
// =============================================================================

void LevelModel::release()
{
    for (int i = 0; i < (int)parts.size(); ++i)
    {
        if (parts[i].vb) { parts[i].vb->Release(); parts[i].vb = NULL; }
        if (parts[i].ib) { parts[i].ib->Release(); parts[i].ib = NULL; }
    }
    parts.clear();
}

// =============================================================================
//  4x4 matrix inverse via cofactor expansion (row-major)
// =============================================================================
// Mirrors level_patcher.py::_mat_inverse exactly so C++ preview and Python
// bake agree on bind-matrix arithmetic to floating-point precision. Used by
// LevelModel::authoritativeBindWorld() below to invert the Pandemic-shipped
// inverse-bind matrices back into bind-pose world matrices.
//
// Singular input (det near 0) writes identity into `out` and returns false,
// matching the Python fallback. Caller should fall back gracefully.
static bool mat4_inverse_rm(const float m[16], float out[16])
{
    const float m00 = m[ 0], m01 = m[ 1], m02 = m[ 2], m03 = m[ 3];
    const float m10 = m[ 4], m11 = m[ 5], m12 = m[ 6], m13 = m[ 7];
    const float m20 = m[ 8], m21 = m[ 9], m22 = m[10], m23 = m[11];
    const float m30 = m[12], m31 = m[13], m32 = m[14], m33 = m[15];

    float inv[16];
    inv[ 0] =  m11*m22*m33 - m11*m23*m32 - m21*m12*m33 + m21*m13*m32 + m31*m12*m23 - m31*m13*m22;
    inv[ 4] = -m10*m22*m33 + m10*m23*m32 + m20*m12*m33 - m20*m13*m32 - m30*m12*m23 + m30*m13*m22;
    inv[ 8] =  m10*m21*m33 - m10*m23*m31 - m20*m11*m33 + m20*m13*m31 + m30*m11*m23 - m30*m13*m21;
    inv[12] = -m10*m21*m32 + m10*m22*m31 + m20*m11*m32 - m20*m12*m31 - m30*m11*m22 + m30*m12*m21;
    inv[ 1] = -m01*m22*m33 + m01*m23*m32 + m21*m02*m33 - m21*m03*m32 - m31*m02*m23 + m31*m03*m22;
    inv[ 5] =  m00*m22*m33 - m00*m23*m32 - m20*m02*m33 + m20*m03*m32 + m30*m02*m23 - m30*m03*m22;
    inv[ 9] = -m00*m21*m33 + m00*m23*m31 + m20*m01*m33 - m20*m03*m31 - m30*m01*m23 + m30*m03*m21;
    inv[13] =  m00*m21*m32 - m00*m22*m31 - m20*m01*m32 + m20*m02*m31 + m30*m01*m22 - m30*m02*m21;
    inv[ 2] =  m01*m12*m33 - m01*m13*m32 - m11*m02*m33 + m11*m03*m32 + m31*m02*m13 - m31*m03*m12;
    inv[ 6] = -m00*m12*m33 + m00*m13*m32 + m10*m02*m33 - m10*m03*m32 - m30*m02*m13 + m30*m03*m12;
    inv[10] =  m00*m11*m33 - m00*m13*m31 - m10*m01*m33 + m10*m03*m31 + m30*m01*m13 - m30*m03*m11;
    inv[14] = -m00*m11*m32 + m00*m12*m31 + m10*m01*m32 - m10*m02*m31 - m30*m01*m12 + m30*m02*m11;
    inv[ 3] = -m01*m12*m23 + m01*m13*m22 + m11*m02*m23 - m11*m03*m22 - m21*m02*m13 + m21*m03*m12;
    inv[ 7] =  m00*m12*m23 - m00*m13*m22 - m10*m02*m23 + m10*m03*m22 + m20*m02*m13 - m20*m03*m12;
    inv[11] = -m00*m11*m23 + m00*m13*m21 + m10*m01*m23 - m10*m03*m21 - m20*m01*m13 + m20*m03*m11;
    inv[15] =  m00*m11*m22 - m00*m12*m21 - m10*m01*m22 + m10*m02*m21 + m20*m01*m12 - m20*m02*m11;

    float det = m00*inv[0] + m01*inv[4] + m02*inv[8] + m03*inv[12];
    if (det > -1e-12f && det < 1e-12f)
    {
        for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return false;
    }
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
    return true;
}

// =============================================================================
//  LevelModel::authoritativeBindWorld
// =============================================================================
// Return the bind-pose world matrix for `boneIdx` using the PAK's
// authoritative skin_binds when the bone is in the skin palette, falling
// back to restBoneWorld (the cascade product) otherwise. Identity if the
// bone is unknown to both. Matches level_patcher.py:1816-1844 exactly so
// the C++ live preview and the Python bake agree on positioning.
bool LevelModel::authoritativeBindWorld(int boneIdx, float outMat16[16]) const
{
    if (boneIdx < 0)
    {
        for (int i = 0; i < 16; ++i) outMat16[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        return false;
    }

    // Search skin palette for this bone. Linear scan, palette is small
    // (typically <100 entries).
    for (size_t i = 0; i < skinOrder.size(); ++i)
    {
        if (skinOrder[i] != (uint32_t)boneIdx) continue;

        // Found. Invert the stored inverse-bind to get the authoritative
        // bind-pose world matrix. This is what Pandemic's content build
        // tool actually shipped, and what the runtime uses every frame.
        if (i * 16 + 16 > skinBindsRowMajor.size())
        {
            // Defensive: palette index in range but bind data short. Bail.
            break;
        }
        const float* invBind = &skinBindsRowMajor[i * 16];
        return mat4_inverse_rm(invBind, outMat16);
    }

    // Not in palette: cascade fallback. The cascade can drift from
    // Pandemic's authority but is the best we've got for non-skinned bones.
    if (!restBoneWorld.empty() && (size_t)(boneIdx * 16 + 16) <= restBoneWorld.size())
    {
        memcpy(outMat16, &restBoneWorld[boneIdx * 16], 16 * sizeof(float));
        return true;
    }

    // Bone unknown to both paths. Identity is the safest fallback.
    for (int i = 0; i < 16; ++i) outMat16[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    return false;
}

void LevelWorldCollisionMesh::release()
{
    if (vb) { vb->Release(); vb = NULL; }
    if (ib) { ib->Release(); ib = NULL; }
}

// =============================================================================
//  LevelScene
// =============================================================================

LevelScene::LevelScene()
    : m_device(NULL), m_editorFont(NULL), m_loaded(false), m_drawCallsLast(0),
      m_boundsValid(false), m_showEditorObjs(true),
      m_levelVS(NULL), m_levelPS(NULL), m_levelDecl(NULL), m_whiteTex(NULL), m_flatNormalTex(NULL),
      m_shadowMapTex(NULL), m_shadowMapSurf(NULL), m_shadowMapDS(NULL),
      m_shadowVS(NULL), m_shadowPS(NULL), m_shadowsReady(false),
      m_sceneRT(NULL), m_sceneRTSurf(NULL), m_bloomRT(NULL), m_bloomRTSurf(NULL),
      m_bloomRT2(NULL), m_bloomRT2Surf(NULL), m_brightPassPS(NULL), m_blurPS(NULL),
      m_toneMapPS(NULL), m_postVS(NULL), m_postDecl(NULL), m_postReady(false),
      m_sceneRTWidth(0), m_sceneRTHeight(0), m_gameShaders(NULL), m_bridgeVS(NULL), m_bridgeVS_AN(NULL),
      m_gamePS_A(NULL), m_gamePS_AN(NULL), m_gamePS_AN_Dn(NULL), m_gamePS_strauss_AN(NULL), m_gamePS_strauss_ANS(NULL),
      m_ambientCube(NULL),
      m_shadersReady(false), m_shaderInitAttempted(false), m_lightingExtracted(false),
      m_hoveredIdx(-1), m_selectedIdx(-1), m_gameModeFilter(-1), m_gmfShowGlobals(true), m_gmfShowScripts(false), m_gmfBitMask(0xFFFFFFFF),
      m_editorObjMaxDist(0.0f), m_editorObjFadeStart(0.0f),
      m_editorObjMinDist(0.0f), m_editorObjVisibleCount(0), m_editorObjCategoryMask(0x1FFFu),
      m_editorObjSearchMode(0), m_editorObjLabelMaxCount(0),
      m_hoveredEditorIdx(-1), m_selectedEditorIdx(-1),
      m_showCollisions(false),
      m_showPathLinks(true), m_showCaptureRadii(true), m_showSpawnChains(true),
      // The other agent forgot these two when they added the Phase 3-7 viz
      // work. Without init, the bools read whatever shit was on the heap,
      // which on a release build would MOST of the time be zero but on a
      // debug build with some heap poisoning fills would be 0xCD or
      // 0xCCCCCCCC - both of which cast to `true`. Meaning a fresh
      // LevelScene in debug mode would attempt to render AI-goal labels
      // and sound-emitter circles BEFORE load() ever populated the
      // m_go* caches it reads from. Empty maps, but still a bunch of
      // pointless work and one random frame where the result depends on
      // allocator state. Initialize the motherfuckers.
      m_showAIGoals(true), m_showSoundRadii(true),
      // Phase 1 XSI authoring: event wires default-on so the user sees
      // the wiring overlay the second they hit F4. Focus GUID 0 means
      // "draw all of them", non-zero means "isolate this one chain".
      m_showEventWires(true), m_eventWireFocusGuid(0),
      // Phase 3 XSI authoring: layer manager. Both layer states and
      // both isolated/active GUIDs default to "no opinion": no layers
      // hidden, no layers locked, no isolation, no active-layer
      // selection. The DLL panel writes into these as the user works.
      m_isolatedLayer(0), m_activeLayer(0),
      // Phase 4 XSI authoring: viewport gizmo. Mode starts off so the
      // user has to opt in via the gizmo toolbar, otherwise the new
      // axis arrows just clutter every selection. World space default.
      m_gizmoMode(0), m_gizmoSpace(0), m_gizmoActiveAxis(0),
      m_gizmoDragging(false), m_gizmoDragStartProj(0.0f),
      m_gizmoDragStartAngle(0.0f), m_gizmoDragStartDist(1.0f),
      m_gizmoSnap(0.0f), m_gizmoTargetKind(0)
{
    memset(m_gizmoStartMat, 0, sizeof(m_gizmoStartMat));
    memset(&m_lighting, 0, sizeof(m_lighting));
    // Sensible defaults so the viewport doesn't render pure-black entities when
    // a level isn't loaded (Crowd Mesh Builder preview, model inspector etc).
    // Real level data overrides these in extractLighting() once a level loads.
    m_lighting.sunDir[0]      = 0.3f;  m_lighting.sunDir[1] = -0.8f; m_lighting.sunDir[2] = 0.5f; m_lighting.sunDir[3] = 0.0f;
    m_lighting.sunCol[0]      = 1.0f;  m_lighting.sunCol[1] =  1.0f; m_lighting.sunCol[2] = 1.0f; m_lighting.sunCol[3] = 1.0f;
    m_lighting.topDomeCol[0]  = 0.55f; m_lighting.topDomeCol[1] = 0.6f; m_lighting.topDomeCol[2] = 0.7f; m_lighting.topDomeCol[3] = 1.0f;
    m_lighting.botDomeCol[0]  = 0.25f; m_lighting.botDomeCol[1] = 0.22f; m_lighting.botDomeCol[2] = 0.18f; m_lighting.botDomeCol[3] = 1.0f;
    m_lighting.inscatterCol[0] = 0.0f; m_lighting.inscatterCol[1] = 0.0f; m_lighting.inscatterCol[2] = 0.0f; m_lighting.inscatterCol[3] = 0.0f;
    memset(m_cachedVP, 0, sizeof(m_cachedVP));
    memset(m_editorObjSearchTerm, 0, sizeof(m_editorObjSearchTerm));
    // Identity 3x3
    memset(m_levelRot, 0, sizeof(m_levelRot));
    m_levelRot[0] = m_levelRot[4] = m_levelRot[8] = 1.0f;
    // Body-init the trailing crowd-authoring members per the
    // "don't init new members in the init list — declaration-order
    // warnings on partial recompiles" policy. crowdDirty starts false
    // because Load() repopulates it from the binary, and a freshly-
    // constructed LevelScene with nothing loaded is by definition clean.
    m_crowdDirty = false;
    m_crowdFocusModeOn = false;
    m_crowdFocusedItem = -1;
}

LevelScene::~LevelScene() { unload(); destroyShaders(); }

void LevelScene::setDevice(IDirect3DDevice9* device) { m_device = device; }

// =============================================================================
//  HLSL shader source — game-accurate, based on D3DXDisassembleShader output
//  of all 2298 compiled .vso/.pso shader bytecodes (SM3.0).
//
//  VS: Mg_VP_Lit_WPos_VNorm (world transform + viewProj, outputs wpos/normal/uv/vcol)
//  PS: Mg_FP_Lit_A_Vd_Ao_WPos_Shdw_VNorm_VtxAtm (Lambert + AO, no shadow/cubemap)
//      + MgFP_ScreenScattering_ (Rayleigh+Mie scattering, applied inline)
//
//  Game register layout (decompiled from CTAB):
//    VS: c178-c181=g__worldMatrix, c239-c242=g__viewProjMatrix
//    PS: c1=g__ambient, c2=g__sunCol, c3=g__sunDir, c5=g__diffColor,
//        c180-c188=scattering constants, c200=g__ambientOcclusionParms
//  We remap to c0-c9 since we set our own constants.
// =============================================================================

static const char s_levelVS[] =
    "float4x4 g_World    : register(c0);\n"  // game: c178-c181
    "float4x4 g_ViewProj : register(c4);\n"  // game: c239-c242
    "\n"
    "struct VS_OUT {\n"
    "  float4 pos  : POSITION;\n"
    "  float3 nrm  : TEXCOORD0;\n"   // world normal
    "  float4 wpos : TEXCOORD1;\n"   // world position
    "  float2 uv   : TEXCOORD2;\n"   // texture coords
    "  float4 vcol : TEXCOORD3;\n"   // vertex color (AO/baked lighting)
    "  float4 tang : TEXCOORD4;\n"   // world tangent xyz + handedness w
    "};\n"
    "\n"
    "VS_OUT main(float3 p : POSITION, float3 n : NORMAL,\n"
    "            float4 c : COLOR0, float2 t : TEXCOORD0,\n"
    "            float4 tan : TANGENT) {\n"
    "  VS_OUT o;\n"
    "  float4 wp = mul(g_World, float4(p, 1.0));\n"
    "  o.pos  = mul(g_ViewProj, wp);\n"
    "  o.nrm  = mul((float3x3)g_World, n);\n"
    "  o.wpos = wp;\n"
    "  o.uv   = t;\n"
    "  o.vcol = c;\n"
    "  o.tang = float4(mul((float3x3)g_World, tan.xyz), tan.w);\n"
    "  return o;\n"
    "}\n";

// Game-accurate pixel shader based on disassembled bytecode:
//   Per-object lighting:  Mg_FP_Lit_A_Vd_Ao_WPos_Shdw_VNorm_VtxAtm.pso
//   Scattering math:      MgFP_ScreenScattering_.pso (applied inline)
//
// Removed non-game effects: wrap lighting, dome glow, Blinn-Phong specular,
// Reinhard tone mapping, contrast grading (none of these exist in the game's
// per-object pixel shader — confirmed by disassembly of all 1665 PS).
static const char s_levelPS[] =
    "sampler2D g_Tex       : register(s0);\n"
    "sampler2D g_NormalMap : register(s1);\n"
    "sampler2D g_SpecMap   : register(s2);\n"
    "\n"
    "float4 g_Ambient    : register(c0);\n"  // .rgb = ambient color * scale
    "float4 g_SunCol     : register(c1);\n"  // .rgb = sun color * colorScale
    "float4 g_SunDir     : register(c2);\n"  // .xyz = sun direction (toward light)
    "float4 g_DiffColor  : register(c3);\n"  // material diffuse + alpha
    "float4 g_CamPos     : register(c4);\n"  // camera world position
    "float4 g_ScatParams : register(c5);\n"  // x=density, y=heightFalloff, z=hazeDensity, w=optDepthScale
    "float4 g_InscatCol  : register(c6);\n"  // rgb=inscatter color, w=inscatter mult
    "float4 g_ExtinctCol : register(c7);\n"  // rgb=extinction tint, w=extinction mult
    "float4 g_ScatHG     : register(c8);\n"  // x=(1-g^2), y=(1+g^2), z=(-2g)
    "float4 g_MiscParams : register(c9);\n"  // x=aoScale, y=aoBias, z=rayleighStr, w=mieStr
    "float4 g_MapFlags   : register(c10);\n" // x=hasNormalMap, y=hasSpecMap, z=normalStr
    "float4 g_Sun2Col    : register(c11);\n" // secondary light color (w=hasSun2)
    "float4 g_Sun2Dir    : register(c12);\n" // secondary light direction
    "float4x4 g_LightVP  : register(c13);\n" // light ViewProj for shadow mapping
    "float4 g_ShadowParams : register(c17);\n" // x=hasShadow, y=bias, z=texelSize, w=minShadow
    "sampler2D g_ShadowMap : register(s3);\n" // shadow depth map
    "sampler2D g_DetailMap : register(s4);\n" // detail/grunge texture (Mat2 tex1)
    "sampler2D g_EmissiveMap : register(s5);\n" // emissive glow texture (tex5)
    "float4 g_EmissiveParams : register(c18);\n" // x=hasEmissive, y=emissiveStrength
    "float4 g_WaterParams : register(c19);\n"   // x=isWater, y=time, z=scrollSpeed, w=opacity
    "float4 g_TopDomeCol  : register(c20);\n"  // hemisphere ambient: sky color (top)
    "float4 g_BotDomeCol  : register(c21);\n"  // hemisphere ambient: ground bounce (bottom)
    "float4 g_TerrainParams : register(c22);\n" // x=isTerrain, y=tileScale
    "sampler2D g_TerrainL2 : register(s6);\n"  // terrain layer 2 diffuse
    "sampler2D g_TerrainL3 : register(s7);\n"  // terrain layer 3 diffuse
    "sampler2D g_TerrainM0 : register(s8);\n"  // terrain blend mask 0
    "sampler2D g_TerrainM1 : register(s9);\n"  // terrain blend mask 1
    "sampler2D g_TerrainM2 : register(s10);\n" // terrain blend mask 2
    "\n"
    "float4 main(\n"
    "  float3 nrm  : TEXCOORD0,\n"
    "  float4 wpos : TEXCOORD1,\n"
    "  float2 uv   : TEXCOORD2,\n"
    "  float4 vcol : TEXCOORD3,\n"
    "  float4 tang : TEXCOORD4\n"
    ") : COLOR {\n"
    "  float3 N = normalize(nrm);\n"
    "\n"
    // Water UV animation: scroll two layers at different speeds/directions
    "  float2 texUV = uv;\n"
    "  if (g_WaterParams.x > 0.5) {\n"
    "    float t = g_WaterParams.y;\n"
    "    float spd = g_WaterParams.z;\n"
    // Layer 1: scroll diagonally
    "    texUV = uv + float2(t * spd * 0.7, t * spd * 0.3);\n"
    "  }\n"
    "  float4 tex = tex2D(g_Tex, texUV);\n"
    // Water second layer blend
    "  if (g_WaterParams.x > 0.5) {\n"
    "    float t = g_WaterParams.y;\n"
    "    float spd = g_WaterParams.z;\n"
    "    float2 uv2 = uv + float2(-t * spd * 0.4, t * spd * 0.6);\n"
    "    float4 tex2layer = tex2D(g_Tex, uv2);\n"
    "    tex.rgb = lerp(tex.rgb, tex2layer.rgb, 0.5);\n"
    "  }\n"
    "  float alpha = tex.a * g_DiffColor.a;\n"
    "\n"
    // ── Normal mapping (derivative tangent frame, no vertex format change) ──
    // ── Normal mapping (vertex tangent frame from decoded data) ──
    "  if (g_MapFlags.x > 0.5) {\n"
    "    float3 T = normalize(tang.xyz);\n"
    "    float3 B = normalize(cross(N, T) * tang.w);\n"
    "    float3 tanN = tex2D(g_NormalMap, texUV).xyz * 2.0 - 1.0;\n"
    "    float nStr = g_MapFlags.z;\n"
    "    tanN.xy *= nStr;\n"
    "    tanN.z = sqrt(max(1.0 - tanN.x*tanN.x - tanN.y*tanN.y, 0.0));\n"
    "    float3 mapN = normalize(tanN.x * T + tanN.y * B + tanN.z * N);\n"
    "    if (dot(mapN, N) < 0.0) mapN = N;\n"
    "    N = mapN;\n"
    "  }\n"
    "\n"
    // ── Game-accurate per-object lighting (Mg_FP_Lit) ──
    "  float3 albedo = tex.rgb * g_DiffColor.rgb * vcol.rgb;\n"
    // Detail/grunge texture overlay (Mat2 materials — tiled at 4x UV frequency)
    "  if (g_MapFlags.w > 0.5) {\n"
    "    float3 detail = tex2D(g_DetailMap, texUV * 4.0).rgb;\n"
    "    albedo *= detail * 2.0;\n"
    "  }\n"
    // Terrain multi-layer blending (Mat4, kind=1)
    // 3 tiled texture layers blended by per-chunk mask textures
    "  if (g_TerrainParams.x > 0.5) {\n"
    "    float tileScale = g_TerrainParams.y;\n"
    "    float2 tileUV = wpos.xz * tileScale;\n" // world-space tiling
    "    float3 layer1 = tex.rgb;\n" // already sampled from tex0
    "    float3 layer2 = tex2D(g_TerrainL2, tileUV).rgb;\n"
    "    float3 layer3 = tex2D(g_TerrainL3, tileUV).rgb;\n"
    // Masks use the mesh UV (maps 1:1 to chunk)
    "    float w0 = tex2D(g_TerrainM0, uv).r;\n"
    "    float w1 = tex2D(g_TerrainM1, uv).r;\n"
    "    float w2 = tex2D(g_TerrainM2, uv).r;\n"
    // Blend layers by mask weights
    "    float wTotal = w0 + w1 + w2;\n"
    "    if (wTotal > 0.001) {\n"
    "      albedo = (layer1 * w0 + layer2 * w1 + layer3 * w2) / wTotal;\n"
    "    }\n"
    "    albedo *= vcol.rgb;\n" // vertex color tint
    "  }\n"
    "  float ao = saturate(vcol.a * g_MiscParams.x + g_MiscParams.y);\n"
    "\n"
    // Primary sun (Lambert)
    "  float NdL = saturate(dot(N, g_SunDir.xyz));\n"
    "\n"
    // Shadow map sampling (PCF 2x2, symmetric kernel).
    //
    // The old kernel was rookie-tier horseshit: tap at (0,0) /(ts,0)
    // /(0,ts) /(ts,ts), which is a 2x2 grid anchored at the UPPER-LEFT
    // of the sample point. Read that pattern again. Three of the four
    // taps are on one side. Penumbras drift one diagonal direction by
    // half a texel and every fucking shadow edge picks up a directional
    // fringe. On Helm's Deep walls the fringe reads as cool bleed
    // because the dome ambient is blue (separate disease, separate
    // bandage). Centering this kernel doesn't fix the dome shit but it
    // kills the goddamn directional drift.
    //
    // Center the kernel at suv. Distribute taps across all four
    // quadrants. Same fucking cost, no bias. This is the textbook fix
    // and the original code just shipped without doing the math.
    "  float shadow = 1.0;\n"
    "  if (g_ShadowParams.x > 0.5) {\n"
    "    float4 lpos = mul(g_LightVP, wpos);\n"
    "    float2 suv = lpos.xy / lpos.w * 0.5 + 0.5;\n"
    "    suv.y = 1.0 - suv.y;\n"
    "    float depth = lpos.z / lpos.w;\n"
    "    float bias = g_ShadowParams.y;\n"
    "    float th = g_ShadowParams.z * 0.5;\n"
    "    if (suv.x > 0.0 && suv.x < 1.0 && suv.y > 0.0 && suv.y < 1.0) {\n"
    "      float s0 = (tex2D(g_ShadowMap, suv + float2(-th,-th)).r + bias < depth) ? 0.0 : 1.0;\n"
    "      float s1 = (tex2D(g_ShadowMap, suv + float2( th,-th)).r + bias < depth) ? 0.0 : 1.0;\n"
    "      float s2 = (tex2D(g_ShadowMap, suv + float2(-th, th)).r + bias < depth) ? 0.0 : 1.0;\n"
    "      float s3 = (tex2D(g_ShadowMap, suv + float2( th, th)).r + bias < depth) ? 0.0 : 1.0;\n"
    "      shadow = (s0 + s1 + s2 + s3) * 0.25;\n"
    "      shadow = lerp(g_ShadowParams.w, 1.0, shadow);\n"
    "    }\n"
    "  }\n"
    "\n"
    "  float3 Qa = albedo * g_Ambient.rgb * ao;\n"
    "  float3 Qd = albedo * NdL * shadow;\n"
    "  float3 Ir = g_SunCol.rgb * Qd + Qa;\n"
    "\n"
    // Secondary light (fill / back light)
    "  if (g_Sun2Col.w > 0.5) {\n"
    "    float NdL2 = saturate(dot(N, g_Sun2Dir.xyz));\n"
    "    Ir += g_Sun2Col.rgb * albedo * NdL2;\n"
    "  }\n"
    "\n"
    // ── Specular with Fresnel (Schlick approximation, metallic surfaces) ──
    //
    // The spec map does double duty: its luminance picks the BRDF power
    // (broad-rough vs tight-mirror) AND it scales the final spec
    // contribution. The original code skipped step 2. So dielectric
    // F0=0.04 plus the Fresnel rim term got slapped on every leather,
    // cloth, dirt, and unpainted-wood texture in the entire fucking
    // game, putting a cheap plastic sheen on every Gondor cloak and
    // every Uruk-hai loincloth. Whole army looking like wet rubber.
    //
    // Map luminance IS the material's spec strength. Respect that, you
    // motherfucker. Multiply the spec contribution by saturate(specLum
    // + small floor). The floor keeps polished metal rimming properly
    // when its map happens to read dim. Cloth stays matte. Metal stays
    // shiny. The shader stops lying.
    "  if (g_MapFlags.y > 0.5) {\n"
    "    float3 V = normalize(g_CamPos.xyz - wpos.xyz);\n"
    "    float3 H = normalize(g_SunDir.xyz + V);\n"
    "    float NdH = saturate(dot(N, H));\n"
    "    float VdH = saturate(dot(V, H));\n"
    "    float3 specMap = tex2D(g_SpecMap, uv).rgb;\n"
    "    float specLum = dot(specMap, float3(0.3, 0.6, 0.1));\n"
    "    float specPow = lerp(40.0, 256.0, specLum);\n"
    "    float spec = pow(NdH, specPow) * NdL;\n"
    // Schlick Fresnel: F = F0 + (1-F0)(1-VdH)^5
    // F0 from spec map — metals have colored F0, dielectrics ~0.04.
    "    float3 F0 = lerp(float3(0.04,0.04,0.04), specMap, specLum);\n"
    "    float3 fresnel = F0 + (1.0 - F0) * pow(1.0 - VdH, 5.0);\n"
    // Material-strength gate. Low-spec textures eat the L. Small floor
    // keeps polished metal (specLum near 1) and gilded edges rimming
    // properly when their map happens to read dim. Metal stays metal,
    // leather stays leather. The shader stops embarrassing itself.
    "    float specStrength = saturate(specLum + 0.05);\n"
    "    Ir += g_SunCol.rgb * fresnel * spec * shadow * specStrength;\n"
    "  }\n"
    "\n"
    // ── Atmospheric scattering: GUTTED ──
    //
    // The block below USED to be a full inline MgFP_ScreenScattering port:
    // height-dependent density, Rayleigh phase, Henyey-Greenstein Mie,
    // extinction, inscatter, all the works. Mathematically correct,
    // visually catastrophic on Intel UHD and AMD Adrenalin drivers.
    //
    // Why it was broken: the math was tuned for an HDR pipeline that
    // tone-maps AFTER the atmosphere term, using captured constants like
    // ExtinctionMultiplier=17.5 which only make sense if a Reinhard pass
    // is going to compress them back down later. That pipeline is FORCED
    // OFF (see "HDR PIPELINE FORCED OFF" block above near line 1045 and
    // the giant log dump that prints every frame). With no tonemap, the
    // raw atmosphere values land directly on a non-HDR sRGB backbuffer.
    // NVIDIA drivers happened to clamp it in a way that looked fine.
    // Intel and AMD do exactly what the shader says, which means the
    // entire scene gets multiplied by exp(-optDepth)*17.5 then saturated,
    // and the inscatter term bleeds through everything left, producing
    // the dark-blue-fog-blocks-my-view appearance reported by every
    // tester running on anything other than NVIDIA.
    //
    // Until someone implements the post-FX tonemap properly (Option A or
    // B from the HDR comment block — both still pending), running this
    // math per-pixel without a balancing pass is worse than not running
    // it at all. So it's gone. Hemisphere ambient stays. Extinction tint
    // stays. The Rayleigh/Mie nonsense and the multiply-by-17.5 don't.
    //
    // To restore: revert this diff AND ship a working tonemap. Doing one
    // without the other is exactly how we got here.
    //
    // Hemisphere ambient: add a subtle ground bounce from below
    // (simulates BottomDomeColor/DynamicAmbientScale fill the game does via ambient cube)
    "  float hemiBlend = N.y * 0.5 + 0.5;\n" // 0=down, 1=up
    "  float3 hemiAmb = lerp(g_BotDomeCol.rgb, g_TopDomeCol.rgb, hemiBlend);\n"
    "  Ir += albedo * hemiAmb * ao;\n"
    "\n"
    // Extinction tint at unit strength: keeps the level's color grading
    // intent (warm/cool tint) without scaling the scene by ExtinctionMult.
    "  Ir = Ir * g_ExtinctCol.rgb;\n"
    "\n"
    // Emissive glow (lava, fire, magic — added after scattering so it shines through fog)
    "  if (g_EmissiveParams.x > 0.5) {\n"
    "    float3 emissive = tex2D(g_EmissiveMap, texUV).rgb;\n"
    "    Ir += emissive * g_EmissiveParams.y;\n"
    "  }\n"
    "\n"
    // Water Fresnel transparency: more see-through when looking straight down
    "  if (g_WaterParams.x > 0.5 && g_EmissiveParams.x < 0.5) {\n"
    "    float3 V = normalize(g_CamPos.xyz - wpos.xyz);\n"
    "    float NdV = saturate(dot(N, V));\n"
    "    float fresnel = 0.02 + 0.98 * pow(1.0 - NdV, 4.0);\n"
    "    alpha = lerp(g_WaterParams.w, 1.0, fresnel);\n"
    "    Ir = lerp(Ir, Ir + float3(0.1, 0.15, 0.2), fresnel);\n"
    "  }\n"
    "\n"
    "  return float4(saturate(Ir), alpha);\n"
    "}\n";

// ── Post-processing shaders (bloom + tone mapping) ──
// Fullscreen quad vertex shader
static const char s_postVS[] =
    "float4 g_HalfPixel : register(c0);\n"
    "struct VS_OUT { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(float3 p : POSITION, float2 t : TEXCOORD0) {\n"
    "  VS_OUT o; o.pos = float4(p.xy + g_HalfPixel.xy, p.z, 1.0); o.uv = t; return o;\n"
    "}\n";

// Bright pass — extract pixels above threshold for bloom
static const char s_brightPassPS[] =
    "sampler2D g_Tex : register(s0);\n"
    "float4 g_Params : register(c0);\n" // x=threshold, y=intensity
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 c = tex2D(g_Tex, uv).rgb;\n"
    "  float lum = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "  float bloom = max(lum - g_Params.x, 0.0) * g_Params.y;\n"
    "  return float4(c * bloom, 1.0);\n"
    "}\n";

// Gaussian blur (separable — run twice: H then V)
static const char s_blurPS[] =
    "sampler2D g_Tex : register(s0);\n"
    "float4 g_Dir : register(c0);\n" // xy=blur direction (texel step)
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 c = tex2D(g_Tex, uv).rgb * 0.227;\n"
    "  c += tex2D(g_Tex, uv + g_Dir.xy * 1.384).rgb * 0.316;\n"
    "  c += tex2D(g_Tex, uv - g_Dir.xy * 1.384).rgb * 0.316;\n"
    "  c += tex2D(g_Tex, uv + g_Dir.xy * 3.228).rgb * 0.070;\n"
    "  c += tex2D(g_Tex, uv - g_Dir.xy * 3.228).rgb * 0.070;\n"
    "  return float4(c, 1.0);\n"
    "}\n";

// Final composite: tone map (Reinhard) + bloom add + gamma
// Tone mapping — matches game's MgFP_ToneMap disassembly.
// Game uses per-channel brightness/gamma for warm color grading.
// c0 = (exposure, bloomStr, whitepoint, 0)
// c1 = brightness per-channel (R=0.515, G=0.525, B=0.575 from capture c219)
// c2 = gamma per-channel (R=1.05, G=1.0, B=0.957 from capture c217)
// c3 = (contrast, 0, 0, 0) (1.15 from capture c218)
static const char s_toneMapPS[] =
    "sampler2D g_Scene : register(s0);\n"
    "sampler2D g_Bloom : register(s1);\n"
    "float4 g_ToneParams  : register(c0);\n" // x=exposure, y=bloomStr, z=whitepoint
    "float4 g_Brightness  : register(c1);\n" // per-channel brightness (c219)
    "float4 g_Gamma       : register(c2);\n" // per-channel gamma (c217)
    "float4 g_ContrastExp : register(c3);\n" // x=contrast (c218)
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 scene = tex2D(g_Scene, uv).rgb;\n"
    "  float3 bloom = tex2D(g_Bloom, uv).rgb;\n"
    "  float3 c = scene + bloom * g_ToneParams.y;\n"
    // Exposure (game c220.x = 1.25)
    "  c *= g_ToneParams.x;\n"
    // Reinhard tone mapping with white point (game c220.z)
    "  float wp2 = g_ToneParams.z * g_ToneParams.z;\n"
    "  c = c * (1.0 + c / wp2) / (1.0 + c);\n"
    // Per-channel gamma (game c217 — R slightly warm, B slightly cool)
    // This creates the warm Mordor color grade without crushing brightness
    "  c.r = pow(max(c.r, 0.001), 1.0 / g_Gamma.r);\n"
    "  c.g = pow(max(c.g, 0.001), 1.0 / g_Gamma.g);\n"
    "  c.b = pow(max(c.b, 0.001), 1.0 / g_Gamma.b);\n"
    // Per-channel color balance (warm tint, applied AFTER tone mapping)
    // Ratios normalized so max=1: R=0.896, G=0.913, B=1.0
    "  c *= g_Brightness.rgb;\n"
    "  return float4(c, 1.0);\n"
    "}\n";

// Bridge VS — transforms our vertex format to the game PS input layout.
// Our vertex: POSITION(0) NORMAL(12) COLOR(24) TEXCOORD(28) TANGENT(36)
// Game PS expects:
//   COLOR0 v0    = vertex color (AO in .w)
//   NORMAL v1    = world normal
//   TEXCOORD0 v2 = world position (xyz, w=1)
//   TEXCOORD1 v3 = UV (xy)
//   TEXCOORD4 v4 = atmosphere color (computed from sun+normal)
static const char s_bridgeVS[] =
    "float4x4 g_World    : register(c0);\n"   // our layout: c0-c3
    "float4x4 g_ViewProj : register(c4);\n"   // our layout: c4-c7
    "float4x4 g_WorldGame : register(c178);\n" // game layout: c178-c181
    "float4x4 g_ViewProjGame : register(c239);\n" // game layout: c239-c242
    "float4 g_SunCol : register(c250);\n"      // VS sun color
    "float4 g_SunDir : register(c251);\n"      // VS sun dir
    "float4 g_MtlColor : register(c197);\n"    // per-material brightness tint
    "float4 g_CamPos : register(c245);\n"      // camera world position
    "float4 g_InscatCol : register(c246);\n"   // inscatter color (rgb) * multiplier
    "float4 g_ScatParams : register(c247);\n"  // x=density, y=maxDist, z=0, w=0
    "\n"
    "struct VS_OUT {\n"
    "  float4 pos   : POSITION;\n"
    "  float4 color : COLOR0;\n"     // v0: mtlColor (game: c197)
    "  float4 nrm   : NORMAL;\n"     // v1: xyz=worldNormal, w=AO
    "  float4 wpos  : TEXCOORD0;\n"  // v2: world position
    "  float2 uv    : TEXCOORD1;\n"  // v3: UV
    "  float3 atm   : TEXCOORD4;\n"  // v4: atmosphere (ADDITIVE in PS)
    "};\n"
    "\n"
    "VS_OUT main(float3 p : POSITION, float3 n : NORMAL,\n"
    "            float4 c : COLOR0, float2 t : TEXCOORD0,\n"
    "            float4 tan : TANGENT) {\n"
    "  VS_OUT o;\n"
    "  float4 wp = mul(g_WorldGame, float4(p, 1.0));\n"
    "  o.pos   = mul(g_ViewProjGame, wp);\n"
    "  o.color = g_MtlColor;\n"
    "  float3 wn = normalize(mul((float3x3)g_WorldGame, n));\n"
    "  o.nrm   = float4(wn, c.a);\n"
    "  o.wpos  = wp;\n"
    "  o.uv    = t;\n"
    // Distance-based atmospheric inscatter (game does this in post-processing,
    // we approximate per-vertex). PS adds: final = albedo * atm + litResult.
    // fog = 1 - exp(-dist * density) gives exponential distance fog.
    "  float dist = length(wp.xyz - g_CamPos.xyz);\n"
    "  float fog = saturate(1.0 - exp(-dist * g_ScatParams.x));\n"
    "  o.atm = g_InscatCol.rgb * fog;\n"
    "  return o;\n"
    "}\n";

// Bridge VS for AN path: outputs full TBN frame for normal-mapped game PS
// Game PS AN expects: COLOR0=mtlColor, NORMAL=(worldN,AO), TEXCOORD0=worldPos,
// TEXCOORD1-3 = transposed TBN columns (Bx,By,Bz), TEXCOORD4=UV0+UV1, TEXCOORD5=UV2
static const char s_bridgeVS_AN[] =
    "float4x4 g_World    : register(c0);\n"
    "float4x4 g_ViewProj : register(c4);\n"
    "float4x4 g_WorldGame : register(c178);\n"
    "float4x4 g_ViewProjGame : register(c239);\n"
    "float4 g_MtlColor : register(c197);\n"    // per-material brightness tint
    "float4 g_CamPos : register(c245);\n"      // camera world position
    "float4 g_InscatCol : register(c246);\n"   // inscatter color
    "float4 g_ScatParams : register(c247);\n"  // x=density
    "\n"
    "struct VS_OUT {\n"
    "  float4 pos   : POSITION;\n"
    "  float4 color : COLOR0;\n"     // mtlColor from c197
    "  float4 nrm   : NORMAL;\n"     // xyz=worldNormal, w=AO
    "  float4 wpos  : TEXCOORD0;\n"  // world position
    "  float4 bx    : TEXCOORD1;\n"  // (T.x, B.x, N.x, staticLighting.x)
    "  float4 by    : TEXCOORD2;\n"  // (T.y, B.y, N.y, staticLighting.y)
    "  float4 bz    : TEXCOORD3;\n"  // (T.z, B.z, N.z, staticLighting.z)
    "  float4 uv01  : TEXCOORD4;\n"  // (UV0.x, UV0.y, UV1.x, UV1.y)
    "  float4 uv2   : TEXCOORD5;\n"  // (UV2.xy, UV_spec.zw) — ANS reads .zw for specular UV
    "};\n"
    "\n"
    "VS_OUT main(float3 p : POSITION, float3 n : NORMAL,\n"
    "            float4 c : COLOR0, float2 t : TEXCOORD0,\n"
    "            float4 tan : TANGENT,\n"
    "            float4 sl : TEXCOORD5) {\n"  // static lighting from vertex data
    "  VS_OUT o;\n"
    "  float4 wp = mul(g_WorldGame, float4(p, 1.0));\n"
    "  o.pos   = mul(g_ViewProjGame, wp);\n"
    "  o.wpos  = wp;\n"
    "\n"
    "  float3 N = normalize(mul((float3x3)g_WorldGame, n));\n"
    "  float3 T = normalize(mul((float3x3)g_WorldGame, tan.xyz));\n"
    "  float3 B = normalize(cross(N, T) * tan.w);\n"
    "\n"
    "  o.color = g_MtlColor;\n"       // per-material tint from c197
    "  o.nrm   = float4(N, c.a);\n"   // world normal + AO in w
    "\n"
    // Transposed TBN: each row = one component of T,B,N + static lighting
    // Static lighting from vertex data + atmospheric inscatter.
    // PS adds: final = albedo * SL.rgb * SL.w + litResult (ADDITIVE).
    // We combine actual vertex SL with distance-based inscatter fog.
    "  float dist = length(wp.xyz - g_CamPos.xyz);\n"
    "  float fog = saturate(1.0 - exp(-dist * g_ScatParams.x));\n"
    "  float3 inscat = g_InscatCol.rgb * fog;\n"
    "  o.bx = float4(T.x, B.x, N.x, sl.r + inscat.r);\n"
    "  o.by = float4(T.y, B.y, N.y, sl.g + inscat.g);\n"
    "  o.bz = float4(T.z, B.z, N.z, sl.b + inscat.b);\n"
    "\n"
    // All UV sets are the same (we only have one UV set from mesh data).
    // TEXCOORD4: xy=UV0 (albedo), zw=UV1 (normal map)
    // TEXCOORD5: xy=UV2 (normal in AN), zw=UV3 (specular in ANS)
    "  o.uv01 = float4(t.x, t.y, t.x, t.y);\n"
    "  o.uv2  = float4(t.x, t.y, t.x, t.y);\n"
    "  return o;\n"
    "}\n";

// Shadow depth shaders — minimal: just output depth to R32F
static const char s_shadowVS[] =
    "float4x4 g_World    : register(c0);\n"
    "float4x4 g_LightVP  : register(c4);\n"
    "struct VS_OUT { float4 pos : POSITION; float depth : TEXCOORD0; };\n"
    "VS_OUT main(float3 p : POSITION) {\n"
    "  VS_OUT o;\n"
    "  float4 wp = mul(g_World, float4(p, 1.0));\n"
    "  o.pos = mul(g_LightVP, wp);\n"
    "  o.depth = o.pos.z / o.pos.w;\n"
    "  return o;\n"
    "}\n";

static const char s_shadowPS[] =
    "float4 main(float depth : TEXCOORD0) : COLOR {\n"
    "  return float4(depth, depth, depth, 1.0);\n"
    "}\n";

// =============================================================================
//  initShaders / destroyShaders
// =============================================================================

bool LevelScene::initShaders()
{
    if (m_shadersReady) return true;
    if (m_shaderInitAttempted) return false;
    m_shaderInitAttempted = true;
    if (!m_device) { LevelLog("[Shaders] No device, skipping init"); return false; }

    // Check device vertex processing mode — software VP makes shaders very slow
    D3DDEVICE_CREATION_PARAMETERS createParams;
    if (SUCCEEDED(m_device->GetCreationParameters(&createParams)))
    {
        DWORD bflags = createParams.BehaviorFlags;
        bool hwVP   = (bflags & D3DCREATE_HARDWARE_VERTEXPROCESSING) != 0;
        bool swVP   = (bflags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
        bool mixed  = (bflags & D3DCREATE_MIXED_VERTEXPROCESSING)    != 0;
        LevelLogf("[Shaders] Device flags: HW_VP=%d SW_VP=%d MIXED=%d flags=0x%08X",
                  hwVP?1:0, swVP?1:0, mixed?1:0, bflags);
        if (swVP && !hwVP && !mixed)
        {
            LevelLog("[Shaders] ABORT: Device uses SOFTWARE vertex processing — shaders would be very slow");
            return false;
        }
        // For mixed mode, try to enable hardware VP
        if (mixed)
        {
            m_device->SetSoftwareVertexProcessing(FALSE);
            LevelLog("[Shaders] Mixed mode: forced hardware vertex processing");
        }
    }

    D3DCAPS9 caps;
    if (SUCCEEDED(m_device->GetDeviceCaps(&caps)))
    {
        LevelLogf("[Shaders] VS version: %d.%d, PS version: %d.%d",
                  D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion),
                  D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
                  D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),
                  D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));
    }

    LevelLog("[Shaders] Compiling VS (vs_3_0)...");
    ID3DXBuffer* vsBlob = NULL;
    ID3DXBuffer* vsErr  = NULL;
    ID3DXBuffer* psBlob = NULL;
    ID3DXBuffer* psErr  = NULL;

    HRESULT hr = D3DXCompileShader(s_levelVS, (UINT)strlen(s_levelVS),
        NULL, NULL, "main", "vs_3_0", 0, &vsBlob, &vsErr, NULL);
    if (FAILED(hr))
    {
        LevelLogf("[Shaders] VS compile FAILED hr=0x%08X", (unsigned)hr);
        if (vsErr) { LevelLogf("[Shaders] VS error: %s", (const char*)vsErr->GetBufferPointer()); vsErr->Release(); }
        return false;
    }
    LevelLog("[Shaders] VS compiled OK");

    LevelLog("[Shaders] Compiling PS (ps_3_0)...");
    hr = D3DXCompileShader(s_levelPS, (UINT)strlen(s_levelPS),
        NULL, NULL, "main", "ps_3_0", 0, &psBlob, &psErr, NULL);
    if (FAILED(hr))
    {
        LevelLogf("[Shaders] PS compile FAILED hr=0x%08X", (unsigned)hr);
        if (psErr) { LevelLogf("[Shaders] PS error: %s", (const char*)psErr->GetBufferPointer()); psErr->Release(); }
        vsBlob->Release();
        return false;
    }
    LevelLog("[Shaders] PS compiled OK");

    hr = m_device->CreateVertexShader((const DWORD*)vsBlob->GetBufferPointer(), &m_levelVS);
    vsBlob->Release();
    if (vsErr) vsErr->Release();
    if (FAILED(hr)) { LevelLogf("[Shaders] CreateVertexShader FAILED hr=0x%08X", (unsigned)hr); psBlob->Release(); if (psErr) psErr->Release(); return false; }

    hr = m_device->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &m_levelPS);
    psBlob->Release();
    if (psErr) psErr->Release();
    if (FAILED(hr)) { LevelLogf("[Shaders] CreatePixelShader FAILED hr=0x%08X", (unsigned)hr); m_levelVS->Release(); m_levelVS = NULL; return false; }

    // Vertex declaration matching LevelVertex (68 bytes)
    // pos(12) + normal(12) + color(4) + uv(8) + tangent(16) + staticLighting(16)
    D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION,  0 },
        { 0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,    0 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,     0 },
        { 0, 28, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,  0 },
        { 0, 36, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,   0 },
        { 0, 52, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,  5 }, // static lighting
        D3DDECL_END()
    };
    hr = m_device->CreateVertexDeclaration(decl, &m_levelDecl);
    if (FAILED(hr))
    {
        m_levelVS->Release(); m_levelVS = NULL;
        m_levelPS->Release(); m_levelPS = NULL;
        return false;
    }

    // 1x1 white texture for untextured mesh parts
    hr = m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &m_whiteTex, NULL);
    if (SUCCEEDED(hr))
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_whiteTex->LockRect(0, &lr, NULL, 0)))
        {
            *(DWORD*)lr.pBits = 0xFFFFFFFF;
            m_whiteTex->UnlockRect(0);
        }
    }

    // 1x1 flat normal texture (128,128,255) = tangent-space (0,0,1) = "no perturbation"
    m_flatNormalTex = NULL;
    hr = m_device->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8,
                                  D3DPOOL_MANAGED, &m_flatNormalTex, NULL);
    if (SUCCEEDED(hr))
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(m_flatNormalTex->LockRect(0, &lr, NULL, 0)))
        {
            *(DWORD*)lr.pBits = 0xFFFF8080; // ARGB: A=FF, R=80, G=80, B=FF → normal (0,0,1)
            m_flatNormalTex->UnlockRect(0);
        }
    }

    // Ambient cubemap for game PS s6 — 1x1 per face
    // Game PS: amb = texCube(s6, worldNormal) * NdN * c1.w * c72 + c1.rgb
    // The cubemap value is multiplied by c1.w (=1.0) and c72 (=1,1,1,1) then
    // ADDED to c1.rgb (ambient). So cubemap should NOT be bright — use a dim
    // neutral value that adds subtle ambient bounce without overblow.
    m_ambientCube = NULL;
    {
        HRESULT chr = m_device->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &m_ambientCube, NULL);
        if (SUCCEEDED(chr) && m_ambientCube) {
            // Dim ambient fill (0.15 = subtle indirect light contribution)
            DWORD ambCol = 0xFF262626; // ~0.15 grey
            for (int face = 0; face < 6; ++face) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(m_ambientCube->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0))) {
                    *(DWORD*)lr.pBits = ambCol;
                    m_ambientCube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
                }
            }
            LevelLog("[Shaders] Ambient cubemap created (1x1 dim neutral)");
        }
    }

    // Shadow map resources
    m_shadowsReady = false;
    m_shadowMapTex = NULL; m_shadowMapSurf = NULL; m_shadowMapDS = NULL;
    m_shadowVS = NULL; m_shadowPS = NULL;
    memset(m_lightVP, 0, sizeof(m_lightVP));
    {
        // Compile shadow shaders
        ID3DXBuffer *svBlob=NULL, *spBlob=NULL, *svErr=NULL, *spErr=NULL;
        HRESULT hr1 = D3DXCompileShader(s_shadowVS, (UINT)strlen(s_shadowVS),
            NULL, NULL, "main", "vs_3_0", 0, &svBlob, &svErr, NULL);
        HRESULT hr2 = D3DXCompileShader(s_shadowPS, (UINT)strlen(s_shadowPS),
            NULL, NULL, "main", "ps_3_0", 0, &spBlob, &spErr, NULL);
        if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
            m_device->CreateVertexShader((const DWORD*)svBlob->GetBufferPointer(), &m_shadowVS);
            m_device->CreatePixelShader((const DWORD*)spBlob->GetBufferPointer(), &m_shadowPS);
        }
        if (svBlob) svBlob->Release(); if (spBlob) spBlob->Release();
        if (svErr) svErr->Release(); if (spErr) spErr->Release();

        // Create R32F shadow map texture
        if (m_shadowVS && m_shadowPS) {
            hr = m_device->CreateTexture(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1,
                D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &m_shadowMapTex, NULL);
            if (SUCCEEDED(hr)) {
                m_shadowMapTex->GetSurfaceLevel(0, &m_shadowMapSurf);
                hr = m_device->CreateDepthStencilSurface(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE,
                    D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &m_shadowMapDS, NULL);
                if (SUCCEEDED(hr)) {
                    m_shadowsReady = true;
                    LevelLog("[Shaders] Shadow map created (2048x2048 R32F)");
                }
            }
        }
        if (!m_shadowsReady) LevelLog("[Shaders] Shadow map creation FAILED — shadows disabled");
    }

    // Post-processing resources
    m_postReady = false;
    m_sceneRT = NULL; m_sceneRTSurf = NULL;
    m_bloomRT = NULL; m_bloomRTSurf = NULL;
    m_bloomRT2 = NULL; m_bloomRT2Surf = NULL;
    m_brightPassPS = NULL; m_blurPS = NULL; m_toneMapPS = NULL;
    m_postVS = NULL; m_postDecl = NULL;
    m_sceneRTWidth = 0; m_sceneRTHeight = 0;
    {
        // Compile post-process shaders
        ID3DXBuffer *b1=NULL,*b2=NULL,*b3=NULL,*b4=NULL,*e1=NULL,*e2=NULL,*e3=NULL,*e4=NULL;
        bool allOk = true;
        if (FAILED(D3DXCompileShader(s_postVS, (UINT)strlen(s_postVS), NULL,NULL,"main","vs_3_0",0,&b1,&e1,NULL))) allOk=false;
        if (FAILED(D3DXCompileShader(s_brightPassPS, (UINT)strlen(s_brightPassPS), NULL,NULL,"main","ps_3_0",0,&b2,&e2,NULL))) allOk=false;
        if (FAILED(D3DXCompileShader(s_blurPS, (UINT)strlen(s_blurPS), NULL,NULL,"main","ps_3_0",0,&b3,&e3,NULL))) allOk=false;
        if (FAILED(D3DXCompileShader(s_toneMapPS, (UINT)strlen(s_toneMapPS), NULL,NULL,"main","ps_3_0",0,&b4,&e4,NULL))) allOk=false;

        if (allOk) {
            m_device->CreateVertexShader((const DWORD*)b1->GetBufferPointer(), &m_postVS);
            m_device->CreatePixelShader((const DWORD*)b2->GetBufferPointer(), &m_brightPassPS);
            m_device->CreatePixelShader((const DWORD*)b3->GetBufferPointer(), &m_blurPS);
            m_device->CreatePixelShader((const DWORD*)b4->GetBufferPointer(), &m_toneMapPS);
        }
        if (b1) b1->Release(); if (b2) b2->Release(); if (b3) b3->Release(); if (b4) b4->Release();
        if (e1) e1->Release(); if (e2) e2->Release(); if (e3) e3->Release(); if (e4) e4->Release();

        // Post-process vertex declaration (screen quad: pos + uv)
        if (m_postVS) {
            D3DVERTEXELEMENT9 postDecl[] = {
                { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
                D3DDECL_END()
            };
            m_device->CreateVertexDeclaration(postDecl, &m_postDecl);
        }

        if (m_postVS && m_brightPassPS && m_blurPS && m_toneMapPS && m_postDecl) {
            // ══ HDR POST IS FUCKING DEAD AND IT IS STAYING DEAD ══════════
            //
            // The shaders compile. ARGB8-safe RT rewrite from the other
            // agent legitimately fixed the FP16 StretchRect crash that
            // had been a cocksucking ghost in this engine for months -
            // genuine motherfucking respect for that fix. But they then
            // turned the whole goddamn pipeline back ON without touching
            // the actual tonemap shader, which is still pure mathematical
            // horseshit:
            //
            //     c *= g_ToneParams.x * 1.15
            //
            // where g_ToneParams.x is the AtmosphereSetting "key" field,
            // which on Helm's Deep is around 0.15. So every fucking pixel
            // of a perfectly fine HDR scene gets multiplied by 0.17 and
            // CRUSHED into the asshole of the brightness range. Whatever
            // residual color survives Reinhard is the inscatter sky tint,
            // which on Helm's Deep is BLUE. End result: the whole
            // motherfucking viewport renders dark and blue and looks like
            // somebody photographed a haunted swimming pool through a
            // dirty fucking lens at an ungodly hour. Hours of debugging
            // wasted before we figured out the WHOLE PIPELINE was wrong.
            //
            // The shader author confused two completely separate concepts.
            // "Key" in Reinhard auto-exposure is the TARGET middle-grey
            // luminance (~0.18) used as `exposure = key / avgLuminance`.
            // It is NOT a direct fucking brightness multiplier. Plugging
            // it in as one inverts the intended behavior - levels with a
            // low key (intentionally moody) get DOUBLY crushed instead
            // of getting properly exposed up. Pure rookie horseshit.
            //
            // Until tonemap.hlsl gets a real auto-exposure pass OR at
            // bare minimum a `key / 0.18` translation, this whole
            // pipeline stays the fuck off. Ungraded scene - no bloom,
            // no tonemap, no gamma - is uglier in the abstract but it
            // does NOT look like a goddamn haunted aquarium. Trade
            // accepted. Eat it.
            m_postReady = false;
            LevelLog("[Shaders] ═══════════════════════════════════════════════════════════");
            LevelLog("[Shaders] HDR PIPELINE FORCED OFF - post FX shaders compiled, we are");
            LevelLog("[Shaders] LEAVING THEM THE FUCK ALONE.");
            LevelLog("[Shaders] ═══════════════════════════════════════════════════════════");
            LevelLog("[Shaders] Reason: tonemap.hlsl does `c *= key * 1.15`. That is pure");
            LevelLog("[Shaders] rookie motherfucking horseshit. `key` is the Reinhard");
            LevelLog("[Shaders] MIDDLE-GREY TARGET (~0.18), a luminance value you DIVIDE");
            LevelLog("[Shaders] by the scene average to PRODUCE exposure. Whoever wrote the");
            LevelLog("[Shaders] shader fucking YOLO-multiplied the scene BY it instead.");
            LevelLog("[Shaders] Every level with key < 0.18 gets crushed toward black. Every");
            LevelLog("[Shaders] cool-atmosphere level ends up rendering dark AND blue because");
            LevelLog("[Shaders] the inscatter bleed is the only thing surviving. User spent");
            LevelLog("[Shaders] hours staring at a haunted aquarium before we tracked it down.");
            LevelLog("[Shaders] ─── FIX before re-enabling ───");
            LevelLog("[Shaders] Option A: real auto-exposure pass (downsample to 1x1, avg");
            LevelLog("[Shaders]           luminance, key/avg = exposure). The correct fix.");
            LevelLog("[Shaders] Option B: quick-and-filthy `(key / 0.18)` translation so a");
            LevelLog("[Shaders]           typical 0.15 becomes roughly 0.83x instead of 0.17x.");
            LevelLog("[Shaders] Until one of those lands, the pipeline STAYS OFF. Eat it.");
        } else {
            LevelLog("[Shaders] Post FX shader COMPILE FAILED. Pipeline off either way - if");
            LevelLog("[Shaders] it had compiled we would have force-disabled it anyway because");
            LevelLog("[Shaders] the goddamn math is broken. Double disability. If you ever");
            LevelLog("[Shaders] want HDR back: fix the compile failure FIRST, then the math.");
            LevelLog("[Shaders] In that order. Do not skip step 1 thinking the other branch");
            LevelLog("[Shaders] will save you - it will not, it is also disabled on purpose.");
        }
    }

    // Load the game's compiled shader cache. Pandemic shipped THREE bins
    // (Shaders_PC_nvidia.bin, Shaders_PC_ati.bin, Shaders_PC_generic.bin)
    // and the retail exe at MgD3D.cpp:67-78 picks one BY GPU VENDOR ID.
    // NVIDIA-compiled D3D9 bytecode uses _pp partial-precision hints and
    // instruction orderings that NVIDIA's driver translates fine but
    // Intel UHD's translator implements as FP16/FP24, which is why a
    // tester on Intel UHD was seeing wet-cardboard textures while the
    // NVIDIA box looked sharp. We do the same vendor switch the retail
    // game does, then fall back to whatever bin actually exists on disk
    // so a partial drop (only the nvidia bin shipped) still boots.
    m_gameShaders = new GameShaderCache();
    {
        // Ask D3D9 for the adapter the device was created on, then ask
        // the IDirect3D9 object for that adapter's vendor ID. NVIDIA is
        // 0x10DE, AMD/ATI is 0x1002, Intel is 0x8086 (everything else
        // also falls through to "generic"). Anything goes wrong here,
        // default to "generic" because that bin was compiled without
        // vendor hints and is the most portable of the three.
        DWORD vendorId = 0;
        D3DDEVICE_CREATION_PARAMETERS cp;
        memset(&cp, 0, sizeof(cp));
        if (SUCCEEDED(m_device->GetCreationParameters(&cp))) {
            IDirect3D9* d3d9 = NULL;
            if (SUCCEEDED(m_device->GetDirect3D(&d3d9)) && d3d9) {
                D3DADAPTER_IDENTIFIER9 ident;
                memset(&ident, 0, sizeof(ident));
                if (SUCCEEDED(d3d9->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &ident))) {
                    vendorId = ident.VendorId;
                }
                d3d9->Release();
            }
        }
        const char* vendorTag =
            (vendorId == 0x10DE) ? "nvidia" :
            (vendorId == 0x1002) ? "ati"    :
                                   "generic";
        LevelLogf("[Shaders] GPU vendor ID=0x%04X => preferred bin=Shaders_PC_%s.bin",
                  (unsigned)vendorId, vendorTag);

        // Preferred bin first, then the other two as fallback. Each name
        // tried at three search locations the way the original code did.
        // Order matters: vendor-matched takes priority, then generic
        // (cross-vendor safe), then whatever's left over.
        const char* fnameOrder[3] = { NULL, "generic", NULL };
        if (vendorId == 0x10DE) { fnameOrder[0] = "nvidia"; fnameOrder[2] = "ati"; }
        else if (vendorId == 0x1002) { fnameOrder[0] = "ati"; fnameOrder[2] = "nvidia"; }
        else { fnameOrder[0] = "generic"; fnameOrder[1] = "nvidia"; fnameOrder[2] = "ati"; }

        // Search locations: working dir, Scene3D, and RE (which is where
        // the ati + generic bins actually live in this tree). Each name
        // hits all locations before we fall back to the next vendor.
        const char* prefixes[] = {
            "",
            "Scene3D\\",
            "..\\Scene3D\\",
            "RE\\",
            "..\\RE\\"
        };
        const int kPrefixCount = (int)(sizeof(prefixes) / sizeof(prefixes[0]));
        bool loaded = false;
        for (int ni = 0; ni < 3 && !loaded; ++ni) {
            if (!fnameOrder[ni]) continue;
            for (int pi = 0; pi < kPrefixCount && !loaded; ++pi) {
                char fullPath[256];
                _snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE,
                    "%sShaders_PC_%s.bin", prefixes[pi], fnameOrder[ni]);
                if (m_gameShaders->load(fullPath, m_device)) {
                    LevelLogf("[Shaders] Game shader cache loaded: %d VS, %d PS (%d named) from %s",
                              m_gameShaders->vsCount(), m_gameShaders->psCount(),
                              m_gameShaders->namedCount(), fullPath);
                    loaded = true;
                }
            }
        }
        if (!loaded)
            LevelLog("[Shaders] Game shader cache NOT found — using custom shaders");
    }

    // Compile bridge VS variants (our vertex format → game PS inputs)
    m_bridgeVS = NULL;
    m_bridgeVS_AN = NULL;
    {
        // Bridge VS for A path (simple, no TBN)
        ID3DXBuffer* bvBlob = NULL; ID3DXBuffer* bvErr = NULL;
        HRESULT bhr = D3DXCompileShader(s_bridgeVS, (UINT)strlen(s_bridgeVS),
            NULL, NULL, "main", "vs_3_0", 0, &bvBlob, &bvErr, NULL);
        if (SUCCEEDED(bhr) && bvBlob) {
            m_device->CreateVertexShader((const DWORD*)bvBlob->GetBufferPointer(), &m_bridgeVS);
            LevelLog("[Shaders] Bridge VS (A) compiled");
        } else {
            if (bvErr) LevelLogf("[Shaders] Bridge VS (A) FAILED: %s", (const char*)bvErr->GetBufferPointer());
        }
        if (bvBlob) bvBlob->Release();
        if (bvErr) bvErr->Release();

        // Bridge VS for AN path (with TBN frame)
        bvBlob = NULL; bvErr = NULL;
        bhr = D3DXCompileShader(s_bridgeVS_AN, (UINT)strlen(s_bridgeVS_AN),
            NULL, NULL, "main", "vs_3_0", 0, &bvBlob, &bvErr, NULL);
        if (SUCCEEDED(bhr) && bvBlob) {
            m_device->CreateVertexShader((const DWORD*)bvBlob->GetBufferPointer(), &m_bridgeVS_AN);
            LevelLog("[Shaders] Bridge VS (AN) compiled — TBN frame output");
        } else {
            if (bvErr) LevelLogf("[Shaders] Bridge VS (AN) FAILED: %s", (const char*)bvErr->GetBufferPointer());
        }
        if (bvBlob) bvBlob->Release();
        if (bvErr) bvErr->Release();
    }

    // Cache game PS pointers for per-material selection
    m_gamePS_A = NULL; m_gamePS_AN = NULL; m_gamePS_AN_Dn = NULL;
    m_gamePS_strauss_AN = NULL; m_gamePS_strauss_ANS = NULL;
    if (m_gameShaders) {
        m_gamePS_A          = m_gameShaders->getPS("Mg_FP_Lit_A_Vd_Ao_WPos_Shdw_VNorm_VtxAtm");
        m_gamePS_AN         = m_gameShaders->getPS("Mg_FP_Lit_AN_Vd_Ao_WPos_Shdw_VNorm_VtxAtm");
        m_gamePS_AN_Dn      = m_gameShaders->getPS("Mg_FP_Lit_AN_Dn_Vd_Ao_WPos_Shdw_VNorm_VtxAtm");
        m_gamePS_strauss_AN = m_gameShaders->getPS("Mg_FP_strauss_AN_Vd_Ao_WPos_Shdw_VNorm_VtxAtm");
        m_gamePS_strauss_ANS= m_gameShaders->getPS("Mg_FP_strauss_ANS_Vd_Ao_WPos_Shdw_VNorm_VtxAtm");
        LevelLogf("[Shaders] Game PS cached: A=%s AN=%s AN_Dn=%s strauss_AN=%s strauss_ANS=%s",
            m_gamePS_A?"OK":"MISS", m_gamePS_AN?"OK":"MISS", m_gamePS_AN_Dn?"OK":"MISS",
            m_gamePS_strauss_AN?"OK":"MISS", m_gamePS_strauss_ANS?"OK":"MISS");
    }

    m_shadersReady = true;
    LevelLog("[Shaders] All resources created — shader path READY");
    return true;
}

void LevelScene::destroyShaders()
{
    if (m_levelVS)   { m_levelVS->Release();   m_levelVS = NULL; }
    if (m_levelPS)   { m_levelPS->Release();   m_levelPS = NULL; }
    if (m_levelDecl) { m_levelDecl->Release(); m_levelDecl = NULL; }
    if (m_whiteTex)  { m_whiteTex->Release();  m_whiteTex = NULL; }
    if (m_flatNormalTex) { m_flatNormalTex->Release(); m_flatNormalTex = NULL; }
    if (m_shadowMapSurf) { m_shadowMapSurf->Release(); m_shadowMapSurf = NULL; }
    if (m_shadowMapDS) { m_shadowMapDS->Release(); m_shadowMapDS = NULL; }
    if (m_shadowMapTex) { m_shadowMapTex->Release(); m_shadowMapTex = NULL; }
    if (m_shadowVS) { m_shadowVS->Release(); m_shadowVS = NULL; }
    if (m_shadowPS) { m_shadowPS->Release(); m_shadowPS = NULL; }
    m_shadowsReady = false;
    if (m_sceneRTSurf) { m_sceneRTSurf->Release(); m_sceneRTSurf = NULL; }
    if (m_sceneRT) { m_sceneRT->Release(); m_sceneRT = NULL; }
    if (m_bloomRTSurf) { m_bloomRTSurf->Release(); m_bloomRTSurf = NULL; }
    if (m_bloomRT) { m_bloomRT->Release(); m_bloomRT = NULL; }
    if (m_bloomRT2Surf) { m_bloomRT2Surf->Release(); m_bloomRT2Surf = NULL; }
    if (m_bloomRT2) { m_bloomRT2->Release(); m_bloomRT2 = NULL; }
    if (m_brightPassPS) { m_brightPassPS->Release(); m_brightPassPS = NULL; }
    if (m_blurPS) { m_blurPS->Release(); m_blurPS = NULL; }
    if (m_toneMapPS) { m_toneMapPS->Release(); m_toneMapPS = NULL; }
    if (m_postVS) { m_postVS->Release(); m_postVS = NULL; }
    if (m_postDecl) { m_postDecl->Release(); m_postDecl = NULL; }
    m_postReady = false;
    if (m_bridgeVS) { m_bridgeVS->Release(); m_bridgeVS = NULL; }
    if (m_bridgeVS_AN) { m_bridgeVS_AN->Release(); m_bridgeVS_AN = NULL; }
    if (m_ambientCube) { m_ambientCube->Release(); m_ambientCube = NULL; }
    // Game PS pointers are owned by GameShaderCache — don't Release() them here
    m_gamePS_A = NULL; m_gamePS_AN = NULL; m_gamePS_AN_Dn = NULL;
    m_gamePS_strauss_AN = NULL; m_gamePS_strauss_ANS = NULL;
    if (m_gameShaders) { delete m_gameShaders; m_gameShaders = NULL; }
    m_shadersReady = false;
    m_shaderInitAttempted = false;
}

// ── Hot-reload shaders from external .hlsl files ─────────────────────────────
// Reads shaders/tonemap.hlsl, shaders/brightpass.hlsl, shaders/blur.hlsl
// and recompiles them at runtime. Returns true if anything changed.
static bool ReadFileToString(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize(sz);
    fread(&out[0], 1, sz, f);
    fclose(f);
    return true;
}

bool LevelScene::hotReloadShaders()
{
    if (!m_device) return false;
    bool anyChanged = false;

    struct ShaderReload {
        const char* path;
        const char* name;
        const char* profile;
        void** target; // IDirect3DPixelShader9** or IDirect3DVertexShader9**
        bool isPS;
    };

    ShaderReload reloads[] = {
        { "shaders/tonemap.hlsl",    "ToneMap",    "ps_3_0", (void**)&m_toneMapPS,    true },
        { "shaders/brightpass.hlsl", "BrightPass", "ps_3_0", (void**)&m_brightPassPS, true },
        { "shaders/blur.hlsl",       "Blur",       "ps_3_0", (void**)&m_blurPS,       true },
    };

    for (int i = 0; i < 3; ++i) {
        std::string src;
        if (!ReadFileToString(reloads[i].path, src)) continue;

        ID3DXBuffer* blob = NULL; ID3DXBuffer* err = NULL;
        HRESULT hr = D3DXCompileShader(src.c_str(), (UINT)src.size(),
            NULL, NULL, "main", reloads[i].profile, 0, &blob, &err, NULL);
        if (SUCCEEDED(hr) && blob) {
            // Release old shader
            if (reloads[i].isPS && *reloads[i].target) {
                ((IDirect3DPixelShader9*)*reloads[i].target)->Release();
                *reloads[i].target = NULL;
            }
            // Create new
            if (reloads[i].isPS) {
                m_device->CreatePixelShader((const DWORD*)blob->GetBufferPointer(),
                    (IDirect3DPixelShader9**)reloads[i].target);
            }
            LevelLogf("[HotReload] %s recompiled OK from %s", reloads[i].name, reloads[i].path);
            anyChanged = true;
        } else {
            if (err) LevelLogf("[HotReload] %s FAILED: %s", reloads[i].name, (const char*)err->GetBufferPointer());
        }
        if (blob) blob->Release();
        if (err) err->Release();
    }

    if (anyChanged) LevelLog("[HotReload] Shaders updated — changes visible immediately");
    else LevelLog("[HotReload] No shader files found or no changes");
    return anyChanged;
}

// ── Release all D3DPOOL_DEFAULT resources before device Reset ────────────────
// Called when the user toggles fullscreen or the device is lost. Without this,
// IDirect3DDevice9::Reset() fails with D3DERR_INVALIDCALL because default-pool
// resources are still allocated.
void LevelScene::releaseDefaultPoolResources() {
    // Shadow/post targets are created inside initShaders(), so a partial
    // default-pool release would leave shadows disabled after Reset().
    destroyShaders();
    m_sceneRTWidth = 0;
    m_sceneRTHeight = 0;

    LevelLog("[DeviceReset] Released LevelScene render resources for Reset()");
}

// ── Screenshot to BMP ────────────────────────────────────────────────────────
bool LevelScene::saveScreenshot(const char* path)
{
    if (!m_device) return false;
    IDirect3DSurface9* backbuf = NULL;
    if (FAILED(m_device->GetRenderTarget(0, &backbuf)) || !backbuf) return false;

    // D3DXSaveSurfaceToFile is in d3dx9
    HRESULT hr = D3DXSaveSurfaceToFileA(path, D3DXIFF_BMP, backbuf, NULL, NULL);
    backbuf->Release();

    if (SUCCEEDED(hr)) {
        LevelLogf("[Screenshot] Saved to %s", path);
        return true;
    }
    LevelLogf("[Screenshot] FAILED to save to %s (hr=0x%08X)", path, hr);
    return false;
}

// ── extractLighting ──────────────────────────────────────────────────────────
// Scans parsed game objects for AtmosphereSetting and light_sun, extracts
// per-level lighting/fog/color-grading from the actual PAK data.

static inline void UnpackColor(uint32_t argb, float out[3])
{
    out[0] = ((argb >> 16) & 0xFF) / 255.0f;  // R
    out[1] = ((argb >>  8) & 0xFF) / 255.0f;  // G
    out[2] = ((argb      ) & 0xFF) / 255.0f;  // B
}

static inline float GetFloat(const std::map<uint32_t, float>& m, uint32_t crc, float def)
{
    std::map<uint32_t, float>::const_iterator it = m.find(crc);
    return it != m.end() ? it->second : def;
}

static inline uint32_t GetInt(const std::map<uint32_t, uint32_t>& m, uint32_t crc, uint32_t def)
{
    std::map<uint32_t, uint32_t>::const_iterator it = m.find(crc);
    return it != m.end() ? it->second : def;
}

void LevelScene::extractLighting(const ZeroEngine::LevelReader& reader)
{
    m_lightingExtracted = false;
    LevelLighting& L = m_lighting;

    // Defaults (reasonable fallback if objects not found)
    float defAmb[4]    = { 0.15f, 0.15f, 0.18f, 1.0f };
    float defSun[4]    = { 1.0f, 0.9f, 0.7f, 1.0f };
    float defDir[4]    = { 0.33f, 0.73f, 0.60f, 0.0f };
    float defDiff[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    float defScatP[4]  = { 1.0f, 0.007f, 0.0f, 0.03f };
    float defInsc[4]   = { 0.5f, 0.5f, 0.5f, 1.0f };
    float defExt[4]    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float defHG[4]     = { 0.4224f, 1.5776f, -1.52f, 0.0f };  // g=0.76
    float defMisc[4]   = { 1.0f, 0.25f, 0.3f, 0.2f };
    memcpy(L.ambient,       defAmb,   16);
    memcpy(L.sunCol,        defSun,   16);
    memcpy(L.sunDir,        defDir,   16);
    memcpy(L.diffCol,       defDiff,  16);
    memcpy(L.scatterParams, defScatP, 16);
    memcpy(L.inscatterCol,  defInsc,  16);
    memcpy(L.extinctCol,    defExt,   16);
    memcpy(L.scatterHG,     defHG,    16);
    memcpy(L.miscParams,    defMisc,  16);
    L.drawDist = 1800.0f;

    // CRCs for field names
    static const uint32_t CRC_AMBIENT       = ZeroEngine::LotrHashString("ambient");
    static const uint32_t CRC_AMBIENTSCALE  = ZeroEngine::LotrHashString("AmbientScale");
    static const uint32_t CRC_COLOR         = ZeroEngine::LotrHashString("Color");
    static const uint32_t CRC_COLORSCALE    = ZeroEngine::LotrHashString("ColorScale");
    static const uint32_t CRC_INSCATCOL     = ZeroEngine::LotrHashString("InscatteringColor");
    static const uint32_t CRC_EXTINCTCOL    = ZeroEngine::LotrHashString("ExtinctionColor");
    static const uint32_t CRC_ATMLIMIT      = ZeroEngine::LotrHashString("AtmosphereLimit");
    static const uint32_t CRC_DISTSCALE     = ZeroEngine::LotrHashString("DistanceScale");
    static const uint32_t CRC_DRAWDIST      = ZeroEngine::LotrHashString("DrawDistance");
    static const uint32_t CRC_AO_DIFF_OFF   = ZeroEngine::LotrHashString("AmbientOcclusionDiffuseOffset");
    static const uint32_t CRC_AO_DIFF_SCALE = ZeroEngine::LotrHashString("AmbientOcclusionDiffuseScale");
    static const uint32_t CRC_ATMFORCE      = ZeroEngine::LotrHashString("AtmosphereForce");
    static const uint32_t CRC_HAZEFORCE     = ZeroEngine::LotrHashString("HazeForce");
    static const uint32_t CRC_BETARAY       = ZeroEngine::LotrHashString("BetaRayMultipler");
    static const uint32_t CRC_BETAMIE       = ZeroEngine::LotrHashString("BetaMieMultipler");
    static const uint32_t CRC_HG            = ZeroEngine::LotrHashString("hg");
    static const uint32_t CRC_INSCATMULT    = ZeroEngine::LotrHashString("InscatteringMultiplier");
    static const uint32_t CRC_EXTINCTMULT   = ZeroEngine::LotrHashString("ExtinctionMultiplier");

    const std::vector<ZeroEngine::LevelGameObjEntry>& objs = reader.GetGameObjs();
    const ZeroEngine::LevelGameObjEntry* atm = NULL;
    const ZeroEngine::LevelGameObjEntry* sun = NULL;
    const ZeroEngine::LevelGameObjEntry* tmpl = NULL;

    for (size_t i = 0; i < objs.size(); ++i)
    {
        if (objs[i].type_name == "AtmosphereSetting" && !atm) atm = &objs[i];
        if (objs[i].type_name == "light_sun" && !sun) sun = &objs[i];
        if (objs[i].type_name == "templateLevel" && !tmpl) tmpl = &objs[i];
    }

    // --- AtmosphereSetting ---
    if (atm)
    {
        LevelLogf("[Lighting] Found AtmosphereSetting '%s' (GUID %u)", atm->name.c_str(), atm->guid);

        // Ambient color * AmbientScale
        uint32_t ambCol  = GetInt(atm->int_fields, CRC_AMBIENT, 0xFF404040);
        float    ambScale = GetFloat(atm->float_fields, CRC_AMBIENTSCALE, 1.0f);
        float ambRGB[3]; UnpackColor(ambCol, ambRGB);
        L.ambient[0] = ambRGB[0] * ambScale;
        L.ambient[1] = ambRGB[1] * ambScale;
        L.ambient[2] = ambRGB[2] * ambScale;
        // Game PS uses c1.w to scale the ambient cubemap contribution:
        // amb = cubeSample * NdN * c1.w * c72 + c1.rgb
        // Captured: c1.w = 1.0. Setting to 0 kills the cubemap entirely.
        L.ambient[3] = 1.0f;

        // AO parameters (game: c200 g__ambientOcclusionParms)
        L.miscParams[0] = GetFloat(atm->float_fields, CRC_AO_DIFF_SCALE, 1.0f);  // aoScale
        L.miscParams[1] = GetFloat(atm->float_fields, CRC_AO_DIFF_OFF, 0.25f);   // aoBias

        // Inscattering color + multiplier (game: c180 g__scatteringInscatteringColor)
        uint32_t inscatCol = GetInt(atm->int_fields, CRC_INSCATCOL, 0xFF808080);
        UnpackColor(inscatCol, L.inscatterCol);
        L.inscatterCol[3] = GetFloat(atm->float_fields, CRC_INSCATMULT, 1.0f);

        // Extinction color + multiplier (game: c181 g__scatteringExtinctionColor)
        uint32_t extinctCol = GetInt(atm->int_fields, CRC_EXTINCTCOL, 0xFFFFFFFF);
        UnpackColor(extinctCol, L.extinctCol);
        L.extinctCol[3] = GetFloat(atm->float_fields, CRC_EXTINCTMULT, 1.0f);

        // Scattering density (game: c182 g__scatteringConstant)
        float atmLimit  = GetFloat(atm->float_fields, CRC_ATMLIMIT, 150.0f);
        float distScale = GetFloat(atm->float_fields, CRC_DISTSCALE, 17.5f);
        float atmForce  = GetFloat(atm->float_fields, CRC_ATMFORCE, 1.0f);
        float hazeForce = GetFloat(atm->float_fields, CRC_HAZEFORCE, 0.0f);

        // Height falloff: derived from AtmosphereLimit (epsilon = 1/atmLimit)
        float heightFalloff = 1.0f / (atmLimit > 10.0f ? atmLimit : 10.0f);

        // Optical depth scale: distScale / atmLimit² gives proper fog distances
        // e.g. distScale=20, atmLimit=150 → optDepthScale=0.00089
        // → moderate fog at ~1000 units, heavy at ~2000, matches level scale
        //
        // Guard against atmLimit==0 — Ankh-Morpork and other Discworld
        // levels legitimately ship with AtmosphereLimit=0 (flat-world, no
        // atmospheric perspective). Without this clamp the denominator
        // goes to zero and optDepthScale prints as INF, downstream shaders
        // get infinity uniforms, and the fog pass produces NaN. Clamp to
        // the lower bound we already use for heightFalloff.
        float atmLimitClamped = (atmLimit > 10.0f) ? atmLimit : 10.0f;
        float baseDens = atmForce + hazeForce;
        if (baseDens < 0.001f) baseDens = 1.0f;
        float optDepthScale = distScale / (atmLimitClamped * atmLimitClamped * baseDens);

        L.scatterParams[0] = atmForce;       // atmosphere density
        L.scatterParams[1] = heightFalloff;   // height falloff (epsilon)
        L.scatterParams[2] = hazeForce;       // haze density
        L.scatterParams[3] = optDepthScale;   // optical depth scale

        // Henyey-Greenstein phase function params (game: c187 g__scatteringHG)
        float g = GetFloat(atm->float_fields, CRC_HG, 0.76f);
        L.scatterHG[0] = 1.0f - g * g;       // (1-g^2)
        L.scatterHG[1] = 1.0f + g * g;       // (1+g^2)
        L.scatterHG[2] = -2.0f * g;          // (-2g)
        L.scatterHG[3] = 0.0f;

        // Rayleigh/Mie inscatter strengths (from BetaRay/BetaMie multipliers)
        float betaRay = GetFloat(atm->float_fields, CRC_BETARAY, 1.0f);
        float betaMie = GetFloat(atm->float_fields, CRC_BETAMIE, 1.0f);
        float betaTotal = betaRay + betaMie;
        if (betaTotal < 0.001f) betaTotal = 1.0f;
        L.miscParams[2] = (betaRay / betaTotal) * 0.5f;  // Rayleigh strength
        L.miscParams[3] = (betaMie / betaTotal) * 0.5f;   // Mie strength

        // Scale ambient to match game capture (our extraction gives ~2.5x too high).
        // Game applies an additional reduction factor not present in entity data.
        // Captured: BlackGates ambient c1=(0.027,0.034,0.035) vs our extract (0.071,0.065,0.054).
        // Apply 0.4x correction. Floor matches captured BlackGates ambient (0.027,0.034,0.035).
        L.ambient[0] *= 0.4f; L.ambient[1] *= 0.4f; L.ambient[2] *= 0.4f;
        float minAmb = 0.027f;
        if (L.ambient[0] < minAmb) L.ambient[0] = minAmb;
        if (L.ambient[1] < minAmb) L.ambient[1] = minAmb;
        if (L.ambient[2] < minAmb) L.ambient[2] = minAmb;

        LevelLogf("[Lighting] ambient=(%.3f,%.3f,%.3f) scale=%.2f (clamped min=%.2f)",
                  L.ambient[0], L.ambient[1], L.ambient[2], ambScale, minAmb);
        LevelLogf("[Lighting] AO: scale=%.2f bias=%.2f",
                  L.miscParams[0], L.miscParams[1]);
        LevelLogf("[Lighting] inscatter=(%.3f,%.3f,%.3f) mult=%.2f",
                  L.inscatterCol[0], L.inscatterCol[1], L.inscatterCol[2], L.inscatterCol[3]);
        LevelLogf("[Lighting] extinct=(%.3f,%.3f,%.3f) mult=%.2f",
                  L.extinctCol[0], L.extinctCol[1], L.extinctCol[2], L.extinctCol[3]);
        LevelLogf("[Lighting] atmForce=%.3f hazeForce=%.3f hg=%.3f heightFalloff=%.5f optDepthScale=%.5f",
                  atmForce, hazeForce, g, heightFalloff, optDepthScale);
        // Yes the third arg here USED to be int literal 0 being printed
        // with %.0f. Passed a 4-byte int where a 8-byte double was asked
        // for. C varargs doesn't promote, it just grabs whatever 8 bytes
        // come next on the stack and prints them as a double. On PC that
        // was usually zeros, so nobody noticed. On BE load with different
        // stack state it came out as 3.27e+150 and I spent thirty seconds
        // thinking it was an endian bug. It's not. It's printf doing
        // exactly what you asked it to do and hating you for asking.
        LevelLogf("[Lighting] atmLimit=%.0f distScale=%.1f fullFogDist=%.0f",
                  atmLimit, distScale, 0.0f);
        LevelLogf("[Lighting] betaRay=%.2f betaMie=%.2f rayStr=%.3f mieStr=%.3f",
                  betaRay, betaMie, L.miscParams[2], L.miscParams[3]);
    }

    // --- light_sun (primary + secondary) ---
    L.hasSun2 = false;
    memset(L.sun2Col, 0, 16);
    memset(L.sun2Dir, 0, 16);

    // Collect all light_sun entities
    const ZeroEngine::LevelGameObjEntry* suns[4] = { NULL, NULL, NULL, NULL };
    int sunCount = 0;
    for (size_t i = 0; i < objs.size() && sunCount < 4; ++i) {
        if (objs[i].type_name == "light_sun")
            suns[sunCount++] = &objs[i];
    }

    if (sunCount > 0)
    {
        // Primary sun (first light_sun found)
        const ZeroEngine::LevelGameObjEntry* s0 = suns[0];
        LevelLogf("[Lighting] Found %d light_sun entities, primary='%s'", sunCount, s0->name.c_str());

        uint32_t sCol = GetInt(s0->int_fields, CRC_COLOR, 0xFFFFFFFF);
        float    sScale = GetFloat(s0->float_fields, CRC_COLORSCALE, 1.0f);
        float sRGB[3]; UnpackColor(sCol, sRGB);
        L.sunCol[0] = sRGB[0] * sScale;
        L.sunCol[1] = sRGB[1] * sScale;
        L.sunCol[2] = sRGB[2] * sScale;
        L.sunCol[3] = 1.0f;

        if (s0->has_transform) {
            L.sunDir[0] = s0->world_transform[8];
            L.sunDir[1] = s0->world_transform[9];
            L.sunDir[2] = s0->world_transform[10];
            L.sunDir[3] = 0.0f;
        }

        LevelLogf("[Lighting] sun0 color=(%.3f,%.3f,%.3f) scale=%.1f dir=(%.3f,%.3f,%.3f)",
                  L.sunCol[0], L.sunCol[1], L.sunCol[2], sScale,
                  L.sunDir[0], L.sunDir[1], L.sunDir[2]);

        // Secondary sun from level data, or auto-generated back-fill
        if (sunCount >= 2) {
            const ZeroEngine::LevelGameObjEntry* s1 = suns[1];
            uint32_t s1Col = GetInt(s1->int_fields, CRC_COLOR, 0xFFFFFFFF);
            float    s1Scale = GetFloat(s1->float_fields, CRC_COLORSCALE, 1.0f);
            float s1RGB[3]; UnpackColor(s1Col, s1RGB);
            L.sun2Col[0] = s1RGB[0] * s1Scale * 0.5f; // fill at half intensity
            L.sun2Col[1] = s1RGB[1] * s1Scale * 0.5f;
            L.sun2Col[2] = s1RGB[2] * s1Scale * 0.5f;
            L.sun2Col[3] = 1.0f;
            if (s1->has_transform) {
                L.sun2Dir[0] = s1->world_transform[8];
                L.sun2Dir[1] = s1->world_transform[9];
                L.sun2Dir[2] = s1->world_transform[10];
                L.sun2Dir[3] = 0.0f;
            }
            L.hasSun2 = true;
            LevelLogf("[Lighting] sun1 (from level) color=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f)",
                      L.sun2Col[0], L.sun2Col[1], L.sun2Col[2],
                      L.sun2Dir[0], L.sun2Dir[1], L.sun2Dir[2]);
        } else {
            // Auto-generated back-fill. Single-sun levels need SOMETHING
            // pushing photons onto the back of every silhouette or the
            // shadow side falls to pure motherfucking black like we are
            // running on a 1996 Voodoo.
            //
            // The old recipe was pure rookie-tier garbage: stamp a flat
            // (0.15, 0.20, 0.30) tint on top of the sun color. Read that
            // ratio. TWO TIMES MORE BLUE THAN RED. We were force-feeding
            // every Pandemic level a fucking Instagram cool filter on top
            // of whatever the actual atmosphere data said. Mordor's red
            // dusk, Helm's torchlight, Black Gate's ash haze, all of them
            // came out the same shade of "winter morning at a Stockholm
            // bus stop". The level data was warm. The artists picked
            // warm. WE were the goddamn frostbite.
            //
            // New recipe: desaturate the real sun color a touch so it
            // reads as bounce instead of a second key, drop it to 20%
            // luminance, do not pre-shift the hue. Whatever mood the
            // artist baked into the PAK is the mood we ship. Eat it.
            L.sun2Dir[0] = -L.sunDir[0];
            L.sun2Dir[1] =  0.3f; // slightly from above, sky-direction bounce
            L.sun2Dir[2] = -L.sunDir[2];
            float len = sqrtf(L.sun2Dir[0]*L.sun2Dir[0] + L.sun2Dir[1]*L.sun2Dir[1] + L.sun2Dir[2]*L.sun2Dir[2]);
            if (len > 0.001f) { L.sun2Dir[0]/=len; L.sun2Dir[1]/=len; L.sun2Dir[2]/=len; }
            L.sun2Dir[3] = 0.0f;
            float sunLum = L.sunCol[0] * 0.299f + L.sunCol[1] * 0.587f + L.sunCol[2] * 0.114f;
            const float backFillStr = 0.20f; // 20% of sun energy
            const float desat       = 0.70f; // 70% sun hue, 30% neutral
            L.sun2Col[0] = (L.sunCol[0] * desat + sunLum * (1.0f - desat)) * backFillStr;
            L.sun2Col[1] = (L.sunCol[1] * desat + sunLum * (1.0f - desat)) * backFillStr;
            L.sun2Col[2] = (L.sunCol[2] * desat + sunLum * (1.0f - desat)) * backFillStr;
            L.sun2Col[3] = 1.0f;
            L.hasSun2 = true;
            LevelLogf("[Lighting] sun1 (auto back-fill, neutral) color=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f,%.3f)",
                      L.sun2Col[0], L.sun2Col[1], L.sun2Col[2],
                      L.sun2Dir[0], L.sun2Dir[1], L.sun2Dir[2]);
        }
    }

    // --- templateLevel (for DrawDistance) ---
    if (tmpl)
    {
        L.drawDist = GetFloat(tmpl->float_fields, CRC_DRAWDIST, 1800.0f);
        LevelLogf("[Lighting] DrawDistance=%.0f", L.drawDist);
    }

    // Dome colors (hemisphere ambient).
    //
    // The old fallback was (0.02, 0.03, 0.05) on the top dome. Read that
    // shit again: blue value is more than DOUBLE the red. Then on top of
    // that, when the atmosphere object existed but the TopDome CRC was
    // missing, we'd default to 0xFF333355 which is ALSO fucking blue.
    // Two layers of "if all else fails, smear blue on it". On every
    // level whose AtmosphereSetting we miss-parse for any goddamn
    // reason, the world inherited a permanent freezer-burn cast that
    // had ZERO basis in the actual game data.
    //
    // Neutral grey is the correct "no opinion" fallback. If the artist
    // wanted blue, the artist will have written blue into the PAK and
    // we'll read it. If the read fails, default to nothing instead of
    // committing a hue crime.
    L.topDomeCol[0] = 0.04f; L.topDomeCol[1] = 0.04f; L.topDomeCol[2] = 0.04f; L.topDomeCol[3] = 0;
    L.botDomeCol[0] = 0.06f; L.botDomeCol[1] = 0.05f; L.botDomeCol[2] = 0.04f; L.botDomeCol[3] = 0;
    if (atm) {
        static const uint32_t CRC_TOPDOME   = ZeroEngine::LotrHashString("TopDomeColor");
        static const uint32_t CRC_TOPSCALE  = ZeroEngine::LotrHashString("TopDomeColorScale");
        static const uint32_t CRC_BOTDOME   = ZeroEngine::LotrHashString("BottomDomeColor");
        static const uint32_t CRC_BOTSCALE  = ZeroEngine::LotrHashString("BottomDomeColorScale");
        // Per-field defaults: atmosphere object exists, dome CRC missing.
        // Neutral grey, NOT the old blue 0xFF333355. Same disease, same
        // cure.
        uint32_t topCol = GetInt(atm->int_fields, CRC_TOPDOME, 0xFF454545);
        float topScale  = GetFloat(atm->float_fields, CRC_TOPSCALE, 1.0f);
        uint32_t botCol = GetInt(atm->int_fields, CRC_BOTDOME, 0xFF554433);
        float botScale  = GetFloat(atm->float_fields, CRC_BOTSCALE, 1.0f);
        float tRGB[3], bRGB[3]; UnpackColor(topCol, tRGB); UnpackColor(botCol, bRGB);
        // Scale and clamp to reasonable hemisphere ambient (0-0.15 range)
        // Scale dome colors to match game's low ambient (~0.03)
        // Game uses ambient cube map for fill; we approximate with hemisphere
        for (int c = 0; c < 3; ++c) {
            L.topDomeCol[c] = tRGB[c] * topScale * 0.02f;
            L.botDomeCol[c] = bRGB[c] * botScale * 0.02f;
            if (L.topDomeCol[c] > 0.06f) L.topDomeCol[c] = 0.06f;
            if (L.botDomeCol[c] > 0.06f) L.botDomeCol[c] = 0.06f;
        }
        LevelLogf("[Lighting] TopDome=(%.3f,%.3f,%.3f) BotDome=(%.3f,%.3f,%.3f)",
                  L.topDomeCol[0], L.topDomeCol[1], L.topDomeCol[2],
                  L.botDomeCol[0], L.botDomeCol[1], L.botDomeCol[2]);
    }

    // Tone mapping params from AtmosphereSetting
    L.exposure = 1.0f;
    L.gamma = 1.1f;
    L.whitepoint = 2.0f;
    L.bloomThreshold = 0.85f;
    if (atm) {
        static const uint32_t CRC_KEY      = ZeroEngine::LotrHashString("key");
        static const uint32_t CRC_GAMMAR   = ZeroEngine::LotrHashString("GammaR");
        static const uint32_t CRC_WHITEPT  = ZeroEngine::LotrHashString("Whitepoint");
        static const uint32_t CRC_BLOOMTHR = ZeroEngine::LotrHashString("BloomThreshold");
        L.exposure = GetFloat(atm->float_fields, CRC_KEY, 1.0f);
        L.gamma = GetFloat(atm->float_fields, CRC_GAMMAR, 1.1f);
        L.whitepoint = GetFloat(atm->float_fields, CRC_WHITEPT, 2.0f);
        L.bloomThreshold = GetFloat(atm->float_fields, CRC_BLOOMTHR, 0.85f);
        // Clamp bloom threshold to reasonable range (game uses 0.2-0.3 but per-object inline is different)
        if (L.bloomThreshold < 0.5f) L.bloomThreshold = 0.7f;
        LevelLogf("[Lighting] ToneMap: exposure=%.2f gamma=%.2f whitepoint=%.1f bloomThreshold=%.2f",
                  L.exposure, L.gamma, L.whitepoint, L.bloomThreshold);
    }

    m_lightingExtracted = true;
}

// ─────────────────────────────────────────────────────────────────────────
//  XBOX 360 DXT TEXTURE UNTILE + U16 BYTE-SWAP
// ─────────────────────────────────────────────────────────────────────────
//
// Pandemic shipped 360 DXT textures in two layers of platform-native
// weirdness on top of the standard DXT1/DXT5 block format:
//
//   1. SWIZZLE: blocks are stored in a 32x32-block tiled layout, not
//      linear row-major. The 360 GPU sampler hardware reads them in
//      tiled order natively. PC samplers want linear. Without untile,
//      every texture renders with its 32x32 block tiles SCRAMBLED into
//      the wrong spatial positions - rainbow garbage tiles even when
//      individual pixel colors are right.
//
//   2. U16-PAIR BYTE SWAP: within each DXT block, every adjacent pair
//      of bytes is byte-swapped (16-bit color endpoints AND 32-bit
//      index portions both stored as BE u16-pairs). Without this,
//      colors come out wrong (endpoints inverted) and index-to-color
//      mapping is shuffled (pixels pick wrong endpoint) - washed-out
//      whites, pinks, and greens that don't match the actual texture.
//
// Port of conv_img_dxt1 / conv_img_dxt5 from the Rust parser
// (lotrc-rust/src/bin.rs lines 577-643). Same swizzle math, same
// per-u16 swap pattern. Validated against the Rust parser's output.
//
// totalBlocksH/W = image dimensions divided by 4 (one DXT block covers
// 4x4 pixels). For a 512x512 texture, totalBlocksH = totalBlocksW = 128.
// ─────────────────────────────────────────────────────────────────────────

static void Untile360DXT1_C(const uint8_t* src, size_t srcSize,
                            std::vector<uint8_t>& out,
                            int totalBlocksH, int totalBlocksW)
{
    out.assign((size_t)totalBlocksH * totalBlocksW * 8, 0);

    // Tiles in 32-block (32*4 = 128 pixel) units. Min 1 to handle tiny mips
    // where the image is smaller than one full tile - 360 still reserves
    // the full 32x32 tile worth of memory even for a 4x4 mip.
    int wTiles = totalBlocksW >> 5;
    if (wTiles < 1) wTiles = 1;
    int hTiles = totalBlocksH >> 5;
    if (hTiles < 1) hTiles = 1;

    for (int j = 0; j < hTiles * wTiles; ++j) {
        int xTile = (j % wTiles) << 5;
        int yTile = (j / wTiles) << 5;
        for (int i = 0; i < 1024; ++i) {
            size_t k = ((size_t)i + ((size_t)j << 10)) << 3;
            if (k + 8 > srcSize) break;
            int x = xTile + ((((i >> 5) & 16) + i) & 24)
                          + ((i >> 3) & 4) + ((i >> 1) & 2) + (i & 1);
            int y = yTile + ((i >> 6) & 8) + (((i >> 4) & 16))
                          + ((i >> 5) & 6) + ((i >> 1) & 1);
            if (x < 0 || x >= totalBlocksW || y < 0 || y >= totalBlocksH) continue;
            size_t dstOff = (size_t)((y * totalBlocksW + x) << 3);
            for (int b = 0; b < 4; ++b) {
                out[dstOff + 2*b]     = src[k + 2*b + 1];
                out[dstOff + 2*b + 1] = src[k + 2*b];
            }
        }
    }
}

static void Untile360DXT5_C(const uint8_t* src, size_t srcSize,
                            std::vector<uint8_t>& out,
                            int totalBlocksH, int totalBlocksW)
{
    out.assign((size_t)totalBlocksH * totalBlocksW * 16, 0);

    int wTiles = totalBlocksW >> 5;
    if (wTiles < 1) wTiles = 1;
    int hTiles = totalBlocksH >> 5;
    if (hTiles < 1) hTiles = 1;

    for (int j = 0; j < hTiles * wTiles; ++j) {
        int xTile = (j % wTiles) << 5;
        int yTile = (j / wTiles) << 5;
        for (int i = 0; i < 1024; ++i) {
            size_t k = ((size_t)i + ((size_t)j << 10)) << 4;
            if (k + 16 > srcSize) break;
            int x = xTile + ((((i >> 5) & 16) + (i << 1)) & 24)
                          + ((i >> 3) & 6) + ((i >> 1) & 1);
            int y = yTile + ((i >> 6) & 8) + (((i >> 3) & 16))
                          + ((i >> 6) & 4) + ((i >> 5) & 2) + (i & 1);
            if (x < 0 || x >= totalBlocksW || y < 0 || y >= totalBlocksH) continue;
            size_t dstOff = (size_t)((y * totalBlocksW + x) << 4);
            for (int b = 0; b < 8; ++b) {
                out[dstOff + 2*b]     = src[k + 2*b + 1];
                out[dstOff + 2*b + 1] = src[k + 2*b];
            }
        }
    }
}

// ── getOrLoadTexture ──────────────────────────────────────────────────────────
// Finds the TextureInfo entry with the given CRC, retrieves raw DDS bytes from
// the BIN file, and creates a D3D9 texture.  Result is cached by CRC.

IDirect3DTexture9* LevelScene::getOrLoadTexture(
    const ZeroEngine::LevelReader& reader,
    uint32_t                       texCrc)
{
    if (texCrc == 0) return NULL;

    std::map<uint32_t, IDirect3DTexture9*>::iterator it = m_texCache.find(texCrc);
    if (it != m_texCache.end()) return it->second;

    IDirect3DTexture9* tex = NULL;

    // Find TextureInfo entry with this CRC → get asset_key for BIN lookup
    const std::vector<ZeroEngine::LevelTextureEntry>& textures = reader.GetTextures();
    uint32_t assetKey  = 0;
    uint32_t texW = 1, texH = 1, texLevels = 1, texFmt = 10;
    bool     foundCrc  = false;
    for (size_t i = 0; i < textures.size(); ++i)
    {
        if (textures[i].crc == texCrc)
        {
            assetKey  = textures[i].asset_key;
            texW      = textures[i].width  ? textures[i].width  : 1u;
            texH      = textures[i].height ? textures[i].height : 1u;
            texLevels = textures[i].levels ? textures[i].levels : 1u;
            texFmt    = textures[i].format;
            foundCrc  = true;
            break;
        }
    }

    // Log first 5 unique texture lookups to the debug file
    static int s_texLogCount = 0;
    if (s_texLogCount < 5)
    {
        FILE* tf = fopen("levelscene_texdbg.txt",
                         s_texLogCount == 0 ? "w" : "a");
        if (tf)
        {
            fprintf(tf, "Tex[%d] crc=0x%08X foundInTable=%s assetKey=0x%08X w=%u h=%u fmt=%u lvl=%u\n",
                    s_texLogCount, texCrc, foundCrc ? "yes" : "NO", assetKey,
                    texW, texH, texFmt, texLevels);
            fclose(tf);
        }
        ++s_texLogCount;
    }

    if (assetKey != 0)
    {
        // Texture-only lookup. If a model in the BIN happens to share
        // this asset's key (CRD body and its baked atlas reusing the
        // body's name), the generic GetBinAssetData would return the
        // model handle and we would treat its mesh bytes as DDS.
        std::vector<uint8_t> ddsData;
        bool gotData = reader.GetBinTextureData(assetKey, ddsData);
        if (s_texLogCount <= 5)
        {
            FILE* tf = fopen("levelscene_texdbg.txt", "a");
            if (tf)
            {
                fprintf(tf, "  -> GetBinTextureData=%s ddsSize=%u\n",
                        gotData ? "ok" : "FAIL", (unsigned)ddsData.size());
                fclose(tf);
            }
        }

        // ── BIG-ENDIAN: untile + u16-swap DXT blocks ──────────────────────
        // 360 textures land here as raw DXT bytes that are (a) tiled in
        // 32x32-block swizzle and (b) per-u16 byte-swapped within each
        // block. Both must be undone or D3DXCreateTextureFromFileInMemoryEx
        // happily accepts the bytes and renders rainbow garbage that looks
        // half-correct - colors approximately right in regions, spatial
        // layout completely scrambled. The two undo passes are fused into
        // one Untile360DXT* helper above. Only DXT1 (fmt 7,8) and DXT5
        // (fmt 10,11,12,17) are 360-tiled - uncompressed formats (RGB565,
        // ARGB) are stored linear and need no untile, just a u32-swap of
        // the pixel data. Defer uncompressed handling - Discworld leans
        // hard on DXT for almost everything.
        if (gotData && reader.IsBigEndian() && !ddsData.empty()) {
            bool isDXT1 = (texFmt == 7 || texFmt == 8);
            bool isDXT5 = (texFmt == 10 || texFmt == 11 || texFmt == 12 || texFmt == 17);
            if (isDXT1 || isDXT5) {
                int totalBlocksW = (int)((texW + 3u) / 4u);
                int totalBlocksH = (int)((texH + 3u) / 4u);
                std::vector<uint8_t> untiled;
                if (isDXT1) {
                    Untile360DXT1_C(&ddsData[0], ddsData.size(),
                                    untiled, totalBlocksH, totalBlocksW);
                } else {
                    Untile360DXT5_C(&ddsData[0], ddsData.size(),
                                    untiled, totalBlocksH, totalBlocksW);
                }
                ddsData.swap(untiled);
            }
        }

        if (gotData && !ddsData.empty())
        {
            // BIN stores raw pixel data without a DDS header — build one from TextureInfo
            bool hasDDSMagic = (ddsData.size() >= 4 &&
                                ddsData[0]=='D' && ddsData[1]=='D' &&
                                ddsData[2]=='S' && ddsData[3]==' ');

            const uint8_t* pData   = &ddsData[0];
            UINT           dataLen = (UINT)ddsData.size();
            std::vector<uint8_t> fullDDS;

            if (!hasDDSMagic)
            {
                // Map LOTRC format id → DDS pixel format description
                uint32_t fourCC     = 0;
                uint32_t pfFlags    = 0;
                uint32_t rgbBits    = 0;
                uint32_t rM=0, gM=0, bM=0, aM=0;
                bool     compressed = true;

                switch (texFmt)
                {
                case 7: case 8:
                    fourCC = 0x31545844u; break;          // DXT1
                case 9:
                    fourCC = 0x33545844u; break;          // DXT3
                case 10: case 11: case 12: case 17:
                    fourCC = 0x35545844u; break;          // DXT5
                case 0:   // R5G6B5
                    compressed=false; pfFlags=0x40u; rgbBits=16;
                    rM=0xF800u; gM=0x07E0u; bM=0x001Fu; break;
                case 1:   // A4R4G4B4
                    compressed=false; pfFlags=0x41u; rgbBits=16;
                    aM=0xF000u; rM=0x0F00u; gM=0x00F0u; bM=0x000Fu; break;
                case 2:   // A1R5G5B5
                    compressed=false; pfFlags=0x41u; rgbBits=16;
                    aM=0x8000u; rM=0x7C00u; gM=0x03E0u; bM=0x001Fu; break;
                case 4:   // X8R8G8B8
                    compressed=false; pfFlags=0x40u; rgbBits=32;
                    rM=0x00FF0000u; gM=0x0000FF00u; bM=0x000000FFu; break;
                case 6:   // A8
                    compressed=false; pfFlags=0x02u; rgbBits=8; aM=0xFFu; break;
                default:
                    fourCC = 0x35545844u; break;          // unknown → assume DXT5
                }

                // BIN assets store only the base mip level — always use 1
                bool     hasMips   = false;
                uint32_t headerMips = 1u;
                uint32_t blockSize = (fourCC == 0x31545844u) ? 8u : 16u;
                uint32_t linSize   = compressed
                    ? (((texW+3u)/4u) * ((texH+3u)/4u) * blockSize)
                    : (texW * (rgbBits / 8u));
                if (linSize == 0) linSize = blockSize;

                uint32_t ddFlags = 0x1007u;              // CAPS|HEIGHT|WIDTH|PIXELFMT
                ddFlags |= compressed ? 0x80000u : 0x8u; // LINEARSIZE or PITCH
                if (hasMips) ddFlags |= 0x20000u;        // MIPMAPCOUNT

                uint32_t caps1 = 0x1000u;                // DDSCAPS_TEXTURE
                if (hasMips) caps1 |= 0x400008u;         // MIPMAP|COMPLEX

                uint8_t h128[128]; memset(h128, 0, 128);
                h128[0]='D'; h128[1]='D'; h128[2]='S'; h128[3]=' ';
                { uint32_t v=124u; memcpy(h128+ 4, &v, 4); }
                memcpy(h128+ 8, &ddFlags,                 4);
                memcpy(h128+12, &texH,                    4);
                memcpy(h128+16, &texW,                    4);
                memcpy(h128+20, &linSize,                 4);
                memcpy(h128+28, &headerMips,              4);
                { uint32_t v=32u;  memcpy(h128+76,  &v, 4); }
                { uint32_t v = compressed ? 4u : pfFlags; memcpy(h128+80, &v, 4); }
                memcpy(h128+84, &fourCC,                  4);
                memcpy(h128+88, &rgbBits,                 4);
                memcpy(h128+92,  &rM,                     4);
                memcpy(h128+96,  &gM,                     4);
                memcpy(h128+100, &bM,                     4);
                memcpy(h128+104, &aM,                     4);
                memcpy(h128+108, &caps1,                  4);

                fullDDS.resize(128 + ddsData.size());
                memcpy(&fullDDS[0],   h128,        128);
                memcpy(&fullDDS[128], &ddsData[0], ddsData.size());
                pData   = &fullDDS[0];
                dataLen = (UINT)fullDDS.size();
            }

            HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
                m_device, pData, dataLen,
                D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0,
                D3DFMT_UNKNOWN, D3DPOOL_MANAGED,
                D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex);
            if (s_texLogCount <= 5)
            {
                FILE* tf = fopen("levelscene_texdbg.txt", "a");
                if (tf)
                {
                    fprintf(tf, "  -> D3DXCreate hr=0x%08X tex=%s\n",
                            (unsigned)hr, tex ? "OK" : "NULL");
                    fclose(tf);
                }
            }
            if (FAILED(hr)) tex = NULL;
        }
    }

    m_texCache[texCrc] = tex;
    return tex;
}

// ── loadModelFromBinary ───────────────────────────────────────────────────────
// Decodes one model's LOD0 static geometry from Block1/Block2 and fills 'out'
// with one LevelMeshPart per active BufferInfo slot.

bool LevelScene::loadModelFromBinary(
    const ZeroEngine::LevelReader& reader,
    uint32_t                       modelCrc,
    const std::string&             modelName,
    LevelModel*                    out)
{
    using namespace ZeroEngine;

    // Skip-reason log — opened once per process, all skips appended
    static FILE* sf = fopen("levelscene_skiplog.txt", "w");

    const PakHeader&            hdr = reader.GetPakHeader();
    const std::vector<uint8_t>& b1  = reader.GetBlock1();

    if (b1.empty() || !m_device) return false;

    // ── Find ModelInfo by CRC ─────────────────────────────────────────────────
    if (hdr.model_info_num == 0 || hdr.model_info_offset == 0) return false;
    {
        uint64_t need = (uint64_t)hdr.model_info_offset
                      + (uint64_t)sizeof(ModelInfo) * hdr.model_info_num;
        if (need > b1.size()) return false;
    }

    const ModelInfo* models =
        reinterpret_cast<const ModelInfo*>(&b1[hdr.model_info_offset]);
    int mi = -1;
    for (uint32_t i = 0; i < hdr.model_info_num; ++i)
        if (models[i].key == modelCrc) { mi = (int)i; break; }
    if (mi < 0)
    {
        if (sf) { fprintf(sf, "SKIP NOT_IN_MODELINFO crc=0x%08X name=%s\n", modelCrc, modelName.c_str()); fflush(sf); }
        return false;
    }

    const ModelInfo& mdl = models[mi];

    // ── Read bone NAMES (CRC array) for Crowd Mesh Builder attach-bone
    // dropdown. The bones_offset table is parallel to bone_parents +
    // bone_transforms — same `bones_num` entries, each a 4-byte CRC of
    // the bone's name string. We resolve every CRC through the reader's
    // string table; anything that doesn't resolve becomes "bone_<hex>"
    // so the user can still pick it from the dropdown without losing
    // the slot. Models with no skeleton (props, terrain) leave out->bones
    // empty, which is fine — Crowd Mesh Builder just won't list them.
    if (mdl.bones_num > 0 && mdl.bones_offset != 0)
    {
        uint64_t bnEnd = (uint64_t)mdl.bones_offset + (uint64_t)mdl.bones_num * 4u;
        if (bnEnd <= b1.size())
        {
            const uint32_t* boneCrcs =
                reinterpret_cast<const uint32_t*>(&b1[mdl.bones_offset]);
            out->bones.reserve(mdl.bones_num);
            for (uint32_t b = 0; b < mdl.bones_num; ++b)
            {
                uint32_t bcrc = boneCrcs[b];
                std::string bn = reader.ResolveCrc(bcrc);
                if (bn.empty()) {
                    char hex[16];
                    sprintf(hex, "bone_%08X", bcrc);
                    bn = hex;
                }
                out->bones.push_back(bn);
            }
        }
    }

    // ── Read bone/skin data for breakable mesh rest-pose pre-skinning ─────────
    bool hasSkinData = false;
    std::vector<float> restSkinMats;
    if (mdl.skin_binds_num > 0 && mdl.bones_num > 0
        && mdl.bone_transforms_offset != 0 && mdl.skin_binds_offset != 0
        && mdl.skin_order_offset != 0 && mdl.bone_parents_offset != 0)
    {
        uint64_t btEnd = (uint64_t)mdl.bone_transforms_offset + (uint64_t)mdl.bones_num * 64u;
        uint64_t sbEnd = (uint64_t)mdl.skin_binds_offset + (uint64_t)mdl.skin_binds_num * 64u;
        uint64_t soEnd = (uint64_t)mdl.skin_order_offset + (uint64_t)mdl.skin_binds_num * 4u;
        uint64_t bpEnd = (uint64_t)mdl.bone_parents_offset + (uint64_t)mdl.bones_num * 4u;

        if (btEnd <= b1.size() && sbEnd <= b1.size() && soEnd <= b1.size() && bpEnd <= b1.size())
        {
            const float*    boneXforms  = reinterpret_cast<const float*>(&b1[mdl.bone_transforms_offset]);
            const float*    skinBinds   = reinterpret_cast<const float*>(&b1[mdl.skin_binds_offset]);
            const uint32_t* skinOrder   = reinterpret_cast<const uint32_t*>(&b1[mdl.skin_order_offset]);
            const int32_t*  boneParents = reinterpret_cast<const int32_t*>(&b1[mdl.bone_parents_offset]);

            std::vector<float> boneWorld(mdl.bones_num * 16);
            for (uint32_t b = 0; b < mdl.bones_num; ++b)
            {
                const float* local = &boneXforms[b * 16];
                int32_t par = boneParents[b];
                if (par < 0 || (uint32_t)par >= mdl.bones_num) {
                    memcpy(&boneWorld[b * 16], local, 64);
                } else {
                    const float* pw = &boneWorld[(uint32_t)par * 16];
                    float* w = &boneWorld[b * 16];
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            w[r*4+c] = local[r*4+0]*pw[0*4+c] + local[r*4+1]*pw[1*4+c]
                                      + local[r*4+2]*pw[2*4+c] + local[r*4+3]*pw[3*4+c];
                }
            }

            // Cache rest-pose world matrices on the model for the Crowd Mesh
            // Builder preview to look up "where is bone X in this model?" later.
            out->restBoneWorld = boneWorld;

            // ============================================================
            // AUTHORITATIVE SKIN PALETTE: the matrices Pandemic actually shipped
            // ============================================================
            // restBoneWorld above is OUR parent-cascade computation. It drifts
            // from what Pandemic's content build tool baked into the skin_binds
            // (inverse-bind matrices) by up to 10+ degrees of rotation. The
            // preview MUST use the authoritative bind for bones in the palette,
            // or the visible preview position lies about where the bake will
            // actually land the weapon. Cache both arrays raw so the preview
            // matrix calc can look them up cheaply.
            out->skinOrder.assign(skinOrder, skinOrder + mdl.skin_binds_num);
            out->skinBindsRowMajor.assign(skinBinds, skinBinds + mdl.skin_binds_num * 16);

            restSkinMats.resize(mdl.skin_binds_num * 16);
            hasSkinData = true;
            for (uint32_t i = 0; i < mdl.skin_binds_num; ++i)
            {
                uint32_t boneIdx = skinOrder[i];
                if (boneIdx >= mdl.bones_num) { hasSkinData = false; break; }

                const float* sb = &skinBinds[i * 16];
                const float* bw = &boneWorld[boneIdx * 16];
                float* sm = &restSkinMats[i * 16];

                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        sm[r*4+c] = sb[r*4+0]*bw[0*4+c] + sb[r*4+1]*bw[1*4+c]
                                  + sb[r*4+2]*bw[2*4+c] + sb[r*4+3]*bw[3*4+c];
            }
        }
    }

    // ── Load per-model BIN asset (contains vertex + index buffer data) ────────
    // VBuffInfo.offset and IBuffInfo.offset are byte offsets into this buffer,
    // NOT into Block2.  asset_key == model's own CRC in practice.
    std::vector<uint8_t> binAsset;
    if (mdl.asset_key == 0 || !reader.GetBinAssetData(mdl.asset_key, binAsset)
        || binAsset.empty())
    {
        if (sf) fprintf(sf, "SKIP NO_BIN_ASSET crc=0x%08X asset_key=0x%08X name=%s\n",
                        modelCrc, mdl.asset_key, modelName.c_str());
        return false;
    }

    // ── BIG-ENDIAN SURGICAL SWAP ──────────────────────────────────────────
    // On BE (360/PS3) files GetBinAssetData now returns RAW bytes. We own
    // the swap here because only THIS code knows which byte ranges inside
    // binAsset are vertex data (swap as u32) vs index data (swap as u16).
    // The blanket u32-swap we used to do in GetBinAssetData pair-scrambled
    // every triangle's index pair - user saw empty terrain with scattered
    // wireframe "atoms" where triangles should be. This dedupe set tracks
    // which regions we already swapped within this asset, because two
    // mesh slots can point to the same vbuff/ibuff and we MUST NOT swap
    // the same bytes twice (un-swap == disaster).
    const bool binIsBigEndian = reader.IsBigEndian();
    std::set<uint32_t> swappedVbuffOffsets;
    std::set<uint32_t> swappedIbuffOffsets;

    // Use breakable_end as the full LOD0 range end.
    // Props (ladders, towers, barrels) have geometry in the BREAKABLE section
    // (static_end == start == 0), not just the STATIC section.
    // Blender's imp/model.py renders all of [start..breakable_end] for LOD0.
    int lod0Start = (int)mdl.lod0.start;
    int lod0End   = (int)mdl.lod0.breakable_end;
    if (lod0End <= lod0Start)
    {
        if (sf) fprintf(sf, "SKIP NO_LOD0 crc=0x%08X lod0=[%d..%d] breakable_end=%d name=%s\n",
                        modelCrc, lod0Start, (int)mdl.lod0.static_end, lod0End, modelName.c_str());
        return false;
    }

    // ── Validate mesh_order offset ────────────────────────────────────────────
    if (mdl.mesh_order_offset == 0 || mdl.mesh_order_offset >= (uint32_t)b1.size())
    {
        if (sf) fprintf(sf, "SKIP BAD_MESH_ORDER_OFF crc=0x%08X off=%u name=%s\n",
                        modelCrc, mdl.mesh_order_offset, modelName.c_str());
        return false;
    }
    if (mdl.buffer_info_offset == 0 || mdl.buffer_info_offset >= (uint32_t)b1.size())
    {
        if (sf) fprintf(sf, "SKIP BAD_BUF_INFO_OFF crc=0x%08X off=%u name=%s\n",
                        modelCrc, mdl.buffer_info_offset, modelName.c_str());
        return false;
    }
    if (hdr.buffer_info_size < 268) return false;  // need at least ibuff_info_offset at byte 260

    const uint32_t* meshOrder =
        reinterpret_cast<const uint32_t*>(&b1[mdl.mesh_order_offset]);

    // ── Iterate LOD0 static mesh slots ────────────────────────────────────────
    // TARGETED DIAGNOSTIC: when loading the user's bow embed test, log every
    // continue with file:line context. Lets us track down exactly which check
    // rejects the merged model since the generic NO_VISUAL_PARTS skiplog
    // entry only says "nothing made it through" without naming the culprit.
    const bool _dbgEmbed = (modelName == "CRD_CH_elf_ancn_bowtest_01"
                         || modelName == "CRD_CH_elf_ancn_stripped_01"
                         || modelName.find("bowtest") != std::string::npos
                         || modelName.find("elf_ancn_swd_03") != std::string::npos);
    if (_dbgEmbed)
        LevelLogf("[BowDbg] loadModelFromBinary START name=%s lod0=[%d..%d] mat_num=%u vbuff_num=%u",
                  modelName.c_str(), lod0Start, lod0End, mdl.mat_num, mdl.vbuff_num);

    for (int slot = lod0Start; slot < lod0End; ++slot)
    {
        // Bounds-check the meshOrder read
        uint32_t moByteEnd = mdl.mesh_order_offset + ((uint32_t)slot + 1u) * 4u;
        if (moByteEnd > (uint32_t)b1.size()) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] BAIL moByteEnd OOR (%u > %u)",
                                     slot, moByteEnd, (unsigned)b1.size());
            break;
        }

        uint32_t mo     = meshOrder[slot];
        // Bit 31 = visual geometry, Bit 30 = collision/shadow mesh.
        // Skip any slot that has ONLY bit 30 set (explicit collision marker).
        if ((mo & 0xC0000000u) == 0x40000000u) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] SKIP (collision-only flag, mo=0x%08X)",
                                     slot, mo);
            continue;
        }
        uint32_t bufIdx = mo & 0x3FFFFFFFu;
        if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] mo=0x%08X bufIdx=%u", slot, mo, bufIdx);

        // ── Read BufferInfo entry (each is hdr.buffer_info_size = 356 bytes) ──
        // Confirmed field layout from Rust source (lotrc-0.6.0/src/pak/mod.rs):
        //   byte   0: vbuff_info_offset   — byte ptr into Block1 for VBuffInfo
        //   byte   4: vbuff_info_offset_2 — alt byte ptr (0 = unused)
        //   byte 128: v_size              — vertex stride for vbuff_0
        //   byte 132: v_size_2            — vertex stride for vbuff_2
        //   byte 260: ibuff_info_offset   — byte ptr into Block1 for IBuffInfo
        //   byte 264: i_num               — number of indices
        uint32_t biByteOff = mdl.buffer_info_offset + bufIdx * hdr.buffer_info_size;
        if ((uint64_t)biByteOff + hdr.buffer_info_size > b1.size()) continue;
        const uint8_t* bi = &b1[biByteOff];

        uint32_t vbInfoOff0, vbInfoOff2, vStride0, vStride2, ibInfoOff, i_num;
        memcpy(&vbInfoOff0, bi +   0, 4);
        memcpy(&vbInfoOff2, bi +   4, 4);
        memcpy(&vStride0,   bi + 128, 4);
        memcpy(&vStride2,   bi + 132, 4);
        memcpy(&ibInfoOff,  bi + 260, 4);
        memcpy(&i_num,      bi + 264, 4);

        uint32_t bufSkinOff = 0;
        if (hasSkinData && hdr.buffer_info_size >= 276u)
            memcpy(&bufSkinOff, bi + 272, 4);

        // Prefer vbuff_2 if present (nonzero and not sentinel 0xFFFFFFFF)
        uint32_t vbInfoOff = (vbInfoOff2 != 0 && vbInfoOff2 != 0xFFFFFFFFu) ? vbInfoOff2 : vbInfoOff0;
        uint32_t vStride   = (vbInfoOff2 != 0 && vbInfoOff2 != 0xFFFFFFFFu) ? vStride2   : vStride0;

        // Diagnostic: log raw BufferInfo values BEFORE any continues
        static int s_biLog = 0;
        if (s_biLog < 6)
        {
            FILE* vdbg = fopen("levelscene_vbdbg.txt",
                               s_biLog == 0 ? "w" : "a");
            if (vdbg)
            {
                fprintf(vdbg, "Slot[%d]: bufIdx=%u vbOff0=%u vbOff2=%u stride0=%u stride2=%u ibOff=%u i_num=%u\n",
                        s_biLog, bufIdx, vbInfoOff0, vbInfoOff2, vStride0, vStride2, ibInfoOff, i_num);
                fprintf(vdbg, "  chosen vbInfoOff=%u vStride=%u b1.size=%u valid=%d\n",
                        vbInfoOff, vStride, (unsigned)b1.size(),
                        (int)(vbInfoOff != 0 && (uint64_t)vbInfoOff + sizeof(VBuffInfo) <= b1.size()));
                fclose(vdbg);
            }
            ++s_biLog;
        }

        if (vbInfoOff == 0 || (uint64_t)vbInfoOff + sizeof(VBuffInfo) > b1.size()) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE bad vbInfoOff=%u b1=%u",
                                     slot, vbInfoOff, (unsigned)b1.size());
            continue;
        }
        if (ibInfoOff == 0 || (uint64_t)ibInfoOff + sizeof(IBuffInfo) > b1.size()) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE bad ibInfoOff=%u b1=%u",
                                     slot, ibInfoOff, (unsigned)b1.size());
            continue;
        }

        // ── VBuffInfo — direct byte pointer from BufferInfo ───────────────────
        // VBuffInfo layout (8 × u32 = 32 bytes):
        //   byte  0: unk_0
        //   byte  4: size   (total byte size of vertex data in BIN asset)
        //   byte  8: unk_3
        //   byte 12: offset (byte offset into BIN asset, NOT Block2)
        const VBuffInfo& vbi = *reinterpret_cast<const VBuffInfo*>(&b1[vbInfoOff]);

        // ── IBuffInfo — direct byte pointer from BufferInfo ───────────────────
        // IBuffInfo layout (6 × u32 = 24 bytes):
        //   byte  0: unk_0
        //   byte  4: size   (total byte size of index data in BIN asset)
        //   byte  8: format (0=U16)
        //   byte 12: vbuff_alt_fmt
        //   byte 16: offset (byte offset into BIN asset, NOT Block2)
        const IBuffInfo& ibi = *reinterpret_cast<const IBuffInfo*>(&b1[ibInfoOff]);

        // The bit-30 slot flag (0x40000000 set, 0x80000000 clear) is the definitive
        // collision-only marker — already checked above in the mesh_order loop.
        // Do NOT additionally filter by UV count: Blender renders all visual slots
        // regardless of UV presence (vertex-colored meshes have no UV channels).

        // ── Vertex data from BIN asset ────────────────────────────────────────
        uint32_t vOff  = vbi.offset;  // byte offset into binAsset
        uint32_t vSize = vbi.size;    // total byte size

        if (vStride < 12 || vSize == 0) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE bad vStride=%u vSize=%u",
                                     slot, vStride, vSize);
            continue;
        }
        if ((uint64_t)vOff + vSize > binAsset.size()) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE vbuff OVERFLOW vOff=%u vSize=%u binAsset=%u",
                                     slot, vOff, vSize, (unsigned)binAsset.size());
            continue;
        }

        int vertCount = (int)(vSize / vStride);
        if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] vOff=%u vSize=%u vStride=%u vertCount=%d binAsset=%u",
                                 slot, vOff, vSize, vStride, vertCount, (unsigned)binAsset.size());
        if (vertCount <= 0) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE vertCount<=0", slot);
            continue;
        }

        // ── Index data from BIN asset ─────────────────────────────────────────
        uint32_t iOff  = ibi.offset;  // byte offset into binAsset
        uint32_t iSize = ibi.size;
        // ibi.format encodes bits-per-index (16=U16, 32=U32), NOT a boolean flag.
        // Derive actual element size from iSize/i_num to be safe.
        uint32_t iElem = 2u;
        if (i_num > 0 && iSize > 0) iElem = (iSize / i_num >= 4u) ? 4u : 2u;

        // ── BE surgical swap: flip THIS vbuff (u32) and THIS ibuff (u16) ──────
        // Dedupe by offset so shared buffers are swapped exactly once.
        // u32 swap on vertex region = correct for float position/normal/UV/
        // tangent components. Packed u8 color/blend-weight channels survive
        // because a 4-byte packed-u8 quad byte-reversed is still the same
        // four bytes in different roles - we accept that minor color drift
        // on first pass rather than hand-parse every vertex.
        // u16 swap on index region = correct for 16-bit triangle indices.
        // The prior blanket u32-swap in GetBinAssetData pair-shuffled every
        // adjacent index pair, which turned every triangle into "here's a
        // vertex I never wanted to connect to, now go render it flat".
        if (binIsBigEndian) {
            if (swappedVbuffOffsets.find(vOff) == swappedVbuffOffsets.end()) {
                if ((uint64_t)vOff + vSize <= binAsset.size()) {
                    uint32_t n4 = vSize & ~(uint32_t)3;
                    if (n4 > 0) {
                        uint8_t* p = &binAsset[vOff];
                        for (uint32_t i = 0; i < n4; i += 4) {
                            uint8_t t;
                            t = p[i+0]; p[i+0] = p[i+3]; p[i+3] = t;
                            t = p[i+1]; p[i+1] = p[i+2]; p[i+2] = t;
                        }
                    }
                }
                swappedVbuffOffsets.insert(vOff);
            }
            if (swappedIbuffOffsets.find(iOff) == swappedIbuffOffsets.end()) {
                if ((uint64_t)iOff + iSize <= binAsset.size()) {
                    if (iElem == 2u) {
                        uint32_t n2 = iSize & ~(uint32_t)1;
                        if (n2 > 0) {
                            uint8_t* p = &binAsset[iOff];
                            for (uint32_t i = 0; i < n2; i += 2) {
                                uint8_t t = p[i]; p[i] = p[i+1]; p[i+1] = t;
                            }
                        }
                    } else { // iElem == 4u, rare u32 index buffers
                        uint32_t n4 = iSize & ~(uint32_t)3;
                        if (n4 > 0) {
                            uint8_t* p = &binAsset[iOff];
                            for (uint32_t i = 0; i < n4; i += 4) {
                                uint8_t t;
                                t = p[i+0]; p[i+0] = p[i+3]; p[i+3] = t;
                                t = p[i+1]; p[i+1] = p[i+2]; p[i+2] = t;
                            }
                        }
                    }
                }
                swappedIbuffOffsets.insert(iOff);
            }
        }
        bool is32bit   = (iElem == 4u);
        if (i_num == 0) i_num = iSize / iElem;
        if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] iOff=%u iSize=%u iElem=%u i_num=%u is32=%d",
                                 slot, iOff, iSize, iElem, i_num, is32bit ? 1 : 0);
        if (i_num < 3) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE i_num<3", slot);
            continue;
        }
        if ((uint64_t)iOff + iSize > binAsset.size()) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE ibuff OVERFLOW iOff=%u iSize=%u binAsset=%u",
                                     slot, iOff, iSize, (unsigned)binAsset.size());
            continue;
        }

        // ── Build LevelVertex array ───────────────────────────────────────────
        // LOTRC static-mesh vertex layout decoded from VBuffInfo.fmt1:
        //   byte  0-11: Position      (3 x float32)        fmt1 & 0x001
        //   byte 12+  : BlendWeight   (Unorm4x8, 4 bytes)  fmt1 & 0x400  (skinned meshes)
        //   byte   +  : BlendIndices  (Unorm4x8, 4 bytes)  fmt1 & 0x800  (skinned meshes)
        //   byte   +  : Normal        (Unorm4x8, 4 bytes)  fmt1 & 0x002
        //   byte   +  : Color(0)      (Unorm4x8, 4 bytes)  fmt1 & 0x100
        //   byte   +  : Color(1)      (Unorm4x8, 4 bytes)  fmt1 & 0x200
        //   byte   +  : UV0           (2 x float32)        (fmt1>>2)&0xF >= 1
        //   byte   +  : Tangent       (Unorm4x8, 4 bytes)  fmt1 & 0x040  (after UV)
        uint32_t normalByteOff = 12u;
        if ((vbi.fmt1 & 0x400u) != 0u) normalByteOff += 4u;  // BlendWeight before normal
        if ((vbi.fmt1 & 0x800u) != 0u) normalByteOff += 4u;  // BlendIndices before normal
        // Color offset: after normal (if present)
        // Binary order: ... Normal → Color(0) → Color(1) → UV ...
        // Game VS uses dcl_color1 (Color INDEX 1) for AO — most level geometry
        // has Color(1) (fmt1 & 0x200) but NOT Color(0) (fmt1 & 0x100).
        uint32_t color0ByteOff = normalByteOff;
        if ((vbi.fmt1 & 0x002u) != 0u) color0ByteOff += 4u;  // Normal before color
        bool hasColor0 = (vbi.fmt1 & 0x100u) != 0u && (vStride >= color0ByteOff + 4u);
        uint32_t color1ByteOff = color0ByteOff;
        if (hasColor0) color1ByteOff += 4u;  // Color(1) comes after Color(0)
        bool hasColor1 = (vbi.fmt1 & 0x200u) != 0u && (vStride >= color1ByteOff + 4u);
        uint32_t uvByteOff = normalByteOff;
        if ((vbi.fmt1 & 0x002u) != 0u) uvByteOff += 4u;  // Normal present
        if ((vbi.fmt1 & 0x100u) != 0u) uvByteOff += 4u;  // Color(0) present
        if ((vbi.fmt1 & 0x200u) != 0u) uvByteOff += 4u;  // Color(1) present
        bool      hasUV  = (((vbi.fmt1 >> 2) & 0xFu) >= 1u) && (vStride >= uvByteOff + 8u);

        uint32_t blendIdxByteOff = 12u;
        if ((vbi.fmt1 & 0x400u) != 0u) blendIdxByteOff += 4u;
        bool hasBlendIdx = (vbi.fmt1 & 0x800u) != 0u && hasSkinData
                        && (blendIdxByteOff + 4u <= vStride);

        const uint8_t* vData = &binAsset[vOff];
        std::vector<LevelVertex> verts(vertCount);
        for (int v = 0; v < vertCount; ++v)
        {
            const uint8_t* vp = vData + (size_t)v * vStride;
            LevelVertex& lv = verts[v];

            memcpy(&lv.x, vp + 0, 4);
            memcpy(&lv.y, vp + 4, 4);
            memcpy(&lv.z, vp + 8, 4);

            // Apply rest-pose skin offset for breakable/skinned meshes
            if (hasSkinData)
            {
                uint32_t blendIdx = bufSkinOff;
                if (hasBlendIdx)
                {
                    uint8_t bw4[4] = {255, 0, 0, 0};
                    uint8_t bi4[4];
                    if ((vbi.fmt1 & 0x400u) != 0u)
                        memcpy(bw4, vp + 12, 4);
                    memcpy(bi4, vp + blendIdxByteOff, 4);
                    int maxW = -1, maxK = 0;
                    for (int k = 0; k < 4; ++k) {
                        if ((int)bw4[k] > maxW) { maxW = (int)bw4[k]; maxK = k; }
                    }
                    blendIdx = (uint32_t)bi4[maxK] + bufSkinOff;
                }
                if (blendIdx < mdl.skin_binds_num) {
                    const float* sm = &restSkinMats[blendIdx * 16];
                    float ox = lv.x, oy = lv.y, oz = lv.z;
                    lv.x = ox*sm[0] + oy*sm[4] + oz*sm[8]  + sm[12];
                    lv.y = ox*sm[1] + oy*sm[5] + oz*sm[9]  + sm[13];
                    lv.z = ox*sm[2] + oy*sm[6] + oz*sm[10] + sm[14];
                }
            }

            // Expand scene and model bounding boxes
            if (!m_boundsValid)
            {
                m_boundsMin[0] = m_boundsMax[0] = lv.x;
                m_boundsMin[1] = m_boundsMax[1] = lv.y;
                m_boundsMin[2] = m_boundsMax[2] = lv.z;
                m_boundsValid = true;
            }
            else
            {
                if (lv.x < m_boundsMin[0]) m_boundsMin[0] = lv.x;
                if (lv.y < m_boundsMin[1]) m_boundsMin[1] = lv.y;
                if (lv.z < m_boundsMin[2]) m_boundsMin[2] = lv.z;
                if (lv.x > m_boundsMax[0]) m_boundsMax[0] = lv.x;
                if (lv.y > m_boundsMax[1]) m_boundsMax[1] = lv.y;
                if (lv.z > m_boundsMax[2]) m_boundsMax[2] = lv.z;
            }
            // Per-model local bounds
            if (!out->hasBounds) {
                out->localMin[0]=out->localMax[0]=lv.x;
                out->localMin[1]=out->localMax[1]=lv.y;
                out->localMin[2]=out->localMax[2]=lv.z;
                out->hasBounds = true;
            } else {
                if (lv.x<out->localMin[0]) out->localMin[0]=lv.x;
                if (lv.y<out->localMin[1]) out->localMin[1]=lv.y;
                if (lv.z<out->localMin[2]) out->localMin[2]=lv.z;
                if (lv.x>out->localMax[0]) out->localMax[0]=lv.x;
                if (lv.y>out->localMax[1]) out->localMax[1]=lv.y;
                if (lv.z>out->localMax[2]) out->localMax[2]=lv.z;
            }

            if ((vbi.fmt1 & 0x002u) && vStride >= normalByteOff + 4u)
            {
                uint32_t pk;
                memcpy(&pk, vp + normalByteOff, 4);
                UnpackNormal(pk, lv.nx, lv.ny, lv.nz);
            }
            else { lv.nx = 0.0f; lv.ny = 1.0f; lv.nz = 0.0f; }

            // Vertex color / ambient occlusion
            // Game uses Color(1) (dcl_color1) for AO on most level geometry.
            // Prefer Color(1) if present, fall back to Color(0), then white.
            if (hasColor1)
                memcpy(&lv.color, vp + color1ByteOff, 4);
            else if (hasColor0)
                memcpy(&lv.color, vp + color0ByteOff, 4);
            else
                lv.color = 0xFFFFFFFF;  // white = no AO

            if (hasUV)
            {
                memcpy(&lv.u, vp + uvByteOff,     4);
                memcpy(&lv.v, vp + uvByteOff + 4, 4);
            }
            else
            {
                // No UV channels in vertex format: generate world-space XZ tiling UV.
                // Terrain and similar meshes use position-based texture projection.
                // Blender mat scale = 0.004 → tile every 250 world units.
                lv.u = lv.x * 0.004f;
                lv.v = lv.z * 0.004f;
            }

            // Multiple UV sets: (fmt1>>2)&0xF gives the UV set count (1-6)
            // We only use UV0, but need to skip UV1+ to find tangent/texcoord5
            uint32_t numUVSets = hasUV ? ((vbi.fmt1 >> 2) & 0xFu) : 0u;
            if (numUVSets > 6) numUVSets = 1; // sanity

            // Tangent offset: after ALL UV sets (not just UV0!)
            uint32_t tangentByteOff = uvByteOff + numUVSets * 8u;
            if ((vbi.fmt1 & 0x040u) && tangentByteOff + 4u <= vStride) {
                uint32_t pk;
                memcpy(&pk, vp + tangentByteOff, 4);
                // Same unpacking as normal: signed byte4 → float3 + sign
                lv.tx = ((float)((int8_t)(pk & 0xFF))) / 127.0f;
                lv.ty = ((float)((int8_t)((pk >> 8) & 0xFF))) / 127.0f;
                lv.tz = ((float)((int8_t)((pk >> 16) & 0xFF))) / 127.0f;
                lv.tw = ((float)((int8_t)((pk >> 24) & 0xFF))) / 127.0f;
                if (lv.tw == 0.0f) lv.tw = 1.0f; // default handedness
            } else {
                // No tangent data — generate from normal
                float ax = fabsf(lv.nx), ay = fabsf(lv.ny), az = fabsf(lv.nz);
                if (ay < ax && ay < az) {
                    lv.tx = lv.nz; lv.ty = 0; lv.tz = -lv.nx;
                } else {
                    lv.tx = 0; lv.ty = -lv.nz; lv.tz = lv.ny;
                }
                float tlen = sqrtf(lv.tx*lv.tx + lv.ty*lv.ty + lv.tz*lv.tz);
                if (tlen > 0.001f) { lv.tx /= tlen; lv.ty /= tlen; lv.tz /= tlen; }
                else { lv.tx = 1; lv.ty = 0; lv.tz = 0; }
                lv.tw = 1.0f;
            }

            // Static lighting (game texcoord5): packed RGBA after tangent
            // Game VS: mul r2.xyz, v6.w, v6; mul r2.xyz, r2, 31.875
            // → staticLighting = v6.rgb * v6.a * 31.875 (HDR baked GI)
            uint32_t slByteOff = tangentByteOff + ((vbi.fmt1 & 0x040u) ? 4u : 0u);
            if (slByteOff + 4u <= vStride) {
                // There are extra bytes after tangent — likely static lighting
                uint32_t slPk;
                memcpy(&slPk, vp + slByteOff, 4);
                float r = (float)((slPk >>  0) & 0xFF) / 255.0f;
                float g = (float)((slPk >>  8) & 0xFF) / 255.0f;
                float b = (float)((slPk >> 16) & 0xFF) / 255.0f;
                float a = (float)((slPk >> 24) & 0xFF) / 255.0f;
                // Apply game's HDR decode: SL = rgb * a * 31.875
                float scale = a * 31.875f;
                lv.slr = r * scale;
                lv.slg = g * scale;
                lv.slb = b * scale;
                lv.slw = (scale > 0.001f) ? 1.0f : 0.0f; // w=1 if SL is active
            } else {
                lv.slr = lv.slg = lv.slb = 0.0f;
                lv.slw = 0.0f;
            }
        }

        // Log unique fmt1/stride combinations to find static lighting
        {
            static std::set<uint32_t> s_loggedFmt;
            if (s_loggedFmt.find(vbi.fmt1) == s_loggedFmt.end() && vertCount > 0) {
                s_loggedFmt.insert(vbi.fmt1);
                int slActive = 0;
                float maxSL = 0;
                for (int vi = 0; vi < vertCount; ++vi) {
                    if (verts[vi].slw > 0) slActive++;
                    float m = verts[vi].slr > verts[vi].slg ? verts[vi].slr : verts[vi].slg;
                    if (verts[vi].slb > m) m = verts[vi].slb;
                    if (m > maxSL) maxSL = m;
                }
                uint32_t nUV = hasUV ? ((vbi.fmt1 >> 2) & 0xFu) : 0u;
                uint32_t tOff = uvByteOff + nUV * 8u;
                uint32_t slOff = tOff + ((vbi.fmt1 & 0x040u) ? 4u : 0u);
                LevelLogf("[Vtx] fmt1=0x%03X stride=%d uvSets=%d tangOff=%d slOff=%d extraBytes=%d SL=%d/%d maxSL=%.2f",
                    vbi.fmt1, vStride, nUV, tOff, slOff, (int)vStride-(int)slOff-4, slActive, vertCount, maxSL);
            }
        }

        // ── D3D9 Vertex Buffer ────────────────────────────────────────────────
        IDirect3DVertexBuffer9* vb = NULL;
        if (FAILED(m_device->CreateVertexBuffer(
                vertCount * LEVEL_VERTEX_SIZE, D3DUSAGE_WRITEONLY,
                0, D3DPOOL_MANAGED, &vb, NULL)) || !vb)  // FVF=0, using vertex declaration
        {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE CreateVertexBuffer FAILED bytes=%d",
                                     slot, vertCount * LEVEL_VERTEX_SIZE);
            continue;
        }

        LevelVertex* vbPtr = NULL;
        if (FAILED(vb->Lock(0, 0, (void**)&vbPtr, 0))) {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE vb->Lock FAILED", slot);
            vb->Release();
            continue;
        }
        memcpy(vbPtr, &verts[0], vertCount * LEVEL_VERTEX_SIZE);
        vb->Unlock();

        // ── D3D9 Index Buffer (always U16) ────────────────────────────────────
        const uint8_t* iData = &binAsset[iOff];
        std::vector<uint16_t> indices(i_num);
        if (is32bit)
        {
            for (uint32_t k = 0; k < i_num && (k + 1u) * 4u <= iSize; ++k)
            {
                uint32_t idx32;
                memcpy(&idx32, iData + k * 4u, 4);
                indices[k] = (uint16_t)(idx32 & 0xFFFFu);
            }
        }
        else
        {
            uint32_t copySz = i_num * 2u;
            if (copySz > iSize) copySz = iSize;
            memcpy(&indices[0], iData, copySz);
        }

        IDirect3DIndexBuffer9* ib = NULL;
        if (FAILED(m_device->CreateIndexBuffer(
                i_num * sizeof(uint16_t), D3DUSAGE_WRITEONLY,
                D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL)) || !ib)
        {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE CreateIndexBuffer FAILED bytes=%u",
                                     slot, i_num * (unsigned)sizeof(uint16_t));
            vb->Release();
            continue;
        }

        uint16_t* ibPtr = NULL;
        if (FAILED(ib->Lock(0, 0, (void**)&ibPtr, 0)))
        {
            if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] CONTINUE ib->Lock FAILED", slot);
            vb->Release(); ib->Release();
            continue;
        }
        memcpy(ibPtr, &indices[0], i_num * sizeof(uint16_t));
        ib->Unlock();

        // ── Material lookup ───────────────────────────────────────────────────
        // mdl.mat_offset -> array of u32 byte offsets into Block1 (one per buffer).
        // Each points to a MatBase struct (PC layout):
        //   +0   unk_0, +4 unk_1(PC), +8 tex0(CRC)
        //   +200 flags(u64) — bit62 = transparent/additive
        //   +218 side_flags(u16) — 0x100 = two-sided
        IDirect3DTexture9* tex = NULL;
        IDirect3DTexture9* normalTex = NULL;
        IDirect3DTexture9* specTex = NULL;
        IDirect3DTexture9* detailTex = NULL;
        IDirect3DTexture9* emissiveTex = NULL;
        IDirect3DTexture9* terrainL2 = NULL;
        IDirect3DTexture9* terrainL3 = NULL;
        IDirect3DTexture9* terrainM0 = NULL;
        IDirect3DTexture9* terrainM1 = NULL;
        IDirect3DTexture9* terrainM2 = NULL;
        uint32_t partTexCrc = 0;
        uint64_t matFlags64 = 0;
        uint16_t matSideFlags = 0;
        float matColorOut[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (mdl.mat_offset != 0 && mdl.mat_num > 0)
        {
            uint32_t matOrderIdx = (bufIdx < mdl.mat_num) ? bufIdx : 0u;
            uint32_t matPtrOff = mdl.mat_offset + matOrderIdx * 4u;
            if ((uint64_t)matPtrOff + 4u <= b1.size())
            {
                uint32_t matStructOff;
                memcpy(&matStructOff, &b1[matPtrOff], 4);
                if ((uint64_t)matStructOff + 12u <= b1.size())
                {
                    // ── Name-based texture classification ──
                    // Read all 6 CRCs, resolve names, classify by suffix.
                    // This prevents Mat4 (terrain) diffuse textures from being
                    // loaded as normal maps (which causes yellow blowout).
                    uint32_t texCrcs[6];
                    memset(texCrcs, 0, sizeof(texCrcs));
                    int maxSlots = ((uint64_t)matStructOff + 32u <= b1.size()) ? 6 :
                                   ((uint64_t)matStructOff + 28u <= b1.size()) ? 5 :
                                   ((uint64_t)matStructOff + 12u <= b1.size()) ? 1 : 0;
                    for (int ti = 0; ti < maxSlots; ++ti)
                        memcpy(&texCrcs[ti], &b1[matStructOff + 8 + ti*4], 4);

                    // tex0 is ALWAYS diffuse
                    partTexCrc = texCrcs[0];
                    if (texCrcs[0] != 0)
                        tex = getOrLoadTexture(reader, texCrcs[0]);

                    // Classify tex1-tex5 by resolved name suffix
                    for (int ti = 1; ti < 6; ++ti) {
                        if (texCrcs[ti] == 0) continue;
                        if (texCrcs[ti] == texCrcs[0]) continue; // skip if same as diffuse
                        std::string nm = reader.ResolveCrc(texCrcs[ti]);

                        // Unresolved hex string — cannot classify, skip entirely
                        if (nm.size() >= 2 && nm[0] == '0' && nm[1] == 'x') continue;

                        // Classify by name suffix/pattern
                        size_t len = nm.size();
                        char lastTwo[3] = {0};
                        if (len >= 2) { lastTwo[0] = nm[len-2]; lastTwo[1] = nm[len-1]; }

                        bool isNormal = (lastTwo[0] == '_' && (lastTwo[1] == 'N' || lastTwo[1] == 'n'));
                        if (nm.find("Normal") != std::string::npos) isNormal = true;
                        if (nm.find("EnvMap") != std::string::npos) isNormal = true;

                        bool isSpec = (lastTwo[0] == '_' && (lastTwo[1] == 'S' || lastTwo[1] == 's'));

                        bool isDetail = (nm.find("_GV") != std::string::npos);

                        // Assign first match of each role
                        if (isNormal && !normalTex)
                            normalTex = getOrLoadTexture(reader, texCrcs[ti]);
                        else if (isSpec && !specTex)
                            specTex = getOrLoadTexture(reader, texCrcs[ti]);
                        else if (isDetail && !detailTex)
                            detailTex = getOrLoadTexture(reader, texCrcs[ti]);
                        // Don't load unknown-role textures as anything
                    }

                    // MatBase.flags (u64 at +200) and side_flags (u16 at +218)
                    if ((uint64_t)matStructOff + 208u <= b1.size())
                        memcpy(&matFlags64, &b1[matStructOff + 200], 8);
                    if ((uint64_t)matStructOff + 220u <= b1.size())
                        memcpy(&matSideFlags, &b1[matStructOff + 218], 2);

                    // Terrain multi-layer (kind=1, Mat4): load layers 2-3 + blend masks
                    uint16_t matKind = 0;
                    if ((uint64_t)matStructOff + 210u <= b1.size())
                        memcpy(&matKind, &b1[matStructOff + 208], 2); // kind at flags+8 = +208
                    // NOTE: Mat4 (terrain) has a different binary layout — terrain
                    // multi-layer loading deferred until Mat4 struct is decoded.

                    // ── Material color (mtlColor → game VS c197) ──
                    // TODO: Need to reverse-engineer the exact MatBase offset for
                    // the per-material brightness. Scanning caused false positives.
                    // Default to white until the correct offset is known.
                    matColorOut[0] = matColorOut[1] = matColorOut[2] = 1.0f;
                    matColorOut[3] = 1.0f;
                }
            }
        }

        if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] PASSED all checks. Adding part: vertCount=%d i_num=%u tex=%p",
                                 slot, vertCount, i_num, (void*)tex);

        // ── Commit mesh part ──────────────────────────────────────────────────
        LevelMeshPart part;
        part.vb          = vb;
        part.ib          = ib;
        part.vertexCount = vertCount;
        part.indexCount  = (int)i_num;
        part.diffuseTex  = tex;
        part.normalTex   = normalTex;
        part.specularTex = specTex;
        part.detailTex   = detailTex;
        part.emissiveTex = emissiveTex;
        part.terrainLayer2 = terrainL2;
        part.terrainLayer3 = terrainL3;
        part.terrainMask0  = terrainM0;
        part.terrainMask1  = terrainM1;
        part.terrainMask2  = terrainM2;
        part.texCrc      = partTexCrc;
        memcpy(part.matColor, matColorOut, sizeof(float)*4);

        // CPU position mirror for the embedded-weapon editor. The VB itself
        // is D3DUSAGE_WRITEONLY so we can't reliably read positions back
        // from it for the "original baseline" snapshot the bone-local
        // gizmo needs. Mirror lives only on parts that might host embedded
        // weapons; in practice every part gets one (~12 bytes × vertCount,
        // ~40 KB for a typical CRD). Cheap insurance.
        part.positionsCpu.resize((size_t)vertCount * 3);
        for (int vi = 0; vi < vertCount; ++vi) {
            part.positionsCpu[vi*3 + 0] = verts[vi].x;
            part.positionsCpu[vi*3 + 1] = verts[vi].y;
            part.positionsCpu[vi*3 + 2] = verts[vi].z;
        }

        // CPU mirror of BlendIndices + BlendWeight per slot[0] vert. Lets
        // the bone-weight selection mode ask "which verts are weighted
        // to Bone_X" without re-parsing the BIN. Walks the same vbi.fmt1
        // layout the LevelVertex decode loop above uses. Skipped (left
        // empty) when the part has no skin streams — terrain, props, etc.
        if (hasBlendIdx) {
            part.blendIndicesCpu.resize((size_t)vertCount);
            part.blendWeightsCpu.resize((size_t)vertCount);
            const uint8_t* vDataB = &binAsset[vOff];
            for (int vi = 0; vi < vertCount; ++vi) {
                const uint8_t* vp = vDataB + (size_t)vi * vStride;
                uint32_t bi32 = 0, bw32 = 0;
                memcpy(&bi32, vp + blendIdxByteOff, 4);
                if ((vbi.fmt1 & 0x400u) != 0u) memcpy(&bw32, vp + 12, 4);
                else                            bw32 = 0x000000FFu;  // assume full primary weight in byte 0
                part.blendIndicesCpu[vi] = bi32;
                part.blendWeightsCpu[vi] = bw32;
            }
        }

        // Index buffer CPU mirror — needed by the triangle picker for
        // ray-vs-triangle tests. The GPU IB is WRITEONLY so we copy from
        // the already-decoded `indices` vector instead of locking it back.
        part.indicesCpu.assign(indices.begin(), indices.end());

        out->parts.push_back(part);

        // Store flags in static map (no header struct changes)
        uint32_t pf = 0;
        if (matFlags64 & ((uint64_t)1 << 62)) pf |= 1;  // transparent
        if (matSideFlags & 0x100)              pf |= 2;  // two-sided
        // Skip occluder models — invisible collision/occlusion volumes
        if (modelName.find("ccluder") != std::string::npos ||
            modelName.find("Occluder") != std::string::npos) {
            pf |= 16; // bit4 = hidden (skip rendering)
        }
        // Detect water/waterfall models by name prefix
        if (modelName.size() >= 3 && (modelName.substr(0,3) == "WT_" ||
            modelName.find("Water") != std::string::npos ||
            modelName.find("water") != std::string::npos)) {
            pf |= 4;  // water
            pf |= 1;  // also mark transparent for alpha blend
        }
        // Detect lava models — self-lit emissive + UV scroll
        if (modelName.find("Lava") != std::string::npos ||
            modelName.find("lava") != std::string::npos) {
            pf |= 4;  // use water UV scroll
            pf |= 8;  // lava flag — fully emissive, no Lambert
        }
        if (pf) g_partFlags[std::make_pair((const void*)out, (int)out->parts.size()-1)] = pf;
        if (_dbgEmbed) LevelLogf("[BowDbg] slot[%d] PART ADDED verts=%d tris=%u",
                                 slot, vertCount, i_num / 3);
    }

    if (_dbgEmbed)
        LevelLogf("[BowDbg] loadModelFromBinary END name=%s parts=%d",
                  modelName.c_str(), (int)out->parts.size());

    if (out->parts.empty())
    {
        if (sf) fprintf(sf, "SKIP NO_VISUAL_PARTS crc=0x%08X name=%s lod0=[%d..%d]\n",
                        modelCrc, modelName.c_str(), lod0Start, lod0End);
    }
    return !out->parts.empty();
}

// ── load() ────────────────────────────────────────────────────────────────────

bool LevelScene::load(const ZeroEngine::LevelReader& reader)
{
    if (!m_device) return false;
    unload();

    m_levelName = reader.GetLevelName();

    const std::vector<ZeroEngine::LevelGameObjEntry>& objs = reader.GetGameObjs();
    int loaded = 0, skipped = 0;

    // Debug log — written once per load attempt
    FILE* dbg = fopen("levelscene_debug.txt", "w");
    if (dbg)
    {
        fprintf(dbg, "LevelScene::load  level=%s  objs=%d\n",
                m_levelName.c_str(), (int)objs.size());
        const ZeroEngine::PakHeader& hdr = reader.GetPakHeader();
        fprintf(dbg, "PakHeader: model_info_num=%u model_info_offset=%u\n",
                hdr.model_info_num, hdr.model_info_offset);
        fprintf(dbg, "PakHeader: buffer_info_size=%u vbuff_info_size=%u ibuff_info_size=%u\n",
                hdr.buffer_info_size, hdr.vbuff_info_size, hdr.ibuff_info_size);
        fprintf(dbg, "Block1.size=%u  Block2.size=%u\n",
                (unsigned)reader.GetBlock1().size(), (unsigned)reader.GetBlock2().size());
        // Probe first model with a mesh CRC
        for (size_t i = 0; i < objs.size(); ++i)
        {
            const ZeroEngine::LevelGameObjEntry& o = objs[i];
            if (!o.has_transform || o.mesh_crc == 0) continue;
            fprintf(dbg, "First mesh obj: mesh_crc=0x%08X mesh='%s'\n",
                    o.mesh_crc, o.mesh_name.c_str());
            // Scan ModelInfo for this CRC
            if (hdr.model_info_num && hdr.model_info_offset + sizeof(ZeroEngine::ModelInfo)*hdr.model_info_num <= reader.GetBlock1().size())
            {
                const ZeroEngine::ModelInfo* mods = reinterpret_cast<const ZeroEngine::ModelInfo*>(&reader.GetBlock1()[hdr.model_info_offset]);
                for (uint32_t m = 0; m < hdr.model_info_num; ++m)
                {
                    if (mods[m].key == o.mesh_crc)
                    {
                        const ZeroEngine::ModelInfo& mdl = mods[m];
                        fprintf(dbg, "  Found ModelInfo[%u]: key=0x%08X lod0=(%u..%u) mesh_order_off=%u buf_info_off=%u\n",
                                m, mdl.key, mdl.lod0.start, mdl.lod0.static_end,
                                mdl.mesh_order_offset, mdl.buffer_info_offset);
                        fprintf(dbg, "  vbuff_off=%u vbuff_num=%u ibuff_off=%u ibuff_num=%u mat_off=%u mat_num=%u\n",
                                mdl.vbuff_offset, mdl.vbuff_num, mdl.ibuff_offset, mdl.ibuff_num,
                                mdl.mat_offset, mdl.mat_num);
                        // Read first BufferInfo entry
                        if (hdr.buffer_info_size >= 268 && mdl.buffer_info_offset != 0 &&
                            mdl.buffer_info_offset + hdr.buffer_info_size <= reader.GetBlock1().size())
                        {
                            const uint8_t* bi = &reader.GetBlock1()[mdl.buffer_info_offset];
                            uint32_t vbOff0, vbOff2, vStride0, vStride2, ibIdx, i_num;
                            memcpy(&vbOff0,   bi +   0, 4);
                            memcpy(&vbOff2,   bi +   4, 4);
                            memcpy(&vStride0, bi + 128, 4);
                            memcpy(&vStride2, bi + 132, 4);
                            memcpy(&ibIdx,    bi + 260, 4);
                            memcpy(&i_num,    bi + 264, 4);
                            fprintf(dbg, "  BufferInfo[0]: vbOff0=%u vbOff2=%u vStride0=%u vStride2=%u ibIdx=%u i_num=%u\n",
                                    vbOff0, vbOff2, vStride0, vStride2, ibIdx, i_num);
                            // Log VBuffInfo and first vertex from BIN asset
                            fprintf(dbg, "  mdl.asset_key=0x%08X\n", mdl.asset_key);
                            const std::vector<uint8_t>& b1d = reader.GetBlock1();
                            uint32_t vbPtr = vbOff0;
                            if (vbPtr > 0 && (uint64_t)vbPtr + 32 <= b1d.size())
                            {
                                uint32_t vb_size, vb_offset, vb_fmt1;
                                memcpy(&vb_size,   &b1d[vbPtr+ 4], 4);
                                memcpy(&vb_offset, &b1d[vbPtr+12], 4);
                                memcpy(&vb_fmt1,   &b1d[vbPtr+16], 4);
                                fprintf(dbg, "  VBuffInfo: size=%u binAssetOffset=%u fmt1=0x%08X\n",
                                        vb_size, vb_offset, vb_fmt1);
                                std::vector<uint8_t> binDbg;
                                if (reader.GetBinAssetData(mdl.asset_key, binDbg)
                                    && (uint64_t)vb_offset + 12 <= binDbg.size())
                                {
                                    float fx, fy, fz;
                                    memcpy(&fx, &binDbg[vb_offset+ 0], 4);
                                    memcpy(&fy, &binDbg[vb_offset+ 4], 4);
                                    memcpy(&fz, &binDbg[vb_offset+ 8], 4);
                                    fprintf(dbg, "  v[0].xyz=(%.4f, %.4f, %.4f)\n", fx, fy, fz);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            break;
        }
        fprintf(dbg, "hdr.vbuff_info_offset=%u hdr.ibuff_info_offset=%u hdr.buffer_info_offset=%u\n",
                hdr.vbuff_info_offset, hdr.ibuff_info_offset, hdr.buffer_info_offset);
    }

    // Debug: log first 10 objects that have mesh_crc but no transform
    if (dbg)
    {
        int noXformCount = 0;
        for (size_t oi = 0; oi < objs.size() && noXformCount < 10; ++oi)
        {
            const ZeroEngine::LevelGameObjEntry& o = objs[oi];
            if (o.mesh_crc != 0 && !o.has_transform)
            {
                fprintf(dbg, "NoXform[%d] type=%s mesh=%s\n",
                        noXformCount, o.type_name.c_str(), o.mesh_name.c_str());
                ++noXformCount;
            }
        }
        // Also log first 5 objects that have both mesh+transform but different type
        int withXformCount = 0;
        for (size_t oi = 0; oi < objs.size() && withXformCount < 5; ++oi)
        {
            const ZeroEngine::LevelGameObjEntry& o = objs[oi];
            if (o.mesh_crc != 0 && o.has_transform)
            {
                fprintf(dbg, "WithXform[%d] type=%s mesh=%s pos=(%.1f,%.1f,%.1f)\n",
                        withXformCount, o.type_name.c_str(), o.mesh_name.c_str(),
                        o.world_transform[12], o.world_transform[13], o.world_transform[14]);
                ++withXformCount;
            }
        }
    }

    for (size_t oi = 0; oi < objs.size(); ++oi)
    {
        const ZeroEngine::LevelGameObjEntry& obj = objs[oi];

        // Skip objects without a world transform
        if (!obj.has_transform) continue;

        // Parse Collision entities into wireframe collision volumes
        if (obj.type_name == "Collision" || obj.type_name == "collision") {
            if (dbg) fprintf(dbg, "COLLISION[%s] spline_nodes=%d\n", obj.name.c_str(), (int)obj.spline_nodes.size());
            if (!obj.spline_nodes.empty()) {
                LevelCollisionVolume cv;
                cv.nodes.resize(obj.spline_nodes.size());
                for (size_t ni = 0; ni < obj.spline_nodes.size(); ++ni) {
                    cv.nodes[ni].x = obj.spline_nodes[ni].x;
                    cv.nodes[ni].y = obj.spline_nodes[ni].y;
                    cv.nodes[ni].z = obj.spline_nodes[ni].z;
                }
                cv.worldPos[0] = obj.world_transform[12];
                cv.worldPos[1] = obj.world_transform[13];
                cv.worldPos[2] = obj.world_transform[14];
                cv.guid = obj.guid;
                cv.name = obj.name;
                cv.gameModeMask = obj.gamemodemask;
                // Read Height and closed from parsed fields
                static const uint32_t CRC_HEIGHT = 0x86010476u;
                static const uint32_t CRC_CLOSED = 0xF1BE7E52u;
                cv.height = 10.0f; // default
                cv.closed = false;
                {
                    std::map<uint32_t, float>::const_iterator hIt = obj.float_fields.find(CRC_HEIGHT);
                    if (hIt != obj.float_fields.end()) cv.height = hIt->second;
                    std::map<uint32_t, uint32_t>::const_iterator cIt = obj.int_fields.find(CRC_CLOSED);
                    if (cIt != obj.int_fields.end()) cv.closed = (cIt->second != 0);
                }
                // Phase 6: Determine collision flag profile from collision fields
                {
                    static const uint32_t CRC_CC = 0x85CA06C7u; // CollideWithCharacters
                    static const uint32_t CRC_CP = 0x4247B76Du; // CollideWithPlayers
                    static const uint32_t CRC_CM = 0x7D873DE2u; // CollideWithMounts
                    static const uint32_t CRC_CS = 0xDDBC8FF6u; // CollideWithSiege
                    static const uint32_t CRC_CPR= 0xF5101794u; // CollideWithProjectiles
                    bool cc=false, cp=false, cm=false, cs=false, cpr=false;
                    std::map<uint32_t, uint32_t>::const_iterator fi;
                    fi = obj.int_fields.find(CRC_CC);  if (fi != obj.int_fields.end()) cc  = (fi->second != 0);
                    fi = obj.int_fields.find(CRC_CP);  if (fi != obj.int_fields.end()) cp  = (fi->second != 0);
                    fi = obj.int_fields.find(CRC_CM);  if (fi != obj.int_fields.end()) cm  = (fi->second != 0);
                    fi = obj.int_fields.find(CRC_CS);  if (fi != obj.int_fields.end()) cs  = (fi->second != 0);
                    fi = obj.int_fields.find(CRC_CPR); if (fi != obj.int_fields.end()) cpr = (fi->second != 0);
                    if (cc && cp && cm && cs && cpr) cv.collFlags = 0x02u;      // blockade (all)
                    else if (cp && !cc)              cv.collFlags = 0x04u;      // player-only
                    else if (cc && !cp)              cv.collFlags = 0x08u;      // AI-only
                    else if (cc && cp)               cv.collFlags = 0x01u;      // boundary
                    else                             cv.collFlags = 0x01u;      // default boundary
                }
                m_collisionVolumes.push_back(cv);
            }
            continue;
        }

        if (obj.mesh_crc == 0) {
            if (dbg && (obj.type_name == "Collision" || obj.type_name == "collision"))
                fprintf(dbg, "  -> collisionVolumes total: %d\n", (int)m_collisionVolumes.size());
            continue;
        }

        // Skip non-visual object types (roads, invisible game logic)
        {
            const std::string& t = obj.type_name;
            if (t == "Road" || t == "CPSpline" ||
                t == "child_object") continue;
        }

        uint32_t meshCrc = obj.mesh_crc;

        // Model cache lookup (by mesh CRC)
        LevelModel* model = NULL;
        std::map<uint32_t, LevelModel*>::iterator cit = m_modelCache.find(meshCrc);
        if (cit != m_modelCache.end())
        {
            model = cit->second;
        }
        else
        {
            LevelModel* nm = new LevelModel();
            nm->name = obj.mesh_name;
            nm->hasBounds = false;
            if (loadModelFromBinary(reader, meshCrc, obj.mesh_name, nm))
                model = nm;
            else
                delete nm;
            m_modelCache[meshCrc] = model;  // cache even NULL (avoid retrying)
        }

        if (!model) {
            ++skipped;
            if (dbg) fprintf(dbg, "SKIPPED_INST[%d] type=%s mesh=0x%08X name=%s\n",
                             skipped, obj.type_name.c_str(), meshCrc, obj.mesh_name.c_str());
            continue;
        }

        LevelInstance inst;
        inst.model = model;
        memcpy(inst.mat, obj.world_transform, sizeof(float) * 16);


        inst.objName      = obj.name;
        inst.typeName     = obj.type_name;
        inst.meshName     = obj.mesh_name;
        inst.guid         = obj.guid;
        inst.parentGuid   = obj.parent_guid;
        inst.gameModeMask = obj.gamemodemask;
        inst.block1WtOffset = obj.block1_wt_offset;
        inst.layerGuid    = obj.layer_guid; // Phase 3 XSI layer manager
        // Compute world-space AABB by transforming model's 8 local AABB corners
        if (model->hasBounds) {
            const float* lmin = model->localMin;
            const float* lmax = model->localMax;
            const float* m = inst.mat;
            inst.bboxMin[0]=inst.bboxMin[1]=inst.bboxMin[2]= 1e30f;
            inst.bboxMax[0]=inst.bboxMax[1]=inst.bboxMax[2]=-1e30f;
            for (int ci = 0; ci < 8; ++ci) {
                float cx = (ci & 1) ? lmax[0] : lmin[0];
                float cy = (ci & 2) ? lmax[1] : lmin[1];
                float cz = (ci & 4) ? lmax[2] : lmin[2];
                // row-vector * matrix: wx = cx*m[0]+cy*m[4]+cz*m[8]+m[12]
                float wx = cx*m[0]+cy*m[4]+cz*m[8]+m[12];
                float wy = cx*m[1]+cy*m[5]+cz*m[9]+m[13];
                float wz = cx*m[2]+cy*m[6]+cz*m[10]+m[14];
                if (wx<inst.bboxMin[0]) inst.bboxMin[0]=wx;
                if (wy<inst.bboxMin[1]) inst.bboxMin[1]=wy;
                if (wz<inst.bboxMin[2]) inst.bboxMin[2]=wz;
                if (wx>inst.bboxMax[0]) inst.bboxMax[0]=wx;
                if (wy>inst.bboxMax[1]) inst.bboxMax[1]=wy;
                if (wz>inst.bboxMax[2]) inst.bboxMax[2]=wz;
            }
        } else {
            float cx = inst.mat[12], cy = inst.mat[13], cz = inst.mat[14];
            float r = 15.0f;
            inst.bboxMin[0]=cx-r; inst.bboxMin[1]=cy-r; inst.bboxMin[2]=cz-r;
            inst.bboxMax[0]=cx+r; inst.bboxMax[1]=cy+r; inst.bboxMax[2]=cz+r;
        }
        m_instances.push_back(inst);

        // Log first 5 instance transforms for debugging
        if (dbg && loaded < 5)
        {
            const float* m = obj.world_transform;
            fprintf(dbg, "Inst[%d] mesh=%s\n", loaded, obj.mesh_name.c_str());
            fprintf(dbg, "  [%7.2f %7.2f %7.2f %7.2f]\n", m[0],  m[1],  m[2],  m[3]);
            fprintf(dbg, "  [%7.2f %7.2f %7.2f %7.2f]\n", m[4],  m[5],  m[6],  m[7]);
            fprintf(dbg, "  [%7.2f %7.2f %7.2f %7.2f]\n", m[8],  m[9],  m[10], m[11]);
            fprintf(dbg, "  [%7.2f %7.2f %7.2f %7.2f]  <-- row3/translation\n", m[12], m[13], m[14], m[15]);
        }

        // Log first 15 NON-IDENTITY instances (with actual position/rotation)
        {
            static int s_nonIdentInst = 0;
            if (dbg && s_nonIdentInst < 15) {
                const float* m = obj.world_transform;
                bool isIdent = (m[12]*m[12] + m[13]*m[13] + m[14]*m[14] < 0.01f) &&
                               (m[0] > 0.99f && m[5] > 0.99f && m[10] > 0.99f);
                if (!isIdent) {
                    fprintf(dbg, "NonIdentInst[%d] mesh=%s type=%s\n",
                            s_nonIdentInst, obj.mesh_name.c_str(), obj.type_name.c_str());
                    fprintf(dbg, "  [%10.4f %10.4f %10.4f %10.4f]\n", m[0],  m[1],  m[2],  m[3]);
                    fprintf(dbg, "  [%10.4f %10.4f %10.4f %10.4f]\n", m[4],  m[5],  m[6],  m[7]);
                    fprintf(dbg, "  [%10.4f %10.4f %10.4f %10.4f]\n", m[8],  m[9],  m[10], m[11]);
                    fprintf(dbg, "  [%10.4f %10.4f %10.4f %10.4f]  <-- pos\n", m[12], m[13], m[14], m[15]);
                    ++s_nonIdentInst;
                }
            }
        }

        ++loaded;
    }

    // ── Diagnostic: compare vertex bounds vs ModelInfo bounding box ──
    if (dbg)
    {
        const ZeroEngine::PakHeader& hdr2 = reader.GetPakHeader();
        const std::vector<uint8_t>& b1d = reader.GetBlock1();
        const ZeroEngine::ModelInfo* mdls2 = (hdr2.model_info_num > 0 && hdr2.model_info_offset > 0)
            ? reinterpret_cast<const ZeroEngine::ModelInfo*>(&b1d[hdr2.model_info_offset]) : NULL;

        fprintf(dbg, "\n=== MODEL BOUNDS DIAGNOSTIC ===\n");
        int diagCount = 0;
        for (std::map<uint32_t, LevelModel*>::iterator it = m_modelCache.begin();
             it != m_modelCache.end() && diagCount < 20; ++it)
        {
            LevelModel* mdl = it->second;
            if (!mdl || !mdl->hasBounds) continue;

            // Find ModelInfo by CRC
            int miIdx = -1;
            if (mdls2) {
                for (uint32_t i = 0; i < hdr2.model_info_num; ++i)
                    if (mdls2[i].key == it->first) { miIdx = (int)i; break; }
            }

            float vtxCtr[3] = {
                (mdl->localMin[0]+mdl->localMax[0])*0.5f,
                (mdl->localMin[1]+mdl->localMax[1])*0.5f,
                (mdl->localMin[2]+mdl->localMax[2])*0.5f
            };

            fprintf(dbg, "Model 0x%08X '%s'\n", it->first, mdl->name.c_str());
            fprintf(dbg, "  VertexBounds min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)\n",
                    mdl->localMin[0], mdl->localMin[1], mdl->localMin[2],
                    mdl->localMax[0], mdl->localMax[1], mdl->localMax[2]);
            fprintf(dbg, "  VertexCenter=(%.3f,%.3f,%.3f)\n", vtxCtr[0], vtxCtr[1], vtxCtr[2]);

            if (miIdx >= 0) {
                const ZeroEngine::PakBoundingBox& bb = mdls2[miIdx].bounding_box;
                fprintf(dbg, "  ModelInfo.bbox center=(%.3f,%.3f,%.3f) half=(%.3f,%.3f,%.3f)\n",
                        bb.center[0], bb.center[1], bb.center[2],
                        bb.half_width[0], bb.half_width[1], bb.half_width[2]);
                float dx = vtxCtr[0]-bb.center[0], dy = vtxCtr[1]-bb.center[1], dz = vtxCtr[2]-bb.center[2];
                fprintf(dbg, "  DELTA (vtx-bbox): (%.3f, %.3f, %.3f)\n", dx, dy, dz);
            }
            ++diagCount;
        }

        // Dump first 10 instances with their world-space centers
        fprintf(dbg, "\n=== INSTANCE WORLD POSITIONS ===\n");
        int instDiag = 0;
        for (int ii = 0; ii < (int)m_instances.size() && instDiag < 10; ++ii)
        {
            const LevelInstance& inst = m_instances[ii];
            if (!inst.model || !inst.model->hasBounds) continue;
            float vtxCtr[3] = {
                (inst.model->localMin[0]+inst.model->localMax[0])*0.5f,
                (inst.model->localMin[1]+inst.model->localMax[1])*0.5f,
                (inst.model->localMin[2]+inst.model->localMax[2])*0.5f
            };
            const float* m = inst.mat;
            float wx = vtxCtr[0]*m[0]+vtxCtr[1]*m[4]+vtxCtr[2]*m[8]+m[12];
            float wy = vtxCtr[0]*m[1]+vtxCtr[1]*m[5]+vtxCtr[2]*m[9]+m[13];
            float wz = vtxCtr[0]*m[2]+vtxCtr[1]*m[6]+vtxCtr[2]*m[10]+m[14];
            fprintf(dbg, "Inst[%d] '%s' mesh='%s'\n", ii, inst.objName.c_str(), inst.meshName.c_str());
            fprintf(dbg, "  WT_pos=(%.3f,%.3f,%.3f) vtxCtr_local=(%.3f,%.3f,%.3f) vtxCtr_world=(%.3f,%.3f,%.3f)\n",
                    m[12], m[13], m[14], vtxCtr[0], vtxCtr[1], vtxCtr[2], wx, wy, wz);
            ++instDiag;
        }

        // Dump PROP instances (HMD_PR_* or non-BL) with full WorldTransform
        fprintf(dbg, "\n=== PROP INSTANCE TRANSFORMS ===\n");
        int propDiag = 0;
        for (int ii = 0; ii < (int)m_instances.size() && propDiag < 15; ++ii)
        {
            const LevelInstance& inst = m_instances[ii];
            if (!inst.model) continue;
            // Skip building-level geometry — we want props
            const std::string& mn = inst.meshName;
            bool isBL = (mn.size() > 7 && mn[0]=='H' && mn[4]=='B' && mn[5]=='L' && mn[6]=='_');
            if (isBL) continue;
            const float* m = inst.mat;
            fprintf(dbg, "Prop[%d] inst=%d '%s' mesh='%s' type='%s' GMM=%d\n",
                    propDiag, ii, inst.objName.c_str(), inst.meshName.c_str(),
                    inst.typeName.c_str(), inst.gameModeMask);
            fprintf(dbg, "  Row0: [%9.4f %9.4f %9.4f %9.4f]\n", m[0], m[1], m[2], m[3]);
            fprintf(dbg, "  Row1: [%9.4f %9.4f %9.4f %9.4f]\n", m[4], m[5], m[6], m[7]);
            fprintf(dbg, "  Row2: [%9.4f %9.4f %9.4f %9.4f]\n", m[8], m[9], m[10], m[11]);
            fprintf(dbg, "  Row3: [%9.4f %9.4f %9.4f %9.4f]  <-- translation\n", m[12], m[13], m[14], m[15]);
            if (inst.model->hasBounds) {
                fprintf(dbg, "  localMin=(%.3f,%.3f,%.3f) localMax=(%.3f,%.3f,%.3f)\n",
                        inst.model->localMin[0], inst.model->localMin[1], inst.model->localMin[2],
                        inst.model->localMax[0], inst.model->localMax[1], inst.model->localMax[2]);
            }
            ++propDiag;
        }
    }

    // ── Detect level building rotation from first _BL_ instance with zero translation ──
    // Each level uses a different coordinate rotation for buildings (90° for Helm's Deep,
    // -45° for Isengard, etc.).  Collision data shares this space.
    // We detect it from the data — never hardcoded.
    {
        memset(m_levelRot, 0, sizeof(m_levelRot));
        m_levelRot[0] = m_levelRot[4] = m_levelRot[8] = 1.0f; // default identity
        for (int ii = 0; ii < (int)m_instances.size(); ++ii)
        {
            const LevelInstance& inst = m_instances[ii];
            const std::string& mn = inst.meshName;
            // Look for _BL_ building meshes with zero/near-zero translation
            if (mn.find("_BL_") == std::string::npos) continue;
            float t2 = inst.mat[12]*inst.mat[12] + inst.mat[13]*inst.mat[13] + inst.mat[14]*inst.mat[14];
            if (t2 > 1.0f) continue; // skip buildings with non-zero translation
            // Extract 3x3 rotation
            m_levelRot[0]=inst.mat[0]; m_levelRot[1]=inst.mat[1]; m_levelRot[2]=inst.mat[2];
            m_levelRot[3]=inst.mat[4]; m_levelRot[4]=inst.mat[5]; m_levelRot[5]=inst.mat[6];
            m_levelRot[6]=inst.mat[8]; m_levelRot[7]=inst.mat[9]; m_levelRot[8]=inst.mat[10];
            if (dbg) fprintf(dbg, "LevelRot detected from '%s': [%.3f,%.3f,%.3f / %.3f,%.3f,%.3f / %.3f,%.3f,%.3f]\n",
                            mn.c_str(), m_levelRot[0],m_levelRot[1],m_levelRot[2],
                            m_levelRot[3],m_levelRot[4],m_levelRot[5],
                            m_levelRot[6],m_levelRot[7],m_levelRot[8]);
            break;
        }
    }

    // ── Editor-object pass: collect all placed objects that have no visual mesh ──
    // These are logic nodes (spawn_point, trigger, capture_point, FED_*, etc.).
    // They are rendered as colored wireframe shapes so their position is visible.
    {
        // Derive a color for a type string (simple hash → hue)
        // Returns 0xFF______ ARGB
        for (size_t oi = 0; oi < objs.size(); ++oi)
        {
            const ZeroEngine::LevelGameObjEntry& obj = objs[oi];
            if (!obj.has_transform) continue;

            // Only add as editor object if the mesh was NOT successfully rendered
            bool hasMesh = false;
            if (obj.mesh_crc != 0)
            {
                std::map<uint32_t, LevelModel*>::iterator cit = m_modelCache.find(obj.mesh_crc);
                if (cit != m_modelCache.end() && cit->second != NULL)
                    hasMesh = true;
            }
            if (hasMesh) continue;

            LevelEditorObj eo;
            memcpy(eo.mat, obj.world_transform, sizeof(float) * 16);
            eo.size[0]  = obj.editor_size[0];
            eo.size[1]  = obj.editor_size[1];
            eo.size[2]  = obj.editor_size[2];
            eo.outer    = obj.editor_outer;
            eo.color    = obj.editor_color;
            eo.type     = obj.type_name;
            eo.shape    = obj.editor_shape;
            eo.guid          = obj.guid;
            eo.parentGuid    = obj.parent_guid;
            eo.gameModeMask  = obj.gamemodemask;
            eo.name          = obj.name;
            eo.block1WtOffset = obj.block1_wt_offset;
            eo.block1TfOffset = obj.block1_tf_offset;
            eo.layerGuid     = obj.layer_guid; // Phase 3 XSI layer manager

            // If the stored color is fully transparent/black, derive one from type name
            if ((eo.color & 0x00FFFFFFu) == 0)
            {
                // Simple hash of type name → RGB
                uint32_t h = 2166136261u;
                for (size_t ci = 0; ci < eo.type.size(); ++ci)
                    h = (h ^ (uint8_t)eo.type[ci]) * 16777619u;
                uint8_t r = (uint8_t)(((h      ) & 0x7Fu) + 64);
                uint8_t g = (uint8_t)(((h >>  8) & 0x7Fu) + 64);
                uint8_t b = (uint8_t)(((h >> 16) & 0x7Fu) + 64);
                eo.color = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
            else
            {
                eo.color |= 0xFF000000u; // ensure full alpha
            }

            // Default size if none set
            if (eo.size[0] <= 0.0f && eo.size[1] <= 0.0f && eo.size[2] <= 0.0f)
            {
                float def = (eo.outer > 0.1f) ? eo.outer : 1.5f;
                eo.size[0] = eo.size[1] = eo.size[2] = def;
            }

            m_editorObjs.push_back(eo);
        }
        if (dbg)
        {
            fprintf(dbg, "EditorObjs: %d\n", (int)m_editorObjs.size());
            // Log first 10 for verification
            int logN = (int)m_editorObjs.size() < 10 ? (int)m_editorObjs.size() : 10;
            for (int di = 0; di < logN; ++di)
            {
                const LevelEditorObj& e2 = m_editorObjs[di];
                fprintf(dbg, "  EO[%d] type=%s shape=%s pos=(%.1f,%.1f,%.1f) size=(%.2f,%.2f,%.2f) color=0x%08X\n",
                        di, e2.type.c_str(), e2.shape.c_str(),
                        e2.mat[12], e2.mat[13], e2.mat[14],
                        e2.size[0], e2.size[1], e2.size[2], e2.color);
            }
        }
    }

    // ── Build GUID -> editor obj index map (for Phase 3-5 visualizations) ──
    m_guidToEditorObj.clear();
    for (int gi = 0; gi < (int)m_editorObjs.size(); ++gi)
        m_guidToEditorObj[m_editorObjs[gi].guid] = gi;
    m_showPathLinks = true;
    m_showCaptureRadii = true;
    m_showSpawnChains = true;
    m_showAIGoals = true;
    m_showSoundRadii = true;
    m_showEventWires = true;
    m_eventWireFocusGuid = 0;

    // Cache game object fields needed by Phase 3-5 render passes
    m_goNode1.clear(); m_goNode2.clear(); m_goIntFields.clear();
    m_goFloatFields.clear(); m_goGuidRefs.clear(); m_goNodes.clear();
    m_goListRefs.clear(); m_goEditorOuter.clear(); m_goEditorSize0.clear();
    m_goOutputEvent.clear(); m_goInputEvent.clear();
    m_goTargetGuid.clear(); m_goOutputOwner.clear();
    {
        const std::vector<ZeroEngine::LevelGameObjEntry>& go = reader.GetGameObjs();
        for (size_t gi = 0; gi < go.size(); ++gi) {
            uint32_t g = go[gi].guid;
            if (go[gi].node1_guid)   m_goNode1[g] = go[gi].node1_guid;
            if (go[gi].node2_guid)   m_goNode2[g] = go[gi].node2_guid;
            if (!go[gi].int_fields.empty())   m_goIntFields[g] = go[gi].int_fields;
            if (!go[gi].float_fields.empty()) m_goFloatFields[g] = go[gi].float_fields;
            if (!go[gi].guid_refs.empty())    m_goGuidRefs[g] = go[gi].guid_refs;
            if (!go[gi].nodes.empty())        m_goNodes[g] = go[gi].nodes;
            if (!go[gi].list_refs.empty())    m_goListRefs[g] = go[gi].list_refs;
            if (go[gi].editor_outer > 0.01f)  m_goEditorOuter[g] = go[gi].editor_outer;
            if (go[gi].editor_size[0] > 0.01f) m_goEditorSize0[g] = go[gi].editor_size[0];

            // Output entity caches: stash event/target so the render pass
            // doesn't have to walk go[] again per frame.
            if (!go[gi].output_event.empty()) m_goOutputEvent[g] = go[gi].output_event;
            if (!go[gi].input_event.empty())  m_goInputEvent[g]  = go[gi].input_event;
            if (go[gi].target_guid != 0)      m_goTargetGuid[g]  = go[gi].target_guid;

            // Reverse-walk outputs[] to populate owner. Pandemic's exporter
            // didn't bother stamping owner directly on Output entities, so
            // we figure it out by looking at who CLAIMS each Output. If two
            // entities both list the same Output GUID (shouldn't happen but
            // who the fuck knows what corner cases the disk format hides),
            // last write wins.
            for (size_t oi = 0; oi < go[gi].outputs.size(); ++oi) {
                uint32_t outputGuid = go[gi].outputs[oi];
                if (outputGuid != 0) m_goOutputOwner[outputGuid] = g;
            }
        }
    }

    // ── Terrain_ pass: load models not referenced by any WorldTransform game object ──
    // Blender's imp/level.py places every model whose name starts with "Terrain_"
    // at identity under the templateLevel node.  These models don't appear as
    // static_object entries with WorldTransform — they're placed implicitly.
    {
        const std::vector<ZeroEngine::LevelModelEntry>& allMdl = reader.GetModels();
        int terrainLoaded = 0, terrainSkipped = 0;
        for (size_t mi = 0; mi < allMdl.size(); ++mi)
        {
            const ZeroEngine::LevelModelEntry& me = allMdl[mi];
            // Case-insensitive prefix check for "terrain_"
            const std::string& n = me.name;
            if (n.size() < 8) continue;
            bool isTerrain = (tolower((unsigned char)n[0]) == 't' &&
                              tolower((unsigned char)n[1]) == 'e' &&
                              tolower((unsigned char)n[2]) == 'r' &&
                              tolower((unsigned char)n[3]) == 'r' &&
                              tolower((unsigned char)n[4]) == 'a' &&
                              tolower((unsigned char)n[5]) == 'i' &&
                              tolower((unsigned char)n[6]) == 'n' &&
                              n[7] == '_');
            if (!isTerrain) continue;

            // Look up or load model
            LevelModel* model = NULL;
            std::map<uint32_t, LevelModel*>::iterator cit = m_modelCache.find(me.crc);
            if (cit != m_modelCache.end())
            {
                model = cit->second;
            }
            else
            {
                LevelModel* nm = new LevelModel();
                nm->name = me.name;
                if (loadModelFromBinary(reader, me.crc, me.name, nm))
                    model = nm;
                else
                    delete nm;
                m_modelCache[me.crc] = model;
            }

            if (!model) { ++terrainSkipped; continue; }

            LevelInstance inst;
            inst.model = model;
            // Identity world transform
            memset(inst.mat, 0, sizeof(inst.mat));
            inst.mat[0] = inst.mat[5] = inst.mat[10] = inst.mat[15] = 1.0f;
            inst.layerGuid = 0; // terrain instances have no entity backing
            m_instances.push_back(inst);
            ++terrainLoaded;
        }
        if (dbg) fprintf(dbg, "Terrain pass: loaded=%d skipped=%d\n", terrainLoaded, terrainSkipped);
    }

    // ── Spline + cinematic camera collection ────────────────────────────────────
    {
        // Pass 1: collect all Spline objects
        std::map<uint32_t, size_t> splineIdx; // guid → index in m_splines
        for (size_t oi = 0; oi < objs.size(); ++oi)
        {
            const ZeroEngine::LevelGameObjEntry& obj = objs[oi];
            if (obj.spline_nodes.empty()) continue;

            LevelSpline spl;
            spl.guid = obj.guid;
            spl.name = obj.name;
            spl.nodes.resize(obj.spline_nodes.size());
            for (size_t ni = 0; ni < obj.spline_nodes.size(); ++ni)
            {
                spl.nodes[ni].x = obj.spline_nodes[ni].x;
                spl.nodes[ni].y = obj.spline_nodes[ni].y;
                spl.nodes[ni].z = obj.spline_nodes[ni].z;
                spl.nodes[ni].s = obj.spline_nodes[ni].s;
            }
            splineIdx[spl.guid] = m_splines.size();
            m_splines.push_back(spl);
        }

        // Pass 2: collect fancy_cinematic_camera objects
        for (size_t oi = 0; oi < objs.size(); ++oi)
        {
            const ZeroEngine::LevelGameObjEntry& obj = objs[oi];
            if (obj.position_track_guid == 0 && obj.target_track_guid == 0) continue;
            if (obj.total_duration <= 0.0f) continue;

            LevelCinematicCamera cam;
            cam.guid = obj.guid;
            cam.name = obj.name;
            memcpy(cam.mat, obj.world_transform, sizeof(float) * 16);
            cam.fov = obj.fov;
            cam.totalDuration = obj.total_duration;
            cam.positionTravelTime = obj.position_travel_time;
            cam.targetTravelTime = obj.target_travel_time;
            cam.positionTrackGuid = obj.position_track_guid;
            cam.targetTrackGuid = obj.target_track_guid;

            // Resolve spline pointers
            cam.positionTrack = NULL;
            cam.targetTrack = NULL;
            std::map<uint32_t, size_t>::iterator pit = splineIdx.find(obj.position_track_guid);
            if (pit != splineIdx.end()) cam.positionTrack = &m_splines[pit->second];
            std::map<uint32_t, size_t>::iterator tit = splineIdx.find(obj.target_track_guid);
            if (tit != splineIdx.end()) cam.targetTrack = &m_splines[tit->second];

            m_cineCameras.push_back(cam);
        }

        if (dbg)
        {
            fprintf(dbg, "Splines: %d  CinematicCameras: %d\n",
                    (int)m_splines.size(), (int)m_cineCameras.size());
            for (size_t ci = 0; ci < m_cineCameras.size(); ++ci)
            {
                const LevelCinematicCamera& c = m_cineCameras[ci];
                fprintf(dbg, "  CineCam[%d] name='%s' dur=%.1f posTrack=%s(%d pts) tgtTrack=%s(%d pts)\n",
                        (int)ci, c.name.c_str(), c.totalDuration,
                        c.positionTrack ? c.positionTrack->name.c_str() : "NULL",
                        c.positionTrack ? (int)c.positionTrack->nodes.size() : 0,
                        c.targetTrack ? c.targetTrack->name.c_str() : "NULL",
                        c.targetTrack ? (int)c.targetTrack->nodes.size() : 0);
            }
        }
    }

    if (dbg)
    {
        int texOk = 0, texNull = 0;
        for (std::map<uint32_t, IDirect3DTexture9*>::iterator ti = m_texCache.begin();
             ti != m_texCache.end(); ++ti)
        {
            if (ti->second) ++texOk; else ++texNull;
        }
        fprintf(dbg, "Result: loaded=%d skipped=%d m_instances=%d\n",
                loaded, skipped, (int)m_instances.size());
        fprintf(dbg, "Textures: loaded=%d failed/missing=%d (see levelscene_texdbg.txt)\n",
                texOk, texNull);
        if (m_boundsValid)
            fprintf(dbg, "Bounds: min=(%.1f,%.1f,%.1f) max=(%.1f,%.1f,%.1f) extentXZ=%.1f\n",
                    m_boundsMin[0], m_boundsMin[1], m_boundsMin[2],
                    m_boundsMax[0], m_boundsMax[1], m_boundsMax[2],
                    (m_boundsMax[0]-m_boundsMin[0]) > (m_boundsMax[2]-m_boundsMin[2])
                        ? (m_boundsMax[0]-m_boundsMin[0]) : (m_boundsMax[2]-m_boundsMin[2]));
        else
            fprintf(dbg, "Bounds: NONE (no valid vertex data read)\n");
        fclose(dbg);
    }

    // ── 3D Crowd loading ──────────────────────────────────────────────────
    // Two-pass now. First pass parses the WHOLE Block2 3dCrowd sub-block into
    // m_crowdItems (the new authoring source-of-truth). Second pass walks
    // m_crowdItems and pushes one LevelInstance per CrowdVal, TAGGED with
    // (crowdItemIdx, crowdValIdx) so a later gizmo drag can find its way
    // back to the originating data and write the new pos/rot through.
    //
    // Binary format (refresher because I'll forget in a week):
    //   u32 magic(0x65), u32 count, u32 offs[count], then CrowdItem[count]
    //   each CrowdItem: CrowdHeader(28B) + Crc[anim_num](4 each) + CrowdVal[inst_num](20 each)
    //   CrowdHeader: key(4) key_main(4) key_right(4) key_left(4) unk_4(4) anim_num(4) inst_num(4)
    //   CrowdVal:    position(12) rotation(4) lod(4) = 20 bytes
    m_crowdItems.clear();
    m_crowdDirty = false;
    m_crowdFocusModeOn = false;
    m_crowdFocusedItem = -1;
    {
        int crowdLoaded = 0;
        static const uint32_t CRC_3DCROWD = ZeroEngine::LotrHashString("3dCrowd");
        const std::vector<uint8_t>& b2 = reader.GetBlock2();
        uint32_t sb2Off = reader.GetPakHeader().sub_blocks2_offset;

        // Find the 3dCrowd sub_block offset in block2
        // SubBlocks header: 16 bytes, then 12-byte entries (key, offset, size)
        uint32_t crowdDataOff = 0;
        uint32_t crowdDataSize = 0;
        if (!b2.empty() && sb2Off + 16 <= (uint32_t)b2.size())
        {
            uint32_t block_num;
            memcpy(&block_num, &b2[sb2Off + 4], 4);
            if (block_num <= 65536 && sb2Off + 16 + block_num * 12 <= (uint32_t)b2.size())
            {
                for (uint32_t si = 0; si < block_num; ++si)
                {
                    uint32_t sk, soff, ssz;
                    memcpy(&sk,   &b2[sb2Off + 16 + si * 12 + 0], 4);
                    memcpy(&soff, &b2[sb2Off + 16 + si * 12 + 4], 4);
                    memcpy(&ssz,  &b2[sb2Off + 16 + si * 12 + 8], 4);
                    if (sk == CRC_3DCROWD)
                    {
                        crowdDataOff = sb2Off + soff;
                        crowdDataSize = ssz;
                        break;
                    }
                }
            }
        }

        if (crowdDataOff > 0 && crowdDataSize > 8 &&
            (uint64_t)crowdDataOff + crowdDataSize <= b2.size())
        {
            const uint8_t* cd = &b2[crowdDataOff];
            uint32_t magic, itemCount;
            memcpy(&magic, cd + 0, 4);
            memcpy(&itemCount, cd + 4, 4);

            if (magic == 0x65 && itemCount > 0 && itemCount < 10000)
            {
                LevelLogf("[Crowd] Found 3dCrowd block: %u items, %u bytes", itemCount, crowdDataSize);

                // ── Pass 1: parse the whole block into m_crowdItems ──
                std::vector<uint32_t> itemOffsets(itemCount);
                if (8 + itemCount * 4 <= crowdDataSize)
                {
                    memcpy(&itemOffsets[0], cd + 8, itemCount * 4);

                    for (uint32_t ci = 0; ci < itemCount; ++ci)
                    {
                        uint32_t ioff = itemOffsets[ci];
                        if (ioff + 28 > crowdDataSize) continue;
                        const uint8_t* item = cd + ioff;

                        // CrowdHeader read in full this time, not the half-assed
                        // four-fields-and-bail we did before. Every byte gets a home.
                        uint32_t meshCrc, modelCrc, rightCrc, leftCrc;
                        float    playbackRate;
                        uint32_t animNum, instNum;
                        memcpy(&meshCrc,      item +  0, 4);
                        memcpy(&modelCrc,     item +  4, 4);
                        memcpy(&rightCrc,     item +  8, 4);
                        memcpy(&leftCrc,      item + 12, 4);
                        memcpy(&playbackRate, item + 16, 4);
                        memcpy(&animNum,      item + 20, 4);
                        memcpy(&instNum,      item + 24, 4);

                        if (instNum > 100000) continue;
                        if (animNum > 1024) continue; // sanity

                        uint32_t instOff = 28 + animNum * 4;
                        if (ioff + instOff + instNum * 20 > crowdDataSize) continue;

                        LevelCrowdItem outItem;
                        outItem.meshKey       = meshCrc;
                        outItem.modelKey      = modelCrc;
                        outItem.rightHandKey  = rightCrc;
                        outItem.leftHandKey   = leftCrc;
                        outItem.playbackRate  = playbackRate;
                        outItem.meshKeyName   = reader.ResolveCrc(meshCrc);
                        outItem.modelKeyName  = reader.ResolveCrc(modelCrc);
                        outItem.rightHandName = reader.ResolveCrc(rightCrc);
                        outItem.leftHandName  = reader.ResolveCrc(leftCrc);

                        // Animation CRC list — finally consumed instead of skipped
                        outItem.animationKeys.reserve(animNum);
                        outItem.animationNames.reserve(animNum);
                        for (uint32_t ai = 0; ai < animNum; ++ai) {
                            uint32_t aCrc;
                            memcpy(&aCrc, item + 28 + ai * 4, 4);
                            outItem.animationKeys.push_back(aCrc);
                            outItem.animationNames.push_back(reader.ResolveCrc(aCrc));
                        }

                        // Per-placement CrowdVal entries
                        const uint8_t* vals = item + instOff;
                        outItem.instances.reserve(instNum);
                        for (uint32_t vi = 0; vi < instNum; ++vi) {
                            const uint8_t* v = vals + vi * 20;
                            LevelCrowdInstance cinst;
                            memcpy(&cinst.position[0], v + 0,  4);
                            memcpy(&cinst.position[1], v + 4,  4);
                            memcpy(&cinst.position[2], v + 8,  4);
                            memcpy(&cinst.rotation,    v + 12, 4);
                            memcpy(&cinst.lod,         v + 16, 4);
                            outItem.instances.push_back(cinst);
                        }

                        LevelLogf("[Crowd]   item %u: model=%s meshCrc=0x%08X instances=%u anims=%u rate=%.2f",
                                  ci, outItem.modelKeyName.c_str(), meshCrc,
                                  (unsigned)outItem.instances.size(),
                                  (unsigned)outItem.animationKeys.size(),
                                  playbackRate);

                        // ── Populate the model cache so Pass 2 can render ──
                        // The refactor that split this loader into two passes
                        // dropped the model-load step. Without it,
                        // rebuildCrowdInstances() does a cache miss and silently
                        // skips every crowd member, so the viewport renders
                        // nothing. This bug bit the user once. Don't let it
                        // happen again — load each archetype's master rig
                        // exactly the way the original pre-refactor loader did.
                        if (m_modelCache.find(modelCrc) == m_modelCache.end()) {
                            LevelModel* nm = new LevelModel();
                            nm->name = outItem.modelKeyName;
                            if (loadModelFromBinary(reader, modelCrc, outItem.modelKeyName, nm)) {
                                m_modelCache[modelCrc] = nm;
                            } else {
                                delete nm;
                                m_modelCache[modelCrc] = NULL; // remember the failure so we don't retry
                                LevelLogf("[Crowd]   FAILED to load model 0x%08X (%s) — crowd item will not render",
                                          modelCrc, outItem.modelKeyName.c_str());
                            }
                        }

                        m_crowdItems.push_back(outItem);
                    }
                }

                // ── Pass 2: hand m_crowdItems to the LevelInstance pump ──
                rebuildCrowdInstances();
                for (size_t ii = 0; ii < m_crowdItems.size(); ++ii) {
                    crowdLoaded += (int)m_crowdItems[ii].instances.size();
                }
            }
        }

        if (crowdLoaded > 0)
            LevelLogf("[Crowd] Loaded %d crowd instances across %d items",
                      crowdLoaded, (int)m_crowdItems.size());
    }

    m_loaded = (loaded > 0);

    // ── Parse collision shapes from Block1 for all loaded model instances ────
    if (m_loaded) {
        const std::vector<uint8_t>& b1 = reader.GetBlock1();
        const ZeroEngine::PakHeader& hdr = reader.GetPakHeader();
        uint32_t shapeStride = hdr.shape_info_size;
        uint32_t shapeBase   = hdr.shape_info_offset;
        uint32_t shapeCount  = hdr.shape_info_num;

        m_collisionShapes.clear();

        // Build a map: shape_info byte offset → list of instances using that model.
        // ModelInfo.shape_offset is a BYTE OFFSET relative to shape_info_offset.
        // ModelInfo.shape_num is the count of shapes for that model.
        uint32_t miBase = hdr.model_info_offset;
        uint32_t miCount = hdr.model_info_num;

        // shape byte offset → (model CRC, shape count)
        struct ShapeRef { uint32_t byteOff; uint32_t num; uint32_t modelCrc; };
        std::vector<ShapeRef> modelShapeRefs;

        if (miBase + miCount * 256 <= (uint32_t)b1.size()) {
            for (uint32_t mi = 0; mi < miCount; ++mi) {
                const ZeroEngine::ModelInfo* mdl =
                    reinterpret_cast<const ZeroEngine::ModelInfo*>(&b1[miBase + mi * 256]);
                if (mdl->shape_num > 0 && mdl->shape_num < 10000) {
                    // Try multiple interpretations of shape_offset:
                    // A) absolute Block1 offset
                    // B) relative to shape_info_offset
                    // C) index into shape_info array
                    uint32_t offA = mdl->shape_offset;
                    uint32_t offB = shapeBase + mdl->shape_offset;
                    uint32_t offC = shapeBase + mdl->shape_offset * shapeStride;
                    uint32_t best = 0;
                    if (offA + mdl->shape_num * shapeStride <= (uint32_t)b1.size()) best = offA;
                    else if (offB + mdl->shape_num * shapeStride <= (uint32_t)b1.size()) best = offB;
                    else if (offC + mdl->shape_num * shapeStride <= (uint32_t)b1.size()) best = offC;
                    if (best > 0) {
                        ShapeRef sr;
                        sr.byteOff = best;
                        sr.num = mdl->shape_num;
                        sr.modelCrc = mdl->key;
                        modelShapeRefs.push_back(sr);
                    }
                    // Log first few for debugging
                    if (mi < 5 && mdl->shape_num > 0) {
                        LevelLogf("[Collision] Model %u: shape_offset=%u shape_num=%u shapeBase=%u stride=%u best=%u",
                                   mdl->key, mdl->shape_offset, mdl->shape_num, shapeBase, shapeStride, best);
                    }
                }
            }
        }

        // Build model CRC → instance list map
        std::map<uint32_t, std::vector<int> > crcToInstances;
        for (int ii = 0; ii < (int)m_instances.size(); ++ii) {
            if (!m_instances[ii].model) continue;
            for (std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.begin();
                 it != m_modelCache.end(); ++it) {
                if (it->second == m_instances[ii].model) {
                    crcToInstances[it->first].push_back(ii);
                    break;
                }
            }
        }

        LevelLogf("[Collision] %d models have shapes (stride=%d, count=%d, base=%d)",
                   (int)modelShapeRefs.size(), shapeStride, shapeCount, shapeBase);

        if (shapeStride > 0)
        {
            for (size_t ri = 0; ri < modelShapeRefs.size(); ++ri) {
                const ShapeRef& ref = modelShapeRefs[ri];
                std::map<uint32_t, std::vector<int> >::const_iterator iit = crcToInstances.find(ref.modelCrc);
                if (iit == crcToInstances.end()) continue;
                const std::vector<int>& instList = iit->second;

                for (uint32_t si = 0; si < ref.num; ++si) {
                    // shape_offset is absolute Block1 offset
                    uint32_t sOff = ref.byteOff + si * shapeStride;
                    if (sOff + shapeStride > (uint32_t)b1.size()) break;
                    const uint8_t* sd = &b1[sOff];

                    uint32_t kind;
                    memcpy(&kind, sd + 4, 4);

                    // Read shape local translation (Vector3 at offset 24)
                    float lx0, ly0, lz0;
                    memcpy(&lx0, sd + 24, 4);
                    memcpy(&ly0, sd + 28, 4);
                    memcpy(&lz0, sd + 32, 4);
                    // Apply level rotation
                    const float* R = m_levelRot;
                    float lx = lx0*R[0] + ly0*R[3] + lz0*R[6];
                    float ly = lx0*R[1] + ly0*R[4] + lz0*R[7];
                    float lz = lx0*R[2] + ly0*R[5] + lz0*R[8];

                    // Read dimensions
                    float halfX = 1.0f, halfY = 1.0f, halfZ = 1.0f;
                    float radius = 1.0f;
                    float p1[3] = {0,0,0}, p2[3] = {0,0,0};
                    if (shapeStride >= 80) {
                        float f0, f1, f2;
                        memcpy(&f0, sd + 52, 4);
                        memcpy(&f1, sd + 56, 4);
                        memcpy(&f2, sd + 60, 4);
                        if (kind == 1) {
                            if (f0 > 0.001f) halfX = f0;
                            if (f1 > 0.001f) halfY = f1;
                            if (f2 > 0.001f) halfZ = f2;
                        } else if (kind == 2) {
                            if (f0 > 0.001f) radius = f0;
                        } else if (kind == 3 || kind == 4) {
                            float rp1[3], rp2[3];
                            memcpy(rp1, sd + 52, 12);
                            memcpy(rp2, sd + 64, 12);
                            memcpy(&radius, sd + 76, 4);
                            // Apply level rotation to endpoints
                            p1[0]=rp1[0]*R[0]+rp1[1]*R[3]+rp1[2]*R[6];
                            p1[1]=rp1[0]*R[1]+rp1[1]*R[4]+rp1[2]*R[7];
                            p1[2]=rp1[0]*R[2]+rp1[1]*R[5]+rp1[2]*R[8];
                            p2[0]=rp2[0]*R[0]+rp2[1]*R[3]+rp2[2]*R[6];
                            p2[1]=rp2[0]*R[1]+rp2[1]*R[4]+rp2[2]*R[7];
                            p2[2]=rp2[0]*R[2]+rp2[1]*R[5]+rp2[2]*R[8];
                        }
                    }

                    // Create one collision shape per instance that uses this model
                    for (size_t ni = 0; ni < instList.size(); ++ni) {
                        int ii = instList[ni];
                        const LevelInstance& inst = m_instances[ii];
                        const float* m = inst.mat;

                        LevelCollisionShape cs;
                        memset(&cs, 0, sizeof(cs));
                        cs.kind = (int)kind;
                        cs.instanceIdx = ii;
                        cs.modelCrc = ref.modelCrc;
                        cs.block1Offset = sOff;

                        // Transform local position by instance world matrix
                        float wx = lx*m[0] + ly*m[4] + lz*m[8]  + m[12];
                        float wy = lx*m[1] + ly*m[5] + lz*m[9]  + m[13];
                        float wz = lx*m[2] + ly*m[6] + lz*m[10] + m[14];

                        memcpy(cs.worldMat, inst.mat, 64);
                        cs.worldMat[12] = wx;
                        cs.worldMat[13] = wy;
                        cs.worldMat[14] = wz;

                        cs.halfExt[0] = halfX; cs.halfExt[1] = halfY; cs.halfExt[2] = halfZ;
                        cs.radius = radius;
                        memcpy(cs.pt1, p1, 12);
                        memcpy(cs.pt2, p2, 12);

                        m_collisionShapes.push_back(cs);
                    }
                }
            }
        }
        LevelLogf("[Collision] Loaded %d collision shapes (%d model refs, %d instances)",
                   (int)m_collisionShapes.size(), (int)modelShapeRefs.size(), (int)m_instances.size());

        // ── Load world collision meshes (BVTree / kind=6) from HkShapeInfo ──
        uint32_t hkBase   = hdr.hk_shape_info_offset;
        uint32_t hkStride = hdr.hk_shape_info_size;
        uint32_t hkCount  = hdr.hk_shape_info_num;
        int bvtLoaded = 0;

        if (hkStride >= 80 && hkCount > 0 && hkBase + hkCount * hkStride <= (uint32_t)b1.size()) {
            for (uint32_t hi = 0; hi < hkCount; ++hi) {
                const uint8_t* hd = &b1[hkBase + hi * hkStride];
                uint32_t hkKind;
                memcpy(&hkKind, hd + 32, 4);
                if (hkKind != 6) continue; // only BVTreeMesh

                // BVTreeMeshInfo layout (pak_alt/shape.rs):
                // +64: vert_num, +68: verts_offset, +72: tri_num, +76: inds_offset
                uint32_t vertNum, vertsOff, triNum, indsOff;
                memcpy(&vertNum,  hd + 64, 4);
                memcpy(&vertsOff, hd + 68, 4);
                memcpy(&triNum,   hd + 72, 4);
                memcpy(&indsOff,  hd + 76, 4);

                // Translation (Vector4 at offset 0)
                float tx, ty, tz;
                memcpy(&tx, hd + 0, 4);
                memcpy(&ty, hd + 4, 4);
                memcpy(&tz, hd + 8, 4);

                if (bvtLoaded < 3) {
                    LevelLogf("[BVTree] #%d: kind=%d vertNum=%u vertsOff=%u triNum=%u indsOff=%u pos=(%.1f,%.1f,%.1f) b1size=%u",
                               bvtLoaded, hkKind, vertNum, vertsOff, triNum, indsOff, tx, ty, tz, (unsigned)b1.size());
                }

                if (vertNum == 0 || vertNum > 500000 || triNum == 0 || triNum > 500000) continue;
                uint32_t vertEnd = vertsOff + vertNum * 12; // Vector3 = 12 bytes
                uint32_t idxEnd  = indsOff + triNum * 3 * 2; // u16 indices
                if (vertEnd > (uint32_t)b1.size() || idxEnd > (uint32_t)b1.size()) {
                    if (bvtLoaded < 3)
                        LevelLogf("[BVTree] SKIP: vertEnd=%u idxEnd=%u exceeds b1size=%u", vertEnd, idxEnd, (unsigned)b1.size());
                    continue;
                }

                // Create D3D vertex buffer
                struct WireVert { float x,y,z; DWORD col; };
                DWORD bvtCol = 0x40FF4040; // semi-transparent red for world collision

                IDirect3DVertexBuffer9* vb = NULL;
                HRESULT hr = m_device->CreateVertexBuffer(
                    vertNum * sizeof(WireVert), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_DIFFUSE,
                    D3DPOOL_MANAGED, &vb, NULL);
                if (FAILED(hr) || !vb) continue;

                WireVert* vptr = NULL;
                vb->Lock(0, 0, (void**)&vptr, 0);
                const uint8_t* vSrc = &b1[vertsOff];
                for (uint32_t vi = 0; vi < vertNum; ++vi) {
                    float vx, vy, vz;
                    memcpy(&vx, vSrc + vi * 12 + 0, 4);
                    memcpy(&vy, vSrc + vi * 12 + 4, 4);
                    memcpy(&vz, vSrc + vi * 12 + 8, 4);
                    // Apply level building rotation (detected from first _BL_ instance)
                    const float* R = m_levelRot;
                    vptr[vi].x = vx*R[0] + vy*R[3] + vz*R[6];
                    vptr[vi].y = vx*R[1] + vy*R[4] + vz*R[7];
                    vptr[vi].z = vx*R[2] + vy*R[5] + vz*R[8];
                    vptr[vi].col = bvtCol;
                }
                vb->Unlock();

                // Create D3D index buffer (convert u16 triangle indices to wireframe line pairs)
                // Each triangle = 3 edges = 6 indices
                uint32_t lineIdxCount = triNum * 6;
                IDirect3DIndexBuffer9* ib = NULL;
                hr = m_device->CreateIndexBuffer(
                    lineIdxCount * 2, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                    D3DPOOL_MANAGED, &ib, NULL);
                if (FAILED(hr) || !ib) { vb->Release(); continue; }

                uint16_t* iptr = NULL;
                ib->Lock(0, 0, (void**)&iptr, 0);
                const uint16_t* triIdx = reinterpret_cast<const uint16_t*>(&b1[indsOff]);
                for (uint32_t ti = 0; ti < triNum; ++ti) {
                    uint16_t i0 = triIdx[ti * 3 + 0];
                    uint16_t i1 = triIdx[ti * 3 + 1];
                    uint16_t i2 = triIdx[ti * 3 + 2];
                    iptr[ti * 6 + 0] = i0; iptr[ti * 6 + 1] = i1;
                    iptr[ti * 6 + 2] = i1; iptr[ti * 6 + 3] = i2;
                    iptr[ti * 6 + 4] = i2; iptr[ti * 6 + 5] = i0;
                }
                ib->Unlock();

                LevelWorldCollisionMesh wcm;
                wcm.vb = vb;
                wcm.ib = ib;
                wcm.vertCount = (int)vertNum;
                wcm.triCount = (int)triNum;
                wcm.translation[0] = tx;
                wcm.translation[1] = ty;
                wcm.translation[2] = tz;
                m_worldCollMeshes.push_back(wcm);
                ++bvtLoaded;
            }
        }
        LevelLogf("[Collision] Loaded %d world collision meshes (BVTree)", bvtLoaded);
    }

    if (m_loaded) extractLighting(reader);

    // Set sky tint from atmospheric inscatter (makes skybox warm red/orange for Mordor)
    {
        extern float g_skyTint[4];
        // Inscatter color = the atmospheric haze color (warm for Mordor levels)
        // Mix with white so the skybox texture shows through but is tinted
        float ir = m_lighting.inscatterCol[0] * m_lighting.inscatterCol[3];
        float ig = m_lighting.inscatterCol[1] * m_lighting.inscatterCol[3];
        float ib = m_lighting.inscatterCol[2] * m_lighting.inscatterCol[3];
        // Normalize so the brightest channel = 1.0, then blend 60% tint + 40% white
        float maxI = ir > ig ? ir : ig; if (ib > maxI) maxI = ib;
        if (maxI > 0.01f) { ir /= maxI; ig /= maxI; ib /= maxI; }
        else { ir = ig = ib = 1.0f; }
        float blend = 0.6f; // 60% inscatter tint, 40% white
        g_skyTint[0] = ir * blend + (1.0f - blend);
        g_skyTint[1] = ig * blend + (1.0f - blend);
        g_skyTint[2] = ib * blend + (1.0f - blend);
        g_skyTint[3] = 1.0f;
        LevelLogf("[Lighting] Sky tint: (%.2f, %.2f, %.2f)", g_skyTint[0], g_skyTint[1], g_skyTint[2]);
    }

    // ══ AMBIENT CUBEMAP - DESATURATED HARD ON FUCKING PURPOSE ════════════
    //
    // Some bright spark added a per-face ambient cubemap bound to game
    // PS stage 6, sampled by the engine's ambient-lookup formula:
    //
    //     amb = texCube(s6, worldNormal) * c1.w * c72 + c1.rgb
    //
    // In theory: cute. Adds atmospheric tint to ambient fill. Pretty for
    // Mordor where the inscatter is red and the world should glow warm.
    // In practice on every other goddamn level: an absolute fucking
    // disaster. They jammed `inscatterCol * multiplier * 0.25` straight
    // into the cube faces, which for Helm's Deep means every face is
    // PURE motherfucking BLUE, and every pixel of every model in the
    // scene gets a fat blue ambient slap on top of whatever the actual
    // texture is trying to show. The whole goddamn level renders looking
    // submerged in a cursed swimming pool. Hours of "why is everything
    // blue" debugging because the blue tint was being painted by an
    // entirely different system than the one we were staring at.
    //
    // FIX: desaturate the cube before writing it. Rec.709 luminance of
    // the inscatter tint, then blend 80% toward neutral grey + 20%
    // toward the original hue. Mordor still reads warm because its
    // luminance carries the warm signal regardless. Helm's Deep ambient
    // cube ends up near-neutral with a 20% blue whisper that is
    // effectively fucking invisible at runtime. Subtle hint, not
    // full-screen colored fog. Which is what an ambient cube was
    // supposed to do in the first goddamn place.
    if (m_ambientCube && m_lightingExtracted) {
        float ir0 = m_lighting.inscatterCol[0] * m_lighting.inscatterCol[3] * 0.25f;
        float ig0 = m_lighting.inscatterCol[1] * m_lighting.inscatterCol[3] * 0.25f;
        float ib0 = m_lighting.inscatterCol[2] * m_lighting.inscatterCol[3] * 0.25f;
        if (ir0 > 0.6f) ir0 = 0.6f; if (ig0 > 0.6f) ig0 = 0.6f; if (ib0 > 0.6f) ib0 = 0.6f;
        // Desaturate: blend heavily toward the LUMINANCE (Rec.709). That
        // preserves the brightness the level designer wanted but strips
        // the saturated channel dominance that turned everything blue.
        float lum = ir0 * 0.2126f + ig0 * 0.7152f + ib0 * 0.0722f;
        const float kTintBlend = 0.20f;  // 20% original hue, 80% grey
        float ir = lum * (1.0f - kTintBlend) + ir0 * kTintBlend;
        float ig = lum * (1.0f - kTintBlend) + ig0 * kTintBlend;
        float ib = lum * (1.0f - kTintBlend) + ib0 * kTintBlend;
        // Sun-facing side gets sun-tinted ambient (stronger contribution)
        float sr = m_lighting.sunCol[0] * 0.12f + ir;
        float sg = m_lighting.sunCol[1] * 0.12f + ig;
        float sb = m_lighting.sunCol[2] * 0.12f + ib;
        // Ground bounce from bottom dome
        float gr = m_lighting.botDomeCol[0] + ir;
        float gg = m_lighting.botDomeCol[1] + ig;
        float gb = m_lighting.botDomeCol[2] + ib;
        // Top = sky color (dark for Mordor)
        float tr = m_lighting.topDomeCol[0] + ir;
        float tg = m_lighting.topDomeCol[1] + ig;
        float tb = m_lighting.topDomeCol[2] + ib;

        // D3DCUBEMAP_FACE order: +X, -X, +Y, -Y, +Z, -Z
        float faceColors[6][3] = {
            { sr, sg, sb },  // +X: side (sun-ish)
            { ir, ig, ib },  // -X: side (ambient)
            { tr, tg, tb },  // +Y: top (sky)
            { gr, gg, gb },  // -Y: bottom (ground bounce)
            { sr, sg, sb },  // +Z: side (sun-ish)
            { ir, ig, ib },  // -Z: side (ambient)
        };
        for (int face = 0; face < 6; ++face) {
            uint8_t r8 = (uint8_t)(faceColors[face][0] > 1.0f ? 255 : (int)(faceColors[face][0]*255));
            uint8_t g8 = (uint8_t)(faceColors[face][1] > 1.0f ? 255 : (int)(faceColors[face][1]*255));
            uint8_t b8 = (uint8_t)(faceColors[face][2] > 1.0f ? 255 : (int)(faceColors[face][2]*255));
            DWORD col = 0xFF000000 | (r8 << 16) | (g8 << 8) | b8;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(m_ambientCube->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0))) {
                *(DWORD*)lr.pBits = col;
                m_ambientCube->UnlockRect((D3DCUBEMAP_FACES)face, 0);
            }
        }
        LevelLogf("[Lighting] Ambient cubemap DESATURATED and updated.");
        LevelLogf("[Lighting]   raw inscatter tint: (%.2f,%.2f,%.2f)  lum=%.2f  20%% blend preserved",
                  ir0, ig0, ib0, lum);
        LevelLogf("[Lighting]   final cube tint:    (%.2f,%.2f,%.2f)  sun-side: (%.2f,%.2f,%.2f)",
                  ir, ig, ib, sr, sg, sb);
        LevelLogf("[Lighting]   (Raw inscatter dominated the cube and made every cool-atmosphere");
        LevelLogf("[Lighting]    level render underwater. Desaturation keeps the atmospheric hint");
        LevelLogf("[Lighting]    without drowning the scene in fucking swimming pool tint.)");
    }

    s_renderLogOnce = false; // reset so first-frame render log fires again
    LevelLogf("[Load] Complete: %s — %d instances loaded, %d skipped, %d models, %d textures",
              m_levelName.c_str(), loaded, skipped, (int)m_modelCache.size(), (int)m_texCache.size());
    if (m_boundsValid)
        LevelLogf("[Load] Bounds: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
                  m_boundsMin[0], m_boundsMin[1], m_boundsMin[2],
                  m_boundsMax[0], m_boundsMax[1], m_boundsMax[2]);
    return m_loaded;
}

// ── unload() ──────────────────────────────────────────────────────────────────

void LevelScene::unload()
{
    g_partFlags.clear();
    m_instances.clear();
    m_editorObjs.clear();
    m_splines.clear();
    m_cineCameras.clear();
    m_collisionShapes.clear();
    for (size_t wi = 0; wi < m_worldCollMeshes.size(); ++wi) m_worldCollMeshes[wi].release();
    m_worldCollMeshes.clear();
    if (m_editorFont) { m_editorFont->Release(); m_editorFont = NULL; }

    for (std::map<uint32_t, LevelModel*>::iterator it = m_modelCache.begin();
         it != m_modelCache.end(); ++it)
    {
        if (it->second) { it->second->release(); delete it->second; }
    }
    m_modelCache.clear();

    for (std::map<uint32_t, IDirect3DTexture9*>::iterator it = m_texCache.begin();
         it != m_texCache.end(); ++it)
    {
        if (it->second) it->second->Release();
    }
    m_texCache.clear();

    m_levelName.clear();
    m_loaded        = false;
    m_drawCallsLast = 0;
    m_boundsValid   = false;
    m_hoveredEditorIdx  = -1;
    m_selectedEditorIdx = -1;
}

// ── Editor icon shapes ───────────────────────────────────────────────────────
// ── Case-insensitive substring match (VS2005 safe) ───────────────────────────
static bool eoContainsNoCase(const char* haystack, const char* needle)
{
    if (!needle || !needle[0]) return true;
    if (!haystack) return false;
    for (const char* h = haystack; *h; ++h)
    {
        const char* hp = h;
        const char* np = needle;
        while (*hp && *np && ((*hp >= 'A' && *hp <= 'Z') ? (*hp + 32) : *hp) ==
                             ((*np >= 'A' && *np <= 'Z') ? (*np + 32) : *np))
        { ++hp; ++np; }
        if (!*np) return true;
    }
    return false;
}

enum EditorIconShape {
    ICON_DIAMOND = 0,   // default octahedron
    ICON_CAMERA,        // frustum/pyramid
    ICON_ARROW_UP,      // upward arrow (archer)
    ICON_TURRET,        // crenellated tower (fort)
    ICON_STARBURST,     // radiating lines (emitter/FX)
    ICON_CROSSHAIR,     // crosshair (AI)
    ICON_FLAG,          // flag on pole (capture/spawn)
    ICON_SOUND,         // concentric arcs (sound)
    ICON_SUN,           // sun rays (light)
    ICON_SKULL,         // X cross (death zone)
    ICON_CUBE,          // wireframe cube (invisible/trigger)
};

// Returns icon shape for every editor object type.
// Every type now gets a distinctive icon instead of the default diamond.
// CASE-INSENSITIVE — conquest_strings.txt has mixed case: "CapturePoint", "SoundEmitter", "spawn_point"
static int GetXsiIconShape(const std::string& t)
{
    if (t.empty()) return ICON_DIAMOND;
    // FED types (always FED_ prefix, case consistent)
    if (t.find("FED_camera")     == 0) return ICON_CAMERA;
    if (t.find("FED_nodearcher") == 0) return ICON_ARROW_UP;
    if (t.find("FED_nodefort")   == 0) return ICON_TURRET;
    if (t.find("FED_emitter")    == 0) return ICON_STARBURST;
    if (t.find("FED_")           == 0) return ICON_STARBURST;
    // Use case-insensitive matching for all other types
    // Spawns + waypoints → crosshair
    if (eoContainsNoCase(t.c_str(), "spawn"))     return ICON_CROSSHAIR;
    // Paths → crosshair
    if (eoContainsNoCase(t.c_str(), "PathNode"))  return ICON_CROSSHAIR;
    if (eoContainsNoCase(t.c_str(), "PathLink"))  return ICON_CROSSHAIR;
    if (eoContainsNoCase(t.c_str(), "PathNetwork")) return ICON_CROSSHAIR;
    if (eoContainsNoCase(t.c_str(), "Spline"))    return ICON_CROSSHAIR;
    // Capture/control points → flag
    if (eoContainsNoCase(t.c_str(), "capture"))   return ICON_FLAG;
    if (eoContainsNoCase(t.c_str(), "control"))   return ICON_FLAG;
    // Sound → speaker arcs
    if (eoContainsNoCase(t.c_str(), "sound"))     return ICON_SOUND;
    if (eoContainsNoCase(t.c_str(), "Music"))     return ICON_SOUND;
    if (eoContainsNoCase(t.c_str(), "VoiceOver")) return ICON_SOUND;
    if (eoContainsNoCase(t.c_str(), "Rumble"))    return ICON_SOUND;
    // Lights → sun
    if (eoContainsNoCase(t.c_str(), "light"))     return ICON_SUN;
    // Death/kill zones → X
    if (eoContainsNoCase(t.c_str(), "dead"))      return ICON_SKULL;
    if (eoContainsNoCase(t.c_str(), "death"))     return ICON_SKULL;
    if (eoContainsNoCase(t.c_str(), "Kill"))      return ICON_SKULL;
    // Triggers → cube
    if (eoContainsNoCase(t.c_str(), "trigger"))   return ICON_CUBE;
    if (eoContainsNoCase(t.c_str(), "invisible")) return ICON_CUBE;
    if (eoContainsNoCase(t.c_str(), "occluder"))  return ICON_CUBE;
    if (eoContainsNoCase(t.c_str(), "world_bound")) return ICON_CUBE;
    // AI → arrow
    if (eoContainsNoCase(t.c_str(), "ai_"))       return ICON_ARROW_UP;
    if (eoContainsNoCase(t.c_str(), "AIGoal"))    return ICON_ARROW_UP;
    if (eoContainsNoCase(t.c_str(), "aigoal"))    return ICON_ARROW_UP;
    // Cameras (all types)
    if (eoContainsNoCase(t.c_str(), "camera") || eoContainsNoCase(t.c_str(), "Cam")) return ICON_CAMERA;
    // Effects/particle → starburst
    if (eoContainsNoCase(t.c_str(), "Effect"))    return ICON_STARBURST;
    if (eoContainsNoCase(t.c_str(), "particle"))  return ICON_STARBURST;
    // Logic → diamond (the default, but explicit for clarity)
    if (eoContainsNoCase(t.c_str(), "logic_"))    return ICON_DIAMOND;
    return ICON_DIAMOND;
}

// Type-based icon size multiplier — case-insensitive
static float GetEditorIconScale(const std::string& t)
{
    const char* s = t.c_str();
    if (eoContainsNoCase(s, "capture") || eoContainsNoCase(s, "CapturePoint")) return 1.5f;
    if (eoContainsNoCase(s, "spawn"))    return 1.0f;
    if (t.compare(0, 4, "FED_") == 0)   return 0.8f;
    if (eoContainsNoCase(s, "trigger"))  return 1.0f;
    if (eoContainsNoCase(s, "light"))    return 0.8f;
    if (eoContainsNoCase(s, "sound") || eoContainsNoCase(s, "Music")) return 0.7f;
    if (eoContainsNoCase(s, "camera") || eoContainsNoCase(s, "Cam")) return 0.6f;
    if (eoContainsNoCase(s, "ai_") || eoContainsNoCase(s, "AIGoal")) return 0.5f;
    if (eoContainsNoCase(s, "path") || eoContainsNoCase(s, "Spline")) return 0.4f;
    if (eoContainsNoCase(s, "dead") || eoContainsNoCase(s, "death") || eoContainsNoCase(s, "Kill")) return 0.6f;
    if (eoContainsNoCase(s, "Effect") || eoContainsNoCase(s, "particle")) return 0.7f;
    return 0.8f;
}

// Maps type_name prefix → icon label + fixed ARGB color.
// Returns false if the type should be skipped (no marker drawn).
static bool EditorGetTypeInfo(const std::string& t, char labelBuf[8], DWORD& colorOut)
{
    struct Entry { const char* prefix; const char* lbl; DWORD col; };
    static const Entry kTable[] = {
        {"spawn_point",    "SP",  0xFF22FF22},
        {"spawn",          "SP",  0xFF22FF22},
        {"capture_point",  "CP",  0xFFFFCC00},
        {"control_point",  "CP",  0xFFFFCC00},
        {"capture",        "CP",  0xFFFFCC00},
        {"FED_camera",     "CAM", 0xFF00EEFF},
        {"FED_emitter",    "EM",  0xFFFF8800},
        {"FED_nodearcher", "ARCH",0xFFFF6699},
        {"FED_nodefort",   "FORT",0xFFAA88FF},
        {"FED_node",       "ND",  0xFFCC44FF},
        {"trigger",        "TRG", 0xFFFF2222},
        {"camera",         "CAM", 0xFF00CCFF},
        {"ai_",            "AI",  0xFFAAFF00},
        {"dead",           "DZ",  0xFF882222},
        {"death",          "DZ",  0xFF882222},
        {"particle",       "FX",  0xFFFFAA44},
        {"sound",          "SFX", 0xFF44AAFF},
        {"light",          "LT",  0xFFFFFF88},
        {"invisible",      "INV", 0xFF888888},
        {"static_object",  NULL,  0},
        {NULL, NULL, 0}
    };
    colorOut = 0;
    for (int i = 0; kTable[i].prefix; ++i)
    {
        // Case-insensitive prefix match — conquest_strings has mixed case
        if (t.find(kTable[i].prefix) == 0 || eoContainsNoCase(t.c_str(), kTable[i].prefix))
        {
            if (!kTable[i].lbl) return false;
            strncpy(labelBuf, kTable[i].lbl, 7); labelBuf[7] = 0;
            colorOut = kTable[i].col;
            return true;
        }
    }
    strncpy(labelBuf, t.c_str(), 3); labelBuf[3] = 0;
    return true;
}

// ── Editor object category classification ────────────────────────────────────
// Maps type string prefix → category bit for the toolbar filter.
// Bit assignments:
//   0x0001 = Spawns     0x0002 = Capture    0x0004 = Cameras
//   0x0008 = Triggers   0x0010 = AI         0x0020 = FED Nodes
//   0x0040 = Deaths     0x0080 = Effects    0x0100 = Sound
//   0x0200 = Lights     0x0400 = Invisible  0x0800 = Paths
//   0x1000 = Logic/Other
static unsigned int EditorObjCategoryBit(const std::string& t)
{
    // Case-insensitive — conquest_strings.txt has mixed case
    const char* s = t.c_str();
    if (eoContainsNoCase(s, "spawn"))     return 0x0001u;
    if (eoContainsNoCase(s, "capture") || eoContainsNoCase(s, "control_area")) return 0x0002u;
    if (eoContainsNoCase(s, "camera") || eoContainsNoCase(s, "Cam")) return 0x0004u;
    if (eoContainsNoCase(s, "trigger"))   return 0x0008u;
    if (eoContainsNoCase(s, "ai_") || eoContainsNoCase(s, "AIGoal") || eoContainsNoCase(s, "aigoal")) return 0x0010u;
    if (t.compare(0, 4, "FED_") == 0)    return 0x0020u; // FED_ prefix is always consistent
    if (eoContainsNoCase(s, "dead") || eoContainsNoCase(s, "death") || eoContainsNoCase(s, "Kill")) return 0x0040u;
    if (eoContainsNoCase(s, "Effect") || eoContainsNoCase(s, "particle")) return 0x0080u;
    if (eoContainsNoCase(s, "sound") || eoContainsNoCase(s, "Music") || eoContainsNoCase(s, "VoiceOver") || eoContainsNoCase(s, "Rumble")) return 0x0100u;
    if (eoContainsNoCase(s, "light"))     return 0x0200u;
    if (eoContainsNoCase(s, "invisible") || eoContainsNoCase(s, "occluder")) return 0x0400u;
    if (eoContainsNoCase(s, "path") || eoContainsNoCase(s, "Path") || eoContainsNoCase(s, "Spline")) return 0x0800u;
    return 0x1000u; // logic/other
}

// ── shouldDrawEditorObj() ─────────────────────────────────────────────────────
// Centralized filter for editor objects. Returns 0.0 = skip, 1.0 = draw,
// 0.0-1.0 = fade zone. Also resolves label text and color.
// ALL filtering logic lives here — relation, gamemode, distance, category, search.
// The three render loops just call this and skip if <= 0.

float LevelScene::shouldDrawEditorObj(int idx, const float camPos[3],
                                       char lbl[8], unsigned long& fixedCol) const
{
    const LevelEditorObj& eo = m_editorObjs[idx];
    lbl[0] = '\0';
    fixedCol = 0;

    // Relation filter: skip objects not in the filter set
    if (!m_editorObjFilter.empty() &&
        m_editorObjFilter.find(eo.guid) == m_editorObjFilter.end())
        return 0.0f;

    // Phase 3 XSI layer manager: hidden / non-isolated layers vanish
    // entirely from F4 mode. layerGuid==0 is "no layer", always shown
    // unless explicitly tracked elsewhere.
    if (eo.layerGuid != 0 && !layerVisibleForRender(eo.layerGuid))
        return 0.0f;

    // Gamemode filter: skip objects that don't match current mode
    if (m_gmfBitMask != 0xFFFFFFFF &&
        ((eo.gameModeMask == -1 && !m_gmfShowGlobals) ||
         (eo.gameModeMask == 0 && !m_gmfShowScripts) ||
         (eo.gameModeMask != -1 && eo.gameModeMask != 0 && !((unsigned int)eo.gameModeMask & m_gmfBitMask))))
        return 0.0f;

    // Type info: skip static_object etc., resolve label + color
    if (!EditorGetTypeInfo(eo.type, lbl, fixedCol))
        return 0.0f;

    // Distance cull with fade (Phase 1)
    if (m_editorObjMaxDist > 0.0f)
    {
        float dx = eo.mat[12] - camPos[0];
        float dy = eo.mat[13] - camPos[1];
        float dz = eo.mat[14] - camPos[2];
        float distSq = dx*dx + dy*dy + dz*dz;

        // Min distance exclusion zone
        if (m_editorObjMinDist > 0.0f && distSq < m_editorObjMinDist * m_editorObjMinDist)
            return 0.0f;

        float maxDistSq = m_editorObjMaxDist * m_editorObjMaxDist;
        if (distSq > maxDistSq)
            return 0.0f;

        // Fade zone: between fadeStart and maxDist, lerp from 1.0 to 0.0
        if (m_editorObjFadeStart > 0.0f)
        {
            float fadeStartSq = m_editorObjFadeStart * m_editorObjFadeStart;
            if (distSq > fadeStartSq)
            {
                float dist = sqrtf(distSq);
                float t = (dist - m_editorObjFadeStart) / (m_editorObjMaxDist - m_editorObjFadeStart);
                if (t >= 1.0f) return 0.0f;
                return 1.0f - t;
            }
        }
    }

    // Category filter (Phase 2)
    if (m_editorObjCategoryMask != 0x1FFFu)
    {
        if (!(EditorObjCategoryBit(eo.type) & m_editorObjCategoryMask))
            return 0.0f;
    }

    // Search filter (Phase 3)
    if (m_editorObjSearchTerm[0] != '\0')
    {
        switch (m_editorObjSearchMode)
        {
        case 1: // type: prefix — match type string only
            if (!eoContainsNoCase(eo.type.c_str(), m_editorObjSearchTerm))
                return 0.0f;
            break;
        case 2: // layer: prefix — match name for now (layerGuid lookup is Phase 5)
            if (!eoContainsNoCase(eo.name.c_str(), m_editorObjSearchTerm))
                return 0.0f;
            break;
        default: // plain substring — match name OR type
            if (!eoContainsNoCase(eo.name.c_str(), m_editorObjSearchTerm) &&
                !eoContainsNoCase(eo.type.c_str(), m_editorObjSearchTerm))
                return 0.0f;
            break;
        }
    }

    return 1.0f;
}

// ── Forward decls for the Phase 4 gizmo helpers ──
// The full bodies live at the bottom of this file next to the gizmo
// state-machine methods. render() needs gizmoGetAxes + gizmoSizeAt to
// draw the manipulator on top of the rest of the scene, so we
// pre-announce them here. The drag-math methods further down are the
// only callers of the rest, so those don't need forward decls.
static void  gizmoGetAxes(const float mat[16], int space,
                          float outX[3], float outY[3], float outZ[3]);
static float gizmoSizeAt(const float origin[3], const float camPos[3]);

// ── render() ──────────────────────────────────────────────────────────────────

void LevelScene::render()
{
    if (!m_device || !m_loaded || m_instances.empty()) return;

    // ── Crowd Mesh Builder preview matrix update ────────────────────────────
    // Walk every instance flagged as a preview child. Recompute its world
    // matrix as: parent.mat × authoritativeBindWorld[boneIdx] × offset.
    // CRITICAL: Use the AUTHORITATIVE bind from skin_binds, not the cascade
    // product. The cascade drifts from Pandemic's authority by 10+ degrees
    // on some bones (verified: Bone_LHand off by ~10deg + 22cm). Lying
    // preview = trial-and-error tuning hell for the user. With this fix
    // the preview matches what the Python bake (level_patcher.py) will
    // actually produce after Save Level + repack + reload.
    //
    // Done here at the top of render() so both shadow and main pass see
    // the updated matrices. Cost: linear in m_instances but only a tiny
    // fraction are previews (0-2 normally), so it's negligible.
    for (size_t pi = 0; pi < m_instances.size(); ++pi)
    {
        LevelInstance& child = m_instances[pi];
        if (child.previewParentIdx < 0) continue;
        if (child.previewParentIdx >= (int)m_instances.size()) continue;
        const LevelInstance& parent = m_instances[child.previewParentIdx];
        if (!parent.model) continue;
        int boneIdx = child.previewParentBoneIdx;
        if (boneIdx < 0) continue;

        // Pull the authoritative bind matrix. Falls back to restBoneWorld
        // for bones outside the skin palette; identity if neither has it.
        float boneMat[16];
        if (!parent.model->authoritativeBindWorld(boneIdx, boneMat))
        {
            // Bone unknown to both palette and cascade. Preview would
            // render at identity, which puts the weapon at parent origin.
            // Skip this child to avoid a misleading display.
            continue;
        }

        // Build the offset transform from XYZ translation + yaw/pitch/roll.
        // YPR uses ZYX intrinsic order (yaw=Y axis, pitch=X axis, roll=Z axis).
        float yaw   = child.previewOffset[3];
        float pitch = child.previewOffset[4];
        float roll  = child.previewOffset[5];
        float cy = cosf(yaw),   sy = sinf(yaw);
        float cp = cosf(pitch), sp = sinf(pitch);
        float cr = cosf(roll),  sr = sinf(roll);
        float offset[16] = {
            cy*cr + sy*sp*sr,   cp*sr,   -sy*cr + cy*sp*sr,   0.0f,
            -cy*sr + sy*sp*cr,  cp*cr,    sy*sr + cy*sp*cr,   0.0f,
             sy*cp,            -sp,       cy*cp,              0.0f,
             child.previewOffset[0], child.previewOffset[1], child.previewOffset[2], 1.0f,
        };

        // child.mat = parent.mat × boneMat × offset
        // Multiplication order is row-major: (A × B)[r][c] = sum A[r][k] B[k][c].
        float t0[16], t1[16];
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += parent.mat[r*4+k] * boneMat[k*4+c];
            t0[r*4+c] = s;
        }
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += t0[r*4+k] * offset[k*4+c];
            t1[r*4+c] = s;
        }
        memcpy(child.mat, t1, sizeof(t1));
    }

    // Lazy-init shaders on first render
    if (!m_shaderInitAttempted) initShaders();

    // One-time render log
    if (!s_renderLogOnce)
    {
        s_renderLogOnce = true;
        LevelLogf("[Render] First frame: %d instances, %d models, %d textures cached",
                  (int)m_instances.size(), (int)m_modelCache.size(), (int)m_texCache.size());
        LevelLogf("[Render] Shaders ready=%d, bounds valid=%d", m_shadersReady ? 1 : 0, m_boundsValid ? 1 : 0);
        if (m_boundsValid)
            LevelLogf("[Render] Bounds: min=(%.1f, %.1f, %.1f) max=(%.1f, %.1f, %.1f)",
                      m_boundsMin[0], m_boundsMin[1], m_boundsMin[2],
                      m_boundsMax[0], m_boundsMax[1], m_boundsMax[2]);
        LevelLogf("[Render] Using: %s, alpha test, 8x AF",
                  m_shadersReady ? "game-accurate shaders (Lambert+AO+Rayleigh/Mie scattering)" : "fixed-function fallback");

        // Count textured vs untextured parts
        int totalParts = 0, texParts = 0;
        for (std::map<uint32_t, LevelModel*>::iterator it = m_modelCache.begin(); it != m_modelCache.end(); ++it)
        {
            if (!it->second) continue;
            for (int p = 0; p < (int)it->second->parts.size(); ++p)
            {
                ++totalParts;
                if (it->second->parts[p].diffuseTex) ++texParts;
            }
        }
        LevelLogf("[Render] Mesh parts: %d total, %d textured, %d untextured",
                  totalParts, texParts, totalParts - texParts);
    }

    // Save existing state
    IDirect3DVertexShader9* sVS = NULL;
    IDirect3DPixelShader9*  sPS = NULL;
    m_device->GetVertexShader(&sVS);
    m_device->GetPixelShader(&sPS);

    DWORD sLighting, sCull, sZEnable, sZWrite, sAlpha, sColWrite, sFVF;
    DWORD sAlphaTest, sAlphaRef, sAlphaFunc, sFog;
    DWORD sFill = D3DFILL_SOLID, sSrcBlend = D3DBLEND_SRCALPHA, sDestBlend = D3DBLEND_INVSRCALPHA;
    DWORD sSpecular = FALSE, sNormalize = FALSE;
    DWORD sAmbient = 0, sTextureFactor = 0xFFFFFFFF, sColorVertex = TRUE;
    DWORD sDiffuseMaterialSource = D3DMCS_COLOR1, sSrgbWrite = FALSE;
    enum { kSavedLevelTextureSlots = 11 }; // Level shader path uses slots 0..10.
    IDirect3DBaseTexture9* sTex[kSavedLevelTextureSlots];
    memset(sTex, 0, sizeof(sTex));
    static const D3DTEXTURESTAGESTATETYPE kSavedTss[] = {
        D3DTSS_COLOROP,
        D3DTSS_COLORARG1,
        D3DTSS_COLORARG2,
        D3DTSS_ALPHAOP,
        D3DTSS_ALPHAARG1,
        D3DTSS_ALPHAARG2
    };
    DWORD sTss[2][sizeof(kSavedTss) / sizeof(kSavedTss[0])];
    memset(sTss, 0, sizeof(sTss));
    static const D3DSAMPLERSTATETYPE kSavedSamp[] = {
        D3DSAMP_ADDRESSU,
        D3DSAMP_ADDRESSV,
        D3DSAMP_MAGFILTER,
        D3DSAMP_MINFILTER,
        D3DSAMP_MIPFILTER,
        D3DSAMP_MAXANISOTROPY,
        D3DSAMP_MIPMAPLODBIAS,
        D3DSAMP_SRGBTEXTURE
    };
    DWORD sSamp[kSavedLevelTextureSlots][sizeof(kSavedSamp) / sizeof(kSavedSamp[0])];
    memset(sSamp, 0, sizeof(sSamp));
    IDirect3DVertexDeclaration9* sDecl = NULL;
    D3DMATRIX sWorld, sView, sProj;
    memset(&sWorld, 0, sizeof(sWorld));
    memset(&sView, 0, sizeof(sView));
    memset(&sProj, 0, sizeof(sProj));
    D3DVIEWPORT9 sViewport;
    memset(&sViewport, 0, sizeof(sViewport));
    bool sHaveViewport = SUCCEEDED(m_device->GetViewport(&sViewport));
    D3DMATERIAL9 sMaterial;
    memset(&sMaterial, 0, sizeof(sMaterial));
    bool sHaveMaterial = SUCCEEDED(m_device->GetMaterial(&sMaterial));
    BOOL sLightEnabled[3] = { FALSE, FALSE, FALSE };
    D3DLIGHT9 sLights[3];
    bool sHaveLight[3] = { false, false, false };
    memset(sLights, 0, sizeof(sLights));
    m_device->GetRenderState(D3DRS_LIGHTING,         &sLighting);
    m_device->GetRenderState(D3DRS_CULLMODE,         &sCull);
    m_device->GetRenderState(D3DRS_ZENABLE,          &sZEnable);
    m_device->GetRenderState(D3DRS_ZWRITEENABLE,     &sZWrite);
    m_device->GetRenderState(D3DRS_ALPHABLENDENABLE, &sAlpha);
    m_device->GetRenderState(D3DRS_COLORWRITEENABLE, &sColWrite);
    m_device->GetRenderState(D3DRS_ALPHATESTENABLE,  &sAlphaTest);
    m_device->GetRenderState(D3DRS_ALPHAREF,         &sAlphaRef);
    m_device->GetRenderState(D3DRS_ALPHAFUNC,        &sAlphaFunc);
    m_device->GetRenderState(D3DRS_FOGENABLE,        &sFog);
    m_device->GetRenderState(D3DRS_FILLMODE,         &sFill);
    m_device->GetRenderState(D3DRS_SRCBLEND,         &sSrcBlend);
    m_device->GetRenderState(D3DRS_DESTBLEND,        &sDestBlend);
    m_device->GetRenderState(D3DRS_SPECULARENABLE,   &sSpecular);
    m_device->GetRenderState(D3DRS_NORMALIZENORMALS, &sNormalize);
    m_device->GetRenderState(D3DRS_AMBIENT,          &sAmbient);
    m_device->GetRenderState(D3DRS_TEXTUREFACTOR,    &sTextureFactor);
    m_device->GetRenderState(D3DRS_COLORVERTEX,      &sColorVertex);
    m_device->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &sDiffuseMaterialSource);
    m_device->GetRenderState(D3DRS_SRGBWRITEENABLE,  &sSrgbWrite);
    m_device->GetTransform(D3DTS_WORLD,      &sWorld);
    m_device->GetTransform(D3DTS_VIEW,       &sView);
    m_device->GetTransform(D3DTS_PROJECTION, &sProj);
    for (int li = 0; li < 3; ++li) {
        m_device->GetLightEnable(li, &sLightEnabled[li]);
        sHaveLight[li] = SUCCEEDED(m_device->GetLight(li, &sLights[li]));
    }
    m_device->GetFVF(&sFVF);
    for (int si = 0; si < kSavedLevelTextureSlots; ++si)
        m_device->GetTexture(si, &sTex[si]);
    for (int si = 0; si < kSavedLevelTextureSlots; ++si)
        for (int sa = 0; sa < (int)(sizeof(kSavedSamp) / sizeof(kSavedSamp[0])); ++sa)
            m_device->GetSamplerState(si, kSavedSamp[sa], &sSamp[si][sa]);
    for (int stage = 0; stage < 2; ++stage)
        for (int ti = 0; ti < (int)(sizeof(kSavedTss) / sizeof(kSavedTss[0])); ++ti)
            m_device->GetTextureStageState(stage, kSavedTss[ti], &sTss[stage][ti]);
    m_device->GetVertexDeclaration(&sDecl);

    const DWORD kRgbWrite =
        D3DCOLORWRITEENABLE_RED |
        D3DCOLORWRITEENABLE_GREEN |
        D3DCOLORWRITEENABLE_BLUE;
    DWORD levelColorWrite = sColWrite & (kRgbWrite | D3DCOLORWRITEENABLE_ALPHA);
    if ((levelColorWrite & kRgbWrite) == 0)
        levelColorWrite = kRgbWrite | D3DCOLORWRITEENABLE_ALPHA;

    // ── Shadow map pass ──────────────────────────────────────────────────────
    if (m_shadowsReady) {
        // Build light orthographic projection covering the scene
        float cx = (m_boundsMin[0] + m_boundsMax[0]) * 0.5f;
        float cy = (m_boundsMin[1] + m_boundsMax[1]) * 0.5f;
        float cz = (m_boundsMin[2] + m_boundsMax[2]) * 0.5f;
        float ex = (m_boundsMax[0] - m_boundsMin[0]) * 0.5f + 10.0f;
        float ey = (m_boundsMax[1] - m_boundsMin[1]) * 0.5f + 10.0f;
        float ez = (m_boundsMax[2] - m_boundsMin[2]) * 0.5f + 10.0f;
        float span = ex > ez ? ex : ez; if (ey > span) span = ey;

        // Light view: look from above along sun direction
        float* ld = m_lighting.sunDir;
        float lEye[3] = { cx - ld[0]*span, cy - ld[1]*span, cz - ld[2]*span };
        float lAt[3] = { cx, cy, cz };

        // Build view matrix (lookAt)
        float fwd[3] = { lAt[0]-lEye[0], lAt[1]-lEye[1], lAt[2]-lEye[2] };
        float flen = sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
        if (flen > 0.001f) { fwd[0]/=flen; fwd[1]/=flen; fwd[2]/=flen; }
        float up[3] = { 0,1,0 };
        if (fabsf(fwd[1]) > 0.99f) { up[0]=1; up[1]=0; up[2]=0; }
        float rt[3] = { fwd[1]*up[2]-fwd[2]*up[1], fwd[2]*up[0]-fwd[0]*up[2], fwd[0]*up[1]-fwd[1]*up[0] };
        float rlen = sqrtf(rt[0]*rt[0]+rt[1]*rt[1]+rt[2]*rt[2]);
        if (rlen > 0.001f) { rt[0]/=rlen; rt[1]/=rlen; rt[2]/=rlen; }
        float u2[3] = { rt[1]*fwd[2]-rt[2]*fwd[1], rt[2]*fwd[0]-rt[0]*fwd[2], rt[0]*fwd[1]-rt[1]*fwd[0] };

        float lView[16] = {
            rt[0], u2[0], fwd[0], 0,
            rt[1], u2[1], fwd[1], 0,
            rt[2], u2[2], fwd[2], 0,
            -(rt[0]*lEye[0]+rt[1]*lEye[1]+rt[2]*lEye[2]),
            -(u2[0]*lEye[0]+u2[1]*lEye[1]+u2[2]*lEye[2]),
            -(fwd[0]*lEye[0]+fwd[1]*lEye[1]+fwd[2]*lEye[2]),
            1
        };
        // Orthographic projection
        float lProj[16];
        memset(lProj, 0, sizeof(lProj));
        lProj[0] = 1.0f / span;
        lProj[5] = 1.0f / span;
        lProj[10] = 1.0f / (span * 2.0f);
        lProj[15] = 1.0f;

        // LightVP = View * Proj (row-major)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += lView[r*4+k] * lProj[k*4+c];
                m_lightVP[r*4+c] = s;
            }

        // Render to shadow map
        IDirect3DSurface9* oldRT = NULL; IDirect3DSurface9* oldDS = NULL;
        m_device->GetRenderTarget(0, &oldRT);
        m_device->GetDepthStencilSurface(&oldDS);
        m_device->SetTexture(3, NULL);
        m_device->SetTexture(5, NULL);
        m_device->SetRenderTarget(0, m_shadowMapSurf);
        m_device->SetDepthStencilSurface(m_shadowMapDS);
        D3DVIEWPORT9 shadowVP = { 0, 0, (DWORD)SHADOW_MAP_SIZE, (DWORD)SHADOW_MAP_SIZE, 0.0f, 1.0f };
        m_device->SetViewport(&shadowVP);
        m_device->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);

        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        m_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        m_device->SetVertexShader(m_shadowVS);
        m_device->SetPixelShader(m_shadowPS);
        m_device->SetVertexDeclaration(m_levelDecl);
        m_device->SetTexture(0, NULL);
        m_device->SetVertexShaderConstantF(4, m_lightVP, 4); // LightVP at c4-c7

        for (int instIdx = 0; instIdx < (int)m_instances.size(); ++instIdx) {
            const LevelInstance& inst = m_instances[instIdx];
            if (!inst.model || inst.model->parts.empty()) continue;
            m_device->SetVertexShaderConstantF(0, inst.mat, 4); // World at c0-c3
            for (int pi = 0; pi < (int)inst.model->parts.size(); ++pi) {
                const LevelMeshPart& part = inst.model->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                uint32_t pf = GetPartFlags(inst.model, pi);
                if (pf & 16) continue; // hidden occluder volumes should not cast visible shadows
                if (pf & 5) continue;  // transparent/water parts need alpha-aware shadows; skip opaque silhouettes
                m_device->SetStreamSource(0, part.vb, 0, LEVEL_VERTEX_SIZE);
                m_device->SetIndices(part.ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                    0, 0, part.vertexCount, 0, part.indexCount / 3);
            }
        }

        // Restore render target
        m_device->SetRenderTarget(0, oldRT);
        m_device->SetDepthStencilSurface(oldDS);
        if (sHaveViewport) m_device->SetViewport(&sViewport);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
    }

    // Common render state
    m_device->SetRenderState(D3DRS_ZENABLE,          TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE,         D3DCULL_CW);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE, levelColorWrite);

    // Alpha test OFF for level geometry — most textures store specular/gloss
    // in alpha, not cutout masks.  Alpha test at 128 punches holes everywhere.
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);

    // Sampler state (shared by both paths). MINFILTER and MAXANISOTROPY
    // come from the cached device cap query because Intel UHD reports
    // MaxAnisotropy clamps lower than 8 and on some driver builds also
    // refuses ANISOTROPIC entirely. Hardcoded values here used to leave
    // those rigs running POINT-sampled, which is why every texture in
    // Helm's Deep looked like wet cardboard on the tester's laptop.
    {
        const GpuFilterCaps& fc = GetGpuFilterCaps(m_device);
        m_device->SetSamplerState(0, D3DSAMP_MINFILTER,     fc.anisoFilter);
        m_device->SetSamplerState(0, D3DSAMP_MAGFILTER,     D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_MIPFILTER,     D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, fc.maxAnisotropy);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSU,      D3DTADDRESS_WRAP);
        m_device->SetSamplerState(0, D3DSAMP_ADDRESSV,      D3DTADDRESS_WRAP);
    }

    int calls = 0;
    IDirect3DTexture9* curTex = NULL;

    // Post-processing state (declared outside shader block for scope)
    IDirect3DSurface9* postOldRT = NULL;
    IDirect3DSurface9* postOldDS = NULL;
    bool usingPostRT = false;

    // ── Shader-based rendering (game-accurate pipeline) ──
    // Falls back to fixed-function if shaders failed to compile.
    if (m_shadersReady)
    {
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        // Retrieve view+proj from D3D9
        D3DMATRIX d3dView, d3dProj;
        m_device->GetTransform(D3DTS_VIEW,       &d3dView);
        m_device->GetTransform(D3DTS_PROJECTION, &d3dProj);

        ZMat4x4 zView, zProj, zViewProj;
        zView.load((const float*)&d3dView);
        zProj.load((const float*)&d3dProj);
        ZMatMul(&zViewProj, &zView, &zProj);
        memcpy(m_cachedVP, zViewProj.f, sizeof(float) * 16);

        // Camera world position
        float camPos[4];
        {
            const float* v = zView.f;
            float tx = v[12], ty = v[13], tz = v[14];
            camPos[0] = -ZDot3f(v[0], v[1], v[2],   tx, ty, tz);
            camPos[1] = -ZDot3f(v[4], v[5], v[6],   tx, ty, tz);
            camPos[2] = -ZDot3f(v[8], v[9], v[10],  tx, ty, tz);
            camPos[3] = 1.0f;
        }
        m_cachedCamPos[0] = camPos[0];
        m_cachedCamPos[1] = camPos[1];
        m_cachedCamPos[2] = camPos[2];

        // Per-material game PS selection: pick bridge VS + game PS based on textures
        // Custom HLSL shaders only — game shaders cause visual issues without full HDR pipeline
        bool useGameShaders = false;
        (void)m_gameShaders; (void)m_bridgeVS; (void)m_gamePS_A;

        if (useGameShaders) {
            // ── Game PS path — set shared constants once, per-part PS switching in macro ──
            m_device->SetVertexShader(m_bridgeVS);  // default A bridge, switched per-part
            m_device->SetPixelShader(m_gamePS_A);    // default A PS, switched per-part
            m_device->SetVertexDeclaration(m_levelDecl);

            // VS shared constants (same for both bridge VS variants)
            m_device->SetVertexShaderConstantF(239, zViewProj.f, 4);   // c239-c242 = ViewProj
            m_device->SetVertexShaderConstantF(245, camPos, 1);         // c245 = cameraPos
            // Inscatter for per-vertex atmosphere (bridge VS computes fog)
            // inscatterCol already has the multiplier baked in from extraction
            float inscatRGB[4] = {
                m_lighting.inscatterCol[0] * m_lighting.inscatterCol[3],
                m_lighting.inscatterCol[1] * m_lighting.inscatterCol[3],
                m_lighting.inscatterCol[2] * m_lighting.inscatterCol[3],
                1.0f
            };
            m_device->SetVertexShaderConstantF(246, inscatRGB, 1);     // c246 = inscatter color * mult
            // Fog density — boost from extracted value for visible atmosphere
            // Extracted: 0.00194, but this is too subtle for per-vertex fog
            // At 200 units with 0.004: fog = 1-exp(-200*0.004) = 0.55 = good mid-range
            float fogDensity = m_lighting.scatterParams[3] * 2.0f;
            if (fogDensity < 0.003f) fogDensity = 0.003f; // minimum for visible haze
            float scatP[4] = { fogDensity, m_lighting.drawDist, 0, 0 };
            m_device->SetVertexShaderConstantF(247, scatP, 1);         // c247 = density, maxDist
            m_device->SetVertexShaderConstantF(250, m_lighting.sunCol, 1); // c250 = sun color
            m_device->SetVertexShaderConstantF(251, m_lighting.sunDir, 1); // c251 = sun dir

            // PS: game register layout (from F9 capture + disassembly)
            m_device->SetPixelShaderConstantF(1,  m_lighting.ambient,    1);  // c1 = g__ambient
            m_device->SetPixelShaderConstantF(2,  m_lighting.sunCol,     1);  // c2 = g__sunCol
            m_device->SetPixelShaderConstantF(3,  m_lighting.sunDir,     1);  // c3 = g__sunDir
            m_device->SetPixelShaderConstantF(5,  m_lighting.diffCol,    1);  // c5 = g__diffColor
            m_device->SetPixelShaderConstantF(10, camPos,                1);  // c10 = g__cameraPos

            // c200 = g__ambientOcclusionParms: PS computes ao = sat(NORMAL.w * c200.x + c200.y)
            // Captured: most common = (1.0, 0.25, ...) which biases AO up by 25%,
            // preventing completely black shadows. (0.125,0) variant is too subtle.
            float aoParms[4] = { 1.0f, 0.25f, 0.0f, 0.0f };
            m_device->SetPixelShaderConstantF(200, aoParms, 1);

            // c11 = g__cameraFront — camera forward direction in world space
            // Used by PS for shadow cascade selection: dot(camFront, worldPos - camPos)
            float camFront[4] = { zView.f[2], zView.f[6], zView.f[10], 0.0f };
            m_device->SetPixelShaderConstantF(11, camFront, 1);

            // Shadow cascades (from F9 capture: c28, c30)
            // c28 = cascade distances (depth breaks for 4 cascades)
            // c30 = shadow misc: x=minShadow, y=farDist, z=maxDist, w=cascadeBlend
            if (m_shadowsReady) {
                m_device->SetPixelShaderConstantF(20, m_lightVP, 4);  // c20-c23 = shadow matrices
                float shadowDist[4] = { 0.1f, 6.0f, 13.0f, 30.0f };  // captured cascade breaks
                m_device->SetPixelShaderConstantF(28, shadowDist, 1);
                float shadowMisc[4] = { 0.1f, 30.0f, 100.0f, 4.0f };  // captured shadow misc
                m_device->SetPixelShaderConstantF(30, shadowMisc, 1);
                // c24-c27 = shadow scale/offset per cascade (identity = single cascade)
                float sso[16] = { 1,0,0,0, 0,1,0,0, 1,0,0,0, 0,1,0,0 };
                m_device->SetPixelShaderConstantF(24, sso, 4);
            } else {
                // No shadows: zero matrices + minShadow=1 → PS always returns fully lit
                float zeroMat[16]; memset(zeroMat, 0, sizeof(zeroMat));
                m_device->SetPixelShaderConstantF(20, zeroMat, 4);   // c20-c23 = zero
                m_device->SetPixelShaderConstantF(24, zeroMat, 4);   // c24-c27 = zero
                float noShadowDist[4] = { 0,0,0,0 };
                float noShadowMisc[4] = { 1.0f, 0, 0, 0 };  // c30.x=1 = minShadow fully lit
                m_device->SetPixelShaderConstantF(28, noShadowDist, 1);
                m_device->SetPixelShaderConstantF(30, noShadowMisc, 1);
            }

            // c72 = g__ambientCubeMapColor — tint for ambient cubemap sample
            // PS: amb = cubeSample * NdotN * c1.w * c72 + c1.rgb
            // Without actual cubemap texture, c72 just scales what s6 returns (black if no tex)
            float cubeCol[4] = {1,1,1,1};
            m_device->SetPixelShaderConstantF(72, cubeCol, 1);

            // c7 = g__emissiveColor, c8 = g__emissiveParms
            // PS: outCol = lerp(Ir, Ir*c7.w+c7, c8.z) → c8.z=0 disables emissive
            float emissiveColor[4] = {0,0,0,0};
            float emissiveParms[4] = {0,0,0,0};
            m_device->SetPixelShaderConstantF(7, emissiveColor, 1);
            m_device->SetPixelShaderConstantF(8, emissiveParms, 1);

            // c6 = g__specColor (Strauss specular color, default white)
            float specColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            m_device->SetPixelShaderConstantF(6, specColor, 1);

            // c66 = strauss params: x=smoothness, y=metalness, z=transparency, w=specPower
            float defaultStrauss[4] = { 0.0f, 0.167f, 0.25f, 128.0f };
            m_device->SetPixelShaderConstantF(66, defaultStrauss, 1);

            // s6 = ambient cubemap — game PS samples this for ambient fill lighting
            // PS: amb = texCube(s6, worldNormal) * c1.w * c72 + c1.rgb
            if (m_ambientCube) {
                m_device->SetTexture(6, m_ambientCube);
                m_device->SetSamplerState(6, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(6, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            }

        } else {
            // ── Custom shader fallback — our register layout ──
            m_device->SetVertexShader(m_levelVS);
            m_device->SetPixelShader(m_levelPS);
            m_device->SetVertexDeclaration(m_levelDecl);

            // VS c4-c7 = ViewProj (our layout)
            m_device->SetVertexShaderConstantF(4, zViewProj.f, 4);

            // PS constants — our layout (c0-c9)
            m_device->SetPixelShaderConstantF(0, m_lighting.ambient,       1);
            m_device->SetPixelShaderConstantF(1, m_lighting.sunCol,        1);
            m_device->SetPixelShaderConstantF(2, m_lighting.sunDir,        1);
            m_device->SetPixelShaderConstantF(3, m_lighting.diffCol,       1);
            m_device->SetPixelShaderConstantF(4, camPos,                   1);
            m_device->SetPixelShaderConstantF(5, m_lighting.scatterParams, 1);
            m_device->SetPixelShaderConstantF(6, m_lighting.inscatterCol,  1);
            m_device->SetPixelShaderConstantF(7, m_lighting.extinctCol,    1);
            m_device->SetPixelShaderConstantF(8, m_lighting.scatterHG,     1);
            m_device->SetPixelShaderConstantF(9, m_lighting.miscParams,    1);
        } // end custom shader fallback
        // Custom shader path uses different register layout for these constants
        // Game PS: c11=cameraFront, c20-c23=shadowMatrices, s5=shadowMap
        // Custom PS: c11=sun2Col, c20=topDome, c21=botDome, c13-c16=lightVP, s3=shadowMap
        if (!useGameShaders) {
            // Dome colors for hemisphere ambient (c20-c21)
            m_device->SetPixelShaderConstantF(20, m_lighting.topDomeCol, 1);
            m_device->SetPixelShaderConstantF(21, m_lighting.botDomeCol, 1);
            // Secondary light (c11-c12)
            float sun2ColW[4] = { m_lighting.sun2Col[0], m_lighting.sun2Col[1], m_lighting.sun2Col[2],
                                  m_lighting.hasSun2 ? 1.0f : 0.0f };
            m_device->SetPixelShaderConstantF(11, sun2ColW, 1);
            m_device->SetPixelShaderConstantF(12, m_lighting.sun2Dir, 1);
            // Shadow map (c13-c16 = lightVP, c17 = params, s3 = shadow tex)
            if (m_shadowsReady) {
                m_device->SetPixelShaderConstantF(13, m_lightVP, 4);
                float shadowParams[4] = { 1.0f, 0.002f, 1.0f / SHADOW_MAP_SIZE, 0.20f };
                m_device->SetPixelShaderConstantF(17, shadowParams, 1);
                m_device->SetTexture(3, m_shadowMapTex);
                m_device->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                m_device->SetSamplerState(3, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                m_device->SetSamplerState(3, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            } else {
                float shadowOff[4] = { 0, 0, 0, 1 };
                m_device->SetPixelShaderConstantF(17, shadowOff, 1);
            }
        }

        // ── Redirect to custom post render target ───────────────────────────
        if (m_postReady) {
            D3DVIEWPORT9 vp; m_device->GetViewport(&vp);

            m_device->GetRenderTarget(0, &postOldRT);
            m_device->GetDepthStencilSurface(&postOldDS);

            D3DFORMAT postFormat = D3DFMT_A8R8G8B8;
            D3DSURFACE_DESC copyDesc;
            memset(&copyDesc, 0, sizeof(copyDesc));
            bool haveCopyDesc = false;
            if (postOldRT) {
                if (SUCCEEDED(postOldRT->GetDesc(&copyDesc))) {
                    haveCopyDesc = true;
                    postFormat = copyDesc.Format;
                    if (postFormat != D3DFMT_A8R8G8B8 && postFormat != D3DFMT_X8R8G8B8)
                        postFormat = D3DFMT_A8R8G8B8;
                }
            }

            RECT srcRect;
            srcRect.left = (LONG)vp.X;
            srcRect.top = (LONG)vp.Y;
            srcRect.right = (LONG)(vp.X + vp.Width);
            srcRect.bottom = (LONG)(vp.Y + vp.Height);
            if (haveCopyDesc) {
                if (srcRect.left < 0) srcRect.left = 0;
                if (srcRect.top < 0) srcRect.top = 0;
                if (srcRect.right > (LONG)copyDesc.Width) srcRect.right = (LONG)copyDesc.Width;
                if (srcRect.bottom > (LONG)copyDesc.Height) srcRect.bottom = (LONG)copyDesc.Height;
            }
            int w = (int)(srcRect.right - srcRect.left);
            int h = (int)(srcRect.bottom - srcRect.top);
            if (w < 1) w = 1; if (h < 1) h = 1;

            bool recreatePostRT = (m_sceneRTWidth != w || m_sceneRTHeight != h);
            if (!m_sceneRT || !m_sceneRTSurf || !m_bloomRT || !m_bloomRTSurf || !m_bloomRT2 || !m_bloomRT2Surf)
                recreatePostRT = true;
            if (!recreatePostRT && m_sceneRTSurf) {
                D3DSURFACE_DESC sceneDesc;
                if (FAILED(m_sceneRTSurf->GetDesc(&sceneDesc)) || sceneDesc.Format != postFormat)
                    recreatePostRT = true;
            }

            if (recreatePostRT) {
                if (m_sceneRTSurf) { m_sceneRTSurf->Release(); m_sceneRTSurf = NULL; }
                if (m_sceneRT) { m_sceneRT->Release(); m_sceneRT = NULL; }
                if (m_bloomRTSurf) { m_bloomRTSurf->Release(); m_bloomRTSurf = NULL; }
                if (m_bloomRT) { m_bloomRT->Release(); m_bloomRT = NULL; }
                if (m_bloomRT2Surf) { m_bloomRT2Surf->Release(); m_bloomRT2Surf = NULL; }
                if (m_bloomRT2) { m_bloomRT2->Release(); m_bloomRT2 = NULL; }

                // Full-res post scene. Match the active RT when it is one of
                // D3D9's sane 8-bit formats. FP16 looked fancy, then turned
                // into a blue motherfucking smoke grenade on drivers that
                // refused ARGB8-to-FP16 StretchRect. Never again.
                if (SUCCEEDED(m_device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET,
                    postFormat, D3DPOOL_DEFAULT, &m_sceneRT, NULL))) {
                    m_sceneRT->GetSurfaceLevel(0, &m_sceneRTSurf);
                }
                // Quarter-res bloom targets
                int bw = w/4, bh = h/4; if (bw<1) bw=1; if (bh<1) bh=1;
                if (SUCCEEDED(m_device->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET,
                    postFormat, D3DPOOL_DEFAULT, &m_bloomRT, NULL))) {
                    m_bloomRT->GetSurfaceLevel(0, &m_bloomRTSurf);
                }
                if (SUCCEEDED(m_device->CreateTexture(bw, bh, 1, D3DUSAGE_RENDERTARGET,
                    postFormat, D3DPOOL_DEFAULT, &m_bloomRT2, NULL))) {
                    m_bloomRT2->GetSurfaceLevel(0, &m_bloomRT2Surf);
                }
                m_sceneRTWidth = w; m_sceneRTHeight = h;
            }
            if (m_sceneRTSurf && m_bloomRTSurf && m_bloomRT2Surf) {
                // Copy the active viewport (with skybox) into the post scene. If the
                // driver rejects this, do not clear the scene to a fake sky
                // color. That blue fallback was a silent graphics bomb: one
                // failed copy poisoned the whole fucking frame.
                HRESULT srHr = E_FAIL;
                if (postOldRT && srcRect.right > srcRect.left && srcRect.bottom > srcRect.top)
                    srHr = m_device->StretchRect(postOldRT, &srcRect, m_sceneRTSurf, NULL, D3DTEXF_NONE);
                if (SUCCEEDED(srHr)) {
                    m_device->SetRenderTarget(0, m_sceneRTSurf);
                    D3DVIEWPORT9 sceneVP = { 0, 0, (DWORD)w, (DWORD)h, 0.0f, 1.0f };
                    m_device->SetViewport(&sceneVP);
                    m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                    usingPostRT = true;
                } else {
                    static int s_postStretchFailLogged = 0;
                    if (!s_postStretchFailLogged) {
                        LevelLogf("[Post] StretchRect backbuffer copy failed hr=0x%08X; rendering direct this frame",
                                  (unsigned)srHr);
                        s_postStretchFailLogged = 1;
                    }
                }
            }
        }

        // ── Inline draw helper macro (no lambdas — older MSVC compat) ──
        // Per-material game PS selection: picks the right bridge VS + game PS
        // based on which texture slots the mesh part has.
        #define DRAW_MESH_PART(thePart, theMdl, thePI) do { \
            uint32_t _pf = GetPartFlags(theMdl, thePI); \
            m_device->SetRenderState(D3DRS_CULLMODE, (_pf & 2) ? D3DCULL_NONE : D3DCULL_CW); \
            IDirect3DTexture9* _tex = (thePart).diffuseTex ? (thePart).diffuseTex : m_whiteTex; \
            if (_tex != curTex) { curTex = _tex; m_device->SetTexture(0, curTex); } \
            if (useGameShaders) { \
                bool _hasN = ((thePart).normalTex != NULL); \
                bool _hasS = ((thePart).specularTex != NULL); \
                if (_hasN && _hasS && m_gamePS_strauss_ANS && m_bridgeVS_AN) { \
                    /* Specular + normal → Strauss ANS (full metallic) */ \
                    m_device->SetVertexShader(m_bridgeVS_AN); \
                    m_device->SetPixelShader(m_gamePS_strauss_ANS); \
                    m_device->SetTexture(2, (thePart).normalTex); \
                    m_device->SetTexture(3, (thePart).specularTex); /* s3=specularMap */ \
                    m_device->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                    float _sp[4] = { 0.0f, 0.167f, 0.25f, 128.0f }; \
                    m_device->SetPixelShaderConstantF(66, _sp, 1); /* c66=straussParms */ \
                    float _sp1[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; \
                    m_device->SetPixelShaderConstantF(67, _sp1, 1); /* c67=straussParms1 */ \
                } else if (_hasN && _hasS && m_gamePS_strauss_AN && m_bridgeVS_AN) { \
                    /* Specular + normal (no spec tex) → Strauss AN */ \
                    m_device->SetVertexShader(m_bridgeVS_AN); \
                    m_device->SetPixelShader(m_gamePS_strauss_AN); \
                    m_device->SetTexture(2, (thePart).normalTex); \
                    float _sp[4] = { 0.0f, 0.167f, 0.25f, 128.0f }; \
                    m_device->SetPixelShaderConstantF(66, _sp, 1); \
                } else if (_hasN && m_gamePS_AN && m_bridgeVS_AN) { \
                    /* Normal map → Lit AN (most common: 137 calls in capture) */ \
                    m_device->SetVertexShader(m_bridgeVS_AN); \
                    m_device->SetPixelShader(m_gamePS_AN); \
                    m_device->SetTexture(2, (thePart).normalTex); \
                } else { \
                    /* No normal map → Lit A (simple) */ \
                    m_device->SetVertexShader(m_bridgeVS); \
                    m_device->SetPixelShader(m_gamePS_A); \
                } \
                /* Shadow map in s5 for game PS (game uses s5 not s3) */ \
                if (m_shadowsReady) { \
                    m_device->SetTexture(5, m_shadowMapTex); \
                    m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(5, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); \
                    m_device->SetSamplerState(5, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP); \
                } \
            } else { \
                /* Custom shader path — when game shaders unavailable */ \
                m_device->SetTexture(1, (thePart).normalTex ? (thePart).normalTex : m_flatNormalTex); \
                m_device->SetTexture(2, (thePart).specularTex); \
                float _mf[4] = { (thePart).normalTex?1.0f:0.0f, (thePart).specularTex?1.0f:0.0f, 1.0f, (thePart).detailTex?1.0f:0.0f }; \
                m_device->SetPixelShaderConstantF(10, _mf, 1); \
                if ((thePart).detailTex) { \
                    m_device->SetTexture(4, (thePart).detailTex); \
                    m_device->SetSamplerState(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                } \
                float _ep[4] = { (thePart).emissiveTex?1.0f:0.0f, 1.5f, 0, 0 }; \
                m_device->SetPixelShaderConstantF(18, _ep, 1); \
                if ((thePart).emissiveTex) { \
                    m_device->SetTexture(5, (thePart).emissiveTex); \
                    m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                } \
                /* Terrain multi-layer params */ \
                bool _isTerrain = ((thePart).terrainMask0 != NULL); \
                float _tp[4] = { _isTerrain ? 1.0f : 0.0f, 0.025f, 0, 0 }; \
                m_device->SetPixelShaderConstantF(22, _tp, 1); \
                if (_isTerrain) { \
                    m_device->SetTexture(6, (thePart).terrainLayer2); \
                    m_device->SetTexture(7, (thePart).terrainLayer3); \
                    m_device->SetTexture(8, (thePart).terrainMask0); \
                    m_device->SetTexture(9, (thePart).terrainMask1); \
                    m_device->SetTexture(10, (thePart).terrainMask2); \
                    for (int _ts = 6; _ts <= 10; ++_ts) { \
                        m_device->SetSamplerState(_ts, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                        m_device->SetSamplerState(_ts, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                        m_device->SetSamplerState(_ts, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP); \
                        m_device->SetSamplerState(_ts, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP); \
                    } \
                } \
                /* Water + lava params */ \
                uint32_t _wpf = GetPartFlags(theMdl, thePI); \
                float _wp[4] = { (_wpf & 4) ? 1.0f : 0.0f, s_waterTime, 0.08f, 0.6f }; \
                m_device->SetPixelShaderConstantF(19, _wp, 1); \
                if (_wpf & 8) { \
                    float _lep[4] = { 1.0f, 2.0f, 0, 0 }; \
                    m_device->SetPixelShaderConstantF(18, _lep, 1); \
                    m_device->SetTexture(5, (thePart).diffuseTex); \
                    m_device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR); \
                    m_device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); \
                } \
            } \
            /* Per-material color (game VS c197 → COLOR0 brightness multiplier) */ \
            if (useGameShaders) { \
                m_device->SetVertexShaderConstantF(197, (thePart).matColor, 1); \
            } \
            m_device->SetStreamSource(0, (thePart).vb, 0, LEVEL_VERTEX_SIZE); \
            m_device->SetIndices((thePart).ib); \
            m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, (thePart).vertexCount, 0, (thePart).indexCount / 3); \
            ++calls; \
        } while(0)

        // ══ WATER / LAVA TIME - REAL FUCKING DELTA, NOT HARDCODED 60FPS ════
        //
        // Was previously `s_waterTime += 0.016f` on the lazy-ass
        // dogshit assumption that every frame is exactly 16ms. The
        // editor routinely runs at 120-240fps on any GPU built this
        // decade, so water UV-scrolled at 2x-4x the intended speed
        // and lava boiled like a goddamn supervolcano. Helm's Deep
        // moats looked like a motherfucking tsunami had just rolled
        // in. Mt Doom looked like the End Times arrived early. User
        // caught this immediately, which is the only reason we fixed
        // it before it ended up in someone's published video.
        //
        // Use actual wall-clock delta from GetTickCount so animation
        // speed stays framerate-agnostic forever. First-frame guard
        // clamps absurd initial deltas because a paused editor
        // resuming can hand us a 30-second jump if we let it, which
        // would catapult the water forward like the ocean got shot
        // out of a fucking cannon. Anything over 500ms gets reset to
        // a nominal 16ms. If the editor actually freezes for half a
        // second (busy save, load, whatever), water does NOT warp -
        // it just resumes from where it was. That is the trade.
        static float s_waterTime = 0.0f;
        static DWORD s_waterTickPrev = 0;
        DWORD nowTick = GetTickCount();
        float dt = 0.016f;
        if (s_waterTickPrev != 0) {
            DWORD delta = nowTick - s_waterTickPrev;
            if (delta > 500) delta = 16; // first-frame or paused-editor guard
            dt = (float)delta / 1000.0f;
        }
        s_waterTickPrev = nowTick;
        s_waterTime += dt;

        // ── PASS 1: Opaque geometry ─────────────────────────────────────────
        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

        for (int instIdx = 0; instIdx < (int)m_instances.size(); ++instIdx)
        {
            const LevelInstance& inst = m_instances[instIdx];
            if (!inst.model || inst.model->parts.empty()) continue;
            if (m_gmfBitMask != 0xFFFFFFFF) {
                int mask = inst.gameModeMask;
                if (mask == 0 && !m_gmfShowScripts) continue;
                if (mask != -1 && mask != 0 && !((unsigned int)mask & m_gmfBitMask)) continue;
            }
            // Phase 3 XSI layer manager: hide whole layers, isolate one.
            // layerGuid == 0 means "no layer" (terrain pass, crowd) and
            // those entities ALWAYS render unless something specific
            // hides them, because they have no business being layer-
            // filtered out (they're scenery, not designer content).
            if (inst.layerGuid != 0 && !layerVisibleForRender(inst.layerGuid)) continue;
            m_device->SetVertexShaderConstantF(0, inst.mat, 4);   // our VS: c0-c3
            m_device->SetVertexShaderConstantF(178, inst.mat, 4); // game VS: c178-c181

            for (int pi = 0; pi < (int)inst.model->parts.size(); ++pi) {
                const LevelMeshPart& part = inst.model->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                uint32_t pf = GetPartFlags(inst.model, pi);
                if (pf & 16) continue; // skip hidden (occluders)
                if (pf & 1) continue; // skip transparent, rendered in pass 2
                DRAW_MESH_PART(part, inst.model, pi);
            }
        }

        // ── PASS 2: Alpha-tested cutouts + blended transparent geometry ─────
        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHAREF, (DWORD)128);
        m_device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

        // Alpha blend ordering. Pre-sort transparent instances back-to-
        // front by view-space Z or the goddamn blend math comes out
        // garbage.
        //
        // Without this we'd draw in m_instances order, which is the
        // order the motherfucking PAK gives us, which is the order the
        // disk had on a 2008 Xbox 360. So a Gondor banner that happens
        // to live earlier in the file gets drawn BEFORE the smoke stack
        // standing fifty meters behind it. Banner writes color, depth
        // gets locked, smoke draws second, smoke loses the depth fight,
        // smoke disappears. Or worse, the banner picks up the smoke
        // alpha and blends WITH ITSELF. Pure rookie-tier draw-order
        // horseshit, no fault of the artists, all on the renderer.
        //
        // Sort by world-AABB center. Coarse compared to per-part, but
        // we'd have to re-bound every fucking part every frame to do
        // per-part right and that's a real cost. The visible win at
        // viewport scale is enormous. Cheap and good. We eat.
        std::vector<std::pair<float,int> > xparentSort;
        xparentSort.reserve(m_instances.size());
        D3DMATRIX xpView;
        m_device->GetTransform(D3DTS_VIEW, &xpView);
        for (int instIdx = 0; instIdx < (int)m_instances.size(); ++instIdx)
        {
            const LevelInstance& inst = m_instances[instIdx];
            if (!inst.model || inst.model->parts.empty()) continue;
            if (m_gmfBitMask != 0xFFFFFFFF) {
                int mask = inst.gameModeMask;
                if (mask == 0 && !m_gmfShowScripts) continue;
                if (mask != -1 && mask != 0 && !((unsigned int)mask & m_gmfBitMask)) continue;
            }
            // Phase 3 XSI layer manager: same filter as the opaque pass.
            // Skipping the layer at SORT TIME instead of draw time means
            // hidden layers don't even compete for view-Z slots.
            if (inst.layerGuid != 0 && !layerVisibleForRender(inst.layerGuid)) continue;
            bool hasXparent = false;
            for (int pi = 0; pi < (int)inst.model->parts.size(); ++pi) {
                uint32_t pf = GetPartFlags(inst.model, pi);
                if (pf & 16) continue;          // hidden occluder
                if (pf & 1) { hasXparent = true; break; }
            }
            if (!hasXparent) continue;
            float cx = (inst.bboxMin[0] + inst.bboxMax[0]) * 0.5f;
            float cy = (inst.bboxMin[1] + inst.bboxMax[1]) * 0.5f;
            float cz = (inst.bboxMin[2] + inst.bboxMax[2]) * 0.5f;
            // D3D9 row-major view: viewZ = world_pos . view_col2 (rows of
            // the inverse camera transform). Larger viewZ = farther from
            // camera in left-handed convention. Which is the convention
            // we are stuck with, because the game shipped on Xbox 360,
            // and Microsoft's whole D3D9 stack is left-handed, and we
            // don't get to vote.
            float viewZ = cx*xpView._13 + cy*xpView._23 + cz*xpView._33 + xpView._43;
            xparentSort.push_back(std::make_pair(viewZ, instIdx));
        }
        // Descending by viewZ → far-first → back-to-front draw order.
        std::sort(xparentSort.begin(), xparentSort.end(),
                  std::greater<std::pair<float,int> >());

        for (size_t k = 0; k < xparentSort.size(); ++k)
        {
            int instIdx = xparentSort[k].second;
            const LevelInstance& inst = m_instances[instIdx];
            m_device->SetVertexShaderConstantF(0, inst.mat, 4);   // our VS: c0-c3
            m_device->SetVertexShaderConstantF(178, inst.mat, 4); // game VS: c178-c181

            for (int pi = 0; pi < (int)inst.model->parts.size(); ++pi) {
                const LevelMeshPart& part = inst.model->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                uint32_t pf = GetPartFlags(inst.model, pi);
                if (pf & 16) continue; // skip hidden (occluders)
                if (!(pf & 1)) continue; // skip opaque — already rendered

                // Water uses smooth alpha/refraction. A hard alpha test at 128
                // punches holes in the surface before blending can do its job.
                m_device->SetRenderState(D3DRS_ALPHATESTENABLE, (pf & 4) ? FALSE : TRUE);
                DRAW_MESH_PART(part, inst.model, pi);
            }
        }

        #undef DRAW_MESH_PART

        // Restore state
        m_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
    }
    else
    {
        // ── Fallback: fixed-function 3-point lighting ──
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetRenderState(D3DRS_LIGHTING, TRUE);
        m_device->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
        m_device->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
        m_device->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_COLORVALUE(0.40f, 0.40f, 0.42f, 1.0f));
        m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        m_device->SetFVF(LEVEL_VERTEX_FVF);

        D3DLIGHT9 keyLight;
        memset(&keyLight, 0, sizeof(keyLight));
        keyLight.Type        = D3DLIGHT_DIRECTIONAL;
        keyLight.Diffuse.r   = 0.80f; keyLight.Diffuse.g  = 0.75f; keyLight.Diffuse.b  = 0.68f; keyLight.Diffuse.a  = 1.0f;
        keyLight.Specular.r  = 0.50f; keyLight.Specular.g = 0.48f; keyLight.Specular.b = 0.42f; keyLight.Specular.a = 1.0f;
        keyLight.Direction.x = 0.35f; keyLight.Direction.y = -0.75f; keyLight.Direction.z = 0.56f;
        m_device->SetLight(0, &keyLight);
        m_device->LightEnable(0, TRUE);

        D3DLIGHT9 fillLight;
        memset(&fillLight, 0, sizeof(fillLight));
        fillLight.Type        = D3DLIGHT_DIRECTIONAL;
        fillLight.Diffuse.r   = 0.25f; fillLight.Diffuse.g  = 0.28f; fillLight.Diffuse.b  = 0.35f; fillLight.Diffuse.a  = 1.0f;
        fillLight.Direction.x = -0.35f; fillLight.Direction.y = -0.30f; fillLight.Direction.z = -0.56f;
        m_device->SetLight(1, &fillLight);
        m_device->LightEnable(1, TRUE);

        D3DLIGHT9 rimLight;
        memset(&rimLight, 0, sizeof(rimLight));
        rimLight.Type        = D3DLIGHT_DIRECTIONAL;
        rimLight.Diffuse.r   = 0.18f; rimLight.Diffuse.g  = 0.15f; rimLight.Diffuse.b  = 0.12f; rimLight.Diffuse.a  = 1.0f;
        rimLight.Direction.x = 0.0f;  rimLight.Direction.y = 0.85f; rimLight.Direction.z = -0.52f;
        m_device->SetLight(2, &rimLight);
        m_device->LightEnable(2, TRUE);

        D3DMATERIAL9 mat;
        memset(&mat, 0, sizeof(mat));
        mat.Diffuse.r  = 1.0f; mat.Diffuse.g  = 1.0f; mat.Diffuse.b  = 1.0f; mat.Diffuse.a  = 1.0f;
        mat.Ambient.r  = 1.0f; mat.Ambient.g  = 1.0f; mat.Ambient.b  = 1.0f; mat.Ambient.a  = 1.0f;
        mat.Specular.r = 0.3f; mat.Specular.g = 0.3f; mat.Specular.b = 0.3f; mat.Specular.a = 1.0f;
        mat.Power      = 20.0f;
        m_device->SetMaterial(&mat);

        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        // ── All geometry (fixed-function, single pass, two-sided support) ────
        for (int instIdx = 0; instIdx < (int)m_instances.size(); ++instIdx)
        {
            const LevelInstance& inst = m_instances[instIdx];
            LevelModel* mdl = inst.model;
            if (!mdl || mdl->parts.empty()) continue;
            if (m_gmfBitMask != 0xFFFFFFFF) {
                int mask = inst.gameModeMask;
                // Models: always show globals (-1) to keep the map visible
                // Only filter mode-specific instances by bitmask
                if (mask == 0 && !m_gmfShowScripts) continue;
                if (mask != -1 && mask != 0 && !((unsigned int)mask & m_gmfBitMask))
                    continue;
            }

            const float* m = inst.mat;
            D3DMATRIX wm;
            wm._11=m[0]; wm._12=m[1]; wm._13=m[2]; wm._14=m[3];
            wm._21=m[4]; wm._22=m[5]; wm._23=m[6]; wm._24=m[7];
            wm._31=m[8]; wm._32=m[9]; wm._33=m[10];wm._34=m[11];
            wm._41=m[12];wm._42=m[13];wm._43=m[14];wm._44=m[15];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            for (int pi = 0; pi < (int)mdl->parts.size(); ++pi)
            {
                const LevelMeshPart& part = mdl->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                uint32_t pf = GetPartFlags(mdl, pi);
                // bit62 meshes also drawn in pass 0 as background — render again
                // here so they're not lost behind opaque geometry

                m_device->SetRenderState(D3DRS_CULLMODE, (pf & 2) ? D3DCULL_NONE : D3DCULL_CW);

                if (part.diffuseTex != curTex)
                {
                    curTex = part.diffuseTex;
                    m_device->SetTexture(0, curTex);
                    m_device->SetTextureStageState(0, D3DTSS_COLOROP,
                        curTex ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
                }

                m_device->SetStreamSource(0, part.vb, 0, LEVEL_VERTEX_SIZE);
                m_device->SetIndices(part.ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                    0, 0, part.vertexCount, 0, part.indexCount / 3);
                ++calls;
            }
        }
        m_device->LightEnable(0, FALSE);
        m_device->LightEnable(1, FALSE);
        m_device->LightEnable(2, FALSE);
        m_device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        m_device->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE);
    }
    m_drawCallsLast = calls;

    // ── Post-processing: bloom + tone mapping ────────────────────────────────
    if (usingPostRT && m_postReady && m_bloomRTSurf && m_bloomRT2Surf) {
        // Helper: draw fullscreen quad
        struct PostVert { float x,y,z,u,v; };
        PostVert quad[4] = {
            {-1, 1, 0, 0, 0}, { 1, 1, 0, 1, 0},
            {-1,-1, 0, 0, 1}, { 1,-1, 0, 1, 1}
        };

        m_device->SetVertexShader(m_postVS);
        m_device->SetVertexDeclaration(m_postDecl);
        m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        m_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        m_device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);

        int bw = m_sceneRTWidth/4, bh = m_sceneRTHeight/4;
        if (bw<1) bw=1; if (bh<1) bh=1;
        float postHalfPixel[4] = { -1.0f / (float)bw, 1.0f / (float)bh, 0.0f, 0.0f };
        m_device->SetVertexShaderConstantF(0, postHalfPixel, 1);

        // Pass 1: Bright pass (scene → bloomRT)
        m_device->SetTexture(0, NULL);
        m_device->SetTexture(1, NULL);
        m_device->SetRenderTarget(0, m_bloomRTSurf);
        D3DVIEWPORT9 bvp = { 0, 0, (DWORD)bw, (DWORD)bh, 0.0f, 1.0f };
        m_device->SetViewport(&bvp);
        m_device->SetPixelShader(m_brightPassPS);
        m_device->SetTexture(0, m_sceneRT);
        m_device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        float bpParams[4] = { m_lighting.bloomThreshold, 1.5f, 0, 0 }; // from AtmosphereSetting
        m_device->SetPixelShaderConstantF(0, bpParams, 1);
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVert));

        // Pass 2-5: Two iterations of separable gaussian blur (smoother bloom)
        for (int blurIter = 0; blurIter < 2; ++blurIter) {
            // Horizontal (bloomRT → bloomRT2)
            m_device->SetTexture(0, NULL);
            m_device->SetRenderTarget(0, m_bloomRT2Surf);
            m_device->SetPixelShader(m_blurPS);
            m_device->SetTexture(0, m_bloomRT);
            float blurH[4] = { 1.0f/bw, 0, 0, 0 };
            m_device->SetPixelShaderConstantF(0, blurH, 1);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVert));

            // Vertical (bloomRT2 → bloomRT)
            m_device->SetTexture(0, NULL);
            m_device->SetRenderTarget(0, m_bloomRTSurf);
            m_device->SetTexture(0, m_bloomRT2);
            float blurV[4] = { 0, 1.0f/bh, 0, 0 };
            m_device->SetPixelShaderConstantF(0, blurV, 1);
            m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVert));
        }

        // Pass 4: Final composite — tone map + bloom → backbuffer
        m_device->SetRenderTarget(0, postOldRT);
        if (postOldDS) m_device->SetDepthStencilSurface(postOldDS);
        D3DVIEWPORT9 fullVP = { 0, 0, (DWORD)m_sceneRTWidth, (DWORD)m_sceneRTHeight, 0.0f, 1.0f };
        m_device->SetViewport(sHaveViewport ? &sViewport : &fullVP);
        DWORD finalW = sHaveViewport ? sViewport.Width : fullVP.Width;
        DWORD finalH = sHaveViewport ? sViewport.Height : fullVP.Height;
        if (finalW < 1) finalW = 1;
        if (finalH < 1) finalH = 1;
        postHalfPixel[0] = -1.0f / (float)finalW;
        postHalfPixel[1] =  1.0f / (float)finalH;
        m_device->SetVertexShaderConstantF(0, postHalfPixel, 1);
        m_device->SetPixelShader(m_toneMapPS);
        m_device->SetTexture(0, m_sceneRT);
        m_device->SetTexture(1, m_bloomRT);
        m_device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        m_device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        // Game tone mapping constants (from F9 capture c217-c220):
        // c0 = (exposure, bloomStr, whitepoint, 0)
        float toneParams[4] = { m_lighting.exposure, 0.15f, m_lighting.whitepoint, 0 };
        m_device->SetPixelShaderConstantF(0, toneParams, 1);
        // c1 = per-channel color balance (from captured c219, normalized: max=1.0)
        // Game c219 = (0.515, 0.525, 0.575) — we normalize so B=1.0
        // This gives warm tint (R slightly reduced) without darkening
        float brightness[4] = { 0.896f, 0.913f, 1.0f, 0.0f };
        m_device->SetPixelShaderConstantF(1, brightness, 1);
        // c2 = per-channel gamma (captured c217: R>G>B = warm shift)
        float gamma[4] = { m_lighting.gamma, 1.0f, 0.957f, 0.0f };
        // Use extracted gamma for R channel, captured ratios for G/B
        if (m_lighting.gamma > 0.5f) {
            gamma[1] = m_lighting.gamma / 1.05f;       // G relative to R
            gamma[2] = m_lighting.gamma * 0.957f/1.05f; // B relative to R
        }
        m_device->SetPixelShaderConstantF(2, gamma, 1);
        // c3 unused for now (contrast removed — needs adaptive exposure to work correctly)
        m_device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PostVert));

        m_device->SetTexture(1, NULL);

        // Reset state after post-processing so editor overlays (F4 wireframes) work
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetVertexDeclaration(NULL);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        // Clear extra texture samplers used by mesh rendering
        for (int si = 1; si < kSavedLevelTextureSlots; ++si) m_device->SetTexture(si, NULL);
        // Reset texture stage ops to default for fixed-function overlays
        m_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    if (postOldRT) postOldRT->Release();
    if (postOldDS) postOldDS->Release();

    // ── Fix fixed-function pipeline alignment ────────────────────────────────
    // The shader path uses WorldMatrix*ViewProj from constants c0-c7.
    // Havok's D3DTS_VIEW has an inherent RHS offset that misaligns all
    // fixed-function overlays. Fix: View=Identity, Projection=cachedViewProj.
    // This makes fixed-function World*View*Proj = World*Identity*VP = World*VP,
    // matching the shader path exactly.
    {
        D3DMATRIX identMat;
        memset(&identMat, 0, sizeof(identMat));
        identMat._11 = identMat._22 = identMat._33 = identMat._44 = 1.0f;
        D3DMATRIX vpMat;
        memcpy(&vpMat, m_cachedVP, sizeof(float) * 16);
        m_device->SetTransform(D3DTS_VIEW, &identMat);
        m_device->SetTransform(D3DTS_PROJECTION, &vpMat);
    }

    // ── Highlight pass: wireframe overlay for hovered/selected instance ────
    // Use the SAME vertex shader as the mesh pass to guarantee identical transforms.
    // The fixed-function pipeline has a subtle offset from Havok's view matrix.
    for (int pass = 0; pass < 2; ++pass)
    {
        int idx = (pass == 0) ? m_hoveredIdx : m_selectedIdx;
        if (idx < 0 || idx >= (int)m_instances.size()) continue;
        const LevelInstance& inst = m_instances[idx];
        LevelModel* mdl = inst.model;
        if (!mdl || mdl->parts.empty()) continue;

        // Use the level vertex shader for transform (same as mesh pass)
        if (m_levelVS) m_device->SetVertexShader(m_levelVS);
        m_device->SetPixelShader(NULL);
        m_device->SetTexture(0, NULL);
        if (m_levelDecl) m_device->SetVertexDeclaration(m_levelDecl);
        m_device->SetRenderState(D3DRS_LIGHTING,        FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE,          TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_FILLMODE,         (pass == 0) ? D3DFILL_WIREFRAME : D3DFILL_SOLID);
        m_device->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
        // Hovered = yellow wireframe, Selected = cyan solid tint
        DWORD col = (pass == 0) ? 0x80FFFF00 : 0x4000FFFF;
        m_device->SetRenderState(D3DRS_TEXTUREFACTOR, col);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        // Set world matrix via shader constants (same path as mesh render)
        m_device->SetVertexShaderConstantF(0, inst.mat, 4);  // c0-c3 = World
        // ViewProj already in c4-c7 from shader pass

        for (int pi = 0; pi < (int)mdl->parts.size(); ++pi) {
            const LevelMeshPart& part = mdl->parts[pi];
            if (!part.vb || !part.ib || part.indexCount < 3) continue;
            m_device->SetStreamSource(0, part.vb, 0, LEVEL_VERTEX_SIZE);
            m_device->SetIndices(part.ib);
            m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                0, 0, part.vertexCount, 0, part.indexCount / 3);
        }
        m_device->SetRenderState(D3DRS_FILLMODE,         D3DFILL_SOLID);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
    }

    // ── Asset tree highlight pass: orange wireframe for multiple instances ──────
    if (!m_assetHighlightIndices.empty())
    {
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING,        FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE,          TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_FILLMODE,         D3DFILL_WIREFRAME);
        m_device->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
        m_device->SetFVF(LEVEL_VERTEX_FVF);
        DWORD hlCol = 0x90FF8800; // orange wireframe
        m_device->SetRenderState(D3DRS_TEXTUREFACTOR, hlCol);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        for (int hi = 0; hi < (int)m_assetHighlightIndices.size(); ++hi) {
            int idx = m_assetHighlightIndices[hi];
            if (idx < 0 || idx >= (int)m_instances.size()) continue;
            const LevelInstance& inst = m_instances[idx];
            LevelModel* mdl = inst.model;
            if (!mdl || mdl->parts.empty()) continue;

            const float* m = inst.mat;
            D3DMATRIX wm;
            wm._11=m[0]; wm._12=m[1]; wm._13=m[2]; wm._14=m[3];
            wm._21=m[4]; wm._22=m[5]; wm._23=m[6]; wm._24=m[7];
            wm._31=m[8]; wm._32=m[9]; wm._33=m[10];wm._34=m[11];
            wm._41=m[12];wm._42=m[13];wm._43=m[14];wm._44=m[15];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            for (int pi = 0; pi < (int)mdl->parts.size(); ++pi) {
                const LevelMeshPart& part = mdl->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                m_device->SetStreamSource(0, part.vb, 0, LEVEL_VERTEX_SIZE);
                m_device->SetIndices(part.ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                    0, 0, part.vertexCount, 0, part.indexCount / 3);
            }
        }
        m_device->SetRenderState(D3DRS_FILLMODE,         D3DFILL_SOLID);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
    }

    // ── Collision shape visualization pass ─────────────────────────────────────
    // Only show collisions for the selected or hovered instance (not all at once).
    if (m_showCollisions && !m_collisionShapes.empty() &&
        (m_selectedIdx >= 0 || m_hoveredIdx >= 0))
    {
        int filterIdx = (m_selectedIdx >= 0) ? m_selectedIdx : m_hoveredIdx;
        uint32_t filterModelCrc = 0;
        if (filterIdx >= 0 && filterIdx < (int)m_instances.size() && m_instances[filterIdx].model) {
            for (std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.begin();
                 it != m_modelCache.end(); ++it) {
                if (it->second == m_instances[filterIdx].model) { filterModelCrc = it->first; break; }
            }
        }
        struct ColVert { float x,y,z; DWORD col; };
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE,  TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_CULLMODE,  D3DCULL_NONE);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);

        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        for (int ci = 0; ci < (int)m_collisionShapes.size(); ++ci)
        {
            const LevelCollisionShape& cs = m_collisionShapes[ci];
            // Show shapes for the selected instance, or any instance of the same model
            bool match = (cs.instanceIdx == filterIdx) ||
                         (filterModelCrc != 0 && cs.modelCrc == filterModelCrc && cs.instanceIdx == filterIdx);
            if (!match && filterModelCrc != 0 && cs.modelCrc == filterModelCrc) match = true; // fallback: same model
            if (!match) continue;
            DWORD col;
            switch (cs.kind) {
                case 1: col = 0x8000FF00; break; // box: green
                case 2: col = 0x800080FF; break; // sphere: blue
                case 3: col = 0x80FFFF00; break; // capsule: yellow
                case 4: col = 0x80FF8000; break; // cylinder: orange
                case 5: col = 0x80FF00FF; break; // convex: magenta
                case 6: col = 0x8000FFFF; break; // bvtree: cyan
                default:col = 0x80808080; break; // generic: gray
            }

            // Set world transform from collision shape
            const float* m = cs.worldMat;
            D3DMATRIX wm;
            wm._11=m[0]; wm._12=m[1]; wm._13=m[2]; wm._14=m[3];
            wm._21=m[4]; wm._22=m[5]; wm._23=m[6]; wm._24=m[7];
            wm._31=m[8]; wm._32=m[9]; wm._33=m[10];wm._34=m[11];
            wm._41=m[12];wm._42=m[13];wm._43=m[14];wm._44=m[15];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            if (cs.kind == 1) {
                // Box: 12 edges wireframe
                float hx = cs.halfExt[0], hy = cs.halfExt[1], hz = cs.halfExt[2];
                ColVert bv[8] = {
                    {-hx,-hy,-hz,col},{hx,-hy,-hz,col},{hx,hy,-hz,col},{-hx,hy,-hz,col},
                    {-hx,-hy, hz,col},{hx,-hy, hz,col},{hx,hy, hz,col},{-hx,hy, hz,col}
                };
                static const WORD bIdx[24] = {
                    0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7
                };
                m_device->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, 8, 12, bIdx, D3DFMT_INDEX16, bv, sizeof(ColVert));
            }
            else if (cs.kind == 2) {
                // Sphere: 3 orthogonal circles
                const int SEG = 24;
                ColVert sv[SEG * 3];
                for (int s = 0; s < SEG; ++s) {
                    float a = 6.28318f * s / SEG;
                    float ca = cosf(a) * cs.radius, sa = sinf(a) * cs.radius;
                    sv[s].x=ca; sv[s].y=sa; sv[s].z=0; sv[s].col=col;
                    sv[SEG+s].x=ca; sv[SEG+s].y=0; sv[SEG+s].z=sa; sv[SEG+s].col=col;
                    sv[SEG*2+s].x=0; sv[SEG*2+s].y=ca; sv[SEG*2+s].z=sa; sv[SEG*2+s].col=col;
                }
                WORD sIdx[SEG * 3 * 2];
                for (int s = 0; s < SEG; ++s) {
                    sIdx[s*2] = (WORD)s; sIdx[s*2+1] = (WORD)((s+1)%SEG);
                    sIdx[SEG*2+s*2] = (WORD)(SEG+s); sIdx[SEG*2+s*2+1] = (WORD)(SEG+(s+1)%SEG);
                    sIdx[SEG*4+s*2] = (WORD)(SEG*2+s); sIdx[SEG*4+s*2+1] = (WORD)(SEG*2+(s+1)%SEG);
                }
                m_device->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, SEG*3, SEG*3, sIdx, D3DFMT_INDEX16, sv, sizeof(ColVert));
            }
            else if (cs.kind == 3 || cs.kind == 4) {
                // Capsule/Cylinder: draw line between endpoints + circles at each end
                ColVert lv[2] = {{cs.pt1[0],cs.pt1[1],cs.pt1[2],col},{cs.pt2[0],cs.pt2[1],cs.pt2[2],col}};
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, lv, sizeof(ColVert));

                // Circles at endpoints (approximate, XZ plane)
                const int SEG = 16;
                float r = cs.radius > 0.01f ? cs.radius : 1.0f;
                for (int ep = 0; ep < 2; ++ep) {
                    const float* p = ep == 0 ? cs.pt1 : cs.pt2;
                    ColVert cv[SEG];
                    for (int s = 0; s < SEG; ++s) {
                        float a = 6.28318f * s / SEG;
                        cv[s].x=p[0]+cosf(a)*r; cv[s].y=p[1]; cv[s].z=p[2]+sinf(a)*r; cv[s].col=col;
                    }
                    WORD cIdx[SEG * 2];
                    for (int s = 0; s < SEG; ++s) { cIdx[s*2] = (WORD)s; cIdx[s*2+1] = (WORD)((s+1)%SEG); }
                    m_device->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, SEG, SEG, cIdx, D3DFMT_INDEX16, cv, sizeof(ColVert));
                }
            }
            else {
                // Generic/convex/bvtree: draw a small diamond marker
                float r = 1.0f;
                ColVert dv[6] = {
                    {r,0,0,col},{-r,0,0,col},{0,r,0,col},{0,-r,0,col},{0,0,r,col},{0,0,-r,col}
                };
                static const WORD dIdx[24] = {
                    0,2,0,3,0,4,0,5, 1,2,1,3,1,4,1,5, 2,4,2,5,3,4,3,5
                };
                m_device->DrawIndexedPrimitiveUP(D3DPT_LINELIST, 0, 6, 12, dIdx, D3DFMT_INDEX16, dv, sizeof(ColVert));
            }
        }

        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);

        // Draw text labels for each visible collision shape
        if (m_editorFont) {
            D3DVIEWPORT9 vp;
            m_device->GetViewport(&vp);
            D3DXMATRIX matView, matProj;
            m_device->GetTransform(D3DTS_VIEW, &matView);
            m_device->GetTransform(D3DTS_PROJECTION, &matProj);
            D3DXMATRIX matVP = matView * matProj;

            static const char* kindNames[] = {
                "Generic", "Box", "Sphere", "Capsule", "Cylinder", "Convex", "BVTree"
            };

            for (int ci = 0; ci < (int)m_collisionShapes.size(); ++ci) {
                const LevelCollisionShape& cs = m_collisionShapes[ci];
                bool lmatch = (cs.instanceIdx == filterIdx) ||
                              (filterModelCrc != 0 && cs.modelCrc == filterModelCrc);
                if (!lmatch) continue;

                // Project world position to screen
                D3DXVECTOR3 wp(cs.worldMat[12], cs.worldMat[13], cs.worldMat[14]);
                D3DXVECTOR3 sp;
                D3DXVec3Project(&sp, &wp, &vp, &matProj, &matView, NULL);
                if (sp.z < 0 || sp.z > 1) continue;

                int sx = (int)sp.x, sy = (int)sp.y;
                const char* kn = (cs.kind >= 0 && cs.kind <= 6) ? kindNames[cs.kind] : "?";
                char label[64];
                if (cs.kind == 1)
                    sprintf(label, "[%s] %.1fx%.1fx%.1f", kn, cs.halfExt[0]*2, cs.halfExt[1]*2, cs.halfExt[2]*2);
                else if (cs.kind == 2)
                    sprintf(label, "[%s] r=%.2f", kn, cs.radius);
                else if (cs.kind == 3 || cs.kind == 4)
                    sprintf(label, "[%s] r=%.2f", kn, cs.radius);
                else
                    sprintf(label, "[%s]", kn);

                RECT rc = { sx - 60, sy - 8, sx + 60, sy + 8 };
                // Shadow
                RECT rcS = { rc.left+1, rc.top+1, rc.right+1, rc.bottom+1 };
                m_editorFont->DrawTextA(NULL, label, -1, &rcS, DT_CENTER | DT_NOCLIP, 0xFF000000);
                // Color by kind
                DWORD txtCol;
                switch (cs.kind) {
                    case 1: txtCol = 0xFF00FF00; break;
                    case 2: txtCol = 0xFF8080FF; break;
                    case 3: txtCol = 0xFFFFFF00; break;
                    case 4: txtCol = 0xFFFF8000; break;
                    case 5: txtCol = 0xFFFF00FF; break;
                    case 6: txtCol = 0xFF00FFFF; break;
                    default:txtCol = 0xFFCCCCCC; break;
                }
                m_editorFont->DrawTextA(NULL, label, -1, &rc, DT_CENTER | DT_NOCLIP, txtCol);
            }
        }
    }

    // ── Collision volume wireframe pass (Collision entity node walls) ──────────
    if (m_showCollisions && !m_collisionVolumes.empty())
    {
        struct CVert { float x,y,z; DWORD col; };
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE,  TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_CULLMODE,  D3DCULL_NONE);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        // Identity world matrix — we transform manually
        D3DMATRIX identity;
        memset(&identity, 0, sizeof(identity));
        identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
        m_device->SetTransform(D3DTS_WORLD, &identity);

        for (int vi = 0; vi < (int)m_collisionVolumes.size(); ++vi) {
            const LevelCollisionVolume& cv = m_collisionVolumes[vi];
            if (cv.nodes.size() < 2) continue;

            // Phase 6: Color by collision flag profile
            DWORD colBottom, colTop, colVert;
            switch (cv.collFlags) {
            case 0x02u: colBottom=0xC0FFFF00; colTop=0x80FFFF80; colVert=0x60FFFFAA; break; // blockade=yellow
            case 0x04u: colBottom=0xC04444FF; colTop=0x808080FF; colVert=0x60AAAAFF; break; // player-only=blue
            case 0x08u: colBottom=0xC000FF00; colTop=0x8080FF80; colVert=0x60AAFFAA; break; // AI-only=green
            default:    colBottom=0xC0FF4040; colTop=0x80FF8080; colVert=0x60FFAAAA; break; // boundary=red (default)
            }

            // GameMode filter
            if (m_gmfBitMask != 0xFFFFFFFF &&
                cv.gameModeMask != -1 &&
                ((cv.gameModeMask == 0 && !m_gmfShowScripts) ||
                 (cv.gameModeMask != 0 && !((unsigned int)cv.gameModeMask & m_gmfBitMask))))
                continue;

            int n = (int)cv.nodes.size();
            float ox = cv.worldPos[0], oy = cv.worldPos[1], oz = cv.worldPos[2];
            float h = cv.height;

            // Draw lines: bottom edges, top edges, vertical pillars
            // Each segment = 6 lines: bottom, top, 2 verticals (left+right of segment)
            // Use DrawPrimitiveUP with D3DPT_LINELIST
            std::vector<CVert> lines;
            lines.reserve(n * 6);

            for (int ni = 0; ni < n - 1; ++ni) {
                float x0 = cv.nodes[ni].x + ox,   y0 = cv.nodes[ni].y + oy,   z0 = cv.nodes[ni].z + oz;
                float x1 = cv.nodes[ni+1].x + ox, y1 = cv.nodes[ni+1].y + oy, z1 = cv.nodes[ni+1].z + oz;

                // Bottom edge
                CVert v; v.col = colBottom;
                v.x=x0; v.y=y0; v.z=z0; lines.push_back(v);
                v.x=x1; v.y=y1; v.z=z1; lines.push_back(v);

                // Top edge
                v.col = colTop;
                v.x=x0; v.y=y0+h; v.z=z0; lines.push_back(v);
                v.x=x1; v.y=y1+h; v.z=z1; lines.push_back(v);

                // Vertical line at start node
                v.col = colVert;
                v.x=x0; v.y=y0;   v.z=z0; lines.push_back(v);
                v.x=x0; v.y=y0+h; v.z=z0; lines.push_back(v);
            }
            // Last vertical line at end node
            {
                int last = n - 1;
                float xL = cv.nodes[last].x + ox, yL = cv.nodes[last].y + oy, zL = cv.nodes[last].z + oz;
                CVert v; v.col = colVert;
                v.x=xL; v.y=yL;   v.z=zL; lines.push_back(v);
                v.x=xL; v.y=yL+h; v.z=zL; lines.push_back(v);
            }

            if (!lines.empty()) {
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2),
                                          &lines[0], sizeof(CVert));
            }
        }

        m_device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }

    // ── World collision mesh pass (BVTree wireframe) ───────────────────────────
    if (m_showCollisions && !m_worldCollMeshes.empty())
    {
        m_device->SetVertexShader(NULL);
        m_device->SetPixelShader(NULL);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_ZENABLE,  TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_device->SetRenderState(D3DRS_CULLMODE,  D3DCULL_NONE);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        for (int wi = 0; wi < (int)m_worldCollMeshes.size(); ++wi) {
            const LevelWorldCollisionMesh& wcm = m_worldCollMeshes[wi];
            if (!wcm.vb || !wcm.ib || wcm.triCount < 1) continue;

            D3DMATRIX wm;
            memset(&wm, 0, sizeof(wm));
            wm._11 = wm._22 = wm._33 = wm._44 = 1.0f;
            wm._41 = wcm.translation[0];
            wm._42 = wcm.translation[1];
            wm._43 = wcm.translation[2];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            m_device->SetStreamSource(0, wcm.vb, 0, 16);
            m_device->SetIndices(wcm.ib);
            m_device->DrawIndexedPrimitive(D3DPT_LINELIST, 0, 0, wcm.vertCount, 0, wcm.triCount * 3);
        }

        m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
    }

    // ViewProj fix already applied once after shader pass — all subsequent passes inherit it.

    // ── Editor-object pass: wireframe volumes + diamond markers + text labels ──
    if (!m_editorObjs.empty() && m_showEditorObjs)
    {
        struct WireVert { float x,y,z; DWORD col; };

        // Lazy-create font for type labels
        if (!m_editorFont && m_device)
        {
            D3DXCreateFontA(m_device, 10, 0, FW_BOLD, 1, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            "Arial", &m_editorFont);
        }

        // Octahedron (diamond) edges for the marker
        static const float kOct[6][3] = {
            { 0, 1, 0},   // 0 top
            { 0,-1, 0},   // 1 bottom
            { 1, 0, 0},   // 2 right
            {-1, 0, 0},   // 3 left
            { 0, 0, 1},   // 4 front
            { 0, 0,-1}    // 5 back
        };
        static const int kOctEdge[12][2] = {
            {0,2},{0,3},{0,4},{0,5},
            {1,2},{1,3},{1,4},{1,5},
            {2,4},{4,3},{3,5},{5,2}
        };

        // Box corners and edges for volume shapes
        static const float kBC[8][3] = {
            {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
            {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
        };
        static const int kBE[12][2] = {
            {0,1},{1,2},{2,3},{3,0},
            {4,5},{5,6},{6,7},{7,4},
            {0,4},{1,5},{2,6},{3,7}
        };

        // -- Wireframe volume pass (with depth test, no depth write, alpha blend for fade) --
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING,     FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE,     D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_ZENABLE,      TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_device->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        m_device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        // Extract camera world position from D3D9 view matrix. Used by
        // both distance-cull AND the screen-space pixel-floor logic in
        // the icon pass below. Extracted UNCONDITIONALLY so the icon
        // pass doesn't have to redo the inverse-rotation math, and so a
        // future bug doesn't ship because someone toggled max-dist off
        // and silently zeroed the cam pos used by everything else.
        float eoCamPos[3] = {0, 0, 0};
        {
            D3DMATRIX vmat;
            m_device->GetTransform(D3DTS_VIEW, &vmat);
            float tx = vmat._41, ty = vmat._42, tz = vmat._43;
            eoCamPos[0] = -(vmat._11*tx + vmat._12*ty + vmat._13*tz);
            eoCamPos[1] = -(vmat._21*tx + vmat._22*ty + vmat._23*tz);
            eoCamPos[2] = -(vmat._31*tx + vmat._32*ty + vmat._33*tz);
        }
        m_editorObjVisibleCount = 0;

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
        {
            const LevelEditorObj& eo = m_editorObjs[ei];
            char lbl[8] = {0};
            DWORD fixedCol = 0;
            float alpha = shouldDrawEditorObj(ei, eoCamPos, lbl, fixedCol);
            if (alpha <= 0.0f) continue;
            m_editorObjVisibleCount++;
            DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;
            if (ei == m_selectedEditorIdx) col = 0xFFFFFFFF;
            else if (ei == m_hoveredEditorIdx) col = 0xFFCCCCCC;
            // Apply fade alpha to ARGB color
            if (alpha < 1.0f) {
                DWORD a = (DWORD)((float)((col >> 24) & 0xFF) * alpha);
                col = (a << 24) | (col & 0x00FFFFFF);
            }

            const float* m = eo.mat;
            D3DMATRIX wm;
            wm._11=m[0]; wm._12=m[1]; wm._13=m[2]; wm._14=m[3];
            wm._21=m[4]; wm._22=m[5]; wm._23=m[6]; wm._24=m[7];
            wm._31=m[8]; wm._32=m[9]; wm._33=m[10];wm._34=m[11];
            wm._41=m[12];wm._42=m[13];wm._43=m[14];wm._44=m[15];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            // Scale down large volumes: clamp half-extents so they don't overwhelm the view
            // C2: Use higher clamp for trigger volumes to show accurate sizes
            bool isTrigger = eoContainsNoCase(eo.type.c_str(), "trigger");
            const float VOL_MAX = isTrigger ? 50.0f : 3.0f;
            float sx = eo.size[0] > VOL_MAX ? VOL_MAX : eo.size[0];
            float sy = eo.size[1] > VOL_MAX ? VOL_MAX : eo.size[1];
            float sz = eo.size[2] > VOL_MAX ? VOL_MAX : eo.size[2];

            if (eo.shape == "Sphere")
            {
                float r = sx;
                const int SEG = 20;
                WireVert verts[SEG * 2 * 3];
                int vi = 0;
                for (int plane = 0; plane < 3; ++plane)
                {
                    for (int s = 0; s < SEG; ++s)
                    {
                        float a0 = (float)s      / SEG * 6.2831853f;
                        float a1 = (float)(s + 1) / SEG * 6.2831853f;
                        float c0 = cosf(a0)*r, ss0 = sinf(a0)*r;
                        float c1 = cosf(a1)*r, ss1 = sinf(a1)*r;
                        WireVert& va = verts[vi++];
                        WireVert& vb = verts[vi++];
                        if      (plane == 0) { va.x=c0; va.y=ss0; va.z=0; vb.x=c1; vb.y=ss1; vb.z=0; }
                        else if (plane == 1) { va.x=c0; va.y=0;   va.z=ss0; vb.x=c1; vb.y=0; vb.z=ss1; }
                        else                 { va.x=0;  va.y=c0;  va.z=ss0; vb.x=0;  vb.y=c1; vb.z=ss1; }
                        va.col = col; vb.col = col;
                    }
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, SEG * 3, verts, sizeof(WireVert));
            }
            else if (sx > 0.01f || sy > 0.01f || sz > 0.01f) // Box volume
            {
                WireVert verts[24];
                for (int e = 0; e < 12; ++e)
                {
                    const float* c0 = kBC[kBE[e][0]];
                    const float* c1 = kBC[kBE[e][1]];
                    verts[e*2+0].x=c0[0]*sx; verts[e*2+0].y=c0[1]*sy; verts[e*2+0].z=c0[2]*sz; verts[e*2+0].col=col;
                    verts[e*2+1].x=c1[0]*sx; verts[e*2+1].y=c1[1]*sy; verts[e*2+1].z=c1[2]*sz; verts[e*2+1].col=col;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 12, verts, sizeof(WireVert));
            }
        }

        // -- Diamond marker + vertical spike pass (NO depth test = always on top) --
        m_device->SetRenderState(D3DRS_ZENABLE, FALSE);

        // Screen-space pixel floor for icons.
        //
        // Path-node icons get iconScale=0.4x. At any camera distance
        // past 80m or so they shrink to subpixel and FUCKING VANISH,
        // which on a level with 200 path nodes turns the whole nav
        // graph into invisible spaghetti the moment you pull back to
        // frame the level. The user is trying to debug AI navigation
        // and the editor is showing them an empty map. Cool, very
        // helpful, ten out of ten.
        //
        // Pin a minimum world-radius so the icon never shrinks below
        // MIN_ICON_PX on screen. Math: D3D9 perspective projects a
        // world segment of length L at depth d to pxSize = L *
        // focalPxY / d, where focalPxY = projMat._22 * (viewportH/2)
        // is the perspective scale mapping world-Y at unit depth to
        // pixels at unit depth. Invert to solve for L given pxSize.
        // Standard projective shit. Should have been here from day
        // one.
        float focalPxY = 0.0f;
        {
            D3DMATRIX iconProj; D3DVIEWPORT9 iconVP;
            m_device->GetTransform(D3DTS_PROJECTION, &iconProj);
            m_device->GetViewport(&iconVP);
            focalPxY = iconProj._22 * (float)iconVP.Height * 0.5f;
        }
        const float MIN_ICON_PX = 8.0f;

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
        {
            const LevelEditorObj& eo = m_editorObjs[ei];
            char lbl[8] = {0};
            DWORD fixedCol = 0;
            float alpha = shouldDrawEditorObj(ei, eoCamPos, lbl, fixedCol);
            if (alpha <= 0.0f) continue;
            DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;
            // Apply fade alpha
            if (alpha < 1.0f) {
                DWORD a = (DWORD)((float)((col >> 24) & 0xFF) * alpha);
                col = (a << 24) | (col & 0x00FFFFFF);
            }

            // World position from translation column
            float wx = eo.mat[12], wy = eo.mat[13], wz = eo.mat[14];

            // Identity world (marker is in world space directly)
            static const D3DMATRIX s_id2 = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            m_device->SetTransform(D3DTS_WORLD, &s_id2);

            const bool isSelEO = (ei == m_selectedEditorIdx);
            const bool isHovEO = (ei == m_hoveredEditorIdx);
            float iconScale = GetEditorIconScale(eo.type);
            float R = (isSelEO ? 1.2f : 0.8f) * iconScale;
            if (focalPxY > 0.0f) {
                float dx = wx - eoCamPos[0];
                float dy = wy - eoCamPos[1];
                float dz = wz - eoCamPos[2];
                float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                float minR = MIN_ICON_PX * dist / focalPxY;
                if (R < minR) R = minR;
            }
            if (isSelEO) col = 0xFFFFFFFF;
            else if (isHovEO) col = (col & 0x00FFFFFF) | 0xFFCCCCCC;
            int xsiIcon = GetXsiIconShape(eo.type);

            if (xsiIcon == ICON_CAMERA)
            {
                // Camera frustum: near rect → far rect
                float nr = R*0.35f, fr = R, d = R*2.0f;
                WireVert v[24]; int vi = 0;
                float n[4][3] = {{wx-nr,wy+nr,wz},{wx+nr,wy+nr,wz},{wx+nr,wy-nr,wz},{wx-nr,wy-nr,wz}};
                float f[4][3] = {{wx-fr,wy+fr,wz+d},{wx+fr,wy+fr,wz+d},{wx+fr,wy-fr,wz+d},{wx-fr,wy-fr,wz+d}};
                for (int e=0;e<4;++e){ int e2=(e+1)%4;
                    v[vi].x=n[e][0]; v[vi].y=n[e][1]; v[vi].z=n[e][2]; v[vi].col=col; vi++;
                    v[vi].x=n[e2][0];v[vi].y=n[e2][1];v[vi].z=n[e2][2];v[vi].col=col; vi++;
                    v[vi].x=f[e][0]; v[vi].y=f[e][1]; v[vi].z=f[e][2]; v[vi].col=col; vi++;
                    v[vi].x=f[e2][0];v[vi].y=f[e2][1];v[vi].z=f[e2][2];v[vi].col=col; vi++;
                    v[vi].x=n[e][0]; v[vi].y=n[e][1]; v[vi].z=n[e][2]; v[vi].col=col; vi++;
                    v[vi].x=f[e][0]; v[vi].y=f[e][1]; v[vi].z=f[e][2]; v[vi].col=col; vi++;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_ARROW_UP)
            {
                // Upward arrow: shaft + 4-prong arrowhead
                float tip = wy + R*2.0f, base = wy - R, hr = R*0.5f;
                WireVert v[10];
                v[0].x=wx; v[0].y=base; v[0].z=wz; v[0].col=col;
                v[1].x=wx; v[1].y=tip;  v[1].z=wz; v[1].col=col;
                v[2].x=wx; v[2].y=tip;  v[2].z=wz; v[2].col=col;
                v[3].x=wx+hr; v[3].y=tip-R; v[3].z=wz; v[3].col=col;
                v[4].x=wx; v[4].y=tip;  v[4].z=wz; v[4].col=col;
                v[5].x=wx-hr; v[5].y=tip-R; v[5].z=wz; v[5].col=col;
                v[6].x=wx; v[6].y=tip;  v[6].z=wz; v[6].col=col;
                v[7].x=wx; v[7].y=tip-R; v[7].z=wz+hr; v[7].col=col;
                v[8].x=wx; v[8].y=tip;  v[8].z=wz; v[8].col=col;
                v[9].x=wx; v[9].y=tip-R; v[9].z=wz-hr; v[9].col=col;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 5, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_TURRET)
            {
                // Tower: 4 pillars + top/bottom rings + merlons
                float hw = R*0.6f, hh = R, mh = R*0.5f;
                WireVert v[40]; int vi = 0;
                float cx[4]={wx-hw,wx+hw,wx+hw,wx-hw};
                float cz[4]={wz-hw,wz-hw,wz+hw,wz+hw};
                for (int p=0;p<4;++p){
                    v[vi].x=cx[p]; v[vi].y=wy-hh;    v[vi].z=cz[p]; v[vi].col=col; vi++;
                    v[vi].x=cx[p]; v[vi].y=wy+hh;    v[vi].z=cz[p]; v[vi].col=col; vi++;
                }
                for (int p=0;p<4;++p){ int p2=(p+1)%4;
                    v[vi].x=cx[p];  v[vi].y=wy-hh; v[vi].z=cz[p];  v[vi].col=col; vi++;
                    v[vi].x=cx[p2]; v[vi].y=wy-hh; v[vi].z=cz[p2]; v[vi].col=col; vi++;
                }
                for (int p=0;p<4;++p){ int p2=(p+1)%4;
                    v[vi].x=cx[p];  v[vi].y=wy+hh; v[vi].z=cz[p];  v[vi].col=col; vi++;
                    v[vi].x=cx[p2]; v[vi].y=wy+hh; v[vi].z=cz[p2]; v[vi].col=col; vi++;
                }
                for (int p=0;p<4;++p){
                    v[vi].x=cx[p]; v[vi].y=wy+hh;    v[vi].z=cz[p]; v[vi].col=col; vi++;
                    v[vi].x=cx[p]; v[vi].y=wy+hh+mh; v[vi].z=cz[p]; v[vi].col=col; vi++;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_STARBURST)
            {
                // 6 radiating lines from center
                WireVert v[12];
                float dirs[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
                for (int d=0;d<6;++d){
                    v[d*2].x=wx; v[d*2].y=wy; v[d*2].z=wz; v[d*2].col=col;
                    v[d*2+1].x=wx+dirs[d][0]*R*1.5f; v[d*2+1].y=wy+dirs[d][1]*R*1.5f; v[d*2+1].z=wz+dirs[d][2]*R*1.5f; v[d*2+1].col=col;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 6, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_CUBE)
            {
                // Wireframe cube
                WireVert v[24];
                for (int e=0;e<12;++e){
                    v[e*2].x  =wx+kBC[kBE[e][0]][0]*R; v[e*2].y  =wy+kBC[kBE[e][0]][1]*R; v[e*2].z  =wz+kBC[kBE[e][0]][2]*R; v[e*2].col  =col;
                    v[e*2+1].x=wx+kBC[kBE[e][1]][0]*R; v[e*2+1].y=wy+kBC[kBE[e][1]][1]*R; v[e*2+1].z=wz+kBC[kBE[e][1]][2]*R; v[e*2+1].col=col;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 12, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_CROSSHAIR)
            {
                // Crosshair: + shape (two perpendicular lines) + small circle
                WireVert v[20]; int vi = 0;
                // Horizontal line
                v[vi].x=wx-R; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+R; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                // Vertical line
                v[vi].x=wx; v[vi].y=wy-R; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy+R; v[vi].z=wz; v[vi].col=col; vi++;
                // Depth line
                v[vi].x=wx; v[vi].y=wy; v[vi].z=wz-R; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy; v[vi].z=wz+R; v[vi].col=col; vi++;
                // Small diamond in center (4 lines)
                float cr = R*0.3f;
                v[vi].x=wx+cr; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy+cr; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy+cr; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-cr; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-cr; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy-cr; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy-cr; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+cr; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_FLAG)
            {
                // Flag on pole: vertical pole + triangular flag
                WireVert v[8]; int vi = 0;
                float poleTop = wy + R*2.0f;
                // Pole
                v[vi].x=wx; v[vi].y=wy; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=poleTop; v[vi].z=wz; v[vi].col=col; vi++;
                // Flag triangle (top of pole → right → back to pole)
                v[vi].x=wx; v[vi].y=poleTop; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+R*1.2f; v[vi].y=poleTop-R*0.4f; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+R*1.2f; v[vi].y=poleTop-R*0.4f; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=poleTop-R*0.8f; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=poleTop-R*0.8f; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=poleTop; v[vi].z=wz; v[vi].col=col; vi++;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_SOUND)
            {
                // Speaker: box + 3 concentric arcs (4 segments each)
                // Buffer: 8 (box) + 3*4*2 (arcs) = 32 verts
                WireVert v[32]; int vi = 0;
                // Small speaker box
                float bw = R*0.3f, bh = R*0.4f;
                v[vi].x=wx-bw; v[vi].y=wy-bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+bw; v[vi].y=wy-bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+bw; v[vi].y=wy-bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+bw; v[vi].y=wy+bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+bw; v[vi].y=wy+bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-bw; v[vi].y=wy+bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-bw; v[vi].y=wy+bh; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-bw; v[vi].y=wy-bh; v[vi].z=wz; v[vi].col=col; vi++;
                // 3 arcs (semi-circles in YZ plane, 4 segments each)
                for (int arc = 1; arc <= 3; ++arc) {
                    float ar = R * 0.4f * (float)arc;
                    for (int s = 0; s < 4; ++s) {
                        float a0 = -1.2f + 2.4f * (float)s / 4.0f;
                        float a1 = -1.2f + 2.4f * (float)(s+1) / 4.0f;
                        v[vi].x=wx; v[vi].y=wy+sinf(a0)*ar; v[vi].z=wz+cosf(a0)*ar; v[vi].col=col; vi++;
                        v[vi].x=wx; v[vi].y=wy+sinf(a1)*ar; v[vi].z=wz+cosf(a1)*ar; v[vi].col=col; vi++;
                    }
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_SUN)
            {
                // Sun: circle + 8 radiating rays
                WireVert v[40]; int vi = 0;
                // Circle (8 segments in XZ plane)
                float cr = R*0.6f;
                for (int s = 0; s < 8; ++s) {
                    float a0 = 6.2832f * (float)s / 8.0f;
                    float a1 = 6.2832f * (float)(s+1) / 8.0f;
                    v[vi].x=wx+cosf(a0)*cr; v[vi].y=wy; v[vi].z=wz+sinf(a0)*cr; v[vi].col=col; vi++;
                    v[vi].x=wx+cosf(a1)*cr; v[vi].y=wy; v[vi].z=wz+sinf(a1)*cr; v[vi].col=col; vi++;
                }
                // 8 rays
                for (int r2 = 0; r2 < 8; ++r2) {
                    float a = 6.2832f * (float)r2 / 8.0f;
                    v[vi].x=wx+cosf(a)*cr; v[vi].y=wy; v[vi].z=wz+sinf(a)*cr; v[vi].col=col; vi++;
                    v[vi].x=wx+cosf(a)*R*1.3f; v[vi].y=wy; v[vi].z=wz+sinf(a)*R*1.3f; v[vi].col=col; vi++;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi/2, v, sizeof(WireVert));
            }
            else if (xsiIcon == ICON_SKULL)
            {
                // Death zone: 3D X-cross — universally reads as "danger"
                WireVert v[8]; int vi = 0;
                // X in XY plane
                v[vi].x=wx-R; v[vi].y=wy-R; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+R; v[vi].y=wy+R; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx+R; v[vi].y=wy-R; v[vi].z=wz; v[vi].col=col; vi++;
                v[vi].x=wx-R; v[vi].y=wy+R; v[vi].z=wz; v[vi].col=col; vi++;
                // X in YZ plane (gives depth so it reads from any angle)
                v[vi].x=wx; v[vi].y=wy-R; v[vi].z=wz-R; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy+R; v[vi].z=wz+R; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy-R; v[vi].z=wz+R; v[vi].col=col; vi++;
                v[vi].x=wx; v[vi].y=wy+R; v[vi].z=wz-R; v[vi].col=col; vi++;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 4, v, sizeof(WireVert));
            }
            else
            {
                // Default diamond (octahedron)
                WireVert oct[24];
                for (int e = 0; e < 12; ++e)
                {
                    const float* c0 = kOct[kOctEdge[e][0]];
                    const float* c1 = kOct[kOctEdge[e][1]];
                    oct[e*2+0].x = wx + c0[0]*R; oct[e*2+0].y = wy + c0[1]*R; oct[e*2+0].z = wz + c0[2]*R; oct[e*2+0].col = col;
                    oct[e*2+1].x = wx + c1[0]*R; oct[e*2+1].y = wy + c1[1]*R; oct[e*2+1].z = wz + c1[2]*R; oct[e*2+1].col = col;
                }
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 12, oct, sizeof(WireVert));
            }

            // Vertical spike above (pole) — 3 units tall
            WireVert spike[2];
            spike[0].x = wx; spike[0].y = wy + R;       spike[0].z = wz; spike[0].col = col;
            spike[1].x = wx; spike[1].y = wy + R + 3.0f; spike[1].z = wz; spike[1].col = col;
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, spike, sizeof(WireVert));
        }

        // -- Text label pass using D3DXFont --
        if (m_editorFont)
        {
            // Build ViewProj for label projection using ZeroMath
            D3DMATRIX d3dV, d3dP;
            D3DVIEWPORT9 vp;
            m_device->GetTransform(D3DTS_VIEW,       &d3dV);
            m_device->GetTransform(D3DTS_PROJECTION, &d3dP);
            m_device->GetViewport(&vp);
            ZMat4x4 labelView, labelProj, labelVP;
            labelView.load((const float*)&d3dV);
            labelProj.load((const float*)&d3dP);
            ZMatMul(&labelVP, &labelView, &labelProj);

            int labelCount = 0;
            for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
            {
                const LevelEditorObj& eo = m_editorObjs[ei];
                char lbl[8] = {0};
                DWORD fixedCol = 0;
                float alpha = shouldDrawEditorObj(ei, eoCamPos, lbl, fixedCol);
                if (alpha <= 0.0f) continue;
                // Label density cap (Phase 4)
                if (m_editorObjLabelMaxCount > 0 && labelCount >= m_editorObjLabelMaxCount) continue;
                ++labelCount;
                DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;
                // Apply fade alpha to label colors
                if (alpha < 1.0f) {
                    DWORD a = (DWORD)((float)((col >> 24) & 0xFF) * alpha);
                    col = (a << 24) | (col & 0x00FFFFFF);
                }

                const bool isSelLabel = (ei == m_selectedEditorIdx);

                // Project world position (+ spike top) to screen using ZeroMath
                float sx, sy, sz;
                ZProjectToScreen(sx, sy, sz,
                                  eo.mat[12], eo.mat[13] + 5.0f, eo.mat[14],
                                  &labelVP,
                                  (float)vp.X, (float)vp.Y,
                                  (float)vp.Width, (float)vp.Height,
                                  vp.MinZ, vp.MaxZ);

                if (sz < 0.0f || sz > 1.0f) continue;  // behind or too far

                RECT r;
                r.left   = (LONG)sx - 40;
                r.right  = (LONG)sx + 40;
                r.top    = (LONG)sy - 6;
                r.bottom = (LONG)sy + 6;

                // White outline (shadow)
                RECT ro = { r.left+1, r.top+1, r.right+1, r.bottom+1 };
                m_editorFont->DrawText(NULL, lbl, -1, &ro, DT_CENTER | DT_NOCLIP, 0xFF000000);
                // Type color foreground (white for selected)
                DWORD lblCol = isSelLabel ? 0xFFFFFFFF : col;
                m_editorFont->DrawText(NULL, lbl, -1, &r,  DT_CENTER | DT_NOCLIP, lblCol);

                // When selected, show full name below the label
                if (isSelLabel && !eo.name.empty())
                {
                    RECT rn;
                    rn.left   = (LONG)sx - 120;
                    rn.right  = (LONG)sx + 120;
                    rn.top    = (LONG)sy + 7;
                    rn.bottom = (LONG)sy + 19;
                    RECT rno = { rn.left+1, rn.top+1, rn.right+1, rn.bottom+1 };
                    m_editorFont->DrawText(NULL, eo.name.c_str(), -1, &rno, DT_CENTER | DT_NOCLIP, 0xFF000000);
                    m_editorFont->DrawText(NULL, eo.name.c_str(), -1, &rn,  DT_CENTER | DT_NOCLIP, 0xFFFFFF88);
                }
            }
        }

        // Restore Z state
        m_device->SetRenderState(D3DRS_ZENABLE, TRUE);
    }

    // ── Spline path rendering ───────────────────────────────────────────────────
    // Splines are tied to PTH category (0x0800). When PTH is off, skip splines.
    if (!m_splines.empty() && m_showEditorObjs && (m_editorObjCategoryMask & 0x0800u))
    {
        struct WV { float x,y,z; DWORD col; };

        static const D3DMATRIX s_idSpl = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        m_device->SetTransform(D3DTS_WORLD, &s_idSpl);
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING,     FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE,     D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_ZENABLE,      TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        for (size_t si = 0; si < m_splines.size(); ++si)
        {
            const LevelSpline& spl = m_splines[si];
            if (spl.nodes.size() < 2) continue;

            // When editor object filter is active, only draw splines in the filter set
            if (!m_editorObjFilter.empty() && m_editorObjFilter.find(spl.guid) == m_editorObjFilter.end())
                continue;

            // Determine color: cyan for position tracks, magenta for target tracks
            DWORD splCol = 0xFF00FFFF; // default cyan
            for (size_t ci = 0; ci < m_cineCameras.size(); ++ci)
            {
                if (m_cineCameras[ci].positionTrack == &spl) { splCol = 0xFF00FF88; break; }
                if (m_cineCameras[ci].targetTrack   == &spl) { splCol = 0xFFFF88FF; break; }
            }

            // Draw Catmull-Rom interpolated curve between nodes
            int totalSegs = 0;
            for (size_t ni = 0; ni + 1 < spl.nodes.size(); ++ni)
                totalSegs += 4; // 4 subdivisions per segment

            // Allocate line verts (2 per sub-segment)
            std::vector<WV> verts(totalSegs * 2);
            int vi = 0;
            for (size_t ni = 0; ni + 1 < spl.nodes.size(); ++ni)
            {
                // Clamp Catmull-Rom control points
                int i0 = (int)ni > 0 ? (int)ni - 1 : 0;
                int i1 = (int)ni;
                int i2 = (int)ni + 1;
                int i3 = (int)ni + 2 < (int)spl.nodes.size() ? (int)ni + 2 : (int)spl.nodes.size() - 1;

                const LevelSpline::Node& p0 = spl.nodes[i0];
                const LevelSpline::Node& p1 = spl.nodes[i1];
                const LevelSpline::Node& p2 = spl.nodes[i2];
                const LevelSpline::Node& p3 = spl.nodes[i3];

                for (int sub = 0; sub < 4; ++sub)
                {
                    float t0 = (float)sub / 4.0f;
                    float t1 = (float)(sub + 1) / 4.0f;

                    // Catmull-Rom at t0
                    float t0_2 = t0*t0, t0_3 = t0_2*t0;
                    float ax0 = 0.5f*(-p0.x + 3*p1.x - 3*p2.x + p3.x);
                    float bx0 = 0.5f*(2*p0.x - 5*p1.x + 4*p2.x - p3.x);
                    float cx0 = 0.5f*(-p0.x + p2.x);
                    float x0v = ax0*t0_3 + bx0*t0_2 + cx0*t0 + p1.x;

                    float ay0 = 0.5f*(-p0.y + 3*p1.y - 3*p2.y + p3.y);
                    float by0 = 0.5f*(2*p0.y - 5*p1.y + 4*p2.y - p3.y);
                    float cy0 = 0.5f*(-p0.y + p2.y);
                    float y0v = ay0*t0_3 + by0*t0_2 + cy0*t0 + p1.y;

                    float az0 = 0.5f*(-p0.z + 3*p1.z - 3*p2.z + p3.z);
                    float bz0 = 0.5f*(2*p0.z - 5*p1.z + 4*p2.z - p3.z);
                    float cz0 = 0.5f*(-p0.z + p2.z);
                    float z0v = az0*t0_3 + bz0*t0_2 + cz0*t0 + p1.z;

                    // Catmull-Rom at t1
                    float t1_2 = t1*t1, t1_3 = t1_2*t1;
                    float x1v = ax0*t1_3 + bx0*t1_2 + cx0*t1 + p1.x;
                    float y1v = ay0*t1_3 + by0*t1_2 + cy0*t1 + p1.y;
                    float z1v = az0*t1_3 + bz0*t1_2 + cz0*t1 + p1.z;

                    verts[vi].x = x0v; verts[vi].y = y0v; verts[vi].z = z0v; verts[vi].col = splCol; vi++;
                    verts[vi].x = x1v; verts[vi].y = y1v; verts[vi].z = z1v; verts[vi].col = splCol; vi++;
                }
            }
            if (vi > 0)
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, vi / 2, &verts[0], sizeof(WV));

            // Draw small diamonds at each control point
            for (size_t ni = 0; ni < spl.nodes.size(); ++ni)
            {
                float nx = spl.nodes[ni].x, ny = spl.nodes[ni].y, nz = spl.nodes[ni].z;
                float r = 0.5f;
                WV cp[6];
                cp[0].x=nx-r; cp[0].y=ny;   cp[0].z=nz;   cp[0].col=splCol;
                cp[1].x=nx+r; cp[1].y=ny;   cp[1].z=nz;   cp[1].col=splCol;
                cp[2].x=nx;   cp[2].y=ny-r; cp[2].z=nz;   cp[2].col=splCol;
                cp[3].x=nx;   cp[3].y=ny+r; cp[3].z=nz;   cp[3].col=splCol;
                cp[4].x=nx;   cp[4].y=ny;   cp[4].z=nz-r; cp[4].col=splCol;
                cp[5].x=nx;   cp[5].y=ny;   cp[5].z=nz+r; cp[5].col=splCol;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 3, cp, sizeof(WV));
            }
        }

        // Draw lines connecting cinematic cameras to their spline start points
        for (size_t ci = 0; ci < m_cineCameras.size(); ++ci)
        {
            const LevelCinematicCamera& cam = m_cineCameras[ci];
            float cx = cam.mat[12], cy = cam.mat[13], cz = cam.mat[14];

            if (cam.positionTrack && !cam.positionTrack->nodes.empty())
            {
                const LevelSpline::Node& n = cam.positionTrack->nodes[0];
                WV line[2];
                line[0].x=cx; line[0].y=cy; line[0].z=cz; line[0].col=0xFF00FF88;
                line[1].x=n.x; line[1].y=n.y; line[1].z=n.z; line[1].col=0xFF00FF88;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line, sizeof(WV));
            }
            if (cam.targetTrack && !cam.targetTrack->nodes.empty())
            {
                const LevelSpline::Node& n = cam.targetTrack->nodes[0];
                WV line[2];
                line[0].x=cx; line[0].y=cy; line[0].z=cz; line[0].col=0xFFFF88FF;
                line[1].x=n.x; line[1].y=n.y; line[1].z=n.z; line[1].col=0xFFFF88FF;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line, sizeof(WV));
            }
        }

        // ── SplineEditor handles + extend arrows ─────────────────────────
        // The 3D draggable handles, head/tail spheres, extend arrows, and
        // hover highlights are rendered by SplineEditor::DrawHandles. No-op
        // when edit mode is off. All the actual logic lives in
        // SplineEditor.cpp - this is the ONLY line in this file that
        // touches the editor module. Stay tiny on purpose, this file is
        // already 7800 lines and growing more by the goddamn day.
        {
            D3DXMATRIX vMat; m_device->GetTransform(D3DTS_VIEW, (D3DMATRIX*)&vMat);
            D3DXMATRIX pMat; m_device->GetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&pMat);
            // Eye position = inverse(view).translation. For an orthonormal
            // view matrix this is -view_rotation^T * view_translation.
            const float* v = (const float*)&vMat;
            float editorEye[3];
            editorEye[0] = -(v[0]*v[12] + v[1]*v[13] + v[2]*v[14]);
            editorEye[1] = -(v[4]*v[12] + v[5]*v[13] + v[6]*v[14]);
            editorEye[2] = -(v[8]*v[12] + v[9]*v[13] + v[10]*v[14]);
            SplineEditor::DrawHandles(m_device, this, editorEye,
                                      (const float*)&vMat, (const float*)&pMat);
            // Path Place: render path nodes + segment polyline + sampled
            // spawn markers when the crowd path tool is in draw mode.
            // Reads from module-scope cache populated by
            // SplineEditor::UpdateExternalPath earlier this frame.
            // No-op when path-place is inactive.
            SplineEditor::DrawExternalPathHandles(
                m_device, this, editorEye,
                (const float*)&vMat, (const float*)&pMat);
        }
    }

    // ── Path Place handles render: UNGATED ───────────────────────────
    // The block above is gated on cinematic splines existing + spline
    // category being visible. Path-place is a totally separate tool
    // and must render even when there are zero cinematic cameras (most
    // levels) or when the user has spline-category editor objects
    // filtered out. Compute camera matrices independently and call
    // DrawExternalPathHandles here so it ALWAYS renders.
    {
        D3DXMATRIX vMat2; m_device->GetTransform(D3DTS_VIEW, (D3DMATRIX*)&vMat2);
        D3DXMATRIX pMat2; m_device->GetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&pMat2);
        const float* v2 = (const float*)&vMat2;
        float pathEye[3];
        pathEye[0] = -(v2[0]*v2[12] + v2[1]*v2[13] + v2[2]*v2[14]);
        pathEye[1] = -(v2[4]*v2[12] + v2[5]*v2[13] + v2[6]*v2[14]);
        pathEye[2] = -(v2[8]*v2[12] + v2[9]*v2[13] + v2[10]*v2[14]);
        SplineEditor::DrawExternalPathHandles(
            m_device, this, pathEye,
            (const float*)&vMat2, (const float*)&pMat2);
    }

    // ── Phases 3-7: Editor overlay debug lines (x-ray pass) ──
    //
    // PathLinks, capture radii, spawn chains, AI claim circles, sound
    // emitter circles. All of them live at EXACT terrain height because
    // that's where the artists placed the goddamn nav nodes. Depth-test
    // them against the terrain and the GPU gets to pick a winner at
    // sub-millimeter Z deltas, which means every one of these debug
    // lines turns into shimmer soup the second the camera moves.
    // Looking at the navmesh through a kaleidoscope.
    //
    // These are CONCEPTUAL overlays. They want to be readable, not
    // physically correct. Force always-on-top for the whole debug
    // overlay block. The state-restore at end of RenderLevel puts
    // ZENABLE back to whatever the host had, so we don't poison the
    // next frame.
    if (m_showEditorObjs) {
        m_device->SetRenderState(D3DRS_ZENABLE, FALSE);
    }

    // ── Phase 3: PathLink visualization (lines between PathNodes) ──
    if (m_showPathLinks && m_showEditorObjs && (m_editorObjCategoryMask & 0x0800u)) {
        struct WVL { float x,y,z; DWORD col; };
        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei) {
            const LevelEditorObj& eo = m_editorObjs[ei];
            if (eo.type.find("PathLink") == std::string::npos &&
                eo.type.find("pathlink") == std::string::npos) continue;
            if (m_gameModeFilter != -1 && eo.gameModeMask != -1 &&
                !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
            // Resolve node1/node2 from cached maps
            std::map<uint32_t, uint32_t>::const_iterator n1It = m_goNode1.find(eo.guid);
            std::map<uint32_t, uint32_t>::const_iterator n2It = m_goNode2.find(eo.guid);
            if (n1It == m_goNode1.end() || n2It == m_goNode2.end()) continue;
            std::map<uint32_t, int>::const_iterator it1 = m_guidToEditorObj.find(n1It->second);
            std::map<uint32_t, int>::const_iterator it2 = m_guidToEditorObj.find(n2It->second);
            if (it1 == m_guidToEditorObj.end() || it2 == m_guidToEditorObj.end()) continue;
            const LevelEditorObj& n1 = m_editorObjs[it1->second];
            const LevelEditorObj& n2 = m_editorObjs[it2->second];
            // Color by PathLink properties
            DWORD col = 0xFF00FFCC; // cyan
            std::map<uint32_t, std::map<uint32_t, uint32_t> >::const_iterator ifIt = m_goIntFields.find(eo.guid);
            if (ifIt != m_goIntFields.end()) {
                std::map<uint32_t, uint32_t>::const_iterator fi;
                fi = ifIt->second.find(0x96BDE7B1u); // Ladder
                if (fi != ifIt->second.end() && fi->second) col = 0xFFFFFF00;
                fi = ifIt->second.find(0x8CDBC792u); // Jump
                if (fi != ifIt->second.end() && fi->second) col = 0xFFFF00FF;
            }
            WVL line[2];
            line[0].x = n1.mat[12]; line[0].y = n1.mat[13]; line[0].z = n1.mat[14]; line[0].col = col;
            line[1].x = n2.mat[12]; line[1].y = n2.mat[13]; line[1].z = n2.mat[14]; line[1].col = col;
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line, sizeof(WVL));
        }
    }

    // ── Phase 1 (XSI authoring): Event wiring overlay ──
    //
    // For every Output entity in the level, draw the wire chain that
    // tells the engine "when X happens to entity A, fire input Y on
    // entity B". Pandemic's authoring tool let designers SEE this in
    // the viewport. We've been making people read JSON to figure out
    // which trigger opens which gate. Pure motherfucking dark ages.
    //
    // The wire has two segments:
    //   owner -> Output entity   (the entity that emits the event)
    //   Output -> target entity  (who receives the input action)
    //
    // Color by event family so a glance tells you what kind of action
    // is being wired: trigger, capture, death, spawn, completion, etc.
    // Arrowhead at target end points BACK at the Output, so direction
    // is unambiguous even when wires cross.
    //
    // Focus mode: if m_eventWireFocusGuid is non-zero we draw ONLY that
    // Output's chain. Used by the Outputs panel "isolate this wire"
    // feature. Otherwise we draw all of them, which on a busy level is
    // a lot of fucking spaghetti, but better visible spaghetti than
    // hidden spaghetti.
    if (m_showEventWires && m_showEditorObjs)
    {
        struct WVL { float x,y,z; DWORD col; };
        const float Y_LIFT = 0.5f; // lift wires slightly above ground icons

        // Phase 10a: F4 was a fucking eyesore — every Output in the level
        // shooting arrows in every direction at the same world-space size
        // regardless of camera distance. Now it filters HARD:
        //   1. Forge focus set    → just that one Output's chain
        //   2. Else: viewport sel → wires that touch the selected entity
        //                           (as owner, output, OR target)
        //   3. Else                → NOTHING. Clean view, clean head.
        // And arrowheads are sized by screen distance so a wire across the
        // map doesn't end with a bargain-bin Mordor banner stuck to it.
        uint32_t selectedGuid = 0;
        if (m_eventWireFocusGuid == 0)
        {
            if (m_selectedEditorIdx >= 0 && m_selectedEditorIdx < (int)m_editorObjs.size())
                selectedGuid = m_editorObjs[m_selectedEditorIdx].guid;
            else if (m_selectedIdx >= 0 && m_selectedIdx < (int)m_instances.size())
                selectedGuid = m_instances[m_selectedIdx].guid;
        }
        // If neither focus nor selection is live, draw nothing. The user
        // pressed F4 to look at SOMETHING, not a yarn explosion.
        if (m_eventWireFocusGuid == 0 && selectedGuid == 0) goto skip_event_wires;

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
        {
            const LevelEditorObj& outEO = m_editorObjs[ei];
            // Only Output entities. Match exact "Output" or anything
            // starting with "Output" (OutputVolume etc) but NOT some
            // unrelated type that just happens to have Output in the
            // middle of its name.
            const char* tn = outEO.type.c_str();
            if (!(eoContainsNoCase(tn, "Output") &&
                  (_strnicmp(tn, "Output", 6) == 0))) continue;

            if (m_gameModeFilter != -1 && outEO.gameModeMask != -1 &&
                !(outEO.gameModeMask & (1 << m_gameModeFilter))) continue;

            // Focus filter: when isolating one chain, skip everything else.
            if (m_eventWireFocusGuid != 0 && outEO.guid != m_eventWireFocusGuid) continue;

            // Resolve owner and target.
            std::map<uint32_t, uint32_t>::const_iterator ownIt  = m_goOutputOwner.find(outEO.guid);
            std::map<uint32_t, uint32_t>::const_iterator tgtIt  = m_goTargetGuid.find(outEO.guid);
            if (tgtIt == m_goTargetGuid.end()) continue; // broken wire, no target
            uint32_t ownerGuid = (ownIt != m_goOutputOwner.end()) ? ownIt->second : 0;
            uint32_t targetGuid = tgtIt->second;

            // Phase 10a selection filter: only draw wires that touch the
            // selected entity. Match if selection is the owner, the Output
            // node, or the target. Skips quietly when nothing matches —
            // that's the whole point of "F4 cleans up after itself".
            if (m_eventWireFocusGuid == 0 && selectedGuid != 0)
            {
                if (ownerGuid  != selectedGuid &&
                    outEO.guid != selectedGuid &&
                    targetGuid != selectedGuid) continue;
            }

            std::map<uint32_t, int>::const_iterator ownEoIt = m_guidToEditorObj.find(ownerGuid);
            std::map<uint32_t, int>::const_iterator tgtEoIt = m_guidToEditorObj.find(targetGuid);
            // Owner can be missing if it has a mesh (instance, not editor obj).
            // Target ALSO can be on the instance side. For Phase 1 we only
            // draw the wires that resolve to editor-obj endpoints. The full
            // mesh-instance lookup comes when we wire the outliner.
            if (tgtEoIt == m_guidToEditorObj.end()) continue;

            const LevelEditorObj& tgtEO = m_editorObjs[tgtEoIt->second];

            // Owner pos: prefer editor obj, fall back to Output's own
            // position so the wire still draws (single segment) when the
            // owner is a mesh instance the lookup doesn't reach yet.
            float ownerX = outEO.mat[12], ownerY = outEO.mat[13], ownerZ = outEO.mat[14];
            bool haveOwnerEO = (ownEoIt != m_guidToEditorObj.end());
            if (haveOwnerEO) {
                const LevelEditorObj& ownEO = m_editorObjs[ownEoIt->second];
                ownerX = ownEO.mat[12]; ownerY = ownEO.mat[13]; ownerZ = ownEO.mat[14];
            }

            // Color by event family. Pulled from level.json analysis: top
            // recurring event names are OnTrigger, OnEnter, OnComplete,
            // OnDeath, OnCapture, OnSpawn, OnUser*, OnExpired, OnThreshold.
            DWORD col = 0xFFCCCCCCu; // default: pale grey, "unclassified"
            std::map<uint32_t, std::string>::const_iterator evIt = m_goOutputEvent.find(outEO.guid);
            if (evIt != m_goOutputEvent.end())
            {
                const char* ev = evIt->second.c_str();
                if      (eoContainsNoCase(ev, "Capture") ||
                         eoContainsNoCase(ev, "Neutralize"))     col = 0xFFFFD040u; // gold (capture)
                else if (eoContainsNoCase(ev, "Death") ||
                         eoContainsNoCase(ev, "Die") ||
                         eoContainsNoCase(ev, "Suicide") ||
                         eoContainsNoCase(ev, "Killed"))         col = 0xFFFF4040u; // red (death)
                else if (eoContainsNoCase(ev, "Trigger"))        col = 0xFF40DDFFu; // cyan (trigger)
                else if (eoContainsNoCase(ev, "Enter") ||
                         eoContainsNoCase(ev, "Exit"))           col = 0xFFFFEE40u; // yellow (volume)
                else if (eoContainsNoCase(ev, "Spawn") ||
                         eoContainsNoCase(ev, "Reinforcement"))  col = 0xFFFF40FFu; // magenta (spawn)
                else if (eoContainsNoCase(ev, "Complete") ||
                         eoContainsNoCase(ev, "Failed") ||
                         eoContainsNoCase(ev, "Threshold"))      col = 0xFF40FF60u; // green (logic)
                else if (eoContainsNoCase(ev, "User"))           col = 0xFFFFAA40u; // orange (user)
            }

            // Output endpoint position with Y lift.
            float outX = outEO.mat[12], outY = outEO.mat[13] + Y_LIFT, outZ = outEO.mat[14];
            float ownX = ownerX, ownY = ownerY + Y_LIFT, ownZ = ownerZ;
            float tgtX = tgtEO.mat[12], tgtY = tgtEO.mat[13] + Y_LIFT, tgtZ = tgtEO.mat[14];

            // Segment 1: owner -> output (only if we have a real owner)
            if (haveOwnerEO)
            {
                WVL seg[2];
                seg[0].x = ownX; seg[0].y = ownY; seg[0].z = ownZ;
                seg[0].col = (col & 0x00FFFFFFu) | 0x80000000u; // dim half on the upstream leg
                seg[1].x = outX; seg[1].y = outY; seg[1].z = outZ;
                seg[1].col = col;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, seg, sizeof(WVL));
            }

            // Segment 2: output -> target (full color)
            WVL main[2];
            main[0].x = outX; main[0].y = outY; main[0].z = outZ; main[0].col = col;
            main[1].x = tgtX; main[1].y = tgtY; main[1].z = tgtZ; main[1].col = col;
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, main, sizeof(WVL));

            // Arrowhead at target end. 3-line fletching pointing BACK at
            // the output so anyone reading the diagram knows direction
            // even when two outputs cross paths.
            float dx = outX - tgtX, dy = outY - tgtY, dz = outZ - tgtZ;
            float len = sqrtf(dx*dx + dy*dy + dz*dz);
            if (len > 0.001f)
            {
                float inv = 1.0f / len;
                dx *= inv; dy *= inv; dz *= inv;
                // Pick a perpendicular: cross with world-up, fall back to
                // a different axis if the wire is near-vertical.
                float upX = 0.0f, upY = 1.0f, upZ = 0.0f;
                float side1X = dy*upZ - dz*upY;
                float side1Y = dz*upX - dx*upZ;
                float side1Z = dx*upY - dy*upX;
                float sideLen = sqrtf(side1X*side1X + side1Y*side1Y + side1Z*side1Z);
                if (sideLen < 0.01f) {
                    // wire is vertical, use X axis instead
                    side1X = 0.0f; side1Y = 0.0f; side1Z = 1.0f;
                    sideLen = 1.0f;
                }
                side1X /= sideLen; side1Y /= sideLen; side1Z /= sideLen;
                // Second perpendicular = forward x side1
                float side2X = dy*side1Z - dz*side1Y;
                float side2Y = dz*side1X - dx*side1Z;
                float side2Z = dx*side1Y - dy*side1X;

                // Phase 10a: screen-space arrowhead. Old code scaled with
                // wire length, which produced car-sized arrows across the
                // map and pin-prick arrows on short wires. Now the head is
                // sized off camera distance to the target endpoint so it
                // looks the same on screen no matter how far you fly:
                //   arrow_world_units ≈ cam_dist * SCREEN_FACTOR
                // SCREEN_FACTOR ≈ tan(fov/2) * (target_pixels / screenH).
                // FOV ~60° (game default), screenH ~720, target ~10 px →
                // factor 0.008. Clamp keeps it sane near/far.
                float cdx = tgtX - m_cachedCamPos[0];
                float cdy = tgtY - m_cachedCamPos[1];
                float cdz = tgtZ - m_cachedCamPos[2];
                float camDist = sqrtf(cdx*cdx + cdy*cdy + cdz*cdz);
                float ah = camDist * 0.008f;
                if (ah < 0.05f) ah = 0.05f;
                if (ah > 0.6f)  ah = 0.6f;
                // Don't let the arrowhead exceed half the wire — looks
                // stupid on a 0.3-unit wire when cam is 200 units away.
                if (ah > len * 0.45f) ah = len * 0.45f;
                // Tip of fletching = a bit BEHIND the target along the wire.
                float tipX = tgtX + dx * ah;
                float tipY = tgtY + dy * ah;
                float tipZ = tgtZ + dz * ah;
                float spread = ah * 0.6f;

                WVL fletch[6];
                // 4 lines from target point outward to fletching tips
                fletch[0].x=tgtX; fletch[0].y=tgtY; fletch[0].z=tgtZ; fletch[0].col=col;
                fletch[1].x=tipX + side1X*spread; fletch[1].y=tipY + side1Y*spread; fletch[1].z=tipZ + side1Z*spread; fletch[1].col=col;
                fletch[2].x=tgtX; fletch[2].y=tgtY; fletch[2].z=tgtZ; fletch[2].col=col;
                fletch[3].x=tipX - side1X*spread; fletch[3].y=tipY - side1Y*spread; fletch[3].z=tipZ - side1Z*spread; fletch[3].col=col;
                fletch[4].x=tgtX; fletch[4].y=tgtY; fletch[4].z=tgtZ; fletch[4].col=col;
                fletch[5].x=tipX + side2X*spread; fletch[5].y=tipY + side2Y*spread; fletch[5].z=tipZ + side2Z*spread; fletch[5].col=col;
                m_device->DrawPrimitiveUP(D3DPT_LINELIST, 3, fletch, sizeof(WVL));
            }
        }
        skip_event_wires: ;
    }

    // ── Phase 4: CapturePoint radius visualization ──
    if (m_showCaptureRadii && m_showEditorObjs && (m_editorObjCategoryMask & 0x0002u)) {
        struct WVL { float x,y,z; DWORD col; };
        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei) {
            const LevelEditorObj& eo = m_editorObjs[ei];
            // Case-insensitive match for CapturePoint / capture_point / capture
            if (!eoContainsNoCase(eo.type.c_str(), "capture")) continue;
            // Skip trigger_ entities that also contain "capture" in their name
            if (eoContainsNoCase(eo.type.c_str(), "trigger")) continue;
            if (m_gameModeFilter != -1 && eo.gameModeMask != -1 &&
                !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
            // Try to get radius from CaptureArea -> trigger_radius chain
            float radius = 15.0f; // default fallback
            std::map<uint32_t, std::map<uint32_t, uint32_t> >::const_iterator grIt = m_goGuidRefs.find(eo.guid);
            if (grIt != m_goGuidRefs.end()) {
                std::map<uint32_t, uint32_t>::const_iterator caIt = grIt->second.find(0x841AF57Eu); // CaptureArea
                if (caIt != grIt->second.end() && caIt->second != 0) {
                    uint32_t trigGuid = caIt->second;
                    std::map<uint32_t, float>::const_iterator outerIt = m_goEditorOuter.find(trigGuid);
                    if (outerIt != m_goEditorOuter.end() && outerIt->second > 0.1f) radius = outerIt->second;
                    std::map<uint32_t, float>::const_iterator sizeIt = m_goEditorSize0.find(trigGuid);
                    if (sizeIt != m_goEditorSize0.end() && sizeIt->second > 0.1f) radius = sizeIt->second;
                }
            }
            // Fallback: use CP's own editor size if available
            if (radius <= 0.1f || radius == 15.0f) {
                if (eo.outer > 0.1f) radius = eo.outer;
                else if (eo.size[0] > 0.1f) radius = eo.size[0];
            }
            // Team color
            DWORD col = 0xFFFFFFFF; // neutral=white
            std::map<uint32_t, std::map<uint32_t, uint32_t> >::const_iterator ifIt = m_goIntFields.find(eo.guid);
            if (ifIt != m_goIntFields.end()) {
                std::map<uint32_t, uint32_t>::const_iterator tIt = ifIt->second.find(0xEDF0E1CFu); // Team
                if (tIt != ifIt->second.end()) {
                    if (tIt->second == 1) col = 0xFF4444FF;      // team1=blue
                    else if (tIt->second == 2) col = 0xFFFF4444; // team2=red
                }
                // Also check RedTeam field (CapturePoint uses this for initial ownership)
                if (col == 0xFFFFFFFF) {
                    tIt = ifIt->second.find(0x6594FA5Bu); // RedTeam
                    if (tIt != ifIt->second.end() && tIt->second == 2) col = 0xFFFF4444;
                }
            }
            float cpx = eo.mat[12], cpy = eo.mat[13], cpz = eo.mat[14];
            const int NSEG = 32;
            WVL circle[NSEG * 2];
            for (int s = 0; s < NSEG; ++s) {
                float a0 = (float)s * 6.283185f / (float)NSEG;
                float a1 = (float)(s+1) * 6.283185f / (float)NSEG;
                circle[s*2+0].x = cpx + radius * cosf(a0); circle[s*2+0].y = cpy;
                circle[s*2+0].z = cpz + radius * sinf(a0); circle[s*2+0].col = col;
                circle[s*2+1].x = cpx + radius * cosf(a1); circle[s*2+1].y = cpy;
                circle[s*2+1].z = cpz + radius * sinf(a1); circle[s*2+1].col = col;
            }
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, NSEG, circle, sizeof(WVL));
        }
    }

    // ── Phase 5: Spawn chain visualization ──
    if (m_showSpawnChains && m_showEditorObjs && (m_editorObjCategoryMask & 0x0001u)) {
        struct WVL { float x,y,z; DWORD col; };
        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei) {
            const LevelEditorObj& eo = m_editorObjs[ei];
            bool isEmitter = (eo.type.find("spawn_emitter") != std::string::npos);
            bool isPoint   = (eo.type.find("spawn_point") != std::string::npos && !isEmitter);
            if (!isEmitter && !isPoint) continue;
            if (m_gameModeFilter != -1 && eo.gameModeMask != -1 &&
                !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
            float sx = eo.mat[12], sy = eo.mat[13], sz = eo.mat[14];
            // Draw lines to Nodes[] (green for emitter->point, dim green for point->node)
            std::map<uint32_t, std::vector<uint32_t> >::const_iterator nodesIt = m_goNodes.find(eo.guid);
            if (nodesIt != m_goNodes.end()) {
                DWORD colA = isEmitter ? 0xFF00FF00u : 0xFF00AA00u;
                DWORD colB = isEmitter ? 0xFF00AA00u : 0xFF006600u;
                for (size_t ni = 0; ni < nodesIt->second.size(); ++ni) {
                    std::map<uint32_t, int>::const_iterator nit = m_guidToEditorObj.find(nodesIt->second[ni]);
                    if (nit == m_guidToEditorObj.end()) continue;
                    const LevelEditorObj& tgt = m_editorObjs[nit->second];
                    WVL line[2];
                    line[0].x=sx; line[0].y=sy; line[0].z=sz; line[0].col=colA;
                    line[1].x=tgt.mat[12]; line[1].y=tgt.mat[13]; line[1].z=tgt.mat[14]; line[1].col=colB;
                    m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line, sizeof(WVL));
                }
            }
        }
    }

    // ── Phase 6: AIGoal priority labels + claim radius circles ──
    if (m_showAIGoals && m_showEditorObjs && (m_editorObjCategoryMask & 0x0010u)) {
        struct WVL { float x,y,z; DWORD col; };
        // Get view/proj for text label projection
        D3DMATRIX d3dVAI, d3dPAI; D3DVIEWPORT9 vpAI;
        m_device->GetTransform(D3DTS_VIEW, &d3dVAI);
        m_device->GetTransform(D3DTS_PROJECTION, &d3dPAI);
        m_device->GetViewport(&vpAI);
        ZMat4x4 aiView, aiProj, aiVP;
        aiView.load((const float*)&d3dVAI);
        aiProj.load((const float*)&d3dPAI);
        ZMatMul(&aiVP, &aiView, &aiProj);

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei) {
            const LevelEditorObj& eo = m_editorObjs[ei];
            if (!eoContainsNoCase(eo.type.c_str(), "AIGoal") &&
                !eoContainsNoCase(eo.type.c_str(), "ai_goal")) continue;
            if (m_gameModeFilter != -1 && eo.gameModeMask != -1 &&
                !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;

            // Get priority and team from int_fields
            int priority = 0; int team = 0;
            std::map<uint32_t, std::map<uint32_t, uint32_t> >::const_iterator ifIt = m_goIntFields.find(eo.guid);
            if (ifIt != m_goIntFields.end()) {
                std::map<uint32_t, uint32_t>::const_iterator pIt = ifIt->second.find(0xFFF34C6Fu); // Priority
                if (pIt != ifIt->second.end()) priority = (int)pIt->second;
                std::map<uint32_t, uint32_t>::const_iterator tIt = ifIt->second.find(0xEDF0E1CFu); // Team
                if (tIt != ifIt->second.end()) team = (int)tIt->second;
            }
            // Get claim radius from float_fields
            float claimRadius = 10.0f;
            std::map<uint32_t, std::map<uint32_t, float> >::const_iterator ffIt = m_goFloatFields.find(eo.guid);
            if (ffIt != m_goFloatFields.end()) {
                std::map<uint32_t, float>::const_iterator crIt = ffIt->second.find(0xEBFE18C3u); // ClaimRadius
                if (crIt != ffIt->second.end() && crIt->second > 0.1f) claimRadius = crIt->second;
            }

            // Team color
            DWORD col = 0xFFAAFF00; // default lime green
            if (team == 1) col = 0xFF4488FF; // blue
            else if (team == 2) col = 0xFFFF4444; // red

            float gx = eo.mat[12], gy = eo.mat[13], gz = eo.mat[14];

            // Draw claim radius circle on ground
            const int NSEG = 24;
            WVL circle[NSEG * 2];
            for (int s = 0; s < NSEG; ++s) {
                float a0 = (float)s * 6.283185f / (float)NSEG;
                float a1 = (float)(s+1) * 6.283185f / (float)NSEG;
                circle[s*2+0].x = gx + claimRadius * cosf(a0); circle[s*2+0].y = gy;
                circle[s*2+0].z = gz + claimRadius * sinf(a0); circle[s*2+0].col = col;
                circle[s*2+1].x = gx + claimRadius * cosf(a1); circle[s*2+1].y = gy;
                circle[s*2+1].z = gz + claimRadius * sinf(a1); circle[s*2+1].col = col;
            }
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, NSEG, circle, sizeof(WVL));

            // Draw priority label as 3D text overlay
            if (m_editorFont && priority > 0) {
                float sx2, sy2, sz2;
                ZProjectToScreen(sx2, sy2, sz2, gx, gy + 7.0f, gz, &aiVP,
                    (float)vpAI.X, (float)vpAI.Y, (float)vpAI.Width, (float)vpAI.Height,
                    vpAI.MinZ, vpAI.MaxZ);
                if (sz2 >= 0.0f && sz2 <= 1.0f) {
                    char plbl[16]; _snprintf(plbl, sizeof(plbl), "P:%d", priority);
                    RECT rp; rp.left = (LONG)sx2 - 20; rp.right = (LONG)sx2 + 20;
                    rp.top = (LONG)sy2 - 6; rp.bottom = (LONG)sy2 + 6;
                    RECT rpo = { rp.left+1, rp.top+1, rp.right+1, rp.bottom+1 };
                    m_editorFont->DrawText(NULL, plbl, -1, &rpo, DT_CENTER | DT_NOCLIP, 0xFF000000);
                    m_editorFont->DrawText(NULL, plbl, -1, &rp,  DT_CENTER | DT_NOCLIP, col);
                }
            }
        }
    }

    // ── Phase 7: Sound emitter audible radius circles ──
    if (m_showSoundRadii && m_showEditorObjs && (m_editorObjCategoryMask & 0x0100u)) {
        struct WVL { float x,y,z; DWORD col; };
        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei) {
            const LevelEditorObj& eo = m_editorObjs[ei];
            if (!eoContainsNoCase(eo.type.c_str(), "Sound") &&
                !eoContainsNoCase(eo.type.c_str(), "sound")) continue;
            if (eoContainsNoCase(eo.type.c_str(), "Environment")) continue; // skip SoundEnvironment
            if (m_gameModeFilter != -1 && eo.gameModeMask != -1 &&
                !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;

            // Get radius from float_fields or editor_outer
            float radius = 0.0f;
            std::map<uint32_t, std::map<uint32_t, float> >::const_iterator ffIt = m_goFloatFields.find(eo.guid);
            if (ffIt != m_goFloatFields.end()) {
                // Look for Radius field — scan all float fields for one that could be radius
                for (std::map<uint32_t, float>::const_iterator fi = ffIt->second.begin(); fi != ffIt->second.end(); ++fi) {
                    if (fi->second > 1.0f && fi->second < 500.0f && fi->second > radius)
                        radius = fi->second; // take largest reasonable float as radius
                }
            }
            if (radius <= 0.1f) {
                if (eo.outer > 0.1f) radius = eo.outer;
                else radius = 10.0f; // default fallback
            }

            float sx2 = eo.mat[12], sy2 = eo.mat[13], sz2 = eo.mat[14];
            DWORD col = 0xFF44AAFF; // light blue for sound

            // Draw radius circle on ground
            const int NSEG = 24;
            WVL circle[NSEG * 2];
            for (int s = 0; s < NSEG; ++s) {
                float a0 = (float)s * 6.283185f / (float)NSEG;
                float a1 = (float)(s+1) * 6.283185f / (float)NSEG;
                circle[s*2+0].x = sx2 + radius * cosf(a0); circle[s*2+0].y = sy2;
                circle[s*2+0].z = sz2 + radius * sinf(a0); circle[s*2+0].col = col;
                circle[s*2+1].x = sx2 + radius * cosf(a1); circle[s*2+1].y = sy2;
                circle[s*2+1].z = sz2 + radius * sinf(a1); circle[s*2+1].col = col;
            }
            m_device->DrawPrimitiveUP(D3DPT_LINELIST, NSEG, circle, sizeof(WVL));
        }
    }

    // ── Phase 4 XSI: viewport gizmo render ──
    // Draw the active manipulator on top of EVERYTHING. Depth test is
    // already off here (we entered the x-ray block back at Phase 3
    // and never re-enabled it). World matrix at identity since we
    // build vertices in world space directly.
    if (m_gizmoMode != 0)
    {
        float gMat[16];
        if (gizmoGetTargetMatrix(gMat))
        {
            float origin[3] = { gMat[12], gMat[13], gMat[14] };
            float ax[3], ay[3], az[3];
            gizmoGetAxes(gMat, m_gizmoSpace, ax, ay, az);
            float* axisTable[3] = { ax, ay, az };
            const DWORD axisCol[3] = { 0xFFFF3030u, 0xFF30FF30u, 0xFF3060FFu };
            const DWORD axisHotCol[3] = { 0xFFFFFF60u, 0xFFFFFF60u, 0xFFFFFF60u };

            // Camera world position via the inverse view matrix (same
            // trick as the editor object pass uses).
            D3DMATRIX vmat;
            m_device->GetTransform(D3DTS_VIEW, &vmat);
            float tx = vmat._41, ty = vmat._42, tz = vmat._43;
            float camPos[3];
            camPos[0] = -(vmat._11*tx + vmat._12*ty + vmat._13*tz);
            camPos[1] = -(vmat._21*tx + vmat._22*ty + vmat._23*tz);
            camPos[2] = -(vmat._31*tx + vmat._32*ty + vmat._33*tz);
            float gSize = gizmoSizeAt(origin, camPos);

            static const D3DMATRIX gizId = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            m_device->SetTransform(D3DTS_WORLD, &gizId);
            m_device->SetTexture(0, NULL);
            m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
            m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
            m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

            struct GV { float x,y,z; DWORD col; };

            if (m_gizmoMode == 1 || m_gizmoMode == 3) {
                // Translate or scale: 3 axis rays from origin. Scale
                // gets a tiny cube tip, translate gets a cone-like
                // double line for the arrowhead.
                for (int a = 0; a < 3; ++a) {
                    bool hot = (m_gizmoActiveAxis == a + 1);
                    DWORD col = hot ? axisHotCol[a] : axisCol[a];
                    float* ad = axisTable[a];
                    float tip[3] = {
                        origin[0] + ad[0]*gSize,
                        origin[1] + ad[1]*gSize,
                        origin[2] + ad[2]*gSize
                    };
                    GV line[2];
                    line[0].x=origin[0]; line[0].y=origin[1]; line[0].z=origin[2]; line[0].col=col;
                    line[1].x=tip[0];    line[1].y=tip[1];    line[1].z=tip[2];    line[1].col=col;
                    m_device->DrawPrimitiveUP(D3DPT_LINELIST, 1, line, sizeof(GV));

                    // Arrow head (translate) or cube tip (scale).
                    int u = (a + 1) % 3, w = (a + 2) % 3;
                    float* uV = axisTable[u];
                    float* wV = axisTable[w];
                    if (m_gizmoMode == 1) {
                        float headLen = gSize * 0.20f;
                        float headRad = gSize * 0.08f;
                        float baseC[3] = {
                            tip[0] - ad[0]*headLen,
                            tip[1] - ad[1]*headLen,
                            tip[2] - ad[2]*headLen
                        };
                        const int HSEG = 6;
                        GV cone[HSEG*4];
                        int vi = 0;
                        for (int hs = 0; hs < HSEG; ++hs) {
                            float t0 = (float)hs / HSEG * 6.2831853f;
                            float t1 = (float)(hs+1) / HSEG * 6.2831853f;
                            float p0[3] = {
                                baseC[0] + (cosf(t0)*uV[0] + sinf(t0)*wV[0])*headRad,
                                baseC[1] + (cosf(t0)*uV[1] + sinf(t0)*wV[1])*headRad,
                                baseC[2] + (cosf(t0)*uV[2] + sinf(t0)*wV[2])*headRad
                            };
                            float p1[3] = {
                                baseC[0] + (cosf(t1)*uV[0] + sinf(t1)*wV[0])*headRad,
                                baseC[1] + (cosf(t1)*uV[1] + sinf(t1)*wV[1])*headRad,
                                baseC[2] + (cosf(t1)*uV[2] + sinf(t1)*wV[2])*headRad
                            };
                            cone[vi].x=p0[0]; cone[vi].y=p0[1]; cone[vi].z=p0[2]; cone[vi].col=col; ++vi;
                            cone[vi].x=tip[0]; cone[vi].y=tip[1]; cone[vi].z=tip[2]; cone[vi].col=col; ++vi;
                            cone[vi].x=p0[0]; cone[vi].y=p0[1]; cone[vi].z=p0[2]; cone[vi].col=col; ++vi;
                            cone[vi].x=p1[0]; cone[vi].y=p1[1]; cone[vi].z=p1[2]; cone[vi].col=col; ++vi;
                        }
                        m_device->DrawPrimitiveUP(D3DPT_LINELIST, HSEG*2, cone, sizeof(GV));
                    } else {
                        float boxR = gSize * 0.08f;
                        GV box[24];
                        int kBE[12][2] = {
                            {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},
                            {6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
                        };
                        float kBC[8][3] = {
                            {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
                            {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
                        };
                        for (int e = 0; e < 12; ++e) {
                            for (int side = 0; side < 2; ++side) {
                                int corner = kBE[e][side];
                                float dx = kBC[corner][0]*boxR, dy = kBC[corner][1]*boxR, dz = kBC[corner][2]*boxR;
                                box[e*2+side].x = tip[0] + dx;
                                box[e*2+side].y = tip[1] + dy;
                                box[e*2+side].z = tip[2] + dz;
                                box[e*2+side].col = col;
                            }
                        }
                        m_device->DrawPrimitiveUP(D3DPT_LINELIST, 12, box, sizeof(GV));
                    }
                }
            } else if (m_gizmoMode == 2) {
                // Rotate: 3 rings, one per axis, drawn in the plane
                // perpendicular to that axis.
                const int RSEG = 32;
                for (int a = 0; a < 3; ++a) {
                    bool hot = (m_gizmoActiveAxis == a + 1);
                    DWORD col = hot ? axisHotCol[a] : axisCol[a];
                    int u = (a + 1) % 3, w = (a + 2) % 3;
                    float* uV = axisTable[u];
                    float* wV = axisTable[w];
                    float ringR = gSize * 0.85f;
                    GV ring[RSEG*2];
                    for (int rs = 0; rs < RSEG; ++rs) {
                        float t0 = (float)rs / RSEG * 6.2831853f;
                        float t1 = (float)(rs+1) / RSEG * 6.2831853f;
                        ring[rs*2+0].x = origin[0] + (cosf(t0)*uV[0] + sinf(t0)*wV[0])*ringR;
                        ring[rs*2+0].y = origin[1] + (cosf(t0)*uV[1] + sinf(t0)*wV[1])*ringR;
                        ring[rs*2+0].z = origin[2] + (cosf(t0)*uV[2] + sinf(t0)*wV[2])*ringR;
                        ring[rs*2+0].col = col;
                        ring[rs*2+1].x = origin[0] + (cosf(t1)*uV[0] + sinf(t1)*wV[0])*ringR;
                        ring[rs*2+1].y = origin[1] + (cosf(t1)*uV[1] + sinf(t1)*wV[1])*ringR;
                        ring[rs*2+1].z = origin[2] + (cosf(t1)*uV[2] + sinf(t1)*wV[2])*ringR;
                        ring[rs*2+1].col = col;
                    }
                    m_device->DrawPrimitiveUP(D3DPT_LINELIST, RSEG, ring, sizeof(GV));
                }
            }
        }
    }

    // Restore render state
    for (int si = 0; si < kSavedLevelTextureSlots; ++si) {
        m_device->SetTexture(si, sTex[si]);
        if (sTex[si]) sTex[si]->Release();
    }
    for (int stage = 0; stage < 2; ++stage)
        for (int ti = 0; ti < (int)(sizeof(kSavedTss) / sizeof(kSavedTss[0])); ++ti)
            m_device->SetTextureStageState(stage, kSavedTss[ti], sTss[stage][ti]);
    for (int si = 0; si < kSavedLevelTextureSlots; ++si)
        for (int sa = 0; sa < (int)(sizeof(kSavedSamp) / sizeof(kSavedSamp[0])); ++sa)
            m_device->SetSamplerState(si, kSavedSamp[sa], sSamp[si][sa]);

    m_device->SetTransform(D3DTS_WORLD,      &sWorld);
    m_device->SetTransform(D3DTS_VIEW,       &sView);
    m_device->SetTransform(D3DTS_PROJECTION, &sProj);
    if (sHaveViewport) m_device->SetViewport(&sViewport);
    if (sHaveMaterial) m_device->SetMaterial(&sMaterial);
    m_device->SetRenderState(D3DRS_LIGHTING,         sLighting);
    m_device->SetRenderState(D3DRS_CULLMODE,         sCull);
    m_device->SetRenderState(D3DRS_ZENABLE,          sZEnable);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE,     sZWrite);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, sAlpha);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE, sColWrite);
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE,  sAlphaTest);
    m_device->SetRenderState(D3DRS_ALPHAREF,         sAlphaRef);
    m_device->SetRenderState(D3DRS_ALPHAFUNC,        sAlphaFunc);
    m_device->SetRenderState(D3DRS_FOGENABLE,        sFog);
    m_device->SetRenderState(D3DRS_FILLMODE,         sFill);
    m_device->SetRenderState(D3DRS_SRCBLEND,         sSrcBlend);
    m_device->SetRenderState(D3DRS_DESTBLEND,        sDestBlend);
    m_device->SetRenderState(D3DRS_SPECULARENABLE,   sSpecular);
    m_device->SetRenderState(D3DRS_NORMALIZENORMALS, sNormalize);
    m_device->SetRenderState(D3DRS_AMBIENT,          sAmbient);
    m_device->SetRenderState(D3DRS_TEXTUREFACTOR,    sTextureFactor);
    m_device->SetRenderState(D3DRS_COLORVERTEX,      sColorVertex);
    m_device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, sDiffuseMaterialSource);
    m_device->SetRenderState(D3DRS_SRGBWRITEENABLE,  sSrgbWrite);
    for (int li = 0; li < 3; ++li) {
        if (sHaveLight[li]) m_device->SetLight(li, &sLights[li]);
        m_device->LightEnable(li, sLightEnabled[li]);
    }
    m_device->SetVertexShader(sVS);
    m_device->SetPixelShader(sPS);
    if (sVS) sVS->Release();
    if (sPS) sPS->Release();
    if (sDecl) {
        m_device->SetVertexDeclaration(sDecl);
        sDecl->Release();
    } else if (sFVF != 0) {
        m_device->SetFVF(sFVF);
    } else {
        m_device->SetVertexDeclaration(NULL);
    }
}

// ---------------------------------------------------------------------------
// drawSingleModelWithShaders
//
// One-model render through the custom level shader path. The Model Viewer
// popup used to draw with fixed-function and a single-stage texture combiner;
// the main viewport draws with m_levelVS + m_levelPS doing Lambert + ambient
// + secondary sun + scatter inscatter (no shadow / no atmosphere fog here,
// nobody needs cascaded shadows on a 256-pixel character preview). The two
// outputs looked nothing alike and the user could not trust what the popup
// showed. Now the popup calls into this and we draw with the same shaders,
// same material flags, same texture slots as the main loop. Shadow map and
// terrain / water / emissive overrides are left at safe defaults because
// the popup is showing isolated character meshes, not lit terrain panels.
//
// Caller responsibilities (we deliberately do NOT do these so the caller
// can keep its existing state save/restore block):
//   - bind RT + DS, set viewport, clear
//   - save VS/PS/decl/texture/render-state before calling and restore after
// We bind shaders, constants, textures and draw. We unbind textures at the
// end so the caller's ImGui draw does not eat a stale sampler from us.
// Returns false if the shader pipeline never initialized (caller should
// fall back to FFP) or if there is no device/model.
// ---------------------------------------------------------------------------
bool LevelScene::drawSingleModelWithShaders(LevelModel*  model,
                                             const float  worldMat[16],
                                             const float  viewMat[16],
                                             const float  projMat[16])
{
    // ── Render the model via the EXACT main-viewport path ──────────────
    //
    // Per user request: "find the original viewport codes and apply it to
    // the model viewer". The original is render(). Instead of hand-rolling
    // a duplicate shader-bind sequence (which kept ending up dark/
    // transparent because we missed a constant, a sampler, or a render
    // state), we temporarily swap m_instances with a single fake instance
    // for this model, set the device's view+proj transforms (render()
    // reads them via GetTransform), and call render(). All the lighting,
    // shadow, scatter, post-fx, and per-part material logic the main
    // viewport uses runs verbatim against our one model.
    //
    // The caller has already bound the destination RT, depth surface,
    // and viewport (see ZeroEngine3DViewport.cpp triPicker pre-frame).
    // render() doesn't touch the RT, so its output goes into our RT.
    //
    // Save/restore everything we mutate so the surrounding host code
    // sees no side effects.
    if (!m_device || !model || model->parts.empty()) return false;

    // Copy row-major float[16] -> D3DMATRIX (memory layout identical).
    D3DMATRIX d3dView, d3dProj, d3dWorld;
    memcpy(&d3dView,  viewMat,  sizeof(d3dView));
    memcpy(&d3dProj,  projMat,  sizeof(d3dProj));
    memcpy(&d3dWorld, worldMat, sizeof(d3dWorld));

    // Save device transforms.
    D3DMATRIX savedView, savedProj, savedWorld;
    m_device->GetTransform(D3DTS_VIEW,       &savedView);
    m_device->GetTransform(D3DTS_PROJECTION, &savedProj);
    m_device->GetTransform(D3DTS_WORLD,      &savedWorld);

    // Stash m_instances so render() only sees our one fake instance.
    std::vector<LevelInstance> savedInstances;
    savedInstances.swap(m_instances);   // m_instances is empty now

    // ── Override level lighting with a neutral white preview rig ───────
    // Helm's Deep ships with a night-atmosphere setting where ambient is
    // around (0.04, 0.04, 0.04) and the sun is dim. That renders the
    // main viewport correctly but turns the model viewer into a black
    // smudge nobody can pick triangles on. Save the level's lighting,
    // swap in a Maya-style three-point rig (white ambient + bright key
    // light + warm fill), render, restore. The override is invisible to
    // the main viewport because render() reads m_lighting fresh each
    // frame and we put the original back before returning.
    LevelLighting savedLighting = m_lighting;
    m_lighting.ambient[0] = 0.45f; m_lighting.ambient[1] = 0.45f;
    m_lighting.ambient[2] = 0.50f; m_lighting.ambient[3] = 1.0f;
    m_lighting.sunCol[0] = 1.0f; m_lighting.sunCol[1] = 0.98f;
    m_lighting.sunCol[2] = 0.92f; m_lighting.sunCol[3] = 1.0f;
    // Key light direction — slightly above-camera diagonal so silhouette
    // reads. Normalized.
    {
        float d[3] = { -0.4f, -0.8f, -0.4f };
        float L = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (L > 1e-6f) { d[0]/=L; d[1]/=L; d[2]/=L; }
        m_lighting.sunDir[0] = d[0]; m_lighting.sunDir[1] = d[1];
        m_lighting.sunDir[2] = d[2]; m_lighting.sunDir[3] = 0.0f;
    }
    m_lighting.diffCol[0] = 1.0f; m_lighting.diffCol[1] = 1.0f;
    m_lighting.diffCol[2] = 1.0f; m_lighting.diffCol[3] = 1.0f;
    // Hemisphere ambient — bright sky / mid floor.
    m_lighting.topDomeCol[0] = 0.6f; m_lighting.topDomeCol[1] = 0.7f; m_lighting.topDomeCol[2] = 0.85f;
    m_lighting.botDomeCol[0] = 0.3f; m_lighting.botDomeCol[1] = 0.3f; m_lighting.botDomeCol[2] = 0.35f;
    // Secondary light — warm fill from the other side.
    m_lighting.hasSun2 = true;
    m_lighting.sun2Col[0] = 0.6f; m_lighting.sun2Col[1] = 0.55f; m_lighting.sun2Col[2] = 0.45f;
    {
        float d[3] = { 0.5f, 0.3f, 0.5f };
        float L = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (L > 1e-6f) { d[0]/=L; d[1]/=L; d[2]/=L; }
        m_lighting.sun2Dir[0] = d[0]; m_lighting.sun2Dir[1] = d[1];
        m_lighting.sun2Dir[2] = d[2]; m_lighting.sun2Dir[3] = 0.0f;
    }
    // Kill atmosphere fog — we don't want haze in a tiny model preview.
    m_lighting.scatterParams[0] = 0.0f;
    m_lighting.inscatterCol[0]  = 0.0f;
    m_lighting.inscatterCol[1]  = 0.0f;
    m_lighting.inscatterCol[2]  = 0.0f;
    m_lighting.inscatterCol[3]  = 0.0f;

    // ── Bypass the post-FX pipeline for this render ───────────────────
    // The reason every earlier "make it bright" attempt failed: render()
    // runs a bright-pass + bloom + Reinhard-tonemap chain when m_postReady
    // is true. The tonemap reads exposure/gamma/whitepoint/bloomThreshold
    // from m_lighting — fields the LevelLighting struct doesn't expose to
    // the field-by-field override above, so they keep their level-loaded
    // values (Helm's Deep night = dim-on-purpose). The tonemap then crushes
    // even pure-white preview ambient to ~0.2 of input. Worse, the post
    // pipeline StretchRects the EMBEDDED RT's pre-cleared background into
    // m_sceneRT, contaminating the bloom source.
    //
    // Simplest, most reversible fix: flip m_postReady off for the render
    // and back on at exit. The main viewport sees no change because we
    // restore before returning. The embedded RT now receives the raw
    // shader output, lit by the override we just installed.
    const bool savedPostReady = m_postReady;
    m_postReady = false;

    LevelInstance fake;
    fake.model = model;
    memcpy(fake.mat, worldMat, sizeof(fake.mat));
    // Bounding box for the instance (used by shadow pass framing): copy
    // from the model's local bounds. Treat worldMat as identity-ish for
    // the popup case so the model bbox doubles as the instance bbox.
    fake.bboxMin[0] = model->localMin[0];
    fake.bboxMin[1] = model->localMin[1];
    fake.bboxMin[2] = model->localMin[2];
    fake.bboxMax[0] = model->localMax[0];
    fake.bboxMax[1] = model->localMax[1];
    fake.bboxMax[2] = model->localMax[2];
    fake.guid       = 0;
    fake.parentGuid = 0;
    fake.gameModeMask = -1;
    fake.block1WtOffset = 0;
    fake.layerGuid  = 0;
    m_instances.push_back(fake);

    // Hand render() the camera it wants via D3D transforms.
    m_device->SetTransform(D3DTS_VIEW,       &d3dView);
    m_device->SetTransform(D3DTS_PROJECTION, &d3dProj);
    m_device->SetTransform(D3DTS_WORLD,      &d3dWorld);

    // GO. Same shader pipeline the main viewport uses. Same lighting,
    // same per-part material selection, same alpha/cull/depth state.
    render();

    // Restore everything.
    m_postReady = savedPostReady;
    m_lighting  = savedLighting;
    m_instances.swap(savedInstances);
    m_device->SetTransform(D3DTS_VIEW,       &savedView);
    m_device->SetTransform(D3DTS_PROJECTION, &savedProj);
    m_device->SetTransform(D3DTS_WORLD,      &savedWorld);
    return true;
}

// ---------------------------------------------------------------------------
// Embedded-weapon editor support.
//
// snapshotEmbeddedWeaponVerts: copy original Position floats for verts
// [vertFirst..vertFirst+vertCount) out of slot[0]'s CPU mirror into outOrig
// (size = vertCount*3). The Adjust Baked Weapon panel calls this once
// when the user picks a weapon; subsequent slider changes re-apply the
// absolute transform to this snapshot so we never accumulate float drift.
// We use the mirror because the VB is D3DUSAGE_WRITEONLY and reading
// through Lock is undefined on that flag.
//
// applyEmbeddedWeaponDelta: rewrite slot[0]'s VB Positions for the same
// range as worldDelta * origPositions[i]. Caller built worldDelta as
//   worldDelta = bind_world * delta_local * inv(bind_world)
// so XYZ/YPR slider moves run along Bone_LHand's local axes rather than
// world axes. Row-major 4x4 convention — same as the rest of this file.
// Only the 12 position bytes per vert are touched; normal / tangent / UV
// stay. Multi-slot bakes are unsupported (would need part_index in tag);
// embed mode is what the current panel produces and what the user iterates.
// ---------------------------------------------------------------------------
bool LevelScene::snapshotEmbeddedWeaponVerts(LevelModel*  model,
                                              uint32_t     vertFirst,
                                              uint32_t     vertCount,
                                              std::vector<float>& outOrig)
{
    outOrig.clear();
    if (!model || model->parts.empty() || vertCount == 0) return false;
    const LevelMeshPart& part = model->parts[0];
    if ((uint32_t)part.vertexCount < vertFirst + vertCount) return false;
    // Use the CPU position mirror that loadModelFromBinary populates.
    // The VB itself is D3DUSAGE_WRITEONLY — reading from a Lock'd pointer
    // is undefined and gave us garbage on a couple drivers during early
    // bring-up. The mirror is 12 bytes per vert; ~7 KB for the bow case.
    if (part.positionsCpu.size() < (size_t)(vertFirst + vertCount) * 3) return false;
    outOrig.assign(part.positionsCpu.begin() + (size_t)vertFirst * 3,
                   part.positionsCpu.begin() + (size_t)(vertFirst + vertCount) * 3);
    return true;
}

bool LevelScene::applyEmbeddedWeaponDelta(LevelModel*  model,
                                           uint32_t     vertFirst,
                                           uint32_t     vertCount,
                                           const float* origPositions,
                                           const float  worldDelta[16])
{
    if (!model || model->parts.empty() || vertCount == 0) return false;
    if (!origPositions || !worldDelta) return false;
    const LevelMeshPart& part = model->parts[0];
    if (!part.vb) return false;
    if ((uint32_t)part.vertexCount < vertFirst + vertCount) return false;

    void* data = NULL;
    if (FAILED(part.vb->Lock(0, 0, &data, 0))) return false;
    uint8_t* base = (uint8_t*)data;
    for (uint32_t i = 0; i < vertCount; ++i) {
        float ox = origPositions[i*3 + 0];
        float oy = origPositions[i*3 + 1];
        float oz = origPositions[i*3 + 2];
        float nx = worldDelta[0]*ox + worldDelta[1]*oy + worldDelta[2]*oz  + worldDelta[3];
        float ny = worldDelta[4]*ox + worldDelta[5]*oy + worldDelta[6]*oz  + worldDelta[7];
        float nz = worldDelta[8]*ox + worldDelta[9]*oy + worldDelta[10]*oz + worldDelta[11];
        float* p = (float*)(base + (size_t)(vertFirst + i) * LEVEL_VERTEX_SIZE);
        p[0] = nx;
        p[1] = ny;
        p[2] = nz;
    }
    part.vb->Unlock();
    return true;
}

// ---------------------------------------------------------------------------
// Bone-weight selection: which slot[0] verts are weighted to a palette slot?
//
// Every CRD in this engine — shipped or merged-by-us — has its weapons
// rigid-weighted to a hand bone (Bone_LHand, Bone_RHand, Bone_Hand,
// Bone_R_Hand, depending on the rig). To move that weapon's verts without
// caring whether we baked the merge ourselves, we just ask "which verts
// in slot[0] have any non-zero weight pointing at this palette slot?" and
// take the union of all 4 bone-slot bytes. paletteIdx is the index into
// the model's skinOrder array — caller resolves it from a bone NAME by
// looking up model->bones, then scanning model->skinOrder for that bone.
// ---------------------------------------------------------------------------
bool LevelScene::getVertsWeightedToBone(LevelModel*  model,
                                         uint32_t     paletteIdx,
                                         std::vector<uint32_t>& outIndices)
{
    outIndices.clear();
    if (!model || model->parts.empty()) return false;
    const LevelMeshPart& part = model->parts[0];
    if (part.blendIndicesCpu.empty() || part.blendWeightsCpu.empty()) return false;
    if (part.blendIndicesCpu.size() != part.blendWeightsCpu.size()) return false;

    const uint8_t target = (uint8_t)(paletteIdx & 0xFFu);
    const uint32_t n = (uint32_t)part.blendIndicesCpu.size();
    outIndices.reserve(n / 8);  // educated guess

    // Pass 1: strict pair match — (BI byte k, BW byte k) paired at the same
    // slot. Standard D3D9 indexed-vertex-blending convention. Pandemic's
    // shipped CRDs follow it exactly.
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bi = part.blendIndicesCpu[i];
        uint32_t bw = part.blendWeightsCpu[i];
        for (int k = 0; k < 4; ++k) {
            uint8_t b = (uint8_t)((bi >> (k * 8)) & 0xFFu);
            uint8_t w = (uint8_t)((bw >> (k * 8)) & 0xFFu);
            if (b == target && w > 0) {
                outIndices.push_back(i);
                break;
            }
        }
    }
    if (!outIndices.empty()) return true;

    // Pass 2 (fallback): loose match — any BI byte equals target AND any
    // BW byte is non-zero. Handles the M2-era embed bug where weapon
    // verts were written with the bone in BlendIndices byte 0 but the
    // primary weight in BlendWeight byte 2 (mismatched slots). The bow
    // verts on CRD_CH_elf_ancn_bowtest_01 and any other early merge
    // hit this. Skipped for target == 0 because EVERY vert has a 0 in
    // its dud BI bytes — loose match on 0 would select half the mesh.
    if (target == 0) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bi = part.blendIndicesCpu[i];
        uint32_t bw = part.blendWeightsCpu[i];
        if (bw == 0) continue;
        bool boneHit = false;
        for (int k = 0; k < 4; ++k) {
            if (((bi >> (k * 8)) & 0xFFu) == target) { boneHit = true; break; }
        }
        if (boneHit) outIndices.push_back(i);
    }
    return !outIndices.empty();
}

bool LevelScene::snapshotVertsByIndex(LevelModel*  model,
                                       const std::vector<uint32_t>& indices,
                                       std::vector<float>& outOrig)
{
    outOrig.clear();
    if (!model || model->parts.empty() || indices.empty()) return false;
    const LevelMeshPart& part = model->parts[0];
    if (part.positionsCpu.empty()) return false;
    outOrig.resize(indices.size() * 3);
    const uint32_t vMax = (uint32_t)part.vertexCount;
    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t vi = indices[i];
        if (vi >= vMax) {
            outOrig.clear();
            return false;
        }
        outOrig[i*3 + 0] = part.positionsCpu[vi*3 + 0];
        outOrig[i*3 + 1] = part.positionsCpu[vi*3 + 1];
        outOrig[i*3 + 2] = part.positionsCpu[vi*3 + 2];
    }
    return true;
}

bool LevelScene::applyDeltaByIndex(LevelModel*  model,
                                    const std::vector<uint32_t>& indices,
                                    const float* origPositions,
                                    const float  worldDelta[16])
{
    if (!model || model->parts.empty() || indices.empty()) return false;
    if (!origPositions || !worldDelta) return false;
    const LevelMeshPart& part = model->parts[0];
    if (!part.vb) return false;

    void* data = NULL;
    if (FAILED(part.vb->Lock(0, 0, &data, 0))) return false;
    uint8_t* base = (uint8_t*)data;
    const uint32_t vMax = (uint32_t)part.vertexCount;
    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t vi = indices[i];
        if (vi >= vMax) continue;
        float ox = origPositions[i*3 + 0];
        float oy = origPositions[i*3 + 1];
        float oz = origPositions[i*3 + 2];
        float nx = worldDelta[0]*ox + worldDelta[1]*oy + worldDelta[2]*oz  + worldDelta[3];
        float ny = worldDelta[4]*ox + worldDelta[5]*oy + worldDelta[6]*oz  + worldDelta[7];
        float nz = worldDelta[8]*ox + worldDelta[9]*oy + worldDelta[10]*oz + worldDelta[11];
        float* p = (float*)(base + (size_t)vi * LEVEL_VERTEX_SIZE);
        p[0] = nx;
        p[1] = ny;
        p[2] = nz;
    }
    part.vb->Unlock();
    return true;
}

// ---------------------------------------------------------------------------
// pickTriangleOnModel — Möller–Trumbore ray-triangle intersection across
// every triangle in slot[0]. The model's positions are kept in part.
// positionsCpu (mirror populated at load); the index buffer lives in
// part.indicesCpu. Returns the closest forward hit (smallest positive t)
// in outTriIdx / outHitT. Brute force — for a ~4000-triangle CRD this is
// trivial on click.
// ---------------------------------------------------------------------------
bool LevelScene::pickTriangleOnModel(LevelModel*  model,
                                      const float  rayOrigin[3],
                                      const float  rayDir[3],
                                      int*         outTriIdx,
                                      float*       outHitT)
{
    if (outTriIdx) *outTriIdx = -1;
    if (outHitT)   *outHitT = 0.0f;
    if (!model || model->parts.empty()) return false;
    const LevelMeshPart& part = model->parts[0];
    if (part.indicesCpu.empty() || part.positionsCpu.empty()) return false;
    const uint32_t triCount = (uint32_t)part.indicesCpu.size() / 3;
    if (triCount == 0) return false;

    const float* P = &part.positionsCpu[0];
    const uint32_t* IB = &part.indicesCpu[0];
    int   bestTri = -1;
    float bestT   = 1e30f;
    const float eps = 1e-7f;

    for (uint32_t t = 0; t < triCount; ++t) {
        uint32_t i0 = IB[t*3 + 0];
        uint32_t i1 = IB[t*3 + 1];
        uint32_t i2 = IB[t*3 + 2];
        if ((size_t)i0 * 3 + 2 >= part.positionsCpu.size()) continue;
        if ((size_t)i1 * 3 + 2 >= part.positionsCpu.size()) continue;
        if ((size_t)i2 * 3 + 2 >= part.positionsCpu.size()) continue;

        const float* v0 = &P[i0 * 3];
        const float* v1 = &P[i1 * 3];
        const float* v2 = &P[i2 * 3];

        // Möller–Trumbore
        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float pvec[3] = {
            rayDir[1]*e2[2] - rayDir[2]*e2[1],
            rayDir[2]*e2[0] - rayDir[0]*e2[2],
            rayDir[0]*e2[1] - rayDir[1]*e2[0]
        };
        float det = e1[0]*pvec[0] + e1[1]*pvec[1] + e1[2]*pvec[2];
        if (det > -eps && det < eps) continue;  // ray parallel
        float invDet = 1.0f / det;
        float tvec[3] = { rayOrigin[0]-v0[0], rayOrigin[1]-v0[1], rayOrigin[2]-v0[2] };
        float u = (tvec[0]*pvec[0] + tvec[1]*pvec[1] + tvec[2]*pvec[2]) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        float qvec[3] = {
            tvec[1]*e1[2] - tvec[2]*e1[1],
            tvec[2]*e1[0] - tvec[0]*e1[2],
            tvec[0]*e1[1] - tvec[1]*e1[0]
        };
        float v = (rayDir[0]*qvec[0] + rayDir[1]*qvec[1] + rayDir[2]*qvec[2]) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        float tt = (e2[0]*qvec[0] + e2[1]*qvec[1] + e2[2]*qvec[2]) * invDet;
        if (tt > eps && tt < bestT) {
            bestT = tt;
            bestTri = (int)t;
        }
    }

    if (bestTri < 0) return false;
    if (outTriIdx) *outTriIdx = bestTri;
    if (outHitT)   *outHitT = bestT;
    return true;
}

// ---------------------------------------------------------------------------
// applyWorldTransformToVerts — locks slot[0]'s VB and writes new positions
// for an arbitrary index list. Same lock-write-unlock dance as
// applyDeltaByIndex / applyEmbeddedWeaponDelta, just exposed under a name
// that reads as "world-space transform" since the triangle picker doesn't
// do bone-local conjugation.
// ---------------------------------------------------------------------------
bool LevelScene::applyWorldTransformToVerts(LevelModel*  model,
                                             const std::vector<uint32_t>& indices,
                                             const float* origPositions,
                                             const float  worldMat[16])
{
    // Same as applyDeltaByIndex — keep one implementation by delegating.
    return applyDeltaByIndex(model, indices, origPositions, worldMat);
}

bool LevelScene::getBindWorldForPaletteIdx(LevelModel* model,
                                            uint32_t    paletteIdx,
                                            float       outBindWorld[16])
{
    if (!model || !outBindWorld) return false;
    if (paletteIdx >= model->skinOrder.size()) return false;
    if ((size_t)(paletteIdx + 1) * 16 > model->skinBindsRowMajor.size()) return false;
    // skinBindsRowMajor stores the INVERSE bind for each palette slot.
    // bind_world = inverse(inv_bind). Both are rigid (rotation+translation
    // only), no scale, so affine-inverse handles it.
    const float* inv = &model->skinBindsRowMajor[paletteIdx * 16];
    outBindWorld[0]  = inv[0];  outBindWorld[1]  = inv[4];  outBindWorld[2]  = inv[8];
    outBindWorld[4]  = inv[1];  outBindWorld[5]  = inv[5];  outBindWorld[6]  = inv[9];
    outBindWorld[8]  = inv[2];  outBindWorld[9]  = inv[6];  outBindWorld[10] = inv[10];
    float tx = inv[3], ty = inv[7], tz = inv[11];
    outBindWorld[3]  = -(outBindWorld[0]*tx + outBindWorld[1]*ty + outBindWorld[2]*tz);
    outBindWorld[7]  = -(outBindWorld[4]*tx + outBindWorld[5]*ty + outBindWorld[6]*tz);
    outBindWorld[11] = -(outBindWorld[8]*tx + outBindWorld[9]*ty + outBindWorld[10]*tz);
    outBindWorld[12] = 0; outBindWorld[13] = 0; outBindWorld[14] = 0; outBindWorld[15] = 1;
    return true;
}

// ---------------------------------------------------------------------------
// Extract all level triangles in world space for physics collision.
// Locks D3D VB/IB, reads positions, transforms by instance matrix.
// ---------------------------------------------------------------------------
bool LevelScene::getCollisionTriangles(std::vector<float>& outPos,
                                        std::vector<int>&   outIdx) const
{
    outPos.clear();
    outIdx.clear();
    if (!m_loaded) return false;

    for (size_t inst = 0; inst < m_instances.size(); ++inst)
    {
        const LevelInstance& li = m_instances[inst];
        if (!li.model) continue;
        // Load world matrix once per instance (game-accurate SSE path: FUN_0040365f)
        ZMat4x4 zWorld;
        zWorld.load(li.mat);

        for (size_t pi = 0; pi < li.model->parts.size(); ++pi)
        {
            const LevelMeshPart& part = li.model->parts[pi];
            if (!part.vb || !part.ib || part.vertexCount <= 0 || part.indexCount < 3)
                continue;

            int baseVert = (int)(outPos.size() / 3);

            // Lock VB (read-only) — LevelVertex: pos(12) normal(12) color(4) uv(8)
            void* vRaw = NULL;
            if (FAILED(part.vb->Lock(0, 0, &vRaw, D3DLOCK_READONLY)))
                continue;
            const LevelVertex* verts = (const LevelVertex*)vRaw;

            outPos.reserve(outPos.size() + part.vertexCount * 3);
            for (int v = 0; v < part.vertexCount; ++v)
            {
                // row-vector * row-major: pos * World (same as FUN_0040365f)
                ZVec4 lp(verts[v].x, verts[v].y, verts[v].z, 1.0f);
                ZVec4 wp = ZTransformPoint(&lp, &zWorld);
                outPos.push_back(wp.x);
                outPos.push_back(wp.y);
                outPos.push_back(wp.z);
            }
            part.vb->Unlock();

            // Lock IB (read-only) — 16-bit indices
            void* iRaw = NULL;
            if (FAILED(part.ib->Lock(0, 0, &iRaw, D3DLOCK_READONLY)))
                continue;
            const unsigned short* indices = (const unsigned short*)iRaw;

            outIdx.reserve(outIdx.size() + part.indexCount);
            for (int i = 0; i < part.indexCount; ++i)
                outIdx.push_back(baseVert + (int)indices[i]);
            part.ib->Unlock();
        }
    }
    return !outPos.empty();
}

// =============================================================================
//  Object picking — ray vs AABB
// =============================================================================

static bool RayAABB(const float o[3], const float d[3],
                    const float bmin[3], const float bmax[3], float& tOut)
{
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (d[i] != 0.0f) {
            float invD = 1.0f / d[i];
            float t1 = (bmin[i] - o[i]) * invD;
            float t2 = (bmax[i] - o[i]) * invD;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
        } else {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return false;
        }
    }
    if (tmin > tmax || tmax < 0.0f) return false;
    tOut = tmin >= 0.0f ? tmin : tmax;
    return true;
}

int LevelScene::pickInstance(const float rayOrigin[3], const float rayDir[3]) const
{
    int best = -1;
    float bestT = 1e30f;
    const float MIN_HALF = 1.0f; // minimum half-extent to prevent degenerate/tiny AABBs
    for (int i = 0; i < (int)m_instances.size(); ++i) {
        const LevelInstance& inst = m_instances[i];
        // Crowd Mesh Builder previews are visual-only — they must NOT
        // grab the cursor away from real picks underneath them. The
        // Builder panel owns them, the rest of the editor pretends
        // they aren't there.
        if (inst.previewParentIdx >= 0) continue;
        // Phase 3 XSI layer manager: hidden layers and locked layers
        // are unpickable. Hidden because the user can't see them, so a
        // ray landing there would be a goddamn ghost click. Locked
        // because the user explicitly told us "do not let me move this
        // shit", and respecting that is the whole reason locking
        // exists. Isolated layer wins both ways: anything else cannot
        // be picked while solo is active.
        if (inst.layerGuid != 0) {
            if (!layerVisibleForRender(inst.layerGuid)) continue;
            if (layerLocked(inst.layerGuid)) continue;
        }
        // Inflate degenerate AABBs so flat/tiny objects remain pickable
        float bmin[3], bmax[3];
        for (int a = 0; a < 3; ++a) {
            float half = (inst.bboxMax[a] - inst.bboxMin[a]) * 0.5f;
            float center = (inst.bboxMax[a] + inst.bboxMin[a]) * 0.5f;
            if (half < MIN_HALF) half = MIN_HALF;
            bmin[a] = center - half;
            bmax[a] = center + half;
        }
        float t;
        if (RayAABB(rayOrigin, rayDir, bmin, bmax, t)) {
            if (t < bestT) { bestT = t; best = i; }
        }
    }
    return best;
}

bool LevelScene::raycastScene(const float rayOrigin[3], const float rayDir[3], float outPos[3]) const
{
    float bestT = 1e30f;
    for (int i = 0; i < (int)m_instances.size(); ++i) {
        const LevelInstance& inst = m_instances[i];
        float t;
        if (RayAABB(rayOrigin, rayDir, inst.bboxMin, inst.bboxMax, t)) {
            if (t < bestT) bestT = t;
        }
    }
    if (bestT < 1e29f) {
        outPos[0] = rayOrigin[0] + rayDir[0] * bestT;
        outPos[1] = rayOrigin[1] + rayDir[1] * bestT;
        outPos[2] = rayOrigin[2] + rayDir[2] * bestT;
        return true;
    }
    return false;
}

bool LevelScene::getModelCollisionData(uint32_t meshCrc,
                                        std::vector<float>& outVerts,
                                        std::vector<uint16_t>& outIndices) const
{
    outVerts.clear();
    outIndices.clear();

    std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(meshCrc);
    if (it == m_modelCache.end() || !it->second) return false;

    const LevelModel* model = it->second;
    uint16_t baseVertex = 0;

    for (int pi = 0; pi < (int)model->parts.size(); ++pi) {
        const LevelMeshPart& part = model->parts[pi];
        if (!part.vb || !part.ib || part.vertexCount == 0 || part.indexCount == 0) continue;

        // Lock VB read-only and extract positions
        void* vbData = NULL;
        if (FAILED(part.vb->Lock(0, 0, &vbData, D3DLOCK_READONLY))) continue;

        const uint8_t* vptr = reinterpret_cast<const uint8_t*>(vbData);
        int stride = sizeof(LevelVertex); // 36 bytes
        for (int vi = 0; vi < part.vertexCount; ++vi) {
            const float* pos = reinterpret_cast<const float*>(vptr + vi * stride);
            outVerts.push_back(pos[0]); // x
            outVerts.push_back(pos[1]); // y
            outVerts.push_back(pos[2]); // z
        }
        part.vb->Unlock();

        // Lock IB read-only and copy indices with base offset
        void* ibData = NULL;
        if (FAILED(part.ib->Lock(0, 0, &ibData, D3DLOCK_READONLY))) {
            // Rollback verts
            outVerts.resize(outVerts.size() - part.vertexCount * 3);
            continue;
        }

        const uint16_t* iptr = reinterpret_cast<const uint16_t*>(ibData);
        for (int ii = 0; ii < part.indexCount; ++ii) {
            outIndices.push_back(iptr[ii] + baseVertex);
        }
        part.ib->Unlock();

        baseVertex += (uint16_t)part.vertexCount;
    }

    return !outVerts.empty() && !outIndices.empty();
}

int LevelScene::pickEditorObj(const float rayOrigin[3], const float rayDir[3], float* outDist) const
{
    if (!m_showEditorObjs) return -1;
    int best = -1;
    float bestT = 1e30f;
    for (int i = 0; i < (int)m_editorObjs.size(); ++i)
    {
        const LevelEditorObj& eo = m_editorObjs[i];
        char lbl[8]; DWORD col;
        float pickAlpha = shouldDrawEditorObj(i, rayOrigin, lbl, col);
        if (pickAlpha <= 0.0f) continue;

        float cx = eo.mat[12], cy = eo.mat[13], cz = eo.mat[14];
        // Scale pick radius with distance from camera so far objects stay clickable
        float dx = rayOrigin[0] - cx, dy = rayOrigin[1] - cy, dz = rayOrigin[2] - cz;
        float dist = sqrtf(dx*dx + dy*dy + dz*dz);
        // Min 2.0, scales up to ~5% of distance, capped at 30
        float pickR = dist * 0.05f;
        if (pickR < 2.0f) pickR = 2.0f;
        if (pickR > 30.0f) pickR = 30.0f;
        // Also use the editor outer/size if larger
        if (eo.outer > pickR) pickR = eo.outer;

        // Ray-sphere test
        float b = dx*rayDir[0] + dy*rayDir[1] + dz*rayDir[2];
        float c = dx*dx + dy*dy + dz*dz - pickR*pickR;
        float disc = b*b - c;
        if (disc < 0) continue;
        float t = -b - sqrtf(disc);
        if (t < 0) t = -b + sqrtf(disc);
        if (t < 0) continue;
        if (t < bestT) { bestT = t; best = i; }
    }
    if (outDist) *outDist = bestT;
    return best;
}

// ── Asset tree → map highlight methods ──────────────────────────────────────
void LevelScene::setAssetHighlight(const int* indices, int count) {
    m_assetHighlightIndices.assign(indices, indices + count);
}
void LevelScene::clearAssetHighlight() {
    m_assetHighlightIndices.clear();
}
void LevelScene::findInstancesByModelCrc(uint32_t modelCrc, std::vector<int>& out) const {
    out.clear();
    std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(modelCrc);
    if (it == m_modelCache.end()) return;
    LevelModel* target = it->second;
    for (int i = 0; i < (int)m_instances.size(); ++i) {
        if (m_instances[i].model == target)
            out.push_back(i);
    }
}
void LevelScene::findInstancesByTextureCrc(uint32_t texCrc, std::vector<int>& out) const {
    out.clear();
    for (int i = 0; i < (int)m_instances.size(); ++i) {
        const LevelModel* mdl = m_instances[i].model;
        if (!mdl) continue;
        for (int p = 0; p < (int)mdl->parts.size(); ++p) {
            if (mdl->parts[p].texCrc == texCrc) {
                out.push_back(i);
                break; // one match is enough for this instance
            }
        }
    }
}

void LevelScene::screenToRay(int screenX, int screenY, int vpW, int vpH,
                              float outOrigin[3], float outDir[3]) const
{
    // Use cached ViewProj from last render (avoids stale D3D transforms)
    float ndcX =  (2.0f * screenX / vpW - 1.0f);
    float ndcY = -(2.0f * screenY / vpH - 1.0f);

    D3DXMATRIX vp(m_cachedVP[0], m_cachedVP[1], m_cachedVP[2], m_cachedVP[3],
                  m_cachedVP[4], m_cachedVP[5], m_cachedVP[6], m_cachedVP[7],
                  m_cachedVP[8], m_cachedVP[9], m_cachedVP[10],m_cachedVP[11],
                  m_cachedVP[12],m_cachedVP[13],m_cachedVP[14],m_cachedVP[15]);
    D3DXMATRIX ivp;
    D3DXMatrixInverse(&ivp, NULL, &vp);

    D3DXVECTOR4 nearPt, farPt;
    D3DXVECTOR4 nearNDC(ndcX, ndcY, 0.0f, 1.0f);
    D3DXVECTOR4 farNDC(ndcX, ndcY, 1.0f, 1.0f);
    D3DXVec4Transform(&nearPt, &nearNDC, &ivp);
    D3DXVec4Transform(&farPt,  &farNDC,  &ivp);

    if (nearPt.w != 0.0f) { nearPt.x /= nearPt.w; nearPt.y /= nearPt.w; nearPt.z /= nearPt.w; }
    if (farPt.w  != 0.0f) { farPt.x  /= farPt.w;  farPt.y  /= farPt.w;  farPt.z  /= farPt.w;  }

    outOrigin[0] = nearPt.x; outOrigin[1] = nearPt.y; outOrigin[2] = nearPt.z;
    float dx = farPt.x - nearPt.x, dy = farPt.y - nearPt.y, dz = farPt.z - nearPt.z;
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
    outDir[0] = dx; outDir[1] = dy; outDir[2] = dz;
}

// ═════════════════════════════════════════════════════════════════════
//  PHASE 4 XSI AUTHORING: VIEWPORT GIZMO MATH + STATE MACHINE
// ═════════════════════════════════════════════════════════════════════
//
// XSI / Maya / Blender / 3ds Max have spent thirty years building these
// goddamn manipulators. We have spent two days. The math is the math:
// closest point on an infinite line to a ray, ray-plane intersect to
// extract an angle around an axis, signed scale ratio along an axis.
// What we DON'T do here is the fancy quad-corner uniform-scale handle
// or the screen-space view-aligned rotate ring. Three primary axes for
// each mode covers the 99% case of "nudge this spawn point a meter
// north" or "rotate that AI goal forty-five degrees". Power users can
// keep using the inspector DragFloat fields for the precise stuff.
//
// Live drag pipeline:
//   gizmoBeginDrag  - LMB hits an axis handle, snapshot the entity's
//                     world matrix, record click projection / angle.
//   gizmoUpdateDrag - WM_MOUSEMOVE recomputes axis-aligned delta and
//                     stamps a new matrix onto the live LevelInstance
//                     or LevelEditorObj. NO FieldEdit yet, that would
//                     spam the save log with a thousand intermediate
//                     edits.
//   gizmoEndDrag    - LMB-up. Compute the final delta vs the snapshot,
//                     write ONE FieldEdit("WorldTransform", kind=4) so
//                     the change survives SavePak.
//   gizmoCancelDrag - escape hatch (Esc key etc.). Restore the
//                     snapshot, drop the drag state.

static inline float gizmoVecDot(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static inline float gizmoVecLen(const float a[3]) {
    return sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
}
static inline void gizmoVecNorm(float v[3]) {
    float L = gizmoVecLen(v);
    if (L > 1e-8f) { v[0]/=L; v[1]/=L; v[2]/=L; }
}

static void gizmoLineRayClosest(const float P[3], const float L[3],
                                const float O[3], const float D[3],
                                float& outT, float& outDistSq)
{
    float w[3] = { P[0]-O[0], P[1]-O[1], P[2]-O[2] };
    float a = gizmoVecDot(L, L);
    float b = gizmoVecDot(L, D);
    float c = gizmoVecDot(D, D);
    float d = gizmoVecDot(L, w);
    float e = gizmoVecDot(D, w);
    float denom = a*c - b*b;
    if (denom < 1e-6f) { outT = 0.0f; outDistSq = 1e30f; return; }
    float t = (b*e - c*d) / denom;
    float s = (a*e - b*d) / denom;
    float closestLine[3] = { P[0]+t*L[0], P[1]+t*L[1], P[2]+t*L[2] };
    float closestRay[3]  = { O[0]+s*D[0], O[1]+s*D[1], O[2]+s*D[2] };
    float dx = closestLine[0]-closestRay[0];
    float dy = closestLine[1]-closestRay[1];
    float dz = closestLine[2]-closestRay[2];
    outT = t;
    outDistSq = dx*dx + dy*dy + dz*dz;
}

static bool gizmoRayPlane(const float O[3], const float D[3],
                          const float P[3], const float N[3],
                          float outHit[3])
{
    float denom = gizmoVecDot(N, D);
    if (fabsf(denom) < 1e-6f) return false;
    float w[3] = { P[0]-O[0], P[1]-O[1], P[2]-O[2] };
    float t = gizmoVecDot(N, w) / denom;
    if (t < 0.0f) return false;
    outHit[0] = O[0] + t*D[0];
    outHit[1] = O[1] + t*D[1];
    outHit[2] = O[2] + t*D[2];
    return true;
}

static void gizmoGetAxes(const float mat[16], int space,
                         float outX[3], float outY[3], float outZ[3])
{
    if (space == 1) {
        outX[0]=mat[0]; outX[1]=mat[1]; outX[2]=mat[2];
        outY[0]=mat[4]; outY[1]=mat[5]; outY[2]=mat[6];
        outZ[0]=mat[8]; outZ[1]=mat[9]; outZ[2]=mat[10];
        gizmoVecNorm(outX); gizmoVecNorm(outY); gizmoVecNorm(outZ);
    } else {
        outX[0]=1; outX[1]=0; outX[2]=0;
        outY[0]=0; outY[1]=1; outY[2]=0;
        outZ[0]=0; outZ[1]=0; outZ[2]=1;
    }
}

static float gizmoSizeAt(const float origin[3], const float camPos[3]) {
    float dx = origin[0]-camPos[0], dy = origin[1]-camPos[1], dz = origin[2]-camPos[2];
    float dist = sqrtf(dx*dx + dy*dy + dz*dz);
    return dist * 0.10f + 0.5f;
}

bool LevelScene::gizmoGetTargetMatrix(float outMat[16]) const
{
    if (m_selectedIdx >= 0 && m_selectedIdx < (int)m_instances.size()) {
        memcpy(outMat, m_instances[m_selectedIdx].mat, sizeof(float)*16);
        return true;
    }
    if (m_selectedEditorIdx >= 0 && m_selectedEditorIdx < (int)m_editorObjs.size()) {
        memcpy(outMat, m_editorObjs[m_selectedEditorIdx].mat, sizeof(float)*16);
        return true;
    }
    return false;
}

bool LevelScene::gizmoBeginDrag(const float rayOrigin[3], const float rayDir[3])
{
    if (m_gizmoMode == 0) return false;
    float curMat[16];
    if (!gizmoGetTargetMatrix(curMat)) return false;

    if (m_selectedIdx >= 0 && m_selectedIdx < (int)m_instances.size())
        m_gizmoTargetKind = 0;
    else
        m_gizmoTargetKind = 1;

    float origin[3] = { curMat[12], curMat[13], curMat[14] };
    float ax[3], ay[3], az[3];
    gizmoGetAxes(curMat, m_gizmoSpace, ax, ay, az);
    float* axisTable[3] = { ax, ay, az };

    float camPos[3] = { rayOrigin[0], rayOrigin[1], rayOrigin[2] };
    float gSize = gizmoSizeAt(origin, camPos);
    float pickThresh = gSize * 0.25f;
    pickThresh *= pickThresh;

    if (m_gizmoMode == 1) {
        int bestAxis = 0;
        float bestDist = 1e30f;
        float bestT = 0.0f;
        for (int a = 0; a < 3; ++a) {
            float t, dsq;
            gizmoLineRayClosest(origin, axisTable[a], rayOrigin, rayDir, t, dsq);
            if (t < 0.0f || t > gSize) continue;
            if (dsq < bestDist) { bestDist = dsq; bestT = t; bestAxis = a + 1; }
        }
        if (bestAxis == 0 || bestDist > pickThresh) return false;
        m_gizmoActiveAxis = bestAxis;
        m_gizmoDragStartProj = bestT;
        memcpy(m_gizmoStartMat, curMat, sizeof(curMat));
        m_gizmoDragging = true;
        return true;
    }
    if (m_gizmoMode == 2) {
        int bestAxis = 0;
        float bestRingDist = 1e30f;
        float bestAngle = 0.0f;
        for (int a = 0; a < 3; ++a) {
            float* N = axisTable[a];
            float hit[3];
            if (!gizmoRayPlane(rayOrigin, rayDir, origin, N, hit)) continue;
            float v[3] = { hit[0]-origin[0], hit[1]-origin[1], hit[2]-origin[2] };
            float r = gizmoVecLen(v);
            float ringR = gSize * 0.85f;
            float ringTol = gSize * 0.25f;
            if (fabsf(r - ringR) > ringTol) continue;
            int u = (a + 1) % 3;
            int w = (a + 2) % 3;
            float* uV = axisTable[u];
            float* wV = axisTable[w];
            float angle = atan2f(gizmoVecDot(v, wV), gizmoVecDot(v, uV));
            float ringDist = fabsf(r - ringR);
            if (ringDist < bestRingDist) {
                bestRingDist = ringDist;
                bestAngle = angle;
                bestAxis = a + 1;
            }
        }
        if (bestAxis == 0) return false;
        m_gizmoActiveAxis = bestAxis;
        m_gizmoDragStartAngle = bestAngle;
        memcpy(m_gizmoStartMat, curMat, sizeof(curMat));
        m_gizmoDragging = true;
        return true;
    }
    if (m_gizmoMode == 3) {
        int bestAxis = 0;
        float bestDist = 1e30f;
        float bestT = 0.0f;
        for (int a = 0; a < 3; ++a) {
            float t, dsq;
            gizmoLineRayClosest(origin, axisTable[a], rayOrigin, rayDir, t, dsq);
            if (t < 0.0f || t > gSize) continue;
            if (dsq < bestDist) { bestDist = dsq; bestT = t; bestAxis = a + 1; }
        }
        if (bestAxis == 0 || bestDist > pickThresh) return false;
        m_gizmoActiveAxis = bestAxis;
        m_gizmoDragStartDist = (bestT < 0.05f) ? 0.05f : bestT;
        memcpy(m_gizmoStartMat, curMat, sizeof(curMat));
        m_gizmoDragging = true;
        return true;
    }
    return false;
}

void LevelScene::gizmoUpdateDrag(const float rayOrigin[3], const float rayDir[3])
{
    if (!m_gizmoDragging || m_gizmoActiveAxis == 0) return;
    int axisIdx = m_gizmoActiveAxis - 1;
    float ax[3], ay[3], az[3];
    gizmoGetAxes(m_gizmoStartMat, m_gizmoSpace, ax, ay, az);
    float* axisTable[3] = { ax, ay, az };
    float* axisDir = axisTable[axisIdx];
    float origin[3] = { m_gizmoStartMat[12], m_gizmoStartMat[13], m_gizmoStartMat[14] };

    float newMat[16];
    memcpy(newMat, m_gizmoStartMat, sizeof(newMat));

    if (m_gizmoMode == 1) {
        float t, dsq;
        gizmoLineRayClosest(origin, axisDir, rayOrigin, rayDir, t, dsq);
        float delta = t - m_gizmoDragStartProj;
        if (m_gizmoSnap > 0.0f) {
            delta = floorf(delta / m_gizmoSnap + 0.5f) * m_gizmoSnap;
        }
        newMat[12] = m_gizmoStartMat[12] + axisDir[0]*delta;
        newMat[13] = m_gizmoStartMat[13] + axisDir[1]*delta;
        newMat[14] = m_gizmoStartMat[14] + axisDir[2]*delta;
    } else if (m_gizmoMode == 2) {
        float* N = axisDir;
        float hit[3];
        if (!gizmoRayPlane(rayOrigin, rayDir, origin, N, hit)) return;
        float v[3] = { hit[0]-origin[0], hit[1]-origin[1], hit[2]-origin[2] };
        int u = (axisIdx + 1) % 3;
        int w = (axisIdx + 2) % 3;
        float angle = atan2f(gizmoVecDot(v, axisTable[w]), gizmoVecDot(v, axisTable[u]));
        float delta = angle - m_gizmoDragStartAngle;
        if (m_gizmoSnap > 0.0f) {
            float snapRad = m_gizmoSnap * 0.0174532925f;
            delta = floorf(delta / snapRad + 0.5f) * snapRad;
        }
        float c = cosf(delta), s = sinf(delta);
        float t1 = 1.0f - c;
        float x = N[0], y = N[1], z = N[2];
        float R[9] = {
            t1*x*x + c,    t1*x*y - s*z,  t1*x*z + s*y,
            t1*x*y + s*z,  t1*y*y + c,    t1*y*z - s*x,
            t1*x*z - s*y,  t1*y*z + s*x,  t1*z*z + c
        };
        const float* M = m_gizmoStartMat;
        float NR[9];
        for (int r = 0; r < 3; ++r) {
            for (int cc = 0; cc < 3; ++cc) {
                NR[r*3 + cc] =
                    R[r*3+0] * M[0*4 + cc] +
                    R[r*3+1] * M[1*4 + cc] +
                    R[r*3+2] * M[2*4 + cc];
            }
        }
        newMat[0]=NR[0]; newMat[1]=NR[1]; newMat[2]=NR[2];
        newMat[4]=NR[3]; newMat[5]=NR[4]; newMat[6]=NR[5];
        newMat[8]=NR[6]; newMat[9]=NR[7]; newMat[10]=NR[8];
    } else if (m_gizmoMode == 3) {
        float t, dsq;
        gizmoLineRayClosest(origin, axisDir, rayOrigin, rayDir, t, dsq);
        if (m_gizmoDragStartDist == 0.0f) return;
        float ratio = t / m_gizmoDragStartDist;
        if (m_gizmoSnap > 0.0f) {
            ratio = floorf(ratio / m_gizmoSnap + 0.5f) * m_gizmoSnap;
        }
        if (ratio > -0.01f && ratio < 0.01f) ratio = (ratio < 0.0f) ? -0.01f : 0.01f;
        int row = axisIdx;
        for (int cc = 0; cc < 3; ++cc) {
            newMat[row*4 + cc] = m_gizmoStartMat[row*4 + cc] * ratio;
        }
    }

    if (m_gizmoTargetKind == 0 && m_selectedIdx >= 0 &&
        m_selectedIdx < (int)m_instances.size()) {
        memcpy(m_instances[m_selectedIdx].mat, newMat, sizeof(newMat));
    } else if (m_gizmoTargetKind == 1 && m_selectedEditorIdx >= 0 &&
               m_selectedEditorIdx < (int)m_editorObjs.size()) {
        memcpy(m_editorObjs[m_selectedEditorIdx].mat, newMat, sizeof(newMat));
    }
}

uint32_t LevelScene::gizmoEndDrag(ZeroEngine::LevelReader* reader)
{
    if (!m_gizmoDragging) return 0;
    m_gizmoDragging = false;
    m_gizmoActiveAxis = 0;
    if (!reader) return 0;

    float curMat[16];
    uint32_t guid = 0;
    uint32_t wtOff = 0;
    if (m_gizmoTargetKind == 0 && m_selectedIdx >= 0 &&
        m_selectedIdx < (int)m_instances.size()) {
        const LevelInstance& inst = m_instances[m_selectedIdx];
        memcpy(curMat, inst.mat, sizeof(curMat));
        guid = inst.guid;
        wtOff = inst.block1WtOffset;
    } else if (m_gizmoTargetKind == 1 && m_selectedEditorIdx >= 0 &&
               m_selectedEditorIdx < (int)m_editorObjs.size()) {
        const LevelEditorObj& eo = m_editorObjs[m_selectedEditorIdx];
        memcpy(curMat, eo.mat, sizeof(curMat));
        guid = eo.guid;
        wtOff = eo.block1WtOffset;
    } else {
        return 0;
    }

    bool changed = false;
    for (int i = 0; i < 16; ++i) {
        if (fabsf(curMat[i] - m_gizmoStartMat[i]) > 1e-5f) { changed = true; break; }
    }
    if (!changed) return 0;

    // ── 3dCrowd writeback short-circuit ──
    // If the dragged thing is a crowd member, the FieldEdit pipeline does
    // not apply (crowd lives in Block2, not Block1, and isn't a GameObj
    // entity — there's no entityGuid the patcher could resolve). Route
    // the new position + Y-yaw back into m_crowdItems and bail before
    // any FieldEdit / SetWorldTransform attempt runs.
    if (m_gizmoTargetKind == 0 && m_selectedIdx >= 0 &&
        m_selectedIdx < (int)m_instances.size()) {
        const LevelInstance& selInst = m_instances[m_selectedIdx];
        if (selInst.crowdItemIdx >= 0 && selInst.crowdValIdx >= 0) {
            // Extract yaw from the curMat row-0 / row-2 elements. Crowd
            // characters stand upright (no pitch / no roll), so atan2 on
            // -mat[2] vs mat[0] is the canonical inverse of the Y-rotation
            // we BUILT into the matrix at load time. Translation is just
            // the position column.
            float yaw = atan2f(-curMat[2], curMat[0]);
            float px  = curMat[12], py = curMat[13], pz = curMat[14];
            setCrowdInstancePosRot(selInst.crowdItemIdx, selInst.crowdValIdx,
                                   px, py, pz, yaw);
            // No FieldEdit log — save pipeline reads m_crowdItems directly.
            return 0;
        }
    }

    if (wtOff != 0) {
        reader->SetWorldTransform(wtOff, curMat);
    }
    ZeroEngine::FieldEdit fe;
    memset(&fe, 0, sizeof(fe));
    fe.entityGuid = guid;
    fe.fieldName  = "WorldTransform";
    fe.kind       = 4;
    memcpy(fe.matrixVal, curMat, sizeof(curMat));
    reader->AddFieldEdit(fe);
    return guid;
}

void LevelScene::gizmoCancelDrag()
{
    if (!m_gizmoDragging) return;
    if (m_gizmoTargetKind == 0 && m_selectedIdx >= 0 &&
        m_selectedIdx < (int)m_instances.size()) {
        memcpy(m_instances[m_selectedIdx].mat, m_gizmoStartMat, sizeof(m_gizmoStartMat));
    } else if (m_gizmoTargetKind == 1 && m_selectedEditorIdx >= 0 &&
               m_selectedEditorIdx < (int)m_editorObjs.size()) {
        memcpy(m_editorObjs[m_selectedEditorIdx].mat, m_gizmoStartMat, sizeof(m_gizmoStartMat));
    }
    m_gizmoDragging = false;
    m_gizmoActiveAxis = 0;
}

// =========================================================================
//  3dCrowd authoring API — the place where crowds stop being read-only
//  decor and start being something the designer can actually edit.
//
//  For years this engine treated 3dCrowd like a sealed binary tomb: load
//  it, render it, never touch it again. Not anymore. LevelScene owns the
//  parsed data now (m_crowdItems), every LevelInstance pushed by the
//  crowd loader carries a back-reference (crowdItemIdx, crowdValIdx),
//  gizmo drags route the new pos/rot back into the source-of-truth array,
//  and the dump function spits the whole block back out as the JSON shape
//  the Rust parser eats. Round-trip complete. Pandemic's frozen wall of
//  spearmen is editable now.
//
//  Why these helpers live at the BOTTOM of the file: the layout-end
//  commandment runs on m_crowdItems / m_crowdDirty (data members were
//  appended at the end of the class declaration). Keeping the code in
//  one fenced section at the END of the .cpp makes partial-recompile
//  drift easier to catch — if you see this comment, you know nothing
//  above it depends on the crowd structs.
// =========================================================================

bool LevelScene::setCrowdInstancePosRot(int item, int val,
                                         float x, float y, float z, float rotY)
{
    if (!isValidCrowdInstance(item, val)) return false;
    LevelCrowdInstance& ci = m_crowdItems[item].instances[val];
    ci.position[0] = x;
    ci.position[1] = y;
    ci.position[2] = z;
    ci.rotation    = rotY;
    m_crowdDirty   = true;
    return true;
}

int LevelScene::addCrowdInstanceToItem(int item, float x, float y, float z,
                                        float rotY, float lod)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return -1;
    LevelCrowdInstance ci;
    ci.position[0] = x;
    ci.position[1] = y;
    ci.position[2] = z;
    ci.rotation    = rotY;
    ci.lod         = lod;
    m_crowdItems[item].instances.push_back(ci);
    m_crowdDirty   = true;
    return (int)m_crowdItems[item].instances.size() - 1;
}

bool LevelScene::deleteCrowdInstance(int item, int val)
{
    if (!isValidCrowdInstance(item, val)) return false;
    std::vector<LevelCrowdInstance>& vs = m_crowdItems[item].instances;
    vs.erase(vs.begin() + val);
    m_crowdDirty = true;
    return true;
}

int LevelScene::addCrowdItem(uint32_t modelCrc, const std::string& modelName,
                              uint32_t meshCrc,  const std::string& meshName,
                              uint32_t rightCrc, const std::string& rightName,
                              uint32_t leftCrc,  const std::string& leftName,
                              float playbackRate)
{
    LevelCrowdItem item;
    item.modelKey      = modelCrc;
    item.modelKeyName  = modelName;
    item.meshKey       = meshCrc;
    item.meshKeyName   = meshName;
    item.rightHandKey  = rightCrc;
    item.rightHandName = rightName;
    item.leftHandKey   = leftCrc;
    item.leftHandName  = leftName;
    item.playbackRate  = playbackRate;
    m_crowdItems.push_back(item);
    m_crowdDirty = true;
    return (int)m_crowdItems.size() - 1;
}

// Drops every crowd-tagged LevelInstance and re-pushes from m_crowdItems.
// Non-crowd LevelInstances (regular entities, kit spawns) survive untouched.
// Selection / hover indices that pointed at now-gone entries get cleared so
// the next picker frame doesn't index-out-of-bounds.
void LevelScene::rebuildCrowdInstances()
{
    for (int i = (int)m_instances.size() - 1; i >= 0; --i) {
        if (m_instances[i].crowdItemIdx >= 0) {
            m_instances.erase(m_instances.begin() + i);
        }
    }
    if (m_selectedIdx >= (int)m_instances.size()) m_selectedIdx = -1;
    if (m_hoveredIdx  >= (int)m_instances.size()) m_hoveredIdx  = -1;

    for (size_t ci = 0; ci < m_crowdItems.size(); ++ci) {
        // 3dCrowd Editor focus mode — when on, gate out every item except
        // the focused one so the user can inspect one archetype at a time
        // without the other 6 crowding the viewport. Toggling it triggers
        // a rebuild and the skipped ones come back next pass.
        if (m_crowdFocusModeOn && (int)ci != m_crowdFocusedItem) continue;

        LevelCrowdItem& item = m_crowdItems[ci];

        LevelModel* model = NULL;
        std::map<uint32_t, LevelModel*>::iterator cit = m_modelCache.find(item.modelKey);
        if (cit != m_modelCache.end()) model = cit->second;

        // NOTE: a crowd item added at runtime via addCrowdItem() whose
        // modelKey is not already in the model cache will skip rendering
        // until something else (a level reload, or a deliberate
        // loadModelFromBinary pass) populates the cache. m_crowdItems
        // still captures it for save round-trip — only the draw side is
        // affected.
        if (!model) continue;

        for (size_t vi = 0; vi < item.instances.size(); ++vi) {
            const LevelCrowdInstance& cv = item.instances[vi];
            float c = cosf(cv.rotation), s = sinf(cv.rotation);
            LevelInstance inst;
            inst.model = model;
            inst.mat[0]  = c;    inst.mat[1]  = 0.0f; inst.mat[2]  = -s;   inst.mat[3]  = 0.0f;
            inst.mat[4]  = 0.0f; inst.mat[5]  = 1.0f; inst.mat[6]  = 0.0f; inst.mat[7]  = 0.0f;
            inst.mat[8]  = s;    inst.mat[9]  = 0.0f; inst.mat[10] = c;    inst.mat[11] = 0.0f;
            inst.mat[12] = cv.position[0];
            inst.mat[13] = cv.position[1];
            inst.mat[14] = cv.position[2];
            inst.mat[15] = 1.0f;
            inst.layerGuid       = 0;
            inst.crowdItemIdx    = (int)ci;
            inst.crowdValIdx     = (int)vi;
            inst.guid            = 0;
            inst.parentGuid      = 0;
            inst.gameModeMask    = -1;
            inst.block1WtOffset  = 0;

            // World-space AABB so pickInstance() can hit this thing.
            // Earlier we zeroed bbox here — every crowd instance ended
            // up overlapping at world origin, un-pickable in place.
            //
            // Strategy: if the model has bounds AND they aren't a
            // degenerate single point, transform the 8 local corners
            // through inst.mat and take the world min/max. Otherwise
            // fall back to a generous 3m-wide × 4m-tall cube around
            // the instance position — guaranteed clickable for any
            // humanoid-scale crowd model.
            bool boundsUsable = model->hasBounds
                && (model->localMax[0] - model->localMin[0] > 0.01f
                 || model->localMax[1] - model->localMin[1] > 0.01f
                 || model->localMax[2] - model->localMin[2] > 0.01f);
            if (boundsUsable) {
                float wmin[3] = { 1e30f,  1e30f,  1e30f};
                float wmax[3] = {-1e30f, -1e30f, -1e30f};
                for (int corner = 0; corner < 8; ++corner) {
                    float lx = (corner & 1) ? model->localMax[0] : model->localMin[0];
                    float ly = (corner & 2) ? model->localMax[1] : model->localMin[1];
                    float lz = (corner & 4) ? model->localMax[2] : model->localMin[2];
                    // D3D9 row-major matrix, row-vector convention:
                    // worldRow = localRow * M, where rows of M are basis.
                    float wx = lx * inst.mat[0] + ly * inst.mat[4]
                             + lz * inst.mat[8] + inst.mat[12];
                    float wy = lx * inst.mat[1] + ly * inst.mat[5]
                             + lz * inst.mat[9] + inst.mat[13];
                    float wz = lx * inst.mat[2] + ly * inst.mat[6]
                             + lz * inst.mat[10] + inst.mat[14];
                    if (wx < wmin[0]) wmin[0] = wx;
                    if (wy < wmin[1]) wmin[1] = wy;
                    if (wz < wmin[2]) wmin[2] = wz;
                    if (wx > wmax[0]) wmax[0] = wx;
                    if (wy > wmax[1]) wmax[1] = wy;
                    if (wz > wmax[2]) wmax[2] = wz;
                }
                inst.bboxMin[0] = wmin[0]; inst.bboxMin[1] = wmin[1]; inst.bboxMin[2] = wmin[2];
                inst.bboxMax[0] = wmax[0]; inst.bboxMax[1] = wmax[1]; inst.bboxMax[2] = wmax[2];
            } else {
                // Generous humanoid-scale cube around instance pos.
                // 3m wide on X/Z, 4m tall on Y starting at the foot pos.
                inst.bboxMin[0] = cv.position[0] - 1.5f;
                inst.bboxMin[1] = cv.position[1];
                inst.bboxMin[2] = cv.position[2] - 1.5f;
                inst.bboxMax[0] = cv.position[0] + 1.5f;
                inst.bboxMax[1] = cv.position[1] + 4.0f;
                inst.bboxMax[2] = cv.position[2] + 1.5f;
            }
            m_instances.push_back(inst);
        }
    }
}

// =====================================================================
// 3dCrowd Editor authoring helpers — every one of these is the kind of
// boring one-line setter the user used to have to do in JSON with a
// text editor. Marking dirty so the save pipeline picks it up. The
// model-key swap also touches the model cache so the viewport reflects
// the change immediately (Pass-1 cache pattern from line ~3389).
// =====================================================================

bool LevelScene::renameCrowdItemKey(int item, uint32_t newCrc, const std::string& name)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    m_crowdItems[item].meshKey     = newCrc;
    m_crowdItems[item].meshKeyName = name;
    m_crowdDirty = true;
    return true;
}

bool LevelScene::setCrowdItemModelKey(int item, uint32_t newCrc, const std::string& name,
                                       const ZeroEngine::LevelReader& reader)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    m_crowdItems[item].modelKey     = newCrc;
    m_crowdItems[item].modelKeyName = name;

    // Pass-1 cache pattern: if the new model isn't cached yet, load it now
    // so rebuildCrowdInstances sees a non-null model and emits LevelInstances.
    // Without this, the swap appears to do nothing visually until next reload.
    if (m_modelCache.find(newCrc) == m_modelCache.end()) {
        LevelModel* nm = new LevelModel();
        nm->name = name;
        if (loadModelFromBinary(reader, newCrc, name, nm)) {
            m_modelCache[newCrc] = nm;
        } else {
            delete nm;
            m_modelCache[newCrc] = NULL; // remember failure, don't retry every frame
        }
    }
    m_crowdDirty = true;
    return true;
}

bool LevelScene::setCrowdItemMeshKey(int item, uint32_t newCrc, const std::string& name)
{
    // Note: "mesh" field in CrowdItem is the same as the `key` (meshKey/meshKeyName)
    // per the JSON parser. Kept as a separate setter in case future schema separates them.
    return renameCrowdItemKey(item, newCrc, name);
}

bool LevelScene::setCrowdItemRightKey(int item, uint32_t newCrc, const std::string& name)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    m_crowdItems[item].rightHandKey  = newCrc;
    m_crowdItems[item].rightHandName = name;
    m_crowdDirty = true;
    return true;
}

bool LevelScene::setCrowdItemLeftKey(int item, uint32_t newCrc, const std::string& name)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    m_crowdItems[item].leftHandKey  = newCrc;
    m_crowdItems[item].leftHandName = name;
    m_crowdDirty = true;
    return true;
}

bool LevelScene::setCrowdItemPlayback(int item, float rate)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    if (rate < 0.0f) rate = 0.0f;
    if (rate > 6.0f) rate = 6.0f;  // unk_4 / playback rate ceiling per memo
    m_crowdItems[item].playbackRate = rate;
    m_crowdDirty = true;
    return true;
}

bool LevelScene::addCrowdItemAnim(int item, uint32_t crc, const std::string& name)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    LevelCrowdItem& it = m_crowdItems[item];
    // De-dupe — adding the same anim CRC twice causes the engine's anim
    // selector to pick it with double probability, almost certainly not
    // what the user meant.
    for (size_t i = 0; i < it.animationKeys.size(); ++i) {
        if (it.animationKeys[i] == crc) return false;
    }
    it.animationKeys.push_back(crc);
    it.animationNames.push_back(name);
    m_crowdDirty = true;
    return true;
}

bool LevelScene::removeCrowdItemAnim(int item, int animIdx)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    LevelCrowdItem& it = m_crowdItems[item];
    if (animIdx < 0 || animIdx >= (int)it.animationKeys.size()) return false;
    it.animationKeys.erase(it.animationKeys.begin() + animIdx);
    it.animationNames.erase(it.animationNames.begin() + animIdx);
    m_crowdDirty = true;
    return true;
}

int LevelScene::duplicateCrowdItem(int item)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return -1;
    LevelCrowdItem copy = m_crowdItems[item];  // deep copy via vector copy
    copy.meshKeyName += "_copy";
    // meshKey CRC stays the same as the source until the user renames —
    // the rename pipeline (renameCrowdItemKey) will overwrite it and the
    // save flow queues the new name string for pak_strings.json injection.
    m_crowdItems.push_back(copy);
    m_crowdDirty = true;
    return (int)m_crowdItems.size() - 1;
}

bool LevelScene::deleteCrowdItemAt(int item)
{
    if (item < 0 || item >= (int)m_crowdItems.size()) return false;
    m_crowdItems.erase(m_crowdItems.begin() + item);
    m_crowdDirty = true;
    // Clear focus if we just deleted the focused item; downstream rebuild
    // will restore visibility for everything else automatically.
    if (m_crowdFocusedItem == item) {
        m_crowdFocusModeOn = false;
        m_crowdFocusedItem = -1;
    } else if (m_crowdFocusedItem > item) {
        m_crowdFocusedItem--;  // shift down to track the survivor
    }
    return true;
}

// Iterate the live model cache and pick every entry whose name starts
// with 'prefix'. Names are already resolved via reader.ResolveCrc at
// load time (LevelModel::name). Output is APPENDED, not cleared, so
// callers can union multiple prefixes (e.g., CRD_ + HMD_ + IGD_).
void LevelScene::getModelNamesByPrefix(const char* prefix, std::vector<std::string>& out) const
{
    if (!prefix || !*prefix) return;
    size_t plen = strlen(prefix);
    for (std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.begin();
         it != m_modelCache.end(); ++it)
    {
        if (!it->second) continue;
        const std::string& n = it->second->name;
        if (n.size() < plen) continue;
        if (memcmp(n.c_str(), prefix, plen) == 0) out.push_back(n);
    }
}

// ============================================================
// ensureModelLoaded - THE LAZY LOAD ESCAPE HATCH FOR AUTHORING TOOLS
// ============================================================
//
// The Mesh Surgery and Crowd Mesh Builder dropdowns are built from
// the goddamn string table, which lists every CRD_/CH_ name the
// PAK ever heard of, not just the ones some entity on the current
// map is actively rendering. So the user picks "CRD_CH_elf_swd"
// on Helm's Deep (a map that ships zero defender CRDs), the host
// hits getModelByCrc, gets NULL because the model never got
// instanced, and the panel slaps up "no UV/tex found" while the
// user yells WHY at the monitor.
//
// This is the fucking escape hatch. Cache miss falls through to
// loadModelFromBinary (the same path renameCrowdItemKey uses
// when the user swaps a CrowdItem to a new model), we crack the
// model out of the BIN by CRC, drop the new LevelModel into the
// cache, hand it back. Authoring tools work everywhere now,
// instanced or not.
//
// Caching NULL on failure is intentional. If the BIN really has
// no entry for this CRC (model was named in the string table but
// never actually packed by lotrc_rs because nothing referenced
// it - rare but possible on hand-edited dumps) we want to
// remember that fact, not re-try on every fucking frame and lock
// the UI thread doing the same failing disk read. Better to fail
// fast and silent than to spin.
LevelModel* LevelScene::ensureModelLoaded(const ZeroEngine::LevelReader& reader,
                                          uint32_t                       meshCrc,
                                          const std::string&             modelName)
{
    std::map<uint32_t, LevelModel*>::iterator it = m_modelCache.find(meshCrc);
    if (it != m_modelCache.end()) return it->second;

    LevelModel* nm = new LevelModel();
    nm->name = modelName;
    if (loadModelFromBinary(reader, meshCrc, modelName, nm)) {
        m_modelCache[meshCrc] = nm;
        return nm;
    }
    delete nm;
    m_modelCache[meshCrc] = NULL;
    return NULL;
}

// Every loaded model, no prefix filter. Used by the 3dCrowd Editor's
// mesh and weapon dropdowns so the user can pick ANY model in the level
// (per the user's feedback that prefix-filtered lists were too narrow).
void LevelScene::getAllModelNames(std::vector<std::string>& out) const
{
    for (std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.begin();
         it != m_modelCache.end(); ++it)
    {
        if (!it->second) continue;
        const std::string& n = it->second->name;
        if (!n.empty()) out.push_back(n);
    }
}

// Skinned-only walk over the cache. The Adjust Mesh by Bone panel uses
// this to filter the dropdown to models that have a usable bone palette;
// props / terrain / FX meshes show no bones and would just clutter the
// list.
void LevelScene::getSkinnedModelNames(std::vector<std::string>& out) const
{
    for (std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.begin();
         it != m_modelCache.end(); ++it)
    {
        if (!it->second) continue;
        if (it->second->bones.empty()) continue;
        const std::string& n = it->second->name;
        if (!n.empty()) out.push_back(n);
    }
}

// Union of every anim name across every CrowdItem. De-duplicated by
// string compare — typical level has 50-200 unique anim names so a
// linear-scan dedupe is fine.
void LevelScene::getAllCrowdAnimNames(std::vector<std::string>& out) const
{
    for (size_t ci = 0; ci < m_crowdItems.size(); ++ci) {
        const std::vector<std::string>& names = m_crowdItems[ci].animationNames;
        for (size_t ai = 0; ai < names.size(); ++ai) {
            const std::string& n = names[ai];
            bool seen = false;
            for (size_t k = 0; k < out.size(); ++k) {
                if (out[k] == n) { seen = true; break; }
            }
            if (!seen) out.push_back(n);
        }
    }
}

// =====================================================================
// Crowd Mesh Builder support — bone enumeration + preview LevelInstance
// lifecycle. The Builder panel asks for the bone list of whatever base
// mesh the user picked, then spawns one preview LevelInstance per
// weapon so the user can see roughly where the weapon will sit before
// committing to a bake. v1 of this path spawns at identity — the per-
// frame "follow parent's bone palette" recompute is wired in a later
// phase. Even the identity-spawned preview is useful: the user can
// scoot it via the offset sliders and check the orientation lines up
// against the base mesh's hand by eyeball.
// =====================================================================

// Look up a cached model by CRC and copy out its bone-name list (resolved
// via the reader's CRC table at load time). Out-of-cache OR models with
// no skeleton (bones empty) return false; otherwise true with names
// appended to 'out'. Caller may pre-fill 'out' — we append, not clear,
// matching the getAllModelNames / getModelNamesByPrefix convention.
bool LevelScene::getModelBoneNames(uint32_t modelCrc, std::vector<std::string>& out) const
{
    std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(modelCrc);
    if (it == m_modelCache.end() || !it->second) return false;
    const LevelModel* m = it->second;
    if (m->bones.empty()) return false;
    for (size_t i = 0; i < m->bones.size(); ++i) {
        out.push_back(m->bones[i]);
    }
    return true;
}

// Linear scan, case-sensitive — the bone names in PAK string tables are
// authored with a fixed convention (Bone_LHand_attach etc.), no mixed
// case in the wild. Returns -1 if not found.
int LevelScene::findBoneIndexByName(uint32_t modelCrc, const std::string& boneName) const
{
    std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(modelCrc);
    if (it == m_modelCache.end() || !it->second) return -1;
    const LevelModel* m = it->second;
    for (size_t i = 0; i < m->bones.size(); ++i) {
        if (m->bones[i] == boneName) return (int)i;
    }
    return -1;
}

// Push a non-saved LevelInstance flagged as a preview. The model has to
// already be in m_modelCache (typically true because the base mesh of the
// crowd is already loaded — every Helm's Deep CH_/CRD_/WP_ shows up in
// the level's model list and gets cached on first encounter). Caller
// owns parentInstIdx validation (must point at the live base preview).
// Returns the new instance index, or -1 if the child model isn't cached.
//
// The preview's mat[] starts at IDENTITY. The plan calls for a per-frame
// recompute that walks the parent's bone palette and re-derives this
// instance's world matrix from parent.boneWorld[parentBoneIdx] * offset.
// That recompute is wired in Phase 3 part B — for now identity is good
// enough to validate the spawn/clear plumbing.
int LevelScene::spawnPreviewMesh(uint32_t childModelCrc,
                                  int parentInstIdx,
                                  int parentBoneIdx,
                                  const float offsetXYZ[3],
                                  const float offsetYPR[3])
{
    std::map<uint32_t, LevelModel*>::const_iterator it = m_modelCache.find(childModelCrc);
    if (it == m_modelCache.end() || !it->second) return -1;
    LevelModel* model = it->second;

    LevelInstance inst;
    inst.model = model;
    // Identity world transform — Phase 3 part B will overwrite per-frame.
    memset(inst.mat, 0, sizeof(inst.mat));
    inst.mat[0] = inst.mat[5] = inst.mat[10] = inst.mat[15] = 1.0f;
    inst.objName.clear();
    inst.typeName     = "CrowdMeshBuilderPreview";
    inst.meshName     = model->name;
    inst.guid         = 0;
    inst.parentGuid   = 0;
    inst.gameModeMask = -1;
    inst.block1WtOffset = 0;
    inst.layerGuid    = 0;
    inst.crowdItemIdx = -1;
    inst.crowdValIdx  = -1;
    inst.previewParentIdx     = parentInstIdx;
    inst.previewParentBoneIdx = parentBoneIdx;
    if (offsetXYZ) {
        inst.previewOffset[0] = offsetXYZ[0];
        inst.previewOffset[1] = offsetXYZ[1];
        inst.previewOffset[2] = offsetXYZ[2];
    } else {
        inst.previewOffset[0] = inst.previewOffset[1] = inst.previewOffset[2] = 0.0f;
    }
    if (offsetYPR) {
        inst.previewOffset[3] = offsetYPR[0];
        inst.previewOffset[4] = offsetYPR[1];
        inst.previewOffset[5] = offsetYPR[2];
    } else {
        inst.previewOffset[3] = inst.previewOffset[4] = inst.previewOffset[5] = 0.0f;
    }

    // World-space AABB — copy from model bounds transformed by identity,
    // same pattern as the crowd LevelInstance bbox in rebuildCrowdInstances.
    // Preview instances spawned at identity sit at the model's local origin
    // so the AABB falls out trivially as localMin/localMax. If the model
    // has no bounds (rare for crowd-relevant meshes but possible) fall
    // back to a generous humanoid cube around origin so the user still
    // gets SOMETHING rendered.
    bool boundsUsable = model->hasBounds
        && (model->localMax[0] - model->localMin[0] > 0.01f
         || model->localMax[1] - model->localMin[1] > 0.01f
         || model->localMax[2] - model->localMin[2] > 0.01f);
    if (boundsUsable) {
        inst.bboxMin[0] = model->localMin[0];
        inst.bboxMin[1] = model->localMin[1];
        inst.bboxMin[2] = model->localMin[2];
        inst.bboxMax[0] = model->localMax[0];
        inst.bboxMax[1] = model->localMax[1];
        inst.bboxMax[2] = model->localMax[2];
    } else {
        inst.bboxMin[0] = -1.5f; inst.bboxMin[1] = 0.0f; inst.bboxMin[2] = -1.5f;
        inst.bboxMax[0] =  1.5f; inst.bboxMax[1] = 4.0f; inst.bboxMax[2] =  1.5f;
    }

    // TODO Phase 3 part B: per-frame preview matrix recompute from parent
    // bone palette. For Phase 3 part A we just spawn at identity; the user
    // can manually scoot via the offset sliders once the loop is wired.

    m_instances.push_back(inst);
    return (int)m_instances.size() - 1;
}

// Sweep every preview-flagged instance out of m_instances. Iterate
// BACKWARDS so erase() doesn't shift indices under our feet. Also clear
// m_selectedIdx / m_hoveredIdx if they point at deleted slots, because
// dangling those past a preview teardown would make a stale rectangle
// render around an instance that no longer exists.
void LevelScene::clearPreviewMeshes()
{
    for (int i = (int)m_instances.size() - 1; i >= 0; --i) {
        if (m_instances[i].previewParentIdx >= 0) {
            if (m_selectedIdx == i) m_selectedIdx = -1;
            if (m_hoveredIdx  == i) m_hoveredIdx  = -1;
            // Indices ABOVE this one shift down by one after the erase.
            // If selected/hovered point above, decrement them so they
            // still point at the same logical entity.
            if (m_selectedIdx > i) --m_selectedIdx;
            if (m_hoveredIdx  > i) --m_hoveredIdx;
            m_instances.erase(m_instances.begin() + i);
        }
    }
}

void LevelScene::setCrowdFocusMode(bool on, int focusedItem)
{
    m_crowdFocusModeOn = on;
    m_crowdFocusedItem = on ? focusedItem : -1;
    // Caller is responsible for calling rebuildCrowdInstances() to apply.
}

// Dumps m_crowdItems as the JSON shape lotrc_rs -d emits for sub_blocks2/
// 3dcrowd.json. The save pipeline overwrites the parser-dumped file with
// this version before -c repacks it. Field order matches the Rust parser
// exactly so the deserializer round-trips clean.
bool LevelScene::dumpCrowdAsJson(const std::string& path) const
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return false;

    fprintf(f, "[\n");
    for (size_t ii = 0; ii < m_crowdItems.size(); ++ii) {
        const LevelCrowdItem& it = m_crowdItems[ii];
        if (ii > 0) fprintf(f, ",\n");
        fprintf(f, "  {\n");
        fprintf(f, "    \"header\": {\n");
        char hbuf[32];
        if (!it.meshKeyName.empty())
            fprintf(f, "      \"key\": \"%s\",\n", it.meshKeyName.c_str());
        else { sprintf(hbuf, "0x%08X", it.meshKey);
            fprintf(f, "      \"key\": \"%s\",\n", hbuf); }
        if (!it.modelKeyName.empty())
            fprintf(f, "      \"key_main\": \"%s\",\n", it.modelKeyName.c_str());
        else { sprintf(hbuf, "0x%08X", it.modelKey);
            fprintf(f, "      \"key_main\": \"%s\",\n", hbuf); }
        if (!it.rightHandName.empty())
            fprintf(f, "      \"key_right\": \"%s\",\n", it.rightHandName.c_str());
        else if (it.rightHandKey != 0) {
            sprintf(hbuf, "0x%08X", it.rightHandKey);
            fprintf(f, "      \"key_right\": \"%s\",\n", hbuf);
        } else
            fprintf(f, "      \"key_right\": \"\",\n");
        if (!it.leftHandName.empty())
            fprintf(f, "      \"key_left\": \"%s\",\n", it.leftHandName.c_str());
        else if (it.leftHandKey != 0) {
            sprintf(hbuf, "0x%08X", it.leftHandKey);
            fprintf(f, "      \"key_left\": \"%s\",\n", hbuf);
        } else
            fprintf(f, "      \"key_left\": \"\",\n");
        fprintf(f, "      \"unk_4\": %.7g,\n", it.playbackRate);
        fprintf(f, "      \"animation_num\": %u,\n", (unsigned)it.animationKeys.size());
        fprintf(f, "      \"instance_num\": %u\n",   (unsigned)it.instances.size());
        fprintf(f, "    },\n");
        fprintf(f, "    \"animations\": [");
        for (size_t ai = 0; ai < it.animationKeys.size(); ++ai) {
            if (ai > 0) fprintf(f, ", ");
            if (!it.animationNames[ai].empty())
                fprintf(f, "\"%s\"", it.animationNames[ai].c_str());
            else {
                sprintf(hbuf, "0x%08X", it.animationKeys[ai]);
                fprintf(f, "\"%s\"", hbuf);
            }
        }
        fprintf(f, "],\n");
        fprintf(f, "    \"instances\": [\n");
        for (size_t vi = 0; vi < it.instances.size(); ++vi) {
            const LevelCrowdInstance& cv = it.instances[vi];
            fprintf(f, "      { \"position\": { \"x\": %.7g, \"y\": %.7g, \"z\": %.7g }, \"rotation\": %.7g, \"lod\": %.7g }%s\n",
                    cv.position[0], cv.position[1], cv.position[2],
                    cv.rotation, cv.lod,
                    (vi + 1 == it.instances.size()) ? "" : ",");
        }
        fprintf(f, "    ]\n");
        fprintf(f, "  }");
    }
    fprintf(f, "\n]\n");
    fclose(f);
    return true;
}

