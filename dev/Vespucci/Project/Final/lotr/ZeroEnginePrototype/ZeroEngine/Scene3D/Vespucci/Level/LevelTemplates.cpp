// ================================================================
// LevelTemplates.cpp - Pre-built Level Entity Templates
// See LevelTemplates.h for documentation
// ================================================================

#include "LevelTemplates.h"
#include "LevelReader.h"
#include "LevelConstants.h"
#include <string.h>
#include <math.h>

using namespace ZeroEngine;

// ------------------------------------------------------------------
// Helper: find type_def_index by type CRC
// ------------------------------------------------------------------
static int FindTypeIndex(const LevelReader& reader, uint32_t typeCRC) {
    const std::vector<LevelGameObjTypeDef>& types = reader.GetGameObjTypes();
    for (int i = 0; i < (int)types.size(); ++i) {
        if (types[i].crc == typeCRC) return i;
    }
    return -1;
}

// ------------------------------------------------------------------
// Helper: create a basic PendingGameObj with identity transform
// ------------------------------------------------------------------
static PendingGameObj MakeEntity(
    const LevelReader& reader,
    uint32_t typeCRC, uint32_t layerGuid,
    int32_t gameModeMask, const char* name,
    float x, float y, float z)
{
    PendingGameObj obj;
    memset(&obj, 0, sizeof(obj));
    obj.guid = 0;  // will be assigned by AddPendingEntity
    obj.parent_guid = 0;
    obj.layer_guid = layerGuid;
    obj.type_crc = typeCRC;
    obj.gamemodemask = gameModeMask;
    obj.type_def_index = FindTypeIndex(reader, typeCRC);
    obj.mesh_crc = 0;
    obj.name_str = name;
    obj.name_crc = 0;  // will be computed from name_str

    // Identity matrix with translation
    float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    id[12] = x; id[13] = y; id[14] = z;
    memcpy(obj.world_transform, id, sizeof(id));

    return obj;
}

// ================================================================
//  Conquest Template (~30 entities)
//
//  Layout (top-down view):
//    CP_NW [-50,0,-50]    CP_NE [50,0,-50]
//           T1_Spawn [0,0,-80]
//
//           T2_Spawn [0,0,80]
//    CP_SW [-50,0,50]     CP_SE [50,0,50]
// ================================================================
int CreateConquestTemplate(LevelReader& reader, float cx, float cz) {
    int created = 0;

    // Layer GUIDs (local — will be auto-assigned)
    uint32_t layerRoot = 100;
    uint32_t layerAlways = 101;
    uint32_t layerConquest = 102;

    // Tier 1: Structural
    PendingGameObj tmplLevel = MakeEntity(reader, LC_TYPE_TEMPLATELEVEL, layerRoot, -1, "templateLevel", cx, 0, cz);
    reader.AddPendingEntity(tmplLevel); created++;

    PendingGameObj layAlways = MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, -1, "Art", cx, 0, cz);
    reader.AddPendingEntity(layAlways); created++;

    PendingGameObj layCQ = MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, LC_MASK_CONQUEST, "MP_Conquest", cx, 0, cz);
    reader.AddPendingEntity(layCQ); created++;

    // Tier 1: Atmosphere
    PendingGameObj atm = MakeEntity(reader, LC_TYPE_ATMOSPHERESETTING, layerAlways, -1, "AtmosphereSetting", cx, 0, cz);
    reader.AddPendingEntity(atm); created++;

    PendingGameObj sun = MakeEntity(reader, LC_TYPE_LIGHT_SUN, layerAlways, -1, "Sun", cx, 100, cz);
    reader.AddPendingEntity(sun); created++;

    // Tier 1: Gamemode
    PendingGameObj gm = MakeEntity(reader, LC_TYPE_GAMEMODE, layerRoot, LC_MASK_CONQUEST, "Conquest", cx, 0, cz);
    reader.AddPendingEntity(gm); created++;

    // Tier 2: Team 1 spawn chain
    PendingGameObj t1emit = MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerConquest, LC_MASK_CONQUEST, "T1_Emitter", cx, 0, cz - 80);
    reader.AddPendingEntity(t1emit); created++;

    PendingGameObj t1cls1 = MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerConquest, LC_MASK_CONQUEST, "T1_Warrior", cx, 0, cz - 80);
    reader.AddPendingEntity(t1cls1); created++;

    PendingGameObj t1cls2 = MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerConquest, LC_MASK_CONQUEST, "T1_Archer", cx, 0, cz - 80);
    reader.AddPendingEntity(t1cls2); created++;

    PendingGameObj t1pt = MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerConquest, LC_MASK_CONQUEST, "T1_SpawnPoint", cx, 0, cz - 80);
    reader.AddPendingEntity(t1pt); created++;

    // Spawn nodes (4 positions around spawn point)
    float sOff = 3.0f;
    PendingGameObj t1n1 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T1_Node1", cx-sOff, 0, cz-80-sOff);
    reader.AddPendingEntity(t1n1); created++;
    PendingGameObj t1n2 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T1_Node2", cx+sOff, 0, cz-80-sOff);
    reader.AddPendingEntity(t1n2); created++;
    PendingGameObj t1n3 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T1_Node3", cx-sOff, 0, cz-80+sOff);
    reader.AddPendingEntity(t1n3); created++;
    PendingGameObj t1n4 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T1_Node4", cx+sOff, 0, cz-80+sOff);
    reader.AddPendingEntity(t1n4); created++;

    // Tier 2: Team 2 spawn chain (mirror)
    PendingGameObj t2emit = MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerConquest, LC_MASK_CONQUEST, "T2_Emitter", cx, 0, cz + 80);
    reader.AddPendingEntity(t2emit); created++;

    PendingGameObj t2cls1 = MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerConquest, LC_MASK_CONQUEST, "T2_Warrior", cx, 0, cz + 80);
    reader.AddPendingEntity(t2cls1); created++;

    PendingGameObj t2cls2 = MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerConquest, LC_MASK_CONQUEST, "T2_Archer", cx, 0, cz + 80);
    reader.AddPendingEntity(t2cls2); created++;

    PendingGameObj t2pt = MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerConquest, LC_MASK_CONQUEST, "T2_SpawnPoint", cx, 0, cz + 80);
    reader.AddPendingEntity(t2pt); created++;

    PendingGameObj t2n1 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T2_Node1", cx-sOff, 0, cz+80-sOff);
    reader.AddPendingEntity(t2n1); created++;
    PendingGameObj t2n2 = MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerConquest, LC_MASK_CONQUEST, "T2_Node2", cx+sOff, 0, cz+80+sOff);
    reader.AddPendingEntity(t2n2); created++;

    // Tier 2: PlayerRespawner
    PendingGameObj respawner = MakeEntity(reader, LC_TYPE_PLAYERRESPAWNER, layerConquest, LC_MASK_CONQUEST, "PlayerRespawner", cx, 0, cz);
    reader.AddPendingEntity(respawner); created++;

    // Tier 3: 4 CapturePoints with trigger_radius zones
    float cpDist = 50.0f;
    float cpPos[4][2] = { {cx-cpDist, cz-cpDist}, {cx+cpDist, cz-cpDist},
                          {cx-cpDist, cz+cpDist}, {cx+cpDist, cz+cpDist} };
    const char* cpNames[4] = {"CP_NW", "CP_NE", "CP_SW", "CP_SE"};
    for (int c = 0; c < 4; ++c) {
        PendingGameObj trig = MakeEntity(reader, LC_TYPE_TRIGGER_RADIUS, layerConquest, LC_MASK_CONQUEST,
            (std::string(cpNames[c]) + "_Area").c_str(), cpPos[c][0], 0, cpPos[c][1]);
        reader.AddPendingEntity(trig); created++;

        PendingGameObj cp = MakeEntity(reader, LC_TYPE_CAPTUREPOINT, layerConquest, LC_MASK_CONQUEST,
            cpNames[c], cpPos[c][0], 0, cpPos[c][1]);
        reader.AddPendingEntity(cp); created++;
    }

    // Tier 3: Scoring
    PendingGameObj pm1 = MakeEntity(reader, LC_TYPE_POINTMANAGER, layerConquest, LC_MASK_CONQUEST, "Team1_Score", cx, 0, cz);
    reader.AddPendingEntity(pm1); created++;

    PendingGameObj pm2 = MakeEntity(reader, LC_TYPE_POINTMANAGER, layerConquest, LC_MASK_CONQUEST, "Team2_Score", cx, 0, cz);
    reader.AddPendingEntity(pm2); created++;

    // Tier 3: Logic
    PendingGameObj gamestart = MakeEntity(reader, LC_TYPE_LOGIC_GAMESTART, layerConquest, LC_MASK_CONQUEST, "logic_gamestart", cx, 0, cz);
    reader.AddPendingEntity(gamestart); created++;

    PendingGameObj endgame = MakeEntity(reader, LC_TYPE_LOGIC_ENDGAME, layerConquest, LC_MASK_CONQUEST, "logic_endgame", cx, 0, cz);
    reader.AddPendingEntity(endgame); created++;

    // Tier 4: Navigation (2 nodes, 1 link — absolute minimum)
    PendingGameObj pathNet = MakeEntity(reader, LC_TYPE_PATHNETWORK, layerAlways, -1, "PathNetwork", cx, 0, cz);
    reader.AddPendingEntity(pathNet); created++;

    PendingGameObj pn1 = MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_T1", cx, 0, cz - 40);
    reader.AddPendingEntity(pn1); created++;

    PendingGameObj pn2 = MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_T2", cx, 0, cz + 40);
    reader.AddPendingEntity(pn2); created++;

    PendingGameObj pl = MakeEntity(reader, LC_TYPE_PATHLINK, layerAlways, -1, "PathLink_Center", cx, 0, cz);
    reader.AddPendingEntity(pl); created++;

    return created;
}

// ================================================================
//  TDM Template (~20 entities)
//  Simpler: no capture points, just spawn + scoring + nav
// ================================================================
int CreateTDMTemplate(LevelReader& reader, float cx, float cz) {
    int created = 0;

    uint32_t layerRoot = 200;
    uint32_t layerAlways = 201;
    uint32_t layerTDM = 202;

    // Structural
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELEVEL, layerRoot, -1, "templateLevel", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, -1, "Art", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, LC_MASK_TDM, "MP_TDM", cx, 0, cz)); created++;

    // Atmosphere
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_ATMOSPHERESETTING, layerAlways, -1, "AtmosphereSetting", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LIGHT_SUN, layerAlways, -1, "Sun", cx, 100, cz)); created++;

    // Gamemode (TDM)
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_GAMEMODE, layerRoot, LC_MASK_TDM, "TeamDeathmatch", cx, 0, cz)); created++;

    // Team 1 spawn
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerTDM, LC_MASK_TDM, "T1_Emitter", cx-30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerTDM, LC_MASK_TDM, "T1_Class", cx-30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerTDM, LC_MASK_TDM, "T1_Point", cx-30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerTDM, LC_MASK_TDM, "T1_Node", cx-33, 0, cz)); created++;

    // Team 2 spawn
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerTDM, LC_MASK_TDM, "T2_Emitter", cx+30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerTDM, LC_MASK_TDM, "T2_Class", cx+30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerTDM, LC_MASK_TDM, "T2_Point", cx+30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerTDM, LC_MASK_TDM, "T2_Node", cx+33, 0, cz)); created++;

    // Respawner + scoring
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PLAYERRESPAWNER, layerTDM, LC_MASK_TDM, "PlayerRespawner", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_POINTMANAGER, layerTDM, LC_MASK_TDM, "T1_Score", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_POINTMANAGER, layerTDM, LC_MASK_TDM, "T2_Score", cx, 0, cz)); created++;

    // Logic
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LOGIC_GAMESTART, layerTDM, LC_MASK_TDM, "logic_gamestart", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LOGIC_ENDGAME, layerTDM, LC_MASK_TDM, "logic_endgame", cx, 0, cz)); created++;

    // Navigation
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNETWORK, layerAlways, -1, "PathNetwork", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_1", cx-20, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_2", cx+20, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHLINK, layerAlways, -1, "PathLink_1", cx, 0, cz)); created++;

    return created;
}

// ================================================================
//  CTR Template (~25 entities)
//  Like TDM but with AcquireObject (ring) and CaptureRegions
// ================================================================
int CreateCTRTemplate(LevelReader& reader, float cx, float cz) {
    int created = 0;

    uint32_t layerRoot = 300;
    uint32_t layerAlways = 301;
    uint32_t layerCTR = 302;

    // Structural
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELEVEL, layerRoot, -1, "templateLevel", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, -1, "Art", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TEMPLATELAYER, layerRoot, LC_MASK_CTF, "MP_CTR", cx, 0, cz)); created++;

    // Atmosphere
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_ATMOSPHERESETTING, layerAlways, -1, "AtmosphereSetting", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LIGHT_SUN, layerAlways, -1, "Sun", cx, 100, cz)); created++;

    // Gamemode (CTR)
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_GAMEMODE, layerRoot, LC_MASK_CTF, "CTR", cx, 0, cz)); created++;

    // Ring (AcquireObject at center)
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_ACQUIREOBJECT, layerCTR, LC_MASK_CTF, "TheRing", cx, 1, cz)); created++;

    // Team 1 spawn + base
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerCTR, LC_MASK_CTF, "T1_Emitter", cx-50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerCTR, LC_MASK_CTF, "T1_Class", cx-50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerCTR, LC_MASK_CTF, "T1_Point", cx-50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerCTR, LC_MASK_CTF, "T1_Node", cx-53, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TRIGGER_RADIUS, layerCTR, LC_MASK_CTF, "T1_CaptureZone", cx-50, 0, cz)); created++;

    // Team 2 spawn + base
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_EMITTER, layerCTR, LC_MASK_CTF, "T2_Emitter", cx+50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_CLASS, layerCTR, LC_MASK_CTF, "T2_Class", cx+50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_POINT, layerCTR, LC_MASK_CTF, "T2_Point", cx+50, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_SPAWN_NODE, layerCTR, LC_MASK_CTF, "T2_Node", cx+53, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_TRIGGER_RADIUS, layerCTR, LC_MASK_CTF, "T2_CaptureZone", cx+50, 0, cz)); created++;

    // Respawner + scoring
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PLAYERRESPAWNER, layerCTR, LC_MASK_CTF, "PlayerRespawner", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_POINTMANAGER, layerCTR, LC_MASK_CTF, "T1_Score", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_POINTMANAGER, layerCTR, LC_MASK_CTF, "T2_Score", cx, 0, cz)); created++;

    // Logic
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LOGIC_GAMESTART, layerCTR, LC_MASK_CTF, "logic_gamestart", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_LOGIC_ENDGAME, layerCTR, LC_MASK_CTF, "logic_endgame", cx, 0, cz)); created++;

    // Navigation
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNETWORK, layerAlways, -1, "PathNetwork", cx, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_1", cx-30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHNODE, layerAlways, -1, "PathNode_2", cx+30, 0, cz)); created++;
    reader.AddPendingEntity(MakeEntity(reader, LC_TYPE_PATHLINK, layerAlways, -1, "PathLink_1", cx, 0, cz)); created++;

    return created;
}
