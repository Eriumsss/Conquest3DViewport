@echo off
REM Build the standalone Vespucci corpus builder. This is the offline
REM tool that walks every .pak in a directory and emits the
REM forge_global_corpus.bin blob the editor loads as a cold-start prior.
REM
REM Usage after build:
REM   corpus_builder.exe <pak_dir> <output_corpus.bin>
REM
REM Standalone exe — does NOT link against imgui_d3d9.dll. Pulls in only
REM Tools/CorpusBuilderMain.cpp + the Corpus/Schema/Core sources it
REM transitively depends on.

setlocal enabledelayedexpansion
pushd "%~dp0"

for /f "tokens=*" %%A in ('dir /s /b "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" 2^>nul') do (
  set "VCVARS_PATH=%%A"
)

if not defined VCVARS_PATH (
  echo ERROR: vcvarsall.bat not found
  popd
  exit /b 1
)

call "!VCVARS_PATH!" x86
if %ERRORLEVEL% NEQ 0 (
  popd
  exit /b 1
)

cl /nologo /std:c++17 /EHsc /MT ^
  /DWIN32 /D_WINDOWS /D_CONSOLE ^
  /IVespucci ^
  Vespucci\Tools\CorpusBuilderMain.cpp ^
  Vespucci\Corpus\CorpusPriors.cpp ^
  Vespucci\Corpus\LocalCorpusBuilder.cpp ^
  Vespucci\Corpus\GlobalCorpusBuilder.cpp ^
  Vespucci\Corpus\CorpusReader.cpp ^
  Vespucci\Corpus\CorpusWriter.cpp ^
  Vespucci\Corpus\CorpusMerger.cpp ^
  Vespucci\Corpus\Decay.cpp ^
  Vespucci\Corpus\Smoothing.cpp ^
  Vespucci\Schema\ZETypeRegistry.cpp ^
  Vespucci\Schema\ZETypeRegistry_Builtins.cpp ^
  Vespucci\Schema\EventActionSignatureDB.cpp ^
  Vespucci\Schema\SignatureLoader_Lua.cpp ^
  Vespucci\Schema\SchemaVersion.cpp ^
  Vespucci\Scene\SceneSnapshot.cpp ^
  Vespucci\Scene\EntityIndex.cpp ^
  Vespucci\Scene\WireGraphIndex.cpp ^
  Vespucci\Scene\LayerIndex.cpp ^
  Vespucci\Scene\SnapshotDiff.cpp ^
  Vespucci\Scene\SnapshotDebugDump.cpp ^
  Vespucci\Core\VespucciAssert.cpp ^
  Vespucci\Core\Hash.cpp ^
  Vespucci\Core\Crc32Tables.cpp ^
  Vespucci\Core\StringPool.cpp ^
  Vespucci\Core\Arena.cpp ^
  Vespucci\Core\RingBuffer.cpp ^
  Vespucci\Core\StableSort.cpp ^
  Vespucci\Core\HeatTimer.cpp ^
  Vespucci\Core\PathUtils.cpp ^
  Vespucci\Core\FileIO.cpp ^
  Vespucci\Core\JsonLite.cpp ^
  Vespucci\Core\Logging.cpp ^
  /link /OUT:corpus_builder.exe user32.lib
if %ERRORLEVEL% NEQ 0 (
  popd
  exit /b 1
)

popd
endlocal
