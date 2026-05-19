// Tooltip.h
// =============================================================================
// VESPUCCI TOOLTIP - DELAYED, RICH-CONTENT, KEYBOARD-FRIENDLY
// =============================================================================
// Written by: Eriumsss
//
// ImGui's BeginTooltip is fine for plain text. We need richer surface
// area: a header line, a body, optional reason chips, optional GUID
// + numeric details. This wraps BeginTooltip with the Vespucci theme
// and exposes a structured TooltipBuilder so call sites stay clean.
// =============================================================================

#ifndef VESPUCCI_UI_COMMON_TOOLTIP_H_
#define VESPUCCI_UI_COMMON_TOOLTIP_H_

#include "../../Core/VespucciTypes.h"
#include "../../Suggest/SuggestionTypes.h"

namespace Vespucci {
namespace UI {
namespace Common {

class TooltipBuilder {
public:
    TooltipBuilder();

    void SetHeader(const char* header);
    void AddBodyLine(const char* line);
    void AddKeyValue(const char* key, const char* value);
    void AddReasonChips(const Suggest::ReasonChip* chips, i32 count);
    void Render();

private:
    const char*  m_header;
    const char*  m_lines[8];
    i32          m_lineCount;
    const char*  m_kvKeys[8];
    const char*  m_kvVals[8];
    i32          m_kvCount;
    const Suggest::ReasonChip* m_chips;
    i32          m_chipCount;
};

} // namespace Common
} // namespace UI
} // namespace Vespucci

#endif // VESPUCCI_UI_COMMON_TOOLTIP_H_
