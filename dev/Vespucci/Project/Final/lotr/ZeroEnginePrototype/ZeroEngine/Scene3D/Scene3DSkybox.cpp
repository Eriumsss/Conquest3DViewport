// Scene3DSkybox.cpp — The Sky Above Pandemic's Dead Worlds
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Skybox management: preloads skybox models from the game files (each
// level in Conquest had its own sky — Minas Tirith's fiery war sky,
// Helm's Deep's stormy clouds, the Shire's peaceful blue), handles
// skybox selection for cycling between loaded skies, supports three
// render modes (backdrop, mesh, hybrid), and an optional cloud layer
// overlay using a second skybox model.
//
// The rendering pass disables depth writes, renders the sky mesh at
// infinite distance (camera-centered), then re-enables depth for the
// scene. Classic skybox technique. Pandemic did it exactly the same
// way — we found the D3D9 state calls (SetRenderState D3DRS_ZWRITEENABLE
// FALSE) in the .exe's render function at 0x00732A80.
//
// "Yesterday is history, tomorrow is a mystery, today is a gift."
//   — Eleanor Roosevelt. Yesterday Pandemic's skies were trapped in
// binary files nobody could read. Tomorrow someone might port this to
// Vulkan. Today, the sky renders in D3D9 at 60fps and it's beautiful
// and I don't give a fuck that the API is 22 years old.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "GameModelLoader.h"
#include <Graphics/Common/Texture/hkgTexture.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ZeroMath.h"

// ---------------------------------------------------------------------------
// Skybox management
// ---------------------------------------------------------------------------
void Scene3DRenderer::clearSkyboxes()
{
    for (size_t i = 0; i < m_skyboxes.size(); ++i)
    {
        if (m_skyboxes[i].model)
        {
            m_skyboxes[i].model->release();
            delete m_skyboxes[i].model;
            m_skyboxes[i].model = NULL;
        }
    }
    m_skyboxes.clear();
    m_activeSkyboxIndex = -1;
    m_cloudLayerEnabled = false;
    m_cloudSkyboxIndex = -1;
}

bool Scene3DRenderer::preloadSkyboxModel(const char* skyboxName, const char* jmodelPath, const char* glbPath, const char* textureDir)
{
    if (!m_context || !jmodelPath || !glbPath)
    {
        return false;
    }

    GameModelLoader modelLoader;
    modelLoader.setDisplayContext(m_context);
    GameModel* model = modelLoader.loadModel(jmodelPath, glbPath, textureDir);
    if (!model)
    {
        return false;
    }

    SkyboxEntry entry;
    entry.model = model;
    entry.name[0] = '\0';
    if (skyboxName && skyboxName[0] != '\0')
    {
        strcpy_s(entry.name, skyboxName);
    }
    else
    {
        const char* p = strrchr(jmodelPath, '\\');
        if (!p) p = strrchr(jmodelPath, '/');
        strcpy_s(entry.name, p ? (p + 1) : jmodelPath);
    }
    m_skyboxes.push_back(entry);
    if (m_activeSkyboxIndex < 0)
    {
        m_activeSkyboxIndex = 0;
    }

    char msg[256];
    sprintf_s(msg, "Preloaded skybox: %s", entry.name);
    RendererLog(msg);
    return true;
}

const char* Scene3DRenderer::getActiveSkyboxName() const
{
    if (m_activeSkyboxIndex < 0 || m_activeSkyboxIndex >= (int)m_skyboxes.size())
    {
        return "<none>";
    }
    return m_skyboxes[m_activeSkyboxIndex].name;
}

const char* Scene3DRenderer::getSkyboxName(int index) const
{
    if (index < 0 || index >= (int)m_skyboxes.size())
    {
        return "<none>";
    }
    return m_skyboxes[index].name;
}

void Scene3DRenderer::setActiveSkyboxIndex(int index)
{
    if (m_skyboxes.empty())
    {
        m_activeSkyboxIndex = -1;
        return;
    }
    if (index < 0) index = 0;
    if (index >= (int)m_skyboxes.size()) index = (int)m_skyboxes.size() - 1;
    m_activeSkyboxIndex = index;
}

bool Scene3DRenderer::selectSkyboxByName(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return false;
    }
    for (int i = 0; i < (int)m_skyboxes.size(); ++i)
    {
        if (_stricmp(m_skyboxes[i].name, name) == 0)
        {
            m_activeSkyboxIndex = i;
            return true;
        }
    }
    return false;
}

void Scene3DRenderer::setCloudSkyboxIndex(int index)
{
    if (m_skyboxes.empty())
    {
        m_cloudSkyboxIndex = -1;
        return;
    }
    if (index < 0) index = 0;
    if (index >= (int)m_skyboxes.size()) index = (int)m_skyboxes.size() - 1;
    m_cloudSkyboxIndex = index;
}

const char* Scene3DRenderer::getCloudSkyboxName() const
{
    if (m_cloudSkyboxIndex < 0 || m_cloudSkyboxIndex >= (int)m_skyboxes.size())
    {
        return "<none>";
    }
    return m_skyboxes[m_cloudSkyboxIndex].name;
}

void Scene3DRenderer::selectNextSkybox()
{
    if (m_skyboxes.empty())
    {
        m_activeSkyboxIndex = -1;
        return;
    }
    m_activeSkyboxIndex++;
    if (m_activeSkyboxIndex >= (int)m_skyboxes.size())
    {
        m_activeSkyboxIndex = 0;
    }
}

void Scene3DRenderer::setSkyRenderMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    m_skyRenderMode = static_cast<SkyRenderMode>(mode);
}

const char* Scene3DRenderer::getSkyRenderModeName() const
{
    if (m_skyRenderMode == SKY_MESH) return "MESH";
    if (m_skyRenderMode == SKY_HYBRID) return "HYBRID";
    return "BACKDROP";
}

// ---------------------------------------------------------------------------
// Skybox mesh rendering pass
// ---------------------------------------------------------------------------
void Scene3DRenderer::renderSkyboxMesh()
{
    if (!m_context || !m_skyboxEnabled)
    {
        return;
    }
    if (m_activeSkyboxIndex < 0 || m_activeSkyboxIndex >= (int)m_skyboxes.size())
    {
        return;
    }

    GameModel* model = m_skyboxes[m_activeSkyboxIndex].model;
    if (!model || model->meshPartCount <= 0)
    {
        return;
    }

    // Separate pass for sky rendering: keep depth test, disable depth writes.
    m_context->setDepthReadState(false);
    m_context->setDepthWriteState(false);
    m_context->setBlendState(false);
    m_context->setCullFaceState(false);
    m_context->setLightingState(false);
    m_context->setTexture2DState(true);

    // Reset D3D texture stage to standard modulate mode.
    // This guards against state leaks from the particle renderer's
    // no-texture fallback which switches stage 0 to SELECTARG1/DIFFUSE.
    if (m_d3dDevice)
    {
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }

    hkgTexture* primaryTexture = HK_NULL;
    bool hasNoUvSkyPart = false;
    for (int i = 0; i < model->meshPartCount; i++)
    {
        GameModel::MeshPart& p = model->meshParts[i];
        if (!primaryTexture && p.diffuseTexture)
        {
            primaryTexture = p.diffuseTexture;
        }
        if (!p.hasTexcoord0)
        {
            hasNoUvSkyPart = true;
        }
    }


    // Mode 0 (BACKDROP) is the safe default for this toolchain:
    // it avoids game-specific atmosphere shells that appear as central opaque spheres.
    const bool drawBackdrop = (m_skyRenderMode != SKY_MESH);
    const bool drawMesh = (m_skyRenderMode != SKY_BACKDROP);
    const bool skipNoUvShells = (m_skyRenderMode != SKY_MESH);

    if (drawBackdrop && primaryTexture && m_windowWidth > 0 && m_windowHeight > 0)
    {
        float cx = m_cameraDistance * cosf(m_cameraPitch) * sinf(m_cameraYaw) + m_cameraTarget(0);
        float cy = m_cameraDistance * sinf(m_cameraPitch) + m_cameraTarget(1);
        float cz = m_cameraDistance * cosf(m_cameraPitch) * cosf(m_cameraYaw) + m_cameraTarget(2);

        float tx = m_cameraTarget(0);
        float ty = m_cameraTarget(1);
        float tz = m_cameraTarget(2);

        float fx = tx - cx;
        float fy = ty - cy;
        float fz = tz - cz;
        float fl = ZLength3f(fx, fy, fz);
        if (fl > 1e-5f)
        {
            ZNormalize3f(fx, fy, fz);

            float rx = -fz, ry = 0.0f, rz = fx;
            float rl = ZLength3f(rx, ry, rz);
            if (rl < 1e-5f)
            {
                rx = 1.0f; ry = 0.0f; rz = 0.0f;
                rl = 1.0f;
            }
            ZNormalize3f(rx, ry, rz);

            float ux, uy, uz;
            ZCross3f(ux, uy, uz, rx, ry, rz, fx, fy, fz);
            float ul = ZLength3f(ux, uy, uz);
            if (ul > 1e-5f)
            {
                ZNormalize3f(ux, uy, uz);
            }
            else
            {
                ux = 0.0f; uy = 1.0f; uz = 0.0f;
            }

            float aspect = (float)m_windowWidth / (float)m_windowHeight;
            float tanHalf = tanf((m_cameraFovDegrees * 0.5f) * 0.0174532925f);
            float dist = 50.0f;
            float halfH = tanHalf * dist;
            float halfW = halfH * aspect;

            float center[3] = { cx + fx * dist, cy + fy * dist, cz + fz * dist };
            float tl[3] = { center[0] - rx * halfW + ux * halfH, center[1] - ry * halfW + uy * halfH, center[2] - rz * halfW + uz * halfH };
            float tr[3] = { center[0] + rx * halfW + ux * halfH, center[1] + ry * halfW + uy * halfH, center[2] + rz * halfW + uz * halfH };
            float bl[3] = { center[0] - rx * halfW - ux * halfH, center[1] - ry * halfW - uy * halfH, center[2] - rz * halfW - uz * halfH };
            float br[3] = { center[0] + rx * halfW - ux * halfH, center[1] + ry * halfW - uy * halfH, center[2] + rz * halfW - uz * halfH };
            float uvTL[2] = { 0.0f, 0.0f };
            float uvTR[2] = { 1.0f, 0.0f };
            float uvBL[2] = { 0.0f, 1.0f };
            float uvBR[2] = { 1.0f, 1.0f };
            float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

            m_context->setCurrentTexture(0, primaryTexture, HKG_TEXTURE_MODULATE);
            m_context->setTexture2DState(true);
            m_context->setCurrentColor4(white);
            m_context->beginGroup(HKG_IMM_TRIANGLE_LIST);
            m_context->setCurrentTextureCoord(uvTL); m_context->setCurrentPosition(tl);
            m_context->setCurrentTextureCoord(uvBL); m_context->setCurrentPosition(bl);
            m_context->setCurrentTextureCoord(uvTR); m_context->setCurrentPosition(tr);
            m_context->setCurrentTextureCoord(uvTR); m_context->setCurrentPosition(tr);
            m_context->setCurrentTextureCoord(uvBL); m_context->setCurrentPosition(bl);
            m_context->setCurrentTextureCoord(uvBR); m_context->setCurrentPosition(br);
            m_context->endGroup();
        }
    }

    int renderedParts = 0;
    int skippedMode = 0;
    int skippedNoTexture = 0;
    int skippedNoUv = 0;
    for (int i = 0; i < model->meshPartCount; i++)
    {
        GameModel::MeshPart& part = model->meshParts[i];
        if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
        {
            continue;
        }

        if (!drawMesh)
        {
            skippedMode++;
            continue;
        }

        if (skipNoUvShells && !part.hasTexcoord0)
        {
            skippedNoUv++;
            continue;
        }

        // Skyboxes should never render untextured parts; those appear as white domes.
        if (!part.diffuseTexture)
        {
            skippedNoTexture++;
            continue;
        }


        m_context->setCurrentTexture(0, part.diffuseTexture, HKG_TEXTURE_MODULATE);
        m_context->setTexture2DState(true);

        float white[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_context->setCurrentColor4(white);
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

            if (part.vertexStride >= 20)
            {
                float uv0[2] = { v0[3], v0[4] };
                float uv1[2] = { v1[3], v1[4] };
                float uv2[2] = { v2[3], v2[4] };
                float p0[3] = { v0[0], v0[1], v0[2] };
                float p1[3] = { v1[0], v1[1], v1[2] };
                float p2[3] = { v2[0], v2[1], v2[2] };

                m_context->setCurrentTextureCoord(uv0); m_context->setCurrentPosition(p0);
                m_context->setCurrentTextureCoord(uv1); m_context->setCurrentPosition(p1);
                m_context->setCurrentTextureCoord(uv2); m_context->setCurrentPosition(p2);
            }
            else
            {
                float uv[2] = { 0.0f, 0.0f };
                float p0[3] = { v0[0], v0[1], v0[2] };
                float p1[3] = { v1[0], v1[1], v1[2] };
                float p2[3] = { v2[0], v2[1], v2[2] };
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p0);
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p1);
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p2);
            }
        }

        m_context->endGroup();
        renderedParts++;
    }

    m_context->flush();
    m_context->setTexture2DState(false);
    m_context->setCurrentTexture(0, HK_NULL, HKG_TEXTURE_MODULATE);
    // Restore expected solid state for regular scene passes.
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(true);
    m_context->setBlendState(false);
    m_context->setCullFaceState(false);
    m_context->setLightingState(false);

    static int s_lastSkyboxIndex = -1;
    static SkyRenderMode s_lastSkyRenderMode = static_cast<SkyRenderMode>(-1);
    static int s_lastSkippedMode = -1;
    static int s_lastRenderedParts = -1;
    static int s_lastSkippedNoTexture = -1;
    static int s_lastSkippedNoUv = -1;
    if (s_lastSkyboxIndex != m_activeSkyboxIndex ||
        s_lastSkyRenderMode != m_skyRenderMode ||
        s_lastSkippedMode != skippedMode ||
        s_lastRenderedParts != renderedParts ||
        s_lastSkippedNoTexture != skippedNoTexture ||
        s_lastSkippedNoUv != skippedNoUv)
    {
        char msg[320];
        sprintf_s(msg, "Skybox '%s' mode=%s: rendered=%d skippedMode=%d skippedNoTexture=%d skippedNoUv=%d hasNoUv=%d",
            getActiveSkyboxName(), getSkyRenderModeName(), renderedParts, skippedMode, skippedNoTexture, skippedNoUv, hasNoUvSkyPart ? 1 : 0);
        RendererLog(msg);
        s_lastSkyboxIndex = m_activeSkyboxIndex;
        s_lastSkyRenderMode = m_skyRenderMode;
        s_lastSkippedMode = skippedMode;
        s_lastRenderedParts = renderedParts;
        s_lastSkippedNoTexture = skippedNoTexture;
        s_lastSkippedNoUv = skippedNoUv;
    }

    // -----------------------------------------------------------------------
    // Optional cloud overlay pass (second sky layer)
    // -----------------------------------------------------------------------
    if (!m_cloudLayerEnabled)
    {
        return;
    }
    if (m_cloudSkyboxIndex < 0 || m_cloudSkyboxIndex >= (int)m_skyboxes.size())
    {
        return;
    }
    if (m_cloudSkyboxIndex == m_activeSkyboxIndex)
    {
        return;
    }

    GameModel* cloud = m_skyboxes[m_cloudSkyboxIndex].model;
    if (!cloud || cloud->meshPartCount <= 0)
    {
        return;
    }

    // Cloud shells typically rely on texture alpha; render with alpha blending.
    m_context->setDepthReadState(false);
    m_context->setDepthWriteState(false);
    m_context->setCullFaceState(false);
    m_context->setLightingState(false);
    m_context->setTexture2DState(true);
    m_context->setBlendState(true);
    if (m_d3dDevice)
    {
        m_d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        m_d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        // Keep alpha stage in MODULATE mode so texture alpha controls blending.
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        m_d3dDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }

    int cloudRendered = 0;
    int cloudSkippedNoTexture = 0;
    int cloudSkippedNoUv = 0;
    for (int i = 0; i < cloud->meshPartCount; ++i)
    {
        GameModel::MeshPart& part = cloud->meshParts[i];
        if (!part.vertices || !part.indices || part.vertexCount <= 0 || part.indexCount <= 0)
        {
            continue;
        }
        // Always skip no-UV shells for the overlay layer; those are usually atmosphere blockers.
        if (!part.hasTexcoord0)
        {
            cloudSkippedNoUv++;
            continue;
        }
        if (!part.diffuseTexture)
        {
            cloudSkippedNoTexture++;
            continue;
        }

        m_context->setCurrentTexture(0, part.diffuseTexture, HKG_TEXTURE_MODULATE);
        m_context->setTexture2DState(true);

        float white[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        m_context->setCurrentColor4(white);
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

            if (part.vertexStride >= 20)
            {
                float uv0[2] = { v0[3], v0[4] };
                float uv1[2] = { v1[3], v1[4] };
                float uv2[2] = { v2[3], v2[4] };
                float p0[3] = { v0[0], v0[1], v0[2] };
                float p1[3] = { v1[0], v1[1], v1[2] };
                float p2[3] = { v2[0], v2[1], v2[2] };

                m_context->setCurrentTextureCoord(uv0); m_context->setCurrentPosition(p0);
                m_context->setCurrentTextureCoord(uv1); m_context->setCurrentPosition(p1);
                m_context->setCurrentTextureCoord(uv2); m_context->setCurrentPosition(p2);
            }
            else
            {
                float uv[2] = { 0.0f, 0.0f };
                float p0[3] = { v0[0], v0[1], v0[2] };
                float p1[3] = { v1[0], v1[1], v1[2] };
                float p2[3] = { v2[0], v2[1], v2[2] };
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p0);
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p1);
                m_context->setCurrentTextureCoord(uv); m_context->setCurrentPosition(p2);
            }
        }

        m_context->endGroup();
        cloudRendered++;
    }

    m_context->flush();
    m_context->setTexture2DState(false);
    m_context->setCurrentTexture(0, HK_NULL, HKG_TEXTURE_MODULATE);
    m_context->setDepthReadState(true);
    m_context->setDepthWriteState(true);
    m_context->setBlendState(false);
    m_context->setCullFaceState(false);
    m_context->setLightingState(false);

    static int s_lastCloudIndex = -1;
    static int s_lastCloudRendered = -1;
    static int s_lastCloudSkippedNoTexture = -1;
    static int s_lastCloudSkippedNoUv = -1;
    if (s_lastCloudIndex != m_cloudSkyboxIndex ||
        s_lastCloudRendered != cloudRendered ||
        s_lastCloudSkippedNoTexture != cloudSkippedNoTexture ||
        s_lastCloudSkippedNoUv != cloudSkippedNoUv)
    {
        char msg[320];
        sprintf_s(msg, "Cloud layer '%s': rendered=%d skippedNoTexture=%d skippedNoUv=%d",
            getCloudSkyboxName(), cloudRendered, cloudSkippedNoTexture, cloudSkippedNoUv);
        RendererLog(msg);
        s_lastCloudIndex = m_cloudSkyboxIndex;
        s_lastCloudRendered = cloudRendered;
        s_lastCloudSkippedNoTexture = cloudSkippedNoTexture;
        s_lastCloudSkippedNoUv = cloudSkippedNoUv;
    }
}
