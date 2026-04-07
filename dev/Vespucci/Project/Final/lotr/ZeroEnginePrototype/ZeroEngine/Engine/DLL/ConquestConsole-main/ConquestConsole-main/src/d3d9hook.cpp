#include "d3d9hook.h"
#include "debugrenderer.h"
#include <cstdio>

// ============================================================================
// D3D9 Hook Implementation - EndScene + Reset (for Alt+Enter stability)
// ============================================================================
//Eriumsss
namespace D3D9Hook {

    EndScene_t OriginalEndScene = nullptr;
    Reset_t OriginalReset = nullptr;

    static IDirect3DDevice9* g_Device = nullptr;
    static bool g_Initialized = false;
    static RenderCallback g_RenderCallback = nullptr;
    static BYTE g_OriginalEndSceneBytes[5] = { 0 };
    static BYTE g_OriginalResetBytes[5] = { 0 };
    static void* g_EndSceneAddr = nullptr;
    static void* g_ResetAddr = nullptr;
    static bool g_DeviceLost = false;

    // Wireframe toggle (display only - actual wireframe not implemented)
    static bool g_WireframeEnabled = false;

    void SetWireframe(bool enabled) {
        g_WireframeEnabled = enabled;
    }

    bool GetWireframe() {
        return g_WireframeEnabled;
    }

    // Our hooked Reset - releases resources before Reset, recreates after
    HRESULT __stdcall HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters) {
        // Release all D3D resources before Reset
        g_DeviceLost = true;
        DebugRenderer::Release();

        // Restore original bytes, call original, re-apply hook
        DWORD oldProtect;
        VirtualProtect(g_ResetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(g_ResetAddr, g_OriginalResetBytes, 5);
        VirtualProtect(g_ResetAddr, 5, oldProtect, &oldProtect);

        HRESULT result = OriginalReset(device, pPresentationParameters);

        VirtualProtect(g_ResetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
        DWORD relAddr = (DWORD)HookedReset - (DWORD)g_ResetAddr - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy(g_ResetAddr, jmp, 5);
        VirtualProtect(g_ResetAddr, 5, oldProtect, &oldProtect);

        // If Reset succeeded, mark device as recovered
        if (SUCCEEDED(result)) {
            g_DeviceLost = false;
            // DebugRenderer will reinitialize on next EndScene call
        }

        return result;
    }

    // Our hooked EndScene - draws overlay
    HRESULT __stdcall HookedEndScene(IDirect3DDevice9* device) {
        g_Device = device;

        // Skip rendering if device is in lost state
        if (!g_DeviceLost) {
            // Call the user's render callback
            if (g_RenderCallback) {
                g_RenderCallback(device);
            }
        }

        // Restore original bytes, call original, re-apply hook
        DWORD oldProtect;
        VirtualProtect(g_EndSceneAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        memcpy(g_EndSceneAddr, g_OriginalEndSceneBytes, 5);
        VirtualProtect(g_EndSceneAddr, 5, oldProtect, &oldProtect);

        HRESULT result = OriginalEndScene(device);

        VirtualProtect(g_EndSceneAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
        DWORD relAddr = (DWORD)HookedEndScene - (DWORD)g_EndSceneAddr - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy(g_EndSceneAddr, jmp, 5);
        VirtualProtect(g_EndSceneAddr, 5, oldProtect, &oldProtect);

        return result;
    }

    bool GetD3D9VTable(void** vtable) {
        // Use unique class name with timestamp
        char className[64];
        sprintf(className, "D3D9Dummy%u", GetTickCount());

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(WNDCLASSEXA);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = className;

        if (!RegisterClassExA(&wc)) {
            MessageBoxA(NULL, "Failed: RegisterClassExA", "Debug", MB_OK);
            return false;
        }

        HWND hwnd = CreateWindowExA(0, className, "D3D9 Dummy", WS_OVERLAPPEDWINDOW,
                                     0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
        if (!hwnd) {
            MessageBoxA(NULL, "Failed: CreateWindowExA", "Debug", MB_OK);
            UnregisterClassA(className, wc.hInstance);
            return false;
        }

        IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d) {
            MessageBoxA(NULL, "Failed: Direct3DCreate9", "Debug", MB_OK);
            DestroyWindow(hwnd);
            UnregisterClassA(className, wc.hInstance);
            return false;
        }

        D3DPRESENT_PARAMETERS params = {};
        params.Windowed = TRUE;
        params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.hDeviceWindow = hwnd;
        params.BackBufferFormat = D3DFMT_UNKNOWN;

        IDirect3DDevice9* device = nullptr;

        // Try HAL first, then NULLREF if that fails (fullscreen games block HAL)
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                        &params, &device);
        if (FAILED(hr)) {
            // Try NULLREF - always works but we just need vtable addresses
            hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                   &params, &device);
        }
        if (FAILED(hr)) {
            d3d->Release();
            DestroyWindow(hwnd);
            UnregisterClassA(className, wc.hInstance);
            return false;
        }

        memcpy(vtable, *(void***)device, sizeof(void*) * 119);

        device->Release();
        d3d->Release();
        DestroyWindow(hwnd);
        UnregisterClassA(className, wc.hInstance);
        return true;
    }

    bool Initialize() {
        if (g_Initialized) return true;

        void* vtable[119];
        if (!GetD3D9VTable(vtable)) {
            return false;
        }

        // Get function addresses
        g_EndSceneAddr = vtable[42];    // EndScene (index 42)
        g_ResetAddr = vtable[16];       // Reset (index 16)
        OriginalEndScene = (EndScene_t)g_EndSceneAddr;
        OriginalReset = (Reset_t)g_ResetAddr;

        // Save original bytes for both hooks
        memcpy(g_OriginalEndSceneBytes, g_EndSceneAddr, 5);
        memcpy(g_OriginalResetBytes, g_ResetAddr, 5);

        DWORD oldProtect;
        BYTE jmp[5] = { 0xE9, 0, 0, 0, 0 };
        DWORD relAddr;

        // Install EndScene hook
        VirtualProtect(g_EndSceneAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        relAddr = (DWORD)HookedEndScene - (DWORD)g_EndSceneAddr - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy(g_EndSceneAddr, jmp, 5);
        VirtualProtect(g_EndSceneAddr, 5, oldProtect, &oldProtect);

        // Install Reset hook (critical for Alt+Enter stability)
        VirtualProtect(g_ResetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        relAddr = (DWORD)HookedReset - (DWORD)g_ResetAddr - 5;
        memcpy(&jmp[1], &relAddr, 4);
        memcpy(g_ResetAddr, jmp, 5);
        VirtualProtect(g_ResetAddr, 5, oldProtect, &oldProtect);

        g_Initialized = true;
        return true;
    }

    void Shutdown() {
        DWORD oldProtect;
        if (g_Initialized) {
            // Restore EndScene
            if (g_EndSceneAddr) {
                VirtualProtect(g_EndSceneAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
                memcpy(g_EndSceneAddr, g_OriginalEndSceneBytes, 5);
                VirtualProtect(g_EndSceneAddr, 5, oldProtect, &oldProtect);
            }
            // Restore Reset
            if (g_ResetAddr) {
                VirtualProtect(g_ResetAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
                memcpy(g_ResetAddr, g_OriginalResetBytes, 5);
                VirtualProtect(g_ResetAddr, 5, oldProtect, &oldProtect);
            }
        }
        g_Initialized = false;
        g_DeviceLost = false;
        g_Device = nullptr;
        g_RenderCallback = nullptr;
    }

    IDirect3DDevice9* GetDevice() { return g_Device; }
    bool IsInitialized() { return g_Initialized; }
    void SetRenderCallback(RenderCallback callback) { g_RenderCallback = callback; }
}

