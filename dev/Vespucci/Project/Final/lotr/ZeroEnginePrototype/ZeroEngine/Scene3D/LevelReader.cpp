// LevelReader.cpp — Cracking Open Pandemic's Sealed Coffins, One Byte at a Time
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// The big parser implementation. Reads PAK and BIN files DIRECTLY from
// the game's Levels/ directory — no external tools, no Rust parser
// dependency (though it was originally ported from lotrc-rust). Handles
// zlib decompression of Block1 and Block2, parses the 472-byte PAK
// header, walks every info table (ModelInfo, TextureInfo, AnimationInfo,
// VBuffInfo, IBuffInfo, EffectInfo, ShapeInfo, HkShapeInfo...), resolves
// CRC→string names from the string table, parses the GameObjs sub-block
// (type definitions + entity instances + field data), and builds clean
// C++ vectors for the rest of the engine to consume.
//
// This file is thousands of lines of binary parsing. Every struct read
// is a memcpy with a size assertion. One byte off and EVERYTHING after
// it is garbage. I have experienced this. Multiple times. At 4 AM.
// The assert messages I wrote at those hours are... colorful.
// -----------------------------------------------------------------------

// Pull in miniz for zlib decompression.
#include "../Engine/wwise/cracking/HashCracking/Tools/vgmstream-master/src/util/miniz.h"

#include "LevelReader.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <stdarg.h>
#include <windows.h>
#include <commctrl.h>

// ============================================================
//  Win32 progress bar for SavePak
// ============================================================
static HWND s_progressWnd = NULL;
static HWND s_progressBar = NULL;
static HWND s_progressLabel = NULL;

static void CreateSaveProgress() {
    if (s_progressWnd) return;
    HINSTANCE hInst = GetModuleHandle(NULL);

    s_progressWnd = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "Saving PAK...",
        WS_POPUP | WS_VISIBLE | WS_BORDER,
        (GetSystemMetrics(SM_CXSCREEN) - 400) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 80) / 2,
        400, 80, NULL, NULL, hInst, NULL);

    s_progressLabel = CreateWindowExA(0, "STATIC", "Initializing...",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 8, 380, 20, s_progressWnd, NULL, hInst, NULL);

    InitCommonControls();
    s_progressBar = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        10, 35, 380, 25, s_progressWnd, NULL, hInst, NULL);

    SendMessage(s_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessage(s_progressBar, PBM_SETPOS, 0, 0);
    UpdateWindow(s_progressWnd);
}

static void UpdateSaveProgress(int percent, const char* text) {
    if (!s_progressWnd) return;
    SendMessage(s_progressBar, PBM_SETPOS, percent, 0);
    SetWindowTextA(s_progressLabel, text);
    // Pump messages so the window repaints
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    UpdateWindow(s_progressWnd);
}

static void CloseSaveProgress() {
    if (s_progressWnd) {
        DestroyWindow(s_progressWnd);
        s_progressWnd = NULL;
        s_progressBar = NULL;
        s_progressLabel = NULL;
    }
}

// ============================================================
//  Level-loader diagnostic log (level_reader.log)
// ============================================================
static FILE* s_lvlLog = NULL;
static void LvlLog(const char* fmt, ...) {
    if (!s_lvlLog) s_lvlLog = fopen("level_reader.log", "w");
    if (!s_lvlLog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_lvlLog, fmt, ap);
    va_end(ap);
    fflush(s_lvlLog);
}

namespace ZeroEngine {

// ============================================================
//  Hash tables (verbatim from types.rs)
// ============================================================

static const uint32_t LOTR_HASHING_ARRAY[256] = {
    0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
    0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
    0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
    0x350c9b64, 0x31cd86d3, 0x3c8ea00a, 0x384fbdbd,
    0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
    0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
    0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
    0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
    0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
    0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5,
    0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
    0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c0b5d,
    0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
    0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
    0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
    0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
    0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
    0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
    0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16,
    0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca,
    0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
    0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02,
    0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066,
    0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
    0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e,
    0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692,
    0x8aad2b2f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
    0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a,
    0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e,
    0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
    0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686,
    0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a,
    0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
    0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb,
    0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f,
    0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
    0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47,
    0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b,
    0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
    0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623,
    0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7,
    0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
    0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f,
    0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3,
    0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
    0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b,
    0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f,
    0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
    0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640,
    0x4e8ee645, 0x4a4ffbf2, 0x470cdd2b, 0x43cdc09c,
    0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
    0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24,
    0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30,
    0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
    0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088,
    0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654,
    0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
    0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c,
    0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18,
    0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
    0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0,
    0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c,
    0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
    0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

// Maps uppercase A-Z (0x41-0x5A) to lowercase a-z (0x61-0x7A); all other bytes unchanged.
static const uint8_t LOTR_INDEX_ARRAY[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
    0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

uint32_t LotrHashString(const char* str, int len) {
    uint32_t h = ~0u;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(str);
    int n = (len < 0) ? static_cast<int>(strlen(str)) : len;
    for (int i = 0; i < n; ++i) {
        h = (h << 8) ^ LOTR_HASHING_ARRAY[LOTR_INDEX_ARRAY[p[i]] ^ (h >> 24)];
    }
    return ~h;
}

// ============================================================
//  Sub-block type classification (from types.rs KEY constants)
// ============================================================

static std::string ClassifySubBlock(uint32_t key, const std::string& name) {
    static const uint32_t KEY_ENGLISH   = LotrHashString("English");
    static const uint32_t KEY_GERMAN    = LotrHashString("German");
    static const uint32_t KEY_FRENCH    = LotrHashString("French");
    static const uint32_t KEY_SPANISH   = LotrHashString("Spanish");
    static const uint32_t KEY_RUSSIAN   = LotrHashString("Russian");
    static const uint32_t KEY_SWEDISH   = LotrHashString("Swedish");
    static const uint32_t KEY_ITALIAN   = LotrHashString("Italian");
    static const uint32_t KEY_POLISH    = LotrHashString("Polish");
    static const uint32_t KEY_NORWEGIAN = LotrHashString("Norwegian");
    static const uint32_t KEY_SPRAY     = LotrHashString("Spray");
    static const uint32_t KEY_CROWD     = LotrHashString("3dCrowd");
    static const uint32_t KEY_PFIELDS   = LotrHashString("PFields");
    static const uint32_t KEY_GAMEOBJS  = LotrHashString("Level");
    static const uint32_t KEY_ATLAS1    = LotrHashString("atlas_1.uv");
    static const uint32_t KEY_ATLAS2    = LotrHashString("atlas_2.uv");

    if (key == KEY_ENGLISH || key == KEY_GERMAN  || key == KEY_FRENCH   ||
        key == KEY_SPANISH || key == KEY_RUSSIAN || key == KEY_SWEDISH  ||
        key == KEY_ITALIAN || key == KEY_POLISH  || key == KEY_NORWEGIAN)
        return "LangStrings";
    if (key == KEY_SPRAY)              return "Spray";
    if (key == KEY_CROWD)              return "Crowd";
    if (key == KEY_PFIELDS)            return "PFields";
    if (key == KEY_GAMEOBJS)           return "GameObjs";
    if (key == KEY_ATLAS1 || key == KEY_ATLAS2) return "AtlasUV";

    // Check by name extension
    size_t ns = name.size();
    if (ns >= 4) {
        std::string ext = name.substr(ns - 4);
        if (ext == ".lua") return "Lua";
        if (ext == ".ssa") return "SSA";
        if (ext == ".csv" || ext == ".txt" || ext == ".dat") return "Data";
    }
    return "Unknown";
}

// ============================================================
//  LevelReader implementation
// ============================================================

void LevelReader::Unload() {
    m_loaded = false;
    m_error.clear();
    m_levelName.clear();
    m_pakRaw.clear();
    m_binRaw.clear();
    m_block1.clear();
    m_block2.clear();
    m_crcMap.clear();
    m_models.clear();
    m_textures.clear();
    m_animations.clear();
    m_effects.clear();
    m_gfxBlocks.clear();
    m_binAssets.clear();
    m_binFlat.clear();
    m_subBlocks1.clear();
    m_subBlocks2.clear();
    m_objas.clear();
    m_obj0s.clear();
    m_animBlocks.clear();
    m_pakVals.clear();
    m_gameObjs.clear();
    m_gameObjTypes.clear();
    m_bufferInfos.clear();
    m_mat1s.clear(); m_mat2s.clear(); m_mat3s.clear(); m_mat4s.clear(); m_matExtras.clear();
    m_shapeInfos.clear(); m_hkShapeInfos.clear(); m_hkConstraintDatas.clear();
    m_vbuffInfos.clear(); m_ibuffInfos.clear(); m_hkConstraintInfos.clear();
    m_pFieldInfos.clear(); m_radiosityInfos.clear(); m_foliageInfos.clear();
    m_pendingObjs.clear();
    m_pendingCollisions.clear();
    m_stringKeysRaw.clear();
    m_parsedData.Clear();
    m_nextGuid = 1;
    m_gameObjsBlock1Offset = 0;
    m_gameObjsBlock1Size = 0;
}

bool LevelReader::ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        m_error = "Cannot open file: " + path;
        return false;
    }
    std::streamsize sz = f.tellg();
    if (sz <= 0) {
        m_error = "Empty or invalid file: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(&out[0]), sz)) {
        m_error = "Read error: " + path;
        return false;
    }
    return true;
}

bool LevelReader::DecompressZlib(const uint8_t* src, uint32_t srcSize,
                                  uint32_t dstSize, std::vector<uint8_t>& out) {
    out.resize(dstSize);
    mz_ulong destLen = dstSize;
    int r = mz_uncompress(&out[0], &destLen, src, srcSize);
    if (r != MZ_OK) {
        char errbuf[64];
        sprintf_s(errbuf, sizeof(errbuf), "zlib decompress failed (code %d)", r);
        m_error = errbuf;
        out.clear();
        return false;
    }
    if (destLen != dstSize) {
        // Size mismatch — still usable but worth noting
        out.resize(static_cast<size_t>(destLen));
    }
    return true;
}

void LevelReader::LoadConquestStrings(const std::string& txtPath) {
    std::ifstream f(txtPath.c_str());
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        // Strip CR if CRLF line endings
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1, 1);
        if (line.empty()) continue;
        uint32_t crc = LotrHashString(line.c_str(), static_cast<int>(line.size()));
        m_crcMap[crc] = line;
    }
}

void LevelReader::ParseStringTable(const uint8_t* strBase, uint32_t bufSize,
                                    uint32_t stringsOffset, uint32_t stringsNum,
                                    const uint8_t* keyBase, uint32_t keysOffset) {
    if (stringsNum == 0) return;

    LvlLog("  ParseStringTable: stringsOffset=%u stringsNum=%u bufSize=%u\n",
           stringsOffset, stringsNum, bufSize);

    // Guard: offset must be within buffer
    if (stringsOffset >= bufSize) {
        LvlLog("  ParseStringTable: SKIP - offset %u beyond buffer size %u\n",
               stringsOffset, bufSize);
        return;
    }

    // Read length-prefixed strings: [u32 length][length bytes] repeated stringsNum times
    const uint8_t* p   = strBase + stringsOffset;
    const uint8_t* end = strBase + bufSize;
    std::vector<std::string> strings;
    strings.reserve(stringsNum);
    for (uint32_t i = 0; i < stringsNum; ++i) {
        if (p + 4 > end) {
            LvlLog("  ParseStringTable: TRUNCATED at entry %u/%u (need 4 bytes for len)\n", i, stringsNum);
            break;
        }
        uint32_t len;
        memcpy(&len, p, 4);
        p += 4;
        if (p + len > end) {
            LvlLog("  ParseStringTable: TRUNCATED at entry %u/%u (need %u bytes for string)\n", i, stringsNum, len);
            break;
        }
        strings.push_back(std::string(reinterpret_cast<const char*>(p), len));
        p += len;
    }
    LvlLog("  ParseStringTable: read %u strings\n", (uint32_t)strings.size());

    // Read string keys: array of stringsNum CRC values
    if (keyBase != NULL && keysOffset != 0) {
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(keyBase + keysOffset);
        for (uint32_t i = 0; i < stringsNum && i < static_cast<uint32_t>(strings.size()); ++i) {
            m_crcMap[keys[i]] = strings[i];
        }
    } else {
        // No explicit key table — compute CRCs from the string names
        for (uint32_t i = 0; i < static_cast<uint32_t>(strings.size()); ++i) {
            uint32_t crc = LotrHashString(strings[i].c_str(), static_cast<int>(strings[i].size()));
            m_crcMap[crc] = strings[i];
        }
    }
}

void LevelReader::ParseGenericB1(uint32_t offset, uint32_t count, uint32_t stride,
                                  uint32_t keyOff, bool hasKey,
                                  uint32_t extraOff1, uint32_t extraOff2,
                                  std::vector<LevelGenericEntry>& out) {
    if (count == 0 || stride == 0 || m_block1.empty()) return;
    // Overflow-safe size check
    if ((uint64_t)offset + (uint64_t)stride * count > (uint64_t)m_block1.size()) {
        LvlLog("  ParseGenericB1: SKIP offset=%u count=%u stride=%u (block1=%u)\n",
               offset, count, stride, (uint32_t)m_block1.size());
        return;
    }
    out.reserve(count);
    char buf[24];
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = &m_block1[0] + offset + i * stride;
        LevelGenericEntry ge;
        ge.extra1 = 0; ge.extra2 = 0;
        if (hasKey && keyOff + 4 <= stride) {
            memcpy(&ge.crc, e + keyOff, 4);
            ge.name = ResolveCrc(ge.crc);
        } else {
            ge.crc = 0;
            sprintf_s(buf, sizeof(buf), "#%04u", i);
            ge.name = buf;
        }
        if (extraOff1 + 4 <= stride) memcpy(&ge.extra1, e + extraOff1, 4);
        if (extraOff2 + 4 <= stride) memcpy(&ge.extra2, e + extraOff2, 4);
        out.push_back(ge);
    }
}

std::string LevelReader::ResolveCrc(uint32_t crc) const {
    std::map<uint32_t, std::string>::const_iterator it = m_crcMap.find(crc);
    if (it != m_crcMap.end()) return it->second;
    char buf[12];
    sprintf_s(buf, sizeof(buf), "0x%08X", crc);
    return buf;
}

bool LevelReader::HasBinAsset(uint32_t assetKey) const {
    return m_binAssets.count(assetKey) != 0;
}

bool LevelReader::GetBinAssetData(uint32_t assetKey, std::vector<uint8_t>& outData) const {
    std::map<uint32_t, BinAssetEntry>::const_iterator it = m_binAssets.find(assetKey);
    if (it == m_binAssets.end()) return false;
    const BinAssetEntry& ae = it->second;

    if (ae.offset + ae.size > m_binRaw.size()) return false;
    const uint8_t* src = &m_binRaw[ae.offset];

    if (ae.size_comp == 0) {
        // Uncompressed
        outData.assign(src, src + ae.size);
    } else {
        // Zlib compressed
        outData.resize(ae.size);
        mz_ulong destLen = ae.size;
        if (mz_uncompress(&outData[0], &destLen, src, ae.size_comp) != MZ_OK)
            return false;
        outData.resize(static_cast<size_t>(destLen));
    }
    return true;
}

// ============================================================
//  BIN parsing
// ============================================================

bool LevelReader::ParseBin(const std::vector<uint8_t>& binData) {
    if (binData.size() < sizeof(BinHeader)) {
        m_error = "BIN too small for header";
        return false;
    }
    const uint8_t* binBase = &binData[0];
    memcpy(&m_binHeader, binBase, sizeof(BinHeader));

    if (m_binHeader.constx06 != 6) {
        char errbuf[64];
        sprintf_s(errbuf, sizeof(errbuf), "BIN header magic mismatch (expected 6, got %u)", m_binHeader.constx06);
        m_error = errbuf;
        return false;
    }

    LvlLog("ParseBin: magic=%u strings_num=%u strings_offset=%u asset_handle_num=%u binSize=%u\n",
           m_binHeader.constx06, m_binHeader.strings_num, m_binHeader.strings_offset,
           m_binHeader.asset_handle_num, (uint32_t)binData.size());

    // Load BIN string table (for CRC resolution) — no explicit key table, CRCs computed from names
    if (m_binHeader.strings_num > 0 && m_binHeader.strings_offset != 0) {
        LvlLog("  Loading BIN string table...\n");
        ParseStringTable(binBase, (uint32_t)binData.size(),
                         m_binHeader.strings_offset,
                         m_binHeader.strings_num,
                         NULL, 0);
        LvlLog("  BIN string table done. crcMap size=%u\n", (uint32_t)m_crcMap.size());
    }

    // Load asset handles
    uint32_t ahNum = m_binHeader.asset_handle_num;
    uint32_t ahOff = m_binHeader.asset_handle_offset;
    if (ahNum > 0 && ahOff + ahNum * sizeof(BinAssetHandle) <= binData.size()) {
        const BinAssetHandle* handles =
            reinterpret_cast<const BinAssetHandle*>(binBase + ahOff);
        for (uint32_t i = 0; i < ahNum; ++i) {
            BinAssetEntry ae;
            ae.key       = handles[i].key;
            ae.offset    = handles[i].offset;
            ae.size      = handles[i].size;
            ae.size_comp = handles[i].size_comp;
            ae.kind      = handles[i].kind;
            m_binAssets[ae.key] = ae;
        }
    }

    return true;
}

// ============================================================
//  Sub-block parsing
// ============================================================

void LevelReader::ParseSubBlocks(const std::vector<uint8_t>& blockData,
                                  uint32_t sbOffset,
                                  std::vector<SubBlockEntry>& out) {
    // SubBlocksHeader = 4 x uint32 = 16 bytes: z0, block_num, z2, z3
    if (blockData.empty() || sbOffset + 16 > static_cast<uint32_t>(blockData.size())) return;

    const uint8_t* data = &blockData[0] + sbOffset;
    uint32_t block_num;
    memcpy(&block_num, data + 4, 4);
    if (block_num == 0 || block_num > 65536) return;

    // SubBlocksBlockHeader entries start right after the 16-byte header
    // Each entry: key(4) + offset(4) + size(4) = 12 bytes
    if (sbOffset + 16 + block_num * 12 > static_cast<uint32_t>(blockData.size())) return;

    for (uint32_t i = 0; i < block_num; ++i) {
        const uint8_t* hdr = data + 16 + i * 12;
        uint32_t key, offset, size;
        memcpy(&key,    hdr + 0, 4);
        memcpy(&offset, hdr + 4, 4);
        memcpy(&size,   hdr + 8, 4);

        uint32_t absOff = sbOffset + offset;

        SubBlockEntry se;
        se.key  = key;
        se.size = size;
        se.name = ResolveCrc(key);
        se.type = ClassifySubBlock(key, se.name);
        se.absOffset = absOff;
        // rawPayload is NOT cached here to save memory on 32-bit builds.
        // SavePak reads directly from m_block1 using absOffset + size.

        out.push_back(se);

        // Parse GameObjs data inline (only from sub_blocks1, key == "Level")
        static const uint32_t KEY_GAMEOBJS_LOCAL = LotrHashString("Level");
        if (key == KEY_GAMEOBJS_LOCAL && m_gameObjs.empty()) {
            if (absOff + 32 <= static_cast<uint32_t>(blockData.size()) &&
                absOff + size <= static_cast<uint32_t>(blockData.size())) {
                m_gameObjsBlock1Offset = absOff;
                m_gameObjsBlock1Size = size;
                ParseGameObjs(&blockData[0] + absOff, size, absOff);
            }
        }
    }
}

// ============================================================
//  GameObjs parsing (level entity placement data)
// ============================================================

// File-scope structs to avoid VS2005 ICE with local struct + std::map
namespace {
    struct GOFieldDef { uint32_t name_crc; uint32_t kind_crc; uint32_t offset; };
    struct GOTypeInfo {
        uint32_t crc;
        uint32_t field_count;
        std::vector<GOFieldDef> fields;
    };
}

void LevelReader::ParseGameObjs(const uint8_t* data, uint32_t dataSize, uint32_t block1AbsOffset) {
    if (!data || dataSize < 32) return;

    // --- Header (32 bytes) ---
    uint32_t magic, types_num, types_off, obj_num, obj_off;
    memcpy(&magic,     data + 0,  4);
    memcpy(&types_num, data + 4,  4);
    memcpy(&types_off, data + 8,  4);
    memcpy(&obj_num,   data + 12, 4);
    memcpy(&obj_off,   data + 16, 4);

    // Magic = 0x4D414704 ("DLTM" / 1296123652)
    if (magic != 1296123652) {
        LvlLog("  GameObjs: bad magic 0x%08X\n", magic);
        return;
    }
    LvlLog("  GameObjs: %u types, %u objects\n", types_num, obj_num);
    if (types_num > 10000 || obj_num > 100000) return; // sanity

    // --- Pre-compute well-known CRCs for field extraction ---
    static const uint32_t CRC_GUID          = LotrHashString("GUID");
    static const uint32_t CRC_PARENTGUID    = LotrHashString("ParentGUID");
    static const uint32_t CRC_GAMEMODEMASK  = LotrHashString("GameModeMask");
    static const uint32_t CRC_NAME          = LotrHashString("Name");
    static const uint32_t CRC_MESH          = LotrHashString("Mesh");
    static const uint32_t CRC_WORLDTRANSFORM= LotrHashString("WorldTransform");
    static const uint32_t CRC_TRANSFORM     = LotrHashString("Transform");
    static const uint32_t CRC_POSITION      = LotrHashString("Position");
    static const uint32_t CRC_ROTATION      = LotrHashString("Rotation");

    // Spline / cinematic camera fields
    static const uint32_t CRC_POSTRACK     = LotrHashString("PositionTrack");
    static const uint32_t CRC_TGTTRACK     = LotrHashString("TargetTrack");
    static const uint32_t CRC_POSTRAVELTIME= LotrHashString("PositionTravelTime");
    static const uint32_t CRC_TGTTRAVELTIME= LotrHashString("TargetTravelTime");
    static const uint32_t CRC_TOTALDURATION= LotrHashString("TotalDuration");
    static const uint32_t CRC_FOV          = LotrHashString("FOV");

    // Event system fields
    static const uint32_t CRC_TARGET        = LotrHashString("target");
    static const uint32_t CRC_OUTPUT        = LotrHashString("Output");
    static const uint32_t CRC_INPUT         = LotrHashString("Input");
    static const uint32_t CRC_DELAY         = LotrHashString("Delay");
    static const uint32_t CRC_STICKY        = LotrHashString("Sticky");
    static const uint32_t CRC_PARAMETER     = LotrHashString("Parameter");

    // GUID array fields
    static const uint32_t CRC_OUTPUTS       = LotrHashString("Outputs");
    static const uint32_t CRC_LAYERS        = LotrHashString("Layers");
    static const uint32_t CRC_NODES         = LotrHashString("Nodes");
    static const uint32_t CRC_INITCHILD     = LotrHashString("InitialChildObjects");
    static const uint32_t CRC_DROPITEMS     = LotrHashString("DropItems");
    static const uint32_t CRC_MODE          = LotrHashString("Mode");
    static const uint32_t CRC_NODE1         = LotrHashString("node1");
    static const uint32_t CRC_NODE2         = LotrHashString("node2");

    // Editor visualization fields
    static const uint32_t CRC_EDTYPE        = LotrHashString("Type");
    static const uint32_t CRC_COLOR         = LotrHashString("Color");
    static const uint32_t CRC_TEXTURE       = LotrHashString("Texture");
    static const uint32_t CRC_OUTER         = LotrHashString("Outer");
    static const uint32_t CRC_SIZE          = LotrHashString("size");

    // Type CRCs for kind identification
    static const uint32_t KIND_INT      = LotrHashString("Int");
    static const uint32_t KIND_FLOAT    = LotrHashString("Float");
    static const uint32_t KIND_BOOL     = LotrHashString("Bool");
    static const uint32_t KIND_CRC      = LotrHashString("CRC");
    static const uint32_t KIND_GUID     = LotrHashString("GUID");
    static const uint32_t KIND_MATRIX   = LotrHashString("Matrix4x4");
    static const uint32_t KIND_VEC3    = LotrHashString("Vector3");
    static const uint32_t KIND_QUAT    = LotrHashString("Quaternion");
    static const uint32_t KIND_ROTATION= LotrHashString("Rotation");
    static const uint32_t KIND_STRING     = LotrHashString("String");
    static const uint32_t KIND_OBJLIST    = LotrHashString("ObjectList");
    static const uint32_t KIND_NODELIST   = LotrHashString("NodeList");
    static const uint32_t KIND_INTLIST    = LotrHashString("IntList");
    static const uint32_t KIND_COLOR     = LotrHashString("Color");
    static const uint32_t KIND_COLOUR    = LotrHashString("Colour");
    (void)KIND_FLOAT; (void)KIND_BOOL; (void)KIND_COLOR; (void)KIND_COLOUR;

    // --- Parse type definitions ---
    // Build a map: type_crc -> vector of (field_name_crc, field_kind_crc, field_offset)
    std::map<uint32_t, GOTypeInfo> typeMap;

    uint32_t off = types_off;
    for (uint32_t t = 0; t < types_num; ++t) {
        if (off + 12 > dataSize) break;
        uint32_t tkey, tcount, tres;
        memcpy(&tkey,   data + off + 0, 4);
        memcpy(&tcount, data + off + 4, 4);
        memcpy(&tres,   data + off + 8, 4);
        off += 12;
        if (tcount > 500) break; // sanity

        GOTypeInfo ti;
        ti.crc = tkey;
        ti.field_count = tcount;
        ti.fields.reserve(tcount);
        for (uint32_t f = 0; f < tcount; ++f) {
            if (off + 12 > dataSize) break;
            GOFieldDef fd;
            memcpy(&fd.name_crc, data + off + 0, 4);
            memcpy(&fd.kind_crc, data + off + 4, 4);
            memcpy(&fd.offset,   data + off + 8, 4);
            ti.fields.push_back(fd);
            off += 12;
        }
        typeMap[tkey] = ti;
    }

    // Build type defs for UI (with full field schemas for property editor)
    m_gameObjTypes.reserve(types_num);
    for (std::map<uint32_t, GOTypeInfo>::iterator it = typeMap.begin(); it != typeMap.end(); ++it) {
        LevelGameObjTypeDef td;
        td.crc = it->first;
        td.name = ResolveCrc(it->first);
        td.field_count = it->second.field_count;
        td.instance_count = 0;
        td.fields.reserve(it->second.fields.size());
        for (size_t fi = 0; fi < it->second.fields.size(); ++fi) {
            GOFieldDefEntry fde;
            fde.name_crc  = it->second.fields[fi].name_crc;
            fde.kind_crc  = it->second.fields[fi].kind_crc;
            fde.offset    = it->second.fields[fi].offset;
            fde.name      = ResolveCrc(fde.name_crc);
            fde.kind_name = ResolveCrc(fde.kind_crc);
            td.fields.push_back(fde);
        }
        m_gameObjTypes.push_back(td);
    }

    // --- Parse objects ---
    m_gameObjs.reserve(obj_num);
    off = obj_off;
    for (uint32_t o = 0; o < obj_num; ++o) {
        if (off + 16 > dataSize) break;

        uint32_t layer, type_key;
        uint16_t obj_size, z3;
        uint32_t z4;
        memcpy(&layer,    data + off + 0,  4);
        memcpy(&type_key, data + off + 4,  4);
        memcpy(&obj_size, data + off + 8,  2);
        memcpy(&z3,       data + off + 10, 2);
        memcpy(&z4,       data + off + 12, 4);
        off += 16;

        if (off + obj_size > dataSize) break;
        const uint8_t* objData = data + off;

        LevelGameObjEntry e;
        e.guid = 0;
        e.parent_guid = 0;
        e.gamemodemask = -1;
        e.name_crc = 0;
        e.type_crc = type_key;
        e.type_name = ResolveCrc(type_key);
        e.layer_guid = layer;
        e.mesh_crc = 0;
        e.has_transform = false;
        e.block1_wt_offset = 0;
        e.block1_tf_offset = 0;
        e.has_world_transform = false;
        e.pos[0] = e.pos[1] = e.pos[2] = 0.0f;
        memset(e.world_transform, 0, sizeof(e.world_transform));
        // identity
        e.world_transform[0] = e.world_transform[5] = e.world_transform[10] = e.world_transform[15] = 1.0f;
        e.target_guid = 0;
        e.delay = 0.0f;
        e.sticky = false;
        e.node1_guid = 0;
        e.node2_guid = 0;
        e.editor_color = 0xFFFFFFFF;
        e.editor_outer = 0.0f;
        e.editor_size[0] = e.editor_size[1] = e.editor_size[2] = 0.0f;
        e.block1_obj_offset = block1AbsOffset + off;
        e.block1_obj_size   = obj_size;
        e.type_def_index    = -1;
        e.position_track_guid = 0;
        e.target_track_guid = 0;
        e.fov = 90.0f;
        e.total_duration = 0.0f;
        e.position_travel_time = 0.0f;
        e.target_travel_time = 0.0f;

        // Look up type definition to extract known fields
        std::map<uint32_t, GOTypeInfo>::iterator tit = typeMap.find(type_key);
        if (tit != typeMap.end()) {
            const std::vector<GOFieldDef>& fields = tit->second.fields;
            for (size_t fi = 0; fi < fields.size(); ++fi) {
                const GOFieldDef& fd = fields[fi];
                if (fd.offset + 4 > obj_size) continue;
                const uint8_t* fptr = objData + fd.offset;

                if (fd.name_crc == CRC_GUID || fd.kind_crc == KIND_GUID) {
                    // First GUID-type field at offset 0 is the object GUID
                    if (fd.name_crc == CRC_GUID || fd.offset == 0) {
                        memcpy(&e.guid, fptr, 4);
                    }
                }
                if (fd.name_crc == CRC_PARENTGUID) {
                    memcpy(&e.parent_guid, fptr, 4);
                }
                if (fd.name_crc == CRC_GAMEMODEMASK && (fd.kind_crc == KIND_INT || fd.kind_crc == CRC_GAMEMODEMASK)) {
                    memcpy(&e.gamemodemask, fptr, 4);
                }
                if (fd.name_crc == CRC_NAME && (fd.kind_crc == KIND_CRC || fd.kind_crc == CRC_NAME)) {
                    memcpy(&e.name_crc, fptr, 4);
                    e.name = ResolveCrc(e.name_crc);
                }
                if (fd.name_crc == CRC_MESH && (fd.kind_crc == KIND_CRC || fd.kind_crc == CRC_MESH)) {
                    memcpy(&e.mesh_crc, fptr, 4);
                    e.mesh_name = ResolveCrc(e.mesh_crc);
                }
                // WorldTransform — the absolute world-space placement matrix.
                // Transform is identical to WorldTransform for most objects (confirmed by JSON extraction).
                if (fd.kind_crc == KIND_MATRIX && fd.offset + 64 <= obj_size) {
                    if (fd.name_crc == CRC_WORLDTRANSFORM) {
                        memcpy(e.world_transform, fptr, 64);
                        e.pos[0] = e.world_transform[12];
                        e.pos[1] = e.world_transform[13];
                        e.pos[2] = e.world_transform[14];
                        e.has_transform = true;
                        e.has_world_transform = true;
                        // Store absolute block1 offset for binary write-back
                        // off points to objData start (after 16-byte header), fd.offset is within objData
                        e.block1_wt_offset = block1AbsOffset + off + fd.offset;
                    }
                    // Always store Transform offset when it's a matrix type named "Transform"
                    if (fd.name_crc == CRC_TRANSFORM) {
                        e.block1_tf_offset = block1AbsOffset + off + fd.offset;
                        if (!e.has_world_transform) {
                            memcpy(e.world_transform, fptr, 64);
                            e.pos[0] = e.world_transform[12];
                            e.pos[1] = e.world_transform[13];
                            e.pos[2] = e.world_transform[14];
                            e.has_transform = true;
                        }
                    }
                }

                // Position field (Vector3) — fallback for objects without WorldTransform
                // (e.g. light_point, light_point_animated, etc.)
                if (fd.name_crc == CRC_POSITION && !e.has_transform && fd.offset + 12 <= obj_size) {
                    float px, py, pz;
                    memcpy(&px, fptr + 0, 4);
                    memcpy(&py, fptr + 4, 4);
                    memcpy(&pz, fptr + 8, 4);
                    // Build identity matrix with this position
                    memset(e.world_transform, 0, sizeof(e.world_transform));
                    e.world_transform[0] = e.world_transform[5] = e.world_transform[10] = e.world_transform[15] = 1.0f;
                    e.world_transform[12] = px;
                    e.world_transform[13] = py;
                    e.world_transform[14] = pz;
                    e.pos[0] = px; e.pos[1] = py; e.pos[2] = pz;
                    e.has_transform = true;
                }
                // Rotation field — apply to existing position-only transform
                if (fd.name_crc == CRC_ROTATION && e.has_transform && !e.has_world_transform && fd.offset + 16 <= obj_size) {
                    // Quaternion (x,y,z,w) → 3x3 rotation matrix
                    float qx, qy, qz, qw;
                    memcpy(&qx, fptr + 0, 4);
                    memcpy(&qy, fptr + 4, 4);
                    memcpy(&qz, fptr + 8, 4);
                    memcpy(&qw, fptr + 12, 4);
                    float xx=qx*qx, yy=qy*qy, zz=qz*qz;
                    float xy=qx*qy, xz=qx*qz, yz=qy*qz;
                    float wx=qw*qx, wy=qw*qy, wz=qw*qz;
                    e.world_transform[0] = 1-2*(yy+zz); e.world_transform[1] = 2*(xy-wz);   e.world_transform[2] = 2*(xz+wy);
                    e.world_transform[4] = 2*(xy+wz);   e.world_transform[5] = 1-2*(xx+zz); e.world_transform[6] = 2*(yz-wx);
                    e.world_transform[8] = 2*(xz-wy);   e.world_transform[9] = 2*(yz+wx);   e.world_transform[10]= 1-2*(xx+yy);
                }

                // --- Event system fields (for Output blocks) ---
                // Match by field name only — kind may vary in binary
                if (fd.name_crc == CRC_TARGET) {
                    memcpy(&e.target_guid, fptr, 4);
                }
                if (fd.name_crc == CRC_OUTPUT && fd.name_crc != CRC_OUTPUTS) {
                    uint32_t crc; memcpy(&crc, fptr, 4);
                    e.output_event = ResolveCrc(crc);
                }
                if (fd.name_crc == CRC_INPUT) {
                    uint32_t crc; memcpy(&crc, fptr, 4);
                    e.input_event = ResolveCrc(crc);
                }
                if (fd.name_crc == CRC_DELAY) {
                    memcpy(&e.delay, fptr, 4);
                }
                if (fd.name_crc == CRC_STICKY) {
                    uint32_t bval; memcpy(&bval, fptr, 4);
                    e.sticky = (bval != 0);
                }
                if (fd.name_crc == CRC_MODE) {
                    uint32_t crc; memcpy(&crc, fptr, 4);
                    e.mode_name = ResolveCrc(crc);
                }
                // PathLink: node1 and node2
                if (fd.name_crc == CRC_NODE1) {
                    memcpy(&e.node1_guid, fptr, 4);
                }
                if (fd.name_crc == CRC_NODE2) {
                    memcpy(&e.node2_guid, fptr, 4);
                }

                // --- Editor visualization fields ---
                if (fd.name_crc == CRC_EDTYPE && fd.kind_crc == KIND_CRC) {
                    uint32_t crc; memcpy(&crc, fptr, 4);
                    e.editor_shape = ResolveCrc(crc);
                }
                if (fd.name_crc == CRC_COLOR && fd.kind_crc == KIND_INT) {
                    memcpy(&e.editor_color, fptr, 4);
                }
                if (fd.name_crc == CRC_TEXTURE && fd.kind_crc == KIND_CRC) {
                    uint32_t crc; memcpy(&crc, fptr, 4);
                    e.editor_texture = ResolveCrc(crc);
                }
                if (fd.name_crc == CRC_OUTER && fd.kind_crc == KIND_FLOAT) {
                    memcpy(&e.editor_outer, fptr, 4);
                }
                // "size" field — can be a float or a vector3 depending on entity type
                if (fd.name_crc == CRC_SIZE) {
                    if (fd.kind_crc == KIND_FLOAT) {
                        memcpy(&e.editor_size[0], fptr, 4);
                        e.editor_size[1] = e.editor_size[2] = e.editor_size[0];
                    } else if (fd.offset + 12 <= obj_size) {
                        memcpy(&e.editor_size[0], fptr, 12); // vec3
                    }
                }

                // --- Cinematic camera fields ---
                if (fd.name_crc == CRC_POSTRACK) { memcpy(&e.position_track_guid, fptr, 4); }
                if (fd.name_crc == CRC_TGTTRACK) { memcpy(&e.target_track_guid, fptr, 4); }
                if (fd.name_crc == CRC_FOV && fd.kind_crc == KIND_FLOAT) { memcpy(&e.fov, fptr, 4); }
                if (fd.name_crc == CRC_TOTALDURATION && fd.kind_crc == KIND_FLOAT) { memcpy(&e.total_duration, fptr, 4); }
                if (fd.name_crc == CRC_POSTRAVELTIME && fd.kind_crc == KIND_FLOAT) { memcpy(&e.position_travel_time, fptr, 4); }
                if (fd.name_crc == CRC_TGTTRAVELTIME && fd.kind_crc == KIND_FLOAT) { memcpy(&e.target_travel_time, fptr, 4); }

                // --- Spline Nodes field (array of float4: x,y,z,arc_length) ---
                if (fd.name_crc == CRC_NODES && fd.kind_crc != KIND_OBJLIST) {
                    // Spline nodes: stored as count(u16) + offset(u16), then count × 16 bytes of float4
                    if (fd.offset + 4 <= obj_size) {
                        uint16_t nodeNum, nodeOff;
                        memcpy(&nodeNum, fptr, 2);
                        memcpy(&nodeOff, fptr + 2, 2);
                        uint32_t nStart = fd.offset + (uint32_t)nodeOff + 4;
                        if (nodeNum > 0 && nodeNum < 10000 && nStart + nodeNum * 16 <= obj_size) {
                            e.spline_nodes.resize(nodeNum);
                            for (uint16_t ni = 0; ni < nodeNum; ++ni) {
                                const uint8_t* np = objData + nStart + ni * 16;
                                memcpy(&e.spline_nodes[ni].x, np + 0, 4);
                                memcpy(&e.spline_nodes[ni].y, np + 4, 4);
                                memcpy(&e.spline_nodes[ni].z, np + 8, 4);
                                memcpy(&e.spline_nodes[ni].s, np + 12, 4);
                            }
                        }
                    }
                }

                // --- GUID array fields (ObjectList / NodeList / any list kind) ---
                // Match by KIND or by known ObjectList field NAME CRC
                {
                    static const uint32_t OLF[] = {
                        0x121F66AF,0x72572773,0xD671CFEB,0x74751F35,0xAB74BDD7,0x3C23AE5E,
                        0x698106D7,0xB43932D8,0x0CB88CDF,0x3988A4DC,0x16388DE4,0xF6AA91FA,
                        0x3C96D3B5,0x68C1B52E,0x951A0BAA,0x85602B9F,0x13234C9E,0x9611E41D,
                        0x073D2568,0x1AD1B6F3,0xD13CC610,0xF331CC93,0xEFDEE547,0xD0E51465,
                        0x3AD5D5D1,0x720159E8,0x250BE3C1,0xF4940CCF,0x7B3B9F58,0x24B867A0,
                        0x50338A7F,0x2C936192,0x39ACE347,0xFFB67B6E,0x1EBBF67F,0x6143CD24,
                        0x5CC81BCD,0xF8D17C44,0x451C73A9,0xDD412AE5,0xF1B955D1
                    };
                    bool isObjList = (fd.kind_crc == KIND_OBJLIST || fd.kind_crc == KIND_NODELIST);
                    if (!isObjList) {
                        for (size_t oc = 0; oc < sizeof(OLF)/sizeof(OLF[0]); ++oc)
                            if (fd.name_crc == OLF[oc]) { isObjList = true; break; }
                    }
                    if (isObjList) {
                    if (fd.offset + 4 <= obj_size) {
                        uint16_t listNum, listOffset;
                        memcpy(&listNum, fptr, 2);
                        memcpy(&listOffset, fptr + 2, 2);
                        uint32_t dataStart = fd.offset + (uint32_t)listOffset + 4;
                        if (listNum > 0 && listNum < 10000 && dataStart + listNum * 4 <= obj_size) {
                            std::vector<uint32_t> arr(listNum);
                            memcpy(&arr[0], objData + dataStart, listNum * 4);

                            // Named fields go to dedicated vectors
                            if (fd.name_crc == CRC_OUTPUTS)        e.outputs = arr;
                            else if (fd.name_crc == CRC_LAYERS)    e.layers = arr;
                            else if (fd.name_crc == CRC_NODES)     e.nodes = arr;
                            else if (fd.name_crc == CRC_INITCHILD) e.initial_child_objects = arr;
                            else if (fd.name_crc == CRC_DROPITEMS) e.drop_items = arr;
                            // ALL objectlist/nodelist fields also go to generic map
                            e.list_refs[fd.name_crc] = arr;
                        }
                    }
                  }
                } // end objectlist block

                // --- Generic GUID field capture ---
                // Match by KIND == GUID OR by known GUID field NAME CRC
                // (many reference fields have kind=Int in binary but hold GUIDs)
                {
                    static const uint32_t GF[] = {
                        0x06A3B1BA,0xD082B6EF,0x0528F11C,0x5889F681,0xC6272F08,0x589899CE,
                        0x08374064,0x1AF366C3,0x841AF57E,0x20A35703,0xAC3094D4,0x12997F1D,
                        0xD03DC290,0xDCE429F6,0x0FAAC06D,0xA302E408,0x4D99476C,0x8742200E,
                        0x769A103E,0x994DEC3D,0x0D4AC74E,0xBDAF028D,0x800C490B,0x4406F0D4,
                        0x1DAC4465,0xFB68E2A9,0x1AE0C78A,0xF7401F41,0xD2FB7BA5,0x9A4CA38F,
                        0x0D685A61,0x61D350F0,0x0F735ABC,0xC0039801,0x9D20CCB4,
                        0xC7771673,0x7D114CB4,0xE5A4CB50,0xBD724F6A,0x2B641E32,
                        0xF9F652CD,0x2CD7B3BB,0xCA4A9EDF,0x5239EA36,0x9F6BE12E,
                        0xDB456B30,0x251B1BDD,0x7BF8C367,0xEE4ACF5D,0x7EE79717,
                        0xC59ACFFA,0x0B9C01F7,0x42FDAAC9,0x0F0F8073,0xB40975E5,
                        0x5B33284F,0x088E79A1,0xFF446425,0x10F35F8F,0x5A5202B7,
                        0x2A0A4753,0xFBF17CC4,0xB693A4AF,0x61D2E2C1,0x0F4F4522,
                        0x4D7351D8,0x4F0512F1,0x42463428,0x41F0C325,0x6CE449B7,
                        0xD6A8B702
                    };
                    bool isRef = (fd.kind_crc == KIND_GUID);
                    if (!isRef && fd.offset + 4 <= obj_size) {
                        for (size_t gc = 0; gc < sizeof(GF)/sizeof(GF[0]); ++gc)
                            if (fd.name_crc == GF[gc]) { isRef = true; break; }
                    }
                    if (isRef && fd.name_crc != CRC_GUID && fd.name_crc != CRC_PARENTGUID &&
                        fd.offset + 4 <= obj_size) {
                        uint32_t refGuid;
                        memcpy(&refGuid, fptr, 4);
                        if (refGuid != 0 && refGuid > 1000)
                            e.guid_refs[fd.name_crc] = refGuid;
                    }
                }

                // --- Generic float/int field capture ---
                // Captures all typed scalars into maps for AtmosphereSetting, light_sun, etc.
                if (fd.kind_crc == KIND_FLOAT && fd.offset + 4 <= obj_size) {
                    float fv; memcpy(&fv, fptr, 4);
                    e.float_fields[fd.name_crc] = fv;
                }
                if ((fd.kind_crc == KIND_INT || fd.kind_crc == KIND_COLOR || fd.kind_crc == KIND_COLOUR) && fd.offset + 4 <= obj_size) {
                    uint32_t iv; memcpy(&iv, fptr, 4);
                    e.int_fields[fd.name_crc] = iv;
                }
            }
        }

        // Fallback: if GUID not found by field name, read raw u32 at offset 0
        if (e.guid == 0 && obj_size >= 4) {
            memcpy(&e.guid, objData, 4);
        }
        // Fallback: ParentGUID at offset 4
        if (e.parent_guid == 0 && obj_size >= 8) {
            memcpy(&e.parent_guid, objData + 4, 4);
        }
        // Fallback: GameModeMask at offset 8
        if (e.gamemodemask == -1 && obj_size >= 12) {
            int32_t rawMask;
            memcpy(&rawMask, objData + 8, 4);
            // Only use if it looks like a valid mask (not random data)
            if (rawMask == -1 || rawMask == 0 || (rawMask > 0 && rawMask < 256))
                e.gamemodemask = rawMask;
        }
        // Fallback: Name CRC at offset 12
        if (e.name_crc == 0 && obj_size >= 16) {
            uint32_t rawName;
            memcpy(&rawName, objData + 12, 4);
            std::string resolved = ResolveCrc(rawName);
            if (resolved.size() > 2 && resolved[0] != '0') {
                e.name_crc = rawName;
                e.name = resolved;
            }
        }

        m_gameObjs.push_back(e);

        // Update instance count in type defs + set type_def_index on entity
        for (size_t ti = 0; ti < m_gameObjTypes.size(); ++ti) {
            if (m_gameObjTypes[ti].crc == type_key) {
                m_gameObjTypes[ti].instance_count++;
                m_gameObjs.back().type_def_index = (int)ti;
                break;
            }
        }

        off += obj_size;
    }

    // --- Resolve local-only transforms using parent hierarchy ---
    // Build GUID → index map for parent lookups
    {
        std::map<uint32_t, size_t> guidIdx;
        for (size_t i = 0; i < m_gameObjs.size(); ++i)
            if (m_gameObjs[i].guid != 0) guidIdx[m_gameObjs[i].guid] = i;

        int resolved = 0;
        for (size_t i = 0; i < m_gameObjs.size(); ++i) {
            LevelGameObjEntry& child = m_gameObjs[i];
            if (!child.has_transform || child.has_world_transform) continue;
            if (child.parent_guid == 0) continue;

            std::map<uint32_t, size_t>::iterator pit = guidIdx.find(child.parent_guid);
            if (pit == guidIdx.end()) continue;
            const LevelGameObjEntry& parent = m_gameObjs[pit->second];
            if (!parent.has_world_transform) continue;

            // world = local * parent_world  (row-major multiply)
            float result[16];
            const float* a = child.world_transform;  // local
            const float* b = parent.world_transform;  // parent world
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    result[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c]
                                  + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
            memcpy(child.world_transform, result, 64);
            child.pos[0] = child.world_transform[12];
            child.pos[1] = child.world_transform[13];
            child.pos[2] = child.world_transform[14];
            child.has_world_transform = true;
            ++resolved;
        }
        if (resolved > 0)
            LvlLog("  Parent-resolve pass: %d local transforms resolved to world\n", resolved);
    }

    // --- Transform source stats ---
    {
        int nWorld=0, nLocal=0, nPos=0, nNone=0;
        for (size_t i = 0; i < m_gameObjs.size(); ++i) {
            const LevelGameObjEntry& o = m_gameObjs[i];
            if (o.has_world_transform) ++nWorld;
            else if (o.has_transform)  ++nLocal;
            else                       ++nNone;
        }
        // Count objects that got transform from Position field (has_transform but not world, no parent)
        for (size_t i = 0; i < m_gameObjs.size(); ++i) {
            const LevelGameObjEntry& o = m_gameObjs[i];
            if (o.has_transform && !o.has_world_transform && o.parent_guid == 0) ++nPos;
        }
        LvlLog("  Transform stats: worldTransform=%d local=%d positionOnly=%d none=%d\n",
               nWorld, nLocal, nPos, nNone);
    }

    // --- Diagnostic: dump type definitions that have Matrix4x4 fields ---
    {
        static const uint32_t CRC_STATIC_OBJ = LotrHashString("static_object");
        for (std::map<uint32_t, GOTypeInfo>::iterator it = typeMap.begin(); it != typeMap.end(); ++it) {
            const GOTypeInfo& ti = it->second;
            bool hasMatrix = false;
            for (size_t f = 0; f < ti.fields.size(); ++f)
                if (ti.fields[f].kind_crc == KIND_MATRIX) { hasMatrix = true; break; }
            if (!hasMatrix) continue;

            LvlLog("  TypeDef 0x%08X '%s' (%u fields):\n", ti.crc, ResolveCrc(ti.crc).c_str(), (unsigned)ti.fields.size());
            for (size_t f = 0; f < ti.fields.size(); ++f) {
                const GOFieldDef& fd = ti.fields[f];
                LvlLog("    [%2u] name=0x%08X '%s' kind=0x%08X '%s' offset=%u\n",
                       (unsigned)f, fd.name_crc, ResolveCrc(fd.name_crc).c_str(),
                       fd.kind_crc, ResolveCrc(fd.kind_crc).c_str(), fd.offset);
            }
        }
    }

    // --- Diagnostic: find objects with NON-ZERO WorldTransform positions ---
    {
        int zeroPos = 0, nonZeroPos = 0, logged = 0;
        for (size_t i = 0; i < m_gameObjs.size(); ++i) {
            const LevelGameObjEntry& o = m_gameObjs[i];
            if (!o.has_world_transform) continue;
            float px = o.world_transform[12], py = o.world_transform[13], pz = o.world_transform[14];
            if (px*px + py*py + pz*pz > 1.0f) {
                ++nonZeroPos;
                if (logged < 10) {
                    const float* m = o.world_transform;
                    LvlLog("  HAS_POS[%d] obj=%u type='%s' mesh='%s' name='%s'\n",
                           logged, (unsigned)i, o.type_name.c_str(), o.mesh_name.c_str(), o.name.c_str());
                    LvlLog("    pos=(%.4f, %.4f, %.4f) rot0=[%.4f,%.4f,%.4f]\n",
                           px, py, pz, m[0], m[1], m[2]);
                    ++logged;
                }
            } else {
                ++zeroPos;
            }
        }
        LvlLog("  Position stats: %d with position, %d at origin, %d logged\n", nonZeroPos, zeroPos, logged);
    }

    LvlLog("  CRC_GUID=0x%08X, CRC_NAME=0x%08X, CRC_OUTPUTS=0x%08X\n", CRC_GUID, CRC_NAME, CRC_OUTPUTS);
    // Count entities with guid==0 and check for specific GUID
    int zeroGuids = 0, withOutputs = 0;
    int found683 = -1;
    for (size_t di = 0; di < m_gameObjs.size(); ++di) {
        if (m_gameObjs[di].guid == 0) zeroGuids++;
        if (!m_gameObjs[di].outputs.empty()) withOutputs++;
        if (m_gameObjs[di].guid == 109024243) found683 = (int)di;
    }
    LvlLog("  GUID stats: %d zero, %d with outputs, entity 109024243 at idx=%d\n", zeroGuids, withOutputs, found683);
    for (size_t di = 0; di < 5 && di < m_gameObjs.size(); ++di)
        LvlLog("  obj[%u] guid=%u name='%s' type='%s' outputs=%u\n",
            (unsigned)di, m_gameObjs[di].guid, m_gameObjs[di].name.c_str(),
            m_gameObjs[di].type_name.c_str(), (unsigned)m_gameObjs[di].outputs.size());
    LvlLog("  GameObjs parsed: %u objects, %u types\n",
           (uint32_t)m_gameObjs.size(), (uint32_t)m_gameObjTypes.size());

    // Diagnostic: dump WorldTransform for first 5 placed static_objects with non-zero position
    {
        int dumped = 0;
        for (size_t di = 0; di < m_gameObjs.size() && dumped < 5; ++di) {
            const LevelGameObjEntry& e = m_gameObjs[di];
            if (e.type_name != "static_object" && e.type_name != "scaled_object") continue;
            if (!e.has_transform) continue;
            if (e.pos[0] == 0.0f && e.pos[1] == 0.0f && e.pos[2] == 0.0f) continue;
            LvlLog("  [DIAG] obj[%u] '%s' mesh='%s' guid=%u pos=(%.3f, %.3f, %.3f) hasWT=%d\n",
                   (unsigned)di, e.name.c_str(), e.mesh_name.c_str(), e.guid,
                   e.pos[0], e.pos[1], e.pos[2], e.has_world_transform ? 1 : 0);
            LvlLog("    WT row0: [%.6f, %.6f, %.6f, %.6f]\n",
                   e.world_transform[0], e.world_transform[1], e.world_transform[2], e.world_transform[3]);
            LvlLog("    WT row1: [%.6f, %.6f, %.6f, %.6f]\n",
                   e.world_transform[4], e.world_transform[5], e.world_transform[6], e.world_transform[7]);
            LvlLog("    WT row2: [%.6f, %.6f, %.6f, %.6f]\n",
                   e.world_transform[8], e.world_transform[9], e.world_transform[10], e.world_transform[11]);
            LvlLog("    WT row3: [%.6f, %.6f, %.6f, %.6f]\n",
                   e.world_transform[12], e.world_transform[13], e.world_transform[14], e.world_transform[15]);
            LvlLog("    b1_wt_off=%u b1_tf_off=%u b1_obj_off=%u obj_size=%u\n",
                   e.block1_wt_offset, e.block1_tf_offset, e.block1_obj_offset, e.block1_obj_size);
            ++dumped;
        }
    }

    // Scan max GUID for entity creation
    m_nextGuid = 1;
    for (size_t gi = 0; gi < m_gameObjs.size(); ++gi) {
        if (m_gameObjs[gi].guid >= m_nextGuid)
            m_nextGuid = m_gameObjs[gi].guid + 1;
    }
    LvlLog("  Next GUID for creation: %u\n", m_nextGuid);
}

// ============================================================
//  PAK parsing
// ============================================================

bool LevelReader::ParsePak(const std::vector<uint8_t>& pakData) {
    if (pakData.size() < sizeof(PakHeader)) {
        m_error = "PAK too small for header";
        return false;
    }
    const uint8_t* pakBase = &pakData[0];
    memcpy(&m_pakHeader, pakBase, sizeof(PakHeader));

    // Basic sanity check
    if (m_pakHeader.constx13 != 0x13) {
        char errbuf[64];
        sprintf_s(errbuf, sizeof(errbuf), "PAK header magic mismatch (expected 0x13, got 0x%X)", m_pakHeader.constx13);
        m_error = errbuf;
        return false;
    }

    // Decompress Block1
    {
        uint32_t off  = m_pakHeader.block1_offset;
        uint32_t sz   = m_pakHeader.block1_size;
        uint32_t szc  = m_pakHeader.block1_size_comp;

        if (off == 0 || sz == 0) {
            m_error = "PAK has no Block1";
            return false;
        }
        if (off + (szc ? szc : sz) > pakData.size()) {
            m_error = "PAK Block1 extends beyond file";
            return false;
        }

        if (szc == 0) {
            // Not compressed — copy directly
            m_block1.assign(pakBase + off, pakBase + off + sz);
        } else {
            if (!DecompressZlib(pakBase + off, szc, sz, m_block1))
                return false;
        }
    }

    // Decompress Block2 (may be empty on some levels)
    {
        uint32_t off  = m_pakHeader.block2_offset;
        uint32_t sz   = m_pakHeader.block2_size;
        uint32_t szc  = m_pakHeader.block2_size_comp;

        if (off != 0 && sz > 0) {
            if (off + (szc ? szc : sz) <= pakData.size()) {
                if (szc == 0) {
                    m_block2.assign(pakBase + off, pakBase + off + sz);
                } else {
                    DecompressZlib(pakBase + off, szc, sz, m_block2);
                    // Block2 failure is non-fatal (animations still listed)
                }
            }
        }
    }

    LvlLog("ParsePak: magic=0x%X block1_off=%u block1_sz=%u block1_szc=%u\n",
           m_pakHeader.constx13, m_pakHeader.block1_offset,
           m_pakHeader.block1_size, m_pakHeader.block1_size_comp);
    LvlLog("  block1 decompressed: size=%u\n", (uint32_t)m_block1.size());
    LvlLog("  strings_num=%u strings_offset=%u pakSize=%u\n",
           m_pakHeader.strings_num, m_pakHeader.strings_offset, (uint32_t)pakData.size());
    LvlLog("  model_info_num=%u texture_info_num=%u animation_info_num=%u effect_info_num=%u gfx_block_num=%u\n",
           m_pakHeader.model_info_num, m_pakHeader.texture_info_num,
           m_pakHeader.animation_info_num, m_pakHeader.effect_info_num, m_pakHeader.gfx_block_info_num);
    LvlLog("  obja_num=%u obj0_num=%u anim_block_num=%u block_a_num=%u\n",
           m_pakHeader.obja_num, m_pakHeader.obj0_num,
           m_pakHeader.animation_block_info_num, m_pakHeader.block_a_num);

    // Load PAK string table — strings are in raw PAK at strings_offset.
    // Compute CRCs from names (LotrHashString) — valid for all game asset names.
    // Note: string_keys_offset in block1 does NOT span all stringsNum entries,
    // so reading it directly would overrun block1.
    if (m_pakHeader.strings_num > 0 && m_pakHeader.strings_offset != 0) {
        LvlLog("  Loading PAK string table...\n");
        ParseStringTable(pakBase, (uint32_t)pakData.size(),
                         m_pakHeader.strings_offset,
                         m_pakHeader.strings_num,
                         NULL, 0);
        LvlLog("  PAK string table done. crcMap size=%u\n", (uint32_t)m_crcMap.size());
    }

    // ---- Parse asset tables from Block1 ----
    LvlLog("  --- parsing asset tables ---\n");

    // Models
    LvlLog("  Models: num=%u offset=%u\n", m_pakHeader.model_info_num, m_pakHeader.model_info_offset);
    {
        const ModelInfo* arr = B1Array<ModelInfo>(
            m_pakHeader.model_info_offset, m_pakHeader.model_info_num);
        m_models.reserve(m_pakHeader.model_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.model_info_num; ++i) {
            const ModelInfo& mi = arr[i];
            LevelModelEntry e;
            e.crc          = mi.key;
            e.name         = ResolveCrc(mi.key);
            e.asset_key    = mi.asset_key;
            e.bbox_center[0] = mi.bounding_box.center[0];
            e.bbox_center[1] = mi.bounding_box.center[1];
            e.bbox_center[2] = mi.bounding_box.center[2];
            e.bbox_half_width[0] = mi.bounding_box.half_width[0];
            e.bbox_half_width[1] = mi.bounding_box.half_width[1];
            e.bbox_half_width[2] = mi.bounding_box.half_width[2];
            e.mat_num      = mi.mat_num;
            e.bones_num    = mi.bones_num;
            e.vbuff_num    = mi.vbuff_num;
            e.ibuff_num    = mi.ibuff_num;
            e.gamemodemask = mi.gamemodemask;
            m_models.push_back(e);
        }
    }
    LvlLog("  Models done: %u loaded\n", (uint32_t)m_models.size());

    // Textures
    LvlLog("  Textures: num=%u offset=%u\n", m_pakHeader.texture_info_num, m_pakHeader.texture_info_offset);
    {
        const TextureInfo* arr = B1Array<TextureInfo>(
            m_pakHeader.texture_info_offset, m_pakHeader.texture_info_num);
        m_textures.reserve(m_pakHeader.texture_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.texture_info_num; ++i) {
            const TextureInfo& ti = arr[i];
            LevelTextureEntry e;
            e.crc          = ti.key;
            e.name         = ResolveCrc(ti.key);
            e.asset_key    = ti.asset_key;
            e.width        = ti.width;
            e.height       = ti.height;
            e.depth        = ti.depth;
            e.levels       = ti.levels;
            e.format       = ti.format;
            e.kind         = ti.kind;
            e.gamemodemask = ti.gamemodemask;
            m_textures.push_back(e);
        }
    }
    LvlLog("  Textures done: %u loaded\n", (uint32_t)m_textures.size());

    // Animations — read from animation_info (931 entries), not animation_block_info (6 entries)
    LvlLog("  Animations: num=%u offset=%u\n", m_pakHeader.animation_info_num, m_pakHeader.animation_info_offset);
    {
        const AnimationInfo* arr = B1Array<AnimationInfo>(
            m_pakHeader.animation_info_offset, m_pakHeader.animation_info_num);
        m_animations.reserve(m_pakHeader.animation_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.animation_info_num; ++i) {
            const AnimationInfo& ai = arr[i];
            LevelAnimEntry e;
            e.crc          = ai.key;
            e.name         = ResolveCrc(ai.key);
            e.guid         = ai.kind;
            e.key_name_crc = ai.bones_num1;
            e.key_name     = "";
            e.size         = ai.size;
            e.size_comp    = 0;
            e.offset       = ai.offset;
            e.gamemodemask = ai.gamemodemask;
            m_animations.push_back(e);
        }
    }
    LvlLog("  Animations done: %u loaded\n", (uint32_t)m_animations.size());

    // Effects
    LvlLog("  Effects: num=%u offset=%u\n", m_pakHeader.effect_info_num, m_pakHeader.effect_info_offset);
    {
        const EffectInfo* arr = B1Array<EffectInfo>(
            m_pakHeader.effect_info_offset, m_pakHeader.effect_info_num);
        m_effects.reserve(m_pakHeader.effect_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.effect_info_num; ++i) {
            const EffectInfo& fi = arr[i];
            LevelEffectEntry e;
            e.crc           = fi.key;
            e.name          = ResolveCrc(fi.key);
            e.offset        = fi.offset;
            e.size          = fi.size;
            e.gamemodemask  = fi.gamemodemask;
            m_effects.push_back(e);
        }
    }
    LvlLog("  Effects done: %u loaded\n", (uint32_t)m_effects.size());

    // GFX Blocks
    LvlLog("  GFX blocks: num=%u offset=%u\n", m_pakHeader.gfx_block_info_num, m_pakHeader.gfx_block_info_offset);
    {
        const GFXBlockInfo* arr = B1Array<GFXBlockInfo>(
            m_pakHeader.gfx_block_info_offset, m_pakHeader.gfx_block_info_num);
        m_gfxBlocks.reserve(m_pakHeader.gfx_block_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.gfx_block_info_num; ++i) {
            const GFXBlockInfo& gi = arr[i];
            LevelGfxBlockEntry e;
            e.crc    = gi.key;
            e.name   = ResolveCrc(gi.key);
            e.offset = gi.offset;
            e.size   = gi.size;
            m_gfxBlocks.push_back(e);
        }
    }
    LvlLog("  GFX blocks done: %u loaded\n", (uint32_t)m_gfxBlocks.size());

    // Sub-blocks (Lua scripts, GameObjs, LangStrings, Data, etc.)
    LvlLog("  SubBlocks1: offset=%u\n", m_pakHeader.sub_blocks1_offset);
    if (m_pakHeader.sub_blocks1_offset > 0)
        ParseSubBlocks(m_block1, m_pakHeader.sub_blocks1_offset, m_subBlocks1);
    LvlLog("  SubBlocks1 done: %u\n", (uint32_t)m_subBlocks1.size());

    // string_keys read directly from m_block1 during save (no caching)
    LvlLog("  StringKeys at offset %u\n", m_pakHeader.string_keys_offset);
    LvlLog("  SubBlocks2: offset=%u block2size=%u\n", m_pakHeader.sub_blocks2_offset, (uint32_t)m_block2.size());
    if (!m_block2.empty())
        ParseSubBlocks(m_block2, m_pakHeader.sub_blocks2_offset, m_subBlocks2);
    LvlLog("  SubBlocks2 done: %u\n", (uint32_t)m_subBlocks2.size());

    // ObjAs (compressed object/model data blocks) — in block1
    LvlLog("  ObjAs: num=%u offset=%u\n", m_pakHeader.obja_num, m_pakHeader.obja_offset);
    {
        const ObjA* arr = B1Array<ObjA>(m_pakHeader.obja_offset, m_pakHeader.obja_num);
        m_objas.reserve(m_pakHeader.obja_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.obja_num; ++i) {
            LevelObjAEntry e;
            e.crc       = arr[i].key;
            e.name      = ResolveCrc(arr[i].key);
            e.size      = arr[i].size;
            e.size_comp = arr[i].size_comp;
            e.kind      = arr[i].kind;
            m_objas.push_back(e);
        }
    }
    LvlLog("  ObjAs done: %u loaded\n", (uint32_t)m_objas.size());

    // Obj0s (asset type references) — in block1
    LvlLog("  Obj0s: num=%u offset=%u\n", m_pakHeader.obj0_num, m_pakHeader.obj0_offset);
    {
        const Obj0* arr = B1Array<Obj0>(m_pakHeader.obj0_offset, m_pakHeader.obj0_num);
        m_obj0s.reserve(m_pakHeader.obj0_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.obj0_num; ++i) {
            LevelObj0Entry e;
            e.type_code = arr[i].unk_0;
            e.crc       = arr[i].key;
            e.name      = ResolveCrc(arr[i].key);
            m_obj0s.push_back(e);
        }
    }
    LvlLog("  Obj0s done: %u loaded\n", (uint32_t)m_obj0s.size());

    // Animation blocks (per-game-mode compressed animation data) — in block1
    LvlLog("  AnimBlocks: num=%u offset=%u\n", m_pakHeader.animation_block_info_num, m_pakHeader.animation_block_info_offset);
    {
        const AnimationBlockInfo* arr = B1Array<AnimationBlockInfo>(
            m_pakHeader.animation_block_info_offset, m_pakHeader.animation_block_info_num);
        m_animBlocks.reserve(m_pakHeader.animation_block_info_num);
        for (uint32_t i = 0; arr && i < m_pakHeader.animation_block_info_num; ++i) {
            LevelAnimBlockEntry e;
            e.crc       = arr[i].key;
            e.name      = ResolveCrc(arr[i].key);
            e.guid      = arr[i].guid;
            e.key_name  = ResolveCrc(arr[i].key_name);
            e.size      = arr[i].size;
            e.size_comp = arr[i].size_comp;
            m_animBlocks.push_back(e);
        }
    }
    LvlLog("  AnimBlocks done: %u loaded\n", (uint32_t)m_animBlocks.size());

    // Pak vals (block_a — per-asset PAK values) — in raw PAK at absolute block_a_offset
    LvlLog("  PakVals: num=%u offset=%u\n", m_pakHeader.block_a_num, m_pakHeader.block_a_offset);
    if (m_pakHeader.block_a_num > 0 && m_pakHeader.block_a_offset != 0) {
        const uint8_t* pakBase = &pakData[0];
        uint32_t bvSize = m_pakHeader.block_a_num * static_cast<uint32_t>(sizeof(BlockAVal));
        if (m_pakHeader.block_a_offset + bvSize <= static_cast<uint32_t>(pakData.size())) {
            const BlockAVal* arr = reinterpret_cast<const BlockAVal*>(pakBase + m_pakHeader.block_a_offset);
            m_pakVals.reserve(m_pakHeader.block_a_num);
            for (uint32_t i = 0; i < m_pakHeader.block_a_num; ++i) {
                LevelPakValEntry e;
                e.crc          = arr[i].key;
                e.name         = ResolveCrc(arr[i].key);
                e.gamemodemask = arr[i].gamemodemask;
                m_pakVals.push_back(e);
            }
        }
    }
    LvlLog("  PakVals done: %u loaded\n", (uint32_t)m_pakVals.size());

    // ---- Remaining PAK block1 types (generic stride reader) ----
    // BufferInfo: no key, extra1=vbuff_info_offset@0, extra2=kind@(stride-4)
    LvlLog("  BufferInfos: num=%u stride=%u offset=%u\n",
           m_pakHeader.buffer_info_num, m_pakHeader.buffer_info_size, m_pakHeader.buffer_info_offset);
    ParseGenericB1(m_pakHeader.buffer_info_offset, m_pakHeader.buffer_info_num,
                   m_pakHeader.buffer_info_size, 0, false, 0, 4, m_bufferInfos);
    LvlLog("  BufferInfos done: %u\n", (uint32_t)m_bufferInfos.size());

    // Mat1: key_guid at offset 32 (after unk0,unk1,tex0-tex5)
    LvlLog("  Mat1s: num=%u stride=%u\n", m_pakHeader.mat1_num, m_pakHeader.mat1_size);
    ParseGenericB1(m_pakHeader.mat1_offset, m_pakHeader.mat1_num,
                   m_pakHeader.mat1_size, 32, true, 8, 12, m_mat1s);
    LvlLog("  Mat1s done: %u\n", (uint32_t)m_mat1s.size());

    LvlLog("  Mat2s: num=%u\n", m_pakHeader.mat2_num);
    ParseGenericB1(m_pakHeader.mat2_offset, m_pakHeader.mat2_num,
                   m_pakHeader.mat2_size, 32, true, 8, 12, m_mat2s);

    LvlLog("  Mat3s: num=%u\n", m_pakHeader.mat3_num);
    ParseGenericB1(m_pakHeader.mat3_offset, m_pakHeader.mat3_num,
                   m_pakHeader.mat3_size, 32, true, 8, 12, m_mat3s);

    LvlLog("  Mat4s: num=%u\n", m_pakHeader.mat4_num);
    ParseGenericB1(m_pakHeader.mat4_offset, m_pakHeader.mat4_num,
                   m_pakHeader.mat4_size, 32, true, 8, 12, m_mat4s);

    // MatExtra: no key (pure raw data)
    LvlLog("  MatExtras: num=%u stride=%u\n", m_pakHeader.mat_extra_num, m_pakHeader.mat_extra_size);
    ParseGenericB1(m_pakHeader.mat_extra_offset, m_pakHeader.mat_extra_num,
                   m_pakHeader.mat_extra_size, 0, false, 0, 4, m_matExtras);
    LvlLog("  MatExtras done: %u\n", (uint32_t)m_matExtras.size());

    // ShapeInfo: no key, extra1=offset@0, extra2=kind@4
    LvlLog("  ShapeInfos: num=%u stride=%u offset=%u\n",
           m_pakHeader.shape_info_num, m_pakHeader.shape_info_size, m_pakHeader.shape_info_offset);
    ParseGenericB1(m_pakHeader.shape_info_offset, m_pakHeader.shape_info_num,
                   m_pakHeader.shape_info_size, 0, false, 0, 4, m_shapeInfos);
    LvlLog("  ShapeInfos done: %u\n", (uint32_t)m_shapeInfos.size());

    // HkShapeInfo: no key, kind at offset 32 (after 2x Vector4), a_num at 36
    LvlLog("  HkShapeInfos: num=%u stride=%u\n", m_pakHeader.hk_shape_info_num, m_pakHeader.hk_shape_info_size);
    ParseGenericB1(m_pakHeader.hk_shape_info_offset, m_pakHeader.hk_shape_info_num,
                   m_pakHeader.hk_shape_info_size, 0, false, 32, 36, m_hkShapeInfos);
    LvlLog("  HkShapeInfos done: %u\n", (uint32_t)m_hkShapeInfos.size());

    // HkConstraintData: no key, extra1=kind@0, extra2=unk_1@4
    LvlLog("  HkConstraintDatas: num=%u stride=%u\n", m_pakHeader.hk_constraint_data_num, m_pakHeader.hk_constraint_data_size);
    ParseGenericB1(m_pakHeader.hk_constraint_data_offset, m_pakHeader.hk_constraint_data_num,
                   m_pakHeader.hk_constraint_data_size, 0, false, 0, 4, m_hkConstraintDatas);
    LvlLog("  HkConstraintDatas done: %u\n", (uint32_t)m_hkConstraintDatas.size());

    // VBuffInfo: no key, extra1=size@4, extra2=offset@12
    LvlLog("  VBuffInfos: num=%u stride=%u\n", m_pakHeader.vbuff_info_num, m_pakHeader.vbuff_info_size);
    ParseGenericB1(m_pakHeader.vbuff_info_offset, m_pakHeader.vbuff_info_num,
                   m_pakHeader.vbuff_info_size, 0, false, 4, 12, m_vbuffInfos);
    LvlLog("  VBuffInfos done: %u\n", (uint32_t)m_vbuffInfos.size());

    // IBuffInfo: no key, extra1=size@4, extra2=format@8
    LvlLog("  IBuffInfos: num=%u stride=%u\n", m_pakHeader.ibuff_info_num, m_pakHeader.ibuff_info_size);
    ParseGenericB1(m_pakHeader.ibuff_info_offset, m_pakHeader.ibuff_info_num,
                   m_pakHeader.ibuff_info_size, 0, false, 4, 8, m_ibuffInfos);
    LvlLog("  IBuffInfos done: %u\n", (uint32_t)m_ibuffInfos.size());

    // HkConstraintInfo: no key, extra1=kind@0, extra2=bone_parents_num@8
    LvlLog("  HkConstraintInfos: num=%u stride=%u\n", m_pakHeader.hk_constraint_info_num, m_pakHeader.hk_constraint_info_size);
    ParseGenericB1(m_pakHeader.hk_constraint_info_offset, m_pakHeader.hk_constraint_info_num,
                   m_pakHeader.hk_constraint_info_size, 0, false, 0, 8, m_hkConstraintInfos);
    LvlLog("  HkConstraintInfos done: %u\n", (uint32_t)m_hkConstraintInfos.size());

    // PFieldInfo: link_guid@0 (maybe CRC), extra1=gamemode_guid@4, extra2=width@8
    LvlLog("  PFieldInfos: num=%u stride=%u\n", m_pakHeader.pfield_info_num, m_pakHeader.pfield_info_size);
    ParseGenericB1(m_pakHeader.pfield_info_offset, m_pakHeader.pfield_info_num,
                   m_pakHeader.pfield_info_size, 0, true, 4, 8, m_pFieldInfos);
    LvlLog("  PFieldInfos done: %u\n", (uint32_t)m_pFieldInfos.size());

    // RadiosityValsInfo: guid@0 (CRC), extra1=num@4, extra2=offset@8
    LvlLog("  RadiosityInfos: num=%u stride=%u offset=%u\n",
           m_pakHeader.radiosity_vals_info_num, m_pakHeader.radiosity_vals_info_size,
           m_pakHeader.radiosity_vals_info_offset);
    ParseGenericB1(m_pakHeader.radiosity_vals_info_offset, m_pakHeader.radiosity_vals_info_num,
                   m_pakHeader.radiosity_vals_info_size, 0, true, 4, 8, m_radiosityInfos);
    LvlLog("  RadiosityInfos done: %u\n", (uint32_t)m_radiosityInfos.size());

    // FoliageInfo: key@0 (CRC), extra1=kind@4
    LvlLog("  FoliageInfos: num=%u stride=%u\n", m_pakHeader.foliage_info_num, m_pakHeader.foliage_info_size);
    ParseGenericB1(m_pakHeader.foliage_info_offset, m_pakHeader.foliage_info_num,
                   m_pakHeader.foliage_info_size, 0, true, 4, 8, m_foliageInfos);
    LvlLog("  FoliageInfos done: %u\n", (uint32_t)m_foliageInfos.size());

    // --- Phase 1: Parse variable-length data from Block1 ---
    LvlLog("  Phase 1: parsing variable-length data...\n");
    {
        std::string parseErr;
        if (!ParseBlock1Data(&m_block1[0], (uint32_t)m_block1.size(),
                             m_pakHeader, m_parsedData, parseErr)) {
            LvlLog("  Phase 1 parse FAILED: %s\n", parseErr.c_str());
        } else {
            LvlLog("  Phase 1 done: radiosityVals=%u foliage=%u gfxBlock=%u effect=%u\n",
                   (uint32_t)m_parsedData.radiosityVals.size(),
                   (uint32_t)m_parsedData.foliageData.size(),
                   (uint32_t)m_parsedData.gfxBlockData.size(),
                   (uint32_t)m_parsedData.effectData.size());
        }
    }

    // --- Phase 2: Parse buffer infrastructure (VBuffInfo, IBuffInfo, block2_offsets) ---
    LvlLog("  Phase 2: parsing buffer infrastructure...\n");
    {
        std::string parseErr;
        const uint8_t* b2ptr = m_block2.empty() ? NULL : &m_block2[0];
        uint32_t b2size = (uint32_t)m_block2.size();
        if (!ParseBlock1Phase2(&m_block1[0], (uint32_t)m_block1.size(),
                               b2ptr, b2size,
                               m_pakHeader, m_parsedData, parseErr)) {
            LvlLog("  Phase 2 parse FAILED: %s\n", parseErr.c_str());
        } else {
            LvlLog("  Phase 2 done: vbuffMap=%u ibuffMap=%u block2Offsets=%u\n",
                   (uint32_t)m_parsedData.vbuffInfoMap.size(),
                   (uint32_t)m_parsedData.ibuffInfoMap.size(),
                   (uint32_t)m_parsedData.block2Offsets.size());
        }
    }

    // --- Phase 3: Parse Shape data + all raw info arrays ---
    LvlLog("  Phase 3: parsing shapes + raw info arrays...\n");
    {
        std::string parseErr;
        if (!ParseBlock1Phase3(&m_block1[0], (uint32_t)m_block1.size(),
                               m_pakHeader, m_parsedData, parseErr)) {
            LvlLog("  Phase 3 parse FAILED: %s\n", parseErr.c_str());
        } else {
            uint32_t shapeCount = 0;
            for (uint32_t i = 0; i < (uint32_t)m_parsedData.shapeData.size(); ++i)
                if (!m_parsedData.shapeData[i].empty()) shapeCount++;
            LvlLog("  Phase 3 done: shapes=%u (kind=0), rawArrays=21\n", shapeCount);
        }
    }

    // --- Phase 4: Parse Model, HkShape, HkConstraint variable-length data ---
    LvlLog("  Phase 4: parsing model/hkshape/hkconstraint data...\n");
    {
        std::string parseErr;
        if (!ParseBlock1Phase4(&m_block1[0], (uint32_t)m_block1.size(),
                               m_pakHeader, m_parsedData, parseErr)) {
            LvlLog("  Phase 4 parse FAILED: %s\n", parseErr.c_str());
        } else {
            uint32_t hkCount = 0, conCount = 0, mdlCount = 0;
            for (uint32_t i = 0; i < (uint32_t)m_parsedData.hkShapeData.size(); ++i)
                if (!m_parsedData.hkShapeData[i].empty()) hkCount++;
            for (uint32_t i = 0; i < (uint32_t)m_parsedData.hkConstraintData.size(); ++i)
                if (!m_parsedData.hkConstraintData[i].empty()) conCount++;
            for (uint32_t i = 0; i < (uint32_t)m_parsedData.modelData.size(); ++i)
                if (!m_parsedData.modelData[i].boneKeys.empty()) mdlCount++;
            LvlLog("  Phase 4 done: hkShapes=%u hkConstraints=%u models=%u\n",
                   hkCount, conCount, mdlCount);
        }
    }

    LvlLog("ParsePak: returning true\n");

    return true;
}

// ============================================================
//  Public Load entry point
// ============================================================

bool LevelReader::Load(const std::string& pakPath, const std::string& binPath) {
    // Reopen log fresh each Load() call
    // Don't reset log file — append to keep SavePak progress visible
    if (!s_lvlLog) s_lvlLog = fopen("level_reader.log", "w");
    else { /* keep existing log open */ }
    LvlLog("Load() called: PAK=%s BIN=%s\n", pakPath.c_str(), binPath.c_str());
    Unload();

    m_pakPath  = pakPath;
    m_binPath  = binPath;

    // Extract level name from PAK path (e.g. "Training" from "Levels/Training.PAK")
    {
        std::string base = pakPath;
        size_t slash = base.find_last_of("/\\");
        if (slash != std::string::npos) base = base.substr(slash + 1);
        size_t dot = base.rfind('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        m_levelName = base;
    }

    // Load conquest_strings.txt — prefer the persistent base path set by SetBaseStringsPath(),
    // fall back to cwd-relative candidates only if base path isn't set.
    if (!m_baseStringsPath.empty()) {
        LoadConquestStrings(m_baseStringsPath);
    } else {
        const char* candidates[] = {
            "conquest_strings.txt",
            "../conquest_strings.txt",
            "../GameFiles/lotrcparser/CoreScripts/lotrc-rust/lotrc/res/conquest_strings.txt",
            "../GameFiles/lotrc/lotrc-0.6.0/lotrc-0.6.0/lotrc/res/conquest_strings.txt",
            NULL
        };
        for (int i = 0; candidates[i]; ++i) {
            std::ifstream probe(candidates[i]);
            if (probe.is_open()) {
                probe.close();
                LoadConquestStrings(candidates[i]);
                break;
            }
        }
    }

    // Read raw file data
    LvlLog("  Reading PAK file...\n");
    if (!ReadFileBytes(pakPath, m_pakRaw)) { LvlLog("  ERROR reading PAK\n"); return false; }
    LvlLog("  PAK size: %u bytes\n", (uint32_t)m_pakRaw.size());

    LvlLog("  Reading BIN file...\n");
    if (!ReadFileBytes(binPath, m_binRaw)) { LvlLog("  ERROR reading BIN\n"); return false; }
    LvlLog("  BIN size: %u bytes\n", (uint32_t)m_binRaw.size());

    // Parse BIN first (builds m_binAssets — needed by model lookup but not PAK parse)
    LvlLog("  Calling ParseBin...\n");
    if (!ParseBin(m_binRaw)) { LvlLog("  ParseBin FAILED: %s\n", m_error.c_str()); return false; }
    LvlLog("  ParseBin done. binAssets=%u\n", (uint32_t)m_binAssets.size());

    // Parse PAK (builds m_block1 and all asset lists)
    LvlLog("  Calling ParsePak...\n");
    if (!ParsePak(m_pakRaw)) { LvlLog("  ParsePak FAILED: %s\n", m_error.c_str()); return false; }
    LvlLog("  ParsePak done.\n");

    m_loaded = true;

    // Build flat BIN asset list with CRC-resolved names (done after ParsePak so string tables are loaded)
    LvlLog("  Building BIN flat list...\n");
    for (std::map<uint32_t, BinAssetEntry>::const_iterator it = m_binAssets.begin();
         it != m_binAssets.end(); ++it)
    {
        BinFlatEntry fe;
        fe.key       = it->second.key;
        fe.name      = ResolveCrc(it->second.key);
        fe.kind      = it->second.kind;
        fe.size      = it->second.size;
        fe.size_comp = it->second.size_comp;
        m_binFlat.push_back(fe);
    }
    LvlLog("  BIN flat list done: %u entries\n", (uint32_t)m_binFlat.size());
    LvlLog("Load() complete. TOTAL crcMap=%u models=%u textures=%u anims=%u effects=%u gfx=%u objas=%u obj0s=%u animblocks=%u pakvals=%u\n",
           (uint32_t)m_crcMap.size(), (uint32_t)m_models.size(), (uint32_t)m_textures.size(),
           (uint32_t)m_animations.size(), (uint32_t)m_effects.size(), (uint32_t)m_gfxBlocks.size(),
           (uint32_t)m_objas.size(), (uint32_t)m_obj0s.size(), (uint32_t)m_animBlocks.size(),
           (uint32_t)m_pakVals.size());

    return true;
}

// ============================================================
//  Binary write-back: modify WorldTransform in decompressed Block1
// ============================================================

bool LevelReader::SetWorldTransform(uint32_t block1Offset, const float matrix[16]) {
    if (block1Offset == 0 || block1Offset + 64 > m_block1.size()) return false;
    memcpy(&m_block1[block1Offset], matrix, 64);
    return true;
}

bool LevelReader::SetFieldValue(uint32_t block1Offset, const void* data, uint32_t size) {
    if (block1Offset == 0 || block1Offset + size > m_block1.size()) return false;
    memcpy(&m_block1[block1Offset], data, size);
    return true;
}

bool LevelReader::GetFieldValue(uint32_t block1Offset, void* outData, uint32_t size) const {
    if (block1Offset + size > m_block1.size()) return false;
    memcpy(outData, &m_block1[block1Offset], size);
    return true;
}

// ============================================================
//  Entity creation — GUID generation and pending buffer
// ============================================================

uint32_t LevelReader::GenerateGuid() {
    return m_nextGuid++;
}

void LevelReader::AddPendingEntity(const PendingGameObj& obj) {
    // Generate GUID and set type CRC
    PendingGameObj pgo = obj;
    pgo.guid = GenerateGuid();
    if (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size()) {
        pgo.type_crc = m_gameObjTypes[pgo.type_def_index].crc;
    } else {
        pgo.type_crc = 0;
    }

    // Build field data by cloning from an existing instance of the same type.
    // This ensures ALL fields have valid game defaults (Health, Team, LODs, etc.).
    // Then override identity fields: GUID, ParentGUID, GameModeMask, Name, Mesh,
    // WorldTransform, Transform.

    // Well-known field name CRCs
    static const uint32_t CRC_GUID            = 0xCF98052Fu;
    static const uint32_t CRC_PARENTGUID      = 0xBB757061u;
    static const uint32_t CRC_GAMEMODEMASK    = 0x141C4D91u;
    static const uint32_t CRC_NAME            = 0xB11241E0u;
    static const uint32_t CRC_MESH            = 0x9805A0A6u;
    static const uint32_t CRC_WORLDTRANSFORM  = 0xD486DE80u;
    static const uint32_t CRC_TRANSFORM       = 0x869492C8u;
    static const uint32_t CRC_OUTPUTS         = 0x250BE3C1u;
    static const uint32_t CRC_INITCHILDOBJS   = 0x68C1B52Eu;

    {
        // ---- Step 1: Find an existing instance of the same type to clone from ----
        int donorIdx = -1;
        for (int gi = 0; gi < (int)m_gameObjs.size(); ++gi) {
            if (m_gameObjs[gi].type_crc == pgo.type_crc &&
                m_gameObjs[gi].block1_obj_offset > 0 &&
                m_gameObjs[gi].block1_obj_size > 0) {
                donorIdx = gi;
                break;
            }
        }

        bool cloned = false;
        if (donorIdx >= 0) {
            const LevelGameObjEntry& donor = m_gameObjs[donorIdx];
            uint32_t srcOff  = donor.block1_obj_offset;
            uint32_t srcSize = donor.block1_obj_size;

            if (srcOff + srcSize <= m_block1.size() && srcSize >= 16) {
                // Clone the raw field data byte-for-byte
                pgo.fieldData.assign(&m_block1[srcOff], &m_block1[srcOff + srcSize]);
                cloned = true;
                LvlLog("AddPendingEntity: cloned %u bytes from donor '%s' (idx=%d)\n",
                       srcSize, donor.name.c_str(), donorIdx);
            }
        }

        if (!cloned) {
            // No donor found — build minimal field data from type definition
            uint32_t fieldDataSize = 16;
            if (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size()) {
                const LevelGameObjTypeDef& td = m_gameObjTypes[pgo.type_def_index];
                for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                    FieldKind fk = FieldKindFromCrc(td.fields[fi].kind_crc);
                    uint32_t end = td.fields[fi].offset + FieldKindFixedSize(fk);
                    if (end > fieldDataSize) fieldDataSize = end;
                }
            }
            fieldDataSize = (fieldDataSize + 15) & 0xFFFFFFF0u;
            pgo.fieldData.assign(fieldDataSize, 0);
            LvlLog("AddPendingEntity: no donor, built %u zero bytes\n", fieldDataSize);
        }

        // ---- Step 2: Override identity fields using type definition offsets ----
        // We need the field offset for each CRC. Get from type definition.
        if (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size()) {
            const LevelGameObjTypeDef& td = m_gameObjTypes[pgo.type_def_index];
            for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                const GOFieldDefEntry& fd = td.fields[fi];
                FieldKind fk = FieldKindFromCrc(fd.kind_crc);
                uint32_t fsize = FieldKindFixedSize(fk);

                if (fd.offset + fsize > pgo.fieldData.size()) continue;
                uint8_t* fptr = &pgo.fieldData[0] + fd.offset;

                if (fd.name_crc == CRC_GUID) {
                    memcpy(fptr, &pgo.guid, 4);
                } else if (fd.name_crc == CRC_PARENTGUID) {
                    memcpy(fptr, &pgo.parent_guid, 4);
                } else if (fd.name_crc == CRC_GAMEMODEMASK) {
                    memcpy(fptr, &pgo.gamemodemask, 4);
                } else if (fd.name_crc == CRC_NAME) {
                    memcpy(fptr, &pgo.name_crc, 4);
                } else if (fd.name_crc == CRC_MESH && pgo.mesh_crc != 0) {
                    memcpy(fptr, &pgo.mesh_crc, 4);
                } else if (fd.name_crc == CRC_WORLDTRANSFORM || fd.name_crc == CRC_TRANSFORM) {
                    if (fk == FK_MATRIX4X4 && fd.offset + 64 <= (uint32_t)pgo.fieldData.size()) {
                        memcpy(fptr, pgo.world_transform, 64);
                    }
                } else if (fd.name_crc == CRC_OUTPUTS || fd.name_crc == CRC_INITCHILDOBJS) {
                    // Clear list references — new entity has no wired outputs/children.
                    // Write empty list header: num=0, offset points past fixed region.
                    if (FieldKindIsList(fk)) {
                        uint16_t listNum = 0;
                        uint16_t listOff = (uint16_t)(pgo.fieldData.size() - fd.offset - 4);
                        memcpy(fptr, &listNum, 2);
                        memcpy(fptr + 2, &listOff, 2);
                    }
                }
            }
        } else {
            // Fallback: no type def, write at fixed offsets 0/4/8/12
            if (pgo.fieldData.size() >= 16) {
                memcpy(&pgo.fieldData[0],  &pgo.guid, 4);
                memcpy(&pgo.fieldData[4],  &pgo.parent_guid, 4);
                memcpy(&pgo.fieldData[8],  &pgo.gamemodemask, 4);
                memcpy(&pgo.fieldData[12], &pgo.name_crc, 4);
            }
        }
    }

    m_pendingObjs.push_back(pgo);

    // Also append a LevelGameObjEntry for immediate UI display
    LevelGameObjEntry e;
    e.guid = pgo.guid;
    e.parent_guid = pgo.parent_guid;
    e.gamemodemask = pgo.gamemodemask;
    e.name_crc = pgo.name_crc;
    e.name = ResolveCrc(pgo.name_crc);
    if (e.name.empty()) {
        char buf[32];
        sprintf(buf, "0x%08X", pgo.name_crc);
        e.name = buf;
    }
    e.type_crc = pgo.type_crc;
    e.type_name = ResolveCrc(pgo.type_crc);
    e.layer_guid = pgo.layer_guid;
    e.mesh_crc = pgo.mesh_crc;
    e.mesh_name = pgo.mesh_crc ? ResolveCrc(pgo.mesh_crc) : "";
    e.has_transform = true;
    e.has_world_transform = true;
    memcpy(e.pos, &pgo.world_transform[12], 12);
    memcpy(e.world_transform, pgo.world_transform, 64);
    e.block1_wt_offset = 0; // no Block1 offset yet (pending)
    e.block1_tf_offset = 0;
    e.block1_obj_offset = 0;
    e.block1_obj_size = (uint32_t)pgo.fieldData.size();
    e.type_def_index = pgo.type_def_index;
    e.target_guid = 0;
    e.delay = 0;
    e.sticky = false;
    e.node1_guid = 0;
    e.node2_guid = 0;
    e.editor_color = 0xFF00FF00;
    e.editor_outer = 2.0f;
    e.editor_size[0] = e.editor_size[1] = e.editor_size[2] = 1.0f;
    e.editor_shape = "Sphere";
    e.position_track_guid = 0;
    e.target_track_guid = 0;
    e.fov = 0;
    e.total_duration = 0;
    e.position_travel_time = 0;
    e.target_travel_time = 0;
    m_gameObjs.push_back(e);

    LvlLog("AddPendingEntity: type=%s guid=%u parent=%u layer=%u pos=(%.1f,%.1f,%.1f) fieldData=%u bytes\n",
           e.type_name.c_str(), pgo.guid, pgo.parent_guid, pgo.layer_guid,
           e.pos[0], e.pos[1], e.pos[2], (unsigned)pgo.fieldData.size());
}

// ============================================================
//  Collision creation
// ============================================================

void LevelReader::AddPendingCollision(const PendingCollisionMesh& mesh) {
    m_pendingCollisions.push_back(mesh);
    LvlLog("AddPendingCollision: key=0x%08X verts=%u tris=%u mopp=%u bytes\n",
           mesh.key, (unsigned)(mesh.verts.size() / 3),
           (unsigned)(mesh.indices.size() / 3),
           (unsigned)mesh.moppData.size());
}

bool LevelReader::ModelHasCollision(uint32_t modelCrc) const {
    if (m_block1.empty()) return false;

    // Check 1: per-model ShapeInfo (ModelInfo.shape_num > 0)
    uint32_t miBase = m_pakHeader.model_info_offset;
    uint32_t miCount = m_pakHeader.model_info_num;
    for (uint32_t i = 0; i < miCount; ++i) {
        uint32_t off = miBase + i * 256;
        if (off + 256 > m_block1.size()) break;
        uint32_t key;
        memcpy(&key, &m_block1[off], 4);
        if (key == modelCrc) {
            uint32_t shapeNum;
            memcpy(&shapeNum, &m_block1[off + 228], 4);
            if (shapeNum > 0 && shapeNum < 10000) return true;
            break;
        }
    }

    // Check 2: HkShapeInfo BVTree entries with matching key CRC
    uint32_t hkBase = m_pakHeader.hk_shape_info_offset;
    uint32_t hkCount = m_pakHeader.hk_shape_info_num;
    uint32_t hkStride = m_pakHeader.hk_shape_info_size;
    if (hkStride == 0) hkStride = 80;
    for (uint32_t i = 0; i < hkCount; ++i) {
        uint32_t off = hkBase + i * hkStride;
        if (off + hkStride > m_block1.size()) break;
        uint32_t kind, key;
        memcpy(&kind, &m_block1[off + 32], 4);
        memcpy(&key,  &m_block1[off + 36], 4);
        if (kind == 6 && key == modelCrc) return true;
    }

    // Check 3: pending collisions not yet saved
    for (size_t i = 0; i < m_pendingCollisions.size(); ++i) {
        if (m_pendingCollisions[i].key == modelCrc) return true;
    }

    return false;
}

// ============================================================
//  (FixU32 / FixAllBlockOffsets removed — replaced by full rebuild)
// ============================================================

// Fix ALL Block1 absolute offset fields in ALL info structs after an insert+shift.
// hdr must have its own offset fields already updated (via SHIFT_IF_PAST).
// oldHkCount = number of HkShapeInfo entries BEFORE the new ones were added.
// (FixAllBlockOffsets body removed — full rebuild handles all offsets)
#if 0
static void FixAllBlockOffsets_REMOVED(std::vector<uint8_t>& b1,
                               const PakHeader& hdr,
                               uint32_t insertPt, uint32_t shift,
                               uint32_t oldHkCount) {
    // --- ModelInfo (256 bytes): 20 offset fields ---
    {
        uint32_t base = hdr.model_info_offset;
        uint32_t num  = hdr.model_info_num;
        static const uint32_t offsets[] = {
            8,12,48,136,140,144,152,160,164,172,180,196,200,204,224,232,240,244,248,252
        };
        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * 256;
            for (int k = 0; k < 20; ++k)
                FixU32(b1, e + offsets[k], insertPt, shift);
        }
    }
    // --- BufferInfo (buffer_info_size bytes): 4 offset fields ---
    {
        uint32_t base = hdr.buffer_info_offset;
        uint32_t num  = hdr.buffer_info_num;
        uint32_t stride = hdr.buffer_info_size;
        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * stride;
            FixU32(b1, e + 0,   insertPt, shift); // vbuff_info_offset
            FixU32(b1, e + 4,   insertPt, shift); // vbuff_info_offset_2
            FixU32(b1, e + 8,   insertPt, shift); // vbuff_info_offset_3
            FixU32(b1, e + 260, insertPt, shift); // ibuff_info_offset
        }
    }
    // --- ShapeInfo (shape_info_size bytes): 2 offset fields ---
    {
        uint32_t base = hdr.shape_info_offset;
        uint32_t num  = hdr.shape_info_num;
        uint32_t stride = hdr.shape_info_size;
        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * stride;
            FixU32(b1, e + 0,   insertPt, shift); // offset (shape data)
            FixU32(b1, e + 112, insertPt, shift); // hk_shape_offset
        }
    }
    // --- HkShapeInfo (80 bytes, ALL entries including new): kind-dependent offset fields ---
    // New entries also have pre-shift offsets that need fixing.
    {
        uint32_t base = hdr.hk_shape_info_offset;
        uint32_t totalHkCount = hdr.hk_shape_info_num; // old + new
        (void)oldHkCount; // no longer used to limit iteration
        for (uint32_t i = 0; i < totalHkCount; ++i) {
            uint32_t e = base + i * 80;
            if (e + 80 > (uint32_t)b1.size()) break;
            uint32_t kind;
            memcpy(&kind, &b1[e + 32], 4);
            if (kind == 5) {
                FixU32(b1, e + 44, insertPt, shift); // a_offset (planes)
                FixU32(b1, e + 52, insertPt, shift); // b_offset (verts)
            } else if (kind == 6) {
                FixU32(b1, e + 60, insertPt, shift); // c_offset (MOPP tree)
                FixU32(b1, e + 68, insertPt, shift); // d_offset (verts)
                FixU32(b1, e + 76, insertPt, shift); // e_offset (indices)
            }
        }
    }
    // --- HkConstraintInfo (hk_constraint_info_size bytes): 6 offset fields ---
    {
        uint32_t base = hdr.hk_constraint_info_offset;
        uint32_t num  = hdr.hk_constraint_info_num;
        uint32_t stride = hdr.hk_constraint_info_size;
        for (uint32_t i = 0; i < num; ++i) {
            uint32_t e = base + i * stride;
            FixU32(b1, e + 4,  insertPt, shift); // bone_parents_offset
            FixU32(b1, e + 12, insertPt, shift); // bone_names_offset
            FixU32(b1, e + 20, insertPt, shift); // bone_transforms_offset
            FixU32(b1, e + 40, insertPt, shift); // bones_offset
            FixU32(b1, e + 48, insertPt, shift); // bone_order_offset
            FixU32(b1, e + 64, insertPt, shift); // vals2_offset
        }
    }
    // --- EffectInfo (16 bytes): offset at byte 8 ---
    {
        uint32_t base = hdr.effect_info_offset;
        uint32_t num  = hdr.effect_info_num;
        for (uint32_t i = 0; i < num; ++i)
            FixU32(b1, base + i * 16 + 8, insertPt, shift);
    }
    // --- GFXBlockInfo (12 bytes): offset at byte 4 ---
    {
        uint32_t base = hdr.gfx_block_info_offset;
        uint32_t num  = hdr.gfx_block_info_num;
        for (uint32_t i = 0; i < num; ++i)
            FixU32(b1, base + i * 12 + 4, insertPt, shift);
    }
    // --- FoliageInfo: offset at byte 28 ---
    {
        uint32_t base = hdr.foliage_info_offset;
        uint32_t num  = hdr.foliage_info_num;
        uint32_t stride = hdr.foliage_info_size;
        for (uint32_t i = 0; i < num; ++i)
            FixU32(b1, base + i * stride + 28, insertPt, shift);
    }
    // --- RadiosityValsInfo: offset at byte 8 ---
    {
        uint32_t base = hdr.radiosity_vals_info_offset;
        uint32_t num  = hdr.radiosity_vals_info_num;
        uint32_t stride = hdr.radiosity_vals_info_size;
        for (uint32_t i = 0; i < num; ++i)
            FixU32(b1, base + i * stride + 8, insertPt, shift);
    }
    // --- PFieldInfo: offset at byte 16 ---
    {
        uint32_t base = hdr.pfield_info_offset;
        uint32_t num  = hdr.pfield_info_num;
        uint32_t stride = hdr.pfield_info_size;
        for (uint32_t i = 0; i < num; ++i)
            FixU32(b1, base + i * stride + 16, insertPt, shift);
    }
    // --- Model sub-data: vbuff/ibuff reference arrays ---
    // Each model has vbuff_num u32s at vbuff_offset and ibuff_num u32s at ibuff_offset.
    // These u32 values are Block1 offsets of VBuffInfo/IBuffInfo entries.
    // ModelInfo fields are already fixed at this point, so we read the UPDATED offsets.
    {
        uint32_t miBase = hdr.model_info_offset;
        uint32_t miNum  = hdr.model_info_num;
        for (uint32_t i = 0; i < miNum; ++i) {
            uint32_t miOff = miBase + i * 256;
            if (miOff + 256 > (uint32_t)b1.size()) break;
            uint32_t vbOff, vbNum, ibOff, ibNum;
            memcpy(&vbOff, &b1[miOff + 164], 4); // already shifted
            memcpy(&vbNum, &b1[miOff + 168], 4);
            memcpy(&ibOff, &b1[miOff + 172], 4); // already shifted
            memcpy(&ibNum, &b1[miOff + 176], 4);
            for (uint32_t j = 0; j < vbNum; ++j)
                FixU32(b1, vbOff + j * 4, insertPt, shift);
            for (uint32_t j = 0; j < ibNum; ++j)
                FixU32(b1, ibOff + j * 4, insertPt, shift);
        }
    }
}
#endif // FixAllBlockOffsets_REMOVED

// ============================================================
//  Save modified PAK file — Full rebuild from parsed structs.
//  All offsets computed fresh at write time. No shifting.
// ============================================================

bool LevelReader::SavePak(const std::string& outputPath) {
    if (m_pakRaw.empty() || m_block1.empty()) {
        m_error = "No PAK data loaded";
        return false;
    }
    LvlLog("SavePak: === REBUILD START ===\n");
    CreateSaveProgress();
    UpdateSaveProgress(5, "Syncing entity positions...");

    // Free BIN raw data temporarily — 110MB that isn't needed during save.
    // This prevents 32-bit address space exhaustion during Block1 rebuild.
    std::vector<uint8_t> savedBinRaw;
    savedBinRaw.swap(m_binRaw); // move 110MB out, m_binRaw now empty

    // ---- Sync pending entity positions from display entries (user may have moved them) ----
    if (!m_pendingObjs.empty()) {
        static const uint32_t CRC_WT = 0xD486DE80u; // WorldTransform
        static const uint32_t CRC_TF = 0x869492C8u; // Transform
        for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
            PendingGameObj& p = m_pendingObjs[pi];
            // Find matching display entry by GUID
            for (size_t gi = 0; gi < m_gameObjs.size(); ++gi) {
                if (m_gameObjs[gi].guid == p.guid && m_gameObjs[gi].has_world_transform) {
                    // Copy updated world transform
                    memcpy(p.world_transform, m_gameObjs[gi].world_transform, 64);
                    // Update WorldTransform + Transform fields in fieldData
                    if (p.type_def_index >= 0 && p.type_def_index < (int)m_gameObjTypes.size()) {
                        const LevelGameObjTypeDef& td = m_gameObjTypes[p.type_def_index];
                        for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                            const GOFieldDefEntry& fd = td.fields[fi];
                            if ((fd.name_crc == CRC_WT || fd.name_crc == CRC_TF) &&
                                fd.kind_crc == 0xB968BDDBu && // Matrix4x4
                                fd.offset + 64 <= (uint32_t)p.fieldData.size()) {
                                memcpy(&p.fieldData[fd.offset], p.world_transform, 64);
                            }
                        }
                    }
                    LvlLog("SavePak: synced pending entity %u pos=(%.1f,%.1f,%.1f)\n",
                           p.guid, p.world_transform[12], p.world_transform[13], p.world_transform[14]);
                    break;
                }
            }
        }
    }

    // ================================================================
    //  REBUILD Block1 from scratch (like the Rust lotrc parser)
    //  No patching, no delta arithmetic. All offsets computed fresh.
    // ================================================================

    // ---- Collect new strings from pending entities ----
    std::vector<std::string> newStrings;
    std::vector<uint32_t>    newStringCrcs;
    for (size_t i = 0; i < m_pendingObjs.size(); ++i) {
        const PendingGameObj& p = m_pendingObjs[i];
        if (!p.name_str.empty() && m_crcMap.find(p.name_crc) == m_crcMap.end()) {
            newStrings.push_back(p.name_str);
            newStringCrcs.push_back(p.name_crc);
            m_crcMap[p.name_crc] = p.name_str;
        }
    }

    // ---- Build GameObjs payload ----
    static const uint32_t KEY_GAMEOBJS = LotrHashString("Level");
    std::vector<uint8_t> goPayload;
    if (m_gameObjsBlock1Offset > 0 && m_gameObjsBlock1Size > 0 &&
        m_gameObjsBlock1Offset + m_gameObjsBlock1Size <= m_block1.size()) {
        const uint8_t* goData = &m_block1[m_gameObjsBlock1Offset];
        uint32_t goSize = m_gameObjsBlock1Size;

        uint32_t obj_num, obj_off;
        memcpy(&obj_num, goData + 12, 4);
        memcpy(&obj_off, goData + 16, 4);

        // Walk existing objects to find the end
        uint32_t existingEnd = obj_off;
        {
            uint32_t walkOff = obj_off;
            for (uint32_t o = 0; o < obj_num; ++o) {
                if (walkOff + 16 > goSize) break;
                uint16_t osize;
                memcpy(&osize, goData + walkOff + 8, 2);
                walkOff += 16 + osize;
            }
            existingEnd = walkOff;
        }

        // Copy existing data (header + types + existing objects)
        goPayload.assign(goData, goData + existingEnd);

        // Append pending entities
        for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
            const PendingGameObj& p = m_pendingObjs[pi];
            uint8_t hdr[16];
            memset(hdr, 0, 16);
            memcpy(hdr + 0, &p.layer_guid, 4);
            memcpy(hdr + 4, &p.type_crc, 4);
            uint16_t osize = (uint16_t)p.fieldData.size();
            memcpy(hdr + 8, &osize, 2);
            goPayload.insert(goPayload.end(), hdr, hdr + 16);
            goPayload.insert(goPayload.end(), p.fieldData.begin(), p.fieldData.end());
        }

        // Update obj_num in header
        uint32_t newObjNum = obj_num + (uint32_t)m_pendingObjs.size();
        memcpy(&goPayload[12], &newObjNum, 4);

        LvlLog("SavePak: GameObjs rebuilt: %u -> %u objects, %u -> %u bytes\n",
               obj_num, newObjNum, goSize, (unsigned)goPayload.size());
    }
    m_pendingObjs.clear();

    // ---- Build newBlock1 — FULL REBUILD from parsed structs ----
    std::vector<uint8_t> newBlock1;
    PakHeader newHdr = m_pakHeader;

    // ---- Append pending collisions to parsed data ----
    // Game requires the FULL chain: ModelInfo → ShapeInfo → HkShapeInfo (BVTreeMesh).
    // Orphan HkShapeInfo entries crash Havok broadphase. We must create all three.
    if (!m_pendingCollisions.empty()) {
        UpdateSaveProgress(15, "Appending collision data...");
        LvlLog("SavePak: appending %u pending collisions to parsed data\n",
               (unsigned)m_pendingCollisions.size());

        uint32_t siSize = m_pakHeader.shape_info_size;     // 124 bytes
        uint32_t hkSize = m_pakHeader.hk_shape_info_size;  // 80 bytes
        if (siSize == 0) siSize = 124;
        if (hkSize == 0) hkSize = 80;

        for (size_t ci = 0; ci < m_pendingCollisions.size(); ++ci) {
            const PendingCollisionMesh& pcm = m_pendingCollisions[ci];
            uint32_t numVerts = (uint32_t)(pcm.verts.size() / 3);
            uint32_t numTris  = (uint32_t)(pcm.indices.size() / 3);
            if (numVerts == 0 || numTris == 0) continue;

            // Skip if model already has shapes — overwriting breaks existing collision
            {
                uint32_t miSize2 = m_pakHeader.model_info_size;
                uint32_t miNum2  = m_pakHeader.model_info_num;
                bool skip = false;
                for (uint32_t mi = 0; mi < miNum2; ++mi) {
                    uint32_t miByte = mi * miSize2;
                    if (miByte + miSize2 > m_parsedData.rawModelInfo.size()) break;
                    uint32_t modelKey;
                    memcpy(&modelKey, &m_parsedData.rawModelInfo[miByte], 4);
                    if (modelKey == pcm.key) {
                        uint32_t existNum;
                        memcpy(&existNum, &m_parsedData.rawModelInfo[miByte + 228], 4);
                        if (existNum > 0) {
                            LvlLog("SavePak: SKIP collision for 0x%08X — already has %u shapes\n",
                                   pcm.key, existNum);
                            skip = true;
                        }
                        break;
                    }
                }
                if (skip) continue;
            }

            // Compute AABB
            float minB[3] = {1e30f, 1e30f, 1e30f};
            float maxB[3] = {-1e30f, -1e30f, -1e30f};
            for (uint32_t vi = 0; vi < numVerts; ++vi) {
                float vx = pcm.verts[vi*3], vy = pcm.verts[vi*3+1], vz = pcm.verts[vi*3+2];
                if (vx < minB[0]) minB[0] = vx; if (vy < minB[1]) minB[1] = vy; if (vz < minB[2]) minB[2] = vz;
                if (vx > maxB[0]) maxB[0] = vx; if (vy > maxB[1]) maxB[1] = vy; if (vz > maxB[2]) maxB[2] = vz;
            }
            float aabbMin[3] = {minB[0]-0.05f, minB[1]-0.05f, minB[2]-0.05f};
            float maxExt = 0;
            for (int a = 0; a < 3; ++a) { float e = maxB[a]-minB[a]; if (e > maxExt) maxExt = e; }
            float treeScale = 254.f * 256.f * 256.f / (maxExt + 0.1f);
            uint32_t treeSize = (uint32_t)pcm.moppData.size();

            // ---- 1. Build 80-byte HkShapeInfo entry (BVTreeMesh, kind=6) ----
            uint8_t hkEntry[80];
            memset(hkEntry, 0, 80);
            memcpy(hkEntry + 0, pcm.translation, 16);
            memcpy(hkEntry + 16, pcm.rotation, 16);
            { uint32_t k = 6; memcpy(hkEntry + 32, &k, 4); }
            // key = empty (0) — matches Rust parser's "" key for new entries
            memcpy(hkEntry + 40, aabbMin, 12);
            memcpy(hkEntry + 52, &treeScale, 4);
            memcpy(hkEntry + 56, &treeSize, 4);
            // c_offset(60), d_offset(68), e_offset(76) = 0 (set by Step 2e in rebuild)
            memcpy(hkEntry + 64, &numVerts, 4);
            memcpy(hkEntry + 72, &numTris, 4);

            // The new HkShapeInfo index (after appending)
            uint32_t newHkIdx = (uint32_t)(m_parsedData.rawHkShapeInfo.size() / hkSize);

            m_parsedData.rawHkShapeInfo.insert(m_parsedData.rawHkShapeInfo.end(),
                hkEntry, hkEntry + 80);

            // Build collision data blob (MOPP + verts + indices)
            std::vector<uint8_t> collData;
            collData.insert(collData.end(), pcm.moppData.begin(), pcm.moppData.end());
            collData.insert(collData.end(),
                reinterpret_cast<const uint8_t*>(&pcm.verts[0]),
                reinterpret_cast<const uint8_t*>(&pcm.verts[0]) + numVerts * 12);
            collData.insert(collData.end(),
                reinterpret_cast<const uint8_t*>(&pcm.indices[0]),
                reinterpret_cast<const uint8_t*>(&pcm.indices[0]) + numTris * 6);
            m_parsedData.hkShapeData.push_back(collData);

            // ---- 2. Build 124-byte ShapeInfo entry (kind=1) ----
            // Byte layout (from real ShapeInfo entries):
            //   0: offset(0)  4: kind(1)  8: unk_2(0)
            //  12: unk_3(0)  16: unk_4(0)  20: unk_5(0)
            //  24: trans.x(0)  28: trans.y(0)  32: trans.z(0)
            //  36: rot.x(0)  40: rot.y(0)  44: rot.z(0)  48: rot.w(1.0)
            //  52: mat3x3 identity diagonal at 52,68,84
            //  88: unk_22(0)  92: unk_23(0.5)  96-104: zeros
            // 108: hk_shape_num(1)  112: hk_shape_offset
            // 116: unk_29(0)  120: unk_30(-1.0)
            uint8_t siEntry[124];
            memset(siEntry, 0, 124);
            { uint32_t k = 1; memcpy(siEntry + 4, &k, 4); }      // kind = 1
            { float w = 1.0f; memcpy(siEntry + 48, &w, 4); }     // rotation.w = 1.0
            { float one = 1.0f;
              memcpy(siEntry + 52, &one, 4);                       // mat[0][0] = 1.0
              memcpy(siEntry + 68, &one, 4);                       // mat[1][1] = 1.0
              memcpy(siEntry + 84, &one, 4);                       // mat[2][2] = 1.0
            }
            { float half = 0.5f; memcpy(siEntry + 92, &half, 4); } // unk_23 = 0.5
            { uint32_t one = 1; memcpy(siEntry + 108, &one, 4); }  // hk_shape_num = 1
            // hk_shape_offset: set to ORIGINAL offset space (Step 3 remaps with hkiDelta)
            { uint32_t hkOff = m_pakHeader.hk_shape_info_offset + newHkIdx * hkSize;
              memcpy(siEntry + 112, &hkOff, 4); }
            { float neg1 = -1.0f; memcpy(siEntry + 120, &neg1, 4); } // unk_30 = -1.0

            // Append ShapeInfo to raw array
            uint32_t newSiIdx = (uint32_t)(m_parsedData.rawShapeInfo.size() / siSize);
            m_parsedData.rawShapeInfo.insert(m_parsedData.rawShapeInfo.end(),
                siEntry, siEntry + siSize);
            // Also append empty shapeData entry (kind=1 has no variable data)
            m_parsedData.shapeData.push_back(std::vector<uint8_t>());

            // ---- 3. Update ModelInfo to link to the new ShapeInfo ----
            // Find the ModelInfo entry for this model CRC and set shape_num + shape_offset
            uint32_t miSize = m_pakHeader.model_info_size; // 256
            uint32_t miNum  = m_pakHeader.model_info_num;
            uint32_t miOff  = m_pakHeader.model_info_offset;
            for (uint32_t mi = 0; mi < miNum; ++mi) {
                uint32_t miByte = mi * miSize;
                if (miByte + miSize > m_parsedData.rawModelInfo.size()) break;
                uint32_t modelKey;
                memcpy(&modelKey, &m_parsedData.rawModelInfo[miByte], 4);
                if (modelKey == pcm.key) {
                    // Only add collision if model doesn't already have shapes.
                    // Overwriting existing shape_offset breaks the original collision chain.
                    uint32_t existingShapeNum;
                    memcpy(&existingShapeNum, &m_parsedData.rawModelInfo[miByte + 228], 4);
                    if (existingShapeNum > 0) {
                        LvlLog("SavePak: ModelInfo[%u] (key=0x%08X) already has %u shapes — SKIPPING collision\n",
                               mi, modelKey, existingShapeNum);
                    } else {
                        uint32_t shapeOff = m_pakHeader.shape_info_offset + newSiIdx * siSize;
                        uint32_t shapeNum = 1;
                        memcpy(&m_parsedData.rawModelInfo[miByte + 224], &shapeOff, 4);
                        memcpy(&m_parsedData.rawModelInfo[miByte + 228], &shapeNum, 4);
                        LvlLog("SavePak: linked ModelInfo[%u] (key=0x%08X) -> ShapeInfo[%u] -> HkShapeInfo[%u]\n",
                               mi, modelKey, newSiIdx, newHkIdx);
                    }
                    break;
                }
            }

            LvlLog("SavePak: collision[%u]: %u verts %u tris mopp=%u bytes (ShapeInfo[%u] -> HkShapeInfo[%u])\n",
                   (unsigned)ci, numVerts, numTris, treeSize, newSiIdx, newHkIdx);
        }
    }

    UpdateSaveProgress(20, "Rebuilding Block1 from parsed data...");
    // FULL REBUILD: build newBlock1 from scratch using RebuildBlock1Full.
    // All info arrays at fresh positions, all variable-length data at fresh positions,
    // all cross-references computed at write time. No verbatim copy, no shifting.
    {
        std::string rebuildErr;
        if (!RebuildBlock1Full(m_block1, m_pakHeader, m_parsedData,
                               newBlock1, newHdr, goPayload, KEY_GAMEOBJS,
                               m_subBlocks1, rebuildErr)) {
            m_error = "Block1 full rebuild failed: " + rebuildErr;
            m_binRaw.swap(savedBinRaw);
            CloseSaveProgress();
            return false;
        }
        LvlLog("SavePak: FULL REBUILD OK — newBlock1=%u bytes (was %u)\n",
               (unsigned)newBlock1.size(), (unsigned)m_block1.size());
    }
    m_pendingCollisions.clear();

    #if 0 // -------- OLD CODE (dead — kept for reference) --------
    // 1. Copy fixed info arrays region [0..sub_blocks1_offset)
    //    This includes ALL in-place edits (SetWorldTransform, SetFieldValue)
    if (oldSbOff > 0 && oldSbOff <= (uint32_t)m_block1.size()) {
        newBlock1.resize(oldSbOff);
        memcpy(&newBlock1[0], &m_block1[0], oldSbOff);
    }

    // 1b. Phase 1 rebuild: write Effect, GFXBlock, Foliage, RadiosityVals
    //     from parsed structs. dataWritePos=0 → in-place mode (same positions).
    {
        std::string rebuildErr;
        uint32_t dataPos = 0; // 0 = in-place mode (write at original info.offset)
        if (!RebuildBlock1Phase1(newBlock1, newHdr, m_parsedData, dataPos, rebuildErr)) {
            LvlLog("SavePak: Phase 1 rebuild FAILED: %s\n", rebuildErr.c_str());
        }
    }

    // 1c. Phase 4 rebuild: write Model, HkShape, HkConstraint data.
    {
        std::string rebuildErr;
        uint32_t dataPos = 0; // in-place
        if (!RebuildBlock1Phase4(newBlock1, newHdr, m_parsedData, dataPos, rebuildErr)) {
            LvlLog("SavePak: Phase 4 rebuild FAILED: %s\n", rebuildErr.c_str());
        }
    }

    // 1d. Phase 3 rebuild: write Shape data at original positions.
    {
        std::string rebuildErr;
        uint32_t dataPos = 0; // in-place
        if (!RebuildBlock1Phase3(newBlock1, newHdr, m_parsedData, dataPos, rebuildErr)) {
            LvlLog("SavePak: Phase 3 rebuild FAILED: %s\n", rebuildErr.c_str());
        }
    }

    // 1d. Phase 2 rebuild: remap block2_offsets cross-references.
    //     In in-place mode, old offsets == new offsets so this is a no-op.
    //     When full rebuild mode lands, this becomes essential.
    {
        std::string rebuildErr;
        if (!RebuildBlock1Phase2(newBlock1, m_pakHeader, newHdr, m_parsedData, rebuildErr)) {
            LvlLog("SavePak: Phase 2 rebuild FAILED: %s\n", rebuildErr.c_str());
        }
    }

    // 2. Rebuild sub-blocks table + payloads
    {
        // Align to 16
        uint32_t aligned = ((uint32_t)newBlock1.size() + 15) & ~15u;
        newBlock1.resize(aligned, 0);
        uint32_t newSbOff = (uint32_t)newBlock1.size();
        newHdr.sub_blocks1_offset = newSbOff;

        uint32_t sbCount = (uint32_t)m_subBlocks1.size();
        uint32_t tableHeaderSize = 16;
        uint32_t tableEntriesSize = sbCount * 12;
        uint32_t tableSize = tableHeaderSize + tableEntriesSize;

        // Compute payload sizes and offsets (relative to table start)
        std::vector<uint32_t> pSizes(sbCount);
        std::vector<uint32_t> pOffsets(sbCount);
        uint32_t cursor = tableSize;
        for (uint32_t i = 0; i < sbCount; ++i) {
            if (m_subBlocks1[i].key == KEY_GAMEOBJS && !goPayload.empty())
                pSizes[i] = (uint32_t)goPayload.size();
            else
                pSizes[i] = m_subBlocks1[i].size;
            pOffsets[i] = cursor;
            cursor += pSizes[i];
        }

        // Write table header: z0, block_num, z2, z3
        uint32_t zero = 0;
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&sbCount, (uint8_t*)&sbCount + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);
        newBlock1.insert(newBlock1.end(), (uint8_t*)&zero, (uint8_t*)&zero + 4);

        // Write table entries
        for (uint32_t i = 0; i < sbCount; ++i) {
            uint8_t entry[12];
            memcpy(entry + 0, &m_subBlocks1[i].key, 4);
            memcpy(entry + 4, &pOffsets[i], 4);
            memcpy(entry + 8, &pSizes[i], 4);
            newBlock1.insert(newBlock1.end(), entry, entry + 12);
        }

        // Write payloads
        for (uint32_t i = 0; i < sbCount; ++i) {
            if (m_subBlocks1[i].key == KEY_GAMEOBJS && !goPayload.empty()) {
                newBlock1.insert(newBlock1.end(), goPayload.begin(), goPayload.end());
            } else {
                // Read payload directly from m_block1 (no rawPayload cache)
                uint32_t pOff = m_subBlocks1[i].absOffset;
                uint32_t pSz  = m_subBlocks1[i].size;
                if (pOff + pSz <= (uint32_t)m_block1.size()) {
                    newBlock1.insert(newBlock1.end(),
                        &m_block1[pOff], &m_block1[pOff + pSz]);
                }
            }
        }

        LvlLog("SavePak: sub_blocks1 rebuilt at %u (%u blocks)\n", newSbOff, sbCount);
    }

    // 3. String keys
    {
        uint32_t aligned = ((uint32_t)newBlock1.size() + 31) & ~31u;
        newBlock1.resize(aligned, 0);
        uint32_t newSkOff = (uint32_t)newBlock1.size();
        newHdr.string_keys_offset = newSkOff;
        // Read string_keys directly from m_block1
        uint32_t oldSkOff = m_pakHeader.string_keys_offset;
        LvlLog("SavePak: string_keys read: oldSkOff=%u block1_size=%u newBlock1_size=%u\n",
               oldSkOff, (unsigned)m_block1.size(), (unsigned)newBlock1.size());
        uint32_t skBytes = 0;
        if (oldSkOff > 0 && oldSkOff + 16 <= (uint32_t)m_block1.size()) {
            LvlLog("SavePak: reading numA...\n");
            uint16_t numA;
            memcpy(&numA, &m_block1[oldSkOff], 2);
            skBytes = 16 + (uint32_t)numA * 12;
            LvlLog("SavePak: numA=%u skBytes=%u end=%u\n", (unsigned)numA, skBytes, oldSkOff + skBytes);
            if (oldSkOff + skBytes <= (uint32_t)m_block1.size()) {
                LvlLog("SavePak: copying string_keys into newBlock1...\n");
                uint32_t prevSize = (uint32_t)newBlock1.size();
                newBlock1.resize(prevSize + skBytes);
                memcpy(&newBlock1[prevSize], &m_block1[oldSkOff], skBytes);
                LvlLog("SavePak: copy done, newBlock1=%u\n", (unsigned)newBlock1.size());
            } else {
                LvlLog("SavePak: SKIP — string_keys exceeds block1\n");
            }
        } else {
            LvlLog("SavePak: SKIP — oldSkOff out of bounds\n");
        }
        LvlLog("SavePak: string_keys at %u (%u bytes)\n", newSkOff, skBytes);
    }

    // 4. Collision — FULL REBUILD INSERT + SHIFT with comprehensive cross-reference fixups.
    //    Insert N*80 bytes at end of HkShapeInfo array, shift everything after,
    //    then fix ALL Block1 absolute offset fields in ALL info structs.
    //    This is the same approach as the Rust parser's full rebuild but applied
    //    incrementally — ALL cross-references are updated so the file stays valid
    //    across unlimited saves.
    if (!m_pendingCollisions.empty()) {
        uint32_t oldHkBase  = newHdr.hk_shape_info_offset;
        uint32_t oldHkCount = newHdr.hk_shape_info_num;
        uint32_t insertPoint = oldHkBase + oldHkCount * 80;
        uint32_t newEntryCount = 0;

        LvlLog("SavePak: collision full rebuild: %u pending, insertPoint=%u\n",
               (unsigned)m_pendingCollisions.size(), insertPoint);

        // A. Find collision data write position (scan existing HkShapeInfo entries)
        uint32_t dataWritePos = 0;
        for (uint32_t hi = 0; hi < oldHkCount; ++hi) {
            uint32_t eOff = oldHkBase + hi * 80;
            if (eOff + 80 > (uint32_t)newBlock1.size()) break;
            uint32_t kind;
            memcpy(&kind, &newBlock1[eOff + 32], 4);
            if (kind == 6) {
                uint32_t vn,vo,tn,io2,ts,to2;
                memcpy(&vn,&newBlock1[eOff+64],4); memcpy(&vo,&newBlock1[eOff+68],4);
                memcpy(&tn,&newBlock1[eOff+72],4); memcpy(&io2,&newBlock1[eOff+76],4);
                memcpy(&ts,&newBlock1[eOff+56],4); memcpy(&to2,&newBlock1[eOff+60],4);
                uint32_t ve=vo+vn*12, ie=io2+tn*3*2, te=to2+ts;
                if(ve>dataWritePos)dataWritePos=ve;
                if(ie>dataWritePos)dataWritePos=ie;
                if(te>dataWritePos)dataWritePos=te;
            } else if (kind == 5) {
                uint32_t an,ao,bn,bo;
                memcpy(&an,&newBlock1[eOff+40],4); memcpy(&ao,&newBlock1[eOff+44],4);
                memcpy(&bn,&newBlock1[eOff+48],4); memcpy(&bo,&newBlock1[eOff+52],4);
                uint32_t ae=ao+an*16, be=bo+bn*12;
                if(ae>dataWritePos)dataWritePos=ae;
                if(be>dataWritePos)dataWritePos=be;
            }
        }
        dataWritePos = (dataWritePos + 15) & ~15u;

        // B. Write collision data (verts/indices/MOPP) into free space + build entries
        std::vector<uint8_t> newEntries;
        for (size_t ci = 0; ci < m_pendingCollisions.size(); ++ci) {
            const PendingCollisionMesh& pcm = m_pendingCollisions[ci];
            uint32_t numVerts = (uint32_t)(pcm.verts.size() / 3);
            uint32_t numTris  = (uint32_t)(pcm.indices.size() / 3);
            if (numVerts == 0 || numTris == 0) continue;

            // Bounds check
            uint32_t needed = numVerts*12 + numTris*3*2 + (uint32_t)pcm.moppData.size() + 64;
            if (dataWritePos + needed > (uint32_t)newBlock1.size()) {
                LvlLog("SavePak: collision[%u] skipped — would exceed Block1 bounds\n", (unsigned)ci);
                continue;
            }

            uint32_t vertsOff = dataWritePos;
            memcpy(&newBlock1[vertsOff], &pcm.verts[0], numVerts*12);
            dataWritePos += numVerts*12;

            uint32_t indsOff = dataWritePos;
            memcpy(&newBlock1[indsOff], &pcm.indices[0], numTris*3*2);
            dataWritePos += numTris*3*2;
            dataWritePos = (dataWritePos+3)&~3u;

            uint32_t treeOff = dataWritePos;
            uint32_t treeSize = (uint32_t)pcm.moppData.size();
            if (treeSize > 0) {
                memcpy(&newBlock1[treeOff], &pcm.moppData[0], treeSize);
                dataWritePos += treeSize;
            }
            dataWritePos = (dataWritePos+15)&~15u;

            // Compute AABB
            float minB[3]={1e30f,1e30f,1e30f}, maxB[3]={-1e30f,-1e30f,-1e30f};
            for (uint32_t vi=0; vi<numVerts; ++vi) {
                float vx=pcm.verts[vi*3], vy=pcm.verts[vi*3+1], vz=pcm.verts[vi*3+2];
                if(vx<minB[0])minB[0]=vx; if(vy<minB[1])minB[1]=vy; if(vz<minB[2])minB[2]=vz;
                if(vx>maxB[0])maxB[0]=vx; if(vy>maxB[1])maxB[1]=vy; if(vz>maxB[2])maxB[2]=vz;
            }
            float aabbMin[3]={minB[0]-0.05f,minB[1]-0.05f,minB[2]-0.05f};
            float maxExt=0;
            for(int a=0;a<3;++a){float e2=maxB[a]-minB[a];if(e2>maxExt)maxExt=e2;}
            float treeScale=254.f*256.f*256.f/(maxExt+0.1f);

            // Build 80-byte HkShapeInfo entry (offsets are PRE-SHIFT — will be fixed below)
            uint8_t entry[80]; memset(entry,0,80);
            memcpy(entry+0, pcm.translation,16);
            memcpy(entry+16,pcm.rotation,16);
            uint32_t kind=6; memcpy(entry+32,&kind,4); memcpy(entry+36,&pcm.key,4);
            memcpy(entry+40,aabbMin,12); memcpy(entry+52,&treeScale,4);
            memcpy(entry+56,&treeSize,4); memcpy(entry+60,&treeOff,4);
            memcpy(entry+64,&numVerts,4); memcpy(entry+68,&vertsOff,4);
            memcpy(entry+72,&numTris,4);  memcpy(entry+76,&indsOff,4);
            newEntries.insert(newEntries.end(), entry, entry+80);
            newEntryCount++;

            LvlLog("SavePak: collision[%u]: %u verts %u tris data@%u mopp=%u\n",
                   (unsigned)ci, numVerts, numTris, vertsOff, treeSize);
        }

        if (newEntryCount > 0) {
            uint32_t shiftAmount = newEntryCount * 80;

            // C. INSERT: grow newBlock1 + shift data after insertPoint forward
            uint32_t oldSize = (uint32_t)newBlock1.size();
            newBlock1.resize(oldSize + shiftAmount);
            memmove(&newBlock1[insertPoint + shiftAmount],
                    &newBlock1[insertPoint],
                    oldSize - insertPoint);

            // D. Write new HkShapeInfo entries at insertPoint.
            //    Their collision data offsets are pre-shift values — they'll be fixed
            //    by FixAllBlockOffsets below (data was past insertPoint, so gets +shift).
            memcpy(&newBlock1[insertPoint], &newEntries[0], newEntries.size());

            // E. Update PAK header: all Block1-internal offsets past insertPoint
            #define SHIFT_IF_PAST(field) if (newHdr.field > insertPoint) newHdr.field += shiftAmount
            SHIFT_IF_PAST(obja_offset);
            SHIFT_IF_PAST(obj0_offset);
            SHIFT_IF_PAST(model_info_offset);
            SHIFT_IF_PAST(buffer_info_offset);
            SHIFT_IF_PAST(mat1_offset);
            SHIFT_IF_PAST(mat2_offset);
            SHIFT_IF_PAST(mat3_offset);
            SHIFT_IF_PAST(mat4_offset);
            SHIFT_IF_PAST(mat_extra_offset);
            SHIFT_IF_PAST(shape_info_offset);
            // hk_shape_info_offset stays — new entries appended to end of array
            SHIFT_IF_PAST(hk_constraint_data_offset);
            SHIFT_IF_PAST(vbuff_info_offset);
            SHIFT_IF_PAST(ibuff_info_offset);
            SHIFT_IF_PAST(texture_info_offset);
            SHIFT_IF_PAST(animation_info_offset);
            SHIFT_IF_PAST(hk_constraint_info_offset);
            SHIFT_IF_PAST(effect_info_offset);
            SHIFT_IF_PAST(pfield_info_offset);
            SHIFT_IF_PAST(gfx_block_info_offset);
            SHIFT_IF_PAST(animation_block_info_offset);
            SHIFT_IF_PAST(foliage_info_offset);
            SHIFT_IF_PAST(radiosity_vals_info_offset);
            SHIFT_IF_PAST(sub_blocks1_offset);
            SHIFT_IF_PAST(string_keys_offset);
            #undef SHIFT_IF_PAST

            newHdr.hk_shape_info_num = oldHkCount + newEntryCount;

            // F. COMPREHENSIVE FIXUP: fix ALL Block1 absolute offset fields in
            //    ALL info structs — ModelInfo, BufferInfo, ShapeInfo, HkShapeInfo,
            //    HkConstraintInfo, EffectInfo, GFXBlockInfo, FoliageInfo,
            //    RadiosityValsInfo, PFieldInfo, and model sub-data vbuff/ibuff refs.
            FixAllBlockOffsets(newBlock1, newHdr, insertPoint, shiftAmount, oldHkCount);

            LvlLog("SavePak: FULL REBUILD — inserted %u bytes at %u, fixed ALL cross-refs, hk_num=%u\n",
                   shiftAmount, insertPoint, newHdr.hk_shape_info_num);
        }
    }
    #endif // -------- OLD CODE END --------

    // ---- Update Block1 size ----
    newHdr.block1_size = (uint32_t)newBlock1.size();

    // ---- Build Block2 with updated block2_offsets ----
    UpdateSaveProgress(35, "Rebuilding Block2 offsets...");
    std::vector<uint32_t> newBlock2Offsets;
    BuildBlock2Offsets(newBlock1, newHdr, newBlock2Offsets);
    LvlLog("SavePak: block2_offsets rebuilt: %u entries (was %u)\n",
           (unsigned)newBlock2Offsets.size(), m_pakHeader.block2_offsets_num);

    std::vector<uint8_t> newBlock2;
    {
        // Copy sub_blocks2 data (everything before block2_offsets_offset)
        uint32_t b2oOff = m_pakHeader.block2_offsets_offset;
        if (b2oOff > 0 && b2oOff <= (uint32_t)m_block2.size()) {
            newBlock2.assign(m_block2.begin(), m_block2.begin() + b2oOff);
        } else {
            newBlock2 = m_block2;
        }
        // Append new block2_offsets
        newHdr.block2_offsets_offset = (uint32_t)newBlock2.size();
        newHdr.block2_offsets_num = (uint32_t)newBlock2Offsets.size();
        for (size_t i = 0; i < newBlock2Offsets.size(); ++i) {
            uint32_t v = newBlock2Offsets[i];
            newBlock2.insert(newBlock2.end(), (uint8_t*)&v, (uint8_t*)&v + 4);
        }
        newHdr.block2_size = (uint32_t)newBlock2.size();
        LvlLog("SavePak: newBlock2=%u bytes (was %u)\n",
               (unsigned)newBlock2.size(), (unsigned)m_block2.size());
    }

    // ---- Compress Block1 and Block2 ----
    UpdateSaveProgress(40, "Compressing blocks...");
    LvlLog("SavePak: compressing block1 (%u bytes)...\n", (unsigned)newBlock1.size());
    mz_ulong compBound = mz_compressBound((mz_ulong)newBlock1.size());
    std::vector<uint8_t> compBlock1(compBound);
    mz_ulong compSize = compBound;
    int ret = mz_compress2(&compBlock1[0], &compSize, &newBlock1[0],
                           (mz_ulong)newBlock1.size(), MZ_DEFAULT_COMPRESSION);
    if (ret != MZ_OK) {
        m_error = "Block1 compression failed";
        CloseSaveProgress();
        return false;
    }
    compBlock1.resize(compSize);
    newHdr.block1_size_comp = (uint32_t)compSize;
    LvlLog("SavePak: block1 compressed %u -> %u bytes\n",
           (unsigned)newBlock1.size(), (unsigned)compSize);

    LvlLog("SavePak: compressing block2 (%u bytes)...\n", (unsigned)newBlock2.size());
    mz_ulong comp2Bound = mz_compressBound((mz_ulong)newBlock2.size());
    std::vector<uint8_t> compBlock2(comp2Bound);
    mz_ulong comp2Size = comp2Bound;
    ret = mz_compress2(&compBlock2[0], &comp2Size, &newBlock2[0],
                       (mz_ulong)newBlock2.size(), MZ_DEFAULT_COMPRESSION);
    if (ret != MZ_OK) {
        m_error = "Block2 compression failed";
        CloseSaveProgress();
        return false;
    }
    compBlock2.resize(comp2Size);
    newHdr.block2_size_comp = (uint32_t)comp2Size;
    LvlLog("SavePak: block2 compressed %u -> %u bytes\n",
           (unsigned)newBlock2.size(), (unsigned)comp2Size);

    // ---- Build new PAK file (piece by piece) ----
    // Layout: [header+anims] [Block1] [gap] [Block2] [gap] [strings] [block_a]
    uint32_t oldB1Off = m_pakHeader.block1_offset;
    uint32_t oldB1CompSz = m_pakHeader.block1_size_comp;
    uint32_t oldB1End = oldB1Off + oldB1CompSz;

    uint32_t oldB2Off = m_pakHeader.block2_offset;
    uint32_t oldB2CompSz = m_pakHeader.block2_size_comp;
    if (oldB2CompSz == 0) oldB2CompSz = m_pakHeader.block2_size; // uncompressed
    uint32_t oldB2End = oldB2Off + oldB2CompSz;

    std::vector<uint8_t> newPak;
    newPak.reserve(m_pakRaw.size() + 1024 * 1024);

    // 1. Copy everything before Block1 (header + animation blocks)
    newPak.insert(newPak.end(), m_pakRaw.begin(), m_pakRaw.begin() + oldB1Off);
    // 2. Write compressed Block1
    newHdr.block1_offset = (uint32_t)newPak.size();
    newPak.insert(newPak.end(), compBlock1.begin(), compBlock1.end());
    // 3. Copy gap between Block1 and Block2 (padding)
    if (oldB1End < oldB2Off) {
        newPak.insert(newPak.end(), m_pakRaw.begin() + oldB1End, m_pakRaw.begin() + oldB2Off);
    }
    // 4. Write compressed Block2
    newHdr.block2_offset = (uint32_t)newPak.size();
    newPak.insert(newPak.end(), compBlock2.begin(), compBlock2.end());
    // 5. Copy everything after Block2 (padding + strings + block_a)
    //    Adjust file-level offsets for whatever shifted
    int32_t afterB2Delta = (int32_t)newPak.size() - (int32_t)oldB2End;
    if (oldB2End < (uint32_t)m_pakRaw.size()) {
        newPak.insert(newPak.end(), m_pakRaw.begin() + oldB2End, m_pakRaw.end());
    }

    // 6. Fix up header — write computed header + adjust file-level offsets
    PakHeader* hdr = reinterpret_cast<PakHeader*>(&newPak[0]);
    *hdr = newHdr;
    // block1_offset and block2_offset already set above
    // strings_offset and block_a_offset come after Block2, shift by afterB2Delta
    if (afterB2Delta != 0) {
        if (m_pakHeader.strings_offset >= oldB2End)
            hdr->strings_offset = (uint32_t)((int32_t)m_pakHeader.strings_offset + afterB2Delta);
        if (m_pakHeader.block_a_offset >= oldB2End)
            hdr->block_a_offset = (uint32_t)((int32_t)m_pakHeader.block_a_offset + afterB2Delta);
    }

    // ---- Append new strings to pak_strings at end of PAK ----
    if (!newStrings.empty() && hdr->strings_offset > 0) {
        uint32_t oldStrOff = hdr->strings_offset;
        uint32_t oldStrSize = hdr->strings_size;
        uint32_t oldStrNum = hdr->strings_num;

        std::vector<uint8_t> fullStrData;
        if (oldStrOff + oldStrSize <= newPak.size()) {
            fullStrData.assign(&newPak[oldStrOff], &newPak[oldStrOff + oldStrSize]);
        }
        for (size_t si = 0; si < newStrings.size(); ++si) {
            uint32_t slen = (uint32_t)newStrings[si].size();
            fullStrData.insert(fullStrData.end(),
                reinterpret_cast<const uint8_t*>(&slen),
                reinterpret_cast<const uint8_t*>(&slen) + 4);
            fullStrData.insert(fullStrData.end(),
                newStrings[si].begin(), newStrings[si].end());
        }
        uint32_t newStrOff = (uint32_t)newPak.size();
        newPak.insert(newPak.end(), fullStrData.begin(), fullStrData.end());
        hdr = reinterpret_cast<PakHeader*>(&newPak[0]);
        hdr->strings_offset = newStrOff;
        hdr->strings_size = (uint32_t)fullStrData.size();
        hdr->strings_num = oldStrNum + (uint32_t)newStrings.size();
        LvlLog("SavePak: pak_strings rebuilt at %u (%u strings)\n", newStrOff, hdr->strings_num);
    }

    // ---- Write to file ----
    FILE* f = fopen(outputPath.c_str(), "wb");
    if (!f) {
        m_error = "Cannot open output file: " + outputPath;
        CloseSaveProgress();
        return false;
    }
    fwrite(&newPak[0], 1, newPak.size(), f);
    fclose(f);
    UpdateSaveProgress(70, "Writing PAK file...");
    LvlLog("SavePak: wrote %u bytes to %s\n", (unsigned)newPak.size(), outputPath.c_str());

    // Update m_pakRaw and m_pakHeader from the saved data so subsequent
    // saves work from the correct base. No Unload/Load — keep current state.
    m_pakRaw.swap(newPak);
    if (m_pakRaw.size() >= sizeof(PakHeader)) {
        memcpy(&m_pakHeader, &m_pakRaw[0], sizeof(PakHeader));
    }
    m_block1.swap(newBlock1);
    m_block2.swap(newBlock2);
    m_pakPath = outputPath;
    // Update cached sub-block payloads and string_keys for next save
    for (size_t i = 0; i < m_subBlocks1.size(); ++i) {
        uint32_t sbOff = newHdr.sub_blocks1_offset;
        if (sbOff + 16 + (uint32_t)m_subBlocks1.size() * 12 <= (uint32_t)m_block1.size()) {
            uint32_t entOff = sbOff + 16 + (uint32_t)i * 12;
            uint32_t relOff, sz;
            memcpy(&relOff, &m_block1[entOff + 4], 4);
            memcpy(&sz,     &m_block1[entOff + 8], 4);
            uint32_t absOff = sbOff + relOff;
            m_subBlocks1[i].size = sz;
            m_subBlocks1[i].absOffset = absOff;
        }
    }
    // string_keys offset updated via m_pakHeader (no separate cache)
    // Update GameObjs offset/size from the rebuilt sub-blocks table
    {
        static const uint32_t KEY_GO = LotrHashString("Level");
        uint32_t sbOff = newHdr.sub_blocks1_offset;
        uint32_t sbCount = (uint32_t)m_subBlocks1.size();
        for (uint32_t i = 0; i < sbCount; ++i) {
            if (m_subBlocks1[i].key == KEY_GO) {
                m_gameObjsBlock1Offset = m_subBlocks1[i].absOffset;
                m_gameObjsBlock1Size = m_subBlocks1[i].size;
                break;
            }
        }
    }

    // Re-parse Phase 1-4 data from the new Block1 (for subsequent saves)
    UpdateSaveProgress(80, "Re-parsing Block1 data...");
    LvlLog("SavePak: re-parsing Phase 1...\n");
    {
        std::string parseErr;
        m_parsedData.Clear();
        ParseBlock1Data(&m_block1[0], (uint32_t)m_block1.size(),
                        m_pakHeader, m_parsedData, parseErr);
        LvlLog("SavePak: re-parsing Phase 2...\n");
        const uint8_t* b2ptr = m_block2.empty() ? NULL : &m_block2[0];
        ParseBlock1Phase2(&m_block1[0], (uint32_t)m_block1.size(),
                          b2ptr, (uint32_t)m_block2.size(),
                          m_pakHeader, m_parsedData, parseErr);
        LvlLog("SavePak: re-parsing Phase 3...\n");
        ParseBlock1Phase3(&m_block1[0], (uint32_t)m_block1.size(),
                          m_pakHeader, m_parsedData, parseErr);
        // Phase 4 re-parse skipped after sequential rebuild — slot scanning
        // can hang on relocated model data. Re-parsed on next Load().
        LvlLog("SavePak: Phase 4 re-parse skipped\n");

        // Re-parse generic display entries so the viewer sees new collision shapes
        LvlLog("SavePak: re-parsing generic HkShapeInfos...\n");
        m_hkShapeInfos.clear();
        ParseGenericB1(m_pakHeader.hk_shape_info_offset, m_pakHeader.hk_shape_info_num,
                       m_pakHeader.hk_shape_info_size, 0, false, 32, 36, m_hkShapeInfos);
        LvlLog("SavePak: HkShapeInfos re-parsed: %u entries\n", (uint32_t)m_hkShapeInfos.size());

        LvlLog("SavePak: re-parse complete\n");
    }

    // Restore BIN raw data
    m_binRaw.swap(savedBinRaw);

    UpdateSaveProgress(100, "Save complete!");
    LvlLog("SavePak: === REBUILD COMPLETE ===\n");
    CloseSaveProgress();
    return true;
}

} // namespace ZeroEngine
