# Conquest3DViewport
Engine-level reverse-engineered tooling suite for The Lord of the Rings: Conquest (2009) focused on rendering inspection, animation experimentation, level structure editing, crash diagnostics, and live audio debugging built for structural engine analysis without redistributing game assets or proprietary middleware.

Before starting please extract the dev.rar on your game executable directory.

Disable Windows Defender and any other AntiViruses before starting because Windows keeps flagging the injector and other files as trojan and etc.

Eriumsss — LOTR Conquest Modding Toolkit V1.0

A reverse-engineered, engine-level tooling suite for The Lord of the Rings: Conquest (Pandemic Studios, 2009).

This toolkit enables structured inspection, editing, and debugging of game systems — spanning 3D scene rendering, animation experimentation, level construction, crash diagnostics, and live audio analysis.

It operates as a runtime instrumentation and development layer around the original engine architecture.

3D Scene Viewer

A real-time Direct3D9 renderer capable of loading and visualizing native engine data structures including meshes, skeletal rigs, animation channels, materials, particles, and sky environments.

Conquest3DViewport.exe

Loads and renders animation JSON files, meshes, and skeletal data

Real-time camera controls (orbit, pan, zoom)

Bone editor with per-bone rotation and translation gizmos

Animation playback with blending support and Lua-scripted animation graphs

Particle system preview (MgPacked particle format)

Skybox rendering from game environment data

Material inspector and shader preview

Asset browser for batch loading game content

Built-in audio manager for scene sound playback

ImGui-based runtime interface via imgui_d3d9.dll

Designed for structural analysis, animation testing, and controlled content experimentation.

Level Editor

Block-based world editing utilities aligned with the engine’s native world structure.

Tool	Description
ZE GUI.exe	Visual level editor — load, arrange, and export world blocks via a Win32 GUI
ZeroEnginePrototype.exe	CLI companion tool for scripted level prototyping and data manipulation

Focused on geometry layout, object placement, and world data restructuring.

Crash Preventer

A drop-in stability and diagnostic layer that intercepts fatal errors, generates minidumps, and monitors runtime behavior.

File	Role
version.dll	Auto-loader placed in the game directory to load debugger at launch
ConquestDebugger.dll	Crash handler, minidump writer, memory watcher, thread monitor
d3d9.dll (16 KB)	Lightweight Direct3D9 proxy for render-path stability

Usage:
Copy all three files into the game's root directory. No configuration required.

Purpose:

Catch unhandled exceptions

Generate diagnostic dumps

Monitor threads and memory in real time

Improve development stability during mod testing

Audio Debugger & Modding Tools

A runtime audio inspection overlay with BNK soundbank browsing and custom playback capabilities for sound replacement validation.

File	Role
d3d9.dll (50 KB)	Direct3D9 proxy with runtime debug hooks
DebugOverlay.dll	In-game overlay — BNK browser, audio event viewer, custom playback
Injector.exe	Injects DebugOverlay into the running game process

Usage:
Launch the game first, then run Injector.exe.

Enables live inspection of sound events and controlled playback testing.

Requirements

The Lord of the Rings: Conquest (retail copy required)

Windows 7 or later (32-bit compatible)

DirectX End-User Runtime (for 3D Viewer)

Installation

Download the latest release from Releases

Extract to a folder of your choice

For Crash Preventer:
Copy version.dll, ConquestDebugger.dll, and d3d9.dll (16KB) into the game directory

For Audio Debugger:
Launch the game first, then run Injector.exe

For 3D Viewer / Level Editor:
Extract the dev folder into the game’s root directory and launch from there

Community

Certain additional game-specific files are required for full functionality.
Most programs will not operate without the correct game-side data present.

Join our Discord for setup assistance and required configuration guidance:

Discord: https://discord.gg/rEh5Tfz6JD

Disclaimer

This project is not affiliated with, endorsed by, or associated with
Electronic Arts, Pandemic Studios, Warner Bros. Interactive Entertainment,
or the Tolkien Estate.

No game assets, executables, audio, textures, models, or copyrighted creative content are distributed in this repository.

This is an independently developed fan-made modding toolkit created through clean-room reverse engineering for interoperability and research purposes.

Certain Lua scripts and JSON configuration files originating from
The Lord of the Rings: Conquest are included solely to provide data mappings required for interpreting game formats. These remain the property of their respective copyright holders and are provided only to enable software interoperability.

Use at your own risk.

## Table of Contents

1. [Getting Started](#getting-started)
2. [Folder Structure](#folder-structure)
3. [User Interface Overview](#user-interface-overview)
4. [Keyboard Shortcuts](#keyboard-shortcuts)
5. [Mouse Controls](#mouse-controls)
6. [Asset Browser](#asset-browser)
7. [Animation Playback](#animation-playback)
8. [Bone Editor (Edit Mode)](#bone-editor-edit-mode)
9. [Gizmo Controls](#gizmo-controls)
10. [Snap Settings](#snap-settings)
11. [Skybox & Environment](#skybox--environment)
12. [Effects System](#effects-system)
13. [Inverse Kinematics (IK)](#inverse-kinematics-ik)
14. [Physics & Ragdoll](#physics--ragdoll)
15. [Animation Blending](#animation-blending)
16. [Animation Graph](#animation-graph)
17. [Motion Matching](#motion-matching)
18. [Root Motion](#root-motion)
19. [Time Warp](#time-warp)
20. [Compression](#compression)
21. [Export](#export)
22. [UI Panels](#ui-panels)
23. [Theme & Display](#theme--display)
24. [Overlay & Help](#overlay--help)
25. [Audio](#audio)
26. [Camera Presets](#camera-presets)
27. [Troubleshooting](#troubleshooting)

---

## Getting Started

1. Place `ZeroEngine3DViewport.exe` and all of its required DLLs in the `Scene3D` folder.
2. Ensure the `GameFiles` folder exists one level above the exe (`../GameFiles/`).
3. Double-click the exe. It will automatically scan `GameFiles` for models, animations, textures, and effects.

### Required DLLs

| DLL | Purpose |
|---|---|
| `imgui_d3d9.dll` | ImGui docking UI |
| `cg.dll` / `cgGL.dll` | Cg shader runtime |
| `dppDx.dll` | DirectX processing |
| `d3dx9_29.dll` | DirectX 9 helper functions |

> **Note:** Debug DLLs (`d3dx9d_29.dll`, `dppDx_d.dll`) are not required for release.

## User Interface Overview

The viewer provides two UI modes:

- **ImGui Docking UI** (default) — modern tabbed workflow: Browser, Inspector, Viewport, Timeline
- **Legacy Win32 UI** — traditional Windows panels with listboxes and buttons (toggle with **F9**)

Both UIs can be active simultaneously. The ImGui docking UI provides full access to all features.

### ImGui Panels

| Panel | Content |
|---|---|
| **Browser** | Model list, animation list, effect list, model filter |
| **Inspector** | Material info, bone list, bone TRS values, IK settings, physics, blending, graph, motion matching |
| **Viewport** | 3D render view with gizmo overlays |
| **Timeline** | Keyframe editing timeline, playback controls, event markers |

### Legacy Win32 Panels

| Panel | Content |
|---|---|
| Left panel | Model list, animation list, filter fields, load buttons |
| Right panel | Bone list, playback speed slider, snap settings, export controls |
| Bottom panel | Timeline scrubber, playback transport, frame stepping |

> **Ctrl+1** / **Ctrl+2** / **Ctrl+3** collapse/expand the left, right, and timeline panels respectively.

---

## Keyboard Shortcuts

### General

| Key | Action |
|---|---|
| **F1** | Toggle help overlay |
| **H** | Toggle info overlay (model, anim, skybox, settings) |
| **F2** | Toggle Asset Browser overlay |
| **F5** | Rescan GameFiles (reload asset lists) |
| **F9** | Toggle Legacy Win32 panels (ImGui stays active) |
| **F11** | Toggle dark / light theme |
| **Esc** | Cancel active bone edit; if nothing active, quit application |

### Animation Playback

| Key | Action |
|---|---|
| **Space** or **Insert** | Play / Pause animation |
| **F7** or **Page Up** | Previous animation |
| **F8** or **Page Down** | Next animation |
| **Z** or **Home** or **Delete** | Seek to start (time = 0) |
| **X** or **End** | Seek to end |
| **8** or **\[** or **Numpad 8** | Seek backward 0.25 seconds |
| **9** or **\]** or **Numpad 9** | Seek forward 0.25 seconds |

### Skybox & Environment

| Key | Action |
|---|---|
| **F4** | Toggle skybox on/off |
| **F6** | Cycle to next skybox |
| **F10** | Cycle sky render mode (Skybox / EnvMap / Off) |

### Bone Editor

| Key | Action |
|---|---|
| **B** | Toggle Edit Mode on/off |
| **W** | Set gizmo to **Move** (Translate) mode |
| **E** | Set gizmo to **Rotate** mode |
| **Q** | Toggle gizmo space: **Local** ↔ **World** |
| **V** | Toggle snap on/off (for current gizmo mode) |
| **N** | Cycle snap step size (see [Snap Settings](#snap-settings)) |
| **I** | Toggle interpolation mode: **Hold** ↔ **Linear** |
| **Enter** or **K** | Commit current edit / set keyframe |
| **Esc** | Cancel current drag/edit |
| **F** | Focus camera on model |

### Axis Lock (while dragging)

Hold these keys during a bone drag to constrain to a single axis:

| Key | Axis |
|---|---|
| **X** (hold) | Lock to X axis |
| **Y** (hold) | Lock to Y axis |
| **Z** (hold) | Lock to Z axis |
| **Ctrl** (hold) | Fine precision (0.25× speed) |
| **Shift** (hold) | Coarse/fast (2× speed for rotate, Y-axis for translate) |

### Asset Browser (when open)

| Key | Action |
|---|---|
| **Tab** | Switch browser mode: Model ↔ Animation |
| **Up Arrow** | Select previous item |
| **Down Arrow** | Select next item |
| **Enter** | Load selected model/animation |
| **Esc** | Close browser |

### Panel Collapse (Legacy UI)

| Key | Action |
|---|---|
| **Ctrl+1** | Collapse/expand left panel |
| **Ctrl+2** | Collapse/expand right panel |
| **Ctrl+3** | Collapse/expand timeline panel |

---

## Mouse Controls

### Camera (default — Edit Mode off)

| Input | Action |
|---|---|
| **Left Mouse + Drag** | Orbit camera |
| **Right Mouse + Drag** | Rotate camera |
| **Mouse Wheel** | Zoom in/out |

### Camera (Edit Mode on — requires Alt)

When Edit Mode is active, camera movement requires holding **Alt** to prevent accidental camera moves while editing bones:

| Input | Action |
|---|---|
| **Alt + Left Mouse + Drag** | Orbit camera around model |
| **Alt + Middle Mouse + Drag** | Pan camera |
| **Alt + Right Mouse + Drag** | Dolly camera (zoom via drag) |

### Bone Editing (Edit Mode on)

| Input | Action |
|---|---|
| **Left Click on gizmo axis/ring** | Begin rotate or translate drag on that axis |
| **Ctrl + Left Click** | Free-axis fallback (drag without specific axis hit) |
| **Right Click on gizmo** | Same as left click (alternative hand) |
| **Drag** | Apply rotation/translation to selected bone |
| **Release** | End drag (edit is pending, not yet committed) |

### Timeline

| Input | Action |
|---|---|
| **Left Click on timeline** | Seek to that time position |
| **Shift + Left Click** | Seek with snap to nearest keyframe |
| **Right Click on timeline** | Context menu (Add Sound Event, Add Sound Cue, Delete Event) |
| **Mouse Wheel on timeline** | Zoom timeline |

---

## Asset Browser

The Asset Browser scans `../GameFiles/` for:

- **JModels** — `.json` files in `jmodels/` (model descriptors referencing `.glb` geometry)
- **Models** — `.glb` files in `models/` (3D geometry)
- **Animations** — `.json` files in `animations/`
- **Textures** — `.dds` files in `textures/`
- **Effects** — `.json` files in `effects/`

### Using the Browser

1. Press **F2** to open the overlay browser, or use the ImGui Browser panel.
2. Press **Tab** to switch between Model and Animation mode.
3. Use **Up/Down** arrows to select an asset.
4. Press **Enter** to load.
5. Press **F5** to rescan files if you added new assets while the viewer is running.

### Filtering

In the Legacy UI and ImGui Browser panel, type in the filter field to narrow the model or animation list.

---

## Animation Playback

### Transport Controls

| Control | Function |
|---|---|
| Play/Pause | **Space** / **Insert** or the Play button |
| Stop | Stop button (Legacy UI) |
| Previous | **F7** / **Page Up** or Prev button |
| Next | **F8** / **Page Down** or Next button |
| Frame step back | `|◀` button (Legacy UI) / **8** / **\[** |
| Frame step forward | `▶|` button (Legacy UI) / **9** / **\]** |
| First key | `|◀◀` button |
| Last key | `▶▶|` button |

### Speed Control

Playback speed is adjustable via:
- The speed slider in the Legacy UI right panel
- The speed control in the ImGui Timeline/Inspector panel

### Loop

Toggle looping via the **Loop** checkbox in the UI or through the ImGui panel.

### Fixed Timestep

Enable fixed-step animation updates from the ImGui Inspector:
- **Enabled**: Toggle fixed timestep on/off
- **Step Size**: Frame duration (default: 1/30s)
- **Max Steps**: Maximum catchup steps per frame

---

## Bone Editor (Edit Mode)

The bone editor allows you to pose individual bones and create custom keyframe animations.

### Workflow

1. Press **B** to enter Edit Mode.
2. Select a bone from the bone list (Legacy UI right panel or ImGui Inspector).
3. Click a gizmo axis/ring in the viewport to begin a drag.
4. Drag to rotate or translate the bone.
5. Press **Enter** or **K** to commit the edit as a keyframe.
6. Press **Esc** to cancel the current edit.

### Recording / Auto-Key

When **Recording** (AutoKey) is enabled, every committed edit is automatically recorded as a keyframe at the current playback time.

### Numeric Entry

The right panel (Legacy UI) or ImGui Inspector provides numeric fields for precise entry:
- **RX, RY, RZ** — Rotation in degrees
- **TX, TY, TZ** — Translation
- **Read** button — Read current bone values into fields
- **Set Key** button — Apply numeric values and set keyframe

### Easing

Each keyframe can have custom easing:
- **Type**: Linear, Ease In, Ease Out, Ease In-Out, Custom Bezier
- **Control Points**: Adjustable Bezier control points (cp1x, cp1y, cp2x, cp2y) via the ImGui Inspector

---

## Gizmo Controls

| Key | Mode |
|---|---|
| **W** | Translate (Move) gizmo — shows arrows for X/Y/Z axes |
| **E** | Rotate gizmo — shows rings for X/Y/Z rotation |
| **Q** | Toggle coordinate space: Local ↔ World |

The active gizmo mode and space are shown in the overlay and status bar.

---

## Snap Settings

### Rotation Snap

| Key / Control | Action |
|---|---|
| **V** (while in Rotate mode) | Toggle rotation snap on/off |
| **N** (while in Rotate mode) | Cycle snap step: 5° → 15° → 30° → 45° → 5° |
| Checkbox / field (UI) | Set custom rotation snap value |

### Translation Snap

| Key / Control | Action |
|---|---|
| **V** (while in Move mode) | Toggle translation snap on/off |
| **N** (while in Move mode) | Cycle snap step: 0.01 → 0.05 → 0.10 → 0.25 → 0.01 |
| Checkbox / field (UI) | Set custom move snap value |

---

## Skybox & Environment

| Key | Action |
|---|---|
| **F4** | Toggle skybox visibility |
| **F6** | Cycle to next skybox |
| **F10** | Cycle sky render mode |

### Sky Render Modes

| Mode | Description |
|---|---|
| **Skybox** | Render as full environment background |
| **EnvMap** | Render as environment/reflection map |
| **Off** | No sky rendering |

### Cloud Layer

A cloud layer can be toggled independently via the ImGui Inspector. It uses a separate skybox model as the cloud source.

---

## Effects System

Particle effects (`.json` files in `GameFiles/effects/`) can be spawned in the viewport:

1. Select an effect from the Effects list in the ImGui Browser or Legacy UI.
2. Click **Spawn Effect** to place it at the camera target position.
3. Effects reference textures from `GameFiles/textures/`.

---

## Inverse Kinematics (IK)

Available in the ImGui Inspector panel:

| Setting | Description |
|---|---|
| **IK Enabled** | Master IK toggle |
| **Foot IK** | Ground-contact foot IK |
| **Look-At IK** | Head/bone tracks a target point |
| **Aim IK** | Bone aims toward a target point |
| **Look-At Bone** | Which bone performs look-at |
| **Aim Bone** | Which bone performs aim |
| **Target X/Y/Z** | World-space target coordinates |
| **Weight** | Blend weight (0.0–1.0) |
| **IK Chains** | Per-chain enable/disable and target position |

---

## Physics & Ragdoll

Available in the ImGui Inspector:

| Setting | Description |
|---|---|
| **Physics Enabled** | Toggle physical animation simulation |
| **Ragdoll Enabled** | Toggle ragdoll mode |
| **Ragdoll Blend** | Blend between animation and ragdoll (0.0–1.0) |
| **Position Stiffness** | How stiffly bones follow position targets |
| **Position Damping** | Position velocity damping |
| **Rotation Stiffness** | How stiffly bones follow rotation targets |
| **Rotation Damping** | Rotation velocity damping |
| **Gravity** | Ragdoll gravity (default: -9.81) |

---

## Animation Blending

Blend two animations together:

| Setting | Description |
|---|---|
| **Blend Enabled** | Toggle blending on/off |
| **Blend Mode** | Full body / Additive |
| **Blend Rot Mode** | Rotation blending method |
| **Blend Alpha** | Mix factor between base and blend animation (0.0–1.0) |
| **Blend Anim** | Select the second animation to blend with |
| **Layer Root Bone** | For partial-body blending, which bone is the layer root |

---

## Animation Graph

The Lua-driven animation graph system allows state-machine-based animation control:

| Setting | Description |
|---|---|
| **Graph Enabled** | Toggle graph-driven playback |
| **Graph Name/Path** | Currently loaded graph |
| **States** | View and force-set the current state |
| **Parameters** | Adjust float/int/bool parameters that drive transitions |

Load animation graphs from `.lua` files in the GameFiles directory.

---

## Motion Matching

Data-driven animation selection based on velocity and facing:

| Setting | Description |
|---|---|
| **Motion Match Enabled** | Toggle motion matching |
| **Target Velocity X/Z** | Desired movement velocity |
| **Target Facing X/Z** | Desired facing direction |
| **Search Interval** | How often to search for best match (seconds) |
| **Blend Duration** | Cross-fade duration when switching clips |
| **Last Score** | Quality score of current match |
| **Current Clip** | Name of currently playing matched clip |

---

## Root Motion

Control how root bone translation is applied:

| Mode | Description |
|---|---|
| **Full** | Apply all root motion as-is |
| **ClampY** | Apply root motion but clamp vertical movement |
| **Off** | Ignore root motion entirely |
| **Extract** | Extract and display root motion data |

Additional controls:
- **Lock X/Y/Z** — Lock individual root motion axes
- **Root Motion Warp** — Warp root motion toward a target position
- **Warp Mode** — How warping is applied
- **Warp Target X/Y/Z** — Destination position

---

## Time Warp

Non-linear time remapping for animation playback:

| Setting | Description |
|---|---|
| **Time Warp Enabled** | Toggle time warping |
| **Type** | Easing type (Linear, Ease In, Ease Out, Custom Bezier) |
| **Control Points** | Bezier curve control points for custom warp curves |

---

## Compression

Animation compression tools (ImGui Inspector):

| Setting | Description |
|---|---|
| **Auto Compress** | Automatically compress on load |
| **Quantize Rotations** | Quantize rotation keyframes |
| **Strip Rotations** | Remove redundant rotation data |
| **Position Tolerance** | Position compression threshold |
| **Rotation Tolerance** | Rotation compression threshold (degrees) |
| **Scale Tolerance** | Scale compression threshold |
| **Root Tolerance** | Root bone compression threshold |
| **Stats** | Before/after keyframe counts for rotation, translation, scale, root |

---

## Export

Export custom animations created in the bone editor:

1. Enter a name in the **Export Name** field.
2. Click **Export** to save.
3. Exported clips are written as `.json` animation files.

Configure the timeline duration in the UI before exporting to set the clip length.

---

## UI Panels

### Legacy UI Buttons

| Button | Action |
|---|---|
| **Load Model** | Load selected model from list |
| **Load Anim** | Load selected animation from list |
| **Rescan** | Rescan GameFiles directory |
| **Play** | Play animation |
| **Stop** | Stop animation |
| **Prev / Next** | Previous / next animation |
| **◀ Frame / Frame ▶** | Step one frame back / forward |
| **◀◀ First / Last ▶▶** | Jump to first / last keyframe |
| **Spawn Effect** | Spawn selected effect |
| **Set A / Load A** | Save / load camera preset A |
| **Set B / Load B** | Save / load camera preset B |
| **Record** | Toggle auto-key recording |
| **Export** | Export animation clip |
| **Commit** | Commit pending bone edit |
| **Cancel** | Cancel pending bone edit |
| **Read** | Read current bone TRS into numeric fields |
| **Set Key** | Apply numeric TRS values and create keyframe |
| **Set Duration** | Set timeline duration from input field |

---

## Theme & Display

| Key | Action |
|---|---|
| **F11** | Toggle between **Dark** and **Light** theme |

Both themes apply to the Legacy Win32 panels (background gradients, text colors, panel colors). The ImGui docking UI has its own theme that changes with this setting.

---

## Overlay & Help

### Info Overlay (H)

Press **H** to toggle the on-screen information overlay showing:
- Current animation name, time, duration, play state
- Current model name
- Skybox name, state, and render mode
- Browser shortcut reference
- Playback shortcut reference
- Editor settings (AutoKey, pending edits, gizmo mode/space, axis locks)
- Snap settings
- Interpolation mode
- Decode mode, JSON mode, root motion mode, ground clamp mode

### Help Overlay (F1)

Press **F1** for a compact shortcut reference line:

## Audio

If Wwise sound banks are present in `../SoundBanks/`, audio events can be triggered:

- Sound banks are loaded automatically from the exe directory + `../SoundBanks/`.
- The timeline supports **Sound Event** and **Sound Cue** markers (right-click timeline to add).
- Events fire at their marker time during playback.

---

## Camera Presets

Save and recall camera positions:

| Control | Action |
|---|---|
| **Set A** / **Set B** | Save current camera to preset A or B |
| **Load A** / **Load B** | Restore camera from preset A or B |
| **F** | Focus camera on current model |
| **Ctrl+1** / **Ctrl+2** / **Ctrl+3** | Camera presets 1, 2, 3 (when not in panel collapse context) |

---

## Troubleshooting

### "0xC0000142 — Application was unable to start"
- Missing Visual C++ 2005 runtime. Install the VC++ 2005 SP1 Redistributable (x86).
- Ensure all required DLLs are next to the exe.

### No models/animations appear
- Verify `../GameFiles/` exists relative to the exe and contains `jmodels/`, `models/`, `animations/`, `textures/` folders.
- Press **F5** to rescan.

### Black viewport / no rendering
- Ensure `d3dx9_29.dll` is next to the exe.
- Check that your GPU supports DirectX 9.

### Skybox not showing
- Press **F4** to toggle skybox on.
- Check that skybox jmodel/glb files exist in `GameFiles/jmodels/` and `GameFiles/models/`.

---

*Generated for Conquest 3D Viewport (Eriumsss)*

Third-Party Notices
Library	License
Dear ImGui	MIT
nlohmann/json	MIT
miniaudio	Public Domain / MIT-0
Lua 5.1	MIT
License

MIT License
Copyright (c) 2025–2026 Eriumsss
