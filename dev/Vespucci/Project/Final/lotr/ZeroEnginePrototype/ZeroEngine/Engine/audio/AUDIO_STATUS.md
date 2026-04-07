# ZeroEngine Audio System — Status & Observations

## SDK
- **Wwise v2.8** (SDK 2.1.2821), `AK_BANK_READER_VERSION = 34`
- All extracted banks are version 34 — **no version mismatch**

## What Works
- MemoryMgr, StreamMgr, Blocking I/O Device, SoundEngine, MusicEngine — all init OK
- Vorbis codec registered
- 56 banks loaded from `extracted/` and `extracted/english(us)/`
- Event mapping table built (2,817 entries from AudioHook DLL)
- `LowLevelIO::Read()` correctly uses `pOverlapped->Offset` for file position
- `RenderAudio()` called every frame in main loop
- Listener position updated from camera each frame

## What Does NOT Work
- **No audible sound** — PostEvent for "swing" returns a playing ID but nothing is heard
- "footstep_walk" returns pid=0 (event ID 0x453674FD not in mapping table)

## Root Cause Candidates

### 1. `bGlobalFocus` was `false` (DEFAULT) — **FIXED**
DirectSound mutes output when the window loses focus. The default from
`GetDefaultPlatformInitSettings` is `false`.
- **Fix applied**: `platformSettings.bGlobalFocus = true;` (AudioManager.cpp:423)

### 2. `SetActiveListeners` was never called — **FIXED**
Game object must be associated with at least one listener via bitmask.
Without this, Wwise has no listener to route audio to.
- **Fix applied**: `SetActiveListeners(DEFAULT_GAME_OBJECT, 0x1);` (AudioManager.cpp:450)

### 3. WEM Files in Wrong Directory — **NOT YET FIXED**
Streaming sounds need `.wem` files. LowLevelIO searches:
- `extracted/` → 53 .bnk, **0 .wem**
- `extracted/english(us)/` → 41 .bnk, **0 .wem**

Actual WEM locations (63,082 files total):
- `ExtractedSounds/wem/` → 411 SFX .wem files
- `ExtractedSounds/wem/english(us)/` → 4,398 VO .wem files

> **Note**: BaseCombat (3.1 MB) and Effects (2.5 MB) have embedded DATA chunks,
> so "swing" should NOT need external WEMs. This issue only blocks streamed sounds.

### 4. Init.bnk Bus Volumes — **UNVERIFIED**
The STMG chunk in Init.bnk defines master bus hierarchy.
If master volume is 0 or muted, all sound is silent regardless of events firing.

### 5. ADPCM Codec Not Registered — **UNVERIFIED**
Only Vorbis is registered. If some embedded media uses ADPCM encoding,
those sounds will fail silently. ADPCM may be built-in for this SDK version.

### 6. PostEvent Uses Wide String — **LOW RISK**
`PostEvent(wchar_t*, ...)` relies on Wwise internal `GetIDFromString`.
If the bank was built with hashed IDs only (no string table), string-based
lookup fails. Alternative: use `PostEvent(AkUniqueID, ...)` with the FNV-1 hash directly.

## File Layout
```
ExtractedSounds/
├── extracted/          ← BNK files (53 root + 41 english(us))
│   ├── *.bnk
│   └── english(us)/*.bnk
├── wem/                ← WEM files (NOT searched by LowLevelIO)
│   ├── *.wem           (411 SFX)
│   └── english(us)/    (4,398 VO)
├── Organized_Final/    ← WEMs sorted by bank/category
└── Organized/          ← WEMs + BNKs (alternate layout)
```

## Key Banks
| Bank | File | Size | Has Embedded Media |
|------|------|------|--------------------|
| Init | 1355168291.bnk | 5.6 KB | No (STMG/HIRC/FXPR/ENVS only) |
| BaseCombat | 3503657562.bnk | 3.1 MB | Yes (3 MB DATA + DIDX) |
| Effects | 1942696649.bnk | 2.5 MB | Yes (2.3 MB DATA + DIDX) |

## Next Steps (Priority Order)
1. **Build & test** with `bGlobalFocus` + `SetActiveListeners` fixes
2. If still silent: try `PostEvent(0x8E3F67ADU, gameObj)` (numeric ID for "swing")
3. Add WEM search paths (`wem/` and `wem/english(us)/`) to LowLevelIO
4. Parse Init.bnk STMG chunk to verify master bus volume > 0
5. Register ADPCM codec if available in SDK
6. Add Wwise Monitor error callback for internal error visibility

