// Vespucci.h
// =============================================================================
// VESPUCCI SMART WIRING - PUBLIC ENTRY POINT
// =============================================================================
// Written by: Eriumsss
//
// This is the ONE header imgui_glue_dll.cpp includes to wire Vespucci
// into The Forge. Everything else is internal. Mirror of the
// EventGraphEditor / SplineEditor module convention: namespace
// `Vespucci`, with Init / Update / DrawAllPanels / Shutdown.
//
// Lifecycle:
//   - Vespucci::Init() called once at DLL bootstrap. Builds registries,
//     loads corpus, registers lang packs, wires global singletons.
//   - Vespucci::Update(args) called every frame BEFORE DrawFrame.
//     Refreshes the SceneSnapshot, ticks Telemetry.
//   - Vespucci::DrawAllPanels(args) called inside DrawFrame.
//     Renders every Vespucci UI panel that the user has open.
//   - Vespucci::Shutdown() called once at DLL teardown.
// =============================================================================

#ifndef VESPUCCI_VESPUCCI_H_
#define VESPUCCI_VESPUCCI_H_

struct ImGuiGlueFrameArgs;

namespace Vespucci {

bool Init();
void Update(ImGuiGlueFrameArgs& args);
void DrawAllPanels(ImGuiGlueFrameArgs& args);
void Shutdown();

// One-shot: open one of the alternate UI surfaces (sidebar /
// healthfix / brain / compat editor / corpus browser / wizard /
// palette / hud). Called from imgui_glue_dll.cpp menu items.
enum SurfaceKind {
    SURFACE_SuggestedSidebar = 0,
    SURFACE_HealthFix        = 1,
    SURFACE_BrainDebug       = 2,
    SURFACE_CompatEditor     = 3,
    SURFACE_CorpusBrowser    = 4,
    SURFACE_SchemaDiff       = 5,
    SURFACE_ModalWizard      = 6,
    SURFACE_CommandPalette   = 7,
    SURFACE_FloatingHud      = 8,
    // Appended at END per feedback_struct_layout.md — older callers
    // that only knew 0..8 stay binary-compatible.
    SURFACE_ReplayViewer     = 9,
    SURFACE_GoldenRunner     = 10,
    SURFACE_Count            = 11
};
void OpenSurface(SurfaceKind k);
void CloseSurface(SurfaceKind k);
bool IsSurfaceOpen(SurfaceKind k);

} // namespace Vespucci

#endif // VESPUCCI_VESPUCCI_H_
