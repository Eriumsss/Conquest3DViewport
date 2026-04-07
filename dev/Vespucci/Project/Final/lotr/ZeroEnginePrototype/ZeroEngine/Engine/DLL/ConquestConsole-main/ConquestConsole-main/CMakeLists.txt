cmake_minimum_required(VERSION 3.15)
project(DebugOverlay)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Wwise SDK paths (vendored v2.1.2821 / 2008)
# ============================================================================
set(WWISE_SDK_ROOT "${CMAKE_SOURCE_DIR}/../../../ZeroEnginePrototype/ZeroEngine/Engine/source/WwiseV28/SDK")
set(WWISE_INCLUDE  "${WWISE_SDK_ROOT}/include")
set(WWISE_LIB_DIR  "${WWISE_SDK_ROOT}/Win32/Release/lib")

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
    src/customaudio.cpp
    src/customaudio.h
    src/wwisesdk.cpp
    src/wwisesdk.h
)

# Wwise static library linkage - creates a separate engine instance
target_compile_definitions(DebugOverlay PRIVATE
    AKSOUNDENGINE_STATIC
    AK_OPTIMIZED
)

target_include_directories(DebugOverlay PRIVATE
    ${WWISE_INCLUDE}
)

target_link_directories(DebugOverlay PRIVATE
    ${WWISE_LIB_DIR}
)

# Link against D3D9 + Wwise static libraries
target_link_libraries(DebugOverlay
    d3d9
    AkMemoryMgr.lib
    AkStreamMgr.lib
    AkAudioEngine.lib
    AkLowerEngine.lib
    AkMusicEngine.lib
    AkVorbisDecoder.lib
    dsound.lib
    dxguid.lib
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

