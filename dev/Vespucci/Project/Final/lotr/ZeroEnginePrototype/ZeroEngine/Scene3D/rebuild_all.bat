@echo off
call "C:\PROGRA~2\Microsoft Visual Studio 8\VC\vcvarsall.bat" x86 >nul 2>&1
subst H: /D >nul 2>&1
subst W: /D >nul 2>&1
subst H: "C:\Users\Yusuf\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Engine\source\havok\hk550" >nul 2>&1
subst W: "C:\Users\Yusuf\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Engine\source\WwiseV28\SDK" >nul 2>&1
set HAVOK_ROOT=H:
set DXSDK_DIR=C:\Program Files (x86)\Microsoft DirectX SDK (March 2008)\
set LUA_SRC=..\Engine\source\lua-5.1.5\src
set AUDIO_DIR=..\Engine\audio
cd /d "c:\Users\Yusuf\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Scene3D"

set CLFLAGS=/nologo /c /EHsc /W0 /MT /O2 /Z7 /D "WIN32" /D "NDEBUG" /D "HK_COMPILER_MSVC" /D "HK_PLATFORM_WIN32" /D "_WIN32_WINNT=0x0501" /D "AKSOUNDENGINE_STATIC" /D "_CRT_SECURE_NO_WARNINGS" /I "." /I "%HAVOK_ROOT%" /I "%HAVOK_ROOT%\Source" /I "%HAVOK_ROOT%\Demo" /I "%DXSDK_DIR%Include" /I "%LUA_SRC%" /I "%AUDIO_DIR%" /I "W:\include"

echo ============================================ > rebuild_out.txt
echo FULL REBUILD START %date% %time% >> rebuild_out.txt
echo ============================================ >> rebuild_out.txt

set ERRORS=0

for %%F in (
    ZeroEngine3DViewport.cpp
    Scene3DLoader.cpp
    HavokToDisplayConverter.cpp
    Scene3DRenderer.cpp
    MgPackedParticleShaders.cpp
    Scene3DCamera.cpp
    Scene3DSkybox.cpp
    Scene3DMaterial.cpp
    Scene3DAnimation.cpp
    LuaAnimationRuntime.cpp
    LuaAnimationGraphParser.cpp
    Scene3DBoneEditor.cpp
    AnimationSystem.cpp
    Scene3DEffects.cpp
    Scene3DEffectLoader.cpp
    Scene3DEffectManager.cpp
    HavokCompat.cpp
    AssetBrowser.cpp
    GameModelLoader.cpp
    LevelReader.cpp
    LevelScene.cpp
    LevelInspector.cpp
    PakRebuilder.cpp
    EntityFieldDefs.cpp
    MocapBridge.cpp
    MocapRetargeter.cpp
    MocapExporter.cpp
    miniz.c
) do (
    echo [COMPILE] %%F
    echo [COMPILE] %%F >> rebuild_out.txt
    cl.exe %CLFLAGS% %%F >> rebuild_out.txt 2>&1
    if errorlevel 1 (
        echo [FAILED] %%F
        echo [FAILED] %%F >> rebuild_out.txt
        set /a ERRORS+=1
    ) else (
        echo [OK] %%F
    )
)

echo.
echo === Audio files ===
for %%F in (
    "%AUDIO_DIR%\AudioManager.cpp"
    "%AUDIO_DIR%\BankLoadProgress.cpp"
    "%AUDIO_DIR%\EventMappingData.cpp"
) do (
    echo [COMPILE] %%~nxF
    echo [COMPILE] %%~nxF >> rebuild_out.txt
    cl.exe %CLFLAGS% %%F >> rebuild_out.txt 2>&1
    if errorlevel 1 (
        echo [FAILED] %%~nxF
        echo [FAILED] %%~nxF >> rebuild_out.txt
        set /a ERRORS+=1
    ) else (
        echo [OK] %%~nxF
    )
)

echo.
echo === Lua files ===
for %%F in (
    "%LUA_SRC%\lapi.c"
    "%LUA_SRC%\lauxlib.c"
    "%LUA_SRC%\lbaselib.c"
    "%LUA_SRC%\lcode.c"
    "%LUA_SRC%\ldblib.c"
    "%LUA_SRC%\ldebug.c"
    "%LUA_SRC%\ldo.c"
    "%LUA_SRC%\ldump.c"
    "%LUA_SRC%\lfunc.c"
    "%LUA_SRC%\lgc.c"
    "%LUA_SRC%\linit.c"
    "%LUA_SRC%\liolib.c"
    "%LUA_SRC%\llex.c"
    "%LUA_SRC%\lmathlib.c"
    "%LUA_SRC%\lmem.c"
    "%LUA_SRC%\loadlib.c"
    "%LUA_SRC%\lobject.c"
    "%LUA_SRC%\lopcodes.c"
    "%LUA_SRC%\loslib.c"
    "%LUA_SRC%\lparser.c"
    "%LUA_SRC%\lstate.c"
    "%LUA_SRC%\lstring.c"
    "%LUA_SRC%\lstrlib.c"
    "%LUA_SRC%\ltable.c"
    "%LUA_SRC%\ltablib.c"
    "%LUA_SRC%\ltm.c"
    "%LUA_SRC%\lundump.c"
    "%LUA_SRC%\lvm.c"
    "%LUA_SRC%\lzio.c"
) do (
    echo [COMPILE] %%~nxF
    echo [COMPILE] %%~nxF >> rebuild_out.txt
    cl.exe /nologo /c /W0 /MT /O2 /Z7 /D "WIN32" /D "NDEBUG" /D "_CRT_SECURE_NO_WARNINGS" /I "%LUA_SRC%" %%F >> rebuild_out.txt 2>&1
    if errorlevel 1 (
        echo [FAILED] %%~nxF
        set /a ERRORS+=1
    ) else (
        echo [OK] %%~nxF
    )
)

echo.
echo ============================================
echo COMPILE DONE — %ERRORS% errors
echo ============================================ >> rebuild_out.txt
echo COMPILE_ERRORS:%ERRORS% >> rebuild_out.txt

if %ERRORS% GTR 0 (
    echo Skipping link due to compile errors
    echo LINK_SKIPPED >> rebuild_out.txt
    goto :end
)

echo.
echo === LINKING ===
link.exe /OUT:ZeroEngine3DViewport.exe /LIBPATH:"H:\Lib\win32_net_8-0\release_multithreaded" /LIBPATH:"%DXSDK_DIR%Lib\x86" /LIBPATH:"W:\Win32\Release\lib" /MACHINE:X86 /SUBSYSTEM:WINDOWS /DEBUG /NODEFAULTLIB:MSVCRT ZeroEngine3DViewport.obj Scene3DLoader.obj HavokToDisplayConverter.obj Scene3DRenderer.obj MgPackedParticleShaders.obj Scene3DCamera.obj Scene3DSkybox.obj Scene3DMaterial.obj Scene3DAnimation.obj LuaAnimationRuntime.obj LuaAnimationGraphParser.obj Scene3DBoneEditor.obj AnimationSystem.obj Scene3DEffects.obj Scene3DEffectLoader.obj Scene3DEffectManager.obj HavokCompat.obj AssetBrowser.obj GameModelLoader.obj LevelReader.obj LevelScene.obj LevelInspector.obj PakRebuilder.obj EntityFieldDefs.obj MocapBridge.obj MocapRetargeter.obj MocapExporter.obj miniz.obj AudioManager.obj BankLoadProgress.obj EventMappingData.obj lapi.obj lauxlib.obj lbaselib.obj lcode.obj ldblib.obj ldebug.obj ldo.obj ldump.obj lfunc.obj lgc.obj linit.obj liolib.obj llex.obj lmathlib.obj lmem.obj loadlib.obj lobject.obj lopcodes.obj loslib.obj lparser.obj lstate.obj lstring.obj lstrlib.obj ltable.obj ltablib.obj ltm.obj lundump.obj lvm.obj lzio.obj hkBase.lib hkSerialize.lib hkSceneData.lib hkVisualize.lib hkCompat.lib hkaAnimation.lib hkaInternal.lib hkaRagdoll.lib hkgBridge.lib hkgCommon.lib hkgDx9.lib hkpCollide.lib hkpConstraintSolver.lib hkpDynamics.lib hkpInternal.lib hkpUtilities.lib hkpVehicle.lib AkAudioEngine.lib AkLowerEngine.lib AkMemoryMgr.lib AkStreamMgr.lib AkMusicEngine.lib AkVorbisDecoder.lib gdi32.lib user32.lib kernel32.lib opengl32.lib advapi32.lib dinput8.lib dxguid.lib d3d9.lib d3dx9.lib comctl32.lib dsound.lib ole32.lib >> rebuild_out.txt 2>&1
echo LINK_EXIT:%ERRORLEVEL% >> rebuild_out.txt
if errorlevel 1 (
    echo [LINK FAILED]
) else (
    echo [LINK OK]
)

:end
echo.
echo Full log: rebuild_out.txt
