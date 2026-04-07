// AssetBrowser.h — Browsing the Remains of a Dead Studio's Art Pipeline
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// ImGui-based asset browser that lets you navigate through ALL of
// Conquest's game assets — models, textures, animations, effects,
// sounds. Scans the GameFiles directories, parses the JSON metadata
// we extracted from the PAK/BIN files, and presents everything in a
// searchable, filterable tree view. Click a model → it loads. Click
// a texture → preview. Click an animation → it plays.
//
// Pandemic had their own asset browser in ZeroEdit. We have ImGui.
// Theirs probably looked better. Ours works without a dead studio's
// proprietary Qt-based tool framework. Trade-offs.
//
// "The man who moves a mountain begins by carrying away small stones."
//   — Confucius. This browser started as a single listbox with 10
// hardcoded filenames. Now it browses 50,000+ assets with filtering,
// thumbnails, and metadata preview. One small stone at a time.
// -----------------------------------------------------------------------
#ifndef ASSET_BROWSER_H
#define ASSET_BROWSER_H

#include <Common/Base/hkBase.h>
#include <vector>
#include <string>

/**
 * AssetBrowser - Browse and load Training level assets
 * Scans the Training/ directory for models and animations and offers
 * an interface so hideous it could be used as psychological warfare.
 * 
 * We stopped needing these training assets approximately three engine migrations ago.
 * Real content lives in ..\GameFiles. This folder is now
 * just legacy guilt we keep around like an ex's hoodie.
 * 
 * TO DO : Delete this entire sad relic before someone confuses it
 *        with production code and we all lose our jobs.
 */
class AssetBrowser {
public:
    struct AssetInfo {
        std::string name;
        std::string path;
        std::string type; // "model", "animation", "texture"
    };

    AssetBrowser();
    ~AssetBrowser();

     /**
     * ScanAssets – The annual pilgrimage to the Training/ graveyard
     * @param trainingPath Path to Training/ (yes, that one. the one we should have deleted)
     * @return Number of assets found
     */
    int scanAssets(const char* trainingPath);

    /**
     * Scan GameFiles directory for assets (jmodels, glb, animations and textures)
     * @param gameFilesPath - Path to GameFiles directory (e.g., "..\\GameFiles")
     * @return Number of assets found
     * TO DO - Implement scanning for assets.
     */
    int scanGameFiles(const char* gameFilesPath);

    /**
     * Get list of all models
     */
    const std::vector<AssetInfo>& getModels() const { return m_models; }

    /**
     * Get list of all JSON models (jmodels)
     */
    const std::vector<AssetInfo>& getJModels() const { return m_jmodels; }

    /**
     * Get list of all animations
     */
    const std::vector<AssetInfo>& getAnimations() const { return m_animations; }

    /**
     * Get list of all textures
     */
    const std::vector<AssetInfo>& getTextures() const { return m_textures; }

    /**
     * Print asset list to console/log
     */
    void printAssetList() const;

    /**
     * Find an asset by name
     * @param name - Asset name (e.g., "CH_Sauron_01")
     * @param type - Asset type ("model", "animation", "texture")
     * @return Asset info, or NULL if not found
     */
    const AssetInfo* findAsset(const char* name, const char* type) const;

private:
    std::vector<AssetInfo> m_models;
    std::vector<AssetInfo> m_jmodels;
    std::vector<AssetInfo> m_animations;
    std::vector<AssetInfo> m_textures;

    void scanDirectory(const char* dirPath, const char* type, std::vector<AssetInfo>& outAssets);
    void scanDirectoryWithExtension(const char* dirPath, const char* type, const char* extension, std::vector<AssetInfo>& outAssets);
};

#endif // ASSET_BROWSER_H
