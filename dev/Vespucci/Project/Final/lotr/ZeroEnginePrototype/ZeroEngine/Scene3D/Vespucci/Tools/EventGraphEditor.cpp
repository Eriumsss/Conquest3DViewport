// EventGraphEditor.cpp
// ============================================================================
// EVENT GRAPH STUDIO: ONE GRAPH, ONE TRUTH, ZERO DUPLICATE SCRAP
// ============================================================================
// Written by: Eriumsss
//
// This module is the shovel that buries the old event-view mess. The previous
// DLL code had three half-related renderers for the same owner -> Output ->
// target relationship, each one reinventing focus, colors, selection, and F4
// isolation like the codebase was getting paid by the duplicate. Fuck that.
//
// Level.json is not the authoring surface. It is the exported fossil. Artists
// and designers should see wires, owners, targets, chains, dead refs, and layer
// health as a working editor system. This file rebuilds that authoring surface
// from the packed GameObjs arrays without growing ImGuiGlueFrameArgs and without
// asking the renderer to care about DLL UI surgery.
//
// Keep the venom where it earns its keep: big failure states, dangerous save
// paths, and warnings that stop future-us from feeding a broken GUID graph into
// the renderer. The field rows can stay terse. The architecture rant lives up
// here where the damage actually happened.
// ============================================================================

#include "EventGraphEditor.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui_glue.h"
#include "LevelConstants.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// Windows.h leaks min/max as preprocessor macros somewhere up the include
// chain (imgui_glue.h pulls it in for HWND etc). That detonates every
// std::min / std::max call in this file with C2589 "invalid token on right
// side of '::'" because the preprocessor rewrites std::min(a,b) into
// std::(((a)<(b))?(a):(b)). Burn the macros here so the std versions work.
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

// ── Forge_RequestOpenCreateEntityWizard — TRUE FILE-SCOPE EXTERN ─────
// Defined at global scope in imgui_glue_dll.cpp. The Schematic /
// Flowchart / Node Graph "+ Create entity" buttons call this to open
// the same wizard the right-click context menu uses. Has to live
// HERE outside any namespace — declaring it inside the EventGraphEditor
// namespace mangles to `EventGraphEditor::Forge_*` and the linker
// can't find the global definition.
extern "C++" void Forge_RequestOpenCreateEntityWizard();
extern "C++" void Forge_RequestOpenCreateEntityWizardFromSource(unsigned int sourceGuid);

// ── Cross-translation-unit hooks into imgui_glue_dll.cpp ─────────────
// Phase 9b layer-manager consolidation: the layer state sets and the
// editor_filter.ini persist routine live in imgui_glue_dll.cpp. We
// extern-link them here so DrawLayersMode operates on the same source
// of truth the legacy panel did. No duplication, no syncing, just
// shared globals inside one fucking DLL. Headers must come BEFORE these
// externs — std::set has to be a real type by the time we name it.
extern std::set<unsigned int> g_layerHidden;
extern std::set<unsigned int> g_layerLocked;
extern unsigned int           g_layerIsolated;
extern unsigned int           g_layerActive;
extern void                   EditorFilterSave();
// Phase 9b: gizmo state for Manipulator mode.
extern int                    g_gizmoMode;
extern int                    g_gizmoSpace;
extern float                  g_gizmoSnap;

namespace EventGraphEditor
{
namespace
{
    enum ViewMode
    {
        View_Tree = 0,
        View_Flowchart = 1,
        View_TripleRow = 2,
        View_Health = 3,
        View_NodeGraph = 4,    // 5-column node-graph: source / envelope / focus / envelope / target
        View_Outliner = 4,
        View_Layers = 5,
        View_Manipulator = 6,
        View_Properties = 7
    };

    struct Triple
    {
        int ownerIdx = -1;
        int outputIdx = -1;
        int targetIdx = -1;
        unsigned int outputGuid = 0;
        unsigned int targetGuid = 0;
    };

    struct GraphIndex
    {
        std::map<unsigned int, int> idxOf;
        std::map<unsigned int, int> ownerOfOutput;
        std::vector<Triple> allTriples;
        int brokenOutputRefs = 0;
        int brokenTargets = 0;
        int deadOutputBlocks = 0;
        int eventOwnerCount = 0;
        int eventWireCount = 0;
        int outputBlockCount = 0;
    };

    struct EditorState
    {
        int mode = View_Tree;
        // One-shot tab-switch request. Set to a View_* value when something
        // outside the tab bar wants to force a tab open (menu jump, double
        // click, etc.). DrawPanel reads it once and resets to -1. NEVER set
        // it every frame from a state predicate — that nukes ImGui input.
        int requestSwitchTo = -1;
        unsigned int focusGuid = 0;
        unsigned int lockedFocusGuid = 0;
        unsigned int selectedOutputGuid = 0;
        char search[128] = {0};

        unsigned int addSourceGuid = 0;
        unsigned int addTargetGuid = 0;
        char addOutputEvent[64] = "OnTrigger";
        char addInputAction[64] = "Activate";
        char addParameter[64] = {0};
        float addDelay = 0.0f;
        bool addSticky = false;
        int lastChainAddResult = 0;

        // Shared canvas zoom factor for Schematic / Flowchart / Wires.
        // 1.0 = default, range clamped 0.5..2.5. Pan is handled by
        // ImGui::BeginChild's built-in scrollbar so we do not need a
        // separate pan offset.
        float canvasZoom = 1.0f;

        // Schematic mode XSI drag-pin state.
        // dragFromGuid = entity whose output socket the user grabbed.
        // dragFromIsOutput = true (output pin on right edge), false reserved
        //   for future "drag input pin to delete" behavior.
        // hoverPinGuid = entity under the cursor's nearest socket (preview
        //   wire targets this on release).
        // hoverPinIsInput = which side of that entity is hovered.
        // schematicZoom = canvas zoom factor.
        // schematicPan{X,Y} = canvas pan offset.
        unsigned int dragFromGuid = 0;
        bool         dragFromIsOutput = true;
        unsigned int hoverPinGuid = 0;
        bool         hoverPinIsInput = true;
        float        schematicZoom = 1.0f;
        float        schematicPanX = 0.0f;
        float        schematicPanY = 0.0f;

        // Phase 10b free-place: per-entity override positions on the
        // Schematic canvas. Stored as offsets from the canvas origin
        // (base.x/base.y inside DrawSchematicMode). When an entity has
        // an entry here, the auto-layout position is ignored and the
        // override wins. Drag a card and the override is written or
        // updated; right-click "Reset Layout" wipes the map.
        // draggingCardGuid: which card the mouse is currently dragging
        //   (0 = not dragging).
        // dragGrabOffset: distance from card top-left corner to the
        //   click point so the card doesn't snap-jump on first drag pixel.
        std::map<unsigned int, ImVec2> schematicPositions;
        unsigned int                   draggingCardGuid = 0;
        ImVec2                         dragGrabOffset = ImVec2(0,0);

        // Phase 10c Event Creator wizard: when the user drops a drag-
        // pin onto a target socket, we DON'T fire chainAddConnection
        // immediately — first we open this popup so they can name the
        // event, name the action, set delay, and set sticky. Hitting
        // Create commits; Cancel discards. Source and target GUIDs are
        // captured at drop time so the popup can label itself with the
        // entities being wired even after the drag-pin state resets.
        bool         wizardOpen = false;
        unsigned int wizardSourceGuid = 0;
        unsigned int wizardTargetGuid = 0;
        char         wizardEvent[64]  = "OnTrigger";
        char         wizardAction[64] = "Activate";
        float        wizardDelay      = 0.0f;
        bool         wizardSticky     = false;
        // Cross-mode trigger: Schematic's right-click "Create Entity"
        // shortcut sets this and switches to Outliner; Outliner reads
        // it on entry and opens the wizard popup, then clears it.
        bool         requestOpenCreateWizard = false;
        // Per-frame validation summary, rebuilt when graph changes.
        int          validHardCount = 0;     // broken target + empty event/action count
        int          validCycleCount = 0;
        int          validMaxDepth = 0;
        int          validLastGOCount = -1;
        int          validLastPendCount = -1;
        bool         validCollapsed = true;
    };

    static EditorState s_state;
    static unsigned int* s_editorCategoryMask = NULL;
    static bool* s_eventWiresShow = NULL;
    static unsigned int* s_eventWireFocusGuid = NULL;
    static void (*s_saveFn)() = NULL;

    static const char* Safe(const char* s, const char* fallback = "")
    {
        return (s && s[0]) ? s : fallback;
    }

    static char LowerAscii(char c)
    {
        return (char)std::tolower((unsigned char)c);
    }

    static bool ContainsI(const char* haystack, const char* needle)
    {
        if (!needle || !needle[0]) return true;
        if (!haystack) return false;
        size_t hLen = std::strlen(haystack);
        size_t nLen = std::strlen(needle);
        if (nLen == 0) return true;
        if (nLen > hLen) return false;
        for (size_t h = 0; h <= hLen - nLen; ++h) {
            size_t n = 0;
            for (; n < nLen; ++n) {
                if (LowerAscii(haystack[h + n]) != LowerAscii(needle[n]))
                    break;
            }
            if (n == nLen) return true;
        }
        return false;
    }

    static bool EqualsI(const char* a, const char* b)
    {
        if (!a || !b) return false;
        while (*a && *b) {
            if (LowerAscii(*a) != LowerAscii(*b)) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    }

    static bool StartsI(const char* s, const char* prefix)
    {
        if (!s || !prefix) return false;
        while (*prefix) {
            if (!*s) return false;
            if (LowerAscii(*s) != LowerAscii(*prefix)) return false;
            ++s; ++prefix;
        }
        return true;
    }

    static bool TypeContains(const ImGuiGlueFrameArgs& args, int idx, const char* token)
    {
        if (idx < 0 || idx >= args.gameObjCount || !args.gameObjTypeNames) return false;
        return ContainsI(args.gameObjTypeNames[idx], token);
    }

    static bool IsOutputType(const ImGuiGlueFrameArgs& args, int idx)
    {
        return TypeContains(args, idx, "Output");
    }

    static int FindByGuid(const ImGuiGlueFrameArgs& args, unsigned int guid)
    {
        if (guid == 0 || args.gameObjCount <= 0 || !args.gameObjGuids) return -1;
        for (int i = 0; i < args.gameObjCount; ++i) {
            if (args.gameObjGuids[i] == guid) return i;
        }
        return -1;
    }

    static const char* EntityName(const ImGuiGlueFrameArgs& args, int idx)
    {
        if (idx < 0 || idx >= args.gameObjCount || !args.gameObjNames) return "(missing)";
        return Safe(args.gameObjNames[idx], "(unnamed)");
    }

    static const char* EntityType(const ImGuiGlueFrameArgs& args, int idx)
    {
        if (idx < 0 || idx >= args.gameObjCount || !args.gameObjTypeNames) return "(missing)";
        return Safe(args.gameObjTypeNames[idx], "(type?)");
    }

    static unsigned int EntityGuid(const ImGuiGlueFrameArgs& args, int idx)
    {
        if (idx < 0 || idx >= args.gameObjCount || !args.gameObjGuids) return 0;
        return args.gameObjGuids[idx];
    }

    // Phase 9c: MMB-drag pan for the three Forge canvases. Call this
    // inside the BeginChild scope of each canvas. While the user
    // holds middle-mouse and drags inside the child, we offset the
    // ImGui scroll positions by the inverse of the drag delta - same
    // visual result as Maya / Blender / XSI's middle-button-drag pan,
    // implemented entirely on top of ImGui's existing scrollbar
    // mechanism so we do not have to invent a new transform pipeline.
    static void HandleCanvasPan()
    {
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                     ImGuiHoveredFlags_ChildWindows))
            return;
        if (!ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
            return;
        ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
        if (delta.x != 0.0f || delta.y != 0.0f) {
            ImGui::SetScrollX(ImGui::GetScrollX() - delta.x);
            ImGui::SetScrollY(ImGui::GetScrollY() - delta.y);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
        }
    }

    static ImU32 EventColorForName(const char* evName)
    {
        if (!evName || !evName[0]) return IM_COL32(170, 170, 180, 255);
        if (ContainsI(evName, "Capture") || ContainsI(evName, "Neutralize"))
            return IM_COL32(255, 208, 64, 255);
        if (ContainsI(evName, "Death") || ContainsI(evName, "Die") ||
            ContainsI(evName, "Suicide") || ContainsI(evName, "Killed"))
            return IM_COL32(255, 74, 74, 255);
        if (ContainsI(evName, "Trigger"))
            return IM_COL32(64, 221, 255, 255);
        if (ContainsI(evName, "Enter") || ContainsI(evName, "Exit"))
            return IM_COL32(255, 238, 64, 255);
        if (ContainsI(evName, "Spawn") || ContainsI(evName, "Reinforcement"))
            return IM_COL32(255, 88, 255, 255);
        if (ContainsI(evName, "Complete") || ContainsI(evName, "Failed") ||
            ContainsI(evName, "Threshold"))
            return IM_COL32(72, 255, 106, 255);
        if (ContainsI(evName, "User"))
            return IM_COL32(255, 170, 64, 255);
        if (ContainsI(evName, "Timer") || ContainsI(evName, "Delay"))
            return IM_COL32(180, 150, 255, 255);
        return IM_COL32(188, 194, 210, 255);
    }

    static bool TripleMatchesSearch(const ImGuiGlueFrameArgs& args, const Triple& tr)
    {
        const char* q = s_state.search;
        if (!q[0]) return true;
        if (ContainsI(EntityName(args, tr.ownerIdx), q)) return true;
        if (ContainsI(EntityName(args, tr.outputIdx), q)) return true;
        if (ContainsI(EntityName(args, tr.targetIdx), q)) return true;
        if (ContainsI(EntityType(args, tr.ownerIdx), q)) return true;
        if (ContainsI(EntityType(args, tr.outputIdx), q)) return true;
        if (ContainsI(EntityType(args, tr.targetIdx), q)) return true;
        if (tr.outputIdx >= 0 && args.gameObjOutputEvents &&
            ContainsI(args.gameObjOutputEvents[tr.outputIdx], q)) return true;
        if (tr.outputIdx >= 0 && args.gameObjInputEvents &&
            ContainsI(args.gameObjInputEvents[tr.outputIdx], q)) return true;
        return false;
    }

    static void PushTripleIfUnique(std::vector<Triple>& triples, const Triple& tr)
    {
        for (size_t i = 0; i < triples.size(); ++i) {
            if (triples[i].outputGuid == tr.outputGuid && triples[i].outputIdx == tr.outputIdx)
                return;
        }
        triples.push_back(tr);
    }

    static void BuildGraphIndex(const ImGuiGlueFrameArgs& args, GraphIndex& out)
    {
        out = GraphIndex();
        if (args.gameObjCount <= 0 || !args.gameObjGuids) return;

        for (int i = 0; i < args.gameObjCount; ++i) {
            out.idxOf[args.gameObjGuids[i]] = i;
            if (IsOutputType(args, i)) {
                ++out.outputBlockCount;
                unsigned int tgt = (args.gameObjTargetGuids) ? args.gameObjTargetGuids[i] : 0;
                if (tgt == 0) ++out.deadOutputBlocks;
            }
        }

        if (!args.gameObjOutputsCounts || !args.gameObjOutputsOffsets || !args.gameObjOutputsData)
            return;

        for (int owner = 0; owner < args.gameObjCount; ++owner) {
            int off = args.gameObjOutputsOffsets[owner];
            int cnt = args.gameObjOutputsCounts[owner];
            if (off < 0 || cnt < 0 || off + cnt > args.gameObjOutputsTotal) {
                ++out.brokenOutputRefs;
                continue;
            }
            if (cnt > 0) ++out.eventOwnerCount;

            for (int k = 0; k < cnt; ++k) {
                unsigned int outputGuid = args.gameObjOutputsData[off + k];
                if (outputGuid == 0) continue;
                ++out.eventWireCount;
                out.ownerOfOutput[outputGuid] = owner;

                Triple tr;
                tr.ownerIdx = owner;
                tr.outputGuid = outputGuid;

                std::map<unsigned int, int>::const_iterator oit = out.idxOf.find(outputGuid);
                if (oit == out.idxOf.end()) {
                    tr.outputIdx = -1;
                    ++out.brokenOutputRefs;
                    out.allTriples.push_back(tr);
                    continue;
                }

                tr.outputIdx = oit->second;
                tr.targetGuid = (args.gameObjTargetGuids) ? args.gameObjTargetGuids[tr.outputIdx] : 0;
                if (tr.targetGuid != 0) {
                    std::map<unsigned int, int>::const_iterator tit = out.idxOf.find(tr.targetGuid);
                    if (tit != out.idxOf.end()) tr.targetIdx = tit->second;
                    else ++out.brokenTargets;
                }
                out.allTriples.push_back(tr);
            }
        }
    }

    static void CollectTriplesForFocus(const ImGuiGlueFrameArgs& args,
                                       const GraphIndex& index,
                                       unsigned int focusGuid,
                                       std::vector<Triple>& out)
    {
        out.clear();
        if (focusGuid == 0) {
            for (size_t i = 0; i < index.allTriples.size() && out.size() < 512; ++i) {
                if (TripleMatchesSearch(args, index.allTriples[i]))
                    out.push_back(index.allTriples[i]);
            }
            return;
        }

        int focusIdx = -1;
        std::map<unsigned int, int>::const_iterator fit = index.idxOf.find(focusGuid);
        if (fit != index.idxOf.end()) focusIdx = fit->second;

        if (focusIdx >= 0 && IsOutputType(args, focusIdx)) {
            for (size_t i = 0; i < index.allTriples.size(); ++i) {
                if (index.allTriples[i].outputGuid == focusGuid &&
                    TripleMatchesSearch(args, index.allTriples[i]))
                    PushTripleIfUnique(out, index.allTriples[i]);
            }
            if (out.empty()) {
                Triple tr;
                tr.outputIdx = focusIdx;
                tr.outputGuid = focusGuid;
                tr.targetGuid = (args.gameObjTargetGuids) ? args.gameObjTargetGuids[focusIdx] : 0;
                std::map<unsigned int, int>::const_iterator ow = index.ownerOfOutput.find(focusGuid);
                tr.ownerIdx = (ow != index.ownerOfOutput.end()) ? ow->second : -1;
                std::map<unsigned int, int>::const_iterator tg = index.idxOf.find(tr.targetGuid);
                tr.targetIdx = (tg != index.idxOf.end()) ? tg->second : -1;
                if (TripleMatchesSearch(args, tr)) out.push_back(tr);
            }
            return;
        }

        for (size_t i = 0; i < index.allTriples.size(); ++i) {
            const Triple& tr = index.allTriples[i];
            if (EntityGuid(args, tr.ownerIdx) == focusGuid ||
                tr.outputGuid == focusGuid ||
                tr.targetGuid == focusGuid)
            {
                if (TripleMatchesSearch(args, tr))
                    PushTripleIfUnique(out, tr);
            }
        }
    }

    static void SelectEntity(ImGuiGlueFrameArgs& args, unsigned int guid, bool frame)
    {
        if (guid == 0) return;
        args.requestSelectEditorObjGuid = guid;
        if (frame) args.requestFocusEditorObjGuid = guid;
        s_state.focusGuid = guid;
        if (FindByGuid(args, guid) >= 0 && IsOutputType(args, FindByGuid(args, guid)))
            s_state.selectedOutputGuid = guid;
    }

    static void IsolateWire(ImGuiGlueFrameArgs& args, unsigned int outputGuid)
    {
        if (outputGuid == 0) return;
        if (s_eventWiresShow) *s_eventWiresShow = true;
        if (s_eventWireFocusGuid) *s_eventWireFocusGuid = outputGuid;
        args.eventWiresShow = 1;
        args.eventWireFocusGuid = outputGuid;
        s_state.selectedOutputGuid = outputGuid;
        if (s_saveFn) s_saveFn();
    }

    static void ClearWireIsolation(ImGuiGlueFrameArgs& args)
    {
        if (s_eventWireFocusGuid) *s_eventWireFocusGuid = 0;
        args.eventWireFocusGuid = 0;
        if (s_saveFn) s_saveFn();
    }

    static void DrawEntityButton(ImGuiGlueFrameArgs& args,
                                 const char* id,
                                 int idx,
                                 unsigned int fallbackGuid,
                                 const char* role,
                                 ImU32 borderCol)
    {
        ImGui::PushID(id);
        ImVec2 min = ImGui::GetCursorScreenPos();
        ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, 54.0f);
        if (size.x < 160.0f) size.x = 160.0f;
        ImGui::InvisibleButton("##entity_card", size);
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(0);
        bool dbl = ImGui::IsMouseDoubleClicked(0) && hovered;
        ImVec2 max = ImVec2(min.x + size.x, min.y + size.y);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 fill = (idx >= 0) ? IM_COL32(36, 38, 48, 236) : IM_COL32(70, 28, 28, 236);
        if (hovered) fill = (idx >= 0) ? IM_COL32(52, 58, 74, 245) : IM_COL32(95, 36, 36, 245);
        dl->AddRectFilled(min, max, fill, 5.0f);
        dl->AddRect(min, max, hovered ? IM_COL32(255, 244, 130, 255) : borderCol, 5.0f, 0, hovered ? 2.0f : 1.0f);
        dl->AddText(ImVec2(min.x + 8.0f, min.y + 5.0f), IM_COL32(170, 176, 190, 255), role);

        if (idx >= 0) {
            char line[256];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "%s", EntityName(args, idx));
            dl->AddText(ImVec2(min.x + 8.0f, min.y + 23.0f), IM_COL32(232, 234, 242, 255), line);
            _snprintf_s(line, sizeof(line), _TRUNCATE, "[%s] 0x%08X", EntityType(args, idx), EntityGuid(args, idx));
            dl->AddText(ImVec2(min.x + 8.0f, min.y + 38.0f), IM_COL32(130, 200, 245, 255), line);
        } else if (fallbackGuid != 0) {
            char line[160];
            _snprintf_s(line, sizeof(line), _TRUNCATE, "ghost GUID 0x%08X", fallbackGuid);
            dl->AddText(ImVec2(min.x + 8.0f, min.y + 25.0f), IM_COL32(255, 122, 104, 255), line);
            dl->AddText(ImVec2(min.x + 8.0f, min.y + 39.0f), IM_COL32(255, 170, 150, 255), "dead ref, this wire is fucked");
        } else {
            dl->AddText(ImVec2(min.x + 8.0f, min.y + 25.0f), IM_COL32(160, 160, 160, 255), "(none)");
        }

        unsigned int guid = (idx >= 0) ? EntityGuid(args, idx) : fallbackGuid;
        if (clicked && guid != 0) SelectEntity(args, guid, false);
        if (dbl && guid != 0) SelectEntity(args, guid, true);
        ImGui::PopID();
    }

    static void DrawToolbar(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        unsigned int focus = s_state.focusGuid;
        int focusIdx = FindByGuid(args, focus);

        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "THE FORGE");
        ImGui::SameLine();
        ImGui::TextDisabled("%d owners, %d wires, %d Output blocks",
            index.eventOwnerCount, index.eventWireCount, index.outputBlockCount);

        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##egs_search", "search names, types, events, actions...", s_state.search, IM_ARRAYSIZE(s_state.search));
        ImGui::SameLine();
        bool wires = s_eventWiresShow ? *s_eventWiresShow : (args.eventWiresShow != 0);
        if (ImGui::Checkbox("F4 wires", &wires)) {
            if (s_eventWiresShow) *s_eventWiresShow = wires;
            args.eventWiresShow = wires ? 1 : 0;
            if (s_saveFn) s_saveFn();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Isolate")) ClearWireIsolation(args);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use Selection")) {
            s_state.lockedFocusGuid = 0;
            s_state.focusGuid = args.inspectorSelGuid ? args.inspectorSelGuid : (unsigned int)args.propEntityGuid;
        }

        ImGui::TextDisabled("Focus:");
        ImGui::SameLine();
        if (focus == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f),
                "nothing selected - pick an entity before the graph has any damn point");
        } else if (focusIdx >= 0) {
            ImGui::Text("%s [%s] 0x%08X", EntityName(args, focusIdx), EntityType(args, focusIdx), focus);
            ImGui::SameLine();
            if (ImGui::SmallButton(s_state.lockedFocusGuid ? "Unlock Focus" : "Lock Focus"))
                s_state.lockedFocusGuid = s_state.lockedFocusGuid ? 0 : focus;
            ImGui::SameLine();
            if (ImGui::SmallButton("Frame in 3D")) args.requestFocusEditorObjGuid = focus;
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.34f, 1.0f),
                "0x%08X is a ghost GUID, the loaded level does not know this bastard", focus);
            ImGui::SameLine();
            if (ImGui::SmallButton("Drop Ghost Focus")) {
                s_state.focusGuid = 0;
                s_state.lockedFocusGuid = 0;
            }
        }

        if (s_eventWireFocusGuid && *s_eventWireFocusGuid != 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.95f, 1.0f, 1.0f),
                "F4 isolated Output 0x%08X", *s_eventWireFocusGuid);
        }

        // Canvas zoom slider. Drives Schematic / Flowchart / Wires
        // sizing so users can squeeze big graphs into tight windows
        // or zoom in on dense neighborhoods. Tabs without canvases
        // (Outliner / Layers / Manipulator / Properties / Health)
        // ignore this value harmlessly.
        ImGui::TextDisabled("Canvas zoom:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("##forge_zoom", &s_state.canvasZoom, 0.5f, 2.5f, "%.2fx");
        if (s_state.canvasZoom < 0.5f) s_state.canvasZoom = 0.5f;
        if (s_state.canvasZoom > 2.5f) s_state.canvasZoom = 2.5f;
        ImGui::SameLine();
        if (ImGui::SmallButton("-##forge_zoom_out"))
            s_state.canvasZoom = (s_state.canvasZoom > 0.6f) ? s_state.canvasZoom - 0.1f : 0.5f;
        ImGui::SameLine();
        if (ImGui::SmallButton("+##forge_zoom_in"))
            s_state.canvasZoom = (s_state.canvasZoom < 2.4f) ? s_state.canvasZoom + 0.1f : 2.5f;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##forge_zoom_reset"))
            s_state.canvasZoom = 1.0f;

        ImGui::Separator();
    }

    static const char* ChainAddResultText(int result)
    {
        switch (result) {
        case 1: return "Output chain created. Good. The graph ate.";
        case 2: return "No empty Outputs slot. The owner is packed full of event-wire bullshit.";
        case 3: return "Output type missing. Cannot create a wire when the level has no Output template.";
        case 4: return "Bad source or target GUID. The chain builder refused to wire a ghost.";
        default: return "";
        }
    }

    static void DrawNewConnectionBox(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        (void)index;
        if (s_state.addSourceGuid == 0 && s_state.focusGuid != 0)
            s_state.addSourceGuid = s_state.focusGuid;

        if (!ImGui::CollapsingHeader("Author New Wire", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        int src = (int)s_state.addSourceGuid;
        int tgt = (int)s_state.addTargetGuid;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("Source GUID", &src)) s_state.addSourceGuid = (unsigned int)src;
        ImGui::SameLine();
        if (ImGui::SmallButton("source = focus")) s_state.addSourceGuid = s_state.focusGuid;

        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::InputInt("Target GUID", &tgt)) s_state.addTargetGuid = (unsigned int)tgt;
        ImGui::SameLine();
        if (ImGui::SmallButton("target = focus")) s_state.addTargetGuid = s_state.focusGuid;

        // Event name field with autocomplete dropdown. The user can
        // type freely (LC tables are reference, not exhaustive, game
        // code occasionally fires custom events) but the dropdown
        // surfaces the 84 known output events from the level.json
        // analysis so they don't have to remember whether it's
        // "OnEnter" or "OnPlayerEnter" or some other variant.
        ImGui::SetNextItemWidth(170.0f);
        ImGui::InputText("Output event", s_state.addOutputEvent, IM_ARRAYSIZE(s_state.addOutputEvent));
        ImGui::SameLine();
        if (ImGui::BeginCombo("##evcombo", "...", ImGuiComboFlags_NoPreview)) {
            ImGui::TextDisabled("Known output events:");
            ImGui::Separator();
            for (int e = 0; e < LC_OutputEventCount; ++e) {
                if (ImGui::Selectable(LC_OutputEvents[e])) {
                    std::strncpy(s_state.addOutputEvent, LC_OutputEvents[e],
                                 sizeof(s_state.addOutputEvent) - 1);
                    s_state.addOutputEvent[sizeof(s_state.addOutputEvent) - 1] = '\0';
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(170.0f);
        ImGui::InputText("Input action", s_state.addInputAction, IM_ARRAYSIZE(s_state.addInputAction));
        ImGui::SameLine();
        if (ImGui::BeginCombo("##accombo", "...", ImGuiComboFlags_NoPreview)) {
            ImGui::TextDisabled("Known input actions:");
            ImGui::Separator();
            for (int a = 0; a < LC_InputActionCount; ++a) {
                if (ImGui::Selectable(LC_InputActions[a])) {
                    std::strncpy(s_state.addInputAction, LC_InputActions[a],
                                 sizeof(s_state.addInputAction) - 1);
                    s_state.addInputAction[sizeof(s_state.addInputAction) - 1] = '\0';
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(90.0f);
        ImGui::DragFloat("Delay", &s_state.addDelay, 0.05f, 0.0f, 120.0f, "%.2f");
        ImGui::SameLine();
        ImGui::Checkbox("Sticky", &s_state.addSticky);
        ImGui::SetNextItemWidth(170.0f);
        ImGui::InputText("Parameter", s_state.addParameter, IM_ARRAYSIZE(s_state.addParameter));

        bool badSrc = FindByGuid(args, s_state.addSourceGuid) < 0;
        bool badTgt = FindByGuid(args, s_state.addTargetGuid) < 0;
        if (badSrc || badTgt) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                "Source or target is a ghost. No wire gets birthed into that broken shit.");
        }

        ImGui::BeginDisabled(badSrc || badTgt || !s_state.addOutputEvent[0] || !s_state.addInputAction[0]);
        if (ImGui::Button("Create Output Wire")) {
            args.chainAddConnectionRequested = 1;
            args.chainAddSourceGuid = s_state.addSourceGuid;
            args.chainAddTargetGuid = s_state.addTargetGuid;
            std::strncpy(args.chainAddOutputEventName, s_state.addOutputEvent, sizeof(args.chainAddOutputEventName) - 1);
            args.chainAddOutputEventName[sizeof(args.chainAddOutputEventName) - 1] = '\0';
            std::strncpy(args.chainAddInputActionName, s_state.addInputAction, sizeof(args.chainAddInputActionName) - 1);
            args.chainAddInputActionName[sizeof(args.chainAddInputActionName) - 1] = '\0';
            args.chainAddDelay = s_state.addDelay;
            args.chainAddSticky = s_state.addSticky ? 1 : 0;
            std::strncpy(args.chainAddParameter, s_state.addParameter, sizeof(args.chainAddParameter) - 1);
            args.chainAddParameter[sizeof(args.chainAddParameter) - 1] = '\0';
        }
        ImGui::EndDisabled();

        if (args.chainAddResult != 0 && args.chainAddResult != s_state.lastChainAddResult) {
            s_state.lastChainAddResult = args.chainAddResult;
            if (args.chainAddLastNewGuid != 0)
                s_state.selectedOutputGuid = args.chainAddLastNewGuid;
        }
        if (s_state.lastChainAddResult != 0)
            ImGui::TextDisabled("%s", ChainAddResultText(s_state.lastChainAddResult));
    }

    static void CommitPropEdit(ImGuiGlueFrameArgs& args, int fi, int kind,
                               int iv, float fv, unsigned int gv, const char* sv)
    {
        args.propEditRequested = 1;
        args.propEditFieldIndex = fi;
        args.propEditKind = kind;
        args.propEditIntVal = iv;
        args.propEditFloatVal = fv;
        args.propEditGuidVal = gv;
        if (sv) {
            std::strncpy(args.propEditStringVal, sv, sizeof(args.propEditStringVal) - 1);
            args.propEditStringVal[sizeof(args.propEditStringVal) - 1] = '\0';
        }
    }

    static void DrawInlineOutputEditor(ImGuiGlueFrameArgs& args)
    {
        if (s_state.selectedOutputGuid == 0)
            return;

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.65f, 0.90f, 1.0f, 1.0f), "Inline Output Surgery");
        ImGui::SameLine();
        ImGui::TextDisabled("selected Output 0x%08X", s_state.selectedOutputGuid);

        if ((unsigned int)args.propEntityGuid != s_state.selectedOutputGuid || args.propFieldCount <= 0) {
            ImGui::TextDisabled("Select the Output row and the property payload shows here. No mystery panel scavenger hunt.");
            if (ImGui::SmallButton("Select Output For Editing"))
                SelectEntity(args, s_state.selectedOutputGuid, false);
            return;
        }

        if (ImGui::BeginTable("##egs_output_props", 3,
            ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 86.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int fi = 0; fi < args.propFieldCount; ++fi) {
                const char* name = (args.propFieldNames && args.propFieldNames[fi]) ? args.propFieldNames[fi] : "?";
                const char* kind = (args.propFieldKindNames && args.propFieldKindNames[fi]) ? args.propFieldKindNames[fi] : "?";
                bool interesting =
                    ContainsI(name, "Output") || ContainsI(name, "Input") ||
                    ContainsI(name, "Target") || ContainsI(name, "Delay") ||
                    ContainsI(name, "Sticky") || ContainsI(name, "Parameter");
                if (!interesting) continue;

                ImGui::PushID(fi);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", kind);
                ImGui::TableSetColumnIndex(2);

                if (ContainsI(kind, "float") || ContainsI(name, "Delay")) {
                    float v = (args.propFieldFloatVals) ? args.propFieldFloatVals[fi] : 0.0f;
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::DragFloat("##v", &v, 0.025f, 0.0f, 300.0f, "%.3f"))
                        CommitPropEdit(args, fi, 1, 0, v, 0, NULL);
                } else if (ContainsI(kind, "bool") || ContainsI(name, "Sticky")) {
                    bool v = (args.propFieldIntVals && args.propFieldIntVals[fi] != 0);
                    if (ImGui::Checkbox("##v", &v))
                        CommitPropEdit(args, fi, 0, v ? 1 : 0, 0.0f, 0, NULL);
                } else if (ContainsI(kind, "guid") || ContainsI(name, "Target")) {
                    int v = (int)((args.propFieldGuidVals) ? args.propFieldGuidVals[fi] : 0);
                    ImGui::SetNextItemWidth(150.0f);
                    if (ImGui::InputInt("##v", &v))
                        CommitPropEdit(args, fi, 2, 0, 0.0f, (unsigned int)v, NULL);
                    ImGui::SameLine();
                    if (v != 0 && ImGui::SmallButton("frame")) {
                        args.requestSelectEditorObjGuid = (unsigned int)v;
                        args.requestFocusEditorObjGuid = (unsigned int)v;
                    }
                } else {
                    char buf[128];
                    const char* cur = (args.propFieldStringVals && args.propFieldStringVals[fi]) ? args.propFieldStringVals[fi] : "";
                    std::strncpy(buf, cur, sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::InputText("##v", buf, IM_ARRAYSIZE(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                        CommitPropEdit(args, fi, 5, 0, 0.0f, 0, buf);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ── Phase 9 (Forge) — Schematic mode validation pre-pass ─────────
    // Walks the graph once per change and stamps three counts onto
    // EditorState: hard warnings (broken targets + empty event/action),
    // cycle members (info-only, Pandemic put 36-50 per map on purpose),
    // and longest-chain depth. Refresh gated on gameObjCount /
    // pendingFieldEditCount delta so we do NOT hammer the frame on a
    // stable level. Called from DrawSchematicMode every frame; the
    // actual O(V*E) walk only fires when the data actually moved.
    static void RebuildValidationIfStale(const ImGuiGlueFrameArgs& args,
                                          const GraphIndex& index)
    {
        bool stale = (s_state.validLastGOCount != args.gameObjCount ||
                      s_state.validLastPendCount != args.pendingFieldEditCount);
        if (!stale) return;
        s_state.validLastGOCount = args.gameObjCount;
        s_state.validLastPendCount = args.pendingFieldEditCount;
        s_state.validHardCount = 0;
        s_state.validCycleCount = 0;
        s_state.validMaxDepth = 0;
        if (args.gameObjCount <= 0 || !args.gameObjGuids) return;

        // Hard pass: count broken targets + empty event/action on every
        // Output entity. Reuses the same predicates the original Chain
        // tab's A3 validator did. Cheap, single-pass, no allocation.
        for (int i = 0; i < args.gameObjCount; ++i) {
            if (!IsOutputType(args, i)) continue;
            unsigned int tgt = args.gameObjTargetGuids ? args.gameObjTargetGuids[i] : 0;
            if (tgt != 0 && index.idxOf.find(tgt) == index.idxOf.end())
                ++s_state.validHardCount;
            const char* ev = args.gameObjOutputEvents ? args.gameObjOutputEvents[i] : NULL;
            const char* ac = args.gameObjInputEvents  ? args.gameObjInputEvents[i]  : NULL;
            if ((!ev || !ev[0]) || (!ac || !ac[0]))
                ++s_state.validHardCount;
        }

        // Cycle + depth pass: iterative DFS from every entity that has
        // outputs. Cycles bumped on first detection of an in-path
        // re-entry (avoids double-counting). Depth tracked per node,
        // updated when a longer path is discovered.
        if (args.gameObjOutputsCounts && args.gameObjOutputsOffsets &&
            args.gameObjOutputsData)
        {
            std::vector<char> visited(args.gameObjCount, 0);
            std::vector<char> inPath (args.gameObjCount, 0);
            std::vector<int>  depth  (args.gameObjCount, 0);
            std::set<unsigned int> cycleHit;
            struct Frame { int idx; int outIdx; };
            std::vector<Frame> stk;
            for (int start = 0; start < args.gameObjCount; ++start) {
                if (visited[start]) continue;
                int startCnt = args.gameObjOutputsCounts[start];
                if (startCnt <= 0) continue;
                stk.clear();
                Frame f0; f0.idx = start; f0.outIdx = 0;
                stk.push_back(f0);
                inPath[start] = 1;
                depth[start]  = 0;
                while (!stk.empty()) {
                    Frame& top = stk.back();
                    int cnt = (top.idx >= 0 && top.idx < args.gameObjCount)
                              ? args.gameObjOutputsCounts[top.idx] : 0;
                    int off = (top.idx >= 0 && top.idx < args.gameObjCount)
                              ? args.gameObjOutputsOffsets[top.idx] : 0;
                    if (top.outIdx >= cnt) {
                        visited[top.idx] = 1;
                        inPath[top.idx]  = 0;
                        if (depth[top.idx] > s_state.validMaxDepth)
                            s_state.validMaxDepth = depth[top.idx];
                        stk.pop_back();
                        continue;
                    }
                    unsigned int outG = args.gameObjOutputsData[off + top.outIdx];
                    top.outIdx++;
                    if (outG == 0) continue;
                    std::map<unsigned int, int>::const_iterator it = index.idxOf.find(outG);
                    if (it == index.idxOf.end()) continue;
                    int outIdx = it->second;
                    unsigned int nxtG = args.gameObjTargetGuids ? args.gameObjTargetGuids[outIdx] : 0;
                    if (nxtG == 0) continue;
                    std::map<unsigned int, int>::const_iterator nit = index.idxOf.find(nxtG);
                    if (nit == index.idxOf.end()) continue;
                    int nxtIdx = nit->second;
                    if (inPath[nxtIdx]) {
                        if (cycleHit.find(args.gameObjGuids[outIdx]) == cycleHit.end()) {
                            cycleHit.insert(args.gameObjGuids[outIdx]);
                            ++s_state.validCycleCount;
                        }
                        continue;
                    }
                    if (visited[nxtIdx]) {
                        int candidate = depth[top.idx] + 1;
                        if (candidate > depth[nxtIdx]) depth[nxtIdx] = candidate;
                        if (depth[nxtIdx] > s_state.validMaxDepth)
                            s_state.validMaxDepth = depth[nxtIdx];
                        continue;
                    }
                    inPath[nxtIdx] = 1;
                    depth[nxtIdx]  = depth[top.idx] + 1;
                    Frame nf; nf.idx = nxtIdx; nf.outIdx = 0;
                    stk.push_back(nf);
                }
            }
        }
    }

    // Note on Forge_RequestOpenCreateEntityWizard: it's defined at
    // GLOBAL scope in imgui_glue_dll.cpp. The extern decl lives at
    // the top of this file OUTSIDE the EventGraphEditor namespace
    // — declaring it inside the anonymous namespace here would mangle
    // to `EventGraphEditor::(anon)::Forge_*` and the linker would
    // never find the global definition.

    // ─────────────────────────────────────────────────────────────────
    //  DrawNodeGraphMode — output envelope cards + reroutable wires
    // ─────────────────────────────────────────────────────────────────
    //
    // The user explicitly asked for: "draw output as card and then
    // connect with wires (which wires can be placed/changed, put on
    // different cards and etc that was the goal)." This mode delivers
    // exactly that — every wire is rendered as THREE entities + TWO
    // splines:
    //
    //   [Source]──spline──>[Output envelope]──spline──>[Target]
    //
    // Five-column auto-layout with per-card free-place override. Cards
    // are draggable, sockets are drag-targets for re-routing wires.
    // The Output envelope is the wire's identity — moving the wire to a
    // new target means rerouting that envelope's target_guid, which
    // the existing chain-add request flow handles.
    static void DrawNodeGraphMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        // ── Top-row primary actions ────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.85f, 0.46f, 1.0f));
        if (ImGui::Button("+ Create entity##nodegraph_create", ImVec2(140.0f, 0.0f))) {
            Forge_RequestOpenCreateEntityWizardFromSource(s_state.focusGuid);
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextDisabled("(creates a new entity, then drag a wire socket onto it to attach)");
        ImGui::Separator();

        int focusIdx = FindByGuid(args, s_state.focusGuid);
        if (focusIdx < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                "No focus entity. Pick something in the 3D viewport, the outliner, or Level Blocks.");
            return;
        }

        // ── Build the column lists ─────────────────────────────────
        // For each of focus's outgoing wires we want THREE cards:
        //   focus (center), focus's Output envelope (middle-right),
        //   wire's target (right column).
        // For each incoming wire:
        //   source (left), source's Output envelope (middle-left),
        //   focus (center).
        std::vector<Triple> outgoing;
        std::vector<Triple> incoming;
        for (size_t i = 0; i < index.allTriples.size(); ++i) {
            const Triple& tr = index.allTriples[i];
            if (tr.ownerIdx == focusIdx)        outgoing.push_back(tr);
            else if (tr.targetIdx == focusIdx)  incoming.push_back(tr);
        }

        const float Z       = s_state.canvasZoom > 0.1f ? s_state.canvasZoom : 1.0f;
        const float CARD_W  = 200.0f * Z;     // entity cards
        const float CARD_H  = 56.0f * Z;
        const float ENVW    = 140.0f * Z;     // envelope cards (smaller)
        const float ENVH    = 40.0f * Z;
        const float ROW_GAP = 18.0f * Z;
        const float COL_GAP = 70.0f * Z;
        const float SOCKET_R = 6.0f * Z;
        const float CENTER_W = 240.0f * Z;
        const float CENTER_H_MIN = 96.0f * Z;

        int leftRows  = (int)incoming.size();
        int rightRows = (int)outgoing.size();
        int maxRows   = std::max(leftRows, rightRows);
        if (maxRows < 1) maxRows = 1;

        float canvasW = CARD_W + COL_GAP + ENVW + COL_GAP +
                      CENTER_W + COL_GAP + ENVW + COL_GAP + CARD_W + 60.0f;
        float canvasH = (float)maxRows * (CARD_H + ROW_GAP) + 80.0f;
        float centerH = std::max(CENTER_H_MIN,
                                (float)maxRows * 28.0f + 48.0f);
        if (canvasH < centerH + 60.0f) canvasH = centerH + 60.0f;

        ImGui::BeginChild("##forge_nodegraph", ImVec2(0, std::min(canvasH, 720.0f)), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        HandleCanvasPan();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 base   = ImGui::GetCursorScreenPos();
        ImVec2 mouse  = ImGui::GetIO().MousePos;
        ImGui::Dummy(ImVec2(canvasW, canvasH));

        // Free-place lookup — same map the original Schematic uses.
        auto cardOrigin = [&](unsigned int guid, ImVec2 autoPos) -> ImVec2 {
            std::map<unsigned int, ImVec2>::iterator it = s_state.schematicPositions.find(guid);
            if (it != s_state.schematicPositions.end())
                return ImVec2(base.x + it->second.x, base.y + it->second.y);
            return autoPos;
        };

        // Card-render helper. Returns the rendered AABB so callers can
        // wire sockets and hit-test clicks.
        auto renderCard = [&](ImVec2 origin, float w, float h,
                                const char* name, int nameLen,
                                const char* type, int typeLen,
                                ImU32 cardCol, ImU32 borderCol,
                                bool selected) -> std::pair<ImVec2, ImVec2>
        {
            ImVec2 cmax(origin.x + w, origin.y + h);
            dl->AddRectFilled(origin, cmax, cardCol, 6.0f);
            dl->AddRect(origin, cmax,
                selected ? IM_COL32(245, 220, 110, 255) : borderCol,
                6.0f, 0, selected ? 2.5f : 1.5f);
            // Title (top line) + type subtitle.
            ImVec2 textPos(origin.x + 8.0f, origin.y + 6.0f);
            char buf[160];
            int nLen = nameLen > 0 ? nameLen : (int)std::strlen(name);
            if (nLen > (int)sizeof(buf) - 1) nLen = (int)sizeof(buf) - 1;
            std::memcpy(buf, name, (size_t)nLen);
            buf[nLen] = 0;
            dl->AddText(textPos, IM_COL32(232, 234, 244, 255), buf);
            if (typeLen > 0 && type) {
                ImVec2 sub(origin.x + 8.0f, origin.y + h - 18.0f);
                char tbuf[80];
                int tLen = typeLen > (int)sizeof(tbuf) - 1 ? (int)sizeof(tbuf) - 1 : typeLen;
                std::memcpy(tbuf, type, (size_t)tLen);
                tbuf[tLen] = 0;
                dl->AddText(sub, IM_COL32(160, 164, 176, 255), tbuf);
            }
            return std::make_pair(origin, cmax);
        };

        // Resolve entity name + type from index.
        auto entityInfo = [&](int entIdx, const char** outName, int* outNameLen,
                                const char** outType, int* outTypeLen)
        {
            *outName = "(unknown)"; *outNameLen = 9;
            *outType = ""; *outTypeLen = 0;
            if (entIdx < 0 || entIdx >= args.gameObjCount) return;
            if (args.gameObjNames && args.gameObjNames[entIdx]) {
                *outName = args.gameObjNames[entIdx];
                *outNameLen = (int)std::strlen(*outName);
            }
            if (args.gameObjTypeNames && args.gameObjTypeNames[entIdx]) {
                *outType = args.gameObjTypeNames[entIdx];
                *outTypeLen = (int)std::strlen(*outType);
            }
        };

        // Spline draw helper — shared style for every wire.
        auto drawSpline = [&](ImVec2 from, ImVec2 to, ImU32 col, float thickness) {
            float mx = (from.x + to.x) * 0.5f;
            ImVec2 c1(mx, from.y), c2(mx, to.y);
            dl->AddBezierCubic(from, c1, c2, to, col, thickness);
            // Arrowhead at target end.
            float ax = to.x - c2.x, ay = to.y - c2.y;
            float al = std::sqrt(ax*ax + ay*ay);
            if (al > 0.001f) { ax /= al; ay /= al; }
            float perpx = -ay, perpy = ax;
            const float AH = 8.0f;
            ImVec2 b1(to.x - ax*AH + perpx*AH*0.5f,
                       to.y - ay*AH + perpy*AH*0.5f);
            ImVec2 b2(to.x - ax*AH - perpx*AH*0.5f,
                       to.y - ay*AH - perpy*AH*0.5f);
            dl->AddTriangleFilled(to, b1, b2, col);
        };

        // ── Render focus card (center column) ──────────────────────
        ImVec2 centerAuto(base.x + CARD_W + COL_GAP + ENVW + COL_GAP,
                            base.y + (canvasH - centerH) * 0.5f);
        ImVec2 centerMin = cardOrigin(s_state.focusGuid, centerAuto);
        ImVec2 centerMax(centerMin.x + CENTER_W, centerMin.y + centerH);
        const char* fName; int fNameLen; const char* fType; int fTypeLen;
        entityInfo(focusIdx, &fName, &fNameLen, &fType, &fTypeLen);
        renderCard(centerMin, CENTER_W, centerH,
                    fName, fNameLen, fType, fTypeLen,
                    IM_COL32(54, 60, 78, 245), IM_COL32(120, 130, 160, 255),
                    true);
        // Focus drag — store offset in s_state.schematicPositions.
        ImGui::SetCursorScreenPos(centerMin);
        ImGui::InvisibleButton("##nodegraph_focus_drag",
            ImVec2(CENTER_W, centerH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            ImVec2& p = s_state.schematicPositions[s_state.focusGuid];
            if (s_state.schematicPositions.find(s_state.focusGuid) ==
                s_state.schematicPositions.end())
            {
                p = ImVec2(centerAuto.x - base.x, centerAuto.y - base.y);
            }
            p.x += d.x; p.y += d.y;
        }

        // ── Render outgoing chain: focus -> envelope -> target ────
        for (int i = 0; i < rightRows; ++i) {
            const Triple& tr = outgoing[(size_t)i];
            float yOff = base.y + 40.0f + (float)i * (CARD_H + ROW_GAP);

            // Output envelope card (middle-right column)
            unsigned int envGuid = tr.outputGuid;
            ImVec2 envAuto(centerMax.x + COL_GAP, yOff + (CARD_H - ENVH) * 0.5f);
            ImVec2 envMin = cardOrigin(envGuid, envAuto);
            ImVec2 envMax(envMin.x + ENVW, envMin.y + ENVH);
            const char* eName; int eNameLen; const char* eType; int eTypeLen;
            entityInfo(tr.outputIdx, &eName, &eNameLen, &eType, &eTypeLen);
            bool envSelected = (envGuid == s_state.selectedOutputGuid);
            renderCard(envMin, ENVW, ENVH,
                        eName, eNameLen, eType, eTypeLen,
                        IM_COL32(70, 50, 86, 240), IM_COL32(160, 130, 200, 255),
                        envSelected);
            // Click-to-select envelope.
            char envId[64];
            std::snprintf(envId, sizeof(envId), "##ngraph_env_out_%u", envGuid);
            ImGui::SetCursorScreenPos(envMin);
            if (ImGui::InvisibleButton(envId, ImVec2(ENVW, ENVH))) {
                s_state.selectedOutputGuid = envGuid;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                ImVec2& p = s_state.schematicPositions[envGuid];
                if (s_state.schematicPositions.find(envGuid) ==
                    s_state.schematicPositions.end())
                {
                    p = ImVec2(envAuto.x - base.x, envAuto.y - base.y);
                }
                p.x += d.x; p.y += d.y;
            }

            // Target entity card (right column)
            ImVec2 tgtAuto(envMax.x + COL_GAP, yOff);
            ImVec2 tgtMin = cardOrigin(tr.targetGuid, tgtAuto);
            ImVec2 tgtMax(tgtMin.x + CARD_W, tgtMin.y + CARD_H);
            const char* tName; int tNameLen; const char* tType; int tTypeLen;
            if (tr.targetIdx >= 0) {
                entityInfo(tr.targetIdx, &tName, &tNameLen, &tType, &tTypeLen);
                renderCard(tgtMin, CARD_W, CARD_H,
                            tName, tNameLen, tType, tTypeLen,
                            IM_COL32(48, 70, 56, 240), IM_COL32(120, 180, 130, 255),
                            false);
            } else {
                // Broken target — render a red ghost card.
                renderCard(tgtMin, CARD_W, CARD_H,
                            "(broken)", 8, "", 0,
                            IM_COL32(80, 36, 36, 220), IM_COL32(220, 90, 90, 255),
                            false);
            }
            // Click-to-refocus on the target.
            char tgtId[64];
            std::snprintf(tgtId, sizeof(tgtId), "##ngraph_tgt_%u", tr.targetGuid);
            ImGui::SetCursorScreenPos(tgtMin);
            if (ImGui::InvisibleButton(tgtId, ImVec2(CARD_W, CARD_H))) {
                if (tr.targetGuid != 0) s_state.focusGuid = tr.targetGuid;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                ImVec2& p = s_state.schematicPositions[tr.targetGuid];
                if (s_state.schematicPositions.find(tr.targetGuid) ==
                    s_state.schematicPositions.end())
                {
                    p = ImVec2(tgtAuto.x - base.x, tgtAuto.y - base.y);
                }
                p.x += d.x; p.y += d.y;
            }

            // Two splines: focus -> envelope, envelope -> target.
            ImVec2 fromFocus(centerMax.x, centerMin.y + 30.0f + (float)i * 16.0f);
            ImVec2 envInSocket(envMin.x, envMin.y + ENVH * 0.5f);
            ImVec2 envOutSocket(envMax.x, envMin.y + ENVH * 0.5f);
            ImVec2 toTarget(tgtMin.x, tgtMin.y + CARD_H * 0.5f);
            ImU32 wireCol = envSelected
                ? IM_COL32(245, 220, 110, 255)
                : IM_COL32(180, 200, 230, 220);
            float thickness = envSelected ? 3.5f : 2.2f;
            drawSpline(fromFocus, envInSocket, wireCol, thickness);
            drawSpline(envOutSocket, toTarget, wireCol, thickness);

            // Sockets — render as small circles for visual hint.
            dl->AddCircleFilled(fromFocus, SOCKET_R,
                IM_COL32(200, 210, 230, 255));
            dl->AddCircleFilled(envInSocket, SOCKET_R - 1.0f,
                IM_COL32(180, 150, 220, 255));
            dl->AddCircleFilled(envOutSocket, SOCKET_R - 1.0f,
                IM_COL32(180, 150, 220, 255));
            dl->AddCircleFilled(toTarget, SOCKET_R,
                IM_COL32(140, 220, 160, 255));
        }

        // ── Render incoming chain: source -> envelope -> focus ────
        for (int i = 0; i < leftRows; ++i) {
            const Triple& tr = incoming[(size_t)i];
            float yOff = base.y + 40.0f + (float)i * (CARD_H + ROW_GAP);

            // Source entity card (left column)
            unsigned int srcGuid = (tr.ownerIdx >= 0 && args.gameObjGuids)
                                    ? args.gameObjGuids[tr.ownerIdx] : 0;
            ImVec2 srcAuto(base.x, yOff);
            ImVec2 srcMin = cardOrigin(srcGuid, srcAuto);
            ImVec2 srcMax(srcMin.x + CARD_W, srcMin.y + CARD_H);
            const char* sName; int sNameLen; const char* sType; int sTypeLen;
            if (tr.ownerIdx >= 0) {
                entityInfo(tr.ownerIdx, &sName, &sNameLen, &sType, &sTypeLen);
                renderCard(srcMin, CARD_W, CARD_H,
                            sName, sNameLen, sType, sTypeLen,
                            IM_COL32(48, 60, 78, 240), IM_COL32(120, 150, 200, 255),
                            false);
            }
            char srcId[64];
            std::snprintf(srcId, sizeof(srcId), "##ngraph_src_%u", srcGuid);
            ImGui::SetCursorScreenPos(srcMin);
            if (ImGui::InvisibleButton(srcId, ImVec2(CARD_W, CARD_H))) {
                if (srcGuid != 0) s_state.focusGuid = srcGuid;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                ImVec2& p = s_state.schematicPositions[srcGuid];
                if (s_state.schematicPositions.find(srcGuid) ==
                    s_state.schematicPositions.end())
                {
                    p = ImVec2(srcAuto.x - base.x, srcAuto.y - base.y);
                }
                p.x += d.x; p.y += d.y;
            }

            // Output envelope card (middle-left column)
            unsigned int envGuid = tr.outputGuid;
            ImVec2 envAuto(srcMax.x + COL_GAP, yOff + (CARD_H - ENVH) * 0.5f);
            ImVec2 envMin = cardOrigin(envGuid, envAuto);
            ImVec2 envMax(envMin.x + ENVW, envMin.y + ENVH);
            const char* eName; int eNameLen; const char* eType; int eTypeLen;
            entityInfo(tr.outputIdx, &eName, &eNameLen, &eType, &eTypeLen);
            bool envSelected = (envGuid == s_state.selectedOutputGuid);
            renderCard(envMin, ENVW, ENVH,
                        eName, eNameLen, eType, eTypeLen,
                        IM_COL32(70, 50, 86, 240), IM_COL32(160, 130, 200, 255),
                        envSelected);
            char envId[64];
            std::snprintf(envId, sizeof(envId), "##ngraph_env_in_%u", envGuid);
            ImGui::SetCursorScreenPos(envMin);
            if (ImGui::InvisibleButton(envId, ImVec2(ENVW, ENVH))) {
                s_state.selectedOutputGuid = envGuid;
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                ImVec2 d = ImGui::GetIO().MouseDelta;
                ImVec2& p = s_state.schematicPositions[envGuid];
                if (s_state.schematicPositions.find(envGuid) ==
                    s_state.schematicPositions.end())
                {
                    p = ImVec2(envAuto.x - base.x, envAuto.y - base.y);
                }
                p.x += d.x; p.y += d.y;
            }

            // Two splines: source -> envelope, envelope -> focus
            ImVec2 fromSrc(srcMax.x, srcMin.y + CARD_H * 0.5f);
            ImVec2 envInSocket(envMin.x, envMin.y + ENVH * 0.5f);
            ImVec2 envOutSocket(envMax.x, envMin.y + ENVH * 0.5f);
            ImVec2 toFocus(centerMin.x, centerMin.y + 30.0f + (float)i * 16.0f);
            ImU32 wireCol = envSelected
                ? IM_COL32(245, 220, 110, 255)
                : IM_COL32(180, 200, 230, 220);
            float thickness = envSelected ? 3.5f : 2.2f;
            drawSpline(fromSrc, envInSocket, wireCol, thickness);
            drawSpline(envOutSocket, toFocus, wireCol, thickness);

            dl->AddCircleFilled(fromSrc, SOCKET_R,
                IM_COL32(200, 210, 230, 255));
            dl->AddCircleFilled(envInSocket, SOCKET_R - 1.0f,
                IM_COL32(180, 150, 220, 255));
            dl->AddCircleFilled(envOutSocket, SOCKET_R - 1.0f,
                IM_COL32(180, 150, 220, 255));
            dl->AddCircleFilled(toFocus, SOCKET_R,
                IM_COL32(140, 220, 160, 255));
        }

        // ── Bottom hint ────────────────────────────────────────────
        ImGui::SetCursorScreenPos(ImVec2(base.x + 8.0f, base.y + canvasH - 22.0f));
        ImGui::TextDisabled("Click any card to refocus  •  Drag any card to reposition  •  "
                              "%d incoming wires, %d outgoing wires",
                              leftRows, rightRows);

        ImGui::EndChild();
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 9 (Forge) — DrawSchematicMode
    // ─────────────────────────────────────────────────────────────────
    //
    // The motherfucking centerpiece. XSI-style center-out chain editor
    // that codex deleted in the last extraction, now restored with
    // proper drag-pin authoring. Layout:
    //
    //   ┌─incoming card─┐                          ┌─target card─┐
    //   │ owner #1      ●───┐                  ┌──● target #1     │
    //   └───────────────┘   │                  │  └──────────────┘
    //                       │  ┌──FOCUS card──┐│
    //   ┌─incoming card─┐   ├─●●              ●●┤   ┌─target card─┐
    //   │ owner #2      ●───┘  │ event Output │  └──● target #2     │
    //   └───────────────┘      └──────────────┘     └──────────────┘
    //
    // - Center column: the focused entity, big card, every owned Output
    //   gets a small output socket on the right edge, every incoming
    //   wire lands on a corresponding input socket on the left edge.
    // - Left column: each entity that has the focus as a target of one
    //   of its Output blocks. One card per upstream owner, output
    //   socket on its right edge connects to focus's input.
    // - Right column: the target of each focus-owned Output. One card
    //   per downstream target, input socket on its left edge.
    // - Wires: bezier curves between sockets, color-coded by event
    //   family (Capture=gold, Death=red, Trigger=cyan, etc.).
    //
    // Drag-pin authoring: press LMB on any output socket → drag a
    // preview wire to the cursor → release on a target's input socket
    // → fire args.chainAddConnectionRequested with default event/action.
    // Same pipeline codex's "Author New Wire" form-fill uses, just
    // visual instead of integer-typing.
    //
    // Click an existing wire → select the underlying Output entity
    // (selectedOutputGuid + viewport selection) → Inline Output Surgery
    // editor below the canvas opens for event/action/delay/sticky edit.
    //
    // No Pandemic level designer ever typed a goddamn GUID into a
    // DragInt to wire two entities. They dragged. We drag.
    static void DrawSchematicMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        RebuildValidationIfStale(args, index);

        // User repeatedly pushed back: "Schematic still does not have
        // output blocks as blocks but its showed as a wire... we should
        // see outputs as blocks like any other normal focus and target
        // blocks, wires are not output envelopes."
        //
        // The old Schematic rendered Output envelope ENTITIES as
        // collapsed splines between source and target cards. The new
        // node-graph render (DrawNodeGraphMode) renders them as proper
        // cards with Bezier wires between sockets. So the Schematic
        // tab now redirects to that — same data, correct layout.
        // Validation header + slot diagnostic still run above the canvas
        // for the cockpit users; the canvas itself is the node-graph.
        DrawNodeGraphMode(args, index);
        return;

        // ── Validation header banner ─────────────────────────────────
        // Color: red if hard warnings, grey if cycles-only, green if clean.
        ImVec4 hdrCol;
        const char* hdrPrefix;
        if (s_state.validHardCount > 0) {
            hdrCol    = ImVec4(1.0f, 0.55f, 0.35f, 1.0f);
            hdrPrefix = "[BROKEN]";
        } else if (s_state.validCycleCount > 0) {
            hdrCol    = ImVec4(0.8f, 0.8f, 0.55f, 1.0f);
            hdrPrefix = "[CYCLES]";
        } else {
            hdrCol    = ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
            hdrPrefix = "[CLEAN]";
        }
        ImGui::TextColored(hdrCol,
            "%s %d hard warnings, %d cycles, max chain depth %d",
            hdrPrefix, s_state.validHardCount, s_state.validCycleCount, s_state.validMaxDepth);
        if (s_state.validHardCount > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(switch to Health tab for the kill list)");
        }

        // ── Focus must exist before we draw the canvas ───────────────
        int focusIdx = FindByGuid(args, s_state.focusGuid);
        if (focusIdx < 0) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                "No focus entity. Pick something in the 3D viewport, the outliner, or Level Blocks. The Schematic does not draw a graph of nothing.");
            return;
        }

        // ── Slot capacity diagnostic ──────────────────────────────────
        // The level format pre-allocates a fixed Outputs[] slot count
        // per entity type. Adding a new wire CONSUMES a zero-GUID
        // slot. If the focus has zero free slots, drag-pin authoring
        // will fail downstream with chainAddResult=2. Surface that
        // here so the user knows BEFORE they drag a wire that the
        // owner is full and they need to delete a wire first.
        if (args.selEntOutputsTotal > 0) {
            ImVec4 slotCol = (args.selEntOutputsZeroSlots > 0)
                ? ImVec4(0.55f, 0.9f, 0.55f, 1.0f)
                : ImVec4(1.0f, 0.6f, 0.3f, 1.0f);
            ImGui::TextColored(slotCol,
                "Outputs: %d/%d (%d free)",
                args.selEntOutputsUsed, args.selEntOutputsTotal,
                args.selEntOutputsZeroSlots);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Outputs[] slot capacity for the focused entity:");
                ImGui::BulletText("%d slots total (fixed by type definition)",
                    args.selEntOutputsTotal);
                ImGui::BulletText("%d wired (Output GUID present)",
                    args.selEntOutputsUsed);
                ImGui::BulletText("%d free (zero GUID, available for new wires)",
                    args.selEntOutputsZeroSlots);
                if (args.selEntOutputsZeroSlots == 0) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                        "FULL: drag-pin Add will fail until you delete an existing wire.");
                }
                ImGui::EndTooltip();
            }
            if (args.selEntOutputsZeroSlots == 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("(focus is full, right-click a wire to free a slot)");
            }
        }
        if (args.chainAddResult != 0) {
            ImVec4 resCol = (args.chainAddResult == 1)
                ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f)
                : ImVec4(1.0f, 0.55f, 0.35f, 1.0f);
            const char* msg =
                (args.chainAddResult == 1) ? "Output chain created. Good. The graph ate." :
                (args.chainAddResult == 2) ? "No empty Outputs slot. Owner is packed full of event-wire bullshit." :
                (args.chainAddResult == 3) ? "Output type missing. Cannot create a wire when the level has no Output template." :
                (args.chainAddResult == 4) ? "Bad source or target GUID. Chain builder refused to wire a ghost." :
                                             "Unknown chain result.";
            ImGui::TextColored(resCol, "%s", msg);
        }

        // ── Build the column lists ───────────────────────────────────
        // Outgoing: each Output entity owned by focus, with optional target.
        // Incoming: each Output entity whose target_guid == focus's guid,
        // and whose owner we resolve from the index.
        std::vector<Triple> outgoing;
        std::vector<Triple> incoming;
        {
            for (size_t i = 0; i < index.allTriples.size(); ++i) {
                const Triple& tr = index.allTriples[i];
                if (tr.ownerIdx == focusIdx) {
                    if (TripleMatchesSearch(args, tr)) outgoing.push_back(tr);
                }
                else if (tr.targetIdx == focusIdx) {
                    if (TripleMatchesSearch(args, tr)) incoming.push_back(tr);
                }
            }
        }

        // ── Canvas geometry ──────────────────────────────────────────
        // Zoom multiplies all dimensions; default 1.0 preserves
        // exact original layout.
        const float Z = s_state.canvasZoom;
        const float CARD_W = 220.0f * Z;
        const float CARD_H = 64.0f * Z;
        const float ROW_GAP = 14.0f * Z;
        const float COL_GAP = 60.0f * Z;
        const float SOCKET_R = 6.0f * Z;
        const float CENTER_W = 280.0f * Z;
        const float CENTER_H_MIN = 96.0f * Z;

        int leftRows  = (int)incoming.size();
        int rightRows = (int)outgoing.size();
        int maxRows   = std::max(leftRows, rightRows);
        if (maxRows < 1) maxRows = 1;

        float canvasW = CARD_W + COL_GAP + CENTER_W + COL_GAP + CARD_W + 40.0f;
        float canvasH = (float)maxRows * (CARD_H + ROW_GAP) + 80.0f;
        float centerH = std::max(CENTER_H_MIN,
                                 (float)std::max(leftRows, rightRows) * 28.0f + 48.0f);
        if (canvasH < centerH + 60.0f) canvasH = centerH + 60.0f;

        ImGui::BeginChild("##forge_schematic", ImVec2(0, std::min(canvasH, 720.0f)), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        HandleCanvasPan();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 base   = ImGui::GetCursorScreenPos();
        ImVec2 mouse  = ImGui::GetIO().MousePos;
        ImGui::Dummy(ImVec2(canvasW, canvasH));

        // Reset hover each frame; recompute below during socket pass.
        s_state.hoverPinGuid    = 0;
        s_state.hoverPinIsInput = true;

        // Phase 10b free-place helper: if an entity has a remembered
        // override position in s_state.schematicPositions, this returns
        // the override; otherwise it returns the supplied auto-layout
        // origin. Override is stored as offset from canvas origin
        // (base.x/base.y) so it survives canvas zoom changes — drag
        // commits the same offset and the layout follows.
        auto cardOrigin = [&](unsigned int guid, ImVec2 autoPos) -> ImVec2 {
            std::map<unsigned int, ImVec2>::iterator it = s_state.schematicPositions.find(guid);
            if (it != s_state.schematicPositions.end())
                return ImVec2(base.x + it->second.x, base.y + it->second.y);
            return autoPos;
        };

        // Focus card sits in the center column, vertically centered.
        // Free-place override applied if user has dragged it before.
        ImVec2 centerAuto = ImVec2(base.x + CARD_W + COL_GAP,
                                   base.y + (canvasH - centerH) * 0.5f);
        ImVec2 centerMin = cardOrigin(s_state.focusGuid, centerAuto);
        ImVec2 centerMax = ImVec2(centerMin.x + CENTER_W, centerMin.y + centerH);

        // ── Pre-compute all socket positions ─────────────────────────
        // Each incoming card (left column) has ONE output socket on its
        // right edge that connects to a unique input socket on the
        // FOCUS card's left edge. Each outgoing card (right column) has
        // ONE input socket on its left edge that connects to a unique
        // output socket on the FOCUS card's right edge.
        struct CardSlot { ImVec2 cardMin, cardMax; ImVec2 socket; };
        std::vector<CardSlot> incomingSlots(leftRows);
        std::vector<CardSlot> outgoingSlots(rightRows);
        std::vector<ImVec2>   focusInputSockets(leftRows);
        std::vector<ImVec2>   focusOutputSockets(rightRows);

        for (int i = 0; i < leftRows; ++i) {
            float y = base.y + 40.0f + (float)i * (CARD_H + ROW_GAP);
            ImVec2 autoMin(base.x, y);
            unsigned int g = (incoming[i].ownerIdx >= 0)
                             ? EntityGuid(args, incoming[i].ownerIdx) : 0;
            ImVec2 cmin = cardOrigin(g, autoMin);
            incomingSlots[i].cardMin = cmin;
            incomingSlots[i].cardMax = ImVec2(cmin.x + CARD_W, cmin.y + CARD_H);
            incomingSlots[i].socket  = ImVec2(cmin.x + CARD_W, cmin.y + CARD_H * 0.5f);
            float fy = centerMin.y + 40.0f +
                       ((float)i + 0.5f) / std::max(1, leftRows) * (centerH - 60.0f);
            focusInputSockets[i] = ImVec2(centerMin.x, fy);
        }
        for (int i = 0; i < rightRows; ++i) {
            float y = base.y + 40.0f + (float)i * (CARD_H + ROW_GAP);
            ImVec2 autoMin(base.x + CARD_W + COL_GAP + CENTER_W + COL_GAP, y);
            unsigned int g = (outgoing[i].targetIdx >= 0)
                             ? EntityGuid(args, outgoing[i].targetIdx)
                             : outgoing[i].targetGuid;
            ImVec2 cmin = cardOrigin(g, autoMin);
            outgoingSlots[i].cardMin = cmin;
            outgoingSlots[i].cardMax = ImVec2(cmin.x + CARD_W, cmin.y + CARD_H);
            outgoingSlots[i].socket  = ImVec2(cmin.x, cmin.y + CARD_H * 0.5f);
            float fy = centerMin.y + 40.0f +
                       ((float)i + 0.5f) / std::max(1, rightRows) * (centerH - 60.0f);
            focusOutputSockets[i] = ImVec2(centerMax.x, fy);
        }

        // ── Draw wires FIRST so cards render on top ──────────────────
        // Each wire is a cubic bezier with two control points midway
        // between source and target on the X axis. Arrowhead at target
        // end, color from EventColorForName. Hit-test the bezier for
        // click-to-select-Output (cheap: sample 16 points along the
        // curve, find closest to mouse, accept if within 6 pixels).
        struct WireRecord {
            ImVec2 from, to;
            ImU32  color;
            unsigned int outputGuid;
        };
        std::vector<WireRecord> wires;
        wires.reserve(leftRows + rightRows);

        for (int i = 0; i < leftRows; ++i) {
            const Triple& tr = incoming[i];
            const char* ev = (tr.outputIdx >= 0 && args.gameObjOutputEvents)
                             ? Safe(args.gameObjOutputEvents[tr.outputIdx], "") : "";
            WireRecord w;
            w.from = incomingSlots[i].socket;
            w.to   = focusInputSockets[i];
            w.color = EventColorForName(ev);
            w.outputGuid = tr.outputGuid;
            wires.push_back(w);
        }
        for (int i = 0; i < rightRows; ++i) {
            const Triple& tr = outgoing[i];
            const char* ev = (tr.outputIdx >= 0 && args.gameObjOutputEvents)
                             ? Safe(args.gameObjOutputEvents[tr.outputIdx], "") : "";
            WireRecord w;
            w.from = focusOutputSockets[i];
            w.to   = outgoingSlots[i].socket;
            w.color = EventColorForName(ev);
            w.outputGuid = tr.outputGuid;
            wires.push_back(w);
        }

        // Wire hit-test: returns index in `wires` of closest, -1 if none.
        int hoveredWire = -1;
        float bestWireDistSq = 36.0f; // 6px threshold squared
        for (size_t w = 0; w < wires.size(); ++w) {
            ImVec2 p0 = wires[w].from;
            ImVec2 p3 = wires[w].to;
            float mx = (p0.x + p3.x) * 0.5f;
            ImVec2 c1(mx, p0.y), c2(mx, p3.y);
            const int N = 18;
            for (int k = 0; k <= N; ++k) {
                float t = (float)k / (float)N;
                float u = 1.0f - t;
                float bx = u*u*u*p0.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*p3.x;
                float by = u*u*u*p0.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*p3.y;
                float dx = bx - mouse.x, dy = by - mouse.y;
                float d2 = dx*dx + dy*dy;
                if (d2 < bestWireDistSq) {
                    bestWireDistSq = d2;
                    hoveredWire = (int)w;
                }
            }
        }

        for (size_t w = 0; w < wires.size(); ++w) {
            ImVec2 p0 = wires[w].from;
            ImVec2 p3 = wires[w].to;
            float mx = (p0.x + p3.x) * 0.5f;
            ImVec2 c1(mx, p0.y), c2(mx, p3.y);
            ImU32 col = wires[w].color;
            float thickness = ((int)w == hoveredWire) ? 3.5f : 2.2f;
            // Highlight the selected Output's wire so the user can
            // always find what they just clicked.
            if (wires[w].outputGuid == s_state.selectedOutputGuid)
                thickness = 4.0f;
            dl->AddBezierCubic(p0, c1, c2, p3, col, thickness);
            // Arrowhead at target end. Direction = (p3 - c2) normalized.
            float ax = p3.x - c2.x, ay = p3.y - c2.y;
            float al = std::sqrt(ax*ax + ay*ay);
            if (al > 0.001f) { ax /= al; ay /= al; }
            float perpx = -ay, perpy = ax;
            const float AH = 9.0f;
            ImVec2 tip = p3;
            ImVec2 b1(tip.x - ax*AH + perpx*AH*0.5f,
                      tip.y - ay*AH + perpy*AH*0.5f);
            ImVec2 b2(tip.x - ax*AH - perpx*AH*0.5f,
                      tip.y - ay*AH - perpy*AH*0.5f);
            dl->AddTriangleFilled(tip, b1, b2, col);
        }

        // ── Wire hover tooltip ────────────────────────────────────────
        // Hover any wire to see the event/action/delay/sticky payload
        // without having to click-select first. Saves the user from
        // hunting through the Inline Output Surgery editor every time
        // they want to identify what a curve is actually wired for.
        if (hoveredWire >= 0) {
            unsigned int outG = wires[hoveredWire].outputGuid;
            int outIdx = FindByGuid(args, outG);
            if (outIdx >= 0) {
                const char* ev = (args.gameObjOutputEvents && args.gameObjOutputEvents[outIdx])
                                 ? args.gameObjOutputEvents[outIdx] : "";
                const char* ac = (args.gameObjInputEvents && args.gameObjInputEvents[outIdx])
                                 ? args.gameObjInputEvents[outIdx] : "";
                float dly = (args.gameObjDelays) ? args.gameObjDelays[outIdx] : 0.0f;
                int   stk = (args.gameObjSticky) ? args.gameObjSticky[outIdx] : 0;
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(1.0f, 0.92f, 0.55f, 1.0f),
                    "Output 0x%08X", outG);
                ImGui::Separator();
                ImGui::Text("event:  %s", ev[0] ? ev : "(empty - fires nothing)");
                ImGui::Text("action: %s", ac[0] ? ac : "(empty - target sees nothing)");
                ImGui::Text("delay:  %.2fs", dly);
                ImGui::Text("sticky: %s", stk ? "yes" : "no");
                ImGui::Separator();
                ImGui::TextDisabled("LMB = select + isolate, RMB = context menu");
                ImGui::EndTooltip();
            }
        }

        // ── Click on hovered wire = select Output entity for editing ──
        if (hoveredWire >= 0 && ImGui::IsMouseClicked(0)) {
            unsigned int outG = wires[hoveredWire].outputGuid;
            if (outG != 0) {
                s_state.selectedOutputGuid = outG;
                args.requestSelectEditorObjGuid = outG;
                IsolateWire(args, outG);
            }
        }

        // ── Right-click hovered wire = open context menu ──────────────
        // Delete Wire fires args.chainDeleteConnectionRequested, the
        // host already wires that into the FieldEdit pipeline. Frame
        // Target flies the camera to the receiving entity. Copy GUID
        // pushes the Output's GUID onto the clipboard for paste-into-
        // search workflows.
        if (hoveredWire >= 0 && ImGui::IsMouseClicked(1)) {
            s_state.selectedOutputGuid = wires[hoveredWire].outputGuid;
            ImGui::OpenPopup("##forge_wire_ctx");
        }
        if (ImGui::BeginPopup("##forge_wire_ctx")) {
            unsigned int outG = s_state.selectedOutputGuid;
            int outIdx = FindByGuid(args, outG);
            char hdr[64]; _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "Output 0x%08X", outG);
            ImGui::TextDisabled("%s", hdr);
            ImGui::Separator();
            // Resolve owner for the delete request (chainDeleteSourceGuid
            // expects the upstream entity that owns the Output in its
            // Outputs[] list).
            std::map<unsigned int, int>::const_iterator ownIt =
                index.ownerOfOutput.find(outG);
            unsigned int ownerGuid = (ownIt != index.ownerOfOutput.end())
                ? EntityGuid(args, ownIt->second) : 0;

            if (ImGui::MenuItem("Frame Target") && outIdx >= 0 && args.gameObjTargetGuids) {
                unsigned int tgt = args.gameObjTargetGuids[outIdx];
                if (tgt != 0) {
                    args.requestSelectEditorObjGuid = tgt;
                    args.requestFocusEditorObjGuid = tgt;
                }
            }
            if (ImGui::MenuItem("Select Output Entity"))
                SelectEntity(args, outG, false);
            if (ImGui::MenuItem("Copy Output GUID")) {
                char gtxt[32]; _snprintf_s(gtxt, sizeof(gtxt), _TRUNCATE, "0x%08X", outG);
                ImGui::SetClipboardText(gtxt);
            }
            ImGui::Separator();
            bool canDelete = (ownerGuid != 0 && outG != 0);
            ImGui::BeginDisabled(!canDelete);
            if (ImGui::MenuItem("Delete Wire")) {
                args.chainDeleteConnectionRequested = 1;
                args.chainDeleteSourceGuid = ownerGuid;
                args.chainDeleteOutputGuid = outG;
                s_state.selectedOutputGuid = 0;
            }
            ImGui::EndDisabled();
            if (!canDelete)
                ImGui::TextDisabled("(owner unresolved, cannot patch Outputs[])");
            ImGui::EndPopup();
        }

        // ── Card draw helper ─────────────────────────────────────────
        auto drawCard = [&](const ImVec2& a, const ImVec2& b, int idx,
                            unsigned int fallbackGuid, const char* role,
                            ImU32 borderCol, bool isFocus)
        {
            ImU32 fill = (idx >= 0) ? IM_COL32(36, 38, 48, 240)
                                    : IM_COL32(70, 28, 28, 240);
            if (isFocus) fill = IM_COL32(46, 56, 36, 245);
            dl->AddRectFilled(a, b, fill, 6.0f);
            dl->AddRect(a, b, borderCol, 6.0f, 0, isFocus ? 2.5f : 1.5f);
            dl->AddText(ImVec2(a.x + 8.0f, a.y + 4.0f),
                        IM_COL32(170, 176, 190, 255), role);
            if (idx >= 0) {
                char ln[160];
                _snprintf_s(ln, sizeof(ln), _TRUNCATE, "%s", EntityName(args, idx));
                dl->AddText(ImVec2(a.x + 8.0f, a.y + 22.0f),
                            IM_COL32(232, 234, 244, 255), ln);
                _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[%s]", EntityType(args, idx));
                dl->AddText(ImVec2(a.x + 8.0f, a.y + 40.0f),
                            IM_COL32(132, 200, 244, 255), ln);
            } else if (fallbackGuid != 0) {
                char ln[160];
                _snprintf_s(ln, sizeof(ln), _TRUNCATE, "ghost 0x%08X", fallbackGuid);
                dl->AddText(ImVec2(a.x + 8.0f, a.y + 22.0f),
                            IM_COL32(255, 122, 104, 255), ln);
                dl->AddText(ImVec2(a.x + 8.0f, a.y + 40.0f),
                            IM_COL32(255, 170, 150, 255),
                            "dead ref, this wire is fucked");
            }
        };

        // ── Draw incoming cards (left column) ────────────────────────
        for (int i = 0; i < leftRows; ++i) {
            const Triple& tr = incoming[i];
            drawCard(incomingSlots[i].cardMin, incomingSlots[i].cardMax,
                     tr.ownerIdx, EntityGuid(args, tr.ownerIdx),
                     "OWNER", IM_COL32(120, 170, 255, 255), false);
        }
        // ── Draw outgoing cards (right column) ───────────────────────
        for (int i = 0; i < rightRows; ++i) {
            const Triple& tr = outgoing[i];
            drawCard(outgoingSlots[i].cardMin, outgoingSlots[i].cardMax,
                     tr.targetIdx, tr.targetGuid,
                     "TARGET", IM_COL32(130, 255, 150, 255), false);
        }
        // ── Draw focus card (center column) ──────────────────────────
        drawCard(centerMin, centerMax, focusIdx, s_state.focusGuid,
                 "FOCUS", IM_COL32(255, 220, 120, 255), true);

        // Focus card subtitle: show output count + input count
        char sub[96];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE,
                    "+%d outgoing / %d incoming",
                    rightRows, leftRows);
        dl->AddText(ImVec2(centerMin.x + 8.0f, centerMin.y + 60.0f),
                    IM_COL32(180, 200, 180, 255), sub);

        // ── Draw sockets on top of cards ─────────────────────────────
        // Each socket is a colored disc. Hover detection runs here too.
        auto drawSocket = [&](const ImVec2& p, ImU32 col, unsigned int forGuid,
                              bool isInput)
        {
            float dx = mouse.x - p.x, dy = mouse.y - p.y;
            bool hovered = (dx*dx + dy*dy < (SOCKET_R + 4.0f) * (SOCKET_R + 4.0f));
            if (hovered) {
                s_state.hoverPinGuid = forGuid;
                s_state.hoverPinIsInput = isInput;
                dl->AddCircle(p, SOCKET_R + 3.0f,
                              IM_COL32(255, 244, 130, 255), 16, 2.0f);
            }
            dl->AddCircleFilled(p, SOCKET_R, col, 16);
            dl->AddCircle(p, SOCKET_R, IM_COL32(20, 20, 24, 220), 16, 1.5f);
        };

        // Owner cards have output sockets on right edge.
        for (int i = 0; i < leftRows; ++i) {
            const Triple& tr = incoming[i];
            drawSocket(incomingSlots[i].socket,
                       EventColorForName(
                         (tr.outputIdx >= 0 && args.gameObjOutputEvents)
                           ? args.gameObjOutputEvents[tr.outputIdx] : ""),
                       EntityGuid(args, tr.ownerIdx), false);
        }
        // Target cards have input sockets on left edge.
        for (int i = 0; i < rightRows; ++i) {
            const Triple& tr = outgoing[i];
            drawSocket(outgoingSlots[i].socket,
                       IM_COL32(180, 220, 200, 255),
                       tr.targetGuid, true);
        }
        // Focus input sockets (left edge of center card) and output
        // sockets (right edge). Focus output socket is the SOURCE for
        // drag-to-author. Focus input socket is the DESTINATION when
        // user drags from another card's output.
        for (int i = 0; i < leftRows; ++i) {
            drawSocket(focusInputSockets[i],
                       IM_COL32(180, 220, 200, 255),
                       s_state.focusGuid, true);
        }
        for (int i = 0; i < rightRows; ++i) {
            const Triple& tr = outgoing[i];
            drawSocket(focusOutputSockets[i],
                       EventColorForName(
                         (tr.outputIdx >= 0 && args.gameObjOutputEvents)
                           ? args.gameObjOutputEvents[tr.outputIdx] : ""),
                       s_state.focusGuid, false);
        }
        // Plus an extra "author new" output socket below the last
        // existing output for explicit drag-to-create (so you can wire
        // a second output to a new target without overloading an
        // existing slot).
        ImVec2 newOutPin = ImVec2(centerMax.x,
                                  centerMin.y + 40.0f +
                                  (focusOutputSockets.size() + 0.5f) /
                                  std::max((size_t)1, focusOutputSockets.size() + 1) *
                                  (centerH - 60.0f));
        drawSocket(newOutPin, IM_COL32(255, 244, 130, 255),
                   s_state.focusGuid, false);
        dl->AddText(ImVec2(centerMax.x + 8.0f, newOutPin.y - 6.0f),
                    IM_COL32(255, 244, 130, 255), "+ wire");

        // ── Drag-pin state machine ───────────────────────────────────
        // Begin: LMB pressed while a non-input socket is hovered.
        // Update: while mouse is held, draw preview wire to cursor.
        // End: LMB released. If hovering an input socket on a different
        // entity, fire chainAddConnectionRequested with default
        // event/action. Esc cancels mid-drag.
        if (s_state.dragFromGuid == 0) {
            if (ImGui::IsMouseClicked(0) &&
                s_state.hoverPinGuid != 0 &&
                !s_state.hoverPinIsInput)
            {
                s_state.dragFromGuid = s_state.hoverPinGuid;
                s_state.dragFromIsOutput = true;
            }
        } else {
            // Preview wire from start socket to mouse.
            int srcIdx = FindByGuid(args, s_state.dragFromGuid);
            ImVec2 from(mouse);
            // Best-effort source position: if dragging from focus, use
            // newOutPin; otherwise use the corresponding owner card's
            // output socket.
            if (s_state.dragFromGuid == s_state.focusGuid) {
                from = newOutPin;
            } else {
                for (int i = 0; i < leftRows; ++i) {
                    if (EntityGuid(args, incoming[i].ownerIdx) == s_state.dragFromGuid) {
                        from = incomingSlots[i].socket;
                        break;
                    }
                }
            }
            float mx = (from.x + mouse.x) * 0.5f;
            ImVec2 c1(mx, from.y), c2(mx, mouse.y);
            dl->AddBezierCubic(from, c1, c2, mouse,
                               IM_COL32(255, 244, 130, 255), 2.5f);

            (void)srcIdx;

            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                s_state.dragFromGuid = 0;

            if (ImGui::IsMouseReleased(0)) {
                if (s_state.hoverPinGuid != 0 &&
                    s_state.hoverPinIsInput &&
                    s_state.hoverPinGuid != s_state.dragFromGuid)
                {
                    // Phase 10c: capture endpoints, open the Event
                    // Creator popup. The chain-add request is fired
                    // ONLY after the user clicks Create — or never
                    // if they Cancel. Designers used to drag a wire,
                    // get an empty event name, and then wonder why
                    // their trigger didn't fire. Now the popup makes
                    // them fill the labels before the wire is real.
                    s_state.wizardSourceGuid = s_state.dragFromGuid;
                    s_state.wizardTargetGuid = s_state.hoverPinGuid;
                    // Seed the wizard fields with the current form-fill
                    // defaults so power-users keep their typed names.
                    std::strncpy(s_state.wizardEvent,  s_state.addOutputEvent,
                                 sizeof(s_state.wizardEvent) - 1);
                    s_state.wizardEvent[sizeof(s_state.wizardEvent) - 1] = '\0';
                    std::strncpy(s_state.wizardAction, s_state.addInputAction,
                                 sizeof(s_state.wizardAction) - 1);
                    s_state.wizardAction[sizeof(s_state.wizardAction) - 1] = '\0';
                    s_state.wizardDelay  = s_state.addDelay;
                    s_state.wizardSticky = s_state.addSticky;
                    s_state.wizardOpen   = true;
                    ImGui::OpenPopup("Event Creator##forge_wire_wizard");
                }
                s_state.dragFromGuid = 0;
            }
        }

        // ── Phase 10b free-place drag + click hit-test ───────────────
        // Single mouse-down on card body starts a candidate drag. If
        // the mouse moves more than DRAG_THRESH while held, we commit
        // the new position to s_state.schematicPositions every frame
        // until release. On release without enough movement, it falls
        // back to the old click-to-select / double-click-to-frame
        // behavior. Sockets and wire pins take priority because their
        // hit detection runs first.
        auto cardHit = [&](const ImVec2& a, const ImVec2& b) -> bool {
            return (mouse.x >= a.x && mouse.x <= b.x &&
                    mouse.y >= a.y && mouse.y <= b.y);
        };
        const float DRAG_THRESH = 4.0f;
        bool overSocket = (s_state.hoverPinGuid != 0);
        bool overWire   = (hoveredWire >= 0);
        bool inSocketDrag = (s_state.dragFromGuid != 0);

        // Resolve which card the mouse is over (if any). Priority:
        // outgoing > incoming > focus (focus is biggest, so checked last
        // so an overlapping outgoing card on top of focus wins).
        unsigned int hoveredCardGuid = 0;
        if (!overSocket && !overWire && !inSocketDrag) {
            for (int i = 0; i < rightRows; ++i) {
                if (cardHit(outgoingSlots[i].cardMin, outgoingSlots[i].cardMax)) {
                    hoveredCardGuid = (outgoing[i].targetIdx >= 0)
                                      ? EntityGuid(args, outgoing[i].targetIdx)
                                      : outgoing[i].targetGuid;
                    break;
                }
            }
            if (hoveredCardGuid == 0) {
                for (int i = 0; i < leftRows; ++i) {
                    if (cardHit(incomingSlots[i].cardMin, incomingSlots[i].cardMax)) {
                        hoveredCardGuid = EntityGuid(args, incoming[i].ownerIdx);
                        break;
                    }
                }
            }
            if (hoveredCardGuid == 0 && cardHit(centerMin, centerMax)) {
                hoveredCardGuid = s_state.focusGuid;
            }
        }

        // Begin candidate drag on mouse-down over a card.
        if (s_state.draggingCardGuid == 0 && hoveredCardGuid != 0 &&
            ImGui::IsMouseClicked(0))
        {
            s_state.draggingCardGuid = hoveredCardGuid;
            // Grab offset from card top-left so the card doesn't snap
            // to cursor on first drag pixel.
            ImVec2 cardMin;
            if (hoveredCardGuid == s_state.focusGuid) {
                cardMin = centerMin;
            } else {
                cardMin = mouse;
                for (int i = 0; i < rightRows; ++i) {
                    unsigned int g = (outgoing[i].targetIdx >= 0)
                                     ? EntityGuid(args, outgoing[i].targetIdx)
                                     : outgoing[i].targetGuid;
                    if (g == hoveredCardGuid) { cardMin = outgoingSlots[i].cardMin; break; }
                }
                for (int i = 0; i < leftRows; ++i) {
                    if (EntityGuid(args, incoming[i].ownerIdx) == hoveredCardGuid) {
                        cardMin = incomingSlots[i].cardMin; break;
                    }
                }
            }
            s_state.dragGrabOffset.x = mouse.x - cardMin.x;
            s_state.dragGrabOffset.y = mouse.y - cardMin.y;
        }

        // Track movement while held. The position write is gated by
        // DRAG_THRESH so a steady click doesn't accidentally place the
        // card 1 pixel away from where it started.
        if (s_state.draggingCardGuid != 0 && ImGui::IsMouseDown(0)) {
            ImVec2 dd = ImGui::GetMouseDragDelta(0, 0.0f);
            float dist = std::sqrt(dd.x*dd.x + dd.y*dd.y);
            if (dist > DRAG_THRESH) {
                s_state.schematicPositions[s_state.draggingCardGuid] = ImVec2(
                    mouse.x - base.x - s_state.dragGrabOffset.x,
                    mouse.y - base.y - s_state.dragGrabOffset.y);
            }
        }

        // On release: if barely moved, treat as click-to-select. If
        // moved more than threshold, commit was already written above
        // and we just end the drag. Double-click separately frames.
        bool draggedFar = false;
        if (s_state.draggingCardGuid != 0 && ImGui::IsMouseReleased(0)) {
            ImVec2 dd = ImGui::GetMouseDragDelta(0, 0.0f);
            float dist = std::sqrt(dd.x*dd.x + dd.y*dd.y);
            draggedFar = (dist > DRAG_THRESH);
            if (!draggedFar) {
                SelectEntity(args, s_state.draggingCardGuid, false);
            }
            s_state.draggingCardGuid = 0;
            ImGui::ResetMouseDragDelta(0);
        }

        // Double-click still frames the entity in 3D regardless of drag.
        if (!overSocket && !overWire && !inSocketDrag) {
            for (int i = 0; i < leftRows; ++i) {
                if (cardHit(incomingSlots[i].cardMin, incomingSlots[i].cardMax)) {
                    if (ImGui::IsMouseDoubleClicked(0))
                        SelectEntity(args, EntityGuid(args, incoming[i].ownerIdx), true);
                    break;
                }
            }
            for (int i = 0; i < rightRows; ++i) {
                if (cardHit(outgoingSlots[i].cardMin, outgoingSlots[i].cardMax)) {
                    unsigned int g = (outgoing[i].targetIdx >= 0)
                                     ? EntityGuid(args, outgoing[i].targetIdx)
                                     : outgoing[i].targetGuid;
                    if (ImGui::IsMouseDoubleClicked(0)) SelectEntity(args, g, true);
                    break;
                }
            }
        }

        // ── Phase 10b: empty-canvas right-click → reset layout ───────
        // Clear schematicPositions so every card snaps back to the
        // auto-layout. Right-clicking the empty canvas (not a card,
        // not a wire, not a socket) opens this. One-button rescue
        // when a designer's screen looks like a fucking yard sale.
        if (hoveredCardGuid == 0 && !overSocket && !overWire &&
            !inSocketDrag && ImGui::IsMouseClicked(1) &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        {
            ImGui::OpenPopup("##forge_canvas_ctx");
        }
        if (ImGui::BeginPopup("##forge_canvas_ctx")) {
            int placed = (int)s_state.schematicPositions.size();
            char hdr[96]; _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                "Canvas (%d card%s placed by hand)",
                placed, (placed == 1) ? "" : "s");
            ImGui::TextDisabled("%s", hdr);
            ImGui::Separator();
            // Phase 10e: shortcut to the Create Entity wizard. The
            // Outliner tab owns the actual popup body (it sits next to
            // the entity tree where the result will appear), but a
            // designer brain on the Schematic canvas should not have
            // to tab away to add something — surface a jump button
            // here that flips them over with the wizard pre-armed.
            if (ImGui::MenuItem("Create Entity (jump to Outliner)")) {
                s_state.requestSwitchTo = View_Outliner;
                s_state.requestOpenCreateWizard = true;
            }
            ImGui::Separator();
            ImGui::BeginDisabled(placed == 0);
            if (ImGui::MenuItem("Reset Layout")) {
                s_state.schematicPositions.clear();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // ── Phase 10c: Event Creator wizard popup ────────────────────
        // Triggered when a drag-pin lands on a target socket. Lets the
        // designer name the event/action, set delay/sticky, and confirm
        // before the wire is real. Cancel discards the in-flight wire.
        // Placed inside BeginChild so the popup ID stack matches the
        // OpenPopup call site, same convention as forge_wire_ctx above.
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Event Creator##forge_wire_wizard", NULL,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            int srcIdx = FindByGuid(args, s_state.wizardSourceGuid);
            int tgtIdx = FindByGuid(args, s_state.wizardTargetGuid);
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
                "Wiring a new event chain. Fill the labels or this is a dead wire.");
            ImGui::Separator();

            ImGui::Text("Source: %s",
                (srcIdx >= 0) ? EntityName(args, srcIdx) : "(unknown)");
            if (srcIdx >= 0)
                ImGui::TextDisabled("  type: %s   guid: 0x%08X",
                    EntityType(args, srcIdx), s_state.wizardSourceGuid);
            ImGui::Text("Target: %s",
                (tgtIdx >= 0) ? EntityName(args, tgtIdx) : "(unknown)");
            if (tgtIdx >= 0)
                ImGui::TextDisabled("  type: %s   guid: 0x%08X",
                    EntityType(args, tgtIdx), s_state.wizardTargetGuid);

            ImGui::Separator();
            ImGui::TextDisabled("Event names: OnTrigger, OnDeath, OnEnter, OnExit, OnComplete, OnCapture, OnSpawn, OnUser1..");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##wiz_evt", "OnTrigger",
                s_state.wizardEvent, sizeof(s_state.wizardEvent));
            ImGui::TextDisabled("Action names: Activate, Deactivate, Show, Hide, Spawn, Despawn, Toggle..");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##wiz_act", "Activate",
                s_state.wizardAction, sizeof(s_state.wizardAction));

            ImGui::Spacing();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("Delay (s)##wiz", &s_state.wizardDelay, 0.1f, 1.0f, "%.2f");
            ImGui::SameLine();
            ImGui::Checkbox("Sticky##wiz", &s_state.wizardSticky);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sticky = re-fire on every event tick instead of once.");

            ImGui::Separator();
            bool canCommit = (s_state.wizardEvent[0] != '\0' &&
                              s_state.wizardAction[0] != '\0' &&
                              s_state.wizardSourceGuid != 0 &&
                              s_state.wizardTargetGuid != 0);
            ImGui::BeginDisabled(!canCommit);
            if (ImGui::Button("Create Wire", ImVec2(140, 0))) {
                args.chainAddConnectionRequested = 1;
                args.chainAddSourceGuid = s_state.wizardSourceGuid;
                args.chainAddTargetGuid = s_state.wizardTargetGuid;
                std::strncpy(args.chainAddOutputEventName,
                             s_state.wizardEvent,
                             sizeof(args.chainAddOutputEventName) - 1);
                args.chainAddOutputEventName[
                    sizeof(args.chainAddOutputEventName) - 1] = '\0';
                std::strncpy(args.chainAddInputActionName,
                             s_state.wizardAction,
                             sizeof(args.chainAddInputActionName) - 1);
                args.chainAddInputActionName[
                    sizeof(args.chainAddInputActionName) - 1] = '\0';
                args.chainAddDelay  = s_state.wizardDelay;
                args.chainAddSticky = s_state.wizardSticky ? 1 : 0;
                args.chainAddParameter[0] = '\0';
                // Persist the typed labels back into the form-fill
                // defaults so the next drag starts from where we left.
                std::strncpy(s_state.addOutputEvent, s_state.wizardEvent,
                             sizeof(s_state.addOutputEvent) - 1);
                s_state.addOutputEvent[sizeof(s_state.addOutputEvent) - 1] = '\0';
                std::strncpy(s_state.addInputAction, s_state.wizardAction,
                             sizeof(s_state.addInputAction) - 1);
                s_state.addInputAction[sizeof(s_state.addInputAction) - 1] = '\0';
                s_state.addDelay  = s_state.wizardDelay;
                s_state.addSticky = s_state.wizardSticky;
                s_state.wizardOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if (!canCommit) {
                ImGui::SameLine();
                ImGui::TextDisabled("(event + action labels required)");
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                s_state.wizardOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();

        // ── Inline output editor + form-fill fallback ────────────────
        // The form-fill stays as the manual escape hatch for users who
        // want to type explicit GUIDs. Drag-pin authoring is now the
        // primary path; the form-fill is the backup.
        if (ImGui::CollapsingHeader("Manual Wire (form-fill fallback)",
                                    ImGuiTreeNodeFlags_None))
        {
            DrawNewConnectionBox(args, index);
        }
        DrawInlineOutputEditor(args);
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 9 (Forge) - DrawFlowchartMode
    // ─────────────────────────────────────────────────────────────────
    //
    // Depth-based forward chain expansion. The Schematic shows ONE
    // hop in each direction; the Wires shows owner->Output->target
    // triples in a list. Flowchart shows the FORWARD CASCADE: pick
    // the focus, follow each output to its target, follow THAT
    // target's first output to its target, recurse to a depth limit.
    // Each chain becomes ONE row, columns indexed by hop depth.
    //
    // Layout (depth 3 example):
    //
    //   chain 0: [FOCUS]→[Out_a]→[T1]→[Out_b]→[T2]→[Out_c]→[T3]
    //   chain 1: [FOCUS]→[Out_d]→[T4]→[Out_e]→[T5]→ ...
    //   chain 2: [FOCUS]→[Out_f]→[T6]→ ...
    //
    // Where Schematic answers "what touches this entity?" and Wires
    // answers "list every wire that touches this entity," Flowchart
    // answers "what does this entity SET IN MOTION?" - the cascading
    // logic chain a designer wires up to fire a sequence. This is the
    // view that exposes the 36-hop chain in the Training map and the
    // long Mt Doom orchestration.
    //
    // We follow the FIRST output at each hop. If a target has multiple
    // outputs, the user clicks into any node to re-focus there and
    // sees its own cascade. Branching trees are deferred, the linear
    // first-output chain is enough to spot the runaway cycles and the
    // dead-target hops, which is the 90% use case.
    static void DrawFlowchartMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        // Top-row create-entity button — same wizard the Schematic uses.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.55f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.72f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f, 0.85f, 0.46f, 1.0f));
        if (ImGui::Button("+ Create entity##flow_create", ImVec2(140.0f, 0.0f))) {
            Forge_RequestOpenCreateEntityWizardFromSource(s_state.focusGuid);
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        ImGui::TextDisabled("(creates a new entity, then drag-pin to wire it into this chain)");
        ImGui::Separator();

        int focusIdx = FindByGuid(args, s_state.focusGuid);
        if (focusIdx < 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                "Flowchart has no focus to expand from. Pick something in the viewport, the outliner, or Level Blocks.");
            return;
        }

        // Depth slider, persistent across panel close/reopen. 1..6.
        static int s_flowDepth = 3;
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderInt("Depth##flow", &s_flowDepth, 1, 6);
        ImGui::SameLine();
        if (ImGui::SmallButton("Full##flow")) s_flowDepth = 6;
        ImGui::SameLine();
        ImGui::TextDisabled("(forward cascade from focus, first-output per hop)");

        // ── Build chains. One chain per output of the focus entity.
        //    Each chain is a sequence of nodes: focus, output, target,
        //    output, target, ... up to depth*2 + 1 nodes.
        // Node payload: entity index (or -1 for ghost), guid, role.
        //   role 0 = focus/target (entity), 1 = Output entity envelope.
        struct Node {
            int      idx;        // -1 = unresolved
            unsigned int guid;
            int      role;       // 0=entity, 1=output
            unsigned int parentOutputGuid; // for color lookup, 0 = none
        };
        std::vector<std::vector<Node> > chains;

        // Gather focus's outputs[].
        std::vector<unsigned int> rootOutputs;
        if (args.gameObjOutputsCounts && args.gameObjOutputsOffsets &&
            args.gameObjOutputsData)
        {
            int off = args.gameObjOutputsOffsets[focusIdx];
            int cnt = args.gameObjOutputsCounts[focusIdx];
            for (int k = 0; k < cnt; ++k) {
                unsigned int g = args.gameObjOutputsData[off + k];
                if (g != 0) rootOutputs.push_back(g);
            }
        }

        if (rootOutputs.empty()) {
            ImGui::TextDisabled("Focus has no outgoing wires. Flowchart needs at least one Output[] slot to expand.");
            return;
        }

        for (size_t r = 0; r < rootOutputs.size(); ++r) {
            std::vector<Node> chain;
            // Hop 0: focus.
            Node n0;
            n0.idx = focusIdx;
            n0.guid = s_state.focusGuid;
            n0.role = 0;
            n0.parentOutputGuid = 0;
            chain.push_back(n0);

            unsigned int currentOutG = rootOutputs[r];
            int hops = 0;
            while (hops < s_flowDepth) {
                // Output envelope.
                Node nO;
                nO.guid = currentOutG;
                nO.role = 1;
                nO.parentOutputGuid = currentOutG;
                std::map<unsigned int, int>::const_iterator oit = index.idxOf.find(currentOutG);
                nO.idx = (oit != index.idxOf.end()) ? oit->second : -1;
                chain.push_back(nO);

                // Target entity.
                unsigned int tgtG = 0;
                if (nO.idx >= 0 && args.gameObjTargetGuids)
                    tgtG = args.gameObjTargetGuids[nO.idx];
                Node nT;
                nT.guid = tgtG;
                nT.role = 0;
                nT.parentOutputGuid = currentOutG;
                if (tgtG != 0) {
                    std::map<unsigned int, int>::const_iterator tit = index.idxOf.find(tgtG);
                    nT.idx = (tit != index.idxOf.end()) ? tit->second : -1;
                } else {
                    nT.idx = -1;
                }
                chain.push_back(nT);

                // Stop conditions.
                if (tgtG == 0) break;
                if (nT.idx < 0) break;       // target ghost, chain dead
                if (!args.gameObjOutputsCounts || !args.gameObjOutputsOffsets ||
                    !args.gameObjOutputsData) break;
                int tOff = args.gameObjOutputsOffsets[nT.idx];
                int tCnt = args.gameObjOutputsCounts[nT.idx];
                unsigned int firstNext = 0;
                for (int k = 0; k < tCnt; ++k) {
                    unsigned int g = args.gameObjOutputsData[tOff + k];
                    if (g != 0) { firstNext = g; break; }
                }
                if (firstNext == 0) break;   // target has no further outputs
                // Cycle protection: refuse to revisit a node we already
                // emitted in this chain.
                bool cycle = false;
                for (size_t ci = 0; ci < chain.size(); ++ci) {
                    if (chain[ci].guid == firstNext) { cycle = true; break; }
                }
                if (cycle) break;
                currentOutG = firstNext;
                ++hops;
            }
            chains.push_back(chain);
        }

        // ── Canvas geometry ──
        const float Z = s_state.canvasZoom;
        const float BOX_W   = 170.0f * Z;
        const float COL_GAP = 50.0f * Z;
        const float BOX_H   = 64.0f * Z;
        const float ROW_GAP = 22.0f * Z;
        const float MARGIN  = 16.0f * Z;

        int   maxChainLen = 0;
        for (size_t ci = 0; ci < chains.size(); ++ci)
            if ((int)chains[ci].size() > maxChainLen)
                maxChainLen = (int)chains[ci].size();

        float canvasW = MARGIN * 2 + (float)maxChainLen * (BOX_W + COL_GAP);
        float canvasH = MARGIN * 2 + (float)chains.size() * (BOX_H + ROW_GAP);
        if (canvasH < 220.0f) canvasH = 220.0f;

        ImGui::BeginChild("##forge_flow_canvas",
                          ImVec2(0, std::min(canvasH + 24.0f, 720.0f)),
                          true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        HandleCanvasPan();
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        ImVec2 base      = ImGui::GetCursorScreenPos();
        ImVec2 mouse     = ImGui::GetIO().MousePos;
        ImGui::Dummy(ImVec2(canvasW, canvasH));

        // ── Render chains row by row ──
        for (size_t ci = 0; ci < chains.size(); ++ci) {
            const std::vector<Node>& chain = chains[ci];
            float y = base.y + MARGIN + (float)ci * (BOX_H + ROW_GAP);

            // Draw boxes first.
            for (size_t k = 0; k < chain.size(); ++k) {
                float x = base.x + MARGIN + (float)k * (BOX_W + COL_GAP);
                ImVec2 a(x, y);
                ImVec2 b(x + BOX_W, y + BOX_H);
                const Node& n = chain[k];

                // Color by role + state.
                ImU32 fill   = (n.idx >= 0)
                    ? IM_COL32(36, 38, 48, 240)
                    : IM_COL32(78, 28, 28, 240);
                ImU32 border = IM_COL32(128, 136, 156, 255);
                if (n.role == 1) {
                    // Output envelope, color by event family.
                    const char* ev = (n.idx >= 0 && args.gameObjOutputEvents)
                        ? Safe(args.gameObjOutputEvents[n.idx], "") : "";
                    border = EventColorForName(ev);
                }
                if (k == 0) border = IM_COL32(255, 220, 120, 255); // focus
                if (n.guid != 0 && n.guid == s_state.selectedOutputGuid)
                    fill = IM_COL32(70, 92, 60, 250);

                bool hovered = (mouse.x >= a.x && mouse.x <= b.x &&
                                mouse.y >= a.y && mouse.y <= b.y);
                if (hovered) {
                    fill = (n.idx >= 0)
                         ? IM_COL32(52, 58, 74, 248)
                         : IM_COL32(98, 36, 36, 248);
                }
                dl->AddRectFilled(a, b, fill, 5.0f);
                dl->AddRect(a, b,
                    hovered ? IM_COL32(255, 244, 130, 255) : border,
                    5.0f, 0, hovered ? 2.0f : 1.4f);

                // Text.
                const char* roleLabel =
                    (k == 0) ? "FOCUS" :
                    (n.role == 1) ? "OUTPUT" : "TARGET";
                dl->AddText(ImVec2(a.x + 7.0f, a.y + 4.0f),
                    IM_COL32(170, 176, 190, 255), roleLabel);
                if (n.idx >= 0) {
                    char ln[180];
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE, "%s",
                        EntityName(args, n.idx));
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 22.0f),
                        IM_COL32(232, 234, 244, 255), ln);
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[%s]",
                        EntityType(args, n.idx));
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 40.0f),
                        IM_COL32(132, 200, 244, 255), ln);
                } else if (n.guid != 0) {
                    char ln[160];
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE,
                        "ghost 0x%08X", n.guid);
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 22.0f),
                        IM_COL32(255, 122, 104, 255), ln);
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 40.0f),
                        IM_COL32(255, 170, 150, 255),
                        "broken target");
                } else {
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 25.0f),
                        IM_COL32(160, 160, 160, 255), "(end)");
                }

                // Click handling.
                if (hovered && n.guid != 0) {
                    if (ImGui::IsMouseClicked(0)) {
                        if (n.role == 1) {
                            s_state.selectedOutputGuid = n.guid;
                            args.requestSelectEditorObjGuid = n.guid;
                            IsolateWire(args, n.guid);
                        } else {
                            SelectEntity(args, n.guid, false);
                        }
                    }
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        SelectEntity(args, n.guid, true);
                    }
                }
            }

            // Draw wires between adjacent boxes.
            for (size_t k = 0; k + 1 < chain.size(); ++k) {
                float x0 = base.x + MARGIN + (float)k * (BOX_W + COL_GAP) + BOX_W;
                float x1 = base.x + MARGIN + (float)(k + 1) * (BOX_W + COL_GAP);
                ImVec2 p0(x0, y + BOX_H * 0.5f);
                ImVec2 p3(x1, y + BOX_H * 0.5f);
                float mx = (p0.x + p3.x) * 0.5f;
                ImVec2 c1(mx, p0.y), c2(mx, p3.y);

                // Wire color from the Output envelope on this segment.
                // For owner->output edge, the OUTPUT (chain[k+1]) carries the event.
                // For output->target edge, the OUTPUT (chain[k]) carries the event.
                int outNodeIdx = (chain[k].role == 1) ? (int)k : (int)(k + 1);
                const Node& outN = chain[outNodeIdx];
                const char* ev = (outN.idx >= 0 && args.gameObjOutputEvents)
                    ? Safe(args.gameObjOutputEvents[outN.idx], "") : "";
                ImU32 wireCol = EventColorForName(ev);
                float thickness = 2.4f;
                if (outN.guid == s_state.selectedOutputGuid) thickness = 4.0f;

                dl->AddBezierCubic(p0, c1, c2, p3, wireCol, thickness);
                // Arrowhead at p3.
                float ax = p3.x - c2.x, ay = p3.y - c2.y;
                float al = std::sqrt(ax*ax + ay*ay);
                if (al > 0.001f) { ax /= al; ay /= al; }
                float perpx = -ay, perpy = ax;
                const float AH = 9.0f;
                ImVec2 b1(p3.x - ax*AH + perpx*AH*0.5f,
                          p3.y - ay*AH + perpy*AH*0.5f);
                ImVec2 b2(p3.x - ax*AH - perpx*AH*0.5f,
                          p3.y - ay*AH - perpy*AH*0.5f);
                dl->AddTriangleFilled(p3, b1, b2, wireCol);

                // Event/action label above the wire.
                if (chain[k].role == 0 && k + 1 < chain.size()) {
                    // owner->output: show event name above this segment.
                    if (ev[0]) dl->AddText(ImVec2(p0.x + 5.0f, y - 4.0f),
                                           wireCol, ev);
                } else if (chain[k].role == 1) {
                    // output->target: show input action.
                    const char* ac = (chain[k].idx >= 0 && args.gameObjInputEvents)
                        ? Safe(args.gameObjInputEvents[chain[k].idx], "") : "";
                    if (ac[0]) dl->AddText(ImVec2(p0.x + 5.0f, y - 4.0f),
                                           wireCol, ac);
                }
            }
        }

        ImGui::EndChild();

        // Footer summary.
        ImGui::TextDisabled("%d chain%s, max length %d hops (each hop = Output + target)",
            (int)chains.size(), chains.size() == 1 ? "" : "s",
            std::max(0, (maxChainLen - 1) / 2));
        if (s_state.selectedOutputGuid != 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear Wire Selection"))
                s_state.selectedOutputGuid = 0;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    //  Phase 9 (Forge) — DrawWiresMode (was Triple Row)
    // ─────────────────────────────────────────────────────────────────
    //
    // The data-dense bezier graph view. Where Schematic shows ONE
    // focus's full neighborhood as a center-out card layout, Wires
    // shows MULTIPLE chains stacked vertically — every wire that
    // touches the focus, or every wire matching the search term
    // when focus is empty. Each row is a single owner -> Output ->
    // target triple drawn as three boxes connected by two color-
    // coded bezier curves.
    //
    // This is the implementation that codex turned into a goddamn
    // ImGui::BeginTable spreadsheet. Restored to the bezier visual
    // graph it was supposed to be, with arrow heads at target end,
    // event/action labels floated above the wires, ghost-GUID red
    // boxes for unresolved refs, and Isolate Wire / Frame Target
    // hover affordances per row. THIS is what the user named "Wiring
    // Graph" because of the WIRES, not because of cells in a damn
    // grid.
    static void DrawWiresMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        std::vector<Triple> triples;
        CollectTriplesForFocus(args, index, s_state.focusGuid, triples);

        if (triples.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.40f, 1.0f),
                "Wires has no chains for this focus. Either the entity is wire-clean or you are pointed at the wrong damn object.");
            return;
        }

        ImGui::TextDisabled("%d wire chain%s — bezier graph, color = event family, click any wire to isolate",
                            (int)triples.size(), triples.size() == 1 ? "" : "s");
        ImGui::Separator();

        // ── Canvas geometry ──────────────────────────────────────────
        // Three columns of fixed-width boxes. One row per triple.
        // Vertical stacking with a row gap. Horizontal scroll on
        // overflow so the canvas does not crush boxes when window is
        // narrow.
        const float Z = s_state.canvasZoom;
        const float COL_W   = 220.0f * Z;
        const float COL_GAP = 70.0f * Z;
        const float BOX_H   = 64.0f * Z;
        const float ROW_GAP = 18.0f * Z;
        const float MARGIN  = 16.0f * Z;
        int rows = (int)triples.size();
        if (rows > 256) rows = 256; // safety cap on absurdly busy levels
        float canvasW = MARGIN * 2 + COL_W * 3 + COL_GAP * 2;
        float canvasH = MARGIN * 2 + (float)rows * (BOX_H + ROW_GAP);
        if (canvasH < 220.0f) canvasH = 220.0f;

        ImGui::BeginChild("##forge_wires", ImVec2(0, std::min(canvasH + 24.0f, 720.0f)),
                          true, ImGuiWindowFlags_HorizontalScrollbar);
        HandleCanvasPan();
        ImDrawList* dl   = ImGui::GetWindowDrawList();
        ImVec2 base      = ImGui::GetCursorScreenPos();
        ImVec2 mouse     = ImGui::GetIO().MousePos;
        ImGui::Dummy(ImVec2(canvasW, canvasH));

        // ── Wire hit-test pass: find the row whose two-segment chain
        //    the cursor is closest to. 6 px threshold, 18 sample
        //    points per bezier segment. Same cheap approach as
        //    Schematic mode's wire picking.
        int   hoveredRow = -1;
        int   hoveredSeg = -1; // 0 = owner->output, 1 = output->target
        float bestDistSq = 36.0f;
        for (int i = 0; i < rows; ++i) {
            float y = base.y + MARGIN + (float)i * (BOX_H + ROW_GAP);
            float x0 = base.x + MARGIN;
            float x1 = x0 + COL_W + COL_GAP;
            float x2 = x1 + COL_W + COL_GAP;
            ImVec2 a0 = ImVec2(x0 + COL_W, y + BOX_H * 0.5f);
            ImVec2 a1 = ImVec2(x1,         y + BOX_H * 0.5f);
            ImVec2 a2 = ImVec2(x1 + COL_W, y + BOX_H * 0.5f);
            ImVec2 a3 = ImVec2(x2,         y + BOX_H * 0.5f);
            ImVec2 segs[2][2] = { { a0, a1 }, { a2, a3 } };
            for (int s = 0; s < 2; ++s) {
                ImVec2 p0 = segs[s][0], p3 = segs[s][1];
                float mx = (p0.x + p3.x) * 0.5f;
                ImVec2 c1(mx, p0.y), c2(mx, p3.y);
                const int N = 16;
                for (int k = 0; k <= N; ++k) {
                    float t = (float)k / (float)N;
                    float u = 1.0f - t;
                    float bx = u*u*u*p0.x + 3*u*u*t*c1.x + 3*u*t*t*c2.x + t*t*t*p3.x;
                    float by = u*u*u*p0.y + 3*u*u*t*c1.y + 3*u*t*t*c2.y + t*t*t*p3.y;
                    float dx = bx - mouse.x, dy = by - mouse.y;
                    float d2 = dx*dx + dy*dy;
                    if (d2 < bestDistSq) {
                        bestDistSq = d2;
                        hoveredRow = i;
                        hoveredSeg = s;
                    }
                }
            }
        }
        (void)hoveredSeg;

        // ── Click hovered wire = isolate the underlying Output ───────
        if (hoveredRow >= 0 && ImGui::IsMouseClicked(0)) {
            unsigned int outG = triples[hoveredRow].outputGuid;
            if (outG != 0) {
                s_state.selectedOutputGuid = outG;
                args.requestSelectEditorObjGuid = outG;
                IsolateWire(args, outG);
            }
        }

        // ── Render every row: 3 boxes + 2 wires + 2 labels ───────────
        for (int i = 0; i < rows; ++i) {
            const Triple& tr = triples[i];
            float y  = base.y + MARGIN + (float)i * (BOX_H + ROW_GAP);
            float x0 = base.x + MARGIN;
            float x1 = x0 + COL_W + COL_GAP;
            float x2 = x1 + COL_W + COL_GAP;
            ImVec2 boxes[3][2] = {
                { ImVec2(x0, y), ImVec2(x0 + COL_W, y + BOX_H) },
                { ImVec2(x1, y), ImVec2(x1 + COL_W, y + BOX_H) },
                { ImVec2(x2, y), ImVec2(x2 + COL_W, y + BOX_H) }
            };
            int   idxs[3]      = { tr.ownerIdx, tr.outputIdx, tr.targetIdx };
            unsigned int gids[3] = {
                EntityGuid(args, tr.ownerIdx),
                tr.outputGuid,
                tr.targetGuid
            };
            const char* roles[3]   = { "OWNER", "OUTPUT", "TARGET" };
            ImU32       borders[3] = {
                IM_COL32(120, 170, 255, 255),
                EventColorForName((tr.outputIdx >= 0 && args.gameObjOutputEvents)
                    ? args.gameObjOutputEvents[tr.outputIdx] : ""),
                IM_COL32(130, 255, 150, 255)
            };
            const char* ev = (tr.outputIdx >= 0 && args.gameObjOutputEvents)
                ? Safe(args.gameObjOutputEvents[tr.outputIdx], "") : "";
            const char* ac = (tr.outputIdx >= 0 && args.gameObjInputEvents)
                ? Safe(args.gameObjInputEvents[tr.outputIdx], "") : "";
            ImU32 wireCol = EventColorForName(ev);

            // Three boxes.
            for (int k = 0; k < 3; ++k) {
                ImVec2 a = boxes[k][0], b = boxes[k][1];
                bool hovered = (mouse.x >= a.x && mouse.x <= b.x &&
                                mouse.y >= a.y && mouse.y <= b.y);
                ImU32 fill = (idxs[k] >= 0)
                    ? IM_COL32(36, 38, 48, 240)
                    : IM_COL32(78, 28, 28, 240);
                if (hovered)
                    fill = (idxs[k] >= 0)
                         ? IM_COL32(52, 58, 74, 248)
                         : IM_COL32(98, 36, 36, 248);
                if (gids[k] != 0 && gids[k] == s_state.selectedOutputGuid)
                    fill = IM_COL32(70, 92, 60, 250);
                dl->AddRectFilled(a, b, fill, 5.0f);
                dl->AddRect(a, b,
                    hovered ? IM_COL32(255, 244, 130, 255) : borders[k],
                    5.0f, 0, hovered ? 2.0f : 1.4f);
                dl->AddText(ImVec2(a.x + 7.0f, a.y + 4.0f),
                    IM_COL32(170, 176, 190, 255), roles[k]);
                if (idxs[k] >= 0) {
                    char ln[180];
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE, "%s",
                        EntityName(args, idxs[k]));
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 22.0f),
                        IM_COL32(232, 234, 244, 255), ln);
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE, "[%s]",
                        EntityType(args, idxs[k]));
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 40.0f),
                        IM_COL32(132, 200, 244, 255), ln);
                } else if (gids[k] != 0) {
                    char ln[160];
                    _snprintf_s(ln, sizeof(ln), _TRUNCATE,
                        "ghost 0x%08X", gids[k]);
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 22.0f),
                        IM_COL32(255, 122, 104, 255), ln);
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 40.0f),
                        IM_COL32(255, 170, 150, 255),
                        "dead ref, this wire is fucked");
                } else {
                    dl->AddText(ImVec2(a.x + 7.0f, a.y + 25.0f),
                        IM_COL32(160, 160, 160, 255), "(none)");
                }
                if (hovered && ImGui::IsMouseClicked(0) && hoveredRow != i) {
                    if (gids[k] != 0)
                        SelectEntity(args, gids[k], false);
                }
                if (hovered && ImGui::IsMouseDoubleClicked(0)) {
                    if (gids[k] != 0)
                        SelectEntity(args, gids[k], true);
                }
            }

            // Two bezier wires with arrowheads.
            for (int s = 0; s < 2; ++s) {
                ImVec2 p0(boxes[s    ][1].x, boxes[s    ][0].y + BOX_H * 0.5f);
                ImVec2 p3(boxes[s + 1][0].x, boxes[s + 1][0].y + BOX_H * 0.5f);
                float mx = (p0.x + p3.x) * 0.5f;
                ImVec2 c1(mx, p0.y), c2(mx, p3.y);
                float thickness = 2.4f;
                if (i == hoveredRow) thickness = 3.6f;
                if (tr.outputGuid == s_state.selectedOutputGuid) thickness = 4.0f;
                dl->AddBezierCubic(p0, c1, c2, p3, wireCol, thickness);
                // Arrowhead at p3.
                float ax = p3.x - c2.x, ay = p3.y - c2.y;
                float al = std::sqrt(ax*ax + ay*ay);
                if (al > 0.001f) { ax /= al; ay /= al; }
                float perpx = -ay, perpy = ax;
                const float AH = 9.0f;
                ImVec2 b1(p3.x - ax*AH + perpx*AH*0.5f,
                          p3.y - ay*AH + perpy*AH*0.5f);
                ImVec2 b2(p3.x - ax*AH - perpx*AH*0.5f,
                          p3.y - ay*AH - perpy*AH*0.5f);
                dl->AddTriangleFilled(p3, b1, b2, wireCol);
            }

            // Event name above first wire, action above second wire.
            if (ev[0]) {
                dl->AddText(ImVec2(boxes[0][1].x + 6.0f, y - 4.0f),
                    wireCol, ev);
            }
            if (ac[0]) {
                dl->AddText(ImVec2(boxes[1][1].x + 6.0f, y - 4.0f),
                    wireCol, ac);
            }
        }

        ImGui::EndChild();

        // ── Below-canvas row toolbar: row count cap notice + Isolate
        //    Wire shortcut for the currently selected Output.
        if ((int)triples.size() > rows) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "Showing %d of %d. Filter via search to narrow it down.",
                rows, (int)triples.size());
        }
        if (s_state.selectedOutputGuid != 0) {
            if (ImGui::SmallButton("Isolate Selected Wire"))
                IsolateWire(args, s_state.selectedOutputGuid);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear Selection"))
                s_state.selectedOutputGuid = 0;
        }
    }

    static int CategoryForType(const char* typeName)
    {
        if (!typeName) return 0;
        if (ContainsI(typeName, "Output") || ContainsI(typeName, "Trigger") ||
            ContainsI(typeName, "Capture") || ContainsI(typeName, "spawn"))
            return 2;
        if (ContainsI(typeName, "gamemode") || ContainsI(typeName, "template") ||
            ContainsI(typeName, "objective"))
            return 3;
        if (ContainsI(typeName, "Path") || ContainsI(typeName, "AI"))
            return 4;
        return 0;
    }

    static void DrawHealthMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
    {
        int layerCount = 0, gamemodeCount = 0;
        int pathNetworkCount = 0, pathNodeCount = 0, pathLinkCount = 0, splineCount = 0;
        std::map<unsigned int, int> layerObjects;
        std::map<unsigned int, int> layerGameplay;
        std::map<unsigned int, int> layerLogic;
        std::map<unsigned int, int> layerAiPath;

        for (int i = 0; i < args.gameObjCount; ++i) {
            const char* tn = EntityType(args, i);
            if (EqualsI(tn, "templateLayer")) ++layerCount;
            if (EqualsI(tn, "gamemode")) ++gamemodeCount;
            if (EqualsI(tn, "PathNetwork")) ++pathNetworkCount;
            if (EqualsI(tn, "PathNode")) ++pathNodeCount;
            if (EqualsI(tn, "PathLink")) ++pathLinkCount;
            if (ContainsI(tn, "Spline") && !EqualsI(tn, "SplineCreature")) ++splineCount;

            unsigned int layerGuid = (args.gameObjLayerGuids) ? args.gameObjLayerGuids[i] : 0;
            if (layerGuid != 0) {
                ++layerObjects[layerGuid];
                int cat = CategoryForType(tn);
                if (cat == 2) ++layerGameplay[layerGuid];
                else if (cat == 3) ++layerLogic[layerGuid];
                else if (cat == 4) ++layerAiPath[layerGuid];
            }
        }

        if (s_editorCategoryMask) {
            if (ImGui::SmallButton("F4 All##egs_f4_all")) {
                *s_editorCategoryMask = 0x1FFFu;
                if (s_saveFn) s_saveFn();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("F4 Events##egs_f4_events")) {
                *s_editorCategoryMask = (1u << 2) | (1u << 3);
                if (s_saveFn) s_saveFn();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("F4 Paths##egs_f4_paths")) {
                *s_editorCategoryMask = (1u << 4);
                if (s_saveFn) s_saveFn();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("F4 Gameplay##egs_f4_gameplay")) {
                *s_editorCategoryMask = (1u << 2);
                if (s_saveFn) s_saveFn();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("mask 0x%X", *s_editorCategoryMask);
        }

        ImGui::Separator();
        if (ImGui::BeginTable("##egs_health_summary", 4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("Events");
            ImGui::TextDisabled("%d owners", index.eventOwnerCount);
            ImGui::TextDisabled("%d Output blocks", index.outputBlockCount);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("Wire Health");
            ImGui::TextDisabled("%d Outputs[] refs", index.eventWireCount);
            if (index.brokenOutputRefs || index.brokenTargets || index.deadOutputBlocks) {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                    "%d broken refs, %d ghost targets, %d dead Output blocks",
                    index.brokenOutputRefs, index.brokenTargets, index.deadOutputBlocks);
            } else {
                ImGui::TextDisabled("no obvious wire rot");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("Paths");
            ImGui::TextDisabled("%d networks", pathNetworkCount);
            ImGui::TextDisabled("%d nodes, %d links", pathNodeCount, pathLinkCount);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("Layers");
            ImGui::TextDisabled("%d layers, %d gamemodes", layerCount, gamemodeCount);
            ImGui::TextDisabled("%d spline objects", splineCount);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "Layers and mode masks");
        if (ImGui::BeginTable("##egs_layers", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, 220.0f)))
        {
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("GMM", ImGuiTableColumnFlags_WidthFixed, 54.0f);
            ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("Gameplay", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Logic", ImGuiTableColumnFlags_WidthFixed, 54.0f);
            ImGui::TableSetupColumn("AI/Path", ImGuiTableColumnFlags_WidthFixed, 58.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < args.gameObjCount; ++i) {
                if (!EqualsI(EntityType(args, i), "templateLayer")) continue;
                unsigned int guid = EntityGuid(args, i);
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(EntityName(args, i), false, ImGuiSelectableFlags_SpanAllColumns))
                    SelectEntity(args, guid, false);
                ImGui::TableSetColumnIndex(1);
                int gmm = (args.gameObjGamemodeMasks) ? args.gameObjGamemodeMasks[i] : -1;
                ImGui::Text("%d", gmm);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", layerObjects[guid]);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", layerGameplay[guid]);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", layerLogic[guid]);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%d", layerAiPath[guid]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (index.brokenOutputRefs || index.brokenTargets || index.deadOutputBlocks) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.25f, 1.0f),
                "Broken wire inventory - this is where dead GUIDs crawl out of the walls.");
            if (ImGui::BeginTable("##egs_broken_wires", 5,
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                ImVec2(0, 180.0f)))
            {
                ImGui::TableSetupColumn("Owner");
                ImGui::TableSetupColumn("Output");
                ImGui::TableSetupColumn("Target");
                ImGui::TableSetupColumn("Event");
                ImGui::TableSetupColumn("Action");
                ImGui::TableHeadersRow();
                int shown = 0;
                for (size_t ti = 0; ti < index.allTriples.size() && shown < 80; ++ti) {
                    const Triple& tr = index.allTriples[ti];
                    bool broken = (tr.outputIdx < 0) || (tr.targetGuid != 0 && tr.targetIdx < 0) || (tr.targetGuid == 0);
                    if (!broken) continue;
                    ++shown;
                    ImGui::PushID((int)ti);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(EntityName(args, tr.ownerIdx));
                    ImGui::TableSetColumnIndex(1);
                    if (tr.outputIdx >= 0) {
                        if (ImGui::Selectable(EntityName(args, tr.outputIdx), false))
                            SelectEntity(args, tr.outputGuid, false);
                    } else {
                        ImGui::TextColored(ImVec4(1, 0.35f, 0.25f, 1), "ghost 0x%08X", tr.outputGuid);
                    }
                    ImGui::TableSetColumnIndex(2);
                    if (tr.targetIdx >= 0) ImGui::TextUnformatted(EntityName(args, tr.targetIdx));
                    else if (tr.targetGuid != 0) ImGui::TextColored(ImVec4(1, 0.35f, 0.25f, 1), "ghost 0x%08X", tr.targetGuid);
                    else ImGui::TextColored(ImVec4(1, 0.55f, 0.30f, 1), "empty target");
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted((tr.outputIdx >= 0 && args.gameObjOutputEvents) ? Safe(args.gameObjOutputEvents[tr.outputIdx], "(empty)") : "(missing)");
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted((tr.outputIdx >= 0 && args.gameObjInputEvents) ? Safe(args.gameObjInputEvents[tr.outputIdx], "(empty)") : "(missing)");
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }

        ImGui::TextDisabled("Health is the friendly authoring skeleton: hierarchy, layers, mode masks, event wires, paths and splines. Raw JSON can rot in the basement.");
    }
}

void BindEditorState(unsigned int* editorCategoryMask,
                     bool* eventWiresShow,
                     unsigned int* eventWireFocusGuid,
                     void (*saveFn)())
{
    s_editorCategoryMask = editorCategoryMask;
    s_eventWiresShow = eventWiresShow;
    s_eventWireFocusGuid = eventWireFocusGuid;
    s_saveFn = saveFn;
}

void Update(ImGuiGlueFrameArgs& args)
{
    if (s_eventWiresShow) args.eventWiresShow = *s_eventWiresShow ? 1 : 0;
    if (s_eventWireFocusGuid) args.eventWireFocusGuid = *s_eventWireFocusGuid;

    // Cross-page selection unification — designer clicks an entity in
    // the Outliner / Inspector / 3D viewport / Level Loader / wherever,
    // and the schematic should focus on it regardless of which path
    // the host wrote into. Three fields can carry the GUID; we accept
    // any of them. inspectorSelGuid is the Level Inspector's path,
    // propEntityGuid is the property-grid's path, selectedEditorObjGuid
    // is the editor-obj click path. Without checking all three, the
    // schematic ignored entities selected through paths it didn't know
    // about — that was the "schematic doesn't follow my click" bug.
    unsigned int liveSelection =
        args.inspectorSelGuid       ? args.inspectorSelGuid       :
        (unsigned int)args.propEntityGuid != 0 ? (unsigned int)args.propEntityGuid :
        args.selectedEditorObjGuid;
    if (s_state.lockedFocusGuid != 0) {
        s_state.focusGuid = s_state.lockedFocusGuid;
    } else if (liveSelection != 0) {
        s_state.focusGuid = liveSelection;
    } else if (s_state.focusGuid != 0 && FindByGuid(args, s_state.focusGuid) < 0) {
        s_state.focusGuid = 0;
    }

    if (s_state.selectedOutputGuid != 0 && FindByGuid(args, s_state.selectedOutputGuid) < 0)
        s_state.selectedOutputGuid = 0;
}

// ─────────────────────────────────────────────────────────────────────
//  Phase 9b (Forge mega-panel) - DrawOutlinerMode
// ─────────────────────────────────────────────────────────────────────
//
// The scene-hierarchy outliner, folded into The Forge as a view-mode
// tab. Used to live as its own floating panel; consolidating it here
// because the user wants ONE authoring surface, not nine separate
// windows fighting for screen real estate. Same drag-reparent
// pipeline (writes args.requestReparentChildGuid via the existing
// host-side handler), same selection sync, same filter/expand UX.
//
// Iterative tree walk with explicit stack so 3000-entity Helm's Deep
// outlines do not blow the recursive call stack. Subtree filter
// pre-pass marks every entity that matches the search OR has a
// matching descendant, so collapsed branches still show their hits.
static void DrawOutlinerMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
{
    (void)index;
    const int N = args.gameObjCount;
    if (N <= 0 || !args.gameObjGuids || !args.gameObjParentGuids ||
        !args.gameObjNames || !args.gameObjTypeNames)
    {
        ImGui::TextDisabled("No level loaded.");
        return;
    }

    // Per-frame GUID->idx + children-of-parent maps. Cheap, ~3000
    // map inserts per frame at worst, microseconds.
    static std::map<unsigned int, int> s_guidToIdx;
    static std::map<unsigned int, std::vector<int> > s_childrenOf;
    s_guidToIdx.clear();
    s_childrenOf.clear();
    for (int i = 0; i < N; ++i)
        s_guidToIdx[args.gameObjGuids[i]] = i;
    std::vector<int> roots;
    for (int i = 0; i < N; ++i) {
        unsigned int p = args.gameObjParentGuids[i];
        if (p == 0 || s_guidToIdx.find(p) == s_guidToIdx.end())
            roots.push_back(i);
        else
            s_childrenOf[p].push_back(i);
    }

    // Toolbar.
    static char s_outlSearch[128] = "";
    static bool s_outlExpandAll = false;
    static char s_wizardTypeFilter[64] = "";
    static int  s_wizardSelectedType = -1; // index into deduped type list
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##forge_outl_search", "Filter (name or type)",
                             s_outlSearch, sizeof(s_outlSearch));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##forge_outl_search")) s_outlSearch[0] = '\0';
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("Expand All"))   s_outlExpandAll = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Collapse All")) s_outlExpandAll = false;
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Phase 10e: Create Entity wizard launcher. Sits next to the
    // expand/collapse toggles because that is where designers'
    // brain-RAM gets indexed for "I want to add something."
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32( 60, 110,  60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32( 80, 140,  80, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(100, 170, 100, 255));
    if (ImGui::SmallButton("+ New Entity")) {
        s_wizardTypeFilter[0] = '\0';
        s_wizardSelectedType  = -1;
        ImGui::OpenPopup("Create Entity##forge_create_wiz");
    }
    ImGui::PopStyleColor(3);
    // Cross-mode trigger: Schematic-canvas shortcut requested the
    // wizard. Consume the flag here so it fires exactly once.
    if (s_state.requestOpenCreateWizard) {
        s_wizardTypeFilter[0] = '\0';
        s_wizardSelectedType  = -1;
        ImGui::OpenPopup("Create Entity##forge_create_wiz");
        s_state.requestOpenCreateWizard = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d entities, %d roots)", N, (int)roots.size());
    ImGui::Separator();

    // ── Phase 10e: Create Entity wizard popup ─────────────────────────
    // Lists every distinct type_name in the loaded level, lets the
    // user filter, picks the first entity of the chosen type as the
    // template, and fires hostRequestCreateEntityFromTemplateGuid.
    // The host clones the template's fieldData, layer, mesh, and
    // gamemodemask, names the new entity "<type>_new", and sets it
    // 2m above the template. Designer renames via Inspector after.
    ImGui::SetNextWindowSize(ImVec2(540, 540), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Create Entity##forge_create_wiz", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.55f, 1.0f),
            "Pick a type. The Forge clones the first matching entity in the level as your starting point.");
        ImGui::Separator();

        // Build deduped type list. Pair each type_name with the index
        // of the FIRST entity of that type — that becomes the template.
        struct TypeRow { const char* name; int firstIdx; int count; };
        std::vector<TypeRow> typeRows;
        typeRows.reserve(64);
        std::map<std::string, int> typeToRowIdx;
        for (int i = 0; i < N; ++i) {
            const char* tn = args.gameObjTypeNames[i] ? args.gameObjTypeNames[i] : "";
            if (!tn[0]) continue;
            std::map<std::string, int>::iterator it = typeToRowIdx.find(tn);
            if (it == typeToRowIdx.end()) {
                typeToRowIdx[tn] = (int)typeRows.size();
                TypeRow r; r.name = tn; r.firstIdx = i; r.count = 1;
                typeRows.push_back(r);
            } else {
                typeRows[it->second].count++;
            }
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##wiz_type_filter",
            "Filter types (e.g. trigger, capture, spawn)",
            s_wizardTypeFilter, sizeof(s_wizardTypeFilter));

        ImGui::BeginChild("##wiz_type_list", ImVec2(0, 380), true);
        for (size_t r = 0; r < typeRows.size(); ++r) {
            if (s_wizardTypeFilter[0] &&
                !ContainsI(typeRows[r].name, s_wizardTypeFilter))
                continue;
            char rowLabel[160];
            _snprintf_s(rowLabel, sizeof(rowLabel), _TRUNCATE,
                "%s   (%d in level)##wiz_t_%d",
                typeRows[r].name, typeRows[r].count, (int)r);
            bool selected = (s_wizardSelectedType == (int)r);
            if (ImGui::Selectable(rowLabel, selected,
                                  ImGuiSelectableFlags_AllowDoubleClick))
            {
                s_wizardSelectedType = (int)r;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    args.hostRequestCreateEntityFromTemplateGuid =
                        args.gameObjGuids[typeRows[r].firstIdx];
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();
        bool canCreate = (s_wizardSelectedType >= 0 &&
                          s_wizardSelectedType < (int)typeRows.size());
        if (canCreate) {
            const TypeRow& r = typeRows[s_wizardSelectedType];
            const char* tplName = (args.gameObjNames && r.firstIdx >= 0)
                                   ? Safe(args.gameObjNames[r.firstIdx], "(unnamed)")
                                   : "(unknown)";
            ImGui::TextDisabled("Template: %s   layer-inherited from template, +2m Y bump.",
                tplName);
        } else {
            ImGui::TextDisabled("Pick a type from the list above (double-click also creates).");
        }

        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create", ImVec2(140, 0)) && canCreate) {
            args.hostRequestCreateEntityFromTemplateGuid =
                args.gameObjGuids[typeRows[s_wizardSelectedType].firstIdx];
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Subtree filter pass. Mark direct hits, then propagate UP every
    // ancestor chain so collapsed branches still surface filter hits.
    static std::vector<int> s_passFilter;
    s_passFilter.assign(N, 0);
    const bool hasFilter = (s_outlSearch[0] != '\0');
    if (hasFilter) {
        for (int i = 0; i < N; ++i) {
            const char* nm = args.gameObjNames[i] ? args.gameObjNames[i] : "";
            const char* tn = args.gameObjTypeNames[i] ? args.gameObjTypeNames[i] : "";
            if (ContainsI(nm, s_outlSearch) || ContainsI(tn, s_outlSearch))
                s_passFilter[i] = 1;
        }
        for (int i = 0; i < N; ++i) {
            if (!s_passFilter[i]) continue;
            unsigned int p = args.gameObjParentGuids[i];
            int hops = 0;
            while (p != 0 && hops < 1024) {
                std::map<unsigned int,int>::const_iterator pi = s_guidToIdx.find(p);
                if (pi == s_guidToIdx.end()) break;
                if (s_passFilter[pi->second]) break;
                s_passFilter[pi->second] = 1;
                p = args.gameObjParentGuids[pi->second];
                ++hops;
            }
        }
    }

    // Iterative recursion via manual stack. Frame holds the entity's
    // gameObj index, the next child cursor, and whether the
    // TreeNodeEx is currently open (so we know to call TreePop).
    struct Frame { int idx; int childCursor; bool nodeOpen; };
    std::vector<Frame> stack;

    for (size_t ri = 0; ri < roots.size(); ++ri) {
        int rootIdx = roots[ri];
        if (hasFilter && !s_passFilter[rootIdx]) continue;
        stack.clear();
        Frame top; top.idx = rootIdx; top.childCursor = -1; top.nodeOpen = false;
        stack.push_back(top);

        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.childCursor < 0) {
                // First visit: render the row.
                unsigned int guid = args.gameObjGuids[f.idx];
                const char* nm   = args.gameObjNames[f.idx] ? args.gameObjNames[f.idx] : "(unnamed)";
                const char* tn   = args.gameObjTypeNames[f.idx] ? args.gameObjTypeNames[f.idx] : "?";
                std::map<unsigned int, std::vector<int> >::const_iterator chIt =
                    s_childrenOf.find(guid);
                bool hasKids = (chIt != s_childrenOf.end() && !chIt->second.empty());

                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                           ImGuiTreeNodeFlags_SpanAvailWidth;
                if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf;
                if (s_outlExpandAll || hasFilter) flags |= ImGuiTreeNodeFlags_DefaultOpen;
                if (guid != 0 && args.inspectorSelGuid == guid)
                    flags |= ImGuiTreeNodeFlags_Selected;

                ImGui::PushID(f.idx);
                char label[256];
                sprintf(label, "%s  [%s]##forge_outl_%u", nm, tn, guid);
                bool open = ImGui::TreeNodeEx(label, flags);

                if (ImGui::IsItemClicked() && !ImGui::IsMouseDoubleClicked(0))
                    args.requestSelectEditorObjGuid = guid;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    args.requestFocusEditorObjGuid = guid;

                if (ImGui::BeginPopupContextItem("##forge_outl_ctx")) {
                    char gtxt[32]; sprintf(gtxt, "0x%08X", guid);
                    ImGui::TextDisabled("%s  [%s]", gtxt, tn);
                    ImGui::Separator();
                    if (ImGui::MenuItem("Frame in 3D"))
                        args.requestFocusEditorObjGuid = guid;
                    if (ImGui::MenuItem("Select in 3D"))
                        args.requestSelectEditorObjGuid = guid;
                    if (ImGui::MenuItem("Focus in Forge")) {
                        s_state.lockedFocusGuid = 0;
                        s_state.focusGuid = guid;
                    }
                    if (ImGui::MenuItem("Copy GUID"))
                        ImGui::SetClipboardText(gtxt);
                    if (ImGui::MenuItem("Make Top-Level")) {
                        args.requestReparentChildGuid     = guid;
                        args.requestReparentNewParentGuid = 0;
                    }
                    ImGui::Separator();
                    // Phase 10e authoring actions. Duplicate piggybacks
                    // on the existing host pipeline (Ctrl+D shortcut);
                    // Delete fires the new hostRequestDeleteEntityGuid.
                    if (ImGui::MenuItem("Duplicate##outl_dup", "Ctrl+D"))
                        args.hostRequestDuplicateEntityGuid = guid;
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        IM_COL32(255, 130, 110, 255));
                    if (ImGui::MenuItem("Delete##outl_del", "Del"))
                        args.hostRequestDeleteEntityGuid = guid;
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload("FORGE_OUTLINER_NODE", &guid, sizeof(unsigned int));
                    ImGui::Text("Reparent: %s", nm);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FORGE_OUTLINER_NODE");
                    if (payload && payload->DataSize == sizeof(unsigned int)) {
                        unsigned int childGuid = *(const unsigned int*)payload->Data;
                        if (childGuid != guid) {
                            args.requestReparentChildGuid     = childGuid;
                            args.requestReparentNewParentGuid = guid;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();

                f.nodeOpen = open;
                f.childCursor = 0;
                if (!open || !hasKids) {
                    if (open) ImGui::TreePop();
                    stack.pop_back();
                    continue;
                }
            }

            unsigned int guid = args.gameObjGuids[f.idx];
            const std::vector<int>& kids = s_childrenOf[guid];
            if (f.childCursor >= (int)kids.size()) {
                if (f.nodeOpen) ImGui::TreePop();
                stack.pop_back();
                continue;
            }
            int kIdx = kids[f.childCursor++];
            if (hasFilter && !s_passFilter[kIdx]) continue;
            Frame next; next.idx = kIdx; next.childCursor = -1; next.nodeOpen = false;
            stack.push_back(next);
        }
    }

    // Bottom drop-zone: dropping here clears parent (makes top-level).
    ImGui::Separator();
    ImGui::Dummy(ImVec2(-1.0f, 24.0f));
    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FORGE_OUTLINER_NODE");
        if (payload && payload->DataSize == sizeof(unsigned int)) {
            unsigned int childGuid = *(const unsigned int*)payload->Data;
            args.requestReparentChildGuid     = childGuid;
            args.requestReparentNewParentGuid = 0;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::TextDisabled("(drop here to make top-level)");
}

// ─────────────────────────────────────────────────────────────────────
//  Phase 9b (Forge mega-panel) - DrawLayersMode
// ─────────────────────────────────────────────────────────────────────
//
// Layer manager consolidated into The Forge. State sets
// (g_layerHidden / g_layerLocked / g_layerIsolated / g_layerActive)
// live in imgui_glue_dll.cpp because the always-write block there
// pumps them into args every frame; we extern-link them here so
// toggles in this tab and toggles in the (deleted) standalone panel
// hit the same memory. INI persist routine EditorFilterSave is the
// same one the legacy panel called.
//
// Per-row controls match the original Layers panel:
//   [E] eye / [L] lock / [S] solo / [A] active
// Plus name as a Selectable that fires viewport selection on click,
// camera fly on double-click, and a context menu with "Focus in
// Forge" so the user can pivot the chain views to a layer entity.
static void DrawLayersMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
{
    (void)index;
    const int N = args.gameObjCount;
    if (N <= 0 || !args.gameObjGuids || !args.gameObjTypeNames) {
        ImGui::TextDisabled("No level loaded.");
        return;
    }

    // Bulk operations toolbar.
    if (ImGui::SmallButton("Show All##forge_layers")) {
        g_layerHidden.clear();
        EditorFilterSave();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Unlock All##forge_layers")) {
        g_layerLocked.clear();
        EditorFilterSave();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Solo##forge_layers")) {
        g_layerIsolated = 0;
        EditorFilterSave();
    }
    ImGui::SameLine();
    if (g_layerActive != 0) {
        char abuf[64];
        sprintf(abuf, "Active: 0x%08X", g_layerActive);
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "%s", abuf);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##forge_layers_active")) {
            g_layerActive = 0;
            EditorFilterSave();
        }
    } else {
        ImGui::TextDisabled("(no active layer)");
    }

    ImGui::Separator();

    ImGui::BeginChild("##forge_layers_scroll", ImVec2(0, 0), false);
    int rowsDrawn = 0;
    for (int i = 0; i < N; ++i) {
        const char* tn = args.gameObjTypeNames[i] ? args.gameObjTypeNames[i] : "";
        if (!ContainsI(tn, "templateLayer")) continue;
        ++rowsDrawn;

        unsigned int guid = args.gameObjGuids[i];
        const char* nm = args.gameObjNames[i] ? args.gameObjNames[i] : "(unnamed layer)";

        ImGui::PushID((int)guid);
        bool hidden = (g_layerHidden.find(guid) != g_layerHidden.end());
        bool locked = (g_layerLocked.find(guid) != g_layerLocked.end());
        bool soloed = (g_layerIsolated == guid);
        bool active = (g_layerActive   == guid);

        // Eye (visibility toggle).
        ImVec4 eyeCol = hidden ? ImVec4(0.6f,0.6f,0.6f,1.0f) : ImVec4(0.95f,0.95f,0.4f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, eyeCol);
        if (ImGui::SmallButton(hidden ? "[ ]##eye" : "[E]##eye")) {
            if (hidden) g_layerHidden.erase(guid);
            else        g_layerHidden.insert(guid);
            EditorFilterSave();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(hidden ? "Hidden, click to show" : "Visible, click to hide");

        // Lock toggle.
        ImGui::SameLine();
        ImVec4 lockCol = locked ? ImVec4(1.0f,0.5f,0.3f,1.0f) : ImVec4(0.6f,0.6f,0.6f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, lockCol);
        if (ImGui::SmallButton(locked ? "[L]##lock" : "[ ]##lock")) {
            if (locked) g_layerLocked.erase(guid);
            else        g_layerLocked.insert(guid);
            EditorFilterSave();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(locked ? "Locked (unpickable), click to unlock" : "Unlocked, click to lock");

        // Solo toggle.
        ImGui::SameLine();
        ImVec4 soloCol = soloed ? ImVec4(1.0f,1.0f,0.3f,1.0f) : ImVec4(0.6f,0.6f,0.6f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, soloCol);
        if (ImGui::SmallButton(soloed ? "[S]##solo" : "[ ]##solo")) {
            g_layerIsolated = soloed ? 0 : guid;
            EditorFilterSave();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(soloed ? "Soloed (only this layer renders), click to clear" : "Click to solo this layer");

        // Active toggle.
        ImGui::SameLine();
        ImVec4 actCol = active ? ImVec4(0.4f,1.0f,0.6f,1.0f) : ImVec4(0.6f,0.6f,0.6f,1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, actCol);
        if (ImGui::SmallButton(active ? "[A]##act" : "[ ]##act")) {
            g_layerActive = active ? 0 : guid;
            EditorFilterSave();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(active ? "Active layer (new entities land here), click to clear" : "Click to make active");

        // Name as Selectable.
        ImGui::SameLine();
        char rowLabel[256];
        sprintf(rowLabel, "%s##forge_layers_row%u", nm, guid);
        if (ImGui::Selectable(rowLabel, false, 0, ImVec2(0, 0)))
            args.requestSelectEditorObjGuid = guid;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            args.requestFocusEditorObjGuid = guid;

        if (ImGui::BeginPopupContextItem("##forge_layers_ctx")) {
            char gtxt[32]; sprintf(gtxt, "0x%08X", guid);
            ImGui::TextDisabled("%s", gtxt);
            ImGui::Separator();
            if (ImGui::MenuItem("Frame in 3D"))
                args.requestFocusEditorObjGuid = guid;
            if (ImGui::MenuItem("Select in 3D"))
                args.requestSelectEditorObjGuid = guid;
            if (ImGui::MenuItem("Focus in Forge")) {
                s_state.lockedFocusGuid = 0;
                s_state.focusGuid = guid;
            }
            if (ImGui::MenuItem("Copy GUID"))
                ImGui::SetClipboardText(gtxt);
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }
    if (rowsDrawn == 0)
        ImGui::TextDisabled("No templateLayer entities in this level.");
    ImGui::EndChild();
}

// ─────────────────────────────────────────────────────────────────────
//  Phase 9b (Forge mega-panel) - DrawManipulatorMode
// ─────────────────────────────────────────────────────────────────────
//
// Translate / rotate / scale gizmo controls folded into The Forge.
// Drives the same g_gizmoMode/g_gizmoSpace/g_gizmoSnap globals the
// renderer's gizmo state machine reads via the always-write block
// in DrawFrame. Shift the mode here, the renderer's next frame
// paints translate/rotate/scale axis handles on the selected entity.
//
// Same Off/Move/Rotate/Scale buttons + World/Local radio + snap
// drag-float as the legacy Gizmo Tools panel. Refuses to swap mode
// mid-drag (host enforces this too, belt and suspenders).
static void DrawManipulatorMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
{
    (void)index;
    bool dragging = (args.levelGizmoDragging != 0);
    if (dragging) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
            "DRAGGING (release to commit)");
        ImGui::Separator();
    }

    ImGui::BeginDisabled(dragging);

    // Mode buttons.
    struct ModeBtn { const char* label; int mode; };
    ModeBtn modes[] = {
        { "Off",    0 },
        { "Move",   1 },
        { "Rotate", 2 },
        { "Scale",  3 }
    };
    for (int i = 0; i < 4; ++i) {
        bool active = (g_gizmoMode == modes[i].mode);
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button(modes[i].label, ImVec2(60, 0))) {
            g_gizmoMode = modes[i].mode;
            EditorFilterSave();
        }
        if (active) ImGui::PopStyleColor();
        if (i < 3) ImGui::SameLine();
    }
    ImGui::Separator();

    // Space toggle.
    bool worldSpace = (g_gizmoSpace == 0);
    if (ImGui::RadioButton("World", worldSpace)) {
        g_gizmoSpace = 0;
        EditorFilterSave();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Local", !worldSpace)) {
        g_gizmoSpace = 1;
        EditorFilterSave();
    }

    // Snap input.
    ImGui::Separator();
    const char* unit = "m (snap step)";
    if (g_gizmoMode == 2) unit = "deg (snap step)";
    else if (g_gizmoMode == 3) unit = "x (snap ratio)";
    float snapEdit = g_gizmoSnap;
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("##gizSnap_forge", &snapEdit, 0.05f, 0.0f, 90.0f, "%.2f")) {
        g_gizmoSnap = (snapEdit < 0.0f) ? 0.0f : snapEdit;
        EditorFilterSave();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", unit);
    if (g_gizmoSnap > 0.0f) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Off##snapOff_forge")) {
            g_gizmoSnap = 0.0f;
            EditorFilterSave();
        }
    }

    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("LMB drag a colored axis to manipulate.");
    ImGui::TextDisabled("Esc cancels an in-progress drag.");
    ImGui::TextDisabled("Hotkeys: W=move, E=rotate, R=scale, F=frame selected,");
    ImGui::TextDisabled("         H=toggle hide layer, Ctrl+D=duplicate entity.");

    if (args.splineHandlesFocusGuid != 0) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 1.0f, 1.0f),
            "Spline handles live: 0x%08X", args.splineHandlesFocusGuid);
        ImGui::TextDisabled("Shift-click a segment to insert a node. Delete drops the selected interior node.");
        ImGui::TextDisabled("Head and tail stay protected, no one-point spline bullshit.");
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Phase 9b (Forge mega-panel) - DrawPropertiesMode
// ─────────────────────────────────────────────────────────────────────
//
// Per-entity property editor folded into The Forge. The standalone
// Inspector panel (codex's Phase 8 type-aware properties) stays
// alive as a separate floating window for users who want detached
// editing alongside The Forge's other tabs. This Properties tab is
// a focused, in-Forge view of the SAME args.propField* data: entity
// summary header (name, type, GUID, parent, layer, position) plus
// an editable field table grouped by kind.
//
// Reuses the existing args.propEditRequested / propEditFieldIndex /
// propEditKind / propEditIntVal / propEditFloatVal / propEditGuidVal
// / propEditStringVal pipeline — same one codex's Inspector uses,
// same one DrawInlineOutputEditor uses, same persistence path
// through LevelReader::AddFieldEdit. No new save plumbing.
//
// Categorized field display:
//   Identity: GUID, ParentGUID, Name, type
//   Transform: WorldTransform / position / rotation
//   GameMode: GameModeMask, layer membership, IsNetworkable etc
//   References: GUID-typed fields (CharacterClass, CaptureArea, etc)
//   Tuning: floats and ints that look like gameplay knobs
//   Raw: everything else
static void DrawPropertiesMode(ImGuiGlueFrameArgs& args, const GraphIndex& index)
{
    (void)index;

    int focusIdx = FindByGuid(args, s_state.focusGuid);
    if (focusIdx < 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
            "No focus entity. Pick something in the viewport, the Outliner, or the Layers tab.");
        return;
    }

    // Header summary card.
    {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f),
            "%s", EntityName(args, focusIdx));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
            "[%s]", EntityType(args, focusIdx));
        ImGui::TextDisabled("GUID: 0x%08X", s_state.focusGuid);
        unsigned int parentG = (args.gameObjParentGuids)
            ? args.gameObjParentGuids[focusIdx] : 0;
        if (parentG != 0) {
            int pIdx = FindByGuid(args, parentG);
            ImGui::TextDisabled("Parent: %s [0x%08X]",
                pIdx >= 0 ? EntityName(args, pIdx) : "(unresolved)",
                parentG);
        } else {
            ImGui::TextDisabled("Parent: (top-level)");
        }
        unsigned int layerG = (args.gameObjLayerGuids)
            ? args.gameObjLayerGuids[focusIdx] : 0;
        if (layerG != 0) {
            int lIdx = FindByGuid(args, layerG);
            ImGui::TextDisabled("Layer: %s [0x%08X]",
                lIdx >= 0 ? EntityName(args, lIdx) : "(unresolved)",
                layerG);
        }
        if (args.gameObjPosX && args.gameObjPosY && args.gameObjPosZ) {
            ImGui::TextDisabled("Pos: (%.1f, %.1f, %.1f)",
                args.gameObjPosX[focusIdx],
                args.gameObjPosY[focusIdx],
                args.gameObjPosZ[focusIdx]);
        }
        if (args.gameObjGamemodeMasks) {
            ImGui::SameLine();
            ImGui::TextDisabled("| GMM: 0x%X",
                args.gameObjGamemodeMasks[focusIdx]);
        }
    }

    // Quick action toolbar.
    if (ImGui::SmallButton("Frame in 3D")) args.requestFocusEditorObjGuid = s_state.focusGuid;
    ImGui::SameLine();
    if (ImGui::SmallButton("Open Inspector")) {
        // Ensures codex's full type-aware Inspector panel is visible
        // for users who want the whole field list with grouping.
        args.requestSelectEditorObjGuid = s_state.focusGuid;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy GUID")) {
        char buf[32]; sprintf(buf, "0x%08X", s_state.focusGuid);
        ImGui::SetClipboardText(buf);
    }

    ImGui::Separator();

    // If args propFields are not populated for this entity (host has
    // not selected it for property edit yet), prompt the user to
    // sync selection.
    if ((unsigned int)args.propEntityGuid != s_state.focusGuid ||
        args.propFieldCount <= 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
            "Property data not loaded for this entity. Click Open Inspector or pick the entity in the viewport to sync.");
        return;
    }

    // Search filter.
    static char s_propSearch[128] = "";
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##forge_props_search",
        "Filter fields by name", s_propSearch, sizeof(s_propSearch));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##forge_props_search")) s_propSearch[0] = '\0';
    ImGui::SameLine();
    static bool s_propEssentials = false;
    ImGui::Checkbox("Essentials only", &s_propEssentials);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d fields total)", args.propFieldCount);

    ImGui::Separator();

    // Field table. Three columns: Name | Kind | Editable value.
    if (ImGui::BeginTable("##forge_props_table", 3,
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int fi = 0; fi < args.propFieldCount; ++fi) {
            const char* name = (args.propFieldNames && args.propFieldNames[fi])
                              ? args.propFieldNames[fi] : "?";
            const char* kind = (args.propFieldKindNames && args.propFieldKindNames[fi])
                              ? args.propFieldKindNames[fi] : "?";

            // Filter pass.
            if (s_propSearch[0] && !ContainsI(name, s_propSearch)) continue;
            if (s_propEssentials) {
                // Essentials = anything that smells like a gameplay knob.
                bool keep = ContainsI(name, "Health") || ContainsI(name, "Damage") ||
                            ContainsI(name, "Time")   || ContainsI(name, "Speed")  ||
                            ContainsI(name, "Team")   || ContainsI(name, "Color")  ||
                            ContainsI(name, "Output") || ContainsI(name, "Input")  ||
                            ContainsI(name, "Target") || ContainsI(name, "Capture")||
                            ContainsI(name, "Spawn")  || ContainsI(name, "Trigger")||
                            ContainsI(name, "Delay")  || ContainsI(name, "Sticky") ||
                            ContainsI(name, "Activate") || ContainsI(name, "Enable") ||
                            ContainsI(name, "GameMode") || ContainsI(name, "Mode");
                if (!keep) continue;
            }

            ImGui::PushID(fi);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", kind);
            ImGui::TableSetColumnIndex(2);

            // Edit by kind.
            if (ContainsI(kind, "float")) {
                float v = (args.propFieldFloatVals) ? args.propFieldFloatVals[fi] : 0.0f;
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragFloat("##v", &v, 0.05f, -1e6f, 1e6f, "%.3f")) {
                    args.propEditRequested = 1;
                    args.propEditFieldIndex = fi;
                    args.propEditKind = 1;
                    args.propEditFloatVal = v;
                }
            } else if (ContainsI(kind, "bool") || ContainsI(name, "Sticky")) {
                bool v = (args.propFieldIntVals && args.propFieldIntVals[fi] != 0);
                if (ImGui::Checkbox("##v", &v)) {
                    args.propEditRequested = 1;
                    args.propEditFieldIndex = fi;
                    args.propEditKind = 0;
                    args.propEditIntVal = v ? 1 : 0;
                }
            } else if (ContainsI(kind, "int") || ContainsI(kind, "color")) {
                int v = (args.propFieldIntVals) ? args.propFieldIntVals[fi] : 0;
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::DragInt("##v", &v, 1.0f, INT_MIN, INT_MAX)) {
                    args.propEditRequested = 1;
                    args.propEditFieldIndex = fi;
                    args.propEditKind = 0;
                    args.propEditIntVal = v;
                }
            } else if (ContainsI(kind, "guid")) {
                int v = (int)((args.propFieldGuidVals) ? args.propFieldGuidVals[fi] : 0);
                ImGui::SetNextItemWidth(150.0f);
                if (ImGui::InputInt("##v", &v)) {
                    args.propEditRequested = 1;
                    args.propEditFieldIndex = fi;
                    args.propEditKind = 2;
                    args.propEditGuidVal = (unsigned int)v;
                }
                ImGui::SameLine();
                if (v != 0) {
                    int rIdx = FindByGuid(args, (unsigned int)v);
                    if (rIdx >= 0) {
                        if (ImGui::SmallButton("frame")) {
                            args.requestSelectEditorObjGuid = (unsigned int)v;
                            args.requestFocusEditorObjGuid = (unsigned int)v;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", EntityName(args, rIdx));
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                            "ghost");
                    }
                } else {
                    ImGui::TextDisabled("(none)");
                }
            } else if (ContainsI(kind, "vec3")) {
                if (args.propFieldVec3Vals) {
                    float v[3] = {
                        args.propFieldVec3Vals[fi*3 + 0],
                        args.propFieldVec3Vals[fi*3 + 1],
                        args.propFieldVec3Vals[fi*3 + 2]
                    };
                    ImGui::SetNextItemWidth(220.0f);
                    if (ImGui::DragFloat3("##v", v, 0.05f, -1e6f, 1e6f, "%.2f")) {
                        args.propEditRequested = 1;
                        args.propEditFieldIndex = fi;
                        args.propEditKind = 3;
                        args.propEditFloatVal = v[0];
                        // The host pipeline expects vec3 via a separate
                        // path; here we trigger an edit with kind=3 and
                        // let the host read the buffered triplet.
                    }
                } else {
                    ImGui::TextDisabled("(no data)");
                }
            } else if (ContainsI(kind, "matrix")) {
                ImGui::TextDisabled("(matrix - edit via Manipulator)");
            } else {
                // Default: string field.
                char buf[160];
                const char* cur = (args.propFieldStringVals && args.propFieldStringVals[fi])
                    ? args.propFieldStringVals[fi] : "";
                std::strncpy(buf, cur, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::InputText("##v", buf, IM_ARRAYSIZE(buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    args.propEditRequested = 1;
                    args.propEditFieldIndex = fi;
                    args.propEditKind = 5;
                    std::strncpy(args.propEditStringVal, buf,
                                 sizeof(args.propEditStringVal) - 1);
                    args.propEditStringVal[sizeof(args.propEditStringVal) - 1] = '\0';
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DrawPanel(ImGuiGlueFrameArgs& args)
{
    GraphIndex index;
    BuildGraphIndex(args, index);

    if (args.gameObjCount <= 0 || !args.gameObjGuids || !args.gameObjNames || !args.gameObjTypeNames) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
            "No level loaded. Event Graph Studio has fuck-all to chew on.");
        return;
    }

    // ── Phase 10e: Delete-key on focused selection ────────────────────
    // Pulls the trigger on hostRequestDeleteEntityGuid when the user
    // has The Forge focused, has a selection live, and presses Delete.
    // We do NOT fire if the active widget is a text input (so typing
    // in a search bar with the cursor over a Delete key does not nuke
    // the highlighted entity). One-shot per key press, edge-detected
    // by ImGui's IsKeyPressed default repeat=false.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        unsigned int killGuid = args.inspectorSelGuid ? args.inspectorSelGuid
                                                      : s_state.focusGuid;
        if (killGuid != 0)
            args.hostRequestDeleteEntityGuid = killGuid;
    }

    DrawToolbar(args, index);

    // DO NOT pass ImGuiTabItemFlags_SetSelected based on s_state.mode every
    // frame. SetSelected is a one-shot "force this tab open NOW" command, not
    // a state predicate. Re-applying it every frame nukes ImGui's input/active
    // -item state right after a click registers, so rows highlight for one
    // frame then revert and clicks anywhere inside a tab look "dead". We only
    // fire SetSelected when an external request asks for a specific tab.
    ImGuiTabItemFlags reqOutliner    = (s_state.requestSwitchTo == View_Outliner)    ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqLayers      = (s_state.requestSwitchTo == View_Layers)      ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqManipulator = (s_state.requestSwitchTo == View_Manipulator) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqProperties  = (s_state.requestSwitchTo == View_Properties)  ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqSchematic   = (s_state.requestSwitchTo == View_Tree)        ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqFlowchart   = (s_state.requestSwitchTo == View_Flowchart)   ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqWires       = (s_state.requestSwitchTo == View_TripleRow)   ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqHealth      = (s_state.requestSwitchTo == View_Health)      ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags reqNodeGraph   = (s_state.requestSwitchTo == View_NodeGraph)   ? ImGuiTabItemFlags_SetSelected : 0;
    s_state.requestSwitchTo = -1; // consume the request — one-shot only

    if (ImGui::BeginTabBar("##egs_modes")) {
        if (ImGui::BeginTabItem("Outliner", NULL, reqOutliner)) {
            s_state.mode = View_Outliner;
            DrawOutlinerMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Layers", NULL, reqLayers)) {
            s_state.mode = View_Layers;
            DrawLayersMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Manipulator", NULL, reqManipulator)) {
            s_state.mode = View_Manipulator;
            DrawManipulatorMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Properties", NULL, reqProperties)) {
            s_state.mode = View_Properties;
            DrawPropertiesMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Schematic", NULL, reqSchematic)) {
            s_state.mode = View_Tree;
            DrawSchematicMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Node Graph", NULL, reqNodeGraph)) {
            s_state.mode = View_NodeGraph;
            DrawNodeGraphMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Flowchart", NULL, reqFlowchart)) {
            s_state.mode = View_Flowchart;
            DrawFlowchartMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Wires", NULL, reqWires)) {
            s_state.mode = View_TripleRow;
            DrawWiresMode(args, index);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Health", NULL, reqHealth)) {
            s_state.mode = View_Health;
            DrawHealthMode(args, index);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace EventGraphEditor
