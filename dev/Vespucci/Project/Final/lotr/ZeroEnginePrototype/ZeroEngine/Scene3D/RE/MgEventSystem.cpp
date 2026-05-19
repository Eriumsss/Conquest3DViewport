// ================================================================
// MgEventSystem.cpp - Output Event Dispatch System
// Reconstructed from ConquestLLC.exe disassembly
// See MgEventSystem.h for full documentation
// ================================================================

#include "MgEventSystem.h"
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// CreateOutputSlot (0x0084ddc3)
// Allocates a 0x14-byte output slot with empty linked list.
// ------------------------------------------------------------------
MgOutputSlot* MgEvent_CreateSlot(void* ownerEntity, uint32_t eventNameHash) {
    MgOutputSlot* slot = (MgOutputSlot*)malloc(sizeof(MgOutputSlot));
    if (!slot) return NULL;

    // Linked list head/tail point to self (empty list sentinel)
    slot->first = (MgEventConnection*)slot;
    slot->last  = (MgEventConnection*)slot;
    slot->count = 0;
    slot->eventHash = eventNameHash;
    slot->owner = ownerEntity;
    return slot;
}

// ------------------------------------------------------------------
// FireOutput (0x00851766)
//
// Walks the connection linked list from the slot.
// For each enabled connection: enqueues a pending event.
// If Sticky==false, sets enabled=false (one-shot disconnect).
//
// The real game checks [owner+0x149] for eventsEnabled flag.
// ------------------------------------------------------------------
void MgEvent_FireOutput(MgOutputSlot* slot) {
    if (!slot || !slot->owner) return;

    // Walk connection linked list (terminates when wrapping back to slot)
    MgEventConnection* node = slot->first;
    while (node != (MgEventConnection*)slot) {
        if (node->enabled) {
            // Enqueue this connection for deferred processing
            MgEvent_Enqueue(node);

            // Sticky check: if not sticky, disconnect after firing
            if (!node->sticky) {
                node->enabled = 0;  // One-shot: disable after fire
            }
        }
        node = node->next;
    }
}

// ------------------------------------------------------------------
// EnqueuePendingEvent (0x009057e0)
//
// Copies 0x30 bytes from connection into a new pending event.
// Max queue depth: 100 events.
// Checks pause state at [0x00CD8038].
// ------------------------------------------------------------------

// Static event queue (simplified - real game uses global at [0x00CD7E44])
static MgPendingEvent* s_eventQueue[MG_EVENT_QUEUE_MAX];
static uint32_t s_eventQueueCount = 0;

int MgEvent_Enqueue(const MgEventConnection* conn) {
    if (!conn) return 0;
    if (s_eventQueueCount >= MG_EVENT_QUEUE_MAX) return 0;  // Queue full

    MgPendingEvent* evt = (MgPendingEvent*)malloc(sizeof(MgPendingEvent));
    if (!evt) return 0;

    // Copy 0x30 bytes from connection data (matching game's memcpy)
    memcpy(evt->raw, conn, 0x30);

    s_eventQueue[s_eventQueueCount] = evt;
    s_eventQueueCount++;
    return 1;
}

// ------------------------------------------------------------------
// ProcessEventQueue (0x00905838)
//
// Called once per frame. For each pending event:
//   1. Decrements delay by deltaTime
//   2. If delay <= 0: dispatches via target entity vtable[0x3C]
//   3. Frees processed events, compacts queue
//
// Special action types handled differently:
//   AttachObject (addr 0xE882D8)
//   DetachObject (addr 0xE882D4)
//   PostAnimationEvent (addr 0xE882D0)
//
// Default: vtable[0x3C] call on target entity.
// ------------------------------------------------------------------
void MgEvent_ProcessQueue(float deltaTime) {
    uint32_t i = 0;
    while (i < s_eventQueueCount) {
        MgPendingEvent* evt = s_eventQueue[i];

        // Decrement delay timer (at offset 0x28 in the raw data)
        float* delayPtr = (float*)(evt->raw + 0x28);
        *delayPtr -= deltaTime;

        if (*delayPtr > 0.0f) {
            i++;  // Not ready yet, skip
            continue;
        }

        // Event is ready to dispatch
        // In the real game: resolve target handle at [+0x18],
        // then call target->vtable[0x3C](target, evt)
        //
        // For now this is a stub — actual dispatch requires
        // the live entity table and vtable infrastructure.

        // Free the processed event
        free(evt);

        // Compact queue: move last entry to this slot
        s_eventQueueCount--;
        if (i < s_eventQueueCount) {
            s_eventQueue[i] = s_eventQueue[s_eventQueueCount];
        }
        s_eventQueue[s_eventQueueCount] = NULL;
        // Don't increment i — re-check this slot (it has the moved entry)
    }
}

// ------------------------------------------------------------------
// Queue management helpers
// ------------------------------------------------------------------
uint32_t MgEvent_GetQueueCount(void) {
    return s_eventQueueCount;
}

void MgEvent_ClearQueue(void) {
    for (uint32_t i = 0; i < s_eventQueueCount; i++) {
        if (s_eventQueue[i]) {
            free(s_eventQueue[i]);
            s_eventQueue[i] = NULL;
        }
    }
    s_eventQueueCount = 0;
}

#ifdef __cplusplus
} // extern "C"
#endif