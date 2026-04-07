// MgD3D.cpp
// Reconstructed from:
//   FUN_006a5620  (MgD3D_CreateDevice,    366 addr)
//   FUN_006a63e6  (MgD3D_SelectShaderBinary, 47 addr)
//   FUN_0045008d  (MgD3D_CompilePixelShader, ~120 addr)
//   FUN_0045013f  (MgGfx_InitShaders,     ~200 addr)
//   FUN_0068a645  (MgGfx_RegisterDepthSamplers, ~180 addr)

#include "MgD3D.h"
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Depth sampler name table (ordered by registration sequence in FUN_0068a645)
// ---------------------------------------------------------------------------
const char* k_MgDepthSamplerNames[MG_DEPTH_SAMPLER_COUNT] = {
    "MgFP_SimpleSampler",
    "MgFP_HDR_Filtered_Sampler",
    "MgFP_Downsample_4x4",
    "MgFP_DepthDownSample_NODEPTH",
    "MgFP_DepthDownSample_DF24_FETCH1",
    "MgFP_DepthDownSample_DF24_FETCH4",
    "MgFP_DepthDownSample_RAWZ",
    "MgFP_DepthDownSample_INTZ",
    "MgFP_DepthDownSample_R32F",
};

// ---------------------------------------------------------------------------
// GFx HLSL shader sources (game .rdata strings, embedded in binary)
// ---------------------------------------------------------------------------

// Solid-color pixel shader
static const char* k_GfxPS_SolidColor =
    "float4 color : register(c0);\n"
    "void main(out float4 ocolor : COLOR) { ocolor = color; }\n";

// Textured pixel shader with color transform (cxmul/cxadd)
static const char* k_GfxPS_TexCxform =
    "float4 cxmul : register(c2);\n"
    "float4 cxadd : register(c3);\n"
    "sampler tex0 : register(s0);\n"
    "void main(in float2 tc : TEXCOORD0, in float4 color : COLOR0,\n"
    "          out float4 ocolor : COLOR)\n"
    "{\n"
    "    ocolor = tex2D(tex0, tc) * cxmul + cxadd;\n"
    "    ocolor *= color;\n"
    "}\n";

// ---------------------------------------------------------------------------
// FUN_006a63e6 — MgD3D_SelectShaderBinary (47 addr-count)
//
// Reads GPU vendor ID from [0xd17b4c] and copies the shader binary filename
// to [0xe5f3a8].  The copy is done with repeated MOVSDs (4 bytes at a time).
//
// Game .rdata string addresses:
//   0x9fc618 = "Shaders_PC_nvidia.bin"
//   0x9fc630 = "Shaders_PC_ati.bin"
//   0x9fc644 = "Shaders_PC_generic.bin"
// ---------------------------------------------------------------------------

// Shader binary path (written by MgD3D_SelectShaderBinary, read elsewhere)
char g_shader_binary_path[32]; // mirrors [0xe5f3a8] (global in game .data)

// GPU vendor ID (written by D3D adapter query, read here)
uint32_t g_gpu_vendor_id = 0;   // mirrors [0xd17b4c]

void MgD3D_SelectShaderBinary(void)
{
    const char* src;
    switch (g_gpu_vendor_id) {
        case 0x10DE: src = "Shaders_PC_nvidia.bin";  break;
        case 0x1002: src = "Shaders_PC_ati.bin";     break;
        default:     src = "Shaders_PC_generic.bin"; break;
    }
    // MOVSD-based copy in game (copies 4 bytes at a time, up to 32 bytes)
    strncpy(g_shader_binary_path, src, sizeof(g_shader_binary_path) - 1);
    g_shader_binary_path[sizeof(g_shader_binary_path) - 1] = '\0';
}

// ---------------------------------------------------------------------------
// FUN_006a5620 — MgD3D_CreateDevice (366 addr-count)
//
// Game flow:
//   1. Direct3DCreate9(D3D_SDK_VERSION) → pD3D
//   2. MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) → hMon
//   3. For each adapter i (GetAdapterCount):
//        GetAdapterMonitor(i) → compare with hMon → select best adapter
//   4. GetAdapterIdentifier → vendor_id → [0xd17b4c]
//   5. CheckDeviceFormat / GetDeviceCaps
//   6. CreateDevice(...D3DDEVTYPE_HAL, hwnd, flags, &pp, &device) → [0xcd808c]
// Returns 1 on success, 0 on failure.
// ---------------------------------------------------------------------------

// Function pointers for D3D9 (set when game DLL is loaded)
typedef void* (__stdcall* PFN_Direct3DCreate9)(uint32_t sdk_version);
static PFN_Direct3DCreate9 g_pfn_Direct3DCreate9 = NULL; // IAT 0x00659b24

// MonitorFromWindow: IAT [0x009702f4]
typedef void* (__stdcall* PFN_MonitorFromWindow)(void* hwnd, uint32_t flags);
static PFN_MonitorFromWindow g_pfn_MonitorFromWindow = NULL;

// D3DADAPTER_IDENTIFIER9 VendorId byte offset:
//   char Driver[512] + char Description[512] + char DeviceName[32] + LARGE_INTEGER DriverVersion[8] = 1064
#define D3D_IDENTIFIER_VENDORID_OFF 1064
#define D3D_IDENTIFIER_BUFSIZE      1100  // sizeof(D3DADAPTER_IDENTIFIER9) rounded up

// Minimal D3DPRESENT_PARAMETERS layout (exactly sizeof = 0x44 = 68 bytes, matching game usage)
typedef struct {
    uint32_t BackBufferWidth;            // +0x00
    uint32_t BackBufferHeight;           // +0x04
    uint32_t BackBufferFormat;           // +0x08  D3DFORMAT (22 = D3DFMT_X8R8G8B8)
    uint32_t BackBufferCount;            // +0x0C
    uint32_t MultiSampleType;            // +0x10  D3DMULTISAMPLE_TYPE
    uint32_t MultiSampleQuality;         // +0x14
    uint32_t SwapEffect;                 // +0x18  D3DSWAPEFFECT (1 = DISCARD)
    void*    hDeviceWindow;              // +0x1C
    uint32_t Windowed;                   // +0x20
    uint32_t EnableAutoDepthStencil;     // +0x24
    uint32_t AutoDepthStencilFormat;     // +0x28  D3DFORMAT (75 = D3DFMT_D24S8)
    uint32_t Flags;                      // +0x2C
    uint32_t FullScreen_RefreshRateInHz; // +0x30
    uint32_t PresentationInterval;       // +0x34
} MgD3DPresentParams; // sizeof = 0x38 = 56 bytes; game stores at [ESI+0x1568c]

// IDirect3D9 vtable thunk typedefs (all stdcall, first arg = this)
typedef uint32_t (__stdcall* PFN_D3D9_GetAdapterCount)(void* self);
typedef long     (__stdcall* PFN_D3D9_GetAdapterIdentifier)(void* self, uint32_t adapter, uint32_t flags, void* pIdent);
typedef void*    (__stdcall* PFN_D3D9_GetAdapterMonitor)(void* self, uint32_t adapter);
typedef long     (__stdcall* PFN_D3D9_CreateDevice)(void* self, uint32_t adapter, uint32_t devtype,
                                                     void* hwnd, uint32_t flags,
                                                     MgD3DPresentParams* pp, void** ppDevice);
typedef uint32_t (__stdcall* PFN_IUnk_Release)(void* self);

// Exported device/D3D9 object pointers
void* g_pD3D9_d3d   = NULL;  // IDirect3D9*    — populated by MgD3D_CreateDevice
void* g_pDevice_d3d = NULL;  // IDirect3DDevice9* — populated by MgD3D_CreateDevice

int MgD3D_CreateDevice(void* hwnd, int width, int height, int fullscreen)
{
    if (!hwnd || !g_pfn_Direct3DCreate9) return 0;

    // --- 1. Direct3DCreate9(0x20) — IAT thunk at 0x00659b24 ---
    // Game: PUSH 0x20 / CALL 0x00659b24
    void* pD3D = g_pfn_Direct3DCreate9(0x20 /* D3D_SDK_VERSION = 32 */);
    if (!pD3D) return 0;

    void** vtbl = *(void***)pD3D;

    // --- 2. GetAdapterCount: vtable[0x10/4] ---
    PFN_D3D9_GetAdapterCount pfn_cnt =
        (PFN_D3D9_GetAdapterCount)vtbl[0x10 / 4];
    uint32_t adapterCount = pfn_cnt(pD3D);

    // --- 3. MonitorFromWindow(hwnd, 0): find window's monitor ---
    // Game: PUSH 0 / PUSH [ESI+0x38] / CALL [0x009702f4]
    void* hMonWin = NULL;
    if (g_pfn_MonitorFromWindow)
        hMonWin = g_pfn_MonitorFromWindow(hwnd, 0 /* MONITOR_DEFAULTTOPRIMARY */);

    // --- 4. Iterate adapters: GetAdapterMonitor vtable[0x3C/4] ---
    uint32_t bestAdapter = 0;
    if (hMonWin && adapterCount > 0) {
        PFN_D3D9_GetAdapterMonitor pfn_mon =
            (PFN_D3D9_GetAdapterMonitor)vtbl[0x3C / 4];
        for (uint32_t i = 0; i < adapterCount; ++i) {
            if (pfn_mon(pD3D, i) == hMonWin) { bestAdapter = i; break; }
        }
    }

    // --- 5. GetAdapterIdentifier vtable[0x14/4] → VendorId ---
    // D3DADAPTER_IDENTIFIER9: VendorId at byte offset 1064
    uint8_t ident[D3D_IDENTIFIER_BUFSIZE];
    memset(ident, 0, sizeof(ident));
    PFN_D3D9_GetAdapterIdentifier pfn_id =
        (PFN_D3D9_GetAdapterIdentifier)vtbl[0x14 / 4];
    pfn_id(pD3D, bestAdapter, 0, ident);
    g_gpu_vendor_id = *(uint32_t*)(ident + D3D_IDENTIFIER_VENDORID_OFF);

    // --- 6. Set up D3DPRESENT_PARAMETERS ---
    // Game: stores 0x1 at [ESI+0x15b10] (DeviceType=HAL), 0x44 at [ESI+0x15b18] (BehaviorFlags)
    MgD3DPresentParams pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth            = (uint32_t)width;
    pp.BackBufferHeight           = (uint32_t)height;
    pp.BackBufferFormat           = 22;  // D3DFMT_X8R8G8B8
    pp.BackBufferCount            = 1;
    pp.SwapEffect                 = 1;   // D3DSWAPEFFECT_DISCARD
    pp.hDeviceWindow              = hwnd;
    pp.Windowed                   = fullscreen ? 0 : 1;
    pp.EnableAutoDepthStencil     = 1;
    pp.AutoDepthStencilFormat     = 75;  // D3DFMT_D24S8
    pp.PresentationInterval       = 0;   // D3DPRESENT_INTERVAL_DEFAULT

    // --- 7. CreateDevice: vtable[0x40/4] ---
    // BehaviorFlags 0x44 = D3DCREATE_HARDWARE_VERTEXPROCESSING(0x40) | D3DCREATE_MULTITHREADED(0x04)
    PFN_D3D9_CreateDevice pfn_create =
        (PFN_D3D9_CreateDevice)vtbl[0x40 / 4];
    void* pDevice = NULL;
    long hr = pfn_create(pD3D, bestAdapter,
                         1,     /* D3DDEVTYPE_HAL */
                         hwnd,
                         0x44,  /* D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED */
                         &pp, &pDevice);
    if (hr != 0 || !pDevice) {
        // Release IDirect3D9: vtable[0x08/4] = Release
        ((PFN_IUnk_Release)vtbl[0x08 / 4])(pD3D);
        g_pD3D9_d3d = NULL;
        return 0;
    }

    g_pD3D9_d3d   = pD3D;
    g_pDevice_d3d = pDevice;

    // --- 8. Select shader binary for detected GPU ---
    // Game: after CreateDevice, calls 0x006a63e6 (MgD3D_SelectShaderBinary)
    MgD3D_SelectShaderBinary();

    return 1;
}

// ---------------------------------------------------------------------------
// FUN_0045008d — MgD3D_CompilePixelShader (~120 addr-count)
//
// ASM:
//   … build D3DXMACRO / include / flags args …
//   PUSH source / PUSH "ps_3_0" / ... / CALL [0x00544998]  (D3DXCompileShader)
//   if FAILED: return NULL
//   MOV EDX, [device] / MOV EDX,[EDX]   ; device vtable
//   PUSH shader_blob   ; ID3DXBuffer*
//   CALL [EDX + 0x1a8] ; IDirect3DDevice9::CreatePixelShader
// Returns: IDirect3DPixelShader9* (or NULL on error).
// ---------------------------------------------------------------------------

// D3DXCompileShader IAT (0x00544998)
typedef long (__stdcall* PFN_D3DXCompileShader)(
    const char* src, uint32_t src_len, const void* defines,
    void* include, const char* fn_name, const char* profile,
    uint32_t flags, void** out_shader, void** out_errors, void** out_constants);
static PFN_D3DXCompileShader g_pfn_D3DXCompileShader = NULL; // IAT

IDirect3DPixelShader9_opaque* MgD3D_CompilePixelShader(
    IDirect3DDevice9_opaque* device,
    const char*              hlsl_source,
    const char*              entry_point)
{
    if (!device || !hlsl_source || !g_pfn_D3DXCompileShader) return NULL;

    void* shader_blob   = NULL;
    void* error_blob    = NULL;
    void* const_table   = NULL;

    long hr = g_pfn_D3DXCompileShader(
        hlsl_source, (uint32_t)strlen(hlsl_source),
        NULL,         // no macros
        NULL,         // no includes
        entry_point ? entry_point : "main",
        "ps_3_0",
        0,            // flags
        &shader_blob, &error_blob, &const_table);

    if (hr != 0 || !shader_blob) return NULL;

    // IDirect3DDevice9::CreatePixelShader at vtable offset 0x1a8
    // (vtable[0x1a8 / 4] = CreatePixelShader)
    typedef long (__stdcall* PFN_CreatePS)(void* self, const void* func, void** out);
    void** vtbl = *(void***)device;
    PFN_CreatePS pfn_create = (PFN_CreatePS)vtbl[0x1a8 / 4];

    // Get bytecode from blob (blob[0] = QueryInterface-like; [1] = GetBufferPointer)
    typedef void* (__stdcall* PFN_BlobGetPtr)(void* blob);
    void** blob_vtbl = *(void***)shader_blob;
    PFN_BlobGetPtr pfn_get_ptr = (PFN_BlobGetPtr)blob_vtbl[3]; // GetBufferPointer
    const void* bytecode = pfn_get_ptr(shader_blob);

    IDirect3DPixelShader9_opaque* ps = NULL;
    pfn_create(device, bytecode, (void**)&ps);

    // Release compiled blob: blob vtable index 2 = Release
    typedef uint32_t (__stdcall* PFN_Release)(void*);
    ((PFN_Release)blob_vtbl[2])(shader_blob);

    return ps;
}

// ---------------------------------------------------------------------------
// MgD3D_CompileVertexShader — mirrors FUN_0045008d for vertex shaders
//
// FUN_0045013f uses device vtable + 0x158 for CreateVertexShader.
// This function follows the same pattern as MgD3D_CompilePixelShader but
// compiles against "vs_3_0" and calls CreateVertexShader at vtable[0x158/4].
// ---------------------------------------------------------------------------
IDirect3DVertexShader9_base_opaque* MgD3D_CompileVertexShader(
    IDirect3DDevice9_opaque* device,
    const char*              hlsl_source,
    const char*              entry_point)
{
    if (!device || !hlsl_source || !g_pfn_D3DXCompileShader) return NULL;

    void* shader_blob  = NULL;
    void* error_blob   = NULL;
    void* const_table  = NULL;

    long hr = g_pfn_D3DXCompileShader(
        hlsl_source, (uint32_t)strlen(hlsl_source),
        NULL, NULL,
        entry_point ? entry_point : "main",
        "vs_3_0",
        0,
        &shader_blob, &error_blob, &const_table);

    if (hr != 0 || !shader_blob) return NULL;

    // CreateVertexShader: device vtable + 0x158
    typedef long (__stdcall* PFN_CreateVS)(void* self, const void* func, void** out);
    void** vtbl = *(void***)device;
    PFN_CreateVS pfn_create = (PFN_CreateVS)vtbl[0x158 / 4];

    // GetBufferPointer: blob vtable index 3
    typedef void* (__stdcall* PFN_BlobGetPtr)(void* blob);
    void** blob_vtbl = *(void***)shader_blob;
    const void* bytecode = ((PFN_BlobGetPtr)blob_vtbl[3])(shader_blob);

    IDirect3DVertexShader9_base_opaque* vs = NULL;
    pfn_create(device, bytecode, (void**)&vs);

    // Release the compiled blob (vtable index 2 = Release)
    typedef uint32_t (__stdcall* PFN_Release)(void*);
    ((PFN_Release)blob_vtbl[2])(shader_blob);

    return vs;
}

// ---------------------------------------------------------------------------
// FUN_0045013f — MgGfx_InitShaders (~200 addr-count)
//
// Compiles and creates 5 GFx shader pairs.
// Slot stride: 0x0c (sizeof MgGfxShaderSlot).
// Loop: for i in 0..4: compile VS+PS, store in slots[i].
//
// CreateVertexShader: device vtable + 0x158
// CreatePixelShader:  device vtable + 0x1a8
// ---------------------------------------------------------------------------
// Vertex shader source for slot 0 (solid-color rendering, game .rdata 0xa37e0c region)
// FUN_0045013f first loop: iterates 5 VS slots, compiles any that are NULL.
// Only slot 0 carries a real VS (CreateVertexShader vtable+0x158).
// Slots 1-4 reuse the same passthrough VS or have no VS requirement.
static const char* k_GfxVS_SolidColor =
    "float4x4 pos : register(c0);\n"   // transform matrix in VS constants c0-c3
    "void main(in float4 ipos : POSITION,\n"
    "           in float4 icol : COLOR0,\n"
    "           out float4 opos : POSITION,\n"
    "           out float4 ocol : COLOR0)\n"
    "{\n"
    "    opos = mul(pos, ipos);\n"
    "    ocol = icol;\n"
    "}\n";

int MgGfx_InitShaders(IDirect3DDevice9_opaque* device,
                      struct MgGfxShaderSlot*  slots)
{
    if (!device || !slots) return 0;
    if (!g_pfn_D3DXCompileShader) return 0;

    // PS source table: slot 0 = solid color, slots 1-4 = texture+cxform
    static const char* k_ps_sources[5] = {
        k_GfxPS_SolidColor,
        k_GfxPS_TexCxform,
        k_GfxPS_TexCxform,
        k_GfxPS_TexCxform,
        k_GfxPS_TexCxform,
    };

    int ok = 1;

    // --- VS loop: FUN_0045013f lines 0045016f-0045019d ---
    // Iterates 5 slots; slot[i].vs == NULL → compile and CreateVertexShader
    for (int i = 0; i < 5; i++) {
        if (slots[i].vs == NULL) {
            // Only slot 0 has a real VS in the game (others use NULL / fixed-function fallback)
            if (i == 0) {
                slots[0].vs = (IDirect3DVertexShader9_opaque*)
                    MgD3D_CompileVertexShader(device, k_GfxVS_SolidColor, "main");
                if (!slots[0].vs) ok = 0;
            }
        }
    }

    // --- PS loop: FUN_0045013f lines 004501b5-004501d8 ---
    // Iterates 5 slots via FUN_0045008d (MgD3D_CompilePixelShader)
    for (int i = 0; i < 5; i++) {
        if (slots[i].ps == NULL) {
            slots[i].ps = MgD3D_CompilePixelShader(device, k_ps_sources[i], "main");
            if (!slots[i].ps) ok = 0;
        }
        slots[i].flags = 0;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// FUN_0068a645 — MgGfx_RegisterDepthSamplers (~180 addr-count)
//
// Registers 9 named depth sampler objects.
// Each call uses Mg::String::CopyConstruct (0x0067e6d8) to build the name
// then registers with the global device [0xcd808c].
//
// In standalone reconstruction: stores names in k_MgDepthSamplerNames only.
// ---------------------------------------------------------------------------
typedef void (__cdecl* PFN_StringCopyConstruct)(const void* src, void* out);
static PFN_StringCopyConstruct g_pfn_StringCopyConstruct_d3d = NULL; // 0x0067e6d8

void MgGfx_RegisterDepthSamplers(IDirect3DDevice9_opaque* device)
{
    // Game: for each name, calls Mg::String::CopyConstruct then links the
    // sampler object into a registry via internal engine functions.
    // In standalone reconstruction: no-op (names are accessible via the table).
    (void)device;
}
