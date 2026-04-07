@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

echo [BUILD] Neural Test - GLFW + OpenGL 3.3

REM Check dependencies
if not exist "deps\glfw\lib-vc2022\glfw3.lib" (
    echo [ERROR] GLFW not found. Run setup_deps.bat first.
    popd & exit /b 1
)
if not exist "deps\glad\include\glad\glad.h" (
    echo [ERROR] GLAD not found. Run: python -m glad --profile core --out-path deps/glad --api "gl=3.3" --generator c
    popd & exit /b 1
)

REM Find VS2022 BuildTools
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "!VCVARS!" (
    echo [ERROR] VS2022 BuildTools not found
    popd & exit /b 1
)

call "!VCVARS!" x64 >nul 2>&1
echo [BUILD] MSVC x64 toolchain ready

cl /nologo /std:c++17 /EHsc /MD /O2 /W3 ^
    /DWIN32 /D_WINDOWS ^
    /I"deps\glad\include" /I"deps\glfw\include" ^
    neural_test.cpp deps\glad\src\glad.c ^
    /Fe:NeuralTest.exe ^
    /link /NOLOGO /SUBSYSTEM:CONSOLE ^
    /LIBPATH:"deps\glfw\lib-vc2022" ^
    glfw3.lib opengl32.lib user32.lib gdi32.lib shell32.lib

if !ERRORLEVEL! NEQ 0 (
    echo [BUILD] FAILED
    popd & exit /b 1
)

echo [BUILD] SUCCESS - NeuralTest.exe
popd & endlocal
