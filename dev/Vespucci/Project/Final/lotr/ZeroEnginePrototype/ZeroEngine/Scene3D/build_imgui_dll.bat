@echo off
REM Build ImGui docking DLL with MSVC x86 toolset (avoids 64-bit MinGW issues)
REM Now includes Vespucci Smart Wiring (Phase 10d) - 213 files across:
REM   Core / Schema / Compat+DSL / Scene / Index / Corpus / Suggest / Apply
REM   Autocomplete / Repair / UI/* / Telemetry / Plugin / Localization / Docs / QA
REM Tools/* mains are NOT in this build - they live in their own batches.
REM
REM 2026-05-19 layout flat-pack: all root-level Scene3D/editor sources now live
REM under Vespucci\{Engine,Animation,Effects,Level,Mocap,Collab,Tools,Neural,Glue}.
REM Bare-name #include "Foo.h" still works because every domain folder is on /I.

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

REM Aggregate include search paths so every bare-name "Foo.h" still resolves
REM after the flat-pack. Order is: Vespucci root first (existing Smart Wiring
REM headers), then the new domain folders alphabetically.
REM
REM NOTE: Vendor\ is intentionally OFF this list. It contains the VC8-era
REM stdint.h / fix_stdint.h shims that the modern MSVC toolchain doesn't need
REM and that actively break <cstdint> when shadowed. Only rebuild_all.bat
REM (which uses VC8 to compile miniz.c) puts Vendor on its /I path.
set "VINC=/IVespucci /IVespucci\Animation /IVespucci\Collab /IVespucci\Effects /IVespucci\Engine /IVespucci\Glue /IVespucci\Level /IVespucci\Mocap /IVespucci\Neural /IVespucci\Tools"

REM ---------------------------------------------------------------------
REM Phase 1: pre-compile the duplicate-named .cpp files into uniquely-named
REM .obj files. Without this, multiple Panel.obj / IndexFactory.obj clobber
REM each other in the build dir and the linker drops the symbols defined in
REM all but the last one. Each file gets a folder-tagged .obj suffix.
REM ---------------------------------------------------------------------
set "CL_COMMON=/nologo /std:c++17 /EHsc /MT /c /DWIN32 /D_WINDOWS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /DIMGUI_GLUE_EXPORTS /I. /Iimgui !VINC!"

cl !CL_COMMON! /FoIndexFactory_Index.obj    Vespucci\Index\IndexFactory.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoIndexFactory_AC.obj       Vespucci\Autocomplete\IndexFactory.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_SuggestedWires.obj  Vespucci\UI\SuggestedWires\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_HealthFix.obj       Vespucci\UI\HealthFix\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_CompatEditor.obj    Vespucci\UI\CompatEditor\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_CorpusBrowser.obj   Vespucci\UI\CorpusBrowser\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_SchemaDiff.obj      Vespucci\UI\SchemaDiff\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_ReplayViewer.obj    Vespucci\UI\ReplayViewer\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoPanel_GoldenRunner.obj    Vespucci\UI\GoldenRunner\Panel.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoDiffView_HealthFix.obj    Vespucci\UI\HealthFix\DiffView.cpp
if errorlevel 1 ( popd & exit /b 1 )
cl !CL_COMMON! /FoDiffView_GoldenRunner.obj Vespucci\UI\GoldenRunner\DiffView.cpp
if errorlevel 1 ( popd & exit /b 1 )

REM ---------------------------------------------------------------------
REM Phase 2: compile remaining unique-named .cpps and link everything,
REM including the seven prebuilt .obj files from Phase 1.
REM ---------------------------------------------------------------------
cl /nologo /std:c++17 /EHsc /MT ^
  /DWIN32 /D_WINDOWS /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD /DIMGUI_GLUE_EXPORTS ^
  /I. /Iimgui !VINC! ^
  Vespucci\Glue\imgui_glue_dll.cpp Vespucci\Glue\imgui_mocap_panel.cpp ^
  Vespucci\Tools\EventGraphEditor.cpp ^
  Vespucci\Vespucci.cpp ^
  Vespucci\Core\VespucciAssert.cpp Vespucci\Core\Hash.cpp Vespucci\Core\Crc32Tables.cpp ^
  Vespucci\Core\StringPool.cpp Vespucci\Core\Arena.cpp Vespucci\Core\RingBuffer.cpp ^
  Vespucci\Core\StableSort.cpp Vespucci\Core\HeatTimer.cpp Vespucci\Core\PathUtils.cpp ^
  Vespucci\Core\FileIO.cpp Vespucci\Core\JsonLite.cpp Vespucci\Core\Logging.cpp ^
  Vespucci\Schema\ZETypeRegistry.cpp Vespucci\Schema\ZETypeRegistry_Builtins.cpp ^
  Vespucci\Schema\EventActionSignatureDB.cpp Vespucci\Schema\SignatureLoader_Lua.cpp ^
  Vespucci\Schema\SchemaVersion.cpp ^
  Vespucci\Compat\CompatibilityMatrix.cpp Vespucci\Compat\RuleSet.cpp ^
  Vespucci\Compat\DSL\Diagnostics.cpp Vespucci\Compat\DSL\SourceMap.cpp ^
  Vespucci\Compat\DSL\Lexer.cpp Vespucci\Compat\DSL\Parser.cpp ^
  Vespucci\Compat\DSL\AstPrinter.cpp Vespucci\Compat\DSL\Sema.cpp ^
  Vespucci\Compat\DSL\IrLower.cpp Vespucci\Compat\DSL\Codegen.cpp ^
  Vespucci\Compat\DSL\Vm.cpp Vespucci\Compat\DSL\StdLib.cpp ^
  Vespucci\Scene\SceneSnapshot.cpp Vespucci\Scene\EntityIndex.cpp ^
  Vespucci\Scene\WireGraphIndex.cpp Vespucci\Scene\LayerIndex.cpp ^
  Vespucci\Scene\SnapshotDiff.cpp Vespucci\Scene\SnapshotDebugDump.cpp ^
  Vespucci\Index\GridIndex.cpp Vespucci\Index\KdTreeIndex.cpp ^
  Vespucci\Index\RTreeIndex.cpp Vespucci\Index\BvhIndex.cpp ^
  Vespucci\Index\IndexBenchmark.cpp ^
  Vespucci\Corpus\CorpusPriors.cpp Vespucci\Corpus\LocalCorpusBuilder.cpp ^
  Vespucci\Corpus\GlobalCorpusBuilder.cpp Vespucci\Corpus\CorpusReader.cpp ^
  Vespucci\Corpus\CorpusWriter.cpp Vespucci\Corpus\CorpusMerger.cpp ^
  Vespucci\Corpus\Decay.cpp Vespucci\Corpus\Smoothing.cpp ^
  Vespucci\Suggest\Features.cpp Vespucci\Suggest\CandidateGenerator.cpp ^
  Vespucci\Suggest\ConfidenceGate.cpp Vespucci\Suggest\ReasonBuilder.cpp ^
  Vespucci\Suggest\AutocompleteRanker.cpp Vespucci\Suggest\RepairRanker.cpp ^
  Vespucci\Suggest\SuggestionCache.cpp Vespucci\Suggest\SuggestionDebugDump.cpp ^
  Vespucci\Suggest\Strategy\FrequencyRanker.cpp ^
  Vespucci\Suggest\Strategy\TypeDirectedRanker.cpp ^
  Vespucci\Suggest\Strategy\HybridRanker.cpp ^
  Vespucci\Suggest\Strategy\MLStubRanker.cpp ^
  Vespucci\Suggest\Strategy\RankerSelector.cpp ^
  Vespucci\Autocomplete\SortedVecIndex.cpp Vespucci\Autocomplete\TrieIndex.cpp ^
  Vespucci\Autocomplete\MarisaIndex.cpp Vespucci\Autocomplete\CedarIndex.cpp ^
  Vespucci\Autocomplete\FuzzyMatch.cpp ^
  Vespucci\Repair\BrokenRefScanner.cpp Vespucci\Repair\DidYouMean.cpp ^
  Vespucci\Repair\HealPlanner.cpp Vespucci\Repair\HealthScorer.cpp ^
  Vespucci\Repair\HealthRules.cpp ^
  Vespucci\Apply\Apply.cpp Vespucci\Apply\Preview.cpp Vespucci\Apply\Undo.cpp ^
  Vespucci\Apply\BatchOps.cpp Vespucci\Apply\PlacementPolicy.cpp ^
  Vespucci\UI\Common\Theme.cpp Vespucci\UI\Common\ReasonChip.cpp ^
  Vespucci\UI\Common\ScoreBar.cpp Vespucci\UI\Common\Sparkline.cpp ^
  Vespucci\UI\Common\SearchBox.cpp Vespucci\UI\Common\Tooltip.cpp ^
  Vespucci\UI\Autocomplete\EventAutocompletePopup.cpp ^
  Vespucci\UI\Autocomplete\PopupFiltering.cpp ^
  Vespucci\UI\Autocomplete\PopupKeybinds.cpp ^
  Vespucci\UI\SuggestedWires\RankedList.cpp ^
  Vespucci\UI\SuggestedWires\ReasonStripe.cpp Vespucci\UI\SuggestedWires\Filters.cpp ^
  Vespucci\UI\SuggestedWires\AcceptRejectButtons.cpp ^
  Vespucci\UI\SuggestedWires\Pinning.cpp Vespucci\UI\SuggestedWires\HistoryPane.cpp ^
  Vespucci\UI\HealthFix\RepairList.cpp ^
  Vespucci\UI\HealthFix\BatchActions.cpp ^
  Vespucci\UI\DebugRanker\Overlay.cpp Vespucci\UI\DebugRanker\WeightSliders.cpp ^
  Vespucci\UI\DebugRanker\ScoreBreakdown.cpp Vespucci\UI\DebugRanker\FeatureHistogram.cpp ^
  Vespucci\UI\CompatEditor\RuleEditor.cpp ^
  Vespucci\UI\CompatEditor\Validator.cpp Vespucci\UI\CompatEditor\MatrixGrid.cpp ^
  Vespucci\UI\CorpusBrowser\PerTypeView.cpp ^
  Vespucci\UI\CorpusBrowser\PerEventView.cpp ^
  Vespucci\UI\SchemaDiff\MigrationWizard.cpp ^
  Vespucci\UI\ReplayViewer\Timeline.cpp ^
  Vespucci\UI\ReplayViewer\FrameDiff.cpp ^
  Vespucci\UI\GhostOverlay\GhostTargetOverlay.cpp ^
  Vespucci\UI\GhostOverlay\ViewportProj.cpp Vespucci\UI\GhostOverlay\Pulse.cpp ^
  Vespucci\UI\GhostOverlay\ColorRules.cpp ^
  Vespucci\UI\Modes\InlineRibbon.cpp Vespucci\UI\Modes\StatusBarHints.cpp ^
  Vespucci\UI\Modes\ModalWizard.cpp Vespucci\UI\Modes\PaletteSwitcher.cpp ^
  Vespucci\UI\Modes\FloatingHud.cpp ^
  Vespucci\Telemetry\PerfScope.cpp Vespucci\Telemetry\Counters.cpp ^
  Vespucci\Telemetry\Sinks.cpp Vespucci\Telemetry\Dashboard.cpp ^
  Vespucci\Telemetry\HotPathProfiler.cpp Vespucci\Telemetry\EventLog.cpp ^
  Vespucci\Plugin\PluginRegistry.cpp Vespucci\Plugin\PluginLoader.cpp ^
  Vespucci\Localization\StringTable.cpp Vespucci\Localization\PluralRules.cpp ^
  Vespucci\Localization\LangPack_en.cpp Vespucci\Localization\LangPack_tr.cpp ^
  Vespucci\Localization\RtlSupport.cpp ^
  Vespucci\Docs\HeaderScraper.cpp Vespucci\Docs\MarkdownEmitter.cpp ^
  Vespucci\Docs\DslReferenceEmitter.cpp ^
  Vespucci\QA\ReplayRecorder.cpp Vespucci\QA\ReplayPlayer.cpp ^
  Vespucci\QA\ReplayBenchmark.cpp Vespucci\QA\GoldenSuggestionTests.cpp ^
  Vespucci\QA\GoldenCorpus.cpp Vespucci\QA\RegressionRunner.cpp ^
  imgui\imgui.cpp imgui\imgui_draw.cpp imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
  imgui\imgui_impl_win32.cpp imgui\imgui_impl_dx9.cpp ^
  IndexFactory_Index.obj IndexFactory_AC.obj ^
  Panel_SuggestedWires.obj Panel_HealthFix.obj Panel_CompatEditor.obj ^
  Panel_CorpusBrowser.obj Panel_SchemaDiff.obj ^
  Panel_ReplayViewer.obj Panel_GoldenRunner.obj ^
  DiffView_HealthFix.obj DiffView_GoldenRunner.obj ^
  /LD /link /NOLOGO /OUT:imgui_d3d9.dll /IMPLIB:imgui_d3d9.lib ^
  "/LIBPATH:!DX_LIB!" d3d9.lib d3dx9.lib d3dcompiler.lib dwmapi.lib user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib comdlg32.lib
if %ERRORLEVEL% NEQ 0 (
  popd
  exit /b 1
)

popd
endlocal
