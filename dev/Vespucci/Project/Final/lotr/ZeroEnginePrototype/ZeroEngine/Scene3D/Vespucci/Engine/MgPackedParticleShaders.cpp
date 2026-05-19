// MgPackedParticleShaders.cpp — Feeding Pandemic's Compiled Bytecode to Your GPU
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Loads the actual VS/PS bytecode from Shaders_PC_nvidia.bin, creates
// IDirect3DVertexShader9 and IDirect3DPixelShader9 objects, sets up
// constant registers (WVP matrix in c0-c3, camera pos in c4, time in c5),
// and renders particle quads with the EXACT same shaders Pandemic shipped.
// Their GPU code. Our GPU. 2008 meets 2026. The pixels are identical.
// -----------------------------------------------------------------------

#include "MgPackedParticleShaders.h"
#include "Scene3DRendererInternal.h" // RendererLog, HK types
#include "Scene3DEffects.h"
#include "ZeroMath.h"

#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>

struct PackedParticleVertex
{
    float px, py, pz;       // POSITION.xyz (particle center)
    float cr, cg, cb, ca;   // COLOR.rgba
    float rot, alphaMul, colorMul; // TEXCOORD0.xyz
    float u, v;             // TEXCOORD1.xy
    float cornerIndex;      // TEXCOORD2.x (0..3)
    float halfW, halfH;     // TEXCOORD3.xy
    float pivotX, pivotY;   // TEXCOORD4.xy (pivot offset in world units)
    float sunScale;         // TEXCOORD5.x
    float alphaRef;         // TEXCOORD6.x (0..1 threshold)
};

static bool ReadBinaryFile(const char* path, std::vector<unsigned char>& outBytes)
{
    outBytes.clear();
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

    outBytes.resize((size_t)size);
    size_t got = fread(&outBytes[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size)
    {
        outBytes.clear();
        return false;
    }
    return true;
}

static bool ReadPackedShaderBytes(const char* relPath, std::vector<unsigned char>& outBytes)
{
    const char* prefixes[] = { "", ".\\", "..\\", ".\\..\\", "..\\..\\" };
    char full[1024];
    for (int i = 0; i < (int)(sizeof(prefixes) / sizeof(prefixes[0])); ++i)
    {
        sprintf_s(full, "%s%s", prefixes[i], relPath);
        if (ReadBinaryFile(full, outBytes))
            return true;
    }
    return false;
}

static bool uvLooksHalfTexelLocal(float a0, float a1, unsigned int size)
{
    if (size == 0)
        return false;

    float p0 = a0 * (float)size;
    float p1 = a1 * (float)size;

    float frac0 = fabsf((p0 - floorf(p0)) - 0.5f);
    float frac1 = fabsf((p1 - floorf(p1)) - 0.5f);

    return (frac0 < 0.01f && frac1 < 0.01f);
}

static void MulD3DMatrix(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
{
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            out.m[r][c] =
                a.m[r][0] * b.m[0][c] +
                a.m[r][1] * b.m[1][c] +
                a.m[r][2] * b.m[2][c] +
                a.m[r][3] * b.m[3][c];
        }
    }
}

MgPackedParticleShaders::MgPackedParticleShaders()
    : m_decl(NULL)
    , m_vsBillboardVdA(NULL)
    , m_psAVd(NULL)
{
    m_shaderRoot[0] = '\0';
    strcpy_s(m_shaderRoot, "GameFiles\\lotrcparser\\Shaders_PC_nvidia");
}

MgPackedParticleShaders::~MgPackedParticleShaders()
{
    release();
}

void MgPackedParticleShaders::release()
{
    if (m_psAVd)
    {
        m_psAVd->Release();
        m_psAVd = NULL;
    }
    if (m_vsBillboardVdA)
    {
        m_vsBillboardVdA->Release();
        m_vsBillboardVdA = NULL;
    }
    if (m_decl)
    {
        m_decl->Release();
        m_decl = NULL;
    }
}

void MgPackedParticleShaders::setShaderRoot(const char* root)
{
    if (!root || !root[0])
        return;
    strcpy_s(m_shaderRoot, root);
}

bool MgPackedParticleShaders::ensureResources(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    if (m_decl && m_vsBillboardVdA && m_psAVd)
        return true;

    RendererLog("MgPackedParticleShaders: ensureResources begin");
    release();

    D3DVERTEXELEMENT9 decl[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 40, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 48, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
        { 0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3 },
        { 0, 60, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
        { 0, 68, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
        { 0, 72, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 },
        D3DDECL_END()
    };

    HRESULT hr = device->CreateVertexDeclaration(decl, &m_decl);
    if (FAILED(hr) || !m_decl)
    {
        RendererLog("MgPackedParticleShaders: CreateVertexDeclaration failed");
        release();
        return false;
    }
    RendererLog("MgPackedParticleShaders: CreateVertexDeclaration OK");

    std::vector<unsigned char> vsBytes;
    std::vector<unsigned char> psBytes;

    char vsRel[512];
    char psRel[512];
    sprintf_s(vsRel, "%s\\vertex_shaders\\Mg_VP_Particles_Billboard_Vd_A.vso", m_shaderRoot);
    sprintf_s(psRel, "%s\\fragment_shaders\\Mg_FP_Particles_A_Vd.pso", m_shaderRoot);

    if (!ReadPackedShaderBytes(vsRel, vsBytes))
    {
        RendererLog("MgPackedParticleShaders: Failed to read packed vertex shader bytes");
        release();
        return false;
    }
    if (!ReadPackedShaderBytes(psRel, psBytes))
    {
        RendererLog("MgPackedParticleShaders: Failed to read packed pixel shader bytes");
        release();
        return false;
    }
    {
        char msg[256];
        sprintf_s(msg, "MgPackedParticleShaders: Read bytes VS=%u PS=%u", (unsigned)vsBytes.size(), (unsigned)psBytes.size());
        RendererLog(msg);
    }

    hr = device->CreateVertexShader((const DWORD*)&vsBytes[0], &m_vsBillboardVdA);
    if (FAILED(hr) || !m_vsBillboardVdA)
    {
        RendererLog("MgPackedParticleShaders: CreateVertexShader failed");
        release();
        return false;
    }
    RendererLog("MgPackedParticleShaders: CreateVertexShader OK");

    hr = device->CreatePixelShader((const DWORD*)&psBytes[0], &m_psAVd);
    if (FAILED(hr) || !m_psAVd)
    {
        RendererLog("MgPackedParticleShaders: CreatePixelShader failed");
        release();
        return false;
    }
    RendererLog("MgPackedParticleShaders: CreatePixelShader OK");

    RendererLog("MgPackedParticleShaders: Loaded packed particle shaders");
    return true;
}

bool MgPackedParticleShaders::renderBillboard_A_Vd(
    IDirect3DDevice9* device,
    hkgDisplayContext* context,
    hkgCamera* camera,
    const hkVector4& cameraPos,
    const hkVector4& cameraTarget,
    ParticleEmitter* emitter)
{
    if (!device || !context || !camera || !emitter)
        return false;

    if (emitter->particleType != ParticleEmitter::BILLBOARD)
        return false;
    if (emitter->orientMode != ParticleEmitter::ORIENT_NONE)
        return false;

    const int particleCount = emitter->particles.getSize();
    if (particleCount <= 0)
        return true;

    if (!emitter->texture)
        return false;

    if (!ensureResources(device))
        return false;

    static int s_dbg = 0;
    if (s_dbg < 10)
    {
        char msg[256];
        sprintf_s(msg, "MgPackedParticleShaders: render begin emitter='%s' particles=%d", emitter->name, particleCount);
        RendererLog(msg);
        s_dbg++;
    }

    context->flush();

    IDirect3DStateBlock9* stateBlock = NULL;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || !stateBlock)
        return false;
    if (FAILED(stateBlock->Capture()))
    {
        stateBlock->Release();
        return false;
    }

    device->SetVertexDeclaration(m_decl);
    device->SetVertexShader(m_vsBillboardVdA);
    device->SetPixelShader(m_psAVd);

    device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);

    if (emitter->blendMode == ParticleEmitter::ADDITIVE)
    {
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    else
    {
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    device->SetTexture(1, NULL);
    device->SetTexture(2, NULL);
    device->SetTexture(3, NULL);

    // Bind stage 0 via Havok, forcing a rebind.
    context->setCurrentTexture(0, HK_NULL, HKG_TEXTURE_MODULATE);
    context->setCurrentTexture(0, emitter->texture, HKG_TEXTURE_MODULATE);

    // Constants (from shader disassembly / CTAB):
    //  VS: g__worldMatrix=c178..c181, g__viewProjMatrix=c239..c242, g__cameraRight=c243, g__cameraUp=c244, g__cameraPos=c245
    D3DMATRIX world;
    memset(&world, 0, sizeof(world));
    world.m[0][0] = 1.0f;
    world.m[1][1] = 1.0f;
    world.m[2][2] = 1.0f;
    world.m[3][3] = 1.0f;

    D3DMATRIX view;
    D3DMATRIX proj;
    D3DMATRIX viewProj;
    memset(&view, 0, sizeof(view));
    memset(&proj, 0, sizeof(proj));
    view.m[0][0] = view.m[1][1] = view.m[2][2] = view.m[3][3] = 1.0f;
    proj.m[0][0] = proj.m[1][1] = proj.m[2][2] = proj.m[3][3] = 1.0f;

    device->GetTransform(D3DTS_VIEW, &view);
    device->GetTransform(D3DTS_PROJECTION, &proj);
    MulD3DMatrix(view, proj, viewProj);

    device->SetVertexShaderConstantF(178, (const float*)&world.m[0][0], 4);
    device->SetVertexShaderConstantF(239, (const float*)&viewProj.m[0][0], 4);

    float camUp[3], camRight[3];
    camera->getUp(camUp);
    camera->getRight(camRight);

    float cRight[4] = { camRight[0], camRight[1], camRight[2], 0.0f };
    float cUp[4] = { camUp[0], camUp[1], camUp[2], 0.0f };
    float cPos[4] = { cameraPos(0), cameraPos(1), cameraPos(2), 1.0f };
    device->SetVertexShaderConstantF(243, cRight, 1);
    device->SetVertexShaderConstantF(244, cUp, 1);
    device->SetVertexShaderConstantF(245, cPos, 1);

    float sunCol[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    device->SetPixelShaderConstantF(2, sunCol, 1);

    int cols = emitter->uvColumns;
    int rows = emitter->uvRows;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    const AtlasUVRect& rect = emitter->atlasUVRect;
    unsigned int texW = emitter->texture ? emitter->texture->getWidth() : 0;
    unsigned int texH = emitter->texture ? emitter->texture->getHeight() : 0;

    const bool halfU = uvLooksHalfTexelLocal(rect.u0, rect.u1, texW);
    const bool halfV = uvLooksHalfTexelLocal(rect.v0, rect.v1, texH);

    float x0 = halfU ? (rect.u0 * (float)texW - 0.5f) : (rect.u0 * (float)texW);
    float x1 = halfU ? (rect.u1 * (float)texW + 0.5f) : (rect.u1 * (float)texW);
    float y0 = halfV ? (rect.v0 * (float)texH - 0.5f) : (rect.v0 * (float)texH);
    float y1 = halfV ? (rect.v1 * (float)texH + 0.5f) : (rect.v1 * (float)texH);

    float rectW = x1 - x0;
    float rectH = y1 - y0;
    if (rectW <= 0.0f) rectW = 1.0f;
    if (rectH <= 0.0f) rectH = 1.0f;

    const float frameW = rectW / (float)cols;
    const float frameH = rectH / (float)rows;

    std::vector<int> sortedIdx;
    if (emitter->sortParticles && particleCount > 1)
    {
        sortedIdx.resize(particleCount);
        for (int i = 0; i < particleCount; i++)
            sortedIdx[i] = i;

        float fx = cameraTarget(0) - cameraPos(0);
        float fy = cameraTarget(1) - cameraPos(1);
        float fz = cameraTarget(2) - cameraPos(2);
        float fl2 = fx*fx + fy*fy + fz*fz;
        if (fl2 < 1e-6f) { fx = 0.0f; fy = 0.0f; fz = 1.0f; }
        else              { ZNormalize3f(fx, fy, fz); }

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
            DepthCmp(emitter, cameraPos(0), cameraPos(1), cameraPos(2), fx, fy, fz));
    }

    std::vector<PackedParticleVertex> verts;
    verts.reserve(particleCount * 6);

    for (int drawIdx = 0; drawIdx < particleCount; ++drawIdx)
    {
        const int i = sortedIdx.empty() ? drawIdx : sortedIdx[drawIdx];
        const Particle& p = emitter->particles[i];

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
        float alphaRef = (float)aRef / 255.0f;

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
                u0 = rect.u0;
                u1 = rect.u1;
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
                v0_uv = rect.v0;
                v1_uv = rect.v1;
            }
        }
        else
        {
            u0 = rect.u0;
            v0_uv = rect.v0;
            u1 = rect.u1;
            v1_uv = rect.v1;
        }

        float pivotX = p.pivotX;
        float pivotY = p.pivotY;
        if (pivotX < 0.0f) pivotX = 0.0f; else if (pivotX > 1.0f) pivotX = 1.0f;
        if (pivotY < 0.0f) pivotY = 0.0f; else if (pivotY > 1.0f) pivotY = 1.0f;

        const float halfW = p.width * 0.5f;
        const float halfH = p.height * 0.5f;
        const float pivotOffX = (0.5f - pivotX) * p.width;
        const float pivotOffY = (0.5f - pivotY) * p.height;

        const float rot = p.rotation(1);

        const float cr = (float)p.color.r / 255.0f;
        const float cg = (float)p.color.g / 255.0f;
        const float cb = (float)p.color.b / 255.0f;
        const float ca = (float)p.color.a / 255.0f;

        const float colorMul = p.colorMultiplier;
        const float alphaMul = 1.0f;
        const float sunScale = 0.0f;

        const float idxBL = 1.0f;
        const float idxBR = 0.0f;
        const float idxTR = 3.0f;
        const float idxTL = 2.0f;

        PackedParticleVertex vBL;
        vBL.px = p.position(0); vBL.py = p.position(1); vBL.pz = p.position(2);
        vBL.cr = cr; vBL.cg = cg; vBL.cb = cb; vBL.ca = ca;
        vBL.rot = rot; vBL.alphaMul = alphaMul; vBL.colorMul = colorMul;
        vBL.u = u0; vBL.v = v1_uv;
        vBL.cornerIndex = idxBL;
        vBL.halfW = halfW; vBL.halfH = halfH;
        vBL.pivotX = pivotOffX; vBL.pivotY = pivotOffY;
        vBL.sunScale = sunScale;
        vBL.alphaRef = alphaRef;

        PackedParticleVertex vBR = vBL;
        vBR.u = u1; vBR.v = v1_uv;
        vBR.cornerIndex = idxBR;

        PackedParticleVertex vTR = vBL;
        vTR.u = u1; vTR.v = v0_uv;
        vTR.cornerIndex = idxTR;

        PackedParticleVertex vTL = vBL;
        vTL.u = u0; vTL.v = v0_uv;
        vTL.cornerIndex = idxTL;

        verts.push_back(vBL);
        verts.push_back(vBR);
        verts.push_back(vTR);
        verts.push_back(vBL);
        verts.push_back(vTR);
        verts.push_back(vTL);
    }

    bool ok = true;
    if (!verts.empty())
    {
        const UINT primCount = (UINT)(verts.size() / 3);
        if (s_dbg < 10)
        {
            char msg[256];
            sprintf_s(msg, "MgPackedParticleShaders: draw prims=%u verts=%u", (unsigned)primCount, (unsigned)verts.size());
            RendererLog(msg);
        }
        HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, primCount, &verts[0], sizeof(PackedParticleVertex));
        if (FAILED(hr))
            ok = false;
    }

    // Clear stage 0 in Havok's cache before restoring D3D state.
    context->setCurrentTexture(0, HK_NULL, HKG_TEXTURE_MODULATE);

    stateBlock->Apply();
    stateBlock->Release();
    return ok;
}
