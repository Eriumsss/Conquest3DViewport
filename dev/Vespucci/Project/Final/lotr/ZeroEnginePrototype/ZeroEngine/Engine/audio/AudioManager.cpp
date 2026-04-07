// ============================================================================
// AudioManager.cpp - Complete Wwise SDK v2.1.2821 Integration
// Full audio system for LOTR Conquest ZeroEngine Prototype
// ============================================================================

#define AKSOUNDENGINE_STATIC
// AK_OPTIMIZED removed — enables Wwise Monitor error callback for diagnostics

#include "AudioManager.h"
#include "BankLoadProgress.h"

#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkMemoryMgr.h>
#include <AK/SoundEngine/Platforms/Windows/AkModule.h>
#include <AK/SoundEngine/Platforms/Windows/AkWinSoundEngine.h>
#include <AK/SoundEngine/Platforms/Windows/AkStreamMgrModule.h>
#include <AK/MusicEngine/Common/AkMusicEngine.h>
#include <AK/Plugin/AkVorbisFactory.h>
#include <AK/Tools/Common/AkMonitorError.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <algorithm>
#include <malloc.h>   // _aligned_malloc / _aligned_free for in-memory bank loading

// ============================================================================
// Required Wwise Memory Hooks
// ============================================================================
namespace AK
{
    void* AllocHook(size_t in_size)                { return malloc(in_size); }
    void  FreeHook(void* in_ptr)                   { free(in_ptr); }
    void* VirtualAllocHook(void* in_ptr, size_t in_size,
                           DWORD in_type, DWORD in_protect)
    { return VirtualAlloc(in_ptr, in_size, in_type, in_protect); }
    void  VirtualFreeHook(void* in_ptr, size_t in_size, DWORD in_type)
    { VirtualFree(in_ptr, in_size, in_type); }
}

// ============================================================================
// Logging Helpers — write to audio_debug.log (visible to user)
// ============================================================================
static FILE* g_audioLogFile = NULL;

static void EnsureAudioLogOpen()
{
    if (!g_audioLogFile)
        g_audioLogFile = fopen("audio_debug.log", "w");
}

void AudioManager::LogAudio(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);

    // Write to file for user visibility
    EnsureAudioLogOpen();
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[Audio] %s\n", buf);
        fflush(g_audioLogFile);
    }

    // Also keep OutputDebugString for debugger
    OutputDebugStringA("[AudioManager] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

void AudioManager::LogAudioError(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, fmt, args);
    va_end(args);

    // Write to file for user visibility
    EnsureAudioLogOpen();
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[Audio] ERROR: %s\n", buf);
        fflush(g_audioLogFile);
    }

    // Also keep OutputDebugString for debugger
    OutputDebugStringA("[AudioManager] ERROR: ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ============================================================================
// Wwise Monitor Callback — captures internal errors (codec, media, bus, etc.)
// ============================================================================
static void WwiseMonitorCallback(
    AkLpCtstr in_pszError,
    AK::Monitor::ErrorLevel in_eErrorLevel)
{
    char buf[1024];
    WideCharToMultiByte(CP_ACP, 0, in_pszError, -1, buf, sizeof(buf), NULL, NULL);

    const char* level = (in_eErrorLevel & AK::Monitor::ErrorLevel_Error) ? "ERROR" : "MSG";

    EnsureAudioLogOpen();
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[Wwise %s] %s\n", level, buf);
        fflush(g_audioLogFile);
    }
    OutputDebugStringA("[Wwise Monitor] ");
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ============================================================================
// ConquestLowLevelIO Implementation — dual-path file resolution
// ============================================================================

// Bank name → STID file ID lookup table (from BANK_ID_TO_NAME_MAPPING.txt + Init/BaseCombat)
// All names stored lowercase for case-insensitive matching.
struct BankNameEntry { const wchar_t* name; AkUInt32 fileID; };
static const BankNameEntry s_bankSTID[] = {
    {L"init",                  1355168291u},  // special: Init bank (STMG)
    {L"basecombat",            3503657562u},  // special: BaseCombat
    {L"ambience",              0x05174939u},
    {L"chatterherobalrog",     0x0165A2BFu},
    {L"chatterherosauron",     0x024F9CA2u},
    {L"vo_osgiliath",          0x02838A9Du},
    {L"vo_rivendell",          0x028A3B82u},
    {L"sfxwarg",               0x09C7E577u},
    {L"vo_trng",               0x1629AEF4u},
    {L"herowitchking",         0x1888157Fu},
    {L"level_rivendell",       0x19E503C3u},
    {L"level_minastir",        0x19FD14F1u},
    {L"sfxcatapult",           0x213157EAu},
    {L"chatterrohan",          0x29E14FEAu},
    {L"chatterherosaruman",    0x305A8AADu},
    {L"herowormtongue",        0x338EAF70u},
    {L"chatterheroaragorn",    0x33EFEBB8u},
    {L"level_pelennor",        0x3A2858EFu},
    {L"vo_pelennor",           0x3C0BE5F4u},
    {L"chatterheroeowyn",      0x43E4F31Eu},
    {L"heronazgul",            0x440AF85Cu},
    {L"level_shire",           0x447B4BB1u},
    {L"sfxeagle",              0x4CFFF4AAu},
    {L"level_mountdoom",       0x4D296ED4u},
    {L"chattergondor",         0x566B984Fu},
    {L"chatterheromouth",      0x5786C025u},
    {L"herogimli",             0x58F75BAFu},
    {L"herolutrz",             0x5C3485ECu},
    {L"herolurtz",             0x5C3485ECu},  // HeroLurtz (both spellings)
    {L"ui",                    0x5C770DB7u},
    {L"chatterherofaramir",    0x5E32F67Cu},
    {L"sfxfellbeast",          0x632D860Au},
    {L"level_helmsdeep",       0x6DBAAE75u},
    {L"vo_minastir",           0x6F58152Au},
    {L"herosaruman",           0x714C77C2u},
    {L"effects",               0x73CB32C9u},
    {L"chatterherogothmog",    0x7AE35325u},
    {L"sfxhorse",              0x81D0868Du},
    {L"chatterherolutrz",      0x85ABD25Bu},
    {L"chatterherolurtz",      0x85ABD25Bu},  // ChatterHeroLurtz (both spellings)
    {L"vo_mountdoom",          0x8A0F7099u},
    {L"herolegolas",           0x8A265726u},
    {L"chatterherotheoden",    0x8A27738Du},
    {L"level_isengard",        0x95B242E3u},
    {L"sfxbalrog",             0x96F1FF69u},
    {L"chatterheroelrond",     0x9CB6D54Cu},
    {L"level_trng",            0x9DD2F9EBu},
    {L"chatterherofrodo",      0x9DD4E9D6u},
    {L"sfxballista",           0xA1B662E0u},
    {L"level_moria",           0xA56B42E4u},
    {L"chatterherowitchking",  0xA67CA2E4u},
    {L"sfxtroll",              0xA6C9C687u},
    {L"chatterheronazgul",     0xA7AF2725u},  // ChatterHeroNazgul
    {L"heroeowyn",             0xA7C4543Du},
    {L"herofrodo",             0xA8C5FC53u},
    {L"vo_weathertop",         0xA954F1AAu},
    {L"vo_moria",              0xAA229089u},
    {L"herosauron",            0xAE7FDABBu},
    {L"herotheoden",           0xAEE7241Eu},
    {L"level_weathertop",      0xAF7518E5u},
    {L"vo_shire",              0xAFB46E4Cu},
    {L"level_minasmorg",       0xB36D8F45u},
    {L"heroisildur",           0xB659C16Fu},
    {L"herogandalf",           0xB9842D06u},
    {L"chatterhobbit",         0xBD0C4C7Cu},
    {L"chatteruruk",           0xBEE19A09u},
    {L"level_blackgates",      0xC3C20417u},
    {L"vo_blackgates",         0xC463B59Cu},
    {L"sfxoliphant",           0xC53E7D9Bu},
    {L"vo_helmsdeep",          0xC5442280u},
    {L"sfxent",                0xCDCB4FCDu},
    {L"vo_minasmorg",          0xD07A401Cu},
    {L"heroaragorn",           0xD210FF9Bu},
    {L"herotreebeard",         0xD35B3D39u},
    {L"chatterevilhuman",      0xDA68CA19u},
    {L"heromouth",             0xDC91750Eu},
    {L"chatterorc",            0xDD4376FAu},
    {L"sfxbatteringram",       0xDD97A3A8u},
    {L"vo_isengard",           0xE3797390u},
    {L"chatterherogandalf",    0xE4AA8BF9u},
    {L"chatterherowormtongue", 0xE9B8F871u},
    {L"level_osgiliath",       0xEB54E0A4u},
    {L"music",                 0xEDF036D6u},
    {L"chatterherogimli",      0xEE150C68u},
    {L"voiceover",             0xF0E6CC1Bu},
    {L"herogothmog",           0xF3DD57E2u},
    {L"heroelrond",            0xF7D0958Du},
    {L"chatterelf",            0xFB2BA08Bu},
    {L"sfxsiegetower",         0xFBB0EFCEu},
    {L"herofaramir",           0xFC3659ABu},
    {L"chatterherolegolas",    0xFE28C825u},
};
static const int s_bankSTIDCount = sizeof(s_bankSTID) / sizeof(s_bankSTID[0]);

// Lookup bank file ID from name (case-insensitive)
static AkUInt32 LookupBankFileID(const wchar_t* bankName)
{
    // Convert input to lowercase for comparison
    wchar_t lower[256];
    int i = 0;
    for (; bankName[i] && i < 255; ++i)
    {
        wchar_t c = bankName[i];
        lower[i] = (c >= L'A' && c <= L'Z') ? (c + 32) : c;
    }
    lower[i] = L'\0';

    for (int j = 0; j < s_bankSTIDCount; ++j)
    {
        if (wcscmp(lower, s_bankSTID[j].name) == 0)
            return s_bankSTID[j].fileID;
    }
    return 0; // not found
}

AKRESULT ConquestLowLevelIO::OpenByPath(const wchar_t* fullPath, AkFileDesc& out_fileDesc)
{
    HANDLE hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return AK_FileNotFound;

    LARGE_INTEGER fileSize;
    GetFileSizeEx(hFile, &fileSize);

    out_fileDesc.iFileSize        = fileSize.QuadPart;
    out_fileDesc.uSector          = 0;
    out_fileDesc.uCustomParamSize = 0;
    out_fileDesc.pCustomParam     = NULL;
    out_fileDesc.hFile            = hFile;
    out_fileDesc.deviceID         = m_deviceID;
    return AK_Success;
}

// Helper: Search for a bank file in subdirectories (for organized structure)
// Searches basePath\*\fileID\fileID.bnk pattern
static bool FindBankInSubdirs(const wchar_t* basePath, AkUInt32 fileID, wchar_t* outPath, size_t outPathSize)
{
    WIN32_FIND_DATAW findData;
    wchar_t searchPattern[512];  // Increased buffer size
    _snwprintf_s(searchPattern, 512, _TRUNCATE, L"%s*", basePath);

    EnsureAudioLogOpen();
    if (g_audioLogFile) {
        fprintf(g_audioLogFile, "[FindBankInSubdirs] Searching for fileID=%u in basePath=%S\n", fileID, basePath);
        fflush(g_audioLogFile);
    }

    HANDLE hFind = FindFirstFileW(searchPattern, &findData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        if (g_audioLogFile) {
            fprintf(g_audioLogFile, "[FindBankInSubdirs] FindFirstFileW FAILED for pattern: %S\n", searchPattern);
            fflush(g_audioLogFile);
        }
        return false;
    }

    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(findData.cFileName, L".") != 0 &&
            wcscmp(findData.cFileName, L"..") != 0)
        {
            // Try basePath\category\fileID\fileID.bnk
            wchar_t candidatePath[512];  // Increased buffer size
            _snwprintf_s(candidatePath, 512, _TRUNCATE,
                        L"%s%s\\%u\\%u.bnk", basePath, findData.cFileName, fileID, fileID);

            if (g_audioLogFile) {
                fprintf(g_audioLogFile, "[FindBankInSubdirs] Checking category '%S', path: %S\n",
                       findData.cFileName, candidatePath);
                fflush(g_audioLogFile);
            }

            DWORD attrs = GetFileAttributesW(candidatePath);
            if (attrs != INVALID_FILE_ATTRIBUTES)
            {
                if (g_audioLogFile) {
                    fprintf(g_audioLogFile, "[FindBankInSubdirs] FOUND: %S\n", candidatePath);
                    fflush(g_audioLogFile);
                }
                wcscpy_s(outPath, outPathSize, candidatePath);
                FindClose(hFind);
                return true;
            }
            else if (g_audioLogFile) {
                fprintf(g_audioLogFile, "[FindBankInSubdirs] NOT FOUND (attrs=0x%08X): %S\n", attrs, candidatePath);
                fflush(g_audioLogFile);
            }
        }
    } while (FindNextFileW(hFind, &findData));

    if (g_audioLogFile) {
        fprintf(g_audioLogFile, "[FindBankInSubdirs] Exhausted all categories, fileID=%u NOT FOUND\n", fileID);
        fflush(g_audioLogFile);
    }

    FindClose(hFind);
    return false;
}

// Helper: Search for a WEM file in subdirectories (for organized structure)
// Searches basePath\*\*\fileID.wem pattern (all category folders and their bank subfolders)
static bool FindWemInSubdirs(const wchar_t* basePath, AkUInt32 fileID, wchar_t* outPath, size_t outPathSize)
{
    WIN32_FIND_DATAW categoryData;
    wchar_t searchPattern[512];  // Increased buffer size
    _snwprintf_s(searchPattern, 512, _TRUNCATE, L"%s*", basePath);

    HANDLE hCategoryFind = FindFirstFileW(searchPattern, &categoryData);
    if (hCategoryFind == INVALID_HANDLE_VALUE)
        return false;

    // Iterate through category folders (BaseCombat, Effects, ChatterElf, etc.)
    do
    {
        if ((categoryData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(categoryData.cFileName, L".") != 0 &&
            wcscmp(categoryData.cFileName, L"..") != 0)
        {
            // Search within this category folder for bank ID subfolders
            wchar_t categoryPath[512];  // Increased buffer size
            _snwprintf_s(categoryPath, 512, _TRUNCATE, L"%s%s\\*", basePath, categoryData.cFileName);

            WIN32_FIND_DATAW bankData;
            HANDLE hBankFind = FindFirstFileW(categoryPath, &bankData);
            if (hBankFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((bankData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        wcscmp(bankData.cFileName, L".") != 0 &&
                        wcscmp(bankData.cFileName, L"..") != 0)
                    {
                        // Try basePath\category\bankID\fileID.wem
                        wchar_t candidatePath[512];  // Increased buffer size
                        _snwprintf_s(candidatePath, 512, _TRUNCATE,
                                    L"%s%s\\%s\\%u.wem", basePath, categoryData.cFileName, bankData.cFileName, fileID);

                        if (GetFileAttributesW(candidatePath) != INVALID_FILE_ATTRIBUTES)
                        {
                            wcscpy_s(outPath, outPathSize, candidatePath);
                            FindClose(hBankFind);
                            FindClose(hCategoryFind);
                            return true;
                        }
                    }
                } while (FindNextFileW(hBankFind, &bankData));
                FindClose(hBankFind);
            }
        }
    } while (FindNextFileW(hCategoryFind, &categoryData));

    FindClose(hCategoryFind);
    return false;
}

// Open by name: Wwise passes bank name (e.g. L"Init"). STID lookup → numeric .bnk.
// Searches root/ first, then language path.
AKRESULT ConquestLowLevelIO::Open(AkLpCtstr in_pszFileName, AkOpenMode,
                                  AkFileSystemFlags*, AkFileDesc& out_fileDesc)
{
    if (!in_pszFileName) return AK_FileNotFound;

    // Strip .bnk extension if Wwise appended it
    wchar_t nameOnly[MAX_PATH];
    wcscpy_s(nameOnly, in_pszFileName);
    size_t len = wcslen(nameOnly);
    if (len > 4 && _wcsicmp(nameOnly + len - 4, L".bnk") == 0)
        nameOnly[len - 4] = L'\0';

    // Log what Wwise requested
    char narrowName[256];
    WideCharToMultiByte(CP_ACP, 0, nameOnly, -1, narrowName, 256, NULL, NULL);
    EnsureAudioLogOpen();
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[LowLevelIO] Open(name): '%s'\n", narrowName);
        fflush(g_audioLogFile);
    }

    AkUInt32 fileID = LookupBankFileID(nameOnly);
    if (fileID == 0)
    {
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] FAIL: no STID for '%s'\n", narrowName);
            fflush(g_audioLogFile);
        }
        return AK_FileNotFound;
    }

    wchar_t fullPath[512];  // Increased buffer size
    _snwprintf_s(fullPath, 512, _TRUNCATE, L"%s%u.bnk", m_basePath, fileID);

    char narrowPath[512];
    WideCharToMultiByte(CP_ACP, 0, fullPath, -1, narrowPath, 512, NULL, NULL);
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[LowLevelIO] Trying: %s (ID=%u)\n", narrowPath, fileID);
        fflush(g_audioLogFile);
    }

    AKRESULT res = OpenByPath(fullPath, out_fileDesc);
    if (res == AK_Success)
    {
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] OK: %s\n", narrowPath);
            fflush(g_audioLogFile);
        }
        return res;
    }

    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[LowLevelIO] MISS root: %s\n", narrowPath);
        fflush(g_audioLogFile);
    }

    // Fallback: try language path (for VO/chatter banks)
    if (m_langPath[0] != L'\0')
    {
        _snwprintf_s(fullPath, 512, _TRUNCATE, L"%s%u.bnk", m_langPath, fileID);
        WideCharToMultiByte(CP_ACP, 0, fullPath, -1, narrowPath, 512, NULL, NULL);
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] Trying (lang): %s\n", narrowPath);
            fflush(g_audioLogFile);
        }
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile)
            {
                fprintf(g_audioLogFile, "[LowLevelIO] OK (lang): %s\n", narrowPath);
                fflush(g_audioLogFile);
            }
            return res;
        }
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] MISS (lang): %s\n", narrowPath);
            fflush(g_audioLogFile);
        }
    }

    // Fallback: search subdirectories in root path (for organized structure)
    if (FindBankInSubdirs(m_basePath, fileID, fullPath, MAX_PATH))
    {
        WideCharToMultiByte(CP_ACP, 0, fullPath, -1, narrowPath, 512, NULL, NULL);
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] Trying (subdir): %s\n", narrowPath);
            fflush(g_audioLogFile);
        }
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile)
            {
                fprintf(g_audioLogFile, "[LowLevelIO] OK (subdir): %s\n", narrowPath);
                fflush(g_audioLogFile);
            }
            return res;
        }
    }

    // Fallback: search subdirectories in language path (for organized VO/chatter)
    if (m_langPath[0] != L'\0' && FindBankInSubdirs(m_langPath, fileID, fullPath, MAX_PATH))
    {
        WideCharToMultiByte(CP_ACP, 0, fullPath, -1, narrowPath, 512, NULL, NULL);
        if (g_audioLogFile)
        {
            fprintf(g_audioLogFile, "[LowLevelIO] Trying (lang subdir): %s\n", narrowPath);
            fflush(g_audioLogFile);
        }
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile)
            {
                fprintf(g_audioLogFile, "[LowLevelIO] OK (lang subdir): %s\n", narrowPath);
                fflush(g_audioLogFile);
            }
            return res;
        }
    }

    return AK_FileNotFound;
}

// Open by file ID: for streaming .wem files (voice, music).
// Searches: wem/ → wem/english(us)/ → extracted/ → extracted/english(us)/ → .bnk fallback
AKRESULT ConquestLowLevelIO::Open(AkFileID in_fileID, AkOpenMode,
                                  AkFileSystemFlags*, AkFileDesc& out_fileDesc)
{
    wchar_t fullPath[512];  // Increased buffer size
    AKRESULT res;

    EnsureAudioLogOpen();
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[LowLevelIO] Open(fileID=%u)\n", (unsigned)in_fileID);
        fflush(g_audioLogFile);
    }

    // Build wemBase = ".../ExtractedSounds/" from m_basePath = ".../extracted/"
    wchar_t wemBase[512];  // Increased buffer size
    wcscpy_s(wemBase, 512, m_basePath);
    size_t len = wcslen(wemBase);
    if (len > 0 && (wemBase[len-1] == L'\\' || wemBase[len-1] == L'/'))
        wemBase[len-1] = L'\0';
    wchar_t* lastSlash = wcsrchr(wemBase, L'\\');
    if (!lastSlash) lastSlash = wcsrchr(wemBase, L'/');
    if (lastSlash)
        *(lastSlash + 1) = L'\0';  // ".../ExtractedSounds/"

    // 1. Try wem/<id>.wem  (411 SFX .wem files)
    if (lastSlash)
    {
        _snwprintf_s(fullPath, 512, _TRUNCATE, L"%swem\\%u.wem", wemBase, in_fileID);
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK wem/: %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
            return res;
        }
    }

    // 2. Try wem/english(us)/<id>.wem  (4398 VO .wem files)
    if (lastSlash)
    {
        _snwprintf_s(fullPath, 512, _TRUNCATE, L"%swem\\english(us)\\%u.wem", wemBase, in_fileID);
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK wem/english(us)/: %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
            return res;
        }
    }

    // 3. Try extracted/<id>.wem  (basePath)
    _snwprintf_s(fullPath, 512, _TRUNCATE, L"%s%u.wem", m_basePath, in_fileID);
    res = OpenByPath(fullPath, out_fileDesc);
    if (res == AK_Success)
    {
        if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK extracted/: %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
        return res;
    }

    // 4. Try extracted/english(us)/<id>.wem  (langPath)
    if (m_langPath[0] != L'\0')
    {
        _snwprintf_s(fullPath, 512, _TRUNCATE, L"%s%u.wem", m_langPath, in_fileID);
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK langPath/: %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
            return res;
        }
    }

    // 5. Try searching subdirectories in root path (Organized_Final structure)
    if (FindWemInSubdirs(m_basePath, in_fileID, fullPath, MAX_PATH))
    {
        res = OpenByPath(fullPath, out_fileDesc);
        if (res == AK_Success)
        {
            if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK (subdir root): %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
            return res;
        }
    }

    // 6. Try searching subdirectories in language path
    if (m_langPath[0] != L'\0')
    {
        if (FindWemInSubdirs(m_langPath, in_fileID, fullPath, MAX_PATH))
        {
            res = OpenByPath(fullPath, out_fileDesc);
            if (res == AK_Success)
            {
                if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK (subdir lang): %u.wem\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
                return res;
            }
        }
    }

    // 7. Last resort: try .bnk extension (bank-by-ID)
    _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s%u.bnk", m_basePath, in_fileID);
    res = OpenByPath(fullPath, out_fileDesc);
    if (res == AK_Success)
    {
        if (g_audioLogFile) { fprintf(g_audioLogFile, "[LowLevelIO] OK bnk fallback: %u.bnk\n", (unsigned)in_fileID); fflush(g_audioLogFile); }
        return res;
    }

    // All paths failed
    if (g_audioLogFile)
    {
        fprintf(g_audioLogFile, "[LowLevelIO] MISS ALL: fileID=%u (.wem not found anywhere)\n", (unsigned)in_fileID);
        fflush(g_audioLogFile);
    }
    return AK_FileNotFound;
}

AKRESULT ConquestLowLevelIO::Close(const AkFileDesc& in_fileDesc)
{
    if (in_fileDesc.hFile && in_fileDesc.hFile != INVALID_HANDLE_VALUE)
        CloseHandle(in_fileDesc.hFile);
    return AK_Success;
}

AKRESULT ConquestLowLevelIO::Read(AkFileDesc& io_fileDesc, void* out_pBuffer,
                                  AkIOTransferInfo& io_transferInfo)
{
    LARGE_INTEGER pos;
    pos.LowPart  = io_transferInfo.pOverlapped->Offset;
    pos.HighPart = io_transferInfo.pOverlapped->OffsetHigh;
    SetFilePointerEx(io_fileDesc.hFile, pos, NULL, FILE_BEGIN);

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(io_fileDesc.hFile, out_pBuffer,
                       io_transferInfo.uTransferSize, &bytesRead, NULL);
    io_transferInfo.uSizeTransferred = bytesRead;

    if (!ok) return AK_Fail;
    return (bytesRead < io_transferInfo.uTransferSize) ? AK_NoMoreData : AK_DataReady;
}

AKRESULT ConquestLowLevelIO::Write(AkFileDesc&, void*, AkIOTransferInfo&)
{ return AK_Fail; }

AKRESULT ConquestLowLevelIO::GetAsyncResult(AkFileDesc&, AkIOTransferInfo&)
{ return AK_Fail; }

AkUInt32 ConquestLowLevelIO::GetBlockSize(const AkFileDesc&)
{ return 1; }

#ifndef AK_OPTIMIZED
AKRESULT ConquestLowLevelIO::GetDeviceDesc(AkDeviceID in_deviceID, AkDeviceDesc& out_deviceDesc)
{
    out_deviceDesc.deviceID   = in_deviceID;
    out_deviceDesc.bCanRead   = true;
    out_deviceDesc.bCanWrite  = false;
    wcscpy_s(out_deviceDesc.szDeviceName, AK_MONITOR_DEVICENAME_MAXLENGTH, L"ConquestLowLevelIO");
    out_deviceDesc.uStringSize = (AkUInt32)wcslen(out_deviceDesc.szDeviceName);
    return AK_Success;
}
#endif

// ============================================================================
// AudioManager Constructor / Destructor (C++03 — no in-class initializers)
// ============================================================================
AudioManager::AudioManager()
    : m_initialized(false)
    , m_deviceID(AK_INVALID_DEVICE_ID)
    , m_currentVOPlayingID(AK_INVALID_PLAYING_ID)
    , m_voPlaying(false)
    , m_currentMusicPlayingID(AK_INVALID_PLAYING_ID)
    , m_masterVol(1.0f)
    , m_sfxVol(1.0f)
    , m_musicVol(1.0f)
    , m_voiceVol(1.0f)
{
}

AudioManager::~AudioManager()
{
}

// ============================================================================
// AudioManager Singleton
// ============================================================================
AudioManager& AudioManager::Get()
{
    static AudioManager instance;
    return instance;
}

// ============================================================================
// Initialize: MemoryMgr → StreamMgr → Device → SoundEngine → MusicEngine → Vorbis
// ============================================================================
bool AudioManager::Initialize(HWND hWnd, const wchar_t* bankRootPath,
                              const wchar_t* bankLangPath)
{
    if (m_initialized) return true;

    m_lowLevelIO.SetBasePath(bankRootPath);
    if (bankLangPath)
        m_lowLevelIO.SetLanguagePath(bankLangPath);

    // 1. Memory Manager
    AkMemSettings memSettings;
    memSettings.uMaxNumPools = 20;
    if (AK::MemoryMgr::Init(&memSettings) != AK_Success)
    {
        LogAudioError("MemoryMgr init failed");
        return false;
    }

    // 2. Stream Manager
    AkStreamMgrSettings stmSettings;
    stmSettings.pLowLevelIO = &m_lowLevelIO;
    stmSettings.uMemorySize = 64 * 1024;

    AK::IAkStreamMgr* pStreamMgr = AK::CreateStreamMgr(&stmSettings);
    if (!pStreamMgr)
    {
        LogAudioError("StreamMgr creation failed");
        AK::MemoryMgr::Term();
        return false;
    }

    // 3. I/O Device (blocking scheduler)
    AkDeviceSettings deviceSettings;
    deviceSettings.uIOMemorySize             = 256 * 1024;
    deviceSettings.uGranularity              = 32 * 1024;
    deviceSettings.uSchedulerTypeFlags       = AK_SCHEDULER_BLOCKING;
    deviceSettings.pThreadProperties         = NULL;
    deviceSettings.fTargetAutoStmBufferLength = 380.0f;
    deviceSettings.dwIdleWaitTime            = INFINITE;

    m_deviceID = AK::CreateDevice(&deviceSettings);
    if (m_deviceID == AK_INVALID_DEVICE_ID)
    {
        LogAudioError("I/O device creation failed");
        pStreamMgr->Destroy();
        AK::MemoryMgr::Term();
        return false;
    }
    m_lowLevelIO.SetDeviceID(m_deviceID);

    // 4. Sound Engine
    AkInitSettings initSettings;
    AK::SoundEngine::GetDefaultInitSettings(initSettings);

    AkPlatformInitSettings platformSettings;
    AK::SoundEngine::GetDefaultPlatformInitSettings(platformSettings);
    platformSettings.hWnd = hWnd;
    platformSettings.bGlobalFocus = true;   // keep audio playing even when window loses focus

    if (AK::SoundEngine::Init(&initSettings, &platformSettings) != AK_Success)
    {
        LogAudioError("SoundEngine init failed");
        AK::DestroyDevice(m_deviceID);
        pStreamMgr->Destroy();
        AK::MemoryMgr::Term();
        return false;
    }

    // 4b. Register Wwise Monitor callback for error diagnostics
    {
        AKRESULT monRes = AK::Monitor::SetLocalOutput(
            AK::Monitor::ErrorLevel_All, WwiseMonitorCallback);
        if (monRes == AK_Success)
            LogAudio("Wwise Monitor callback registered (ErrorLevel_All)");
        else
            LogAudioError("Wwise Monitor callback FAILED (0x%08X) — AK_OPTIMIZED still active in SDK?",
                         monRes);
    }

    // 5. Music Engine
    AkMusicSettings musicSettings;
    AK::MusicEngine::GetDefaultInitSettings(musicSettings);
    AK::MusicEngine::Init(&musicSettings);

    // 6. Vorbis Codec
    AK::SoundEngine::RegisterCodec(
        AKCOMPANYID_AUDIOKINETIC, AKCODECID_VORBIS,
        CreateVorbisFilePlugin, CreateVorbisBankPlugin);

    // 7. Default game object + listener
    AK::SoundEngine::RegisterGameObj(DEFAULT_GAME_OBJECT, "ZeroEngine_Default");
    AK::SoundEngine::RegisterGameObj(LISTENER_OBJECT, "ZeroEngine_Listener");

    // Associate default game object with listener 0
    AkUInt32 listenerArray[] = { 0 };
    AK::SoundEngine::SetActiveListeners(DEFAULT_GAME_OBJECT, 0x1); // bitmask: listener 0

    // Set default listener position (origin, looking +Z, up +Y)
    AkListenerPosition listenerPos;
    memset(&listenerPos, 0, sizeof(listenerPos));
    listenerPos.OrientationFront.Z = 1.0f;
    listenerPos.OrientationTop.Y   = 1.0f;
    AK::SoundEngine::SetListenerPosition(listenerPos, 0);

    m_initialized = true;

    // Build event mapping lookup (2,817 entries from AudioHook DLL)
    BuildEventLookup();

    LogAudio("Initialized successfully (root=%ls, lang=%ls)",
             bankRootPath, bankLangPath ? bankLangPath : L"<none>");
    return true;
}

// ============================================================================
// Shutdown: reverse order
// ============================================================================
void AudioManager::Shutdown()
{
    if (!m_initialized) return;

    FlushVOQueue();
    AK::SoundEngine::StopAll();
    UnloadAllBanks();
    AK::SoundEngine::UnregisterAllGameObj();

    AK::MusicEngine::Term();
    AK::SoundEngine::Term();

    if (m_deviceID != AK_INVALID_DEVICE_ID)
        AK::DestroyDevice(m_deviceID);

    if (AK::IAkStreamMgr::Get())
        AK::IAkStreamMgr::Get()->Destroy();

    AK::MemoryMgr::Term();

    m_loadedBanks.clear();
    m_voQueue.clear();
    m_currentHero.clear();
    m_currentLevel.clear();
    m_deviceID    = AK_INVALID_DEVICE_ID;
    m_initialized = false;
    LogAudio("Shutdown complete");
}

// ============================================================================
// RenderAudio — call once per frame
// ============================================================================
void AudioManager::RenderAudio()
{
    if (!m_initialized) return;
    UpdateVOQueue();
    AK::SoundEngine::RenderAudio();
}

// ============================================================================
// Bank Management
// ============================================================================
bool AudioManager::LoadBank(const char* bankName, BankCategory cat)
{
    if (!m_initialized || !bankName) return false;

    LogAudio(">>> LoadBank START: '%s' (cat=%d)", bankName, (int)cat);

    // Check if already loaded — increment refCount
    for (size_t i = 0; i < m_loadedBanks.size(); ++i)
    {
        if (m_loadedBanks[i].name == bankName) { m_loadedBanks[i].refCount++; return true; }
    }

    wchar_t wName[256];
    MultiByteToWideChar(CP_ACP, 0, bankName, -1, wName, 256);

    AkBankID bankID = AK_INVALID_BANK_ID;
    AKRESULT res = AK::SoundEngine::LoadBank(wName, AK_DEFAULT_POOL_ID, bankID);

    if (res == AK_Success)
    {
        LoadedBankInfo info;
        info.name = bankName;
        info.bankID = bankID;
        info.category = cat;
        info.refCount = 1;
        m_loadedBanks.push_back(info);
        LogAudio("Loaded bank: %s (ID=%u, cat=%d)", bankName, bankID, (int)cat);
        return true;
    }

    LogAudioError("LoadBank(string) failed: %s (err=0x%08X) — trying in-memory fallback", bankName, res);

    // ---- In-memory fallback: bypass LowLevelIO entirely ----
    // Look up file ID from STID table, build path, read file, pass buffer to Wwise
    {
        // Strip .bnk if caller included it, then lookup
        wchar_t nameOnly[256];
        wcscpy_s(nameOnly, wName);
        size_t len = wcslen(nameOnly);
        if (len > 4 && _wcsicmp(nameOnly + len - 4, L".bnk") == 0)
            nameOnly[len - 4] = L'\0';

        AkUInt32 fileID = LookupBankFileID(nameOnly);
        if (fileID == 0)
        {
            LogAudioError("In-memory fallback: no STID entry for '%s'", bankName);
            return false;
        }

        // Build full path: try root flat, then root subdirs, then lang
        const wchar_t* basePath = m_lowLevelIO.GetBasePath();
        wchar_t fullPath[MAX_PATH];
        _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s%u.bnk", basePath, fileID);

        HANDLE hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

        // Try subdirectory search if flat file not found
        if (hFile == INVALID_HANDLE_VALUE)
        {
            LogAudio("In-memory fallback: searching subdirs for '%s' (ID=%u)", bankName, fileID);
            if (FindBankInSubdirs(basePath, fileID, fullPath, MAX_PATH))
            {
                LogAudio("In-memory fallback: found in subdir: %S", fullPath);
                hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            }
            else
            {
                LogAudio("In-memory fallback: NOT found in subdirs");
            }
        }

        // Try language path
        if (hFile == INVALID_HANDLE_VALUE && m_lowLevelIO.GetLangPath()[0] != L'\0')
        {
            _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s%u.bnk",
                         m_lowLevelIO.GetLangPath(), fileID);
            hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        }

        if (hFile == INVALID_HANDLE_VALUE)
        {
            LogAudioError("In-memory fallback: file not found for '%s' (ID=%u)", bankName, fileID);
            return false;
        }

        LARGE_INTEGER fileSizeLI;
        GetFileSizeEx(hFile, &fileSizeLI);
        DWORD fileSize = (DWORD)fileSizeLI.QuadPart;

        // Allocate 16-byte aligned buffer (AK_BANK_PLATFORM_DATA_ALIGNMENT on Win32)
        void* pBuf = _aligned_malloc(fileSize, 16);
        if (!pBuf)
        {
            CloseHandle(hFile);
            LogAudioError("In-memory fallback: alloc failed (%u bytes) for '%s'", fileSize, bankName);
            return false;
        }

        DWORD bytesRead = 0;
        BOOL readOK = ReadFile(hFile, pBuf, fileSize, &bytesRead, NULL);
        CloseHandle(hFile);

        if (!readOK || bytesRead != fileSize)
        {
            _aligned_free(pBuf);
            LogAudioError("In-memory fallback: read error for '%s' (%u/%u)", bankName, bytesRead, fileSize);
            return false;
        }

        LogAudio("In-memory fallback: read %u bytes for '%s' (ID=%u), attempting LoadBank...",
                 fileSize, bankName, fileID);

        bankID = AK_INVALID_BANK_ID;
        res = AK::SoundEngine::LoadBank(pBuf, fileSize, bankID);

        // Note: if LoadBank succeeds with in-memory, Wwise owns the memory.
        // If it fails, we must free it.
        if (res == AK_Success)
        {
            LogAudio("In-memory fallback SUCCESS: %s (bankID=%u)", bankName, bankID);
            LoadedBankInfo info;
            info.name = bankName;
            info.bankID = bankID;
            info.category = cat;
            info.refCount = 1;
            m_loadedBanks.push_back(info);
            // Do NOT free pBuf — Wwise now references it
            return true;
        }
        else
        {
            _aligned_free(pBuf);
            LogAudioError("In-memory fallback ALSO FAILED: %s (err=0x%08X)", bankName, res);
            LogAudioError("  -> Bank content is incompatible with this Wwise SDK build");
            return false;
        }
    }
}

// Load bank by numeric ID (for banks without names in STID table)
bool AudioManager::LoadBankByID(AkUInt32 fileID, BankCategory cat)
{
    if (!m_initialized) return false;

    char bankName[64];
    sprintf_s(bankName, 64, "Bank_%u", fileID);

    LogAudio(">>> LoadBankByID START: ID=%u (cat=%d)", fileID, (int)cat);

    // Check if already loaded
    for (size_t i = 0; i < m_loadedBanks.size(); ++i)
    {
        if (m_loadedBanks[i].bankID == fileID) {
            m_loadedBanks[i].refCount++;
            LogAudio("Bank ID=%u already loaded, refCount=%d", fileID, m_loadedBanks[i].refCount);
            return true;
        }
    }

    // Try to load via in-memory (since we don't have a name for the STID lookup)
    const wchar_t* basePath = m_lowLevelIO.GetBasePath();

    wchar_t fullPath[512];
    _snwprintf_s(fullPath, 512, _TRUNCATE, L"%s%u.bnk", basePath, fileID);

    // Try flat file first
    HANDLE hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);

    // Try subdirectory search if flat file not found
    if (hFile == INVALID_HANDLE_VALUE) {
        LogAudio("LoadBankByID: searching subdirs for ID=%u", fileID);
        if (FindBankInSubdirs(basePath, fileID, fullPath, 512)) {
            LogAudio("LoadBankByID: found in subdir: %S", fullPath);
            hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        } else {
            LogAudio("LoadBankByID: NOT found in subdirs");
        }
    }

    if (hFile == INVALID_HANDLE_VALUE)
    {
        LogAudioError("LoadBankByID: file not found for ID=%u", fileID);
        return false;
    }

    LARGE_INTEGER fileSizeLI;
    GetFileSizeEx(hFile, &fileSizeLI);
    DWORD fileSize = (DWORD)fileSizeLI.QuadPart;

    void* pBuf = _aligned_malloc(fileSize, 16);
    if (!pBuf)
    {
        CloseHandle(hFile);
        LogAudioError("LoadBankByID: alloc failed (%u bytes) for ID=%u", fileSize, fileID);
        return false;
    }

    DWORD bytesRead = 0;
    BOOL readOK = ReadFile(hFile, pBuf, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    if (!readOK || bytesRead != fileSize)
    {
        _aligned_free(pBuf);
        LogAudioError("LoadBankByID: read error for ID=%u (%u/%u)", fileID, bytesRead, fileSize);
        return false;
    }

    LogAudio("LoadBankByID: read %u bytes for ID=%u, attempting LoadBank...", fileSize, fileID);

    AkBankID bankID = AK_INVALID_BANK_ID;
    AKRESULT res = AK::SoundEngine::LoadBank(pBuf, fileSize, bankID);

    if (res == AK_Success)
    {
        LogAudio("LoadBankByID SUCCESS: ID=%u (bankID=%u)", fileID, bankID);
        LoadedBankInfo info;
        info.name = bankName;
        info.bankID = bankID;
        info.category = cat;
        info.refCount = 1;
        m_loadedBanks.push_back(info);
        return true;
    }
    else
    {
        _aligned_free(pBuf);
        LogAudioError("LoadBankByID FAILED: ID=%u (err=0x%08X)", fileID, res);
        return false;
    }
}

void AudioManager::UnloadBank(const char* bankName)
{
    if (!m_initialized || !bankName) return;

    for (std::vector<LoadedBankInfo>::iterator it = m_loadedBanks.begin(); it != m_loadedBanks.end(); ++it)
    {
        if (it->name == bankName)
        {
            it->refCount--;
            if (it->refCount <= 0)
            {
                wchar_t wName[256];
                MultiByteToWideChar(CP_ACP, 0, bankName, -1, wName, 256);
                AK::SoundEngine::UnloadBank(wName);
                LogAudio("Unloaded bank: %s", bankName);
                m_loadedBanks.erase(it);
            }
            return;
        }
    }
}

void AudioManager::UnloadAllBanks()
{
    for (size_t i = 0; i < m_loadedBanks.size(); ++i)
    {
        wchar_t wName[256];
        MultiByteToWideChar(CP_ACP, 0, m_loadedBanks[i].name.c_str(), -1, wName, 256);
        AK::SoundEngine::UnloadBank(wName);
    }
    m_loadedBanks.clear();
    LogAudio("Unloaded all banks");
}

bool AudioManager::IsBankLoaded(const char* bankName) const
{
    if (!bankName) return false;
    for (size_t i = 0; i < m_loadedBanks.size(); ++i)
    {
        // Case-insensitive comparison (event mapping uses mixed case, banks load as lowercase)
        if (_stricmp(m_loadedBanks[i].name.c_str(), bankName) == 0)
            return true;
    }
    return false;
}

// ============================================================================
// STMG Chunk Parser — reads Init.bnk to check master bus volume
// STMG layout (bank version 34, Wwise v2.8):
//   float  fVolumeThreshold (dB)
//   uint16 uMaxNumVoicesLimitInternal
//   uint32 numStateGroups, then state group data...
// ============================================================================
static void StmgLog(const char* fmt, ...)
{
    EnsureAudioLogOpen();
    if (!g_audioLogFile) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_audioLogFile, "[AUDIO] ");
    vfprintf(g_audioLogFile, fmt, ap);
    fprintf(g_audioLogFile, "\n");
    fflush(g_audioLogFile);
    va_end(ap);
}

static void DiagnoseInitBankSTMG(const wchar_t* bankRootPath)
{
    // Init.bnk file ID = 1355168291 (from STID mapping)
    wchar_t initPath[MAX_PATH];
    _snwprintf_s(initPath, MAX_PATH, _TRUNCATE, L"%s1355168291.bnk", bankRootPath);

    HANDLE hFile = CreateFileW(initPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        StmgLog("STMG: Cannot open Init.bnk");
        return;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize < 16)
    {
        CloseHandle(hFile);
        StmgLog("STMG: Init.bnk too small (%u bytes)", fileSize);
        return;
    }

    BYTE* data = (BYTE*)malloc(fileSize);
    if (!data) { CloseHandle(hFile); return; }
    DWORD bytesRead = 0;
    ReadFile(hFile, data, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    if (bytesRead != fileSize) { free(data); return; }

    bool found = false;
    for (DWORD i = 0; i + 8 <= fileSize; i++)
    {
        if (data[i] == 'S' && data[i+1] == 'T' && data[i+2] == 'M' && data[i+3] == 'G')
        {
            DWORD chunkSize = *(DWORD*)(data + i + 4);
            DWORD chunkStart = i + 8;

            StmgLog("STMG: Found at offset 0x%X, size=%u bytes", i, chunkSize);

            if (chunkStart + 6 <= fileSize)
            {
                float volumeThreshold = *(float*)(data + chunkStart);
                WORD maxVoices = *(WORD*)(data + chunkStart + 4);

                StmgLog("STMG: VolumeThreshold=%.2f dB, MaxVoices=%u",
                        volumeThreshold, maxVoices);

                if (volumeThreshold <= -200.0f)
                    StmgLog("STMG WARNING: VolumeThreshold is %.2f dB - effectively MUTED!", volumeThreshold);
                if (maxVoices == 0)
                    StmgLog("STMG WARNING: MaxVoices is 0 - no voices can play!");
            }

            if (chunkStart + 10 <= fileSize)
            {
                DWORD numStateGroups = *(DWORD*)(data + chunkStart + 6);
                StmgLog("STMG: StateGroups=%u", numStateGroups);
            }

            found = true;
            break;
        }
    }

    if (!found)
        StmgLog("STMG WARNING: Chunk not found in Init.bnk!");

    free(data);
}

// ============================================================================
// LoadInitBankPatched — load Init.bnk in-memory with STMG MaxVoices patch
// The original Init.bnk has MaxVoices=1 in its STMG chunk, which limits
// Wwise to a single simultaneous voice.  We read the file, patch the u16
// MaxVoices field to 255, then hand the buffer to Wwise.
// ============================================================================
bool AudioManager::LoadInitBankPatched()
{
    LogAudio(">>> LoadInitBankPatched START");

    const AkUInt32 INIT_FILE_ID = 1355168291u;   // from STID table
    const WORD     NEW_MAX_VOICES = 255;

    const wchar_t* basePath = m_lowLevelIO.GetBasePath();
    wchar_t fullPath[MAX_PATH];
    _snwprintf_s(fullPath, MAX_PATH, _TRUNCATE, L"%s%u.bnk", basePath, INIT_FILE_ID);

    HANDLE hFile = CreateFileW(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        LogAudioError("InitPatch: cannot open Init.bnk (%u.bnk)", INIT_FILE_ID);
        // Fall back to normal LoadBank (MaxVoices=1 is better than nothing)
        return LoadBank("Init", BankCategory_Core);
    }

    LARGE_INTEGER fileSizeLI;
    GetFileSizeEx(hFile, &fileSizeLI);
    DWORD fileSize = (DWORD)fileSizeLI.QuadPart;

    void* pBuf = _aligned_malloc(fileSize, 16);
    if (!pBuf)
    {
        CloseHandle(hFile);
        LogAudioError("InitPatch: alloc failed (%u bytes)", fileSize);
        return LoadBank("Init", BankCategory_Core);
    }

    DWORD bytesRead = 0;
    BOOL readOK = ReadFile(hFile, pBuf, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);

    if (!readOK || bytesRead != fileSize)
    {
        _aligned_free(pBuf);
        LogAudioError("InitPatch: read error (%u/%u)", bytesRead, fileSize);
        return LoadBank("Init", BankCategory_Core);
    }

    // --- Scan for STMG chunk and patch MaxVoices ---
    BYTE* data = (BYTE*)pBuf;
    bool patched = false;
    for (DWORD i = 0; i + 8 <= fileSize; i += 1)
    {
        if (data[i]=='S' && data[i+1]=='T' && data[i+2]=='M' && data[i+3]=='G')
        {
            DWORD chunkSize = *(DWORD*)(data + i + 4);
            DWORD chunkDataStart = i + 8;  // past tag(4) + size(4)

            // STMG layout (bank v34): float VolumeThreshold(4) + u16 MaxVoices(2)
            if (chunkDataStart + 6 <= fileSize)
            {
                WORD* pMaxVoices = (WORD*)(data + chunkDataStart + 4);
                WORD  oldVal = *pMaxVoices;
                *pMaxVoices = NEW_MAX_VOICES;
                patched = true;

                LogAudio("InitPatch: STMG at 0x%X size=%u — MaxVoices patched %u -> %u",
                         i, chunkSize, oldVal, NEW_MAX_VOICES);
            }
            break;
        }
    }

    if (!patched)
        LogAudioError("InitPatch: STMG chunk not found — loading unpatched");

    // --- Load the (patched) buffer ---
    AkBankID bankID = AK_INVALID_BANK_ID;
    AKRESULT res = AK::SoundEngine::LoadBank(pBuf, fileSize, bankID);

    if (res == AK_Success)
    {
        LogAudio("InitPatch: Init bank loaded OK (bankID=%u, MaxVoices=%u)",
                 bankID, (unsigned)NEW_MAX_VOICES);
        LoadedBankInfo info;
        info.name     = "Init";
        info.bankID   = bankID;
        info.category = BankCategory_Core;
        info.refCount = 1;
        m_loadedBanks.push_back(info);
        return true;
    }
    else
    {
        _aligned_free(pBuf);
        LogAudioError("InitPatch: LoadBank failed (err=0x%08X) — falling back to normal", res);
        return LoadBank("Init", BankCategory_Core);
    }
}

void AudioManager::LoadCoreBanks()
{
    // Load Init.bnk with STMG patch (MaxVoices 1→255)
    LoadInitBankPatched();

    // Load core gameplay banks
    LoadBank("BaseCombat", BankCategory_Combat);
    LoadBank("Effects",    BankCategory_Core);
    LoadBank("Ambience",   BankCategory_Ambience);
    LoadBank("Music",      BankCategory_Core);
    LoadBank("UI",         BankCategory_Core);

    // Load creature SFX banks (commonly used)
    LoadBank("SFXBalrog",  BankCategory_Creature);
    LoadBank("SFXTroll",   BankCategory_Creature);
    LoadBank("SFXWarg",    BankCategory_Creature);
    LoadBank("SFXEnt",     BankCategory_Creature);
    LoadBank("SFXOliphant", BankCategory_Creature);
    LoadBank("SFXFellBeast", BankCategory_Creature);
    LoadBank("SFXEagle",   BankCategory_Creature);
    LoadBank("SFXHorse",   BankCategory_Creature);

    // Load the 6 creature banks from Creatures/ folder
    // These contain generic creature sounds (GetUp_Generic, Bodyfall_Roll, etc.)
    LoadBankByID(2713084640u, BankCategory_Creature);  // Creatures bank 1
    LoadBankByID(2798241415u, BankCategory_Creature);  // Creatures bank 2
    LoadBankByID(3452653517u, BankCategory_Creature);  // Creatures bank 3
    LoadBankByID(3567116701u, BankCategory_Creature);  // Creatures bank 4
    LoadBankByID(4222676942u, BankCategory_Creature);  // Creatures bank 5
    LoadBankByID(975722735u,  BankCategory_Creature);  // Creatures bank 6

    LogAudio("Core banks loaded (%d total)", (int)m_loadedBanks.size());
}

void AudioManager::LoadAllBanks()
{
    LogAudio(">>> LoadAllBanks START - Loading ALL banks from STID table with GUI progress");

    // Create progress dialog
    BankLoadProgressDialog progress;
    progress.Show("Loading Audio Banks", s_bankSTIDCount);

    // Init MUST be loaded first
    LoadInitBankPatched();
    progress.UpdateProgress(1, "Init (patched)", IsBankLoaded("Init"));

    // Load all banks from STID table
    int currentBank = 2;
    for (int i = 0; i < s_bankSTIDCount; ++i)
    {
        // Skip Init (already loaded)
        if (wcscmp(s_bankSTID[i].name, L"init") == 0)
            continue;

        // Convert wide string to narrow
        char bankName[256];
        WideCharToMultiByte(CP_ACP, 0, s_bankSTID[i].name, -1, bankName, 256, NULL, NULL);

        // Determine category from name
        BankCategory cat = BankCategory_Core;
        if (strstr(bankName, "hero")) cat = BankCategory_Hero;
        else if (strstr(bankName, "chatter")) cat = BankCategory_Chatter;
        else if (strstr(bankName, "sfx")) cat = BankCategory_Creature;
        else if (strstr(bankName, "level_")) cat = BankCategory_Level;
        else if (strstr(bankName, "vo_")) cat = BankCategory_VO;
        else if (strstr(bankName, "ambience")) cat = BankCategory_Ambience;
        else if (strstr(bankName, "basecombat")) cat = BankCategory_Combat;

        // Load the bank
        bool success = LoadBank(bankName, cat);
        progress.UpdateProgress(currentBank++, bankName, success);

        // Check for cancel
        if (progress.IsCancelled())
        {
            LogAudio("LoadAllBanks CANCELLED by user at bank %d/%d", currentBank, s_bankSTIDCount);
            break;
        }
    }

    // Keep dialog open for 2 seconds to show final results
    Sleep(2000);
    progress.Close();

    LogAudio("LoadAllBanks COMPLETE: %d banks loaded", (int)m_loadedBanks.size());
}

void AudioManager::LoadBanksForCharacter(const char* heroName)
{
    if (!heroName) return;

    // Unload previous character banks if different
    if (!m_currentHero.empty() && m_currentHero != heroName)
        UnloadBanksForCharacter(m_currentHero.c_str());

    m_currentHero = heroName;

    // Load Hero bank (e.g. "HeroAragorn")
    char bankBuf[128];
    sprintf_s(bankBuf, "Hero%s", heroName);
    LoadBank(bankBuf, BankCategory_Hero);

    // Load ChatterHero bank (e.g. "ChatterHeroAragorn")
    sprintf_s(bankBuf, "ChatterHero%s", heroName);
    LoadBank(bankBuf, BankCategory_Chatter);

    LogAudio("Loaded banks for character: %s", heroName);
}

void AudioManager::UnloadBanksForCharacter(const char* heroName)
{
    if (!heroName) return;

    char bankBuf[128];
    sprintf_s(bankBuf, "Hero%s", heroName);
    UnloadBank(bankBuf);

    sprintf_s(bankBuf, "ChatterHero%s", heroName);
    UnloadBank(bankBuf);

    if (m_currentHero == heroName)
        m_currentHero.clear();
}

void AudioManager::LoadBanksForLevel(const char* levelName)
{
    if (!levelName) return;

    if (!m_currentLevel.empty() && m_currentLevel != levelName)
        UnloadBanksForLevel(m_currentLevel.c_str());

    m_currentLevel = levelName;

    // Level bank (e.g. "Level_Moria")
    char bankBuf[128];
    sprintf_s(bankBuf, "Level_%s", levelName);
    LoadBank(bankBuf, BankCategory_Level);

    // VO bank (e.g. "VO_Moria")
    sprintf_s(bankBuf, "VO_%s", levelName);
    LoadBank(bankBuf, BankCategory_VO);

    // Generic chatter banks (always load these for levels)
    LoadBank("ChatterElf",       BankCategory_Chatter);
    LoadBank("ChatterOrc",       BankCategory_Chatter);
    LoadBank("ChatterUruk",      BankCategory_Chatter);
    LoadBank("ChatterGondor",    BankCategory_Chatter);
    LoadBank("ChatterRohan",     BankCategory_Chatter);
    LoadBank("ChatterHobbit",    BankCategory_Chatter);
    LoadBank("ChatterEvilHuman", BankCategory_Chatter);

    LogAudio("Loaded banks for level: %s (%d total)", levelName,
             (int)m_loadedBanks.size());
}

void AudioManager::UnloadBanksForLevel(const char* levelName)
{
    if (!levelName) return;

    char bankBuf[128];
    sprintf_s(bankBuf, "Level_%s", levelName);
    UnloadBank(bankBuf);

    sprintf_s(bankBuf, "VO_%s", levelName);
    UnloadBank(bankBuf);

    UnloadBank("ChatterElf");
    UnloadBank("ChatterOrc");
    UnloadBank("ChatterUruk");
    UnloadBank("ChatterGondor");
    UnloadBank("ChatterRohan");
    UnloadBank("ChatterHobbit");
    UnloadBank("ChatterEvilHuman");

    if (m_currentLevel == levelName)
        m_currentLevel.clear();
}

// ============================================================================
// Event Mapping Lookup (2,817 event->bank entries from AudioHook DLL)
// ============================================================================
void AudioManager::BuildEventLookup()
{
    m_eventLookup.clear();
    for (int i = 0; i < g_EventMappingCount; ++i)
    {
        m_eventLookup[g_EventMappingData[i].id] = i;
    }
    LogAudio("Built event lookup table: %d events mapped", g_EventMappingCount);
}

const EventMappingEntry* AudioManager::LookupEvent(AkUInt32 eventID) const
{
    std::map<AkUInt32, int>::const_iterator it = m_eventLookup.find(eventID);
    if (it != m_eventLookup.end())
        return &g_EventMappingData[it->second];
    return NULL;
}

// ============================================================================
// Event Playback
// ============================================================================
AkPlayingID AudioManager::PlayCue(const char* cueString, AkGameObjectID gameObjID)
{
    if (!m_initialized || !cueString || cueString[0] == '\0')
        return AK_INVALID_PLAYING_ID;

    AkUInt32 eventID = FNV1Hash(cueString);

    // Look up event in mapping table for diagnostics
    const EventMappingEntry* mapping = LookupEvent(eventID);
    if (mapping)
    {
        // Check if required bank is loaded
        if (mapping->bank && !IsBankLoaded(mapping->bank))
        {
            LogAudioError("PlayCue: '%s' (0x%08X) requires bank '%s' which is NOT loaded!",
                         cueString, eventID, mapping->bank);
        }
        const char* displayName = (mapping->name && mapping->name[0]) ? mapping->name : mapping->bank;
        LogAudio("PlayCue: '%s' -> %s (0x%08X) [bank: %s]",
                cueString, displayName, eventID, mapping->bank);
    }
    else
    {
        LogAudio("PlayCue: '%s' (0x%08X) [unmapped event]", cueString, eventID);
    }

    // Use numeric ID-based PostEvent — matches original game's PostEventByID dispatch
    // (string-based PostEvent relies on Wwise GetIDFromString which may not match FNV-1)
    AkPlayingID pid = AK::SoundEngine::PostEvent(eventID, gameObjID);
    LogAudio("PlayCue result: '%s' (0x%08X) -> pid=%u (0=FAILED)", cueString, eventID, pid);
    if (pid == AK_INVALID_PLAYING_ID)
        LogAudioError("PlayCue FAILED: '%s' (0x%08X, obj=%u) bank=%s",
                     cueString, eventID, gameObjID,
                     mapping ? mapping->bank : "unknown");
    return pid;
}

AkPlayingID AudioManager::PostEventByID(AkUniqueID eventID, AkGameObjectID gameObjID)
{
    if (!m_initialized) return AK_INVALID_PLAYING_ID;
    return AK::SoundEngine::PostEvent(eventID, gameObjID);
}

void AudioManager::StopAll(AkGameObjectID gameObjID)
{
    if (m_initialized)
        AK::SoundEngine::StopAll(gameObjID);
}

void AudioManager::StopEvent(AkPlayingID playingID, int fadeDurationMs)
{
    if (!m_initialized || playingID == AK_INVALID_PLAYING_ID) return;
    // SDK v2.1 StopPlayingID takes only 1 arg (no fade parameter)
    (void)fadeDurationMs;
    AK::SoundEngine::StopPlayingID(playingID);
}

// ============================================================================
// Game Objects
// ============================================================================
bool AudioManager::RegisterGameObject(AkGameObjectID id, const char* name)
{
    if (!m_initialized) return false;
    AKRESULT res;
    if (name)
        res = AK::SoundEngine::RegisterGameObj(id, name);
    else
        res = AK::SoundEngine::RegisterGameObj(id);
    if (res != AK_Success)
        LogAudioError("RegisterGameObject failed: id=%u name=%s", id, name ? name : "<null>");
    return res == AK_Success;
}

void AudioManager::UnregisterGameObject(AkGameObjectID id)
{
    if (m_initialized)
        AK::SoundEngine::UnregisterGameObj(id);
}

// ============================================================================
// 3D Positioning
// ============================================================================
void AudioManager::SetPosition3D(AkGameObjectID id, float x, float y, float z,
                                 float frontX, float frontY, float frontZ)
{
    if (!m_initialized) return;
    AkSoundPosition pos;
    pos.Position.X = x;
    pos.Position.Y = y;
    pos.Position.Z = z;
    pos.Orientation.X = frontX;
    pos.Orientation.Y = frontY;
    pos.Orientation.Z = frontZ;
    AK::SoundEngine::SetPosition(id, pos);
}

void AudioManager::UpdateListenerPosition(float x, float y, float z,
                                          float frontX, float frontY, float frontZ,
                                          float topX, float topY, float topZ)
{
    if (!m_initialized) return;
    AkListenerPosition listenerPos;
    listenerPos.Position.X         = x;
    listenerPos.Position.Y         = y;
    listenerPos.Position.Z         = z;
    listenerPos.OrientationFront.X = frontX;
    listenerPos.OrientationFront.Y = frontY;
    listenerPos.OrientationFront.Z = frontZ;
    listenerPos.OrientationTop.X   = topX;
    listenerPos.OrientationTop.Y   = topY;
    listenerPos.OrientationTop.Z   = topZ;
    AK::SoundEngine::SetListenerPosition(listenerPos, 0);
}

// ============================================================================
// Switches / States / RTPC
// ============================================================================
void AudioManager::SetSwitch(const char* group, const char* state,
                             AkGameObjectID gameObjID)
{
    if (!m_initialized || !group || !state) return;
    AK::SoundEngine::SetSwitch(FNV1Hash(group), FNV1Hash(state), gameObjID);
}

void AudioManager::SetSwitchByID(AkSwitchGroupID gid, AkSwitchStateID sid,
                                 AkGameObjectID gameObjID)
{
    if (!m_initialized) return;
    AK::SoundEngine::SetSwitch(gid, sid, gameObjID);
}

void AudioManager::SetState(const char* group, const char* state)
{
    if (!m_initialized || !group || !state) return;
    AK::SoundEngine::SetState(FNV1Hash(group), FNV1Hash(state));
}

void AudioManager::SetStateByID(AkStateGroupID gid, AkStateID sid)
{
    if (!m_initialized) return;
    AK::SoundEngine::SetState(gid, sid);
}

void AudioManager::SetRTPCValue(const char* rtpcName, float value,
                                AkGameObjectID gameObjID)
{
    if (!m_initialized || !rtpcName) return;
    AK::SoundEngine::SetRTPCValue(FNV1Hash(rtpcName),
                                  static_cast<AkRtpcValue>(value), gameObjID);
}

// Switch automation helpers (5 switch groups from documentation)
void AudioManager::SetCreatureSwitch(const char* creature, AkGameObjectID gameObjID)
{
    SetSwitch("creature", creature, gameObjID);
}

void AudioManager::SetWeaponSwitch(const char* weapon, AkGameObjectID gameObjID)
{
    SetSwitch("weapon", weapon, gameObjID);
}

void AudioManager::SetMaterialSwitch(const char* material, AkGameObjectID gameObjID)
{
    SetSwitch("material", material, gameObjID);
}

void AudioManager::SetAbilitySwitch(const char* ability, AkGameObjectID gameObjID)
{
    SetSwitch("ability", ability, gameObjID);
}

void AudioManager::SetPlayerControlled(bool isPlayer, AkGameObjectID gameObjID)
{
    SetSwitch("player_controlled", isPlayer ? "player" : "npc", gameObjID);
}


// ============================================================================
// Music System
// ============================================================================
void AudioManager::PlayMusic(const char* musicEvent)
{
    if (!m_initialized || !musicEvent) return;
    AkUInt32 eventID = FNV1Hash(musicEvent);
    m_currentMusicPlayingID = AK::SoundEngine::PostEvent(eventID, DEFAULT_GAME_OBJECT);
    LogAudio("PlayMusic: '%s' (hash=0x%08X, pid=%u)", musicEvent, eventID,
             m_currentMusicPlayingID);
}

void AudioManager::StopMusic(int fadeDurationMs)
{
    if (!m_initialized) return;
    if (m_currentMusicPlayingID != AK_INVALID_PLAYING_ID)
    {
        (void)fadeDurationMs;
        AK::SoundEngine::StopPlayingID(m_currentMusicPlayingID);
        m_currentMusicPlayingID = AK_INVALID_PLAYING_ID;
    }
}

void AudioManager::SetMusicState(const char* stateGroup, const char* state)
{
    if (!m_initialized || !stateGroup || !state) return;
    AK::SoundEngine::SetState(FNV1Hash(stateGroup), FNV1Hash(state));
    LogAudio("SetMusicState: %s = %s", stateGroup, state);
}

// ============================================================================
// Volume Control (RTPC-based: Bus_Master, Bus_SFX, Bus_Music, Bus_Voice)
// ============================================================================
void AudioManager::SetMasterVolume(float vol01)
{
    if (!m_initialized) return;
    m_masterVol = (vol01 < 0.f) ? 0.f : (vol01 > 1.f) ? 1.f : vol01;
    AK::SoundEngine::SetRTPCValue(
        FNV1Hash("Bus_Master"), static_cast<AkRtpcValue>(m_masterVol * 100.f),
        AK_INVALID_GAME_OBJECT);
}

void AudioManager::SetSFXVolume(float vol01)
{
    if (!m_initialized) return;
    m_sfxVol = (vol01 < 0.f) ? 0.f : (vol01 > 1.f) ? 1.f : vol01;
    AK::SoundEngine::SetRTPCValue(
        FNV1Hash("Bus_SFX"), static_cast<AkRtpcValue>(m_sfxVol * 100.f),
        AK_INVALID_GAME_OBJECT);
}

void AudioManager::SetMusicVolume(float vol01)
{
    if (!m_initialized) return;
    m_musicVol = (vol01 < 0.f) ? 0.f : (vol01 > 1.f) ? 1.f : vol01;
    AK::SoundEngine::SetRTPCValue(
        FNV1Hash("Bus_Music"), static_cast<AkRtpcValue>(m_musicVol * 100.f),
        AK_INVALID_GAME_OBJECT);
}

void AudioManager::SetVoiceVolume(float vol01)
{
    if (!m_initialized) return;
    m_voiceVol = (vol01 < 0.f) ? 0.f : (vol01 > 1.f) ? 1.f : vol01;
    AK::SoundEngine::SetRTPCValue(
        FNV1Hash("Bus_Voice"), static_cast<AkRtpcValue>(m_voiceVol * 100.f),
        AK_INVALID_GAME_OBJECT);
}

// ============================================================================
// VO / Dialogue Queue
// ============================================================================
void AudioManager::QueueVO(const char* cueString, AkGameObjectID gameObjID,
                           float priority)
{
    if (!m_initialized || !cueString || cueString[0] == '\0') return;
    AkUniqueID eventID = FNV1Hash(cueString);
    QueueVOByID(eventID, gameObjID, priority);
}

void AudioManager::QueueVOByID(AkUniqueID eventID, AkGameObjectID gameObjID,
                               float priority)
{
    if (!m_initialized) return;

    // Insert sorted by priority (highest first)
    VOQueueEntry entry;
    entry.eventID   = eventID;
    entry.gameObjID = gameObjID;
    entry.priority  = priority;
    std::deque<VOQueueEntry>::iterator it = m_voQueue.begin();
    while (it != m_voQueue.end() && it->priority >= priority)
        ++it;
    m_voQueue.insert(it, entry);
}

void AudioManager::FlushVOQueue()
{
    m_voQueue.clear();
    if (m_voPlaying && m_currentVOPlayingID != AK_INVALID_PLAYING_ID)
    {
        AK::SoundEngine::StopPlayingID(m_currentVOPlayingID);
        m_currentVOPlayingID = AK_INVALID_PLAYING_ID;
        m_voPlaying = false;
    }
}

void AudioManager::UpdateVOQueue()
{
    if (!m_initialized) return;

    // Check if current VO is still playing
    if (m_voPlaying)
    {
        // Wwise doesn't have a direct "is playing" query in v2.1;
        // we rely on the playing ID remaining valid until the event ends.
        // For now, assume VO events are short and check if queue has higher
        // priority entries waiting. A callback-based approach would be better
        // but requires AK_EndOfEvent callback registration.
        return; // Let current VO finish
    }

    // Play next VO from queue
    if (!m_voQueue.empty())
    {
        VOQueueEntry entry = m_voQueue.front();
        m_voQueue.pop_front();
        m_currentVOPlayingID = AK::SoundEngine::PostEvent(
            entry.eventID, entry.gameObjID);
        m_voPlaying = (m_currentVOPlayingID != AK_INVALID_PLAYING_ID);
    }
}