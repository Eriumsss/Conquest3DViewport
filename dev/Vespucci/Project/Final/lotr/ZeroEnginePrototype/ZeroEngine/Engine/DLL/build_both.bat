@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x86

echo === Building D3D9Proxy ===
cd /d "%~dp0D3D9Proxy\build"
cmake --build . --config Debug
if %ERRORLEVEL% NEQ 0 (
    echo D3D9Proxy BUILD FAILED
) else (
    echo D3D9Proxy BUILD OK
)

echo.
echo === Building ConquestDebugger ===
cd /d "%~dp0ConquestDebugger\build"
msbuild ConquestDebugger.vcxproj /p:Configuration=Release /p:Platform=Win32 /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo ConquestDebugger BUILD FAILED
) else (
    echo ConquestDebugger BUILD OK
)

echo.
echo === Done ===
pause
