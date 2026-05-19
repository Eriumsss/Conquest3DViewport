// GameShaderCache — loads compiled shader bytecodes from the game's .bin file
//
// The game ships with Shaders_PC_nvidia.bin / ati.bin / generic.bin containing
// all 2298 compiled VS/PS shaders (vs_3_0 / ps_3_0). This class parses the
// file, extracts shader names from embedded #line directives, and provides
// compiled IDirect3DVertexShader9 / IDirect3DPixelShader9 objects by name.
//
// This allows the Vespucci renderer to use the EXACT game shaders instead of
// hand-written HLSL approximations — pixel-perfect rendering.

#include "GameShaderCache.h"
#include <cstdio>
#include <cstring>

GameShaderCache::GameShaderCache()
    : m_device(NULL), m_vsCount(0), m_psCount(0), m_loaded(false) {}

GameShaderCache::~GameShaderCache() { release(); }

void GameShaderCache::release() {
    for (std::map<std::string, IDirect3DVertexShader9*>::iterator it = m_vsByName.begin(); it != m_vsByName.end(); ++it)
        if (it->second) it->second->Release();
    for (std::map<std::string, IDirect3DPixelShader9*>::iterator it = m_psByName.begin(); it != m_psByName.end(); ++it)
        if (it->second) it->second->Release();
    m_vsByName.clear();
    m_psByName.clear();
    m_entries.clear();
    m_rawData.clear();
    m_loaded = false;
}

std::string GameShaderCache::extractName(const uint8_t* bytecode, uint32_t size) {
    // Search for "Mg_" or "MgF" or "MgV" pattern in the bytecodes
    // (embedded in CTAB comment blocks or #line directives)
    for (uint32_t i = 0; i + 5 < size; ++i) {
        if (bytecode[i] == 'M' && bytecode[i+1] == 'g' &&
            (bytecode[i+2] == '_' || bytecode[i+2] == 'F' || bytecode[i+2] == 'V')) {
            // Extract until '.' or null or non-printable
            std::string name;
            for (uint32_t j = i; j < size && j < i + 200; ++j) {
                char c = (char)bytecode[j];
                if (c == 0 || c == '.' || c < 32 || c > 126) break;
                name += c;
            }
            if (name.size() > 5) return name;
        }
    }
    return "";
}

bool GameShaderCache::load(const char* binPath, IDirect3DDevice9* device) {
    if (m_loaded) release();
    m_device = device;

    // Read entire file
    FILE* f = fopen(binPath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    m_rawData.resize(fileSize);
    fread(&m_rawData[0], 1, fileSize, f);
    fclose(f);

    if (fileSize < 32) return false;

    // ── Parse header (matches Rust parser's Header struct exactly) ──
    // +0:  constx1 (version)
    // +4:  vertex_shader_num
    // +8:  fragment_shader_num
    // +12: strings_offset
    // +16: strings_size
    // +20: strings_num
    // +24: shaders_offset    ← WHERE SHADER BYTECODES START
    // +28: shaders_size
    uint32_t vsNum, psNum, stringsOffset, stringsSize, stringsNum, shadersOffset, shadersSize;
    memcpy(&vsNum,          &m_rawData[4], 4);
    memcpy(&psNum,          &m_rawData[8], 4);
    memcpy(&stringsOffset,  &m_rawData[12], 4);
    memcpy(&stringsSize,    &m_rawData[16], 4);
    memcpy(&stringsNum,     &m_rawData[20], 4);
    memcpy(&shadersOffset,  &m_rawData[24], 4);
    memcpy(&shadersSize,    &m_rawData[28], 4);

    uint32_t totalCount = vsNum + psNum;
    if (totalCount > 10000 || totalCount == 0) return false;
    if (shadersOffset + shadersSize > (uint32_t)fileSize) return false;

    // Entry table starts at offset 32 (right after header)
    // Each entry is 16 bytes: CRC(4) + offset(4) + flags(8)
    // VS entries first, then PS entries
    uint32_t entryStart = 32;
    uint32_t entryTableSize = totalCount * 16;
    if (entryStart + entryTableSize > (uint32_t)fileSize) return false;

    // Parse entry table (VS + PS sequential)
    m_entries.resize(totalCount);
    for (uint32_t i = 0; i < totalCount; ++i) {
        uint32_t off = entryStart + i * 16;
        memcpy(&m_entries[i].crc,        &m_rawData[off + 0], 4);
        memcpy(&m_entries[i].dataOffset, &m_rawData[off + 4], 4);
        // flags is u64 (8 bytes), but we only store u32
        memcpy(&m_entries[i].flags,      &m_rawData[off + 8], 4);
    }

    // Compute shader sizes using sorted offsets
    // VS entries [0..vsNum-1], PS entries [vsNum..totalCount-1]
    // VS offsets are relative to shadersOffset, PS offsets also relative to shadersOffset
    // VS data comes before PS data in the file (Rust: vertex_offsets end = fragment_offsets[0])

    // Find the boundary between VS and PS data
    // VS shaders: offset relative to shadersOffset, size = next VS offset - this offset
    // Last VS size = first PS offset - last VS offset
    // PS shaders: similar, last PS size = shadersSize - last PS offset

    // Sort VS offsets to compute sizes
    for (uint32_t i = 0; i < vsNum && i < totalCount; ++i) {
        uint32_t myOff = m_entries[i].dataOffset;
        uint32_t nextOff = shadersSize; // default: end of all shader data
        // Find next VS entry with higher offset
        for (uint32_t j = 0; j < vsNum; ++j) {
            if (m_entries[j].dataOffset > myOff && m_entries[j].dataOffset < nextOff)
                nextOff = m_entries[j].dataOffset;
        }
        // Also check first PS entry as upper bound for last VS
        if (psNum > 0 && m_entries[vsNum].dataOffset > myOff && m_entries[vsNum].dataOffset < nextOff)
            nextOff = m_entries[vsNum].dataOffset;
        m_entries[i].size = nextOff - myOff;
    }
    // PS sizes
    for (uint32_t i = vsNum; i < totalCount; ++i) {
        uint32_t myOff = m_entries[i].dataOffset;
        uint32_t nextOff = shadersSize; // default: end of all shader data
        for (uint32_t j = vsNum; j < totalCount; ++j) {
            if (m_entries[j].dataOffset > myOff && m_entries[j].dataOffset < nextOff)
                nextOff = m_entries[j].dataOffset;
        }
        m_entries[i].size = nextOff - myOff;
    }

    // Extract names and compile shaders
    // ALL offsets are relative to shadersOffset (NOT to entry table end!)
    m_vsCount = 0;
    m_psCount = 0;
    int named = 0;
    int createFail = 0;

    for (uint32_t i = 0; i < totalCount; ++i) {
        ShaderEntry& e = m_entries[i];
        uint32_t absOff = shadersOffset + e.dataOffset;
        if (absOff + 4 > (uint32_t)fileSize) continue;

        const uint8_t* bytecode = &m_rawData[absOff];
        uint32_t magic;
        memcpy(&magic, bytecode, 4);

        // Extract name from bytecodes (#line directives or CTAB)
        e.name = extractName(bytecode, e.size);
        if (!e.name.empty()) named++;

        // Create D3D9 shader objects from compiled bytecodes
        const DWORD* pFunction = (const DWORD*)bytecode;
        bool isVS = (i < vsNum);  // first vsNum entries are VS, rest are PS

        if (isVS && magic == 0xFFFE0300 && !e.name.empty()) {
            IDirect3DVertexShader9* vs = NULL;
            HRESULT hr = device->CreateVertexShader(pFunction, &vs);
            if (SUCCEEDED(hr) && vs) {
                m_vsByName[e.name] = vs;
                m_vsCount++;
            } else { createFail++; }
        } else if (!isVS && magic == 0xFFFF0300 && !e.name.empty()) {
            IDirect3DPixelShader9* ps = NULL;
            HRESULT hr = device->CreatePixelShader(pFunction, &ps);
            if (SUCCEEDED(hr) && ps) {
                m_psByName[e.name] = ps;
                m_psCount++;
            } else { createFail++; }
        } else if (!e.name.empty()) {
            // Magic mismatch — wrong shader type or corrupted data
            createFail++;
        }
    }

    m_loaded = true;

    // Log diagnostic: if createFail > 0, something is wrong with offsets
    if (createFail > 0) {
        // Can't use LevelLog from here, but the caller logs the counts
    }

    return true;
}

IDirect3DVertexShader9* GameShaderCache::getVS(const char* name) {
    std::map<std::string, IDirect3DVertexShader9*>::iterator it = m_vsByName.find(name);
    return (it != m_vsByName.end()) ? it->second : NULL;
}

IDirect3DPixelShader9* GameShaderCache::getPS(const char* name) {
    std::map<std::string, IDirect3DPixelShader9*>::iterator it = m_psByName.find(name);
    return (it != m_psByName.end()) ? it->second : NULL;
}

bool GameShaderCache::hasShader(const char* name) const {
    return m_vsByName.count(name) > 0 || m_psByName.count(name) > 0;
}

void GameShaderCache::getPSNames(std::vector<std::string>& out) const {
    out.clear();
    out.reserve(m_psByName.size());
    for (std::map<std::string, IDirect3DPixelShader9*>::const_iterator it = m_psByName.begin();
         it != m_psByName.end(); ++it) {
        if (!it->first.empty()) out.push_back(it->first);
    }
}

void GameShaderCache::getVSNames(std::vector<std::string>& out) const {
    out.clear();
    out.reserve(m_vsByName.size());
    for (std::map<std::string, IDirect3DVertexShader9*>::const_iterator it = m_vsByName.begin();
         it != m_vsByName.end(); ++it) {
        if (!it->first.empty()) out.push_back(it->first);
    }
}
