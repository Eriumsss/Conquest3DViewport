# PowerShell build script for ZeroEngine 3D Viewport
Write-Host "========================================"
Write-Host "Building ZeroEngine 3D Viewport (MSVC)"
Write-Host "========================================"

# Find Visual Studio
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"

if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    Write-Host "Visual Studio found at: $vsPath"
} else {
    Write-Host "ERROR: vswhere.exe not found"
    exit 1
}

# Set up environment
$vcvars = "$vsPath\VC\Auxiliary\Build\vcvars32.bat"
if (-not (Test-Path $vcvars)) {
    Write-Host "ERROR: vcvars32.bat not found"
    exit 1
}

# Get absolute paths (updated for ZeroEnginePrototype/ZeroEngine/Engine/havok folder structure)
$havokSourcePath = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Engine\source\havok\hk550\Source"
$havokDemoPath = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Engine\source\havok\hk550\Demo"
$havokLibPath = "C:\Users\UserName\Desktop\Oyun\The.Lord.of.the.Rings.Conquest\The Lord of the Rings - Conquest\dev\Vespucci\Project\Final\lotr\ZeroEnginePrototype\ZeroEngine\Engine\source\havok\hk550\Lib\win32_net_8-0\release_multithreaded"

# Create a temporary batch file to set up environment and compile
$tempBat = "temp_build.bat"
@"
@echo off
call "$vcvars" > nul
cl.exe /c /I"$havokSourcePath" /I"$havokSourcePath\\Animation\\Animation\\Playback\\Control\\Default" /I"$havokDemoPath" /DHAVOK_PHYSICS_ENABLED /DHAVOK_ANIMATION_ENABLED /DWIN32 /D_WINDOWS /DNDEBUG /EHsc /MD /O2 /W3 ZeroEngine3DViewport.cpp Scene3DLoader.cpp HavokToDisplayConverter.cpp Scene3DRenderer.cpp Scene3DCamera.cpp Scene3DSkybox.cpp Scene3DMaterial.cpp Scene3DAnimation.cpp Scene3DBoneEditor.cpp AnimationSystem.cpp HavokCompat.cpp LevelReader.cpp
link.exe /OUT:ZeroEngine3DViewport.exe /LIBPATH:"$havokLibPath" /MACHINE:X86 /SUBSYSTEM:WINDOWS ZeroEngine3DViewport.obj Scene3DLoader.obj HavokToDisplayConverter.obj Scene3DRenderer.obj Scene3DCamera.obj Scene3DSkybox.obj Scene3DMaterial.obj Scene3DAnimation.obj Scene3DBoneEditor.obj AnimationSystem.obj HavokCompat.obj LevelReader.obj hkBase.lib hkSerialize.lib hkSceneData.lib hkVisualize.lib hkCompat.lib hkaAnimation.lib hkaInternal.lib hkaRagdoll.lib hkgBridge.lib hkgCommon.lib hkgDx9.lib hkpCollide.lib hkpConstraintSolver.lib hkpDynamics.lib hkpInternal.lib hkpUtilities.lib hkpVehicle.lib gdi32.lib user32.lib kernel32.lib opengl32.lib advapi32.lib dinput8.lib dxguid.lib legacy_stdio_definitions.lib
"@ | Out-File -FilePath $tempBat -Encoding ASCII

# Run the build
& cmd.exe /c $tempBat

# Clean up
Remove-Item $tempBat

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "========================================"
    Write-Host "Build successful!"
    Write-Host "Output: ZeroEngine3DViewport.exe"
    Write-Host "========================================"
} else {
    Write-Host ""
    Write-Host "========================================"
    Write-Host "Build failed with error code $LASTEXITCODE"
    Write-Host "========================================"
}
