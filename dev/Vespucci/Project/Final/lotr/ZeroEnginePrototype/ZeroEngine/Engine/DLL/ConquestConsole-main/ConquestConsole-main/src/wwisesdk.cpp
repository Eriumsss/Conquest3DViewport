// ============================================================================
// Wwise SDK Integration - Separate Engine Instance via Static Libraries
// Links against Wwise SDK v2.1.2821 (2008) static libs to create a fully
// independent sound engine instance, isolated from the game's Wwise instance.
// ============================================================================
// Eriumsss

#define AKSOUNDENGINE_STATIC
#define AK_OPTIMIZED

#include "wwisesdk.h"

#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkMemoryMgr.h>
#include <AK/SoundEngine/Common/IAkStreamMgr.h>
#include <AK/SoundEngine/Platforms/Windows/AkModule.h>
#include <AK/SoundEngine/Platforms/Windows/AkStreamMgrModule.h>
#include <AK/SoundEngine/Platforms/Windows/AkWinSoundEngine.h>
#include <AK/MusicEngine/Common/AkMusicEngine.h>
#include <AK/Plugin/AkVorbisFactory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// ============================================================================
// Required Memory Hooks - Wwise calls these for all allocations
// ============================================================================
namespace AK
{
    void* AllocHook(size_t in_size)
    {
        return malloc(in_size);
    }

    void FreeHook(void* in_pMemAddress)
    {
        free(in_pMemAddress);
    }

    void* VirtualAllocHook(void* in_pMemAddress, size_t in_size,
                           DWORD in_dwAllocationType, DWORD in_dwProtect)
    {
        return VirtualAlloc(in_pMemAddress, in_size, in_dwAllocationType, in_dwProtect);
    }

    void VirtualFreeHook(void* in_pMemAddress, size_t in_size, DWORD in_dwFreeType)
    {
        VirtualFree(in_pMemAddress, in_size, in_dwFreeType);
    }
}

// ============================================================================
// Low-Level I/O Implementation (Blocking, Synchronous)
// Reads .bnk files from ConquestMods/audio/banks/
// ============================================================================
static const wchar_t* BANK_BASE_PATH = L"ConquestMods\\audio\\banks\\";

class ConquestLowLevelIO : public AK::IAkLowLevelIO
{
public:
    ConquestLowLevelIO() : m_deviceID(AK_INVALID_DEVICE_ID) {}

    void SetDeviceID(AkDeviceID id) { m_deviceID = id; }
    AkDeviceID GetDeviceID() const { return m_deviceID; }

    // Open file by name (wide string)
    AKRESULT Open(AkLpCtstr in_pszFileName, AkOpenMode in_eOpenMode,
                  AkFileSystemFlags* in_pFlags, AkFileDesc& out_fileDesc) override
    {
        if (!in_pszFileName)
            return AK_FileNotFound;

        // Build full path: base + filename
        wchar_t fullPath[MAX_PATH];
        _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s%s", BANK_BASE_PATH, in_pszFileName);

        // Open with Win32
        DWORD access = (in_eOpenMode == AK_OpenModeRead) ? GENERIC_READ : GENERIC_READ;
        HANDLE hFile = CreateFileW(fullPath, access, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

        if (hFile == INVALID_HANDLE_VALUE)
        {
            char buf[512];
            WideCharToMultiByte(CP_ACP, 0, fullPath, -1, buf, sizeof(buf), NULL, NULL);
            char msg[600];
            sprintf_s(msg, "[WwiseSDK] Failed to open: %s (err=%lu)\n", buf, GetLastError());
            OutputDebugStringA(msg);
            return AK_FileNotFound;
        }

        // Get file size
        LARGE_INTEGER fileSize;
        GetFileSizeEx(hFile, &fileSize);

        out_fileDesc.iFileSize = fileSize.QuadPart;
        out_fileDesc.uSector = 0;
        out_fileDesc.uCustomParamSize = 0;
        out_fileDesc.pCustomParam = NULL;
        out_fileDesc.hFile = hFile;
        out_fileDesc.deviceID = m_deviceID;

        return AK_Success;
    }

    // Open file by ID - not supported, we use name-based loading
    AKRESULT Open(AkFileID in_fileID, AkOpenMode in_eOpenMode,
                  AkFileSystemFlags* in_pFlags, AkFileDesc& out_fileDesc) override
    {
        return AK_FileNotFound;
    }

    // Close a file
    AKRESULT Close(const AkFileDesc& in_fileDesc) override
    {
        if (in_fileDesc.hFile && in_fileDesc.hFile != INVALID_HANDLE_VALUE)
            CloseHandle(in_fileDesc.hFile);
        return AK_Success;
    }

    // Synchronous read (blocking scheduler)
    AKRESULT Read(AkFileDesc& io_fileDesc, void* out_pBuffer,
                  AkIOTransferInfo& io_transferInfo) override
    {
        // Set file position from the OVERLAPPED structure
        LARGE_INTEGER pos;
        pos.LowPart = io_transferInfo.pOverlapped->Offset;
        pos.HighPart = io_transferInfo.pOverlapped->OffsetHigh;
        SetFilePointerEx(io_fileDesc.hFile, pos, NULL, FILE_BEGIN);

        DWORD bytesRead = 0;
        BOOL ok = ReadFile(io_fileDesc.hFile, out_pBuffer,
                           io_transferInfo.uTransferSize, &bytesRead, NULL);

        io_transferInfo.uSizeTransferred = bytesRead;

        if (!ok)
            return AK_Fail;

        return (bytesRead < io_transferInfo.uTransferSize) ? AK_NoMoreData : AK_DataReady;
    }

    // Write - not supported
    AKRESULT Write(AkFileDesc& io_fileDesc, void* in_pData,
                   AkIOTransferInfo& io_transferInfo) override
    {
        return AK_Fail;
    }

    // Async result - not used with blocking scheduler
    AKRESULT GetAsyncResult(AkFileDesc& io_fileDesc,
                            AkIOTransferInfo& io_transferInfo) override
    {
        return AK_Fail;
    }

    // Block size - no alignment requirement
    AkUInt32 GetBlockSize(const AkFileDesc& in_fileDesc) override
    {
        return 1;
    }

private:
    AkDeviceID m_deviceID;
};

// ============================================================================
// Module State
// ============================================================================
static bool              g_Initialized = false;
static ConquestLowLevelIO g_LowLevelIO;
static AkDeviceID        g_DeviceID = AK_INVALID_DEVICE_ID;

struct LoadedBank {
    std::wstring name;
    AkBankID     bankID;
};
static std::vector<LoadedBank> g_LoadedBanks;

static const AkGameObjectID DEFAULT_GAME_OBJECT = 100;
static bool g_DefaultObjectRegistered = false;

static char g_BankBasePathA[MAX_PATH] = "ConquestMods\\audio\\banks\\";

// ============================================================================
// Public API Implementation
// ============================================================================
namespace WwiseSDK {

bool Initialize(HWND hWnd)
{
    if (g_Initialized)
        return true;

    OutputDebugStringA("[WwiseSDK] Initializing separate Wwise engine instance...\n");

    // Ensure bank directory exists
    CreateDirectoryA("ConquestMods", NULL);
    CreateDirectoryA("ConquestMods\\audio", NULL);
    CreateDirectoryA("ConquestMods\\audio\\banks", NULL);

    // ---- 1. Memory Manager ----
    AkMemSettings memSettings;
    memSettings.uMaxNumPools = 20;
    if (AK::MemoryMgr::Init(&memSettings) != AK_Success)
    {
        OutputDebugStringA("[WwiseSDK] ERROR: Memory Manager init failed\n");
        return false;
    }
    OutputDebugStringA("[WwiseSDK] Memory Manager initialized\n");

    // ---- 2. Stream Manager ----
    AkStreamMgrSettings stmSettings;
    stmSettings.pLowLevelIO = &g_LowLevelIO;
    stmSettings.uMemorySize = 64 * 1024; // 64 KB for stream manager internals

    AK::IAkStreamMgr* pStreamMgr = AK::CreateStreamMgr(&stmSettings);
    if (!pStreamMgr)
    {
        OutputDebugStringA("[WwiseSDK] ERROR: Stream Manager creation failed\n");
        AK::MemoryMgr::Term();
        return false;
    }
    OutputDebugStringA("[WwiseSDK] Stream Manager created\n");

    // Create a blocking I/O device
    AkDeviceSettings deviceSettings;
    deviceSettings.uIOMemorySize = 256 * 1024;   // 256 KB I/O buffer
    deviceSettings.uGranularity = 32 * 1024;      // 32 KB per request
    deviceSettings.uSchedulerTypeFlags = AK_SCHEDULER_BLOCKING;
    deviceSettings.pThreadProperties = NULL;       // Use defaults
    deviceSettings.fTargetAutoStmBufferLength = 380.0f;
    deviceSettings.dwIdleWaitTime = INFINITE;

    g_DeviceID = AK::CreateDevice(&deviceSettings);
    if (g_DeviceID == AK_INVALID_DEVICE_ID)
    {
        OutputDebugStringA("[WwiseSDK] ERROR: I/O device creation failed\n");
        pStreamMgr->Destroy();
        AK::MemoryMgr::Term();
        return false;
    }
    g_LowLevelIO.SetDeviceID(g_DeviceID);
    OutputDebugStringA("[WwiseSDK] I/O device created\n");

    // ---- 3. Sound Engine ----
    AkInitSettings initSettings;
    AK::SoundEngine::GetDefaultInitSettings(initSettings);

    AkPlatformInitSettings platformSettings;
    AK::SoundEngine::GetDefaultPlatformInitSettings(platformSettings);
    platformSettings.hWnd = hWnd;

    AKRESULT result = AK::SoundEngine::Init(&initSettings, &platformSettings);
    if (result != AK_Success)
    {
        char buf[128];
        sprintf_s(buf, "[WwiseSDK] ERROR: Sound Engine init failed (0x%08X)\n", result);
        OutputDebugStringA(buf);
        AK::DestroyDevice(g_DeviceID);
        pStreamMgr->Destroy();
        AK::MemoryMgr::Term();
        return false;
    }
    OutputDebugStringA("[WwiseSDK] Sound Engine initialized\n");

    // ---- 4. Music Engine ----
    AkMusicSettings musicSettings;
    AK::MusicEngine::GetDefaultInitSettings(musicSettings);
    if (AK::MusicEngine::Init(&musicSettings) != AK_Success)
    {
        OutputDebugStringA("[WwiseSDK] WARNING: Music Engine init failed (non-fatal)\n");
        // Continue anyway - music engine is optional
    }
    else
    {
        OutputDebugStringA("[WwiseSDK] Music Engine initialized\n");
    }

    // ---- 5. Register Vorbis Codec ----
    AK::SoundEngine::RegisterCodec(
        AKCOMPANYID_AUDIOKINETIC,
        AKCODECID_VORBIS,
        CreateVorbisFilePlugin,
        CreateVorbisBankPlugin
    );
    OutputDebugStringA("[WwiseSDK] Vorbis codec registered\n");

    // ---- 6. Register default game object ----
    if (AK::SoundEngine::RegisterGameObj(DEFAULT_GAME_OBJECT, "ConquestMods_Default") == AK_Success)
    {
        g_DefaultObjectRegistered = true;
    }

    g_Initialized = true;
    OutputDebugStringA("[WwiseSDK] === Initialization complete ===\n");
    return true;
}

void Shutdown()
{
    if (!g_Initialized)
        return;

    OutputDebugStringA("[WwiseSDK] Shutting down...\n");

    // Stop all sounds
    AK::SoundEngine::StopAll();
    AK::SoundEngine::RenderAudio();

    // Unload all banks
    for (auto& bank : g_LoadedBanks)
    {
        AK::SoundEngine::UnloadBank(bank.bankID);
    }
    g_LoadedBanks.clear();

    // Unregister all game objects
    AK::SoundEngine::UnregisterAllGameObj();
    g_DefaultObjectRegistered = false;

    // Terminate in reverse order
    AK::MusicEngine::Term();
    AK::SoundEngine::Term();

    if (g_DeviceID != AK_INVALID_DEVICE_ID)
    {
        AK::DestroyDevice(g_DeviceID);
        g_DeviceID = AK_INVALID_DEVICE_ID;
    }

    if (AK::IAkStreamMgr::Get())
        AK::IAkStreamMgr::Get()->Destroy();

    AK::MemoryMgr::Term();

    g_Initialized = false;
    OutputDebugStringA("[WwiseSDK] Shutdown complete\n");
}

void RenderAudio()
{
    if (g_Initialized)
        AK::SoundEngine::RenderAudio();
}

bool IsInitialized()
{
    return g_Initialized;
}


// ============================================================================
// Bank Management
// ============================================================================

bool LoadBank(const wchar_t* bankName)
{
    if (!g_Initialized || !bankName)
        return false;

    AkBankID bankID = 0;
    AKRESULT result = AK::SoundEngine::LoadBank(bankName, AK_DEFAULT_POOL_ID, bankID);

    if (result != AK_Success)
    {
        char buf[256];
        char nameA[128];
        WideCharToMultiByte(CP_ACP, 0, bankName, -1, nameA, sizeof(nameA), NULL, NULL);
        sprintf_s(buf, "[WwiseSDK] ERROR: LoadBank '%s' failed (0x%08X)\n", nameA, result);
        OutputDebugStringA(buf);
        return false;
    }

    // Track loaded bank
    LoadedBank entry;
    entry.name = bankName;
    entry.bankID = bankID;
    g_LoadedBanks.push_back(entry);

    char buf[256];
    char nameA[128];
    WideCharToMultiByte(CP_ACP, 0, bankName, -1, nameA, sizeof(nameA), NULL, NULL);
    sprintf_s(buf, "[WwiseSDK] Bank '%s' loaded (ID=0x%08X)\n", nameA, bankID);
    OutputDebugStringA(buf);
    return true;
}

void UnloadBank(const wchar_t* bankName)
{
    if (!g_Initialized || !bankName)
        return;

    AKRESULT result = AK::SoundEngine::UnloadBank(bankName, NULL);

    // Remove from tracking vector
    for (auto it = g_LoadedBanks.begin(); it != g_LoadedBanks.end(); ++it)
    {
        if (it->name == bankName)
        {
            g_LoadedBanks.erase(it);
            break;
        }
    }

    char nameA[128];
    WideCharToMultiByte(CP_ACP, 0, bankName, -1, nameA, sizeof(nameA), NULL, NULL);

    if (result == AK_Success)
    {
        char buf[256];
        sprintf_s(buf, "[WwiseSDK] Bank '%s' unloaded\n", nameA);
        OutputDebugStringA(buf);
    }
    else
    {
        char buf[256];
        sprintf_s(buf, "[WwiseSDK] WARNING: UnloadBank '%s' returned 0x%08X\n", nameA, result);
        OutputDebugStringA(buf);
    }
}

void UnloadAllBanks()
{
    if (!g_Initialized)
        return;

    for (auto& bank : g_LoadedBanks)
    {
        AK::SoundEngine::UnloadBank(bank.bankID);
    }
    g_LoadedBanks.clear();
    OutputDebugStringA("[WwiseSDK] All banks unloaded\n");
}

// ============================================================================
// Event Posting
// ============================================================================

unsigned long PostEvent(const wchar_t* eventName, unsigned long gameObjectID)
{
    if (!g_Initialized || !eventName)
        return 0; // AK_INVALID_PLAYING_ID

    AkPlayingID playingID = AK::SoundEngine::PostEvent(
        eventName,
        static_cast<AkGameObjectID>(gameObjectID),
        0, NULL, NULL
    );

    return static_cast<unsigned long>(playingID);
}

unsigned long PostEventByID(unsigned long eventID, unsigned long gameObjectID)
{
    if (!g_Initialized)
        return 0;

    AkPlayingID playingID = AK::SoundEngine::PostEvent(
        static_cast<AkUniqueID>(eventID),
        static_cast<AkGameObjectID>(gameObjectID),
        0, NULL, NULL
    );

    return static_cast<unsigned long>(playingID);
}

void StopAll(unsigned long gameObjectID)
{
    if (g_Initialized)
        AK::SoundEngine::StopAll(static_cast<AkGameObjectID>(gameObjectID));
}

void StopPlayingID(unsigned long playingID)
{
    if (g_Initialized)
        AK::SoundEngine::StopPlayingID(static_cast<AkPlayingID>(playingID));
}

// ============================================================================
// Game Object Management
// ============================================================================

bool RegisterGameObject(unsigned long gameObjectID, const char* name)
{
    if (!g_Initialized)
        return false;

    AKRESULT result;
    if (name)
        result = AK::SoundEngine::RegisterGameObj(static_cast<AkGameObjectID>(gameObjectID), name);
    else
        result = AK::SoundEngine::RegisterGameObj(static_cast<AkGameObjectID>(gameObjectID));

    return (result == AK_Success);
}

void UnregisterGameObject(unsigned long gameObjectID)
{
    if (g_Initialized)
        AK::SoundEngine::UnregisterGameObj(static_cast<AkGameObjectID>(gameObjectID));
}

// ============================================================================
// Volume & State Control
// ============================================================================

void SetRTPCValue(const wchar_t* rtpcName, float value)
{
    if (g_Initialized && rtpcName)
        AK::SoundEngine::SetRTPCValue(rtpcName, static_cast<AkRtpcValue>(value), AK_INVALID_GAME_OBJECT);
}

void SetSwitch(const wchar_t* switchGroup, const wchar_t* switchState, unsigned long gameObjectID)
{
    if (g_Initialized && switchGroup && switchState)
        AK::SoundEngine::SetSwitch(switchGroup, switchState, static_cast<AkGameObjectID>(gameObjectID));
}

void SetState(const wchar_t* stateGroup, const wchar_t* stateValue)
{
    if (g_Initialized && stateGroup && stateValue)
        AK::SoundEngine::SetState(stateGroup, stateValue);
}

// ============================================================================
// Configuration
// ============================================================================

const char* GetBankBasePath()
{
    return g_BankBasePathA;
}

} // namespace WwiseSDK