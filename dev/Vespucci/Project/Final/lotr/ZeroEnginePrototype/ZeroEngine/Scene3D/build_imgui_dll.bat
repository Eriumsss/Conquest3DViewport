@echo off
REM Build ImGui docking DLL with MSVC x86 toolset (avoids 64-bit MinGW issues)

setlocal enabledelayedexpansion
pushd "%~dp0"

REM Use short paths to avoid spaces in paths
for /f "tokens=*" %%A in ('dir /s /b "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" 2^>nul') do (
  set "VCVARS_PATH=%%A"
)

if not defined VCVARS_PATH (
  echo ERROR: vcvarsall.bat not found
  echo Please ensure Visual Studio 2022 BuildTools is installed
  popd
  exit /b 1
)

set "DXSDK_DIR=C:\Program Files (x86)\Microsoft DirectX SDK (March 2008)"
set "DX_LIB=!DXSDK_DIR!\Lib\x86"

call "!VCVARS_PATH!" x86
if %ERRORLEVEL% NEQ 0 (
  popd
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /MT ^
  /DWIN32 /D_WINDOWS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /DIMGUI_GLUE_EXPORTS ^
  /Iimgui ^
  imgui_glue_dll.cpp imgui_mocap_panel.cpp ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
  imgui\imgui_impl_win32.cpp imgui\imgui_impl_dx9.cpp ^
  /LD /link /NOLOGO /OUT:imgui_d3d9.dll /IMPLIB:imgui_d3d9.lib ^
  "/LIBPATH:!DX_LIB!" d3d9.lib d3dx9.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib
if %ERRORLEVEL% NEQ 0 (
  popd
  exit /b 1
)

popd
endlocal
