// ================================================================
// MgEventSystem.h - Output Event Dispatch System
// Reconstructed from ConquestLLC.exe disassembly
//
// The game uses a two-phase event system:
//   Phase 1: FireOutput (0x00851766) walks connection linked list, enqueues events
//   Phase 2: ProcessEventQueue (0x00905838) per-frame dispatches with delay support
//
// Key structures:
//   MgOutputSlot (0x14 bytes) — named output on an entity
//   MgEventConnection (~0x30 bytes) — links source output to target input
//   MgPendingEvent (0x30 bytes) — queued event with delay timer
//
// Max queue depth: 100 entries (0x64)
// Frame delta time global: 0x00CD8934
// Event queue global: [0x00CD7E44]
//
// Sources:
//   FUN_0084ddc3 — OutputSlot allocation (0x14 bytes)
//   FUN_00851766 — FireOutput (walks linked list, enqueues)
//   FUN_009057e0 — EnqueuePendingEvent (copies 0x30 bytes, max 100)
//   FUN_00905838 — ProcessEventQueue (per-frame dispatch via vtable[0x3C])
//   FUN_00854224 — InitBaseOutputs (OnUser1-4, OnCreate, OnDestroy)
//   FUN_008ebcda — CapturePoint output slot registration
//   FUN_007f0cc3 — PointManager output slot registration
// ================================================================

#pragma once
#include <stdint.h>
#include "MgPblTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// MgOutputSlot — A named event output on an entity (0x14 = 20 bytes)
//
// Each entity has an array of output slots at [entity+0xB8].
// The slot contains a doubly-linked list of connections.
// When the event fires, all connections in the list are enqueued.
//
// From disassembly at 0x0084ddc3:
//   [+0x00] first connection node (linked list head, initially points to self)
//   [+0x04] last connection node (linked list tail, initially points to self)
//   [+0x08] connection count (initially 0)
//   [+0x0C] event name hash (CRC of event name like "OnCapture", "OnDeath")
//   [+0x10] owning entity pointer
// ------------------------------------------------------------------
typedef struct MgOutputSlot {
    struct MgEventConnection* first;      // +0x00 linked list head
    struct MgEventConnection* last;       // +0x04 linked list tail
    uint32_t                  count;      // +0x08 number of connections
    uint32_t                  eventHash;  // +0x0C CRC hash of event name
    void*                     owner;      // +0x10 owning entity
} MgOutputSlot;

// Entity stores output slots at these offsets:
// [entity+0xB8] = pointer array of MgOutputSlot*
// [entity+0xBC] = current slot count
// [entity+0xC0] = slot array capacity

// ------------------------------------------------------------------
// MgEventConnection — Links a source output to a target input (~0x30 bytes)
//
// Forms a doubly-linked list within an MgOutputSlot.
// Each connection defines: who to target, what action, delay, sticky flag.
//
// From disassembly at 0x00851766 and 0x00905838:
//   [+0x08] actionTypeHash — CRC of the Input action name
//   [+0x10] parameter — value passed with the action
//   [+0x18] targetHandle — MgHandle of target entity
//   [+0x24] secondaryHandle — secondary entity reference
//   [+0x28] delay — seconds to wait before firing (float)
//   [+0x2C] sticky — if false, connection disconnects after first fire (bool)
//   [+0x2D] enabled — active flag, set to false when one-shot fires (bool)
// ------------------------------------------------------------------
typedef struct MgEventConnection {
    struct MgEventConnection* next;       // +0x00 next in linked list
    struct MgEventConnection* prev;       // +0x04 previous in linked list
    void*                     data;       // +0x08 -> connection data (self for inline)
    // Connection data (inline when data == self):
    uint32_t  actionTypeHash;             // +0x08 CRC of Input action name
    uint32_t  _pad1;                      // +0x0C
    uint32_t  parameter;                  // +0x10 parameter value
    uint32_t  _pad2;                      // +0x14
    MgHandle  targetHandle;               // +0x18 target entity handle
    uint32_t  _pad3;                      // +0x20
    MgHandle  secondaryHandle;            // +0x24 secondary entity reference
    float     delay;                      // +0x28 delay in seconds
    uint8_t   sticky;                     // +0x2C 1=repeatable, 0=one-shot
    uint8_t   enabled;                    // +0x2D 1=active, 0=disconnected
    uint8_t   _pad4[2];                   // +0x2E
} MgEventConnection;

// ------------------------------------------------------------------
// MgPendingEvent — Queued event (0x30 = 48 bytes, copied from connection)
//
// From disassembly at 0x009057e0:
//   Allocated as 0x30 bytes via malloc
//   memcpy(event, connection_data, 0x30) — direct copy of connection struct
//   Stored in queue array at [queue + count*4 + 0xC]
//   Queue count at [queue + 0x19C]
//   Max 100 events (0x64)
// ------------------------------------------------------------------
typedef struct {
    uint8_t raw[0x30];  // Raw copy of connection data (see MgEventConnection layout)
    // Key fields (same offsets as MgEventConnection):
    // [+0x08] actionTypeHash
    // [+0x10] parameter
    // [+0x18] targetHandle
    // [+0x28] delay (decremented each frame by g_frameDeltaTime)
} MgPendingEvent;

// ------------------------------------------------------------------
// MgEventQueue — Per-frame event processing queue
//
// Global at [0x00CD7E44]
// Frame delta time at [0x00CD8934]
// ------------------------------------------------------------------
#define MG_EVENT_QUEUE_MAX 100  // 0x64 max pending events

typedef struct {
    uint8_t       _header[0x0C];                   // +0x00 internal header
    MgPendingEvent* events[MG_EVENT_QUEUE_MAX];    // +0x0C event pointer array
    uint32_t      count;                           // +0x19C current event count
} MgEventQueue;

// ------------------------------------------------------------------
// CapturePoint output slot offsets (from 0x008ebcda)
// ------------------------------------------------------------------
#define CP_OUTPUT_OnCapture         0x61C
#define CP_OUTPUT_OnRedCapture      0x620
#define CP_OUTPUT_OnBlueCapture     0x624
#define CP_OUTPUT_OnDecapture       0x628
#define CP_OUTPUT_OnRedDecapture    0x62C
#define CP_OUTPUT_OnBlueDecapture   0x630
#define CP_OUTPUT_OnPlayerCapture   0x634
#define CP_OUTPUT_OnStartCapture    0x638
#define CP_OUTPUT_OnStartDispute    0x63C
#define CP_OUTPUT_OnUse             0x640

// ------------------------------------------------------------------
// PointManager output slot offsets (from 0x007f0cc3)
// ------------------------------------------------------------------
#define PM_OUTPUT_OnNotify1         0x154
#define PM_OUTPUT_OnNotify2         0x158
#define PM_OUTPUT_OnNotify3         0x15C
#define PM_OUTPUT_OnVictory         0x160
#define PM_OUTPUT_OnPointChange     0x164
#define PM_OUTPUT_OnActivate        0x168
#define PM_OUTPUT_OnDeactivate      0x16C
#define PM_OUTPUT_On50PointsToGo    0x170
#define PM_OUTPUT_On10PointsToGo    0x174
#define PM_OUTPUT_On5PointsToGo     0x178
#define PM_OUTPUT_On1PointToGo      0x17C
#define PM_OUTPUT_On75Percent       0x180
#define PM_OUTPUT_On50Percent       0x184
#define PM_OUTPUT_On25Percent       0x188

// PointManager victory score offset
#define PM_VICTORY_SCORE_OFFSET     0x194  // [entity+0x194] = points needed to win (default 9999)

// ------------------------------------------------------------------
// Special action type hashes (checked in ProcessEventQueue at 0x00905838)
// ------------------------------------------------------------------
#define MG_ACTION_AttachObject       0xE882D8  // global address storing AttachObject hash
#define MG_ACTION_DetachObject       0xE882D4  // DetachObject hash
#define MG_ACTION_PostAnimEvent      0xE882D0  // PostAnimationEvent hash

// ------------------------------------------------------------------
// Global addresses
// ------------------------------------------------------------------
#define MG_ADDR_EventQueue      0x00CD7E44  // pointer to MgEventQueue
#define MG_ADDR_FrameDeltaTime  0x00CD8934  // float: frame delta time in seconds
#define MG_ADDR_GamePaused      0x00CD8038  // pause state check

// ------------------------------------------------------------------
// Event System Functions
// ------------------------------------------------------------------

// FireOutput (0x00851766)
// Walks the connection linked list in the output slot.
// For each enabled connection: enqueues a pending event.
// If Sticky==false on a connection, sets enabled=false after firing.
// Checks owner entity's eventsEnabled flag at [owner+0x149].
void MgEvent_FireOutput(MgOutputSlot* slot);

// ProcessEventQueue (0x00905838)
// Called once per frame. For each pending event:
//   1. Decrements delay by frameDeltaTime
//   2. If delay <= 0: dispatches via target entity vtable[0x3C]
//   3. Special handling for AttachObject/DetachObject/PostAnimEvent
//   4. Frees processed events, compacts queue
void MgEvent_ProcessQueue(float deltaTime);

// EnqueuePendingEvent (0x009057e0)
// Copies 0x30 bytes from connection into a new pending event.
// Returns false if queue is full (100 events) or game is paused.
int MgEvent_Enqueue(const MgEventConnection* conn);

// CreateOutputSlot (0x0084ddc3)
// Allocates a 0x14-byte output slot, initializes linked list pointers to self.
MgOutputSlot* MgEvent_CreateSlot(void* ownerEntity, uint32_t eventNameHash);

#ifdef __cplusplus
} // extern "C"
#endif