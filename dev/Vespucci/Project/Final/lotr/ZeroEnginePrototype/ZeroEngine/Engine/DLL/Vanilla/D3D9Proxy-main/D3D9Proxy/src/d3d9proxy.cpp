#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

#include "d3d9device.h"

// Real d3d9.dll handle and function
static HMODULE g_RealD3D9 = nullptr;
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t g_RealDirect3DCreate9 = nullptr;

// Our wrapper IDirect3D9 class
class ProxyIDirect3D9 : public IDirect3D9 {
private:
    IDirect3D9* m_pReal;

public:
    ProxyIDirect3D9(IDirect3D9* pReal) : m_pReal(pReal) {}

    // IUnknown
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppvObj) override {
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    ULONG __stdcall AddRef() override { return m_pReal->AddRef(); }
    ULONG __stdcall Release() override {
        ULONG count = m_pReal->Release();
        if (count == 0) delete this;
        return count;
    }

    // IDirect3D9 - forward all except CreateDevice
    HRESULT __stdcall RegisterSoftwareDevice(void* pInit) override {
        return m_pReal->RegisterSoftwareDevice(pInit);
    }
    UINT __stdcall GetAdapterCount() override { return m_pReal->GetAdapterCount(); }
    HRESULT __stdcall GetAdapterIdentifier(UINT a, DWORD b, D3DADAPTER_IDENTIFIER9* c) override {
        return m_pReal->GetAdapterIdentifier(a, b, c);
    }
    UINT __stdcall GetAdapterModeCount(UINT a, D3DFORMAT b) override {
        return m_pReal->GetAdapterModeCount(a, b);
    }
    HRESULT __stdcall EnumAdapterModes(UINT a, D3DFORMAT b, UINT c, D3DDISPLAYMODE* d) override {
        return m_pReal->EnumAdapterModes(a, b, c, d);
    }
    HRESULT __stdcall GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* b) override {
        return m_pReal->GetAdapterDisplayMode(a, b);
    }
    HRESULT __stdcall CheckDeviceType(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, BOOL e) override {
        return m_pReal->CheckDeviceType(a, b, c, d, e);
    }
    HRESULT __stdcall CheckDeviceFormat(UINT a, D3DDEVTYPE b, D3DFORMAT c, DWORD d, D3DRESOURCETYPE e, D3DFORMAT f) override {
        return m_pReal->CheckDeviceFormat(a, b, c, d, e, f);
    }
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE b, D3DFORMAT c, BOOL d, D3DMULTISAMPLE_TYPE e, DWORD* f) override {
        return m_pReal->CheckDeviceMultiSampleType(a, b, c, d, e, f);
    }
    HRESULT __stdcall CheckDepthStencilMatch(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d, D3DFORMAT e) override {
        return m_pReal->CheckDepthStencilMatch(a, b, c, d, e);
    }
    HRESULT __stdcall CheckDeviceFormatConversion(UINT a, D3DDEVTYPE b, D3DFORMAT c, D3DFORMAT d) override {
        return m_pReal->CheckDeviceFormatConversion(a, b, c, d);
    }
    HRESULT __stdcall GetDeviceCaps(UINT a, D3DDEVTYPE b, D3DCAPS9* c) override {
        return m_pReal->GetDeviceCaps(a, b, c);
    }
    HMONITOR __stdcall GetAdapterMonitor(UINT a) override {
        return m_pReal->GetAdapterMonitor(a);
    }

    // CreateDevice - wrap the device!
    HRESULT __stdcall CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pParams,
                                    IDirect3DDevice9** ppDevice) override {
        IDirect3DDevice9* pRealDevice = nullptr;
        HRESULT hr = m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pParams, &pRealDevice);
        if (SUCCEEDED(hr) && pRealDevice) {
            *ppDevice = new ProxyIDirect3DDevice9(pRealDevice);
        }
        return hr;
    }
};

// Function pointers for forwarded functions
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT, void**);
typedef int(WINAPI* D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int(WINAPI* D3DPERF_EndEvent_t)(void);
typedef void(WINAPI* D3DPERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void(WINAPI* D3DPERF_SetRegion_t)(D3DCOLOR, LPCWSTR);
typedef BOOL(WINAPI* D3DPERF_QueryRepeatFrame_t)(void);
typedef void(WINAPI* D3DPERF_SetOptions_t)(DWORD);
typedef DWORD(WINAPI* D3DPERF_GetStatus_t)(void);

static Direct3DCreate9Ex_t g_RealDirect3DCreate9Ex = nullptr;
static D3DPERF_BeginEvent_t g_RealD3DPERF_BeginEvent = nullptr;
static D3DPERF_EndEvent_t g_RealD3DPERF_EndEvent = nullptr;
static D3DPERF_SetMarker_t g_RealD3DPERF_SetMarker = nullptr;
static D3DPERF_SetRegion_t g_RealD3DPERF_SetRegion = nullptr;
static D3DPERF_QueryRepeatFrame_t g_RealD3DPERF_QueryRepeatFrame = nullptr;
static D3DPERF_SetOptions_t g_RealD3DPERF_SetOptions = nullptr;
static D3DPERF_GetStatus_t g_RealD3DPERF_GetStatus = nullptr;

static bool LoadRealD3D9() {
    if (g_RealD3D9) return true;

    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\d3d9.dll");
    g_RealD3D9 = LoadLibraryA(path);
    if (!g_RealD3D9) return false;

    g_RealDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(g_RealD3D9, "Direct3DCreate9");
    g_RealDirect3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_RealD3D9, "Direct3DCreate9Ex");
    g_RealD3DPERF_BeginEvent = (D3DPERF_BeginEvent_t)GetProcAddress(g_RealD3D9, "D3DPERF_BeginEvent");
    g_RealD3DPERF_EndEvent = (D3DPERF_EndEvent_t)GetProcAddress(g_RealD3D9, "D3DPERF_EndEvent");
    g_RealD3DPERF_SetMarker = (D3DPERF_SetMarker_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetMarker");
    g_RealD3DPERF_SetRegion = (D3DPERF_SetRegion_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetRegion");
    g_RealD3DPERF_QueryRepeatFrame = (D3DPERF_QueryRepeatFrame_t)GetProcAddress(g_RealD3D9, "D3DPERF_QueryRepeatFrame");
    g_RealD3DPERF_SetOptions = (D3DPERF_SetOptions_t)GetProcAddress(g_RealD3D9, "D3DPERF_SetOptions");
    g_RealD3DPERF_GetStatus = (D3DPERF_GetStatus_t)GetProcAddress(g_RealD3D9, "D3DPERF_GetStatus");

    return true;
}

// Exported Direct3DCreate9
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    if (!LoadRealD3D9() || !g_RealDirect3DCreate9) return nullptr;

    IDirect3D9* pReal = g_RealDirect3DCreate9(SDKVersion);
    if (!pReal) return nullptr;

    return new ProxyIDirect3D9(pReal);
}

HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
    if (!LoadRealD3D9() || !g_RealDirect3DCreate9Ex) return E_FAIL;
    return g_RealDirect3DCreate9Ex(SDKVersion, (void**)ppD3D);
}

int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_BeginEvent) return 0;
    return g_RealD3DPERF_BeginEvent(color, name);
}

int WINAPI D3DPERF_EndEvent(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_EndEvent) return 0;
    return g_RealD3DPERF_EndEvent();
}

void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetMarker)
        g_RealD3DPERF_SetMarker(color, name);
}

void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetRegion)
        g_RealD3DPERF_SetRegion(color, name);
}

BOOL WINAPI D3DPERF_QueryRepeatFrame(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_QueryRepeatFrame) return FALSE;
    return g_RealD3DPERF_QueryRepeatFrame();
}

void WINAPI D3DPERF_SetOptions(DWORD options) {
    if (LoadRealD3D9() && g_RealD3DPERF_SetOptions)
        g_RealD3DPERF_SetOptions(options);
}

DWORD WINAPI D3DPERF_GetStatus(void) {
    if (!LoadRealD3D9() || !g_RealD3DPERF_GetStatus) return 0;
    return g_RealD3DPERF_GetStatus();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            if (g_RealD3D9) FreeLibrary(g_RealD3D9);
            break;
    }
    return TRUE;
}

