// Minimal overlay - just handles wireframe toggle, no rendering
// The injection-based DebugOverlay handles all UI rendering

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include "d3d9device.h"

void Overlay_OnBeginScene(IDirect3DDevice9* device) {
    // Check F4 input for wireframe toggle
    static bool lastF4 = false;
    bool f4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
    if (f4 && !lastF4) {
        g_WireframeEnabled = !g_WireframeEnabled;
    }
    lastF4 = f4;
}

void Overlay_OnEndScene(IDirect3DDevice9* device) {
    // Nothing to draw - injection overlay handles rendering
}

void Overlay_OnReset() {
    // Nothing to reset
}

