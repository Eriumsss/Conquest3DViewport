#pragma once
// ============================================================================
// AudioManager - Complete Wwise SDK v2.1.2821 Integration
// Full audio system: bank management, 3D positioning, dynamic loading,
// music/ambient, volume control, VO queue, switch/state automation.
// Bank files use numeric FNV-1 hash filenames (e.g. 3503657562.bnk).
// ============================================================================

#define AKSOUNDENGINE_STATIC
// AK_OPTIMIZED removed — enables Wwise Monitor error callback for diagnostics

#include <Windows.h>
#include <AK/SoundEngine/Common/AkTypes.h>
#include <AK/SoundEngine/Common/IAkStreamMgr.h>
#include <AK/SoundEngine/Platforms/Windows/AkStreamMgrModule.h>

#include <string>
#include <vector>
#include <deque>
#include <map>

#include "EventMapping.h"

// ============================================================================
// FNV-1 Hash (Wwise style: lowercase, multiply-then-XOR, 32-bit)
// ============================================================================
inline AkUInt32 FNV1Hash(const char* str)
{
    AkUInt32 hash = 0x811c9dc5u;
    for (; *str; ++str)
    {
        char c = *str;
        if (c >= 'A' && c <= 'Z') c += 32;
        hash *= 0x01000193u;
        hash ^= static_cast<AkUInt32>(static_cast<unsigned char>(c));
    }
    return hash;
}

inline AkUInt32 FNV1HashW(const wchar_t* str)
{
    AkUInt32 hash = 0x811c9dc5u;
    for (; *str; ++str)
    {
        wchar_t c = *str;
        if (c >= L'A' && c <= L'Z') c += 32;
        hash *= 0x01000193u;
        hash ^= static_cast<AkUInt32>(c & 0xFF);
    }
    return hash;
}

// ============================================================================
// Low-Level I/O: dual-path (root/ + Languages/english_us_/) file resolution
// ============================================================================
class ConquestLowLevelIO : public AK::IAkLowLevelIO
{
public:
    ConquestLowLevelIO() : m_deviceID(AK_INVALID_DEVICE_ID)
    { memset(m_basePath, 0, sizeof(m_basePath)); memset(m_langPath, 0, sizeof(m_langPath)); }

    void SetDeviceID(AkDeviceID id)            { m_deviceID = id; }
    AkDeviceID GetDeviceID() const             { return m_deviceID; }
    void SetBasePath(const wchar_t* path)      { wcscpy_s(m_basePath, path); }
    void SetLanguagePath(const wchar_t* path)  { wcscpy_s(m_langPath, path); }
    const wchar_t* GetBasePath() const         { return m_basePath; }
    const wchar_t* GetLangPath() const         { return m_langPath; }

    AKRESULT Open(AkLpCtstr in_pszFileName, AkOpenMode in_eOpenMode,
                  AkFileSystemFlags* in_pFlags, AkFileDesc& out_fileDesc);
    AKRESULT Open(AkFileID in_fileID, AkOpenMode in_eOpenMode,
                  AkFileSystemFlags* in_pFlags, AkFileDesc& out_fileDesc);
    AKRESULT Close(const AkFileDesc& in_fileDesc);
    AKRESULT Read(AkFileDesc& io_fileDesc, void* out_pBuffer,
                  AkIOTransferInfo& io_transferInfo);
    AKRESULT Write(AkFileDesc& io_fileDesc, void* in_pData,
                   AkIOTransferInfo& io_transferInfo);
    AKRESULT GetAsyncResult(AkFileDesc& io_fileDesc,
                            AkIOTransferInfo& io_transferInfo);
    AkUInt32 GetBlockSize(const AkFileDesc& in_fileDesc);

#ifndef AK_OPTIMIZED
    AKRESULT GetDeviceDesc(AkDeviceID in_deviceID, AkDeviceDesc& out_deviceDesc);
#endif

private:
    AKRESULT OpenByPath(const wchar_t* fullPath, AkFileDesc& out_fileDesc);
    AkDeviceID m_deviceID;
    wchar_t    m_basePath[MAX_PATH];   // root/ banks & SFX WEMs
    wchar_t    m_langPath[MAX_PATH];   // Languages/english_us_/ VO WEMs
};

// ============================================================================
// Bank categories (maps to Organized_Final/ directory structure)
// ============================================================================
enum BankCategory {
    BankCategory_Core,       // Init, UI, Effects, Music
    BankCategory_Combat,     // BaseCombat
    BankCategory_Creature,   // SFXWarg, SFXHorse, SFXTroll, etc.
    BankCategory_Hero,       // HeroAragorn, HeroGandalf, etc.
    BankCategory_Level,      // Level_Shire, Level_Moria, etc.
    BankCategory_VO,         // VO_Shire, VO_Moria, VoiceOver, etc.
    BankCategory_Chatter,    // ChatterElf, ChatterOrc, ChatterHeroAragorn, etc.
    BankCategory_Ambience    // Ambience
};

// ============================================================================
// AudioManager public interface — full audio system
// ============================================================================
struct LoadedBankInfo {
    std::string   name;
    AkBankID      bankID;
    BankCategory  category;
    int           refCount;
};

struct VOQueueEntry {
    AkUniqueID      eventID;
    AkGameObjectID  gameObjID;
    float           priority;   // higher = more important
};

class AudioManager
{
public:
    static AudioManager& Get();

    // --- Lifecycle ---
    bool Initialize(HWND hWnd, const wchar_t* bankRootPath,
                    const wchar_t* bankLangPath);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    void RenderAudio();

    // --- Bank Management ---
    bool LoadBank(const char* bankName, BankCategory cat = BankCategory_Core);
    bool LoadBankByID(AkUInt32 fileID, BankCategory cat = BankCategory_Core);
    void UnloadBank(const char* bankName);
    void UnloadAllBanks();
    bool IsBankLoaded(const char* bankName) const;

    // Dynamic bank loading by context
    void LoadBanksForCharacter(const char* heroName);
    void UnloadBanksForCharacter(const char* heroName);
    void LoadBanksForLevel(const char* levelName);
    void UnloadBanksForLevel(const char* levelName);
    void LoadCoreBanks();   // Init + BaseCombat + Effects + UI + Ambience
    void LoadAllBanks();    // Load every known bank (for viewer/debug mode)
    bool LoadInitBankPatched(); // Load Init.bnk with STMG MaxVoices patch (1->255)

    // --- Event Playback ---
    AkPlayingID PlayCue(const char* cueString,
                        AkGameObjectID gameObjID = DEFAULT_GAME_OBJECT);
    AkPlayingID PostEventByID(AkUniqueID eventID,
                              AkGameObjectID gameObjID = DEFAULT_GAME_OBJECT);
    void StopAll(AkGameObjectID gameObjID = AK_INVALID_GAME_OBJECT);
    void StopEvent(AkPlayingID playingID, int fadeDurationMs = 0);

    // --- Game Objects ---
    bool RegisterGameObject(AkGameObjectID id, const char* name = NULL);
    void UnregisterGameObject(AkGameObjectID id);

    // --- 3D Positioning ---
    void SetPosition3D(AkGameObjectID id, float x, float y, float z,
                       float frontX = 0.f, float frontY = 0.f, float frontZ = 1.f);
    void UpdateListenerPosition(float x, float y, float z,
                                float frontX, float frontY, float frontZ,
                                float topX, float topY, float topZ);

    // --- Switches / States / RTPC ---
    void SetSwitch(const char* group, const char* state,
                   AkGameObjectID gameObjID = DEFAULT_GAME_OBJECT);
    void SetSwitchByID(AkSwitchGroupID gid, AkSwitchStateID sid,
                       AkGameObjectID gameObjID = DEFAULT_GAME_OBJECT);
    void SetState(const char* group, const char* state);
    void SetStateByID(AkStateGroupID gid, AkStateID sid);
    void SetRTPCValue(const char* rtpcName, float value,
                      AkGameObjectID gameObjID = AK_INVALID_GAME_OBJECT);

    // Switch automation (set contextual switches for current character)
    void SetCreatureSwitch(const char* creature, AkGameObjectID gameObjID);
    void SetWeaponSwitch(const char* weapon, AkGameObjectID gameObjID);
    void SetMaterialSwitch(const char* material, AkGameObjectID gameObjID);
    void SetAbilitySwitch(const char* ability, AkGameObjectID gameObjID);
    void SetPlayerControlled(bool isPlayer, AkGameObjectID gameObjID);

    // --- Music System ---
    void PlayMusic(const char* musicEvent);
    void StopMusic(int fadeDurationMs = 1000);
    void SetMusicState(const char* stateGroup, const char* state);

    // --- Volume Control ---
    void SetMasterVolume(float vol01);  // 0.0 = silent, 1.0 = full
    void SetSFXVolume(float vol01);
    void SetMusicVolume(float vol01);
    void SetVoiceVolume(float vol01);

    // --- VO / Dialogue Queue ---
    void QueueVO(const char* cueString, AkGameObjectID gameObjID,
                 float priority = 1.0f);
    void QueueVOByID(AkUniqueID eventID, AkGameObjectID gameObjID,
                     float priority = 1.0f);
    void FlushVOQueue();
    void UpdateVOQueue();  // called internally from RenderAudio

    // --- Event Mapping (2,817 event->bank entries from AudioHook DLL) ---
    void BuildEventLookup();   // Build fast lookup map from static array (call after init)
    const EventMappingEntry* LookupEvent(AkUInt32 eventID) const;
    int  GetMappedEventCount() const { return (int)m_eventLookup.size(); }

    // --- Diagnostics ---
    int  GetLoadedBankCount() const { return (int)m_loadedBanks.size(); }
    const std::vector<LoadedBankInfo>& GetLoadedBanks() const { return m_loadedBanks; }

    enum { DEFAULT_GAME_OBJECT = 100 };
    enum { LISTENER_OBJECT     = 0 };

private:
    AudioManager();
    ~AudioManager();
    AudioManager(const AudioManager&);             // no copy
    AudioManager& operator=(const AudioManager&);  // no assign

    bool                        m_initialized;
    ConquestLowLevelIO          m_lowLevelIO;
    AkDeviceID                  m_deviceID;

    // Bank tracking
    std::vector<LoadedBankInfo>  m_loadedBanks;
    std::string                  m_currentHero;
    std::string                  m_currentLevel;

    // VO queue
    std::deque<VOQueueEntry>     m_voQueue;
    AkPlayingID                  m_currentVOPlayingID;
    bool                         m_voPlaying;

    // Music
    AkPlayingID                  m_currentMusicPlayingID;

    // Volume cache
    float m_masterVol;
    float m_sfxVol;
    float m_musicVol;
    float m_voiceVol;

    // Event mapping: event ID -> index into g_EventMappingData[]
    std::map<AkUInt32, int> m_eventLookup;

    // Helpers
    void LogAudio(const char* fmt, ...);
    void LogAudioError(const char* fmt, ...);
};

