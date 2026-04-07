// Scene3DMaterial.cpp — D3D9 Material State: The Art of Making Shit Look Right
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Material management: DDS texture loading and reloading, gamma correction
// toggle (Pandemic shipped in gamma space like everyone in 2008), anisotropic
// filtering (the stolen Havok SDK sets it to 1x by default — we crank it),
// mip bias tuning, rim lighting for character silhouettes, material info
// queries for the ImGui inspector, and validation warnings when textures
// are missing or formats are wrong.
//
// Pandemic stored material references as CRC32 hashes pointing to
// TextureInfo entries in the PAK header. The actual DDS data lives in
// the BIN file at the offset specified by the BinAssetHandle. Three
// levels of indirection between "I want this texture" and "here are
// the pixels." Every level can fail. Every failure is silent. Every
// goddamn time a texture doesn't load, you get a white square and NO
// indication of which CRC failed or why. Debugging material issues
// in this pipeline is an exercise in pure suffering.
//
// "Know thyself." — Socrates. I know myself. I am the kind of person
// who will spend 4 hours debugging a white texture only to discover
// the DDS file was DXT5 and I was creating it as DXT1. I know this
// about myself. I accept this about myself.
// -----------------------------------------------------------------------

#include "Scene3DRendererInternal.h"
#include "GameModelLoader.h"
#include <Graphics/Common/Texture/hkgTexture.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------
static bool HasDdsExtensionLocal(const char* name)
{
    if (!name) return false;
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return (_stricmp(dot, ".dds") == 0);
}

static hkgTexture* LoadTextureFromDir(hkgDisplayContext* context, const char* textureDir, const char* name)
{
    if (!context || !name || name[0] == '\0') return HK_NULL;

    char path[512];
    if (HasDdsExtensionLocal(name))
        sprintf_s(path, "%s%s", textureDir, name);
    else
        sprintf_s(path, "%s%s.dds", textureDir, name);

    hkgTexture* tex = hkgTexture::create(context);
    if (!tex) return HK_NULL;
    if (!tex->loadFromFile(path))
    {
        tex->removeReference();
        return HK_NULL;
    }
    tex->setAutoMipMaps(true);
    tex->setAutoCompression(false);
    tex->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    tex->setTextureWrapModeU(HKG_TEXTURE_WRAP);
    tex->setTextureWrapModeV(HKG_TEXTURE_WRAP);
    tex->realize(false);
    tex->setFilterMode((HKG_TEXTURE_FILTER)(HKG_TEXTURE_FILTER_LINEAR | HKG_TEXTURE_FILTER_ANISOTROPIC));
    return tex;
}

// ---------------------------------------------------------------------------
// Gamma / Aniso / Mip bias / Rim light
// ---------------------------------------------------------------------------
void Scene3DRenderer::setGammaEnabled(bool enabled)
{
    m_gammaEnabled = enabled;
    if (m_d3dDevice)
    {
        m_d3dDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, enabled ? TRUE : FALSE);
        for (int s = 0; s < 4; ++s)
        {
            m_d3dDevice->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, enabled ? TRUE : FALSE);
        }
    }
}

void Scene3DRenderer::setAnisotropy(int maxAniso)
{
    if (maxAniso < 1) maxAniso = 1;
    if (maxAniso > 16) maxAniso = 16;
    m_maxAnisotropy = maxAniso;
    if (m_context)
    {
        m_context->setMaxAnisotropy((hkUint8)m_maxAnisotropy);
    }
}

void Scene3DRenderer::setMipBias(float bias)
{
    m_mipBias = bias;
}

void Scene3DRenderer::toggleRimLight()
{
    m_rimLightEnabled = !m_rimLightEnabled;
    setupDefaultLighting();
}

// ---------------------------------------------------------------------------
// Texture reload
// ---------------------------------------------------------------------------
bool Scene3DRenderer::reloadMaterialTextures(int partIndex)
{
    if (!m_context || !m_gameModel || !m_gameModel->meshParts) return false;

    bool updated = false;
    const char* texDir = "../GameFiles/textures/";
    int start = 0;
    int end = m_gameModel->meshPartCount;
    if (partIndex >= 0 && partIndex < m_gameModel->meshPartCount)
    {
        start = partIndex;
        end = partIndex + 1;
    }

    for (int i = start; i < end; ++i)
    {
        GameModel::MeshPart& part = m_gameModel->meshParts[i];
        bool any = false;

        if (part.diffuseName[0] != '\0')
        {
            hkgTexture* tex = LoadTextureFromDir(m_context, texDir, part.diffuseName);
            if (tex)
            {
                if (part.diffuseTexture) part.diffuseTexture->removeReference();
                part.diffuseTexture = tex;
                any = true;
            }
        }
        if (part.normalName[0] != '\0')
        {
            hkgTexture* tex = LoadTextureFromDir(m_context, texDir, part.normalName);
            if (tex)
            {
                if (part.normalTexture) part.normalTexture->removeReference();
                part.normalTexture = tex;
                any = true;
            }
        }
        if (part.specularName[0] != '\0')
        {
            hkgTexture* tex = LoadTextureFromDir(m_context, texDir, part.specularName);
            if (tex)
            {
                if (part.specularTexture) part.specularTexture->removeReference();
                part.specularTexture = tex;
                any = true;
            }
        }

        if (any)
        {
            updated = true;
        }
    }

    if (updated) RendererLog("Material textures reloaded.");
    return updated;
}

// ---------------------------------------------------------------------------
// Material info queries
// ---------------------------------------------------------------------------
bool Scene3DRenderer::getPrimaryMaterialInfo(MaterialInfo& outInfo) const
{
    if (!m_gameModel || m_gameModel->meshPartCount <= 0)
    {
        return false;
    }
    const GameModel::MeshPart& p = m_gameModel->meshParts[0];
    strcpy_s(outInfo.material, sizeof(outInfo.material), p.materialName);
    strcpy_s(outInfo.diffuse, sizeof(outInfo.diffuse), p.diffuseName);
    strcpy_s(outInfo.normal, sizeof(outInfo.normal), p.normalName);
    strcpy_s(outInfo.specular, sizeof(outInfo.specular), p.specularName);
    outInfo.hasDiffuse = (p.diffuseTexture != HK_NULL);
    outInfo.hasNormal = (p.normalTexture != HK_NULL);
    outInfo.hasSpecular = (p.specularTexture != HK_NULL);
    outInfo.hasTexcoord0 = p.hasTexcoord0;
    outInfo.generatedUV = p.generatedSkyUV;
    return true;
}

int Scene3DRenderer::getMaterialCount() const
{
    return (m_gameModel) ? m_gameModel->meshPartCount : 0;
}

bool Scene3DRenderer::getMaterialInfo(int idx, MaterialInfo& outInfo) const
{
    if (!m_gameModel || idx < 0 || idx >= m_gameModel->meshPartCount)
    {
        return false;
    }
    const GameModel::MeshPart& p = m_gameModel->meshParts[idx];
    strcpy_s(outInfo.material, sizeof(outInfo.material), p.materialName);
    strcpy_s(outInfo.diffuse, sizeof(outInfo.diffuse), p.diffuseName);
    strcpy_s(outInfo.normal, sizeof(outInfo.normal), p.normalName);
    strcpy_s(outInfo.specular, sizeof(outInfo.specular), p.specularName);
    outInfo.hasDiffuse = (p.diffuseTexture != HK_NULL);
    outInfo.hasNormal = (p.normalTexture != HK_NULL);
    outInfo.hasSpecular = (p.specularTexture != HK_NULL);
    outInfo.hasTexcoord0 = p.hasTexcoord0;
    outInfo.generatedUV = p.generatedSkyUV;
    return true;
}

bool Scene3DRenderer::getValidationWarning(char* outText, int maxLen) const
{
    if (!outText || maxLen <= 0) return false;
    outText[0] = '\0';

    if (!m_gameModel)
    {
        sprintf_s(outText, maxLen, "No model loaded");
        return true;
    }

    for (int i = 0; i < m_gameModel->meshPartCount; ++i)
    {
        const GameModel::MeshPart& p = m_gameModel->meshParts[i];
        if (p.vertexCount == 0 || p.indexCount == 0)
        {
            sprintf_s(outText, maxLen, "MeshPart %d is empty", i);
            return true;
        }
        if (!p.diffuseTexture)
        {
            sprintf_s(outText, maxLen, "MeshPart %d missing diffuse texture (%s)", i, p.diffuseName);
            return true;
        }
        if (!p.hasTexcoord0)
        {
            sprintf_s(outText, maxLen, "MeshPart %d has no UV0; using generated mapping", i);
            return true;
        }
    }
    return false;
}


