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
#include <cstddef>
#include <set>
#include <algorithm>
#include <stdarg.h>
#include <windows.h>
#include <commctrl.h>

// ─────────────────────────────────────────────────────────────────────────
//  Endian-swap helper INLINES (definitions, not forward decls)
// ─────────────────────────────────────────────────────────────────────────
// These live at the top of the file, BEFORE ParseStringTable which is the
// first function that actually calls SwapU32 (on the BE length prefix).
//
// First try at this used `static inline` FORWARD DECLS here and kept the
// full definitions further down above ParseBin. VS2005 hated that combo -
// C2129 "static function declared but not defined" fired from deep inside
// <xmemory> during some template instantiation, looking at the forward
// decl and unable to see the body at the point of expansion. Classic
// VS2005 horseshit. Full definitions here, period, no forward decl
// dance. The giant comment block explaining WHY we have these helpers
// still lives above ParseBin where it provides context for the swap
// logic in ParseBin/ParsePak - those comments stay put.
static inline uint16_t SwapU16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t SwapU32(uint32_t v) {
    return ((v >> 24) & 0x000000FFu) |
           ((v >>  8) & 0x0000FF00u) |
           ((v <<  8) & 0x00FF0000u) |
           ((v << 24) & 0xFF000000u);
}
static inline uint64_t SwapU64(uint64_t v) {
    return ((uint64_t)SwapU32((uint32_t)(v & 0xFFFFFFFFull)) << 32) |
           (uint64_t)SwapU32((uint32_t)(v >> 32));
}
static inline float SwapF32(float v) {
    uint32_t u; memcpy(&u, &v, 4); u = SwapU32(u); memcpy(&v, &u, 4); return v;
}
static void SwapU32ArrayInPlace(void* data, size_t byteSize) {
    uint32_t* p = (uint32_t*)data;
    size_t n = byteSize / 4;
    for (size_t i = 0; i < n; ++i) p[i] = SwapU32(p[i]);
}

// Swap N consecutive u16 values in place (for TextureInfo's width/height/
// depth/levels quartet and HkConstraintInfo's bones_num/bone_order_num pair).
// Byte-wise so alignment is never a question.
static void SwapU16ArrayInPlace(void* data, size_t count) {
    uint16_t* p = (uint16_t*)data;
    for (size_t i = 0; i < count; ++i) p[i] = SwapU16(p[i]);
}

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
    m_binTextureAssets.clear();
    m_embeddedWeapons.clear();
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
    // NOTE: m_deletedGuids and m_fieldEdits are intentionally NOT cleared here.
    // The save pipeline uses "always from original PAK + accumulated deltas" design.
    // These must persist across reloads within the same editing session so that
    // subsequent saves from the original PAK still apply all changes.
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

    // Read length-prefixed strings: [u32 length][length bytes] repeated stringsNum times.
    //
    // On big-endian files (Xbox 360 / PS3) the length prefix is stored in
    // reversed byte order, so a string with actual length 12 reads as
    // 0x0C000000 = 201326592 bytes if we slurp it raw. Three things then
    // go wrong in rapid succession:
    //   1. `p + len > end` uses 32-bit pointer arithmetic on a 32-bit build.
    //      `p + 201326592` can WRAP AROUND to a smaller number than end,
    //      so the overflow check fails to trigger and we plow forward.
    //   2. std::string construction reads 200MB of wherever-the-fuck
    //      memory happens to be past our buffer.
    //   3. Access violation, crash, silent death, user sees "cursor
    //      switches to loading then program exits."
    // This cost me a late-night loop of staring at a broken viewport
    // before I wired SwapU32 into the length read. Do not remove the
    // conditional swap below. Endian-aware length prefix reads are the
    // minimum fucking viable safety net on every multi-platform binary
    // format Pandemic ever shipped.
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
        if (m_bigEndian) len = SwapU32(len);
        p += 4;
        // Belt-and-suspenders bounds check. The pointer-arithmetic overflow
        // lecture above is why this uses size_t arithmetic and compares
        // against (end - p), not `p + len > end`.
        if ((size_t)(end - p) < (size_t)len) {
            LvlLog("  ParseStringTable: TRUNCATED at entry %u/%u (need %u bytes for string, have %u)\n",
                   i, stringsNum, len, (uint32_t)(end - p));
            break;
        }
        strings.push_back(std::string(reinterpret_cast<const char*>(p), len));
        p += len;
    }
    LvlLog("  ParseStringTable: read %u strings\n", (uint32_t)strings.size());

    // Read string keys: array of stringsNum CRC values. Keys are u32,
    // so on BE we read-and-swap each one before using it as a map key.
    if (keyBase != NULL && keysOffset != 0) {
        const uint32_t* keys = reinterpret_cast<const uint32_t*>(keyBase + keysOffset);
        for (uint32_t i = 0; i < stringsNum && i < static_cast<uint32_t>(strings.size()); ++i) {
            uint32_t k = keys[i];
            if (m_bigEndian) k = SwapU32(k);
            m_crcMap[k] = strings[i];
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

void LevelReader::GetStringsByPrefix(const char* prefix,
                                      std::vector<std::string>& out,
                                      const char* const* skipSuffixes) const
{
    if (!prefix || !*prefix) return;
    size_t plen = strlen(prefix);
    // Lower-case the prefix once for case-insensitive compare.
    char prefLow[64];
    if (plen >= sizeof(prefLow)) plen = sizeof(prefLow) - 1;
    for (size_t i = 0; i < plen; ++i) {
        char c = prefix[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        prefLow[i] = c;
    }
    prefLow[plen] = 0;

    for (std::map<uint32_t, std::string>::const_iterator it = m_crcMap.begin();
         it != m_crcMap.end(); ++it)
    {
        const std::string& n = it->second;
        if (n.size() < plen) continue;
        // Lower-case the first plen chars of n for compare.
        bool match = true;
        for (size_t i = 0; i < plen; ++i) {
            char c = n[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != prefLow[i]) { match = false; break; }
        }
        if (!match) continue;
        // Skip strings ending in any of the filtered suffixes.
        if (skipSuffixes) {
            bool skip = false;
            for (int s = 0; skipSuffixes[s] && !skip; ++s) {
                size_t slen = strlen(skipSuffixes[s]);
                if (n.size() < slen) continue;
                if (_stricmp(n.c_str() + n.size() - slen, skipSuffixes[s]) == 0) {
                    skip = true;
                }
            }
            if (skip) continue;
        }
        out.push_back(n);
    }
}

void LevelReader::QueueCustomString(const std::string& s) {
    if (s.empty()) return;
    // Same-session sanity: if we already queued this one, ignore.
    for (size_t i = 0; i < m_queuedCustomStrings.size(); ++i) {
        if (m_queuedCustomStrings[i] == s) return;
    }
    m_queuedCustomStrings.push_back(s);

    // Update m_crcMap immediately so within-session ResolveCrc(crc)
    // returns the user's chosen name instead of "0x12345678" hex
    // fallback. This is what makes the Editor panel show the renamed
    // CrowdItem's key correctly until level reload.
    uint32_t crc = LotrHashString(s.c_str(), -1);
    if (m_crcMap.find(crc) == m_crcMap.end()) {
        m_crcMap[crc] = s;
    }
}

bool LevelReader::HasBinAsset(uint32_t assetKey) const {
    return m_binAssets.count(assetKey) != 0
        || m_binTextureAssets.count(assetKey) != 0;
}

// ═════════════════════════════════════════════════════════════════════════
//  ze_embedded_weapons.json — tiny ad-hoc parser
// ═════════════════════════════════════════════════════════════════════════
//
// Why not JsonLite (the in-tree parser)? JsonLite drags Arena, PathUtils,
// Logging, and VespucciAssert with it. Per the feedback note about
// Vespucci namespace compile traps, those aren't a fight worth picking
// for one small fixed-shape sidecar. The shape is dictated by what
// level_patcher.py writes; if we hand-roll a parser tailored to it we
// pay ~120 LOC and zero new dependencies.
//
// Coverage: objects, arrays, strings (with backslash escapes), numbers.
// Booleans / null are skipped via skipValue() when they show up under
// keys we don't care about. Trailing commas: not emitted by json.dump,
// not accepted here either. UTF-8 strings pass through bytewise.
namespace {

class JsonCursor {
public:
    JsonCursor(const char* begin, const char* end) : p(begin), e(end) {}

    void skipWs() {
        while (p < e) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
            else break;
        }
    }

    char peek() { skipWs(); return p < e ? *p : '\0'; }

    bool consume(char c) {
        skipWs();
        if (p < e && *p == c) { ++p; return true; }
        return false;
    }

    bool parseString(std::string& out) {
        skipWs();
        if (p >= e || *p != '"') return false;
        ++p;
        out.clear();
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                char esc = p[1];
                if      (esc == '"' || esc == '\\' || esc == '/') { out.push_back(esc); p += 2; }
                else if (esc == 'n') { out.push_back('\n'); p += 2; }
                else if (esc == 't') { out.push_back('\t'); p += 2; }
                else if (esc == 'r') { out.push_back('\r'); p += 2; }
                else if (esc == 'b') { out.push_back('\b'); p += 2; }
                else if (esc == 'f') { out.push_back('\f'); p += 2; }
                else { out.push_back(*p); ++p; }  // unsupported escape: pass through raw
            } else {
                out.push_back(*p);
                ++p;
            }
        }
        if (p >= e) return false;
        ++p;  // closing quote
        return true;
    }

    bool parseNumber(double& out) {
        skipWs();
        const char* start = p;
        if (p < e && (*p == '-' || *p == '+')) ++p;
        while (p < e) {
            char c = *p;
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') ++p;
            else break;
        }
        if (p == start) return false;
        std::string tmp(start, p - start);
        out = strtod(tmp.c_str(), 0);
        return true;
    }

    // Skip any value the caller doesn't care about. Handles strings,
    // numbers, objects, arrays, and bare literals (true/false/null).
    bool skipValue() {
        skipWs();
        if (p >= e) return false;
        char c = *p;
        if (c == '"') { std::string s; return parseString(s); }
        if (c == '{' || c == '[') return skipBalanced(c, c == '{' ? '}' : ']');
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) { double d; return parseNumber(d); }
        // bare true/false/null literal
        while (p < e) {
            char x = *p;
            if ((x >= 'a' && x <= 'z') || (x >= 'A' && x <= 'Z')) ++p;
            else break;
        }
        return true;
    }

    bool skipBalanced(char open, char close) {
        if (p >= e || *p != open) return false;
        ++p;  // step past opening brace
        int depth = 1;
        while (p < e && depth > 0) {
            char c = *p;
            if (c == '"') { std::string s; if (!parseString(s)) return false; continue; }
            if (c == open) ++depth;
            else if (c == close) --depth;
            ++p;
        }
        return depth == 0;
    }

private:
    const char* p;
    const char* e;
};

// Pull a fixed-length array of floats. Caller knows the expected count;
// we read up to that many, zero-fill any short, and bail (returning what
// we managed) on parse error so a malformed entry doesn't poison the
// whole sidecar.
bool ParseFloatArray(JsonCursor& c, float* out, int expected) {
    for (int i = 0; i < expected; ++i) out[i] = 0.0f;
    if (!c.consume('[')) return false;
    int i = 0;
    while (c.peek() != ']' && c.peek() != '\0') {
        double v = 0.0;
        if (!c.parseNumber(v)) break;
        if (i < expected) out[i++] = (float)v;
        c.consume(',');
    }
    return c.consume(']');
}

} // anonymous namespace

void LevelReader::LoadEmbeddedWeaponSidecar() {
    if (m_pakPath.empty()) return;

    // dump_dir = pak path with the .PAK / .pak extension stripped.
    std::string dumpDir = m_pakPath;
    size_t dot = dumpDir.rfind('.');
    if (dot != std::string::npos && dot + 4 == dumpDir.size()) {
        char e1 = dumpDir[dot + 1];
        char e2 = dumpDir[dot + 2];
        char e3 = dumpDir[dot + 3];
        bool isPak = ((e1 == 'p' || e1 == 'P') &&
                      (e2 == 'a' || e2 == 'A') &&
                      (e3 == 'k' || e3 == 'K'));
        if (isPak) dumpDir = dumpDir.substr(0, dot);
    }
    std::string sidecar = dumpDir + "/ze_embedded_weapons.json";

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(sidecar, bytes) || bytes.empty()) {
        // Not an error — most levels have no merged CRDs and therefore no
        // sidecar. Log at debug level so it doesn't clutter normal output.
        return;
    }
    LvlLog("  Embedded weapons: parsing %s (%u bytes)\n",
           sidecar.c_str(), (uint32_t)bytes.size());

    JsonCursor c((const char*)&bytes[0], (const char*)&bytes[0] + bytes.size());
    if (!c.consume('{')) {
        LvlLog("  Embedded weapons: top-level is not a JSON object\n");
        return;
    }

    int totalTags = 0;
    while (c.peek() != '}' && c.peek() != '\0') {
        std::string key;
        if (!c.parseString(key)) break;
        if (!c.consume(':')) break;

        if (key != "models") {
            c.skipValue();
            c.consume(',');
            continue;
        }

        if (!c.consume('{')) break;
        while (c.peek() != '}' && c.peek() != '\0') {
            std::string modelName;
            if (!c.parseString(modelName)) break;
            if (!c.consume(':')) break;

            std::vector<EmbeddedWeaponTag> tags;
            if (c.consume('{')) {
                while (c.peek() != '}' && c.peek() != '\0') {
                    std::string mkey;
                    if (!c.parseString(mkey)) break;
                    if (!c.consume(':')) break;

                    if (mkey != "weapons") {
                        c.skipValue();
                        c.consume(',');
                        continue;
                    }

                    if (c.consume('[')) {
                        while (c.peek() != ']' && c.peek() != '\0') {
                            EmbeddedWeaponTag t;
                            t.attach_bone_idx = 0;
                            t.bind_idx        = 0;
                            t.vert_first      = 0;
                            t.vert_count      = 0;
                            t.tri_first       = 0;
                            t.tri_count       = 0;
                            for (int i = 0; i < 3;  ++i) t.offset_xyz[i] = 0.0f;
                            for (int i = 0; i < 3;  ++i) t.offset_ypr[i] = 0.0f;
                            for (int i = 0; i < 16; ++i) t.bind_world[i] = (i % 5 == 0) ? 1.0f : 0.0f;

                            if (c.consume('{')) {
                                while (c.peek() != '}' && c.peek() != '\0') {
                                    std::string wkey;
                                    if (!c.parseString(wkey)) break;
                                    if (!c.consume(':')) break;
                                    double dv = 0;
                                    if      (wkey == "weapon_name")     c.parseString(t.weapon_name);
                                    else if (wkey == "attach_bone")     c.parseString(t.attach_bone);
                                    else if (wkey == "attach_bone_idx") { c.parseNumber(dv); t.attach_bone_idx = (uint32_t)dv; }
                                    else if (wkey == "bind_idx")        { c.parseNumber(dv); t.bind_idx        = (uint32_t)dv; }
                                    else if (wkey == "vert_first")      { c.parseNumber(dv); t.vert_first      = (uint32_t)dv; }
                                    else if (wkey == "vert_count")      { c.parseNumber(dv); t.vert_count      = (uint32_t)dv; }
                                    else if (wkey == "tri_first")       { c.parseNumber(dv); t.tri_first       = (uint32_t)dv; }
                                    else if (wkey == "tri_count")       { c.parseNumber(dv); t.tri_count       = (uint32_t)dv; }
                                    else if (wkey == "offset_xyz")      ParseFloatArray(c, t.offset_xyz, 3);
                                    else if (wkey == "offset_ypr")      ParseFloatArray(c, t.offset_ypr, 3);
                                    else if (wkey == "bind_world")      ParseFloatArray(c, t.bind_world, 16);
                                    else                                c.skipValue();
                                    c.consume(',');
                                }
                                c.consume('}');
                            }
                            tags.push_back(t);
                            c.consume(',');
                        }
                        c.consume(']');
                    }
                    c.consume(',');
                }
                c.consume('}');
            }

            if (!tags.empty()) {
                uint32_t modelCrc = LotrHashString(modelName.c_str(), -1);
                m_embeddedWeapons[modelCrc] = tags;
                LvlLog("    %s (crc=0x%08X): %u weapon tag(s)\n",
                       modelName.c_str(), modelCrc, (uint32_t)tags.size());
                totalTags += (int)tags.size();
            }
            c.consume(',');
        }
        c.consume('}');
        c.consume(',');
    }

    LvlLog("  Embedded weapons: loaded %u model entries, %d total weapon tag(s)\n",
           (uint32_t)m_embeddedWeapons.size(), totalTags);
}

// Read a parsed BinAssetEntry's payload (decompressing if needed) out of
// m_binRaw. Factored from GetBinAssetData so the texture variant uses the
// exact same byte-fetch / zlib path. Returns false on bounds violation or
// decompression failure.
static bool ReadBinEntryFromRaw(const ZeroEngine::BinAssetEntry& ae,
                                const std::vector<uint8_t>& binRaw,
                                std::vector<uint8_t>& outData) {
    if (ae.offset + ae.size > binRaw.size()) return false;
    const uint8_t* src = &binRaw[ae.offset];
    if (ae.size_comp == 0) {
        outData.assign(src, src + ae.size);
        return true;
    }
    outData.resize(ae.size);
    mz_ulong destLen = ae.size;
    if (mz_uncompress(&outData[0], &destLen, src, ae.size_comp) != MZ_OK)
        return false;
    outData.resize(static_cast<size_t>(destLen));
    return true;
}

bool LevelReader::GetBinAssetData(uint32_t assetKey, std::vector<uint8_t>& outData) const {
    // Models first, textures only as a fallback. Most callers want a
    // model handle and this preserves the historical behavior for keys
    // that exist in exactly one section. The collision case (same key,
    // both sections) used to lose the model to whichever handle the BIN
    // wrote last — now the model wins for this entry point and texture
    // loaders call GetBinTextureData explicitly.
    std::map<uint32_t, BinAssetEntry>::const_iterator it = m_binAssets.find(assetKey);
    if (it == m_binAssets.end()) {
        it = m_binTextureAssets.find(assetKey);
        if (it == m_binTextureAssets.end()) return false;
    }
    if (!ReadBinEntryFromRaw(it->second, m_binRaw, outData)) return false;

    // ── STAGE 3A.2: BIN ASSET ENDIAN SWAP — NOW CALLER'S JOB ────────────
    //
    // Previously we u32-blob-swapped EVERY BIN asset byte on BE files
    // here. That made float vertex positions readable, but it scrambled
    // u16 triangle index pairs (each adjacent pair of indices traded
    // places), and corrupted DXT texture endpoints, and mangled any
    // asset whose bytes were not a pure array of u32s.
    //
    // User saw the downstream symptom as "terrain meshes do not exist,
    // wireframe shows atoms scattered across empty space". That is a
    // pair-scrambled triangle topology rendering as depth-degenerate
    // triangles (mostly zero area because adjacent indices cross-paired).
    //
    // Fix: GetBinAssetData returns raw bytes now. The caller knows
    // WHAT this asset is (vertex buffer for a model, texture, etc.)
    // and swaps the right bytes the right way.
    //
    //   - Model loader (LevelScene::loadModelFromBinary): walks this
    //     model's VBuffInfo / IBuffInfo entries, surgically swaps each
    //     vertex region as u32 and each index region as u16.
    //   - Texture loader (LevelScene::getOrLoadTexture): DXT blocks are
    //     mostly endian-neutral; a future pass will swap 16-bit color
    //     endpoints inside each DXT block if textures come out wrong.
    //   - Everything else: leave alone until we know what it is.
    //
    // If the caller does NOT do its own swap on a BE file, they get
    // raw BE bytes and downstream breakage is on them.

    return true;
}

// Texture-only lookup. Bypasses the model map entirely so a model that
// shares this key (user-baked atlas reusing the model's name, original
// BIN cases the engine itself ships with) cannot intercept the read.
// getOrLoadTexture calls this; the model path stays on GetBinAssetData.
bool LevelReader::GetBinTextureData(uint32_t assetKey, std::vector<uint8_t>& outData) const {
    std::map<uint32_t, BinAssetEntry>::const_iterator it = m_binTextureAssets.find(assetKey);
    if (it == m_binTextureAssets.end()) return false;
    return ReadBinEntryFromRaw(it->second, m_binRaw, outData);
}

// ═════════════════════════════════════════════════════════════════════════
//  THE MOTHERFUCKING ENDIAN-SWAP RIG (Xbox 360 / PS3 console support)
// ═════════════════════════════════════════════════════════════════════════
//
//                          RIDE OR FUCKING DIE, KING.
//
//            -- 2026-04-24, somewhere between Stage 2 and Stage 3,
//               when we chose to byte-flip an entire 2008 Xbox 360
//               DLC prototype of a cancelled Discworld game instead
//               of banking a header-only archaeology win and going
//               home.
//
//               Shakespeare is dead. We are not. The triangles were
//               in motherfucking hell and we rode down to bring them
//               back to the observable universe. This comment is a
//               receipt. If you read it a decade from now and laugh,
//               that's the only legacy we need.
//
// ═════════════════════════════════════════════════════════════════════════
//
// Pandemic shipped this game on THREE goddamn platforms: PC (x86, little-
// endian, sane), Xbox 360 (PowerPC, big-endian, console), and PS3 (Cell,
// also big-endian, also console). Same struct layouts byte-for-byte, but
// the multi-byte fields are stored in whichever order the compiling CPU
// thought was "native". So a PC ModelInfo and a 360 ModelInfo are the
// same 256 bytes, but every u32 in them is BACKWARDS from each other.
// Read a 360 file with a PC parser and every count is in the billions,
// every offset points into outer space, the dereference faults, the
// process dies in the cradle.
//
// This C++ parser was written PC-only because the original assumption
// was Pandemic would never let a console build leak. They did. Their
// 2008 Discworld DLC prototype - an UNRELEASED Pratchett-universe game
// they were quietly building inside the LOTRC engine - sits on the
// internet right now in extracted Xbox 360 STFS form, big-endian as
// fuck. The Rust parser already handles all three platforms via
// zerocopy derive-macros. Our C++ parser had to be retrofitted by
// hand. Fuck EA (always EA, FOREVER EA) for shutting Pandemic down
// in 2009 before the team could ship unified tooling that would have
// made this work redundant.
//
// Stage 1 of the retrofit (this block + ParseBin/ParsePak edits):
// detect endian at the BIN magic read, byte-swap BinHeader, PakHeader,
// and BinAssetHandle in place so every existing memcpy/cast downstream
// sees little-endian-native values. Asset-table entries (ModelInfo,
// TextureInfo, AnimationInfo, the rest of that fucking zoo) are NOT
// yet swapped - those land in Stage 2. So 360 files get past the magic
// check, get the level-content counts logged, and then HARD STOP at
// the wall in ParsePak before anything dereferences a still-backwards
// offset. Progress, not victory. The wall comes out when Stage 2 is
// in.
//
// If you ever need to add a new platform variant (PSP? Wii? Some
// unannounced fourth Pandemic SKU we have not seen yet?), the magic-
// byte detection in ParseBin is where you add the third branch.
// Headers will swap fine. Per-platform field-layout deltas (VBuffInfo,
// IBuffInfo, Mat3, BufferInfo) need separate handling - mirror what
// the Rust parser does with skipPC / skipXBOX / skipPS3 attributes.
//
// Swap helper INLINES live at the top of this file (before
// ParseStringTable, which is the first function that actually calls
// SwapU32). That placement is non-negotiable on VS2005 - forward-decls
// with static inline trigger C2129 from deep inside <xmemory> template
// expansion. See the block at file-scope line ~35 for the definitions.
// The detail comment about `SwapU32ArrayInPlace` being a data-corruption
// bomb if anyone adds a non-u32 field to BinHeader/PakHeader is here:
//
// ONLY SAFE for structs whose fields are ALL 32-bit (u32/i32/f32) AND
// have ZERO padding. BinHeader and PakHeader both qualify - verified
// by the ZERO_STATIC_ASSERT pinning their sizes to exactly N*4 bytes.
// If some future motherfucker adds a u16, a u64, an embedded sub-struct,
// or god forbid a bool to either header, SwapU32ArrayInPlace TURNS INTO
// A DATA CORRUPTION BOMB. It will run, the swap will "succeed", every
// field after the misaligned spot will be silently scrambled. You will
// not notice for hours. Then a sub-block offset points into Mordor,
// parser dereferences it, late nights staring at stack traces that
// look like keyboard mash. If the static_assert fires when you change
// the struct, you ALSO need a hand-written per-field swap function -
// there is no runtime check for "all u32, no padding". The asserts on
// size are the only fucking guard. Don't eat it.

// ═════════════════════════════════════════════════════════════════════════
//  STAGE 2: THE MOTHERFUCKING ASSET-TABLE SWAP PASS
// ═════════════════════════════════════════════════════════════════════════
//
// This is the function that takes a backwards-ass Xbox 360 / PS3 Block1
// and beats it into little-endian submission so every downstream reader
// can pretend the file always came off a PC disc. Called ONCE, right
// after Block1 decompression. BEFORE any B1Array<T> cast, BEFORE any
// ParseGenericB1 read, BEFORE anyone touches the asset data. Miss that
// ordering and half the parser reads swapped bytes and the other half
// reads raw bytes and everything goes to shit.
//
// What we are flipping:
//
//   1. PURE-U32 STRUCT TABLES - swap every u32 in every entry. The
//      friendly cases - ModelInfo, AnimationInfo, EffectInfo,
//      GFXBlockInfo, ObjA, Obj0, AnimationBlockInfo. u32 swap covers
//      f32 fields too because floats are just 4 bytes and swapping
//      them byte-for-byte is the same as swapping them as u32.
//
//   2. MIXED STRUCTS - hand-rolled swap for the few assholes with u16
//      or u8 fields mixed in. TextureInfo has 12 u32 + 4 u16 + 16 u8,
//      and if you blind-u32-swap the tail you end up swapping two u16s
//      as one u32 which produces different garbage than the swap they
//      actually need. HkConstraintInfo has the same trap with its
//      bones_num / bone_order_num pair.
//
//   3. GENERIC PER-STRIDE TABLES - entries are variable-size structs
//      we parse via ParseGenericB1 at known field offsets. Stride
//      lives in the PakHeader as *_size. Most are all-u32 (ShapeInfo,
//      HkShapeInfo, HkConstraintData, Mat1/2/4, MatExtra, PFieldInfo).
//      The PLATFORM-VARIANT ones (VBuffInfo, IBuffInfo, Mat3,
//      BufferInfo) have DIFFERENT FIELD LAYOUTS on 360 vs PC - u32
//      swap covers byte-order but NOT the field ordering differences.
//      Those are Stage 3 territory and expected to half-misbehave
//      until then. Do not lose sleep when vbuff data looks fucked.
//
//   4. SUB-BLOCK TABLES - Block1 has sub_blocks1 embedded at
//      h.sub_blocks1_offset. Format: [u32 header_magic][u32 block_num]
//      [(u32 key, u32 offset, u32 size) * block_num]. All u32, all
//      swappable. block2_offsets is also a packed u32 array in
//      Block1. Flip both.
//
// What we are NOT touching: raw strings (char bytes, no swap needed),
// the string_keys CRC array in Block2-land (parsed elsewhere),
// mesh vertex/index data (raw blobs), collision MOPP data (Havok
// bytecode, endian-neutral in its own weird way), radiosity vals
// blobs (Stage 3+), foliage val pairs (Stage 3+, those have u16/i16
// fields we parse individually).
//
// Swap happens ONCE per level load. If you ever re-decompress Block1
// without tearing down and re-loading, you must re-call this or half
// the parser will blow up. Don't do that.
void LevelReader::SwapBlock1AssetTablesBE() {
    if (!m_bigEndian || m_block1.empty()) return;

    const size_t bsz = m_block1.size();
    uint8_t* base = &m_block1[0];

    // Inline helper: swap `count` entries of `stride` bytes each as
    // u32 array, bounds-checked. Zero-count or zero-stride skips clean.
    #define SWAP_TABLE_U32(offset, stride, count) do { \
        if ((stride) != 0 && (count) != 0 && (offset) != 0) { \
            uint64_t total = (uint64_t)(stride) * (uint64_t)(count); \
            if ((offset) + total <= bsz) { \
                SwapU32ArrayInPlace(base + (offset), (size_t)total); \
            } else { \
                LvlLog("SwapBlock1: OUT-OF-BOUNDS skip - offset=%u stride=%u count=%u would read past block1 (size=%u). Something lied in the header, not fucking touching this one.\n", \
                       (offset), (stride), (count), (uint32_t)bsz); \
            } \
        } \
    } while(0)

    const PakHeader& h = m_pakHeader;

    // Typed asset tables (known struct sizes)
    SWAP_TABLE_U32(h.model_info_offset, sizeof(ModelInfo), h.model_info_num);
    SWAP_TABLE_U32(h.animation_info_offset, sizeof(AnimationInfo), h.animation_info_num);
    SWAP_TABLE_U32(h.effect_info_offset, sizeof(EffectInfo), h.effect_info_num);
    SWAP_TABLE_U32(h.gfx_block_info_offset, sizeof(GFXBlockInfo), h.gfx_block_info_num);
    SWAP_TABLE_U32(h.obja_offset, sizeof(ObjA), h.obja_num);
    SWAP_TABLE_U32(h.obj0_offset, sizeof(Obj0), h.obj0_num);
    SWAP_TABLE_U32(h.animation_block_info_offset, sizeof(AnimationBlockInfo), h.animation_block_info_num);

    // Generic per-stride tables (stride from PakHeader *_size fields,
    // most are u32-only or close enough that u32-swap covers the signal)
    SWAP_TABLE_U32(h.buffer_info_offset, h.buffer_info_size, h.buffer_info_num);
    SWAP_TABLE_U32(h.mat1_offset, h.mat1_size, h.mat1_num);
    SWAP_TABLE_U32(h.mat2_offset, h.mat2_size, h.mat2_num);
    SWAP_TABLE_U32(h.mat3_offset, h.mat3_size, h.mat3_num);
    SWAP_TABLE_U32(h.mat4_offset, h.mat4_size, h.mat4_num);
    SWAP_TABLE_U32(h.mat_extra_offset, /*stride*/ 200, h.mat_extra_num);  // MatExtra = 50 u32 per Rust def
    SWAP_TABLE_U32(h.shape_info_offset, h.shape_info_size, h.shape_info_num);
    SWAP_TABLE_U32(h.hk_shape_info_offset, h.hk_shape_info_size, h.hk_shape_info_num);
    SWAP_TABLE_U32(h.hk_constraint_data_offset, h.hk_constraint_data_size, h.hk_constraint_data_num);
    SWAP_TABLE_U32(h.vbuff_info_offset, h.vbuff_info_size, h.vbuff_info_num);
    SWAP_TABLE_U32(h.ibuff_info_offset, h.ibuff_info_size, h.ibuff_info_num);
    SWAP_TABLE_U32(h.pfield_info_offset, h.pfield_info_size, h.pfield_info_num);

    // HkConstraintInfo - has 2× u16 inline. Swap as u32 first (wrong for
    // the u16 pair, correct for everything else), then un-swap and
    // re-swap the u16s correctly. Stride = hk_constraint_info_size from
    // PakHeader, u16 pair lives at offset 44 per the Rust layout. Guard
    // against a stride that doesn't match expectations.
    if (h.hk_constraint_info_offset != 0 && h.hk_constraint_info_num != 0 &&
        h.hk_constraint_info_size >= 48 &&
        (uint64_t)h.hk_constraint_info_offset +
            (uint64_t)h.hk_constraint_info_size * h.hk_constraint_info_num <= bsz)
    {
        uint8_t* p = base + h.hk_constraint_info_offset;
        uint32_t stride = h.hk_constraint_info_size;
        SwapU32ArrayInPlace(p, (size_t)stride * h.hk_constraint_info_num);
        // Un-do the u32-swap on bytes [44..48] which is a pair of u16s
        // (bones_num, bone_order_num). After the u32 swap, those 4 bytes
        // are in reverse byte order AS A GROUP. We need them as two
        // independent u16 swaps. Easiest: swap the u32 back (un-do),
        // then swap each u16 individually.
        for (uint32_t i = 0; i < h.hk_constraint_info_num; ++i) {
            uint8_t* entry = p + (uint64_t)i * stride;
            uint32_t* pairAsU32 = (uint32_t*)(entry + 44);
            *pairAsU32 = SwapU32(*pairAsU32);  // undo
            SwapU16ArrayInPlace(entry + 44, 2); // correct pair of u16s
        }
    }

    // TextureInfo - 12 u32 [0..48] + 4 u16 [48..56] + 16 u8 [56..72].
    // Can't SWAP_TABLE_U32 because the u16/u8 tail would corrupt. Walk
    // by hand.
    if (h.texture_info_offset != 0 && h.texture_info_num != 0 &&
        (uint64_t)h.texture_info_offset +
            (uint64_t)sizeof(TextureInfo) * h.texture_info_num <= bsz)
    {
        uint8_t* p = base + h.texture_info_offset;
        for (uint32_t i = 0; i < h.texture_info_num; ++i) {
            uint8_t* entry = p + (uint64_t)i * sizeof(TextureInfo);
            // 48 bytes of pure u32 (12 fields)
            SwapU32ArrayInPlace(entry, 48);
            // 4 u16s (width, height, depth, levels)
            SwapU16ArrayInPlace(entry + 48, 4);
            // 16 bytes of u8 unk - no swap needed
        }
    }

    // sub_blocks1 header + triples. Format: [u32 block_num][(u32 key,
    // u32 offset, u32 size) * block_num]. All u32. The count field
    // lives at `sub_blocks1_offset + 4` per the ParseSubBlocks impl
    // (the first 4 bytes of the sub-blocks region are header magic
    // or similar). Actually we just swap the entire sub_blocks1
    // region up to the end of the last triple.
    //
    // We don't know the exact sub_blocks1 byte length from the header,
    // so swap a generous chunk (block_num at [offset+4], then 12 bytes
    // per triple). Read block_num FIRST from the raw bytes to know how
    // far to swap.
    if (h.sub_blocks1_offset != 0 && h.sub_blocks1_offset + 16 <= bsz) {
        uint8_t* sb = base + h.sub_blocks1_offset;
        // Read block_num in raw BE form - we have not swapped this
        // region yet, so the bytes on disk say "backwards u32".
        uint32_t block_num_be; memcpy(&block_num_be, sb + 4, 4);
        uint32_t block_num = SwapU32(block_num_be);
        // Sanity gate. Pandemic's biggest shipped maps have a few
        // dozen sub-block entries. 10k is a "your file is corrupted
        // or your endian detection is wrong" threshold.
        if (block_num < 10000) {
            // SubBlocksHeader is 16 bytes (z0, block_num, z2, z3) —
            // four motherfucking u32s, not two. Earlier version of this
            // code used 8 here, which left z2/z3 unswapped AND ATE THE
            // LAST 8 BYTES of the final triple — exactly where the
            // 'level' GameObjs sub-block stores its offset and size on
            // every Pandemic build. Result: garbage offset (3.7 billion),
            // bounds gate failed, Stage 4 swap never fired, ZERO entities
            // ever loaded. Cost us a whole night of "the parser must be
            // fooling us" before we caught it. SubBlocksHeader is 16 bytes.
            // Always was. Don't fucking touch this constant.
            size_t sbTotal = 16 + (size_t)block_num * 12;
            if (h.sub_blocks1_offset + sbTotal <= bsz) {
                SwapU32ArrayInPlace(sb, sbTotal);
                LvlLog("SwapBlock1: sub_blocks1 = %u triples, flipped\n", block_num);
            } else {
                LvlLog("SwapBlock1: sub_blocks1 claims %u triples but that runs off the end of block1 (size=%u). Not going to swap garbage into valid data. Skipping.\n", block_num, (uint32_t)bsz);
            }
        } else {
            LvlLog("SwapBlock1: sub_blocks1 block_num=%u - that is an insane fucking number, file is either corrupted or our endian detection lied. Skipping the whole sub_blocks1 swap.\n", block_num);
        }
    }

    // block2_offsets - packed u32 array in Block1 that indexes into
    // Block2 asset payloads. Trivial swap, pure u32.
    if (h.block2_offsets_offset != 0 && h.block2_offsets_num != 0) {
        uint64_t total = (uint64_t)h.block2_offsets_num * 4;
        if (h.block2_offsets_offset + total <= bsz) {
            SwapU32ArrayInPlace(base + h.block2_offsets_offset, (size_t)total);
            LvlLog("SwapBlock1: block2_offsets = %u entries, flipped\n", h.block2_offsets_num);
        }
    }

    // ═════════════════════════════════════════════════════════════════════
    //  STAGE 3A.5: PER-MODEL AUXILIARY u32 ARRAYS
    // ═════════════════════════════════════════════════════════════════════
    //
    // The named asset tables are flipped, but every Model carries half a
    // dozen offsets pointing to LOOSE u32 arrays scattered through Block1
    // - mat_offset, vbuff_offset, ibuff_offset, shape_offset. Those arrays
    // are not part of any named table so the per-table swap above missed
    // them entirely. Result on a 360 file: model fields read native LE
    // (great), but jumping to mdl.mat_offset and reading u32s from there
    // hands you backwards bytes, you get nonsense mat-struct offsets, you
    // jump to garbage memory, you read nonsense texture CRCs, and every
    // single texture lookup misses. Mesh parts render UNTEXTURED. User
    // sees a black map with green entity gizmos and rage-types HOLY SHIT
    // WHY NO TERRAIN. Same root cause for vbuff/ibuff index arrays - they
    // resolve to wrong vertex/index buffers, geometry comes out as soup.
    //
    // Fix: walk every model. For each, swap its aux arrays in place.
    // Use a dedupe set because models can (in theory) share an offset
    // and we DO NOT want to swap the same bytes twice and hand back BE.
    //
    // Models table is already flipped, so we read mdl fields as native
    // LE u32s directly off the stride'd table at hdr.model_info_offset.
    if (h.model_info_offset != 0 && h.model_info_num != 0 &&
        (uint64_t)h.model_info_offset + (uint64_t)sizeof(ModelInfo) * h.model_info_num <= bsz)
    {
        std::set<uint32_t> swappedOffsets;  // dedupe so shared aux arrays don't double-flip
        uint32_t totalAuxArraysSwapped = 0;
        uint32_t totalAuxBytesSwapped  = 0;

        for (uint32_t mi = 0; mi < h.model_info_num; ++mi) {
            const uint8_t* modelEntry = base + h.model_info_offset + (uint64_t)mi * sizeof(ModelInfo);

            // Pull only the fields we need - already swapped so memcpy + LE-read is correct.
            uint32_t mat_off,   mat_num;
            uint32_t vbuff_off, vbuff_num;
            uint32_t ibuff_off, ibuff_num;
            uint32_t shape_off, shape_num;
            memcpy(&mat_off,   modelEntry + offsetof(ModelInfo, mat_offset),    4);
            memcpy(&mat_num,   modelEntry + offsetof(ModelInfo, mat_num),       4);
            memcpy(&vbuff_off, modelEntry + offsetof(ModelInfo, vbuff_offset),  4);
            memcpy(&vbuff_num, modelEntry + offsetof(ModelInfo, vbuff_num),     4);
            memcpy(&ibuff_off, modelEntry + offsetof(ModelInfo, ibuff_offset),  4);
            memcpy(&ibuff_num, modelEntry + offsetof(ModelInfo, ibuff_num),     4);
            memcpy(&shape_off, modelEntry + offsetof(ModelInfo, shape_offset),  4);
            memcpy(&shape_num, modelEntry + offsetof(ModelInfo, shape_num),     4);

            // Helper macro: swap one auxiliary u32 array, dedupe, bounds-check, count.
            // Treats offset 0 / count 0 as no-op. Sanity-gates count to 4096
            // because nobody has 4097 materials per model and this catches
            // already-corrupted reads instead of swapping 16GB of garbage.
            #define SWAP_MODEL_AUX(off, count) do { \
                if ((off) != 0 && (count) != 0 && (count) < 4096) { \
                    if (swappedOffsets.find(off) == swappedOffsets.end()) { \
                        uint64_t total = (uint64_t)(count) * 4; \
                        if ((uint64_t)(off) + total <= bsz) { \
                            SwapU32ArrayInPlace(base + (off), (size_t)total); \
                            swappedOffsets.insert(off); \
                            ++totalAuxArraysSwapped; \
                            totalAuxBytesSwapped += (uint32_t)total; \
                        } \
                    } \
                } \
            } while(0)

            SWAP_MODEL_AUX(mat_off,   mat_num);
            SWAP_MODEL_AUX(vbuff_off, vbuff_num);
            SWAP_MODEL_AUX(ibuff_off, ibuff_num);
            SWAP_MODEL_AUX(shape_off, shape_num);

            #undef SWAP_MODEL_AUX
        }

        LvlLog("SwapBlock1: per-model aux u32 arrays = %u arrays, %u bytes flipped (across %u models)\n",
               totalAuxArraysSwapped, totalAuxBytesSwapped, h.model_info_num);
    }

    LvlLog("SwapBlock1: DONE. Every asset table we know how to swap is now native little-endian. Downstream parsers should pretend this was a PC file from the start. If anything crashes past this point, it is a Stage 3 platform-variant struct (VBuffInfo, IBuffInfo, Mat3, BufferInfo) that we have not properly handled yet.\n");

    #undef SWAP_TABLE_U32
}

// ═════════════════════════════════════════════════════════════════════════
//  STAGE 3A: THE MOTHERFUCKING BLOCK2 VERTEX/INDEX BUFFER SWAP
// ═════════════════════════════════════════════════════════════════════════
//
// If Stage 2 was "make the headers readable", Stage 3 is "make the geometry
// actually fucking visible". Block2 holds raw vertex and index buffer data
// for every model in the level. On PC those floats and u16s are already
// little-endian. On Xbox 360 / PS3 every fucking float in every vertex
// position, every u16 in every triangle index, every byte of every UV
// and normal - ALL BACKWARDS. Hand that garbage to D3D9 and it dutifully
// uploads what it got, renders every triangle as the hell it was handed,
// and the positions come out like (1.84e38, 2.14e-37, 6.99e-38) - either
// astronomically outside the view frustum or sub-picometer at origin.
// Engine draws every triangle PERFECTLY. The triangles are just in
// motherfucking HELL.
//
// What we swap here:
//
//   - VERTEX BUFFERS (one per VBuffInfo entry). Treated as a u32 array
//     spanning [vbuff.offset .. vbuff.offset + vbuff.size]. Floats
//     (position xyz, normal xyz, UV uv, tangent xyzw) swap correctly as
//     u32. The minority vertices with packed u16 bone-index pairs or u8
//     color channels get partially garbled - we eat that loss on first
//     pass and revisit if skinned meshes look fucked. Most Discworld
//     geometry is static props without bone skinning so this is fine.
//
//   - INDEX BUFFERS (one per IBuffInfo entry). LOTRC index buffers are
//     u16 arrays of triangle indices. Swap as u16 array, NOT u32 -
//     if we u32-swapped, each pair of triangle indices would trade
//     places with each other and meshes would render with completely
//     wrong triangle topology (still visible but garbage soup).
//
// What we do NOT swap here:
//
//   - TEXTURE DATA in Block2. DXT compression is endian-neutral - the
//     encoded blocks are 4-bit index maps + RGB565 endpoints designed
//     for GPU hardware to decompress and that hardware doesn't give
//     a shit about host endian. PC and 360 DDS textures interchange
//     without byte-swap. If we DID swap we would destroy them.
//
//   - ANIMATION DATA. Anims have their own format (float keyframes
//     packed into AnimationInfo.offset regions). Those floats ARE BE
//     and WILL need a swap, but that is Stage 3C. First-pass goal is
//     "static geometry renders". Deferred.
//
//   - COLLISION / MOPP DATA. Havok MOPP bytecode is its own self-
//     contained format. Swap-neutral within the bytecode itself.
//
// VBuffInfo / IBuffInfo platform variance note: on 360 VBuffInfo is 40
// bytes (PC is 32 bytes, extra 2 u32 at the end). IBuffInfo is 36 bytes
// on 360 (PC is 24). BUT the relevant fields - `size` at field index 1
// and `offset` at field index 3 (VBuffInfo) / index 4 (IBuffInfo) -
// live at the SAME byte offsets across all three platforms. The extras
// are tacked on at the end. So we read stride from PakHeader (which
// correctly reports 40/36 on a 360 file), use sizeof-by-stride iteration,
// and pull `size` + `offset` from their common positions. No need for
// a platform-variant struct definition just for this swap pass.
void LevelReader::SwapBlock2BuffersBE() {
    if (!m_bigEndian || m_block2.empty()) return;

    const size_t b2sz = m_block2.size();
    uint8_t* b2 = &m_block2[0];

    const PakHeader& h = m_pakHeader;
    uint32_t vbuffSwapped = 0, ibuffSwapped = 0;
    uint64_t vbuffBytes = 0, ibuffBytes = 0;

    // ── VERTEX BUFFERS ──────────────────────────────────────────────────
    // Walk VBuffInfo entries using PakHeader's vbuff_info_size stride.
    // For each entry pull .size (u32 field at byte offset 4) and .offset
    // (u32 field at byte offset 12). These are already swapped by Stage 2
    // so we read them as native u32s.
    if (h.vbuff_info_offset != 0 && h.vbuff_info_num != 0 &&
        h.vbuff_info_size >= 16 && !m_block1.empty())
    {
        uint64_t totalTable = (uint64_t)h.vbuff_info_size * h.vbuff_info_num;
        if (h.vbuff_info_offset + totalTable <= m_block1.size()) {
            const uint8_t* tbl = &m_block1[h.vbuff_info_offset];
            for (uint32_t i = 0; i < h.vbuff_info_num; ++i) {
                const uint8_t* entry = tbl + (uint64_t)i * h.vbuff_info_size;
                uint32_t vbSize;   memcpy(&vbSize,   entry + 4,  4); // .size
                uint32_t vbOffset; memcpy(&vbOffset, entry + 12, 4); // .offset
                if (vbSize == 0 || vbOffset == 0) continue;
                if ((uint64_t)vbOffset + vbSize > b2sz) {
                    LvlLog("SwapBlock2: vbuff[%u] offset=%u size=%u runs past block2 (size=%u). Fucked entry, skipping.\n",
                           i, vbOffset, vbSize, (uint32_t)b2sz);
                    continue;
                }
                SwapU32ArrayInPlace(b2 + vbOffset, vbSize);
                ++vbuffSwapped;
                vbuffBytes += vbSize;
            }
        } else {
            LvlLog("SwapBlock2: VBuffInfo table runs past block1 size. Something lied in the header, bailing on vertex swap.\n");
        }
    }

    // ── INDEX BUFFERS ───────────────────────────────────────────────────
    // IBuffInfo: .size at byte offset 4, .offset at byte offset 16.
    // Indices are u16 per LOTRC convention - DO NOT u32-swap these or
    // triangle topology gets shredded.
    if (h.ibuff_info_offset != 0 && h.ibuff_info_num != 0 &&
        h.ibuff_info_size >= 20 && !m_block1.empty())
    {
        uint64_t totalTable = (uint64_t)h.ibuff_info_size * h.ibuff_info_num;
        if (h.ibuff_info_offset + totalTable <= m_block1.size()) {
            const uint8_t* tbl = &m_block1[h.ibuff_info_offset];
            for (uint32_t i = 0; i < h.ibuff_info_num; ++i) {
                const uint8_t* entry = tbl + (uint64_t)i * h.ibuff_info_size;
                uint32_t ibSize;   memcpy(&ibSize,   entry + 4,  4); // .size
                uint32_t ibOffset; memcpy(&ibOffset, entry + 16, 4); // .offset
                if (ibSize == 0 || ibOffset == 0) continue;
                if ((uint64_t)ibOffset + ibSize > b2sz) {
                    LvlLog("SwapBlock2: ibuff[%u] offset=%u size=%u runs past block2 (size=%u). Fucked entry, skipping.\n",
                           i, ibOffset, ibSize, (uint32_t)b2sz);
                    continue;
                }
                // Size is in bytes, swap as u16 array - so (size/2) u16s.
                SwapU16ArrayInPlace(b2 + ibOffset, ibSize / 2);
                ++ibuffSwapped;
                ibuffBytes += ibSize;
            }
        } else {
            LvlLog("SwapBlock2: IBuffInfo table runs past block1 size. Bailing on index swap.\n");
        }
    }

    LvlLog("SwapBlock2: ═══════════════════════════════════════════════════════\n");
    LvlLog("SwapBlock2: vertex buffers flipped: %u entries, %llu bytes of backwards float garbage turned back into real vertex positions\n",
           vbuffSwapped, (unsigned long long)vbuffBytes);
    LvlLog("SwapBlock2: index buffers flipped:  %u entries, %llu bytes of u16 triangle indices un-reversed\n",
           ibuffSwapped, (unsigned long long)ibuffBytes);
    LvlLog("SwapBlock2: ═══════════════════════════════════════════════════════\n");
    LvlLog("SwapBlock2: If geometry STILL does not render after this, the next suspects are:\n");
    LvlLog("SwapBlock2:   1. Animation keyframe data still BE (Stage 3C)\n");
    LvlLog("SwapBlock2:   2. Skinned mesh vertices with u16 bone indices (minor garble, accepted)\n");
    LvlLog("SwapBlock2:   3. GameObjs entity WorldTransform matrices still BE (Stage 4)\n");
    LvlLog("SwapBlock2:   4. Shader cache mismatch (360 shaders != PC shaders, separate problem entirely)\n");
    LvlLog("SwapBlock2: If geometry renders but the positions are in orbit, look at #3.\n");
    LvlLog("SwapBlock2: If geometry renders with correct placement but every fucking pixel is black,\n");
    LvlLog("SwapBlock2: look at #4. Either way, report back.\n");
}

// ═════════════════════════════════════════════════════════════════════════
//  STAGE 4: THE GODDAMN GAMEOBJS BLOCK PRE-SWAP
// ═════════════════════════════════════════════════════════════════════════
//
// Called BEFORE ParseGameObjs runs on big-endian files. Swaps the entire
// GameObjs block payload in place so the downstream parser reads native
// little-endian values from every memcpy. Without this, every entity's
// WorldTransform matrix is 16 backwards floats, every GUID is a reversed
// u32, every type CRC doesn't resolve, and the level loads with zero
// instances because the whole fucking type system is illegible.
//
// Layout we walk:
//
//   [0..20]                   Header: 5 u32
//                             (magic, types_num, types_off, obj_num, obj_off)
//
//   [types_off..]             Type def array. Per type:
//                               3 u32 header (typeCRC, fieldCount, reserved)
//                               N × 3 u32 field defs (name_crc, kind_crc, offset)
//
//   [obj_off..]               Entity instance array. Per entity:
//                               16-byte header: layer(u32), type_key(u32),
//                                               obj_size(u16), flags(u16),
//                                               z4(u32)
//                               obj_size bytes of field data
//
// What we DO:
//   - Swap header as u32 array
//   - Swap every type def header + field def as u32 array (all u32)
//   - Per entity: swap header with correct u16 handling for (obj_size, flags)
//   - Per entity: swap field data as u32 blob
//
// What we DON'T (known limitations, documented for Stage 4B if we ever care):
//   - List fields (ObjectList, NodeList, IntList, etc.) store their
//     metadata as [u16 count][u16 offset] at the field position. Our
//     u32-blob swap treats this pair as one u32 and scrambles it.
//     Effect: list traversal will read wrong counts and point at wrong
//     offsets, so Outputs[], Layers[], Nodes[] won't resolve correctly.
//     Event wiring between entities therefore breaks. But:
//     WorldTransform (16 floats) IS correct because u32-swap handles
//     floats identically. Entity placement works. Geometry renders.
//     Event graph doesn't. Trade we are making on this pass.
//
//   - String fields store a 4-byte CRC at the field position, pointing
//     into the string table. u32 swap handles that correctly.
//
//   - Variable-length data like lists is stored AFTER the fixed field
//     block within the entity. We u32-blob-swap everything which
//     screws up those lists, but again scalar access (WorldTransform)
//     still works.
//
// If event wiring / chain editor needs to work on 360 files later,
// upgrade this function to walk the type defs and swap each field
// according to its kind_crc. That's maybe 100 more lines of code. For
// now: get geometry on the fucking screen.
void LevelReader::SwapGameObjsBlockBE(uint8_t* data, uint32_t dataSize) {
    if (dataSize < 32) return;

    // ── Step 1: Swap 5-u32 header ──────────────────────────────────────
    SwapU32ArrayInPlace(data, 20);

    // Read swapped header values
    uint32_t types_num, types_off, obj_num, obj_off;
    memcpy(&types_num, data + 4,  4);
    memcpy(&types_off, data + 8,  4);
    memcpy(&obj_num,   data + 12, 4);
    memcpy(&obj_off,   data + 16, 4);

    // Sanity - if counts came out insane, our endian detection fucked up
    // or the block is corrupted. Bail rather than ploughing into garbage.
    if (types_num > 10000 || obj_num > 100000) {
        LvlLog("SwapGameObjs: counts look FUCKED (types=%u objs=%u) - bailing before we shred anything else\n",
               types_num, obj_num);
        return;
    }

    LvlLog("SwapGameObjs: header swapped. types=%u objects=%u types_off=%u obj_off=%u\n",
           types_num, obj_num, types_off, obj_off);

    // ── Step 2: Swap type defs. Each is 3 u32 header + N × 3 u32 fields. ─
    uint32_t off = types_off;
    uint32_t swappedTypes = 0, swappedFields = 0;
    for (uint32_t t = 0; t < types_num; ++t) {
        if (off + 12 > dataSize) {
            LvlLog("SwapGameObjs: type def %u runs past block, bailing type loop\n", t);
            break;
        }
        // Swap the 3-u32 type header in place
        SwapU32ArrayInPlace(data + off, 12);

        // Now read fieldCount to know how many field defs follow
        uint32_t tcount;
        memcpy(&tcount, data + off + 4, 4);
        off += 12;

        // Sanity - field count per type shouldn't exceed a few hundred
        if (tcount > 500) {
            LvlLog("SwapGameObjs: type %u reports %u fields which is bullshit, bailing\n",
                   t, tcount);
            break;
        }

        // Swap tcount × 3 u32 field defs
        if (off + tcount * 12 > dataSize) {
            LvlLog("SwapGameObjs: type %u field defs run past block, bailing\n", t);
            break;
        }
        SwapU32ArrayInPlace(data + off, tcount * 12);
        off += tcount * 12;
        ++swappedTypes;
        swappedFields += tcount;
    }
    LvlLog("SwapGameObjs: %u types swapped (%u total field defs)\n",
           swappedTypes, swappedFields);

    // ── Step 3: Swap per-entity headers + field data ────────────────────
    //
    // Entity header: u32 layer, u32 type_key, u16 obj_size, u16 flags,
    //                u32 z4 (16 bytes total)
    // Then obj_size bytes of field data.
    //
    // We swap the u32 parts individually and the u16 pair as two u16s
    // (NOT as one u32, which would pair-shuffle the values).
    off = obj_off;
    uint32_t swappedObjs = 0;
    for (uint32_t o = 0; o < obj_num; ++o) {
        if (off + 16 > dataSize) {
            LvlLog("SwapGameObjs: entity %u header runs past block (off=%u size=%u) - bailing\n",
                   o, off, dataSize);
            break;
        }

        // Swap entity header fields individually. Read BE, swap, write LE.
        uint32_t layer_be, type_key_be, z4_be;
        uint16_t obj_size_be, flags_be;
        memcpy(&layer_be,     data + off + 0,  4);
        memcpy(&type_key_be,  data + off + 4,  4);
        memcpy(&obj_size_be,  data + off + 8,  2);
        memcpy(&flags_be,     data + off + 10, 2);
        memcpy(&z4_be,        data + off + 12, 4);

        uint32_t layer     = SwapU32(layer_be);
        uint32_t type_key  = SwapU32(type_key_be);
        uint16_t obj_size  = SwapU16(obj_size_be);
        uint16_t flags     = SwapU16(flags_be);
        uint32_t z4        = SwapU32(z4_be);

        memcpy(data + off + 0,  &layer,    4);
        memcpy(data + off + 4,  &type_key, 4);
        memcpy(data + off + 8,  &obj_size, 2);
        memcpy(data + off + 10, &flags,    2);
        memcpy(data + off + 12, &z4,       4);
        off += 16;

        // Sanity on obj_size - no single entity's field data should be
        // larger than a few KB. If it reads insane, bail the entity loop.
        if (obj_size > 65535) {
            LvlLog("SwapGameObjs: entity %u has obj_size=%u which is insane - bailing\n",
                   o, obj_size);
            break;
        }
        if (off + obj_size > dataSize) {
            LvlLog("SwapGameObjs: entity %u field data runs past block (off=%u size=%u block=%u) - bailing\n",
                   o, off, obj_size, dataSize);
            break;
        }

        // Swap the entity's field data as a u32 blob. See the big comment
        // above for what this correctly handles (scalars, WorldTransforms,
        // GUIDs, CRCs, floats, matrices) and what it fucks up (list
        // metadata u16 pairs). WorldTransform is correct. That's what
        // matters for placing geometry.
        if (obj_size >= 4) {
            uint32_t n4 = (uint32_t)obj_size & ~(uint32_t)3;
            if (n4 > 0) SwapU32ArrayInPlace(data + off, n4);
        }
        off += obj_size;
        ++swappedObjs;
    }

    LvlLog("SwapGameObjs: ═══════════════════════════════════════════════════\n");
    LvlLog("SwapGameObjs: DONE. %u types + %u entities flipped. GameObjs block\n",
           swappedTypes, swappedObjs);
    LvlLog("SwapGameObjs: reads native little-endian from here. WorldTransforms\n");
    LvlLog("SwapGameObjs: should decode correctly - entity placements will be\n");
    LvlLog("SwapGameObjs: at real world-space coords instead of 1e38 garbage.\n");
    LvlLog("SwapGameObjs: Event wiring (Outputs[], Layers[]) is knowingly fucked\n");
    LvlLog("SwapGameObjs: this pass because list u16 metadata got pair-scrambled\n");
    LvlLog("SwapGameObjs: by the u32 blob swap. Geometry first, event wiring later.\n");
    LvlLog("SwapGameObjs: ═══════════════════════════════════════════════════\n");
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

    // ── Endian detection BEFORE the memcpy ──────────────────────────────
    // Magic value is 6 at u32[0]. On a sane PC build that means bytes
    // 06 00 00 00. On a backwards-as-fuck Xbox 360 / PS3 build the same
    // value is stored 00 00 00 06, and if we just memcpy and read u32[0]
    // we get 0x06000000 = 100663296 - the same goddamn 1-billion-range
    // garbage that crashed the parser before this fix existed. Read the
    // raw bytes FIRST, decide endian, THEN copy the header. Doing it in
    // any other order means we are interpreting bytes as integers with
    // no idea what byte order they came from. That is how you ship
    // crashes for years and have no fucking idea why.
    m_bigEndian = false;
    if (binData.size() >= 8) {
        if (binBase[0] == 6 && binBase[1] == 0 && binBase[2] == 0 && binBase[3] == 0) {
            m_bigEndian = false; // PC
        } else if (binBase[0] == 0 && binBase[1] == 0 && binBase[2] == 0 && binBase[3] == 6) {
            // BE file. Rust parser distinguishes XBOX vs PS3 via version
            // byte (byte 7 in BE = last byte of u32 version). For our
            // purposes both are "swap every u32 the same way" so we lump
            // them under one big-endian path. If PS3 ever has a different
            // field layout we care about, this is where the split goes.
            m_bigEndian = true;
            const char* plat = (binBase[7] == 2) ? "XBOX 360" :
                               (binBase[7] == 3) ? "PS3" : "unknown-fucking-BE-platform";
            LvlLog("ParseBin: HOLY SHIT - big-endian %s file. Every goddamn u32 in this\n", plat);
            LvlLog("         file is backwards. Pandemic shipped console builds byte-flipped\n");
            LvlLog("         because PowerPC and Cell CPUs are big-endian pieces of shit and\n");
            LvlLog("         the compiler baked the native order straight into the assets.\n");
            LvlLog("         Swapping headers. Sub-blocks are still backwards and will stay\n");
            LvlLog("         that way until Stage 2 - if we try to read them now the parser\n");
            LvlLog("         dies screaming halfway through ParseBlock1. (version byte 0x%02X)\n",
                   binBase[7]);
        } else {
            char errbuf[192];
            sprintf_s(errbuf, sizeof(errbuf),
                      "BIN magic unrecognized: bytes %02X %02X %02X %02X (expected LE 06 00 00 00 for PC or BE 00 00 00 06 for Xbox 360 / PS3). This is not a Pandemic LOTRC BIN file, or it is a format variant nobody has ever seen.",
                      binBase[0], binBase[1], binBase[2], binBase[3]);
            m_error = errbuf;
            LvlLog("ParseBin: WHAT THE FUCK - magic bytes match no known LOTRC platform.\n");
            LvlLog("         First 4 bytes: %02X %02X %02X %02X\n",
                   binBase[0], binBase[1], binBase[2], binBase[3]);
            LvlLog("         Expected either 06 00 00 00 (PC little-endian) or 00 00 00 06\n");
            LvlLog("         (Xbox 360 / PS3 big-endian). Got horseshit instead. Either this\n");
            LvlLog("         is not a Pandemic LOTRC BIN, or it is a beta / prototype / DLC\n");
            LvlLog("         build with a format variant nobody in this fucking codebase has\n");
            LvlLog("         ever seen. Bail out before we crash trying to parse it.\n");
            return false;
        }
    }

    memcpy(&m_binHeader, binBase, sizeof(BinHeader));

    // On BE, swap every u32 in BinHeader. Safe because BinHeader is 43×u32
    // with no holes (verified by the ZERO_STATIC_ASSERT of size==172).
    if (m_bigEndian) {
        SwapU32ArrayInPlace(&m_binHeader, sizeof(BinHeader));
    }

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
    // vdata_num = model count, the section boundary. Indices [0..vdata_num)
    // are models; [vdata_num..ahNum) are textures. lotrc_rs writes them in
    // this order and Vespucci's BIN reader honors the same split now that
    // same-key model+texture pairs (e.g. CRD body + its baked atlas reusing
    // the body's name) no longer get to clobber each other in the map.
    const uint32_t vdataNum = m_binHeader.vdata_num;
    if (ahNum > 0 && ahOff + ahNum * sizeof(BinAssetHandle) <= binData.size()) {
        const BinAssetHandle* handles =
            reinterpret_cast<const BinAssetHandle*>(binBase + ahOff);
        for (uint32_t i = 0; i < ahNum; ++i) {
            // BE branch: swap every u32 in the asset handle before
            // building the entry. BinAssetHandle is 5×u32 = 20 bytes
            // confirmed by ZERO_STATIC_ASSERT. If we forget to swap
            // even ONE field here, that asset's offset/size points
            // into garbage and the next time anyone calls
            // GetBinAssetData on it we eat an access violation. Two
            // separate code paths (BE swap, LE direct) on purpose -
            // costs zero on the LE hot path, keeps the logic readable.
            BinAssetEntry ae;
            if (m_bigEndian) {
                ae.key       = SwapU32(handles[i].key);
                ae.offset    = SwapU32(handles[i].offset);
                ae.size      = SwapU32(handles[i].size);
                ae.size_comp = SwapU32(handles[i].size_comp);
                ae.kind      = SwapU32(handles[i].kind);
            } else {
                ae.key       = handles[i].key;
                ae.offset    = handles[i].offset;
                ae.size      = handles[i].size;
                ae.size_comp = handles[i].size_comp;
                ae.kind      = handles[i].kind;
            }
            if (i < vdataNum) {
                m_binAssets[ae.key] = ae;
            } else {
                m_binTextureAssets[ae.key] = ae;
            }
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

        // ── DIAGNOSTIC: dump every fucking sub-block so we can see what
        // this Discworld-prototype PAK is actually hiding. On 360 files
        // the GameObjs payload may be under a different key name than
        // the PC "Level" — Pandemic renamed shit between milestones and
        // this is the only way to know for sure without disassembling
        // the 360 binary. Remove once we've confirmed the key.
        LvlLog("  SubBlock[%u]: key=0x%08X name='%s' absOff=%u size=%u\n",
               i, key, se.name.empty() ? "<unresolved>" : se.name.c_str(),
               absOff, size);

        // Parse GameObjs data inline (only from sub_blocks1, key == "Level")
        static const uint32_t KEY_GAMEOBJS_LOCAL = LotrHashString("Level");
        if (key == KEY_GAMEOBJS_LOCAL && m_gameObjs.empty()) {
            if (absOff + 32 <= static_cast<uint32_t>(blockData.size()) &&
                absOff + size <= static_cast<uint32_t>(blockData.size())) {
                m_gameObjsBlock1Offset = absOff;
                m_gameObjsBlock1Size = size;

                // ── STAGE 4: flip the GameObjs block on big-endian files ─
                // Swap the entire GameObjs payload in place so the existing
                // parser reads LE-native values. Without this, every entity
                // has a backwards WorldTransform, a backwards GUID, a
                // backwards type CRC, and the parser loads ZERO instances
                // because the type system does not resolve. The swap
                // function modifies blockData in place - blockData is
                // actually m_block1 under the const reference, and the
                // const_cast below is the explicit admission of that.
                if (m_bigEndian) {
                    uint8_t* mutableData = const_cast<uint8_t*>(&blockData[0]);
                    SwapGameObjsBlockBE(mutableData + absOff, size);
                }

                ParseGameObjs(&blockData[0] + absOff, size, absOff);
            }
        }
    }
}

// ============================================================
//  GameObjs parsing (level entity placement data)
//
//  Confirmed by disassembly at FUN_007e8ecb (918 bytes):
//
//  Binary layout — MgGameObjsHeader (32 bytes):
//    [+0x00] magic       uint32 = 0x4D414704
//    [+0x04] numTypes    uint32
//    [+0x08] typeOffset  uint32 (byte offset to type table)
//    [+0x0C] numEntities uint32
//    [+0x10] entityOffset uint32 (byte offset to entity data)
//    [+0x14] reserved[3] uint32
//
//  Type Entry (variable size, 12-byte header + 12 per field):
//    [+0x00] typeCRC      uint32 (pre-computed CRC of type name)
//    [+0x04] fieldCount   uint32
//    [+0x08] reserved     uint32
//    Per field (12 bytes):
//      [+0x00] fieldNameCRC  uint32
//      [+0x04] fieldTypeCRC  uint32
//      [+0x08] dataOffset    uint32
//
//  Entity Entry (16-byte header + dataSize bytes):
//    [+0x00] layerGUID  uint32
//    [+0x04] typeCRC    uint32
//    [+0x08] dataSize   uint16 (size of following data blob)
//    [+0x0A] flags      uint16 (entity flags — see MgLevelLoader.h)
//    [+0x0C] reserved   uint32
//    Followed by dataSize bytes of field data
//
//  After parsing, the game performs:
//    1. GUID resolution (FUN_007e8bf8): MgHandle/MgHandleArray/MgOutputArray
//    2. Entity sort by typeCRC (insertion sort at 0x007e91e0)
//    3. Entity spawn loop (FUN_007e78b7): CreateOnLoad + QualityCategory filter
//
//  CRCs are pre-computed at build time (build-tool CRC, NOT runtime CRC).
//  See MgCRC.h for dual-CRC documentation.
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
    // Binary layout per type (confirmed at FUN_007e8ecb):
    //   [+0x00] typeCRC:4  [+0x04] fieldCount:4  [+0x08] reserved:4
    //   Followed by fieldCount entries of:
    //     [+0x00] fieldNameCRC:4  [+0x04] fieldTypeCRC:4  [+0x08] dataOffset:4
    // Game hashes into lookup map, then entities resolve type by CRC match.
    std::map<uint32_t, GOTypeInfo> typeMap;

    uint32_t off = types_off;
    for (uint32_t t = 0; t < types_num; ++t) {
        if (off + 12 > dataSize) break;
        uint32_t tkey, tcount, tres;
        memcpy(&tkey,   data + off + 0, 4);  // typeCRC
        memcpy(&tcount, data + off + 4, 4);  // fieldCount
        memcpy(&tres,   data + off + 8, 4);  // reserved
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
        uint16_t obj_size, flags;  // flags: entity flags (confirmed by disasm at FUN_007e8ecb)
        uint32_t z4;
        memcpy(&layer,    data + off + 0,  4);  // layerGUID
        memcpy(&type_key, data + off + 4,  4);  // typeCRC (pre-computed)
        memcpy(&obj_size, data + off + 8,  2);  // dataSize
        memcpy(&flags,    data + off + 10, 2);  // entity flags
        memcpy(&z4,       data + off + 12, 4);  // reserved
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

    // PakHeader swap on big-endian. Safe because the struct is 118
    // motherfucking u32 fields straight through with zero padding
    // (ZERO_STATIC_ASSERT on sizeof==472 is the only thing keeping
    // this honest - if anyone adds a u16 in here the swap silently
    // shreds every field after it). m_bigEndian was decided in ParseBin
    // from the BIN magic. Assumes the user pairs a BE BIN with a BE
    // PAK because that is the ONLY combo Pandemic ever fucking shipped.
    // If somebody hands us a BE BIN with an LE PAK (deranged but
    // technically possible if you mix-and-match files from different
    // platform extracts), this goes spectacularly wrong - the constx13
    // check immediately below catches it because 0x13 swapped is
    // 0x13000000 which sure as shit does not equal 0x13.
    if (m_bigEndian) {
        SwapU32ArrayInPlace(&m_pakHeader, sizeof(PakHeader));
        LvlLog("ParsePak: PakHeader flipped. 118 motherfucking u32 fields, all backwards, now native.\n");
    }

    // Basic sanity check
    if (m_pakHeader.constx13 != 0x13) {
        char errbuf[64];
        sprintf_s(errbuf, sizeof(errbuf), "PAK header magic mismatch (expected 0x13, got 0x%X)", m_pakHeader.constx13);
        m_error = errbuf;
        return false;
    }

    // ═════════════════════════════════════════════════════════════════════
    //  STAGE 2: BIG-ENDIAN LEVEL CONTENT SUMMARY (wall removed, swap active)
    // ═════════════════════════════════════════════════════════════════════
    // The STAGE 1 WALL is motherfucking dead. Stage 2 landed per-struct
    // byte-swap for the asset tables, sub-block triples, and the generic
    // ParseGenericB1 reads. We log the content counts so the user sees
    // what's actually in the file, then BARREL THE FUCK FORWARD.
    //
    // Anything that still breaks past this point is Stage 3 territory:
    // platform-variant structs (VBuffInfo / IBuffInfo / Mat3 / BufferInfo
    // have DIFFERENT FIELD LAYOUTS on 360 vs PC, not just byte order) and
    // the GameObjs field-level swap (Stage 4 - per-entity type-def and
    // instance data reads all need individual endian awareness). Those
    // can crash or render garbage. The common path (model/texture/
    // animation/effect/objA/obj0 listing) should work.
    if (m_bigEndian) {
        LvlLog("ParsePak: ═══════════════════════════════════════════════════════════════\n");
        LvlLog("ParsePak: STAGE 2 ACTIVE - Xbox 360 / PS3 PAK. Headers swapped, asset\n");
        LvlLog("ParsePak: tables swapped, sub-blocks swapped, we are FUCKING EATING.\n");
        LvlLog("ParsePak: ═══════════════════════════════════════════════════════════════\n");
        LvlLog("ParsePak: LEVEL CONTENTS (from swapped header):\n");
        LvlLog("ParsePak:   ObjA entries:            %u\n", m_pakHeader.obja_num);
        LvlLog("ParsePak:   Obj0 entries:            %u\n", m_pakHeader.obj0_num);
        LvlLog("ParsePak:   Models:                  %u\n", m_pakHeader.model_info_num);
        LvlLog("ParsePak:   Textures:                %u\n", m_pakHeader.texture_info_num);
        LvlLog("ParsePak:   Animations:              %u\n", m_pakHeader.animation_info_num);
        LvlLog("ParsePak:   Effects:                 %u\n", m_pakHeader.effect_info_num);
        LvlLog("ParsePak:   GFX blocks:              %u\n", m_pakHeader.gfx_block_info_num);
        LvlLog("ParsePak:   Animation blocks:        %u\n", m_pakHeader.animation_block_info_num);
        LvlLog("ParsePak:   Collision shapes:        %u\n", m_pakHeader.shape_info_num);
        LvlLog("ParsePak:   Havok shape infos:       %u\n", m_pakHeader.hk_shape_info_num);
        LvlLog("ParsePak:   Materials (mat1/2/3/4):  %u / %u / %u / %u\n",
               m_pakHeader.mat1_num, m_pakHeader.mat2_num, m_pakHeader.mat3_num, m_pakHeader.mat4_num);
        LvlLog("ParsePak:   Block1 size (raw/zlib):  %u / %u\n",
               m_pakHeader.block1_size, m_pakHeader.block1_size_comp);
        LvlLog("ParsePak:   Block2 size (raw/zlib):  %u / %u\n",
               m_pakHeader.block2_size, m_pakHeader.block2_size_comp);
        LvlLog("ParsePak: ═══════════════════════════════════════════════════════════════\n");
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

        // ── STAGE 2: SWAP ALL THE FUCKING ASSET TABLES ──────────────────
        // Block1 is now decompressed. If this was a big-endian console
        // build, every u32 and u16 in every asset-table entry is still
        // backwards as shit. Flip them in place HERE, before any
        // downstream parsing touches the bytes. No-op on PC files.
        // See SwapBlock1AssetTablesBE definition above for the full
        // "what's in scope, what's not" breakdown.
        if (m_bigEndian) SwapBlock1AssetTablesBE();
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

        // ── STAGE 3A WAS WRONG, KILLING THE FUCKING CALL ────────────────
        //
        // Original Stage 3A swapped vertex and index buffer regions in
        // Block2. Turns out vertex+index data lives in BIN ASSETS, not
        // Block2 - see LevelScene.cpp comment around `GetBinAssetData`
        // for the canonical "VBuffInfo.offset is into the binAsset blob,
        // NOT into Block2" note. We were swapping ARBITRARY chunks of
        // Block2 that probably contained texture or radiosity data and
        // shredding them silently. Bounds came back in the 1e38 range
        // because vertex data was never actually swapped, AND textures
        // came back as zero-loaded because we corrupted the DXT blocks.
        //
        // The correct swap happens in GetBinAssetData below - for BE
        // files, the returned blob gets u32-swapped which makes vertex
        // floats read correctly. Index buffers (u16 inside the same
        // blob) get pair-shuffled by u32 swap which is technically
        // wrong - triangles will render with scrambled topology - but
        // at least vertex POSITIONS will be in the right place. Per-
        // region surgical swap is the next iteration.
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

    // Re-apply accumulated deletions to freshly parsed gameObjs.
    // The save pipeline uses "always from original PAK" design, so m_deletedGuids
    // persists across reloads. Without this filter, entities that were wiped in a
    // previous editing session would reappear in the viewport after reload.
    if (!m_deletedGuids.empty()) {
        int removedOnReload = 0;
        for (int gi = (int)m_gameObjs.size() - 1; gi >= 0; --gi) {
            if (m_deletedGuids.count(m_gameObjs[gi].guid)) {
                m_gameObjs.erase(m_gameObjs.begin() + gi);
                ++removedOnReload;
            }
        }
        if (removedOnReload > 0)
            LvlLog("  Re-applied %d deletions from m_deletedGuids on reload\n", removedOnReload);
    }

    // Re-append pending entities that haven't been saved to this PAK yet.
    // When reloading the original PAK after a save, previously added entities
    // exist only in m_pendingObjs — re-add their display entries to m_gameObjs.
    if (!m_pendingObjs.empty()) {
        int reAdded = 0;
        for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
            const PendingGameObj& pgo = m_pendingObjs[pi];
            // Check if this pending entity already exists in m_gameObjs (loaded from _modified PAK)
            bool exists = false;
            for (size_t gi = 0; gi < m_gameObjs.size(); ++gi) {
                if (m_gameObjs[gi].guid == pgo.guid) { exists = true; break; }
            }
            if (exists) continue;

            LevelGameObjEntry e;
            e.guid = pgo.guid;
            e.parent_guid = pgo.parent_guid;
            e.gamemodemask = pgo.gamemodemask;
            e.name_crc = pgo.name_crc;
            e.name = ResolveCrc(pgo.name_crc);
            if (e.name.empty()) {
                char buf[32]; sprintf(buf, "0x%08X", pgo.name_crc);
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
            e.block1_wt_offset = 0;
            e.block1_tf_offset = 0;
            e.block1_obj_offset = 0;
            e.block1_obj_size = 0;
            e.type_def_index = pgo.type_def_index;
            e.target_guid = pgo.target_guid;
            e.delay = 0;
            e.sticky = (pgo.sticky != 0);
            e.output_event = pgo.output_event;
            e.input_event = pgo.input_action;
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
            ++reAdded;
        }
        if (reAdded > 0)
            LvlLog("  Re-added %d pending entities to m_gameObjs on reload\n", reAdded);
    }

    // Build flat BIN asset list with CRC-resolved names (done after ParsePak so string tables are loaded).
    // Walks both halves of the split map. Models come first to preserve
    // the historical order the inventory panel showed; textures append.
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
    for (std::map<uint32_t, BinAssetEntry>::const_iterator it = m_binTextureAssets.begin();
         it != m_binTextureAssets.end(); ++it)
    {
        BinFlatEntry fe;
        fe.key       = it->second.key;
        fe.name      = ResolveCrc(it->second.key);
        fe.kind      = it->second.kind;
        fe.size      = it->second.size;
        fe.size_comp = it->second.size_comp;
        m_binFlat.push_back(fe);
    }
    LvlLog("  BIN flat list done: %u entries (models=%u textures=%u)\n",
           (uint32_t)m_binFlat.size(),
           (uint32_t)m_binAssets.size(),
           (uint32_t)m_binTextureAssets.size());

    // Embedded-weapon sidecar. Lives at <dump_dir>/ze_embedded_weapons.json,
    // where dump_dir = pak path minus ".PAK". Written by level_patcher.py
    // every time the Crowd Mesh Builder bakes a merged CRD. Optional —
    // levels without baked merged CRDs simply have no sidecar.
    LoadEmbeddedWeaponSidecar();

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

// ═════════════════════════════════════════════════════════════════════════
//  Collab broadcast hooks (Chunk 3)
// ═════════════════════════════════════════════════════════════════════════
//
// Registered by CollabSession the moment someone clicks Host or Join.
// Cleared the moment someone clicks Disconnect or shuts down. In
// between, every single motherfucking edit that lands in this file -
// every DragFloat tick while a user scrubs a slider, every pin-to-pin
// wire drag on the Chain view, every right-click Wipe, every Revert,
// every Cancel All Edits button - fires one of these hooks and the
// networking side broadcasts it to the other peer within about twenty
// milliseconds. Human latency budget for real-time collab is ~150ms
// before it feels like ass; we burn 20 here and still have headroom.
//
// When there is NO session active, every single hook pointer is NULL
// and the hook path is one pointer compare + branch. Branch predictor
// eats that for breakfast on every modern CPU. The cost of living with
// this machinery during solo editing is precisely dick. If you ever
// convince yourself the hooks are "too expensive" and you should #ifdef
// them out for solo builds, you are wrong and you should walk away.
//
// We DELIBERATELY do NOT fire the hook from the ApplyBroadcast* twins.
// Those are the inbound apply path: "peer sent us an edit, apply it
// locally, shut the fuck up about it." If we fired the hook there too,
// every incoming edit would immediately bounce right back out the wire
// to the peer that sent it, they would apply it, which would fire
// THEIR hook, which would bounce it back to us, which would fire ours
// again. In less than a second both editors are saturating a hundred
// megabits of LAN bandwidth shovelling the same single FieldEdit back
// and forth like a thermonuclear echo chamber. It is the networking
// equivalent of criticality: one misplaced neutron, one accidental
// hook fire on the apply path, and you have a runaway chain reaction
// until somebody yanks the ethernet cable and both ImGui threads have
// locked up solid trying to drain a queue that refills faster than
// it empties. Do NOT touch the Apply* path. That path is quiet on
// purpose. That quiet is the ONLY thing keeping the bomb from going
// critical. That is LITERALLY why it exists.
static LevelReader::CollabFieldEditFn        s_collabOnFieldEdit       = NULL;
static LevelReader::CollabAddEntityFn        s_collabOnAddEntity       = NULL;
static LevelReader::CollabDeleteGuidFn       s_collabOnDeleteGuid      = NULL;
static LevelReader::CollabClearFieldEditsFn  s_collabOnClearFieldEdits = NULL;

void LevelReader::SetCollabHooks(CollabFieldEditFn onFieldEdit,
                                 CollabAddEntityFn onAddEntity,
                                 CollabDeleteGuidFn onDelete,
                                 CollabClearFieldEditsFn onClear) {
    s_collabOnFieldEdit       = onFieldEdit;
    s_collabOnAddEntity       = onAddEntity;
    s_collabOnDeleteGuid      = onDelete;
    s_collabOnClearFieldEdits = onClear;
}

void LevelReader::ClearCollabHooks() {
    s_collabOnFieldEdit       = NULL;
    s_collabOnAddEntity       = NULL;
    s_collabOnDeleteGuid      = NULL;
    s_collabOnClearFieldEdits = NULL;
}

// The three tiny mutators that used to be inline one-liners in the header.
// Moved here so they can see the static hook pointers above. Behavior for
// every existing local caller is IDENTICAL - they still push_back, insert,
// clear, the vectors look the exact same as before. The only fucking
// addition is the conditional hook fire at the end, which costs ONE null
// check in solo mode (branch predictor absolutely inhales that, it is
// free) and does the right thing in a live session.
//
// DO NOT, UNDER ANY FUCKING CIRCUMSTANCES, bypass these methods and shove
// data straight into m_fieldEdits / m_deletedGuids / m_pendingObjs from
// some other code path because "it's easier" or "it's a special case" or
// "it's just this one spot". It is never just this one spot. I will find
// your commit. I will be very, VERY disappointed. The other peer will
// never hear about your sneaky little push_back because no hook fired.
// The session will diverge silently - host and client each living in
// their own parallel universe where one has three entities the other
// has never seen - and you will spend a deeply unpleasant hour at the
// debugger wondering why Laptop A says "save successful" and Laptop B
// says "what are you even talking about". USE. THESE. METHODS. Every
// goddamn time. No exceptions. No shortcuts. No "I'll refactor it
// later". Later never comes and the bug does.
void LevelReader::AddFieldEdit(const FieldEdit& edit) {
    m_fieldEdits.push_back(edit);
    if (s_collabOnFieldEdit) s_collabOnFieldEdit(edit);
}

void LevelReader::AddDeletedGuid(uint32_t guid) {
    m_deletedGuids.insert(guid);
    if (s_collabOnDeleteGuid) s_collabOnDeleteGuid(guid);
}

void LevelReader::ClearFieldEdits() {
    m_fieldEdits.clear();
    if (s_collabOnClearFieldEdits) s_collabOnClearFieldEdits();
}

// ─────────────────────────────────────────────────────────────────────────
//  SyncPendingFromFieldEdits — the "takes 2 sessions" killer
// ─────────────────────────────────────────────────────────────────────────
// Pending entities are dumped to ze_pending_entities.json by the EXE-side
// WritePendingEntitiesJson function. That function reads from THREE places:
//   1. PendingGameObj::world_transform (initial spawn matrix)
//   2. PendingGameObj::intOverrides / floatOverrides / stringOverrides
//      (set ONLY at creation time)
//   3. PendingGameObj::fieldData (initial bytes, set in AddPendingEntity)
//
// The property panel and the viewport gizmo both write through SetFieldValue
// + AddFieldEdit. That path lands new bytes in m_block1 and records a
// FieldEdit in m_fieldEdits, but NEVER touches the three places above.
// Result: a brand-new entity that you create + move + edit fields on saves
// with INITIAL coords and INITIAL fields. Reload, edit AGAIN, save AGAIN,
// and on the second pass it works because the entity is now in the
// original Block1 (not pending) and the FieldEdit pipeline targets it.
// Hence the cursed "you need to relaunch twice to actually save anything"
// experience the user has been eating for weeks.
//
// This function closes the gap. Walk m_fieldEdits forward (so latest edit
// wins per (guid, field)), and for every edit whose entityGuid matches a
// pending entity, fold the new value into all three storage locations so
// WritePendingEntitiesJson picks it up on the very first save.
//
// Idempotent. Safe to call multiple times. Call it once right before the
// JSON dump, exactly like a flush.
void LevelReader::SyncPendingFromFieldEdits() {
    if (m_pendingObjs.empty() || m_fieldEdits.empty()) return;

    // Build guid -> pending index map once
    std::map<uint32_t, size_t> guidToPending;
    for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
        guidToPending[m_pendingObjs[pi].guid] = pi;
    }

    for (size_t ei = 0; ei < m_fieldEdits.size(); ++ei) {
        const FieldEdit& fe = m_fieldEdits[ei];
        std::map<uint32_t, size_t>::iterator pit = guidToPending.find(fe.entityGuid);
        if (pit == guidToPending.end()) continue;
        PendingGameObj& pgo = m_pendingObjs[pit->second];

        // ── WorldTransform / Transform matrix edit (gizmo drag) ──
        if (fe.kind == 4) {
            memcpy(pgo.world_transform, fe.matrixVal, 64);
            // Also stamp the matrix into fieldData so any path that reads
            // the matrix from binary bytes (CRC fallback in the dumper)
            // sees the same value.
            if (pgo.type_def_index >= 0 &&
                pgo.type_def_index < (int)m_gameObjTypes.size()) {
                const LevelGameObjTypeDef& td = m_gameObjTypes[pgo.type_def_index];
                for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                    const GOFieldDefEntry& fd = td.fields[fi];
                    FieldKind fk = FieldKindFromCrc(fd.kind_crc);
                    if (fk != FK_MATRIX4X4) continue;
                    bool isWT = _stricmp(fd.name.c_str(), "WorldTransform") == 0;
                    bool isTF = _stricmp(fd.name.c_str(), "Transform") == 0;
                    if (!isWT && !isTF) continue;
                    if (fd.offset + 64 <= (uint32_t)pgo.fieldData.size()) {
                        memcpy(&pgo.fieldData[fd.offset], fe.matrixVal, 64);
                    }
                }
            }
            continue;
        }

        // Field-name-keyed edits (kinds 0/1/2/3/5/6/7/8). Resolve the
        // matching field def by name. Try the literal string match first
        // because case differs between hardcoded paths and reference
        // level JSON imports ("GUID" vs "guid" lurks here too).
        if (pgo.type_def_index < 0 ||
            pgo.type_def_index >= (int)m_gameObjTypes.size()) continue;
        const LevelGameObjTypeDef& td = m_gameObjTypes[pgo.type_def_index];
        const GOFieldDefEntry* targetFd = NULL;
        uint32_t feCrc = LotrHashString(fe.fieldName.c_str());
        for (size_t fi = 0; fi < td.fields.size(); ++fi) {
            const GOFieldDefEntry& fd = td.fields[fi];
            if (!fd.name.empty() && _stricmp(fd.name.c_str(), fe.fieldName.c_str()) == 0) {
                targetFd = &fd;
                break;
            }
            uint32_t altCrc = fd.name.empty() ? fd.name_crc
                                              : LotrHashString(fd.name.c_str());
            if (fd.name_crc == feCrc || altCrc == feCrc) {
                targetFd = &fd;
                break;
            }
        }
        if (!targetFd) continue;
        uint32_t off = targetFd->offset;

        switch (fe.kind) {
        case 0: { // int
            pgo.intOverrides[targetFd->name_crc] = (uint32_t)fe.intVal;
            if (off + 4 <= pgo.fieldData.size())
                memcpy(&pgo.fieldData[off], &fe.intVal, 4);
        } break;
        case 1: { // float
            pgo.floatOverrides[targetFd->name_crc] = fe.floatVal;
            if (off + 4 <= pgo.fieldData.size())
                memcpy(&pgo.fieldData[off], &fe.floatVal, 4);
        } break;
        case 2: { // guid
            pgo.intOverrides[targetFd->name_crc] = fe.guidVal;
            if (off + 4 <= pgo.fieldData.size())
                memcpy(&pgo.fieldData[off], &fe.guidVal, 4);
        } break;
        case 3: { // vec3
            if (off + 12 <= pgo.fieldData.size())
                memcpy(&pgo.fieldData[off], fe.vec3Val, 12);
        } break;
        case 5: { // string -> stored as CRC in binary
            pgo.stringOverrides[targetFd->name_crc] = fe.stringVal;
            if (off + 4 <= pgo.fieldData.size()) {
                uint32_t strCrc = fe.stringVal.empty()
                    ? 0u : LotrHashString(fe.stringVal.c_str());
                memcpy(&pgo.fieldData[off], &strCrc, 4);
            }
        } break;
        // kinds 6/7/8 (listItem / floatArray / listAppend) are list edits.
        // The list-write path in WritePendingEntitiesJson already reads
        // p.listOverrides, but the per-item / per-element / append edit
        // shapes don't have a clean 1:1 mapping to listOverrides without
        // rebuilding the whole list from m_block1. Leave them for the
        // next pass — none of the user-reported "move+edit takes 2
        // sessions" bug is a list edit. Filing as a TODO with teeth.
        default: break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
//  ApplyBroadcastFieldEdit - the receive-side twin that does the REAL work
// ─────────────────────────────────────────────────────────────────────────
// This is the motherfucking function that was missing from the first Chunk
// 3 pass. Without it, every incoming FieldEdit got logged into m_fieldEdits
// and then just SAT THERE, silently, while the user stared at an unchanging
// slider on the other laptop and concluded the whole session was broken.
// The lesson (paid for with an evening of "it does not work and I do not
// know why"): the local edit path is a TWO-step combo. Byte write into
// m_block1 to make the UI update NOW, and record into m_fieldEdits for
// save later. Both. Every time. If an apply path only does half of it,
// the other peer looks like a goddamn broken mirror.
//
// Lookup is entity-by-guid then field-by-name against the type def. We
// use NAME not OFFSET on the wire because field offsets can differ between
// peers if one imported a type def from a different reference level (yes,
// this is a thing that can happen in this codebase, ImportTypeDef can
// pull slightly different fields[] ordering from different source levels).
// Names are authoritative, offsets are derived. Trust the name.
void LevelReader::ApplyBroadcastFieldEdit(const FieldEdit& edit) {
    // 1. Find the entity. Linear scan because m_gameObjs is not indexed
    //    by guid - could add a map but this runs at network-edit rate,
    //    not per-frame, so a scan of a few thousand entries is nothing.
    int entIdx = -1;
    for (size_t i = 0; i < m_gameObjs.size(); ++i) {
        if (m_gameObjs[i].guid == edit.entityGuid) { entIdx = (int)i; break; }
    }
    if (entIdx < 0) {
        LvlLog("ApplyBroadcastFieldEdit: entity guid=0x%08X not found\n", edit.entityGuid);
        m_fieldEdits.push_back(edit); // log it anyway so save sees it, even if UI does not
        return;
    }
    const LevelGameObjEntry& ent = m_gameObjs[entIdx];

    // 2. Find the field in the type def by name. Case-insensitive match
    //    because some reference levels capitalize "Delay" and others
    //    decide to go with "delay" or "DELAY" like it is a fucking
    //    protest sign. Do not assume consistency you do not have.
    uint32_t fieldOff = 0xFFFFFFFF;
    if (ent.type_def_index >= 0 && ent.type_def_index < (int)m_gameObjTypes.size()) {
        const LevelGameObjTypeDef& td = m_gameObjTypes[ent.type_def_index];
        for (size_t i = 0; i < td.fields.size(); ++i) {
            if (_stricmp(td.fields[i].name.c_str(), edit.fieldName.c_str()) == 0) {
                fieldOff = td.fields[i].offset;
                break;
            }
        }
    }
    if (fieldOff == 0xFFFFFFFF) {
        LvlLog("ApplyBroadcastFieldEdit: field '%s' not found on entity guid=0x%08X type_idx=%d\n",
               edit.fieldName.c_str(), edit.entityGuid, ent.type_def_index);
        m_fieldEdits.push_back(edit);
        return;
    }
    uint32_t absOff = ent.block1_obj_offset + fieldOff;

    // 3. Kind-dispatched byte write. Exactly the same width/semantics as
    //    the local SetFieldValue calls in ZeroEngine3DViewport.cpp around
    //    the propEditKind switch - if those ever change, change these too
    //    or live with the two sides silently diverging on specific types.
    switch (edit.kind) {
    case 0: SetFieldValue(absOff, &edit.intVal, 4); break;   // int
    case 1: SetFieldValue(absOff, &edit.floatVal, 4); break; // float
    case 2: SetFieldValue(absOff, &edit.guidVal, 4); break;  // guid
    case 3: SetFieldValue(absOff, edit.vec3Val, 12); break;  // vec3
    case 4: SetFieldValue(absOff, edit.matrixVal, 64); break; // matrix
    case 5: {
        // String fields are stored as a CRC of the string, not the string
        // itself - the bytes we write are the 4-byte hash, not the chars.
        // String table lookup happens elsewhere on the render path.
        uint32_t crc = LotrHashString(edit.stringVal.c_str());
        SetFieldValue(absOff, &crc, 4);
        break;
    }
    case 6: {
        // listItem: edit.listIndex says which slot of the GuidList to
        // overwrite, edit.guidVal is the new GUID. The layout mirrors the
        // local edit path from the viewport (count + offset then items).
        uint16_t listCount = 0, listDataOff = 0;
        GetFieldValue(absOff, &listCount, 2);
        GetFieldValue(absOff + 2, &listDataOff, 2);
        if (edit.listIndex >= 0 && edit.listIndex < listCount) {
            uint32_t itemOff = absOff + 4 + listDataOff + edit.listIndex * 4;
            SetFieldValue(itemOff, &edit.guidVal, 4);
        }
        break;
    }
    case 7: {
        // floatArray - flat float blob (e.g. spline node positions). The
        // local path writes arrayStride * count floats back into m_block1
        // at absOff. Same here.
        if (!edit.arrayVal.empty()) {
            SetFieldValue(absOff, &edit.arrayVal[0], (uint32_t)(edit.arrayVal.size() * sizeof(float)));
        }
        break;
    }
    default:
        LvlLog("ApplyBroadcastFieldEdit: unknown kind %d for field '%s'\n",
               edit.kind, edit.fieldName.c_str());
        break;
    }

    // 4. And THEN record into m_fieldEdits so save picks it up. This
    //    line is what the naive one-line implementation was, and by
    //    itself it is worth exactly dick without the 60 lines above.
    m_fieldEdits.push_back(edit);
}

void LevelReader::AddPendingEntity(const PendingGameObj& obj, bool trustIncomingGuid) {
    // Generate GUID and set type CRC
    PendingGameObj pgo = obj;
    // trustIncomingGuid = true means this call came from the network apply
    // path and the sender already stamped a GUID they expect us to honor.
    // Our local m_nextGuid partitioning (host 0x0X, client 0xC1, 0xC2...)
    // means their GUID is guaranteed not to collide with any we would mint
    // locally. If we re-generated here we would spawn a ghost duplicate
    // with OUR prefix and the original peer's reference-by-GUID edits
    // would hit the wrong entity, or nothing at all.
    if (!(trustIncomingGuid && pgo.guid != 0)) {
        pgo.guid = GenerateGuid();
    }
    if (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size()) {
        pgo.type_crc = m_gameObjTypes[pgo.type_def_index].crc;
    } else {
        pgo.type_crc = 0;
    }

    // Build field data by cloning from an existing instance of the same type.
    // This ensures ALL fields have valid game defaults (Health, Team, LODs, etc.).
    // Then override identity fields: GUID, ParentGUID, GameModeMask, Name, Mesh,
    // WorldTransform, Transform.

    // Field name matching helper — case-insensitive
    // Type definitions from different reference levels use inconsistent casing
    // (e.g., "GUID" vs "guid", "Name" vs "name")
    #define FIELD_IS(fd, str) (_stricmp((fd).name.c_str(), (str)) == 0)

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

                if (FIELD_IS(fd, "GUID")) {
                    memcpy(fptr, &pgo.guid, 4);
                } else if (FIELD_IS(fd, "ParentGUID")) {
                    memcpy(fptr, &pgo.parent_guid, 4);
                } else if (FIELD_IS(fd, "GameModeMask")) {
                    memcpy(fptr, &pgo.gamemodemask, 4);
                } else if (FIELD_IS(fd, "Name")) {
                    memcpy(fptr, &pgo.name_crc, 4);
                } else if (FIELD_IS(fd, "Mesh") && pgo.mesh_crc != 0) {
                    memcpy(fptr, &pgo.mesh_crc, 4);
                } else if (FIELD_IS(fd, "WorldTransform") || FIELD_IS(fd, "Transform")) {
                    if (fk == FK_MATRIX4X4 && fd.offset + 64 <= (uint32_t)pgo.fieldData.size()) {
                        memcpy(fptr, pgo.world_transform, 64);
                    }
                } else if (FIELD_IS(fd, "Outputs") || FIELD_IS(fd, "InitChildObjs")) {
                    // Clear list references — new entity has no wired outputs/children.
                    // Write empty list header: num=0, offset points past fixed region.
                    if (FieldKindIsList(fk)) {
                        uint16_t listNum = 0;
                        uint16_t listOff = (uint16_t)(pgo.fieldData.size() - fd.offset - 4);
                        memcpy(fptr, &listNum, 2);
                        memcpy(fptr + 2, &listOff, 2);
                    }
                } else if (FIELD_IS(fd, "Target") && pgo.target_guid != 0) {
                    memcpy(fptr, &pgo.target_guid, 4);
                } else if (FIELD_IS(fd, "Output") && !pgo.output_event.empty()) {
                    uint32_t evCrc = LotrHashString(pgo.output_event.c_str());
                    memcpy(fptr, &evCrc, 4);
                } else if (FIELD_IS(fd, "Input") && !pgo.input_action.empty()) {
                    uint32_t inCrc = LotrHashString(pgo.input_action.c_str());
                    memcpy(fptr, &inCrc, 4);
                } else if (FIELD_IS(fd, "Sticky")) {
                    int32_t stickyVal = pgo.sticky;
                    memcpy(fptr, &stickyVal, 4);
                }

                // Generic field overrides — try both fd.name_crc and canonical CRC
                // Override maps use LC_GM_* CRCs (PascalCase), but field defs may be lowercase
                uint32_t lookupCrc = fd.name_crc;
                // If field has a name string, also try its canonical (stored) CRC
                // This handles case mismatch: override keyed by LotrHash("Mode") vs field "mode"
                uint32_t altCrc = fd.name.empty() ? fd.name_crc : LotrHashString(fd.name.c_str());
                // Int/bool/guid/crc overrides
                {
                    std::map<uint32_t, uint32_t>::const_iterator oit = pgo.intOverrides.find(lookupCrc);
                    if (oit == pgo.intOverrides.end() && altCrc != lookupCrc)
                        oit = pgo.intOverrides.find(altCrc);
                    if (oit != pgo.intOverrides.end()) {
                        memcpy(fptr, &oit->second, 4);
                    }
                }
                // Float overrides
                {
                    std::map<uint32_t, float>::const_iterator fit = pgo.floatOverrides.find(lookupCrc);
                    if (fit == pgo.floatOverrides.end() && altCrc != lookupCrc)
                        fit = pgo.floatOverrides.find(altCrc);
                    if (fit != pgo.floatOverrides.end()) {
                        memcpy(fptr, &fit->second, 4);
                    }
                }
                // String→CRC overrides
                {
                    std::map<uint32_t, std::string>::const_iterator sit = pgo.stringOverrides.find(lookupCrc);
                    if (sit == pgo.stringOverrides.end() && altCrc != lookupCrc)
                        sit = pgo.stringOverrides.find(altCrc);
                    if (sit != pgo.stringOverrides.end()) {
                        uint32_t strCrc = LotrHashString(sit->second.c_str());
                        memcpy(fptr, &strCrc, 4);
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
    // Append fieldData to m_block1 so the properties system can read/write it
    uint32_t appendOff = (uint32_t)m_block1.size();
    m_block1.insert(m_block1.end(), pgo.fieldData.begin(), pgo.fieldData.end());
    e.block1_wt_offset = 0;
    e.block1_tf_offset = 0;
    e.block1_obj_offset = appendOff;
    e.block1_obj_size = (uint32_t)pgo.fieldData.size();
    e.type_def_index = pgo.type_def_index;
    e.target_guid = pgo.target_guid;
    e.delay = 0;
    e.sticky = (pgo.sticky != 0);
    e.output_event = pgo.output_event;
    e.input_event = pgo.input_action;
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
    for (std::map<uint32_t, std::vector<uint32_t> >::const_iterator lit = pgo.listOverrides.begin();
         lit != pgo.listOverrides.end(); ++lit) {
        e.list_refs[lit->first] = lit->second;
        if (lit->first == LotrHashString("Outputs")) e.outputs = lit->second;
        else if (lit->first == LotrHashString("Layers")) e.layers = lit->second;
        else if (lit->first == LotrHashString("Nodes")) e.nodes = lit->second;
        else if (lit->first == LotrHashString("InitialChildObjects")) e.initial_child_objects = lit->second;
        else if (lit->first == LotrHashString("DropItems")) e.drop_items = lit->second;
    }
    m_gameObjs.push_back(e);

    LvlLog("AddPendingEntity: type=%s guid=%u parent=%u layer=%u pos=(%.1f,%.1f,%.1f) fieldData=%u bytes\n",
           e.type_name.c_str(), pgo.guid, pgo.parent_guid, pgo.layer_guid,
           e.pos[0], e.pos[1], e.pos[2], (unsigned)pgo.fieldData.size());

    // Broadcast hook. Fires for LOCAL calls (user clicked Add Entity, dragged
    // a wire to create an Output, hit the Kit wizard, whatever). Does NOT
    // fire when this is the network apply path (trustIncomingGuid=true),
    // because that is the edit coming IN from the wire and firing would
    // send it right back out. Classic feedback loop we do not want.
    if (!trustIncomingGuid && s_collabOnAddEntity) {
        s_collabOnAddEntity(pgo);
    }
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

// ============================================================
//  ImportTypeDef — Load a type definition from a reference level.json
//  that has the type we need. Searches lotrcparser/<MapName>/sub_blocks1/level.json.
// ============================================================

// Helper: map FieldKind → Rust JSON type string
static const char* FieldKindToJsonType(FieldKind fk) {
    switch (fk) {
    case FK_INT:        return "int";
    case FK_FLOAT:      return "float";
    case FK_BOOL:       return "bool";
    case FK_CRC:        return "crc";
    case FK_GUID:       return "GUID";
    case FK_COLOR:      return "Color";
    case FK_VECTOR3:    return "vector3";
    case FK_MATRIX4X4:  return "matrix4x4";
    case FK_STRING:     return "string";
    case FK_OBJECTLIST: return "objectlist";
    case FK_NODELIST:   return "nodelist";
    case FK_INTLIST:    return "intlist";
    case FK_CRCLIST:    return "crclist";
    case FK_STRINGLIST: return "stringlist";
    case FK_WEIGHTLIST: return "weightlist";
    case FK_MATRIXLIST: return "matrixlist";
    case FK_BYTE:       return "int";
    case FK_VECTOR2:    return "vector3";
    case FK_VECTOR4:    return "vector3";
    default:            return "int";
    }
}

// Helper: map Rust JSON type string → kind CRC
static uint32_t JsonTypeToKindCrc(const char* jsonType) {
    if (!jsonType) return KIND_INT_VAL;
    if (strcmp(jsonType, "int") == 0)         return KIND_INT_VAL;
    if (strcmp(jsonType, "float") == 0)       return KIND_FLOAT_VAL;
    if (strcmp(jsonType, "bool") == 0)        return KIND_BOOL_VAL;
    if (strcmp(jsonType, "crc") == 0)         return KIND_CRC_VAL;
    if (strcmp(jsonType, "GUID") == 0)        return KIND_GUID_VAL;
    if (strcmp(jsonType, "Color") == 0)       return KIND_COLOR_VAL;
    if (strcmp(jsonType, "vector3") == 0)     return KIND_VECTOR3_VAL;
    if (strcmp(jsonType, "matrix4x4") == 0)   return KIND_MATRIX4X4_VAL;
    if (strcmp(jsonType, "string") == 0)      return KIND_STRING_VAL;
    if (strcmp(jsonType, "objectlist") == 0)  return KIND_OBJECTLIST_VAL;
    if (strcmp(jsonType, "nodelist") == 0)    return KIND_NODELIST_VAL;
    if (strcmp(jsonType, "intlist") == 0)     return KIND_INTLIST_VAL;
    if (strcmp(jsonType, "crclist") == 0)     return KIND_CRCLIST_VAL;
    if (strcmp(jsonType, "stringlist") == 0)  return KIND_STRINGLIST_VAL;
    if (strcmp(jsonType, "weightlist") == 0)  return KIND_WEIGHTLIST_VAL;
    if (strcmp(jsonType, "matrixlist") == 0)  return KIND_MATRIXLIST_VAL;
    return KIND_INT_VAL;
}

// Minimal JSON string extractor — finds "key": "value" or "key": number
// Returns empty string if not found. Simple but sufficient for our structured JSON.
static std::string JsonExtractString(const std::string& json, size_t start, const char* key) {
    std::string searchKey = std::string("\"") + key + "\": \"";
    size_t pos = json.find(searchKey, start);
    if (pos == std::string::npos || pos > start + 500) return "";
    pos += searchKey.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static uint32_t JsonExtractUint(const std::string& json, size_t start, const char* key) {
    std::string searchKey = std::string("\"") + key + "\": ";
    size_t pos = json.find(searchKey, start);
    if (pos == std::string::npos || pos > start + 500) return 0;
    pos += searchKey.size();
    return (uint32_t)strtoul(json.c_str() + pos, NULL, 10);
}

int LevelReader::ImportTypeDef(const std::string& typeName, const std::string& lotrcParserDir) {
    // Check if already exists
    for (int ti = 0; ti < (int)m_gameObjTypes.size(); ++ti) {
        if (m_gameObjTypes[ti].name == typeName) return ti;
    }

    // Search reference maps for this type
    static const char* refMaps[] = {
        "BlackGates", "MinasTirith", "Helm'sDeep", "Weathertop",
        "Isengard", "Moria", "PelennorFields", "Mount_Doom",
        "Osgiliath", "MinasTirith_Top", "Rivendell", "Shire",
        "CoriCelesti", "Minas_Morgul", "Training", NULL
    };

    for (int mi = 0; refMaps[mi]; ++mi) {
        std::string refPath = lotrcParserDir + "\\" + refMaps[mi] + "\\sub_blocks1\\level.json";
        FILE* f = fopen(refPath.c_str(), "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        // Only read the types section (at the end of the file)
        // Skip to last 20% of file to find types faster
        long skipTo = sz > 50000 ? sz - (sz / 5) : 0;
        fseek(f, skipTo, SEEK_SET);
        long readSz = sz - skipTo;
        std::vector<char> buf(readSz);
        fread(&buf[0], 1, readSz, f);
        fclose(f);

        std::string chunk(buf.begin(), buf.end());

        // Find the type def for our target type
        std::string typeSearch = "\"name\": \"" + typeName + "\"";
        size_t typePos = chunk.find(typeSearch);
        if (typePos == std::string::npos) continue;

        // Found! Parse the fields array.
        // Walk backwards to find the opening "{" of this type entry
        size_t entryStart = chunk.rfind('{', typePos);
        if (entryStart == std::string::npos) continue;

        // Find "fields": [ ... ]
        size_t fieldsStart = chunk.find("\"fields\":", typePos);
        if (fieldsStart == std::string::npos) continue;
        size_t arrStart = chunk.find('[', fieldsStart);
        if (arrStart == std::string::npos) continue;

        // Find matching "]"
        int depth = 1;
        size_t arrEnd = arrStart + 1;
        while (arrEnd < chunk.size() && depth > 0) {
            if (chunk[arrEnd] == '[') depth++;
            else if (chunk[arrEnd] == ']') depth--;
            arrEnd++;
        }

        // Parse each field entry: { "name": "X", "type": "Y", "offset": N }
        LevelGameObjTypeDef td;
        td.crc = LotrHashString(typeName.c_str());
        td.name = typeName;
        td.instance_count = 0;

        size_t fieldPos = arrStart;
        while (fieldPos < arrEnd) {
            size_t nextBrace = chunk.find('{', fieldPos + 1);
            if (nextBrace == std::string::npos || nextBrace >= arrEnd) break;

            std::string fieldName = JsonExtractString(chunk, nextBrace, "name");
            std::string fieldType = JsonExtractString(chunk, nextBrace, "type");
            uint32_t fieldOffset = JsonExtractUint(chunk, nextBrace, "offset");

            if (!fieldName.empty()) {
                GOFieldDefEntry fe;
                fe.name_crc = LotrHashString(fieldName.c_str());
                fe.kind_crc = JsonTypeToKindCrc(fieldType.c_str());
                fe.offset = fieldOffset;
                fe.name = fieldName;
                fe.kind_name = fieldType;
                td.fields.push_back(fe);

                // Register the field name in CRC map if not already known
                if (m_crcMap.find(fe.name_crc) == m_crcMap.end())
                    m_crcMap[fe.name_crc] = fieldName;
            }

            // Advance past this field entry's closing brace
            size_t closeBrace = chunk.find('}', nextBrace + 1);
            if (closeBrace == std::string::npos) break;
            fieldPos = closeBrace;
        }

        td.field_count = (uint32_t)td.fields.size();

        // Register the type name in CRC map
        if (m_crcMap.find(td.crc) == m_crcMap.end())
            m_crcMap[td.crc] = typeName;

        int newIdx = (int)m_gameObjTypes.size();
        m_gameObjTypes.push_back(td);

        LvlLog("ImportTypeDef: imported '%s' (%d fields) from %s\n",
               typeName.c_str(), (int)td.fields.size(), refMaps[mi]);
        return newIdx;
    }

    LvlLog("ImportTypeDef: type '%s' not found in any reference level\n", typeName.c_str());
    return -1;
}

void LevelReader::ImportAllMissingTypeDefs(const std::string& lotrcParserDir) {
    // Types needed by kit creation
    static const char* kitTypes[] = {
        "construct", "gamemode", "CapturePoint", "trigger_radius", "ToggleObjective",
        "spawn_point", "spawn_node", "spawn_emitter", "spawn_class", "demo_camera",
        "logic_relay", "logic_timer", "logic_counter", "logic_gamestart", "logic_endgame",
        "AIGoal", "aigoal_player", "aigoal_multi_points", "GroupObject", "PlayerRespawner",
        "PointManager", "Output", "Objective", "ObjectiveLocator", "AcquireObject",
        "trigger_box", "trigger_sphere", "Prop_Havok", "Prop_Anim", "HUDMovie",
        "text_object", "PathNetwork", "PathNode", "PathLink", "SoundEmitter",
        "Locator", "AtmosphereSetting", "light_sun", "light_point",
        "LivesManager", "logic_compare", "MetaReward", "Collision",
        NULL
    };

    int imported = 0;
    for (int i = 0; kitTypes[i]; ++i) {
        // Check if already exists
        bool exists = false;
        for (int ti = 0; ti < (int)m_gameObjTypes.size(); ++ti) {
            if (m_gameObjTypes[ti].name == kitTypes[i]) { exists = true; break; }
        }
        if (!exists) {
            int idx = ImportTypeDef(kitTypes[i], lotrcParserDir);
            if (idx >= 0) ++imported;
        }
    }
    if (imported > 0)
        LvlLog("ImportAllMissingTypeDefs: imported %d type definitions\n", imported);

    // Register common editor strings that donors reference but may not be in this level's string table
    static const char* editorStrings[] = {
        "Billboard", "Box", "Box_Trigger", "Cylinder_Wire", "Directional", "Dummy",
        "Sphere", "Sphere_Wire", "point", "directional", "3dReticle", "Crosshair",
        "CircleDir", "ArcAuto", "Source_Inst_Damage", "Source_Inst_Heal",
        "Inst_Damage", "Inst_Heal", "Inst_Dispel", "DamageOverTime", "HealOverTime",
        "DamageAdj", "SpeedAdj", "IncDamageMod", "BreakerOverTime", "Invincible",
        "Stealthed", "LightningAbility", "Src_Inst_Dmg_No_Death",
        "fed_timer.tga", "fed_gamestart.tga", "fed_relay.tga", "fed_counter.tga",
        "fed_endgame.tga", "fed_goal.tga", "fed_group.tga", "fed_locator.tga",
        "fed_soundemitter.tga", "fed_campaignmanager.tga", "fed_case.tga",
        "fed_characterui.tga", "fed_compare.tga", "fed_eventmonitor.tga",
        "fed_globaleffects.tga", "fed_HUDMovie.tga", "fed_kill.tga",
        "fed_objnontoggle.tga", "fed_objpointmgr.tga", "fed_objtoggle.tga",
        "fed_pausewindow.tga", "fed_resourceemitter.tga", "fed_rewards.tga",
        "fed_splinecreature.tga", "fed_staticwindow.tga", "fed_stopgame.tga",
        "fed_target.tga", "fed_voiceover.tga",
        "Fx_TrailDefault", "Fx_TrailDagger", "Fx_Arrow_Trail_03",
        "Fx_TRL_Fire", "Fx_TRL_FirePurple", "Fx_TRL_Undead",
        NULL
    };
    for (int si = 0; editorStrings[si]; ++si) {
        uint32_t crc = LotrHashString(editorStrings[si]);
        if (m_crcMap.find(crc) == m_crcMap.end())
            m_crcMap[crc] = editorStrings[si];
    }
}

// ============================================================
//  ExportPendingToJson — Write pending entities into a Rust parser
//  dump directory. Reads level.json, appends new entities as JSON
//  objects, writes back. Also appends new strings to pak_strings.json.
// ============================================================
bool LevelReader::ExportPendingToJson(const std::string& dumpDir) {
    if (m_pendingObjs.empty()) {
        LvlLog("ExportPendingToJson: no pending entities, nothing to export\n");
        return true;
    }

    // ── Step 1: Find and read level.json ──
    std::string levelJsonPath = dumpDir + "\\sub_blocks1\\level.json";
    // Try sub_blocks0 if sub_blocks1 doesn't exist
    {
        FILE* test = fopen(levelJsonPath.c_str(), "r");
        if (!test) {
            levelJsonPath = dumpDir + "\\sub_blocks0\\level.json";
            test = fopen(levelJsonPath.c_str(), "r");
        }
        if (!test) {
            m_error = "ExportPendingToJson: level.json not found in dump directory";
            return false;
        }
        fclose(test);
    }

    // Read the entire file
    std::vector<char> jsonData;
    {
        FILE* f = fopen(levelJsonPath.c_str(), "rb");
        if (!f) { m_error = "Cannot open " + levelJsonPath; return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        jsonData.resize(sz);
        fread(&jsonData[0], 1, sz, f);
        fclose(f);
    }

    // ── Step 2: Find insertion point ──
    // The JSON structure is: { "gamemodemask": ..., "objs": [ ... ] }
    // We need to insert before the final "]" of the objs array.
    // Find the LAST occurrence of "]" that closes the objs array.
    // Strategy: find last "]\n}" — that's the array close + object close.
    std::string json(jsonData.begin(), jsonData.end());
    size_t insertPos = std::string::npos;
    // Search backwards for the closing of objs array
    size_t lastBracket = json.rfind(']');
    if (lastBracket != std::string::npos) {
        insertPos = lastBracket;
    }
    if (insertPos == std::string::npos) {
        m_error = "ExportPendingToJson: cannot find objs array end in level.json";
        return false;
    }

    // ── Step 3: Build JSON for each pending entity ──
    std::string newEntities;
    for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
        const PendingGameObj& pgo = m_pendingObjs[pi];

        // Get type name
        std::string typeName = (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size())
            ? m_gameObjTypes[pgo.type_def_index].name : "unknown";

        // Get layer GUID
        uint32_t layerGuid = pgo.layer_guid;

        newEntities += ",\n    {\n";
        newEntities += "      \"type\": \"" + typeName + "\",\n";
        char layerBuf[32]; sprintf(layerBuf, "%u", layerGuid);
        newEntities += "      \"layer\": " + std::string(layerBuf) + ",\n";
        newEntities += "      \"fields\": {\n";

        // Write fields from type definition + fieldData
        if (pgo.type_def_index >= 0 && pgo.type_def_index < (int)m_gameObjTypes.size()) {
            const LevelGameObjTypeDef& td = m_gameObjTypes[pgo.type_def_index];
            bool firstField = true;
            for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                const GOFieldDefEntry& fd = td.fields[fi];
                FieldKind fk = FieldKindFromCrc(fd.kind_crc);

                // Resolve field name
                std::string fieldName = ResolveCrc(fd.name_crc);
                if (fieldName.empty()) {
                    char fb[32]; sprintf(fb, "0x%08X", fd.name_crc);
                    fieldName = fb;
                }

                if (!firstField) newEntities += ",\n";
                firstField = false;
                newEntities += "        \"" + fieldName + "\": ";

                // Read value from fieldData
                const uint8_t* fptr = (fd.offset < pgo.fieldData.size())
                    ? &pgo.fieldData[fd.offset] : NULL;

                if (!fptr || fd.offset + FieldKindFixedSize(fk) > (uint32_t)pgo.fieldData.size()) {
                    // Out of bounds — write default
                    if (fk == FK_FLOAT) newEntities += "0.0";
                    else if (fk == FK_BOOL) newEntities += "false";
                    else if (fk == FK_STRING || fk == FK_CRC) newEntities += "\"\"";
                    else if (fk == FK_MATRIX4X4) newEntities += "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
                    else if (fk == FK_VECTOR3) newEntities += "[0,0,0]";
                    else if (FieldKindIsList(fk)) newEntities += "[]";
                    else newEntities += "0";
                    continue;
                }

                char vbuf[128];
                switch (fk) {
                case FK_INT: case FK_GUID: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    sprintf(vbuf, "%u", v);
                    newEntities += vbuf;
                } break;

                case FK_FLOAT: {
                    float v; memcpy(&v, fptr, 4);
                    sprintf(vbuf, "%.7g", v);
                    newEntities += vbuf;
                } break;

                case FK_BOOL: {
                    int32_t v; memcpy(&v, fptr, 4);
                    newEntities += (v ? "true" : "false");
                } break;

                case FK_CRC: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    std::string resolved = ResolveCrc(v);
                    if (!resolved.empty())
                        newEntities += "\"" + resolved + "\"";
                    else {
                        sprintf(vbuf, "%u", v);
                        newEntities += vbuf; // unresolved CRC as number
                    }
                } break;

                case FK_STRING: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    std::string resolved = ResolveCrc(v);
                    newEntities += "\"" + resolved + "\"";
                } break;

                case FK_COLOR: {
                    uint32_t v; memcpy(&v, fptr, 4);
                    sprintf(vbuf, "\"0x%08X\"", v);
                    newEntities += vbuf;
                } break;

                case FK_VECTOR3: {
                    float x, y, z;
                    memcpy(&x, fptr, 4); memcpy(&y, fptr+4, 4); memcpy(&z, fptr+8, 4);
                    sprintf(vbuf, "[%.7g, %.7g, %.7g]", x, y, z);
                    newEntities += vbuf;
                } break;

                case FK_MATRIX4X4: {
                    newEntities += "[\n          ";
                    for (int mi = 0; mi < 16; ++mi) {
                        float v; memcpy(&v, fptr + mi*4, 4);
                        sprintf(vbuf, "%.7g", v);
                        newEntities += vbuf;
                        if (mi < 15) newEntities += ",\n          ";
                    }
                    newEntities += "\n        ]";
                } break;

                case FK_OBJECTLIST: case FK_NODELIST: case FK_INTLIST:
                case FK_CRCLIST: case FK_STRINGLIST: case FK_WEIGHTLIST: case FK_MATRIXLIST: {
                    // List fields: read count + offset from fixed region
                    uint16_t listNum, listOff;
                    memcpy(&listNum, fptr, 2);
                    memcpy(&listOff, fptr + 2, 2);
                    if (listNum == 0 || fk == FK_INTLIST) {
                        newEntities += "[]";
                    } else {
                        // Read list items from fieldData tail region
                        uint32_t dataStart = fd.offset + 4 + listOff;
                        newEntities += "[";
                        for (int li = 0; li < listNum; ++li) {
                            if (li > 0) newEntities += ", ";
                            uint32_t itemOff = dataStart + li * 4;
                            if (itemOff + 4 <= (uint32_t)pgo.fieldData.size()) {
                                uint32_t item; memcpy(&item, &pgo.fieldData[itemOff], 4);
                                if (fk == FK_CRCLIST || fk == FK_STRINGLIST) {
                                    std::string resolved = ResolveCrc(item);
                                    if (!resolved.empty())
                                        newEntities += "\"" + resolved + "\"";
                                    else {
                                        sprintf(vbuf, "%u", item);
                                        newEntities += vbuf;
                                    }
                                } else {
                                    sprintf(vbuf, "%u", item);
                                    newEntities += vbuf;
                                }
                            } else {
                                newEntities += "0";
                            }
                        }
                        newEntities += "]";
                    }
                } break;

                default: {
                    // Unknown kind — write as int
                    uint32_t v = 0;
                    if (fd.offset + 4 <= (uint32_t)pgo.fieldData.size())
                        memcpy(&v, fptr, 4);
                    sprintf(vbuf, "%u", v);
                    newEntities += vbuf;
                } break;
                }
            }
        }

        newEntities += "\n      }\n";
        newEntities += "    }";
    }

    // ── Step 4: Insert entities into objs[] ──
    json.insert(insertPos, newEntities);

    // ── Step 4b: Inject missing type definitions into types[] ──
    // If we added entities of types that don't exist in the dump's types array,
    // the Rust compiler won't know how to serialize them. We must add the type def.
    {
        // FieldKind → Rust JSON type string mapping (uses static function above)

        // Collect unique type names needed by pending entities
        std::set<std::string> neededTypes;
        for (size_t pi = 0; pi < m_pendingObjs.size(); ++pi) {
            if (m_pendingObjs[pi].type_def_index >= 0 &&
                m_pendingObjs[pi].type_def_index < (int)m_gameObjTypes.size()) {
                neededTypes.insert(m_gameObjTypes[m_pendingObjs[pi].type_def_index].name);
            }
        }

        // Check which are already in the JSON (search for "name": "TypeName" in types section)
        // Find the types section
        size_t typesPos = json.find("\"types\":");
        if (typesPos != std::string::npos) {
            // Find the closing "]" of the types array
            size_t typesEnd = json.rfind(']');
            // Make sure this is after the types section start
            if (typesEnd != std::string::npos && typesEnd > typesPos) {
                std::string newTypeDefs;
                for (std::set<std::string>::iterator it = neededTypes.begin(); it != neededTypes.end(); ++it) {
                    const std::string& typeName = *it;
                    // Check if this type already exists in the JSON
                    std::string searchStr = "\"name\": \"" + typeName + "\"";
                    // Search only in the types section (after typesPos)
                    size_t found = json.find(searchStr, typesPos);
                    if (found != std::string::npos && found < typesEnd + newTypeDefs.size()) {
                        continue; // already exists
                    }

                    // Find the type def in our parsed data
                    int tdIdx = -1;
                    for (int ti = 0; ti < (int)m_gameObjTypes.size(); ++ti) {
                        if (m_gameObjTypes[ti].name == typeName) { tdIdx = ti; break; }
                    }
                    if (tdIdx < 0) continue;

                    const LevelGameObjTypeDef& td = m_gameObjTypes[tdIdx];

                    // Build the type definition JSON
                    newTypeDefs += ",\n    {\n";
                    newTypeDefs += "      \"name\": \"" + typeName + "\",\n";
                    newTypeDefs += "      \"fields\": [\n";
                    for (size_t fi = 0; fi < td.fields.size(); ++fi) {
                        const GOFieldDefEntry& fd = td.fields[fi];
                        std::string fieldName = ResolveCrc(fd.name_crc);
                        if (fieldName.empty()) {
                            char fb[16]; sprintf(fb, "0x%08X", fd.name_crc);
                            fieldName = fb;
                        }
                        FieldKind fk = FieldKindFromCrc(fd.kind_crc);
                        const char* kindStr = FieldKindToJsonType(fk);

                        if (fi > 0) newTypeDefs += ",\n";
                        char offBuf[16]; sprintf(offBuf, "%u", fd.offset);
                        newTypeDefs += "        {\"name\": \"" + fieldName + "\", \"type\": \"" + kindStr + "\", \"offset\": " + offBuf + "}";
                    }
                    newTypeDefs += "\n      ]\n";
                    newTypeDefs += "    }";

                    LvlLog("ExportPendingToJson: injected type def '%s' (%d fields)\n",
                           typeName.c_str(), (int)td.fields.size());
                }

                if (!newTypeDefs.empty()) {
                    // Insert before the closing "]" of types array
                    // Recalculate typesEnd since json may have changed from entity insertion
                    typesEnd = json.rfind(']');
                    if (typesEnd != std::string::npos) {
                        json.insert(typesEnd, newTypeDefs);
                    }
                }
            }
        }
    }

    // ── Step 5: Write back ──
    {
        FILE* f = fopen(levelJsonPath.c_str(), "wb");
        if (!f) { m_error = "Cannot write " + levelJsonPath; return false; }
        fwrite(json.c_str(), 1, json.size(), f);
        fclose(f);
    }

    // ── Step 6: Append new strings to pak_strings.json ──
    std::string stringsPath = dumpDir + "\\pak_strings.json";
    {
        // Collect new strings
        std::vector<std::string> newStrings;
        for (size_t i = 0; i < m_pendingObjs.size(); ++i) {
            const PendingGameObj& p = m_pendingObjs[i];
            if (!p.name_str.empty()) newStrings.push_back(p.name_str);
            if (!p.output_event.empty()) newStrings.push_back(p.output_event);
            if (!p.input_action.empty()) newStrings.push_back(p.input_action);
            for (std::map<uint32_t, std::string>::const_iterator sit = p.stringOverrides.begin();
                 sit != p.stringOverrides.end(); ++sit) {
                if (!sit->second.empty()) newStrings.push_back(sit->second);
            }
        }
        // 3dCrowd Editor: append any user-typed CrowdItem `key` strings
        // queued via QueueCustomString. Dedupe is per-session by the queue
        // itself; if a string happens to coincide with one of the pending-
        // entity strings above, both get written and pak_strings.json will
        // contain the duplicate — the engine tolerates it (CRC lookup
        // picks first match), but worth noting.
        for (size_t qi = 0; qi < m_queuedCustomStrings.size(); ++qi) {
            newStrings.push_back(m_queuedCustomStrings[qi]);
        }

        if (!newStrings.empty()) {
            // Read existing pak_strings.json
            FILE* f = fopen(stringsPath.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                std::vector<char> strData(sz);
                fread(&strData[0], 1, sz, f);
                fclose(f);

                std::string strJson(strData.begin(), strData.end());
                // Find the closing "]" of the string array
                size_t arrEnd = strJson.rfind(']');
                if (arrEnd != std::string::npos) {
                    std::string append;
                    for (size_t si = 0; si < newStrings.size(); ++si) {
                        append += ",\n  \"" + newStrings[si] + "\"";
                    }
                    strJson.insert(arrEnd, append);

                    f = fopen(stringsPath.c_str(), "wb");
                    if (f) {
                        fwrite(strJson.c_str(), 1, strJson.size(), f);
                        fclose(f);
                    }
                }
            }
        }
    }

    LvlLog("ExportPendingToJson: wrote %d entities to %s\n",
           (int)m_pendingObjs.size(), levelJsonPath.c_str());
    return true;
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
        // Register Output event/input strings if they're new
        if (!p.output_event.empty()) {
            uint32_t evCrc = LotrHashString(p.output_event.c_str());
            if (m_crcMap.find(evCrc) == m_crcMap.end()) {
                newStrings.push_back(p.output_event);
                newStringCrcs.push_back(evCrc);
                m_crcMap[evCrc] = p.output_event;
            }
        }
        if (!p.input_action.empty()) {
            uint32_t inCrc = LotrHashString(p.input_action.c_str());
            if (m_crcMap.find(inCrc) == m_crcMap.end()) {
                newStrings.push_back(p.input_action);
                newStringCrcs.push_back(inCrc);
                m_crcMap[inCrc] = p.input_action;
            }
        }
        // Register string overrides in CRC map
        for (std::map<uint32_t, std::string>::const_iterator sit = p.stringOverrides.begin();
             sit != p.stringOverrides.end(); ++sit) {
            uint32_t sCrc = LotrHashString(sit->second.c_str());
            if (m_crcMap.find(sCrc) == m_crcMap.end()) {
                newStrings.push_back(sit->second);
                newStringCrcs.push_back(sCrc);
                m_crcMap[sCrc] = sit->second;
            }
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
