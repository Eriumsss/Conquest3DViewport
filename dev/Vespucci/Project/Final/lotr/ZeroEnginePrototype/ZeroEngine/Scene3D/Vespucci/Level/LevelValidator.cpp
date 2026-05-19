// ================================================================
// LevelValidator.cpp - Level Integrity Validation
// See LevelValidator.h for documentation
// ================================================================

#include "LevelValidator.h"
#include "LevelReader.h"
#include "LevelConstants.h"

#include <set>
#include <map>
#include <sstream>

using namespace ZeroEngine;

// Helper: find entity by GUID
static const LevelGameObjEntry* FindByGuid(
    const std::vector<LevelGameObjEntry>& objs, uint32_t guid)
{
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].guid == guid) return &objs[i];
    }
    return NULL;
}

// Helper: count entities of a given type name (case-insensitive prefix)
static int CountType(const std::vector<LevelGameObjEntry>& objs, const char* typeName)
{
    int count = 0;
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].type_name == typeName) ++count;
    }
    return count;
}

static bool HasNonZeroListRef(const LevelGameObjEntry& obj, uint32_t fieldCrc)
{
    std::map<uint32_t, std::vector<uint32_t> >::const_iterator it =
        obj.list_refs.find(fieldCrc);
    if (it == obj.list_refs.end()) return false;
    for (size_t i = 0; i < it->second.size(); ++i) {
        if (it->second[i] != 0) return true;
    }
    return false;
}

static bool IsKnownCharacterClassGuid(uint32_t guid)
{
    for (int i = 0; i < LC_CharClassLabelCount; ++i) {
        if (LC_CharClassLabels[i].guid == guid) return true;
    }
    return false;
}

static bool GameModeMasksDisjoint(int32_t entityMask, int32_t layerMask)
{
    // -1 means all modes. A zero mask appears in some root/script cases, so
    // avoid warning unless both sides are explicit non-overlapping bitmasks.
    if (entityMask == -1 || layerMask == -1) return false;
    if (entityMask == 0 || layerMask == 0) return false;
    return (entityMask & layerMask) == 0;
}

static int32_t EffectiveGameModeMask(const LevelGameObjEntry& obj,
                                     const std::map<uint32_t, int32_t>& layerMasks)
{
    if (obj.gamemodemask != -1 && obj.gamemodemask != 0)
        return obj.gamemodemask;

    std::map<uint32_t, int32_t>::const_iterator lm = layerMasks.find(obj.layer_guid);
    if (lm != layerMasks.end() && lm->second != -1 && lm->second != 0)
        return lm->second;

    return obj.gamemodemask;
}

// Helper: format a message with entity context
static ValidationIssue MakeIssue(
    ValidationSeverity sev, uint32_t guid,
    const std::string& name, const std::string& type,
    const std::string& msg)
{
    ValidationIssue vi;
    vi.severity = sev;
    vi.entityGuid = guid;
    vi.entityName = name;
    vi.entityType = type;
    vi.message = msg;
    return vi;
}

// ================================================================
//  1. Mandatory Entity Checks
// ================================================================
void ValidateMandatoryEntities(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();
    if (objs.empty()) {
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "", "Level has no entities"));
        return;
    }

    // Universal requirements (any mode)
    if (CountType(objs, "templateLevel") == 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "templateLevel",
            "Missing templateLevel entity — level cannot load"));

    if (CountType(objs, "templateLayer") == 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "templateLayer",
            "Missing templateLayer — no layers defined"));

    if (CountType(objs, "gamemode") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "gamemode",
            "No gamemode entity found — mode selection will fail"));

    if (CountType(objs, "AtmosphereSetting") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "AtmosphereSetting",
            "No AtmosphereSetting — lighting will use defaults"));

    if (CountType(objs, "light_sun") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "light_sun",
            "No light_sun — scene will have no directional light"));

    // Spawn system
    int emitters = CountType(objs, "spawn_emitter");
    int points   = CountType(objs, "spawn_point");
    int nodes    = CountType(objs, "spawn_node");
    int classes  = CountType(objs, "spawn_class");

    if (emitters == 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "spawn_emitter",
            "No spawn_emitter — players cannot spawn"));
    if (points == 0 && emitters > 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "spawn_point",
            "No spawn_point — spawn_emitters have nowhere to place players"));
    if (nodes == 0 && points > 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "spawn_node",
            "No spawn_node — spawn_points have no exact spawn positions"));
    if (classes == 0 && emitters > 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "spawn_class",
            "No spawn_class — spawn_emitters have no character classes"));

    if (CountType(objs, "PlayerRespawner") == 0 && emitters > 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "PlayerRespawner",
            "No PlayerRespawner — game cannot manage spawn emitters"));

    // Scoring
    if (CountType(objs, "PointManager") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "PointManager",
            "No PointManager — no score tracking or victory condition"));

    // Victory
    if (CountType(objs, "logic_endgame") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "logic_endgame",
            "No logic_endgame — match cannot end"));

    if (CountType(objs, "logic_gamestart") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "logic_gamestart",
            "No logic_gamestart — match start events will not fire"));

    // Navigation
    if (CountType(objs, "PathNetwork") == 0)
        out.push_back(MakeIssue(VSEV_WARNING, 0, "", "PathNetwork",
            "No PathNetwork — AI cannot navigate"));

    // Mode-specific: check for CapturePoints if Conquest mode exists
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].type_name == "gamemode") {
            std::map<uint32_t, uint32_t>::const_iterator mIt =
                objs[i].int_fields.find(LC_FIELD_MODE);
            // Can't easily check mode string from CRC, but check for CPs anyway
        }
    }

    int cps = CountType(objs, "CapturePoint");
    if (cps > 0 && CountType(objs, "trigger_radius") == 0)
        out.push_back(MakeIssue(VSEV_ERROR, 0, "", "trigger_radius",
            "CapturePoints exist but no trigger_radius — capture zones undefined"));
}

// ================================================================
//  2. GUID Reference Validation
// ================================================================
void ValidateGUIDReferences(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    // Build GUID set for fast lookup
    std::set<uint32_t> guidSet;
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].guid != 0) guidSet.insert(objs[i].guid);
    }

    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];

        // Check ParentGUID
        if (obj.parent_guid != 0 && guidSet.find(obj.parent_guid) == guidSet.end()) {
            std::ostringstream ss;
            ss << "Broken ParentGUID reference: " << obj.parent_guid;
            out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                obj.type_name, ss.str()));
        }

        // Check target GUID (Output entities)
        if (obj.target_guid != 0 && guidSet.find(obj.target_guid) == guidSet.end()) {
            std::ostringstream ss;
            ss << "Broken Output.target reference: " << obj.target_guid;
            out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                obj.type_name, ss.str()));
        }

        // Check node1/node2 (PathLink)
        if (obj.node1_guid != 0 && guidSet.find(obj.node1_guid) == guidSet.end()) {
            std::ostringstream ss;
            ss << "Broken PathLink.node1 reference: " << obj.node1_guid;
            out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                obj.type_name, ss.str()));
        }
        if (obj.node2_guid != 0 && guidSet.find(obj.node2_guid) == guidSet.end()) {
            std::ostringstream ss;
            ss << "Broken PathLink.node2 reference: " << obj.node2_guid;
            out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                obj.type_name, ss.str()));
        }

        // Check Outputs array GUIDs
        for (size_t oi = 0; oi < obj.outputs.size(); ++oi) {
            if (obj.outputs[oi] != 0 && guidSet.find(obj.outputs[oi]) == guidSet.end()) {
                std::ostringstream ss;
                ss << "Broken Outputs[" << oi << "] GUID: " << obj.outputs[oi];
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }

        // Check Nodes array GUIDs
        for (size_t ni = 0; ni < obj.nodes.size(); ++ni) {
            if (obj.nodes[ni] != 0 && guidSet.find(obj.nodes[ni]) == guidSet.end()) {
                std::ostringstream ss;
                ss << "Broken Nodes[" << ni << "] GUID: " << obj.nodes[ni];
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }

        // Check generic GUID refs (single GUID fields like CaptureArea, CharacterClass)
        for (std::map<uint32_t, uint32_t>::const_iterator it = obj.guid_refs.begin();
             it != obj.guid_refs.end(); ++it)
        {
            uint32_t refGuid = it->second;
            if (refGuid != 0 && guidSet.find(refGuid) == guidSet.end()) {
                // Shared template GUIDs (>1000000) are expected to be in the PAK
                // but not in level.json — only warn for local refs
                if (refGuid < 1000000) {
                    std::ostringstream ss;
                    ss << "Broken GUID ref (field 0x" << std::hex << it->first
                       << "): " << std::dec << refGuid;
                    out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                        obj.type_name, ss.str()));
                }
            }
        }

        // Check list_refs (ObjectList GUID arrays like Classes, Points, etc.)
        for (std::map<uint32_t, std::vector<uint32_t> >::const_iterator it = obj.list_refs.begin();
             it != obj.list_refs.end(); ++it)
        {
            const std::vector<uint32_t>& list = it->second;
            for (size_t li = 0; li < list.size(); ++li) {
                if (list[li] != 0 && guidSet.find(list[li]) == guidSet.end()) {
                    if (list[li] < 1000000) {
                        std::ostringstream ss;
                        ss << "Broken list ref (field 0x" << std::hex << it->first
                           << "[" << std::dec << li << "]): " << list[li];
                        out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                            obj.type_name, ss.str()));
                    }
                }
            }
        }
    }
}

// ================================================================
//  3. Wiring Rules
// ================================================================
void ValidateWiringRules(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];

        // spawn_emitter must have Classes[] and Points[]
        if (obj.type_name == "spawn_emitter") {
            bool hasClasses = HasNonZeroListRef(obj, LC_FIELD_SPAWN_CLASSES);
            bool hasPoints = HasNonZeroListRef(obj, LC_FIELD_SPAWN_POINTS) || !obj.nodes.empty();
            if (!hasClasses) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "spawn_emitter.Classes[] is empty — emitter has no spawn classes"));
            }
            if (!hasPoints) {
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name, "spawn_emitter has no Points — players have nowhere to spawn"));
            }
        }

        // CapturePoint must reference a trigger_radius via CaptureArea
        if (obj.type_name == "CapturePoint") {
            std::map<uint32_t, uint32_t>::const_iterator caIt =
                obj.guid_refs.find(LC_FIELD_CAPTUREAREA);
            if (caIt == obj.guid_refs.end() || caIt->second == 0) {
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name, "CapturePoint has no CaptureArea — capture zone undefined"));
            }
        }

        // PathLink must have node1 and node2
        if (obj.type_name == "PathLink") {
            if (obj.node1_guid == 0 || obj.node2_guid == 0) {
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name, "PathLink missing node1 or node2 — navigation broken"));
            }
        }

        // PlayerRespawner must reference at least Team1 and Team2 emitters
        if (obj.type_name == "PlayerRespawner") {
            bool hasT1 = false, hasT2 = false;
            for (std::map<uint32_t, uint32_t>::const_iterator it = obj.guid_refs.begin();
                 it != obj.guid_refs.end(); ++it)
            {
                if (it->second != 0) {
                    hasT1 = true; // Approximate
                    hasT2 = true;
                }
            }
            if (!hasT1 || !hasT2) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "PlayerRespawner may be missing team emitter references"));
            }
        }

        // spawn_class must have CharacterClass GUID
        if (obj.type_name == "spawn_class") {
            std::map<uint32_t, uint32_t>::const_iterator ccIt =
                obj.guid_refs.find(LC_FIELD_CHARACTERCLASS);
            if (ccIt == obj.guid_refs.end() || ccIt->second == 0) {
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name,
                    "spawn_class has no CharacterClass — will crash on spawn"));
            } else if (!IsKnownCharacterClassGuid(ccIt->second)) {
                std::ostringstream ss;
                ss << "spawn_class.CharacterClass GUID " << ccIt->second
                   << " is not in the known shared template character class list";
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }
    }
}

// ================================================================
//  4. Field Requirements
// ================================================================
void ValidateFieldRequirements(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];

        // gamemode must have Mode field set
        if (obj.type_name == "gamemode") {
            std::map<uint32_t, uint32_t>::const_iterator mIt =
                obj.int_fields.find(LC_FIELD_MODE);
            if (mIt == obj.int_fields.end() || mIt->second == 0) {
                out.push_back(MakeIssue(VSEV_ERROR, obj.guid, obj.name,
                    obj.type_name, "gamemode.Mode not set — mode selection will fail"));
            }
        }

        // PointManager must have VictoryPoints > 0
        if (obj.type_name == "PointManager") {
            std::map<uint32_t, uint32_t>::const_iterator vpIt =
                obj.int_fields.find(LC_FIELD_VICTORYPOINTS);
            if (vpIt == obj.int_fields.end() || vpIt->second == 0) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "PointManager.VictoryPoints is 0 — match may never end"));
            }

            std::map<uint32_t, uint32_t>::const_iterator tIt =
                obj.int_fields.find(LC_FIELD_TEAM);
            if (tIt == obj.int_fields.end() || tIt->second == 0) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "PointManager.Team not set — score may not be assigned"));
            }
        }

        // CapturePoint should have Team set
        if (obj.type_name == "CapturePoint") {
            std::map<uint32_t, uint32_t>::const_iterator tIt =
                obj.int_fields.find(LC_FIELD_TEAM);
            if (tIt == obj.int_fields.end()) {
                out.push_back(MakeIssue(VSEV_INFO, obj.guid, obj.name,
                    obj.type_name, "CapturePoint.Team not explicitly set — defaults to neutral"));
            }
        }

        // templateLevel must have DrawDistance > 0
        if (obj.type_name == "templateLevel") {
            std::map<uint32_t, float>::const_iterator ddIt =
                obj.float_fields.find(LC_FIELD_DRAWDISTANCE);
            if (ddIt == obj.float_fields.end() || ddIt->second <= 0.0f) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "templateLevel.DrawDistance not set — nothing will render beyond default distance"));
            }
        }

        // AtmosphereSetting should have key > 0
        if (obj.type_name == "AtmosphereSetting") {
            std::map<uint32_t, float>::const_iterator kIt =
                obj.float_fields.find(LC_FIELD_KEY);
            if (kIt == obj.float_fields.end() || kIt->second <= 0.0f) {
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, "AtmosphereSetting.key not set — tonemapping will produce black screen"));
            }
        }
    }
}

// ================================================================
//  5. Layer Architecture Validation (D1)
// ================================================================
void ValidateLayerArchitecture(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    // Collect layer names/masks from templateLayer entities
    std::set<uint32_t> layerGuids;
    std::map<uint32_t, std::string> layerNames;
    std::map<uint32_t, int32_t> layerMasks;
    std::map<uint32_t, size_t> guidToIndex;
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].guid != 0) guidToIndex[objs[i].guid] = i;
        if (objs[i].type_name == "templateLayer") {
            layerGuids.insert(objs[i].guid);
            layerNames[objs[i].guid] = objs[i].name;
            layerMasks[objs[i].guid] = objs[i].gamemodemask;
        }
    }

    // Check for expected standard layers
    bool hasArt = false, hasLights = false, hasFX = false, hasSound = false, hasPath = false;
    for (std::map<uint32_t, std::string>::const_iterator it = layerNames.begin();
         it != layerNames.end(); ++it)
    {
        const std::string& n = it->second;
        if (n.find("Art") != std::string::npos || n.find("art") != std::string::npos) hasArt = true;
        if (n.find("Light") != std::string::npos || n.find("light") != std::string::npos) hasLights = true;
        if (n.find("FX") != std::string::npos || n.find("fx") != std::string::npos || n.find("Effect") != std::string::npos) hasFX = true;
        if (n.find("Sound") != std::string::npos || n.find("sound") != std::string::npos || n.find("Audio") != std::string::npos) hasSound = true;
        if (n.find("Path") != std::string::npos || n.find("path") != std::string::npos || n.find("Nav") != std::string::npos) hasPath = true;
    }
    if (!hasArt)    out.push_back(MakeIssue(VSEV_INFO, 0, "", "templateLayer", "No 'Art' layer found — consider organizing static geometry"));
    if (!hasLights) out.push_back(MakeIssue(VSEV_INFO, 0, "", "templateLayer", "No 'Lights' layer found — consider separating lighting"));
    if (!hasPath)   out.push_back(MakeIssue(VSEV_INFO, 0, "", "templateLayer", "No 'Path/Nav' layer found — consider separating navigation"));

    // Verify entities reference valid layers
    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];
        if (obj.layer_guid != 0 && layerGuids.find(obj.layer_guid) == layerGuids.end()) {
            // Layer GUID doesn't match any templateLayer — might be a shared template layer
            if (obj.layer_guid < 1000000) {
                std::ostringstream ss;
                ss << "LayerGUID " << obj.layer_guid << " does not match any templateLayer";
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name, obj.type_name, ss.str()));
            }
        } else if (obj.layer_guid != 0) {
            std::map<uint32_t, int32_t>::const_iterator lm = layerMasks.find(obj.layer_guid);
            if (lm != layerMasks.end() && GameModeMasksDisjoint(obj.gamemodemask, lm->second)) {
                std::ostringstream ss;
                ss << "GameModeMask " << obj.gamemodemask
                   << " has no overlap with layer mask " << lm->second;
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name, obj.type_name, ss.str()));
            }
        }
    }

    // Verify gamemode Layers[] references point to valid layers
    for (size_t i = 0; i < objs.size(); ++i) {
        if (objs[i].type_name != "gamemode") continue;
        for (std::map<uint32_t, std::vector<uint32_t> >::const_iterator it = objs[i].list_refs.begin();
             it != objs[i].list_refs.end(); ++it)
        {
            for (size_t li = 0; li < it->second.size(); ++li) {
                uint32_t lg = it->second[li];
                if (lg != 0 && layerGuids.find(lg) == layerGuids.end() && lg < 1000000) {
                    std::ostringstream ss;
                    ss << "gamemode Layers[] reference " << lg << " not found as templateLayer";
                    out.push_back(MakeIssue(VSEV_WARNING, objs[i].guid, objs[i].name,
                        objs[i].type_name, ss.str()));
                } else {
                    std::map<uint32_t, int32_t>::const_iterator lm = layerMasks.find(lg);
                    if (lm != layerMasks.end() && GameModeMasksDisjoint(objs[i].gamemodemask, lm->second)) {
                        std::ostringstream ss;
                        ss << "gamemode Layers[] layer " << lg << " mask " << lm->second
                           << " does not overlap gamemode mask " << objs[i].gamemodemask;
                        out.push_back(MakeIssue(VSEV_WARNING, objs[i].guid, objs[i].name,
                            objs[i].type_name, ss.str()));
                    }
                }
            }
        }
    }

    // Verify cross-entity GUID/list references do not point to objects that are
    // only loaded in a disjoint gamemode/layer mask.
    std::set<std::pair<uint32_t, uint32_t> > warnedRefs;
    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& src = objs[i];
        int32_t srcMask = EffectiveGameModeMask(src, layerMasks);

        for (std::map<uint32_t, uint32_t>::const_iterator gr = src.guid_refs.begin();
             gr != src.guid_refs.end(); ++gr) {
            uint32_t dstGuid = gr->second;
            if (dstGuid == 0 || dstGuid == src.guid) continue;
            std::map<uint32_t, size_t>::const_iterator di = guidToIndex.find(dstGuid);
            if (di == guidToIndex.end()) continue;
            const LevelGameObjEntry& dst = objs[di->second];
            int32_t dstMask = EffectiveGameModeMask(dst, layerMasks);
            if (GameModeMasksDisjoint(srcMask, dstMask) &&
                warnedRefs.insert(std::make_pair(src.guid, dstGuid)).second) {
                std::ostringstream ss;
                ss << "GUID reference to " << dst.name << " (" << dstGuid
                   << ") crosses disjoint GameModeMask/layer masks";
                out.push_back(MakeIssue(VSEV_WARNING, src.guid, src.name, src.type_name, ss.str()));
            }
        }

        for (std::map<uint32_t, std::vector<uint32_t> >::const_iterator lr = src.list_refs.begin();
             lr != src.list_refs.end(); ++lr) {
            for (size_t li = 0; li < lr->second.size(); ++li) {
                uint32_t dstGuid = lr->second[li];
                if (dstGuid == 0 || dstGuid == src.guid) continue;
                std::map<uint32_t, size_t>::const_iterator di = guidToIndex.find(dstGuid);
                if (di == guidToIndex.end()) continue;
                const LevelGameObjEntry& dst = objs[di->second];
                int32_t dstMask = EffectiveGameModeMask(dst, layerMasks);
                if (GameModeMasksDisjoint(srcMask, dstMask) &&
                    warnedRefs.insert(std::make_pair(src.guid, dstGuid)).second) {
                    std::ostringstream ss;
                    ss << "List reference to " << dst.name << " (" << dstGuid
                       << ") crosses disjoint GameModeMask/layer masks";
                    out.push_back(MakeIssue(VSEV_WARNING, src.guid, src.name, src.type_name, ss.str()));
                }
            }
        }
    }
}

// ================================================================
//  6. Naming Convention Warnings (D2)
// ================================================================
void ValidateNamingConventions(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    struct TypePrefix { const char* typeName; const char* prefix; };
    static const TypePrefix kPrefixes[] = {
        { "spawn_emitter", "EMIT_" },
        { "spawn_point",   "SP_" },
        { "trigger_radius","TRIG_" },
        { "trigger_box",   "TRIG_" },
        { "trigger_sphere","TRIG_" },
        { "logic_relay",   "RLY_" },
        { "logic_timer",   "TMR_" },
        { "logic_counter", "CNT_" },
        { "AIGoal",        "AIGL_" },
        { "Objective",     "OBJ_" },
        { NULL, NULL }
    };

    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];
        if (obj.name.empty()) continue;

        for (int p = 0; kPrefixes[p].typeName; ++p) {
            if (obj.type_name != kPrefixes[p].typeName) continue;
            // Check if name starts with expected prefix (case-insensitive)
            const char* expected = kPrefixes[p].prefix;
            size_t plen = strlen(expected);
            bool hasPrefix = false;
            if (obj.name.length() >= plen) {
                hasPrefix = true;
                for (size_t c = 0; c < plen; ++c) {
                    if (toupper((unsigned char)obj.name[c]) != toupper((unsigned char)expected[c])) {
                        hasPrefix = false; break;
                    }
                }
            }
            if (!hasPrefix) {
                std::ostringstream ss;
                ss << "Naming: " << obj.type_name << " \"" << obj.name
                   << "\" should start with " << expected;
                out.push_back(MakeIssue(VSEV_INFO, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
            break;
        }
    }
}

// ================================================================
//  7. Field Value Range Validation (D3)
// ================================================================
void ValidateFieldRanges(
    const LevelReader& reader,
    std::vector<ValidationIssue>& out)
{
    const std::vector<LevelGameObjEntry>& objs = reader.GetGameObjs();

    for (size_t i = 0; i < objs.size(); ++i) {
        const LevelGameObjEntry& obj = objs[i];

        // Team must be 0, 1, 2, or 31
        std::map<uint32_t, uint32_t>::const_iterator tIt = obj.int_fields.find(LC_FIELD_TEAM);
        if (tIt != obj.int_fields.end()) {
            uint32_t t = tIt->second;
            if (t != 0 && t != 1 && t != 2 && t != 31) {
                std::ostringstream ss;
                ss << "Team=" << t << " is invalid (expected 0/1/2/31)";
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }

        // Priority should be 1-100 for AIGoal
        if (obj.type_name == "AIGoal") {
            std::map<uint32_t, uint32_t>::const_iterator pIt = obj.int_fields.find(LC_FIELD_PRIORITY);
            if (pIt != obj.int_fields.end()) {
                int p = (int)pIt->second;
                if (p < 1 || p > 100) {
                    std::ostringstream ss;
                    ss << "AIGoal.Priority=" << p << " out of expected range 1-100";
                    out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                        obj.type_name, ss.str()));
                }
            }
        }

        // Health must be >= 0 for Creature
        if (obj.type_name == "Creature") {
            std::map<uint32_t, uint32_t>::const_iterator hIt = obj.int_fields.find(LC_FIELD_HEALTH);
            if (hIt != obj.int_fields.end() && (int)hIt->second < 0) {
                std::ostringstream ss;
                ss << "Creature.Health=" << (int)hIt->second << " is negative";
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }

        // CaptureTime must be > 0
        if (obj.type_name == "CapturePoint") {
            std::map<uint32_t, float>::const_iterator ctIt = obj.float_fields.find(LC_FIELD_CAPTURETIME);
            if (ctIt != obj.float_fields.end() && ctIt->second <= 0.0f) {
                std::ostringstream ss;
                ss << "CapturePoint.CaptureTime=" << ctIt->second << " must be > 0";
                out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                    obj.type_name, ss.str()));
            }
        }

        // DrawDistance must be > 0
        std::map<uint32_t, float>::const_iterator ddIt = obj.float_fields.find(LC_FIELD_DRAWDISTANCE);
        if (ddIt != obj.float_fields.end() && ddIt->second <= 0.0f) {
            std::ostringstream ss;
            ss << "DrawDistance=" << ddIt->second << " must be > 0";
            out.push_back(MakeIssue(VSEV_WARNING, obj.guid, obj.name,
                obj.type_name, ss.str()));
        }

        // VictoryPoints must be > 0
        if (obj.type_name == "PointManager") {
            std::map<uint32_t, uint32_t>::const_iterator vpIt = obj.int_fields.find(LC_FIELD_VICTORYPOINTS);
            if (vpIt != obj.int_fields.end() && vpIt->second == 0) {
                // Already covered in ValidateFieldRequirements, skip duplicate
            }
        }
    }
}

// ================================================================
//  Main entry point
// ================================================================
int ValidateLevel(
    const LevelReader& reader,
    std::vector<ValidationIssue>& outIssues)
{
    outIssues.clear();

    ValidateMandatoryEntities(reader, outIssues);
    ValidateGUIDReferences(reader, outIssues);
    ValidateWiringRules(reader, outIssues);
    ValidateFieldRequirements(reader, outIssues);
    ValidateLayerArchitecture(reader, outIssues);
    ValidateNamingConventions(reader, outIssues);
    ValidateFieldRanges(reader, outIssues);

    int errorCount = 0;
    for (size_t i = 0; i < outIssues.size(); ++i) {
        if (outIssues[i].severity == VSEV_ERROR) ++errorCount;
    }
    return errorCount;
}
