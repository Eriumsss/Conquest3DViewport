// Vespucci.cpp
// =============================================================================
// THE GODDAMN ORCHESTRATOR - WIRES EVERY VESPUCCI SUBSYSTEM TO THE FORGE
// =============================================================================
// Written by: Eriumsss
//
// 280+ files of code, fifteen subsystems, four ranker strategies,
// four spatial indexes, four prefix-index peers - all of them get
// initialized HERE, in this one cpp, in the right order. If you
// ever need to debug a startup-order issue, this is the file you
// step through line-by-line. Do not be the asshole who reorders
// these calls without understanding why they are where they are.
// =============================================================================

#include "Vespucci.h"

#include "Core/Logging.h"
#include "Core/PathUtils.h"
#include "Schema/ZETypeRegistry.h"
#include "Schema/EventActionSignatureDB.h"
#include "Compat/CompatibilityMatrix.h"
#include "Compat/RuleSet.h"
#include "Scene/SceneSnapshot.h"
#include "Scene/EntityIndex.h"
#include "Index/ISpatialIndex.h"
#include "Corpus/CorpusPriors.h"
#include "Corpus/LocalCorpusBuilder.h"
#include "Suggest/CandidateGenerator.h"
#include "Suggest/IRanker.h"
#include "Localization/StringTable.h"
#include "Telemetry/PerfScope.h"
#include "Telemetry/Counters.h"

#include "UI/SuggestedWires/Panel.h"
#include "UI/HealthFix/Panel.h"
#include "UI/DebugRanker/Overlay.h"
#include "UI/CompatEditor/Panel.h"
#include "UI/CorpusBrowser/Panel.h"
#include "UI/SchemaDiff/Panel.h"
#include "UI/ReplayViewer/Panel.h"
#include "UI/GoldenRunner/Panel.h"

#include "imgui_glue.h"

namespace Vespucci {

namespace Loc {
    void RegisterAllEnglishStrings();
    void RegisterAllTurkishStrings();
}

namespace Telemetry {
    void OpenTelemetrySink(const char* path);
    void CloseTelemetrySink();
}

// Suggest layer factory + global accessors live in their respective .cpps
// (RankerSelector.cpp, CandidateGenerator's bind interface). Forward-decl
// what the orchestrator needs to allocate the singletons.
namespace Suggest {
    class RankerSelector;
    RankerSelector* MakeSelector(const Corpus::CorpusPriors* priors,
                                  const Schema::EventActionSignatureDB* sigDB);
    void            DestroySelector(RankerSelector* s);
    void            SetGlobalSelector(RankerSelector* s);
    RankerSelector* GlobalSelector();
}

namespace Index {
    ISpatialIndex* MakeIndexForLevel(i32 entityCount, ISpatialIndex::Kind preferredOrNone);
    void           DestroyIndex(ISpatialIndex* idx);
}

namespace {
    // Global singletons we own across editor lifetime.
    Schema::ZETypeRegistry*         s_registry  = 0;
    Schema::EventActionSignatureDB* s_signatures = 0;
    Compat::CompatibilityMatrix*    s_matrix    = 0;
    Compat::RuleSet*                s_rules     = 0;
    Scene::SceneSnapshot*           s_snap      = 0;
    Scene::EntityIndexer*           s_entityIndexer = 0;
    Index::ISpatialIndex*           s_spatial   = 0;
    Corpus::CorpusPriors*           s_priors    = 0;
    Corpus::LocalCorpusBuilder*     s_corpusBuilder = 0;
    Suggest::CandidateGenerator*    s_candidateGen = 0;
    Suggest::RankerSelector*        s_rankerSelector = 0;

    // Last snapshot version we fed into the local corpus. When the
    // snapshot bumps, we re-ingest. Cheap (~5 ms over 5k wires on
    // Helm's Deep) so we eat the cost rather than try to delta-update.
    SnapshotVersion                 s_lastIngestVersion = SnapshotVersion(0);

    // Last snapshot version we rebuilt the entity index + spatial index
    // against. Same trick — only rebuild on real version moves, not
    // every frame.
    SnapshotVersion                 s_lastIndexerVersion = SnapshotVersion(0);
    i32                             s_lastIndexedEntityCount = 0;

    // UI panel state.
    UI::SuggestedWires::PanelState  s_sidebarState;
    UI::HealthFix::PanelState        s_healthState;
    UI::DebugRanker::OverlayState    s_brainState;
    UI::CompatEditor::PanelState     s_compatState;
    UI::CorpusBrowser::PanelState    s_corpusState;
    UI::SchemaDiff::PanelState       s_schemaState;
    UI::ReplayViewer::PanelState     s_replayState;
    UI::GoldenRunner::PanelState     s_goldenState;

    bool s_initialized = false;
    u64  s_frameNumber = 0;

    // Surface open/close flags. Sized to match SurfaceKind::SURFACE_Count.
    // Adding a new SurfaceKind enum requires bumping this size — but
    // OpenSurface/CloseSurface bound-check against SURFACE_Count so a
    // mismatch would silently no-op rather than buffer-overflow.
    bool s_surfaceOpen[SURFACE_Count] = {0};
}

bool Init() {
    if (s_initialized) return true;

    // FIRST act of business — light the file sink. If anything below
    // craters, we want a paper trail, not a black box. The trace log
    // sits in cwd next to the running editor; rotate at 1 MiB.
    Core::Logging::InstallFileSink("vespucci_init_trace.log", 1u << 20);
    Core::Logging::Info("Vespucci: sink hot, file lit — boot sequence engaged");

    // Localization — gotta do this before any subsystem reaches for Loc::T().
    Core::Logging::Info("Vespucci: stringing up the language packs (en + tr)");
    Loc::RegisterAllEnglishStrings();
    Core::Logging::Info("Vespucci: english pack mounted");
    Loc::RegisterAllTurkishStrings();
    Core::Logging::Info("Vespucci: turkish pack mounted");
    Loc::SetActiveLanguage(Loc::LANG_en);
    Core::Logging::Info("Vespucci: active language set — en");

    // Schema — the registry of every fucking entity type the brain understands.
    Core::Logging::Info("Vespucci: forging the type registry");
    s_registry = new Schema::ZETypeRegistry();
    Core::Logging::Info("Vespucci: registry allocated, calling init()");
    s_registry->init();
    Core::Logging::Info("Vespucci: registry seeded with builtins, count=%d", s_registry->count());
    Schema::SetGlobalRegistry(s_registry);
    Core::Logging::Info("Vespucci: global registry pointer published");

    Core::Logging::Info("Vespucci: strapping the signature DB");
    s_signatures = new Schema::EventActionSignatureDB();
    Core::Logging::Info("Vespucci: signature DB allocated, calling init(reg)");
    s_signatures->init(s_registry);
    Core::Logging::Info("Vespucci: signature DB lit");
    Schema::SetGlobalSignatureDB(s_signatures);
    Core::Logging::Info("Vespucci: global signature DB pointer published");

    // Compat — hard filter king of the pipeline.
    Core::Logging::Info("Vespucci: compat ruleset spinning up");
    s_rules  = new Compat::RuleSet();
    Core::Logging::Info("Vespucci: ruleset allocated");
    s_matrix = new Compat::CompatibilityMatrix();
    Core::Logging::Info("Vespucci: matrix allocated, binding registry+sigdb+rules");
    s_matrix->bind(s_registry, s_signatures, s_rules);
    Core::Logging::Info("Vespucci: matrix bound and cocked");
    Compat::SetGlobalMatrix(s_matrix);
    Core::Logging::Info("Vespucci: global matrix pointer published");

    // Corpus priors.
    Core::Logging::Info("Vespucci: priming the corpus priors");
    s_priors = new Corpus::CorpusPriors();
    Corpus::SetGlobalPriors(s_priors);
    Core::Logging::Info("Vespucci: corpus priors live and global");

    // Auto-load the global corpus blob if it exists in cwd. This is
    // the cold-start prior — Tools/CorpusBuilderMain.exe builds it
    // offline by walking every PAK; the editor consumes whatever
    // file it finds. No-op (with a debug log) if the file is missing
    // or corrupt — the local builder will populate from the loaded
    // level instead and the ranker still works, just less informed.
    if (Corpus::ReadCorpusOrEatShit(*s_priors, "forge_global_corpus.bin")) {
        Corpus::CorpusPriors::Stats st = s_priors->stats();
        Core::Logging::Info("Vespucci: cold-start corpus loaded from "
            "forge_global_corpus.bin (%d triples, %llu observations)",
            st.tripleEntries, (unsigned long long)st.totalObservations);
    } else {
        Core::Logging::Info("Vespucci: no forge_global_corpus.bin found "
            "in cwd — local builder will populate priors from the "
            "loaded level instead");
    }

    // Local corpus builder — chews on the loaded snapshot every time
    // its version changes and dumps observation counts into the priors.
    // Without this the FrequencyRanker has nothing real to score with.
    s_corpusBuilder = new Corpus::LocalCorpusBuilder();
    s_corpusBuilder->bind(s_priors);
    s_lastIngestVersion = SnapshotVersion(0);
    Core::Logging::Info("Vespucci: local corpus builder bound to priors, ready to ingest");

    // Entity indexer (rebuilt per snapshot version) + ranker selector
    // (created once, prebuilds all four strategies on first active()
    // call). Spatial index is allocated lazily in Update once we know
    // the entity count — IndexFactory picks Grid/KdTree/BVH by N.
    Core::Logging::Info("Vespucci: building entity indexer + ranker selector");
    s_entityIndexer  = new Scene::EntityIndexer();
    Scene::SetGlobalIndexer(s_entityIndexer);

    s_rankerSelector = Suggest::MakeSelector(s_priors, s_signatures);
    Suggest::SetGlobalSelector(s_rankerSelector);
    Core::Logging::Info("Vespucci: ranker selector live (default = Hybrid)");

    s_candidateGen = new Suggest::CandidateGenerator();
    // Spatial bound to NULL initially — it's the only piece that needs
    // entity count to allocate. Update() rebinds once the snapshot fills.
    s_candidateGen->bind(s_snap, s_entityIndexer, 0, s_matrix);
    Suggest::SetGlobalGenerator(s_candidateGen);
    Core::Logging::Info("Vespucci: candidate generator bound to snapshot+indexer+matrix");

    // Scene snapshot.
    Core::Logging::Info("Vespucci: mounting the scene snapshot");
    s_snap = new Scene::SceneSnapshot();
    Scene::SetGlobalSnapshot(s_snap);
    Core::Logging::Info("Vespucci: snapshot mounted, build pipeline armed");

    // UI panel states — six surfaces, one Init each.
    Core::Logging::Info("Vespucci: lacing UI panel states");
    UI::SuggestedWires::InitPanelState(s_sidebarState);
    Core::Logging::Info("Vespucci: SuggestedWires laced");
    UI::HealthFix::InitHealthFixPanel(s_healthState);
    Core::Logging::Info("Vespucci: HealthFix laced");
    UI::DebugRanker::InitOverlayState(s_brainState);
    UI::DebugRanker::SetGlobalBrainState(&s_brainState);
    Core::Logging::Info("Vespucci: DebugRanker (Brain) laced and published as global");
    UI::CompatEditor::InitPanelState(s_compatState);
    Core::Logging::Info("Vespucci: CompatEditor laced");
    UI::CorpusBrowser::InitPanelState(s_corpusState);
    Core::Logging::Info("Vespucci: CorpusBrowser laced");
    UI::SchemaDiff::InitPanelState(s_schemaState);
    UI::ReplayViewer::InitPanelState(s_replayState);
    UI::GoldenRunner::InitPanelState(s_goldenState);
    Core::Logging::Info("Vespucci: SchemaDiff + ReplayViewer + GoldenRunner laced — all eight panels online");

    // Telemetry sink — per-user file in the Vespucci folder.
    Core::Logging::Info("Vespucci: opening the telemetry sink");
    Telemetry::OpenTelemetrySink("vespucci_telemetry.jsonl");
    Core::Logging::Info("Vespucci: telemetry sink open, JSON lines flowing");

    s_initialized = true;
    Core::Logging::Info("Vespucci: Init complete — engines hot, ready for first frame");
    return true;
}

void Update(ImGuiGlueFrameArgs& args) {
    if (!s_initialized) return;
    Telemetry::PerfScope scope("Vespucci::Update");
    s_frameNumber++;

    // ── Editor-wide selection unification ───────────────────────────
    // The editor has FIVE different selection fields on the args
    // struct, each owned by a different page:
    //   levelBlockSelGuid       — Level Blocks (Outliner)
    //   selectedEditorObjGuid   — Editor Object panel + 3D viewport pick
    //   inspectorSelGuid        — Level Inspector panel
    //   propEntityGuid          — Property grid (derived but also read directly)
    //   requestSelectEditorObjGuid — request the EXE to mirror selection
    //
    // Designer's complaint: "select an entity anywhere, the other
    // pages don't follow." Root cause: each panel only writes ITS OWN
    // field on click, and each panel reads ITS OWN field for display.
    // So Outliner click sets levelBlockSelGuid, but Inspector reads
    // inspectorSelGuid which stays zero — Inspector shows nothing.
    //
    // The fix lives here, in the one function every frame guarantees
    // runs before any panel renders: pick whichever selection field
    // is non-zero, fan it out to ALL of them, resolve every index
    // counterpart from args.gameObjGuids[]. After this, every panel
    // sees identical selection state regardless of where the click
    // originated. Single source of truth.
    {
        u32 unified = 0;
        if (args.selectedEditorObjGuid != 0)         unified = args.selectedEditorObjGuid;
        else if (args.inspectorSelGuid != 0)         unified = args.inspectorSelGuid;
        else if (args.levelBlockSelGuid != 0)        unified = args.levelBlockSelGuid;
        else if ((u32)args.propEntityGuid != 0)      unified = (u32)args.propEntityGuid;

        if (unified != 0) {
            args.selectedEditorObjGuid = unified;
            args.inspectorSelGuid      = unified;
            args.levelBlockSelGuid     = unified;
            args.propEntityGuid        = (int)unified;

            // Resolve every index counterpart against the gameObjs array
            // so panels that key on `*Idx` instead of `*Guid` also see
            // the right entity. Note: there's no `propEntityIdx` field
            // on the args struct — the property grid resolves the
            // entity index itself by scanning gameObjGuids for
            // propEntityGuid in ZeroEngine3DViewport.cpp's selection
            // cascade. So we only sync the two `*Idx` fields that
            // exist on the struct.
            if (args.gameObjGuids && args.gameObjCount > 0) {
                for (i32 i = 0; i < args.gameObjCount; ++i) {
                    if (args.gameObjGuids[i] == unified) {
                        args.selectedEditorObjIdx = i;
                        args.inspectorSelectedIdx = i;
                        break;
                    }
                }
            }
        }
    }

    // ── Per-Vespucci-panel focus mirror ─────────────────────────────
    // Now that the host fields are unified, hand the GUID to every
    // Vespucci panel's `focusedEntityGuid` so their renders pivot
    // accordingly. SuggestedWires reads the host field directly
    // inside its own render path so we skip it here.
    Guid focusedNow((u32)args.selectedEditorObjGuid);
    s_brainState.focusedEntityGuid   = focusedNow;
    s_healthState.focusedEntityGuid  = focusedNow;
    s_compatState.focusedEntityGuid  = focusedNow;
    s_corpusState.focusedEntityGuid  = focusedNow;

    // First three frames get loud so we can see args wiring is sane.
    if (s_frameNumber <= 3) {
        Core::Logging::Info("Vespucci: Update frame=%llu, gameObjCount=%d, outputsTotal=%d",
            (unsigned long long)s_frameNumber,
            args.gameObjCount, args.gameObjOutputsTotal);
    }

    // ── Save-status audit trail ─────────────────────────────────────
    // Watch requestSavePak / savePakStatus and log every transition so
    // the user has a record of every save attempt — what was queued,
    // whether the Rust parser pipeline succeeded, where the output
    // PAK landed. Designer hits Save, opens vespucci_init_trace.log,
    // reads the result without having to find the terminal window.
    static int  s_lastSavePakStatus    = 0;
    static u64  s_lastLoggedSaveFrame  = 0;
    if (args.requestSavePak && s_lastLoggedSaveFrame != s_frameNumber) {
        s_lastLoggedSaveFrame = s_frameNumber;
        Core::Logging::Info("Vespucci: SAVE requested at frame %llu — pendingEntityCount=%d "
            "(host fills via WritePendingEntitiesJson + ze_field_edits.json + ze_deleted_guids.json)",
            (unsigned long long)s_frameNumber, args.pendingEntityCount);
    }
    if (args.savePakStatus != s_lastSavePakStatus) {
        const char* statusLabel =
            (args.savePakStatus == 0)  ? "idle" :
            (args.savePakStatus == 1)  ? "running (Python launching)" :
            (args.savePakStatus == 2)  ? "running (Python + Rust parser executing)" :
            (args.savePakStatus == 5)  ? "SUCCESS — _modified PAK on disk" :
            (args.savePakStatus == -1) ? "FAILED — see savePakMessage" :
                                          "(unknown status)";
        Core::Logging::Info("Vespucci: save status %d → %d (%s) — message: %s",
            s_lastSavePakStatus, args.savePakStatus, statusLabel,
            args.savePakMessage ? args.savePakMessage : "(no message)");
        s_lastSavePakStatus = args.savePakStatus;
    }

    if (s_snap) {
        if (s_frameNumber <= 3) Core::Logging::Info("Vespucci: snapshot.build() entering");
        s_snap->build(args);
        if (s_frameNumber <= 3) Core::Logging::Info("Vespucci: snapshot.build() returned clean");

        // Corpus ingest — feed the freshly-built snapshot into the
        // priors whenever the version moves. The builder clears and
        // rebuilds, so a hot level reload doesn't double-count. We
        // skip ingest when version is unchanged to keep the per-frame
        // budget honest. First ingest also gates on having a level
        // actually loaded (gameObjCount > 0) so we don't waste a
        // build on the empty-snapshot frame.
        if (s_corpusBuilder && s_priors &&
            args.gameObjCount > 0 &&
            s_snap->version() != s_lastIngestVersion)
        {
            i32 obs = s_corpusBuilder->buildFromSnapshot(*s_snap);
            s_lastIngestVersion = s_snap->version();
            if (s_frameNumber <= 3 || obs > 0) {
                Core::Logging::Info("Vespucci: corpus ingest — %d observations bumped at version %u",
                    obs, (unsigned)s_lastIngestVersion.raw);
            }
        }

        // Entity index rebuild + spatial index (re)allocation. EntityIndexer
        // is cheap to rebuild (a few unordered_map clears + a single linear
        // pass over the entity rows). Spatial index allocation depends on
        // entity count, so we tear it down + reallocate when the count
        // crosses the IndexFactory's tier thresholds (500 / 5000) — that
        // way Helm's Deep gets BVH and a small intro level gets Grid.
        if (s_entityIndexer && args.gameObjCount > 0 &&
            s_snap->version() != s_lastIndexerVersion)
        {
            s_entityIndexer->build(*s_snap);
            i32 nNow = s_snap->entityCount();
            bool tierChanged =
                (s_lastIndexedEntityCount < 500   && nNow >= 500) ||
                (s_lastIndexedEntityCount < 5000  && nNow >= 5000) ||
                (s_lastIndexedEntityCount >= 500  && nNow < 500) ||
                (s_lastIndexedEntityCount >= 5000 && nNow < 5000) ||
                s_spatial == 0;
            if (tierChanged) {
                if (s_spatial) {
                    Index::SetGlobalSpatial(0);
                    Index::DestroyIndex(s_spatial);
                    s_spatial = 0;
                }
                s_spatial = Index::MakeIndexForLevel(nNow, Index::ISpatialIndex::KIND_None);
                Index::SetGlobalSpatial(s_spatial);
                if (s_candidateGen) {
                    s_candidateGen->bind(s_snap, s_entityIndexer, s_spatial, s_matrix);
                }
            }
            if (s_spatial) s_spatial->build(*s_snap);
            s_lastIndexerVersion = s_snap->version();
            s_lastIndexedEntityCount = nNow;
            if (s_frameNumber <= 3) {
                Core::Logging::Info("Vespucci: indexer + spatial rebuilt for %d entities (spatial=%s)",
                    nNow, s_spatial ? s_spatial->name() : "(none)");
            }
        }
    } else if (s_frameNumber <= 3) {
        Core::Logging::Warn("Vespucci: snapshot pointer is NULL on frame %llu — that should be impossible",
            (unsigned long long)s_frameNumber);
    }
}

void DrawAllPanels(ImGuiGlueFrameArgs& args) {
    if (!s_initialized) return;
    Telemetry::PerfScope scope("Vespucci::DrawAllPanels");

    // Each panel: re-arm `visible = true` before rendering (so a
    // surface that was previously closed via the panel's X button can
    // be reopened by the menu toggle), then sync the opposite way after
    // — if the user X'd the panel during this frame, also flip the
    // surface flag closed so the menu reflects reality.
    if (s_surfaceOpen[SURFACE_SuggestedSidebar]) {
        s_sidebarState.visible = true;
        UI::SuggestedWires::RenderSidebarPanel(s_sidebarState, args);
        if (!s_sidebarState.visible) s_surfaceOpen[SURFACE_SuggestedSidebar] = false;
    }
    if (s_surfaceOpen[SURFACE_HealthFix]) {
        s_healthState.visible = true;
        UI::HealthFix::RenderHealthFixPanel(s_healthState, args);
        if (!s_healthState.visible) s_surfaceOpen[SURFACE_HealthFix] = false;
    }
    if (s_surfaceOpen[SURFACE_BrainDebug]) {
        s_brainState.visible = true;
        UI::DebugRanker::RenderBrainOverlay(s_brainState);
        if (!s_brainState.visible) s_surfaceOpen[SURFACE_BrainDebug] = false;
    }
    if (s_surfaceOpen[SURFACE_CompatEditor]) {
        s_compatState.visible = true;
        UI::CompatEditor::RenderCompatEditor(s_compatState);
        if (!s_compatState.visible) s_surfaceOpen[SURFACE_CompatEditor] = false;
    }
    if (s_surfaceOpen[SURFACE_CorpusBrowser]) {
        s_corpusState.visible = true;
        UI::CorpusBrowser::RenderCorpusBrowser(s_corpusState);
        if (!s_corpusState.visible) s_surfaceOpen[SURFACE_CorpusBrowser] = false;
    }
    if (s_surfaceOpen[SURFACE_SchemaDiff]) {
        s_schemaState.visible = true;
        UI::SchemaDiff::RenderSchemaDiffPanel(s_schemaState);
        if (!s_schemaState.visible) s_surfaceOpen[SURFACE_SchemaDiff] = false;
    }
    if (s_surfaceOpen[SURFACE_ReplayViewer]) {
        s_replayState.visible = true;
        UI::ReplayViewer::RenderReplayViewerPanel(s_replayState, args);
        if (!s_replayState.visible) s_surfaceOpen[SURFACE_ReplayViewer] = false;
    }
    if (s_surfaceOpen[SURFACE_GoldenRunner]) {
        s_goldenState.visible = true;
        UI::GoldenRunner::RenderGoldenRunnerPanel(s_goldenState, args);
        if (!s_goldenState.visible) s_surfaceOpen[SURFACE_GoldenRunner] = false;
    }
}

void Shutdown() {
    if (!s_initialized) return;
    Core::Logging::Info("Vespucci: Shutdown");
    Telemetry::CloseTelemetrySink();
    if (s_rankerSelector) {
        Suggest::SetGlobalSelector(0);
        Suggest::DestroySelector(s_rankerSelector);
        s_rankerSelector = 0;
    }
    delete s_candidateGen;     s_candidateGen     = 0;
    if (s_spatial) {
        Index::SetGlobalSpatial(0);
        Index::DestroyIndex(s_spatial);
        s_spatial = 0;
    }
    delete s_entityIndexer;    s_entityIndexer    = 0;
    delete s_snap;             s_snap             = 0;
    delete s_corpusBuilder;    s_corpusBuilder    = 0;
    delete s_priors;           s_priors           = 0;
    delete s_matrix;           s_matrix           = 0;
    delete s_rules;            s_rules            = 0;
    delete s_signatures;       s_signatures       = 0;
    delete s_registry;         s_registry         = 0;
    s_initialized = false;
}

void OpenSurface(SurfaceKind k) {
    if ((i32)k >= 0 && (i32)k < SURFACE_Count) s_surfaceOpen[(i32)k] = true;
}
void CloseSurface(SurfaceKind k) {
    if ((i32)k >= 0 && (i32)k < SURFACE_Count) s_surfaceOpen[(i32)k] = false;
}
bool IsSurfaceOpen(SurfaceKind k) {
    if ((i32)k >= 0 && (i32)k < SURFACE_Count) return s_surfaceOpen[(i32)k];
    return false;
}

} // namespace Vespucci
