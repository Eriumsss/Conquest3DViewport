// Apply.cpp
// =============================================================================
// Translate Suggestion into chainAdd*/chainDelete*/hostRequest* writes.
// Bonus: every helper has a venom-named guard that catches the dumbass
// inputs (null guid, blank event name, suggestion past confidence).
// =============================================================================
// Written by: Eriumsss

#include "Apply.h"

#include "../Core/Logging.h"
#include "imgui_glue.h"

#include <cstring>

namespace Vespucci {
namespace Apply {

namespace {
    bool RejectMalformedSuggestionAndCryAboutIt(const Suggest::Suggestion& s) {
        if (!s.targetGuid.valid()) {
            Core::Logging::Warn("Apply: suggestion has zero target guid, ignoring");
            return true;
        }
        if (s.eventName.size() == 0 || s.actionName.size() == 0) {
            Core::Logging::Warn("Apply: suggestion has empty event or action name, ignoring");
            return true;
        }
        if (s.faulted) {
            Core::Logging::Warn("Apply: suggestion is flagged faulted, refusing to commit");
            return true;
        }
        return false;
    }

    void CopyTextOrTruncateLikeABastard(char* dst, usize cap, const Core::StringRef& src) {
        usize n = src.size();
        if (n + 1 > cap) n = cap - 1;
        std::memcpy(dst, src.data(), n);
        dst[n] = 0;
    }
} // namespace

bool ApplySuggestionOrChokeOnGarbage(const Suggest::Suggestion& s,
                                       Guid sourceGuid,
                                       ImGuiGlueFrameArgs& args)
{
    if (RejectMalformedSuggestionAndCryAboutIt(s)) return false;
    if (!sourceGuid.valid()) {
        Core::Logging::Warn("Apply: source guid is zero, this is a fuckwit caller");
        return false;
    }

    args.chainAddConnectionRequested = 1;
    args.chainAddSourceGuid = sourceGuid.raw;
    args.chainAddTargetGuid = s.targetGuid.raw;
    CopyTextOrTruncateLikeABastard(args.chainAddOutputEventName,
        sizeof(args.chainAddOutputEventName), s.eventName);
    CopyTextOrTruncateLikeABastard(args.chainAddInputActionName,
        sizeof(args.chainAddInputActionName), s.actionName);
    args.chainAddDelay  = s.suggestedDelay;
    args.chainAddSticky = s.suggestedSticky ? 1 : 0;
    args.chainAddParameter[0] = 0;
    Core::Logging::Info("Apply: queued chainAdd 0x%08X -> 0x%08X (%.*s/%.*s)",
        (unsigned)sourceGuid.raw, (unsigned)s.targetGuid.raw,
        (int)s.eventName.size(),  s.eventName.data(),
        (int)s.actionName.size(), s.actionName.data());
    return true;
}

bool DeleteWireOrPissOff(Guid sourceGuid, Guid outputGuid, ImGuiGlueFrameArgs& args) {
    if (!sourceGuid.valid() || !outputGuid.valid()) {
        Core::Logging::Warn("Apply: DeleteWire with zero guid, fuckwit caller");
        return false;
    }
    args.chainDeleteConnectionRequested = 1;
    args.chainDeleteSourceGuid = sourceGuid.raw;
    args.chainDeleteOutputGuid = outputGuid.raw;
    return true;
}

bool DuplicateEntityOrEatShit(Guid sourceGuid, ImGuiGlueFrameArgs& args) {
    if (!sourceGuid.valid()) return false;
    args.hostRequestDuplicateEntityGuid = sourceGuid.raw;
    return true;
}

bool DeleteEntityOrChokeOnGhost(Guid entityGuid, ImGuiGlueFrameArgs& args) {
    if (!entityGuid.valid()) return false;
    args.hostRequestDeleteEntityGuid = entityGuid.raw;
    return true;
}

bool CreateFromTemplateOrFuckOff(Guid templateGuid, ImGuiGlueFrameArgs& args) {
    if (!templateGuid.valid()) return false;
    args.hostRequestCreateEntityFromTemplateGuid = templateGuid.raw;
    return true;
}

} // namespace Apply
} // namespace Vespucci
