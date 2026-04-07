// MgTexture.h
// Reconstructed texture subsystem.
// Sources: analyzed/Texture/
//
// Functions:
//   FUN_00747153  — LoadTextureFromFile  (D3DXCreateTextureFromFileExA)
//   FUN_00748455  — UploadMipTexture     (D3DXLoadSurfaceFromMemory)
//   FUN_0045ce80  — "Texture"       property reader  (PblCRC-keyed)
//   FUN_004bf690  — "NormalTexture" property reader  (PblCRC-keyed)
//   FUN_00ea4550  — "RefractionTexture" property reader (PblCRC-keyed)
//   FUN_0070f674  — "CloudNoiseTexture" property reader
//   FUN_0089f289  — Texture presence flag (returns 0, 19 addr)
//
// D3D IAT slots:
//   0x005449a4  D3DXCreateTextureFromFileExA
//   0x0054499e  D3DXLoadSurfaceFromMemory
//
// Texture format codes (from FUN_00748455 switch):
//   1  = RGBA8 (raw 32-bit RGBA)
//   2  = RGB special
//   9  = from device caps [EAX+0xdc]
//   10 = 0x31545844  (DXT1 / "1TXD")
//   11 = 0x33545844  (DXT3 / "3TXD")
//   12 = 0x35545844  (DXT5 / "5TXD")

#pragma once
#include <stdint.h>
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// D3D texture format FourCC constants (from FUN_00748455)
// ---------------------------------------------------------------------------
#define MG_D3DFMT_DXT1  0x31545844u  // 'DXT1'
#define MG_D3DFMT_DXT3  0x33545844u  // 'DXT3'
#define MG_D3DFMT_DXT5  0x35545844u  // 'DXT5'

// Internal format type codes (FUN_00748455 switch cases)
#define MG_TEXFMT_RGBA8       1
#define MG_TEXFMT_RGB_SPECIAL 2
#define MG_TEXFMT_DEVICE_CAPS 9
#define MG_TEXFMT_DXT1        10
#define MG_TEXFMT_DXT3        11
#define MG_TEXFMT_DXT5        12

// ---------------------------------------------------------------------------
// MgTextureObject — partial layout recovered from FUN_00747153.
//
//   +0x08 : vtable slot used for Release (IDirect3DTexture9::Release at vtable+0x8)
//   +0x2c : width  (uint32_t, set after CreateTextureFromFile)
//   +0x30 : height (uint32_t)
//   Other fields: opaque (engine-managed D3D texture wrapper)
// ---------------------------------------------------------------------------
struct MgTextureObject {
    void*    vtable;       // +0x00
    void*    _unk_04;      // +0x04
    void*    d3d_tex;      // +0x08 IDirect3DTexture9* (Release via vtable+0x8)
    uint8_t  _pad_0c[0x20];
    uint32_t width;        // +0x2c
    uint32_t height;       // +0x30
};

// ---------------------------------------------------------------------------
// FUN_00747153 — MgTexture_LoadFromFile (~120 addr-count)
//
// Wraps D3DXCreateTextureFromFileExA with a 12-argument call.
// Releases old D3D texture via Release() at vtable+0x8 before loading.
// Stores width/height at obj[+0x2c] / obj[+0x30].
//
// D3DXCreateTextureFromFileExA args (12):
//   device, filename, width, height, mip_levels, usage,
//   format, pool, filter, mip_filter, color_key, &src_info, &palette, &texture
//
// Width/height come from texture desc (src_info.Width/Height).
// ---------------------------------------------------------------------------
int MgTexture_LoadFromFile(struct MgTextureObject* obj,
                           void*                   d3d_device,
                           const char*             filename);

// ---------------------------------------------------------------------------
// FUN_00748455 — MgTexture_UploadMips (~300 addr-count)
//
// Uploads pre-decoded mip-map pixel data into a locked D3D surface.
// Uses D3DXLoadSurfaceFromMemory for compressed (DXT) and uncompressed paths.
//
// LockRect: IDirect3DSurface9::LockRect at vtable+0x48.
//
// Format type dispatch:
//   type 1    → raw RGBA8 copy
//   type 2    → special RGB path
//   type 9    → read from device caps at [device+0xdc]
//   type 10   → D3DFMT_DXT1
//   type 11   → D3DFMT_DXT3
//   type 12   → D3DFMT_DXT5
// ---------------------------------------------------------------------------
int MgTexture_UploadMips(void*          d3d_surface,  // IDirect3DSurface9*
                         const void*    pixel_data,
                         uint32_t       width,
                         uint32_t       height,
                         int            format_type,
                         void*          d3d_device);

// ---------------------------------------------------------------------------
// FUN_0045ce80 — MgTextureProp_GetTexture (~114 addr-count)
//
// Reads a "PblCRC"/"Texture" property from template data.
// Same lookup pattern as shadow props: calls 0x007e866b.
// Default: NULL (no texture).
// String keys: 0x9cXXXX = "PblCRC", 0x9dXXXX = "Texture"
// ---------------------------------------------------------------------------
void* MgTextureProp_GetTexture(const MgAnimBlendEntry* entry);

// ---------------------------------------------------------------------------
// FUN_004bf690 — MgTextureProp_GetNormalTexture (~114 addr-count)
//
// Same pattern; field key = "NormalTexture".
// ---------------------------------------------------------------------------
void* MgTextureProp_GetNormalTexture(const MgAnimBlendEntry* entry);

// ---------------------------------------------------------------------------
// FUN_00ea4550 — MgTextureProp_GetRefractionTexture (~114 addr-count)
//
// Same pattern; field key = "RefractionTexture".
// ---------------------------------------------------------------------------
void* MgTextureProp_GetRefractionTexture(const MgAnimBlendEntry* entry);

// ---------------------------------------------------------------------------
// FUN_0070f674 — MgTextureProp_GetCloudNoiseTexture (62 addr-count)
//
// Same lookup pattern for "CloudNoiseTexture".
// Default: NULL.
// ---------------------------------------------------------------------------
void* MgTextureProp_GetCloudNoiseTexture(const MgAnimBlendEntry* entry);

// ---------------------------------------------------------------------------
// FUN_0089f289 — MgTexture_IsNull (19 addr-count)
//
// ASM: XOR EAX,EAX / ... (always returns 0)
// Appears to be a stub or a property flag reader that always returns 0
// (texture slot present = false, or count = 0).
// ---------------------------------------------------------------------------
int MgTexture_IsNull(void);

#ifdef __cplusplus
} // extern "C"
#endif
