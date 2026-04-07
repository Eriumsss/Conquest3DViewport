# Important — Read Before Using

## ConquestLLC.exe (REQUIRED)

**You MUST launch the game using `ConquestLLC.exe`**, not the original game executable. Without the patched exe, mods and patch fixes will not work.

Obtain `ConquestLLC.exe` from the Discord: https://discord.gg/rEh5Tfz6JD

---

## Scene3D Known Issues

### Animation Events Not Exporting
Events are not exported when an animation finishes. Will be fixed in a future update.

### Event Graph — Overview/Modes Tabs Broken Without conquest_strings.txt
The constellation, galaxy, neural, cosmic visualization scales and the Modes tab require `conquest_strings.txt` to resolve CRC hashes into entity type names. Without it, types show as `"0xA3B2C1D0"` instead of `"static_object"` and the graph can't categorize anything. Cursor trails and basic UI still work — only the data-driven visualizations break.

**Fix:** Place `conquest_strings.txt` next to `ZeroEngine3DViewport.exe`. Available on the Discord.

### Save PAK Needs lotrc_rs.exe
The C++ PAK writer has structural issues. And as a result we wired it to haighcam's rust parser and its going to stay that way
for a while.
A Rust parser pass (`lotrc_rs.exe`) automatically sanitizes the output after saving. Without it, saved PAKs may have formatting problems.

**Fix:** Place `lotrc_rs.exe` next to the EXE or in `tools/`.

### Collision Save Needs Python + lotrc_rs.exe
Collision saving uses `collision_repack.py` which calls `lotrc_rs.exe` internally. Both must be present.
Export the collision to json format first and then hit save, if your game still crashes sanitize the level with parser
and run this prompt:

python collision_repack.py "..\RE\Levels\level_name.PAK" "collision_export.json" "model_name"
 "..\GameFiles\lotrcparser\lotrc_rs.exe"

**Fix:** Install Python 3.x on PATH. Place `lotrc_rs.exe` next to the EXE.

### UI Freezes During Save (10-30 seconds)
Save PAK and collision save run the Rust parser synchronously. The UI freezes until completion. It's working — just wait.

### ~15% Lua Animation Clips Unresolved
Some CRC hashes from Pandemic's Lua scripts can't resolve to clip names — those animations were never shipped as strings on the retail disc. Affected states show a T-pose.

### Object Positioning Offset in Loaded Levels
Some level objects render at incorrect positions. Transform data, vertex data, and shader constants have all been verified correct. Root cause remains unsolved.

### GameFiles Directory Required
The viewer expects `../GameFiles/` relative to the EXE. If missing, no models/textures/animations/effects will appear. Press **F5** to rescan after fixing paths.

---

## Reporting Issues

**Discord:** https://discord.gg/rEh5Tfz6JD

Include: what happened, any error messages, your Windows version, and whether `conquest_strings.txt` and `lotrc_rs.exe` are present.

---

*Eriumsss — V2.0*
