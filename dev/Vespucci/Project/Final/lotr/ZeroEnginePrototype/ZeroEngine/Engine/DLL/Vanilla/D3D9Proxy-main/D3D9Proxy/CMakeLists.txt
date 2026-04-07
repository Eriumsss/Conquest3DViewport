cmake_minimum_required(VERSION 3.10)
project(D3D9Proxy)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# D3D9 Proxy DLL
add_library(d3d9 SHARED
    src/d3d9proxy.cpp
    src/d3d9device.cpp
    src/overlay.cpp
    src/d3d9proxy.def
)

target_include_directories(d3d9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(d3d9 PRIVATE d3d9.lib)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

# Set output name to d3d9.dll
set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
)

