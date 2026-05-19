// Panel.h
// =============================================================================
// SCHEMA DIFF PANEL - SURFACE THE VERSION DELTA WHEN A CORPUS LOAD
// COMPLAINS ABOUT MISMATCHED SCHEMA VERSIONS, AND OFFER MIGRATIONS
// =============================================================================
// Written by: Eriumsss
//
// Doc-locked Phase L surface. When the user loads a corpus shard
// from disk that was written under an older Schema version, the
// reader logs a warning and refuses the load. This panel is the
// designer-facing 'what changed?' explainer + the 'apply migration'
// runner. Without this, mismatch errors send users to the
// SchemaVersion.cpp source code with a flashlight, which is a
// cocksucking failure of editor UX.
// =============================================================================

#ifndef VESPUCCI_UI_SCHEMADIFF_PANEL_H_
#define VESPUCCI_UI_SCHEMADIFF_PANEL_H_

#include "../../Core/VespucciTypes.h"
#include "../../Schema/SchemaVersion.h"

#include <string>

namespace Vespucci {
namespace UI {
namespace SchemaDiff {

struct PanelState {
    bool                     visible;
    Schema::SchemaVersionTriple recordedVersion;
    Schema::SchemaVersionTriple currentVersion;
    std::string              corpusPath;
    bool                     dryRun;
    std::string              migrationLog;
};

void InitPanelState(PanelState& s);
void RenderSchemaDiffPanel(PanelState& s);

} // namespace SchemaDiff
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_SCHEMADIFF_PANEL_H_
