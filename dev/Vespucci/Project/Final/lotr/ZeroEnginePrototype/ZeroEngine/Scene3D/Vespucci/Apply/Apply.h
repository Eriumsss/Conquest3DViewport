// Apply.h
// =============================================================================
// COMMIT SUGGESTIONS - WRITE THROUGH chainAdd*/chainDelete*/hostRequest*
// =============================================================================
// Written by: Eriumsss
//
// The Suggestion came out of the ranker; the user clicked Apply; now
// it has to LAND on disk. That means writing the right fields on the
// frame-args struct so the EXE side picks them up next frame and runs
// the existing chain-add / chain-delete / duplicate / delete / create
// pipelines. We do NOT bypass those - the doc and the existing
// Phase 10c/e Forge work both wired into them. We use them.
// =============================================================================

#ifndef VESPUCCI_APPLY_APPLY_H_
#define VESPUCCI_APPLY_APPLY_H_

#include "../Core/VespucciTypes.h"
#include "../Core/StringRef.h"
#include "../Suggest/SuggestionTypes.h"

struct ImGuiGlueFrameArgs;

namespace Vespucci {
namespace Apply {

// Fire one suggestion as a chain-add. Writes to args fields. Returns
// true if the request was placed; false if the suggestion is malformed.
bool ApplySuggestionOrChokeOnGarbage(const Suggest::Suggestion& s,
                                       Guid sourceGuid,
                                       ImGuiGlueFrameArgs& args);

// Fire a chain-DELETE for the wire identified by source + outputGuid.
bool DeleteWireOrPissOff(Guid sourceGuid, Guid outputGuid,
                          ImGuiGlueFrameArgs& args);

// Duplicate an existing entity through the host pipeline.
bool DuplicateEntityOrEatShit(Guid sourceGuid, ImGuiGlueFrameArgs& args);

// Delete an entity through the host pipeline.
bool DeleteEntityOrChokeOnGhost(Guid entityGuid, ImGuiGlueFrameArgs& args);

// Create a fresh entity from a template entity.
bool CreateFromTemplateOrFuckOff(Guid templateGuid, ImGuiGlueFrameArgs& args);

} // namespace Apply
} // namespace Vespucci

#endif // VESPUCCI_APPLY_APPLY_H_
