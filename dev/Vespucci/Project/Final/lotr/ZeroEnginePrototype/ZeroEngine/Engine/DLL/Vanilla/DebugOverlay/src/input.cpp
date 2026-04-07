#include "input.h"

// ============================================================================
// Input Handler Implementation
// ============================================================================
//Eriumsss
namespace Input {

    static bool g_CurrentState[256] = { false };
    static bool g_PreviousState[256] = { false };

    void Initialize() {
        memset(g_CurrentState, 0, sizeof(g_CurrentState));
        memset(g_PreviousState, 0, sizeof(g_PreviousState));
    }

    void Update() {
        // Copy current to previous
        memcpy(g_PreviousState, g_CurrentState, sizeof(g_CurrentState));

        // Update current state
        for (int i = 0; i < 256; i++) {
            g_CurrentState[i] = (GetAsyncKeyState(i) & 0x8000) != 0;
        }
    }

    bool IsKeyPressed(int vkCode) {
        if (vkCode < 0 || vkCode >= 256) return false;
        return g_CurrentState[vkCode] && !g_PreviousState[vkCode];
    }

    bool IsKeyDown(int vkCode) {
        if (vkCode < 0 || vkCode >= 256) return false;
        return g_CurrentState[vkCode];
    }

    bool IsKeyReleased(int vkCode) {
        if (vkCode < 0 || vkCode >= 256) return false;
        return !g_CurrentState[vkCode] && g_PreviousState[vkCode];
    }
}

