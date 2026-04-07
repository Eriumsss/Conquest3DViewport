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

#include <d3d9.h>
#include <d3dx9.h>

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <windows.h>

#define LEVEL_VERTEX_FVF  (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define LEVEL_VERTEX_SIZE (sizeof(LevelVertex))   // 32 bytes

// ── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_levelLog = NULL;
static bool s_renderLogOnce = false;

// Per-mesh-part material flags — stored here (not in header structs) to avoid
// sizeof changes that break other .obj files.
// Key = (model_ptr, part_index) -> flags: bit0=transparent, bit1=two-sided
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
      m_levelVS(NULL), m_levelPS(NULL), m_levelDecl(NULL), m_whiteTex(NULL),
      m_shadersReady(false), m_shaderInitAttempted(false), m_lightingExtracted(false),
      m_hoveredIdx(-1), m_selectedIdx(-1), m_gameModeFilter(-1),
      m_hoveredEditorIdx(-1), m_selectedEditorIdx(-1),
      m_showCollisions(false)
{
    memset(&m_lighting, 0, sizeof(m_lighting));
    memset(m_cachedVP, 0, sizeof(m_cachedVP));
    // Identity 3x3
    memset(m_levelRot, 0, sizeof(m_levelRot));
    m_levelRot[0] = m_levelRot[4] = m_levelRot[8] = 1.0f;
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
    "};\n"
    "\n"
    // Input: COLOR0 = per-vertex baked AO/lighting from game data
    "VS_OUT main(float3 p : POSITION, float3 n : NORMAL,\n"
    "            float4 c : COLOR0, float2 t : TEXCOORD0) {\n"
    "  VS_OUT o;\n"
    // HLSL column-major + SetVertexShaderConstantF row-major → use mul(M,v)
    "  float4 wp = mul(g_World, float4(p, 1.0));\n"
    "  o.pos  = mul(g_ViewProj, wp);\n"
    "  o.nrm  = mul((float3x3)g_World, n);\n"
    "  o.wpos = wp;\n"
    "  o.uv   = t;\n"
    "  o.vcol = c;\n"
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
    "sampler2D g_Tex : register(s0);\n"
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
    "\n"
    "float4 main(\n"
    "  float3 nrm  : TEXCOORD0,\n"
    "  float4 wpos : TEXCOORD1,\n"
    "  float2 uv   : TEXCOORD2,\n"
    "  float4 vcol : TEXCOORD3\n"
    ") : COLOR {\n"
    "  float3 N = normalize(nrm);\n"
    "  float4 tex = tex2D(g_Tex, uv);\n"
    "  float alpha = tex.a * g_DiffColor.a;\n"
    "\n"
    // ── Game-accurate per-object lighting (Mg_FP_Lit) ──
    // diffuse = tex * diffColor (vertexColor.rgb tints albedo)
    "  float3 albedo = tex.rgb * g_DiffColor.rgb * vcol.rgb;\n"
    // AO from vertex alpha (game: v1.w) with scale + bias from PAK
    "  float ao = saturate(vcol.a * g_MiscParams.x + g_MiscParams.y);\n"
    "\n"
    // Lambert: Ir = sunCol * Qd + Qa  (game: no wrap, no specular for base shader)
    "  float NdL = saturate(dot(N, g_SunDir.xyz));\n"
    "  float3 Qa = albedo * g_Ambient.rgb * ao;\n"
    "  float3 Qd = albedo * NdL;\n"
    "  float3 Ir = g_SunCol.rgb * Qd + Qa;\n"
    "\n"
    // ── Atmospheric scattering (MgFP_ScreenScattering math, inline) ──
    "  float3 toPixel = wpos.xyz - g_CamPos.xyz;\n"
    "  float dist = length(toPixel);\n"
    "  float3 viewDir = toPixel / max(dist, 0.001);\n"
    "\n"
    // Height-dependent density (trapezoidal integration of exp(-h*epsilon))
    "  float hf = g_ScatParams.y;\n"
    "  float densCam = exp(-g_CamPos.y * hf);\n"
    "  float densPix = exp(-wpos.y * hf);\n"
    "  float avgDens = (densCam + densPix) * 0.5;\n"
    "  float baseDensity = g_ScatParams.x + g_ScatParams.z;\n"
    "  float optDepth = baseDensity * avgDens * dist * g_ScatParams.w;\n"
    "\n"
    // Extinction: exp(-opticalDepth) — game: exp2(-depth * log2(e))
    "  float ext = exp(-optDepth);\n"
    "\n"
    // Rayleigh phase: (cos²θ + 1)
    "  float cosTheta = dot(viewDir, g_SunDir.xyz);\n"
    "  float rayleigh = cosTheta * cosTheta + 1.0;\n"
    "\n"
    // Mie phase (Henyey-Greenstein): (1-g²) / (1+g²-2g*cosθ)^1.5
    "  float mieBase = g_ScatHG.y + g_ScatHG.z * cosTheta;\n"
    "  float mie = g_ScatHG.x / (mieBase * sqrt(max(mieBase, 0.0001)));\n"
    "\n"
    // Inscattering: phase-weighted * sunCol * inscatterColor * (1-ext)
    // Attenuated 0.10x: game applies scattering as a screen-space post-process,
    // not per-object.  Inline application needs heavy reduction to avoid washout.
    "  float3 inscatter = (g_MiscParams.z * rayleigh + g_MiscParams.w * mie);\n"
    "  inscatter *= g_SunCol.rgb * g_InscatCol.rgb * g_InscatCol.w * (1.0 - ext);\n"
    "  inscatter *= 0.10;\n"
    "\n"
    // Final: scene * extinction * extinctionTint * extMult + inscattering
    "  Ir = Ir * ext * g_ExtinctCol.rgb * g_ExtinctCol.w + inscatter;\n"
    "\n"
    "  return float4(saturate(Ir), alpha);\n"
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

    // Vertex declaration matching LevelVertex (pos + normal + color + uv)
    D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 28, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
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
    m_shadersReady = false;
    m_shaderInitAttempted = false;
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
    float defAmb[4]    = { 0.15f, 0.15f, 0.18f, 0.0f };
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
        L.ambient[3] = 0.0f;

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
        float baseDens = atmForce + hazeForce;
        if (baseDens < 0.001f) baseDens = 1.0f;
        float optDepthScale = distScale / (atmLimit * atmLimit * baseDens);

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

        LevelLogf("[Lighting] ambient=(%.3f,%.3f,%.3f) scale=%.2f",
                  L.ambient[0], L.ambient[1], L.ambient[2], ambScale);
        LevelLogf("[Lighting] AO: scale=%.2f bias=%.2f",
                  L.miscParams[0], L.miscParams[1]);
        LevelLogf("[Lighting] inscatter=(%.3f,%.3f,%.3f) mult=%.2f",
                  L.inscatterCol[0], L.inscatterCol[1], L.inscatterCol[2], L.inscatterCol[3]);
        LevelLogf("[Lighting] extinct=(%.3f,%.3f,%.3f) mult=%.2f",
                  L.extinctCol[0], L.extinctCol[1], L.extinctCol[2], L.extinctCol[3]);
        LevelLogf("[Lighting] atmForce=%.3f hazeForce=%.3f hg=%.3f heightFalloff=%.5f optDepthScale=%.5f",
                  atmForce, hazeForce, g, heightFalloff, optDepthScale);
        LevelLogf("[Lighting] atmLimit=%.0f distScale=%.1f fullFogDist=%.0f",
                  atmLimit, distScale, 0);
        LevelLogf("[Lighting] betaRay=%.2f betaMie=%.2f rayStr=%.3f mieStr=%.3f",
                  betaRay, betaMie, L.miscParams[2], L.miscParams[3]);
    }

    // --- light_sun ---
    if (sun)
    {
        LevelLogf("[Lighting] Found light_sun '%s' (GUID %u)", sun->name.c_str(), sun->guid);

        // Sun color = Color * ColorScale
        uint32_t sCol = GetInt(sun->int_fields, CRC_COLOR, 0xFFFFFFFF);
        float    sScale = GetFloat(sun->float_fields, CRC_COLORSCALE, 1.0f);
        float sRGB[3]; UnpackColor(sCol, sRGB);
        L.sunCol[0] = sRGB[0] * sScale;
        L.sunCol[1] = sRGB[1] * sScale;
        L.sunCol[2] = sRGB[2] * sScale;
        L.sunCol[3] = 1.0f;

        // Sun direction = Z-axis of WorldTransform (row 2, already normalized)
        if (sun->has_transform)
        {
            L.sunDir[0] = sun->world_transform[8];
            L.sunDir[1] = sun->world_transform[9];
            L.sunDir[2] = sun->world_transform[10];
            L.sunDir[3] = 0.0f;
        }

        LevelLogf("[Lighting] sun color=(%.3f,%.3f,%.3f) scale=%.1f dir=(%.3f,%.3f,%.3f)",
                  L.sunCol[0], L.sunCol[1], L.sunCol[2], sScale,
                  L.sunDir[0], L.sunDir[1], L.sunDir[2]);
    }

    // --- templateLevel (for DrawDistance) ---
    if (tmpl)
    {
        L.drawDist = GetFloat(tmpl->float_fields, CRC_DRAWDIST, 1800.0f);
        LevelLogf("[Lighting] DrawDistance=%.0f", L.drawDist);
    }

    m_lightingExtracted = true;
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
        std::vector<uint8_t> ddsData;
        bool gotData = reader.GetBinAssetData(assetKey, ddsData);
        if (s_texLogCount <= 5)
        {
            FILE* tf = fopen("levelscene_texdbg.txt", "a");
            if (tf)
            {
                fprintf(tf, "  -> GetBinAssetData=%s ddsSize=%u\n",
                        gotData ? "ok" : "FAIL", (unsigned)ddsData.size());
                fclose(tf);
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
    for (int slot = lod0Start; slot < lod0End; ++slot)
    {
        // Bounds-check the meshOrder read
        uint32_t moByteEnd = mdl.mesh_order_offset + ((uint32_t)slot + 1u) * 4u;
        if (moByteEnd > (uint32_t)b1.size()) break;

        uint32_t mo     = meshOrder[slot];
        // Bit 31 = visual geometry, Bit 30 = collision/shadow mesh.
        // Skip any slot that has ONLY bit 30 set (explicit collision marker).
        if ((mo & 0xC0000000u) == 0x40000000u) continue;
        uint32_t bufIdx = mo & 0x3FFFFFFFu;

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

        if (vbInfoOff == 0 || (uint64_t)vbInfoOff + sizeof(VBuffInfo) > b1.size()) continue;
        if (ibInfoOff == 0 || (uint64_t)ibInfoOff + sizeof(IBuffInfo) > b1.size()) continue;

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

        if (vStride < 12 || vSize == 0) continue;
        if ((uint64_t)vOff + vSize > binAsset.size()) continue;

        int vertCount = (int)(vSize / vStride);
        if (vertCount <= 0) continue;

        // ── Index data from BIN asset ─────────────────────────────────────────
        uint32_t iOff  = ibi.offset;  // byte offset into binAsset
        uint32_t iSize = ibi.size;
        // ibi.format encodes bits-per-index (16=U16, 32=U32), NOT a boolean flag.
        // Derive actual element size from iSize/i_num to be safe.
        uint32_t iElem = 2u;
        if (i_num > 0 && iSize > 0) iElem = (iSize / i_num >= 4u) ? 4u : 2u;
        bool is32bit   = (iElem == 4u);
        if (i_num == 0) i_num = iSize / iElem;
        if (i_num < 3) continue;
        if ((uint64_t)iOff + iSize > binAsset.size()) continue;

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
        // Color(0) offset: after normal (if present)
        uint32_t colorByteOff = normalByteOff;
        if ((vbi.fmt1 & 0x002u) != 0u) colorByteOff += 4u;   // Normal before color
        bool hasColor = (vbi.fmt1 & 0x100u) != 0u && (vStride >= colorByteOff + 4u);
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

            // Vertex color / ambient occlusion (game's Color(0) channel)
            if (hasColor)
                memcpy(&lv.color, vp + colorByteOff, 4);
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
        }

        // ── D3D9 Vertex Buffer ────────────────────────────────────────────────
        IDirect3DVertexBuffer9* vb = NULL;
        if (FAILED(m_device->CreateVertexBuffer(
                vertCount * LEVEL_VERTEX_SIZE, D3DUSAGE_WRITEONLY,
                LEVEL_VERTEX_FVF, D3DPOOL_MANAGED, &vb, NULL)) || !vb)
            continue;

        LevelVertex* vbPtr = NULL;
        if (FAILED(vb->Lock(0, 0, (void**)&vbPtr, 0))) { vb->Release(); continue; }
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
            { vb->Release(); continue; }

        uint16_t* ibPtr = NULL;
        if (FAILED(ib->Lock(0, 0, (void**)&ibPtr, 0)))
            { vb->Release(); ib->Release(); continue; }
        memcpy(ibPtr, &indices[0], i_num * sizeof(uint16_t));
        ib->Unlock();

        // ── Material lookup ───────────────────────────────────────────────────
        // mdl.mat_offset -> array of u32 byte offsets into Block1 (one per buffer).
        // Each points to a MatBase struct (PC layout):
        //   +0   unk_0, +4 unk_1(PC), +8 tex0(CRC)
        //   +200 flags(u64) — bit62 = transparent/additive
        //   +218 side_flags(u16) — 0x100 = two-sided
        IDirect3DTexture9* tex = NULL;
        uint32_t partTexCrc = 0;
        uint64_t matFlags64 = 0;
        uint16_t matSideFlags = 0;
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
                    uint32_t tex0crc;
                    memcpy(&tex0crc, &b1[matStructOff + 8], 4);
                    partTexCrc = tex0crc;
                    if (tex0crc != 0)
                        tex = getOrLoadTexture(reader, tex0crc);

                    // MatBase.flags (u64 at +200) and side_flags (u16 at +218)
                    if ((uint64_t)matStructOff + 208u <= b1.size())
                        memcpy(&matFlags64, &b1[matStructOff + 200], 8);
                    if ((uint64_t)matStructOff + 220u <= b1.size())
                        memcpy(&matSideFlags, &b1[matStructOff + 218], 2);
                }
            }
        }

        // ── Commit mesh part ──────────────────────────────────────────────────
        LevelMeshPart part;
        part.vb          = vb;
        part.ib          = ib;
        part.vertexCount = vertCount;
        part.indexCount  = (int)i_num;
        part.diffuseTex  = tex;
        part.texCrc      = partTexCrc;
        out->parts.push_back(part);

        // Store flags in static map (no header struct changes)
        uint32_t pf = 0;
        if (matFlags64 & ((uint64_t)1 << 62)) pf |= 1;  // transparent
        if (matSideFlags & 0x100)              pf |= 2;  // two-sided
        if (pf) g_partFlags[std::make_pair((const void*)out, (int)out->parts.size()-1)] = pf;
    }

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
    // Crowd data lives in sub_blocks2 under CRC("3dCrowd").
    // Binary format: u32 magic(0x65), u32 count, u32[count] offsets, then CrowdItems.
    // Each CrowdItem: CrowdHeader(28B) + Crc[anim_num] + CrowdVal[inst_num]
    // CrowdHeader: key(4) key_main(4) key_right(4) key_left(4) unk4(4) anim_num(4) inst_num(4)
    // CrowdVal: position(12) rotation(4) lod(4) = 20 bytes
    {
        int crowdLoaded = 0;
        static const uint32_t CRC_3DCROWD = ZeroEngine::LotrHashString("3dCrowd");
        const std::vector<ZeroEngine::SubBlockEntry>& sb2 = reader.GetSubBlocks2();
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

                // Read offset table
                std::vector<uint32_t> itemOffsets(itemCount);
                if (8 + itemCount * 4 <= crowdDataSize)
                {
                    memcpy(&itemOffsets[0], cd + 8, itemCount * 4);

                    for (uint32_t ci = 0; ci < itemCount; ++ci)
                    {
                        uint32_t ioff = itemOffsets[ci];
                        if (ioff + 28 > crowdDataSize) continue;
                        const uint8_t* item = cd + ioff;

                        // CrowdHeader: key(4) key_main(4) key_right(4) key_left(4) unk4(4) anim_num(4) inst_num(4)
                        uint32_t meshCrc, modelCrc;
                        uint32_t animNum, instNum;
                        memcpy(&meshCrc,  item + 0, 4);
                        memcpy(&modelCrc, item + 4, 4);
                        memcpy(&animNum,  item + 20, 4);
                        memcpy(&instNum,  item + 24, 4);

                        if (instNum == 0 || instNum > 100000) continue;

                        // Skip animations: animNum * 4 bytes (CRC each)
                        uint32_t instOff = 28 + animNum * 4;
                        if (ioff + instOff + instNum * 20 > crowdDataSize) continue;

                        // Resolve model name for logging
                        std::string modelName = reader.ResolveCrc(modelCrc);
                        LevelLogf("[Crowd]   item %u: model=%s meshCrc=0x%08X instances=%u anims=%u",
                                  ci, modelName.c_str(), meshCrc, instNum, animNum);

                        // Load the model (by modelCrc, same as regular level meshes)
                        LevelModel* model = NULL;
                        std::map<uint32_t, LevelModel*>::iterator cit = m_modelCache.find(modelCrc);
                        if (cit != m_modelCache.end())
                        {
                            model = cit->second;
                        }
                        else
                        {
                            LevelModel* nm = new LevelModel();
                            nm->name = modelName;
                            if (loadModelFromBinary(reader, modelCrc, modelName, nm))
                                model = nm;
                            else
                                delete nm;
                            m_modelCache[modelCrc] = model;
                        }

                        if (!model) continue;

                        // Create instances from CrowdVal entries
                        const uint8_t* vals = item + instOff;
                        for (uint32_t vi = 0; vi < instNum; ++vi)
                        {
                            const uint8_t* v = vals + vi * 20;
                            float px, py, pz, rot;
                            memcpy(&px, v + 0, 4);
                            memcpy(&py, v + 4, 4);
                            memcpy(&pz, v + 8, 4);
                            memcpy(&rot, v + 12, 4);

                            // Build Y-rotation world matrix (row-major)
                            float c = cosf(rot), s = sinf(rot);
                            LevelInstance inst;
                            inst.model = model;
                            // Row 0: right
                            inst.mat[0] = c;   inst.mat[1] = 0.0f; inst.mat[2] = -s;  inst.mat[3] = 0.0f;
                            // Row 1: up
                            inst.mat[4] = 0.0f; inst.mat[5] = 1.0f; inst.mat[6] = 0.0f; inst.mat[7] = 0.0f;
                            // Row 2: forward
                            inst.mat[8] = s;   inst.mat[9] = 0.0f; inst.mat[10] = c;  inst.mat[11] = 0.0f;
                            // Row 3: translation
                            inst.mat[12] = px; inst.mat[13] = py; inst.mat[14] = pz; inst.mat[15] = 1.0f;

                            m_instances.push_back(inst);
                            ++crowdLoaded;
                        }
                    }
                }
            }
        }

        if (crowdLoaded > 0)
            LevelLogf("[Crowd] Loaded %d crowd instances", crowdLoaded);
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

// Returns icon shape for the 5 XSI/invisible placeholders, or -1 for others.
static int GetXsiIconShape(const std::string& t)
{
    if (t.find("FED_camera")     == 0) return ICON_CAMERA;
    if (t.find("FED_nodearcher") == 0) return ICON_ARROW_UP;
    if (t.find("FED_nodefort")   == 0) return ICON_TURRET;
    if (t.find("FED_emitter")    == 0) return ICON_STARBURST;
    if (t.find("invisible")      == 0) return ICON_CUBE;
    return -1;
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
        if (t.find(kTable[i].prefix) == 0)
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

// ── render() ──────────────────────────────────────────────────────────────────

void LevelScene::render()
{
    if (!m_device || !m_loaded || m_instances.empty()) return;

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
    IDirect3DBaseTexture9* sTex = NULL;
    IDirect3DVertexDeclaration9* sDecl = NULL;
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
    m_device->GetFVF(&sFVF);
    m_device->GetTexture(0, &sTex);
    m_device->GetVertexDeclaration(&sDecl);

    // Common render state
    m_device->SetRenderState(D3DRS_ZENABLE,          TRUE);
    m_device->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
    m_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    m_device->SetRenderState(D3DRS_CULLMODE,         D3DCULL_CW);
    m_device->SetRenderState(D3DRS_COLORWRITEENABLE,
                             D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                             D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

    // Alpha test OFF for level geometry — most textures store specular/gloss
    // in alpha, not cutout masks.  Alpha test at 128 punches holes everywhere.
    m_device->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);

    // Sampler state (shared by both paths)
    m_device->SetSamplerState(0, D3DSAMP_MINFILTER,     D3DTEXF_ANISOTROPIC);
    m_device->SetSamplerState(0, D3DSAMP_MAGFILTER,     D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MIPFILTER,     D3DTEXF_LINEAR);
    m_device->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSU,      D3DTADDRESS_WRAP);
    m_device->SetSamplerState(0, D3DSAMP_ADDRESSV,      D3DTADDRESS_WRAP);

    int calls = 0;
    IDirect3DTexture9* curTex = NULL;

    // ── Shader-based rendering (game-accurate pipeline) ──
    // Falls back to fixed-function if shaders failed to compile.
    if (m_shadersReady)
    {
        m_device->SetRenderState(D3DRS_LIGHTING, FALSE);
        m_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        m_device->SetVertexShader(m_levelVS);
        m_device->SetPixelShader(m_levelPS);
        m_device->SetVertexDeclaration(m_levelDecl);

        // Retrieve view+proj from D3D9 and compute ViewProj using game-accurate
        // SSE matrix multiply (ZMatMul — same instruction pattern as FUN_0068f144)
        D3DMATRIX d3dView, d3dProj;
        m_device->GetTransform(D3DTS_VIEW,       &d3dView);
        m_device->GetTransform(D3DTS_PROJECTION, &d3dProj);

        ZMat4x4 zView, zProj, zViewProj;
        zView.load((const float*)&d3dView);
        zProj.load((const float*)&d3dProj);
        ZMatMul(&zViewProj, &zView, &zProj);   // row-major: View * Proj
        memcpy(m_cachedVP, zViewProj.f, sizeof(float) * 16);  // cache for picking
        m_device->SetVertexShaderConstantF(4, zViewProj.f, 4);  // VS c4-c7


        // Camera world position: recover from orthonormal view matrix.
        // View row 3 = (-dot(right,eye), -dot(up,eye), -dot(fwd,eye), 1)
        // eye = -(row0 * t.x + row1 * t.y + row2 * t.z)  [orthonormal inverse]
        float camPos[4];
        {
            const float* v = zView.f;
            float tx = v[12], ty = v[13], tz = v[14]; // row 3 translation
            camPos[0] = -ZDot3f(v[0], v[1], v[2],   tx, ty, tz);
            camPos[1] = -ZDot3f(v[4], v[5], v[6],   tx, ty, tz);
            camPos[2] = -ZDot3f(v[8], v[9], v[10],  tx, ty, tz);
            camPos[3] = 1.0f;
        }

        // PS constants — game-accurate layout from disassembled shaders
        m_device->SetPixelShaderConstantF(0, m_lighting.ambient,       1);  // PS c0: ambient
        m_device->SetPixelShaderConstantF(1, m_lighting.sunCol,        1);  // PS c1: sun color
        m_device->SetPixelShaderConstantF(2, m_lighting.sunDir,        1);  // PS c2: sun direction
        m_device->SetPixelShaderConstantF(3, m_lighting.diffCol,       1);  // PS c3: diffuse color
        m_device->SetPixelShaderConstantF(4, camPos,                   1);  // PS c4: camera pos
        m_device->SetPixelShaderConstantF(5, m_lighting.scatterParams, 1);  // PS c5: scattering density/falloff
        m_device->SetPixelShaderConstantF(6, m_lighting.inscatterCol,  1);  // PS c6: inscatter color+mult
        m_device->SetPixelShaderConstantF(7, m_lighting.extinctCol,    1);  // PS c7: extinction color+mult
        m_device->SetPixelShaderConstantF(8, m_lighting.scatterHG,     1);  // PS c8: HG phase params
        m_device->SetPixelShaderConstantF(9, m_lighting.miscParams,    1);  // PS c9: AO+beta params

        // ── All geometry (single pass, two-sided support) ─────────────────────
        for (int instIdx = 0; instIdx < (int)m_instances.size(); ++instIdx)
        {
            const LevelInstance& inst = m_instances[instIdx];
            LevelModel* mdl = inst.model;
            if (!mdl || mdl->parts.empty()) continue;
            // GameModeMask filter: skip instances not in the selected mode
            if (m_gameModeFilter >= 0) {
                int mask = inst.gameModeMask;
                if (mask != -1 && mask != 0 && !(mask & (1 << m_gameModeFilter)))
                    continue;
            }
            m_device->SetVertexShaderConstantF(0, inst.mat, 4);

            for (int pi = 0; pi < (int)mdl->parts.size(); ++pi)
            {
                const LevelMeshPart& part = mdl->parts[pi];
                if (!part.vb || !part.ib || part.indexCount < 3) continue;
                uint32_t pf = GetPartFlags(mdl, pi);
                // bit62 meshes also drawn in pass 0 as background — render again
                // here so they're not lost behind opaque geometry

                m_device->SetRenderState(D3DRS_CULLMODE, (pf & 2) ? D3DCULL_NONE : D3DCULL_CW);

                IDirect3DTexture9* tex = part.diffuseTex ? part.diffuseTex : m_whiteTex;
                if (tex != curTex) { curTex = tex; m_device->SetTexture(0, curTex); }

                m_device->SetStreamSource(0, part.vb, 0, LEVEL_VERTEX_SIZE);
                m_device->SetIndices(part.ib);
                m_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
                    0, 0, part.vertexCount, 0, part.indexCount / 3);
                ++calls;
            }
        }
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
            if (m_gameModeFilter >= 0) {
                int mask = inst.gameModeMask;
                if (mask != -1 && mask != 0 && !(mask & (1 << m_gameModeFilter)))
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

        DWORD colBottom = 0xC0FF4040; // red-ish bottom edges
        DWORD colTop    = 0x80FF8080; // lighter top edges
        DWORD colVert   = 0x60FFAAAA; // vertical lines

        for (int vi = 0; vi < (int)m_collisionVolumes.size(); ++vi) {
            const LevelCollisionVolume& cv = m_collisionVolumes[vi];
            if (cv.nodes.size() < 2) continue;

            // GameMode filter
            if (m_gameModeFilter >= 0 && cv.gameModeMask != -1 &&
                !(cv.gameModeMask & (1 << m_gameModeFilter))) continue;

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

        // -- Wireframe volume pass (with depth test, no depth write) --
        m_device->SetTexture(0, NULL);
        m_device->SetRenderState(D3DRS_LIGHTING,     FALSE);
        m_device->SetRenderState(D3DRS_CULLMODE,     D3DCULL_NONE);
        m_device->SetRenderState(D3DRS_ZENABLE,      TRUE);
        m_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
        m_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2);
        m_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        m_device->SetTextureStageState(1, D3DTSS_COLOROP,   D3DTOP_DISABLE);

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
        {
            const LevelEditorObj& eo = m_editorObjs[ei];
            // Relation filter: skip objects not in the filter set
            if (!m_editorObjFilter.empty() && m_editorObjFilter.find(eo.guid) == m_editorObjFilter.end()) continue;
            // Gamemode filter: skip objects that don't match current mode
            if (m_gameModeFilter >= 0 && eo.gameModeMask != -1 && !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
            char lbl[8] = {0};
            DWORD fixedCol = 0;
            if (!EditorGetTypeInfo(eo.type, lbl, fixedCol)) continue;
            DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;
            if (ei == m_selectedEditorIdx) col = 0xFFFFFFFF;
            else if (ei == m_hoveredEditorIdx) col = 0xFFCCCCCC;

            const float* m = eo.mat;
            D3DMATRIX wm;
            wm._11=m[0]; wm._12=m[1]; wm._13=m[2]; wm._14=m[3];
            wm._21=m[4]; wm._22=m[5]; wm._23=m[6]; wm._24=m[7];
            wm._31=m[8]; wm._32=m[9]; wm._33=m[10];wm._34=m[11];
            wm._41=m[12];wm._42=m[13];wm._43=m[14];wm._44=m[15];
            m_device->SetTransform(D3DTS_WORLD, &wm);

            // Scale down large volumes: clamp half-extents so they don't overwhelm the view
            const float VOL_MAX = 3.0f;
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

        for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
        {
            const LevelEditorObj& eo = m_editorObjs[ei];
            // Relation filter
            if (!m_editorObjFilter.empty() && m_editorObjFilter.find(eo.guid) == m_editorObjFilter.end()) continue;
            // Gamemode filter
            if (m_gameModeFilter >= 0 && eo.gameModeMask != -1 && !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
            char lbl[8] = {0};
            DWORD fixedCol = 0;
            if (!EditorGetTypeInfo(eo.type, lbl, fixedCol)) continue;
            DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;

            // World position from translation column
            float wx = eo.mat[12], wy = eo.mat[13], wz = eo.mat[14];

            // Identity world (marker is in world space directly)
            static const D3DMATRIX s_id2 = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            m_device->SetTransform(D3DTS_WORLD, &s_id2);

            const bool isSelEO = (ei == m_selectedEditorIdx);
            const bool isHovEO = (ei == m_hoveredEditorIdx);
            const float R = isSelEO ? 1.2f : 0.8f;
            if (isSelEO) col = 0xFFFFFFFF; // white highlight when selected
            else if (isHovEO) col = (col & 0x00FFFFFF) | 0xFFCCCCCC; // brighten on hover
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

            for (int ei = 0; ei < (int)m_editorObjs.size(); ++ei)
            {
                const LevelEditorObj& eo = m_editorObjs[ei];
                // Relation filter
                if (!m_editorObjFilter.empty() && m_editorObjFilter.find(eo.guid) == m_editorObjFilter.end()) continue;
                // Gamemode filter
                if (m_gameModeFilter >= 0 && eo.gameModeMask != -1 && !(eo.gameModeMask & (1 << m_gameModeFilter))) continue;
                char lbl[8] = {0};
                DWORD fixedCol = 0;
                if (!EditorGetTypeInfo(eo.type, lbl, fixedCol)) continue;
                DWORD col = fixedCol ? fixedCol : (DWORD)eo.color;

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
    if (!m_splines.empty() && m_showEditorObjs)
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
    }

    // Restore render state
    m_device->SetTexture(0, sTex);
    if (sTex) sTex->Release();

    static const D3DMATRIX s_id = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    m_device->SetTransform(D3DTS_WORLD, &s_id);
    m_device->SetFVF(sFVF);
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
    m_device->LightEnable(0, FALSE);
    m_device->SetVertexShader(sVS);
    m_device->SetPixelShader(sPS);
    if (sVS) sVS->Release();
    if (sPS) sPS->Release();
    if (sDecl) { m_device->SetVertexDeclaration(sDecl); sDecl->Release(); }
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
        if (!EditorGetTypeInfo(eo.type, lbl, col)) continue;

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
