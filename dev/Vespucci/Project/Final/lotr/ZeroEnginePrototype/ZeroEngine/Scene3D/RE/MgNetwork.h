// MgNetwork.h
// Reconstructed network session authentication ticket builder.
//
// Source function:
//   FUN_005bbc40 (121 addr-count, EC_time category — mislabeled)
//
// String refs: "ticket", "macAddr", "012345678", "consoleId", "tosVersion"
//
// This function builds an EA/online authentication ticket request object.
// It initialises the session struct, sets the request type to 'acct'
// (0x61636374), then populates ticket, macAddr, consoleId, tosVersion fields.
//
// ASM pattern:
//   PUSH ESI / MOV ESI,[ESP+0x8] (session_obj ptr)
//   PUSH EDI / MOV EDI,[0xa32838] (global session_mgr)
//   MOV ECX,ESI / CALL 0x005ade50  (session_obj::init())
//   PUSH EDI / PUSH 0x9a63b4 / MOV ECX,ESI
//   MOV dword[ESI+0x18], 0x61636374  ('acct' tag → request_type)
//   CALL 0x005b37f0  (set_string_field(field_name, value))
//   ... repeat for macAddr, consoleId, tosVersion (with 0x005b3920 for mac which takes 2 args)

#pragma once
#include "MgTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Session object partial layout (from MOV [ESI+0x18], 0x61636374):
//   +0x18: uint32_t request_type = 'acct' (0x61636374)
// ---------------------------------------------------------------------------
struct MgSessionRequest {
    uint8_t  _pad[0x18];
    uint32_t request_type;   // +0x18: 'acct' = 0x61636374
    // ... other fields managed by the set_string_field calls ...
};

// ---------------------------------------------------------------------------
// Dependency function pointers
// ---------------------------------------------------------------------------

// session_obj::init() — game: 0x005ade50
typedef void (__thiscall* PFN_MgSession_Init)(MgSessionRequest* obj);
extern PFN_MgSession_Init g_pfn_MgSession_Init;

// set_string_field(name, value) — game: 0x005b37f0
typedef void (__thiscall* PFN_MgSession_SetStringField)(MgSessionRequest* obj,
                                                          const char* field_name,
                                                          const char* value);
extern PFN_MgSession_SetStringField g_pfn_MgSession_SetStringField;

// set_mac_field(name, data, length) — game: 0x005b3920
typedef void (__thiscall* PFN_MgSession_SetMacField)(MgSessionRequest* obj,
                                                       const char* field_name,
                                                       const void* data,
                                                       int length);
extern PFN_MgSession_SetMacField g_pfn_MgSession_SetMacField;

// Global session manager (game: [0x00a32838])
extern void** g_MgSession_pMgr;

// ---------------------------------------------------------------------------
// FUN_005bbc40 — build session auth ticket request
//
// Parameters (from stack, 5 args total: RET_ADDR + 4 args → RET=0x14 bytes):
//   session_obj: the MgSessionRequest to populate
//   mac_data:    pointer to MAC address bytes
//   mac_len:     length of mac_data
//   console_id:  const char* console identifier string
//   tos_version: const char* (or NULL) terms-of-service version
//
// Field name strings (game .rdata):
//   0x9a63b4 = "ticket"
//   0x9a75e8 = (macAddr field name)
//   0x9a74d8 = "macAddr"
//   0x9a75dc = "012345678"  (default/placeholder mac)
//   0x9a75d0 = (related mac field)
//   0x9a74cc = "consoleId"
// ---------------------------------------------------------------------------
void MgNetwork_BuildAuthTicket(MgSessionRequest* session_obj,
                                const void*       mac_data,
                                int               mac_len,
                                const char*       console_id,
                                const char*       tos_version);

#ifdef __cplusplus
} // extern "C"
#endif
