#pragma once
// ============================================================================
// EventMapping.h - Conquest Audio Event -> Bank Mapping (2,817 entries)
// Auto-generated from AudioHook DLL event_mapping.h (parse_txtp.py)
// C++03 compatible - no unordered_map, no auto, no range-for
// ============================================================================

// Event mapping entry: event hash ID -> source bank + display name
struct EventMappingEntry {
    unsigned long id;
    const char* bank;   // Bank name (always present)
    const char* name;   // Event name (NULL if unresolved)
};

// Forward declaration - array defined in EventMappingData.cpp
extern const EventMappingEntry g_EventMappingData[];
extern const int g_EventMappingCount;

// Linear lookup (use the std::map in AudioManager for fast O(log n) access)
inline const EventMappingEntry* FindEventMapping(unsigned long eventID)
{
    for (int i = 0; i < g_EventMappingCount; ++i)
    {
        if (g_EventMappingData[i].id == eventID)
            return &g_EventMappingData[i];
    }
    return 0;  // NULL in C++03
}

// Get bank name for an event (returns NULL if not found)
inline const char* GetEventBankName(unsigned long eventID)
{
    const EventMappingEntry* e = FindEventMapping(eventID);
    return e ? e->bank : 0;
}

// Get display name for an event (returns NULL if not found or unresolved)
inline const char* GetEventDisplayName(unsigned long eventID)
{
    const EventMappingEntry* e = FindEventMapping(eventID);
    if (!e) return 0;
    if (e->name && e->name[0]) return e->name;
    return 0;
}

