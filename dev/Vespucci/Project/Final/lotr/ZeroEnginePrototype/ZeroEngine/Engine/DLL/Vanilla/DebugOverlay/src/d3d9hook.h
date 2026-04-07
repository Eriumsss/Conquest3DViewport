#pragma once

#include <Windows.h>
#include <d3d9.h>

// ============================================================================
// D3D9 Hook Interface
// Hooks EndScene to inject our debug rendering
// ============================================================================
//Eriumsss
namespace D3D9Hook {

    // Function pointer types for D3D9 methods
    using EndScene_t = HRESULT(__stdcall*)(IDirect3DDevice9*);
    using Reset_t = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
    using Present_t = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

    // Initialize the D3D9 hook
    // Returns true if successful
    bool Initialize();

    // Cleanup and restore original functions
    void Shutdown();

    // Get the hooked device (valid after first EndScene call)
    IDirect3DDevice9* GetDevice();

    // Check if hook is active
    bool IsInitialized();

    // Callback type for rendering
    using RenderCallback = void(*)(IDirect3DDevice9*);

    // Set the callback function for rendering
    void SetRenderCallback(RenderCallback callback);

    // Wireframe mode control
    void SetWireframe(bool enabled);
    bool GetWireframe();

    // Internal: Original function pointers (exposed for advanced use)
    extern EndScene_t OriginalEndScene;
    extern Reset_t OriginalReset;
}

