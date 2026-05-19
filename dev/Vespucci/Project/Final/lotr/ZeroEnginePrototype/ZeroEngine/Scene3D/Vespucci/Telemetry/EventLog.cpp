// EventLog.cpp
// =============================================================================
// IN-MEMORY RING OF RECENT TELEMETRY EVENTS - SHOWN IN THE BRAIN OVERLAY
// =============================================================================
// Written by: Eriumsss

#include "../Core/RingBuffer.h"

#include "../imgui/imgui.h"

#include <cstring>

namespace Vespucci {
namespace Telemetry {

struct EventRow {
    char     name[48];
    char     payload[160];
    u64      unixSec;
};

namespace {
    static Core::RingBuffer<EventRow, 256> s_eventRing(
        Core::RingBuffer<EventRow, 256>::DROP_OLDEST);
}

void PushEventRow(const char* name, const char* payload, u64 unixSec) {
    EventRow r;
    std::strncpy(r.name, name ? name : "?", sizeof(r.name) - 1);
    r.name[sizeof(r.name) - 1] = 0;
    std::strncpy(r.payload, payload ? payload : "", sizeof(r.payload) - 1);
    r.payload[sizeof(r.payload) - 1] = 0;
    r.unixSec = unixSec;
    s_eventRing.push(r);
}

void RenderEventLogPane() {
    i32 size = s_eventRing.size();
    if (size == 0) {
        ImGui::TextDisabled("(no events yet)");
        return;
    }
    if (ImGui::BeginTable("##el_table", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Payload");
        ImGui::TableHeadersRow();
        // Newest first.
        for (i32 i = size - 1; i >= 0; --i) {
            const EventRow& r = s_eventRing.at(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)r.unixSec);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.payload);
        }
        ImGui::EndTable();
    }
}

void ClearEventLog() { s_eventRing.clear(); }

} // namespace Telemetry
} // namespace Vespucci
