cmake_minimum_required(VERSION 3.15)
project(DebugOverlay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Debug Overlay DLL
# ============================================================================
add_library(DebugOverlay SHARED
    src/dllmain.cpp
    src/d3d9hook.cpp
    src/d3d9hook.h
    src/debugrenderer.cpp
    src/debugrenderer.h
    src/input.cpp
    src/input.h
    src/config.h
    src/gamedata.cpp
    src/gamedata.h
    src/audiohook.cpp
    src/audiohook.h
    src/hash_dictionary.cpp
    src/hash_dictionary.h
    src/bnkparser.cpp
    src/bnkparser.h
    src/miniaudio.h
)

# Link against D3D9 (no D3DX9 dependency - uses GDI for text)
target_link_libraries(DebugOverlay
    d3d9
)

# Output to bin folder
set_target_properties(DebugOverlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_compile_options(DebugOverlay PRIVATE /W3 /WX-)

# ============================================================================
# Injector Executable
# ============================================================================
add_executable(Injector
    injector/injector.cpp
)

set_target_properties(Injector PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ============================================================================
# Installation
# ============================================================================
install(TARGETS DebugOverlay Injector
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
)

