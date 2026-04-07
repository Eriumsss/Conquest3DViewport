#pragma once

#include <Windows.h>
//Eriumsss
// ============================================================================
// Input Handler
// Handles keyboard input for toggling debug features
// ============================================================================

namespace Input {

    // Initialize input handling
    void Initialize();

    // Update input state (call each frame)
    void Update();

    // Check if a key was just pressed (not held)
    bool IsKeyPressed(int vkCode);

    // Check if a key is currently held down
    bool IsKeyDown(int vkCode);

    // Check if a key was just released
    bool IsKeyReleased(int vkCode);
}

