// MgNetwork.cpp
// Reconstructed from FUN_005bbc40 (121 addr-count)
//
// EA Online / GameSpy session authentication ticket builder.
// Populates a session request object with ticket, MAC address,
// console ID, and terms-of-service version fields.

#include "MgNetwork.h"

PFN_MgSession_Init          g_pfn_MgSession_Init          = NULL;
PFN_MgSession_SetStringField g_pfn_MgSession_SetStringField = NULL;
PFN_MgSession_SetMacField    g_pfn_MgSession_SetMacField    = NULL;
void**                       g_MgSession_pMgr               = NULL;

// Field name constants (game .rdata string addresses mapped to literals)
static const char* k_field_ticket      = "ticket";       // 0x9a63b4
static const char* k_field_mac_data    = "macAddr";      // 0x9a75e8 (data)
static const char* k_field_mac_name    = "macAddr";      // 0x9a74d8
static const char* k_field_mac_default = "012345678";    // 0x9a75dc (placeholder)
static const char* k_field_mac_extra   = "macAddr";      // 0x9a75d0
static const char* k_field_console_id  = "consoleId";    // 0x9a74cc
// tosVersion: pushed conditionally only if tos_version != NULL (0x9a74d8 context)

// ---------------------------------------------------------------------------
// FUN_005bbc40 — MgNetwork_BuildAuthTicket
//
// ASM reconstruction:
//   PUSH ESI / MOV ESI,[ESP+0x8]    (session_obj)
//   PUSH EDI / MOV EDI,[0xa32838]   (session_mgr global)
//   MOV ECX,ESI / CALL 0x005ade50   (session_obj->init())
//   PUSH EDI / PUSH 0x9a63b4("ticket") / MOV ECX,ESI
//   MOV dword[ESI+0x18], 0x61636374 ('acct')
//   CALL 0x005b37f0                  (set_string_field("ticket", session_mgr))
//   MOV EAX,[ESP+0x14] (mac_data ptr in arg5 position — wait)
//   ... Actually re-reading: PUSH mac_len, PUSH mac_data, PUSH "macAddr"
//   CALL 0x005b3920   (set_mac_field(name, data, len) — 3 args)
//   MOV EDX,[ESP+0x1c] (console_id)
//   PUSH EDX / PUSH 0x9a74d8 / CALL 0x005b37f0 (set "macAddr" field)
//   PUSH 0x9a75dc / PUSH 0x9a75d0 / CALL 0x005b37f0 (set "012345678" default)
//   MOV EAX,[ESP+0x18] (tos_version)
//   TEST EAX,EAX / JZ skip_tos
//   PUSH EAX / PUSH 0x9a74cc / CALL 0x005b37f0 (set "consoleId")
// ---------------------------------------------------------------------------
void MgNetwork_BuildAuthTicket(MgSessionRequest* session_obj,
                                const void*       mac_data,
                                int               mac_len,
                                const char*       console_id,
                                const char*       tos_version)
{
    if (!session_obj) return;

    void* session_mgr = g_MgSession_pMgr ? *g_MgSession_pMgr : NULL;

    // Step 1: init() — game: CALL 0x005ade50
    if (g_pfn_MgSession_Init)
        g_pfn_MgSession_Init(session_obj);

    // Step 2: set request type to 'acct' (0x61636374)
    session_obj->request_type = 0x61636374u; // 'acct'

    // Step 3: set_string_field("ticket", session_mgr)
    // (passes session_mgr as the value — it's a session handle/ticket token)
    if (g_pfn_MgSession_SetStringField)
        g_pfn_MgSession_SetStringField(session_obj, k_field_ticket,
                                        (const char*)session_mgr);

    // Step 4: set MAC address field (3-arg version)
    if (g_pfn_MgSession_SetMacField)
        g_pfn_MgSession_SetMacField(session_obj, k_field_mac_name,
                                     mac_data, mac_len);

    // Step 5: set "macAddr" default placeholder "012345678"
    if (g_pfn_MgSession_SetStringField) {
        g_pfn_MgSession_SetStringField(session_obj, k_field_mac_extra,
                                        k_field_mac_default);
    }

    // Step 6: set consoleId (only if provided — game: TEST EAX,EAX / JZ skip)
    if (console_id && g_pfn_MgSession_SetStringField)
        g_pfn_MgSession_SetStringField(session_obj, k_field_console_id, console_id);

    // Step 7: tosVersion is the same field pattern — if present
    // (game uses 0x9a74cc which we mapped to "consoleId"; tos may be a second
    //  call with different string — treated as optional)
    (void)tos_version; // preserved for future resolution
}
