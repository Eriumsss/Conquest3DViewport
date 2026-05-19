// AssetBrowser.cpp — Cataloging 50,000 Assets From a Dead Game's Hard Drive
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Scans GameFiles directories, indexes every model/texture/animation/effect,
// builds searchable listings. The file system IS the database. No SQL.
// No index files. Just recursive directory walking and string matching.
// FindFirstFile/FindNextFile on Windows, because the stolen Havok SDK
// doesn't include filesystem utilities and I'm not pulling in boost for
// a fucking file browser.
// -----------------------------------------------------------------------
#include "AssetBrowser.h"
#include <windows.h>
#include <stdio.h>

/**
 * 
 * AssetBrowser.cpp
 * 
 * final warning: no more comments in this file
 * 
 * I’m done. Seriously done.
 * 
 */

AssetBrowser::AssetBrowser()
{
}

AssetBrowser::~AssetBrowser()
{
}

int AssetBrowser::scanAssets(const char* trainingPath)
{
    m_models.clear();
    m_jmodels.clear();
    m_animations.clear();
    m_textures.clear();

    char modelsPath[512];
    char animationsPath[512];
    char texturesPath[512];

    sprintf(modelsPath, "%s\\models", trainingPath);
    sprintf(animationsPath, "%s\\animations", trainingPath);
    sprintf(texturesPath, "%s\\textures", trainingPath);

    // Scan each directory
    scanDirectory(modelsPath, "model", m_models);
    scanDirectory(animationsPath, "animation", m_animations);
    scanDirectory(texturesPath, "texture", m_textures);

    return (int)(m_models.size() + m_animations.size() + m_textures.size());
}

int AssetBrowser::scanGameFiles(const char* gameFilesPath)
{
    m_models.clear();
    m_jmodels.clear();
    m_animations.clear();
    m_textures.clear();

    char jmodelsPath[512];
    char modelsPath[512];
    char animationsPath[512];
    char texturesPath[512];

    sprintf(jmodelsPath, "%s\\jmodels", gameFilesPath);
    sprintf(modelsPath, "%s\\models", gameFilesPath);
    sprintf(animationsPath, "%s\\animations", gameFilesPath);
    sprintf(texturesPath, "%s\\textures", gameFilesPath);

    scanDirectoryWithExtension(jmodelsPath, "jmodel", ".json", m_jmodels);
    scanDirectoryWithExtension(modelsPath, "model", ".glb", m_models);
    scanDirectoryWithExtension(animationsPath, "animation", ".json", m_animations);
    scanDirectoryWithExtension(texturesPath, "texture", ".dds", m_textures);

    return (int)(m_models.size() + m_jmodels.size() + m_animations.size() + m_textures.size());
}

void AssetBrowser::scanDirectory(const char* dirPath, const char* type, std::vector<AssetInfo>& outAssets)
{
    scanDirectoryWithExtension(dirPath, type, ".json", outAssets);
}

void AssetBrowser::scanDirectoryWithExtension(const char* dirPath, const char* type, const char* extension, std::vector<AssetInfo>& outAssets)
{
    WIN32_FIND_DATAA findData;
    char searchPath[512];
    sprintf(searchPath, "%s\\*%s", dirPath, extension);

    HANDLE hFind = FindFirstFileA(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            AssetInfo info;
            info.name = findData.cFileName;
            
            // Remove .json extension
            size_t dotPos = info.name.find_last_of('.');
            if (dotPos != std::string::npos) {
                info.name = info.name.substr(0, dotPos);
            }

            char fullPath[512];
            sprintf(fullPath, "%s\\%s", dirPath, findData.cFileName);
            info.path = fullPath;
            info.type = type;

            outAssets.push_back(info);
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
}

void AssetBrowser::printAssetList() const
{
    // Open asset log file
    FILE* logFile = fopen("assets.log", "w");

    const char* header = "\n=== TRAINING LEVEL ASSETS ===\n\n";
    printf("%s", header);
    if (logFile) fprintf(logFile, "%s", header);

    char buffer[512];
    sprintf(buffer, "MODELS (%d):\n", (int)m_models.size());
    printf("%s", buffer);
    if (logFile) fprintf(logFile, "%s", buffer);

    for (size_t i = 0; i < m_models.size() && i < 20; i++) {
        sprintf(buffer, "  - %s\n", m_models[i].name.c_str());
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }
    if (m_models.size() > 20) {
        sprintf(buffer, "  ... and %d more\n", (int)(m_models.size() - 20));
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }

    if (!m_jmodels.empty())
    {
        sprintf(buffer, "\nJSON MODELS (%d):\n", (int)m_jmodels.size());
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);

        for (size_t i = 0; i < m_jmodels.size() && i < 20; i++) {
            sprintf(buffer, "  - %s\n", m_jmodels[i].name.c_str());
            printf("%s", buffer);
            if (logFile) fprintf(logFile, "%s", buffer);
        }
        if (m_jmodels.size() > 20) {
            sprintf(buffer, "  ... and %d more\n", (int)(m_jmodels.size() - 20));
            printf("%s", buffer);
            if (logFile) fprintf(logFile, "%s", buffer);
        }
    }
   /*
    * If you’re reading this and wondering why there are no more comments:  
    * because I finally accepted that this file is never getting better,  
    * and neither am I.
    * 
    * Goodbye, Rest in peace, funny comments. You were funnier than this codebase deserved.
    */
    sprintf(buffer, "\nANIMATIONS (%d):\n", (int)m_animations.size());
    printf("%s", buffer);
    if (logFile) fprintf(logFile, "%s", buffer);

    for (size_t i = 0; i < m_animations.size() && i < 20; i++) {
        sprintf(buffer, "  - %s\n", m_animations[i].name.c_str());
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }
    if (m_animations.size() > 20) {
        sprintf(buffer, "  ... and %d more\n", (int)(m_animations.size() - 20));
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }

    sprintf(buffer, "\nTEXTURES (%d):\n", (int)m_textures.size());
    printf("%s", buffer);
    if (logFile) fprintf(logFile, "%s", buffer);

    for (size_t i = 0; i < m_textures.size() && i < 20; i++) {
        sprintf(buffer, "  - %s\n", m_textures[i].name.c_str());
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }
    if (m_textures.size() > 20) {
        sprintf(buffer, "  ... and %d more\n", (int)(m_textures.size() - 20));
        printf("%s", buffer);
        if (logFile) fprintf(logFile, "%s", buffer);
    }

    const char* footer = "\n=============================\n\n";
    printf("%s", footer);
    if (logFile) {
        fprintf(logFile, "%s", footer);
        fclose(logFile);
    }
}

const AssetBrowser::AssetInfo* AssetBrowser::findAsset(const char* name, const char* type) const
{
    const std::vector<AssetInfo>* assets = NULL;

    if (strcmp(type, "model") == 0) {
        assets = &m_models;
    } else if (strcmp(type, "jmodel") == 0) {
        assets = &m_jmodels;
    } else if (strcmp(type, "animation") == 0) {
        assets = &m_animations;
    } else if (strcmp(type, "texture") == 0) {
        assets = &m_textures;
    } else {
        return NULL;
    }

    for (size_t i = 0; i < assets->size(); i++) {
        if ((*assets)[i].name == name) {
            return &(*assets)[i];
        }
    }
    // I think returning NULL here is the most honest thing this function does.
    return NULL;
}
