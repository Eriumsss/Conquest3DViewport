// MgTexture.cpp
// Reconstructed from:
//   FUN_00747153  (LoadTextureFromFile,    ~120 addr)
//   FUN_00748455  (UploadMips,             ~300 addr)
//   FUN_0045ce80  (GetTexture,             114 addr)
//   FUN_004bf690  (GetNormalTexture,       114 addr)
//   FUN_00ea4550  (GetRefractionTexture,   114 addr)
//   FUN_0070f674  (GetCloudNoiseTexture,    62 addr)
//   FUN_0089f289  (IsNull,                  19 addr)

#include "MgTexture.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// D3DX function pointers (IAT)
// ---------------------------------------------------------------------------
typedef long (__stdcall* PFN_D3DXCreateTextureFromFileExA)(
    void* device, const char* filename,
    uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t usage,
    uint32_t format, uint32_t pool, uint32_t filter, uint32_t mip_filter,
    uint32_t color_key, void* src_info, void* palette, void** out_texture);
static PFN_D3DXCreateTextureFromFileExA g_pfn_D3DXCreateTextureFromFileExA = NULL; // 0x005449a4

typedef long (__stdcall* PFN_D3DXLoadSurfaceFromMemory)(
    void* dst_surface, void* dst_palette, const void* dst_rect,
    const void* src_memory, uint32_t src_format, uint32_t src_pitch,
    const void* src_palette, const void* src_rect, uint32_t filter, uint32_t color_key);
static PFN_D3DXLoadSurfaceFromMemory g_pfn_D3DXLoadSurfaceFromMemory = NULL; // 0x0054499e

// Game find_typed_param (0x007e866b) — shared with MgEngine
typedef int (__cdecl* PFN_FindTypedParam)(void* key1, void* data, void* key2);
static PFN_FindTypedParam g_pfn_FindTypedParam = NULL;

// ---------------------------------------------------------------------------
// FUN_00747153 — MgTexture_LoadFromFile
//
// ASM:
//   ; Release old texture if present: vtable dispatch [obj+0x8] → Release
//   MOV ECX, [ESI + 0x8]  ; obj->d3d_tex
//   TEST ECX, ECX / JZ skip_release
//   MOV EAX, [ECX]        ; vtable
//   PUSH 0                 ; param
//   CALL [EAX + 0x8]       ; IDirect3DTexture9::Release()
//   AND [ESI + 0x8], 0     ; obj->d3d_tex = NULL
//
//   ; Load: D3DXCreateTextureFromFileExA(device, filename, D3DX_DEFAULT, ...)
//   PUSH ... (12 args total)
//   CALL [0x005449a4]   ; D3DXCreateTextureFromFileExA
//
//   ; Store dimensions from D3DXIMAGE_INFO (src_info.Width/Height)
//   MOV [ESI + 0x2c], width
//   MOV [ESI + 0x30], height
// Returns 1 on success, 0 on failure.
// ---------------------------------------------------------------------------
int MgTexture_LoadFromFile(struct MgTextureObject* obj,
                           void*                   d3d_device,
                           const char*             filename)
{
    if (!obj || !d3d_device || !filename) return 0;

    // Release old texture via vtable[0x8/4] = Release
    if (obj->d3d_tex) {
        void** vtbl = *(void***)obj->d3d_tex;
        typedef long (__stdcall* PFN_Release)(void* self);
        PFN_Release pfn = (PFN_Release)vtbl[0x8 / 4];
        pfn(obj->d3d_tex);
        obj->d3d_tex = NULL;
    }

    if (!g_pfn_D3DXCreateTextureFromFileExA) return 0;

    // D3DXIMAGE_INFO buffer on stack (for width/height readback)
    uint8_t src_info[0x24];
    memset(src_info, 0, sizeof(src_info));

    // D3DX_DEFAULT = 0xffffffff
    void* new_tex = NULL;
    long hr = g_pfn_D3DXCreateTextureFromFileExA(
        d3d_device, filename,
        0xffffffff,     // width  = D3DX_DEFAULT
        0xffffffff,     // height = D3DX_DEFAULT
        0xffffffff,     // mip_levels = D3DX_DEFAULT
        0,              // usage = 0
        0,              // format = D3DFMT_UNKNOWN
        1,              // pool  = D3DPOOL_MANAGED
        0xffffffff,     // filter = D3DX_DEFAULT
        0xffffffff,     // mip_filter = D3DX_DEFAULT
        0,              // color_key = 0
        src_info,       // src_info out
        NULL,           // no palette
        &new_tex);

    if (hr != 0 || !new_tex) return 0;

    obj->d3d_tex = new_tex;

    // Width/Height at offset 0x00 and 0x04 inside D3DXIMAGE_INFO
    obj->width  = *(uint32_t*)(src_info + 0x00);
    obj->height = *(uint32_t*)(src_info + 0x04);

    return 1;
}

// ---------------------------------------------------------------------------
// FUN_00748455 — MgTexture_UploadMips
//
// format_type → D3D format FourCC:
//   1  → RGBA8 (D3DFMT_A8R8G8B8 = 21)
//   2  → game-specific RGB path (D3DFMT_R8G8B8? not fully confirmed)
//   9  → read from device caps [device+0xdc]
//   10 → D3DFMT_DXT1 (0x31545844)
//   11 → D3DFMT_DXT3 (0x33545844)
//   12 → D3DFMT_DXT5 (0x35545844)
//
// Steps:
//   LockRect(d3d_surface, &locked, NULL, 0)   ; vtable+0x48
//   D3DXLoadSurfaceFromMemory(dst, NULL, NULL, src, d3d_fmt, pitch, NULL, &src_rect, D3DX_DEFAULT, 0)
//   UnlockRect(d3d_surface)                   ; vtable+0x4c
// ---------------------------------------------------------------------------

// D3D format constants
#define D3DFMT_R8G8B8   20
#define D3DFMT_A8R8G8B8 21

static uint32_t resolve_d3d_format(int format_type, void* d3d_device)
{
    switch (format_type) {
        case MG_TEXFMT_RGBA8:       return D3DFMT_A8R8G8B8;
        case MG_TEXFMT_RGB_SPECIAL: return D3DFMT_R8G8B8;
        case MG_TEXFMT_DEVICE_CAPS:
            if (d3d_device)
                return *(uint32_t*)((uint8_t*)d3d_device + 0xdc);
            return D3DFMT_A8R8G8B8;
        case MG_TEXFMT_DXT1:        return MG_D3DFMT_DXT1;
        case MG_TEXFMT_DXT3:        return MG_D3DFMT_DXT3;
        case MG_TEXFMT_DXT5:        return MG_D3DFMT_DXT5;
        default:                    return D3DFMT_A8R8G8B8;
    }
}

int MgTexture_UploadMips(void*       d3d_surface,
                         const void* pixel_data,
                         uint32_t    width,
                         uint32_t    height,
                         int         format_type,
                         void*       d3d_device)
{
    if (!d3d_surface || !pixel_data) return 0;
    if (!g_pfn_D3DXLoadSurfaceFromMemory) return 0;

    uint32_t d3d_fmt = resolve_d3d_format(format_type, d3d_device);

    // Compute source pitch (bytes per row)
    uint32_t pitch;
    if (format_type == MG_TEXFMT_DXT1)
        pitch = ((width + 3) / 4) * 8;   // DXT1: 8 bytes per 4×4 block
    else if (format_type == MG_TEXFMT_DXT3 || format_type == MG_TEXFMT_DXT5)
        pitch = ((width + 3) / 4) * 16;  // DXT3/5: 16 bytes per 4×4 block
    else
        pitch = width * 4;               // RGBA8 default

    // Source rect (full surface)
    struct { long left, top, right, bottom; } src_rect = { 0, 0, (long)width, (long)height };

    long hr = g_pfn_D3DXLoadSurfaceFromMemory(
        d3d_surface,  // dest IDirect3DSurface9*
        NULL,         // dest palette
        NULL,         // dest rect = full
        pixel_data,
        d3d_fmt,
        pitch,
        NULL,         // src palette
        &src_rect,
        0xffffffff,   // filter = D3DX_DEFAULT
        0);           // color key

    return (hr == 0) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Property readers — common pattern mirrors shadow prop readers.
// All use 0x007e866b (find_typed_param) with type key "PblCRC".
// Default: NULL (no texture assigned).
// ---------------------------------------------------------------------------

static void* texture_prop_lookup(const MgAnimBlendEntry* entry, const char* field_key)
{
    if (!entry || !entry->blend_data) return NULL;
    if (!g_pfn_FindTypedParam)        return NULL;

    static const char* k_type = "PblCRC"; // 0x9cXXXX in game .rdata

    int offset = g_pfn_FindTypedParam((void*)k_type, entry->blend_data, (void*)field_key);
    if (offset == -1) return NULL;

    return *(void**)((const uint8_t*)entry + offset + 0x10);
}

// FUN_0045ce80 — "Texture"
void* MgTextureProp_GetTexture(const MgAnimBlendEntry* entry)
{
    return texture_prop_lookup(entry, "Texture");
}

// FUN_004bf690 — "NormalTexture"
void* MgTextureProp_GetNormalTexture(const MgAnimBlendEntry* entry)
{
    return texture_prop_lookup(entry, "NormalTexture");
}

// FUN_00ea4550 — "RefractionTexture"
void* MgTextureProp_GetRefractionTexture(const MgAnimBlendEntry* entry)
{
    return texture_prop_lookup(entry, "RefractionTexture");
}

// FUN_0070f674 — "CloudNoiseTexture"
void* MgTextureProp_GetCloudNoiseTexture(const MgAnimBlendEntry* entry)
{
    return texture_prop_lookup(entry, "CloudNoiseTexture");
}

// ---------------------------------------------------------------------------
// FUN_0089f289 — MgTexture_IsNull (19 addr-count)
//
// ASM: XOR EAX,EAX / ... (always returns 0)
// Confirmed stub: function body is effectively "return 0".
// ---------------------------------------------------------------------------
int MgTexture_IsNull(void)
{
    return 0;
}
