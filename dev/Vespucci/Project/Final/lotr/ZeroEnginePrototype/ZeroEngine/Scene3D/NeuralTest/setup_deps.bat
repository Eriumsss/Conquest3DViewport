@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo === Neural Test Dependency Setup ===
echo.

REM === GLFW 3.4 Win64 ===
if exist "deps\glfw\lib-vc2022\glfw3.lib" (
    echo [OK] GLFW already downloaded
) else (
    echo [DL] Downloading GLFW 3.4 Win64...
    curl -L -o glfw.zip "https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.bin.WIN64.zip"
    if !ERRORLEVEL! NEQ 0 (
        echo [ERROR] Failed to download GLFW
        exit /b 1
    )
    echo [DL] Extracting GLFW...
    powershell -Command "Expand-Archive -Force 'glfw.zip' 'glfw_tmp'"
    copy "glfw_tmp\glfw-3.4.bin.WIN64\include\GLFW\glfw3.h" "deps\glfw\include\GLFW\" >nul
    copy "glfw_tmp\glfw-3.4.bin.WIN64\include\GLFW\glfw3native.h" "deps\glfw\include\GLFW\" >nul
    copy "glfw_tmp\glfw-3.4.bin.WIN64\lib-vc2022\glfw3.lib" "deps\glfw\lib-vc2022\" >nul
    rd /s /q glfw_tmp
    del glfw.zip
    echo [OK] GLFW installed
)

REM === GLAD (OpenGL 3.3 Core) ===
if exist "deps\glad\glad.h" (
    echo [OK] GLAD already downloaded
) else (
    echo [DL] Downloading GLAD (OpenGL 3.3 Core)...
    curl -L -o glad.zip "https://github.com/nicholasgasior/glfwgladstarter/raw/master/glad.zip"
    if !ERRORLEVEL! NEQ 0 (
        echo [NOTE] Auto-download failed. Please manually generate GLAD:
        echo   1. Go to https://glad.dafrok.com/
        echo   2. Language=C, Specification=OpenGL, Profile=Core, API=3.3, Generate Loader=yes
        echo   3. Download and extract glad.h, glad.c, khrplatform.h to deps\glad\
        exit /b 1
    )
    powershell -Command "Expand-Archive -Force 'glad.zip' 'glad_tmp'"
    REM Try to find glad files in extracted archive
    for /r glad_tmp %%F in (glad.h) do copy "%%F" "deps\glad\" >nul 2>nul
    for /r glad_tmp %%F in (glad.c) do copy "%%F" "deps\glad\" >nul 2>nul
    for /r glad_tmp %%F in (khrplatform.h) do copy "%%F" "deps\glad\" >nul 2>nul
    rd /s /q glad_tmp
    del glad.zip
    if exist "deps\glad\glad.h" (
        echo [OK] GLAD installed
    ) else (
        echo [NOTE] GLAD auto-extract failed. Please manually generate:
        echo   1. Go to https://glad.dafrok.com/
        echo   2. Language=C, Specification=OpenGL, Profile=Core, API=3.3
        echo   3. Place glad.h, glad.c, khrplatform.h in deps\glad\
    )
)

echo.
echo === Dependency check ===
if exist "deps\glfw\lib-vc2022\glfw3.lib" (echo   GLFW:  OK) else (echo   GLFW:  MISSING)
if exist "deps\glad\glad.h"               (echo   GLAD:  OK) else (echo   GLAD:  MISSING)
echo.
echo Done. Run build.bat to compile.
