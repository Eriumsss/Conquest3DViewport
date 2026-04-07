// MgD3D.h
// Reconstructed Direct3D 9 subsystem wrappers.
// Sources: analyzed/D3D/
//
// Functions:
//   FUN_006a5620  — D3D device creation (calls Direct3DCreate9)
//   FUN_006a63e6  — GPU vendor detection → shader binary path
//   FUN_0045008d  — compile a PS 3.0 pixel shader from source
//   FUN_0045013f  — compile and register 5 GFx HLSL shaders (VS+PS)
//   FUN_0068a645  — register 9 post-process depth sampler names
//
// IAT thunk addresses (6-byte JMP stubs, no body):
//   0x00659b24  Direct3DCreate9
//   0x00659b2a  D3DPERF_BeginEvent
//   0x00659b36  D3DPERF_SetMarker
//   0x00544998  D3DXCompileShader
//   0x005449a4  D3DXCreateTextureFromFileExA
//   0x0054499e  D3DXLoadSurfaceFromMemory
//
// Global D3D device pointer: [0x00cd808c]
// Shader binary path global: [0x00e5f3a8] (filled by MgD3D_SelectShaderBinary)
// GPU vendor ID global:      [0x00d17b4c]

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// FUN_006a63e6 — MgD3D_SelectShaderBinary
//
// Reads [0xd17b4c] (GPU vendor ID set earlier by D3D adapter query).
// Copies the matching shader binary filename to [0xe5f3a8].
//
// Vendor map:
//   0x10DE  → "Shaders_PC_nvidia.bin"   (0x9fc618)
//   0x1002  → "Shaders_PC_ati.bin"      (0x9fc630)
//   other   → "Shaders_PC_generic.bin"  (0x9fc644)
//
// The copy uses a 4-char MOVSD loop (string ≤16 chars including NUL).
// ---------------------------------------------------------------------------
void MgD3D_SelectShaderBinary(void);

// ---------------------------------------------------------------------------
// FUN_006a5620 — MgD3D_CreateDevice (366 addr-count)
//
// Creates the IDirect3D9 object and queries available adapters.
// Key operations:
//   1. Direct3DCreate9(D3D_SDK_VERSION)
//   2. MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
//   3. Iterates adapters to find best match to target monitor
//   4. Stores IDirect3DDevice9* at [0xcd808c]
//   5. Reads adapter VendorId → stores at [0xd17b4c]
// Returns: 1 on success, 0 on failure.
// ---------------------------------------------------------------------------
typedef void IDirect3D9_opaque;
typedef void IDirect3DDevice9_opaque;

int MgD3D_CreateDevice(void* hwnd, int width, int height, int fullscreen);

// ---------------------------------------------------------------------------
// FUN_0045008d — MgD3D_CompilePixelShader (compiled from inline HLSL source)
//
// Compiles a ps_3_0 pixel shader from a source string.
// ASM:
//   PUSH source_str / PUSH "ps_3_0" (0x54c998) / PUSH ... flags ...
//   CALL 0x00544998  (D3DXCompileShader IAT)
//   ; On success:
//   MOV  EDX, [EDX]             ; device vtable
//   CALL [EDX + 0x1a8]          ; IDirect3DDevice9::CreatePixelShader
// Returns: IDirect3DPixelShader9* or NULL on error.
// ---------------------------------------------------------------------------
typedef void IDirect3DPixelShader9_opaque;

IDirect3DPixelShader9_opaque* MgD3D_CompilePixelShader(
    IDirect3DDevice9_opaque* device,
    const char*              hlsl_source,
    const char*              entry_point);

// ---------------------------------------------------------------------------
// MgD3D_CompileVertexShader — mirrors FUN_0045008d for vertex shaders
//
// Same flow as CompilePixelShader but uses:
//   profile  = "vs_3_0"
//   CreateVertexShader: device vtable + 0x158
// ---------------------------------------------------------------------------
typedef void IDirect3DVertexShader9_base_opaque;

IDirect3DVertexShader9_base_opaque* MgD3D_CompileVertexShader(
    IDirect3DDevice9_opaque* device,
    const char*              hlsl_source,
    const char*              entry_point);

// ---------------------------------------------------------------------------
// MgD3D device/D3D9 object pointers populated by MgD3D_CreateDevice
// ---------------------------------------------------------------------------
extern void* g_pD3D9_d3d;    // IDirect3D9* — set on successful device creation
extern void* g_pDevice_d3d;  // IDirect3DDevice9* — set on successful device creation

// ---------------------------------------------------------------------------
// FUN_0045013f — MgGfx_InitShaders (5 inline GFx HLSL shaders)
//
// Compiles and registers 5 shaders used by Scaleform GFx rendering.
// Shader slot stride: 0x0c bytes each; 5 slots covering 0x00..0x38.
//
// Embedded HLSL sources (game .rdata):
//   Slot 0: Solid-color VS/PS
//     VS: "float4 pos : POSITION; float4 color : COLOR0;\n
//          void main(in float4 ipos : POSITION, out float4 opos : POSITION,
//                    out float4 ocol : COLOR0)\n
//          { opos = mul(pos, ipos); ocol = color; }\n"
//     PS: "float4 color : register(c0);\n
//          void main(out float4 ocolor : COLOR) { ocolor = color; }\n"
//   Slot 1..4: Texture-sampling with cxmul/cxadd color transforms
//     "float4 cxmul : register(c2);\nfloat4 cxadd : register(c3);\n
//      sampler tex0 : register(s0);\n..."
//
// CreateVertexShader: device vtable + 0x158
// CreatePixelShader:  device vtable + 0x1a8
// ---------------------------------------------------------------------------
typedef void IDirect3DVertexShader9_opaque;

struct MgGfxShaderSlot {
    IDirect3DVertexShader9_opaque* vs;   // +0x00
    IDirect3DPixelShader9_opaque*  ps;   // +0x04
    uint32_t                       flags;// +0x08
};

// Initialises the 5 GFx shader slots in-place.
// slots must point to an array of at least 5 MgGfxShaderSlot structs (stride 0x0c = sizeof).
// Returns: 1 on full success, 0 if any shader fails to compile.
int MgGfx_InitShaders(IDirect3DDevice9_opaque* device,
                      struct MgGfxShaderSlot*  slots);

// ---------------------------------------------------------------------------
// FUN_0068a645 — MgGfx_RegisterDepthSamplers (9 post-process sampler names)
//
// Registers 9 named depth/HDR sampler objects via Mg::String::CopyConstruct.
// Each name is written to a static MgString and linked into the global
// sampler registry using the D3D device at [0xcd808c].
//
// Sampler names (in order):
//   0  "MgFP_SimpleSampler"
//   1  "MgFP_HDR_Filtered_Sampler"
//   2  "MgFP_Downsample_4x4"
//   3  "MgFP_DepthDownSample_NODEPTH"
//   4  "MgFP_DepthDownSample_DF24_FETCH1"
//   5  "MgFP_DepthDownSample_DF24_FETCH4"
//   6  "MgFP_DepthDownSample_RAWZ"
//   7  "MgFP_DepthDownSample_INTZ"
//   8  "MgFP_DepthDownSample_R32F"
//
// All 9 registration calls use 0x0067e6d8 (Mg::String::CopyConstruct).
// ---------------------------------------------------------------------------
#define MG_DEPTH_SAMPLER_COUNT 9

extern const char* k_MgDepthSamplerNames[MG_DEPTH_SAMPLER_COUNT];

void MgGfx_RegisterDepthSamplers(IDirect3DDevice9_opaque* device);

#ifdef __cplusplus
} // extern "C"
#endif
