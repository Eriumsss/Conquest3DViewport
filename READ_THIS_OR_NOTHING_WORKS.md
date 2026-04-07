# STOP. READ THIS FIRST. SERIOUSLY.

If you skip this file, nothing will work. You'll open a GitHub issue. I'll link you back here. Save us both the time.

---

## Step 0: Get the Right Game Executable

You need **`ConquestLLC.exe`** — the patched game executable. The original unpatched exe will NOT load mods, patches, or any of our tools.

Get it from the Discord: https://discord.gg/rEh5Tfz6JD

**Always launch the game using `ConquestLLC.exe`.** Not the original exe. Not a shortcut to the original exe. `ConquestLLC.exe`. I cannot stress this enough.

---

## Step 1: Folder Setup

Extract the release archive so your game directory looks like this:

```
The Lord of the Rings - Conquest/
├── ConquestLLC.exe          ← patched exe (from Discord)
├── BinkW32.dll              ← original game DLLs
├── ...
└── dev/
    └── Vespucci/
        └── Project/
            └── Final/
                └── lotr/
                    └── ZeroEnginePrototype/
                        └── ZeroEngine/
                            ├── Scene3D/          ← the viewer lives here
                            │   ├── Conquest3DViewport.exe
                            │   ├── imgui_d3d9.dll
                            │   ├── cg.dll
                            │   ├── cgGL.dll
                            │   ├── dppDx.dll
                            │   └── d3dx9_29.dll
                            ├── GameFiles/        ← game assets go here
                            │   ├── models/
                            │   ├── textures/
                            │   ├── animations/
                            │   ├── effects/
                            │   ├── jmodels/
                            │   ├── srclua/
                            │   └── lotrcparser/
                            │       ├── lotrc_rs.exe
                            │       └── conquest_strings.txt
                            └── Engine/
                                └── DLL/
```

The viewer (`Conquest3DViewport.exe`) expects `../GameFiles/` one level up from the `Scene3D` folder. If this relative path is wrong, no assets will load.

---

## Step 2: Disable Your Antivirus

I'm not joking. **Disable Windows Defender and any other antivirus before extracting.**

The following files get flagged as false positives:
- `Injector.exe` — DLL injector for the audio debugger
- `version.dll` — DLL proxy for the crash preventer
- `d3d9.dll` — DLL proxy for render hooking

These are NOT malware. They're standard game modding techniques (DLL injection, proxy DLLs). Windows doesn't understand the difference. Turn off real-time protection, extract, then add the folder to your exclusions list.

---

## Step 3: Required Files

### For the 3D Viewer (everyone needs these)

These DLLs must be in the same folder as `Conquest3DViewport.exe`:

| File | What it does | Where to get it |
|---|---|---|
| `imgui_d3d9.dll` | The entire UI | Included in release |
| `cg.dll` | Cg shader runtime | Included in release |
| `cgGL.dll` | Cg OpenGL bridge | Included in release |
| `dppDx.dll` | DirectX processing | Included in release |
| `d3dx9_29.dll` | DirectX 9 helpers | Install [DirectX End-User Runtime](https://www.microsoft.com/en-us/download/details.aspx?id=35) |

### For Level Loading

| File | What it does | Where to get it |
|---|---|---|
| `conquest_strings.txt` | CRC→name resolution for entity types | Discord or `GameFiles/lotrcparser/` |
| `lotrc_rs.exe` | PAK/BIN parser for Save PAK | Discord or `GameFiles/lotrcparser/` |

**Without `conquest_strings.txt`:** Levels load and render, but the Event Graph's Overview/Modes tabs won't work (entity types show as hex instead of names).

**Without `lotrc_rs.exe`:** Levels load and render, but Save PAK won't sanitize properly.

### For Collision Saving

| File | What it does | Where to get it |
|---|---|---|
| Python 3.x | Runs `collision_repack.py` | https://www.python.org/downloads/ |
| `lotrc_rs.exe` | PAK dump + recompile | Discord or `GameFiles/lotrcparser/` |

Python must be on your system PATH. Test by opening cmd and typing `python --version`.

### For the Crash Preventer (optional)

Copy these three files into the **game's root directory** (next to `ConquestLLC.exe`):

| File | What it does |
|---|---|
| `version.dll` | Auto-loads the debugger at game launch |
| `ConquestDebugger.dll` | Crash handler + minidump writer |
| `d3d9.dll` (16 KB) | Lightweight D3D9 proxy |

### For the Audio Debugger (optional)

| File | What it does |
|---|---|
| `d3d9.dll` (50 KB) | D3D9 proxy with audio hooks |
| `DebugOverlay.dll` | In-game audio browser overlay |
| `Injector.exe` | Injects the overlay into the running game |

Launch the game first with `ConquestLLC.exe`, then run `Injector.exe`.

---

## Step 4: First Launch

1. Navigate to `Scene3D/`
2. Double-click `Conquest3DViewport.exe`
3. Wait for the GameFiles scan to complete (~5 seconds)
4. You should see the ImGui docking UI with Browser, Inspector, Viewport, and Timeline panels

### If the window opens but the viewport is black:
- Check that `d3dx9_29.dll` is next to the exe
- Install the DirectX End-User Runtime
- Make sure your GPU supports DirectX 9

### If no models/animations appear in the browser:
- Check that `../GameFiles/` exists with `jmodels/`, `models/`, `animations/`, `textures/` folders inside
- Press **F5** to rescan

### If the Event Graph shows hex values instead of entity names:
- Place `conquest_strings.txt` next to the exe or in `GameFiles/lotrcparser/`
- Available on the Discord

### If Save PAK fails:
- Make sure `lotrc_rs.exe` is next to the exe or in `tools/`
- For collision saves, make sure Python is installed and on PATH

### If the game crashes on launch with Crash Preventer:
- Make sure you're using `ConquestLLC.exe`, not the original exe
- Check that all three DLLs (`version.dll`, `ConquestDebugger.dll`, `d3d9.dll`) are in the game root

### If you get "0xC0000142 — Application was unable to start":
- Install the Visual C++ 2005 SP1 Redistributable (x86)
- Make sure all required DLLs are next to the exe

### If you get "SetProcessDpiAwarenessContext not found in USER32.dll":
- You're on Windows 7/8 with an older version of the DLL
- Update to the latest release — this was fixed
- I understand you like using Win 7 ALL OF US DO but please upgrade your OS.
---

## Step 5: Join the Discord

Seriously. Most setup problems are answered in 30 seconds on Discord. Plus you need it for:
- `ConquestLLC.exe` (patched game exe)
- `conquest_strings.txt` (CRC resolution)
- `lotrc_rs.exe` (PAK parser)
- SDK files (if you want to compile from source)
- General help and community

**Discord:** https://discord.gg/rEh5Tfz6JD

---

*If you read this entire file and still can't get it working, open a GitHub issue with your Windows version, what error you see, and which step you're stuck on. I will help you. If you didn't read this file and open an issue anyway, I will link you back here.*
