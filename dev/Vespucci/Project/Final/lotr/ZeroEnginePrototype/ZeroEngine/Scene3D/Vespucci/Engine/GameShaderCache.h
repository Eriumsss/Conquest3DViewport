#pragma once

#include <d3d9.h>
#include <stdint.h>
#include <string>
#include <map>
#include <vector>

// Loads the game's compiled shader bytecodes from Shaders_PC_nvidia.bin
// and provides them to the renderer by name.
//
// Format (reverse-engineered):
//   Header (32 bytes): version(4), vsCount(4), psCount(4), totalDataSize(4), ...
//   Entry table (N x 16 bytes): CRC(4), bytecodeOffset(4), flags(4), pad(4)
//   Data section: concatenated compiled shader bytecodes (vs_3_0 / ps_3_0)
//
// Shader names are extracted from #line directives embedded in the bytecodes.

class GameShaderCache
{
public:
    GameShaderCache();
    ~GameShaderCache();

    // Load shader cache from .bin file
    bool load(const char* binPath, IDirect3DDevice9* device);

    // Get a compiled shader by name (e.g. "Mg_FP_Lit_AN_Vd_Ao_WPos_Shdw_VNorm_VtxAtm")
    IDirect3DVertexShader9* getVS(const char* name);
    IDirect3DPixelShader9*  getPS(const char* name);

    // Check if a shader exists
    bool hasShader(const char* name) const;

    // Stats
    int vsCount() const { return m_vsCount; }
    int psCount() const { return m_psCount; }
    int namedCount() const { return (int)m_vsByName.size() + (int)m_psByName.size(); }

    // Enumerate every PS / VS we successfully named — used by the
    // Render Debug UI to populate a shader-pick dropdown. Output is
    // alphabetical by name. Skips entries whose name extraction
    // failed (those have empty string keys in the underlying maps).
    void getPSNames(std::vector<std::string>& out) const;
    void getVSNames(std::vector<std::string>& out) const;

    void release();

private:
    struct ShaderEntry {
        uint32_t crc;
        uint32_t dataOffset;    // offset into data section
        uint32_t flags;
        uint32_t size;          // bytecode size in bytes
        std::string name;       // extracted from bytecodes
    };

    std::vector<uint8_t>    m_rawData;      // full .bin file contents
    std::vector<ShaderEntry> m_entries;

    // Compiled shader objects, keyed by name
    std::map<std::string, IDirect3DVertexShader9*> m_vsByName;
    std::map<std::string, IDirect3DPixelShader9*>  m_psByName;

    IDirect3DDevice9* m_device;
    int m_vsCount, m_psCount;
    bool m_loaded;

    std::string extractName(const uint8_t* bytecode, uint32_t size);
};
