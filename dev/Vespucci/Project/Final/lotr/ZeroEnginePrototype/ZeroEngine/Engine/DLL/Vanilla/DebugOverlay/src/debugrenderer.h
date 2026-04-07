#pragma once

#include <Windows.h>
#include <d3d9.h>
#include <string>
//Eriumsss
// ============================================================================
// Debug Renderer
// Provides text and primitive rendering for debug overlays
// ============================================================================

namespace DebugRenderer {

    // Initialize renderer resources (call after D3D device is available)
    bool Initialize(IDirect3DDevice9* device);

    // Release resources (call before device reset or shutdown)
    void Release();

    // Call at start of frame rendering
    void BeginFrame(IDirect3DDevice9* device);

    // Call at end of frame rendering
    void EndFrame();

    // ---- Text Rendering ----
    
    // Draw text at screen position
    void DrawText(int x, int y, DWORD color, const char* text);
    void DrawText(int x, int y, DWORD color, const std::string& text);
    
    // Draw text with shadow
    void DrawTextShadowed(int x, int y, DWORD color, const char* text);
    
    // Draw formatted text
    void DrawTextF(int x, int y, DWORD color, const char* format, ...);

    // ---- Primitive Rendering ----
    
    // Draw a 2D line
    void DrawLine(float x1, float y1, float x2, float y2, DWORD color);
    
    // Draw a 2D rectangle (outline)
    void DrawRect(float x, float y, float width, float height, DWORD color);
    
    // Draw a filled 2D rectangle
    void DrawFilledRect(float x, float y, float width, float height, DWORD color);

    // ---- 3D Rendering (World Space) ----
    // Game uses standard XYZ: X=left/right, Y=height, Z=forward/back

    // Draw a 3D line (requires view/projection matrices)
    void DrawLine3D(float x1, float y1, float z1, float x2, float y2, float z2, DWORD color);

    // Draw a 3D box (axis-aligned)
    void DrawBox3D(float x, float y, float z, float sizeX, float sizeY, float sizeZ, DWORD color);

    // ---- Utility ----

    // Get text dimensions
    void GetTextSize(const char* text, int* width, int* height);

    // Convert world position to screen position (XYZ -> screen, Y is height)
    bool WorldToScreen(float worldX, float worldY, float worldZ, float* screenX, float* screenY);

    // Check if renderer is ready
    bool IsReady();

    // ---- Debug Functions ----
    void EnableW2SDebug(bool enable);
    int GetW2SFailReason();  // 0=ok, 1=no device, 2=view fail, 3=proj fail, 4=vp fail, 5=behind cam
    const D3DMATRIX* GetLastViewMatrix();
    const D3DMATRIX* GetLastProjMatrix();
    const D3DVIEWPORT9* GetLastViewport();
}

